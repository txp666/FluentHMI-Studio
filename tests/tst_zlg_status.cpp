#include "comm/can/zlg_status_internal.h"

#include <QtTest/QtTest>

using namespace fhmi;
using namespace fhmi::zlg;

namespace {

struct Driver
{
    UINT errorCode = 0;
    bool errorOk = true;
    bool statusOk = true;
    BYTE receiveCounter = 0;
    BYTE transmitCounter = 0;
    QStringList calls;

    CanBusState query(Family family)
    {
        return detail::readBusState(
            family,
            [&](ZCAN_CHANNEL_ERR_INFO *info) {
                calls << QStringLiteral("error");
                info->error_code = errorCode;
                return errorOk ? STATUS_OK : STATUS_ERR;
            },
            [&](ZCAN_CHANNEL_STATUS *status) {
                calls << QStringLiteral("status");
                status->regRECounter = receiveCounter;
                status->regTECounter = transmitCounter;
                return statusOk ? STATUS_OK : STATUS_ERR;
            });
    }
};

} // namespace

class TestZlgStatus : public QObject
{
    Q_OBJECT

  private slots:
    void classicKeepsErrorQueryAndNeverReadsStatus_data()
    {
        QTest::addColumn<quint32>("errorCode");
        QTest::addColumn<CanBusState>("expected");
        QTest::newRow("normal") << quint32(0) << CanBusState::Normal;
        QTest::newRow("warning") << ZCAN_ERROR_CAN_ERRALARM << CanBusState::Warning;
        QTest::newRow("passive") << ZCAN_ERROR_CAN_PASSIVE << CanBusState::Passive;
        QTest::newRow("bus-off") << ZCAN_ERROR_CAN_BUSOFF << CanBusState::BusOff;
        QTest::newRow("passive-over-warning")
            << (ZCAN_ERROR_CAN_PASSIVE | ZCAN_ERROR_CAN_ERRALARM) << CanBusState::Passive;
        QTest::newRow("bus-off-over-other-flags")
            << (ZCAN_ERROR_CAN_BUSOFF | ZCAN_ERROR_CAN_PASSIVE | ZCAN_ERROR_CAN_ERRALARM) << CanBusState::BusOff;
    }

    void classicKeepsErrorQueryAndNeverReadsStatus()
    {
        QFETCH(quint32, errorCode);
        QFETCH(CanBusState, expected);
        Driver driver;
        driver.errorCode = errorCode;
        driver.receiveCounter = 255; // would override Normal/Warning if the forbidden callback ran
        QCOMPARE(driver.query(Family::Sja1000), expected);
        QCOMPARE(driver.calls, QStringList{QStringLiteral("error")});
    }

    void failedClassicErrorQueryDoesNotFallBackToBlockingStatus()
    {
        Driver driver;
        driver.errorOk = false;
        driver.errorCode = ZCAN_ERROR_CAN_BUSOFF; // output of a failed call must not be interpreted
        driver.receiveCounter = 255;
        QCOMPARE(driver.query(Family::Sja1000), CanBusState::Normal);
        QCOMPARE(driver.calls, QStringList{QStringLiteral("error")});
    }

    void otherFamiliesKeepCounterStatus_data()
    {
        QTest::addColumn<int>("family");
        QTest::addColumn<int>("receiveCounter");
        QTest::addColumn<int>("transmitCounter");
        QTest::addColumn<CanBusState>("expected");
        for (Family family : {Family::EU, Family::CanFd}) {
            const QByteArray prefix = family == Family::EU ? "eu-" : "fd-";
            QTest::newRow((prefix + "normal").constData()) << int(family) << 95 << 0 << CanBusState::Normal;
            QTest::newRow((prefix + "tx-warning").constData()) << int(family) << 0 << 96 << CanBusState::Warning;
            QTest::newRow((prefix + "rx-warning").constData()) << int(family) << 127 << 0 << CanBusState::Warning;
            QTest::newRow((prefix + "rx-passive").constData()) << int(family) << 128 << 0 << CanBusState::Passive;
            QTest::newRow((prefix + "tx-passive").constData()) << int(family) << 0 << 255 << CanBusState::Passive;
        }
    }

    void otherFamiliesKeepCounterStatus()
    {
        QFETCH(int, family);
        QFETCH(int, receiveCounter);
        QFETCH(int, transmitCounter);
        QFETCH(CanBusState, expected);
        Driver driver;
        driver.errorCode = ZCAN_ERROR_CAN_PASSIVE; // successful status counters retain their original priority
        driver.receiveCounter = BYTE(receiveCounter);
        driver.transmitCounter = BYTE(transmitCounter);
        QCOMPARE(driver.query(Family(family)), expected);
        QCOMPARE(driver.calls, (QStringList{QStringLiteral("error"), QStringLiteral("status")}));
    }

    void otherFamiliesKeepBusOffPriorityAndErrorFallback()
    {
        for (Family family : {Family::EU, Family::CanFd}) {
            Driver busOff;
            busOff.errorCode = ZCAN_ERROR_CAN_BUSOFF;
            QCOMPARE(busOff.query(family), CanBusState::BusOff);
            QCOMPARE(busOff.calls, QStringList{QStringLiteral("error")});

            for (UINT flag : {ZCAN_ERROR_CAN_ERRALARM, ZCAN_ERROR_CAN_PASSIVE}) {
                Driver fallback;
                fallback.errorCode = flag;
                fallback.statusOk = false;
                fallback.receiveCounter = 255; // failed status output is ignored
                QCOMPARE(fallback.query(family),
                         flag == ZCAN_ERROR_CAN_PASSIVE ? CanBusState::Passive : CanBusState::Warning);
                QCOMPARE(fallback.calls, (QStringList{QStringLiteral("error"), QStringLiteral("status")}));
            }

            Driver onlyStatus;
            onlyStatus.errorOk = false;
            onlyStatus.errorCode = ZCAN_ERROR_CAN_BUSOFF;
            onlyStatus.transmitCounter = 96;
            QCOMPARE(onlyStatus.query(family), CanBusState::Warning);
            QCOMPARE(onlyStatus.calls, (QStringList{QStringLiteral("error"), QStringLiteral("status")}));
        }
    }
};

QTEST_APPLESS_MAIN(TestZlgStatus)
#include "tst_zlg_status.moc"
