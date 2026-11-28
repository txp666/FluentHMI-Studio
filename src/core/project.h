#pragma once

#include "core/data_source.h"
#include "core/message.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtCore/QHash>
#include <QtCore/QString>
#include <QtCore/QStringList>

#include <optional>

namespace fhmi {

namespace LinkParser {
struct Format;
}

// A .uidp project: the same JSON document PyQt UI Designer reads and writes.
//
// {
//   "version": "1.0", "appName": "...", "icon": "logo.png",
//   "canvas": {"width": 1200, "height": 800, "gridSize": 10},
//   "widgets": [{"id", "type", "geometry": {x, y, w, h}, "properties": {...},
//                "binding": {...}, "tabIndex": 0, "children": [...]}],
//   "serial": {"baudRate", "dataBits", "parity", "stopBits", "parser", "parserConfig"}
// }
class Project
{
  public:
    static constexpr const char *FormatVersion = "1.3";
    static constexpr const char *FileSuffix = "uidp";

    Project();

    QString appName = QStringLiteral("上位机"); // i18n: value
    // The exported application's icon (PNG, JPG or ICO), relative to the project's folder or absolute; empty
    // for the default one.
    QString icon;
    int canvasWidth = 1200;
    int canvasHeight = 800;
    int gridSize = 10;
    QJsonArray widgets;
    QJsonObject serial;
    QJsonObject bitProfiles;
    QJsonObject logic;
    QJsonObject frameTemplates;
    QString filePath; // empty until saved or loaded
    QHash<QString, QStringList> recentEndpoints; // Runtime preview suggestions for this open project.
    QHash<QString, Message> recentTextSamples;

    static QString formatVersion(const QJsonArray &widgets, const QJsonObject &logic = {},
                                 const QJsonObject &frameTemplates = {});
    QJsonObject toJson() const;

    // Rejects invalid documents before anything replaces the current project.
    // A clipboard selection may refer to a connection card outside that selection. Saved projects always require it.
    static bool validate(const QJsonObject &document, QString *error, bool allowExternalReferences = false,
                         Project *validated = nullptr);
    static bool fromJson(const QJsonObject &document, Project *out, QString *error, bool allowExternalReferences = false);
    // A document toJson() wrote, unchecked: the designer's undo history holds edits in progress, such as a
    // separator cleared to be retyped, which fromJson() would refuse.
    static Project fromTrustedJson(const QJsonObject &document);

    static bool load(const QString &path, Project *out, QString *error);
    // Atomic: writes a temporary file beside the target, then renames it over.
    bool save(const QString &path, QString *error) const;

    // Directory that relative image paths resolve against.
    QString assetBaseDir() const;
    // The icon file, or empty when the project has none.
    QString iconPath() const;

    QString parserType() const;
    QJsonObject parserConfig() const;
    LinkParser::Format receiveFormat() const;
    LinkParser::Format linkReceiveFormat(const QJsonObject &properties) const;
    QStringList linkReceiveTemplateIds(const QJsonObject &properties) const;
    QString linkReceiveTemplateId(const QJsonObject &properties) const;
};

// The configured connections that can serve a route, regardless of current connection state.
QList<QPair<QString, QString>> routeConnections(const QJsonArray &widgets, const RouteFilter &filter,
                                              bool needsEndpoint = false,
                                              const QString &projectParser = QStringLiteral("line"));

enum class RoutePurpose { Send, Binding, DataPoint };

// Resolves a configured route. An empty automatic route can remain unresolved while drafting.
std::optional<DataRoute> resolveProjectRoute(const QJsonArray &widgets, const QString &projectParser,
                                             const RouteFilter &filter, DataRoute route, RoutePurpose purpose,
                                             QString *error = nullptr,
                                             ModbusParserChoice parserChoice = ModbusParserChoice::Project);
QString modbusRouteDescription(const QJsonObject &card, const Project &project);

int chartCurveCount(QStringView type, const QJsonObject &properties);

// Walk a widget tree (depth first); return false from the visitor to stop.
template <typename Visitor> bool visitWidgets(const QJsonArray &widgets, Visitor &&visit)
{
    for (const QJsonValue &value : widgets) {
        const QJsonObject widget = value.toObject();
        if (!visit(widget)) {
            return false;
        }
        if (!visitWidgets(widget.value(QStringLiteral("children")).toArray(), visit)) {
            return false;
        }
    }
    return true;
}

} // namespace fhmi
