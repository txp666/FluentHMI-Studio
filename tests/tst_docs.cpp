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

// docs/uidp-format.md, the guide that lets people and AI assistants write .uidp files by hand.
QString guide()
{
    QFile file(QStringLiteral(FHMI_DOCS_DIR "/uidp-format.md"));
    if (!file.open(QIODevice::ReadOnly)) {
        return QString();
    }
    // A Windows checkout may turn the line endings into CRLF.
    return QString::fromUtf8(file.readAll()).replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
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
    void widgetReferenceIsCurrent()
    {
        const QString document = guide();
        QVERIFY2(!document.isEmpty(), "docs/uidp-format.md not found");
        const QString updated = withWidgetReference(document);
        QVERIFY2(!updated.isEmpty(), "docs/uidp-format.md has lost the marker line of its widget reference");
        QVERIFY2(updated == document, "The widget reference in docs/uidp-format.md is out of date: run "
                                      "<build dir>/uidp_reference docs/uidp-format.md from the source directory");
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
