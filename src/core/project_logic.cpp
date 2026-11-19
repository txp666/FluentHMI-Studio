#include "core/project_logic.h"
#include "core/binding.h"
#include "core/command_encoder.h"
#include "core/expression.h"
#include "core/frame_templates.h"
#include "core/i18n.h"
#include "core/project.h"
#include <QtCore/QCoreApplication>
#include <QtCore/QJsonArray>
#include <QtCore/QSet>
#include <cmath>

namespace fhmi {
namespace {
QString tr(const char *s) { return QCoreApplication::translate("fhmi", s); }
constexpr QLatin1String LegacyPointPrefix("@data:");
constexpr QLatin1String PointPrefix("@point:");
constexpr QLatin1String WidgetPrefix("@widget:");

QJsonObject pointsByName(const QJsonObject &logic)
{
    QJsonObject points;
    for (const auto &p : logic.value(QStringLiteral("points")).toArray())
        points.insert(p.toObject().value(QStringLiteral("name")).toString(), p);
    return points;
}

QJsonArray renameWidgetReferences(QJsonArray widgets, const QString &previous, const QString &current,
                                 bool ambiguousLegacyInput)
{
    const auto renamesInput = [&](const QString &id) {
        return pointNameForInput(id) == previous && !(ambiguousLegacyInput && isLegacyPointInput(id));
    };
    for (qsizetype i = 0; i < widgets.size(); ++i) {
        auto widget = widgets[i].toObject();
        auto binding = widget.value(QStringLiteral("binding")).toObject();
        if (binding.value(QStringLiteral("parseMode")).toString() == ParseMode::DataPoint &&
            binding.value(QStringLiteral("key")).toString() == previous) {
            binding.insert(QStringLiteral("key"), current);
            widget.insert(QStringLiteral("binding"), binding);
        }
        auto props = widget.value(QStringLiteral("properties")).toObject();
        const QString type = widget.value(QStringLiteral("type")).toString();
        if (type == QLatin1String("RealtimeChart") || isTimingChart(type)) {
            for (int curve = 0; curve < 8; ++curve) {
                const QString prefix = QStringLiteral("_curve_%1_").arg(curve);
                if (props.value(prefix + QStringLiteral("parseMode")).toString() == ParseMode::DataPoint &&
                    props.value(prefix + QStringLiteral("key")).toString() == previous)
                    props.insert(prefix + QStringLiteral("key"), current);
            }
        }
        if (type == QLatin1String("LineEdit") && props.value(QStringLiteral("parameterName")).toString() == previous)
            props.insert(QStringLiteral("parameterName"), current);
        if (type == QLatin1String("PushButton"))
            CommandEncoder::mapInputIds(props, [&](const QString &id) { return renamesInput(id) ? pointInputId(current) : id; });
        if (type == QLatin1String("DataTable") &&
            props.value(QStringLiteral("recordSource")).toString() == QLatin1String("points")) {
            if (props.value(QStringLiteral("recordTriggerPoint")).toString() == previous)
                props.insert(QStringLiteral("recordTriggerPoint"), current);
            auto columns = props.value(QStringLiteral("recordColumns")).toArray();
            for (qsizetype index = 0; index < columns.size(); ++index) {
                auto column = columns.at(index).toObject();
                if (column.value(QStringLiteral("field")).toString() == previous) {
                    column.insert(QStringLiteral("field"), current);
                    columns[index] = column;
                }
            }
            if (props.contains(QStringLiteral("recordColumns")))
                props.insert(QStringLiteral("recordColumns"), columns);
        }
        if (props != widget.value(QStringLiteral("properties")).toObject())
            widget.insert(QStringLiteral("properties"), props);
        if (widget.contains(QStringLiteral("children")))
            widget.insert(QStringLiteral("children"),
                          renameWidgetReferences(widget.value(QStringLiteral("children")).toArray(), previous, current,
                                                 ambiguousLegacyInput));
        widgets[i] = widget;
    }
    return widgets;
}
} // namespace

void renameDataPoint(Project *project, const QString &previous, const QString &current)
{
    if (previous == current)
        return;
    auto points = project->logic.value(QStringLiteral("points")).toArray();
    for (qsizetype i = 0; i < points.size(); ++i) {
        auto point = points[i].toObject();
        if (point.value(QStringLiteral("name")).toString() == previous)
            point.insert(QStringLiteral("name"), current);
        if (point.contains(QStringLiteral("expression")))
            point.insert(QStringLiteral("expression"),
                         Expression::renameReference(point.value(QStringLiteral("expression")).toString(), previous, current));
        points[i] = point;
    }
    project->logic.insert(QStringLiteral("points"), points);
    auto alarms = project->logic.value(QStringLiteral("alarms")).toArray();
    for (qsizetype i = 0; i < alarms.size(); ++i) {
        auto alarm = alarms[i].toObject();
        alarm.insert(QStringLiteral("condition"),
                     Expression::renameReference(alarm.value(QStringLiteral("condition")).toString(), previous, current));
        alarms[i] = alarm;
    }
    if (project->logic.contains(QStringLiteral("alarms")))
        project->logic.insert(QStringLiteral("alarms"), alarms);
    const QString legacyInput = LegacyPointPrefix + previous;
    const bool ambiguousLegacyInput = !visitWidgets(project->widgets, [&](const QJsonObject &widget) {
        return widget.value(QStringLiteral("type")).toString() != QLatin1String("LineEdit") ||
               widget.value(QStringLiteral("id")).toString() != legacyInput;
    });
    project->widgets = renameWidgetReferences(project->widgets, previous, current, ambiguousLegacyInput);
}
QStringList dataPointOrder(const QJsonObject &logic)
{
    const auto points = pointsByName(logic);
    QStringList result;
    QSet<QString> visiting;
    std::function<bool(const QString &)> visit = [&](const QString &name) {
        if (result.contains(name))
            return true;
        if (visiting.contains(name) || !points.contains(name))
            return false;
        visiting.insert(name);
        for (const auto &dependency :
             Expression::references(points[name].toObject().value(QStringLiteral("expression")).toString()))
            if (!visit(dependency))
                return false;
        visiting.remove(name);
        result.append(name);
        return true;
    };
    for (const auto &name : points.keys())
        if (!visit(name))
            return {};
    return result;
}

QStringList dataPointNames(const QJsonObject &logic, bool parametersOnly)
{
    QStringList names;
    for (const auto &entry : logic.value(QStringLiteral("points")).toArray()) {
        const auto point = entry.toObject();
        if (!parametersOnly || isProjectParameter(point))
            names.append(point.value(QStringLiteral("name")).toString());
    }
    return names;
}

QJsonObject findDataPoint(const QJsonObject &logic, const QString &name)
{
    for (const auto &entry : logic.value(QStringLiteral("points")).toArray()) {
        const auto point = entry.toObject();
        if (point.value(QStringLiteral("name")).toString() == name)
            return point;
    }
    return {};
}

bool isProjectParameter(const QJsonObject &point) { return point.contains(QStringLiteral("initialValue")); }

QString dataPointCommandText(const QVariant &value)
{
    if (value.typeId() == QMetaType::Bool)
        return QString::number(value.toBool() ? 1 : 0);
    if (value.typeId() == QMetaType::Double || value.typeId() == QMetaType::Float) {
        const double number = value.toDouble();
        constexpr double limit = 9223372036854775808.0; // 2^63, the exclusive upper bound for qint64
        if (number >= -limit && number < limit && std::trunc(number) == number)
            return QString::number(static_cast<qint64>(number));
    }
    return displayString(value);
}

QString pointInputId(const QString &name) { return PointPrefix + name; }

bool isLegacyPointInput(const QString &id) { return id.startsWith(LegacyPointPrefix); }

QString pointNameForInput(const QString &id)
{
    if (id.startsWith(PointPrefix))
        return id.mid(PointPrefix.size());
    return isLegacyPointInput(id) ? id.mid(LegacyPointPrefix.size()) : QString();
}

QString widgetInputId(const QString &id)
{
    if (id.startsWith(WidgetPrefix) || id.startsWith(PointPrefix) || isLegacyPointInput(id))
        return WidgetPrefix + id;
    return id;
}

QString widgetIdForInput(const QString &id)
{
    return id.startsWith(WidgetPrefix) ? id.mid(WidgetPrefix.size()) : id;
}

QString dataPointSourceProblem(const QJsonObject &binding, const QJsonArray &widgets, const QString &projectParser)
{
    const auto entry = BindingEntry::fromWidget(
        {{QStringLiteral("type"), QStringLiteral("NumericDisplay")}, {QStringLiteral("binding"), binding}});
    if (!entry || entry->parseMode == ParseMode::DataPoint)
        return tr("来源绑定不完整");
    QString problem;
    resolveProjectRoute(widgets, projectParser, bindingRouteTransport(entry->parseMode), entry->route(),
                        RoutePurpose::DataPoint, &problem);
    return problem;
}

QString dataPointSourceText(const QJsonObject &binding)
{
    const auto entry = BindingEntry::fromWidget(
        {{QStringLiteral("type"), QStringLiteral("NumericDisplay")}, {QStringLiteral("binding"), binding}});
    if (!entry)
        return tr("未配置");
    QStringList parts{entry->sourceCardId.isEmpty() ? tr("自动匹配唯一连接") : entry->sourceCardId};
    if (!entry->sourceEndpoint.isEmpty())
        parts.append(entry->sourceEndpoint);
    if (entry->parseMode == ParseMode::Modbus) {
        parts.append(QStringLiteral("Modbus"));
        parts.append(QStringLiteral("%1 %2").arg(translated(entry->mbArea)).arg(entry->mbRegIndex));
        if (const QString table = ModbusArea::prefix(entry->mbArea); table == QLatin1String("hr") || table == QLatin1String("ir"))
            parts.append(entry->mbDataType);
        if (entry->mbSlaveFilter >= 0)
            parts.append(tr("从站 %1").arg(entry->mbSlaveFilter));
    } else if (entry->s7) {
        parts.append(QStringLiteral("S7"));
        parts.append(entry->s7->address.canonical);
        parts.append(entry->s7->type.name);
    } else if (entry->parseMode == ParseMode::HexBytes) {
        parts.append(tr("字节 %1").arg(entry->byteOffset));
        parts.append(entry->dataType);
    } else if (entry->parseMode == ParseMode::CanSignal) {
        parts.append(Can::idText(entry->canId));
        parts.append(tr("起始位 %1 · %2 位").arg(binding.value(QStringLiteral("canStartBit")).toInt())
                         .arg(entry->canSignal.length));
    } else {
        parts.append(entry->parseMode == ParseMode::JsonPath ? entry->jsonPath : entry->key);
    }
    return parts.join(QStringLiteral(" · "));
}

QStringList dataPointReferences(const Project &project, const QString &name)
{
    QStringList references;
    for (const auto &entry : project.logic.value(QStringLiteral("points")).toArray()) {
        const auto point = entry.toObject();
        if (Expression::references(point.value(QStringLiteral("expression")).toString()).contains(name))
            references.append(tr("计算点 %1").arg(point.value(QStringLiteral("name")).toString()));
    }
    for (const auto &entry : project.logic.value(QStringLiteral("alarms")).toArray()) {
        const auto alarm = entry.toObject();
        if (Expression::references(alarm.value(QStringLiteral("condition")).toString()).contains(name))
            references.append(tr("报警 %1").arg(alarm.value(QStringLiteral("id")).toString()));
    }
    const auto boundToPoint = [&](const QJsonObject &binding) {
        return binding.value(QStringLiteral("parseMode")).toString() == ParseMode::DataPoint &&
               binding.value(QStringLiteral("key")).toString() == name;
    };
    visitWidgets(project.widgets, [&](const QJsonObject &widget) {
        const auto type = widget.value(QStringLiteral("type")).toString();
        const auto props = widget.value(QStringLiteral("properties")).toObject();
        const auto id = widget.value(QStringLiteral("id")).toString();
        if (boundToPoint(widget.value(QStringLiteral("binding")).toObject()))
            references.append(tr("%1 · 控件绑定").arg(id));
        if (usesChartSeries(type, props)) {
            for (int curve = 0; curve < chartCurveCount(props); ++curve)
                if (boundToPoint(chartCurveBinding(props, curve)))
                    references.append((isTimingChart(type) ? tr("%1 · 通道 %2") : tr("%1 · 曲线 %2"))
                                          .arg(id).arg(curve + 1));
        } else if (type == QLatin1String("LineEdit") && props.value(QStringLiteral("parameterName")).toString() == name) {
            references.append(tr("%1 · 参数输入").arg(id));
        } else if (type == QLatin1String("PushButton")) {
            const auto inputs = CommandEncoder::commandInputIds(props);
            for (qsizetype row = 0; row < inputs.size(); ++row)
                if (pointNameForInput(inputs.at(row)) == name)
                    references.append(tr("%1 · 发送输入第 %2 行").arg(id).arg(row + 1));
        } else if (type == QLatin1String("DataTable") && props.value(QStringLiteral("recordSource")).toString() == QLatin1String("points")) {
            if (props.value(QStringLiteral("recordTriggerPoint")).toString() == name)
                references.append(tr("%1 · 记录触发点").arg(id));
            for (const auto &entry : props.value(QStringLiteral("recordColumns")).toArray()) {
                if (entry.toObject().value(QStringLiteral("field")).toString() == name) {
                    references.append(tr("%1 · 记录列").arg(id));
                    break;
                }
            }
        }
        return true;
    });
    return references;
}

QString projectLogicProblem(const QJsonObject &logic)
{
    for (const char *key : {"points", "alarms"})
        if (logic.contains(QLatin1String(key)) && !logic.value(QLatin1String(key)).isArray())
            return tr("数据点和报警定义必须是列表");
    QSet<QString> names;
    const auto points = logic.value(QStringLiteral("points")).toArray();
    for (const auto &p : points) {
        const auto point = p.toObject();
        const auto name = point.value(QStringLiteral("name")).toString().trimmed();
        if (name.isEmpty() || names.contains(name))
            return tr("数据点名称不能为空或重复");
        names.insert(name);
        const auto timeout = point.value(QStringLiteral("staleAfterMs"));
        if (!timeout.isUndefined() && (!timeout.isDouble() || timeout.toDouble() != timeout.toInt() ||
                                      timeout.toInt() < 0 || timeout.toInt() > 86400000))
            return tr("数据点 %1 的有效时间无效").arg(name);
        if (isProjectParameter(point)) {
            const auto initial = point.value(QStringLiteral("initialValue"));
            if ((!initial.isString() && !initial.isBool() && !initial.isDouble()) ||
                point.contains(QStringLiteral("binding")) || point.contains(QStringLiteral("expression")))
                return tr("工程参数 %1 的初始值无效").arg(name);
            continue;
        }
        const QString expression = point.value(QStringLiteral("expression")).toString().trimmed();
        if (expression.isEmpty()) {
            const auto binding = point.value(QStringLiteral("binding")).toObject();
            if (binding.value(QStringLiteral("parseMode")).toString() == ParseMode::DataPoint ||
                !BindingEntry::fromWidget(
                    {{QStringLiteral("type"), QStringLiteral("NumericDisplay")}, {QStringLiteral("binding"), binding}}))
                return tr("数据点 %1 的来源绑定不完整").arg(name);
            bool valid = false;
            const QString conversion = binding.value(QStringLiteral("expression")).toString().trimmed();
            if (!conversion.isEmpty()) {
                const auto refs = Expression::references(conversion, &valid);
                if (!valid || !refs.isEmpty())
                    return tr("数据点 %1 的换算表达式无效").arg(name);
            }
        }
    }
    const auto check = [&](const QString &expression) -> QString {
        bool valid = false, usesValue = false;
        const auto refs = Expression::references(expression, &valid, &usesValue);
        if (usesValue)
            return tr("工程表达式请用 T[\"名称\"] 引用数据点；value 仅用于单点绑定转换");
        if (!valid)
            return tr("表达式语法无效：%1").arg(expression);
        for (const auto &name : refs)
            if (!names.contains(name))
                return tr("表达式引用的数据点不存在：%1").arg(name);
        return {};
    };
    for (const auto &p : points) {
        const QString expression = p.toObject().value(QStringLiteral("expression")).toString().trimmed();
        if (!expression.isEmpty()) {
            const auto problem = check(expression);
            if (!problem.isEmpty())
                return problem;
        }
    }
    if (dataPointOrder(logic).size() != points.size())
        return tr("数据点表达式存在循环引用");
    QSet<QString> ids;
    for (const auto &a : logic.value(QStringLiteral("alarms")).toArray()) {
        const auto alarm = a.toObject();
        const auto id = alarm.value(QStringLiteral("id")).toString().trimmed();
        if (id.isEmpty() || ids.contains(id))
            return tr("报警名称不能为空或重复");
        ids.insert(id);
        const int level = alarm.value(QStringLiteral("level")).toInt(2);
        if (level < 1 || level > 3)
            return tr("报警级别必须为 1、2 或 3，3 级最高");
        const auto problem = check(alarm.value(QStringLiteral("condition")).toString());
        if (!problem.isEmpty())
            return tr("报警 %1：%2").arg(id, problem);
    }
    return {};
}
} // namespace fhmi
