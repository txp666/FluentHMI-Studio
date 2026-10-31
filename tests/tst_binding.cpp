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

    // Results as Python's str() of eval's result shows them; "raw" where eval raises.
    void expressions()
    {
        const auto eval = [](const char *expression, const QVariant &value) {
            const QVariant result = Expression::evaluate(QString::fromUtf8(expression), value);
            return result.isValid() ? displayString(result) : QStringLiteral("raw");
        };
        QCOMPARE(eval("value * 0.1", 250), QStringLiteral("25.0"));
        QCOMPARE(eval("(value - 32) / 1.8", 212), QStringLiteral("100.0"));
        QCOMPARE(eval("-2 ** 2 + value % 3", 7), QStringLiteral("-3"));
        QCOMPARE(eval("1e3 + value", 1), QStringLiteral("1001.0"));
        QCOMPARE(eval("__import__('os')", 1), QStringLiteral("raw"));
        QCOMPARE(eval("value / 0", 1), QStringLiteral("raw"));
        QCOMPARE(eval("values", 1), QStringLiteral("raw"));

        // Bit tests and comparisons, as the ADC tutorial's lamps use them, with Python's rules.
        QCOMPARE(eval("value & 1", 35), QStringLiteral("1"));
        QCOMPARE(eval("(value >> 1) & 1", 35), QStringLiteral("1"));
        QCOMPARE(eval("(value >> 2) & 1", 35), QStringLiteral("0"));
        QCOMPARE(eval("value | 8 ^ 1", 35), QStringLiteral("43")); // ^ binds tighter than |
        QCOMPARE(eval("~value", 35), QStringLiteral("-36"));
        QCOMPARE(eval("1 + 2 << 1", 0), QStringLiteral("6"));         // + binds tighter than <<
        QCOMPARE(eval("value & 1 == 1", 35), QStringLiteral("True")); // & binds tighter than ==
        QCOMPARE(eval("value * 3.3 / 4095 < 1", 1000), QStringLiteral("True"));
        QCOMPARE(eval("value * 3.3 / 4095 < 1", 1987), QStringLiteral("False"));
        QCOMPARE(eval("0 < value < 40", 35), QStringLiteral("True"));
        QCOMPARE(eval("0 < value < 30", 35), QStringLiteral("False"));
        QCOMPARE(eval("value >= 35 and value != 36", 35), QStringLiteral("True"));
        QCOMPARE(eval("value and 7", 35), QStringLiteral("7")); // and / or give an operand back
        QCOMPARE(eval("0 or value", 35), QStringLiteral("35"));
        QCOMPARE(eval("0.0 or value > 1", 35), QStringLiteral("True"));
        QCOMPARE(eval("not value", 35), QStringLiteral("False"));
        QCOMPARE(eval("value / 2 & 1", 35), QStringLiteral("raw")); // 17.5 is a float
        QCOMPARE(eval("value << -1", 35), QStringLiteral("raw"));
        QCOMPARE(eval("value = 1", 35), QStringLiteral("raw"));
        QCOMPARE(eval("value andx 1", 1), QStringLiteral("raw"));
        QCOMPARE(eval("value > 1", 35), QStringLiteral("True"));
        QCOMPARE(eval("(value > 1) + 1", 35), QStringLiteral("2"));

        // Each value has Python's kind: int, float or bool, and each operator gives Python's.
        QCOMPARE(eval("value // 2", 7), QStringLiteral("3"));
        QCOMPARE(eval("value // 2", 7.0), QStringLiteral("3.0"));
        QCOMPARE(eval("-value // 2", 7), QStringLiteral("-4")); // rounded down
        QCOMPARE(eval("value % -3", 7), QStringLiteral("-2"));  // the divisor's sign
        QCOMPARE(eval("value / 2", 6), QStringLiteral("3.0"));
        QCOMPARE(eval("value ** 2", 3), QStringLiteral("9"));
        QCOMPARE(eval("value ** -1", 4), QStringLiteral("0.25"));
        QCOMPARE(eval("value + 1", 2.0), QStringLiteral("3.0"));
        QCOMPARE(eval("7.0 // 0.1", 0), QStringLiteral("69.0")); // CPython's float floor division
        QCOMPARE(eval("7.0 % 0.1", 0), QStringLiteral("0.09999999999999962"));
        QCOMPARE(eval("value & 0x0F", 35), QStringLiteral("3"));
        QCOMPARE(eval("value & 0b11", 35), QStringLiteral("3"));
        QCOMPARE(eval("0o17", 0), QStringLiteral("15"));
        QCOMPARE(eval("0XfF + 0B1 + 0O1 + 1_000", 0), QStringLiteral("1257"));
        QCOMPARE(eval("07", 0), QStringLiteral("raw")); // no leading zeros in Python
        QCOMPARE(eval("0b12", 0), QStringLiteral("raw"));
        QCOMPARE(eval("value & 1", 3.0), QStringLiteral("raw")); // Python's TypeError
        QCOMPARE(eval("~value", 4.0), QStringLiteral("raw"));
        QCOMPARE(eval("-True", 0), QStringLiteral("-1"));
        QCOMPARE(eval("(value > 1) & (value < 5)", 3), QStringLiteral("True"));
        QCOMPARE(eval("(value > 1) | 0", 3), QStringLiteral("1"));
        QCOMPARE(eval("True + value", 1), QStringLiteral("2"));
        QCOMPARE(eval("value", true), QStringLiteral("True"));
        QCOMPARE(eval("value * 2", true), QStringLiteral("2"));
        QCOMPARE(eval("value << 62", 1), QStringLiteral("4611686018427387904"));
        QCOMPARE(eval("value << 63", 1), QStringLiteral("raw")); // an int beyond 64 bits
        QCOMPARE(eval("value >> 100", -5), QStringLiteral("-1"));
        QCOMPARE(eval("9007199254740993 == 9007199254740992.0", 0), QStringLiteral("False")); // exact
        // An operand `and`, `or` or a comparison chain never reaches is not evaluated.
        QCOMPARE(eval("value and 1 / 0", 0), QStringLiteral("0"));
        QCOMPARE(eval("value or 1 / 0", 2), QStringLiteral("2"));
        QCOMPARE(eval("value > 5 > 1 / 0", 2), QStringLiteral("False"));
        QCOMPARE(eval("value > 1 > 1 / 0", 2), QStringLiteral("raw"));

        // Through a binding: the raw value's kind is `value`'s, and an error shows the raw value.
        const auto bound = [](const char *expression, const QVariant &raw) {
            const auto entry = BindingEntry::fromWidget(
                widget("Label", {{"key", "k"}, {"expression", QString::fromUtf8(expression)}}));
            return entry->evaluate(raw);
        };
        QCOMPARE(bound("value & 0x0F", qlonglong(35)), QVariant(qlonglong(3)));
        QCOMPARE(bound("value & 0b11", qlonglong(35)), QVariant(qlonglong(3)));
        QCOMPARE(bound("0o17", qlonglong(35)), QVariant(qlonglong(15)));
        QCOMPARE(bound("value & 1", 3.0).typeId(), int(QMetaType::Double));
        QCOMPARE(bound("value & 1", 3.0), QVariant(3.0));
        QCOMPARE(bound("~value", 4.0), QVariant(4.0));
        QCOMPARE(displayString(bound("(value>1) & (value<5)", qlonglong(3))), QStringLiteral("True"));
        QCOMPARE(bound("True + value", qlonglong(1)), QVariant(qlonglong(2)));
        QCOMPARE(displayString(bound("value // 2", qlonglong(7))), QStringLiteral("3"));
        QCOMPARE(displayString(bound("value / 2", qlonglong(7))), QStringLiteral("3.5"));
        QCOMPARE(displayString(bound("value * 0.1", qlonglong(250))), QStringLiteral("25.0"));
        QCOMPARE(displayString(bound("value + 1", QStringLiteral("2"))), QStringLiteral("3.0")); // text is a float
        QCOMPARE(displayString(bound("not value", false)), QStringLiteral("True"));

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
