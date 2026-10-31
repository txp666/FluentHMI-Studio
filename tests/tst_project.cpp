#include "core/project.h"
#include "core/widget_meta.h"

#include <QtCore/QDirIterator>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QTemporaryDir>
#include <QtTest/QtTest>

using namespace fhmi;

class TestProject : public QObject
{
    Q_OBJECT

  private slots:
    void registryHasEveryDesignerWidget()
    {
        QCOMPARE(WidgetRegistry::instance().all().size(), 28); // as PyQt UI Designer, without the Modbus card
        const WidgetMeta *button = WidgetRegistry::instance().find("PushButton");
        QVERIFY(button);
        QCOMPARE(button->defaultProperties().value("text").toString(), QStringLiteral("按钮"));
        QVERIFY(WidgetRegistry::instance().find("TabWidget")->container);
    }

    void examplesLoadAndRoundTrip_data()
    {
        QTest::addColumn<QString>("path");
        QDirIterator it(QStringLiteral(FHMI_EXAMPLES_DIR), {QStringLiteral("*.uidp")}, QDir::Files,
                        QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString path = it.next();
            QTest::newRow(qPrintable(QFileInfo(path).fileName())) << path;
        }
    }

    void examplesLoadAndRoundTrip()
    {
        QFETCH(QString, path);
        Project project;
        QString error;
        QVERIFY2(Project::load(path, &project, &error), qPrintable(error));
        QVERIFY(!project.widgets.isEmpty());

        QTemporaryDir dir;
        const QString copy = dir.filePath("copy.uidp");
        QVERIFY2(project.save(copy, &error), qPrintable(error));
        Project reloaded;
        QVERIFY2(Project::load(copy, &reloaded, &error), qPrintable(error));
        QCOMPARE(reloaded.toJson(), project.toJson());
    }

    void rejectsInvalidDocuments_data()
    {
        QTest::addColumn<QByteArray>("json");
        QTest::newRow("version") << QByteArray(R"({"version":"2.0"})");
        QTest::newRow("canvas") << QByteArray(R"({"canvas":{"width":0}})");
        QTest::newRow("unknown type") << QByteArray(R"({"widgets":[{"type":"Nope"}]})");
        QTest::newRow("duplicate id") << QByteArray(R"({"widgets":[{"type":"Label","id":"a"},{"type":"Label","id":"a"}]})");
        QTest::newRow("zero size") << QByteArray(R"({"widgets":[{"type":"Label","geometry":{"w":0}}]})");
        QTest::newRow("parser") << QByteArray(R"({"serial":{"parser":"xml"}})");
        QTest::newRow("tab index") << QByteArray(R"({"widgets":[{"type":"Label","tabIndex":-1}]})");
        QTest::newRow("nested") << QByteArray(R"({"widgets":[{"type":"TabWidget","children":[{"type":"Bad"}]}]})");
        QTest::newRow("period")
            << QByteArray(R"({"widgets":[{"type":"Label","binding":{"parseMode":"Modbus","mbPeriod":60001}}]})");
    }

    void rejectsInvalidDocuments()
    {
        QFETCH(QByteArray, json);
        QString error;
        Project project;
        QVERIFY(!Project::fromJson(QJsonDocument::fromJson(json).object(), &project, &error));
        QVERIFY(!error.isEmpty());
    }

    // A link card's own frame format is checked as the card would build its parser.
    void checksCardFrameFormats_data()
    {
        QTest::addColumn<QByteArray>("json");
        QTest::addColumn<QString>("error");
        QTest::newRow("not an object")
            << QByteArray(
                   R"json({"widgets":[{"type":"SerialConfigCard","id":"uart","properties":{"parserConfig":[]}}]})json")
            << QStringLiteral("卡片 uart 的 parserConfig 必须是对象");
        QTest::newRow("own mode")
            << QByteArray(R"json({"widgets":[{"type":"TcpConfigCard","id":"tcp","properties":{"defaultParser":"hex",
                "parserConfig":{"checksum":"CRC32"}}}]})json")
            << QStringLiteral("卡片 tcp: HEX 校验设置无效");
        QTest::newRow("project mode")
            << QByteArray(R"json({"serial":{"parser":"hex","parserConfig":{}},"widgets":[{"type":"MqttConfigCard",
                "id":"mqtt","properties":{"parserConfig":{"footer":""}}}]})json")
            << QStringLiteral("卡片 mqtt: HEX 帧需要帧尾、长度字段、固定帧长或空闲超时之一来分帧");
        QTest::newRow("line")
            << QByteArray(R"json({"widgets":[{"type":"VBoxLayout","children":[{"type":"SerialConfigCard","id":"s",
                "properties":{"defaultParser":"line","parserConfig":{"separator":""}}}]}]})json")
            << QStringLiteral("卡片 s: 行分隔符不能为空");
        QTest::newRow("line encoding")
            << QByteArray(R"json({"widgets":[{"type":"TcpConfigCard","id":"t","properties":{"defaultParser":"line",
                "parserConfig":{"encoding":"latin-1"}}}]})json")
            << QStringLiteral("卡片 t: 编码只能是 utf-8、gbk 或 ascii");
        QTest::newRow("project line keys")
            << QByteArray(R"json({"serial":{"parser":"line","parserConfig":{"kvSeparator":"=>"}}})json")
            << QStringLiteral("键值分隔符只能是 = 或 :");
        QTest::newRow("project terminator")
            << QByteArray(R"json({"serial":{"parser":"line","parserConfig":{"terminator":""}}})json")
            << QStringLiteral("行结束符不能为空");
        QTest::newRow("modbus ascii")
            << QByteArray(R"json({"serial":{"parser":"modbus_ascii"},"widgets":[{"type":"SerialConfigCard","id":"a",
                "properties":{"defaultParser":"modbus_ascii","parserConfig":{}}},
                {"type":"MqttConfigCard","id":"m","properties":{"defaultParser":"line",
                "parserConfig":{"terminator":";","kvSeparator":":","encoding":"GBK"}}}]})json")
            << QString();
        QTest::newRow("valid")
            << QByteArray(R"json({"widgets":[{"type":"SerialConfigCard","id":"a","properties":{"defaultParser":"hex",
                "parserConfig":{"header":"7E","footer":"","idleTimeout":20}}},
                {"type":"TcpConfigCard","id":"b","properties":{"parserConfig":{}}}]})json")
            << QString();
    }

    void checksCardFrameFormats()
    {
        QFETCH(QByteArray, json);
        QFETCH(QString, error);
        QString message;
        Project project;
        QCOMPARE(Project::fromJson(QJsonDocument::fromJson(json).object(), &project, &message), error.isEmpty());
        QCOMPARE(message, error);
    }

    // There is no Modbus card any more: a project with one is refused as any unknown type is, with no
    // upgrade, wherever it sits. A Modbus binding or chart curve names a register table; 响应内序号 is an
    // error. Other parse modes ignore mbArea.
    void checksModbusKeys_data()
    {
        QTest::addColumn<QByteArray>("json");
        QTest::addColumn<QString>("error");
        QTest::newRow("Modbus card")
            << QByteArray(R"json({"widgets":[{"type":"ModbusConfigCard","id":"bus","properties":{"defaultPoll":500,
                "timeout":1000}}]})json")
            << QStringLiteral("未知控件类型: ModbusConfigCard");
        QTest::newRow("Modbus card in a group")
            << QByteArray(R"json({"widgets":[{"type":"GroupBox","id":"box","children":[
                {"type":"ModbusConfigCard","id":"bus"}]}]})json")
            << QStringLiteral("未知控件类型: ModbusConfigCard");
        QTest::newRow("in response")
            << QByteArray(R"json({"widgets":[{"type":"NumericDisplay","id":"pv",
                "binding":{"parseMode":"Modbus","mbArea":"响应内序号","mbRegIndex":0}}]})json")
            << QStringLiteral("控件 pv 的 mbArea 只能是 保持寄存器、输入寄存器、线圈、离散输入");
        QTest::newRow("not Modbus")
            << QByteArray(R"json({"widgets":[{"type":"NumericDisplay","id":"pv",
                "binding":{"parseMode":"键值匹配","key":"t","mbArea":"响应内序号"}}]})json")
            << QString();
        QTest::newRow("curve")
            << QByteArray(R"json({"widgets":[{"type":"RealtimeChart","id":"trend","properties":{"curveCount":2,
                "_curve_0_parseMode":"Modbus","_curve_0_mbArea":"线圈",
                "_curve_1_parseMode":"Modbus","_curve_1_mbArea":"响应内序号"}}]})json")
            << QStringLiteral("控件 trend 的 _curve_1_mbArea 只能是 保持寄存器、输入寄存器、线圈、离散输入");
        QTest::newRow("period")
            << QByteArray(R"json({"widgets":[{"type":"NumericDisplay","id":"pv",
                "binding":{"parseMode":"Modbus","mbArea":"保持寄存器","mbPeriod":250}}]})json")
            << QString();
    }

    void checksModbusKeys()
    {
        QFETCH(QByteArray, json);
        QFETCH(QString, error);
        const QJsonObject document = QJsonDocument::fromJson(json).object();
        QString message;
        Project project;
        QCOMPARE(Project::fromJson(document, &project, &message), error.isEmpty());
        QCOMPARE(message, error);
        if (error.isEmpty()) { // loaded as written
            QCOMPARE(project.widgets, document.value("widgets").toArray());
        }
        QVERIFY(!WidgetRegistry::instance().find("ModbusConfigCard"));
    }

    // Each link card carries the Modbus poller's settings right after its parser mode and before its frame
    // format, as PyQt UI Designer does: key, label, default and range.
    void linkCardsCarryPollingSettings_data()
    {
        QTest::addColumn<QString>("type");
        for (const char *type : {"SerialConfigCard", "TcpConfigCard", "MqttConfigCard"}) {
            QTest::newRow(type) << QString::fromLatin1(type);
        }
    }

    void linkCardsCarryPollingSettings()
    {
        QFETCH(QString, type);
        const WidgetMeta *meta = WidgetRegistry::instance().find(type);
        QVERIFY(meta);
        QStringList names;
        for (const PropDef &def : meta->properties) {
            names.append(def.name);
        }
        const qsizetype parser = names.indexOf(QStringLiteral("defaultParser"));
        QVERIFY(parser >= 0);
        QCOMPARE(names.mid(parser, 5),
                 (QStringList{"defaultParser", "modbusPeriod", "modbusTimeout", "modbusSlave", "parserConfig"}));
        const struct
        {
            const char *name;
            const char *label;
            int value;
            double lo;
            double hi;
        } expected[] = {{"modbusPeriod", "默认轮询(ms)", 500, 10, 60000},
                        {"modbusTimeout", "响应超时(ms)", 1000, 10, 60000},
                        {"modbusSlave", "默认从站", 1, 1, 247}};
        for (const auto &want : expected) {
            const PropDef *def = meta->prop(QLatin1String(want.name));
            QVERIFY(def);
            QCOMPARE(def->label, QString::fromUtf8(want.label));
            QVERIFY(def->type == PropType::Int);
            QCOMPARE(def->defaultValue, QJsonValue(want.value));
            QCOMPARE(def->minimum.value_or(-1), want.lo);
            QCOMPARE(def->maximum.value_or(-1), want.hi);
        }
    }

    void defaultsFillMissingFields()
    {
        Project project;
        QString error;
        QVERIFY(Project::fromJson(QJsonObject{{"serial", QJsonObject{{"baudRate", 9600}}}}, &project, &error));
        QCOMPARE(project.serial.value("baudRate").toInt(), 9600);
        QCOMPARE(project.parserType(), QStringLiteral("line"));
        QCOMPARE(project.canvasWidth, 1200);
    }

    void rangeNormalization()
    {
        QJsonObject props{{"minimum", 10}, {"maximum", 5}, {"value", 50}};
        QJsonObject out = normalizedRangeProperties("Slider", props, "maximum");
        QCOMPARE(out.value("minimum").toInt(), 5);
        QCOMPARE(out.value("value").toInt(), 5);
        out = normalizedRangeProperties("Slider", props, "minimum");
        QCOMPARE(out.value("maximum").toInt(), 10);
        QCOMPARE(normalizedRangeProperties("Label", props), props);
    }
};

QTEST_APPLESS_MAIN(TestProject)
#include "tst_project.moc"
