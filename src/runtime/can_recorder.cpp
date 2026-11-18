#include "runtime/can_recorder.h"
#include "core/protocol_parser.h"
#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QThread>
#include <QtCore/QTimeZone>
#include <QtCore/QUuid>

namespace fhmi {

QString canCsvTime(qint64 timestamp)
{
    return timestamp > 0
               ? QDateTime::fromMSecsSinceEpoch(timestamp, QTimeZone::systemTimeZone()).toString(Qt::ISODateWithMs)
               : QString();
}
QString canCsvType(const CanFrame &frame)
{
    return frame.rtr  ? QStringLiteral("RTR")
           : frame.fd ? (frame.brs ? QStringLiteral("FD BRS") : QStringLiteral("FD"))
                      : QStringLiteral("Data");
}
QByteArray canCsvLine(QStringList fields)
{
    for (QString &field : fields) {
        field.replace(QLatin1Char('"'), QStringLiteral("\"\""));
        field = QLatin1Char('"') + field + QLatin1Char('"');
    }
    return (fields.join(QLatin1Char(',')) + QStringLiteral("\r\n")).toUtf8();
}

class CaptureWriter : public QObject
{
    Q_OBJECT
  public:
    CaptureWriter(QString directory, int rows, std::shared_ptr<std::atomic<quint64>> pending)
        : m_directory(std::move(directory)), m_limit(std::max(1, rows)), m_pending(std::move(pending)),
          m_prefix(QStringLiteral("can-%1-%2")
                       .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss-zzz")),
                            QUuid::createUuid().toString(QUuid::Id128)))
    {
    }
    void append(const QList<CanFrame> &frames, bool sent, const DataSource &source, const QString &channel,
                const std::shared_ptr<const CanSignalSet> &signalSet)
    {
        if (m_failed)
            return;
        for (const CanFrame &frame : frames) {
            if (!m_file.isOpen() && !openFile())
                return;
            const CanId id{frame.id, frame.extended};
            const QByteArray line =
                canCsvLine({QString::number(++m_sequence), canCsvTime(frame.timestampMs), Can::idText(id),
                            sent ? QStringLiteral("TX") : QStringLiteral("RX"), canCsvType(frame),
                            QString::number(frame.data.size()),
                            frame.rtr ? QString() : QString::fromLatin1(frame.data.toHex(' ').toUpper()),
                            signalSet ? signalSet->messageName(id) : QString(), QString::number(frame.timestampMs),
                            source.connectionId, channel});
            if (m_file.write(line) != line.size()) {
                fail(m_file.errorString());
                return;
            }
            ++m_rows;
            --*m_pending;
            if (m_rows >= quint64(m_limit) && !finishFile())
                return;
        }
        if (m_file.isOpen() && !m_file.flush())
            fail(m_file.errorString());
    }
    void fail(const QString &reason)
    {
        if (m_failed)
            return;
        m_failed = true;
        // Include the current file when flushing fails: its durability is unknown.
        emit failed(reason, m_pending->load() + m_rows);
        m_file.close();
    }
    void finish()
    {
        if (!m_failed)
            finishFile();
    }
  signals:
    void failed(const QString &reason, quint64 unsavedFrames);

  private:
    bool openFile()
    {
        if (!QDir().mkpath(m_directory)) {
            fail(tr("无法创建 CAN 自动保存目录"));
            return false;
        }
        m_file.setFileName(
            QDir(m_directory)
                .filePath(QStringLiteral("%1-%2.csv").arg(m_prefix).arg(++m_part, 5, 10, QLatin1Char('0'))));
        if (!m_file.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
            fail(m_file.errorString());
            return false;
        }
        m_rows = 0;
        const QByteArray header =
            QByteArray::fromHex("efbbbf") +
            canCsvLine({QStringLiteral("sequence"), QStringLiteral("time"), QStringLiteral("id"),
                        QStringLiteral("direction"), QStringLiteral("type"), QStringLiteral("length"),
                        QStringLiteral("data"), QStringLiteral("name"), QStringLiteral("timestamp_ms"),
                        QStringLiteral("connection_id"), QStringLiteral("channel")});
        if (m_file.write(header) != header.size()) {
            fail(m_file.errorString());
            return false;
        }
        return true;
    }
    bool finishFile()
    {
        if (!m_file.isOpen())
            return true;
        if (!m_file.flush()) {
            fail(m_file.errorString());
            return false;
        }
        m_file.close();
        m_rows = 0;
        return true;
    }
    QString m_directory;
    int m_limit;
    std::shared_ptr<std::atomic<quint64>> m_pending;
    QString m_prefix;
    QFile m_file;
    int m_part = 0;
    quint64 m_rows = 0;
    quint64 m_sequence = 0;
    bool m_failed = false;
};

CanRecorder::~CanRecorder() { stop(); }
void CanRecorder::start(const QString &directory, int rowsPerFile)
{
    stop();
    m_pending = std::make_shared<std::atomic<quint64>>(0);
    auto *worker = new CaptureWriter(directory, rowsPerFile, m_pending);
    auto *thread = new QThread;
    m_worker = worker;
    m_recording = true;
    worker->moveToThread(thread);
    connect(worker, &CaptureWriter::failed, this, [this, worker](const QString &reason, quint64 unsaved) {
        const bool current = m_worker == worker;
        if (current)
            stop();
        emit failed(reason, unsaved, current);
    });
    connect(thread, &QThread::finished, this, &CanRecorder::finished);
    connect(thread, &QThread::finished, worker, &QObject::deleteLater);
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    // Normal application exit waits for the final disk flush; ordinary stop/detach stays asynchronous.
    connect(qApp, &QCoreApplication::aboutToQuit, worker, [worker, thread] {
        worker->finish();
        thread->quit();
    });
    connect(qApp, &QCoreApplication::aboutToQuit, thread, [thread] { thread->wait(); });
    thread->start();
}
void CanRecorder::append(const QList<CanFrame> &frames, bool sent, const DataSource &source, const QString &channel,
                         std::shared_ptr<const CanSignalSet> signalSet)
{
    if (!m_recording || frames.isEmpty())
        return;
    auto *worker = static_cast<CaptureWriter *>(m_worker.data());
    *m_pending += quint64(frames.size());
    if (*m_pending > 250000) {
        m_recording = false;
        QMetaObject::invokeMethod(
            worker, [worker] { worker->fail(tr("磁盘写入跟不上 CAN 报文，自动保存已停止")); }, Qt::QueuedConnection);
        return;
    }
    QMetaObject::invokeMethod(
        worker,
        [worker, frames, sent, source, channel, signalSet = std::move(signalSet)] {
            worker->append(frames, sent, source, channel, signalSet);
        },
        Qt::QueuedConnection);
}
void CanRecorder::stop()
{
    m_recording = false;
    if (!m_worker)
        return;
    auto *worker = static_cast<CaptureWriter *>(m_worker.data());
    auto *thread = worker->thread();
    m_worker = nullptr;
    QMetaObject::invokeMethod(
        worker,
        [worker, thread] {
            worker->finish();
            thread->quit();
        },
        Qt::QueuedConnection);
}

} // namespace fhmi
#include "can_recorder.moc"
