#include "comm/can/can_diagnostics.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtCore/QDebug>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QStandardPaths>
#include <QtCore/QThread>
#include <QtCore/QUuid>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace fhmi::CanDiagnostics {

bool enabled() { return BuildEnabled; }

bool statusQueriesEnabled()
{
    if constexpr (BuildEnabled) {
        return qgetenv("FHMI_CAN_STATUS_QUERIES") == "1";
    }
    return true;
}

qint64 monotonicUs()
{
    using Clock = std::chrono::steady_clock;
    static const auto origin = Clock::now();
    return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - origin).count();
}

#if defined(FHMI_CAN_DIAGNOSTICS) && FHMI_CAN_DIAGNOSTICS
namespace {

QJsonObject stamped(const char *event, QJsonObject fields)
{
    fields.insert(QStringLiteral("event"), QLatin1String(event));
    fields.insert(QStringLiteral("monotonic_us"), monotonicUs());
    fields.insert(QStringLiteral("thread"), QString::number(quintptr(QThread::currentThreadId()), 16));
    return fields;
}

// No Qt event loop is needed by the writer. Producers never perform file IO or JSON serialization.
// Overflow is visible in a trace_dropped record rather than allowing logging to consume unlimited memory.
class Writer
{
  public:
    Writer()
    {
        const QString requested = qEnvironmentVariable("FHMI_CAN_TRACE_DIR");
        QString fallback = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
        if (fallback.isEmpty()) {
            fallback = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
        }
        fallback = QDir(fallback).filePath(QStringLiteral("can-diagnostics"));
        const QString name = QStringLiteral("can-%1-%2-%3.jsonl")
                                 .arg(QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd-HHmmss-zzz")))
                                 .arg(QCoreApplication::applicationPid())
                                 .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
        const QString directory = requested.isEmpty() ? fallback : requested;
        m_path = QDir(directory).absoluteFilePath(name);
        m_thread = std::thread([this, directory, fallback, name] { run(directory, fallback, name); });
        std::unique_lock<std::mutex> lock(m_mutex);
        m_done.wait(lock, [this] { return m_ready; });
        lock.unlock();
        enqueue(stamped("session", {{"pid", QCoreApplication::applicationPid()},
                                    {"utc", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
                                    {"application", QCoreApplication::applicationName()},
                                    {"version", QCoreApplication::applicationVersion()},
                                    {"diagnostic_build", true},
                                    {"status_queries_enabled", statusQueriesEnabled()},
                                    {"status_queries_environment", qEnvironmentVariable("FHMI_CAN_STATUS_QUERIES")},
                                    {"queue_capacity", qint64(kCapacity)},
                                    {"path", m_path}}));
        if (!m_openWarning.isEmpty()) {
            enqueue(stamped("trace_error", {{"message", m_openWarning}}));
        }
    }

    ~Writer()
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stopping = true;
        }
        m_wake.notify_one();
        m_thread.join();
    }

    void enqueue(QJsonObject record)
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_stopping || !m_ok) {
                return;
            }
            if (m_queue.size() >= kCapacity) {
                ++m_dropped;
            } else {
                record.insert(QStringLiteral("sequence"), qint64(++m_accepted));
                m_queue.push_back(std::move(record));
            }
        }
        m_wake.notify_one();
    }

    QString path() const { return m_path; } // fixed before the constructor returns

    bool flush()
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        const quint64 target = ++m_flushRequested;
        m_wake.notify_one();
        m_done.wait(lock, [this, target] { return !m_ok || m_flushCompleted >= target; });
        return m_ok;
    }

  private:
    void run(const QString &directory, const QString &fallback, const QString &name)
    {
        QFile file;
        const auto openIn = [&](const QString &dir) {
            if (!QDir().mkpath(dir)) {
                return false;
            }
            file.setFileName(QDir(dir).absoluteFilePath(name));
            return file.open(QIODevice::WriteOnly | QIODevice::NewOnly);
        };
        bool opened = openIn(directory);
        if (!opened && directory != fallback) {
            m_openWarning =
                QStringLiteral("Cannot open trace in %1: %2; trying %3").arg(directory, file.errorString(), fallback);
            qWarning().noquote() << m_openWarning;
            opened = openIn(fallback);
        }
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_path = file.fileName();
            m_ok = opened;
            m_ready = true;
        }
        m_done.notify_all();
        if (!opened) {
            qWarning().noquote() << "CAN diagnostics cannot open trace:" << file.fileName() << file.errorString();
            return;
        }
        while (true) {
            std::deque<QJsonObject> batch;
            quint64 dropped = 0;
            quint64 flushRequest = 0;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_wake.wait(lock, [this] {
                    return m_stopping || !m_queue.empty() || m_dropped || m_flushRequested > m_flushCompleted;
                });
                if (m_stopping && m_queue.empty() && !m_dropped) {
                    break;
                }
                batch.swap(m_queue);
                dropped = std::exchange(m_dropped, 0);
                flushRequest = m_flushRequested;
            }
            if (dropped) {
                batch.push_front(stamped("trace_dropped", {{"count", qint64(dropped)}}));
            }
            QByteArray bytes;
            for (const QJsonObject &record : batch) {
                bytes += QJsonDocument(record).toJson(QJsonDocument::Compact);
                bytes += '\n';
            }
            // fflush, not fsync: makes enter records readable by another process during a blocked call.
            const bool written = file.write(bytes) == bytes.size() && file.flush();
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_ok = written;
                m_flushCompleted = flushRequest;
            }
            m_done.notify_all();
            if (!written) {
                qWarning().noquote() << "CAN diagnostics trace write failed:" << file.fileName() << file.errorString();
                break;
            }
        }
    }

    static constexpr size_t kCapacity = 8192;
    QString m_path;
    QString m_openWarning;
    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::condition_variable m_done;
    std::deque<QJsonObject> m_queue;
    std::thread m_thread;
    quint64 m_accepted = 0;
    quint64 m_flushRequested = 0;
    quint64 m_flushCompleted = 0;
    quint64 m_dropped = 0;
    bool m_ready = false;
    bool m_ok = true;
    bool m_stopping = false;
};

Writer &writer()
{
    static Writer instance;
    return instance;
}

std::atomic<qint64> nextCallId{0};

} // namespace
#endif

QString logFilePath()
{
#if defined(FHMI_CAN_DIAGNOSTICS) && FHMI_CAN_DIAGNOSTICS
    return writer().path();
#else
    return {};
#endif
}

void record(const char *event, QJsonObject fields)
{
#if defined(FHMI_CAN_DIAGNOSTICS) && FHMI_CAN_DIAGNOSTICS
    writer().enqueue(stamped(event, std::move(fields)));
#else
    Q_UNUSED(event);
    Q_UNUSED(fields);
#endif
}

bool flush()
{
#if defined(FHMI_CAN_DIAGNOSTICS) && FHMI_CAN_DIAGNOSTICS
    return writer().flush();
#else
    return true;
#endif
}

Call::Call(const char *name, QJsonObject fields) : m_name(QLatin1String(name))
{
#if defined(FHMI_CAN_DIAGNOSTICS) && FHMI_CAN_DIAGNOSTICS
    m_id = ++nextCallId;
    fields.insert(QStringLiteral("api"), m_name);
    fields.insert(QStringLiteral("call_id"), m_id);
    record("driver_enter", std::move(fields));
    m_startedUs = monotonicUs();
#else
    Q_UNUSED(fields);
#endif
}

Call::~Call()
{
    if constexpr (BuildEnabled) {
        if (!m_finished) {
            record("driver_abandoned",
                   {{"api", m_name}, {"call_id", m_id}, {"duration_us", monotonicUs() - m_startedUs}});
        }
    }
}

void Call::finish(const QJsonValue &result)
{
    if constexpr (BuildEnabled) {
        const qint64 duration = monotonicUs() - m_startedUs;
        m_finished = true;
        record("driver_exit", {{"api", m_name}, {"call_id", m_id}, {"result", result}, {"duration_us", duration}});
    } else {
        Q_UNUSED(result);
    }
}

} // namespace fhmi::CanDiagnostics
