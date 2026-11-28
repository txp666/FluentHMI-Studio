#include "core/project.h"
#include "core/bit_profile.h"
#include "core/project_logic.h"
#include "core/s7.h"
#include "core/value_text.h"

#include "core/binding.h"
#include "core/chart_limits.h"
#include "core/command_encoder.h"
#include "core/data_source.h"
#include "core/frame_templates.h"
#include "core/manual_command.h"
#include "core/protocol_parser.h"
#include "core/widget_meta.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QSaveFile>
#include <QtCore/QSet>

#include <algorithm>
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
bool validateCardParser(const QJsonObject &item, const Project &project, QString *error)
{
    const QJsonObject props = item.value(QStringLiteral("properties")).toObject();
    const auto group = props.value(FrameTemplates::ReceiveIds);
    const bool grouped = !group.isUndefined() && group != QJsonValue(QJsonArray{});
    const bool bytes = supportsRouteTransport(item.value(QStringLiteral("type")).toString(), RouteTransport::Bytes);
    if (!bytes)
        return !grouped || fail(error, QCoreApplication::translate("fhmi", "只有字节连接卡可以选择接收模板组"));
    const QJsonValue config = props.value(QStringLiteral("parserConfig"));
    const QString id = item.value(QStringLiteral("id")).toString();
    if (!config.isUndefined() && !config.isObject()) {
        return fail(error, QCoreApplication::translate("fhmi", "卡片 %1 的 parserConfig 必须是对象").arg(id));
    }
    if (grouped) {
        if (!props.value(FrameTemplates::ReceiveId).toString().isEmpty())
            return fail(error, QCoreApplication::translate("fhmi", "卡片 %1 同时设置了单个接收模板和接收模板组").arg(id));
        const auto problem = FrameTemplates::receiveGroupProblem(project.frameTemplates, group);
        if (!problem.isEmpty())
            return fail(error, QCoreApplication::translate("fhmi", "卡片 %1: %2").arg(id, problem));
        return true;
    }
    const auto format = project.linkReceiveFormat(props);
    QString parserError;
    if (!createParser(format.type, format.settings, &parserError)) {
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
    const auto badPeriod = [](const QJsonValue &period) {
        return !period.isUndefined() && !period.isNull() &&
               (!isStrictInt(period) || period.toDouble() < 0 || period.toDouble() > 60000);
    };
    const auto badBit = [](const QJsonValue &bit, int minimum) {
        return !bit.isUndefined() && !bit.isNull() &&
               (!isStrictInt(bit) || bit.toDouble() < minimum || bit.toDouble() > 15);
    };
    const QString onlyAreas = areas.join(QCoreApplication::translate("fhmi", "、"));
    const QJsonObject binding = item.value(QStringLiteral("binding")).toObject();
    if (binding.value(QStringLiteral("parseMode")).toString() == ParseMode::Modbus) {
        if (badArea(binding.value(QStringLiteral("mbArea"))))
            return fail(error, QCoreApplication::translate("fhmi", "控件 %1 的 mbArea 只能是 %2").arg(id, onlyAreas));
        if (badPeriod(binding.value(QStringLiteral("mbPeriod"))))
            return fail(error, QCoreApplication::translate("fhmi", "控件 %1 的 mbPeriod 必须是 0–60000 的整数").arg(id));
        if (badBit(binding.value(QStringLiteral("mbBit")), CommandEncoder::WholeRegister))
            return fail(error, QCoreApplication::translate("fhmi", "控件 %1 的 mbBit 必须是 -1–15 的整数").arg(id));
    }
    const QString type = item.value(QStringLiteral("type")).toString();
    const QJsonObject props = item.value(QStringLiteral("properties")).toObject();
    if (props.value(QStringLiteral("commandMode")).toString() == QLatin1String("Modbus")) {
        if (type == QLatin1String("ToggleSwitch")) {
            if (props.value(QStringLiteral("mbArea")).toString() == ModbusArea::HoldingRegisterBit &&
                badBit(props.value(QStringLiteral("mbBit")), 0))
                return fail(error, QCoreApplication::translate("fhmi", "控件 %1 的 mbBit 必须是 0–15 的整数").arg(id));
        } else if (badBit(props.value(QStringLiteral("mbBit")), CommandEncoder::WholeRegister)) {
            return fail(error, QCoreApplication::translate("fhmi", "控件 %1 的 mbBit 必须是 -1–15 的整数").arg(id));
        }
        const auto action = props.value(QStringLiteral("mbBitAction"));
        const QStringList actions = CommandEncoder::bitActions();
        if (!action.isUndefined() && !action.isNull() && !actions.contains(action.toString())) {
            return fail(error, QCoreApplication::translate("fhmi", "控件 %1 的 mbBitAction 只能是 %2")
                                   .arg(id, actions.join(QCoreApplication::translate("fhmi", "、"))));
        }
    }
    if (usesChartSeries(type, props)) {
        const int count = chartCurveCount(type, props);
        for (int curve = 0; curve < count; ++curve) {
            const QString prefix = QStringLiteral("_curve_%1_").arg(curve);
            const QString area = prefix + QStringLiteral("mbArea");
            if (props.value(prefix + QStringLiteral("parseMode")).toString() != ParseMode::Modbus)
                continue;
            if (badArea(props.value(area))) {
                return fail(error,
                            QCoreApplication::translate("fhmi", "控件 %1 的 %2 只能是 %3").arg(id, area, onlyAreas));
            }
            if (badPeriod(props.value(prefix + QStringLiteral("mbPeriod"))))
                return fail(error, QCoreApplication::translate("fhmi", "控件 %1 的 mbPeriod 必须是 0–60000 的整数").arg(id));
            if (badBit(props.value(prefix + QStringLiteral("mbBit")), CommandEncoder::WholeRegister))
                return fail(error, QCoreApplication::translate("fhmi", "控件 %1 的 mbBit 必须是 -1–15 的整数").arg(id));
        }
    }
    return true;
}

bool validateTemplateReference(const QJsonValue &value, const QJsonObject &library, const QString &owner, QString *error)
{
    if (value.isUndefined() || (value.isString() && (value.toString().isEmpty() || library.contains(value.toString()))))
        return true;
    return fail(error, QCoreApplication::translate("fhmi", "%1 引用的帧格式模板不存在：%2").arg(owner, value.toString()));
}

bool validateWidgets(const QJsonValue &items, int depth, QSet<QString> &ids, const Project &project,
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
            if (!id.isString() || id.toString().isEmpty() || id.toString() == DataRoute::Broadcast ||
                ids.contains(id.toString())) {
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
        const auto properties = props.toObject();
        for (const auto &key : {FrameTemplates::ReceiveId, FrameTemplates::SendId})
            if (!validateTemplateReference(properties.value(key), project.frameTemplates, id.toString(), error))
                return false;
        const auto sequence = properties.value(FrameTemplates::SendSequence);
        if (!sequence.isUndefined()) {
            if (type != QLatin1String("PushButton") || !sequence.isArray())
                return fail(error, QCoreApplication::translate("fhmi", "多模板混发只能配置在按钮上"));
            for (const auto &value : sequence.toArray()) {
                const auto frame = value.toObject();
                const auto entry = FrameTemplates::find(project.frameTemplates, frame.value(FrameTemplates::SendId).toString());
                if (!entry || entry->format.type != QLatin1String("hex"))
                    return fail(error, QCoreApplication::translate("fhmi", "%1 的混发序列引用了无效的 HEX 模板").arg(id.toString()));
                if (frame.contains(FrameTemplates::SendBindings) && !frame.value(FrameTemplates::SendBindings).isArray())
                    return fail(error, QCoreApplication::translate("fhmi", "混发模板的字段配置必须是数组"));
            }
        }
        if (!validateCardParser(item, project, error)) {
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
        if (!children.isUndefined() && !validateWidgets(children, depth + 1, ids, project, error)) {
            return false;
        }
    }
    return true;
}

bool usesModbusBitWrite(const QString &type, const QJsonObject &props)
{
    if (props.value(QStringLiteral("commandMode")).toString() != QLatin1String("Modbus"))
        return false;
    if (type == QLatin1String("ToggleSwitch"))
        return props.value(QStringLiteral("mbArea")).toString() == ModbusArea::HoldingRegisterBit;
    return CommandEncoder::functionCode(props.value(QStringLiteral("mbFunc")).toString()) == 0x06 &&
           CommandEncoder::modbusBit(props) != CommandEncoder::WholeRegister;
}

bool usesV13Features(const QJsonArray &widgets)
{
    const auto versionedBinding = [](const QJsonObject &binding) {
        const QString mode = binding.value(QStringLiteral("parseMode")).toString();
        return mode == ParseMode::DataPoint || mode == ParseMode::CanField ||
               (mode == ParseMode::Modbus &&
                binding.value(QStringLiteral("mbBit")).toInt(CommandEncoder::WholeRegister) >= 0);
    };
    bool used = false;
    visitWidgets(widgets, [&](const QJsonObject &widget) {
        const auto props = widget.value(QStringLiteral("properties")).toObject();
        if (!FrameTemplates::receiveIds(props).isEmpty() ||
            !FrameTemplates::sendIds(props).isEmpty() ||
            props.value(FrameTemplates::SendFieldsEnabled).toBool() ||
            !props.value(FrameTemplates::SendBindings).toArray().isEmpty() ||
            !props.value(FrameTemplates::SendValueField).toString().isEmpty()) {
            used = true;
            return false;
        }
        const QString type = widget.value(QStringLiteral("type")).toString();
        used |= type.startsWith(QLatin1String("Alarm")) ||
                (isTimingChart(type) && TimingSource::usesChannels(props)) ||
                versionedBinding(widget.value(QStringLiteral("binding")).toObject()) ||
                usesModbusBitWrite(type, props) ||
                props.value(QStringLiteral("displayFormat")).toString() == QStringLiteral("枚举") || // i18n: value
                (type == QLatin1String("SpinBox") &&
                 props.value(QStringLiteral("numberMode")).toString() == QStringLiteral("小数")) || // i18n: value
                (type == QLatin1String("LineEdit") &&
                 (props.value(QStringLiteral("inputOnly")).toBool() ||
                  !props.value(QStringLiteral("title")).toString().trimmed().isEmpty())) ||
                (type == QLatin1String("PushButton") &&
                 (!props.value(QStringLiteral("commandInputs")).toArray().isEmpty() ||
                  !props.value(QStringLiteral("mbWriteInputs")).toArray().isEmpty() ||
                  !props.value(QStringLiteral("s7InputId")).toString().isEmpty() ||
                  !props.value(QStringLiteral("mbInputId")).toString().isEmpty())) ||
                !props.value(QStringLiteral("txFrame")).toObject().isEmpty() ||
                (type == QLatin1String("DataSendCard") && ManualCommand::hasFraming(props)) ||
                CommandEncoder::periodicSendInterval(props) > 0 ||
                props.value(QStringLiteral("chargeColors")).toBool() ||
                (props.value(QStringLiteral("autoSave")).toBool() && props.contains(QStringLiteral("autoSaveRows"))) ||
                props.value(QStringLiteral("confirmBeforeSend")).toBool() ||
                props.value(QStringLiteral("autoRecover")).toBool();
        const int limit = isTimingChart(type) ? MaxTimingChannels : type == QLatin1String("RealtimeChart") ? MaxRealtimeCurves : 0;
        for (int i = 0; i < limit; ++i)
            used |= versionedBinding(chartCurveBinding(props, i));
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

bool usesStructuredRows(const QJsonArray &widgets)
{
    return !visitWidgets(widgets, [](const QJsonObject &widget) {
        const QString type = widget.value(QStringLiteral("type")).toString();
        const auto props = widget.value(QStringLiteral("properties")).toObject();
        return !(type == QLatin1String("ComboBox") && props.value(QStringLiteral("options")).isArray()) &&
               !(type == QLatin1String("DataTable") && props.value(QStringLiteral("recordColumns")).isArray());
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
        if (type == QLatin1String("RealtimeChart") || isTimingChart(type)) {
            const int limit = isTimingChart(type) ? MaxTimingChannels : MaxRealtimeCurves;
            for (int curve = 0; curve < limit; ++curve) {
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
        const auto type = widget.value(QStringLiteral("type")).toString();
        const int limit = isTimingChart(type) ? MaxTimingChannels : type == QLatin1String("RealtimeChart") ? MaxRealtimeCurves : 0;
        for (int i = 0; i < limit; ++i) {
            const QString prefix = QStringLiteral("_curve_%1_").arg(i);
            if (routed(props, prefix + QStringLiteral("parseMode"), prefix + QStringLiteral("sourceCardId"),
                       prefix + QStringLiteral("sourceEndpoint")))
                return false;
        }
        return true;
    });
}

bool routeCardSupports(const QJsonObject &widget, const RouteFilter &filter, bool needsEndpoint,
                       const QString &projectParser)
{
    const QString type = widget.value(QStringLiteral("type")).toString();
    const auto transport = filter.transport;
    if (!supportsRouteTransport(type, transport) || (needsEndpoint && !supportsRouteEndpoint(type)))
        return false;
    if (transport != RouteTransport::ModbusSlave && transport != RouteTransport::Modbus)
        return true;
    const auto props = widget.value(QStringLiteral("properties")).toObject();
    const QString parser = LinkParser::effectiveType(props.value(QStringLiteral("defaultParser")).toString(), projectParser);
    if (!FrameTemplates::receiveIds(props).isEmpty() || !isModbusParser(parser))
        return false;
    const bool local = supportsRouteTransport(type, RouteTransport::ModbusSlave) &&
        LinkParser::supportsModbusSlave(parser) &&
        props.value(QStringLiteral("modbusRole")).toString() == QStringLiteral("从站"); // i18n: value
    if (transport == RouteTransport::ModbusSlave)
        return local;
    const int slave = std::clamp(props.value(QStringLiteral("modbusSlave")).toInt(1), 1, 247);
    return !local || (!needsEndpoint && (!filter.modbus || filter.modbus->acceptsLocalSlave(slave)));
}

bool validateRoutes(const QJsonArray &widgets, const QString &projectParser, const QJsonObject &logic,
                    QString *error, bool allowExternal, bool checkSendTargets)
{
    QHash<QString, QJsonObject> cards;
    QSet<QString> inputs;
    const auto projectPoints = dataPointNames(logic);
    const auto parameters = dataPointNames(logic, true);
    visitWidgets(widgets, [&](const QJsonObject &widget) {
        const QString type = widget.value(QStringLiteral("type")).toString();
        if (isConnectionCard(type))
            cards.insert(widget.value(QStringLiteral("id")).toString(), widget);
        if (type == QLatin1String("LineEdit"))
            inputs.insert(widget.value(QStringLiteral("id")).toString());
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
        if (id == DataRoute::Broadcast && transport != RouteTransport::S7 && transport != RouteTransport::ModbusSlave)
            return true;
        if (id.isEmpty() || (allowExternal && !cards.contains(id)))
            return true;
        const auto card = cards.constFind(id);
        if (card == cards.constEnd())
            return fail(error, QCoreApplication::translate("fhmi", "连接引用不存在：%1").arg(id));
        const auto allowed = transport == RouteTransport::Modbus ? RouteTransport::Bytes : transport;
        if (!routeCardSupports(*card, allowed, hasEndpoint, projectParser)) {
            if (transport == RouteTransport::Modbus)
                return fail(error, QCoreApplication::translate("fhmi", "连接 %1 未启用 Modbus 解析").arg(id));
            return fail(error, QCoreApplication::translate("fhmi", "连接 %1 与收发模式不匹配").arg(id));
        }
        return true;
    };
    const auto checkBinding = [&](const QJsonObject &object, QStringView mode) {
        const auto transport = bindingRouteTransport(mode);
        const auto id = object.value(QStringLiteral("sourceCardId")).toString();
        const auto endpoint = object.value(QStringLiteral("sourceEndpoint")).toString();
        if (!check(object, QStringLiteral("sourceCardId"), QStringLiteral("sourceEndpoint"), transport))
            return false;
        return transport != RouteTransport::Modbus || allowExternal || !checkSendTargets ||
               resolveProjectRoute(widgets, projectParser, transport, {id, endpoint}, RoutePurpose::Binding, error,
                                   ModbusParserChoice::OnConnect).has_value();
    };
    return visitWidgets(widgets, [&](const QJsonObject &widget) {
        const auto binding = widget.value(QStringLiteral("binding")).toObject();
        const auto props = widget.value(QStringLiteral("properties")).toObject();
        const QString type = widget.value(QStringLiteral("type")).toString();
        const QString receiveMode = binding.value(QStringLiteral("parseMode")).toString();
        if (!(isTimingChart(type) && TimingSource::usesChannels(props)) &&
            receiveMode != ParseMode::DataPoint && BindingEntry::fromWidget(widget) &&
            !checkBinding(binding, receiveMode))
            return false;
        if (!check(props, QStringLiteral("sourceCardId"), QStringLiteral("sourceEndpoint"), widgetReceiveTransport(type)))
            return false;
        const QString sendMode = props.value(QStringLiteral("commandMode")).toString();
        if (widgetSendsCommands(type, props)) {
            auto command = props;
            const auto point = props.value(writePointKey(sendMode)).toString();
            const bool referenced = !point.isEmpty();
            if (referenced && checkSendTargets && !allowExternal) {
                QString problem;
                const auto resolved = resolvePointWrite(props, logic, widgets, projectParser, &problem, type,
                                                        ModbusParserChoice::OnConnect);
                if (resolved && sendMode == ParseMode::S7) {
                    const auto s7 = S7::point(resolved->value(QStringLiteral("s7Command")).toObject(), &problem);
                    if (s7)
                        problem = S7::writeValueProblem(*resolved, s7->type, type);
                }
                if (!resolved || !problem.isEmpty())
                    return fail(error, QCoreApplication::translate("fhmi", "%1 · %2").arg(
                        widget.value(QStringLiteral("id")).toString(), problem));
                command = *resolved;
            }
            const auto filter = CommandEncoder::sendRouteFilter(command, type);
            // Older manual send cards accepted a CAN destination before the data format was a property.
            const bool oldManualCard = !checkSendTargets && type == QLatin1String("DataSendCard") &&
                !props.contains(QStringLiteral("commandMode"));
            if ((!referenced || (checkSendTargets && !allowExternal)) &&
                !check(command, QStringLiteral("commandTargetId"), QStringLiteral("commandEndpoint"),
                       oldManualCard ? RouteTransport::Any : filter.transport))
                return false;
            const QString target = command.value(QStringLiteral("commandTargetId")).toString();
            if (checkSendTargets && !allowExternal && (filter.modbus || (target.isEmpty() && type == u"PushButton"))) {
                const auto authored = referenced || CommandEncoder::usesFrameFields(props) || CommandEncoder::usesFrameSequence(props) ||
                    !CommandEncoder::commandInputIds(props).isEmpty() || sendMode == QLatin1String("Modbus") ||
                    (sendMode == QLatin1String("S7") &&
                     !props.value(QStringLiteral("s7Command")).toObject().value(QStringLiteral("address")).toString().isEmpty()) ||
                    !props.value(QStringLiteral("command")).toString().trimmed().isEmpty();
                if (authored) {
                    QString problem;
                    if (!resolveProjectRoute(widgets, projectParser, filter,
                                             {target, command.value(QStringLiteral("commandEndpoint")).toString()},
                                             RoutePurpose::Send, &problem, ModbusParserChoice::OnConnect))
                        return fail(error, QCoreApplication::translate("fhmi", "%1 · %2").arg(
                            widget.value(QStringLiteral("id")).toString(), problem));
                }
            }
        }
        if (!allowExternal && type == QLatin1String("PushButton")) {
            const auto entries = CommandEncoder::commandInputIds(props);
            for (qsizetype row = 0; row < entries.size(); ++row) {
                const QString &id = entries.at(row);
                const auto point = pointNameForInput(id);
                if (checkSendTargets && isLegacyPointInput(id) && inputs.contains(id))
                    return fail(error, QCoreApplication::translate("fhmi", "%1 的发送来源 %2 有歧义，请重新选择")
                                          .arg(widget.value(QStringLiteral("id")).toString(), id));
                if (point.isEmpty() ? !inputs.contains(widgetIdForInput(id)) : !projectPoints.contains(point))
                    return fail(error, QCoreApplication::translate("fhmi", "%1 的发送配置第 %2 行来源不存在：%3")
                                          .arg(widget.value(QStringLiteral("id")).toString()).arg(row + 1)
                                          .arg(point.isEmpty() ? id : point));
            }
        }
        if (!allowExternal && type == QLatin1String("LineEdit")) {
            const auto parameter = props.value(QStringLiteral("parameterName")).toString();
            if (!parameter.isEmpty() && !parameters.contains(parameter))
                return fail(error, QCoreApplication::translate("fhmi", "工程参数不存在：%1").arg(parameter));
        }
        if (!allowExternal && type == QLatin1String("DataTable") &&
            props.value(QStringLiteral("recordSource")).toString() == QLatin1String("points")) {
            const auto trigger = props.value(QStringLiteral("recordTriggerPoint")).toString();
            if (!trigger.isEmpty() && !projectPoints.contains(trigger))
                return fail(error, QCoreApplication::translate("fhmi", "记录触发点不存在：%1").arg(trigger));
            for (const auto &entry : props.value(QStringLiteral("recordColumns")).toArray()) {
                const auto field = entry.toObject().value(QStringLiteral("field")).toString();
                if (!field.isEmpty() && field != QLatin1String("@time") && !projectPoints.contains(field))
                    return fail(error, QCoreApplication::translate("fhmi", "记录数据点不存在：%1").arg(field));
            }
        }
        if (usesChartSeries(type, props) && (checkSendTargets || !isTimingChart(type))) {
            const int count = chartCurveCount(type, props);
            for (int i = 0; i < count; ++i) {
                const auto curve = chartCurveBinding(props, i);
                const QString mode = curve.value(QStringLiteral("parseMode")).toString();
                const QJsonObject source{{QStringLiteral("type"), type}, {QStringLiteral("binding"), curve}};
                if (mode != ParseMode::DataPoint && BindingEntry::fromWidget(source) &&
                    !checkBinding(curve, mode))
                    return false;
            }
        }
        return true;
    });
}

} // namespace

int chartCurveCount(QStringView type, const QJsonObject &properties)
{
    const int limit = isTimingChart(type) ? MaxTimingChannels : MaxRealtimeCurves;
    return std::clamp(properties.value(QStringLiteral("curveCount")).toInt(1), 1, limit);
}

QList<QPair<QString, QString>> routeConnections(const QJsonArray &widgets, const RouteFilter &filter, bool needsEndpoint,
                                              const QString &projectParser)
{
    QList<QPair<QString, QString>> connections;
    visitWidgets(widgets, [&](const QJsonObject &widget) {
        const QString id = widget.value(QStringLiteral("id")).toString();
        if (!id.isEmpty() && routeCardSupports(widget, filter, needsEndpoint, projectParser))
            connections.append({id, widget.value(QStringLiteral("type")).toString()});
        return true;
    });
    return connections;
}

std::optional<DataRoute> resolveProjectRoute(const QJsonArray &widgets, const QString &projectParser,
                                             const RouteFilter &filter, DataRoute route, RoutePurpose purpose,
                                             QString *error, ModbusParserChoice parserChoice)
{
    const auto tr = [](const char *message) { return QCoreApplication::translate("fhmi", message); };
    const auto reject = [&](const QString &message) -> std::optional<DataRoute> {
        fail(error, message);
        return std::nullopt;
    };
    const auto transport = filter.transport;
    if (!route.endpoint.isEmpty() && !allowsRouteEndpoint(transport))
        return reject(tr("当前模式不支持子通道"));
    if (transport == RouteTransport::S7 && route.connectionId.isEmpty())
        return reject(tr("请选择 S7 连接"));
    auto matches = routeConnections(widgets, filter, !route.endpoint.isEmpty(), projectParser);
    const bool explicitCard = !route.connectionId.isEmpty() && !route.isBroadcast();
    if (transport == RouteTransport::Modbus && parserChoice == ModbusParserChoice::OnConnect &&
        (explicitCard || matches.isEmpty())) {
        // Existing projects can choose Modbus on the connection card when they connect.
        // Prefer the configured Modbus candidates for automatic routing.
        visitWidgets(widgets, [&](const QJsonObject &card) {
            const auto id = card.value(QStringLiteral("id")).toString();
            if (id.isEmpty() || (explicitCard && id != route.connectionId))
                return true;
            if (routeCardSupports(card, RouteTransport::Bytes, !route.endpoint.isEmpty(), projectParser) &&
                !routeCardSupports(card, RouteTransport::Modbus, false, projectParser))
                matches.append({id, card.value(QStringLiteral("type")).toString()});
            return true;
        });
    }
    if (transport == RouteTransport::Modbus && (route.connectionId.isEmpty() || route.isBroadcast()) &&
        matches.isEmpty() && !widgets.isEmpty())
        return reject(tr("请选择启用 Modbus RTU、ASCII 或 TCP 的连接"));
    if (route.isBroadcast()) {
        if (purpose == RoutePurpose::DataPoint)
            return reject(tr("工程数据点需要唯一来源，可用计算点汇总多路数据"));
        if (transport == RouteTransport::S7)
            return reject(tr("连接 %1 与收发模式不匹配").arg(route.connectionId));
        return route;
    }
    if (route.connectionId.isEmpty()) {
        if (purpose == RoutePurpose::Binding && matches.isEmpty() && !widgets.isEmpty())
            return reject(tr("没有与绑定模式匹配的连接"));
        if (matches.size() > 1)
            return reject(purpose == RoutePurpose::Send ? tr("有多个匹配的发送连接，请选择目标连接或明确启用广播")
                                                       : tr("有多个匹配的来源连接，请明确选择"));
        if (matches.size() == 1)
            route.connectionId = matches.first().first;
    } else if (!std::any_of(matches.cbegin(), matches.cend(), [&](const auto &card) { return card.first == route.connectionId; })) {
        if (widgets.isEmpty())
            return route;
        if (filter.modbus) {
            const auto local = routeConnections(widgets, RouteTransport::ModbusSlave, false, projectParser);
            if (std::any_of(local.cbegin(), local.cend(), [&](const auto &card) { return card.first == route.connectionId; }))
                return reject(tr("本机 Modbus 从站 %1 不匹配此功能码、从站地址或子通道").arg(route.connectionId));
        }
        if (transport == RouteTransport::Modbus)
            return reject(tr("连接 %1 未启用 Modbus 解析").arg(route.connectionId));
        return reject(tr("连接 %1 与收发模式不匹配").arg(route.connectionId));
    }
    return route;
}

QString modbusRouteDescription(const QJsonObject &card, const Project &project)
{
    const auto tr = [](const char *message) { return QCoreApplication::translate("fhmi", message); };
    const auto props = card.value(QStringLiteral("properties")).toObject();
    const QString parser = project.linkReceiveFormat(props).type;
    if (!isModbusParser(parser))
        return tr("连接时选择 Modbus 解析");
    if (routeCardSupports(card, RouteTransport::ModbusSlave, false, project.parserType()))
        return tr("本机 Modbus 从站表");
    if (parser == QLatin1String("modbus_tcp"))
        return QStringLiteral("Modbus TCP");
    if (parser == QLatin1String("modbus_ascii"))
        return QStringLiteral("Modbus ASCII");
    if (card.value(QStringLiteral("type")).toString() == QLatin1String("TcpConfigCard"))
        return QStringLiteral("Modbus RTU over TCP");
    return QStringLiteral("Modbus RTU");
}

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

QString Project::formatVersion(const QJsonArray &widgets, const QJsonObject &logic, const QJsonObject &frameTemplates)
{
    if (!frameTemplates.isEmpty() || !logic.isEmpty() || usesV13Features(widgets) ||
        hasNewCanConfiguration(widgets) || usesStructuredRows(widgets))
        return QString::fromLatin1(FormatVersion);
    return usesRouting(widgets) ? QStringLiteral("1.2") : S7::usesS7(widgets) ? QStringLiteral("1.1") : QStringLiteral("1.0");
}

namespace {

const auto SendConfiguration = QStringLiteral("sendConfiguration");
const auto CommandMode = QStringLiteral("commandMode");
const auto RequiresNewSender = QStringLiteral("requiresSendConfiguration");

// UIDP 1.3 readers ignore new properties. Keep the legacy sender inactive when
// the real action depends on a sequence or an inherited address it cannot read.
QJsonArray projectSendConfiguration(QJsonArray widgets, bool saving)
{
    const auto target = QStringLiteral("commandTargetId");
    const auto endpoint = QStringLiteral("commandEndpoint");
    const auto on = QStringLiteral("onCommand");
    const auto off = QStringLiteral("offCommand");
    for (qsizetype index = 0; index < widgets.size(); ++index) {
        auto widget = widgets.at(index).toObject();
        auto props = widget.value(QStringLiteral("properties")).toObject();
        const auto mode = props.value(CommandMode).toString();
        const auto type = widget.value(QStringLiteral("type")).toString();
        const auto pointKey = writePointKey(mode);
        if (saving && widgetSendsCommands(type, props) &&
            (CommandEncoder::usesFrameSequence(props) || usesModbusBitWrite(type, props) ||
             (!pointKey.isEmpty() && !props.value(pointKey).toString().isEmpty()))) {
            QJsonObject configuration;
            for (const auto &key : {CommandMode, target, endpoint, on, off}) {
                if (props.contains(key))
                    configuration.insert(key, props.take(key));
            }
            props.insert(CommandMode, RequiresNewSender);
            if (type == u"ToggleSwitch") {
                // A legacy switch with no command can write through its local slave binding.
                props.insert(on, RequiresNewSender);
                props.insert(off, RequiresNewSender);
            }
            props.insert(SendConfiguration, configuration);
            widget.insert(QStringLiteral("properties"), props);
        } else if (!saving && mode == RequiresNewSender && props.value(SendConfiguration).isObject()) {
            const auto configuration = props.take(SendConfiguration).toObject();
            for (const auto &key : {CommandMode, target, endpoint, on, off}) {
                if (configuration.contains(key))
                    props.insert(key, configuration.value(key));
                else if (key != CommandMode)
                    props.remove(key);
            }
            widget.insert(QStringLiteral("properties"), props);
        }
        const auto children = widget.value(QStringLiteral("children")).toArray();
        if (!children.isEmpty())
            widget.insert(QStringLiteral("children"), projectSendConfiguration(children, saving));
        if (widget != widgets.at(index).toObject())
            widgets[index] = widget;
    }
    return widgets;
}

} // namespace

QJsonObject Project::toJson() const
{
    QJsonObject document{
        {QStringLiteral("version"), formatVersion(widgets, logic, frameTemplates)},
        {QStringLiteral("appName"), appName},
        {QStringLiteral("canvas"), QJsonObject{{QStringLiteral("width"), canvasWidth},
                                               {QStringLiteral("height"), canvasHeight},
                                               {QStringLiteral("gridSize"), gridSize}}},
        {QStringLiteral("widgets"), projectSendConfiguration(widgets, true)},
        {QStringLiteral("serial"), serial},
    };
    if (!logic.isEmpty())
        document.insert(QStringLiteral("logic"), logic);
    if (!frameTemplates.isEmpty())
        document.insert(QStringLiteral("frameTemplates"), frameTemplates);
    if (!bitProfiles.isEmpty()) document.insert(QStringLiteral("bitProfiles"), bitProfiles);
    if (!icon.isEmpty()) {
        document.insert(QStringLiteral("icon"), icon);
    }
    return document;
}

namespace {

QJsonArray migrateLegacyInputIds(QJsonArray widgets, const QSet<QString> &inputWidgets, const QSet<QString> &points)
{
    for (qsizetype i = 0; i < widgets.size(); ++i) {
        auto widget = widgets.at(i).toObject();
        if (widget.value(QStringLiteral("type")).toString() == QLatin1String("PushButton")) {
            const auto original = widget.value(QStringLiteral("properties")).toObject();
            auto props = original;
            CommandEncoder::mapInputIds(props, [&](const QString &id) {
                if (!isLegacyPointInput(id))
                    return id;
                const auto name = pointNameForInput(id);
                const bool isPoint = points.contains(name);
                const bool isWidget = inputWidgets.contains(id);
                if (isPoint == isWidget)
                    return id;
                return isPoint ? pointInputId(name) : widgetInputId(id);
            });
            if (props != original)
                widget.insert(QStringLiteral("properties"), props);
        }
        const auto children = widget.value(QStringLiteral("children")).toArray();
        const auto migrated = migrateLegacyInputIds(children, inputWidgets, points);
        if (migrated != children)
            widget.insert(QStringLiteral("children"), migrated);
        if (widget != widgets.at(i).toObject())
            widgets[i] = widget;
    }
    return widgets;
}

bool prepareProject(QJsonObject &data, QString *error, bool allowExternalReferences, bool checkUniqueRoutes)
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
    const auto templates = data.value(QStringLiteral("frameTemplates"));
    if (!templates.isUndefined() && !templates.isObject())
        return fail(error, QCoreApplication::translate("fhmi", "帧格式模板列表必须是对象"));
    Project protocol;
    protocol.frameTemplates = templates.toObject();
    protocol.serial = serialValue.toObject();
    const auto templateProblem = FrameTemplates::problem(protocol.frameTemplates);
    if (!templateProblem.isEmpty())
        return fail(error, templateProblem);
    if (!validateTemplateReference(protocol.serial.value(FrameTemplates::ReceiveId), protocol.frameTemplates,
                                   QCoreApplication::translate("fhmi", "工程默认"), error))
        return false;
    const auto projectFormat = protocol.receiveFormat();
    const QString parser = projectFormat.type;
    QString parserError;
    if (!createParser(projectFormat.type, projectFormat.settings, &parserError)) {
        return fail(error, parserError);
    }
    QSet<QString> ids;
    QJsonValue widgetList = data.value(QStringLiteral("widgets"));
    if (widgetList.isArray())
        widgetList = projectSendConfiguration(widgetList.toArray(), false);
    if (!widgetList.isUndefined() && !validateWidgets(widgetList, 0, ids, protocol, error))
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
    auto widgets = legacy ? migrateOldS7Fields(widgetList.toArray(), otherCards) : widgetList.toArray();
    if (S7::usesS7(widgets) && version == QLatin1String("1.0"))
        return fail(error, QCoreApplication::translate("fhmi", "S7 工程需要使用 1.1 格式"));
    if (usesRouting(widgets) && legacy)
        return fail(error, QCoreApplication::translate("fhmi", "多通道配置需要使用 1.2 格式"));
    const auto logic = data.value(QStringLiteral("logic"));
    if (!logic.isUndefined() && !logic.isObject())
        return fail(error, QCoreApplication::translate("fhmi", "工程数据与报警配置必须是对象"));
    if ((!logic.toObject().isEmpty() || !protocol.frameTemplates.isEmpty() || usesV13Features(widgets)) &&
        version != QLatin1String("1.3"))
        return fail(error, QCoreApplication::translate("fhmi", "此工程需要使用 UIDP 1.3 格式"));
    if (version != QLatin1String("1.3") && hasNewCanConfiguration(widgets))
        return fail(error, QCoreApplication::translate("fhmi", "CAN 发送初值或 ZLG 自定义时序需要使用 1.3 格式"));
    const auto logicProblem = projectLogicProblem(logic.toObject());
    if (!logicProblem.isEmpty())
        return fail(error, logicProblem);
    const auto names = dataPointNames(logic.toObject());
    const QSet<QString> pointNames(names.cbegin(), names.cend());
    if (!allowExternalReferences) {
        QSet<QString> inputWidgets;
        visitWidgets(widgets, [&](const QJsonObject &widget) {
            if (widget.value(QStringLiteral("type")).toString() == QLatin1String("LineEdit"))
                inputWidgets.insert(widget.value(QStringLiteral("id")).toString());
            return true;
        });
        widgets = migrateLegacyInputIds(widgets, inputWidgets, pointNames);
    }
    QJsonArray withPoints = widgets;
    for (const auto &p : logic.toObject().value(QStringLiteral("points")).toArray()) {
        const auto point = p.toObject();
        if (!isProjectParameter(point) && point.value(QStringLiteral("expression")).toString().trimmed().isEmpty()) {
            const auto binding = point.value(QStringLiteral("binding")).toObject();
            if (checkUniqueRoutes && !allowExternalReferences) {
                const auto problem = dataPointSourceProblem(binding, widgets, parser, ModbusParserChoice::OnConnect);
                if (!problem.isEmpty())
                    return fail(error, QCoreApplication::translate("fhmi", "数据点 %1 · %2").arg(
                        point.value(QStringLiteral("name")).toString(), problem));
            }
            const QJsonObject pointItem{
                {QStringLiteral("id"), pointInputId(point.value(QStringLiteral("name")).toString())},
                {QStringLiteral("type"), QStringLiteral("NumericDisplay")},
                {QStringLiteral("binding"), binding}};
            if (!validateModbus(pointItem, error))
                return false;
            withPoints.append(pointItem);
        }
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
            const auto type = widget.value(QStringLiteral("type")).toString();
            if (!(isTimingChart(type) && TimingSource::usesChannels(props)) &&
                !checkPoint(widget.value(QStringLiteral("binding")).toObject()))
                return false;
            if (usesChartSeries(type, props) && (checkUniqueRoutes || !isTimingChart(type))) {
                const int count = chartCurveCount(type, props);
                for (int i = 0; i < count; ++i)
                    if (!checkPoint(chartCurveBinding(props, i), i))
                        return false;
            }
            return true;
        }))
        return false;
    if (!validateRoutes(withPoints, parser, logic.toObject(), error, allowExternalReferences, checkUniqueRoutes) ||
        !S7::validateWidgets(withPoints, error, allowExternalReferences))
        return false;
    if (widgets != data.value(QStringLiteral("widgets")).toArray())
        data.insert(QStringLiteral("widgets"), widgets);
    return true;
}
} // namespace

bool Project::validate(const QJsonObject &document, QString *error, bool allowExternalReferences, Project *validated)
{
    auto data = document;
    if (!prepareProject(data, error, allowExternalReferences, true))
        return false;
    if (validated)
        *validated = fromTrustedJson(data);
    return true;
}

bool Project::fromJson(const QJsonObject &document, Project *out, QString *error, bool allowExternalReferences)
{
    auto data = document;
    if (!prepareProject(data, error, allowExternalReferences, false))
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
    project.widgets = projectSendConfiguration(data.value(QStringLiteral("widgets")).toArray(), false);
    project.logic = data.value(QStringLiteral("logic")).toObject();
    project.bitProfiles = data.value(QStringLiteral("bitProfiles")).toObject();
    project.frameTemplates = data.value(QStringLiteral("frameTemplates")).toObject();
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
    return receiveFormat().type;
}

QJsonObject Project::parserConfig() const
{
    return receiveFormat().settings;
}

LinkParser::Format Project::receiveFormat() const
{
    if (const auto entry = FrameTemplates::find(frameTemplates, serial.value(FrameTemplates::ReceiveId).toString()))
        return entry->format;
    return {serial.value(QStringLiteral("parser")).toString(QStringLiteral("line")),
            serial.value(QStringLiteral("parserConfig")).toObject()};
}

LinkParser::Format Project::linkReceiveFormat(const QJsonObject &properties) const
{
    return FrameTemplates::linkFormat(frameTemplates, FrameTemplates::receiveIds(properties),
        properties.value(QStringLiteral("defaultParser")).toString(),
        properties.value(QStringLiteral("parserConfig")).toObject(), receiveFormat());
}

QStringList Project::linkReceiveTemplateIds(const QJsonObject &properties) const
{
    auto ids = FrameTemplates::receiveIds(properties);
    if (!ids.isEmpty())
        return ids;
    const QString selection = properties.value(QStringLiteral("defaultParser")).toString();
    if (LinkParser::inheritsProject(selection) ||
        (selection == parserType() && properties.value(QStringLiteral("parserConfig")).toObject().isEmpty())) {
        const auto id = serial.value(FrameTemplates::ReceiveId).toString();
        if (!id.isEmpty())
            ids.append(id);
    }
    return ids;
}

QString Project::linkReceiveTemplateId(const QJsonObject &properties) const
{
    const auto ids = linkReceiveTemplateIds(properties);
    return ids.size() == 1 ? ids.first() : QString();
}

} // namespace fhmi
