#include "core/binding.h"
#include "core/project.h"
#include "core/command_encoder.h"
#include "core/project_logic.h"
#include "core/expression.h"
#include "core/frame_templates.h"
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

    void dataPointRenameFollowsReferencesWithoutChangingDeviceFields()
    {
        const QString renamed = QStringLiteral("新点'\"\\名");
        Project project;
        project.logic = {{"points", QJsonArray{QJsonObject{{"name", "L1"}, {"binding", QJsonObject{{"key", "L1"}}}},
                                               QJsonObject{{"name", "L10"}, {"binding", QJsonObject{{"key", "L10"}}}},
                                               QJsonObject{{"name", "total"}, {"expression", "T [ 'L1' ] + T[\"L1\"] + T['L10']"}}}},
                         {"alarms", QJsonArray{QJsonObject{{"id", "L1"}, {"text", "L1"}, {"condition", "T['L1'] > 0"}}}}};
        const QJsonObject binding{{"parseMode", "工程数据点"}, {"key", "L1"}};
        const QJsonObject raw{{"type", "NumericDisplay"}, {"binding", QJsonObject{{"key", "L1"}}}};
        project.widgets = {QJsonObject{{"type", "GroupBox"}, {"children", QJsonArray{
            QJsonObject{{"type", "NumericDisplay"}, {"binding", binding}, {"properties", QJsonObject{{"title", "L1"}}}},
            QJsonObject{{"type", "RealtimeChart"}, {"properties", QJsonObject{
                {"_curve_0_parseMode", "工程数据点"}, {"_curve_0_key", "L1"},
                {"_curve_1_key", "L1"}, {"_curve_7_parseMode", "工程数据点"}, {"_curve_7_key", "L1"}}}}, raw}}}};
        renameDataPoint(&project, "L1", renamed);
        const auto points = project.logic.value("points").toArray();
        QCOMPARE(points[0].toObject().value("name").toString(), renamed);
        QCOMPARE(points[0].toObject().value("binding").toObject().value("key").toString(), QStringLiteral("L1"));
        const auto formula = points[2].toObject().value("expression").toString();
        QCOMPARE(Expression::references(formula), QStringList({renamed, "L10"}));
        QCOMPARE(Expression::evaluate(formula, {}, [&](const QString &name) { return name == renamed ? 2 : 3; }).toInt(), 7);
        const auto alarm = project.logic.value("alarms").toArray()[0].toObject();
        QCOMPARE(Expression::references(alarm.value("condition").toString()), QStringList({renamed}));
        QCOMPARE(alarm.value("id").toString(), QStringLiteral("L1"));
        QCOMPARE(alarm.value("text").toString(), QStringLiteral("L1"));
        const auto children = project.widgets[0].toObject().value("children").toArray();
        QCOMPARE(children[0].toObject().value("binding").toObject().value("key").toString(), renamed);
        QCOMPARE(children[0].toObject().value("properties").toObject().value("title").toString(), QStringLiteral("L1"));
        const auto props = children[1].toObject().value("properties").toObject();
        QCOMPARE(props.value("_curve_0_key").toString(), renamed);
        QCOMPARE(props.value("_curve_7_key").toString(), renamed); // retain references in temporarily hidden curves
        QCOMPARE(props.value("_curve_1_key").toString(), QStringLiteral("L1"));
        QCOMPARE(children[2].toObject(), raw);
    }

    void projectDataRoundTripsAndRejectsBrokenDependencies()
    {
        Project project;
        QCOMPARE(project.toJson().value("version").toString(), QStringLiteral("1.0"));
        project.widgets = {QJsonObject{{"id", "A"}, {"type", "TcpConfigCard"}},
                           QJsonObject{{"type", "NumericDisplay"},
                                       {"binding", QJsonObject{{"parseMode", "工程数据点"}, {"key", "total"}}}}};
        project.logic = {
            {"points", QJsonArray{QJsonObject{{"name", "input"},
                                              {"binding", QJsonObject{{"key", "value"}, {"sourceCardId", "A"}}}},
                                  QJsonObject{{"name", "total"}, {"expression", "T[\"input\"] * 2"}}}}};
        auto json = project.toJson();
        QCOMPARE(json.value("version").toString(), QStringLiteral("1.3"));
        Project loaded;
        QString error;
        QVERIFY2(Project::fromJson(json, &loaded, &error), qPrintable(error));
        QCOMPARE(loaded.toJson(), json);
        json.insert("version", "1.2");
        QVERIFY(!Project::validate(json, &error));
        auto points = project.logic.value("points").toArray();
        points[0] = QJsonObject{{"name", "input"}, {"expression", "T[\"total\"]"}};
        project.logic.insert("points", points);
        QVERIFY(!Project::validate(project.toJson(), &error));
        QVERIFY(error.contains(QStringLiteral("循环")));
        points[0] = QJsonObject{{"name", "input"}, {"expression", "T[\"missing\"]"}};
        project.logic.insert("points", points);
        QVERIFY(!Project::validate(project.toJson(), &error));
    }

    void enumTablesRoundTripAndRequireCapableRuntime()
    {
        Project project;
        const QJsonObject binary{{"id", "lamp"}, {"type", "LEDIndicator"},
                                 {"properties", QJsonObject{{"onValue", "RUN"}, {"offValue", "STOP"}}}};
        project.widgets = {binary};
        QCOMPARE(project.toJson().value("version").toString(), QStringLiteral("1.0"));
        project.widgets.append(QJsonObject{{"type", "NumericDisplay"},
            {"properties", QJsonObject{{"displayFormat", "枚举"}, {"textList", "1=Running|#008040"}}}});
        QCOMPARE(project.toJson().value("version").toString(), QStringLiteral("1.3"));

        project.enumTables = {{"Pump", "1=Running|#008040\n2=Warning|#e0a000"}};
        QJsonObject props{{"stateMode", "多状态"}, {"enumTable", "Pump"}, {"ledEffects", QJsonObject{{"2.0", "slow"}}}};
        project.widgets[0] = QJsonObject{{"type", "LEDIndicator"}, {"properties", props}};
        auto json = project.toJson();
        QCOMPARE(json.value("version").toString(), QStringLiteral("1.4"));
        QString error;
        Project loaded;
        QVERIFY2(Project::fromJson(json, &loaded, &error), qPrintable(error));
        QCOMPARE(loaded.toJson(), json);
        json.insert("version", "1.3");
        QVERIFY(!Project::validate(json, &error));
        json = project.toJson();
        json.remove("enumTables");
        QVERIFY(!Project::validate(json, &error));
        json.insert("enumTables", QJsonObject{{"Pump", "1=Running\n1.0=Warning"}});
        QVERIFY(!Project::validate(json, &error));
        props.insert("ledEffects", QJsonObject{{"2", "slow"}, {"2.0", "fast"}});
        project.widgets[0] = QJsonObject{{"type", "LEDIndicator"}, {"properties", props}};
        QVERIFY(!Project::validate(project.toJson(), &error));
    }

    void explicitRoutesRoundTripAndRequireCapableRuntime()
    {
        Project project;
        project.widgets={QJsonObject{{"id","link"},{"type","CanConfigCard"}},QJsonObject{{"id","switch"},{"type","ToggleSwitch"},{"properties",QJsonObject{{"commandMode","CAN"},{"commandTargetId","link"}}}}};
        const auto json=project.toJson(); QCOMPARE(json.value("version").toString(),QStringLiteral("1.2"));
        QString error; Project loaded; QVERIFY2(Project::fromJson(json,&loaded,&error),qPrintable(error)); QCOMPARE(loaded.toJson(),json);
        auto older=json; older.insert("version","1.1"); QVERIFY(!Project::validate(older,&error));
        auto missing=json; auto widgets=missing.value("widgets").toArray(); widgets.removeAt(0); missing.insert("widgets",widgets);
        QVERIFY(!Project::validate(missing,&error)); QVERIFY(Project::validate(missing,&error,true)); // clipboard reference

        project.widgets = {
            QJsonObject{{"id", "*"}, {"type", "CanConfigCard"}},
            QJsonObject{{"id", "switch"}, {"type", "ToggleSwitch"},
                        {"properties", QJsonObject{{"commandMode", "CAN"}, {"commandTargetId", "*"}}}},
        };
        QCOMPARE(project.toJson().value("version").toString(), QStringLiteral("1.2"));
        QVERIFY2(Project::fromJson(project.toJson(), &loaded, &error), qPrintable(error));
        QCOMPARE(loaded.widgets, project.widgets);

        project.widgets = {QJsonObject{{"id", "publish"}, {"type", "PushButton"},
                                        {"properties", QJsonObject{{"commandMode", "文本"}, {"commandEndpoint", "*"}}}}};
        QCOMPARE(project.toJson().value("version").toString(), QStringLiteral("1.2"));

        project.widgets = {QJsonObject{{"id", "can"}, {"type", "CanConfigCard"}},
            QJsonObject{{"id", "send"}, {"type", "DataSendCard"}, {"properties", QJsonObject{{"commandTargetId", "can"}}}}};
        auto oldCanSendCard = project.toJson();
        oldCanSendCard.insert("version", "1.3");
        QVERIFY2(Project::fromJson(oldCanSendCard, &loaded, &error), qPrintable(error));
        QVERIFY(!Project::validate(loaded.toJson(), &error)); // Choose the CAN format before saving the new configuration.
    }

    void registryHasEveryDesignerWidget()
    {

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

    void s7VersionAndReferences()
    {
        Project project;
        QCOMPARE(project.toJson().value("version").toString(), QStringLiteral("1.0"));
        const QJsonObject card{{"type", "S7ConfigCard"}, {"id", "plc"}};
        const QJsonObject display{{"type", "NumericDisplay"}, {"id", "value"},
                                 {"binding", QJsonObject{{"parseMode", "S7"}, {"sourceCardId", "plc"},
                                                         {"s7", QJsonObject{{"address", "DB1.DBD4"}, {"dataType", "REAL"}}}}}};
        project.widgets = {card, display};
        auto document = project.toJson();
        QCOMPARE(document.value("version").toString(), QStringLiteral("1.1"));
        QString error;
        QVERIFY2(Project::validate(document, &error), qPrintable(error));
        document.insert("version", "1.0");
        QVERIFY(!Project::validate(document, &error));
        document.insert("version", "1.1");
        document.insert("widgets", QJsonArray{display});
        QVERIFY(!Project::validate(document, &error));
        // A selection can retain a connection outside that selection; a full project cannot.
        QVERIFY2(Project::validate(document, &error, true), qPrintable(error));
        project.widgets = {QJsonObject{{"type", "PushButton"}, {"id", "button"},
                                      {"properties", QJsonObject{{"commandMode", "文本"},
                                                                  {"s7Command", QJsonObject{{"address", "DB1.DBD4"}, {"dataType", "REAL"}}}}}}};
        QCOMPARE(project.toJson().value("version").toString(), QStringLiteral("1.0"));

        // v0.7 kept the last S7 card ID even after a non-S7 mode was selected.
        const QJsonObject legacy{{"version", "1.1"}, {
            "widgets", QJsonArray{card, display, QJsonObject{
                {"type", "PushButton"}, {"id", "text"},
                {"binding", QJsonObject{{"parseMode", "键值匹配"}, {"sourceCardId", "plc"}, {"key", "speed"}}},
                {"properties", QJsonObject{{"commandMode", "文本"}, {"commandTargetId", "plc"}}}}}}};
        Project restored;
        QVERIFY2(Project::fromJson(legacy, &restored, &error), qPrintable(error));
        QCOMPARE(restored.widgets.at(1).toObject(), display);
        const auto migrated = restored.widgets.at(2).toObject();
        QVERIFY(!migrated.value("properties").toObject().contains("commandTargetId"));
        QVERIFY(!migrated.value("binding").toObject().contains("sourceCardId"));
        QCOMPARE(restored.toJson().value("version").toString(), QStringLiteral("1.1"));

        QJsonObject draftSlider{{"type", "Slider"}, {"id", "slider"}};
        QJsonObject sliderProps{{"sendOnChange", false}, {"commandMode", "S7"}, {"commandTargetId", "deleted"}};
        draftSlider.insert("properties", sliderProps);
        const QJsonObject chart{{"type", "RealtimeChart"}, {"id", "chart"},
            {"properties", QJsonObject{{"curveCount", 1}, {"_curve_1_parseMode", "S7"},
                {"_curve_1_sourceCardId", "deleted"}}}};
        project.widgets = {draftSlider, chart};
        QCOMPARE(project.toJson().value("version").toString(), QStringLiteral("1.0"));
        QVERIFY2(Project::fromJson(project.toJson(), &restored, &error), qPrintable(error));
        QCOMPARE(restored.widgets, project.widgets);

        sliderProps.insert("sendOnChange", true);
        draftSlider.insert("properties", sliderProps);
        project.widgets = {draftSlider, chart};
        QVERIFY(!Project::validate(project.toJson(), &error));
    }

    void canOutputConfigurationNeedsCapableRuntime()
    {
        QJsonObject can{{"type", "CanConfigCard"}, {"id", "can"}};
        for (const auto &props : {
                 QJsonObject{{"canTxInitialState", "HMI=IO"}},
                 QJsonObject{{"defaultAdapter", "ZLG"}, {"zlgCustomTiming", "00,1C"}},
                 QJsonObject{{"defaultAdapter", "ZLG"}, {"zlgSamplePoint", "87.5%"}},
             }) {
            can.insert("properties", props);
            Project project;
            project.widgets = {can};
            auto saved = project.toJson();
            QString error;
            QCOMPARE(saved.value("version").toString(), QStringLiteral("1.3"));
            QVERIFY2(Project::validate(saved, &error), qPrintable(error));
            saved.insert("version", "1.0");
            QVERIFY(!Project::validate(saved, &error));
        }

        // The generic fields were already used by PCAN. A legacy file retains them.
        const QJsonObject oldTiming{{"defaultAdapter", "ZLG"}, {"defaultSamplePoint", "87.5%"},
                                    {"customTiming", "1,13,2,1"}};
        can.insert("properties", oldTiming);
        Project loaded;
        QString error;
        QVERIFY2(Project::fromJson(QJsonObject{{"version", "1.0"}, {"widgets", QJsonArray{can}}}, &loaded, &error),
                 qPrintable(error));
        QCOMPARE(loaded.widgets.first().toObject().value("properties").toObject(), oldTiming);
        QCOMPARE(loaded.toJson().value("version").toString(), QStringLiteral("1.0"));
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
        QTest::newRow("inherited format ignores dormant override")
            << QByteArray(R"json({"serial":{"parser":"hex","parserConfig":{}},"widgets":[{"type":"MqttConfigCard",
                "id":"mqtt","properties":{"parserConfig":{"footer":""}}}]})json")
            << QString();
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
        QTest::newRow("valid binding bit")
            << QByteArray(R"json({"version":"1.3","widgets":[{"type":"LEDIndicator","id":"led",
                "binding":{"parseMode":"Modbus","mbArea":"保持寄存器","mbRegIndex":10,"mbBit":7}}]})json")
            << QString();
        QTest::newRow("invalid binding bit")
            << QByteArray(R"json({"widgets":[{"type":"LEDIndicator","id":"led",
                "binding":{"parseMode":"Modbus","mbArea":"保持寄存器","mbRegIndex":10,"mbBit":16}}]})json")
            << QStringLiteral("控件 led 的 mbBit 必须是 -1–15 的整数");
        QTest::newRow("invalid toggle bit")
            << QByteArray(R"json({"widgets":[{"type":"ToggleSwitch","id":"sw",
                "properties":{"commandMode":"Modbus","mbArea":"保持寄存器位","mbAddress":10,"mbBit":-1}}]})json")
            << QStringLiteral("控件 sw 的 mbBit 必须是 0–15 的整数");
        QTest::newRow("invalid button bit")
            << QByteArray(R"json({"widgets":[{"type":"PushButton","id":"btn",
                "properties":{"commandMode":"Modbus","mbFunc":"06 写单寄存器","mbAddress":10,"mbBit":20}}]})json")
            << QStringLiteral("控件 btn 的 mbBit 必须是 -1–15 的整数");
        QTest::newRow("invalid bit action")
            << QByteArray(R"json({"version":"1.3","widgets":[{"type":"PushButton","id":"btn",
                "properties":{"commandMode":"Modbus","mbFunc":"06 写单寄存器","mbAddress":10,"mbBit":2,"mbBitAction":"清零"}}]})json")
            << QStringLiteral("控件 btn 的 mbBitAction 只能是 置位、复位、取反");
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

    void modbusSlaveRoutesUseEffectiveParser()
    {
        const QJsonObject explicitAscii{{"type", "SerialConfigCard"}, {"id", "ascii"},
            {"properties", QJsonObject{{"defaultParser", "modbus_ascii"}, {"modbusRole", "从站"}}}};
        const QJsonObject inherited{{"type", "TcpConfigCard"}, {"id", "inherited"},
            {"properties", QJsonObject{{"modbusRole", "从站"}}}};
        Project project;
        project.serial.insert("parser", "modbus_tcp");
        project.widgets = {explicitAscii, inherited};
        auto framed = inherited;
        framed.insert("properties", QJsonObject{{"defaultParser", "modbus_tcp"}, {FrameTemplates::ReceiveId, "hex"}});
        project.frameTemplates.insert("hex", FrameTemplates::Entry{"Frame", {"hex", {}}, {}}.toJson());
        QCOMPARE(modbusRouteDescription(framed, project), QStringLiteral("连接时选择 Modbus 解析"));
        QString error;
        QVERIFY2(Project::validate(project.toJson(), &error), qPrintable(error));
        const auto sources = routeConnections(project.widgets, RouteTransport::ModbusSlave, false, project.parserType());
        QCOMPARE(sources.size(), 1);
        QCOMPARE(sources.front().first, QStringLiteral("inherited"));
        QVERIFY(routeConnections(project.widgets, RouteTransport::ModbusSlave, false,
                                 QStringLiteral("modbus_ascii")).isEmpty());

        const auto table = [](const QString &source) {
            return QJsonObject{{"type", "ModbusSlaveTable"}, {"id", "table"},
                {"properties", QJsonObject{{"sourceCardId", source}}}};
        };
        project.widgets.append(table(QStringLiteral("inherited")));
        QVERIFY2(Project::validate(project.toJson(), &error), qPrintable(error));
        project.widgets.replace(2, table(QStringLiteral("ascii")));
        QVERIFY(!Project::validate(project.toJson(), &error));
    }

    void newWriteTargetKeepsLegacySenderInactive()
    {
        Project project;
        project.logic = {{"points", QJsonArray{QJsonObject{{"name", "setpoint"}, {"binding", QJsonObject{
            {"parseMode", "Modbus"}, {"sourceCardId", "device"}, {"mbRegIndex", 100}}}}}}};
        const QJsonObject properties{{"commandMode", "Modbus"}, {"mbWritePoint", "setpoint"},
                                     {"commandTargetId", "previous"}, {"mbAddress", 1}, {"mbValue", "1500"},
                                     {"mbFunc", "06 写单寄存器"}};
        project.widgets = {QJsonObject{{"id", "device"}, {"type", "TcpConfigCard"},
                                      {"properties", QJsonObject{{"defaultParser", "modbus_tcp"}}}},
                           QJsonObject{{"id", "write"}, {"type", "PushButton"}, {"properties", properties}}};
        const auto serialized = project.toJson();
        QCOMPARE(serialized.value("version").toString(), QStringLiteral("1.3"));
        const auto raw = serialized.value("widgets").toArray().last().toObject().value("properties").toObject();
        QCOMPARE(raw.value("commandMode").toString(), QStringLiteral("requiresSendConfiguration"));
        QVERIFY(!raw.contains("commandTargetId"));
        QVERIFY(!CommandEncoder::buttonFromProperties(raw, {}).command);
        Project restored;
        QString error;
        QVERIFY2(Project::fromJson(serialized, &restored, &error), qPrintable(error));
        const auto loaded = restored.widgets.last().toObject().value("properties").toObject();
        QCOMPARE(loaded, properties);
        const auto target = resolvePointWrite(loaded, restored.logic, restored.widgets, restored.parserType(), &error);
        QVERIFY2(target, qPrintable(error));
        QCOMPARE(target->value("commandTargetId").toString(), QStringLiteral("device"));
        QCOMPARE(target->value("mbAddress").toInt(), 100);

        restored.widgets.replace(0, QJsonObject{{"id", "device"}, {"type", "MqttConfigCard"},
            {"properties", QJsonObject{{"defaultParser", "modbus_tcp"}, {"defaultPublish", "device/request"}}}});
        auto points = restored.logic.value("points").toArray();
        auto point = points.at(0).toObject();
        auto binding = point.value("binding").toObject();
        binding.insert("sourceEndpoint", "device/reply");
        point.insert("binding", binding);
        points[0] = point;
        restored.logic.insert("points", points);
        const auto mqtt = resolvePointWrite(loaded, restored.logic, restored.widgets, restored.parserType(), &error);
        QVERIFY2(mqtt, qPrintable(error));
        QCOMPARE(mqtt->value("commandTargetId").toString(), QStringLiteral("device"));
        QVERIFY(mqtt->value("commandEndpoint").toString().isEmpty()); // Uses the card's publish topic.

        binding.insert("mbSlaveFilter", 0);
        point.insert("binding", binding);
        points[0] = point;
        restored.logic.insert("points", points);
        QVERIFY(!resolvePointWrite(loaded, restored.logic, restored.widgets, restored.parserType()));

        binding.insert("mbSlaveFilter", 1);
        point.insert("binding", binding);
        point.insert("expression", "1 + 2");
        points[0] = point;
        restored.logic.insert("points", points);
        QVERIFY(!resolvePointWrite(loaded, restored.logic, restored.widgets, restored.parserType()));
    }

    void legacyInputSourcesResolveWithoutGuessing()
    {
        const QJsonObject button{{"type", "PushButton"}, {"id", "send"}, {"properties", QJsonObject{
            {"commandTemplate", "{gain}"}, {"commandInputs", QJsonArray{QJsonObject{{"inputId", "@data:gain"}}}}}}};
        const QJsonObject input{{"type", "LineEdit"}, {"id", "@data:gain"}};
        const auto binding = [](const Project &p) {
            return CommandEncoder::commandInputIds(p.widgets.first().toObject().value("properties").toObject()).value(0);
        };
        Project source, loaded;
        QString error;
        source.widgets = {button, input};
        QVERIFY2(Project::fromJson(source.toJson(), &loaded, &error), qPrintable(error));
        QCOMPARE(binding(loaded), widgetInputId("@data:gain"));
        source.logic = {{"points", QJsonArray{QJsonObject{{"name", "gain"}, {"initialValue", 1500}}}}};
        QVERIFY2(Project::fromJson(source.toJson(), &loaded, &error), qPrintable(error));
        QCOMPARE(binding(loaded), QStringLiteral("@data:gain"));
        source.widgets.removeLast();
        QVERIFY2(Project::fromJson(source.toJson(), &loaded, &error), qPrintable(error));
        QCOMPARE(binding(loaded), pointInputId("gain"));
    }

    // Each link card carries the Modbus poller's settings right after its parser mode and before its frame
    // format, as PyQt UI Designer does: key, label, default and range. A serial, TCP or UDP card has its Modbus role
    // first and its slave settings after them.
    void linkCardsCarryPollingSettings_data()
    {
        QTest::addColumn<QString>("type");
        for (const char *type : {"SerialConfigCard", "TcpConfigCard", "MqttConfigCard", "UdpConfigCard"}) {
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
        const QStringList modbus =
            type == QLatin1String("MqttConfigCard")
                ? QStringList{"defaultParser", "modbusPeriod", "modbusTimeout", "modbusSlave", "parserConfig"}
                : QStringList{"defaultParser", "modbusRole",       "modbusPeriod", "modbusTimeout", "modbusSlave",
                              "modbusDelay",   "modbusExceptions", "slaveCoStart", "slaveCoCount",  "slaveDiStart",
                              "slaveDiCount",  "slaveHrStart",     "slaveHrCount", "slaveIrStart",  "slaveIrCount",
                              "slaveInit",     "parserConfig"};
        QCOMPARE(names.mid(parser, modbus.size()), modbus);
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
        QJsonObject out = normalizedWidgetProperties("Slider", props, "maximum");
        QCOMPARE(out.value("minimum").toInt(), 5);
        QCOMPARE(out.value("value").toInt(), 5);
        out = normalizedWidgetProperties("Slider", props, "minimum");
        QCOMPARE(out.value("maximum").toInt(), 10);
        QCOMPARE(normalizedWidgetProperties("Label", props), props);
        QJsonObject numeric{{"minimum", 0.001}, {"maximum", 12.345678}, {"value", 1.234567}};
        QCOMPARE(normalizedWidgetProperties("SpinBox", numeric).value("value").toInt(), 1);
        numeric.insert("numberMode", QStringLiteral("小数"));
        const auto decimal = normalizedWidgetProperties("SpinBox", numeric);
        QCOMPARE(decimal.value("minimum").toDouble(), 0.001);
        QCOMPARE(decimal.value("maximum").toDouble(), 12.345678);
        QCOMPARE(decimal.value("value").toDouble(), 1.234567);
        Project decimalProject;
        decimalProject.widgets = QJsonArray{QJsonObject{{"id", "setpoint"}, {"type", "SpinBox"}, {"properties", decimal}}};
        const auto savedDecimal = decimalProject.toJson();
        QCOMPARE(savedDecimal.value("version").toString(), QStringLiteral("1.3"));
        Project restored;
        QString error;
        QVERIFY2(Project::fromJson(savedDecimal, &restored, &error), qPrintable(error));
        QCOMPARE(restored.widgets, decimalProject.widgets);
        auto oldDeclaration = savedDecimal;
        oldDeclaration.insert("version", "1.0");
        QVERIFY(!Project::validate(oldDeclaration, &error));
        numeric.remove("numberMode");
        QCOMPARE(Project::formatVersion(QJsonArray{QJsonObject{{"type", "SpinBox"}, {"properties", numeric}}}),
                 QStringLiteral("1.0"));
        auto legacy = normalizedWidgetProperties("CanMonitor", {{"maxMessages", 1234}, {"autoSave", true}});
        QCOMPARE(legacy.value("autoSaveRows").toInt(), 1234);
        legacy.insert("maxMessages", 20);
        QCOMPARE(normalizedWidgetProperties("CanMonitor", legacy).value("autoSaveRows").toInt(), 1234);
        const auto fresh = WidgetRegistry::instance().find("CanMonitor")->newWidgetProperties();
        QCOMPARE(normalizedWidgetProperties("CanMonitor", fresh).value("autoSaveRows").toInt(), 100000);
    }

    void modbusBitDataPointWriteResolution()
    {
        const QJsonArray widgets{QJsonObject{{"id", "plc"}, {"type", "SerialConfigCard"},
                                             {"properties", QJsonObject{{"defaultParser", "modbus"}, {"modbusSlave", 2}}}}};
        const QJsonObject hrBit{{"parseMode", "Modbus"}, {"sourceCardId", "plc"}, {"mbArea", "保持寄存器"},
                                {"mbRegIndex", 10}, {"mbBit", 3}, {"mbDataType", "float32"}};
        const QJsonObject irBit{{"parseMode", "Modbus"}, {"sourceCardId", "plc"}, {"mbArea", "输入寄存器"},
                                {"mbRegIndex", 10}, {"mbBit", 1}};
        const QJsonObject logic{{"points", QJsonArray{
            QJsonObject{{"name", "Relay3"}, {"binding", hrBit}},
            QJsonObject{{"name", "Input1"}, {"binding", irBit}}}}};

        QVERIFY(dataPointSourceText(hrBit).contains(QStringLiteral("保持寄存器 10.3")));
        QVERIFY(!dataPointSourceText(hrBit).contains(QStringLiteral("float32")));

        QString error;
        const auto sw = resolvePointWrite(
            QJsonObject{{"commandMode", "Modbus"}, {"mbWritePoint", "Relay3"}},
            logic, widgets, QStringLiteral("line"), &error, u"ToggleSwitch");
        QVERIFY2(sw.has_value(), qPrintable(error));
        QCOMPARE(sw->value("mbArea").toString(), ModbusArea::HoldingRegisterBit);
        QCOMPARE(sw->value("mbAddress").toInt(), 10);
        QCOMPARE(sw->value("mbBit").toInt(), 3);
        QCOMPARE(sw->value("mbSlave").toInt(), 2);

        const auto btn = resolvePointWrite(
            QJsonObject{{"commandMode", "Modbus"}, {"mbFunc", "06 写单寄存器"},
                        {"mbWritePoint", "Relay3"}, {"mbBitAction", "取反"}},
            logic, widgets, QStringLiteral("line"), &error, u"PushButton");
        QVERIFY2(btn.has_value(), qPrintable(error));
        QCOMPARE(btn->value("mbBit").toInt(), 3);
        QCOMPARE(btn->value("mbBitAction").toString(), QStringLiteral("取反"));

        // FC 16 cannot target a single bit data point; input register bits are read-only.
        QVERIFY(!resolvePointWrite(
            QJsonObject{{"commandMode", "Modbus"}, {"mbFunc", "16 写多个寄存器"}, {"mbWritePoint", "Relay3"}},
            logic, widgets, QStringLiteral("line"), &error, u"PushButton"));
        QCOMPARE(error, QStringLiteral("数据点 Relay3 · 寄存器位数据点请使用 06 写单寄存器"));
        QVERIFY(!resolvePointWrite(
            QJsonObject{{"commandMode", "Modbus"}, {"mbWritePoint", "Input1"}},
            logic, widgets, QStringLiteral("line"), &error, u"ToggleSwitch"));

        // Manual bit writes save as UIDP 1.3 with sendConfiguration so older versions cannot mis-send them.
        Project bitProject;
        bitProject.widgets = QJsonArray{
            QJsonObject{{"id", "plc"}, {"type", "SerialConfigCard"},
                        {"properties", QJsonObject{{"defaultParser", "modbus"}}}},
            QJsonObject{{"id", "sw"}, {"type", "ToggleSwitch"},
                        {"properties", QJsonObject{{"commandMode", "Modbus"}, {"commandTargetId", "plc"},
                                                   {"mbArea", "保持寄存器位"}, {"mbAddress", 10}, {"mbBit", 3}}}},
            QJsonObject{{"id", "btn"}, {"type", "PushButton"},
                        {"properties", QJsonObject{{"commandMode", "Modbus"}, {"commandTargetId", "plc"},
                                                   {"mbFunc", "06 写单寄存器"}, {"mbAddress", 10},
                                                   {"mbBit", 4}, {"mbBitAction", "取反"}}}}};
        const QJsonObject saved = bitProject.toJson();
        QCOMPARE(saved.value("version").toString(), QStringLiteral("1.3"));
        for (int index : {1, 2}) {
            const QJsonObject savedProps = saved.value("widgets").toArray().at(index).toObject()
                                               .value("properties").toObject();
            QCOMPARE(savedProps.value("commandMode").toString(), QStringLiteral("requiresSendConfiguration"));
            QVERIFY(savedProps.value("sendConfiguration").isObject());
        }
        Project restored;
        QVERIFY2(Project::fromJson(saved, &restored, &error), qPrintable(error));
        QCOMPARE(restored.widgets, bitProject.widgets);
    }
};

QTEST_APPLESS_MAIN(TestProject)
#include "tst_project.moc"
