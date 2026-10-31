#include "core/protocol_parser.h"

#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtTest/QtTest>

using namespace fhmi;

namespace {

QByteArray rtuOf(const QByteArray &pdu, int slave = 1)
{
    const QByteArray body = QByteArray(1, char(slave)) + pdu;
    return body + modbusCrc(body);
}

QByteArray tcpOf(const QByteArray &pdu, int transaction = 1, int unit = 1)
{
    QByteArray adu;
    adu.append(char(transaction >> 8)).append(char(transaction)).append('\0').append('\0');
    adu.append(char((pdu.size() + 1) >> 8)).append(char(pdu.size() + 1)).append(char(unit));
    return adu + pdu;
}

// JSON equality where an integer and a double of the same value are equal: Python writes 25.0
// for a float, the parser may hold 25 as a qlonglong, and the other way round.
bool sameJson(const QJsonValue &a, const QJsonValue &b)
{
    if (a.isDouble() && b.isDouble()) {
        return a.toDouble() == b.toDouble();
    }
    if (a.isArray() && b.isArray()) {
        const QJsonArray x = a.toArray();
        const QJsonArray y = b.toArray();
        if (x.size() != y.size()) {
            return false;
        }
        for (qsizetype i = 0; i < x.size(); ++i) {
            if (!sameJson(x.at(i), y.at(i))) {
                return false;
            }
        }
        return true;
    }
    if (a.isObject() && b.isObject()) {
        const QJsonObject x = a.toObject();
        const QJsonObject y = b.toObject();
        if (x.keys() != y.keys()) {
            return false;
        }
        for (auto it = x.begin(); it != x.end(); ++it) {
            if (!sameJson(it.value(), y.value(it.key()))) {
                return false;
            }
        }
        return true;
    }
    return a == b;
}

QByteArray compact(const QJsonArray &value) { return QJsonDocument(value).toJson(QJsonDocument::Compact); }

} // namespace

class TestProtocolParser : public QObject
{
    Q_OBJECT

  private slots:
    // tests/data/parser_vectors.json: byte streams shared with PyQt UI Designer, and the frames and
    // messages both parsers make of them. Each step feeds bytes (frames() then parse() per unit),
    // flushes an idle frame, or notes a request sent; a bad config names its error instead.
    void goldenVectors_data()
    {
        QTest::addColumn<QJsonObject>("vector");
        QFile file(QStringLiteral(FHMI_TEST_DATA_DIR "/parser_vectors.json"));
        QVERIFY2(file.open(QIODevice::ReadOnly), qPrintable(file.errorString()));
        const QJsonArray vectors = QJsonDocument::fromJson(file.readAll()).array();
        QVERIFY(!vectors.isEmpty());
        for (const QJsonValue &vector : vectors) {
            QTest::newRow(vector[QStringLiteral("name")].toString().toUtf8().constData()) << vector.toObject();
        }
    }

    void goldenVectors()
    {
        QFETCH(QJsonObject, vector);
        const QString type = vector.value(QStringLiteral("type")).toString();
        auto requests = std::make_shared<ModbusRequests>();
        QString error;
        auto parser = createParser(type, vector.value(QStringLiteral("config")).toObject(), &error, requests);
        if (vector.contains(QStringLiteral("error"))) {
            QVERIFY(!parser);
            QCOMPARE(error, vector.value(QStringLiteral("error")).toString());
            return;
        }
        QVERIFY2(parser, qPrintable(error));
        const QJsonArray steps = vector.value(QStringLiteral("steps")).toArray();
        const QJsonArray expected = vector.value(QStringLiteral("expect")).toArray();
        QCOMPARE(expected.size(), steps.size());
        for (qsizetype i = 0; i < steps.size(); ++i) {
            const QJsonObject step = steps.at(i).toObject();
            const auto bytes = [&step](const char *key) {
                return QByteArray::fromHex(step.value(QLatin1String(key)).toString().toLatin1());
            };
            QList<QByteArray> units;
            if (step.contains(QStringLiteral("feed"))) {
                units = parser->frames(bytes("feed"));
            } else if (step.contains(QStringLiteral("flush"))) {
                units = parser->flush();
            } else {
                requests->noteFrame(bytes("note"), type);
            }
            QJsonArray got;
            for (const QByteArray &unit : units) {
                QJsonArray messages;
                for (const Message &message : parser->parse(unit)) {
                    messages.append(QJsonObject::fromVariantMap(message));
                }
                got.append(QJsonObject{{QStringLiteral("frame"), QString::fromLatin1(unit.toHex())},
                                       {QStringLiteral("messages"), messages}});
            }
            const QJsonArray want = expected.at(i).toArray();
            QVERIFY2(sameJson(got, want),
                     qPrintable(QStringLiteral("step %1\n   got %2\n  want %3")
                                    .arg(i)
                                    .arg(QString::fromUtf8(compact(got)), QString::fromUtf8(compact(want)))));
        }
    }

    // The table-driven GBK of a Qt without ICU: Python's bytes.decode("gbk", "replace"), and a
    // terminator encoded through the same table.
    void gbkComesFromTheEmbeddedTable()
    {
        const QString gbk = QStringLiteral("gbk");
        QCOMPARE(LineParser::decode(QByteArray::fromHex("cec2b6c8"), gbk), QStringLiteral("温度"));
        // A byte that starts no pair is one U+FFFD and the next byte is read again; AA A1 is the
        // user-defined area, which Python does not decode.
        QCOMPARE(LineParser::decode(QByteArray::fromHex("80aaa1813041"), gbk), QStringLiteral("����0A"));
        QCOMPARE(lineTerminator(QJsonObject{{"terminator", "。"}, {"encoding", "gbk"}}), QByteArray::fromHex("a1a3"));
        QCOMPARE(LineParser::decode(QByteArray::fromHex("a1a3"), gbk), QStringLiteral("。"));
    }

    // What the vectors cannot show. An RTU read is answered once per parser, so each client of a
    // server names its reply; a TCP reply finds its read by transaction, in any order.
    void modbusRepliesFindTheirReads()
    {
        auto requests = std::make_shared<ModbusRequests>();
        auto first = createParser(QStringLiteral("modbus"), {}, nullptr, requests);
        auto second = createParser(QStringLiteral("modbus"), {}, nullptr, requests);
        requests->note(1, 3, 100, 2);
        requests->note(1, 3, 300, 1); // the latest read of a slave and function is the one answered
        const QByteArray reply = rtuOf(QByteArray::fromHex("03020005"));
        QCOMPARE(first->feed(reply).value(0).value("hr300"), QVariant(5LL));
        QVERIFY(!first->feed(reply).value(0).contains("start"));
        QCOMPARE(second->feed(reply).value(0).value("hr300"), QVariant(5LL));
        // Only the coils that were read get addresses; an exception answers a read as well.
        requests->note(1, 1, 7, 3);
        const Message coils = first->feed(rtuOf(QByteArray::fromHex("010105"))).value(0);
        QCOMPARE((QList<QVariant>{coils.value("co7"), coils.value("co8"), coils.value("co9")}),
                 (QList<QVariant>{1LL, 0LL, 1LL}));
        QVERIFY(!coils.contains("co10"));
        requests->note(2, 4, 30, 1);
        QCOMPARE(first->feed(rtuOf(QByteArray::fromHex("8402"), 2)).value(0).value("start"), QVariant(30LL));

        auto tcp = createParser(QStringLiteral("modbus_tcp"), {}, nullptr, requests);
        requests->note(1, 3, 100, 1, 7);
        requests->note(1, 3, 200, 1, 8);
        const QList<Message> both =
            tcp->feed(tcpOf(QByteArray::fromHex("0302000b"), 8) + tcpOf(QByteArray::fromHex("0302000a"), 7));
        QCOMPARE(both.size(), 2);
        QCOMPARE(both.at(0).value("hr200"), QVariant(11LL));
        QCOMPARE(both.at(1).value("hr100"), QVariant(10LL));
        for (const QByteArray &wrong : {tcpOf(QByteArray::fromHex("0302000a"), 7, 2),
                                        tcpOf(QByteArray::fromHex("0402000a"), 7),
                                        tcpOf(QByteArray::fromHex("0302000a"), 9)}) {
            QVERIFY(!tcp->feed(wrong).value(0).contains("start"));
        }
        for (int i = 0; i < 300; ++i) { // old transactions are forgotten, recent ones kept
            requests->note(1, 3, i, 1, 1000 + i);
        }
        QCOMPARE(tcp->feed(tcpOf(QByteArray::fromHex("03020001"), 1299)).value(0).value("hr299"), QVariant(1LL));
        QVERIFY(!tcp->feed(tcpOf(QByteArray::fromHex("03020001"), 1000)).value(0).contains("start"));
    }

    void modbusRequestsAreReadFromOutgoingFrames()
    {
        ModbusRequests requests;
        const QByteArray rtu = buildModbusRtuFrame(1, 3, 100, 0, 6);
        requests.noteFrame(rtu, QStringLiteral("line"));
        requests.noteFrame(rtu.left(7) + '\0', QStringLiteral("modbus")); // bad CRC
        requests.noteFrame(buildModbusRtuFrame(1, 6, 100, 5), QStringLiteral("modbus"));
        QVERIFY(!requests.take(1, 3));
        QVERIFY(!requests.take(1, 6));
        requests.noteFrame(rtu, QStringLiteral("modbus"));
        QCOMPARE(requests.take(1, 3), (std::optional<std::pair<int, int>>{{100, 6}}));
        requests.noteFrame(buildModbusTcpFrame(2, 4, 30, 0, 2, 9), QStringLiteral("modbus_tcp"));
        requests.noteFrame(buildModbusTcpFrame(2, 4, 40, 0, 2, 10), QStringLiteral("modbus"));
        QCOMPARE(requests.take(2, 4, 9), (std::optional<std::pair<int, int>>{{30, 2}}));
        QVERIFY(!requests.take(2, 4, 10));
    }

    // ASCII request frames, and the requests noted from them and from FC 23 (by the registers it reads).
    void asciiAndReadWriteRequests()
    {
        QCOMPARE(modbusLrc(QByteArray::fromHex("01030000000A")), quint8(0xF2));
        QCOMPARE(buildModbusAsciiFrame(1, 3, 0, 0, 10), QByteArray(":01030000000AF2\r\n"));
        QCOMPARE(buildModbusAsciiFrame(0x1A, 6, 0xBEEF, 0xABCD), QByteArray(":1A06BEEFABCDBB\r\n")); // upper case
        QCOMPARE(modbusAsciiBody(":01030000000af2\r\n"),
                 std::optional<QByteArray>(QByteArray::fromHex("01030000000A"))); // either case is read
        for (const char *bad : {":01030000000AF3\r\n", ":01030000000AF2\n", "01030000000AF2\r\n", ":01030000000AF\r\n",
                                ":0103 000000AF2\r\n"}) {
            QVERIFY2(!modbusAsciiBody(bad), bad);
        }

        ModbusRequests requests;
        const QByteArray ascii = buildModbusAsciiFrame(1, 3, 100, 0, 6);
        requests.noteFrame(ascii, QStringLiteral("modbus")); // not an RTU frame
        requests.noteFrame(ascii.chopped(4) + "00\r\n", QStringLiteral("modbus_ascii")); // a bad LRC
        QVERIFY(!requests.take(1, 3));
        requests.noteFrame(ascii, QStringLiteral("modbus_ascii"));
        QCOMPARE(requests.take(1, 3), (std::optional<std::pair<int, int>>{{100, 6}}));
        // Read 2 registers from 3, write 1 at 14; the length must hold the registers written.
        const QByteArray readWrite = QByteArray::fromHex("17 0003 0002 000E 0001 02 00FF");
        requests.noteFrame(rtuOf(readWrite), QStringLiteral("modbus"));
        QCOMPARE(requests.take(1, 0x17), (std::optional<std::pair<int, int>>{{3, 2}}));
        requests.noteFrame(tcpOf(readWrite, 5), QStringLiteral("modbus_tcp"));
        QCOMPARE(requests.take(1, 0x17, 5), (std::optional<std::pair<int, int>>{{3, 2}}));
        const QByteArray short_ = QByteArray::fromHex("17 0003 0002 000E 0002 02 00FF");
        requests.noteFrame(rtuOf(short_, 3), QStringLiteral("modbus"));
        requests.noteFrame(tcpOf(short_, 6), QStringLiteral("modbus_tcp"));
        QVERIFY(!requests.take(3, 0x17));
        QVERIFY(!requests.take(1, 0x17, 6));
    }

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
        LineParser parser({}, 8);
        QVERIFY(parser.feed("abcdefghijk").isEmpty());
        const QList<Message> out = parser.feed("lmn\nx=1\n");
        QCOMPARE(out.size(), 1);
        QCOMPARE(out.at(0).value("x").toLongLong(), 1);
    }

    void hexFrameFields()
    {
        HexFrameConfig config; // header AA55, footer 55AA
        config.fields = {{"a", 0, 1, "uint8"}, {"b", 1, 2, "int16"}, {"c", 3, 4, "float32"}};
        HexFrameParser parser(config);
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
        QCOMPARE(error, QStringLiteral("未知解析模式: nope"));
        QVERIFY(!createParser("line", QJsonObject{{"separator", ""}}, &error));
        QCOMPARE(error, QStringLiteral("行分隔符不能为空"));
        // An empty header or footer is no longer an error: the frame can end another way.
        QVERIFY(createParser("hex", QJsonObject{{"header", ""}}, &error));
        QVERIFY(createParser("hex", QJsonObject{{"header", "AA 55"}, {"footer", "0D0A"}}, &error));
        QVERIFY(createParser("modbus_tcp", {}, &error));
    }
};

QTEST_APPLESS_MAIN(TestProtocolParser)
#include "tst_protocol_parser.moc"
