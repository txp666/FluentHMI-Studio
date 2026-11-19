#include "designer/project_frame_templates.h"

#include "designer/reference_impact_dialog.h"

#include "core/binding.h"
#include "core/command_encoder.h"
#include "core/expression.h"
#include "core/frame_templates.h"
#include "core/i18n.h"
#include "core/project_logic.h"
#include "core/widget_meta.h"
#include "runtime/app_dialog.h"
#include "runtime/ui_helpers.h"

#include <FluentQtWidgets/Views/ItemViews.h>
#include <FluentQtWidgets/Widgets/Button.h>
#include <FluentQtWidgets/Widgets/ComboBox.h>
#include <FluentQtWidgets/Widgets/Label.h>
#include <FluentQtWidgets/Widgets/Menu.h>
#include <FluentQtWidgets/Widgets/ScrollArea.h>
#include <FluentQtWidgets/Widgets/Selection.h>
#include <QtCore/QJsonArray>
#include <QtCore/QCoreApplication>
#include <QtCore/QSet>
#include <QtCore/QSignalBlocker>
#include <QtCore/QUuid>
#include <QtWidgets/QApplication>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <tuple>

namespace fhmi {
namespace {
QString tr(const char *s) { return QCoreApplication::translate("fhmi", s); }

class LibraryPage : public QWidget
{
  public:
    using QWidget::QWidget;
    std::function<void()> refresh;

  protected:
    void showEvent(QShowEvent *event) override
    {
        QWidget::showEvent(event);
        refresh();
    }
};

QString formatLabel(QStringView parser)
{
    return parser == QLatin1String("hex") ? QStringLiteral("HEX") : tr("文本 / JSON");
}

QStringList sortedIds(const QJsonObject &library)
{
    auto ids = library.keys();
    std::sort(ids.begin(), ids.end(), [&](const QString &a, const QString &b) {
        return QString::localeAwareCompare(FrameTemplates::find(library, a)->name,
                                           FrameTemplates::find(library, b)->name) < 0;
    });
    return ids;
}

bool usesTemplate(const Project &project, const QString &id)
{
    if (project.serial.value(FrameTemplates::ReceiveId).toString() == id)
        return true;
    return !visitWidgets(project.widgets, [&](const QJsonObject &widget) {
        const auto props = widget.value(QStringLiteral("properties")).toObject();
        return !FrameTemplates::receiveIds(props).contains(id) && props.value(FrameTemplates::SendId).toString() != id;
    });
}

struct FieldChange
{
    QString replacement;
    QString label;
    bool unavailable = false;
    bool operator!=(const FieldChange &other) const
    {
        return replacement != other.replacement || label != other.label || unavailable != other.unavailable;
    }
};

struct FieldChanges
{
    QHash<QString, FieldChange> fields;
    bool framingChanged = false;

    QString updateReference(QJsonObject &object, const QString &property) const
    {
        const auto previous = object.value(property).toString();
        const auto change = fields.constFind(previous);
        if (change == fields.cend())
            return {};
        const bool renamed = !change->replacement.isEmpty();
        if (renamed)
            object.insert(property, change->replacement);
        if (change->unavailable)
            return tr("%1 · 切换后不可用").arg(change->label);
        if (!change->label.isEmpty())
            return renamed ? tr("%1 · 自动迁移").arg(change->label) : change->label;
        return renamed ? QStringLiteral("%1 → %2").arg(previous, change->replacement) : previous;
    }
};

struct ReceiveReferences
{
    const Project &project;
    const FieldChanges &changes;
    const QHash<QString, FieldChanges> &cards;
    const QString parser;
    bool projectDefault;

    QString reviewField(QJsonObject &object, const QString &property, const DataRoute &route, bool allSources,
                        const QString &mode = ParseMode::KeyValue) const
    {
        const auto key = object.value(property).toString();
        if (key.isEmpty() && mode != ParseMode::HexBytes)
            return {};
        const bool jsonField = mode == ParseMode::JsonPath && !key.contains(u'.');
        const bool byKey = mode == ParseMode::KeyValue || jsonField;
        const auto transport = bindingRouteTransport(mode);
        const FieldChanges *fields = nullptr;
        if (allSources) {
            const auto matches = routeConnections(project.widgets, transport, !route.endpoint.isEmpty(), parser);
            qsizetype bound = 0;
            for (const auto &match : matches) {
                const auto entry = cards.constFind(match.first);
                if (entry == cards.cend() || (!entry->framingChanged && (!byKey || !entry->fields.contains(key))))
                    continue;
                if (fields && byKey && fields->fields.value(key) != entry->fields.value(key))
                    return tr("读取字段 %1 · 多来源，需核对").arg(key);
                fields = &*entry;
                ++bound;
            }
            if (fields && bound != matches.size())
                return byKey ? tr("读取字段 %1 · 多来源，需核对").arg(key)
                             : tr("接收帧格式将变更，请核对取值");
        } else {
            const auto resolved = resolveProjectRoute(project.widgets, parser, transport, route, RoutePurpose::DataPoint);
            if (!resolved)
                return {};
            if (resolved->connectionId.isEmpty()) {
                if (projectDefault)
                    fields = &changes;
            } else if (auto entry = cards.constFind(resolved->connectionId); entry != cards.cend()) {
                fields = &*entry;
            }
        }
        if (!fields)
            return {};
        const auto updated = byKey ? fields->updateReference(object, property) : QString();
        if (jsonField && object.value(property).toString().contains(u'.')) {
            // A qualified HEX field is a literal key, not a path through nested JSON.
            object.insert(QStringLiteral("parseMode"), ParseMode::KeyValue);
            object.insert(QStringLiteral("key"), object.take(property));
        }
        if (updated.isEmpty() && fields->framingChanged)
            return tr("接收帧格式将变更，请核对取值");
        return updated.isEmpty() ? QString() : tr("读取字段 %1").arg(updated);
    }

    QString reviewBinding(QJsonObject &binding, bool allowBroadcast = false) const
    {
        const auto mode = binding.value(QStringLiteral("parseMode")).toString(ParseMode::KeyValue);
        if (mode != ParseMode::KeyValue && mode != ParseMode::JsonPath && mode != ParseMode::HexBytes)
            return {};
        const DataRoute route{binding.value(QStringLiteral("sourceCardId")).toString(),
                              binding.value(QStringLiteral("sourceEndpoint")).toString()};
        const auto property = mode == ParseMode::JsonPath ? QStringLiteral("jsonPath") : QStringLiteral("key");
        return reviewField(binding, property, route, allowBroadcast && route.isBroadcast(), mode);
    }
};

void reviewWidgetReceive(QJsonObject &widget, QJsonObject &props, const ReceiveReferences &receive,
                         const QSet<QString> &affectedPoints, QList<ReferenceRow> &rows)
{
    const auto type = widget.value(QStringLiteral("type")).toString();
    const auto *meta = WidgetRegistry::instance().find(type);
    const auto reviewBinding = [&](QJsonObject &binding) {
        const auto key = binding.value(QStringLiteral("key")).toString();
        if (binding.value(QStringLiteral("parseMode")).toString() == ParseMode::DataPoint && affectedPoints.contains(key))
            return tr("使用数据点 %1").arg(key);
        return receive.reviewBinding(binding, true);
    };
    if (meta && widgetReadsStoredBinding(*meta, props)) {
        const auto original = widget.value(QStringLiteral("binding")).toObject();
        auto binding = original;
        const auto detail = reviewBinding(binding);
        if (!detail.isEmpty())
            rows.append({tr("显示控件"), widgetLabel(widget), detail});
        if (binding != original)
            widget.insert(QStringLiteral("binding"), binding);
    }
    if (type == QLatin1String("DataTable")) {
        const DataRoute route{props.value(QStringLiteral("sourceCardId")).toString(),
                              props.value(QStringLiteral("sourceEndpoint")).toString()};
        const auto original = props.value(QStringLiteral("recordColumns")).toArray();
        auto columns = original;
        QStringList details;
        const bool fromPoints = props.value(QStringLiteral("recordSource")).toString() == QLatin1String("points");
        const auto trigger = props.value(QStringLiteral("recordTriggerPoint")).toString();
        if (fromPoints && affectedPoints.contains(trigger))
            details.append(tr("使用数据点 %1").arg(trigger));
        const bool allSources = route.connectionId.isEmpty() || route.isBroadcast();
        for (qsizetype i = 0; i < columns.size(); ++i) {
            auto column = columns.at(i).toObject();
            const auto field = column.value(QStringLiteral("field")).toString();
            if (fromPoints) {
                if (affectedPoints.contains(field))
                    details.append(tr("使用数据点 %1").arg(field));
                continue;
            }
            if (field == QLatin1String("@time"))
                continue;
            const auto detail = receive.reviewField(column, QStringLiteral("field"), route, allSources);
            if (!detail.isEmpty()) {
                details.append(detail);
                columns[i] = column;
            }
        }
        if (!details.isEmpty()) {
            details.removeDuplicates();
            rows.append({tr("数据表格"), widgetLabel(widget), details.join(QStringLiteral(" · "))});
        }
        if (columns != original)
            props.insert(QStringLiteral("recordColumns"), columns);
    }
    if (type != QLatin1String("RealtimeChart"))
        return;
    for (int i = 0; i < 8; ++i) {
        const auto original = chartCurveBinding(props, i);
        auto binding = original;
        const auto detail = reviewBinding(binding);
        if (detail.isEmpty())
            continue;
        const auto prefix = QStringLiteral("_curve_%1_").arg(i);
        const auto name = props.value(prefix + QStringLiteral("name")).toString().trimmed();
        const auto label = name.isEmpty() ? tr("曲线%1").arg(i + 1) : QStringLiteral("%1 (%2)").arg(name).arg(i + 1);
        rows.append({tr("实时曲线"), QStringLiteral("%1 · %2").arg(widgetLabel(widget), label), detail});
        for (const auto &property : {QStringLiteral("parseMode"), QStringLiteral("key"), QStringLiteral("jsonPath")})
            if (binding.value(property) != original.value(property))
                props.insert(prefix + property, binding.value(property));
    }
}

QJsonArray reviewWidgetReferences(QJsonArray widgets, const ReceiveReferences &receive,
                                  const QSet<QString> &affectedPoints, QList<ReferenceRow> &rows,
                                  const QString &id = {}, const FieldChanges &send = {})
{
    for (qsizetype index = 0; index < widgets.size(); ++index) {
        auto widget = widgets.at(index).toObject();
        const auto type = widget.value(QStringLiteral("type")).toString();
        const auto original = widget.value(QStringLiteral("properties")).toObject();
        auto props = original;
        reviewWidgetReceive(widget, props, receive, affectedPoints, rows);
        const bool active = widgetSendsCommands(type, props);
        QStringList uses;
        if (!id.isEmpty() && props.value(FrameTemplates::SendId).toString() == id) {
            QStringList mapped;
            const auto originalBindings = props.value(FrameTemplates::SendBindings).toArray();
            auto bindings = originalBindings;
            for (qsizetype i = 0; i < bindings.size(); ++i) {
                auto binding = bindings.at(i).toObject();
                const auto field = send.updateReference(binding, QStringLiteral("field"));
                if (!field.isEmpty()) {
                    mapped.append(field);
                    bindings[i] = binding;
                }
            }
            if (bindings != originalBindings)
                props.insert(FrameTemplates::SendBindings, bindings);
            const auto direct = send.updateReference(props, FrameTemplates::SendValueField);
            if (!direct.isEmpty())
                mapped.append(direct);
            mapped.removeDuplicates();
            if (!mapped.isEmpty())
                uses.append(tr("写入字段 %1").arg(mapped.join(QStringLiteral(", "))));
            else if (active && FrameTemplates::sendSettings(receive.project.frameTemplates, props))
                uses.append(tr("使用此发送模板"));
        }
        if (active && type == QLatin1String("PushButton")) {
            QStringList inputs;
            for (const auto &input : CommandEncoder::commandInputIds(props)) {
                const auto point = pointNameForInput(input);
                if (affectedPoints.contains(point))
                    inputs.append(point);
            }
            inputs.removeDuplicates();
            if (!inputs.isEmpty())
                uses.append(tr("使用数据点 %1").arg(inputs.join(QStringLiteral(", "))));
        }
        if (!uses.isEmpty())
            rows.append({tr("发送控件"), widgetLabel(widget), uses.join(QStringLiteral(" · "))});
        if (props != original)
            widget.insert(QStringLiteral("properties"), props);
        const auto children = widget.value(QStringLiteral("children")).toArray();
        const auto updated = reviewWidgetReferences(children, receive, affectedPoints, rows, id, send);
        if (updated != children)
            widget.insert(QStringLiteral("children"), updated);
        if (widget != widgets.at(index))
            widgets[index] = widget;
    }
    return widgets;
}

void reviewReferences(Project &updated, const ReceiveReferences &receive, QList<ReferenceRow> &rows,
                      const QString &sendTemplate = {}, const FieldChanges &send = {})
{
    QSet<QString> affectedPoints;
    const auto &project = receive.project;
    const auto originalPoints = project.logic.value(QStringLiteral("points")).toArray();
    auto points = originalPoints;
    for (qsizetype i = 0; i < points.size(); ++i) {
        auto definition = points.at(i).toObject();
        auto binding = definition.value(QStringLiteral("binding")).toObject();
        const auto detail = receive.reviewBinding(binding);
        if (detail.isEmpty())
            continue;
        const auto name = definition.value(QStringLiteral("name")).toString();
        rows.append({tr("数据点"), name, detail});
        affectedPoints.insert(name);
        if (binding != definition.value(QStringLiteral("binding")).toObject()) {
            definition.insert(QStringLiteral("binding"), binding);
            points[i] = definition;
        }
    }
    if (points != originalPoints)
        updated.logic.insert(QStringLiteral("points"), points);
    if (!affectedPoints.isEmpty()) {
        const auto dependenciesIn = [&](const QString &expression) {
            QStringList dependencies;
            for (const auto &reference : Expression::references(expression))
                if (affectedPoints.contains(reference))
                    dependencies.append(reference);
            return dependencies;
        };
        for (const auto &name : dataPointOrder(project.logic)) {
            const auto expression = findDataPoint(project.logic, name).value(QStringLiteral("expression")).toString();
            const auto dependencies = dependenciesIn(expression);
            if (!dependencies.isEmpty()) {
                affectedPoints.insert(name);
                rows.append({tr("数据点"), name, tr("依赖数据点 %1").arg(dependencies.join(QStringLiteral(", ")))});
            }
        }
        for (const auto &value : project.logic.value(QStringLiteral("alarms")).toArray()) {
            const auto alarm = value.toObject();
            const auto dependencies = dependenciesIn(alarm.value(QStringLiteral("condition")).toString());
            if (!dependencies.isEmpty())
                rows.append({tr("报警"), alarm.value(QStringLiteral("id")).toString(),
                             tr("依赖数据点 %1").arg(dependencies.join(QStringLiteral(", ")))});
        }
    }

    updated.widgets = reviewWidgetReferences(updated.widgets, receive, affectedPoints, rows, sendTemplate, send);
}

struct PublishedFields
{
    struct Field
    {
        QString identity;
        QString label;
    };
    QHash<QString, Field> byKey;
    QHash<QString, QString> keyByIdentity;
};

PublishedFields publishedFields(const LinkParser::Format &format, const QString &templateId,
                               const QJsonObject &library)
{
    PublishedFields result;
    if (format.type == LinkParser::HexGroup) {
        if (const auto variants = hexFrameVariants(format.settings))
            for (const auto &variant : *variants) {
                for (const auto &field : variant.frame.fields) {
                    const auto key = hexTemplateFieldKey(variant.id, field.name);
                    const PublishedFields::Field published{key, QStringLiteral("%1 · %2").arg(variant.name, field.name)};
                    result.keyByIdentity.insert(key, key);
                    result.byKey.insert(key, published);
                    if (variant.unqualifiedFields.contains(field.name))
                        result.byKey.insert(field.name, published);
                }
            }
    } else if (format.type == QLatin1String("hex")) {
        const auto entry = FrameTemplates::find(library, templateId);
        if (const auto config = hexFrameConfig(format.settings))
            for (const auto &field : config->fields) {
                const auto key = entry ? hexTemplateFieldKey(templateId, field.name) : QString();
                result.byKey.insert(field.name, {key, entry ? QStringLiteral("%1 · %2").arg(entry->name, field.name)
                                                            : field.name});
                if (entry)
                    result.keyByIdentity.insert(key, field.name);
            }
    }
    return result;
}

FieldChanges receiveFieldChanges(const PublishedFields &before, const PublishedFields &after,
                                  const QHash<QString, FieldChange> &edits = {}, bool framingChanged = true)
{
    FieldChanges changes;
    changes.framingChanged = framingChanged;
    for (auto it = before.byKey.cbegin(); it != before.byKey.cend(); ++it) {
        const auto &key = it.key();
        const auto &field = it.value();
        const auto edit = edits.constFind(field.identity);
        const QString identity = edit != edits.cend() && !edit->replacement.isEmpty()
            ? edit->replacement : field.identity;
        QString next;
        if (edit == edits.cend() || !edit->unavailable) {
            if (const auto match = after.byKey.constFind(key);
                match != after.byKey.cend() && (field.identity.isEmpty() || match->identity == identity))
                next = key;
            else if (!identity.isEmpty())
                next = after.keyByIdentity.value(identity);
        }
        if (edit == edits.cend() && next == key)
            continue;
        // Remember a removed field's identity so a same-named field from another template cannot take its place.
        const auto replacement = next.isEmpty() ? identity : next;
        changes.fields.insert(key, {replacement != key ? replacement : QString(),
            edit == edits.cend() ? field.label : edit->label, next.isEmpty()});
    }
    // A previously removed reference can become available again when switching to a single template.
    for (auto it = after.keyByIdentity.cbegin(); it != after.keyByIdentity.cend(); ++it) {
        if (it.key() != it.value() && !before.byKey.contains(it.key()))
            changes.fields.insert(it.key(), {it.value(), after.byKey.value(it.value()).label});
    }
    return changes;
}

std::optional<Project> confirmFieldChanges(const Project &project, const QString &id, const FrameTemplates::Entry &entry,
    const LinkParser::Format &edited, const QList<qsizetype> &origins, QWidget *parent)
{
    if (entry.format.type != QLatin1String("hex"))
        return project;
    const auto before = hexFrameConfig(entry.format.settings);
    const auto after = hexFrameConfig(edited.settings);
    if (!before || !after)
        return project;

    QList<const HexField *> fieldsByOrigin(before->fields.size(), nullptr);
    for (qsizetype i = 0; i < origins.size(); ++i)
        if (origins.at(i) >= 0)
            fieldsByOrigin[origins.at(i)] = &after->fields.at(i);
    QHash<QString, int> nameCounts;
    for (const auto &field : before->fields)
        ++nameCounts[field.name];
    QList<ReferenceRow> rows;
    FieldChanges changedFields;
    bool needsBindingReview = false;
    for (qsizetype i = 0; i < before->fields.size(); ++i) {
        const auto &field = before->fields.at(i);
        const auto *current = fieldsByOrigin.at(i);
        QString label = field.name;
        QStringList changes;
        FieldChange change;
        if (!current) {
            changes.append(tr("已删除"));
            change.unavailable = true;
            needsBindingReview = true;
        } else {
            const bool renamed = current->name != field.name;
            const bool typeChanged = current->type != field.type;
            if (renamed) {
                label = QStringLiteral("%1 → %2").arg(field.name, current->name);
                if (!field.name.isEmpty() && nameCounts.value(field.name) == 1)
                    change.replacement = current->name;
                else
                    needsBindingReview = true;
            }
            if (typeChanged)
                changes.append(QStringLiteral("%1 → %2").arg(translated(field.type), translated(current->type)));
            if (current->offset != field.offset || current->littleEndian != field.littleEndian ||
                (!typeChanged && current->length != field.length))
                changes.append(tr("字节布局变更"));
            if (renamed && changes.isEmpty())
                changes.append(tr("字段更名"));
        }
        if (!changes.isEmpty()) {
            if (change.replacement.isEmpty())
                change.label = changes.join(QStringLiteral(" · "));
            changedFields.fields.insert(field.name, change);
            rows.append({tr("字段变更"), label, changes.join(QStringLiteral(" · "))});
        }
    }
    const auto framing = [](const HexFrameConfig &frame) {
        return std::tie(frame.header, frame.footer, frame.matchBytes, frame.matchOffset, frame.fixedLength,
                        frame.lengthOffset, frame.lengthSize, frame.lengthLittleEndian, frame.lengthCovers,
                        frame.checksum, frame.checksumStart, frame.checksumLittleEndian,
                        frame.idleTimeout, frame.maxFrameLength);
    };
    const bool framingChanged = framing(*before) != framing(*after);
    const bool sendChanged = FrameTemplates::requiredFields(entry.format.settings) != FrameTemplates::requiredFields(edited.settings) ||
        entry.format.settings.value(FrameTemplates::SendDefaults) != edited.settings.value(FrameTemplates::SendDefaults);
    if (rows.isEmpty() && before->fields.size() == after->fields.size() && !framingChanged && !sendChanged)
        return project;
    const auto changeCount = rows.size();

    QHash<QString, FieldChange> edits;
    for (auto it = changedFields.fields.cbegin(); it != changedFields.fields.cend(); ++it) {
        auto change = it.value();
        if (!change.replacement.isEmpty())
            change.replacement = hexTemplateFieldKey(id, change.replacement);
        edits.insert(hexTemplateFieldKey(id, it.key()), change);
    }
    Project updated = project;
    updated.frameTemplates.insert(id, FrameTemplates::Entry{entry.name, edited, entry.sample}.toJson());
    const bool projectDefault = project.serial.value(FrameTemplates::ReceiveId).toString() == id;
    const auto defaults = projectDefault ? receiveFieldChanges(
        publishedFields(project.receiveFormat(), id, project.frameTemplates),
        publishedFields(updated.receiveFormat(), id, updated.frameTemplates), edits, framingChanged) : FieldChanges{};
    if (projectDefault)
        rows.append({tr("工程设置"), tr("默认接收帧格式"), tr("使用此接收模板")});
    QHash<QString, FieldChanges> receivingCards;
    visitWidgets(project.widgets, [&](const QJsonObject &widget) {
        const auto props = widget.value(QStringLiteral("properties")).toObject();
        if (supportsRouteTransport(widget.value(QStringLiteral("type")).toString(), RouteTransport::Bytes) &&
            project.linkReceiveTemplateIds(props).contains(id)) {
            const auto templateId = project.linkReceiveTemplateId(props);
            receivingCards.insert(widget.value(QStringLiteral("id")).toString(), receiveFieldChanges(
                publishedFields(project.linkReceiveFormat(props), templateId, project.frameTemplates),
                publishedFields(updated.linkReceiveFormat(props), templateId, updated.frameTemplates), edits, framingChanged));
            rows.append({tr("连接卡"), widgetLabel(widget), tr("使用此接收模板")});
        }
        return true;
    });

    reviewReferences(updated, {project, defaults, receivingCards, project.parserType(), projectDefault},
                     rows, id, changedFields);
    if (rows.size() == changeCount)
        return updated;

    auto message = tr("请核对「%1」的字段改动及引用。").arg(entry.name);
    if (updated.logic != project.logic || updated.widgets != project.widgets)
        message += QLatin1Char(' ') + tr("保存时会自动更新此模板的直接字段引用。");
    if (needsBindingReview)
        message += QLatin1Char(' ') + tr("请检查已删除字段或原名称重复的绑定。");
    if (!confirmReferences(parent, tr("模板修改影响范围"), message, rows, tr("继续保存"), tr("返回编辑")))
        return std::nullopt;
    return updated;
}

void remapClipboardTemplates(QJsonArray &widgets, const QHash<QString, QString> &ids)
{
    for (qsizetype index = 0; index < widgets.size(); ++index) {
        auto widget = widgets.at(index).toObject();
        const auto original = widget.value(QStringLiteral("properties")).toObject();
        auto props = original;
        for (const auto &key : {FrameTemplates::ReceiveId, FrameTemplates::SendId}) {
            const auto id = props.value(key).toString();
            if (ids.contains(id))
                props.insert(key, ids.value(id));
        }
        if (auto selection = props.value(FrameTemplates::ReceiveIds); selection.isArray()) {
            auto group = selection.toArray();
            for (qsizetype i = 0; i < group.size(); ++i) {
                const auto id = group.at(i).toString();
                if (ids.contains(id))
                    group[i] = ids.value(id);
            }
            props.insert(FrameTemplates::ReceiveIds, group);
        }
        if (props != original)
            widget.insert(QStringLiteral("properties"), props);
        if (widget.contains(QStringLiteral("children"))) {
            auto children = widget.value(QStringLiteral("children")).toArray();
            remapClipboardTemplates(children, ids);
            widget.insert(QStringLiteral("children"), children);
        }
        widgets[index] = widget;
    }
}

} // namespace

QHash<QString, QString> mergeFrameTemplateLibrary(QJsonObject &library, const QJsonObject &incoming)
{
    const QString nameKey = QStringLiteral("name");
    QSet<QString> names;
    for (const auto &templateValue : std::as_const(library))
        names.insert(templateValue.toObject().value(nameKey).toString());
    QHash<QString, QString> ids;
    for (auto it = incoming.constBegin(); it != incoming.constEnd(); ++it) {
        auto value = it.value().toObject();
        const QString name = value.value(nameKey).toString();
        auto id = it.key();
        if (library.contains(id)) {
            if (library.value(id) == it.value())
                continue;
            // Repeated pastes can reuse the imported definition, including its renamed display label.
            auto format = value;
            format.remove(nameKey);
            QString reusedId;
            for (auto current = library.constBegin(); current != library.constEnd(); ++current) {
                auto other = current.value().toObject();
                const QString label = other.take(nameKey).toString();
                if ((current.key() == id || label == name || label.startsWith(name + QStringLiteral(" ("))) &&
                    other == format) {
                    reusedId = current.key();
                    break;
                }
            }
            if (!reusedId.isEmpty()) {
                if (reusedId != id)
                    ids.insert(id, reusedId);
                continue;
            }
            id = QUuid::createUuid().toString(QUuid::WithoutBraces);
            ids.insert(it.key(), id);
        }
        if (!name.isEmpty()) {
            QString label = name;
            for (int suffix = 2; names.contains(label); ++suffix)
                label = QStringLiteral("%1 (%2)").arg(name).arg(suffix);
            names.insert(label);
            value.insert(nameKey, label);
        }
        library.insert(id, it.value().isObject() ? QJsonValue(value) : it.value());
    }
    return ids;
}

QJsonObject importClipboardFrameTemplates(Project &clipboard, QJsonObject library)
{
    const auto ids = mergeFrameTemplateLibrary(library, clipboard.frameTemplates);
    if (ids.isEmpty())
        return library;

    const Project before = clipboard;
    clipboard.frameTemplates = library;
    remapClipboardTemplates(clipboard.widgets, ids);
    QHash<QString, FieldChanges> cards;
    visitWidgets(before.widgets, [&](const QJsonObject &widget) {
        if (!supportsRouteTransport(widget.value(QStringLiteral("type")).toString(), RouteTransport::Bytes))
            return true;
        const auto props = widget.value(QStringLiteral("properties")).toObject();
        auto receive = before.linkReceiveTemplateIds(props);
        const auto original = receive;
        for (auto &id : receive)
            id = ids.value(id, id);
        if (receive == original)
            return true;

        const auto oldFields = publishedFields(before.linkReceiveFormat(props), before.linkReceiveTemplateId(props),
                                               before.frameTemplates);
        const auto after = FrameTemplates::linkFormat(library, receive, {}, {}, before.receiveFormat());
        const auto newFields = publishedFields(after, receive.size() == 1 ? receive.first() : QString(), library);
        QHash<QString, FieldChange> edits;
        for (auto it = oldFields.keyByIdentity.cbegin(); it != oldFields.keyByIdentity.cend(); ++it) {
            for (auto moved = ids.cbegin(); moved != ids.cend(); ++moved) {
                const auto prefix = hexTemplateFieldKey(moved.key(), {});
                if (it.key().startsWith(prefix)) {
                    edits.insert(it.key(), {hexTemplateFieldKey(moved.value(), it.key().mid(prefix.size())), {}});
                    break;
                }
            }
        }
        cards.insert(widget.value(QStringLiteral("id")).toString(), receiveFieldChanges(oldFields, newFields, edits, false));
        return true;
    });
    QList<ReferenceRow> ignored;
    clipboard.widgets = reviewWidgetReferences(clipboard.widgets,
        {before, {}, cards, before.parserType(), false}, {}, ignored);
    return library;
}

bool confirmReceiveTemplateSelection(const Project &before, Project &draft, const QString &connectionId, QWidget *parent)
{
    const bool projectDefault = connectionId.isEmpty();
    const auto &library = draft.frameTemplates;
    const auto changes = projectDefault
        ? receiveFieldChanges(publishedFields(before.receiveFormat(), before.serial.value(FrameTemplates::ReceiveId).toString(), library),
                                   publishedFields(draft.receiveFormat(), draft.serial.value(FrameTemplates::ReceiveId).toString(), library))
        : FieldChanges{};
    QHash<QString, QJsonObject> originalCards;
    visitWidgets(before.widgets, [&](const QJsonObject &widget) {
        if (supportsRouteTransport(widget.value(QStringLiteral("type")).toString(), RouteTransport::Bytes))
            originalCards.insert(widget.value(QStringLiteral("id")).toString(), widget.value(QStringLiteral("properties")).toObject());
        return true;
    });
    QList<ReferenceRow> sources;
    if (projectDefault)
        sources.append({tr("工程设置"), tr("默认接收帧格式"), tr("切换接收模板")});
    QHash<QString, FieldChanges> cards;
    visitWidgets(draft.widgets, [&](const QJsonObject &widget) {
        const auto id = widget.value(QStringLiteral("id")).toString();
        if ((!projectDefault && id != connectionId) || !originalCards.contains(id))
            return true;
        const auto oldProps = originalCards.value(id);
        const auto props = widget.value(QStringLiteral("properties")).toObject();
        const auto oldIds = before.linkReceiveTemplateIds(oldProps);
        const auto newIds = draft.linkReceiveTemplateIds(props);
        if (oldIds != newIds) {
            cards.insert(id, receiveFieldChanges(
                publishedFields(before.linkReceiveFormat(oldProps), before.linkReceiveTemplateId(oldProps), library),
                publishedFields(draft.linkReceiveFormat(props), draft.linkReceiveTemplateId(props), library)));
            sources.append({tr("连接卡"), widgetLabel(widget), tr("切换接收模板")});
        }
        return true;
    });

    Project updated = draft;
    QList<ReferenceRow> references;
    reviewReferences(updated, {before, changes, cards, before.parserType(), projectDefault}, references);
    if (references.isEmpty())
        return true;
    sources.append(references);
    if (!confirmReferences(parent, tr("接收模板切换影响范围"), tr("保留的字段引用会自动迁移。请检查标记为不可用或需要核对的引用。"),
                           sources, tr("继续切换"), tr("取消")))
        return false;
    draft = std::move(updated);
    return true;
}

void fillFrameTemplates(FluentQt::ComboBox *box, const QJsonObject &library, const QString &selected,
                        const QString &automaticLabel, QStringView parser)
{
    const QSignalBlocker blocker(box);
    box->clear();
    box->addItem(automaticLabel, QIcon(), QString());
    for (const auto &id : sortedIds(library)) {
        const auto entry = *FrameTemplates::find(library, id);
        if (!parser.isEmpty() && parser != entry.format.type)
            continue;
        box->addItem(parser.isEmpty() ? entry.name + QStringLiteral(" · ") + formatLabel(entry.format.type)
                                     : entry.name, QIcon(), id);
    }
    if (!selectValue(box, selected))
        box->setCurrentIndex(0);
}

bool hasMultipleHexTemplates(const QJsonObject &library)
{
    int count = 0;
    for (const auto &id : library.keys())
        if (FrameTemplates::find(library, id)->format.type == QLatin1String("hex") && ++count == 2)
            return true;
    return false;
}

void editReceiveTemplateSelection(const QJsonObject &library, const QStringList &selected, QWidget *parent,
                                  const std::function<bool(const QStringList &)> &apply, const QString &connectionId)
{
    const bool groupOnly = connectionId.isEmpty();
    auto available = sortedIds(library);
    if (groupOnly)
        available.removeIf([&](const QString &id) {
            return FrameTemplates::find(library, id)->format.type != QLatin1String("hex");
        });
    AppDialog dialog(groupOnly ? tr("选择接收模板组") : tr("更改接收模板 · %1").arg(connectionId), parent);
    dialog.setObjectName(QStringLiteral("receiveTemplateGroupDialog"));
    dialog.setPreferredSize(QSize(560, int(std::clamp<qsizetype>((groupOnly ? 168 : 206) + 52 * available.size(), 272, 500))));
    if (!groupOnly) {
        auto *note = new FluentQt::BodyLabel(tr("未勾选时使用连接的解析设置。"), dialog.widget());
        note->setWordWrap(true);
        dialog.viewLayout()->addWidget(note);
    }
    auto *scroll = new FluentQt::ScrollArea(dialog.widget());
    scroll->setWidgetResizable(true);
    scroll->enableTransparentBackground();
    auto *list = new QWidget(scroll);
    auto *layout = new QVBoxLayout(list);
    layout->setContentsMargins(2, 0, 8, 0);
    QList<QPair<QString, FluentQt::CheckBox *>> choices;
    for (const auto &id : available) {
        const auto entry = FrameTemplates::find(library, id);
        auto *box = new FluentQt::CheckBox(entry->name, list);
        box->setObjectName(QStringLiteral("receiveTemplateCheck_") + id);
        box->setChecked(selected.contains(id));
        layout->addWidget(box);
        choices.append({id, box});
        if (const auto config = hexFrameConfig(entry->format.settings)) {
            QStringList markers;
            if (!config->header.isEmpty())
                markers.append(tr("帧头 %1").arg(toHexString(config->header)));
            if (!config->matchBytes.isEmpty())
                markers.append(tr("字节 %1 = %2").arg(config->matchOffset).arg(toHexString(config->matchBytes)));
            if (config->fixedLength > 0)
                markers.append(tr("固定 %1 B").arg(config->fixedLength));
            else if (config->lengthOffset >= 0)
                markers.append(tr("读取长度字段"));
            else if (!config->footer.isEmpty())
                markers.append(tr("帧尾 %1").arg(toHexString(config->footer)));
            else
                markers.append(tr("空闲 %1 ms").arg(config->idleTimeout));
            if (config->checksum != HexChecksum::None)
                markers.append(translated(HexOptions::checksums().at(int(config->checksum))));
            auto *details = new FluentQt::CaptionLabel(markers.join(QStringLiteral(" · ")), list);
            details->setContentsMargins(26, 0, 0, 4);
            details->setWordWrap(true);
            layout->addWidget(details);
        }
    }
    layout->addStretch();
    scroll->setWidget(list);
    dialog.viewLayout()->addWidget(scroll, 1);
    auto *problem = new FluentQt::CaptionLabel(dialog.widget());
    problem->setWordWrap(true);
    problem->hide();
    dialog.viewLayout()->addWidget(problem);
    dialog.setValidator([&] {
        QStringList ids;
        for (const auto &[id, checkbox] : choices)
            if (checkbox->isChecked())
                ids.append(id);
        const auto message = ids.size() >= 2 ? FrameTemplates::receiveGroupProblem(library, QJsonArray::fromStringList(ids))
            : groupOnly ? tr("请选择至少两个 HEX 接收模板") : QString();
        problem->setText(message);
        problem->setVisible(!message.isEmpty());
        return message.isEmpty() && apply(ids);
    });
    dialog.exec();
}

QString chooseReceiveTemplate(const QJsonObject &group, const QString &fieldKey, QWidget *anchor)
{
    FluentQt::RoundMenu menu;
    for (const auto &value : group.value(LinkParser::GroupTemplates).toArray()) {
        const auto entry = value.toObject();
        const auto id = entry.value(QStringLiteral("id")).toString();
        auto *action = menu.addAction(entry.value(QStringLiteral("name")).toString());
        action->setData(id);
        if (fieldKey.startsWith(hexTemplateFieldKey(id, {}))) {
            action->setCheckable(true);
            action->setChecked(true);
        }
    }
    const auto *picked = menu.execAndWait(anchor->mapToGlobal(QPoint(0, anchor->height())));
    return picked ? picked->data().toString() : QString();
}

std::optional<ReceiveFrameEdit> editProjectFrameTemplate(Project &draft, const QString &id, LinkParser::Format seed,
    const QString &scope, const std::optional<ReceiveValueSettings> &values, QWidget *parent)
{
    FrameEditorOptions options;
    const auto &library = draft.frameTemplates;
    for (const auto &other : library.keys()) {
        if (other != id)
            options.takenNames.append(FrameTemplates::find(library, other)->name);
    }
    const auto entry = FrameTemplates::find(library, id);
    std::optional<Project> reviewed;
    if (entry) {
        seed = entry->format;
        options.templateName = entry->name;
        options.sample = entry->sample;
        options.lockParser = true;
        options.formatProblem = [&](const LinkParser::Format &format) {
            auto proposed = library;
            proposed.insert(id, FrameTemplates::Entry{entry->name, format, entry->sample}.toJson());
            QString error;
            visitWidgets(draft.widgets, [&](const QJsonObject &widget) {
                const auto props = widget.value(QStringLiteral("properties")).toObject();
                const auto group = props.value(FrameTemplates::ReceiveIds);
                if (!group.toArray().isEmpty() && FrameTemplates::receiveIds(props).contains(id)) {
                    const auto problem = FrameTemplates::receiveGroupProblem(proposed, group);
                    if (!problem.isEmpty()) {
                        error = tr("连接 %1 · %2").arg(widgetLabel(widget), problem);
                        return false;
                    }
                }
                return true;
            });
            return error;
        };
        options.confirmChanges = [&](const LinkParser::Format &format, const QList<qsizetype> &origins, QWidget *owner) {
            reviewed = confirmFieldChanges(draft, id, *entry, format, origins, owner);
            return reviewed.has_value();
        };
    } else {
        int suffix = 1;
        do {
            options.templateName = tr("帧模板 %1").arg(suffix++);
        } while (options.takenNames.contains(options.templateName));
    }
    if (!LinkParser::hasEditableFraming(seed.type))
        seed = {QStringLiteral("hex"), {}};
    const auto edited = editReceiveFrameFormat(seed, scope, values, parent, options);
    if (!edited)
        return std::nullopt;
    if (reviewed)
        draft = std::move(*reviewed);
    draft.frameTemplates.insert(id, FrameTemplates::Entry{edited->templateName, edited->format, edited->sample}.toJson());
    return edited;
}

QWidget *createFrameTemplateLibrary(Project *draft, QWidget *parent, LinkParser::Format seed, const QString &selected,
                                   std::function<void()> onSaved)
{
    auto *page = new LibraryPage(parent);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    auto *table = new FluentQt::TableWidget(page);
    table->setObjectName(QStringLiteral("projectFrameTemplates"));
    table->setColumnCount(2);
    table->setHorizontalHeaderLabels({tr("模板名称"), tr("帧格式")});
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    table->verticalHeader()->hide();
    table->verticalHeader()->setDefaultSectionSize(36);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    layout->addWidget(table, 1);
    auto *bar = new QHBoxLayout;
    auto *add = new FluentQt::PushButton(tr("新建模板…"), page);
    add->setObjectName(QStringLiteral("addProjectFrameTemplate"));
    auto *edit = new FluentQt::PushButton(tr("编辑所选"), page);
    auto *remove = new FluentQt::PushButton(tr("删除所选"), page);
    for (auto *action : {add, edit, remove})
        bar->addWidget(action);
    auto *message = new FluentQt::CaptionLabel(page);
    message->setWordWrap(true);
    bar->addWidget(message, 1);
    layout->addLayout(bar);
    const auto selectedId = [table] {
        const auto *cell = table->item(table->currentRow(), 0);
        return cell ? cell->data(Qt::UserRole).toString() : QString();
    };
    const auto sync = [selectedId, edit, remove, message] {
        const bool selected = !selectedId().isEmpty();
        edit->setEnabled(selected);
        remove->setEnabled(selected);
        message->clear();
    };
    const auto fill = [draft, table, sync](const QString &current) {
        const QSignalBlocker blocker(table);
        table->setRowCount(0);
        for (const auto &id : sortedIds(draft->frameTemplates)) {
            const auto entry = *FrameTemplates::find(draft->frameTemplates, id);
            const int row = table->rowCount();
            table->insertRow(row);
            auto *name = new QTableWidgetItem(entry.name);
            name->setData(Qt::UserRole, id);
            table->setItem(row, 0, name);
            table->setItem(row, 1, new QTableWidgetItem(formatLabel(entry.format.type)));
            if (id == current)
                table->setCurrentCell(row, 0);
        }
        sync();
    };
    const auto editTemplate = [draft, page, fill, seed = std::move(seed), onSaved = std::move(onSaved)](QString id) {
        if (id.isEmpty())
            id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        auto *host = QApplication::activeModalWidget();
        if (!editProjectFrameTemplate(*draft, id, seed, tr("引用此模板的接收和发送共用这份定义。"),
                std::nullopt, host ? host : page->window()))
            return;
        if (onSaved)
            onSaved();
        fill(id);
    };
    QObject::connect(table, &QTableWidget::itemSelectionChanged, page, sync);
    QObject::connect(table, &QTableWidget::cellDoubleClicked, page, [editTemplate, selectedId] { editTemplate(selectedId()); });
    QObject::connect(add, &QPushButton::clicked, page, [editTemplate] { editTemplate({}); });
    QObject::connect(edit, &QPushButton::clicked, page, [editTemplate, selectedId] { editTemplate(selectedId()); });
    QObject::connect(remove, &QPushButton::clicked, page, [draft, selectedId, fill, message] {
        const QString id = selectedId();
        if (usesTemplate(*draft, id)) {
            message->setText(tr("此模板正在使用，请先更改引用它的设置。"));
            return;
        }
        draft->frameTemplates.remove(id);
        fill({});
    });
    page->refresh = [fill, selectedId] { fill(selectedId()); };
    fill(selected);
    return page;
}

bool editFrameTemplateLibrary(Project *project, QWidget *parent, const LinkParser::Format &seed, const QString &selected)
{
    Project draft = *project;
    AppDialog dialog(tr("工程帧格式模板"), parent);
    dialog.setObjectName(QStringLiteral("projectFrameTemplateLibrary"));
    dialog.setPreferredSize(QSize(670, 510));
    dialog.viewLayout()->addWidget(createFrameTemplateLibrary(&draft, &dialog, seed, selected), 1);
    if (dialog.exec() != QDialog::Accepted ||
        (draft.frameTemplates == project->frameTemplates && draft.widgets == project->widgets && draft.logic == project->logic))
        return false;
    *project = std::move(draft);
    return true;
}

} // namespace fhmi
