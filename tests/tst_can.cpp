// CAN: the DBC parser and the signal codec against the golden vectors cantools produced, the bit timing against
// the reference rule's vectors, and the small text formats users type (IDs, inline signals, cansend lines, slcan).
#include "comm/can/slcan_protocol.h"
#include "core/can_frame.h"
#include "core/can_signal.h"
#include "core/can_timing.h"
#include "core/dbc.h"

#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtTest/QtTest>

#include <cmath>
#include <memory>

using namespace fhmi;

namespace {

QByteArray readData(const QString &name)
{
    QFile file(QStringLiteral(FHMI_TEST_DATA_DIR "/") + name);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

QJsonObject vectors(const QString &name)
{
    return QJsonDocument::fromJson(readData(name)).object();
}

std::shared_ptr<const CanDatabase> database()
{
    QString error;
    auto db = Dbc::parse(readData(QStringLiteral("can_vectors.dbc")), &error);
    return db ? std::make_shared<const CanDatabase>(*db) : nullptr;
}

QByteArray hex(const QString &text)
{
    return QByteArray::fromHex(text.toLatin1());
}

// Doubles within a relative 1e-12, as the vectors ask; whole numbers exactly.
bool sameNumber(const QVariant &actual, const QJsonValue &expected)
{
    bool ok = false;
    const double value = actual.toDouble(&ok);
    if (!ok) {
        return false;
    }
    const double want = expected.toDouble();
    if (actual.typeId() == QMetaType::LongLong && std::fabs(want) < 9007199254740992.0) {
        return actual.toLongLong() == qint64(want);
    }
    return std::fabs(value - want) <= 1e-12 * std::max(1.0, std::fabs(want));
}

} // namespace

class TestCan : public QObject
{
    Q_OBJECT

  private slots:
    void dbcMessages()
    {
        const auto db = database();
        QVERIFY(db);
        const QJsonArray messages = vectors(QStringLiteral("can_signal_vectors.json")).value("messages").toArray();
        QCOMPARE(db->messages.size(), messages.size());
        for (const QJsonValue &value : messages) {
            const QJsonObject expected = value.toObject();
            const QString name = expected.value("name").toString();
            const CanMessageDef *message = db->findByName(name);
            QVERIFY2(message, qPrintable(name));
            QCOMPARE(qint64(message->id.id), expected.value("id").toInteger());
            QCOMPARE(message->id.extended, expected.value("extended").toBool());
            QCOMPARE(message->fd, expected.value("fd").toBool());
            QCOMPARE(message->length, expected.value("length").toInt());
            QCOMPARE(message->cycleTime, expected.value("cycleTime").toInt());
            const QJsonArray signalList = expected.value("signals").toArray();
            QCOMPARE(message->signalDefs.size(), signalList.size());
            // cantools sorts signals by start bit; the parser keeps the file's order, so they are matched by name.
            for (const QJsonValue &item : signalList) {
                const QJsonObject want = item.toObject();
                const QString signalName = want.value("name").toString();
                const CanSignalDef *signal = nullptr;
                for (const CanSignalDef &candidate : message->signalDefs) {
                    signal = candidate.name == signalName ? &candidate : signal;
                }
                QVERIFY2(signal, qPrintable(name + "." + signalName));
                QCOMPARE(signal->start, want.value("start").toInt());
                QCOMPARE(signal->length, want.value("length").toInt());
                QCOMPARE(signal->motorola, want.value("byteOrder").toString() == QLatin1String("motorola"));
                QCOMPARE(signal->isSigned, want.value("signed").toBool());
                const QString type = want.value("valueType").toString();
                const CanSignalDef::ValueType valueType = type == QLatin1String("float")
                                                              ? CanSignalDef::ValueType::Float32
                                                          : type == QLatin1String("double")
                                                              ? CanSignalDef::ValueType::Double
                                                              : CanSignalDef::ValueType::Integer;
                QVERIFY(signal->valueType == valueType);
                QCOMPARE(signal->factor, want.value("factor").toDouble());
                QCOMPARE(signal->offset, want.value("offset").toDouble());
                QCOMPARE(signal->minimum, want.value("minimum").toDouble());
                QCOMPARE(signal->maximum, want.value("maximum").toDouble());
                QCOMPARE(signal->unit, want.value("unit").toString());
                const QJsonObject descriptions = want.value("values").toObject();
                QCOMPARE(signal->valueDescriptions.size(), descriptions.size());
                for (auto it = descriptions.begin(); it != descriptions.end(); ++it) {
                    QCOMPARE(signal->valueDescriptions.value(it.key().toLongLong()), it.value().toString());
                }
            }
        }
    }

    void decodeVectors()
    {
        const auto db = database();
        QVERIFY(db);
        const auto set = CanSignalSet::build(db, QString());
        QVERIFY(set);
        for (const QJsonValue &value : vectors(QStringLiteral("can_signal_vectors.json")).value("decode").toArray()) {
            const QJsonObject item = value.toObject();
            const QString name = item.value("message").toString();
            CanFrame frame;
            frame.id = quint32(item.value("id").toInteger());
            frame.extended = item.value("extended").toBool();
            frame.data = hex(item.value("data").toString());
            const Message message = set->decode(frame);
            const QJsonObject expect = item.value("expect").toObject();
            const QJsonObject texts = item.value("text").toObject();
            int textKeys = 0;
            for (auto it = message.begin(); it != message.end(); ++it) {
                textKeys += it.key().endsWith(QLatin1String(".text")) ? 1 : 0;
            }
            QCOMPARE(textKeys, 2 * texts.size()); // qualified and bare
            for (auto it = expect.begin(); it != expect.end(); ++it) {
                const QString qualified = name + QLatin1Char('.') + it.key();
                QVERIFY2(sameNumber(message.value(qualified), it.value()),
                         qPrintable(QStringLiteral("%1 = %2, expected %3")
                                        .arg(qualified, message.value(qualified).toString())
                                        .arg(it.value().toDouble(), 0, 'g', 17)));
                QVERIFY(sameNumber(message.value(it.key()), it.value()));
                if (texts.contains(it.key())) {
                    QCOMPARE(message.value(qualified + QLatin1String(".text")).toString(),
                             texts.value(it.key()).toString());
                    QCOMPARE(message.value(it.key() + QLatin1String(".text")).toString(),
                             texts.value(it.key()).toString());
                }
            }
        }
    }

    void encodeVectors()
    {
        const auto db = database();
        QVERIFY(db);
        for (const QJsonValue &value : vectors(QStringLiteral("can_signal_vectors.json")).value("encode").toArray()) {
            const QJsonObject item = value.toObject();
            const CanMessageDef *message = db->findByName(item.value("message").toString());
            QVERIFY(message);
            QByteArray data(message->length, '\0');
            const QJsonObject values = item.value("values").toObject();
            for (const CanSignalDef &signal : message->signalDefs) {
                const QJsonValue physical = values.value(signal.name);
                QVERIFY2(!physical.isUndefined(), qPrintable(signal.name));
                QString error;
                // Numbers go in as a command writes them, as text; value descriptions as their text.
                const QString text = physical.isString() ? physical.toString()
                                                         : QString::number(physical.toDouble(), 'g', 17);
                const auto bits = signal.encode(text, &error);
                QVERIFY2(bits, qPrintable(error));
                signal.putRawBits(data, *bits);
            }
            QCOMPARE(data.toHex(' ').toUpper(), item.value("data").toString().toLatin1());
        }
    }

    void encodeRefusesWhatDoesNotFit()
    {
        const auto db = database();
        QVERIFY(db);
        const CanMessageDef *message = db->findByName(QStringLiteral("EngineData"));
        QVERIFY(message);
        const CanSignalDef &temp = message->signalDefs.at(1); // EngTemp: 7 bits signed, factor 2
        QCOMPARE(temp.name, QStringLiteral("EngTemp"));
        QString error;
        QVERIFY(!temp.encode(QStringLiteral("200"), &error)); // raw 100 > 63
        QVERIFY(error.contains(QStringLiteral("EngTemp")));
        QVERIFY(!temp.encode(QStringLiteral("nan"), &error));
        QVERIFY(!temp.encode(QStringLiteral("Drive"), &error)); // no such description
        QVERIFY(temp.encode(QStringLiteral("-128"), &error));   // raw -64
        QVERIFY(!temp.encode(QStringLiteral("-130"), &error));  // raw -65
    }

    // A binding's Motorola LSB start counts from the signal's LSB; it becomes DBC's MSB start before decoding.
    // Cases checked by hand, the data as frame bytes in hex.
    void motorolaLsbStart()
    {
        struct Case
        {
            int lsbStart;
            int length;
            int msbStart;
            const char *data;
            qint64 value;
        };
        const Case cases[] = {{8, 16, 7, "1234", 0x1234}, {8, 12, 3, "0ABC", 0xABC}, {4, 4, 7, "A0", 0xA}};
        for (const Case &c : cases) {
            const auto msb = motorolaMsbStart(c.lsbStart, c.length);
            QVERIFY(msb);
            QCOMPARE(*msb, c.msbStart);
            CanSignalDef signal;
            signal.start = *msb;
            signal.length = c.length;
            signal.motorola = true;
            QVERIFY(signal.validate(nullptr));
            QCOMPARE(signal.decode(hex(QLatin1String(c.data))).toLongLong(), c.value);
        }
        QVERIFY(!motorolaMsbStart(0, 16)); // its MSB would be in byte -1
    }

    void timingVectors()
    {
        const QJsonObject data = vectors(QStringLiteral("can_timing_vectors.json"));
        const QJsonObject limits = data.value("limits").toObject();
        const auto limitsOf = [&limits](const QString &name) {
            const QJsonObject l = limits.value(name).toObject();
            const QJsonArray brp = l.value("brp").toArray();
            const QJsonArray tseg1 = l.value("tseg1").toArray();
            const QJsonArray tseg2 = l.value("tseg2").toArray();
            return CanTimingLimits{brp.at(0).toInt(),   brp.at(1).toInt(),   tseg1.at(0).toInt(),
                                   tseg1.at(1).toInt(), tseg2.at(0).toInt(), tseg2.at(1).toInt(),
                                   l.value("sjw").toInt(), l.value("minTq").toInt()};
        };
        // The adapters' own limits are the vectors'.
        const CanTimingLimits sja = limitsOf(QStringLiteral("sja1000"));
        QCOMPARE(sja.brpMax, CanBitTiming::Sja1000.brpMax);
        QCOMPARE(sja.tseg1Max, CanBitTiming::Sja1000.tseg1Max);
        QCOMPARE(sja.tseg2Max, CanBitTiming::Sja1000.tseg2Max);
        QCOMPARE(sja.sjwMax, CanBitTiming::Sja1000.sjwMax);
        const QJsonArray cases = data.value("cases").toArray();
        QVERIFY(!cases.isEmpty());
        for (const QJsonValue &value : cases) {
            const QJsonObject item = value.toObject();
            const CanTiming timing =
                CanBitTiming::compute(quint32(item.value("clock").toInteger()), quint32(item.value("bitrate").toInteger()),
                                      item.value("samplePoint").toDouble(),
                                      limitsOf(item.value("limits").toString()), item.value("preferredTq").toInt());
            const QString label = QString::fromUtf8(QJsonDocument(item).toJson(QJsonDocument::Compact));
            if (item.value("expect").isNull()) {
                QVERIFY2(!timing.isValid(), qPrintable(label));
                continue;
            }
            const QJsonObject expect = item.value("expect").toObject();
            QVERIFY2(timing.isValid(), qPrintable(label));
            QVERIFY2(timing.brp == expect.value("brp").toInt() && timing.tseg1 == expect.value("tseg1").toInt() &&
                         timing.tseg2 == expect.value("tseg2").toInt() && timing.sjw == expect.value("sjw").toInt(),
                     qPrintable(QStringLiteral("%1 -> %2,%3,%4,%5")
                                    .arg(label)
                                    .arg(timing.brp)
                                    .arg(timing.tseg1)
                                    .arg(timing.tseg2)
                                    .arg(timing.sjw)));
            QCOMPARE(timing.tq(), expect.value("tq").toInt());
            QCOMPARE(std::round(timing.samplePoint() * 10000) / 10000, expect.value("samplePoint").toDouble());
        }
        for (const QJsonValue &value : data.value("btr").toArray()) {
            const QJsonObject item = value.toObject();
            const CanTiming timing{item.value("brp").toInt(), item.value("tseg1").toInt(), item.value("tseg2").toInt(),
                                   item.value("sjw").toInt()};
            QCOMPARE(QStringLiteral("%1").arg(CanBitTiming::sja1000Btr(timing), 4, 16, QLatin1Char('0')).toUpper(),
                     item.value("btr0btr1").toString());
        }
    }

    void customTiming()
    {
        QString error;
        const auto both = CanBitTiming::parseCustom(QStringLiteral("10,13,2,1; 2,7,2,1"), &error);
        QVERIFY2(both, qPrintable(error));
        QCOMPARE(both->nominal.brp, 10);
        QVERIFY(both->data && both->data->tseg1 == 7);
        QVERIFY(!CanBitTiming::parseCustom(QStringLiteral("10,13,2"), &error));
        QVERIFY(!CanBitTiming::check({65, 13, 2, 1}, CanBitTiming::Sja1000, &error)); // BRP above 64
        QVERIFY(error.contains(QStringLiteral("BRP")));
    }

    void inlineSignals()
    {
        QString error;
        const auto lines = parseInlineSignals(QStringLiteral("# engine\n"
                                                             "102 EngSpeed : 16|16@1+ (1,0) [0|8000] \"rpm\"\n"
                                                             "\n"
                                                             "// a bare one\n"
                                                             "0x18FF50E5 SG_ Temp : 7|8@0- (0.5,-40)\n"
                                                             "00000123 Flag : 0|1@1+\n"),
                                              &error);
        QVERIFY2(lines, qPrintable(error));
        QCOMPARE(lines->size(), 3);
        QCOMPARE(lines->at(0).id, (CanId{0x102, false}));
        QCOMPARE(lines->at(0).signal.unit, QStringLiteral("rpm"));
        QCOMPARE(lines->at(0).signal.maximum, 8000.0);
        QCOMPARE(lines->at(1).id, (CanId{0x18FF50E5, true}));
        QVERIFY(lines->at(1).signal.motorola && lines->at(1).signal.isSigned);
        QCOMPARE(lines->at(1).signal.factor, 0.5);
        QCOMPARE(lines->at(1).signal.offset, -40.0);
        QCOMPARE(lines->at(2).id, (CanId{0x123, true}));

        QVERIFY(!parseInlineSignals(QStringLiteral("102 A : 0|8@1+\n102 B : 8|8@2+\n"), &error));
        QVERIFY2(error.contains(QStringLiteral("2")), qPrintable(error)); // names the line
        // A name twice is an error, against other inline lines and against the DBC's signals of that ID.
        QVERIFY(!CanSignalSet::build(nullptr, QStringLiteral("102 A : 0|8@1+\n103 A : 0|8@1+\n"), &error));
        QVERIFY(!CanSignalSet::build(database(), QStringLiteral("102 EngSpeed : 0|8@1+\n"), &error));
        const auto set = CanSignalSet::build(database(), QStringLiteral("102 Extra : 56|8@1+\n"), &error);
        QVERIFY2(set, qPrintable(error));
        CanFrame frame{0x102, false, false, false, false, false, hex(QStringLiteral("75 40 B8 0B 00 00 00 2A")), 0};
        const Message message = set->decode(frame);
        QCOMPARE(message.value(QStringLiteral("Extra")).toLongLong(), 42);
        QCOMPARE(message.keys().last(), QStringLiteral("Extra")); // DBC signals first, then inline ones
    }

    void idSyntax()
    {
        QCOMPARE(Can::parseId(u"102"), (CanId{0x102, false}));
        QCOMPARE(Can::parseId(u"0x7ff"), (CanId{0x7FF, false}));
        QCOMPARE(Can::parseId(u"800"), (CanId{0x800, true}));            // above 0x7FF
        QCOMPARE(Can::parseId(u"00000123"), (CanId{0x123, true}));       // 8 digits, as cansend
        QCOMPARE(Can::parseId(u"0x18ff50e5"), (CanId{0x18FF50E5, true})); // case-insensitive
        QVERIFY(!Can::parseId(u"20000000"));                              // above 0x1FFFFFFF
        QVERIFY(!Can::parseId(u"123456789"));
        QVERIFY(!Can::parseId(u"12G"));
        QVERIFY(!Can::parseId(u""));
        QCOMPARE(Can::idText(0xA8, false), QStringLiteral("0x0A8"));
        QCOMPARE(Can::idText(0x18FF50E5, true), QStringLiteral("0x18FF50E5"));
        QCOMPARE(Can::idText(0x123, true), QStringLiteral("0x00000123"));
    }

    void fdPadding()
    {
        QCOMPARE(Can::fdPaddedLength(8), 8);
        QCOMPARE(Can::fdPaddedLength(9), 12);
        QCOMPARE(Can::fdPaddedLength(33), 48);
        QCOMPARE(Can::fdPaddedLength(64), 64);
        QCOMPARE(Can::fdPaddedLength(65), -1);
        QByteArray data(13, '\x11');
        QVERIFY(Can::padFdPayload(data));
        QCOMPARE(data.size(), 16);
        QCOMPARE(data.right(3), QByteArray(3, '\0'));
        QVERIFY(Can::isValidFdLength(20) && !Can::isValidFdLength(21));
    }

    void slcanLines()
    {
        CanFrame frame{0x102, false, false, false, false, false, hex(QStringLiteral("75 40")), 0};
        QCOMPARE(Slcan::encodeFrame(frame), QByteArray("t10227540\r"));
        frame = {0x18FF50E5, true, false, false, false, false, hex(QStringLiteral("01")), 0};
        QCOMPARE(Slcan::encodeFrame(frame), QByteArray("T18FF50E5101\r"));
        frame = {0x7FF, false, false, false, true, false, QByteArray(3, '\0'), 0};
        QCOMPARE(Slcan::encodeFrame(frame), QByteArray("r7FF3\r"));
        frame.fd = true;
        QVERIFY(!Slcan::encodeFrame(frame)); // no FD on slcan

        auto decoded = Slcan::decodeFrame("t10021133");
        QVERIFY(decoded && decoded->id == 0x100 && !decoded->extended && decoded->data == hex("1133"));
        decoded = Slcan::decodeFrame("t100211334D67"); // with the time stamp
        QVERIFY(decoded && decoded->data == hex("1133"));
        decoded = Slcan::decodeFrame("T18FF50E58C1433ECE16001003");
        QVERIFY(decoded && decoded->extended && decoded->id == 0x18FF50E5 && decoded->data.size() == 8);
        decoded = Slcan::decodeFrame("R000001232");
        QVERIFY(decoded && decoded->rtr && decoded->extended && decoded->data.size() == 2);
        QVERIFY(!Slcan::decodeFrame("t1002113"));  // half a byte
        QVERIFY(!Slcan::decodeFrame("t800100"));   // 0x800 is no standard ID
        QVERIFY(!Slcan::decodeFrame("t1009"));     // 9 bytes
        QVERIFY(!Slcan::decodeFrame("z"));
        QCOMPARE(Slcan::decodeStatus("F24"), std::optional<quint8>(0x24));
        QCOMPARE(Slcan::bitrateCode(500000), 6);
        QCOMPARE(Slcan::bitrateCode(33333), -1);
    }

    void sendText()
    {
        auto frame = Can::parseSendText(QStringLiteral("123#DEADBEEF"));
        QVERIFY(frame && frame->id == 0x123 && !frame->extended && !frame->fd && frame->data == hex("DEADBEEF"));
        frame = Can::parseSendText(QStringLiteral("00000123#11.22.33"));
        QVERIFY(frame && frame->extended && frame->data == hex("112233"));
        frame = Can::parseSendText(QStringLiteral("5AA# 11 22"));
        QVERIFY(frame && frame->data == hex("1122"));
        frame = Can::parseSendText(QStringLiteral("123#R"));
        QVERIFY(frame && frame->rtr && frame->data.isEmpty());
        frame = Can::parseSendText(QStringLiteral("123#R3"));
        QVERIFY(frame && frame->rtr && frame->data.size() == 3);
        frame = Can::parseSendText(QStringLiteral("123##1112233445566778899"));
        QVERIFY(frame && frame->fd && frame->brs && frame->data.size() == 12);
        QCOMPARE(frame->data.left(9), hex("112233445566778899"));
        frame = Can::parseSendText(QStringLiteral("123##0"));
        QVERIFY(frame && frame->fd && !frame->brs && frame->data.isEmpty());
        QVERIFY(!Can::parseSendText(QStringLiteral("hello")));
        QVERIFY(!Can::parseSendText(QStringLiteral("123#123")));          // half a byte
        QVERIFY(!Can::parseSendText(QStringLiteral("123#112233445566778899"))); // 9 bytes classic
        QVERIFY(!Can::parseSendText(QStringLiteral("#11")));
    }

    void frameLines()
    {
        const CanFrame frame{0x102, false, false, false, false, false, hex(QStringLiteral("7540B80B00000000")), 0};
        QCOMPARE(Can::frameLine(frame), QStringLiteral("0x102 [8] 75 40 B8 0B 00 00 00 00"));
        const CanFrame fd{0x18FF50E5, true, true, true, false, false, hex(QStringLiteral("01")), 0};
        QCOMPARE(Can::frameLine(fd), QStringLiteral("0x18FF50E5 [FD] [BRS] [EXT] [1] 01"));
    }

    void filter()
    {
        QString error;
        const auto filter = CanFilter::parse(QStringLiteral("100-1FF, 18FF50E5"), &error);
        QVERIFY2(filter, qPrintable(error));
        QVERIFY(filter->accepts(0x100, false) && filter->accepts(0x1FF, false) && !filter->accepts(0x200, false));
        QVERIFY(!filter->accepts(0x150, true));
        QVERIFY(filter->accepts(0x18FF50E5, true));
        QVERIFY(CanFilter::parse(QString())->accepts(0x7FF, false));
        QVERIFY(!CanFilter::parse(QStringLiteral("1FF-100"), &error));
    }
};

QTEST_APPLESS_MAIN(TestCan)
#include "tst_can.moc"
