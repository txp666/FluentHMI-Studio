#include "comm/can/can_initial_state.h"
#include "comm/can/can_link.h"
#include "core/dbc.h"

#include <QtCore/QFile>
#include <QtCore/QTemporaryDir>
#include <QtTest/QtTest>

using namespace fhmi;

namespace {

QByteArray fixtureDbc()
{
    QString text = QStringLiteral("BO_ 2147483939 IO: 8 Node\n SG_ Inputs : 0|32@1+ (1,0) [0|0] \"\" Node\n"
                                  " SG_ InputLow : 32|4@1+ (1,0) [0|0] \"\" Node\n");
    const auto outputs = [&]() {
        QString result;
        for (int bit = 36; bit < 56; ++bit) {
            result += QStringLiteral(" SG_ B%1b%2 : %3|1@1+ (1,0) [0|0] \"\" Node\n")
                          .arg(bit / 8).arg(bit % 8).arg(bit);
        }
        return result;
    };
    text += outputs();
    text += QStringLiteral("BO_ 2147483940 HMI: 8 Node\n SG_ Header : 0|8@1+ (1,0) [0|0] \"\" Node\n"
                           " SG_ Mode : 8|24@1+ (1,0) [0|0] \"\" Node\n"
                           " SG_ ModeLow : 32|4@1+ (1,0) [0|0] \"\" Node\n");
    text += outputs();
    text += QStringLiteral("BA_ \"GenSigStartValue\" SG_ 2147483940 Header 17;\n"
                           "BA_ \"GenSigStartValue\" SG_ 2147483940 ModeLow 5;\n");
    return text.toUtf8();
}

std::optional<CanSignalSet> signalSet(QByteArray bytes = fixtureDbc())
{
    auto db = Dbc::parse(bytes);
    return db ? CanSignalSet::build(std::make_shared<CanDatabase>(std::move(*db)), QString()) : std::nullopt;
}

CanFrame feedback(const QByteArray &bytes = QByteArray::fromHex("AABBCCDDA99669"))
{
    CanFrame frame;
    frame.id = 0x123;
    frame.extended = true;
    frame.data = bytes;
    return frame;
}

CanCommand command(const QString &assignments = QStringLiteral("B4b4=1"))
{
    CanCommand result;
    result.target = QStringLiteral("HMI");
    result.assignments = assignments;
    return result;
}

class RecordingBackend : public CanBackend
{
  public:
    bool opened = false;
    bool accept = true;
    QList<CanFrame> sent;
    bool open(const CanSettings &, QString *) override { opened = true; return true; }
    void close() override { opened = false; }
    bool isOpen() const override { return opened; }
    QString description() const override { return QStringLiteral("recording"); }
    bool send(const CanFrame &frame, QString *error) override
    {
        if (!accept) {
            if (error) { *error = QStringLiteral("rejected"); }
            return false;
        }
        sent.append(frame);
        emit framesSent({frame});
        return true;
    }
    void receive(const CanFrame &frame) { emit framesReceived({frame}); }
};

class RecordingLink : public CanLink
{
  public:
    RecordingBackend *driver = nullptr;
    int opens = 0;

  protected:
    std::unique_ptr<CanBackend> createBackend(const QString &, QString *) override
    {
        ++opens;
        auto backend = std::make_unique<RecordingBackend>();
        driver = backend.get();
        return backend;
    }
};

struct Fixture
{
    QTemporaryDir directory;
    QString file;
    Fixture()
    {
        file = directory.filePath(QStringLiteral("test.dbc"));
        QFile output(file);
        if (!output.open(QIODevice::WriteOnly) || output.write(fixtureDbc()) < 0) { file.clear(); }
    }
};

CanSettings settings()
{
    CanSettings result;
    result.txInitialState = QStringLiteral("HMI=IO");
    return result;
}

} // namespace

class TestCanInitialState : public QObject
{
    Q_OBJECT

  private slots:
    void firstSendRequiresFeedbackAndPreservesOnlyMappedSignals()
    {
        Fixture fixture;
        QVERIFY(!fixture.file.isEmpty());
        RecordingLink link;
        QString error;
        QVERIFY2(link.open(settings(), fixture.file, {}, {}, &error), qPrintable(error));
        QVERIFY(!link.prepare(command(), &error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(link.driver->sent.size(), 0);
        link.driver->receive(feedback()); // seven bytes suffice for every mapped output, despite DBC DLC=8
        auto prepared = link.prepare(command(), &error);
        QVERIFY2(prepared, qPrintable(error));
        QCOMPARE(prepared->frame.id, quint32(0x124));
        QVERIFY(prepared->frame.extended);
        QCOMPARE(prepared->frame.data, QByteArray::fromHex("11000000B5966900"));
        QVERIFY(link.commit(*prepared, &error));
        QCOMPARE(link.driver->sent.size(), 1);
        // Feedback still reports the previous red-off state. The next command must keep the queued red-on.
        link.driver->receive(feedback());
        prepared = link.prepare(command(QStringLiteral("B4b6=1")), &error);
        QVERIFY(prepared);
        QCOMPARE(prepared->frame.data, QByteArray::fromHex("11000000F5966900"));
        QVERIFY(link.commit(*prepared, &error));
        prepared = link.prepare(command(QStringLiteral("B4b4=0")), &error);
        QVERIFY(prepared);
        QCOMPARE(prepared->frame.data, QByteArray::fromHex("11000000E5966900"));
    }

    void rejectsRemoteErrorWrongFormatAndIncompleteFrames()
    {
        const auto definitions = signalSet();
        QVERIFY(definitions);
        QString error;
        auto state = CanInitialState::build(*definitions, QStringLiteral("HMI=IO"), &error);
        QVERIFY(state);
        auto invalid = feedback();
        for (int kind = 0; kind < 5; ++kind) {
            invalid = feedback();
            if (kind == 0) { invalid.rtr = true; }
            if (kind == 1) { invalid.error = true; }
            if (kind == 2) { invalid.extended = false; }
            if (kind == 3) { invalid.fd = true; }
            if (kind == 4) { invalid.data.resize(6); }
            state->observe(invalid, 100);
            QByteArray data(8, '\0');
            QVERIFY(!state->initialize({0x124, true}, data, 101, 2000, &error));
            QCOMPARE(data, QByteArray(8, '\0'));
        }
        state->observe(feedback(), 100);
        // An incomplete alternate packet must neither clear the valid sample nor refresh its age.
        state->observe(feedback(QByteArray::fromHex("112200000000")), 2000);
        QByteArray data(8, '\0');
        QVERIFY(state->initialize({0x124, true}, data, 2001, 2000, &error));
        QCOMPARE(data, QByteArray::fromHex("00000000A0966900"));
        QVERIFY(!state->initialize({0x124, true}, data, 2101, 2000, &error));
        QVERIFY(!state->initialize({0x124, true}, data, 99, 2000, &error));
        state->clearReceived();
        QVERIFY(!state->initialize({0x124, true}, data, 101, 2000, &error));
    }

    void reconnectRequiresNewFeedback()
    {
        Fixture fixture;
        RecordingLink link;
        QString error;
        QVERIFY(link.open(settings(), fixture.file, {}, {}, &error));
        link.driver->receive(feedback());
        auto prepared = link.prepare(command(), &error);
        QVERIFY(prepared && link.commit(*prepared, &error));
        QVERIFY(link.open(settings(), fixture.file, {}, {}, &error));
        QVERIFY(!link.prepare(command(), &error));
        QCOMPARE(link.driver->sent.size(), 0);
    }

    void rejectedSendDoesNotSeedTransmitCache()
    {
        Fixture fixture;
        RecordingLink link;
        QString error;
        QVERIFY(link.open(settings(), fixture.file, {}, {}, &error));
        link.driver->receive(feedback());
        auto prepared = link.prepare(command(), &error);
        QVERIFY(prepared);
        link.driver->accept = false;
        QVERIFY(!link.commit(*prepared, &error));
        link.driver->receive(feedback(QByteArray::fromHex("AABBCCDD400102")));
        prepared = link.prepare(command(), &error);
        QVERIFY(prepared);
        QCOMPARE(prepared->frame.data, QByteArray::fromHex("1100000055010200"));
    }

    void staleFeedbackBlocksFirstSend()
    {
        Fixture fixture;
        RecordingLink link;
        QString error;
        auto config = settings();
        config.txInitialMaxAgeMs = 1;
        QVERIFY(link.open(config, fixture.file, {}, {}, &error));
        link.driver->receive(feedback());
        QTest::qWait(10);
        QVERIFY(!link.prepare(command(), &error));
        QCOMPARE(link.driver->sent.size(), 0);
        link.driver->receive(feedback());
        QVERIFY(link.prepare(command(), &error));
    }

    void disabledModeKeepsExistingDbcInitialValues()
    {
        Fixture fixture;
        RecordingLink link;
        QString error;
        QVERIFY(link.open(CanSettings{}, fixture.file, {}, {}, &error));
        link.driver->receive(feedback());
        const auto prepared = link.prepare(command(), &error);
        QVERIFY(prepared);
        QCOMPARE(prepared->frame.data, QByteArray::fromHex("1100000015000000"));
    }

    void invalidConfigurationRejectedBeforeOpeningHardware()
    {
        Fixture fixture;
        for (const QString &mapping : {QStringLiteral("HMI"), QStringLiteral("HMI=unknown"),
                                      QStringLiteral("HMI=IO\nHMI=IO"), QStringLiteral("HMI=125")}) {
            RecordingLink link;
            auto config = settings();
            config.txInitialState = mapping;
            QString error;
            QVERIFY(!link.open(config, fixture.file, {}, {}, &error));
            QVERIFY(!error.isEmpty());
            QCOMPARE(link.opens, 0);
        }
        RecordingLink filtered;
        QString error;
        QVERIFY(!filtered.open(settings(), fixture.file, {}, QStringLiteral("00000124"), &error));
        QCOMPARE(filtered.opens, 0);
        const auto original = fixtureDbc();
        const int hmi = original.indexOf("BO_ 2147483940 HMI");
        auto incompatible = original.left(hmi) + QByteArray(original.mid(hmi)).replace("36|1@1+ (1,0)", "36|1@1+ (2,0)");
        const auto definitions = signalSet(incompatible);
        QVERIFY(definitions);
        QVERIFY(!CanInitialState::build(*definitions, QStringLiteral("HMI=IO"), &error));
    }

    void synchronousNewCommandIsNotOverwritten()
    {
        Fixture fixture;
        RecordingLink link;
        QString error;
        QVERIFY(link.open(settings(), fixture.file, {}, {}, &error));
        link.driver->receive(feedback());
        bool nested = false;
        connect(&link, &CanLink::canFrames, &link, [&](const QList<CanFrame> &, bool sent) {
            if (sent && !nested) {
                nested = true;
                const auto next = link.prepare(command(QStringLiteral("B4b6=1")), &error);
                QVERIFY(next);
                QVERIFY(link.commit(*next, &error));
            }
        });
        auto first = link.prepare(command(), &error);
        QVERIFY(first && link.commit(*first, &error));
        auto last = link.prepare(command(QStringLiteral("B4b4=0")), &error);
        QVERIFY(last);
        QCOMPARE(last->frame.data, QByteArray::fromHex("11000000E5966900"));
    }
};

QTEST_GUILESS_MAIN(TestCanInitialState)
#include "tst_can_initial_state.moc"
