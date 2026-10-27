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

    void expressions()
    {
        QCOMPARE(*Expression::evaluate("value * 0.1", 250), 25.0);
        QCOMPARE(*Expression::evaluate("(value - 32) / 1.8", 212), 100.0);
        QCOMPARE(*Expression::evaluate("-2 ** 2 + value % 3", 7), -3.0);
        QCOMPARE(*Expression::evaluate("1e3 + value", 1), 1001.0);
        QVERIFY(!Expression::evaluate("__import__('os')", 1));
        QVERIFY(!Expression::evaluate("value / 0", 1));
        QVERIFY(!Expression::evaluate("values", 1));

        auto e = BindingEntry::fromWidget(widget("Label", {{"key", "k"}, {"expression", "value * 2"}}));
        QCOMPARE(e->evaluate(qlonglong(21)).typeId(), int(QMetaType::LongLong));
        QCOMPARE(e->evaluate(qlonglong(21)).toLongLong(), 42);
        QCOMPARE(e->evaluate(QStringLiteral("text")).toString(), QStringLiteral("text"));
    }
};

QTEST_APPLESS_MAIN(TestBinding)
#include "tst_binding.moc"
