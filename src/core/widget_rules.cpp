#include "core/widget_rules.h"
#include "core/binding.h"
#include "core/expression.h"
#include "core/project.h"
#include "core/widget_meta.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QRegularExpression>
#include <cmath>
#include <limits>

namespace fhmi {
namespace {
QString tr(const char *source) { return QCoreApplication::translate("fhmi", source); }
}

QList<ColorRange> parseColorRanges(const QJsonValue &rows, QString *error)
{
    QList<ColorRange> result;
    if (!rows.isUndefined() && !rows.isArray()) {
        if (error)
            *error = tr("颜色区间必须是列表");
        return {};
    }
    static const QRegularExpression hex(QStringLiteral("^#[0-9a-fA-F]{6}$"));
    for (const auto &row : rows.toArray()) {
        const auto object = row.toObject();
        const auto min = object.value(QStringLiteral("min"));
        bool ok = false;
        double lower = min.toVariant().toDouble(&ok);
        const bool unbounded = min.isUndefined() || (min.isString() && min.toString().trimmed().isEmpty());
        if (unbounded)
            lower = -std::numeric_limits<double>::infinity();
        const auto color = object.value(QStringLiteral("color")).toString();
        if ((!unbounded && ((!min.isDouble() && !min.isString()) || !ok || !std::isfinite(lower))) ||
            (!result.isEmpty() && lower <= result.last().minimum) || !hex.match(color).hasMatch()) {
            if (error)
                *error = tr("颜色区间第 %1 行无效：下限须递增，仅首行可留空，颜色格式为 #RRGGBB").arg(result.size() + 1);
            return {};
        }
        result.append({lower, color});
    }
    return result;
}

QString rangeColor(const QList<ColorRange> &ranges, double value)
{
    for (auto it = ranges.crbegin(); it != ranges.crend(); ++it)
        if (value >= it->minimum)
            return it->color;
    return {};
}

QStringList tabNames(const QString &text)
{
    QStringList names;
    for (const auto &line : text.split(u'\n'))
        if (!line.trimmed().isEmpty())
            names.append(line.trimmed());
    return names.isEmpty() ? QStringList{QStringLiteral("标签1")} : names; // i18n: value
}

ActionTargets::ActionTargets(const QJsonArray &widgets, const QJsonObject &logic)
{
    visitWidgets(widgets, [&](const QJsonObject &widget) {
        const auto type = widget.value(QStringLiteral("type")).toString();
        const auto id = widget.value(QStringLiteral("id")).toString();
        if (type == u"PushButton")
            buttons.insert(id);
        if (type == u"TabWidget") {
            const auto def = WidgetRegistry::instance().find(type)->prop(QStringLiteral("tabNames"));
            const auto props = widget.value(QStringLiteral("properties")).toObject();
            pages.insert(id, tabNames(props.value(QStringLiteral("tabNames")).toString(def->defaultValue.toString())));
        }
        return true;
    });
    for (const auto &entry : logic.value(QStringLiteral("points")).toArray()) {
        const auto point = entry.toObject();
        const auto name = point.value(QStringLiteral("name")).toString();
        points.insert(name);
        if (point.contains(QStringLiteral("initialValue")))
            parameters.insert(name, point.value(QStringLiteral("initialValue")));
    }
}

QString widgetActionProblem(const QJsonValue &actions, const QString &widgetType, const ActionTargets &targets,
                            bool allowExternal)
{
    if (actions.isUndefined())
        return {};
    if (!actions.isArray())
        return tr("自定义操作必须是列表");
    int row = 0;
    for (const auto &entry : actions.toArray()) {
        ++row;
        const auto problem = [row](const QString &message) { return tr("自定义操作第 %1 行：%2").arg(row).arg(message); };
        const auto action = entry.toObject();
        const auto event = action.value(QStringLiteral("event")).toString();
        const auto kind = action.value(QStringLiteral("type")).toString();
        const auto targetValue = action.value(QStringLiteral("target"));
        const auto target = targetValue.toString();
        const auto value = action.value(QStringLiteral("value"));
        const auto condition = action.value(QStringLiteral("condition"));
        const bool toggle = widgetType == u"ToggleSwitch";
        if (toggle ? (event != u"on" && event != u"off") :
            (event != u"click" || (widgetType != u"PushButton" && widgetType != u"LEDIndicator")))
            return problem(tr("请选择此控件支持的事件"));
        if (!condition.isUndefined() && !condition.isString())
            return problem(tr("条件必须是表达式文本"));
        if (!targetValue.isUndefined() && !targetValue.isString())
            return problem(tr("操作目标必须是名称或控件 ID"));
        if (!condition.toString().trimmed().isEmpty()) {
            bool valid = false;
            const auto refs = Expression::references(condition.toString(), &valid);
            if (!valid)
                return problem(tr("条件表达式无效"));
            for (const auto &ref : refs)
                if (!allowExternal && !targets.points.contains(ref))
                    return problem(tr("数据点不存在：%1").arg(ref));
        }
        if (kind == u"navigate") {
            if (target.isEmpty() || value.toString().isEmpty() ||
                (!allowExternal && targets.pages.value(target).count(value.toString()) != 1))
                return problem(tr("请选择分页容器及其中唯一的页面名称"));
        } else if (kind == u"send") {
            if ((target.isEmpty() && widgetType == u"LEDIndicator") ||
                (!target.isEmpty() && !allowExternal && !targets.buttons.contains(target)))
                return problem(tr("发送目标须为已有的命令按钮"));
        } else if (kind == u"setParameter") {
            const auto initial = targets.parameters.value(target);
            if (target.isEmpty() || (!allowExternal && !targets.parameters.contains(target)))
                return problem(tr("请选择可写的工程参数"));
            if ((!value.isString() && !value.isDouble() && !value.isBool()) ||
                (initial.isDouble() && !bindingFloat(value.toVariant())) ||
                (initial.isBool() && !bindingBoolean(value.toVariant())))
                return problem(tr("工程参数的值与类型不匹配"));
        } else if (kind == u"notify") {
            if (value.toString().trimmed().isEmpty())
                return problem(tr("请填写通知内容"));
        } else {
            return problem(tr("请选择支持的操作类型"));
        }
    }
    return {};
}
} // namespace fhmi
