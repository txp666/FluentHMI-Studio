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
#include <FluentQtWidgets/Theme.h>
#include <QtCore/QJsonArray>
#include <QtCore/QTemporaryDir>
#include <QtGui/QPainter>
#include <QtSvg/QSvgRenderer>
#include <QtTest/QtTest>
#include <QtWidgets/QApplication>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QScrollBar>
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
    void contactSheets()
    {
        const QString output = qEnvironmentVariable("FHMI_INDUSTRIAL_CONTACT_SHEET");
        if (output.isEmpty()) {
            return;
        }
        for (bool dark : {false, true}) {
            FluentQt::ThemeManager::instance()->setTheme(dark ? FluentQt::Theme::Dark : FluentQt::Theme::Light);
            QImage sheet(6 * 192, int((industrialCatalog().size() + 5) / 6) * 176, QImage::Format_ARGB32_Premultiplied);
            sheet.fill(dark ? QColor("#172332") : QColor("#f4f7fa"));
            QPainter painter(&sheet);
            int index = 0;
            for (const auto &item : industrialCatalog()) {
                const auto type = item.toObject().value("type").toString();
                std::unique_ptr<HmiWidget> widget(createHmiWidget(type, {}));
                const QPoint origin(index % 6 * 192 + 16, index / 6 * 176);
                widget->render(&painter, origin);
                painter.setPen(dark ? Qt::white : Qt::black);
                painter.drawText(QRect(origin + QPoint(-16, 144), QSize(192, 24)), Qt::AlignCenter, type);
                ++index;
            }
            painter.end();
            QVERIFY(sheet.save(output + (dark ? "-dark.png" : "-light.png")));
        }
        FluentQt::ThemeManager::instance()->setTheme(FluentQt::Theme::Light);
    }
    void semanticControls()
    {
        BindingEntry binding;
        std::unique_ptr<HmiWidget> flowBase(
            createHmiWidget("FlowMeter", {{"minimum", -100}, {"maximum", 100}, {"value", 50}}));
        auto *flow = dynamic_cast<IndustrialWidget *>(flowBase.get());
        QVERIFY(flow);
        QCOMPARE(flow->animationRate(), 0.525);
        QVERIFY(!flow->animationRunning()); // design canvas remains still
        QVERIFY(flow->readingText().contains(QStringLiteral("正向")));
        flow->applyBoundValue(-100, binding);
        QCOMPARE(flow->animationRate(), -0.9);
        QVERIFY(flow->readingText().contains(QStringLiteral("反向")));
        flow->applyBoundValue(0, binding);
        QCOMPARE(flow->animationRate(), 0.0);
        QVERIFY(flow->readingText().contains(QStringLiteral("零流量")));
        std::unique_ptr<HmiWidget> powerBase(createHmiWidget("PowerMeter", {}));
        auto *power = dynamic_cast<IndustrialWidget *>(powerBase.get());
        power->applyBoundValue(-1, binding);
        QVERIFY(power->readingText().contains(QStringLiteral("输出")));
        std::unique_ptr<HmiWidget> pipeBase(
            createHmiWidget("FlowPipe", {{"pipeShape", "直管"}, {"pipeDirection", "镜像布局"}, {"pipeWidth", 26}}));
        auto *pipe = dynamic_cast<IndustrialWidget *>(pipeBase.get());
        QCOMPARE(pipe->svgDrivers().value("straight"), 1.0);
        QCOMPARE(pipe->svgDrivers().value("elbow"), 0.0);
        QCOMPARE(pipe->svgDrivers().value("mirrored"), 1.0);
        QCOMPARE(pipe->svgDrivers().value("thickness"), 1.0);
        QVERIFY(QSvgRenderer(pipe->renderedSvg()).isValid());
        std::unique_ptr<HmiWidget> motorBase(createHmiWidget("Motor", {{"feedbackMode", "运行位"}}));
        auto *motor = dynamic_cast<IndustrialWidget *>(motorBase.get());
        motor->applyBoundValue(2, binding);
        QVERIFY(!motor->reading());
        QVERIFY(motor->readingText().contains(QStringLiteral("数据异常")));
        motor->applyBoundValue(1, binding);
        QCOMPARE(motor->animationRate(), 0.55);
        QCOMPARE(motor->readingText(), QStringLiteral("运行"));
        motor->applyBoundValue(0, binding);
        QCOMPARE(motor->animationRate(), 0.0);
        std::unique_ptr<HmiWidget> cylinderBase(createHmiWidget("Cylinder", {{"feedbackMode", "端位状态"}}));
        auto *cylinder = dynamic_cast<IndustrialWidget *>(cylinderBase.get());
        cylinder->applyBoundValue(0, binding);
        QVERIFY(cylinder->reading());
        QCOMPARE(cylinder->svgDrivers().value("pistonKnown"), 0.0);
        QVERIFY(cylinder->readingText().contains(QStringLiteral("两端未到位")));
        cylinder->applyBoundValue(1, binding);
        QCOMPARE(cylinder->svgDrivers().value("pistonKnown"), 1.0);
        QCOMPARE(cylinder->svgDrivers().value("level"), 0.0);
        cylinder->applyBoundValue(2, binding);
        QCOMPARE(cylinder->svgDrivers().value("level"), 1.0);
        cylinder->applyBoundValue(3, binding);
        QVERIFY(!cylinder->reading());
        cylinder->applyBoundValue(1, binding);
        QVERIFY(cylinder->reading());
        std::unique_ptr<HmiWidget> compressorBase(createHmiWidget("AirCompressor", {}));
        auto *compressor = dynamic_cast<IndustrialWidget *>(compressorBase.get());
        compressor->applyBoundValue(1, binding);
        QCOMPARE(compressor->animationRate(), 0.55);
        compressor->applyBoundValue(3, binding);
        QCOMPARE(compressor->animationRate(), 0.3);
        compressor->applyBoundValue(2, binding);
        QCOMPARE(compressor->animationRate(), 0.0);
    }
    void animationStopsOutsideViewport()
    {
        RuntimeContext context;
        QScrollArea area;
        area.resize(240, 220);
        auto *content = new QWidget;
        content->setFixedSize(220, 1000);
        area.setWidget(content);
        auto *widget = dynamic_cast<IndustrialWidget *>(createHmiWidget("FlowMeter", {}, content));
        QVERIFY(widget);
        widget->move(10, 10);
        widget->attach(&context);
        context.setActive(true);
        BindingEntry binding;
        widget->applyBoundValue(25, binding);
        area.show();
        QTRY_VERIFY(widget->animationRunning());
        QVERIFY(!widget->visibleRegion().isEmpty());
        area.verticalScrollBar()->setValue(700);
        QApplication::processEvents();
        QVERIFY(widget->isVisible()); // scrolling clips it without hiding it
        QVERIFY(widget->visibleRegion().isEmpty());
        QTRY_VERIFY(!widget->animationRunning());
        const auto paused = widget->renderedSvg();
        QTest::qWait(100);
        QCOMPARE(widget->renderedSvg(), paused);
        area.verticalScrollBar()->setValue(0);
        QTRY_VERIFY(!widget->visibleRegion().isEmpty());
        QTRY_VERIFY(widget->animationRunning());
        QTRY_VERIFY(widget->renderedSvg() != paused);
    }

    void adjustableLimits()
    {
        for (const auto &type : {"PressureGauge", "Tank", "Voltmeter", "Ammeter"}) {
            std::unique_ptr<HmiWidget> base(createHmiWidget(
                type, {{"minimum", 0}, {"maximum", 100}, {"showLimits", true}, {"lowLimit", 20}, {"highLimit", 80}}));
            auto *widget = dynamic_cast<IndustrialWidget *>(base.get());
            BindingEntry binding;
            widget->applyBoundValue(20, binding);
            QCOMPARE(widget->svgDrivers().value("limitsOn"), 1.0);
            QCOMPARE(widget->svgDrivers().value("lowMark"), 0.2);
            QCOMPARE(widget->svgDrivers().value("highMark"), 0.8);
            QCOMPARE(widget->svgDrivers().value("lowAlarm"), 1.0);
            QCOMPARE(widget->svgDrivers().value("highAlarm"), 0.0);
            QVERIFY(widget->readingText().contains(QStringLiteral("低于限值")));
            widget->applyBoundValue(80, binding);
            QCOMPARE(widget->svgDrivers().value("highAlarm"), 1.0);
            widget->applyBoundValue(QStringLiteral("nan"), binding);
            QCOMPARE(widget->svgDrivers().value("highAlarm"), 0.0);
            QVERIFY(!widget->reading());
            widget->applyBoundValue(50, binding);
            QVERIFY(widget->reading());
            QCOMPARE(widget->svgDrivers().value("lowAlarm"), 0.0);
            auto props = widget->properties();
            props.insert("lowLimit", 90);
            props.insert("highLimit", 20);
            widget->setProperties(props);
            QCOMPARE(widget->svgDrivers().value("limitsOn"), 0.0);
            props.insert("lowLimit", -100);
            props.insert("highLimit", 200);
            widget->setProperties(props);
            QCOMPARE(widget->svgDrivers().value("lowMark"), 0.0);
            QCOMPARE(widget->svgDrivers().value("highMark"), 1.0);
            props.insert("showLimits", false);
            widget->setProperties(props);
            QCOMPARE(widget->svgDrivers().value("limitsOn"), 0.0);
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
        QCOMPARE(meta->defaultWidth,
                 definition.value("size").toArray().isEmpty() ? 160 : definition.value("size").toArray().at(0).toInt());
        QCOMPARE(meta->defaultHeight,
                 definition.value("size").toArray().isEmpty() ? 140 : definition.value("size").toArray().at(1).toInt());
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
        QVERIFY(!widget->animationRunning());
        QVERIFY(QSvgRenderer(widget->renderedSvg()).isValid());
        QVERIFY(!widget->renderedSvg().contains("{{"));
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
            QVERIFY(QSvgRenderer(widget->renderedSvg()).isValid());
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
        widget->show();
        QApplication::processEvents();
        const bool motion = definition.value("motion").toString() != "none";
        QCOMPARE(widget->animationRunning(), motion);
        const auto before = widget->renderedSvg();
        if (motion) {
            QTest::qWait(90);
            QVERIFY(before != widget->renderedSvg());
        }
        widget->hide();
        QVERIFY(!widget->animationRunning());
        widget->show();
        context.setActive(false);
        QVERIFY(!widget->animationRunning());
        context.setActive(true);
        QCOMPARE(widget->animationRunning(), motion);
        widget->applyBoundValue(0, *entry);
        QVERIFY(!widget->animationRunning());
        if (definition.value("mode").toString() != "number" && definition.value("mode").toString() != "word" &&
            definition.value("mode").toString() != "timing" && definition.value("mode").toString() != "encoder") {
            widget->applyBoundValue(0.5, *entry);
            QVERIFY(!widget->reading());
            if (definition.value("mode").toString() == "byte") {
                const int maximum = definition.value("maximum").toInt(255);
                widget->applyBoundValue(maximum, *entry);
                QCOMPARE(widget->readingText(), QStringLiteral("0x%1").arg(QString::number(maximum, 16).toUpper()));
                for (int bit = 0; bit < 8; ++bit)
                    QCOMPARE(widget->svgDrivers().value(QStringLiteral("bit%1").arg(bit)),
                             (maximum & (1 << bit)) ? 1.0 : 0.0);
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
