#include "comm/can/zlg_backend.h"
#include "comm/can/zlg_timing.h"

#include <QtTest/QtTest>

#include <limits>
#include <cmath>

using namespace fhmi;
using namespace fhmi::zlg;

namespace {

const Model &model(const char *name) { return *findModel(name, qsizetype(qstrlen(name))); }

CanSettings settings(const char *name, const QString &custom = QString())
{
    CanSettings result;
    result.zlgModel = QLatin1String(name);
    result.customTiming = custom;
    return result;
}

} // namespace

class TestZlgTiming : public QObject
{
    Q_OBJECT

  private slots:
    void calculatorFindsGoldenTimings()
    {
        const auto rows = sjaTimingCandidates(500000, 0.875, 1, 0);
        QCOMPARE(rows.size(), 14);
        QCOMPARE(rows.first().btr, quint16(0x001C));
        QCOMPARE(rows.first().bitrate, 500000.0);
        QCOMPARE(rows.first().timing.samplePoint(), 0.875);
        QCOMPARE(rows.first().bitrateErrorPpm, 0.0);
        QCOMPARE(sjaTimingCandidates(500000, 0.75, 1, 0).first().btr, quint16(0x003A));
        QCOMPARE(sjaTimingCandidates(500000, 0.875, 4, 0).first().btr, quint16(0xC03A));
        QCOMPARE(sjaTimingCandidates(800000, 0.875, 1, 0).first().btr, quint16(0x0007));
    }

    void calculatorHandlesToleranceAndFractionalRates()
    {
        QVERIFY(sjaTimingCandidates(83333, 0.875, 1, 0).isEmpty());
        QVERIFY(sjaTimingCandidates(83333, 0.875, 1, 4).isEmpty());
        const auto approximate = sjaTimingCandidates(83333, 0.875, 1, 5);
        QVERIFY(!approximate.isEmpty());
        QCOMPARE(approximate.first().btr, quint16(0x051C));
        QVERIFY(std::abs(approximate.first().bitrate - 8000000.0 / 96) < 1e-9);
        QVERIFY(approximate.first().bitrateErrorPpm > 4 && approximate.first().bitrateErrorPpm < 5);
        const auto exact = sjaTimingCandidates(8000000.0 / 96, 0.875, 1, 0);
        QVERIFY(!exact.isEmpty());
        QCOMPARE(exact.first().btr, quint16(0x051C));
        QCOMPARE(exact.first().bitrateErrorPpm, 0.0);
    }

    void calculatorRespectsControllerLimits()
    {
        for (const double rate : {5000.0, 10000.0, 125000.0, 500000.0, 1000000.0}) {
            for (const int sjw : {1, 4}) {
                QSet<quint16> seen;
                for (const auto &row : sjaTimingCandidates(rate, 0.875, sjw)) {
                    QVERIFY(!seen.contains(row.btr));
                    seen.insert(row.btr);
                    QString error;
                    QVERIFY2(CanBitTiming::check(row.timing, CanBitTiming::Sja1000, &error), qPrintable(error));
                    QVERIFY(row.timing.tq() >= 8);
                    QCOMPARE(row.timing.sjw, sjw);
                    QVERIFY(row.bitrate <= 1000000 && row.bitrateErrorPpm <= 1000 + 1e-9);
                    QCOMPARE(CanBitTiming::sja1000Btr(row.timing), row.btr);
                }
            }
        }
    }

    void calculatorRejectsInvalidInputs()
    {
        const double nan = std::numeric_limits<double>::quiet_NaN();
        for (const double rate : {0.0, -1.0, 1000001.0, nan, std::numeric_limits<double>::infinity()}) {
            QVERIFY(sjaTimingCandidates(rate, 0.875, 1).isEmpty());
        }
        for (const double sample : {0.0, 1.0, nan}) {
            QVERIFY(sjaTimingCandidates(500000, sample, 1).isEmpty());
        }
        QVERIFY(sjaTimingCandidates(500000, 0.875, 0).isEmpty());
        QVERIFY(sjaTimingCandidates(500000, 0.875, 5).isEmpty());
        QVERIFY(sjaTimingCandidates(500000, 0.875, 1, -1).isEmpty());
        QVERIFY(sjaTimingCandidates(500000, 0.875, 1, 10001).isEmpty());
    }

    void classicRegisters_data()
    {
        QTest::addColumn<QString>("custom");
        QTest::addColumn<double>("samplePoint");
        QTest::addColumn<int>("expectedBtr");
        QTest::newRow("standard-500k") << QString() << 0.875 << 0x001C;
        QTest::newRow("500k-75-percent") << QString() << 0.75 << 0x003A;
        QTest::newRow("two-hex-bytes") << QStringLiteral("00,1C") << 0.75 << 0x001C;
        QTest::newRow("prefixed-hex") << QStringLiteral(" (0x00, 0x1c) ") << 0.75 << 0x001C;
        QTest::newRow("explicit-brp") << QStringLiteral("1,13,2,1") << 0.75 << 0x001C;
        QTest::newRow("nonstandard-200k") << QStringLiteral("2,16,3,1") << 0.75 << 0x012F;
        QTest::newRow("zlg-calculator") << QStringLiteral("500Kbps(87%),(00,1C)") << 0.75 << 0x001C;
        QTest::newRow("triple-sampling-preserved") << QStringLiteral("00,9C") << 0.75 << 0x009C;
    }

    void classicRegisters()
    {
        QFETCH(QString, custom);
        QFETCH(double, samplePoint);
        QFETCH(int, expectedBtr);
        CanSettings input = settings("USBCAN-II", custom);
        input.samplePoint = samplePoint;
        QString error;
        const auto timing = channelTiming(model("USBCAN-II"), input, &error);
        QVERIFY2(timing, qPrintable(error));
        QVERIFY(timing->btr);
        QCOMPARE(int(*timing->btr), expectedBtr);
        if (expectedBtr == 0x001C) {
            QVERIFY(timing->description.contains(QStringLiteral("500k")));
            QVERIFY(timing->description.contains(QStringLiteral("87.5%"))); // actual, not rounded calculator label
        }
        ZCAN_CHANNEL_INIT_CONFIG config{};
        int propertyCalls = 0;
        QVERIFY(applyChannelTiming(*timing, config, [&](const char *, const QString &) {
            ++propertyCalls;
            return false; // compatible DLLs need no property API for SJA1000 timing
        }));
        QCOMPARE(propertyCalls, 0);
        QCOMPARE(int(config.config.can.timing0), expectedBtr >> 8);
        QCOMPARE(int(config.config.can.timing1), expectedBtr & 255);
    }

    void reportsAchievableSamplePoint()
    {
        auto input = settings("USBCAN-I");
        input.bitrate = 800000;
        input.samplePoint = 0.875;
        const auto timing = channelTiming(model("USBCAN-I"), input);
        QVERIFY(timing);
        QCOMPARE(*timing->btr, quint16(0x0007));
        QVERIFY(timing->description.contains(QStringLiteral("90%")));
        QVERIFY(!timing->description.contains(QStringLiteral("87.5%")));
    }

    void nativeCalculatorValues_data()
    {
        QTest::addColumn<QByteArray>("name");
        QTest::addColumn<QString>("custom");
        // Values from the official SDK's device property files and manual V1.21. Formats differ by model.
        QTest::newRow("EU-register") << QByteArray("USBCAN-E-U") << QStringLiteral("1.0Mbps(72%),(004B0001)");
        QTest::newRow("2EU-register") << QByteArray("USBCAN-2E-U") << QStringLiteral("1.0Mbps(72%),(004B0001)");
        QTest::newRow("4EU-decimal-bps") << QByteArray("USBCAN-4E-U") << QStringLiteral("200000");
        QTest::newRow("8EU-two-bytes") << QByteArray("USBCAN-8E-U") << QStringLiteral("1.0Mbps(62%),(00,23)");
        for (const QByteArray name : {QByteArray("USBCANFD-100U-mini"), QByteArray("USBCANFD-100U"),
                                      QByteArray("USBCANFD-200U"), QByteArray("USBCANFD-400U")}) {
            QTest::newRow(name.constData())
                << name << QStringLiteral("1.0Mbps(75%),5.0Mbps(75%),(60,0080040D,00800001)");
        }
        QTest::newRow("800U-eight-parameters")
            << QByteArray("USBCANFD-800U") << QStringLiteral("250Kbps(87%),1.0Mbps(75%),(13,2,2,10,5,2,2,5)");
    }

    void nativeCalculatorValues()
    {
        QFETCH(QByteArray, name);
        QFETCH(QString, custom);
        auto input = settings(name.constData(), custom);
        input.fd = model(name.constData()).family == Family::CanFd;
        input.bitrate = 123456; // custom timing overrides unavailable standard-list selections
        input.dataBitrate = 765432;
        QString error;
        const auto timing = channelTiming(model(name.constData()), input, &error);
        QVERIFY2(timing, qPrintable(error));
        QVERIFY(!timing->btr);
        ZCAN_CHANNEL_INIT_CONFIG config{};
        QStringList calls;
        QVERIFY(applyChannelTiming(*timing, config, [&](const char *property, const QString &value) {
            calls << QLatin1String(property) + QLatin1Char('=') + value;
            return true;
        }));
        QCOMPARE(calls, QStringList{QStringLiteral("baud_rate_custom=") + custom});
        // Neither a nominal nor a data standard-rate property is sent after the custom property.
        calls.clear();
        QVERIFY(!applyChannelTiming(*timing, config, [&](const char *property, const QString &) {
            calls << QLatin1String(property);
            return false;
        }));
        QCOMPARE(calls, QStringList{QStringLiteral("baud_rate_custom")});
    }

    void standardPropertiesRetained()
    {
        auto input = settings("USBCANFD-200U");
        input.fd = true;
        const auto timing = channelTiming(model("USBCANFD-200U"), input);
        QVERIFY(timing);
        ZCAN_CHANNEL_INIT_CONFIG config{};
        QStringList calls;
        QVERIFY(applyChannelTiming(*timing, config, [&](const char *property, const QString &value) {
            calls << QLatin1String(property) + QLatin1Char('=') + value;
            return true;
        }));
        QCOMPARE(calls, (QStringList{QStringLiteral("canfd_abit_baud_rate=500000"),
                                     QStringLiteral("canfd_dbit_baud_rate=2000000")}));
        calls.clear();
        QVERIFY(!applyChannelTiming(*timing, config, [&](const char *property, const QString &) {
            calls << QLatin1String(property);
            return false;
        }));
        QCOMPARE(calls, QStringList{QStringLiteral("canfd_abit_baud_rate")});
        const auto classic = channelTiming(model("USBCAN-2E-U"), settings("USBCAN-2E-U"));
        QVERIFY(classic);
        QCOMPARE(classic->properties.size(), 1);
        QCOMPARE(classic->properties.first().name, QByteArray("baud_rate"));
        QCOMPARE(classic->properties.first().value, QStringLiteral("500000"));
    }

    void rejectsInvalidBeforeLoadingDriver_data()
    {
        QTest::addColumn<QByteArray>("name");
        QTest::addColumn<QString>("custom");
        QTest::newRow("brp-too-large") << QByteArray("USBCAN-II") << QStringLiteral("65,13,2,1");
        QTest::newRow("sjw-exceeds-tseg2") << QByteArray("USBCAN-II") << QStringLiteral("1,13,2,3");
        QTest::newRow("raw-sjw-exceeds-tseg2") << QByteArray("USBCAN-II") << QStringLiteral("C0,1C");
        QTest::newRow("unclosed-byte-pair") << QByteArray("USBCAN-II") << QStringLiteral("(00,1C");
        QTest::newRow("extra-phase-on-classic") << QByteArray("USBCAN-II") << QStringLiteral("1,13,2,1;1,7,2,1");
        QTest::newRow("over-one-megabit") << QByteArray("USBCAN-II") << QStringLiteral("00,00");
        QTest::newRow("fd-registers-on-sja")
            << QByteArray("USBCAN-II") << QStringLiteral("1Mbps(75%),5Mbps(75%),(60,0080040D,00800001)");
        QTest::newRow("sja-registers-on-eu") << QByteArray("USBCAN-2E-U") << QStringLiteral("500Kbps(87%),(00,1C)");
        QTest::newRow("wrong-fd-model") << QByteArray("USBCANFD-800U")
                                        << QStringLiteral("1Mbps(75%),5Mbps(75%),(60,0080040D,00800001)");
        QTest::newRow("pcan-brp-on-fd") << QByteArray("USBCANFD-200U") << QStringLiteral("1,13,2,1;1,7,2,1");
        QTest::newRow("4eu-needs-decimal") << QByteArray("USBCAN-4E-U") << QStringLiteral("1Mbps(72%),(004B0001)");
        QTest::newRow("bad-hex") << QByteArray("USBCAN-2E-U") << QStringLiteral("1Mbps(72%),(xxxx)");
        QTest::newRow("embedded-newline")
            << QByteArray("USBCAN-2E-U") << QStringLiteral("1Mbps(72%),(004B0001)\nextra");
    }

    void rejectsInvalidBeforeLoadingDriver()
    {
        QFETCH(QByteArray, name);
        QFETCH(QString, custom);
        QString error;
        const auto input = settings(name.constData(), custom);
        QVERIFY(!channelTiming(model(name.constData()), input, &error));
        QVERIFY(!error.isEmpty());
        ZlgBackend backend;
        QString openError;
        QVERIFY(!backend.open(input, &openError));
        QCOMPARE(openError, error); // no missing-DLL error: configuration is rejected before opening hardware
    }

    void validatesAutomaticInputsAndCapabilities()
    {
        auto input = settings("USBCAN-II");
        for (const double sp : {0.0, 1.0, std::numeric_limits<double>::quiet_NaN()}) {
            input.samplePoint = sp;
            QVERIFY(!channelTiming(model("USBCAN-II"), input));
        }
        input = settings("USBCAN-II");
        input.bitrate = 123456;
        QVERIFY(!channelTiming(model("USBCAN-II"), input));
        input.fd = true;
        QVERIFY(!channelTiming(model("USBCAN-II"), input));
        QVERIFY(ZlgBackend::caps(QStringLiteral("USBCAN-II")).samplePoint);
        QVERIFY(ZlgBackend::caps(QStringLiteral("USBCAN-II")).customTiming);
        QVERIFY(!ZlgBackend::caps(QStringLiteral("USBCANFD-200U")).samplePoint);
        QVERIFY(ZlgBackend::caps(QStringLiteral("USBCANFD-200U")).customTiming);
        QVERIFY(!ZlgBackend::caps(QStringLiteral("USBCAN-4E-U")).samplePoint);
        QVERIFY(!ZlgBackend::caps(QStringLiteral("unknown")).customTiming);
    }
};

QTEST_APPLESS_MAIN(TestZlgTiming)
#include "tst_zlg_timing.moc"
