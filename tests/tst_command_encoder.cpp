#include "core/command_encoder.h"

#include <QtTest/QtTest>

using namespace fhmi;

class TestCommandEncoder : public QObject
{
    Q_OBJECT

  private slots:
    void textUsesTemplateAndNewline()
    {
        const QJsonObject props{{"commandTemplate", "SET {value}"}, {"commandNewline", "\\n"}};
        const CommandResult r = CommandEncoder::encode(props, "42");
        QVERIFY(r.command);
        QCOMPARE(r.command->bytes, QByteArray("SET 42\n"));
    }

    void textTemplateWithoutPlaceholderIsSentAsIs()
    {
        const CommandResult r = CommandEncoder::encode({{"commandTemplate", "PING"}, {"commandNewline", "无"}}, "x");
        QCOMPARE(r.command->bytes, QByteArray("PING"));
    }

    void defaultNewlineIsCrLf()
    {
        QCOMPARE(CommandEncoder::encode({}, "RUN").command->bytes, QByteArray("RUN\r\n"));
    }

    void hexStringWithHeaderAndTail()
    {
        const QJsonObject props{{"commandMode", "HEX"}, {"frameHeader", "AA 55"}, {"frameTail", "D"}};
        const CommandResult r = CommandEncoder::encode(props, "01 02");
        QCOMPARE(r.command->bytes, QByteArray::fromHex("AA5501020D"));
    }

    void hexInvalidIsAnError()
    {
        const CommandResult r = CommandEncoder::encode({{"commandMode", "HEX"}}, "zz");
        QVERIFY(!r.command);
        QVERIFY(!r.error.isEmpty());
    }

    void hexPackedNumbers()
    {
        QJsonObject props{{"commandMode", "HEX"}, {"dataType", "int16"}, {"commandTemplate", "01{value}"}};
        QCOMPARE(CommandEncoder::encode(props, "-2").command->bytes, QByteArray::fromHex("01FFFE"));
        props["byteOrder"] = "小端(LE)";
        QCOMPARE(CommandEncoder::encode(props, "-2").command->bytes, QByteArray::fromHex("01FEFF"));
        props["dataType"] = "uint8";
        QVERIFY(!CommandEncoder::encode(props, "256").command);
        props["dataType"] = "float32";
        props["byteOrder"] = "大端(BE)";
        QCOMPARE(CommandEncoder::encode(props, "10").command->bytes, QByteArray::fromHex("0141200000"));
    }

    void modbusFromWidget()
    {
        const QJsonObject props{{"commandMode", "Modbus"}, {"mbSlave", 2}, {"mbFunc", "06 写单寄存器"},
                                {"mbAddress", 16}};
        const CommandResult r = CommandEncoder::encode(props, "300");
        QVERIFY(r.command);
        QCOMPARE(r.command->kind, EncodedCommand::Kind::Modbus);
        QCOMPARE(CommandEncoder::rtuFrame(r.command->modbus).left(6), QByteArray::fromHex("02060010012C"));
        QVERIFY(!CommandEncoder::encode(props, "70000").command);
        QVERIFY(!CommandEncoder::encode(props, "1.5").command);
    }

    void modbusReadsCheckQuantity()
    {
        QVERIFY(CommandEncoder::modbusRequest(1, 3, 0, 125));
        QVERIFY(!CommandEncoder::modbusRequest(1, 3, 0, 126));
        QVERIFY(!CommandEncoder::modbusRequest(0, 3, 0, 1));
        QVERIFY(CommandEncoder::modbusRequest(1, 5, 0, 0xFF00));
        QVERIFY(!CommandEncoder::modbusRequest(1, 5, 0, 1));
    }

    void transactionsWrap()
    {
        const int first = CommandEncoder::nextTransaction();
        QCOMPARE(CommandEncoder::nextTransaction(), (first + 1) & 0xFFFF);
    }
};

QTEST_APPLESS_MAIN(TestCommandEncoder)
#include "tst_command_encoder.moc"
