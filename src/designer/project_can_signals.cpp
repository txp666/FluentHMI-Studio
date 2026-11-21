#include "designer/project_can_signals.h"

#include "comm/can/can_initial_state.h"
#include "core/binding.h"
#include "core/chart_limits.h"
#include "core/command_encoder.h"
#include "core/dbc.h"
#include "core/project.h"
#include "designer/can_signal_editor.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QRegularExpression>
#include <QtCore/QSet>

#include <algorithm>

namespace fhmi {
namespace {

QString tr(const char *source) { return QCoreApplication::translate("fhmi", source); }

QString signature(InlineSignal entry)
{
    entry.signal.name = QStringLiteral("_");
    return formatInlineSignal(entry);
}

struct SignalChange
{
    QString name;
    QString replacement;
    QString replacementKey;
    bool definitionChanged = false;

    QString description(const QString &key, const QString &replacementText) const
    {
        if (replacementKey.isEmpty() && !definitionChanged)
            return tr("%1 · 信号已移除，需核对").arg(key);
        if (replacementKey.isEmpty())
            return tr("%1 · 信号定义已更改").arg(key);
        const auto renamed = QStringLiteral("%1 → %2").arg(key, replacementText);
        return definitionChanged ? tr("%1 · 信号定义已更改").arg(renamed) : renamed;
    }
};

QHash<QString, SignalChange> signalChanges(const QList<InlineSignal> &before, const QList<InlineSignal> &after,
                                           const QHash<QString, QString> &renamed)
{
    QHash<QString, InlineSignal> previous, next;
    for (const auto &entry : before)
        previous.insert(inlineSignalKey(entry.id, entry.signal.name), entry);
    for (const auto &entry : after)
        next.insert(inlineSignalKey(entry.id, entry.signal.name), entry);

    // Text edits have no form identity. A unique removed/added pair with the same definition can be migrated.
    const auto explicitTargets = renamed.values();
    QMultiHash<QString, QString> added, removed;
    for (auto it = next.cbegin(); it != next.cend(); ++it)
        if (!previous.contains(it.key()) && !explicitTargets.contains(it.key()))
            added.insert(signature(it.value()), it.key());
    for (auto it = previous.cbegin(); it != previous.cend(); ++it)
        if (!next.contains(it.key()) && !renamed.contains(it.key()))
            removed.insert(signature(it.value()), it.key());

    QHash<QString, SignalChange> changes;
    for (auto it = previous.cbegin(); it != previous.cend(); ++it) {
        const QString layout = signature(it.value());
        auto replacement = next.constFind(renamed.value(it.key(), it.key()));
        if (replacement == next.cend() && removed.count(layout) == 1 && added.count(layout) == 1)
            replacement = next.constFind(added.value(layout));
        SignalChange change{it->signal.name, {}, {}, false};
        if (replacement != next.cend()) {
            change.definitionChanged = signature(replacement.value()) != layout;
            if (replacement.key() != it.key()) {
                change.replacement = replacement->signal.name;
                change.replacementKey = replacement.key();
            } else if (!change.definitionChanged) {
                continue;
            }
        }
        changes.insert(it.key(), change);
    }
    return changes;
}

DataRoute sourceRoute(const QJsonObject &props)
{
    return {props.value(QStringLiteral("sourceCardId")).toString(), props.value(QStringLiteral("sourceEndpoint")).toString()};
}

enum class Match { None, Unique, Multiple };

class ReferenceEdits
{
  public:
    const Project &project;
    const QString &cardId;
    const CanSignalSet *previous;
    const QHash<QString, SignalChange> &changes;
    const QSet<QString> &dbcAliases;
    QList<ReferenceRow> &rows;

    Match scope(const DataRoute &route, RouteTransport transport) const
    {
        int count = 0;
        bool includesCard = false;
        for (const auto &entry : routeConnections(project.widgets, transport, !route.endpoint.isEmpty(), project.parserType())) {
            if (route.acceptsConnection(entry.first)) {
                ++count;
                includesCard |= entry.first == cardId;
            }
        }
        return !includesCard ? Match::None : count == 1 ? Match::Unique : Match::Multiple;
    }

    QString reviewField(QJsonObject &object, const QString &property, const DataRoute &route, RouteTransport transport) const
    {
        const QString key = object.value(property).toString();
        auto change = changes.constFind(key);
        const bool shortName = change == changes.cend();
        if (shortName) {
            change = std::find_if(changes.cbegin(), changes.cend(), [&key](const SignalChange &field) {
                return field.name == key;
            });
            if (change == changes.cend())
                return {};
        }
        const auto match = scope(route, transport);
        if (match == Match::None)
            return {};
        if (match == Match::Multiple)
            return tr("%1 · 多连接，需核对").arg(key);
        if (shortName && dbcAliases.contains(key))
            return tr("%1 · 存在同名 DBC 信号，需核对").arg(key);
        if (!change->replacementKey.isEmpty())
            object.insert(property, change->replacementKey);
        return change->description(key, change->replacementKey);
    }

    QString reviewBinding(QJsonObject &binding) const
    {
        const QString mode = binding.value(QStringLiteral("parseMode")).toString(ParseMode::KeyValue);
        const auto route = sourceRoute(binding);
        if (mode == ParseMode::KeyValue || mode == ParseMode::CanField)
            return reviewField(binding, QStringLiteral("key"), route, bindingRouteTransport(mode));
        if (mode != ParseMode::JsonPath)
            return {};
        const QString path = binding.value(QStringLiteral("jsonPath")).toString();
        if (path.contains(u'.'))
            return {};
        const auto detail = reviewField(binding, QStringLiteral("jsonPath"), route, RouteTransport::Any);
        const auto replacement = binding.value(QStringLiteral("jsonPath")).toString();
        if (replacement != path) {
            binding.insert(QStringLiteral("parseMode"), ParseMode::KeyValue);
            binding.insert(QStringLiteral("key"), replacement);
            binding.insert(QStringLiteral("jsonPath"), QString());
        }
        return detail;
    }

    void reviewSend(const QJsonObject &widget, QJsonObject &props) const
    {
        if (CommandEncoder::sendRouteFilter(props, widget.value(QStringLiteral("type")).toString()).transport != RouteTransport::Can)
            return;
        const DataRoute route{props.value(QStringLiteral("commandTargetId")).toString(),
                              props.value(QStringLiteral("commandEndpoint")).toString()};
        const auto target = previous->target(props.value(QStringLiteral("canId")).toString());
        const auto match = scope(route, RouteTransport::Can);
        if (match == Match::None || !target || !target->hasSignals)
            return;

        QStringList details;
        const auto reviewText = [&](const QString &text) {
            static const QRegularExpression lhs(QStringLiteral(R"((?:^|[,;\n])\s*([A-Za-z_][A-Za-z0-9_]*)\s*=)"));
            auto matches = lhs.globalMatch(text);
            QString updated = text;
            while (matches.hasNext()) {
                const auto token = matches.next();
                const auto change = changes.constFind(inlineSignalKey(target->id, token.captured(1)));
                if (change == changes.cend())
                    continue;
                if (match == Match::Multiple) {
                    details.append(tr("%1 · 多连接，需核对").arg(change->name));
                } else {
                    details.append(change->description(change->name, change->replacement));
                    if (!change->replacement.isEmpty()) {
                        const auto delta = updated.size() - text.size();
                        updated.replace(token.capturedStart(1) + delta, token.capturedLength(1), change->replacement);
                    }
                }
            }
            return updated;
        };
        for (const auto *property : {"commandTemplate", "command", "onCommand", "offCommand"}) {
            const auto text = props.value(QLatin1String(property)).toString();
            const auto updated = reviewText(text);
            if (updated != text)
                props.insert(QLatin1String(property), updated);
        }
        auto options = props.value(QStringLiteral("options")).toArray();
        for (qsizetype i = 0; i < options.size(); ++i) {
            auto entry = options.at(i).toObject();
            const auto value = entry.value(QStringLiteral("value")).toString();
            const auto text = value.trimmed().isEmpty() ? entry.value(QStringLiteral("label")).toString() : value;
            const auto updated = reviewText(text);
            if (updated != text) {
                entry.insert(QStringLiteral("value"), updated);
                options[i] = entry;
            }
        }
        if (options != props.value(QStringLiteral("options")).toArray())
            props.insert(QStringLiteral("options"), options);
        details.removeDuplicates();
        if (!details.isEmpty())
            rows.append({tr("发送控件"), widgetLabel(widget), details.join(QStringLiteral(" · "))});
    }

    void reviewReceive(QJsonObject &widget, QJsonObject &props) const
    {
        const auto type = widget.value(QStringLiteral("type")).toString();
        auto binding = widget.value(QStringLiteral("binding")).toObject();
        if (const auto detail = reviewBinding(binding); !detail.isEmpty()) {
            rows.append({tr("显示控件"), widgetLabel(widget), detail});
            widget.insert(QStringLiteral("binding"), binding);
        }
        if (type == QLatin1String("RealtimeChart") || isTimingChart(type)) {
            const int limit = isTimingChart(type) ? MaxTimingChannels : MaxRealtimeCurves;
            for (int curve = 0; curve < limit; ++curve) {
                const auto original = chartCurveBinding(props, curve);
                auto series = original;
                const auto detail = reviewBinding(series);
                if (detail.isEmpty())
                    continue;
                const auto prefix = QStringLiteral("_curve_%1_").arg(curve);
                for (const auto &property : {QStringLiteral("key"), QStringLiteral("parseMode"), QStringLiteral("jsonPath")})
                    if (series.value(property) != original.value(property))
                        props.insert(prefix + property, series.value(property));
                rows.append({isTimingChart(type) ? tr("数字时序图") : tr("实时曲线"),
                             QStringLiteral("%1 · %2").arg(widgetLabel(widget), chartCurveName(props, curve)), detail});
            }
        }
        if (type == QLatin1String("DataTable") && props.value(QStringLiteral("recordSource")).toString() != QLatin1String("points")) {
            const auto route = sourceRoute(props);
            auto columns = props.value(QStringLiteral("recordColumns")).toArray();
            for (qsizetype column = 0; column < columns.size(); ++column) {
                auto entry = columns.at(column).toObject();
                const auto detail = reviewField(entry, QStringLiteral("field"), route, RouteTransport::Any);
                if (!detail.isEmpty()) {
                    columns[column] = entry;
                    rows.append({tr("数据表格"), widgetLabel(widget), detail});
                }
            }
            if (columns != props.value(QStringLiteral("recordColumns")).toArray())
                props.insert(QStringLiteral("recordColumns"), columns);
        }
    }

    QJsonArray reviewWidgets(QJsonArray widgets, const QJsonObject &card) const
    {
        for (qsizetype i = 0; i < widgets.size(); ++i) {
            auto widget = widgets.at(i).toObject();
            auto props = widget.value(QStringLiteral("properties")).toObject();
            if (widget.value(QStringLiteral("id")).toString() == cardId)
                props = card;
            if (!changes.isEmpty()) {
                reviewReceive(widget, props);
                reviewSend(widget, props);
            }
            if (props != widget.value(QStringLiteral("properties")).toObject())
                widget.insert(QStringLiteral("properties"), props);
            const auto children = widget.value(QStringLiteral("children")).toArray();
            const auto updated = reviewWidgets(children, card);
            if (updated != children)
                widget.insert(QStringLiteral("children"), updated);
            if (widget != widgets.at(i))
                widgets[i] = widget;
        }
        return widgets;
    }
};

} // namespace

QList<ReferenceRow> stageCanSignalEdit(Project &draft, const QString &cardId, const CanSignalEdit &edit)
{
    QJsonObject card;
    visitWidgets(draft.widgets, [&](const QJsonObject &widget) {
        if (widget.value(QStringLiteral("id")).toString() != cardId)
            return true;
        card = widget;
        return false;
    });
    if (card.isEmpty())
        return {};

    auto props = card.value(QStringLiteral("properties")).toObject();
    const QString original = props.value(QStringLiteral("canSignals")).toString();
    const auto dbc = designerCanDatabase(&draft, props.value(QStringLiteral("dbcFile")).toString());
    const auto previous = CanSignalSet::build(dbc, original);
    const auto after = CanSignalSet::build(dbc, edit.text);
    if (!after)
        return {};
    const auto changes = previous ? signalChanges(*parseInlineSignals(original), *parseInlineSignals(edit.text), edit.renamedKeys)
                                  : QHash<QString, SignalChange>{};
    props.insert(QStringLiteral("canSignals"), edit.text);
    QList<ReferenceRow> rows;
    QSet<QString> dbcAliases;
    const ReferenceEdits references{draft, cardId, previous ? &*previous : nullptr, changes, dbcAliases, rows};
    if (!changes.isEmpty()) {
        if (dbc)
            for (const auto &message : dbc->messages)
                for (const auto &field : message.signalDefs)
                    dbcAliases.insert(field.name);
        QHash<QString, QString> mappingChanges;
        for (auto it = changes.cbegin(); it != changes.cend(); ++it)
            mappingChanges.insert(it.key(), it->replacement.isEmpty() ? it->name : it->replacement);
        const auto mapping = props.value(QStringLiteral("canTxInitialState")).toString();
        if (const auto updated = CanInitialState::migrateSignalMapping(*previous, *after, mapping, mappingChanges)) {
            props.insert(QStringLiteral("canTxInitialState"), *updated);
            QString error;
            const bool valid = bool(CanInitialState::build(*after, *updated, &error));
            rows.append({tr("连接卡"), widgetLabel(card), valid ? tr("发送初值保留原配对") : error});
        }
        auto points = draft.logic.value(QStringLiteral("points")).toArray();
        for (qsizetype i = 0; i < points.size(); ++i) {
            auto point = points.at(i).toObject();
            auto binding = point.value(QStringLiteral("binding")).toObject();
            const auto detail = references.reviewBinding(binding);
            if (!detail.isEmpty()) {
                point.insert(QStringLiteral("binding"), binding);
                points[i] = point;
                rows.append({tr("数据点"), point.value(QStringLiteral("name")).toString(), detail});
            }
        }
        if (points != draft.logic.value(QStringLiteral("points")).toArray())
            draft.logic.insert(QStringLiteral("points"), points);
    }
    draft.widgets = references.reviewWidgets(draft.widgets, props);
    return rows;
}

} // namespace fhmi
