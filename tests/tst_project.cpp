#include "core/project.h"
#include "core/widget_meta.h"

#include <QtCore/QDirIterator>
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
    }

    void rejectsInvalidDocuments()
    {
        QFETCH(QByteArray, json);
        QString error;
        Project project;
        QVERIFY(!Project::fromJson(QJsonDocument::fromJson(json).object(), &project, &error));
        QVERIFY(!error.isEmpty());
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
