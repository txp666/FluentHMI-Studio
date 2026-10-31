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
        QCOMPARE(WidgetRegistry::instance().all().size(), 29);
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
        QTest::newRow("poll mode")
            << QByteArray(R"({"widgets":[{"type":"ModbusConfigCard","properties":{"pollMode":"auto"}}]})");
        QTest::newRow("auto start")
            << QByteArray(R"({"widgets":[{"type":"ModbusConfigCard","properties":{"autoStart":1}}]})");
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

    // A Modbus card saved before pollMode existed keeps polling its table; one without a table, and every
    // card that names its mode, load as they are. mbPeriod is a whole number of ms from 0 to 60000.
    void modbusCardsKeepTheirPollMode()
    {
        const QByteArray json = R"json({"widgets":[
            {"type":"ModbusConfigCard","id":"old","properties":{"defaultPoll":500,"pollList":"1,3,0,2"}},
            {"type":"GroupBox","id":"box","children":[
                {"type":"ModbusConfigCard","id":"nested","properties":{"pollList":"1,1,0,4"}}]},
            {"type":"ModbusConfigCard","id":"plain","properties":{"title":"Modbus"}},
            {"type":"ModbusConfigCard","id":"chosen","properties":{"pollMode":"自动","autoStart":false,
                "pollList":"1,3,0,2"}},
            {"type":"NumericDisplay","id":"pv",
                "binding":{"parseMode":"Modbus","mbArea":"保持寄存器","mbPeriod":250}}]})json";
        Project project;
        QString error;
        QVERIFY2(Project::fromJson(QJsonDocument::fromJson(json).object(), &project, &error), qPrintable(error));
        QVERIFY(error.isEmpty());
        const auto props = [&project](int index) {
            return project.widgets.at(index).toObject().value("properties").toObject();
        };
        QCOMPARE(props(0).value("pollMode").toString(), QStringLiteral("轮询表"));
        QCOMPARE(project.widgets.at(1).toObject().value("children").toArray().at(0).toObject().value("properties")
                     .toObject().value("pollMode").toString(),
                 QStringLiteral("轮询表"));
        QVERIFY(!props(2).contains("pollMode")); // the default, 自动
        QCOMPARE(props(3).value("pollMode").toString(), QStringLiteral("自动"));
        QVERIFY(Project::validate(project.toJson(), &error));
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
