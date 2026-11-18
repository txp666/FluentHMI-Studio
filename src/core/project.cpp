#include "core/project.h"
#include "core/bit_profile.h"
#include "core/project_logic.h"
#include "core/s7.h"
#include "core/value_text.h"

#include "core/binding.h"
#include "core/data_source.h"
#include "core/protocol_parser.h"
#include "core/widget_meta.h"

#include <QtCore/QCoreApplication>
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

// A link card's own frame format, checked as the card would build its parser: with its own
// mode, or the project's when it uses the project settings.
bool validateCardParser(const QJsonObject &item, const QString &projectParser, QString *error)
{
    static const QSet<QString> cards = {QStringLiteral("SerialConfigCard"), QStringLiteral("TcpConfigCard"),
                                        QStringLiteral("MqttConfigCard"), QStringLiteral("UdpConfigCard")};
    const QJsonObject props = item.value(QStringLiteral("properties")).toObject();
    const QJsonValue config = props.value(QStringLiteral("parserConfig"));
    if (!cards.contains(item.value(QStringLiteral("type")).toString()) || config.isUndefined()) {
        return true;
    }
    const QString id = item.value(QStringLiteral("id")).toString();
    if (!config.isObject()) {
        return fail(error, QCoreApplication::translate("fhmi", "卡片 %1 的 parserConfig 必须是对象").arg(id));
    }
    QString mode = props.value(QStringLiteral("defaultParser")).toString(QStringLiteral("项目设置")); // i18n: value
    if (mode == QStringLiteral("项目设置")) {                                                         // i18n: value
        mode = projectParser;
    }
    QString parserError;
    if (!createParser(mode, config.toObject(), &parserError)) {
        return fail(error, QCoreApplication::translate("fhmi", "卡片 %1: %2").arg(id, parserError));
    }
    return true;
}

// A Modbus binding names a register table (no more 响应内序号) and a collection period; so does a chart's
// Modbus curve, by its table. Other parse modes ignore these keys.
bool validateModbus(const QJsonObject &item, QString *error)
{
    const QString id = item.value(QStringLiteral("id")).toString();
    const QStringList areas = ModbusArea::options();
    const auto badArea = [&areas](const QJsonValue &area) {
        return !area.isUndefined() && !area.isNull() && !areas.contains(area.toString());
    };
    const QString onlyAreas = areas.join(QCoreApplication::translate("fhmi", "、"));
    const QJsonObject binding = item.value(QStringLiteral("binding")).toObject();
    if (binding.value(QStringLiteral("parseMode")).toString() == ParseMode::Modbus &&
        badArea(binding.value(QStringLiteral("mbArea")))) {
        return fail(error, QCoreApplication::translate("fhmi", "控件 %1 的 mbArea 只能是 %2").arg(id, onlyAreas));
    }
    const QJsonValue period = binding.value(QStringLiteral("mbPeriod"));
    if (!period.isUndefined() && !period.isNull() &&
        (!isStrictInt(period) || period.toDouble() < 0 || period.toDouble() > 60000)) {
        return fail(error, QCoreApplication::translate("fhmi", "控件 %1 的 mbPeriod 必须是 0–60000 的整数").arg(id));
    }
    if (item.value(QStringLiteral("type")).toString() == QLatin1String("RealtimeChart")) {
        const QJsonObject props = item.value(QStringLiteral("properties")).toObject();
        for (int curve = 0; curve < 8; ++curve) {
            const QString prefix = QStringLiteral("_curve_%1_").arg(curve);
            const QString area = prefix + QStringLiteral("mbArea");
            if (props.value(prefix + QStringLiteral("parseMode")).toString() == ParseMode::Modbus &&
                badArea(props.value(area))) {
                return fail(error,
                            QCoreApplication::translate("fhmi", "控件 %1 的 %2 只能是 %3").arg(id, area, onlyAreas));
            }
        }
    }
    return true;
}

bool validateWidgets(const QJsonValue &items, int depth, QSet<QString> &ids, const QString &projectParser,
                     QString *error)
{
    if (!items.isArray() || depth > 64) {
        return fail(error, QCoreApplication::translate("fhmi", "无效控件树"));
    }
    for (const QJsonValue &value : items.toArray()) {
        if (!value.isObject() || !value.toObject().value(QStringLiteral("type")).isString()) {
            return fail(error, QCoreApplication::translate("fhmi", "控件缺少有效类型"));
        }
        const QJsonObject item = value.toObject();
        const QString type = item.value(QStringLiteral("type")).toString();
        if (!WidgetRegistry::instance().find(type)) {
            return fail(error, QCoreApplication::translate("fhmi", "未知控件类型: %1").arg(type));
        }
        const QJsonValue id = item.value(QStringLiteral("id"));
        if (!id.isUndefined() && !id.isNull()) {
            if (!id.isString() || id.toString().isEmpty() || ids.contains(id.toString())) {
                return fail(error, QCoreApplication::translate("fhmi", "控件 ID 无效或重复"));
            }
            ids.insert(id.toString());
        }
        const QJsonValue props = item.value(QStringLiteral("properties"));
        if (!props.isUndefined() && !props.isObject()) {
            return fail(error, QCoreApplication::translate("fhmi", "控件属性必须是对象"));
        }
        if(type==QLatin1String("RotaryEncoder")) {
            const auto ppr=props.toObject().value(QStringLiteral("pulsesPerTurn"));
            if(!ppr.isUndefined()&&(!isStrictInt(ppr)||ppr.toDouble()<1||ppr.toDouble()>9007199254740991.0))
                return fail(error,QCoreApplication::translate("fhmi","每圈脉冲必须是正的安全整数"));
        }
        if (!validateCardParser(item, projectParser, error)) {
            return false;
        }
        if (!validateModbus(item, error)) {
            return false;
        }
        const QJsonValue binding = item.value(QStringLiteral("binding"));
        if (!binding.isUndefined() && !binding.isNull() && !binding.isObject()) {
            return fail(error, QCoreApplication::translate("fhmi", "控件绑定必须是对象"));
        }
        const QJsonValue geometry = item.value(QStringLiteral("geometry"));
        if (!geometry.isUndefined() && !geometry.isObject()) {
            return fail(error, QCoreApplication::translate("fhmi", "控件位置必须是对象"));
        }
        const QJsonObject geo = geometry.toObject();
        for (const char *key : {"x", "y", "w", "h"}) {
            const QJsonValue v = geo.value(QLatin1String(key));
            if (v.isUndefined()) {
                continue;
            }
            if (!v.isDouble() || !std::isfinite(v.toDouble())) {
                return fail(error, QCoreApplication::translate("fhmi", "控件位置必须是有限数值"));
            }
            if ((key[0] == 'w' || key[0] == 'h') && v.toDouble() <= 0) {
                return fail(error, QCoreApplication::translate("fhmi", "控件尺寸必须大于零"));
            }
        }
        const QJsonValue tab = item.value(QStringLiteral("tabIndex"));
        if (!tab.isUndefined() && (!isStrictInt(tab) || tab.toDouble() < 0)) {
            return fail(error, QCoreApplication::translate("fhmi", "无效选项卡索引"));
        }
        const QJsonValue children = item.value(QStringLiteral("children"));
        if (!children.isUndefined() && !validateWidgets(children, depth + 1, ids, projectParser, error)) {
            return false;
        }
    }
    return true;
}

bool usesV13Features(const QJsonArray &widgets)
{
    bool used = false;
    visitWidgets(widgets, [&](const QJsonObject &widget) {
        const auto props = widget.value(QStringLiteral("properties")).toObject();
        used |= widget.value(QStringLiteral("type")).toString().startsWith(QLatin1String("Alarm")) ||
                widget.value(QStringLiteral("binding")).toObject().value(QStringLiteral("parseMode")).toString() ==
                    ParseMode::DataPoint ||
                props.value(QStringLiteral("displayFormat")).toString() == QStringLiteral("枚举") || // i18n: value
                (widget.value(QStringLiteral("type")).toString() == QLatin1String("SpinBox") &&
                 props.value(QStringLiteral("numberMode")).toString() == QStringLiteral("小数")) || // i18n: value
                props.value(QStringLiteral("chargeColors")).toBool() ||
                (props.value(QStringLiteral("autoSave")).toBool() && props.contains(QStringLiteral("autoSaveRows"))) ||
                props.value(QStringLiteral("autoRecover")).toBool();
        for (int i = 0; i < 8; ++i)
            used |= props.value(QStringLiteral("_curve_%1_parseMode").arg(i)).toString() == ParseMode::DataPoint;
        return true;
    });
    return used;
}

bool hasNewCanConfiguration(const QJsonArray &widgets)
{
    return !visitWidgets(widgets, [](const QJsonObject &widget) {
        if (widget.value(QStringLiteral("type")).toString() != QLatin1String("CanConfigCard"))
            return true;
        const auto props = widget.value(QStringLiteral("properties")).toObject();
        const QString sample = props.value(QStringLiteral("zlgSamplePoint")).toString();
        return props.value(QStringLiteral("canTxInitialState")).toString().trimmed().isEmpty() &&
               props.value(QStringLiteral("zlgCustomTiming")).toString().trimmed().isEmpty() &&
               (sample.isEmpty() || sample == QStringLiteral("设备标准")); // i18n: value
    });
}

// Before UIDP 1.2 the card IDs below belonged to S7 only, even if the old editor
// kept the fields after a mode change. Keep references to other connection types
// so a new route declared with the wrong file version is still rejected.
QJsonArray migrateOldS7Fields(const QJsonArray &widgets, const QSet<QString> &otherCards)
{
    auto migrated = widgets;
    const auto removeUnusedS7 = [&otherCards](QJsonObject &object, const QString &modeKey, const QString &idKey) {
        if (object.value(modeKey).toString() != QLatin1String("S7") &&
            !otherCards.contains(object.value(idKey).toString()))
            object.remove(idKey);
    };
    for (qsizetype i = 0; i < widgets.size(); ++i) {
        auto widget = widgets.at(i).toObject();
        const QString type = widget.value(QStringLiteral("type")).toString();
        auto props = widget.value(QStringLiteral("properties")).toObject();
        if (widget.value(QStringLiteral("binding")).isObject()) {
            auto binding = widget.value(QStringLiteral("binding")).toObject();
            removeUnusedS7(binding, QStringLiteral("parseMode"), QStringLiteral("sourceCardId"));
            widget.insert(QStringLiteral("binding"), binding);
        }
        removeUnusedS7(props, QStringLiteral("commandMode"), QStringLiteral("commandTargetId"));
        if (type == QLatin1String("RealtimeChart")) {
            for (int curve = 0; curve < 8; ++curve) {
                const QString prefix = QStringLiteral("_curve_%1_").arg(curve);
                removeUnusedS7(props, prefix + QStringLiteral("parseMode"), prefix + QStringLiteral("sourceCardId"));
            }
        }
        if (props != widget.value(QStringLiteral("properties")).toObject())
            widget.insert(QStringLiteral("properties"), props);
        if (widget.value(QStringLiteral("children")).isArray())
            widget.insert(QStringLiteral("children"),
                          migrateOldS7Fields(widget.value(QStringLiteral("children")).toArray(), otherCards));
        migrated[i] = widget;
    }
    return migrated;
}

// Routing is a format capability: an older runtime must reject it instead of silently broadcasting.
bool usesRouting(const QJsonArray &widgets)
{
    const auto routed = [](const QJsonObject &object, const QString &mode, const QString &id, const QString &endpoint) {
        // The connection ID is already part of the S7 format. Endpoints were introduced with routing.
        return !object.value(endpoint).toString().isEmpty() ||
               (object.value(mode).toString() != QLatin1String("S7") && !object.value(id).toString().isEmpty());
    };
    return !visitWidgets(widgets, [&](const QJsonObject &widget) {
        const auto binding = widget.value(QStringLiteral("binding")).toObject();
        const auto props = widget.value(QStringLiteral("properties")).toObject();
        if (routed(binding, QStringLiteral("parseMode"), QStringLiteral("sourceCardId"), QStringLiteral("sourceEndpoint")) ||
            !props.value(QStringLiteral("sourceCardId")).toString().isEmpty() ||
            !props.value(QStringLiteral("sourceEndpoint")).toString().isEmpty() ||
            props.value(QStringLiteral("autoSave")).toBool() ||
            routed(props, QStringLiteral("commandMode"), QStringLiteral("commandTargetId"), QStringLiteral("commandEndpoint")))
            return false;
        for (int i = 0; i < 8; ++i) {
            const QString prefix = QStringLiteral("_curve_%1_").arg(i);
            if (routed(props, prefix + QStringLiteral("parseMode"), prefix + QStringLiteral("sourceCardId"),
                       prefix + QStringLiteral("sourceEndpoint")))
                return false;
        }
        return true;
    });
}

bool validateRoutes(const QJsonArray &widgets, QString *error, bool allowExternal)
{
    QHash<QString, QString> cards;
    visitWidgets(widgets, [&](const QJsonObject &widget) {
        const QString type = widget.value(QStringLiteral("type")).toString();
        if (isConnectionCard(type))
            cards.insert(widget.value(QStringLiteral("id")).toString(), type);
        return true;
    });
    const auto check = [&](const QJsonObject &object, const QString &idKey, const QString &endpointKey,
                           RouteTransport transport) {
        for (const auto &key : {idKey, endpointKey}) {
            const auto value = object.value(key);
            if (!value.isUndefined() && !value.isString())
                return fail(error, QCoreApplication::translate("fhmi", "连接和子通道必须是文本"));
        }
        const bool hasEndpoint = !object.value(endpointKey).toString().isEmpty();
        if (hasEndpoint && !allowsRouteEndpoint(transport))
            return fail(error, QCoreApplication::translate("fhmi", "当前模式不支持子通道"));
        const QString id = object.value(idKey).toString();
        if (id.isEmpty() || (allowExternal && !cards.contains(id)))
            return true;
        const QString type = cards.value(id);
        if (type.isEmpty())
            return fail(error, QCoreApplication::translate("fhmi", "连接引用不存在：%1").arg(id));
        if (!supportsRouteTransport(type, transport) || (hasEndpoint && !supportsRouteEndpoint(type)))
            return fail(error, QCoreApplication::translate("fhmi", "连接 %1 与收发模式不匹配").arg(id));
        return true;
    };
    return visitWidgets(widgets, [&](const QJsonObject &widget) {
        const auto binding = widget.value(QStringLiteral("binding")).toObject();
        const auto props = widget.value(QStringLiteral("properties")).toObject();
        const QString type = widget.value(QStringLiteral("type")).toString();
        const QString receiveMode = binding.value(QStringLiteral("parseMode")).toString();
        if (receiveMode != ParseMode::DataPoint &&
            !check(binding, QStringLiteral("sourceCardId"), QStringLiteral("sourceEndpoint"),
                   bindingRouteTransport(receiveMode)))
            return false;
        if (!check(props, QStringLiteral("sourceCardId"), QStringLiteral("sourceEndpoint"), widgetReceiveTransport(type)))
            return false;
        const QString sendMode = props.value(QStringLiteral("commandMode")).toString();
        if (!check(props, QStringLiteral("commandTargetId"), QStringLiteral("commandEndpoint"),
                   commandRouteTransport(sendMode, type)))
            return false;
        if (type == QLatin1String("RealtimeChart")) {
            for (int i = 0; i < 8; ++i) {
                const QString prefix = QStringLiteral("_curve_%1_").arg(i);
                const QString mode = props.value(prefix + QStringLiteral("parseMode")).toString();
                if (mode != ParseMode::DataPoint &&
                    !check(props, prefix + QStringLiteral("sourceCardId"), prefix + QStringLiteral("sourceEndpoint"),
                           bindingRouteTransport(mode)))
                    return false;
            }
        }
        return true;
    });
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

QString Project::formatVersion(const QJsonArray &widgets, const QJsonObject &logic)
{
    if (!logic.isEmpty() || usesV13Features(widgets) || hasNewCanConfiguration(widgets))
        return QStringLiteral("1.3");
    return usesRouting(widgets) ? QStringLiteral("1.2") : S7::usesS7(widgets) ? QStringLiteral("1.1") : QStringLiteral("1.0");
}

QJsonObject Project::toJson() const
{
    QJsonObject document{
        {QStringLiteral("version"), formatVersion(widgets, logic)},
        {QStringLiteral("appName"), appName},
        {QStringLiteral("canvas"), QJsonObject{{QStringLiteral("width"), canvasWidth},
                                               {QStringLiteral("height"), canvasHeight},
                                               {QStringLiteral("gridSize"), gridSize}}},
        {QStringLiteral("widgets"), widgets},
        {QStringLiteral("serial"), serial},
    };
    if (!logic.isEmpty())
        document.insert(QStringLiteral("logic"), logic);
    if (!bitProfiles.isEmpty()) document.insert(QStringLiteral("bitProfiles"), bitProfiles);
    if (!icon.isEmpty()) {
        document.insert(QStringLiteral("icon"), icon);
    }
    return document;
}

namespace {

bool prepareProject(QJsonObject &data, QString *error, bool allowExternalReferences)
{
    const QString version = data.value(QStringLiteral("version")).toString(QStringLiteral("1.0"));
    if (version != QLatin1String("1.0") && version != QLatin1String("1.1") && version != QLatin1String("1.2") &&
        version != QLatin1String("1.3")) {
        return fail(error, QCoreApplication::translate("fhmi", "不支持的项目版本"));
    }
    const QJsonValue appName = data.value(QStringLiteral("appName"));
    if (!appName.isUndefined() && !appName.isString()) {
        return fail(error, QCoreApplication::translate("fhmi", "应用名称必须是文本"));
    }
    const auto profiles = data.value(QStringLiteral("bitProfiles"));
    if (!profiles.isUndefined() && !profiles.isObject()) return fail(error, QCoreApplication::translate("fhmi", "bitProfiles 必须是对象"));
    const auto problem = bitProfilesProblem(profiles.toObject());
    if (!problem.isEmpty()) return fail(error, problem);
    const QJsonValue canvasValue = data.value(QStringLiteral("canvas"));
    if (!canvasValue.isUndefined() && !canvasValue.isObject()) {
        return fail(error, QCoreApplication::translate("fhmi", "画布配置必须是对象"));
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
            return fail(error, QCoreApplication::translate("fhmi", "无效画布参数: %1").arg(QLatin1String(limit.name)));
        }
    }
    const QJsonValue serialValue = data.value(QStringLiteral("serial"));
    if (!serialValue.isUndefined() && !serialValue.isObject()) {
        return fail(error, QCoreApplication::translate("fhmi", "串口配置必须是对象"));
    }
    const QJsonObject serialObject = serialValue.toObject();
    const QString parser = serialObject.value(QStringLiteral("parser")).toString(QStringLiteral("line"));
    QString parserError;
    if (!createParser(parser, serialObject.value(QStringLiteral("parserConfig")).toObject(), &parserError)) {
        return fail(error, parserError);
    }
    QSet<QString> ids;
    const QJsonValue widgetList = data.value(QStringLiteral("widgets"));
    if (!widgetList.isUndefined() && !validateWidgets(widgetList, 0, ids, parser, error))
        return false;
    const bool legacy = version == QLatin1String("1.0") || version == QLatin1String("1.1");
    QSet<QString> otherCards;
    if (legacy) {
        visitWidgets(widgetList.toArray(), [&](const QJsonObject &widget) {
            const QString type = widget.value(QStringLiteral("type")).toString();
            if (isConnectionCard(type) && type != QLatin1String("S7ConfigCard"))
                otherCards.insert(widget.value(QStringLiteral("id")).toString());
            return true;
        });
    }
    const auto widgets = legacy ? migrateOldS7Fields(widgetList.toArray(), otherCards) : widgetList.toArray();
    if (S7::usesS7(widgets) && version == QLatin1String("1.0"))
        return fail(error, QCoreApplication::translate("fhmi", "S7 工程需要使用 1.1 格式"));
    if (usesRouting(widgets) && legacy)
        return fail(error, QCoreApplication::translate("fhmi", "多通道配置需要使用 1.2 格式"));
    const auto logic = data.value(QStringLiteral("logic"));
    if (!logic.isUndefined() && !logic.isObject())
        return fail(error, QCoreApplication::translate("fhmi", "工程数据与报警配置必须是对象"));
    if ((!logic.toObject().isEmpty() || usesV13Features(widgets)) && version != QLatin1String("1.3"))
        return fail(error, QCoreApplication::translate("fhmi", "工程数据、报警、枚举显示或小数输入需要使用 1.3 格式"));
    if (version != QLatin1String("1.3") && hasNewCanConfiguration(widgets))
        return fail(error, QCoreApplication::translate("fhmi", "CAN 发送初值或 ZLG 自定义时序需要使用 1.3 格式"));
    const auto logicProblem = projectLogicProblem(logic.toObject());
    if (!logicProblem.isEmpty())
        return fail(error, logicProblem);
    QJsonArray withPoints = widgets;
    QSet<QString> pointNames;
    for (const auto &p : logic.toObject().value(QStringLiteral("points")).toArray()) {
        const auto point = p.toObject();
        pointNames.insert(point.value(QStringLiteral("name")).toString());
        if (point.value(QStringLiteral("expression")).toString().trimmed().isEmpty())
            withPoints.append(QJsonObject{
                {QStringLiteral("id"), QStringLiteral("@data:") + point.value(QStringLiteral("name")).toString()},
                {QStringLiteral("type"), QStringLiteral("NumericDisplay")},
                {QStringLiteral("binding"), point.value(QStringLiteral("binding"))}});
    }
    if (!visitWidgets(widgets, [&](const QJsonObject &widget) {
            const auto props = widget.value(QStringLiteral("properties")).toObject();
            if (props.value(QStringLiteral("displayFormat")).toString() == QStringLiteral("枚举")) { // i18n: value
                QString problem;
                parseValueText(props.value(QStringLiteral("textList")).toString(), &problem);
                if (!problem.isEmpty())
                    return fail(error, problem);
            }
            if (props.value(QStringLiteral("chargeColors")).toBool() &&
                !(props.value(QStringLiteral("chargeLow")).toDouble(20) >= 0 &&
                  props.value(QStringLiteral("chargeWarning")).toDouble(40) <= 100 &&
                  props.value(QStringLiteral("chargeLow")).toDouble(20) <=
                      props.value(QStringLiteral("chargeWarning")).toDouble(40)))
                return fail(error, QCoreApplication::translate("fhmi", "电量阈值必须满足 0 ≤ 低电量 ≤ 预警 ≤ 100"));
            const QString widgetId = widget.value(QStringLiteral("id")).toString();
            const auto checkPoint = [&](const QJsonObject &binding, int curve = -1) {
                const QString key = binding.value(QStringLiteral("key")).toString();
                if (binding.value(QStringLiteral("parseMode")).toString() != ParseMode::DataPoint || key.isEmpty() ||
                    pointNames.contains(key) || allowExternalReferences)
                    return true;
                const auto location = curve < 0 ? widgetId : QCoreApplication::translate("fhmi", "%1 的曲线 %2").arg(widgetId).arg(curve + 1);
                return fail(error, QCoreApplication::translate("fhmi", "%1 绑定的数据点不存在：%2").arg(location, key));
            };
            if (!checkPoint(widget.value(QStringLiteral("binding")).toObject()))
                return false;
            if (widget.value(QStringLiteral("type")).toString() == QLatin1String("RealtimeChart"))
                for (int i = 0; i < 8; ++i)
                    if (!checkPoint(chartCurveBinding(props, i), i))
                        return false;
            return true;
        }))
        return false;
    if (!validateRoutes(withPoints, error, allowExternalReferences) ||
        !S7::validateWidgets(withPoints, error, allowExternalReferences))
        return false;
    if (widgets != widgetList.toArray())
        data.insert(QStringLiteral("widgets"), widgets);
    return true;
}
} // namespace

bool Project::validate(const QJsonObject &document, QString *error, bool allowExternalReferences)
{
    auto data = document;
    return prepareProject(data, error, allowExternalReferences);
}

bool Project::fromJson(const QJsonObject &document, Project *out, QString *error, bool allowExternalReferences)
{
    auto data = document;
    if (!prepareProject(data, error, allowExternalReferences))
        return false;
    *out = fromTrustedJson(data);
    return true;
}

Project Project::fromTrustedJson(const QJsonObject &data)
{
    Project project;
    project.appName = data.value(QStringLiteral("appName")).toString(QStringLiteral("上位机")); // i18n: value
    project.icon = data.value(QStringLiteral("icon")).toString();
    const QJsonObject canvas = data.value(QStringLiteral("canvas")).toObject();
    project.canvasWidth = canvas.value(QStringLiteral("width")).toInt(1200);
    project.canvasHeight = canvas.value(QStringLiteral("height")).toInt(800);
    project.gridSize = canvas.value(QStringLiteral("gridSize")).toInt(10);
    project.widgets = data.value(QStringLiteral("widgets")).toArray();
    project.logic = data.value(QStringLiteral("logic")).toObject();
    project.bitProfiles = data.value(QStringLiteral("bitProfiles")).toObject();
    const QJsonObject serialObject = data.value(QStringLiteral("serial")).toObject();
    for (auto it = serialObject.begin(); it != serialObject.end(); ++it) {
        project.serial.insert(it.key(), it.value());
    }
    return project;
}

bool Project::load(const QString &path, Project *out, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return fail(error, QCoreApplication::translate("fhmi", "无法读取 %1: %2")
                               .arg(QDir::toNativeSeparators(path), file.errorString()));
    }
    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        return fail(error, QCoreApplication::translate("fhmi", "JSON 格式错误: %1").arg(parseError.errorString()));
    }
    if (!doc.isObject()) {
        return fail(error, QCoreApplication::translate("fhmi", "项目根节点必须是对象"));
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
        return fail(error, QCoreApplication::translate("fhmi", "无法写入 %1: %2")
                               .arg(QDir::toNativeSeparators(path), file.errorString()));
    }
    file.write(QJsonDocument(document).toJson(QJsonDocument::Indented));
    if (!file.commit()) {
        return fail(error, QCoreApplication::translate("fhmi", "保存失败: %1").arg(file.errorString()));
    }
    return true;
}

QString Project::assetBaseDir() const
{
    return filePath.isEmpty() ? QDir::currentPath() : QFileInfo(filePath).absolutePath();
}

QString Project::iconPath() const
{
    if (icon.isEmpty()) {
        return QString();
    }
    return QDir::isRelativePath(icon) ? QDir(assetBaseDir()).absoluteFilePath(icon) : icon;
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
