#include "core/protocol_parser.h"

#include <QtTest/QtTest>

using namespace fhmi;

class TestProtocolParser : public QObject
{
    Q_OBJECT

  private slots:
    void lineKeyValueAcrossChunks()
    {
        LineParser parser;
        QVERIFY(parser.feed("temp=25.5,hum").isEmpty());
        const QList<Message> out = parser.feed("idity=60\nrunning=1\n");
        QCOMPARE(out.size(), 2);
        QCOMPARE(out.at(0).value("temp").toDouble(), 25.5);
        QCOMPARE(out.at(0).value("humidity").typeId(), int(QMetaType::LongLong));
        QCOMPARE(out.at(0).value("humidity").toLongLong(), 60);
        QCOMPARE(out.at(1).value("running").toLongLong(), 1);
    }

    void lineCsvIsIndexed()
    {
        LineParser parser;
        const QList<Message> out = parser.feed("1, 2.5 ,abc\r\n");
        QCOMPARE(out.size(), 1);
        QCOMPARE(out.at(0).value("ch0").toLongLong(), 1);
        QCOMPARE(out.at(0).value("ch1").toDouble(), 2.5);
        QCOMPARE(out.at(0).value("ch2").toString(), QStringLiteral("abc"));
    }

    void lineJsonKeepsEscapedNewline()
    {
        LineParser parser;
        const QList<Message> out = parser.feed("{\"msg\":\"a\\nb\",\"data\":{\"t\":[1,2]}}\n");
        QCOMPARE(out.size(), 1);
        QCOMPARE(out.at(0).value("msg").toString(), QStringLiteral("a\nb"));
        QCOMPARE(out.at(0).value("data").toMap().value("t").toList().size(), 2);
    }

    void lineInvalidJsonIsDropped()
    {
        LineParser parser;
        QVERIFY(parser.feed("{broken\n").isEmpty());
    }

    void lineOverlongRecordIsDiscarded()
    {
        LineParser parser(QStringLiteral(","), 8);
        QVERIFY(parser.feed("abcdefghijk").isEmpty());
        const QList<Message> out = parser.feed("lmn\nx=1\n");
        QCOMPARE(out.size(), 1);
        QCOMPARE(out.at(0).value("x").toLongLong(), 1);
    }

    void hexFrameFields()
    {
        HexFrameParser parser(QByteArray::fromHex("AA55"), QByteArray::fromHex("55AA"),
                              {{"a", 0, 1, "uint8"}, {"b", 1, 2, "int16"}, {"c", 3, 4, "float32"}});
        const QByteArray frame = QByteArray::fromHex("0011AA55" "7F" "FFFE" "41200000" "55AA");
        QList<Message> out = parser.feed(frame.left(6));
        QVERIFY(out.isEmpty());
        out = parser.feed(frame.mid(6));
        QCOMPARE(out.size(), 1);
        QCOMPARE(out.at(0).value("a").toLongLong(), 127);
        QCOMPARE(out.at(0).value("b").toLongLong(), -2);
        QCOMPARE(out.at(0).value("c").toDouble(), 10.0);
    }

    void modbusCrcMatchesReference()
    {
        // 01 03 00 00 00 0A -> CRC C5 CD
        QCOMPARE(modbusCrc(QByteArray::fromHex("01030000000A")), QByteArray::fromHex("C5CD"));
        QCOMPARE(buildModbusRtuFrame(1, 3, 0, 0, 10), QByteArray::fromHex("01030000000AC5CD"));
    }

    void modbusRtuReadResponse()
    {
        ModbusRtuParser parser;
        QByteArray pdu = QByteArray::fromHex("010304000A0014");
        const QByteArray frame = pdu + modbusCrc(pdu);
        const QList<Message> out = parser.feed(QByteArray::fromHex("FF") + frame);
        QCOMPARE(out.size(), 1);
        QCOMPARE(out.at(0).value("slave").toLongLong(), 1);
        QCOMPARE(out.at(0).value("fc").toLongLong(), 3);
        QCOMPARE(out.at(0).value("reg0").toLongLong(), 10);
        QCOMPARE(out.at(0).value("reg1").toLongLong(), 20);
    }

    void modbusRtuRejectsBadCrc()
    {
        ModbusRtuParser parser;
        QVERIFY(parser.feed(QByteArray::fromHex("010304000A00140000")).isEmpty());
    }

    void modbusRtuExceptionResponse()
    {
        ModbusRtuParser parser;
        const QByteArray pdu = QByteArray::fromHex("018302");
        const QList<Message> out = parser.feed(pdu + modbusCrc(pdu));
        QCOMPARE(out.size(), 1);
        QCOMPARE(out.at(0).value("error").toLongLong(), 2);
    }

    void modbusRtuCoils()
    {
        ModbusRtuParser parser;
        const QByteArray pdu = QByteArray::fromHex("01010105");
        const QList<Message> out = parser.feed(pdu + modbusCrc(pdu));
        QCOMPARE(out.size(), 1);
        QCOMPARE(out.at(0).value("coil0").toLongLong(), 1);
        QCOMPARE(out.at(0).value("coil1").toLongLong(), 0);
        QCOMPARE(out.at(0).value("coil2").toLongLong(), 1);
    }

    void modbusTcpMatchesRtuOutput()
    {
        ModbusTcpParser parser;
        const QByteArray adu = QByteArray::fromHex("0001000000070103040001FFFF");
        // Two separate statements: the operands of + have no defined evaluation order.
        QList<Message> out = parser.feed(adu.left(5));
        QVERIFY(out.isEmpty());
        out += parser.feed(adu.mid(5));
        QCOMPARE(out.size(), 1);
        QCOMPARE(out.at(0).value("slave").toLongLong(), 1);
        QCOMPARE(out.at(0).value("reg0").toLongLong(), 1);
        QCOMPARE(out.at(0).value("reg1").toLongLong(), 65535);
    }

    void modbusTcpFrameBuild()
    {
        QCOMPARE(buildModbusTcpFrame(1, 6, 16, 300, 300, 7), QByteArray::fromHex("000700000006010600100" "12C"));
    }

    void rawFrameBufferSplitsFrames()
    {
        RawFrameBuffer buffer(QByteArray::fromHex("AA"), QByteArray::fromHex("55"));
        QList<QByteArray> frames = buffer.feed(QByteArray::fromHex("00AA0155AA02"));
        QCOMPARE(frames.size(), 1);
        QCOMPARE(frames.at(0), QByteArray::fromHex("AA0155"));
        frames = buffer.feed(QByteArray::fromHex("55"));
        QCOMPARE(frames.size(), 1);
        QCOMPARE(frames.at(0), QByteArray::fromHex("AA0255"));
    }

    void factoryRejectsBadConfig()
    {
        QString error;
        QVERIFY(!createParser("nope", {}, &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(!createParser("line", QJsonObject{{"separator", ""}}, &error));
        QVERIFY(!createParser("hex", QJsonObject{{"header", ""}}, &error));
        QVERIFY(createParser("hex", QJsonObject{{"header", "AA 55"}, {"footer", "0D0A"}}, &error));
        QVERIFY(createParser("modbus_tcp", {}, &error));
    }
};

QTEST_APPLESS_MAIN(TestProtocolParser)
#include "tst_protocol_parser.moc"
