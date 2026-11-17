#include "comm/can/can_diagnostics.h"
#include "comm/can/can_link.h"
#include "comm/can/can_polling_backend.h"

#include <QtCore/QDateTime>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QSemaphore>
#include <QtCore/QTemporaryDir>
#include <QtCore/QThread>
#include <QtTest/QtTest>

#include <atomic>
#include <thread>

using namespace fhmi;

namespace {

class MockBackend : public CanPollingBackend
{
  public:
    std::atomic<int> stateCalls{0};
    bool failRead = false;

    ~MockBackend() override { close(); }
    bool open(const CanSettings &, QString *) override
    {
        m_emitted = false;
        m_open = true;
        startWorker();
        return true;
    }
    void close() override
    {
        stopWorker();
        m_open = false;
    }
    bool isOpen() const override { return m_open; }
    QString description() const override { return QStringLiteral("diagnostic mock"); }

  protected:
    bool readFrames(QList<CanFrame> &out, QString *error) override
    {
        if (failRead) {
            *error = QStringLiteral("mock device disconnected");
            return false;
        }
        if (!m_emitted) {
            CanFrame frame;
            frame.id = 0x64;
            frame.data = QByteArray::fromHex("0001020304050607");
            out.append(frame);
            m_emitted = true;
        }
        return true;
    }
    bool writeFrame(const CanFrame &, QString *) override { return true; }
    CanBusState readBusState() override
    {
        ++stateCalls;
        return CanBusState::Normal;
    }

  private:
    bool m_open = false;
    bool m_emitted = false;
};

class MockLink : public CanLink
{
  public:
    MockBackend *driver = nullptr;

  protected:
    std::unique_ptr<CanBackend> createBackend(const QString &, QString *) override
    {
        auto backend = std::make_unique<MockBackend>();
        driver = backend.get();
        return backend;
    }
};

QList<QJsonObject> records()
{
    QFile file(CanDiagnostics::logFilePath());
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    QList<QJsonObject> out;
    while (!file.atEnd()) {
        const auto document = QJsonDocument::fromJson(file.readLine());
        if (document.isObject()) {
            out.append(document.object());
        }
    }
    return out;
}

QJsonObject findRecord(const char *event, const QString &key = {}, const QString &value = {})
{
    const auto all = records();
    for (auto it = all.crbegin(); it != all.crend(); ++it) {
        if (it->value(QStringLiteral("event")).toString() == QLatin1String(event) &&
            (key.isEmpty() || it->value(key).toString() == value)) {
            return *it;
        }
    }
    return {};
}

} // namespace

class TestCanDiagnostics : public QObject
{
    Q_OBJECT

  private slots:
    void initTestCase()
    {
        QVERIFY(CanDiagnostics::BuildEnabled);
        QVERIFY(m_directory.isValid());
        m_oldDirectory = qgetenv("FHMI_CAN_TRACE_DIR");
        m_oldQueries = qgetenv("FHMI_CAN_STATUS_QUERIES");
        qputenv("FHMI_CAN_TRACE_DIR", m_directory.path().toUtf8());
        qunsetenv("FHMI_CAN_STATUS_QUERIES");
        QVERIFY(CanDiagnostics::enabled());
        QVERIFY(CanDiagnostics::logFilePath().startsWith(m_directory.path()));
        QVERIFY(CanDiagnostics::flush());
        const auto session = findRecord("session");
        QVERIFY(!session.isEmpty());
        QCOMPARE(session.value("status_queries_enabled").toBool(), false);
    }

    void cleanup() { qunsetenv("FHMI_CAN_STATUS_QUERIES"); }

    void defaultSkipsStatusAndReceives()
    {
        MockBackend backend;
        QSignalSpy received(&backend, &CanBackend::framesReceived);
        QVERIFY(backend.open({}, nullptr));
        QTRY_COMPARE(received.size(), 1);
        QTest::qWait(550); // crosses the original periodic query interval
        backend.close();
        QCOMPARE(backend.stateCalls.load(), 0);
        const auto frames = qvariant_cast<QList<CanFrame>>(received.front().front());
        QCOMPARE(frames.size(), 1);
        QCOMPARE(frames.front().id, quint32(0x64));
        QVERIFY(frames.front().timestampMs > 0); // existing software timestamp behavior is retained
        QVERIFY(frames.front().receivedAtMs >= 0);
    }

    void explicitStatusQueriesAreRestored()
    {
        qputenv("FHMI_CAN_STATUS_QUERIES", "1");
        MockBackend backend;
        QVERIFY(backend.open({}, nullptr));
        QTRY_VERIFY(backend.stateCalls.load() >= 2);
        backend.close();
        QVERIFY(CanDiagnostics::flush());
        const auto start = findRecord("worker_start", "backend", QString::number(quintptr(&backend), 16));
        QCOMPARE(start.value("status_queries_enabled").toBool(), true);
    }

    void receiveFailureIsNotHiddenByDisabledStatus()
    {
        MockBackend backend;
        backend.failRead = true;
        QSignalSpy failed(&backend, &CanBackend::failed);
        QSignalSpy received(&backend, &CanBackend::framesReceived);
        QVERIFY(backend.open({}, nullptr));
        QTRY_COMPARE(failed.size(), 1);
        backend.close();
        QCOMPARE(received.size(), 0);
        QCOMPARE(backend.stateCalls.load(), 0);
        QCOMPARE(failed.front().front().toString(), QStringLiteral("mock device disconnected"));
    }

    void blockedCallEnterIsVisibleBeforeReturn()
    {
        QSemaphore entered;
        QSemaphore resume;
        int result = 0;
        std::thread worker([&] {
            result = CanDiagnostics::invoke("MockBlockingDriver", [&] {
                entered.release();
                resume.acquire();
                return 37;
            });
        });
        // Also release/join on a failing Qt assertion; never leave a test thread hung.
        struct Join
        {
            QSemaphore &resume;
            std::thread &worker;
            ~Join()
            {
                resume.release();
                if (worker.joinable())
                    worker.join();
            }
        } join{resume, worker};
        QVERIFY(entered.tryAcquire(1, 2000));
        // Deliberately no flush() here: the independent writer must persist enter during the blocked API.
        QTRY_VERIFY_WITH_TIMEOUT(!findRecord("driver_enter", "api", "MockBlockingDriver").isEmpty(), 2000);
        QVERIFY(findRecord("driver_exit", "api", "MockBlockingDriver").isEmpty());
        QTest::qWait(80);
        resume.release();
        worker.join();
        QVERIFY(CanDiagnostics::flush());
        QCOMPARE(result, 37);
        const auto entry = findRecord("driver_enter", "api", "MockBlockingDriver");
        const auto exit = findRecord("driver_exit", "api", "MockBlockingDriver");
        QCOMPARE(entry.value("call_id"), exit.value("call_id"));
        QCOMPARE(exit.value("result").toInteger(), qint64(37));
        QVERIFY(exit.value("duration_us").toInteger() >= 80000);
    }

    void workerAndGuiTimesDistinguishEventLoopDelay()
    {
        MockLink link;
        QSignalSpy received(&link, &CanLink::canFrames);
        const QString linkId = QString::number(quintptr(&link), 16);
        QVERIFY(link.open({}, {}, {}, {}, nullptr));
        const QString id = QString::number(quintptr(link.driver), 16);
        // Block only this test's GUI thread, while the actual production worker keeps running.
        QThread::msleep(150);
        QVERIFY(CanDiagnostics::flush());
        const auto read = findRecord("worker_frames_read", "backend", id);
        const auto posted = findRecord("worker_post", "backend", id);
        QVERIFY(!read.isEmpty());
        QVERIFY(!posted.isEmpty());
        QCOMPARE(received.size(), 0);
        QTRY_COMPARE(received.size(), 1);
        const auto frame = qvariant_cast<QList<CanFrame>>(received.front().front()).front();
        QVERIFY(Can::monotonicMs() - frame.receivedAtMs >= 50);
        link.close();
        QVERIFY(CanDiagnostics::flush());
        const auto relayed = findRecord("backend_delivery", "backend", id);
        const auto delivery = findRecord("gui_delivery", "link", linkId);
        QCOMPARE(read.value("count").toInteger(), qint64(1));
        QCOMPARE(relayed.value("received").toInteger(), qint64(1));
        QCOMPARE(delivery.value("received").toInteger(), qint64(1));
        QVERIFY(posted.value("monotonic_us").toInteger() >= read.value("monotonic_us").toInteger());
        QVERIFY(delivery.value("monotonic_us").toInteger() - read.value("monotonic_us").toInteger() >= 50000);
    }

    void flushPersistsAllAcceptedRecords()
    {
        for (int i = 0; i < 500; ++i) {
            CanDiagnostics::record("flush_test", {{"index", i}});
        }
        QVERIFY(CanDiagnostics::flush());
        int expected = 0;
        for (const auto &record : records()) {
            if (record.value("event").toString() == QLatin1String("flush_test")) {
                QCOMPARE(record.value("index").toInt(), expected++);
            }
        }
        QCOMPARE(expected, 500);
    }

    void cleanupTestCase()
    {
        QVERIFY(CanDiagnostics::flush());
        if (m_oldDirectory.isNull())
            qunsetenv("FHMI_CAN_TRACE_DIR");
        else
            qputenv("FHMI_CAN_TRACE_DIR", m_oldDirectory);
        if (m_oldQueries.isNull())
            qunsetenv("FHMI_CAN_STATUS_QUERIES");
        else
            qputenv("FHMI_CAN_STATUS_QUERIES", m_oldQueries);
    }

  private:
    QTemporaryDir m_directory;
    QByteArray m_oldDirectory;
    QByteArray m_oldQueries;
};

QTEST_GUILESS_MAIN(TestCanDiagnostics)
#include "tst_can_diagnostics.moc"
