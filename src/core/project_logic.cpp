#include "core/project_logic.h"
#include "core/binding.h"
#include "core/expression.h"
#include "core/project.h"
#include <QtCore/QCoreApplication>
#include <QtCore/QJsonArray>
#include <QtCore/QSet>

namespace fhmi {
namespace {
QString tr(const char *s) { return QCoreApplication::translate("fhmi", s); }
QJsonObject pointsByName(const QJsonObject &logic)
{
    QJsonObject points;
    for (const auto &p : logic.value(QStringLiteral("points")).toArray())
        points.insert(p.toObject().value(QStringLiteral("name")).toString(), p);
    return points;
}

QJsonArray renameWidgetReferences(QJsonArray widgets, const QString &previous, const QString &current)
{
    for (qsizetype i = 0; i < widgets.size(); ++i) {
        auto widget = widgets[i].toObject();
        auto binding = widget.value(QStringLiteral("binding")).toObject();
        if (binding.value(QStringLiteral("parseMode")).toString() == ParseMode::DataPoint &&
            binding.value(QStringLiteral("key")).toString() == previous) {
            binding.insert(QStringLiteral("key"), current);
            widget.insert(QStringLiteral("binding"), binding);
        }
        if (widget.value(QStringLiteral("type")).toString() == QLatin1String("RealtimeChart")) {
            auto props = widget.value(QStringLiteral("properties")).toObject();
            for (int curve = 0; curve < 8; ++curve) {
                const QString prefix = QStringLiteral("_curve_%1_").arg(curve);
                if (props.value(prefix + QStringLiteral("parseMode")).toString() == ParseMode::DataPoint &&
                    props.value(prefix + QStringLiteral("key")).toString() == previous)
                    props.insert(prefix + QStringLiteral("key"), current);
            }
            if (props != widget.value(QStringLiteral("properties")).toObject())
                widget.insert(QStringLiteral("properties"), props);
        }
        if (widget.contains(QStringLiteral("children")))
            widget.insert(QStringLiteral("children"),
                          renameWidgetReferences(widget.value(QStringLiteral("children")).toArray(), previous, current));
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
    project->widgets = renameWidgetReferences(project->widgets, previous, current);
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
        const QString expression = point.value(QStringLiteral("expression")).toString().trimmed();
        if (expression.isEmpty()) {
            const auto binding = point.value(QStringLiteral("binding")).toObject();
            if (binding.value(QStringLiteral("parseMode")).toString() == ParseMode::DataPoint ||
                !BindingEntry::fromWidget(
                    {{QStringLiteral("type"), QStringLiteral("NumericDisplay")}, {QStringLiteral("binding"), binding}}))
                return tr("数据点 %1 的来源绑定不完整").arg(name);
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
