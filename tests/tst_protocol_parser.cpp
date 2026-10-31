#include "core/protocol_parser.h"

#include <QtCore/QJsonDocument>
#include <QtTest/QtTest>

using namespace fhmi;

namespace {

std::unique_ptr<Parser> hexParser(const char *config, QString *error = nullptr)
{
    return createParser(QStringLiteral("hex"), QJsonDocument::fromJson(config).object(), error);
}

// The error createParser gives for a "hex" config, empty when it builds.
QString hexError(const char *config)
{
    QString error;
    return hexParser(config, &error) ? QString() : error;
}

// Spec example A: AA 55 | LEN | CMD | DATA... | SUM, the length counting the data.
const char *const kConfigA = R"json({"header": "AA55", "footer": "", "lengthOffset": 2, "lengthSize": 1,
    "lengthCovers": "数据", "checksum": "和校验", "checksumStart": 0,
    "fields": [["cmd", 1, 1, "uint8"], ["temp", 2, 2, "int16", "小端(LE)"], ["hum", 4, 1, "uint8"]]})json";

const QByteArray kFrameA = QByteArray::fromHex("AA55 04 01 F6FF 3C 35");
const QByteArray kFrameA2 = QByteArray::fromHex("AA55 04 02 1000 28 3D");
const Message kMessageA{{"cmd", 1LL}, {"temp", -10LL}, {"hum", 60LL}};
const Message kMessageA2{{"cmd", 2LL}, {"temp", 16LL}, {"hum", 40LL}};

QList<QByteArray> bytewise(Parser &parser, const QByteArray &data)
{
    QList<QByteArray> out;
    for (const char c : data) {
        out += parser.frames(QByteArray(1, c));
    }
    return out;
}

// The messages of a stream fed one byte at a time.
QList<Message> fedBytewise(Parser &parser, const QByteArray &data)
{
    QList<Message> out;
    for (const char c : data) {
        out += parser.feed(QByteArray(1, c));
    }
    return out;
}

std::unique_ptr<Parser> lineParser(const char *config, QString *error = nullptr)
{
    return createParser(QStringLiteral("line"), QJsonDocument::fromJson(config).object(), error);
}

QString lineError(const char *config)
{
    QString error;
    return lineParser(config, &error) ? QString() : error;
}

// One response PDU (function code first) from slave 1, framed the three ways.
QByteArray rtuOf(const QByteArray &pdu)
{
    const QByteArray body = QByteArray(1, '\x01') + pdu;
    return body + modbusCrc(body);
}

QByteArray tcpOf(const QByteArray &pdu, int transaction = 1)
{
    QByteArray adu;
    adu.append(char(transaction >> 8)).append(char(transaction)).append('\0').append('\0');
    adu.append(char((pdu.size() + 1) >> 8)).append(char(pdu.size() + 1)).append('\x01');
    return adu + pdu;
}

QByteArray asciiOf(const QByteArray &pdu)
{
    return modbusAsciiFrame(QByteArray(1, '\x01') + pdu);
}

QByteArray framed(const QString &type, const QByteArray &pdu, int transaction = 1)
{
    return type == QLatin1String("modbus_tcp") ? tcpOf(pdu, transaction)
           : type == QLatin1String("modbus_ascii") ? asciiOf(pdu)
                                                   : rtuOf(pdu);
}

const QStringList kModbusTypes = {QStringLiteral("modbus"), QStringLiteral("modbus_tcp"),
                                  QStringLiteral("modbus_ascii")};

} // namespace

class TestProtocolParser : public QObject
{
    Q_OBJECT

  private slots:
    // A read sent over the link names the registers in its reply by address as well.
    void modbusRtuNamesRegistersByAddress()
    {
        const auto rtu = [](const char *hex) {
            const QByteArray pdu = QByteArray::fromHex(hex);
            return pdu + modbusCrc(pdu);
        };
        auto requests = std::make_shared<ModbusRequests>();
        auto parser = createParser(QStringLiteral("modbus"), {}, nullptr, requests);
        requests->note(1, 3, 100, 2);
        requests->note(1, 1, 7, 3);
        requests->note(2, 4, 30, 1);
        Message reply = parser->feed(rtu("01030441ca6666")).value(0);
        QCOMPARE(reply.value("reg1").toLongLong(), 0x6666);
        QCOMPARE(reply.value("start").toLongLong(), 100);
        QCOMPARE(reply.value("hr100").toLongLong(), 0x41CA);
        QCOMPARE(reply.value("hr101").toLongLong(), 0x6666);
        // Only the coils that were read get addresses; the rest of the last byte is padding.
        reply = parser->feed(rtu("01010105")).value(0);
        QCOMPARE(reply.value("coil7").toLongLong(), 0);
        QCOMPARE((QList<qlonglong>{reply.value("co7").toLongLong(), reply.value("co8").toLongLong(),
                                   reply.value("co9").toLongLong()}),
                 (QList<qlonglong>{1, 0, 1}));
        QVERIFY(!reply.contains("co10"));
        // An exception answers the same read; a reply whose count differs gets no addresses.
        reply = parser->feed(rtu("028402")).value(0);
        QCOMPARE(reply.value("start").toLongLong(), 30);
        QCOMPARE(reply.value("error").toLongLong(), 2);
        reply = parser->feed(rtu("02040400010002")).value(0);
        QVERIFY(!reply.contains("start"));
        QVERIFY(!reply.contains("ir30"));
        // An RTU master waits for each reply, so the latest read of that slave and function is
        // answered, and only once: a second reply has no request left to name it.
        requests->note(1, 3, 100, 2);
        requests->note(1, 3, 200, 2);
        QCOMPARE(parser->feed(rtu("010304000a000b")).value(0).value("hr201").toLongLong(), 11);
        QVERIFY(!parser->feed(rtu("010304000a000b")).value(0).contains("start"));
        QVERIFY(!parser->feed(rtu("0503020001")).value(0).contains("start"));
        // Each stream answers a read once, so a server's clients all name their replies.
        requests->note(1, 3, 300, 1);
        auto second = createParser(QStringLiteral("modbus"), {}, nullptr, requests);
        QCOMPARE(parser->feed(rtu("0103020005")).value(0).value("hr300").toLongLong(), 5);
        QCOMPARE(second->feed(rtu("0103020005")).value(0).value("hr300").toLongLong(), 5);
        QVERIFY(!second->feed(rtu("0103020005")).value(0).contains("start"));
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

    void modbusTcpNamesEachReplyByItsTransaction()
    {
        const auto reply = [](int transaction, int unit, const char *hex) {
            const QByteArray pdu = QByteArray::fromHex(hex);
            QByteArray adu;
            adu.append(char(transaction >> 8)).append(char(transaction)).append('\0').append('\0');
            adu.append(char((pdu.size() + 1) >> 8)).append(char(pdu.size() + 1)).append(char(unit));
            return adu + pdu;
        };
        auto requests = std::make_shared<ModbusRequests>();
        auto parser = createParser(QStringLiteral("modbus_tcp"), {}, nullptr, requests);
        requests->note(1, 3, 100, 1, 7);
        requests->note(1, 3, 200, 1, 8);
        // Replies may come back in any order; each finds its own request.
        const QList<Message> both = parser->feed(reply(8, 1, "0302000b") + reply(7, 1, "0302000a"));
        QCOMPARE(both.size(), 2);
        QCOMPARE(both.at(0).value("hr200").toLongLong(), 11);
        QCOMPARE(both.at(1).value("hr100").toLongLong(), 10);
        for (const QByteArray &wrong : {reply(7, 2, "0302000a"), reply(7, 1, "0402000a"), reply(9, 1, "0302000a")}) {
            QVERIFY(!parser->feed(wrong).value(0).contains("start"));
        }
        for (int i = 0; i < 300; ++i) { // old transactions are forgotten, recent ones kept
            requests->note(1, 3, i, 1, 1000 + i);
        }
        QCOMPARE(parser->feed(reply(1299, 1, "03020001")).value(0).value("hr299").toLongLong(), 1);
        QVERIFY(!parser->feed(reply(1000, 1, "03020001")).value(0).contains("start"));
    }

    void modbusAsciiFramesResync()
    {
        const QByteArray frame(":0103040001FFFFF9\r\n");
        const Message expected{{"slave", 1LL}, {"fc", 3LL}, {"reg0", 1LL}, {"reg1", 65535LL}};
        ModbusAsciiParser parser;
        QCOMPARE(parser.feed(frame), QList<Message>{expected});
        QCOMPARE(parser.frames(frame), QList<QByteArray>{frame}); // raw bytes go out by chunk, as for RTU
        // Split anywhere, stuck together, noise in front, lower-case digits: the same frames.
        QCOMPARE(fedBytewise(parser, frame), QList<Message>{expected});
        QCOMPARE(parser.feed("xx\r\n" + frame + frame.toLower()), (QList<Message>{expected, expected}));
        // A wrong LRC, an odd digit count, a character that is no digit: dropped, the next frame read.
        QCOMPARE(parser.feed(":0103040001FFFFF8\r\n" + frame), QList<Message>{expected});
        QCOMPARE(parser.feed(":0103040001FFFFF\r\n" + frame), QList<Message>{expected});
        QCOMPARE(parser.feed(":01030400 1FFFFF9\r\n" + frame), QList<Message>{expected});
        // A colon starts a new frame, so one cut short does not take the next with it.
        QCOMPARE(parser.feed(":010304" + frame), QList<Message>{expected});
        // What cannot be an RTU frame is no ASCII frame: here the byte count is wrong.
        const QByteArray wrongCount = modbusAsciiFrame(QByteArray::fromHex("01030200010002"));
        QCOMPARE(parser.feed(wrongCount + frame), QList<Message>{expected});
        // Nothing longer than the longest frame waits for its CR LF.
        QVERIFY(parser.feed(':' + QByteArray(600, '0')).isEmpty());
        QCOMPARE(parser.feed(frame), QList<Message>{expected});
    }

    void modbusAsciiRequestFrames()
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
    }

    // ASCII requests are noted like RTU ones; FC 23 is noted by the registers it reads.
    void asciiAndReadWriteRequestsAreNoted()
    {
        ModbusRequests requests;
        const QByteArray ascii = buildModbusAsciiFrame(1, 3, 100, 0, 6);
        requests.noteFrame(ascii, QStringLiteral("modbus")); // not an RTU frame
        requests.noteFrame(ascii.chopped(4) + "00\r\n", QStringLiteral("modbus_ascii")); // a bad LRC
        QVERIFY(!requests.take(1, 3));
        requests.noteFrame(ascii, QStringLiteral("modbus_ascii"));
        QCOMPARE(requests.take(1, 3), (std::optional<std::pair<int, int>>{{100, 6}}));

        // Read 2 registers from 3, write 1 at 14.
        const QByteArray readWrite = QByteArray::fromHex("01 17 0003 0002 000E 0001 02 00FF");
        requests.noteFrame(readWrite + modbusCrc(readWrite), QStringLiteral("modbus"));
        QCOMPARE(requests.take(1, 0x17), (std::optional<std::pair<int, int>>{{3, 2}}));
        requests.noteFrame(modbusAsciiFrame(QByteArray::fromHex("02 17 0010 0001 0020 0002 04 00010002")),
                           QStringLiteral("modbus_ascii"));
        QCOMPARE(requests.take(2, 0x17), (std::optional<std::pair<int, int>>{{16, 1}}));
        requests.noteFrame(tcpOf(readWrite.mid(1), 5), QStringLiteral("modbus_tcp"));
        QCOMPARE(requests.take(1, 0x17, 5), (std::optional<std::pair<int, int>>{{3, 2}}));
        // The length must hold the registers written.
        const QByteArray short_ = QByteArray::fromHex("03 17 0003 0002 000E 0002 02 00FF");
        requests.noteFrame(short_ + modbusCrc(short_), QStringLiteral("modbus"));
        requests.noteFrame(tcpOf(short_.mid(1), 6), QStringLiteral("modbus_tcp"));
        QVERIFY(!requests.take(3, 0x17));
        QVERIFY(!requests.take(1, 0x17, 6));
    }

    void newFunctionCodesOnEveryFraming_data()
    {
        QTest::addColumn<QByteArray>("pdu");
        QTest::addColumn<Message>("expected");
        const auto row = [](const char *name, const char *pdu, Message expected) {
            const QByteArray bytes = QByteArray::fromHex(pdu);
            expected.insert("slave", 1LL);
            expected.insert("fc", qlonglong(quint8(bytes.at(0))));
            QTest::newRow(name) << bytes << expected;
        };
        row("07 exception status", "07 6D", {{"status", 0x6DLL}});
        row("08 diagnostics", "08 0000 A537", {{"subfunction", 0LL}, {"data", 0xA537LL}});
        row("17 slave id", "11 05 0A FF 01 02 03", {{"id", 10LL}, {"run", 1LL}, {"extra", "010203"}});
        row("17 slave id, stopped", "11 02 0B 00", {{"id", 11LL}, {"run", 0LL}, {"extra", ""}});
        row("17 slave id, id only", "11 01 0C", {{"id", 12LL}, {"run", 0LL}, {"extra", ""}});
        row("22 mask write", "16 0004 00F2 0025", {{"address", 4LL}, {"andMask", 0xF2LL}, {"orMask", 0x25LL}});
        row("23 read/write", "17 04 00FE 0ACD", {{"reg0", 0xFELL}, {"reg1", 0x0ACDLL}});
        for (const char *exception : {"87 01", "88 03", "91 01", "96 02", "97 04"}) {
            row(exception, exception, {{"error", qlonglong(QByteArray::fromHex(exception).at(1))}});
        }
    }

    void newFunctionCodesOnEveryFraming()
    {
        QFETCH(QByteArray, pdu);
        QFETCH(Message, expected);
        for (const QString &type : kModbusTypes) {
            auto parser = createParser(type, {});
            QVERIFY(parser);
            // Noise in front, and the frame byte by byte: each framing finds it.
            QCOMPARE(parser->feed(QByteArray::fromHex("00") + framed(type, pdu)), QList<Message>{expected});
            QCOMPARE(fedBytewise(*parser, framed(type, pdu)), QList<Message>{expected});
        }
    }

    // Byte counts these responses cannot have: none, or an odd one for registers.
    void newFunctionCodesCheckTheirLength()
    {
        for (const QString &type : kModbusTypes) {
            for (const char *pdu : {"17 03 00FE 0A", "11 00", "17 00"}) {
                auto parser = createParser(type, {});
                QVERIFY2(parser->feed(framed(type, QByteArray::fromHex(pdu))).isEmpty(), qPrintable(type + pdu));
            }
        }
    }

    // An FC 23 reply is named by the registers its request read, on each framing.
    void readWriteRepliesAreNamedByAddress()
    {
        const QByteArray request = QByteArray::fromHex("17 0003 0002 000E 0001 02 00FF");
        for (const QString &type : kModbusTypes) {
            auto requests = std::make_shared<ModbusRequests>();
            auto parser = createParser(type, {}, nullptr, requests);
            const QByteArray sent = type == QLatin1String("modbus_tcp") ? tcpOf(request, 7)
                                    : type == QLatin1String("modbus_ascii")
                                        ? asciiOf(request)
                                        : rtuOf(request);
            requests->noteFrame(sent, type);
            const Message reply = parser->feed(framed(type, QByteArray::fromHex("17 04 00FE 0ACD"), 7)).value(0);
            QCOMPARE(reply.value("start"), QVariant(3LL));
            QCOMPARE(reply.value("hr3"), QVariant(0xFELL));
            QCOMPARE(reply.value("hr4"), QVariant(0x0ACDLL));
            QCOMPARE(reply.value("reg1"), QVariant(0x0ACDLL));
            // RTU and ASCII answer a read once per parser; a TCP reply has its transaction.
            const Message again = parser->feed(framed(type, QByteArray::fromHex("17 04 00FE 0ACD"), 7)).value(0);
            QCOMPARE(again.contains("start"), type == QLatin1String("modbus_tcp"));
            requests->noteFrame(sent, type);
            const Message exception = parser->feed(framed(type, QByteArray::fromHex("97 02"), 7)).value(0);
            QCOMPARE(exception.value("start"), QVariant(3LL));
        }
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

    void lineTerminatorEndsRecords()
    {
        auto semicolon = lineParser(R"json({"terminator": ";"})json");
        QCOMPARE(semicolon->feed("a=1;b"), (QList<Message>{Message{{"a", 1LL}}}));
        QCOMPARE(semicolon->feed("=2; ;c=3\n;"), (QList<Message>{Message{{"b", 2LL}}, Message{{"c", 3LL}}}));
        // Both ends of a record lose their blanks: "\n" also ends a CR LF line, and with "\r" the
        // LF of a CR LF is the next record's leading blank.
        const QList<Message> two{Message{{"a", 1LL}}, Message{{"b", 2LL}}};
        QCOMPARE(lineParser("{}")->feed("a=1\r\nb=2\r\n"), two);
        auto cr = lineParser(R"json({"terminator": "\r"})json");
        QCOMPARE(cr->feed("a=1\r\nb=2\r"), two);
        QVERIFY(cr->feed("\nc=3\n").isEmpty()); // waits for its CR
        // A terminator of several characters may be split over two reads.
        auto crlf = lineParser(R"json({"terminator": "\r\n"})json");
        QVERIFY(crlf->feed("a=1\r").isEmpty());
        QCOMPARE(crlf->feed("\nb=2\r\n"), two);
        QCOMPARE(lineTerminator({}), QByteArray("\n"));
        QCOMPARE(lineTerminator(QJsonObject{{"terminator", "。"}, {"encoding", "gbk"}}), QByteArray("\xa1\xa3"));
    }

    void lineKeyValueSeparatorIsTheOnlyOne()
    {
        auto colon = lineParser(R"json({"kvSeparator": ":"})json");
        QCOMPARE(colon->feed("temp:25.3, hum : 60\n"), (QList<Message>{Message{{"temp", 25.3}, {"hum", 60LL}}}));
        QCOMPARE(colon->feed("temp=25\n"), (QList<Message>{Message{{"ch0", "temp=25"}}}));
        QCOMPARE(lineParser("{}")->feed("temp:25\n"), (QList<Message>{Message{{"ch0", "temp:25"}}}));
        // One part with the separator makes a record pairs; the parts without one are left out.
        QCOMPARE(colon->feed("x,y:2:3\n"), (QList<Message>{Message{{"y", "2:3"}}}));
    }

    void lineDecodesItsEncoding()
    {
        auto gbk = lineParser(R"json({"encoding": "gbk"})json");
        QCOMPARE(gbk->feed("\xce\xc2\xb6\xc8=25\n"), (QList<Message>{Message{{QStringLiteral("温度"), 25LL}}}));
        QCOMPARE(lineParser(R"json({"encoding": "GBK"})json")->feed("\xce\xc2=1\n").value(0).firstKey(),
                 QStringLiteral("温"));
        // Python's errors="replace" for gbk: a byte that starts no character it knows is one U+FFFD,
        // and the byte after it is read again; ICU's private-use characters for the user-defined
        // area (AA A1 here) are not Python's either. A lone lead byte at the end is one U+FFFD.
        QCOMPARE(gbk->feed("\x80\xaa\xa1\x81\x30=1\n").value(0).firstKey(), QStringLiteral("����0"));
        QCOMPARE(gbk->feed("\xce\xc2\xce\n").value(0).value("ch0"), QVariant(QStringLiteral("温�")));
        // UTF-8: one U+FFFD for each maximal subpart, as Python gives it.
        auto utf8 = lineParser("{}");
        QCOMPARE(utf8->feed("\xe6\xb8\xa9\xe5\xba\xa6=25\n"), (QList<Message>{Message{{QStringLiteral("温度"), 25LL}}}));
        QCOMPARE(utf8->feed("\xe4\xb8" "a=1\n").value(0).firstKey(), QStringLiteral("�a"));
        QCOMPARE(utf8->feed("\xed\xa0\x80=2\n").value(0).firstKey(), QStringLiteral("���"));
        QCOMPARE(utf8->feed("\xf0\x9f\x98\x80=3\n").value(0).firstKey(), QString::fromUtf8("\xf0\x9f\x98\x80"));
        // ASCII: every byte past 7F is U+FFFD, not a Latin-1 letter.
        QCOMPARE(lineParser(R"json({"encoding": "ascii"})json")->feed("\xce\xc2=1\n").value(0).firstKey(),
                 QStringLiteral("��"));
        // Python's strip() also takes the information separators 1C..1F.
        QCOMPARE(utf8->feed("\x1c a=1 \x1f\n"), (QList<Message>{Message{{"a", 1LL}}}));
    }

    void lineChecksNmeaSentences()
    {
        auto parser = lineParser("{}");
        const QList<Message> gga =
            parser->feed("$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47\r\n");
        QCOMPARE(gga.size(), 1);
        QCOMPARE(gga.at(0).size(), 15);
        QCOMPARE(gga.at(0).value("ch0"), QVariant(QStringLiteral("GPGGA")));
        QCOMPARE(gga.at(0).value("ch1"), QVariant(123519LL));
        QCOMPARE(gga.at(0).value("ch14"), QVariant(QString()));
        // Lower-case digits do; a wrong checksum drops the sentence.
        QCOMPARE(parser->feed("$GPRMC,1,A*3b\n!GPRMC,1,A*3C\n"),
                 (QList<Message>{Message{{"ch0", "GPRMC"}, {"ch1", 1LL}, {"ch2", "A"}}}));
        // Without *HH a $ record is an ordinary one and keeps its $; pairs work as everywhere.
        QCOMPARE(parser->feed("$GPRMC,1*4\n"), (QList<Message>{Message{{"ch0", "$GPRMC"}, {"ch1", "1*4"}}}));
        QCOMPARE(parser->feed(" $PX,t=5*58 \n").value(0), (Message{{"t", 5LL}}));
    }

    void lineJsonNeedsNoTerminator()
    {
        auto parser = lineParser("{}");
        // Pretty-printed over several lines, then two objects back to back.
        const QList<Message> pretty = parser->feed("{\n  \"temp\": 25.5,\n  \"data\": {\"t\": [1, 2]}\n}\n");
        QCOMPARE(pretty.size(), 1);
        QCOMPARE(pretty.at(0).value("temp").toDouble(), 25.5);
        QCOMPARE(pretty.at(0).value("data").toMap().value("t").toList().size(), 2);
        QCOMPARE(parser->feed("{\"a\":1}{\"a\":2}"), (QList<Message>{Message{{"a", 1LL}}, Message{{"a", 2LL}}}));
        // An array's items are keys "0", "1", ...: JSON路径 0.temp and 键值匹配 0 reach them.
        QCOMPARE(parser->feed("[1, 2.5, \"x\"]\n"), (QList<Message>{Message{{"0", 1LL}, {"1", 2.5}, {"2", "x"}}}));
        QCOMPARE(parser->feed("[{\"temp\": 3}]").value(0).value("0").toMap().value("temp"), QVariant(3LL));
        // The value ends at its bracket; what follows is a record of its own.
        QCOMPARE(parser->feed("{\"a\":1}xyz\n"), (QList<Message>{Message{{"a", 1LL}}, Message{{"ch0", "xyz"}}}));
        // Brackets and escaped quotes inside strings do not count, byte by byte as well.
        const QList<Message> tricky = fedBytewise(*parser, "  {\"s\":\"}{\\\"[\",\"n\":[1,{\"m\":2}]}");
        QCOMPARE(tricky.size(), 1);
        QCOMPARE(tricky.at(0).value("s"), QVariant(QStringLiteral("}{\"[")));
        // Broken or empty JSON is dropped, and the next value is read.
        QCOMPARE(parser->feed("{\"a\":}{}[]{\"b\":2}\n"), (QList<Message>{Message{{"b", 2LL}}}));
        // One terminator right after a value goes with it.
        auto semicolon = lineParser(R"json({"terminator": ";"})json");
        QCOMPARE(semicolon->feed("{\"a\":1};;b=2;"), (QList<Message>{Message{{"a", 1LL}}, Message{{"b", 2LL}}}));
        // JSON in GBK is decoded as GBK.
        QCOMPARE(lineParser(R"json({"encoding": "gbk"})json")->feed("{\"\xce\xc2\": 1}").value(0).firstKey(),
                 QStringLiteral("温"));
    }

    // JSON that never closes is dropped once the buffer is full, up to the next terminator.
    void lineOverlongJsonIsDiscarded()
    {
        LineParser parser({}, 16);
        QVERIFY(parser.feed("{\"a\":\"0123").isEmpty());
        QCOMPARE(parser.feed("456789abcdef\nx=1\n"), (QList<Message>{Message{{"x", 1LL}}}));
        QCOMPARE(parser.feed("{\"b\":2}\n"), (QList<Message>{Message{{"b", 2LL}}}));
    }

    void lineErrorsName_data()
    {
        QTest::addColumn<QByteArray>("config");
        QTest::addColumn<QString>("error");
        const auto row = [](const char *name, const char *config, const char *error) {
            QTest::newRow(name) << QByteArray(config) << QString::fromUtf8(error);
        };
        row("separator", R"json({"separator": ""})json", "行分隔符不能为空");
        row("terminator", R"json({"terminator": ""})json", "行结束符不能为空");
        row("terminator not text", R"json({"terminator": 10})json", "行结束符不能为空");
        row("kv separator", R"json({"kvSeparator": "-"})json", "键值分隔符只能是 = 或 :");
        row("kv separators", R"json({"kvSeparator": "=:"})json", "键值分隔符只能是 = 或 :");
        row("encoding", R"json({"encoding": "latin-1"})json", "编码只能是 utf-8、gbk 或 ascii");
        row("encoding not text", R"json({"encoding": 8})json", "编码只能是 utf-8、gbk 或 ascii");
        row("all valid", R"json({"terminator": "\r\n", "kvSeparator": ":", "encoding": "UTF-8"})json", "");
        row("defaults", "{}", "");
    }

    void lineErrorsName()
    {
        QFETCH(QByteArray, config);
        QFETCH(QString, error);
        QCOMPARE(lineError(config.constData()), error);
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
        QCOMPARE(error, QStringLiteral("未知解析模式: nope"));
        QVERIFY(!createParser("line", QJsonObject{{"separator", ""}}, &error));
        QCOMPARE(error, QStringLiteral("行分隔符不能为空"));
        // An empty header or footer is no longer an error: the frame can end another way.
        QVERIFY(createParser("hex", QJsonObject{{"header", ""}}, &error));
        QVERIFY(createParser("hex", QJsonObject{{"header", "AA 55"}, {"footer", "0D0A"}}, &error));
        QVERIFY(createParser("modbus_tcp", {}, &error));
    }

    // Line and Modbus parsers buffer by themselves, so a unit is the bytes as they came.
    void textAndModbusUnitsAreTheChunks()
    {
        for (const char *type : {"line", "modbus", "modbus_tcp"}) {
            auto parser = createParser(QLatin1String(type), {});
            QCOMPARE(parser->frames("a=1\nb"), QList<QByteArray>{QByteArray("a=1\nb")});
            QVERIFY(parser->flush().isEmpty());
            QCOMPARE(parser->idleTimeout(), 0);
        }
        LineParser line;
        QCOMPARE(line.parse("a=1\nb").size(), 1);
        QCOMPARE(line.parse("=2\n").value(0).value("b").toLongLong(), 2);
    }

    void hexErrorsName_data()
    {
        QTest::addColumn<QByteArray>("config");
        QTest::addColumn<QString>("error");
        const auto row = [](const char *name, const char *config, const char *error) {
            QTest::newRow(name) << QByteArray(config) << QString::fromUtf8(error);
        };
        row("no framing", R"json({"footer": ""})json", "HEX 帧需要帧尾、长度字段、固定帧长或空闲超时之一来分帧");
        row("no framing, spaces", R"json({"header": "", "footer": " "})json",
            "HEX 帧需要帧尾、长度字段、固定帧长或空闲超时之一来分帧");
        row("bad header", R"json({"header": "AG"})json", "HEX 帧头或帧尾不是十六进制");
        row("odd footer", R"json({"footer": "55A"})json", "HEX 帧头或帧尾不是十六进制");
        row("header not text", R"json({"header": 170})json", "HEX 帧头或帧尾不是十六进制");
        row("fixed and length", R"json({"fixedLength": 8, "lengthOffset": 2})json", "固定帧长和长度字段只能选一个");
        row("length offset", R"json({"lengthOffset": -2})json", "HEX 长度字段设置无效");
        row("length size", R"json({"lengthOffset": 2, "lengthSize": 3})json", "HEX 长度字段设置无效");
        row("length endian", R"json({"lengthEndian": "BE"})json", "HEX 长度字段设置无效");
        row("length covers", R"json({"lengthCovers": "全部"})json", "HEX 长度字段设置无效");
        row("fixed negative", R"json({"fixedLength": -1})json", "HEX 固定帧长无效");
        row("fixed too long", R"json({"fixedLength": 65537})json", "HEX 固定帧长无效");
        row("checksum", R"json({"checksum": "CRC32"})json", "HEX 校验设置无效");
        row("checksum start", R"json({"checksumStart": -1})json", "HEX 校验设置无效");
        row("checksum endian", R"json({"checksumEndian": "LE"})json", "HEX 校验设置无效");
        row("idle negative", R"json({"idleTimeout": -1})json", "HEX 空闲超时无效");
        row("idle too long", R"json({"idleTimeout": 60001})json", "HEX 空闲超时无效");
        row("field items", R"json({"fields": [["a", 0, 1]]})json", "无效 HEX 字段: a");
        row("field offset", R"json({"fields": [["a", -1, 1, "uint8"]]})json", "无效 HEX 字段: a");
        row("field length", R"json({"fields": [["a", 0, 0, "ASCII"]]})json", "无效 HEX 字段: a");
        row("field size", R"json({"fields": [["a", 0, 2, "int32"]]})json", "无效 HEX 字段: a");
        row("field type", R"json({"fields": [["a", 0, 2, "word"]]})json", "无效 HEX 字段: a");
        row("field endian", R"json({"fields": [["a", 0, 2, "int16", "LE"]]})json", "无效 HEX 字段: a");
        row("field items, six", R"json({"fields": [["a", 0, 2, "int16", "小端(LE)", 1]]})json", "无效 HEX 字段: a");
    }

    void hexErrorsName()
    {
        QFETCH(QByteArray, config);
        QFETCH(QString, error);
        QCOMPARE(hexError(config.constData()), error);
    }

    void hexAcceptsEveryFramingAlone()
    {
        QCOMPARE(hexError(R"json({"footer": "", "lengthOffset": 0})json"), QString());
        QCOMPARE(hexError(R"json({"footer": "", "fixedLength": 4})json"), QString());
        QCOMPARE(hexError(R"json({"footer": "", "idleTimeout": 20})json"), QString());
        QCOMPARE(hexError(R"json({"header": "aa 55", "footer": "0d 0A"})json"), QString());
        QCOMPARE(hexError(R"json({"fields": [["s", 0, 3, "ASCII"], ["h", 3, 5, "HEX", "大端(BE)"]]})json"), QString());
    }

    void checksumsMatchTheirCheckValues()
    {
        const QByteArray check("123456789");
        QCOMPARE(hexChecksum(HexChecksum::Sum, check), quint16(0xDD));
        QCOMPARE(hexChecksum(HexChecksum::Xor, check), quint16(0x31));
        QCOMPARE(hexChecksum(HexChecksum::Crc8, check), quint16(0xF4));
        QCOMPARE(hexChecksum(HexChecksum::Crc16Modbus, check), quint16(0x4B37));
        QCOMPARE(hexChecksum(HexChecksum::Crc16Ccitt, check), quint16(0x29B1));
        QCOMPARE(hexChecksumSize(HexChecksum::Crc8), 1);
        QCOMPARE(hexChecksumSize(HexChecksum::Crc16Ccitt), 2);
    }

    void lengthFieldFramesWholeFrames()
    {
        auto parser = hexParser(kConfigA);
        QVERIFY(parser);
        QCOMPARE(parser->frames(kFrameA), QList<QByteArray>{kFrameA});
        QCOMPARE(parser->parse(kFrameA), QList<Message>{kMessageA});
        QCOMPARE(parser->idleTimeout(), 0);
        // Byte by byte, two frames stuck together, noise in front: the same frames.
        QCOMPARE(bytewise(*parser, kFrameA + kFrameA2), (QList<QByteArray>{kFrameA, kFrameA2}));
        QCOMPARE(parser->frames(kFrameA + kFrameA2), (QList<QByteArray>{kFrameA, kFrameA2}));
        QCOMPARE(parser->frames(QByteArray::fromHex("1234") + kFrameA), QList<QByteArray>{kFrameA});
        // AA 55 in the data does not cut the frame: the length field says where it ends.
        const QByteArray inner =
            QByteArray::fromHex("AA55 04 AA 55AA 3C") + char(0xAA + 0x55 + 0x04 + 0xAA + 0x55 + 0xAA + 0x3C);
        QCOMPARE(parser->frames(inner + kFrameA), (QList<QByteArray>{inner, kFrameA}));
        QCOMPARE(parser->parse(inner),
                 (QList<Message>{Message{{"cmd", 0xAALL}, {"temp", qlonglong(qint16(0xAA55))}, {"hum", 60LL}}}));
    }

    void badChecksumDropsOnlyThatFrame()
    {
        auto parser = hexParser(kConfigA);
        QByteArray bad = kFrameA;
        bad[7] = char(0x30);
        QCOMPARE(parser->frames(bad + kFrameA2), QList<QByteArray>{kFrameA2});
        QCOMPARE(parser->feed(bad + kFrameA2), QList<Message>{kMessageA2});
        QCOMPARE(bytewise(*parser, bad + kFrameA2), QList<QByteArray>{kFrameA2});
    }

    // A length that cannot be the frame's is noise that looked like a header: skip past it.
    void impossibleLengthsResync()
    {
        auto whole = hexParser(R"json({"header": "AA55", "footer": "", "lengthOffset": 2, "lengthCovers": "整帧",
                                   "fields": [["v", 1, 1, "uint8"]]})json");
        const QByteArray good = QByteArray::fromHex("AA55 05 07 09");
        QCOMPARE(whole->frames(QByteArray::fromHex("AA55 02") + good), QList<QByteArray>{good});
        QCOMPARE(whole->frames(QByteArray::fromHex("AA55 01") + good), QList<QByteArray>{good});
        auto wide = hexParser(R"json({"header": "AA55", "footer": "", "lengthOffset": 2, "lengthSize": 2,
                                  "checksum": "异或"})json");
        const QByteArray body = QByteArray::fromHex("AA55 0001 07");
        const QByteArray frame = body + char(hexChecksum(HexChecksum::Xor, body));
        QCOMPARE(wide->frames(QByteArray::fromHex("AA55 FFFF") + frame), QList<QByteArray>{frame});
        auto little = hexParser(R"json({"header": "AA55", "footer": "", "lengthOffset": 2, "lengthSize": 2,
                                    "lengthEndian": "小端(LE)", "lengthCovers": "长度字段之后"})json");
        const QByteArray two = QByteArray::fromHex("AA55 0200 0102");
        QCOMPARE(little->frames(two + two), (QList<QByteArray>{two, two}));
    }

    // With a footer as well, a frame measured by its length must end in that footer.
    void footerMustCloseAMeasuredFrame()
    {
        auto parser = hexParser(R"json({"header": "AA55", "footer": "0D0A", "lengthOffset": 2,
                                    "fields": [["a", 1, 2, "uint16"]]})json");
        const QByteArray good = QByteArray::fromHex("AA55 02 1122 0D0A");
        QCOMPARE(parser->frames(QByteArray::fromHex("AA55 02 1122 0D0B") + good), QList<QByteArray>{good});
        QCOMPARE(parser->parse(good), (QList<Message>{Message{{"a", 0x1122LL}}}));
        auto fixed = hexParser(R"json({"header": "7E", "footer": "7F", "fixedLength": 4})json");
        QCOMPARE(fixed->frames(QByteArray::fromHex("7E 01 02 03 7E 05 06 7F")),
                 QList<QByteArray>{QByteArray::fromHex("7E05067F")});
    }

    void lengthCanCountThreeWays()
    {
        const auto config = [](const char *covers) {
            return QStringLiteral(R"json({"header": "AA55", "footer": "", "lengthOffset": 2, "lengthCovers": "%1",
                "checksum": "和校验", "fields": [["cmd", 1, 1, "uint8"], ["temp", 2, 2, "int16", "小端(LE)"],
                ["hum", 4, 1, "uint8"]]})json")
                .arg(QString::fromUtf8(covers))
                .toUtf8();
        };
        const struct
        {
            const char *covers;
            const char *frame;
        } cases[] = {{"数据", "AA55 04 01 F6FF 3C 35"},
                     {"长度字段之后", "AA55 05 01 F6FF 3C 36"},
                     {"整帧", "AA55 08 01 F6FF 3C 39"}};
        for (const auto &c : cases) {
            auto parser = hexParser(config(c.covers).constData());
            QVERIFY2(parser, c.covers);
            const QByteArray frame = QByteArray::fromHex(c.frame);
            QCOMPARE(parser->frames(QByteArray::fromHex("00") + frame), QList<QByteArray>{frame});
            QCOMPARE(parser->parse(frame), QList<Message>{kMessageA});
        }
    }

    // Spec example B: fixed length, no header or footer, XOR checksum.
    void fixedLengthWithoutDelimiters()
    {
        auto parser = hexParser(R"json({"header": "", "footer": "", "fixedLength": 4, "checksum": "异或",
                                    "fields": [["v", 0, 2, "uint16"], ["f", 2, 1, "uint8"]]})json");
        QVERIFY(parser);
        const QByteArray stream = QByteArray::fromHex("00100111 00200222");
        QCOMPARE(parser->frames(stream), (QList<QByteArray>{stream.left(4), stream.mid(4)}));
        QCOMPARE(parser->feed(stream),
                 (QList<Message>{Message{{"v", 16LL}, {"f", 1LL}}, Message{{"v", 32LL}, {"f", 2LL}}}));
        // A bad checksum costs one byte without a header to look for.
        QCOMPARE(parser->frames(QByteArray::fromHex("FF") + stream.left(4)), QList<QByteArray>{stream.left(4)});
    }

    // Spec example C: a header and a silence.
    void idleEndsAFrameWithoutAnEndRule()
    {
        auto parser =
            hexParser(R"json({"header": "7E", "footer": "", "idleTimeout": 20, "fields": [["a", 0, 1, "uint8"]]})json");
        QVERIFY(parser);
        QCOMPARE(parser->idleTimeout(), 20);
        QVERIFY(parser->frames(QByteArray::fromHex("7E 01 02")).isEmpty());
        const QList<QByteArray> flushed = parser->flush();
        QCOMPARE(flushed, QList<QByteArray>{QByteArray::fromHex("7E0102")});
        QCOMPARE(parser->parse(flushed.value(0)), (QList<Message>{Message{{"a", 1LL}}}));
        QVERIFY(parser->flush().isEmpty());
        // What came before the header is dropped; a fragment of a header is no frame.
        QVERIFY(parser->frames(QByteArray::fromHex("00 11 7E 05")).isEmpty());
        QCOMPARE(parser->flush(), QList<QByteArray>{QByteArray::fromHex("7E05")});
        auto twoByte = hexParser(R"json({"header": "7E7F", "footer": "", "idleTimeout": 20})json");
        QVERIFY(twoByte->frames(QByteArray::fromHex("01 7E")).isEmpty());
        QVERIFY(twoByte->flush().isEmpty());
        QVERIFY(twoByte->frames(QByteArray::fromHex("7F 01")).isEmpty()); // the 7E went with the flush
        QVERIFY(twoByte->flush().isEmpty());
        // Without a header the whole burst is the frame, checksum and all.
        auto bare = hexParser(R"json({"header": "", "footer": "", "idleTimeout": 5, "checksum": "CRC16-Modbus",
                                  "checksumEndian": "小端(LE)", "fields": [["r", 3, 2, "uint16"]]})json");
        const QByteArray body = QByteArray::fromHex("01030200FF");
        QVERIFY(bare->frames(body + modbusCrc(body)).isEmpty());
        QCOMPARE(bare->feed({}), QList<Message>{});
        const QList<QByteArray> burst = bare->flush();
        QCOMPARE(burst, QList<QByteArray>{body + modbusCrc(body)});
        QCOMPARE(bare->parse(burst.value(0)), (QList<Message>{Message{{"r", 0x00FFLL}}}));
        QVERIFY(bare->frames(body + QByteArray(2, '\0')).isEmpty());
        QVERIFY(bare->flush().isEmpty()); // a bad checksum
    }

    // A buffer past 64 KiB without a frame loses its front, up to the next header.
    void overlongBuffersKeepTheirTail()
    {
        auto bare = hexParser(R"json({"header": "", "footer": "", "idleTimeout": 10})json");
        QVERIFY(bare->frames(QByteArray(70000, 'x')).isEmpty());
        QCOMPARE(bare->flush().value(0), QByteArray(65536, 'x'));
        auto headed = hexParser(R"json({"header": "7E", "footer": "", "idleTimeout": 10})json");
        const QByteArray second = QByteArray(1, 0x7E) + QByteArray(40000, 2);
        QVERIFY(headed->frames(QByteArray(1, 0x7E) + QByteArray(40000, 1) + second).isEmpty());
        QCOMPARE(headed->flush(), QList<QByteArray>{second});
        auto footer = hexParser(R"json({"header": "AA55", "footer": "55AA", "fields": [["v", 0, 1, "uint8"]]})json");
        QVERIFY(footer->frames(QByteArray::fromHex("AA55") + QByteArray(70000, 1)).isEmpty());
        QCOMPARE(footer->feed(QByteArray::fromHex("AA55 07 55AA")), (QList<Message>{Message{{"v", 7LL}}}));
    }

    // With an end rule the silence leaves half a frame, which goes.
    void idleDropsAHalfFrame()
    {
        auto parser = hexParser(R"json({"header": "AA55", "footer": "", "lengthOffset": 2, "checksum": "和校验",
                                    "idleTimeout": 50, "fields": [["cmd", 1, 1, "uint8"]]})json");
        QVERIFY(parser->frames(kFrameA.left(5)).isEmpty());
        QVERIFY(parser->flush().isEmpty());
        QCOMPARE(parser->frames(kFrameA.mid(5) + kFrameA2), QList<QByteArray>{kFrameA2});
    }

    // Spec example D: header, footer and a little-endian CRC16-Modbus that leaves out the header.
    void crcAfterTheHeader()
    {
        auto parser = hexParser(R"json({"header": "AA55", "footer": "0D0A", "checksum": "CRC16-Modbus",
                                    "checksumEndian": "小端(LE)", "checksumStart": 2, "fields": [["t", 0, 4, "float32"]]})json");
        QVERIFY(parser);
        QCOMPARE(hexChecksum(HexChecksum::Crc16Modbus, QByteArray::fromHex("41CA6666")), quint16(0xAC9F));
        const QByteArray frame = QByteArray::fromHex("AA55 41CA6666 9FAC 0D0A");
        QCOMPARE(parser->frames(frame), QList<QByteArray>{frame});
        QCOMPARE(parser->parse(frame), (QList<Message>{Message{{"t", 25.299999237060547}}}));
        QVERIFY(parser->frames(QByteArray::fromHex("AA55 41CA6666 AC9F 0D0A")).isEmpty()); // big-endian on the wire
        auto big =
            hexParser(R"json({"header": "AA55", "footer": "0D0A", "checksum": "CRC16-CCITT", "checksumStart": 2})json");
        const quint16 crc = hexChecksum(HexChecksum::Crc16Ccitt, QByteArray::fromHex("41CA6666"));
        const QByteArray ccitt = QByteArray::fromHex("AA5541CA6666") + char(crc >> 8) + char(crc & 0xFF) + "\r\n";
        QCOMPARE(big->frames(ccitt), QList<QByteArray>{ccitt});
        // A checksum range that starts past the checksum makes every frame invalid.
        auto late =
            hexParser(R"json({"header": "AA55", "footer": "0D0A", "checksum": "CRC8", "checksumStart": 9})json");
        QVERIFY(late->frames(QByteArray::fromHex("AA55 01 00 0D0A")).isEmpty());
    }

    void fieldTypesAndByteOrders()
    {
        auto parser = hexParser(R"json({"header": "", "footer": "", "fixedLength": 64, "fields": [
            ["u8", 0, 1, "uint8"], ["i8", 1, 1, "int8"],
            ["u16", 2, 2, "uint16"], ["u16le", 2, 2, "uint16", "小端(LE)"],
            ["i16", 4, 2, "int16", "大端(BE)"], ["i16le", 4, 2, "int16", "小端(LE)"],
            ["u32", 6, 4, "uint32"], ["u32le", 6, 4, "uint32", "小端(LE)"],
            ["i32", 10, 4, "int32"], ["i32le", 10, 4, "int32", "小端(LE)"],
            ["f32", 14, 4, "float32"], ["f32le", 18, 4, "float32", "小端(LE)"],
            ["f64", 22, 8, "float64"], ["f64le", 30, 8, "float64", "小端(LE)"],
            ["text", 38, 6, "ASCII"], ["odd", 44, 3, "ASCII"], ["raw", 47, 3, "HEX"],
            ["edge", 63, 1, "uint8"], ["past", 63, 2, "uint16"]]})json");
        QVERIFY(parser);
        QByteArray frame = QByteArray::fromHex("FF 80 0102 FFFE 01020304 FFFFFFFE 41200000 00002041"
                                               "4024000000000000 0000000000002440"
                                               "4F4B00585858 41FF42 0A0BFF");
        frame += QByteArray(64 - frame.size() - 1, '\0') + char(0x7F);
        const Message m = parser->parse(frame).value(0);
        QCOMPARE(m.value("u8"), QVariant(255LL));
        QCOMPARE(m.value("i8"), QVariant(-128LL));
        QCOMPARE(m.value("u16"), QVariant(0x0102LL));
        QCOMPARE(m.value("u16le"), QVariant(0x0201LL));
        QCOMPARE(m.value("i16"), QVariant(-2LL));
        QCOMPARE(m.value("i16le"), QVariant(qlonglong(qint16(0xFEFF))));
        QCOMPARE(m.value("u32"), QVariant(0x01020304LL));
        QCOMPARE(m.value("u32le"), QVariant(0x04030201LL));
        QCOMPARE(m.value("i32"), QVariant(-2LL));
        QCOMPARE(m.value("i32le"), QVariant(qlonglong(qint32(0xFEFFFFFF))));
        QCOMPARE(m.value("f32"), QVariant(10.0));
        QCOMPARE(m.value("f32le"), QVariant(10.0));
        QCOMPARE(m.value("f64"), QVariant(10.0));
        QCOMPARE(m.value("f64le"), QVariant(10.0));
        QCOMPARE(m.value("text"), QVariant(QStringLiteral("OK"))); // up to the first NUL
        QCOMPARE(m.value("odd"), QVariant(QStringLiteral("A\uFFFDB")));
        QCOMPARE(m.value("raw"), QVariant(QStringLiteral("0a0bff")));
        QCOMPARE(m.value("edge"), QVariant(0x7FLL));
        QVERIFY(!m.contains("past")); // beyond the frame: skipped
        QCOMPARE(m.value("u8").typeId(), int(QMetaType::LongLong));
        QCOMPARE(m.value("f32").typeId(), int(QMetaType::Double));
    }

    // Spec example E: a project from before length fields frames exactly as it did.
    void oldConfigsFrameAsBefore()
    {
        auto parser = hexParser(R"json({"header": "AA55", "footer": "55AA", "fields": [["temp", 0, 2, "int16"]]})json");
        QVERIFY(parser);
        const QByteArray frame = QByteArray::fromHex("AA55 00FA 55AA");
        QVERIFY(parser->frames(QByteArray::fromHex("0011AA") + frame.left(1)).isEmpty());
        QCOMPARE(parser->frames(frame.mid(1)), QList<QByteArray>{frame});
        QCOMPARE(parser->parse(frame), (QList<Message>{Message{{"temp", 250LL}}}));
        // The footer is looked for after the header; a frame too short for a field is no message.
        QCOMPARE(parser->feed(QByteArray::fromHex("AA55 55AA AA55 FFFF 55AA")),
                 (QList<Message>{Message{{"temp", -1LL}}}));
        QVERIFY(parser->frames(QByteArray::fromHex("AA 55AA")).isEmpty()); // the header's 55 is not a footer
        QVERIFY(parser->flush().isEmpty());
        QCOMPARE(parser->frames(QByteArray::fromHex("55AA")), QList<QByteArray>{}); // flushed away
        // Defaults when nothing is set.
        auto defaults = createParser(QStringLiteral("hex"), {});
        QCOMPARE(defaults->frames(QByteArray::fromHex("AA55 01 55AA")),
                 QList<QByteArray>{QByteArray::fromHex("AA550155AA")});
    }

    void feedIsFramesThenParse()
    {
        const QByteArray stream = QByteArray::fromHex("12") + kFrameA + kFrameA2.left(3);
        auto split = hexParser(kConfigA);
        auto fed = hexParser(kConfigA);
        // Separate statements: the operands of + have no defined evaluation order.
        QList<QByteArray> frames = split->frames(stream);
        frames += split->frames(kFrameA2.mid(3));
        QList<Message> viaFrames;
        for (const QByteArray &frame : frames) {
            viaFrames += split->parse(frame);
        }
        QList<Message> fedMessages = fed->feed(stream);
        fedMessages += fed->feed(kFrameA2.mid(3));
        QCOMPARE(fedMessages, viaFrames);
        QCOMPARE(viaFrames, (QList<Message>{kMessageA, kMessageA2}));
    }
};

QTEST_APPLESS_MAIN(TestProtocolParser)
#include "tst_protocol_parser.moc"
