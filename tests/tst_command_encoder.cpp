#include "core/command_encoder.h"
#include "core/protocol_parser.h"

#include <QtTest/QtTest>

using namespace fhmi;

class TestCommandEncoder : public QObject
{
    Q_OBJECT

  private slots:
    void typedRegisterWritesUseFc06OrFc16()
    {
        struct Case
        {
            int func, address;
            QString value, type, order;
            QByteArray pdu;
        };
        const QString ab = QStringLiteral("大端(AB CD)"), cd = QStringLiteral("小端(CD AB)");
        const QList<Case> cases = {
            {16, 100, "25.3", "float32", ab, QByteArray::fromHex("0110006400020441ca6666")},
            {16, 100, "25.3", "float32", cd, QByteArray::fromHex("01100064000204666641ca")},
            {16, 0, "-2", "int32", ab, QByteArray::fromHex("01100000000204fffffffe")},
            {16, 0, "70000", "uint32", ab, QByteArray::fromHex("01100000000204000111 70")},
            {16, 5, "42", "uint16", ab, QByteArray::fromHex("011000050001 02002a")},
            {6, 7, "-1", "int16", ab, QByteArray::fromHex("01060007ffff")},
        };
        for (const Case &c : cases) {
            QString error;
            const auto request = CommandEncoder::modbusRequest(1, c.func, c.address, c.value, c.type, c.order, &error);
            QVERIFY2(request, qPrintable(error));
            QCOMPARE(CommandEncoder::rtuFrame(*request), c.pdu + modbusCrc(c.pdu));
        }
        const auto request = CommandEncoder::modbusRequest(1, 16, 100, QStringLiteral("25.3"), "float32", ab);
        QCOMPARE(CommandEncoder::tcpFrame(*request, 0x1234).toHex(), QByteArray("12340000000b0110006400020441ca6666"));
        QCOMPARE(CommandEncoder::asciiFrame(*request), QByteArray(":0110006400020441CA6666AE\r\n"));
        const QList<std::tuple<int, int, QString, QString>> rejected = {
            {6, 7, "1", "float32"}, {16, 65535, "1", "uint32"}, {16, 0, "1e39", "float32"},
            {16, 0, "nan", "float32"}, {16, 0, "40000", "int16"}, {16, 0, "1.5", "int32"}, {16, 0, "1", "double"}};
        for (const auto &[func, address, value, type] : rejected) {
            QVERIFY2(!CommandEncoder::modbusRequest(1, func, address, value, type, ab), qPrintable(value + type));
        }
        // Widgets take the type and order from their properties.
        const CommandResult fromWidget = CommandEncoder::encode(
            {{"commandMode", "Modbus"}, {"mbSlave", 1}, {"mbFunc", "16 写多个寄存器"}, {"mbAddress", 100},
             {"mbDataType", "float32"}},
            QStringLiteral("25"));
        QVERIFY(fromWidget.command);
        QCOMPARE(CommandEncoder::rtuFrame(fromWidget.command->modbus).toHex(),
                 QByteArray("0110006400020441c8000061b6"));
    }

    void pollTableListsReadsAndNamesTheBadEntry()
    {
        QString error;
        const auto table = CommandEncoder::modbusPollTable(QStringLiteral("1,3,100,10\n# 线圈\n1，1，0，3; 2,4,0,1"), &error);
        QVERIFY2(table, qPrintable(error));
        QCOMPARE(table->size(), 3);
        QCOMPARE(table->at(1).func, 1);
        QCOMPARE(table->at(1).value, 3);
        QCOMPARE(table->at(2).slave, 2);
        QVERIFY(!CommandEncoder::modbusPollTable(QStringLiteral("1,3,0,1\n1,6,0,1"), &error));
        QCOMPARE(error, QStringLiteral("轮询表第 2 条应为 从站,功能码(1–4),地址,数量"));
        QVERIFY(!CommandEncoder::modbusPollTable(QStringLiteral("1,3,0"), &error));
        QVERIFY(!CommandEncoder::modbusPollTable(QStringLiteral("1,3,0,126"), &error)); // well formed: say why
        QCOMPARE(error, QStringLiteral("轮询表第 1 条：Modbus 值或读取数量超出功能码允许范围"));
        QVERIFY(CommandEncoder::modbusPollTable(QString())->isEmpty());
    }

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
