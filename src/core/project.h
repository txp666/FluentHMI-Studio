#pragma once

#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtCore/QString>

namespace fhmi {

// A .uidp project: the same JSON document PyQt UI Designer reads and writes.
//
// {
//   "version": "1.0", "appName": "...",
//   "canvas": {"width": 1200, "height": 800, "gridSize": 10},
//   "widgets": [{"id", "type", "geometry": {x, y, w, h}, "properties": {...},
//                "binding": {...}, "tabIndex": 0, "children": [...]}],
//   "serial": {"baudRate", "dataBits", "parity", "stopBits", "parser", "parserConfig"}
// }
class Project
{
  public:
    static constexpr const char *FormatVersion = "1.0";
    static constexpr const char *FileSuffix = "uidp";

    Project();

    QString appName = QStringLiteral("上位机");
    int canvasWidth = 1200;
    int canvasHeight = 800;
    int gridSize = 10;
    QJsonArray widgets;
    QJsonObject serial;
    QString filePath; // empty until saved or loaded

    QJsonObject toJson() const;

    // Rejects invalid documents before anything replaces the current project.
    static bool validate(const QJsonObject &document, QString *error);
    static bool fromJson(const QJsonObject &document, Project *out, QString *error);

    static bool load(const QString &path, Project *out, QString *error);
    // Atomic: writes a temporary file beside the target, then renames it over.
    bool save(const QString &path, QString *error) const;

    // Directory that relative image paths resolve against.
    QString assetBaseDir() const;

    QString parserType() const;
    QJsonObject parserConfig() const;
};

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
