#include "comm/comm_hub.h"
#include "comm/link.h"
#include "designer/canvas_item.h"
#include "designer/design_scene.h"
#include "runtime/hmi_widget.h"
#include "runtime/runtime_context.h"
#include "runtime/runtime_surface.h"
#include "studio/export_service.h"

#include <FluentQtWidgets/Widgets/Button.h>

#include <QtCore/QDirIterator>
#include <QtCore/QJsonArray>
#include <QtCore/QTemporaryDir>
#include <QtGui/QImage>
#include <QtTest/QtTest>
#include <QtWidgets/QApplication>
#include <QtWidgets/QLabel>

using namespace fhmi;

namespace {

// A link that is always connected, records what is sent, and injects received bytes.
class FakeLink : public Link
{
  public:
    using Link::Link;
    QString description() const override { return QStringLiteral("fake"); }
    bool isConnected() const override { return true; }
    void close() override {}
    void inject(const QByteArray &bytes) { deliver(defaultParser(), bytes); }
    QList<QByteArray> sent;

  protected:
    qint64 writeBytes(const QByteArray &bytes) override
    {
        sent.append(bytes);
        return bytes.size();
    }
};

QStringList exampleFiles()
{
    QStringList files;
    QDirIterator it(QStringLiteral(FHMI_EXAMPLES_DIR), {QStringLiteral("*.uidp")}, QDir::Files,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        files.append(it.next());
    }
    return files;
}

// What a save writes for a widget tree, with the optional keys a writer may omit.
QJsonArray normalized(const QJsonArray &widgets)
{
    QJsonArray out;
    for (const QJsonValue &value : widgets) {
        QJsonObject widget = value.toObject();
        if (widget.value(QStringLiteral("tabIndex")).toInt(0) == 0) {
            widget.remove(QStringLiteral("tabIndex"));
        }
        if (widget.value(QStringLiteral("binding")).toObject().isEmpty()) {
            widget.remove(QStringLiteral("binding"));
        }
        const QJsonArray children = normalized(widget.value(QStringLiteral("children")).toArray());
        if (children.isEmpty()) {
            widget.remove(QStringLiteral("children"));
        } else {
            widget.insert(QStringLiteral("children"), children);
        }
        widget.insert(QStringLiteral("properties"),
                      normalizedRangeProperties(widget.value(QStringLiteral("type")).toString(),
                                                widget.value(QStringLiteral("properties")).toObject()));
        out.append(widget);
    }
    return out;
}

} // namespace

class TestRuntime : public QObject
{
    Q_OBJECT

  private slots:
    void initTestCase()
    {
        Q_INIT_RESOURCE(fluentqtwidgets);
    }

    void everyTypeHasAWidget()
    {
        for (const WidgetMeta &meta : WidgetRegistry::instance().all()) {
            std::unique_ptr<HmiWidget> widget(createHmiWidget(meta.typeName, {}));
            QVERIFY2(widget, qPrintable(meta.typeName));
            QCOMPARE(widget->typeName(), meta.typeName);
        }
    }

    void examplesBuildAndRun_data()
    {
        QTest::addColumn<QString>("path");
        for (const QString &file : exampleFiles()) {
            QTest::newRow(qPrintable(QFileInfo(file).fileName())) << file;
        }
    }

    void examplesBuildAndRun()
    {
        QFETCH(QString, path);
        Project project;
        QString error;
        QVERIFY2(Project::load(path, &project, &error), qPrintable(error));
        RuntimeSurface surface;
        surface.build(project);
        int expected = 0;
        visitWidgets(project.widgets, [&expected](const QJsonObject &) { return ++expected > 0; });
        QCOMPARE(surface.widgets().size(), expected);
        QCOMPARE(surface.size(), QSize(project.canvasWidth, project.canvasHeight));
        surface.clear();
        QVERIFY(surface.widgets().isEmpty());
    }

    void designerRoundTripsExamples_data()
    {
        examplesBuildAndRun_data();
    }

    void designerRoundTripsExamples()
    {
        QFETCH(QString, path);
        Project project;
        QString error;
        QVERIFY2(Project::load(path, &project, &error), qPrintable(error));
        DesignScene scene;
        QVERIFY2(scene.loadWidgets(project.widgets, &error), qPrintable(error));
        QCOMPARE(normalized(scene.widgetsJson()), normalized(project.widgets));
    }

    void bindingUpdatesDisplayAndButtonSends()
    {
        Project project;
        project.widgets = QJsonArray{
            QJsonObject{{"id", "temp"},
                        {"type", "NumericDisplay"},
                        {"geometry", QJsonObject{{"x", 10}, {"y", 10}, {"w", 160}, {"h", 90}}},
                        {"properties", QJsonObject{{"decimals", "1"}}},
                        {"binding", QJsonObject{{"parseMode", "键值匹配"}, {"key", "t"}}}},
            QJsonObject{{"id", "run"},
                        {"type", "PushButton"},
                        {"geometry", QJsonObject{{"x", 10}, {"y", 120}, {"w", 120}, {"h", 36}}},
                        {"properties", QJsonObject{{"command", "RUN"}, {"commandNewline", "\\n"}}}},
        };
        RuntimeSurface surface;
        surface.build(project);
        FakeLink link;
        surface.context()->hub()->addLink(&link);

        link.inject("t=21.46\n");
        bool shown = false;
        for (QLabel *label : surface.findChildren<QLabel *>()) {
            shown = shown || label->text() == QStringLiteral("21.5");
        }
        QVERIFY(shown);

        auto *button = surface.findChild<FluentQt::PushButton *>();
        QVERIFY(button);
        button->click();
        QCOMPARE(link.sent, QList<QByteArray>{QByteArray("RUN\n")});
        surface.context()->hub()->removeLink(&link);
    }

    void exportCarriesProjectAndImages()
    {
        QTemporaryDir source;
        QTemporaryDir output;
        QImage image(8, 8, QImage::Format_RGB32);
        image.fill(Qt::red);
        QVERIFY(image.save(source.filePath("logo.png")));

        Project project;
        project.appName = QStringLiteral("温控/面板");
        project.filePath = source.filePath("panel.uidp");
        project.widgets = QJsonArray{QJsonObject{{"id", "img"},
                                                 {"type", "ImageDisplay"},
                                                 {"properties", QJsonObject{{"imagePath", "logo.png"}}}}};
        QString error;
        const QString created = ExportService::exportApplication(project, output.path(), &error);
        QVERIFY2(!created.isEmpty(), qPrintable(error));
        QVERIFY(QFileInfo(created).fileName().startsWith(QStringLiteral("温控_面板")));
#ifdef Q_OS_MACOS
        const QDir resources(created + "/Contents/Resources");
        QVERIFY(QFileInfo::exists(created + "/Contents/MacOS/FluentHMIRuntime"));
#else
        const QDir resources(created);
#endif
        Project exported;
        QVERIFY2(Project::load(resources.filePath("project.uidp"), &exported, &error), qPrintable(error));
        QCOMPARE(exported.appName, project.appName);
        const QString bundled = exported.widgets.at(0).toObject().value("properties").toObject().value("imagePath").toString();
        QCOMPARE(bundled, QStringLiteral("assets/logo.png"));
        QVERIFY(QFileInfo::exists(resources.filePath(bundled)));

        // A second export to the same place refuses rather than overwriting.
        QVERIFY(ExportService::exportApplication(project, output.path(), &error).isEmpty());
        QVERIFY(!error.isEmpty());

        // A project without images still gets its project file.
        Project plain;
        plain.appName = QStringLiteral("plain");
        QVERIFY2(!ExportService::exportApplication(plain, output.path(), &error).isEmpty(), qPrintable(error));
    }

    void containerNestingInDesigner()
    {
        DesignScene scene;
        CanvasItem *group = scene.createItem("VBoxLayout", QPointF(100, 100));
        QVERIFY(group);
        group->setSize(QSizeF(300, 300));
        CanvasItem *child = scene.createItem("PushButton", QPointF(150, 200));
        QCOMPARE(child->containerParent(), group);
        QCOMPARE(child->pos().x(), 8.0); // laid out by the vertical box
        const QJsonArray json = scene.widgetsJson();
        QCOMPARE(json.size(), 1);
        QCOMPARE(json.at(0).toObject().value("children").toArray().size(), 1);
    }
};

QTEST_MAIN(TestRuntime)
#include "tst_runtime.moc"
