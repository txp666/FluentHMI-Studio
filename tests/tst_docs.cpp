#include "core/binding.h"
#include "core/project.h"
#include "core/widget_meta.h"
#include "core/widget_reference.h"

#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QRegularExpression>
#include <QtTest/QtTest>

using namespace fhmi;

namespace {

// A file in docs/: uidp-format.md is the guide that lets people and AI assistants write .uidp files by hand,
// uidp-widgets.md its widget reference.
QString doc(const QString &name)
{
    QFile file(QStringLiteral(FHMI_DOCS_DIR "/") + name);
    if (!file.open(QIODevice::ReadOnly)) {
        return QString();
    }
    // A Windows checkout may turn the line endings into CRLF.
    return QString::fromUtf8(file.readAll()).replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
}

QString guide()
{
    return doc(QStringLiteral("uidp-format.md"));
}

// The first ```json block after the heading of the complete example.
QJsonObject exampleProject(const QString &document)
{
    const qsizetype heading = document.indexOf(QStringLiteral("## 一个完整的例子"));
    if (heading < 0) {
        return {};
    }
    static const QRegularExpression block(QStringLiteral("```json\\n(.*?)\\n```"),
                                          QRegularExpression::DotMatchesEverythingOption);
    const QRegularExpressionMatch match = block.match(document, heading);
    return match.hasMatch() ? QJsonDocument::fromJson(match.captured(1).toUtf8()).object() : QJsonObject();
}

} // namespace

class TestDocs : public QObject
{
    Q_OBJECT

  private slots:
    void generatedPartIsCurrent_data()
    {
        QTest::addColumn<QString>("name");
        QTest::addColumn<QString>("generated");
        QTest::newRow("widget list") << QStringLiteral("uidp-format.md") << widgetIndex();
        QTest::newRow("widget reference") << QStringLiteral("uidp-widgets.md") << widgetReference();
    }

    void generatedPartIsCurrent()
    {
        QFETCH(QString, name);
        QFETCH(QString, generated);
        const QString document = doc(name);
        QVERIFY2(!document.isEmpty(), qPrintable(QStringLiteral("docs/%1 not found").arg(name)));
        const QString updated = withGenerated(document, generated);
        QVERIFY2(!updated.isEmpty(), qPrintable(QStringLiteral("docs/%1 has lost the marker line of its "
                                                               "generated part").arg(name)));
        QVERIFY2(updated == document, qPrintable(QStringLiteral("docs/%1 is out of date: run <build dir>/uidp_reference "
                                                                "docs from the source directory").arg(name)));
    }

    // An AI assistant reads the guide with one command. In a test the 32 KB guide came back as "output too
    // large", and the assistant cloned the whole repository to read it, which took several more steps; with
    // the widget reference moved out, the 16 KB guide came back whole.
    void guideFitsOneRead()
    {
        const qsizetype size = guide().toUtf8().size();
        QVERIFY2(size < 24000, qPrintable(QStringLiteral("docs/uidp-format.md is %1 bytes: keep it under 24 000, "
                                                         "details belong in uidp-widgets.md").arg(size)));
    }

    // The README's widget screenshots (docs/images/widgets-light.jpg and widgets-dark.jpg) are this project
    // rendered by FluentHMIRuntime <project> --theme light|dark --screenshot <png>, then made JPEG. It holds
    // every widget type and no other, so adding or removing one fails here until the screenshots follow.
    void galleryHoldsEveryWidget()
    {
        Project project;
        QString error;
        QVERIFY2(Project::load(QStringLiteral(FHMI_DOCS_DIR "/images/widgets.uidp"), &project, &error),
                 qPrintable(error));
        QStringList shown;
        visitWidgets(project.widgets, [&shown](const QJsonObject &widget) {
            shown.append(widget.value(QStringLiteral("type")).toString());
            return true;
        });
        shown.removeDuplicates();
        shown.sort();
        QStringList registered;
        for (const WidgetMeta &meta : WidgetRegistry::instance().all()) {
            registered.append(meta.typeName);
        }
        registered.sort();
        QCOMPARE(shown, registered);
    }

    // The example is what an AI copies from first, so it must load and use only real names and values:
    // a misspelt property or option is not an error when loading, it just does nothing.
    void exampleProjectIsValid()
    {
        const QJsonObject data = exampleProject(guide());
        QVERIFY2(!data.isEmpty(), "no ```json example under \"## 一个完整的例子\"");
        Project project;
        QString error;
        QVERIFY2(Project::fromJson(data, &project, &error), qPrintable(error));
        static const QStringList parseModes = {ParseMode::KeyValue, ParseMode::JsonPath, ParseMode::HexBytes,
                                               ParseMode::Modbus};
        for (const QJsonValue &value : project.widgets) {
            const QJsonObject widget = value.toObject();
            const QString type = widget.value(QStringLiteral("type")).toString();
            const WidgetMeta *meta = WidgetRegistry::instance().find(type);
            QVERIFY(meta);
            const QJsonObject props = widget.value(QStringLiteral("properties")).toObject();
            for (auto it = props.begin(); it != props.end(); ++it) {
                if (type == QLatin1String("RealtimeChart") && it.key().startsWith(QLatin1String("_curve_"))) {
                    continue; // per-curve settings, see "实时曲线" in the guide
                }
                const PropDef *prop = meta->prop(it.key());
                QVERIFY2(prop, qPrintable(QStringLiteral("%1 has no property %2").arg(type, it.key())));
                if (prop->type == PropType::Enum) {
                    QVERIFY2(prop->options.contains(it.value().toString()),
                             qPrintable(QStringLiteral("%1.%2 has no option %3").arg(type, it.key(),
                                                                                    it.value().toString())));
                }
            }
            const QJsonObject binding = widget.value(QStringLiteral("binding")).toObject();
            if (!binding.isEmpty()) {
                QVERIFY2(!meta->bindProperty().isEmpty(), qPrintable(type + QStringLiteral(" cannot be bound")));
                QVERIFY(parseModes.contains(binding.value(QStringLiteral("parseMode")).toString()));
            }
        }
    }
};

QTEST_APPLESS_MAIN(TestDocs)
#include "tst_docs.moc"
