#include "core/project.h"

#include "core/protocol_parser.h"
#include "core/widget_meta.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QSaveFile>
#include <QtCore/QSet>

#include <cmath>

namespace fhmi {

namespace {

bool fail(QString *error, const QString &message)
{
    if (error) {
        *error = message;
    }
    return false;
}

bool isStrictInt(const QJsonValue &value)
{
    if (!value.isDouble()) {
        return false;
    }
    const double d = value.toDouble();
    return std::isfinite(d) && d == std::floor(d);
}

bool validateWidgets(const QJsonValue &items, int depth, QSet<QString> &ids, QString *error)
{
    if (!items.isArray() || depth > 64) {
        return fail(error, QStringLiteral("无效控件树"));
    }
    for (const QJsonValue &value : items.toArray()) {
        if (!value.isObject() || !value.toObject().value(QStringLiteral("type")).isString()) {
            return fail(error, QStringLiteral("控件缺少有效类型"));
        }
        const QJsonObject item = value.toObject();
        const QString type = item.value(QStringLiteral("type")).toString();
        if (!WidgetRegistry::instance().find(type)) {
            return fail(error, QStringLiteral("未知控件类型: %1").arg(type));
        }
        const QJsonValue id = item.value(QStringLiteral("id"));
        if (!id.isUndefined() && !id.isNull()) {
            if (!id.isString() || id.toString().isEmpty() || ids.contains(id.toString())) {
                return fail(error, QStringLiteral("控件 ID 无效或重复"));
            }
            ids.insert(id.toString());
        }
        const QJsonValue props = item.value(QStringLiteral("properties"));
        if (!props.isUndefined() && !props.isObject()) {
            return fail(error, QStringLiteral("控件属性必须是对象"));
        }
        const QJsonValue binding = item.value(QStringLiteral("binding"));
        if (!binding.isUndefined() && !binding.isNull() && !binding.isObject()) {
            return fail(error, QStringLiteral("控件绑定必须是对象"));
        }
        const QJsonValue geometry = item.value(QStringLiteral("geometry"));
        if (!geometry.isUndefined() && !geometry.isObject()) {
            return fail(error, QStringLiteral("控件位置必须是对象"));
        }
        const QJsonObject geo = geometry.toObject();
        for (const char *key : {"x", "y", "w", "h"}) {
            const QJsonValue v = geo.value(QLatin1String(key));
            if (v.isUndefined()) {
                continue;
            }
            if (!v.isDouble() || !std::isfinite(v.toDouble())) {
                return fail(error, QStringLiteral("控件位置必须是有限数值"));
            }
            if ((key[0] == 'w' || key[0] == 'h') && v.toDouble() <= 0) {
                return fail(error, QStringLiteral("控件尺寸必须大于零"));
            }
        }
        const QJsonValue tab = item.value(QStringLiteral("tabIndex"));
        if (!tab.isUndefined() && (!isStrictInt(tab) || tab.toDouble() < 0)) {
            return fail(error, QStringLiteral("无效选项卡索引"));
        }
        const QJsonValue children = item.value(QStringLiteral("children"));
        if (!children.isUndefined() && !validateWidgets(children, depth + 1, ids, error)) {
            return false;
        }
    }
    return true;
}

} // namespace

Project::Project()
{
    serial = QJsonObject{
        {QStringLiteral("baudRate"), 115200},
        {QStringLiteral("dataBits"), 8},
        {QStringLiteral("parity"), QStringLiteral("None")},
        {QStringLiteral("stopBits"), 1},
        {QStringLiteral("parser"), QStringLiteral("line")},
        {QStringLiteral("parserConfig"), QJsonObject{{QStringLiteral("separator"), QStringLiteral(",")}}},
    };
}

QJsonObject Project::toJson() const
{
    return QJsonObject{
        {QStringLiteral("version"), QLatin1String(FormatVersion)},
        {QStringLiteral("appName"), appName},
        {QStringLiteral("canvas"),
         QJsonObject{{QStringLiteral("width"), canvasWidth},
                     {QStringLiteral("height"), canvasHeight},
                     {QStringLiteral("gridSize"), gridSize}}},
        {QStringLiteral("widgets"), widgets},
        {QStringLiteral("serial"), serial},
    };
}

bool Project::validate(const QJsonObject &data, QString *error)
{
    if (data.value(QStringLiteral("version")).toString(QLatin1String(FormatVersion)) !=
        QLatin1String(FormatVersion)) {
        return fail(error, QStringLiteral("不支持的项目版本"));
    }
    const QJsonValue appName = data.value(QStringLiteral("appName"));
    if (!appName.isUndefined() && !appName.isString()) {
        return fail(error, QStringLiteral("应用名称必须是文本"));
    }
    const QJsonValue canvasValue = data.value(QStringLiteral("canvas"));
    if (!canvasValue.isUndefined() && !canvasValue.isObject()) {
        return fail(error, QStringLiteral("画布配置必须是对象"));
    }
    const QJsonObject canvas = canvasValue.toObject();
    const struct
    {
        const char *name;
        int fallback;
        int limit;
    } limits[] = {{"width", 1200, 9999}, {"height", 800, 9999}, {"gridSize", 10, 1000}};
    for (const auto &limit : limits) {
        const QJsonValue v = canvas.value(QLatin1String(limit.name));
        if (v.isUndefined()) {
            continue;
        }
        if (!isStrictInt(v) || v.toDouble() < 1 || v.toDouble() > limit.limit) {
            return fail(error, QStringLiteral("无效画布参数: %1").arg(QLatin1String(limit.name)));
        }
    }
    const QJsonValue serialValue = data.value(QStringLiteral("serial"));
    if (!serialValue.isUndefined() && !serialValue.isObject()) {
        return fail(error, QStringLiteral("串口配置必须是对象"));
    }
    const QJsonObject serialObject = serialValue.toObject();
    QString parserError;
    if (!createParser(serialObject.value(QStringLiteral("parser")).toString(QStringLiteral("line")),
                      serialObject.value(QStringLiteral("parserConfig")).toObject(), &parserError)) {
        return fail(error, parserError);
    }
    QSet<QString> ids;
    const QJsonValue widgetList = data.value(QStringLiteral("widgets"));
    return widgetList.isUndefined() ? true : validateWidgets(widgetList, 0, ids, error);
}

bool Project::fromJson(const QJsonObject &data, Project *out, QString *error)
{
    if (!validate(data, error)) {
        return false;
    }
    Project project;
    project.appName = data.value(QStringLiteral("appName")).toString(QStringLiteral("上位机"));
    const QJsonObject canvas = data.value(QStringLiteral("canvas")).toObject();
    project.canvasWidth = canvas.value(QStringLiteral("width")).toInt(1200);
    project.canvasHeight = canvas.value(QStringLiteral("height")).toInt(800);
    project.gridSize = canvas.value(QStringLiteral("gridSize")).toInt(10);
    project.widgets = data.value(QStringLiteral("widgets")).toArray();
    const QJsonObject serialObject = data.value(QStringLiteral("serial")).toObject();
    for (auto it = serialObject.begin(); it != serialObject.end(); ++it) {
        project.serial.insert(it.key(), it.value());
    }
    *out = project;
    return true;
}

bool Project::load(const QString &path, Project *out, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return fail(error, QStringLiteral("无法读取 %1: %2").arg(QDir::toNativeSeparators(path), file.errorString()));
    }
    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        return fail(error, QStringLiteral("JSON 格式错误: %1").arg(parseError.errorString()));
    }
    if (!doc.isObject()) {
        return fail(error, QStringLiteral("项目根节点必须是对象"));
    }
    Project project;
    if (!fromJson(doc.object(), &project, error)) {
        return false;
    }
    project.filePath = QFileInfo(path).absoluteFilePath();
    *out = project;
    return true;
}

bool Project::save(const QString &path, QString *error) const
{
    const QJsonObject document = toJson();
    if (!validate(document, error)) {
        return false;
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return fail(error, QStringLiteral("无法写入 %1: %2").arg(QDir::toNativeSeparators(path), file.errorString()));
    }
    file.write(QJsonDocument(document).toJson(QJsonDocument::Indented));
    if (!file.commit()) {
        return fail(error, QStringLiteral("保存失败: %1").arg(file.errorString()));
    }
    return true;
}

QString Project::assetBaseDir() const
{
    return filePath.isEmpty() ? QDir::currentPath() : QFileInfo(filePath).absolutePath();
}

QString Project::parserType() const
{
    return serial.value(QStringLiteral("parser")).toString(QStringLiteral("line"));
}

QJsonObject Project::parserConfig() const
{
    return serial.value(QStringLiteral("parserConfig")).toObject();
}

} // namespace fhmi
