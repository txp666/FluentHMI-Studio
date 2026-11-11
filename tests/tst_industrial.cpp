#include "core/i18n.h"
#include "core/industrial_catalog.h"
#include "core/project.h"
#include "designer/canvas_item.h"
#include "designer/design_scene.h"
#include "designer/property_editor.h"
#include "designer/widget_palette.h"
#include "runtime/hmi_widget.h"
#include "runtime/modbus_poller.h"
#include "runtime/widgets/industrial_widget.h"
#include <QtCore/QJsonArray>
#include <QtCore/QTemporaryDir>
#include <QtTest/QtTest>
#include <QtWidgets/QApplication>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QTreeWidget>

using namespace fhmi;

class TestIndustrial : public QObject
{
    Q_OBJECT
  private slots:
    void initTestCase() { Q_INIT_RESOURCE(fluentqtwidgets); }
    void designerIntegration()
    {
        WidgetPalette palette;
        auto *tree = palette.findChild<QTreeWidget *>();
        QVERIFY(tree);
        QMap<QString, int> groups;
        for (int i = 0; i < tree->topLevelItemCount(); ++i) {
            auto *group = tree->topLevelItem(i);
            groups.insert(group->text(0), group->childCount());
        }
        QCOMPARE(groups.value(QStringLiteral("工业设备")), 25);
        QCOMPARE(groups.value(QStringLiteral("工业仪表")), 12);
        QCOMPARE(groups.value(QStringLiteral("汽车电子")), 13);
        QCOMPARE(groups.value(QStringLiteral("电子 DIY")), 14);
        QCOMPARE(groups.value(QStringLiteral("嵌入式")), 14);
        for (const auto &definition : industrialCatalog()) {
            const auto type = definition.toObject().value("type").toString();
            DesignScene scene;
            QString error;
            QVERIFY2(
                scene.loadWidgets(QJsonArray{QJsonObject{
                                      {"type", type}, {"id", "device"}, {"binding", QJsonObject{{"key", "reading"}}}}},
                                  &error),
                qPrintable(error));
            PropertyEditor editor;
            editor.setScene(&scene);
            editor.showItem(scene.findItem("device"));
            bool expressionFound = false;
            for (auto *form : editor.findChildren<QFormLayout *>()) {
                for (int row = 0; row < form->rowCount(); ++row) {
                    auto *caption = form->itemAt(row, QFormLayout::LabelRole);
                    auto *label = caption ? qobject_cast<QLabel *>(caption->widget()) : nullptr;
                    if (label && label->text() == QStringLiteral("转换表达式")) {
                        QCOMPARE(row, form->rowCount() - 1);
                        auto *edit = qobject_cast<QLineEdit *>(form->itemAt(row, QFormLayout::FieldRole)->widget());
                        QVERIFY(edit);
                        edit->setText("value * 2");
                        expressionFound = true;
                    }
                }
            }
            QVERIFY(expressionFound);
            QCOMPARE(scene.widgetsJson().first().toObject().value("binding").toObject().value("expression").toString(),
                     QStringLiteral("value * 2"));
        }
    }
    void englishNames()
    {
        applyLanguage(qApp, kLanguageEnglish);
        QCOMPARE(categoryTitle(WidgetCategory::IndustrialEquipment), QStringLiteral("Industrial equipment"));
        for (const auto &item : industrialCatalog()) {
            const auto object = item.toObject();
            const auto *meta = WidgetRegistry::instance().find(object.value("type").toString());
            QCOMPARE(translated(meta->displayName), object.value("en").toString());
            QCOMPARE(meta->newWidgetProperties().value("title").toString(), object.value("en").toString());
        }
        for (auto *translator : qApp->findChildren<TableTranslator *>()) {
            qApp->removeTranslator(translator);
            delete translator;
        }
        applyLanguage(qApp, kLanguageChinese);
    }
    void semanticControls()
    {
        BindingEntry binding;
        std::unique_ptr<HmiWidget> flowBase(
            createHmiWidget("FlowMeter", {{"minimum", -100}, {"maximum", 100}, {"value", 50}}));
        auto *flow = dynamic_cast<IndustrialWidget *>(flowBase.get());
        QVERIFY(flow);
        QVERIFY(flow->readingText().contains(QStringLiteral("正向")));
        flow->applyBoundValue(-100, binding);
        QVERIFY(flow->readingText().contains(QStringLiteral("反向")));
        flow->applyBoundValue(0, binding);
        QVERIFY(flow->readingText().contains(QStringLiteral("零流量")));
        std::unique_ptr<HmiWidget> powerBase(createHmiWidget("PowerMeter", {}));
        auto *power = dynamic_cast<IndustrialWidget *>(powerBase.get());
        power->applyBoundValue(-1, binding);
        QVERIFY(power->readingText().contains(QStringLiteral("输出")));
        std::unique_ptr<HmiWidget> motorBase(createHmiWidget("Motor", {{"feedbackMode", "运行位"}}));
        auto *motor = dynamic_cast<IndustrialWidget *>(motorBase.get());
        motor->applyBoundValue(2, binding);
        QVERIFY(!motor->reading());
        QVERIFY(motor->readingText().contains(QStringLiteral("数据异常")));
        motor->applyBoundValue(1, binding);
        QCOMPARE(motor->readingText(), QStringLiteral("运行"));
        std::unique_ptr<HmiWidget> cylinderBase(createHmiWidget("Cylinder", {{"feedbackMode", "端位状态"}}));
        auto *cylinder = dynamic_cast<IndustrialWidget *>(cylinderBase.get());
        cylinder->applyBoundValue(0, binding);
        QVERIFY(cylinder->reading());
        QVERIFY(cylinder->readingText().contains(QStringLiteral("两端未到位")));
        cylinder->applyBoundValue(3, binding);
        QVERIFY(!cylinder->reading());
        cylinder->applyBoundValue(1, binding);
        QVERIFY(cylinder->reading());
    }
    void adjustableLimits()
    {
        for (const auto &type : {"PressureGauge", "Tank", "Voltmeter", "Ammeter"}) {
            std::unique_ptr<HmiWidget> base(createHmiWidget(
                type, {{"minimum", 0}, {"maximum", 100}, {"showLimits", true}, {"lowLimit", 20}, {"highLimit", 80}}));
            auto *widget = dynamic_cast<IndustrialWidget *>(base.get());
            BindingEntry binding;
            widget->applyBoundValue(20, binding);
            QVERIFY(widget->readingText().contains(QStringLiteral("低于限值")));
            widget->applyBoundValue(QStringLiteral("nan"), binding);
            QVERIFY(!widget->reading());
            widget->applyBoundValue(50, binding);
            QVERIFY(widget->reading());
        }
    }
    void allTypes_data()
    {
        QTest::addColumn<QString>("type");
        QCOMPARE(industrialCatalog().size(), 78);
        for (const auto &item : industrialCatalog()) {
            const auto type = item.toObject().value("type").toString();
            QTest::newRow(qPrintable(type)) << type;
        }
    }
    void allTypes()
    {
        QFETCH(QString, type);
        const auto definition = industrialDefinition(type);
        const auto *meta = WidgetRegistry::instance().find(type);
        QVERIFY(meta);
        QVERIFY(isIndustrialCategory(meta->category));
        QCOMPARE(meta->bindProperty(), QStringLiteral("value"));
        const int special = type == "FlowPipe" ? 3 : (type == "Motor" || type == "Cylinder" ? 1 : 0);
        const int expected = type == "BmsCellMatrix" || type == "DcPowerSupply" ? 1
                             : type == "StatusWordPanel" || type == "DigitalTimingChart" || type == "RotaryEncoder"
                                 ? 3
                                 : 6 + special + (definition.value("limits").toArray().size() == 2 ? 3 : 0);
        QCOMPARE(meta->properties.size(), expected);
        std::unique_ptr<HmiWidget> created(createHmiWidget(type, {}));
        auto *widget = dynamic_cast<IndustrialWidget *>(created.get());
        QVERIFY(widget);
        QVERIFY(widget->reading());
        RuntimeContext context;
        widget->attach(&context);
        context.registerWidget("device", widget);
        const QJsonObject binding{{"parseMode", ParseMode::KeyValue}, {"key", "reading"}, {"expression", "value + 1"}};
        const QJsonObject serialized{
            {"id", "device"}, {"type", type}, {"properties", widget->properties()}, {"binding", binding}};
        const auto entry = BindingEntry::fromWidget(serialized);
        QVERIFY(entry);
        context.addBinding(*entry);
        context.setActive(true);
        if (type == "BmsCellMatrix" || type == "DcPowerSupply") {
            const QVariant payload = definition.value("value").toVariant();
            emit context.hub()->messageReceived({{"reading", payload}});
            QVERIFY(widget->reading());
            return;
        }

        QVERIFY(!widget->reading());
        QVERIFY(widget->readingText().contains(QStringLiteral("待数据")));
        emit context.hub()->messageReceived({{"unrelated", 99LL}});
        QVERIFY(!widget->reading());
        emit context.hub()->messageReceived({{"reading", 0LL}});
        QCOMPARE(widget->reading(), std::optional<double>(1));
        QCOMPARE(widget->properties().value("value"), serialized.value("properties").toObject().value("value"));
        emit context.hub()->messageReceived({{"reading", QStringLiteral("invalid")}});
        QVERIFY(!widget->reading());
        QVERIFY(widget->readingText().contains(QStringLiteral("数据异常")));
        emit context.hub()->messageReceived({{"reading", 0LL}});
        QCOMPARE(widget->reading(), std::optional<double>(1));
        if (definition.value("mode").toString() != "number" && definition.value("mode").toString() != "word" &&
            definition.value("mode").toString() != "timing" && definition.value("mode").toString() != "encoder") {
            widget->applyBoundValue(0.5, *entry);
            QVERIFY(!widget->reading());
            if (definition.value("mode").toString() == "byte") {
                const int maximum = definition.value("maximum").toInt(255);
                widget->applyBoundValue(maximum, *entry);
                QCOMPARE(widget->readingText(), QStringLiteral("0x%1").arg(QString::number(maximum, 16).toUpper()));
                widget->applyBoundValue(0, *entry);
                QCOMPARE(widget->readingText(), QStringLiteral("0x00"));
            }
            if (definition.value("mode").toString() == "flags") {
                widget->applyBoundValue(5, *entry);
                const auto labels = definition.value("flags").toArray();
                QCOMPARE(widget->readingText(),
                         labels.at(0).toString() + QString::fromUtf8("、") + labels.at(2).toString());
            }
            const auto mode = definition.value("mode").toString();
            widget->applyBoundValue(mode == "byte" || mode == "flags" ? definition.value("maximum").toInt() + 1 : 99,
                                    *entry);
            QVERIFY(!widget->reading());
        }
        QJsonObject mb = serialized;
        mb.insert("binding", QJsonObject{{"parseMode", ParseMode::Modbus}, {"mbRegIndex", 100}});
        const auto modbus = BindingEntry::fromWidget(mb);
        QVERIFY(modbus);
        QVERIFY(modbusNeed(*modbus));
        QCOMPARE(modbus->evaluate(modbus->extractFromParsed({{"hr100", 1LL}})).toDouble(), 1.0);
        context.addBinding(*modbus);
        QCOMPARE(context.modbusNeeds().size(), 1);
        emit context.hub()->messageReceived({{"hr100", 1LL}});
        QCOMPARE(widget->reading(), std::optional<double>(1));
        const auto addBinding = [&](const QJsonObject &fields) {
            QJsonObject data = serialized;
            data.insert("binding", fields);
            auto entry = BindingEntry::fromWidget(data);
            QVERIFY(entry);
            context.addBinding(*entry);
        };
        addBinding({{"parseMode", ParseMode::JsonPath}, {"jsonPath", "payload.value"}, {"expression", "value + 1"}});
        emit context.hub()->messageReceived({{"payload", QVariantMap{{"value", 0}}}});
        QCOMPARE(widget->reading(), std::optional<double>(1));
        addBinding({{"parseMode", ParseMode::HexBytes}, {"dataType", "uint16"}, {"expression", "value + 1"}});
        emit context.hub()->rawReceived(QByteArray::fromHex("0000"));
        QCOMPARE(widget->reading(), std::optional<double>(1));
        addBinding({{"parseMode", ParseMode::CanSignal},
                    {"canId", "0x123"},
                    {"canBitLength", 8},
                    {"expression", "value + 1"}});
        CanFrame frame;
        frame.id = 0x123;
        frame.data = QByteArray::fromHex("00");
        emit context.hub()->canFrames(nullptr, {frame}, false);
        QCOMPARE(widget->reading(), std::optional<double>(1));

        Project project;
        project.widgets = QJsonArray{serialized};
        QTemporaryDir dir;
        QString error;
        QVERIFY2(project.save(dir.filePath("industrial.uidp"), &error), qPrintable(error));
        Project loaded;
        QVERIFY2(Project::load(dir.filePath("industrial.uidp"), &loaded, &error), qPrintable(error));
        QCOMPARE(loaded.widgets, project.widgets);
    }
};
QTEST_MAIN(TestIndustrial)
#include "tst_industrial.moc"
