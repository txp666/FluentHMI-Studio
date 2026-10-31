#include "core/binding.h"
#include "core/expression.h"

#include <QtTest/QtTest>

using namespace fhmi;

namespace {

QJsonObject widget(const QString &type, const QJsonObject &binding, const QJsonObject &props = {})
{
    return QJsonObject{{"id", "w1"}, {"type", type}, {"properties", props}, {"binding", binding}};
}

} // namespace

class TestBinding : public QObject
{
    Q_OBJECT

  private slots:
    // A register table reads by address; 响应内序号 keeps counting within the response.
    void modbusAreasReadByRegisterAddress()
    {
        const Message registers{{"slave", 1LL}, {"fc", 3LL}, {"reg0", 0x41CALL}, {"reg1", 0x6666LL},
                                {"start", 100LL}, {"hr100", 0x41CALL}, {"hr101", 0x6666LL}};
        const Message coils{{"slave", 1LL}, {"fc", 1LL}, {"coil0", 1LL}, {"start", 9LL}, {"co9", 1LL}};
        const auto read = [&](const QJsonObject &binding) {
            const auto entry = BindingEntry::fromWidget(widget("Label", binding));
            QVariantList out;
            for (const Message &message : {registers, coils}) {
                const QVariant value = entry->extractFromParsed(message);
                if (value.isValid()) {
                    out.append(value);
                }
            }
            return out;
        };
        const QJsonObject modbus{{"parseMode", "Modbus"}};
        const auto with = [&modbus](const QJsonObject &extra) {
            QJsonObject out = modbus;
            for (auto it = extra.begin(); it != extra.end(); ++it) {
                out.insert(it.key(), it.value());
            }
            return out;
        };
        QCOMPARE(read(with({{"mbRegIndex", 1}})), (QVariantList{0x6666LL}));
        QCOMPARE(read(with({{"mbArea", "保持寄存器"}, {"mbRegIndex", 100}, {"mbDataType", "float32"}})).value(0).toFloat(),
                 25.3f);
        QCOMPARE(read(with({{"mbArea", "保持寄存器"}, {"mbRegIndex", 101}})), (QVariantList{0x6666LL}));
        QVERIFY(read(with({{"mbArea", "保持寄存器"}, {"mbRegIndex", 101}, {"mbDataType", "float32"}})).isEmpty());
        QVERIFY(read(with({{"mbArea", "保持寄存器"}, {"mbRegIndex", 1}})).isEmpty());
        QVERIFY(read(with({{"mbArea", "输入寄存器"}, {"mbRegIndex", 100}})).isEmpty());
        QCOMPARE(read(with({{"mbArea", "线圈"}, {"mbRegIndex", 9}})), (QVariantList{1LL}));
        QVERIFY(read(with({{"mbArea", "保持寄存器"}, {"mbRegIndex", 100}, {"mbSlaveFilter", 2}})).isEmpty());
    }

    void unconfiguredBindingIsSkipped()
    {
        QVERIFY(!BindingEntry::fromWidget(widget("Label", {{"parseMode", "键值匹配"}, {"key", ""}})));
        QVERIFY(!BindingEntry::fromWidget(QJsonObject{{"id", "w"}, {"type", "Label"}}));
        QVERIFY(BindingEntry::fromWidget(widget("Label", {{"parseMode", "HEX字节提取"}})));
    }

    void bindPropertyFollowsWidgetType()
    {
        QCOMPARE(BindingEntry::fromWidget(widget("LEDIndicator", {{"key", "run"}}))->propName, QStringLiteral("on"));
        QCOMPARE(BindingEntry::fromWidget(widget("ToggleSwitch", {{"key", "k"}}))->propName,
                 QStringLiteral("checked"));
        QCOMPARE(BindingEntry::fromWidget(widget("NumericDisplay", {{"key", "k"}}))->propName,
                 QStringLiteral("value"));
    }

    void keyAndJsonPath()
    {
        auto key = BindingEntry::fromWidget(widget("Label", {{"key", "temp"}}));
        Message msg{{"temp", 21.5}};
        QCOMPARE(key->extractFromParsed(msg).toDouble(), 21.5);

        auto path = BindingEntry::fromWidget(widget("Label", {{"parseMode", "JSON路径"}, {"jsonPath", "a.list.-1.v"}}));
        QVariantMap inner{{"v", 7}};
        Message nested{{"a", QVariantMap{{"list", QVariantList{1, inner}}}}};
        QCOMPARE(path->extractFromParsed(nested).toInt(), 7);
        QVERIFY(!path->extractFromParsed(Message{{"a", 1}}).isValid());
    }

    void modbusRegisters()
    {
        auto e = BindingEntry::fromWidget(widget(
            "NumericDisplay", {{"parseMode", "Modbus"}, {"mbRegIndex", 0}, {"mbDataType", "float32"}}));
        // 0x41200000 = 10.0, big-endian word order
        Message msg{{"slave", 1}, {"fc", 3}, {"reg0", 0x4120}, {"reg1", 0x0000}};
        QCOMPARE(e->extractFromParsed(msg).toDouble(), 10.0);

        e->mbRegOrder = QStringLiteral("小端(CD AB)");
        Message swapped{{"slave", 1}, {"fc", 3}, {"reg0", 0x0000}, {"reg1", 0x4120}};
        QCOMPARE(e->extractFromParsed(swapped).toDouble(), 10.0);

        e->mbDataType = QStringLiteral("int16");
        QCOMPARE(e->extractFromParsed(Message{{"fc", 3}, {"reg0", 0xFFFF}}).toLongLong(), -1);

        e->mbSlaveFilter = 2;
        QVERIFY(!e->extractFromParsed(Message{{"slave", 1}, {"fc", 3}, {"reg0", 1}}).isValid());
    }

    void hexBytesWithFraming()
    {
        auto e = BindingEntry::fromWidget(widget("NumericDisplay", {{"parseMode", "HEX字节提取"},
                                                                    {"byteOffset", 1},
                                                                    {"dataType", "uint16"},
                                                                    {"byteOrder", "小端(LE)"},
                                                                    {"frameHeader", "AA"},
                                                                    {"frameTail", "55"}}));
        QList<QVariant> values = e->extractAllFromRaw(QByteArray::fromHex("AA00"));
        QVERIFY(values.isEmpty());
        values = e->extractAllFromRaw(QByteArray::fromHex("341255"));
        QCOMPARE(values.size(), 1);
        QCOMPARE(values.at(0).toLongLong(), 0x1234);
    }

    void formatting()
    {
        auto e = BindingEntry::fromWidget(widget("Label", {{"key", "k"}}, {{"displayFormat", "十六进制(HEX)"}}));
        QCOMPARE(e->formatValue(qlonglong(10)).toString(), QStringLiteral("0x0A"));
        QCOMPARE(e->formatValue(qlonglong(-255)).toString(), QStringLiteral("-0xFF"));
        e->displayFormat = QStringLiteral("二进制(BIN)");
        QCOMPARE(e->formatValue(5.9).toString(), QStringLiteral("0b00000101"));
        e->displayFormat = QStringLiteral("自动");
        e->decimals = QStringLiteral("2");
        QCOMPARE(e->formatValue(3.14159).toString(), QStringLiteral("3.14"));
        QCOMPARE(e->formatValue(QStringLiteral("abc")).toString(), QStringLiteral("abc"));
    }

    void valueConversions()
    {
        QCOMPARE(displayString(25.0), QStringLiteral("25.0"));
        QCOMPARE(displayString(0.1), QStringLiteral("0.1"));
        QCOMPARE(displayString(true), QStringLiteral("True"));
        QCOMPARE(displayString(QVariantMap{{"code", 202}}), QStringLiteral("{\"code\":202}"));
        QCOMPARE(*bindingInteger(QStringLiteral(" 12 ")), 12);
        QVERIFY(!bindingInteger(QStringLiteral("1.5")));
        QVERIFY(!bindingInteger(qlonglong(1) << 40));
        QCOMPARE(*bindingInteger(7.9), 7);
        QVERIFY(!bindingFloat(QStringLiteral("nan")));
        QCOMPARE(*bindingBoolean(QStringLiteral("ON")), true);
        QCOMPARE(*bindingBoolean(QStringLiteral("0")), false);
        QVERIFY(!bindingBoolean(QStringLiteral("maybe")));
        QCOMPARE(*bindingLed(QStringLiteral("RUN"), QStringLiteral("RUN"), QStringLiteral("STOP")), true);
        QCOMPARE(*bindingLed(QStringLiteral("STOP"), QStringLiteral("RUN"), QStringLiteral("STOP")), false);
        QCOMPARE(*bindingLed(1.0, QStringLiteral("1"), QStringLiteral("0")), true);
    }

    // 自动 drops the noise of binary arithmetic (267 * 0.1) but keeps every digit a double holds.
    void autoFormatTrimsFloatNoise()
    {
        QCOMPARE(bindingDecimals(267 * 0.1, QStringLiteral("自动")).toDouble(), 26.7);
        QCOMPARE(displayString(bindingDecimals(267 * 0.1, QStringLiteral("自动"))), QStringLiteral("26.7"));
        QCOMPARE(displayString(bindingDecimals(0.1 + 0.2, QStringLiteral("自动"))), QStringLiteral("0.3"));
        QCOMPARE(displayString(bindingDecimals(60.0, QStringLiteral("自动"))), QStringLiteral("60.0"));
        QCOMPARE(displayString(bindingDecimals(25.299999237060547, QStringLiteral("自动"))),
                 QStringLiteral("25.2999992370605"));
        QCOMPARE(bindingDecimals(qlonglong(267), QStringLiteral("自动")), QVariant(qlonglong(267)));
        QCOMPARE(bindingDecimals(QStringLiteral("26.700000000000003"), QStringLiteral("自动")),
                 QVariant(QStringLiteral("26.700000000000003")));
        QCOMPARE(bindingDecimals(267 * 0.1, QStringLiteral("2")), QVariant(QStringLiteral("26.70")));
        auto scaled = BindingEntry::fromWidget(
            widget("NumericDisplay", {{"key", "t"}, {"expression", "value * 0.1"}}));
        QCOMPARE(displayString(scaled->formatValue(scaled->evaluate(qlonglong(267)))), QStringLiteral("26.7"));
    }

    void expressions()
    {
        QCOMPARE(*Expression::evaluate("value * 0.1", 250), 25.0);
        QCOMPARE(*Expression::evaluate("(value - 32) / 1.8", 212), 100.0);
        QCOMPARE(*Expression::evaluate("-2 ** 2 + value % 3", 7), -3.0);
        QCOMPARE(*Expression::evaluate("1e3 + value", 1), 1001.0);
        QVERIFY(!Expression::evaluate("__import__('os')", 1));
        QVERIFY(!Expression::evaluate("value / 0", 1));
        QVERIFY(!Expression::evaluate("values", 1));

        // Bit tests and comparisons, as the ADC tutorial's lamps use them, with Python's rules.
        QCOMPARE(*Expression::evaluate("value & 1", 35), 1.0);
        QCOMPARE(*Expression::evaluate("(value >> 1) & 1", 35), 1.0);
        QCOMPARE(*Expression::evaluate("(value >> 2) & 1", 35), 0.0);
        QCOMPARE(*Expression::evaluate("value | 8 ^ 1", 35), 43.0); // ^ binds tighter than |
        QCOMPARE(*Expression::evaluate("~value", 35), -36.0);
        QCOMPARE(*Expression::evaluate("1 + 2 << 1", 0), 6.0);      // + binds tighter than <<
        QCOMPARE(*Expression::evaluate("value & 1 == 1", 35), 1.0); // & binds tighter than ==
        QCOMPARE(*Expression::evaluate("value * 3.3 / 4095 < 1", 1000), 1.0);
        QCOMPARE(*Expression::evaluate("value * 3.3 / 4095 < 1", 1987), 0.0);
        QCOMPARE(*Expression::evaluate("0 < value < 40", 35), 1.0);
        QCOMPARE(*Expression::evaluate("0 < value < 30", 35), 0.0);
        QCOMPARE(*Expression::evaluate("value >= 35 and value != 36", 35), 1.0);
        QCOMPARE(*Expression::evaluate("value and 7", 35), 7.0); // and / or give an operand back
        QCOMPARE(*Expression::evaluate("0 or value", 35), 35.0);
        QCOMPARE(*Expression::evaluate("not value", 35), 0.0);
        QVERIFY(!Expression::evaluate("value / 2 & 1", 35)); // 17.5 is not a whole number
        QVERIFY(!Expression::evaluate("value << -1", 35));
        QVERIFY(!Expression::evaluate("value = 1", 35));
        QVERIFY(!Expression::evaluate("value andx 1", 1));
        bool boolean = false;
        QCOMPARE(*Expression::evaluate("value > 1", 35, &boolean), 1.0);
        QVERIFY(boolean);
        QCOMPARE(*Expression::evaluate("(value > 1) + 1", 35, &boolean), 2.0);
        QVERIFY(!boolean);
        // A comparison binds True / False as Python shows it; a lamp with onValue "1" still lights.
        auto lamp = BindingEntry::fromWidget(widget("LEDIndicator", {{"key", "k"}, {"expression", "value < 1"}}));
        QCOMPARE(lamp->evaluate(qlonglong(0)), QVariant(true));
        QCOMPARE(displayString(lamp->evaluate(qlonglong(0))), QStringLiteral("True"));
        QCOMPARE(*bindingLed(lamp->evaluate(qlonglong(0)), QStringLiteral("1"), QStringLiteral("0")), true);
        QCOMPARE(*bindingLed(lamp->evaluate(qlonglong(5)), QStringLiteral("1"), QStringLiteral("0")), false);

        auto e = BindingEntry::fromWidget(widget("Label", {{"key", "k"}, {"expression", "value * 2"}}));
        QCOMPARE(e->evaluate(qlonglong(21)).typeId(), int(QMetaType::LongLong));
        QCOMPARE(e->evaluate(qlonglong(21)).toLongLong(), 42);
        QCOMPARE(e->evaluate(QStringLiteral("text")).toString(), QStringLiteral("text"));
    }
};

QTEST_APPLESS_MAIN(TestBinding)
#include "tst_binding.moc"
