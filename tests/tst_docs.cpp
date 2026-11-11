#include "core/binding.h"
#include "core/industrial_catalog.h"
#include "core/project.h"
#include "core/widget_meta.h"
#include "core/widget_reference.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
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

    void sharedReferencesRestoreEveryProperty()
    {
        const QString reference = widgetReference();
        const auto linesIn = [](const QString &section) {
            QMap<QString, QString> properties;
            for (const QString &line : section.split('\n')) {
                const auto match = QRegularExpression(QStringLiteral("^- `([^`]+)`")).match(line);
                if (match.hasMatch()) {
                    if (properties.contains(match.captured(1))) {
                        return QMap<QString, QString>(); // duplicates must not hide a missing field
                    }
                    properties.insert(match.captured(1), line);
                }
            }
            return properties;
        };
        QMap<QString, QMap<QString, QString>> groups;
        const QRegularExpression anchor(QStringLiteral("<a id=\"([^\"]+)\"></a>"));
        auto anchors = anchor.globalMatch(reference);
        while (anchors.hasNext()) {
            const auto match = anchors.next();
            auto end = reference.indexOf("\n<a id=", match.capturedEnd());
            if (end < 0) {
                end = reference.indexOf("\n#### `", match.capturedEnd());
            }
            groups.insert(match.captured(1), linesIn(reference.mid(match.capturedEnd(), end - match.capturedEnd())));
        }
        QCOMPARE(groups.size(), 2);
        const QString industrial = doc(QStringLiteral("uidp/industrial.md")) + doc(QStringLiteral("uidp/electronics.md"));
        QVERIFY(reference.contains("uidp/industrial.md"));
        for (const auto &meta : WidgetRegistry::instance().all()) {
            if (isIndustrialCategory(meta.category)) {
                QVERIFY2(industrial.contains(QStringLiteral("| `%1` | %2 |").arg(meta.typeName, meta.displayName)),
                         qPrintable(meta.typeName));
                for (const auto &prop : meta.properties) {
                    QVERIFY2(industrial.contains(QStringLiteral("`%1`").arg(prop.name)) ||
                                 industrial.contains(QStringLiteral("`properties.%1`").arg(prop.name)),
                             qPrintable(prop.name));
                }
                continue;
            }
            const auto start = reference.indexOf(QStringLiteral("\n#### `%1` ").arg(meta.typeName));
            QVERIFY(start >= 0);
            auto end = reference.indexOf("\n#### `", start + 1);
            if (end < 0) {
                end = reference.size();
            }
            const QString section = reference.mid(start, end - start);
            auto restored = linesIn(section);
            const auto link = QRegularExpression(QStringLiteral("\\]\\(#([^)]*)\\)")).match(section);
            if (link.hasMatch()) {
                QVERIFY(groups.contains(link.captured(1)));
                for (auto it = groups[link.captured(1)].cbegin(); it != groups[link.captured(1)].cend(); ++it) {
                    QVERIFY(!restored.contains(it.key()));
                    restored.insert(it.key(), it.value());
                }
            }
            QCOMPARE(restored.size(), meta.properties.size());
            for (const auto &prop : meta.properties) {
                QVERIFY2(restored.contains(prop.name), qPrintable(meta.typeName + "." + prop.name));
                // Every repeated row must be byte-identical, including the label, type, default, range,
                // options and description. Compare against all local or shared appearances of that name.
                if (link.hasMatch() && groups[link.captured(1)].contains(prop.name)) {
                    const auto &types =
                        link.captured(1) == "command-properties"
                            ? QStringList{"PushButton", "LineEdit", "ComboBox", "ToggleSwitch", "Slider", "SpinBox"}
                            : QStringList{"SerialConfigCard", "TcpConfigCard", "UdpConfigCard", "MqttConfigCard"};
                    for (const auto &type : types) {
                        const auto *other = WidgetRegistry::instance().find(type)->prop(prop.name);
                        QVERIFY(other);
                        QCOMPARE(other->label, prop.label);
                        QCOMPARE(other->type, prop.type);
                        QCOMPARE(other->defaultValue, prop.defaultValue);
                        QCOMPARE(other->minimum, prop.minimum);
                        QCOMPARE(other->maximum, prop.maximum);
                        QCOMPARE(other->options, prop.options);
                        QCOMPARE(other->description, prop.description);
                        QCOMPARE(other->bindable, prop.bindable);
                    }
                }
            }
            // Differences must stay in the local section instead of inheriting another widget's row.
            if (meta.typeName == "ToggleSwitch") {
                QVERIFY(linesIn(section).contains("mbAddress"));
            }
            if (meta.typeName == "Slider" || meta.typeName == "SpinBox") {
                QVERIFY(linesIn(section).contains("dataType"));
                QVERIFY(linesIn(section).contains("mbFunc"));
            }
        }
        QVERIFY(groups["command-properties"].contains("commandMode"));
        QVERIFY(!groups["command-properties"].contains("dataType"));
        QVERIFY(!groups["command-properties"].contains("mbFunc"));
        QVERIFY(groups["link-properties"].contains("modbusPeriod"));
        QVERIFY(!groups["link-properties"].contains("modbusRole"));
    }

    // The template should fit in one read. Optional rules and widget definitions have their own pages.
    void guideFitsOneRead()
    {
        const qsizetype size = guide().toUtf8().size();
        QVERIFY2(size < 24000, qPrintable(QStringLiteral("docs/uidp-format.md is %1 bytes: keep it under 24 000, "
                                                         "move details to the linked references")
                                              .arg(size)));
    }

    void linkedTopicsAndProjectsOpen()
    {
        const QDir docs(QStringLiteral(FHMI_DOCS_DIR));
        const QString document = guide();
        const QRegularExpression link(QStringLiteral(R"(\]\((?!https?://|#)([^#)\s]+)(?:#[^)]*)?\))"));
        bool hasTemplate = false;
        auto matches = link.globalMatch(document);
        while (matches.hasNext()) {
            const QFileInfo target(docs, matches.next().captured(1));
            QVERIFY2(target.isFile(), qPrintable(target.filePath()));
            if (target.suffix() == QLatin1String("uidp")) {
                Project project;
                QString error;
                QVERIFY2(Project::load(target.filePath(), &project, &error), qPrintable(error));
                hasTemplate = true;
            }
        }
        QVERIFY2(hasTemplate, "the guide should link to a working example project");
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
