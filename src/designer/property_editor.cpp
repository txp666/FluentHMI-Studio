#include "designer/property_editor.h"
#include "designer/can_signal_editor.h"
#include "designer/command_preview.h"
#include "designer/connection_source_edit.h"
#include "designer/enum_mapping_editor.h"
#include "designer/frame_template_editor.h"
#include "designer/project_can_signals.h"
#include "designer/project_logic_dialog.h"
#include "designer/project_frame_templates.h"
#include "designer/receive_frame_editor.h"
#include "designer/receive_frame_fields.h"
#include "designer/transmit_frame_editor.h"

#include "comm/modbus_slave.h"
#include "comm/can/can_backend.h"
#include "comm/serial_link.h"
#include "core/binding.h"
#include "core/chart_limits.h"
#include "core/bit_profile.h"
#include "core/command_encoder.h"
#include "core/industrial_catalog.h"
#include "core/frame_templates.h"
#include "core/i18n.h"
#include "core/protocol_parser.h"
#include "core/project_logic.h"
#include "core/value_text.h"
#include "designer/canvas_item.h"
#include "designer/design_scene.h"
#include "runtime/app_dialog.h"
#include "runtime/hmi_widget.h"
#include "runtime/field_help.h"
#include "runtime/receive_frame_preview.h"
#include "runtime/ui_helpers.h"
#include "runtime/widgets/data_widgets.h"

#include <FluentQtWidgets/FluentIcon.h>
#include <FluentQtWidgets/Widgets/Button.h>
#include <FluentQtWidgets/Widgets/CardWidget.h>
#include <FluentQtWidgets/Widgets/ComboBox.h>
#include <FluentQtWidgets/Widgets/InfoBar.h>
#include <FluentQtWidgets/Widgets/Label.h>
#include <FluentQtWidgets/Widgets/LineEdit.h>
#include <FluentQtWidgets/Widgets/Menu.h>
#include <FluentQtWidgets/Widgets/SpinBox.h>
#include <FluentQtWidgets/Widgets/SwitchButton.h>
#include <FluentQtWidgets/Views/ItemViews.h>

#include <QtCore/QJsonArray>
#include <QtCore/QLocale>
#include <QtCore/QMap>
#include <QtCore/QSet>
#include <QtCore/QSignalBlocker>
#include <QtCore/QStringListModel>
#include <QtGui/QPainter>
#include <QtGui/QAction>
#include <QtGui/QFocusEvent>
#include <QtGui/QDoubleValidator>
#include <QtGui/QRegularExpressionValidator>
#include <QtWidgets/QAbstractItemView>
#include <QtWidgets/QApplication>
#include <QtWidgets/QCompleter>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QScrollBar>
#include <QtWidgets/QStyledItemDelegate>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace fhmi {

struct FrameSource
{
    QString id;
    LinkParser::Format format;
    QString templateId;
    QString localTemplateId;
    QString error;
};

namespace {

class CompactDoubleSpinBox : public FluentQt::DoubleSpinBox
{
  public:
    using FluentQt::DoubleSpinBox::DoubleSpinBox;

  protected:
    QString textFromValue(double value) const override
    {
        QString text = FluentQt::DoubleSpinBox::textFromValue(value);
        const QString decimal = locale().decimalPoint();
        if (text.contains(decimal)) {
            const QString zero = locale().zeroDigit();
            while (text.endsWith(zero))
                text.chop(zero.size());
            if (text.endsWith(decimal))
                text.chop(decimal.size());
        }
        return text;
    }
};

// The same searchable field is used by widget bindings and individual chart curves.
class BindingKeyEdit : public FluentQt::EditableComboBox
{
  public:
    explicit BindingKeyEdit(QWidget *parent) : FluentQt::EditableComboBox(parent), m_names(this)
    {
        auto *suggestions = new QCompleter(&m_names, this);
        suggestions->setCaseSensitivity(Qt::CaseInsensitive);
        suggestions->setFilterMode(Qt::MatchContains);
        suggestions->setMaxVisibleItems(12);
        setCompleter(suggestions);
        setMaxVisibleItems(12);
    }

    void setNames(const QStringList &names, const QString &placeholder)
    {
        setPlaceholderText(placeholder);
        if (names == m_names.stringList())
            return;
        const QSignalBlocker blocker(this);
        const QString current = text();
        clear();
        addItems(names);
        m_names.setStringList(names);
        setCurrentText(current);
    }

  private:
    QStringListModel m_names;
};

class RecordFieldDelegate : public QStyledItemDelegate
{
  public:
    RecordFieldDelegate(QStringList names, QObject *parent)
        : QStyledItemDelegate(parent), m_names(std::move(names)) {}

    QWidget *createEditor(QWidget *parent, const QStyleOptionViewItem &, const QModelIndex &) const override
    {
        auto *edit = new BindingKeyEdit(parent);
        edit->setNames(m_names, {});
        return edit;
    }

    void setEditorData(QWidget *editor, const QModelIndex &index) const override
    {
        static_cast<BindingKeyEdit *>(editor)->setCurrentText(index.data(Qt::EditRole).toString());
    }

    void setModelData(QWidget *editor, QAbstractItemModel *model, const QModelIndex &index) const override
    {
        model->setData(index, static_cast<BindingKeyEdit *>(editor)->text().trimmed(), Qt::EditRole);
    }

  private:
    QStringList m_names;
};

class ActionDelegate : public QStyledItemDelegate
{
  public:
    ActionDelegate(const QString &widgetType, ActionTargets targets, QObject *parent)
        : QStyledItemDelegate(parent), m_type(widgetType), m_targets(std::move(targets)) {}

    QWidget *createEditor(QWidget *parent, const QStyleOptionViewItem &style, const QModelIndex &index) const override
    {
        const auto kind = index.siblingAtColumn(2).data().toString();
        if (index.column() == 1 || (index.column() == 3 && kind == u"notify") ||
            (index.column() == 4 && kind != u"navigate" && kind != u"send"))
            return QStyledItemDelegate::createEditor(parent, style, index);
        if (index.column() == 4 && kind == u"send")
            return nullptr;
        auto *combo = new QComboBox(parent);
        for (const auto &key : choices(index)) {
            const auto label = index.column() == 0 || index.column() == 2 ? actionLabel(key) : key;
            combo->addItem(key.isEmpty() ? tr("本控件") : label, key);
        }
        return combo;
    }

    void setEditorData(QWidget *editor, const QModelIndex &index) const override
    {
        if (auto *combo = qobject_cast<QComboBox *>(editor))
            combo->setCurrentIndex(std::max(0, combo->findData(index.data().toString())));
        else
            QStyledItemDelegate::setEditorData(editor, index);
    }

    void setModelData(QWidget *editor, QAbstractItemModel *model, const QModelIndex &index) const override
    {
        if (auto *combo = qobject_cast<QComboBox *>(editor)) {
            if (combo->currentIndex() >= 0)
                model->setData(index, combo->currentData(), Qt::EditRole);
        } else {
            QStyledItemDelegate::setModelData(editor, model, index);
        }
    }

  protected:
    void initStyleOption(QStyleOptionViewItem *option, const QModelIndex &index) const override
    {
        QStyledItemDelegate::initStyleOption(option, index);
        if (index.column() == 0 || index.column() == 2)
            option->text = actionLabel(option->text);
        if (index.column() == 3 && option->text.isEmpty() && index.siblingAtColumn(2).data().toString() == u"send" &&
            m_type != u"LEDIndicator")
            option->text = tr("本控件");
    }

  private:
    QString actionLabel(const QString &key) const
    {
        static const QHash<QString, const char *> labels{{"click", QT_TR_NOOP("点击")}, {"on", QT_TR_NOOP("开启")},
            {"off", QT_TR_NOOP("关闭")}, {"send", QT_TR_NOOP("发送已配置命令")}, {"navigate", QT_TR_NOOP("切换页面")},
            {"setParameter", QT_TR_NOOP("写工程参数")}, {"notify", QT_TR_NOOP("显示通知")}};
        return labels.contains(key) ? tr(labels.value(key)) : key;
    }

    QStringList choices(const QModelIndex &index) const
    {
        if (index.column() == 0)
            return m_type == u"ToggleSwitch" ? QStringList{"on", "off"} : QStringList{"click"};
        if (index.column() == 2)
            return {"navigate", "send", "setParameter", "notify"};
        const auto kind = index.siblingAtColumn(2).data().toString();
        if (index.column() == 4)
            return m_targets.pages.value(index.siblingAtColumn(3).data().toString());
        QStringList names;
        if (kind == u"navigate")
            names = m_targets.pages.keys();
        else if (kind == u"setParameter")
            names = m_targets.parameters.keys();
        else if (kind == u"send")
            names = m_targets.buttons.values();
        names.sort();
        if (kind == u"send" && m_type != u"LEDIndicator")
            names.prepend(QString());
        return names;
    }

    QString m_type;
    ActionTargets m_targets;
};

QStringList dataPointNames(const Project *project)
{
    return project ? fhmi::dataPointNames(project->logic) : QStringList{};
}

// Command fields shown for each send mode, as in PyQt UI Designer.
const QHash<QString, QSet<QString>> &modeVisibility()
{
    static const QHash<QString, QSet<QString>> table = {
        {QStringLiteral("文本"), // i18n: value
         {QStringLiteral("command"), QStringLiteral("onCommand"), QStringLiteral("offCommand"),
          QStringLiteral("commandNewline"), QStringLiteral("commandTemplate")}},
        {QStringLiteral("HEX"),
         {QStringLiteral("command"), QStringLiteral("onCommand"), QStringLiteral("offCommand"),
          QStringLiteral("commandTemplate"), QStringLiteral("frameHeader"), QStringLiteral("frameTail"),
          QStringLiteral("dataType"), QStringLiteral("byteOrder")}},
        {QStringLiteral("JSON"),
         {QStringLiteral("command"), QStringLiteral("onCommand"), QStringLiteral("offCommand"),
          QStringLiteral("commandTemplate"), QStringLiteral("commandJsonNewline")}},
        {QStringLiteral("S7"), {QStringLiteral("s7Command")}},
        {QStringLiteral("Modbus"),
         {QStringLiteral("mbSlave"), QStringLiteral("mbFunc"), QStringLiteral("mbAddress"), QStringLiteral("mbBit"),
          QStringLiteral("mbBitAction"), QStringLiteral("mbValue"), QStringLiteral("mbDataType"),
          QStringLiteral("mbRegOrder"), QStringLiteral("mbArea")}},
        // A message with signals takes Signal=value assignments in the template, a raw ID HEX mode's bytes.
        {QStringLiteral("CAN"),
         {QStringLiteral("command"), QStringLiteral("onCommand"), QStringLiteral("offCommand"),
          QStringLiteral("commandTemplate"), QStringLiteral("dataType"), QStringLiteral("byteOrder"),
          QStringLiteral("canId"), QStringLiteral("canFd"), QStringLiteral("canCycle")}},
    };
    return table;
}

const QSet<QString> &modeFields()
{
    static const QSet<QString> fields = [] {
        QSet<QString> result;
        for (const auto &fields : modeVisibility())
            result.unite(fields);
        return result;
    }();
    return fields;
}

// What the project's CAN cards decode with, for completions: the signal keys a binding can use and the message
// names a CAN command can name. Each card's DBC is read again only when the file changed.
struct CanNames
{
    QStringList keys;
    QStringList messages;
    QList<CanPreviewSource> sources;
};

QJsonObject widgetById(const QJsonArray &widgets, const QString &id)
{
    QJsonObject result;
    visitWidgets(widgets, [&](const QJsonObject &widget) {
        if (widget.value(QStringLiteral("id")).toString() != id)
            return true;
        result = widget;
        return false;
    });
    return result;
}

LinkParser::Format frameFormat(const Project *project, const QJsonObject &properties = {})
{
    return project ? project->linkReceiveFormat(properties)
                   : LinkParser::effectiveFormat(properties.value(QStringLiteral("defaultParser")).toString(),
                       properties.value(QStringLiteral("parserConfig")).toObject(), {QStringLiteral("line"), {}});
}

FrameSource cardFrameSource(const Project &project, const QString &id, const QJsonObject &properties)
{
    return {id, project.linkReceiveFormat(properties), project.linkReceiveTemplateId(properties),
            properties.value(FrameTemplates::ReceiveId).toString(), {}};
}

FrameSource defaultFrameSource(const Project &project)
{
    const QString id = project.serial.value(FrameTemplates::ReceiveId).toString();
    return {{}, project.receiveFormat(), id, id, {}};
}

FrameSource frameSource(const Project *project, const QJsonArray &widgets, const QJsonObject &binding,
                        RouteTransport transport)
{
    DataRoute chosen{binding.value(QStringLiteral("sourceCardId")).toString(),
                     binding.value(QStringLiteral("sourceEndpoint")).toString()};
    const auto matches = routeConnections(widgets, transport, !chosen.endpoint.isEmpty(), project->parserType());
    if (chosen.isBroadcast() || (chosen.connectionId.isEmpty() && matches.size() > 1))
        return {{}, {}, {}, {}, PropertyEditor::tr("请先选定要编辑的来源连接")};
    if (chosen.connectionId.isEmpty() && matches.size() == 1)
        chosen.connectionId = matches.first().first;
    if (chosen.connectionId.isEmpty())
        return defaultFrameSource(*project);
    const auto card = widgetById(widgets, chosen.connectionId);
    if (!supportsRouteTransport(card.value(QStringLiteral("type")).toString(), RouteTransport::Bytes))
        return {chosen.connectionId, {}, {}, {}, {}};
    return cardFrameSource(*project, chosen.connectionId, card.value(QStringLiteral("properties")).toObject());
}

bool updateFrameProperties(QJsonArray &widgets, const QString &id, const QJsonObject &changes)
{
    for (qsizetype index = 0; index < widgets.size(); ++index) {
        auto widget = widgets.at(index).toObject();
        if (widget.value(QStringLiteral("id")).toString() == id) {
            auto props = widget.value(QStringLiteral("properties")).toObject();
            for (auto it = changes.begin(); it != changes.end(); ++it)
                props.insert(it.key(), it.value());
            widget.insert(QStringLiteral("properties"), props);
            widgets[index] = widget;
            return true;
        }
        auto children = widget.value(QStringLiteral("children")).toArray();
        if (updateFrameProperties(children, id, changes)) {
            widget.insert(QStringLiteral("children"), children);
            widgets[index] = widget;
            return true;
        }
    }
    return false;
}

bool usesFreeFrame(QStringView mode)
{
    return mode == ParseMode::KeyValue || mode == ParseMode::JsonPath || mode == ParseMode::HexBytes;
}

QMap<QString, CommandEncoder::InputValue> inputDefaults(const QJsonArray &widgets, const Project *project)
{
    QMap<QString, CommandEncoder::InputValue> inputs;
    visitWidgets(widgets, [&inputs, project](const QJsonObject &widget) {
        const QString id = widget.value(QStringLiteral("id")).toString();
        if (!id.isEmpty() && widget.value(QStringLiteral("type")).toString() == QLatin1String("LineEdit")) {
            const auto props = widget.value(QStringLiteral("properties")).toObject();
            const QString parameter = props.value(QStringLiteral("parameterName")).toString();
            const auto initial = project && !parameter.isEmpty()
                ? findDataPoint(project->logic, parameter).value(QStringLiteral("initialValue")) : QJsonValue();
            const QVariant value = initial.isUndefined() || initial.isNull()
                ? QVariant(props.value(QStringLiteral("text")).toString()) : initial.toVariant();
            inputs.insert(widgetInputId(id), {value, props.value(QStringLiteral("title")).toString().trimmed()});
        }
        return true;
    });
    if (project) {
        for (const auto &value : project->logic.value(QStringLiteral("points")).toArray()) {
            const auto point = value.toObject();
            const auto name = point.value(QStringLiteral("name")).toString();
            const auto initial = point.value(QStringLiteral("initialValue"));
            inputs.insert(pointInputId(name), {initial.toVariant(), name});
        }
    }
    return inputs;
}

QString endpointProperty(const QString &connection)
{
    if (connection == QLatin1String("commandTargetId"))
        return QStringLiteral("commandEndpoint");
    if (connection == QLatin1String("curveSourceCardId"))
        return QStringLiteral("curveSourceEndpoint");
    if (connection == QLatin1String("sourceCardId"))
        return QStringLiteral("sourceEndpoint");
    return {};
}

bool hasCanCard(const QJsonArray &widgets)
{
    return !visitWidgets(widgets, [](const QJsonObject &widget) {
        return !supportsRouteTransport(widget.value(QStringLiteral("type")).toString(), RouteTransport::Can);
    });
}

CanNames canNames(const Project *project, const QJsonArray &widgets, const QString &connectionId)
{
    CanNames names;
    if (!project) {
        return names;
    }
    const DataRoute route{connectionId, {}};
    visitWidgets(widgets, [&](const QJsonObject &widget) {
        const QString id = widget.value(QStringLiteral("id")).toString();
        if (!supportsRouteTransport(widget.value(QStringLiteral("type")).toString(), RouteTransport::Can) ||
            !route.acceptsConnection(id))
            return true;
        const auto card = widget.value(QStringLiteral("properties")).toObject();
        QString error;
        const auto dbc = designerCanDatabase(project, card.value(QStringLiteral("dbcFile")).toString(), &error);
        if (!error.isEmpty()) {
            names.sources.append({id, std::nullopt, error});
            return true;
        }
        auto set = CanSignalSet::build(dbc, card.value(QStringLiteral("canSignals")).toString(), &error);
        if (set) {
            names.keys += set->signalKeys();
            names.messages += set->messageNames();
        }
        names.sources.append({id, std::move(set), error, card.value(QStringLiteral("canTxInitialState")).toString()});
        return true;
    });
    names.keys.removeDuplicates();
    names.messages.removeDuplicates();
    return names;
}

QStringList receivedFieldNames(const Project *project, const QJsonArray &widgets, const QString &connectionId, bool jsonPaths = false)
{
    auto names = jsonPaths ? QStringList{} : canNames(project, widgets, connectionId).keys;
    const DataRoute route{connectionId, {}};
    const auto addText = [&](const Message &message) {
        for (const auto &field : textFieldValues(message))
            if (jsonPaths ? field.supportsJsonPath() : !field.nested)
                names.append(field.path);
    };
    visitWidgets(widgets, [&](const QJsonObject &widget) {
        if (!supportsRouteTransport(widget.value(QStringLiteral("type")).toString(), RouteTransport::Bytes) ||
            !route.acceptsConnection(widget.value(QStringLiteral("id")).toString()))
            return true;
        const auto properties = widget.value(QStringLiteral("properties")).toObject();
        const auto format = frameFormat(project, properties);
        if (!jsonPaths && format.type == LinkParser::HexGroup) {
            if (const auto variants = hexFrameVariants(format.settings))
                for (const auto &variant : *variants)
                    for (const auto &field : variant.frame.fields)
                        names.append(hexTemplateFieldKey(variant.id, field.name));
        } else if (!jsonPaths && format.type == QLatin1String("hex")) {
            if (const auto config = hexFrameConfig(format.settings))
                for (const auto &field : config->fields)
                    names.append(field.name);
        } else if (project && format.type == u"line") {
            addText(project->recentTextSamples.value(widget.value(QStringLiteral("id")).toString()));
            const auto entry = FrameTemplates::find(project->frameTemplates, project->linkReceiveTemplateId(properties));
            if (entry && !entry->sample.isEmpty()) {
                auto settings = format.settings;
                settings.insert(QStringLiteral("encoding"), QStringLiteral("utf-8"));
                if (const auto parser = createParser(format.type, settings)) {
                    auto bytes = entry->sample.toUtf8();
                    const auto terminator = lineTerminator(settings);
                    if (!bytes.endsWith(terminator))
                        bytes += terminator;
                    for (const auto &message : parser->feed(bytes))
                        addText(message);
                }
            }
        }
        return true;
    });
    names.removeDuplicates();
    return names;
}

QList<CanPreviewSource> commandCanSources(const Project *project, const QJsonArray &widgets, const QJsonObject &props)
{
    if (props.value(QStringLiteral("commandMode")).toString() != QLatin1String("CAN"))
        return {};
    const QString target = props.value(QStringLiteral("commandTargetId")).toString();
    return canNames(project, widgets, target == DataRoute::Broadcast ? QString() : target).sources;
}

// What a canId stands for on the project's CAN cards, resolved as CanLink::prepare does. A message with signals
// takes dataType and byteOrder from nowhere (its bytes come from the signals); a DBC message also takes its FD
// from the DBC, while an ID with inline signals still honours the command's canFd.
struct CanTargetKind
{
    bool usesRawEncoding = true;
    bool usesFdSetting = true;
};

CanTargetKind canTargetKind(const CanNames &names, const QString &canId)
{
    CanTargetKind kind;
    bool resolved = false;
    for (const auto &source : names.sources) {
        if (!source.signalSet)
            continue;
        if (const auto target = source.signalSet->target(canId)) {
            if (!resolved) {
                kind = {false, false};
                resolved = true;
            }
            kind.usesRawEncoding |= !target->hasSignals;
            kind.usesFdSetting |= !target->hasSignals || target->name.isEmpty();
        }
    }
    return kind;
}

void complete(FluentQt::LineEdit *edit, const QStringList &words)
{
    if (auto *completer = edit->completer()) {
        auto *names = static_cast<QStringListModel *>(completer->model());
        if (names->stringList() != words)
            names->setStringList(words);
        return;
    }
    if (words.isEmpty()) {
        return;
    }
    auto *names = new QStringListModel(words, edit);
    auto *completer = new QCompleter(names, edit);
    completer->setCaseSensitivity(Qt::CaseInsensitive);
    completer->setFilterMode(Qt::MatchContains);
    completer->setMaxVisibleItems(12);
    edit->setCompleter(completer);
    // Leaving the field (Tab, a click elsewhere) also closes the list, so it never outlives the edit.
    QObject::connect(edit, &QLineEdit::editingFinished, completer, [completer]() {
        if (QAbstractItemView *popup = completer->popup()) {
            popup->hide();
        }
    });
}

const QHash<QString, QSet<QString>> &chartParseVisibility()
{
    static const QHash<QString, QSet<QString>> table = {
        {ParseMode::KeyValue, {QStringLiteral("curveKey")}},
        {ParseMode::DataPoint, {QStringLiteral("curveKey")}},
        {ParseMode::JsonPath, {QStringLiteral("curveJsonPath")}},
        {ParseMode::HexBytes,
         {QStringLiteral("curveByteOffset"), QStringLiteral("curveDataType"), QStringLiteral("curveByteOrder"),
          QStringLiteral("curveFrameHeader"), QStringLiteral("curveFrameTail")}},
        {ParseMode::S7, {QStringLiteral("curveS7")}},
        {ParseMode::Modbus,
         {QStringLiteral("curveMbArea"), QStringLiteral("curveRegIndex"), QStringLiteral("curveMbBit"),
          QStringLiteral("curveMbDataType"), QStringLiteral("curveMbRegOrder"), QStringLiteral("curveMbFcFilter"),
          QStringLiteral("curveMbSlaveFilter"), QStringLiteral("curveMbPeriod")}},
    };
    return table;
}

// Virtual chart property -> per-curve storage suffix.
const QHash<QString, QString> &curveFields()
{
    static const QHash<QString, QString> table = {
        {QStringLiteral("curveName"), QStringLiteral("name")},
        {QStringLiteral("curveSourceCardId"),QStringLiteral("sourceCardId")},
        {QStringLiteral("curveSourceEndpoint"),QStringLiteral("sourceEndpoint")},
        {QStringLiteral("curveS7"),QStringLiteral("s7")},
        {QStringLiteral("curveExpression"), QStringLiteral("expression")},
        {QStringLiteral("curveParseMode"), QStringLiteral("parseMode")},
        {QStringLiteral("curveKey"), QStringLiteral("key")},
        {QStringLiteral("curveJsonPath"), QStringLiteral("jsonPath")},
        {QStringLiteral("curveByteOffset"), QStringLiteral("byteOffset")},
        {QStringLiteral("curveDataType"), QStringLiteral("dataType")},
        {QStringLiteral("curveByteOrder"), QStringLiteral("byteOrder")},
        {QStringLiteral("curveFrameHeader"), QStringLiteral("frameHeader")},
        {QStringLiteral("curveFrameTail"), QStringLiteral("frameTail")},
        {QStringLiteral("curveRegIndex"), QStringLiteral("regIndex")},
        {QStringLiteral("curveMbBit"), QStringLiteral("mbBit")},
        {QStringLiteral("curveMbDataType"), QStringLiteral("mbDataType")},
        {QStringLiteral("curveMbRegOrder"), QStringLiteral("mbRegOrder")},
        {QStringLiteral("curveMbPeriod"), QStringLiteral("mbPeriod")},
        {QStringLiteral("curveMbFcFilter"), QStringLiteral("mbFcFilter")},
        {QStringLiteral("curveMbSlaveFilter"), QStringLiteral("mbSlaveFilter")},
        {QStringLiteral("curveMbArea"), QStringLiteral("mbArea")},
        {QStringLiteral("curveCanId"), QStringLiteral("canId")},
        {QStringLiteral("curveCanStartBit"), QStringLiteral("canStartBit")},
        {QStringLiteral("curveCanBitLength"), QStringLiteral("canBitLength")},
        {QStringLiteral("curveCanByteOrder"), QStringLiteral("canByteOrder")},
        {QStringLiteral("curveCanSigned"), QStringLiteral("canSigned")},
        {QStringLiteral("curveCanFactor"), QStringLiteral("canFactor")},
        {QStringLiteral("curveCanOffset"), QStringLiteral("canOffset")},
    };
    return table;
}

// What a factor or offset field accepts. Empty is the default, shown as the placeholder.
const QRegularExpression &numberPattern()
{
    static const QRegularExpression number(QStringLiteral("[-+]?(\\d+\\.?\\d*|\\.\\d+)([eE][-+]?\\d+)?"));
    return number;
}

// The red caption that says why a CAN信号 setup reads nothing; its text is set by updateVisibility. It fixes its own
// height to what its wrapped lines need at its width. Left to the form, a note that appears after an edit can be
// laid out at a wider width's height and lose lines: QFormLayout keeps one table of wrapped heights for two widths.
class ProblemLabel : public FluentQt::CaptionLabel
{
  public:
    explicit ProblemLabel(QWidget *parent) : FluentQt::CaptionLabel(parent)
    {
        setWordWrap(true);
        setTextColor(QColor(0xC4, 0x2B, 0x1C), QColor(0xFF, 0x99, 0xA4)); // Fluent's critical colours
    }

    void setProblem(const QString &problem)
    {
        setText(problem);
        fitHeight();
    }

  protected:
    void resizeEvent(QResizeEvent *event) override
    {
        FluentQt::CaptionLabel::resizeEvent(event);
        fitHeight();
    }

  private:
    // Measured from the font, not heightForWidth(), which never goes below the fixed height already set.
    void fitHeight()
    {
        if (text().isEmpty()) {
            return;
        }
        const QMargins margins = contentsMargins();
        const int extra = 2 * margin();
        const int textWidth = std::max(1, width() - margins.left() - margins.right() - extra);
        const QRect lines = fontMetrics().boundingRect(QRect(0, 0, textWidth, QWIDGETSIZE_MAX),
                                                       int(alignment() | Qt::TextWordWrap), text());
        setFixedHeight(lines.height() + margins.top() + margins.bottom() + extra);
    }
};

ProblemLabel *newProblemLabel(QWidget *parent) { return new ProblemLabel(parent); }

// Commits a complete list of lines when editing ends, not every keystroke: a tab page
// removed mid-edit would move its widgets to the last page for good.
class CommitOnFocusOutEdit : public FluentQt::PlainTextEdit
{
  public:
    using FluentQt::PlainTextEdit::PlainTextEdit;
    std::function<void()> onFinished;

  protected:
    void focusOutEvent(QFocusEvent *event) override
    {
        FluentQt::PlainTextEdit::focusOutEvent(event);
        if (onFinished) {
            onFinished();
        }
    }
};

QJsonValue numberValue(double value, bool integral)
{
    return integral ? QJsonValue(qint64(std::llround(value))) : QJsonValue(value);
}

bool usesByteOrder(QStringView type)
{
    return type != QLatin1String("uint8") && type != QLatin1String("int8") && type != QLatin1String("ASCII") &&
           type != QStringLiteral("HEX字符串"); // i18n: value
}

int editorCurve(const CanvasItem &item)
{
    return std::clamp(item.propertyValue(QStringLiteral("currentCurve")).toInt(), 0,
                      chartCurveCount(item.typeName(), item.properties()) - 1);
}

QString editorCurveChoice(const CanvasItem &item, int index)
{
    const auto name = chartCurveName(item.properties(), index);
    if (!isTimingChart(item.typeName()))
        return name;
    const auto number = QCoreApplication::translate("fhmi", "通道 %1").arg(index + 1);
    return name == number ? number : number + QStringLiteral(" · ") + name;
}

void replaceChoices(FluentQt::ComboBox *box, const QStringList &options, const QString &value)
{
    const QSignalBlocker blocker(box);
    bool changed = box->count() != options.size();
    for (qsizetype index = 0; !changed && index < options.size(); ++index)
        changed = box->itemData(int(index)).toString() != options.at(index);
    if (changed) {
        box->clear();
        addValueItems(box, options);
    }
    if (!selectValue(box, value))
        box->setCurrentIndex(options.isEmpty() ? -1 : 0);
}

int countNonEmptyLines(const QString &text)
{
    int count = 0;
    for (const QString &line : text.split(QLatin1Char('\n'))) {
        if (!line.trimmed().isEmpty()) {
            ++count;
        }
    }
    return count;
}

int countSlaveInitEntries(const QString &text)
{
    int count = 0;
    const QStringList parts = text.split(QRegularExpression(QStringLiteral("[,;\\n]+")), Qt::SkipEmptyParts);
    for (const QString &part : parts) {
        if (!part.trimmed().isEmpty()) {
            ++count;
        }
    }
    return count;
}

QString s7ButtonSummary(const QString &areaKey, int dbNumber, int byteOffset, int bit, const S7::DataType &type)
{
    if (areaKey == QLatin1String("DB")) {
        if (type.isBool())
            return QStringLiteral("DB%1.DBX%2.%3").arg(dbNumber).arg(byteOffset).arg(bit);
        const QChar width = type.byteCount == 1 ? QLatin1Char('B') : type.byteCount == 2 ? QLatin1Char('W') : QLatin1Char('D');
        return QStringLiteral("DB%1.DB%2%3").arg(dbNumber).arg(width).arg(byteOffset);
    }
    if (type.isBool())
        return QStringLiteral("%1%2.%3").arg(areaKey).arg(byteOffset).arg(bit);
    const QChar width = type.byteCount == 1 ? QLatin1Char('B') : type.byteCount == 2 ? QLatin1Char('W') : QLatin1Char('D');
    return QStringLiteral("%1%2%3").arg(areaKey).arg(width).arg(byteOffset);
}

void openS7AddressBuilder(QWidget *parent, const QString &currentAddress, const QString &currentType, bool writeOnly,
                          const std::function<void(const QString &, const QString &)> &apply)
{
    AppDialog dialog(QCoreApplication::translate("fhmi", "S7 地址构建器"), parent);
    dialog.setObjectName(QStringLiteral("s7AddressBuilderDialog"));
    dialog.setPreferredSize({430, 340});
    auto *form = new QFormLayout;
    form->setHorizontalSpacing(10);
    form->setVerticalSpacing(8);

    auto *area = new FluentQt::ComboBox(dialog.widget());
    area->addItem(QCoreApplication::translate("fhmi", "DB · 数据块"), {}, QStringLiteral("DB"));
    area->addItem(QCoreApplication::translate("fhmi", "M · 标志位"), {}, QStringLiteral("M"));
    area->addItem(QCoreApplication::translate("fhmi", "Q · 输出区"), {}, QStringLiteral("Q"));
    if (!writeOnly) {
        area->addItem(QCoreApplication::translate("fhmi", "I · 输入区（只读）"), {}, QStringLiteral("I"));
    }

    auto *dbNum = new FluentQt::SpinBox(dialog.widget());
    dbNum->setRange(1, 65535);
    dbNum->setValue(1);

    auto *typeBox = new FluentQt::ComboBox(dialog.widget());
    for (const QString &t : S7::dataTypes()) {
        typeBox->addItem(t, {}, t);
    }
    selectValue(typeBox, currentType.isEmpty() ? QStringLiteral("REAL") : currentType);

    auto *byteOffset = new FluentQt::SpinBox(dialog.widget());
    byteOffset->setRange(0, 65535);
    byteOffset->setValue(0);

    auto *bitSpin = new FluentQt::SpinBox(dialog.widget());
    bitSpin->setRange(0, 7);
    bitSpin->setValue(0);

    S7::Address parsed;
    if (S7::parseAddress(currentAddress, &parsed)) {
        const QString key = parsed.areaKey();
        if (!selectValue(area, key)) {
            area->setCurrentIndex(0);
        }
        if (parsed.area == S7::MemoryArea::DataBlock && parsed.dbNumber > 0) {
            dbNum->setValue(parsed.dbNumber);
        }
        byteOffset->setValue(parsed.byteOffset);
        if (parsed.bit >= 0) {
            bitSpin->setValue(parsed.bit);
        }
    }

    auto *dbLabel = new FluentQt::BodyLabel(QCoreApplication::translate("fhmi", "DB 编号"), dialog.widget());
    auto *bitLabel = new FluentQt::BodyLabel(QCoreApplication::translate("fhmi", "位号 (0–7)"), dialog.widget());
    form->addRow(QCoreApplication::translate("fhmi", "存储区"), area);
    form->addRow(dbLabel, dbNum);
    form->addRow(QCoreApplication::translate("fhmi", "数据类型"), typeBox);
    form->addRow(QCoreApplication::translate("fhmi", "字节偏移"), byteOffset);
    form->addRow(bitLabel, bitSpin);
    dialog.viewLayout()->addLayout(form);

    auto *preview = new FluentQt::StrongBodyLabel(dialog.widget());
    preview->setWordWrap(true);
    dialog.viewLayout()->addWidget(preview);

    const auto currentResult = [&]() -> QPair<QString, QString> {
        const QString typeName = selectedValue(typeBox);
        S7::DataType dt;
        if (!S7::parseDataType(typeName, &dt)) {
            dt = {};
        }
        return {s7ButtonSummary(selectedValue(area), dbNum->value(), byteOffset->value(), bitSpin->value(), dt),
                dt.name};
    };
    const auto updatePreview = [&]() {
        const bool isDb = selectedValue(area) == QLatin1String("DB");
        dbLabel->setVisible(isDb);
        dbNum->setVisible(isDb);
        S7::DataType dt;
        S7::parseDataType(selectedValue(typeBox), &dt);
        bitLabel->setVisible(dt.isBool());
        bitSpin->setVisible(dt.isBool());
        const auto [addr, typeName] = currentResult();
        preview->setText(QCoreApplication::translate("fhmi", "地址预览：%1（%2 · %3 字节）")
                             .arg(addr, typeName)
                             .arg(dt.byteCount));
    };
    QObject::connect(area, &FluentQt::ComboBox::currentIndexChanged, &dialog, updatePreview);
    QObject::connect(typeBox, &FluentQt::ComboBox::currentIndexChanged, &dialog, updatePreview);
    QObject::connect(dbNum, qOverload<int>(&QSpinBox::valueChanged), &dialog, updatePreview);
    QObject::connect(byteOffset, qOverload<int>(&QSpinBox::valueChanged), &dialog, updatePreview);
    QObject::connect(bitSpin, qOverload<int>(&QSpinBox::valueChanged), &dialog, updatePreview);
    updatePreview();

    if (dialog.exec() == QDialog::Accepted) {
        const auto [addr, typeName] = currentResult();
        apply(addr, typeName);
    }
}

FluentQt::ComboBox *makeModbusFcCombo(QWidget *parent, int current)
{
    auto *box = new FluentQt::ComboBox(parent);
    const QList<QPair<int, QString>> choices = {
        {-1, QCoreApplication::translate("fhmi", "不限功能码")},
        {1, QCoreApplication::translate("fhmi", "01 读线圈")},
        {2, QCoreApplication::translate("fhmi", "02 读离散输入")},
        {3, QCoreApplication::translate("fhmi", "03 读保持寄存器")},
        {4, QCoreApplication::translate("fhmi", "04 读输入寄存器")},
    };
    int selected = -1;
    for (int i = 0; i < choices.size(); ++i) {
        box->addItem(choices.at(i).second, {}, choices.at(i).first);
        if (choices.at(i).first == current) {
            selected = i;
        }
    }
    if (selected < 0) {
        box->addItem(QCoreApplication::translate("fhmi", "功能码 %1").arg(current), {}, current);
        selected = box->count() - 1;
    }
    box->setCurrentIndex(selected);
    return box;
}

} // namespace

PropertyEditor::PropertyEditor(QWidget *parent) : FluentQt::ScrollArea(parent)
{
    setWidgetResizable(true);
    setFrameShape(QFrame::NoFrame);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    rebuild();
}

void PropertyEditor::setProject(Project *project)
{
    m_project = project;
    if (!m_item) {
        rebuild();
    }
}

void PropertyEditor::showItem(CanvasItem *item)
{
    if (m_item == item && item) {
        return;
    }
    if (m_item) {
        disconnect(m_item, nullptr, this, nullptr);
    }
    m_item = item;
    if (m_item) {
        connect(m_item, &QObject::destroyed, this, [this]() { QMetaObject::invokeMethod(this, [this]() { rebuild(); }, Qt::QueuedConnection); });
        connect(m_item, &CanvasItem::editorPageChanged, this, [this] {
            QMetaObject::invokeMethod(this, [this] { rebuild(); }, Qt::QueuedConnection);
        });
    }
    rebuild();
}

void PropertyEditor::clearContent()
{
    m_rows.clear();
    m_helpButtons.clear();
    m_bindingEditors.clear();
    for (auto &spin : m_geometry) {
        spin = nullptr;
    }
    if (QWidget *old = takeWidget()) {
        old->deleteLater();
    }
    m_content = new QWidget;
    m_content->setAttribute(Qt::WA_TranslucentBackground);
    m_layout = new QVBoxLayout(m_content);
    m_layout->setContentsMargins(4, 8, 8, 8);
    m_layout->setSpacing(8);
    setWidget(m_content);
    enableTransparentBackground();
}

void PropertyEditor::rebuild()
{
    m_loading = true;
    clearContent();
    if (m_item) {
        if (m_bindingOnly)
            buildBindingForm(addSection(tr("接收与绑定")));
        else
            buildItemForm();
    } else {
        buildProjectForm();
    }
    m_layout->addStretch();
    updateVisibility();
    m_loading = false;
    // setText leaves the cursor at the end, which scrolls a text longer than its field to its tail ("…ssure pump station")
    for (QLineEdit *edit : m_content->findChildren<QLineEdit *>()) {
        edit->setCursorPosition(0);
    }
}

QFormLayout *PropertyEditor::addSection(const QString &title)
{
    auto *card = new FluentQt::SimpleCardWidget(m_content);
    auto *form = new QFormLayout(card);
    form->setContentsMargins(12, 10, 12, 10);
    form->setHorizontalSpacing(10);
    form->setVerticalSpacing(6);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    form->addRow(new FluentQt::StrongBodyLabel(title, card));
    form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_layout->addWidget(card);
    return form;
}

void PropertyEditor::addRow(QFormLayout *form, const QString &key, const QString &label, QWidget *editor,
                           const QString &helpKey)
{
    auto *caption = new FluentQt::BodyLabel(label, m_content);
    if (!label.isEmpty()) {
        QWidget *input = editor->focusProxy() ? editor->focusProxy() : editor;
        if (input->focusPolicy() == Qt::NoFocus) {
            for (auto *child : input->findChildren<QWidget *>()) {
                if (child->focusPolicy() & Qt::TabFocus) {
                    input = child;
                    break;
                }
            }
        }
        caption->setBuddy(input);
        input->setAccessibleName(label);
    }
    // A long label (English "Transform expression") wraps instead of widening the panel past its dock
    caption->setWordWrap(true);
    QWidget *labelWidget = caption;
    if (const auto topic = fieldHelpTopic(helpKey)) {
        labelWidget = new QWidget(m_content);
        auto *layout = new QHBoxLayout(labelWidget);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(2);
        layout->addWidget(caption, 1);
        auto *help = createFieldHelpButton(*topic, labelWidget);
        help->setObjectName(QStringLiteral("propertyHelp_%1").arg(key));
        layout->addWidget(help, 0, Qt::AlignVCenter);
        m_helpButtons.insert(key, help);
    }
    form->addRow(labelWidget, editor);
    if (!key.isEmpty()) {
        m_rows.insert(key, {labelWidget, editor});
    }
    if (key.endsWith(QLatin1String("Endpoint"))) {
        addRow(form, key + QStringLiteral("Problem"), QString(), newProblemLabel(m_content));
        auto *configure = new FluentQt::TransparentPushButton(tr("配置 Modbus 连接…"), m_content);
        configure->setObjectName(QStringLiteral("configureModbusConnection"));
        addRow(form, key + QStringLiteral("Configure"), {}, configure);
        configure->hide();
        connect(configure, &QPushButton::clicked, this, [this, configure] {
            editModbusConnection(configure->property("preferredConnection").toString());
        });
    }
}

void PropertyEditor::setRowVisible(const QString &key, bool visible)
{
    const auto it = m_rows.constFind(key);
    if (it != m_rows.constEnd()) {
        it->first->setVisible(visible);
        it->second->setVisible(visible);
    }
}

void PropertyEditor::setModbusReadFields(const QJsonObject &binding, const QString &bitRow, const QString &typeRow,
                                         const QString &orderRow)
{
    const QString table = ModbusArea::prefix(binding.value(QStringLiteral("mbArea")).toString(ModbusArea::HoldingRegisters));
    const bool registers = table == QLatin1String("hr") || table == QLatin1String("ir");
    const int bit = binding.value(QStringLiteral("mbBit")).toInt(CommandEncoder::WholeRegister);
    const bool isBit = registers && bit >= 0 && bit <= 15;
    const QString dataType = binding.value(QStringLiteral("mbDataType")).toString(QStringLiteral("uint16"));
    setRowVisible(bitRow, registers);
    setRowVisible(typeRow, registers && !isBit);
    setRowVisible(orderRow, registers && !isBit && CommandEncoder::usesRegisterOrder(dataType));
}

void PropertyEditor::updateRouteEndpoint(const QString &connection, const QString &endpoint, bool active)
{
    auto *edit = qobject_cast<FluentQt::LineEdit *>(m_rows.value(endpoint).second);
    if (!edit)
        return;
    auto *select = static_cast<ConnectionSourceEdit *>(m_rows.value(connection).second);
    select->bindEndpointEditor(edit);
    const bool filled = !edit->text().isEmpty();
    const bool problem = active && filled && !select->supportsEndpoint();
    const bool ambiguous = active && select->ambiguous();
    const auto compatibility = active ? select->selectionProblem() : QString();
    const QString problemKey = endpoint + QStringLiteral("Problem");
    auto *message = static_cast<ProblemLabel *>(m_rows.value(problemKey).second);
    message->setProblem(problem ? tr("当前连接或模式不支持子通道。清空此项，或重新选择连接。")
                        : ambiguous ? tr("有多个匹配的连接，请明确选择。") : compatibility);
    edit->setError(problem);
    edit->setPlaceholderText(select->endpointPlaceholder());
    setRowVisible(endpoint, active && (select->offersEndpoint() || filled));
    setRowVisible(problemKey, problem || ambiguous || !compatibility.isEmpty());
    const auto configureKey = endpoint + QStringLiteral("Configure");
    if (auto *configure = m_rows.value(configureKey).second) {
        configure->setProperty("preferredConnection", selectedValue(select));
        setRowVisible(configureKey, m_project && !compatibility.isEmpty() &&
            !routeConnections(currentWidgets(m_project, m_scene), RouteTransport::Bytes).isEmpty());
    }
}

void PropertyEditor::editModbusConnection(const QString &preferred)
{
    Project before = *m_project;
    before.widgets = currentWidgets(m_project, m_scene);
    AppDialog dialog(tr("配置 Modbus 连接"), window());
    dialog.setPreferredSize({490, 290});
    auto *form = new QFormLayout;
    auto *connection = new FluentQt::ComboBox(&dialog);
    const auto cards = routeConnections(before.widgets, RouteTransport::Bytes);
    for (const auto &[id, type] : cards) {
        const auto *meta = WidgetRegistry::instance().find(type);
        connection->addItem(id + QStringLiteral(" · ") + translated(meta->displayName), {}, id);
    }
    selectValue(connection, preferred);
    if (connection->currentIndex() < 0)
        connection->setCurrentIndex(0);
    auto *parser = new FluentQt::ComboBox(&dialog);
    parser->addItem(QStringLiteral("Modbus TCP"), {}, QStringLiteral("modbus_tcp"));
    parser->addItem(QStringLiteral("Modbus RTU"), {}, QStringLiteral("modbus"));
    parser->addItem(QStringLiteral("Modbus ASCII"), {}, QStringLiteral("modbus_ascii"));
    const auto preferredParser = [&] {
        const auto card = widgetById(before.widgets, selectedValue(connection));
        const auto format = before.linkReceiveFormat(card.value(QStringLiteral("properties")).toObject());
        const bool tcp = card.value(QStringLiteral("type")).toString() == QLatin1String("TcpConfigCard");
        selectValue(parser, isModbusParser(format.type) ? format.type
            : tcp ? QStringLiteral("modbus_tcp") : QStringLiteral("modbus"));
    };
    preferredParser();
    connect(connection, &FluentQt::ComboBox::currentIndexChanged, &dialog, preferredParser);
    form->addRow(tr("连接"), connection);
    form->addRow(tr("解析模式"), parser);
    dialog.viewLayout()->addLayout(form);
    if (dialog.exec() != QDialog::Accepted)
        return;
    const auto id = selectedValue(connection);
    const auto mode = selectedValue(parser);
    Project draft = before;
    const auto props = widgetById(before.widgets, id).value(QStringLiteral("properties")).toObject();
    const auto previous = before.linkReceiveFormat(props);
    if (previous.type == mode)
        return;
    updateFrameProperties(draft.widgets, id, {{QStringLiteral("defaultParser"), mode},
        {QStringLiteral("parserConfig"), QJsonObject{}}, {FrameTemplates::ReceiveId, QString()},
        {FrameTemplates::ReceiveIds, QJsonArray{}}});
    if (!confirmReceiveTemplateSelection(before, draft, id, window()))
        return;
    applyProjectDraft(draft);
    emit projectSettingsChanged({});
    const int scroll = verticalScrollBar()->value();
    QMetaObject::invokeMethod(this, [this, scroll] {
        rebuild();
        verticalScrollBar()->setValue(scroll);
    }, Qt::QueuedConnection);
}

QWidget *PropertyEditor::createEditor(const PropDef &def, const QJsonValue &value, const Commit &commit)
{
    if (def.name == QLatin1String("currentCurve") || def.name == QLatin1String("currentIndex")) {
        auto *box = new FluentQt::ComboBox(m_content);
        if (def.name == QLatin1String("currentCurve")) {
            const auto props = m_item->properties();
            for (int i = 0; i < chartCurveCount(m_item->typeName(), props); ++i)
                box->addItem(editorCurveChoice(*m_item, i), QIcon(), i);
        } else {
            const auto options = CommandEncoder::selectionOptions(m_item->propertyValue(QStringLiteral("options")).toArray(),
                m_item->propertyValue(QStringLiteral("enumTable")).toString(), m_project ? m_project->enumTables : QJsonObject{});
            for (const auto &option : options)
                box->addItem(option.label);
        }
        if (box->count() > 0)
            box->setCurrentIndex(std::clamp(value.toInt(), 0, box->count() - 1));
        connect(box, &FluentQt::ComboBox::currentIndexChanged, this, [commit](int index) { commit(index); });
        return box;
    }
    if (def.name == QLatin1String("mbValue")) {
        using CommandEncoder::ModbusArgument;
        const auto spec = CommandEncoder::modbusFunctionSpec(
            CommandEncoder::functionCode(m_item->propertyValue(QStringLiteral("mbFunc")).toString()));
        const QString stored = value.toVariant().toString();
        if (spec.argument == ModbusArgument::ReadCount) {
            auto *count = new FluentQt::SpinBox(m_content);
            count->setRange(1, spec.maximumCount);
            count->setValue(stored.toInt());
            count->setKeyboardTracking(false);
            connect(count, qOverload<int>(&QSpinBox::valueChanged), this,
                    [commit](int number) { commit(QString::number(number)); });
            return count;
        }
        if (spec.argument == ModbusArgument::Coil) {
            auto *box = new FluentQt::ComboBox(m_content);
            box->addItem(tr("关"), QIcon(), QStringLiteral("0"));
            box->addItem(tr("开"), QIcon(), QStringLiteral("1"));
            box->setCurrentIndex(stored == QLatin1String("0") ? 0 : 1);
            connect(box, &FluentQt::ComboBox::currentIndexChanged, this,
                    [box, commit] { commit(selectedValue(box)); });
            return box;
        }
        auto *edit = new FluentQt::LineEdit(m_content);
        edit->setText(stored);
        const QString type = m_item->propertyValue(QStringLiteral("mbDataType")).toString();
        if (const auto range = CommandEncoder::modbusValueRange(type)) {
            auto *validator = new QDoubleValidator(range->minimum, range->maximum, type == QLatin1String("float32") ? 15 : 0, edit);
            validator->setLocale(QLocale::c());
            if (type != QLatin1String("float32"))
                validator->setNotation(QDoubleValidator::StandardNotation);
            edit->setValidator(validator);
        }
        connect(edit, &QLineEdit::textChanged, this, [edit, commit](const QString &text) {
            edit->setError(!edit->hasAcceptableInput());
            commit(text);
        });
        return edit;
    }
    if (def.name == QLatin1String("value") && m_item && isIndustrialCategory(m_item->meta().category)) {
        const auto caps = industrialCapabilities(m_item->typeName(),
                          m_item->propertyValue(QStringLiteral("feedbackMode")).toString());
        if (!caps.states.isEmpty()) {
            auto *box = new FluentQt::ComboBox(m_content);
            box->addItems(caps.states);
            box->setCurrentIndex(std::clamp(value.toInt(), 0, int(caps.states.size()) - 1));
            connect(box, &FluentQt::ComboBox::currentIndexChanged, this, [commit](int index) { commit(index); });
            return box;
        }
    }
    if (def.name == u"curveKey" || def.name == u"curveJsonPath") {
        auto *edit = new BindingKeyEdit(m_content);
        edit->setCurrentText(value.toString());
        connect(edit, &FluentQt::EditableComboBox::currentTextChanged, this,
                [commit](const QString &text) { commit(text); });
        return edit;
    }
    if (def.name == QLatin1String("commandTargetId") || def.name == QLatin1String("curveSourceCardId") ||
        def.name == QLatin1String("sourceCardId")) {
        const QString type = m_item->typeName();
        RouteFilter filter = widgetReceiveTransport(type);
        if (def.name == QLatin1String("commandTargetId")) {
            filter = CommandEncoder::sendRouteFilter(m_item->properties(), type);
        } else if (def.name == QLatin1String("curveSourceCardId")) {
            filter = bindingRouteTransport(m_item->properties().value(storageKey(QStringLiteral("curveParseMode"))).toString());
        }
        const auto purpose = def.name == QLatin1String("commandTargetId") ? ConnectionSourceEdit::Purpose::Send
                             : def.name == QLatin1String("sourceCardId") ? ConnectionSourceEdit::Purpose::Observe
                                                                        : ConnectionSourceEdit::Purpose::Binding;
        auto *box = new ConnectionSourceEdit(m_content, purpose);
        box->setSources(m_project, m_scene, filter, value.toString());
        connect(box, &FluentQt::ComboBox::currentIndexChanged, this, [box, commit] { commit(selectedValue(box)); });
        return box;
    }
    if (def.name == QLatin1String("defaultPortName")) {
        auto *box = new FluentQt::ComboBox(m_content);
        box->addItem(tr("运行时选择"), {}, QString());
        for (const auto &port : SerialLink::selectablePorts())
            box->addItem(SerialLink::portLabel(port), {}, port);
        const QString saved = value.toString();
        if (!selectValue(box, saved)) {
            box->addItem(SerialLink::portLabel(saved), {}, saved);
            box->setCurrentIndex(box->count() - 1);
        }
        connect(box, &FluentQt::ComboBox::currentIndexChanged, this, [box, commit] { commit(selectedValue(box)); });
        return box;
    }
    if (def.name == QLatin1String("parameterName") || def.name == QLatin1String("recordTriggerPoint")) {
        const bool parameter = def.name == QLatin1String("parameterName");
        auto *box = new FluentQt::ComboBox(m_content);
        const auto names = m_project ? fhmi::dataPointNames(m_project->logic, parameter) : QStringList{};
        box->addItem(parameter ? tr("独立输入") : tr("任一记录点收到有效值"), {}, QString());
        addValueItems(box, names);
        if (!selectValue(box, value.toString())) {
            box->addItem(value.toString(), {}, value.toString());
            box->setCurrentIndex(box->count() - 1);
        }
        connect(box, &FluentQt::ComboBox::currentIndexChanged, this, [box, commit] { commit(selectedValue(box)); });
        return box;
    }

    if(def.name==QLatin1String("pulsesPerTurn")) {
        auto *edit=new FluentQt::LineEdit(m_content);edit->setText(QString::number(value.toDouble(1024),'f',0));
        auto *validator = new QDoubleValidator(1, 9007199254740991.0, 0, edit);
        validator->setLocale(QLocale::c());
        validator->setNotation(QDoubleValidator::StandardNotation);
        edit->setValidator(validator);
        connect(edit, &QLineEdit::textChanged, this, [edit] { edit->setError(!edit->hasAcceptableInput()); });
        connect(edit, &QLineEdit::editingFinished, this, [commit, edit] { commit(edit->text().toDouble()); });
        return edit;
    }
    switch (def.type) {
    case PropType::Int:
    case PropType::Float: {
        if (def.name == QLatin1String("curveCanFactor") || def.name == QLatin1String("curveCanOffset")) {
            // A spin box rounds to its decimals, and a DBC's factor can be 0.0001 or finer: a text field keeps
            // what is typed. Empty is the default, which is not stored.
            auto *edit = new FluentQt::LineEdit(m_content);
            edit->setText(QString::number(value.toDouble(), 'g', QLocale::FloatingPointShortest));
            edit->setPlaceholderText(QString::number(def.defaultValue.toDouble()));
            edit->setValidator(new QRegularExpressionValidator(numberPattern(), edit));
            connect(edit, &QLineEdit::textChanged, this, [commit](const QString &text) {
                bool ok = false;
                const double number = text.toDouble(&ok);
                commit(ok ? QJsonValue(number) : QJsonValue(QJsonValue::Undefined));
            });
            return edit;
        }
        const bool spinParameter = m_item && m_item->meta().typeName == QLatin1String("SpinBox") &&
                                   (def.name == QLatin1String("minimum") || def.name == QLatin1String("maximum") ||
                                    def.name == QLatin1String("value") || def.name == QLatin1String("singleStep"));
        const bool decimalInput = spinParameter &&
                                  m_item->propertyValue(QStringLiteral("numberMode")).toString() == QStringLiteral("小数"); // i18n: value
        const bool integral = def.type == PropType::Int || (spinParameter && !decimalInput);
        QAbstractSpinBox *box = nullptr;
        if (integral) {
            auto *spin = new FluentQt::SpinBox(m_content);
            spin->setRange(int(std::ceil(def.minimum.value_or(-999999))), int(def.maximum.value_or(999999)));
            spin->setValue(value.isString() ? value.toString().toInt() : value.toInt());
            if (def.name == QLatin1String("curveMbPeriod"))
                spin->setSpecialValueText(tr("默认周期"));
            if ((def.name == QLatin1String("mbBit") && def.minimum && *def.minimum < 0) ||
                def.name == QLatin1String("curveMbBit"))
                spin->setSpecialValueText(tr("整个寄存器"));
            if (def.name == CommandEncoder::SendInterval) {
                spin->setSpecialValueText(tr("单次发送"));
                spin->setSuffix(QStringLiteral(" ms"));
            }
            if (def.name == QLatin1String("canCycle"))
                spin->setSpecialValueText(tr("沿用 DBC 周期"));
            connect(spin, qOverload<int>(&QSpinBox::valueChanged), this,
                    [commit](int v) { commit(QJsonValue(v)); });
            box = spin;
        } else {
            auto *spin = new CompactDoubleSpinBox(m_content);
            spin->setDecimals(decimalInput ? m_item->propertyValue(QStringLiteral("decimals")).toInt(2)
                                          : m_item && isIndustrialCategory(m_item->meta().category) ? 6 : 3);
            spin->setRange(def.minimum.value_or(-999999), def.maximum.value_or(999999));
            if (decimalInput) {
                const double unit = std::pow(10., -spin->decimals());
                spin->setSingleStep(unit);
                if (def.name == QLatin1String("singleStep"))
                    spin->setMinimum(unit);
            }
            spin->setValue(value.toDouble());
            connect(spin, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
                    [commit](double v) { commit(numberValue(v, false)); });
            box = spin;
        }
        box->setKeyboardTracking(false);
        return box;
    }
    case PropType::Bool: {
        auto *toggle = new FluentQt::SwitchButton(m_content);
        toggle->setOnText(tr("开"));
        toggle->setOffText(tr("关"));
        toggle->setChecked(value.isBool() ? value.toBool() : value.toDouble() != 0);
        connect(toggle, &FluentQt::SwitchButton::checkedChanged, this, [commit](bool on) { commit(on); });
        return toggle;
    }
    case PropType::Enum: {
        auto *combo = new FluentQt::ComboBox(m_content);
        QStringList options = def.options;
        if (def.name == QLatin1String("curveParseMode") && !hasCanCard(currentWidgets(m_project, m_scene))) {
            for (const auto &mode : {ParseMode::CanSignal, ParseMode::CanField})
                if (value.toString() != mode)
                    options.removeAll(mode);
        }
        if (def.name == QLatin1String("recordSource")) {
            combo->addItem(tr("接收报文"), {}, QStringLiteral("messages"));
            combo->addItem(tr("工程数据点"), {}, QStringLiteral("points"));
        } else if (def.name == QLatin1String("curveParseMode")) {
            for (const auto &mode : options)
                combo->addItem(mode == ParseMode::CanSignal ? tr("CAN 按位取值") : translated(mode), {}, mode);
        } else {
            addValueItems(combo, options);
        }
        if (def.name == QLatin1String("defaultParser") && m_project)
            showInheritedParser(combo, m_project->parserType());
        if (!selectValue(combo, value.toVariant().toString())) {
            combo->setCurrentIndex(0);
        }
        connect(combo, &FluentQt::ComboBox::currentIndexChanged, this,
                [combo, commit]() { commit(selectedValue(combo)); });
        return combo;
    }
    case PropType::Text: {
        auto *edit = new CommitOnFocusOutEdit(m_content);
        edit->setPlainText(value.toString());
        edit->setFixedHeight(84);
        if (def.name == QLatin1String("tabNames")) {
            edit->onFinished = [edit, commit]() { commit(edit->toPlainText()); };
        } else {
            connect(edit, &QPlainTextEdit::textChanged, this, [edit, commit]() { commit(edit->toPlainText()); });
        }
        return edit;
    }
    case PropType::Color: {
        auto *row = new QWidget(m_content);
        auto *layout = new QHBoxLayout(row);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(4);
        auto *edit = new FluentQt::LineEdit(row);
        edit->setText(value.toString());
        auto *pick = new FluentQt::ToolButton(row);
        pick->setAccessibleName(tr("选择颜色"));
        fluentToolTip(pick, tr("选择颜色"));
        keepSquareBeside(pick, edit);
        layout->addWidget(edit, 1);
        layout->addWidget(pick);
        const auto swatch = [pick](const QString &text) {
            QPixmap image(24, 24);
            image.fill(Qt::transparent);
            QPainter painter(&image);
            const QColor color(text);
            painter.setPen(QColor("#808080"));
            painter.setBrush(color.isValid() ? color : Qt::transparent);
            painter.drawRoundedRect(QRectF(2, 2, 19, 19), 3, 3);
            painter.end();
            pick->setIcon(QIcon(image));
        };
        swatch(edit->text());
        connect(edit, &QLineEdit::textChanged, this, [commit, swatch](const QString &text) {
            swatch(text);
            commit(text);
        });
        connect(pick, &QToolButton::clicked, this, [this, edit] {
            const QColor color = chooseColor(QColor(edit->text()), tr("选择颜色"), this);
            if (color.isValid())
                edit->setText(color.name());
        });
        return row;
    }
    case PropType::String:
    case PropType::Command:
    case PropType::Object: // not reached: an object has a section of its own
        break;
    }

    FluentQt::LineEdit *edit = def.name == QLatin1String("defaultPassword") ?
        static_cast<FluentQt::LineEdit *>(new FluentQt::PasswordLineEdit(m_content)) : new FluentQt::LineEdit(m_content);
    edit->setText(value.toVariant().toString());
    if (m_item && m_item->typeName() == QLatin1String("ToggleSwitch") &&
        (def.name == QLatin1String("onValue") || def.name == QLatin1String("offFeedbackValue"))) {
        edit->setValidator(new QRegularExpressionValidator(numberPattern(), edit));
        edit->setPlaceholderText(tr("输入数字"));
    }
    if (def.name == QLatin1String("offFeedbackValue")) {
        edit->setPlaceholderText(tr("留空时其余值为关"));
    }
    connect(edit, &QLineEdit::textChanged, this, [commit](const QString &t) { commit(t); });
    const bool dbc = def.name == QLatin1String("dbcFile");
    if (def.name != QLatin1String("imagePath") && !dbc) {
        return edit;
    }
    auto *row = new QWidget(m_content);
    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);
    auto *browse = new FluentQt::ToolButton(row);
    browse->setIcon(FluentQt::icon(FluentQt::FluentIcon::Folder));
    keepSquareBeside(browse, edit);
    layout->addWidget(edit, 1);
    layout->addWidget(browse);
    connect(browse, &QToolButton::clicked, this, [this, edit, dbc]() {
        const QString path =
            dbc ? QFileDialog::getOpenFileName(this, tr("选择 DBC 文件"), QString(), tr("DBC 文件 (*.dbc)"))
                : QFileDialog::getOpenFileName(this, tr("选择图片"), QString(),
                                               tr("图片 (*.png *.jpg *.jpeg *.bmp *.gif *.svg)"));
        if (path.isEmpty()) {
            return;
        }
        // Relative to a saved project, so the project folder can move as a whole.
        QString stored = path;
        if (m_project && !m_project->filePath.isEmpty()) {
            const QString relative = QDir(m_project->assetBaseDir()).relativeFilePath(path);
            if (!relative.startsWith(QLatin1String(".."))) {
                stored = relative;
            }
        }
        edit->setText(stored);
    });
    return row;
}

// ---------------------------------------------------------------------------
// Project settings
// ---------------------------------------------------------------------------

void PropertyEditor::buildBitProfilesForm()
{
    if (!m_project)
        return;
    if (m_item)
        m_bitProfileName = m_item->propertyValue(QStringLiteral("bitProfile")).toString(QStringLiteral("default"));
    auto *form = addSection(tr("共享位配置"));
    auto *selector = new FluentQt::ComboBox(m_content);
    QStringList names = m_project->bitProfiles.keys();
    if (!names.contains(QStringLiteral("default")))
        names.prepend(QStringLiteral("default"));
    if (!names.contains(m_bitProfileName))
        names.append(m_bitProfileName);
    selector->addItems(names);
    selector->setCurrentText(m_bitProfileName);
    addRow(form, QStringLiteral("profile.select"), tr("配置名"), selector);
    connect(selector, &FluentQt::ComboBox::currentTextChanged, this, [this](const QString &name) {
        m_bitProfileName = name;
        if (m_item) {
            commitProperty(QStringLiteral("bitProfile"), name);
        } else {
            QMetaObject::invokeMethod(this, [this] { rebuild(); }, Qt::QueuedConnection);
        }
    });
    const auto publish = [this](const QString &key) {
        if (m_scene)
            m_scene->setBitProfiles(m_project->bitProfiles);
        emit projectSettingsChanged(QStringLiteral("bitProfiles:") + m_bitProfileName + QLatin1Char(':') + key);
    };
    auto *name = new FluentQt::LineEdit(m_content);
    name->setText(m_bitProfileName);
    addRow(form, QStringLiteral("profile.name"), tr("新配置名"), name);
    auto *actions = new QWidget(m_content);
    auto *layout = new QHBoxLayout(actions);
    layout->setContentsMargins(0, 0, 0, 0);
    auto *create = new FluentQt::PushButton(tr("新建配置"), actions);
    auto *rename = new FluentQt::PushButton(tr("重命名配置"), actions);
    layout->addWidget(create);
    layout->addWidget(rename);
    form->addRow(actions);
    connect(create, &QPushButton::clicked, this, [this, name, publish] {
        const QString target = name->text().trimmed();
        if (target.isEmpty() || m_project->bitProfiles.contains(target))
            return;
        m_project->bitProfiles.insert(target, effectiveBitProfile({}, QStringLiteral("default")));
        m_bitProfileName = target;
        if (m_item) {
            const QSignalBlocker blocker(m_scene);
            m_item->setPropertyValue(QStringLiteral("bitProfile"), target);
        }
        publish(QStringLiteral("create"));
        QMetaObject::invokeMethod(this, [this] { rebuild(); }, Qt::QueuedConnection);
    });
    connect(rename, &QPushButton::clicked, this, [this, name, publish] {
        const QString target = name->text().trimmed(), old = m_bitProfileName;
        if (target.isEmpty() || target == old || m_project->bitProfiles.contains(target))
            return;
        auto profile = m_project->bitProfiles.value(old).toObject();
        if (profile.isEmpty())
            profile = effectiveBitProfile({}, QStringLiteral("default"));
        m_project->bitProfiles.remove(old);
        m_project->bitProfiles.insert(target, profile);
        std::function<QJsonArray(QJsonArray)> rewrite = [&](QJsonArray widgets) {
            for (int i = 0; i < widgets.size(); ++i) {
                auto data = widgets.at(i).toObject();
                const QString type = data.value(QStringLiteral("type")).toString();
                auto props = data.value(QStringLiteral("properties")).toObject();
                if ((type == QLatin1String("StatusWordPanel") || type == QLatin1String("DigitalTimingChart")) &&
                    props.value(QStringLiteral("bitProfile")).toString(QStringLiteral("default")) == old) {
                    props.insert(QStringLiteral("bitProfile"), target);
                    data.insert(QStringLiteral("properties"), props);
                }
                if (data.contains(QStringLiteral("children")))
                    data.insert(QStringLiteral("children"), rewrite(data.value(QStringLiteral("children")).toArray()));
                widgets[i] = data;
            }
            return widgets;
        };
        m_project->widgets = rewrite(m_project->widgets);
        if (m_scene) {
            const QSignalBlocker blocker(m_scene);
            for (auto *item : m_scene->allItems())
                if (item->meta().prop(QStringLiteral("bitProfile")) &&
                    item->propertyValue(QStringLiteral("bitProfile")).toString() == old)
                    item->setPropertyValue(QStringLiteral("bitProfile"), target);
        }
        m_bitProfileName = target;
        publish(QStringLiteral("rename"));
        QMetaObject::invokeMethod(this, [this] { rebuild(); }, Qt::QueuedConnection);
    });
    const auto profile = effectiveBitProfile(m_project->bitProfiles, m_bitProfileName);
    auto *width = new FluentQt::ComboBox(m_content);
    width->addItems({QStringLiteral("8"), QStringLiteral("16")});
    width->setCurrentText(QString::number(profile.value(QStringLiteral("width")).toInt()));
    addRow(form, QStringLiteral("profile.width"), tr("位宽"), width);
    connect(width, &FluentQt::ComboBox::currentTextChanged, this, [this, publish](const QString &value) {
        auto data = m_project->bitProfiles.value(m_bitProfileName).toObject();
        data.insert(QStringLiteral("width"), value.toInt());
        m_project->bitProfiles.insert(m_bitProfileName, data);
        publish(QStringLiteral("width"));
        QMetaObject::invokeMethod(this, [this] { rebuild(); }, Qt::QueuedConnection);
    });
    const auto bits = profile.value(QStringLiteral("bits")).toArray();
    for (int i = 0; i < bits.size(); ++i) {
        auto *row = new QWidget(m_content);
        auto *h = new QHBoxLayout(row);
        h->setContentsMargins(0, 0, 0, 0);
        auto *label = new FluentQt::LineEdit(row);
        auto *color = new FluentQt::LineEdit(row);
        auto *pick = new FluentQt::ToolButton(row);
        pick->setIcon(FluentQt::icon(FluentQt::FluentIcon::Palette));
        label->setText(bits.at(i).toObject().value(QStringLiteral("name")).toString());
        color->setText(bits.at(i).toObject().value(QStringLiteral("color")).toString());
        color->setMaximumWidth(92);
        h->addWidget(label, 1);
        h->addWidget(color);
        h->addWidget(pick);
        addRow(form, QStringLiteral("profile.bit%1").arg(i), QStringLiteral("B%1").arg(i), row);
        const auto commit = [this, publish, i, label, color] {
            auto data = m_project->bitProfiles.value(m_bitProfileName).toObject();
            auto values = data.value(QStringLiteral("bits")).toArray();
            const auto defaults = effectiveBitProfile(m_project->bitProfiles, m_bitProfileName);
            const auto defaultBits = defaults.value(QStringLiteral("bits")).toArray();
            while (values.size() < defaultBits.size())
                values.append(defaultBits.at(values.size()));
            auto bit = values.at(i).toObject();
            bit.insert(QStringLiteral("name"), label->text());
            bit.insert(QStringLiteral("color"), color->text());
            values[i] = bit;
            data.insert(QStringLiteral("width"), defaults.value(QStringLiteral("width")));
            data.insert(QStringLiteral("bits"), values);
            m_project->bitProfiles.insert(m_bitProfileName, data);
            publish(QStringLiteral("bit%1").arg(i));
        };
        connect(label, &QLineEdit::textChanged, this, [commit] { commit(); });
        connect(color, &QLineEdit::textChanged, this, [commit] { commit(); });
        connect(pick, &QToolButton::clicked, this, [this, color] {
            const auto picked = chooseColor(QColor(color->text()), tr("点亮颜色"), this);
            if (picked.isValid())
                color->setText(picked.name());
        });
    }
}
void PropertyEditor::buildProjectForm()
{
    if (!m_project) {
        return;
    }
    QFormLayout *general = addSection(tr("项目"));
    auto *name = new FluentQt::LineEdit(m_content);
    name->setText(m_project->appName);
    connect(name, &QLineEdit::textChanged, this, [this](const QString &text) {
        m_project->appName = text;
        emit projectSettingsChanged(QStringLiteral("appName"));
    });
    addRow(general, QString(), tr("应用名称"), createIconButton(name));

    const auto spin = [this](const char *key, int lo, int hi, int value, const std::function<void(int)> &apply) {
        auto *box = new FluentQt::SpinBox(m_content);
        box->setRange(lo, hi);
        box->setValue(value);
        box->setKeyboardTracking(false);
        connect(box, qOverload<int>(&QSpinBox::valueChanged), this, [this, apply, key](int v) {
            apply(v);
            emit projectSettingsChanged(QLatin1String(key));
        });
        return box;
    };
    addRow(general, QString(), tr("画布宽度"),
           spin("canvasWidth", 100, 9999, m_project->canvasWidth, [this](int v) { m_project->canvasWidth = v; }));
    addRow(general, QString(), tr("画布高度"),
           spin("canvasHeight", 100, 9999, m_project->canvasHeight, [this](int v) { m_project->canvasHeight = v; }));
    addRow(general, QString(), tr("网格大小"),
           spin("gridSize", 1, 100, m_project->gridSize, [this](int v) { m_project->gridSize = v; }));

    const int pointCount = int(m_project->logic.value(QStringLiteral("points")).toArray().size());
    const int alarmCount = int(m_project->logic.value(QStringLiteral("alarms")).toArray().size());
    const QString logicLabel = (pointCount > 0 || alarmCount > 0)
                                   ? tr("工程数据与报警（%1 点 · %2 报警）…").arg(pointCount).arg(alarmCount)
                                   : tr("工程数据与报警…");
    auto *logic = new FluentQt::PushButton(logicLabel, m_content);
    addRow(general, QStringLiteral("logic"), tr("数据与报警"), logic, QStringLiteral("project.logic"));
    connect(logic, &QPushButton::clicked, this, &PropertyEditor::editProjectLogic);
    buildBitProfilesForm();
    QFormLayout *frame = addSection(tr("默认帧格式"));
    auto *hint = new FluentQt::CaptionLabel(tr("链路卡片选择“项目设置”时使用"), m_content);
    hint->setWordWrap(true);
    frame->addRow(hint);
    addReceiveTemplateRow(frame, QStringLiteral("project.receiveTemplate"), ReceiveTarget::Defaults);
    auto *parser = new FluentQt::ComboBox(m_content);
    parser->addItems({QStringLiteral("line"), QStringLiteral("hex"), QStringLiteral("modbus"),
                      QStringLiteral("modbus_tcp"), QStringLiteral("modbus_ascii")});
    parser->setCurrentIndex(std::max(0, parser->findText(m_project->parserType())));
    addRow(frame, QStringLiteral("parser"), tr("解析模式"), parser, QStringLiteral("receive.framing"));

    connect(parser, &FluentQt::ComboBox::currentTextChanged, this, [this](const QString &type) {
        m_project->serial.insert(QStringLiteral("parser"), type);
        updateVisibility();
        emit projectSettingsChanged(QStringLiteral("parser"));
    });
    addFrameRows(
        frame, QString(), [this]() { return m_project->parserConfig(); },
        [this](const QJsonObject &config, const QString &key) {
            m_project->serial.insert(QStringLiteral("parserConfig"), config);
            updateVisibility();
            emit projectSettingsChanged(QStringLiteral("parserConfig.") + key);
        });

    auto *tip = new FluentQt::CaptionLabel(tr("从左侧拖拽控件到画布；选中控件后在这里编辑属性和数据绑定。"), m_content);
    tip->setWordWrap(true);
    m_layout->addWidget(tip);
}

void PropertyEditor::editProjectLogic()
{
    if (!m_project)
        return;
    Project draft = *m_project;
    draft.widgets = currentWidgets(m_project, m_scene);
    if (fhmi::editProjectLogic(&draft, this)) {
        applyProjectDraft(draft);
        emit projectSettingsChanged(QString()); // each accepted dialog is one independent undo step
        QMetaObject::invokeMethod(this, [this] { rebuild(); }, Qt::QueuedConnection);
    }
}

void PropertyEditor::applyProjectDraft(const Project &draft)
{
    m_project->logic = draft.logic;
    m_project->serial = draft.serial;
    m_project->widgets = draft.widgets;
    m_project->frameTemplates = draft.frameTemplates;
    if (!m_scene)
        return;
    const QSignalBlocker blocker(m_scene);
    visitWidgets(draft.widgets, [this](const QJsonObject &widget) {
        if (auto *item = m_scene->findItem(widget.value(QStringLiteral("id")).toString())) {
            const auto binding = widget.value(QStringLiteral("binding")).toObject();
            if (item->binding() != binding)
                item->setBinding(binding);
            const auto props = widget.value(QStringLiteral("properties")).toObject();
            for (auto it = props.begin(); it != props.end(); ++it)
                if (item->propertyValue(it.key()) != it.value())
                    item->setPropertyValue(it.key(), it.value());
        }
        return true;
    });
}

QWidget *PropertyEditor::createIconButton(QWidget *nameField)
{
    auto *row = new QWidget(m_content);
    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);
    auto *button = new FluentQt::ToolButton(row);
    fluentToolTip(button, tr("应用图标"));
    button->setIconSize(QSize(20, 20));
    // The studio's own logo stands for the default icon, which is what an exported application gets.
    const QIcon icon = iconFromFile(m_project->iconPath());
    button->setIcon(icon.isNull() ? QIcon(QStringLiteral(":/fluenthmi/logo.png")) : icon);
    keepSquareBeside(button, nameField);
    nameField->setParent(row);
    layout->addWidget(button);
    layout->addWidget(nameField, 1);

    const auto setIcon = [this](const QString &icon) {
        m_project->icon = icon;
        emit projectSettingsChanged(QString());
        QMetaObject::invokeMethod(this, [this]() { rebuild(); }, Qt::QueuedConnection);
    };
    const auto choose = [this, setIcon]() {
        const QString file =
            QFileDialog::getOpenFileName(this, tr("选择应用图标"), QString(), tr("图片 (*.png *.jpg *.jpeg *.ico)"));
        if (file.isEmpty() || !m_project) {
            return;
        }
        // Stored like an image path: relative to a saved project when inside its folder.
        QString stored = file;
        if (!m_project->filePath.isEmpty()) {
            const QString relative = QDir(m_project->assetBaseDir()).relativeFilePath(file);
            if (!relative.startsWith(QLatin1String(".."))) {
                stored = relative;
            }
        }
        setIcon(stored);
    };
    connect(button, &QToolButton::clicked, this, [this, button, setIcon, choose]() {
        auto *menu = new FluentQt::RoundMenu(this);
        connect(menu, &QMenu::aboutToHide, menu, &QObject::deleteLater);
        // The dialog runs an event loop of its own, which may delete the menu: open it once the menu is done.
        connect(menu->addAction(tr("选择图标…")), &QAction::triggered, this,
                [this, choose]() { QMetaObject::invokeMethod(this, choose, Qt::QueuedConnection); });
        QAction *reset = menu->addAction(tr("使用默认图标"));
        reset->setEnabled(!m_project->icon.isEmpty());
        connect(reset, &QAction::triggered, this, [setIcon]() { setIcon(QString()); });
        menu->exec(button->mapToGlobal(QPoint(0, button->height())), true);
    });
    return row;
}

void PropertyEditor::addFrameRows(QFormLayout *form, const QString &prefix, const ConfigRead &read,
                                  const ConfigWrite &write)
{
    addReceiveFrameFields(m_content, read, write,
        [this, form, prefix](const QString &key, const QString &label, QWidget *editor, const QString &help) {
            addRow(form, prefix + key, label, editor, help);
        }, true);
    addReceivePreviewRow(form, prefix + QStringLiteral("preview"));
}

void PropertyEditor::addReceivePreviewRow(QFormLayout *form, const QString &key)
{
    auto *row = new QWidget(m_content);
    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    auto *edit = new FluentQt::PushButton(tr("编辑帧格式…"), row);
    edit->setObjectName(QStringLiteral("editReceiveFrameFormat"));
    layout->addWidget(edit, 1);
    auto *preview = new FluentQt::PushButton(tr("解析预览…"), row);
    preview->setObjectName(QStringLiteral("previewReceiveFrame"));
    layout->addWidget(preview, 1);
    addRow(form, key, tr("示例报文"), row);
    connect(edit, &QPushButton::clicked, this, [this, edit] {
        openReceiveEditor(m_item ? ReceiveTarget::Connection : ReceiveTarget::Defaults, edit);
    });
    connect(preview, &QPushButton::clicked, this, [this] {
        const auto source = m_project ? receiveSource(m_item ? ReceiveTarget::Connection : ReceiveTarget::Defaults)
            : FrameSource{{}, frameFormat(nullptr, m_item ? m_item->properties() : QJsonObject()), {}, {}, {}};
        const auto &format = source.format;
        const bool grouped = format.type == LinkParser::HexGroup;
        const auto editor = QPointer<PropertyEditor>(this);
        SaveReceivedField save;
        if (m_project)
            save = [editor, sourceId = source.id, grouped](const QString &field, bool json) {
                if (editor)
                    editor->addDataPoint({{QStringLiteral("parseMode"), json ? ParseMode::JsonPath : ParseMode::KeyValue},
                                          {QStringLiteral("sourceCardId"), sourceId},
                                          {json ? QStringLiteral("jsonPath") : QStringLiteral("key"), field}},
                                         grouped ? field.section(u'.', 1) : field);
            };
        const auto entry = m_project ? FrameTemplates::find(m_project->frameTemplates, source.templateId) : std::nullopt;
        if (grouped) {
            openReceiveGroupPreview(format.settings, window(), std::move(save));
        } else if (format.type == QLatin1String("hex")) {
            const auto sample = entry ? parseHex(entry->sample).value_or(QByteArray{}) : QByteArray();
            openReceiveFramePreview(format.settings, window(), sample, std::move(save), entry ? entry->name : QString());
        } else {
            openReceiveTextPreview(format.settings, window(), entry ? entry->sample : QString(), std::move(save));
        }
    });
}

void PropertyEditor::addReceiveEditorRow(QFormLayout *form, const QString &key, ReceiveTarget target)
{
    const bool binds = target == ReceiveTarget::Binding || target == ReceiveTarget::Curve;
    if (binds) {
        auto *fields = new FluentQt::ComboBox(m_content);
        fields->setObjectName(target == ReceiveTarget::Curve ? QStringLiteral("curveTemplateField")
                                                            : QStringLiteral("bindingTemplateField"));
        addRow(form, key + QStringLiteral(".field"), tr("模板字段"), fields);
        connect(fields, &FluentQt::ComboBox::currentIndexChanged, this, [this, fields, target] {
            const QString field = selectedValue(fields);
            if (field.isEmpty() || m_loading)
                return;
            bindReceiveField(target, ReceivedField{field}, receiveSource(target).id);
            emit modified({});
            const int scroll = verticalScrollBar()->value();
            QMetaObject::invokeMethod(this, [this, scroll] {
                rebuild();
                verticalScrollBar()->setValue(scroll);
            }, Qt::QueuedConnection);
        });
    }
    auto *edit = new FluentQt::PushButton(binds ? tr("可视化取值…") : tr("可视化编辑帧格式…"), m_content);
    edit->setObjectName(QStringLiteral("editReceiveFrameFormat"));
    addRow(form, key, binds ? tr("取值与帧格式") : tr("接收帧格式"), edit);
    connect(edit, &QPushButton::clicked, this, [this, target, edit] { openReceiveEditor(target, edit); });
    if (m_project) {
        auto *change = new FluentQt::PushButton(tr("更改来源连接模板…"), m_content);
        change->setObjectName(QStringLiteral("editSourceReceiveTemplates"));
        addRow(form, key + QStringLiteral(".template"), tr("连接帧模板"), change);
        connect(change, &QPushButton::clicked, this, [this, target] {
            const auto source = receiveSource(target);
            if (source.id.isEmpty())
                return;
            const auto card = widgetById(currentWidgets(m_project, m_scene), source.id);
            const auto selected = FrameTemplates::receiveIds(card.value(QStringLiteral("properties")).toObject());
            editReceiveTemplateSelection(m_project->frameTemplates, selected, window(),
                [this, target](const QStringList &ids) { return setReceiveTemplates(target, ids); }, source.id);
        });
    }
}

void PropertyEditor::addTemplateChoiceRow(QFormLayout *form, const QString &key, const QString &label,
                                          const std::function<void(const QString &)> &choose,
                                          const std::function<void()> &manage, const std::function<void()> &chooseGroup)
{
    auto *row = new QWidget(m_content);
    auto *choice = new FluentQt::ComboBox(row);
    choice->setObjectName(key);
    choice->setAccessibleName(label);
    choice->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    auto *button = new FluentQt::PushButton(tr("管理…"), row);
    auto *layout = new QHBoxLayout;
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);
    if (chooseGroup) {
        auto *column = new QVBoxLayout(row);
        column->setContentsMargins(0, 0, 0, 0);
        column->setSpacing(6);
        column->addWidget(choice);
        column->addLayout(layout);
        auto *multiple = new FluentQt::PushButton(tr("多选…"), row);
        multiple->setObjectName(QStringLiteral("chooseReceiveTemplateGroup"));
        multiple->setAccessibleName(tr("选择接收模板组"));
        layout->addWidget(multiple);
        connect(multiple, &QPushButton::clicked, this, chooseGroup);
    } else {
        row->setLayout(layout);
        layout->addWidget(choice, 1);
    }
    layout->addWidget(button);
    addRow(form, key, label, row);
    connect(choice, &FluentQt::ComboBox::currentIndexChanged, this, [choice, choose] { choose(selectedValue(choice)); });
    connect(button, &QPushButton::clicked, this, manage);
}

FrameSource PropertyEditor::receiveSource(ReceiveTarget target) const
{
    if (!m_project)
        return {};
    if (target == ReceiveTarget::Defaults)
        return defaultFrameSource(*m_project);
    if (target == ReceiveTarget::Connection)
        return cardFrameSource(*m_project, m_item->widgetId(), m_item->properties());
    const auto binding = target == ReceiveTarget::Curve ? chartCurveBinding(m_item->properties(), editorCurve(*m_item))
        : target == ReceiveTarget::Observer ? m_item->properties() : m_item->binding();
    const auto transport = target == ReceiveTarget::Observer ? widgetReceiveTransport(m_item->typeName())
        : bindingRouteTransport(binding.value(QStringLiteral("parseMode")).toString(ParseMode::KeyValue));
    return frameSource(m_project, currentWidgets(m_project, m_scene), binding, transport);
}

void PropertyEditor::manageFrameTemplates(const LinkParser::Format &seed, const QString &selected)
{
    Project draft = *m_project;
    draft.widgets = currentWidgets(m_project, m_scene);
    if (!editFrameTemplateLibrary(&draft, window(), seed, selected))
        return;
    applyProjectDraft(draft);
    emit projectSettingsChanged({});
    const int scroll = verticalScrollBar()->value();
    QMetaObject::invokeMethod(this, [this, scroll] {
        rebuild();
        verticalScrollBar()->setValue(scroll);
    }, Qt::QueuedConnection);
}

void PropertyEditor::addReceiveTemplateRow(QFormLayout *form, const QString &key, ReceiveTarget target)
{
    if (!m_project)
        return;
    std::function<void()> chooseGroup;
    if (target != ReceiveTarget::Defaults && hasMultipleHexTemplates(m_project->frameTemplates)) {
        chooseGroup = [this, target] {
            const auto selected = m_project->linkReceiveTemplateIds(m_item->properties());
            editReceiveTemplateSelection(m_project->frameTemplates, selected, window(),
                [this, target](const QStringList &ids) { return setReceiveTemplates(target, ids); });
        };
    }
    addTemplateChoiceRow(form, key, tr("接收帧模板"), [this, target](const QString &id) {
        setReceiveTemplate(target, id);
    }, [this, target] {
        const auto source = receiveSource(target);
        manageFrameTemplates(source.format, source.templateId);
    }, chooseGroup);
}

bool PropertyEditor::updateReceiveTemplateRow(const QString &key, ReceiveTarget target, bool active)
{
    const auto source = active ? receiveSource(target) : FrameSource{};
    const bool resolved = !source.format.type.isEmpty();
    const bool editable = LinkParser::hasEditableFraming(source.format.type) || source.format.type == LinkParser::HexGroup;
    auto *row = m_rows.value(key).second;
    if (qobject_cast<QPushButton *>(row)) {
        setRowVisible(key, resolved && !source.id.isEmpty() && !m_project->frameTemplates.isEmpty());
        return editable;
    }
    const bool visible = resolved || !source.error.isEmpty();
    setRowVisible(key, visible);
    if (visible && row) {
        auto *choice = row->findChild<FluentQt::ComboBox *>();
        const auto inherited = source.localTemplateId.isEmpty() && !source.templateId.isEmpty()
            ? FrameTemplates::find(m_project->frameTemplates, source.templateId) : std::nullopt;
        const QString automatic = !resolved ? tr("请先选择来源连接")
            : inherited ? tr("跟随工程（%1）").arg(inherited->name)
            : tr("独立配置");
        fillFrameTemplates(choice, m_project->frameTemplates, source.localTemplateId, automatic);
        if (source.format.type == LinkParser::HexGroup) {
            const QSignalBlocker blocker(choice);
            const auto count = source.format.settings.value(LinkParser::GroupTemplates).toArray().size();
            choice->addItem(tr("接收模板组 · %1 个").arg(count), {}, QString());
            choice->setCurrentIndex(choice->count() - 1);
            choice->setItemEnabled(choice->currentIndex(), false);
        }
        choice->setEnabled(resolved && !m_project->frameTemplates.isEmpty());
        choice->setToolTip(source.id.isEmpty() ? QString() : tr("此选择应用到来源连接 %1").arg(source.id));
        if (auto *multiple = row->findChild<FluentQt::PushButton *>(QStringLiteral("chooseReceiveTemplateGroup")))
            multiple->setVisible(resolved && !source.id.isEmpty() && hasMultipleHexTemplates(m_project->frameTemplates));
    }
    return editable;
}

bool PropertyEditor::updateTemplateFieldPicker(const QString &row, ReceiveTarget target, FluentQt::ComboBox *mode,
                                              const QString &parseMode, const QString &currentField)
{
    const auto source = usesFreeFrame(parseMode) ? receiveSource(target) : FrameSource{};
    const bool group = source.format.type == LinkParser::HexGroup;
    const bool usesTemplate = group || (!source.templateId.isEmpty() && source.format.type == QLatin1String("hex"));
    if (mode) {
        const QSignalBlocker blocker(mode);
        const auto setText = [mode](const QString &value, const QString &text) {
            const auto index = mode->findData(value);
            if (index >= 0 && mode->itemText(index) != text)
                mode->setItemText(index, text);
        };
        setText(ParseMode::KeyValue, usesTemplate ? tr("按模板字段取值") : translated(ParseMode::KeyValue));
        setText(ParseMode::HexBytes, usesTemplate ? tr("手动按字节取值") : translated(ParseMode::HexBytes));
    }

    const bool byTemplate = usesTemplate && parseMode == ParseMode::KeyValue;
    const bool byBytes = usesTemplate && parseMode == ParseMode::HexBytes;
    QList<QPair<QString, QString>> names;
    if (byTemplate || byBytes) {
        if (group) {
            if (const auto variants = hexFrameVariants(source.format.settings))
                for (const auto &variant : *variants)
                    for (const auto &field : variant.frame.fields) {
                        const auto label = QStringLiteral("%1 · %2").arg(variant.name, field.name);
                        names.append({hexTemplateFieldKey(variant.id, field.name), label});
                        if (field.name == currentField && variant.unqualifiedFields.contains(field.name))
                            names.append({field.name, label});
                    }
        } else if (const auto config = hexFrameConfig(source.format.settings)) {
            for (const auto &field : config->fields)
                if (!field.name.isEmpty())
                    names.append({field.name, field.name});
        }
    }
    const bool visible = byTemplate || (byBytes && !names.isEmpty());
    setRowVisible(row, visible);
    if (visible) {
        auto *fields = static_cast<FluentQt::ComboBox *>(m_rows.value(row).second);
        const QSignalBlocker blocker(fields);
        fields->clear();
        fields->addItem(names.isEmpty() ? tr("模板尚未定义字段") : tr("选择模板字段"), {}, QString());
        fields->setItemEnabled(0, false);
        for (const auto &[key, label] : names)
            if (fields->findData(key) < 0)
                fields->addItem(label, {}, key);
        if (byTemplate && !currentField.isEmpty() && !selectValue(fields, currentField)) {
            fields->addItem(tr("%1（字段不可用）").arg(currentField), {}, currentField);
            fields->setCurrentIndex(fields->count() - 1);
            fields->setItemEnabled(fields->currentIndex(), false);
        }
        fields->setEnabled(!names.isEmpty());
    }
    return byTemplate;
}

void PropertyEditor::setReceiveTemplate(ReceiveTarget target, const QString &id)
{
    setReceiveTemplates(target, id.isEmpty() ? QStringList{} : QStringList{id});
}

bool PropertyEditor::setReceiveTemplates(ReceiveTarget target, const QStringList &ids)
{
    const auto source = receiveSource(target);
    if (source.format.type.isEmpty())
        return false;
    const auto id = ids.size() == 1 ? ids.first() : QString();
    const bool group = ids.size() > 1;
    if (!group && source.format.type != LinkParser::HexGroup && source.localTemplateId == id)
        return true;
    Project before = *m_project;
    before.widgets = currentWidgets(m_project, m_scene);
    Project draft = before;
    QJsonObject changes;
    bool staged = false;
    if (source.id.isEmpty()) {
        draft.serial.insert(FrameTemplates::ReceiveId, id);
    } else {
        const auto props = target == ReceiveTarget::Connection ? m_item->properties()
            : widgetById(before.widgets, source.id).value(QStringLiteral("properties")).toObject();
        const auto selected = FrameTemplates::receiveIds(props);
        if (QSet<QString>(selected.cbegin(), selected.cend()) == QSet<QString>(ids.cbegin(), ids.cend()))
            return true;
        changes = {{FrameTemplates::ReceiveId, id},
                   {FrameTemplates::ReceiveIds, group ? QJsonArray::fromStringList(ids) : QJsonArray{}}};
        staged = updateFrameProperties(draft.widgets, source.id, changes);
        if (!staged && target != ReceiveTarget::Connection)
            return false;
    }
    if (!confirmReceiveTemplateSelection(before, draft, source.id, window())) {
        updateVisibility();
        return false;
    }
    applyProjectDraft(draft);
    if (target == ReceiveTarget::Connection && (!m_scene || !staged))
        for (auto it = changes.cbegin(); it != changes.cend(); ++it)
            m_item->setPropertyValue(it.key(), it.value());
    emit projectSettingsChanged({});
    if (target != ReceiveTarget::Connection && target != ReceiveTarget::Defaults) {
        updateVisibility();
        return true;
    }
    const int scroll = verticalScrollBar()->value();
    QMetaObject::invokeMethod(this, [this, scroll] {
        rebuild();
        verticalScrollBar()->setValue(scroll);
    }, Qt::QueuedConnection);
    return true;
}

void PropertyEditor::openReceiveEditor(ReceiveTarget target, QWidget *anchor)
{
    if (!m_project)
        return;
    const bool curve = target == ReceiveTarget::Curve;
    const bool binds = curve || target == ReceiveTarget::Binding;
    const QPointer<CanvasItem> item = m_item;
    const int curveIndex = curve ? editorCurve(*item) : 0;
    auto values = binds ? std::optional<ReceiveValueSettings>{{
        curve ? chartCurveBinding(item->properties(), curveIndex) : item->binding(), item->typeName(), m_bindingOnly}}
        : std::nullopt;

    Project draft = *m_project;
    draft.widgets = currentWidgets(m_project, m_scene);
    auto source = receiveSource(target);
    if (!source.error.isEmpty()) {
        emit message(tr("接收帧格式"), source.error, true);
        return;
    }
    const bool group = source.format.type == LinkParser::HexGroup;
    if (group) {
        const auto key = values ? values->binding.value(QStringLiteral("key")).toString() : QString();
        source.templateId = chooseReceiveTemplate(source.format.settings, key, anchor);
        if (source.templateId.isEmpty())
            return;
        source.format = FrameTemplates::find(draft.frameTemplates, source.templateId)->format;
        const auto prefix = hexTemplateFieldKey(source.templateId, {});
        if (values && key.startsWith(prefix))
            values->binding.insert(QStringLiteral("key"), key.mid(prefix.size()));
    }
    if (!LinkParser::hasEditableFraming(source.format.type))
        return;
    const auto entry = FrameTemplates::find(draft.frameTemplates, source.templateId);
    const QString scope = entry ? tr("模板 %1 · 引用它的接收和发送共用").arg(entry->name) : source.id.isEmpty()
        ? tr("编辑工程默认帧格式 · 使用项目设置的连接共用")
        : tr("编辑连接 %1 的帧格式 · 该连接上的数据点、控件和曲线共用").arg(source.id);
    const auto updated = entry
        ? editProjectFrameTemplate(draft, source.templateId, source.format, scope, values, window())
        : editReceiveFrameFormat(source.format, scope, values, window());
    if (!updated)
        return;
    const auto &format = updated->format;
    const bool changed = source.format.type != format.type || source.format.settings != format.settings ||
        (entry && (updated->sample != entry->sample || updated->templateName != entry->name));
    auto patch = updated->bindingChanges;
    if (group && patch.contains(QStringLiteral("key")))
        patch.insert(QStringLiteral("key"), hexTemplateFieldKey(source.templateId, patch.value(QStringLiteral("key")).toString()));
    if (!changed && patch.isEmpty())
        return;
    if (changed && !entry) {
        if (source.id.isEmpty()) {
            draft.serial.insert(QStringLiteral("parser"), format.type);
            draft.serial.insert(QStringLiteral("parserConfig"), format.settings);
        } else if (!updateFrameProperties(draft.widgets, source.id,
                       {{QStringLiteral("defaultParser"), format.type}, {QStringLiteral("parserConfig"), format.settings}}) &&
                   target == ReceiveTarget::Connection) {
            item->setPropertyValue(QStringLiteral("defaultParser"), format.type);
            item->setPropertyValue(QStringLiteral("parserConfig"), format.settings);
        }
    }
    applyProjectDraft(draft);
    if (item && !patch.isEmpty()) {
        const QString modeKey = QStringLiteral("parseMode");
        const auto mode = patch.value(modeKey).toString(values->binding.value(modeKey).toString(ParseMode::KeyValue));
        const auto field = mode == ParseMode::JsonPath ? QStringLiteral("jsonPath") : QStringLiteral("key");
        if (!source.id.isEmpty() && (patch.contains(modeKey) || (mode != ParseMode::HexBytes && patch.contains(field))))
            patch.insert(QStringLiteral("sourceCardId"), source.id);
        if (curve) {
            const auto prefix = QStringLiteral("_curve_%1_").arg(curveIndex);
            for (auto it = patch.cbegin(); it != patch.cend(); ++it)
                item->setPropertyValue(prefix + it.key(), it.value());
        } else {
            auto binding = item->binding();
            for (auto it = patch.cbegin(); it != patch.cend(); ++it)
                binding.insert(it.key(), it.value());
            item->setBinding(binding);
        }
    }
    emit projectSettingsChanged({});
    const int scroll = verticalScrollBar()->value();
    QMetaObject::invokeMethod(this, [this, scroll] {
        rebuild();
        verticalScrollBar()->setValue(scroll);
    }, Qt::QueuedConnection);
}

void PropertyEditor::bindReceiveField(ReceiveTarget target, const ReceivedField &field, const QString &sourceId)
{
    const QString mode = field.jsonPath ? ParseMode::JsonPath : ParseMode::KeyValue;
    const QString key = field.jsonPath ? QStringLiteral("jsonPath") : QStringLiteral("key");
    if (target == ReceiveTarget::Curve) {
        const auto prefix = QStringLiteral("_curve_%1_").arg(editorCurve(*m_item));
        m_item->setPropertyValue(prefix + QStringLiteral("parseMode"), mode);
        m_item->setPropertyValue(prefix + key, field.key);
        if (!sourceId.isEmpty())
            m_item->setPropertyValue(prefix + QStringLiteral("sourceCardId"), sourceId);
        return;
    }
    auto binding = m_item->binding();
    binding.insert(QStringLiteral("parseMode"), mode);
    binding.insert(key, field.key);
    if (!sourceId.isEmpty())
        binding.insert(QStringLiteral("sourceCardId"), sourceId);
    m_item->setBinding(binding);
}

void PropertyEditor::addDataPoint(QJsonObject binding, const QString &suggestedName, CanvasItem *boundItem)
{
    if (!m_project)
        return;
    Project draft = *m_project;
    draft.widgets = currentWidgets(m_project, m_scene);
    auto points = draft.logic.value(QStringLiteral("points")).toArray();
    const auto names = dataPointNames(m_project);
    QString name;
    for (const auto &value : points) {
        const auto point = value.toObject();
        if (point.value(QStringLiteral("binding")).toObject() == binding) {
            name = point.value(QStringLiteral("name")).toString();
            break;
        }
    }
    const QPointer<CanvasItem> target(boundItem);
    if (name.isEmpty()) {
        const QString prefix = suggestedName.trimmed().isEmpty() ? QStringLiteral("Point") : suggestedName.trimmed();
        name = prefix;
        for (int i = 2; names.contains(name); ++i)
            name = prefix + QString::number(i);
        QJsonObject point{{QStringLiteral("name"), name}, {QStringLiteral("binding"), std::move(binding)}};
        if (!editDevicePoint(&draft, &point, QApplication::activeModalWidget() ? QApplication::activeModalWidget() : window(), true))
            return;
        name = point.value(QStringLiteral("name")).toString();
        points = draft.logic.value(QStringLiteral("points")).toArray();
        points.append(point);
        draft.logic.insert(QStringLiteral("points"), points);
        applyProjectDraft(draft);
    }
    if (target)
        target->setBinding({{QStringLiteral("parseMode"), ParseMode::DataPoint}, {QStringLiteral("key"), name}});
    emit projectSettingsChanged({});
    emit message(tr("工程数据点"), tr("已准备好数据点 %1").arg(name), false);
    QMetaObject::invokeMethod(this, [this] { rebuild(); }, Qt::QueuedConnection);
}

void PropertyEditor::setFrameRowsVisible(const QString &prefix, const QString &mode)
{
    const auto config = m_item ? cardFrameConfig() : m_project->parserConfig();
    const auto source = receiveSource(m_item ? ReceiveTarget::Connection : ReceiveTarget::Defaults);
    const bool manual = source.localTemplateId.isEmpty();
    updateReceiveFrameFields({mode, config}, [this, &prefix, manual](const QString &key, bool visible) {
        setRowVisible(prefix + key, manual && visible);
    });
    setRowVisible(prefix + QStringLiteral("preview"), LinkParser::hasEditableFraming(manual ? mode : source.format.type));
}

QJsonObject PropertyEditor::cardFrameConfig() const
{
    return frameFormat(m_project, m_item->properties()).settings;
}

// ---------------------------------------------------------------------------
// Item properties
// ---------------------------------------------------------------------------

QString PropertyEditor::storageKey(const QString &name) const
{
    if (!m_item) {
        return name;
    }
    if (usesChartSeries(m_item->typeName(), m_item->properties()) && curveFields().contains(name)) {
        const int curve = editorCurve(*m_item);
        return QStringLiteral("_curve_%1_%2").arg(curve).arg(curveFields().value(name));
    }
    if (m_item->typeName() == QLatin1String("TabWidget") && name == QLatin1String("tabLayout")) {
        return QStringLiteral("_tabLayout_%1").arg(m_item->editorPage());
    }
    return name;
}

QJsonValue PropertyEditor::itemValue(const PropDef &def) const
{
    const QString key = storageKey(def.name);
    const QJsonValue stored = m_item->properties().value(key);
    if (!stored.isUndefined() && !stored.isNull()) {
        return stored;
    }
    if (def.name == QLatin1String("curveName") && !isTimingChart(m_item->typeName()))
        return chartCurveName(m_item->properties(), editorCurve(*m_item));
    return def.defaultValue;
}

void PropertyEditor::commitProperty(const QString &name, const QJsonValue &value, const QString &detail)
{
    if (m_loading || !m_item) {
        return;
    }
    if ((name==QLatin1String("commandMode") || name==QLatin1String("curveParseMode")) && value.toString()==QLatin1String("S7")) {
        const QString objectName = name==QLatin1String("commandMode") ? QStringLiteral("s7Command") : QStringLiteral("curveS7");
        const auto *def=m_item->meta().prop(objectName);
        if(def && !m_item->properties().contains(storageKey(objectName)))m_item->setPropertyValue(storageKey(objectName),def->defaultValue);
    }
    const QString key = storageKey(name);
    const bool templateContextChanged = m_item->meta().prop(QStringLiteral("commandInputs")) &&
        (name == QLatin1String("commandInputs") || name == QLatin1String("commandMode") ||
         name == QLatin1String("canId") || name == QLatin1String("commandTargetId"));
    const auto previous = templateContextChanged ? m_item->properties() : QJsonObject{};
    const int curves = chartCurveCount(m_item->typeName(), m_item->properties());
    const QString endpoint = endpointProperty(name);
    // A new card starts with its own default destination. A mode change keeps the
    // existing scope until the operator explicitly replaces or clears it.
    if (!endpoint.isEmpty() && m_item->propertyValue(key).toString() != value.toString() &&
        m_rows.contains(endpoint)) {
        m_item->setPropertyValue(storageKey(endpoint), QJsonValue(QJsonValue::Undefined));
        auto *edit = static_cast<QLineEdit *>(m_rows.value(endpoint).second);
        const QSignalBlocker blocker(edit);
        edit->clear();
    }
    m_item->setPropertyValue(key, value);
    if ((name == QLatin1String("commandMode") || name == QLatin1String("canId") || name == QLatin1String("commandTargetId")) &&
        m_item->meta().prop(QStringLiteral("canId")) &&
        m_item->propertyValue(QStringLiteral("commandMode")).toString() == QLatin1String("CAN")) {
        const auto props = m_item->properties();
        if (CommandEncoder::usesFrameFields(props) || !props.value(FrameTemplates::SendValueField).toString().isEmpty()) {
            const auto target = props.value(QStringLiteral("commandTargetId")).toString();
            const auto names = canNames(m_project, currentWidgets(m_project, m_scene),
                                        target == DataRoute::Broadcast ? QString() : target);
            if (!canTargetKind(names, props.value(QStringLiteral("canId")).toString()).usesRawEncoding) {
                if (props.contains(FrameTemplates::SendFieldsEnabled))
                    m_item->setPropertyValue(FrameTemplates::SendFieldsEnabled, false);
                if (props.contains(FrameTemplates::SendValueField))
                    m_item->setPropertyValue(FrameTemplates::SendValueField, QString());
            }
        }
    }
    if (templateContextChanged)
        updateDefaultFrameTemplate(previous);
    if (name == QLatin1String("bitProfile") && m_scene && m_project)
        m_scene->setBitProfiles(m_project->bitProfiles);
    if (name == QLatin1String("mbFunc")) {
        const auto spec = CommandEncoder::modbusFunctionSpec(CommandEncoder::functionCode(value.toString()));
        const QString dataType = m_item->propertyValue(QStringLiteral("mbDataType")).toString();
        if (!spec.dataTypes.isEmpty() && !spec.dataTypes.contains(dataType))
            m_item->setPropertyValue(QStringLiteral("mbDataType"), spec.dataTypes.first());
        if (m_item->meta().prop(QStringLiteral("mbValue"))) {
            bool number = false;
            const int amount = m_item->propertyValue(QStringLiteral("mbValue")).toVariant().toString().toInt(&number);
            if ((spec.argument == CommandEncoder::ModbusArgument::ReadCount &&
                 (!number || amount < 1 || amount > spec.maximumCount)) ||
                (spec.argument == CommandEncoder::ModbusArgument::Coil && (!number || (amount != 0 && amount != 1))))
                m_item->setPropertyValue(QStringLiteral("mbValue"), QStringLiteral("1"));
        }
    }
    if (m_item->typeName() == QLatin1String("CanConfigCard") &&
        (name == QLatin1String("defaultAdapter") || name == QLatin1String("zlgModel"))) {
        const auto caps = CanAdapter::caps(m_item->propertyValue(QStringLiteral("defaultAdapter")).toString(),
                                           m_item->propertyValue(QStringLiteral("zlgModel")).toString());
        const QString channel = QStringLiteral("defaultChannel");
        if (caps.channelCount > 0 && m_item->propertyValue(channel).toInt() >= caps.channelCount)
            m_item->setPropertyValue(channel, caps.channelCount - 1);
        const auto fitPreset = [this](const QString &field, const QStringList &presets) {
            if (!presets.isEmpty() && !presets.contains(m_item->propertyValue(field).toString())) {
                const QString fallback = m_item->meta().prop(field)->defaultValue.toString();
                m_item->setPropertyValue(field, presets.contains(fallback) ? fallback : presets.first());
            }
        };
        fitPreset(QStringLiteral("defaultBitrate"), caps.nominalBitrates);
        fitPreset(QStringLiteral("defaultDataBitrate"), caps.dataBitrates);
    }
    if (name == QLatin1String("curveName") || name == QLatin1String("curveKey") || name == QLatin1String("curveParseMode")) {
        if (auto *box = qobject_cast<FluentQt::ComboBox *>(m_rows.value(QStringLiteral("currentCurve")).second)) {
            const int index = box->currentIndex();
            if (index >= 0)
                box->setItemText(index, editorCurveChoice(*m_item, index));
        }
    }
    // These change which per-curve / per-page values, or which fields, the form shows;
    // defaultParser decides whether a link card has a frame format section, and what it starts from.
    static const QSet<QString> structural = {QStringLiteral("bitProfile"), QStringLiteral("currentCurve"),
                                             QStringLiteral("curveCount"), TimingSource::Property,
                                             QStringLiteral("tabNames"),
                                             QStringLiteral("defaultParser"), QStringLiteral("options"),
                                             QStringLiteral("recordColumns"), QStringLiteral("commandInputs"), QStringLiteral("enumTable"),
                                             QStringLiteral("actions"), QStringLiteral("colorRanges"),
                                             QStringLiteral("feedbackMode"), QStringLiteral("parameterName")};
    const bool modbusEditorChanged = (name == QLatin1String("mbFunc") || name == QLatin1String("mbDataType")) &&
                                     m_item->meta().prop(QStringLiteral("mbValue"));
    const bool numberEditorChanged = m_item->meta().typeName == QLatin1String("SpinBox") &&
                                     (name == QLatin1String("numberMode") || name == QLatin1String("decimals"));
    if (structural.contains(name) || modbusEditorChanged || numberEditorChanged) {
        if (name == QLatin1String("curveCount")) {
            const int count = chartCurveCount(m_item->typeName(), m_item->properties());
            // Added curves are named in the interface language, as a new chart's first one is.
            for (int i = curves; !isTimingChart(m_item->typeName()) && i < count; ++i) {
                const QString curveName = QStringLiteral("_curve_%1_name").arg(i);
                if (m_item->properties().value(curveName).isUndefined()) {
                    m_item->setPropertyValue(curveName, tr("曲线%1").arg(i + 1));
                }
            }
            if (m_item->propertyValue(QStringLiteral("currentCurve")).toInt(0) >= count) {
                m_item->setPropertyValue(QStringLiteral("currentCurve"), count - 1);
            }
        }
        const int scrollPosition = verticalScrollBar()->value();
        const QPointer<CanvasItem> item = m_item;
        QMetaObject::invokeMethod(this, [this, item, scrollPosition] {
            if (item && item == m_item) {
                rebuild();
                verticalScrollBar()->setValue(scrollPosition);
            }
        }, Qt::QueuedConnection);
        emit modified(QStringLiteral("property:%1:%2:%3").arg(m_item->widgetId(), key, detail));
        return;
    }
    if (m_geometry[3] && m_item->isLinkCard()) { // its rows (displayMode) set how short it may be
        m_loading = true;
        m_geometry[3]->setMinimum(int(std::ceil(m_item->minimumHeight())));
        m_loading = false;
        refreshGeometry();
    }
    refreshGeometry();
    refreshNumericRows();
    updateVisibility();
    emit modified(QStringLiteral("property:%1:%2:%3").arg(m_item->widgetId(), key, detail));
}

void PropertyEditor::refreshGeometry()
{
    if (!m_item || !m_geometry[0]) {
        return;
    }
    m_loading = true;
    m_geometry[2]->setMinimum(int(std::ceil(m_item->minimumWidth())));
    m_geometry[3]->setMinimum(int(std::ceil(m_item->minimumHeight())));
    const int values[4] = {qRound(m_item->pos().x()), qRound(m_item->pos().y()), qRound(m_item->size().width()),
                           qRound(m_item->size().height())};
    for (int i = 0; i < 4; ++i) {
        m_geometry[i]->setValue(values[i]);
    }
    m_loading = false;
}

void PropertyEditor::refreshNumericRows()
{
    const auto range = widgetValueRange(m_item->meta(), m_item->properties());
    if (!range && !m_item->meta().prop(QStringLiteral("bitProfile")))
        return;
    for (const char *field : {"minimum", "maximum", "value"}) {
        const QString key = QLatin1String(field);
        QWidget *editor = m_rows.value(key).second;
        if (!editor)
            continue;
        const bool valueRow = key == QLatin1String("value");
        const QVariant value = m_item->propertyValue(key).toVariant();
        const QSignalBlocker blocker(editor);
        if (auto *spin = qobject_cast<QSpinBox *>(editor)) {
            if (valueRow && m_item->meta().prop(QStringLiteral("bitProfile"))) {
                const int width = bitProfileWidth(m_project ? m_project->bitProfiles : QJsonObject{},
                    m_item->propertyValue(QStringLiteral("bitProfile")).toString());
                spin->setRange(0, (1 << width) - 1);
            } else if (valueRow && range) {
                spin->setRange(int(std::ceil(range->minimum)), int(std::floor(range->maximum)));
            }
            spin->setValue(value.toInt());
        } else if (auto *spin = qobject_cast<QDoubleSpinBox *>(editor)) {
            if (valueRow && range)
                spin->setRange(range->minimum, range->maximum);
            spin->setValue(value.toDouble());
        }
    }
}

void PropertyEditor::addPageRows(QFormLayout *form)
{
    const QStringList names = tabNames(m_item->propertyValue(QStringLiteral("tabNames")).toString());
    const int current = m_item->editorPage();
    auto *pages = new FluentQt::ComboBox(m_content);
    pages->setObjectName(QStringLiteral("tabPageSelector"));
    pages->addItems(names);
    pages->setCurrentIndex(current);
    connect(pages, &FluentQt::ComboBox::currentIndexChanged, m_item, &CanvasItem::setEditorPage);
    addRow(form, QStringLiteral("editorPage"), tr("当前编辑页"), pages);
    auto *startup = new FluentQt::ComboBox(m_content);
    startup->setObjectName(QStringLiteral("tabStartupPage"));
    startup->addItems(names);
    startup->setCurrentIndex(clampTabIndex(m_item->propertyValue(QStringLiteral("currentTab")).toInt(0),
                                          int(names.size())));
    connect(startup, &FluentQt::ComboBox::currentIndexChanged, this,
            [this](int index) { commitProperty(QStringLiteral("currentTab"), index); });
    addRow(form, QStringLiteral("currentTab"), tr("运行时启动页"), startup);

    const auto finish = [this]() {
        emit modified(QString());
        QMetaObject::invokeMethod(this, [this]() { rebuild(); }, Qt::QueuedConnection);
    };
    auto *name = new FluentQt::LineEdit(m_content);
    name->setObjectName(QStringLiteral("tabPageName"));
    name->setText(names.at(current));
    connect(name, &QLineEdit::editingFinished, this, [this, name, current, names, finish]() {
        if (m_item && m_item->renamePage(current, name->text())) {
            if (m_scene && names.count(names.at(current)) == 1) {
                for (auto *source : m_scene->allItems()) {
                    auto props = source->properties();
                    editWidgetActions(props, [&](QJsonObject &row) {
                        if (row.value(QStringLiteral("type")).toString() == u"navigate" &&
                            row.value(QStringLiteral("target")) == m_item->widgetId() &&
                            row.value(QStringLiteral("value")) == names.at(current))
                            row.insert(QStringLiteral("value"), name->text().trimmed());
                    });
                    if (props != source->properties())
                        source->setPropertyValue(QStringLiteral("actions"), props.value(QStringLiteral("actions")));
                }
            }
            finish();
        } else if (m_item) {
            name->setText(tabNames(m_item->propertyValue(QStringLiteral("tabNames")).toString()).value(current));
        }
    });
    addRow(form, QStringLiteral("tabNames"), tr("页面名称"), name);

    auto *actions = new QWidget(m_content);
    auto *layout = new QHBoxLayout(actions);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);
    const auto button = [actions, layout](const QString &objectName, FluentQt::FluentIcon icon, const QString &label) {
        auto *control = new FluentQt::ToolButton(actions);
        control->setObjectName(objectName);
        control->setIcon(FluentQt::icon(icon));
        control->setAccessibleName(label);
        fluentToolTip(control, label);
        layout->addWidget(control);
        return control;
    };
    auto *add = button(QStringLiteral("addTabPage"), FluentQt::FluentIcon::Add, tr("添加页面"));
    connect(add, &QToolButton::clicked, this, [this, finish]() {
        if (m_item && m_item->addPage() >= 0)
            finish();
    });
    auto *remove = button(QStringLiteral("removeTabPage"), FluentQt::FluentIcon::Remove, tr("删除当前页面"));
    remove->setEnabled(names.size() > 1);
    connect(remove, &QToolButton::clicked, this, [this, current, finish]() {
        if (m_item && m_item->removePage(current))
            finish();
    });
    auto *previous = button(QStringLiteral("moveTabPagePrevious"), FluentQt::FluentIcon::LeftArrow, tr("页面前移"));
    previous->setEnabled(current > 0);
    connect(previous, &QToolButton::clicked, this, [this, current, finish]() {
        if (m_item && m_item->movePage(current, current - 1))
            finish();
    });
    auto *next = button(QStringLiteral("moveTabPageNext"), FluentQt::FluentIcon::RightArrow, tr("页面后移"));
    next->setEnabled(current + 1 < names.size());
    connect(next, &QToolButton::clicked, this, [this, current, finish]() {
        if (m_item && m_item->movePage(current, current + 1))
            finish();
    });
    layout->addStretch();
    addRow(form, QStringLiteral("tabPageActions"), tr("页面管理"), actions);
}

void PropertyEditor::buildItemForm()
{
    const WidgetMeta &meta = m_item->meta();

    auto *header = new FluentQt::SimpleCardWidget(m_content);
    auto *headerLayout = new QFormLayout(header);
    headerLayout->setContentsMargins(12, 10, 12, 10);
    headerLayout->setSpacing(8);
    headerLayout->addRow(new FluentQt::SubtitleLabel(translated(meta.displayName), header));
    auto *id = new FluentQt::LineEdit(header);
    id->setText(m_item->widgetId());
    connect(id, &QLineEdit::editingFinished, this, [this, id]() {
        if (!m_item || id->text() == m_item->widgetId()) {
            return;
        }
        QString error;
        if (!m_scene || !m_scene->renameItem(m_item, id->text(), &error)) {
            emit message(tr("无法重命名"), error, true);
            id->setText(m_item->widgetId());
        }
    });
    headerLayout->addRow(new FluentQt::BodyLabel(tr("控件 ID"), header), id);
    m_layout->addWidget(header);

    QFormLayout *geometry = addSection(tr("位置与尺寸"));
    const bool automaticWidth = m_item->widthManagedByLayout();
    const QString labels[4] = {QStringLiteral("X"), QStringLiteral("Y"),
                               automaticWidth ? tr("宽度（自动）") : tr("宽度"), tr("高度")};
    for (int i = 0; i < 4; ++i) {
        auto *spin = new FluentQt::SpinBox(m_content);
        spin->setRange(i == 2 ? int(std::ceil(m_item->minimumWidth())) :
                       i == 3 ? int(std::ceil(m_item->minimumHeight())) : -9999, 9999);
        spin->setKeyboardTracking(false);
        m_geometry[i] = spin;
        connect(spin, qOverload<int>(&QSpinBox::valueChanged), this, [this, i](int value) {
            if (m_loading || !m_item) {
                return;
            }
            if (i < 2) {
                QPointF p = m_item->pos();
                (i == 0 ? p.rx() : p.ry()) = value;
                m_item->setPos(p);
            } else {
                QSizeF s = m_item->size();
                (i == 2 ? s.rwidth() : s.rheight()) = value;
                m_item->setSize(s);
            }
            if (CanvasItem *parent = m_item->containerParent()) {
                parent->relayoutChildren();
            }
            emit modified(QStringLiteral("geometry:%1:%2").arg(m_item->widgetId()).arg(i));
        });
        geometry->addRow(new FluentQt::BodyLabel(labels[i], m_content), spin);
    }
    refreshGeometry();
    m_loading = true;
    if (!(m_item->flags() & QGraphicsItem::ItemIsMovable)) {
        // A layout container places this child; only its size is the user's.
        m_geometry[0]->setEnabled(false);
        m_geometry[1]->setEnabled(false);
    }
    m_geometry[2]->setEnabled(!automaticWidth);

    buildItemProperties();
    refreshNumericRows();
    if (meta.prop(QStringLiteral("bitProfile")) &&
        !(isTimingChart(meta.typeName) && TimingSource::usesChannels(m_item->properties())))
        buildBitProfilesForm();
    // A link card with a mode of its own frames with its own settings, edited like the project's.
    const PropDef *frame = meta.prop(QStringLiteral("parserConfig"));
    const QString parser = m_item->propertyValue(QStringLiteral("defaultParser")).toString();
    if (frame && FrameTemplates::receiveIds(m_item->properties()).isEmpty() &&
        LinkParser::hasEditableFraming(parser)) {
        addFrameRows(
            addSection(tr("帧格式")), QStringLiteral("parserConfig."), [this]() { return cardFrameConfig(); },
            [this](const QJsonObject &config, const QString &key) {
                commitProperty(QStringLiteral("parserConfig"), config, key);
            });
    }
}

void PropertyEditor::buildItemProperties()
{
    const auto &meta = m_item->meta();
    const bool timing = isTimingChart(meta.typeName);
    const bool timingChannels = timing && TimingSource::usesChannels(m_item->properties());
    const bool hasCommands = meta.prop(QStringLiteral("commandMode"));
    const QStringList firstSendFields{QStringLiteral("actions"), commandActivationProperty(meta.typeName), QStringLiteral("commandMode"),
                                     QStringLiteral("commandTargetId"), QStringLiteral("commandEndpoint"),
                                     CommandEncoder::SendInterval,
                                     QStringLiteral("confirmBeforeSend"), QStringLiteral("showSendConfirmationDetails")};
    QList<const PropDef *> general;
    QList<const PropDef *> send;
    QList<const PropDef *> receive;
    for (const QString &name : firstSendFields) {
        if (const auto *def = meta.prop(name))
            send.append(def);
    }
    static const QSet<QString> feedbackFields = {
        QStringLiteral("sourceCardId"), QStringLiteral("sourceEndpoint"), QStringLiteral("canFilter"),
        QStringLiteral("onCondition"), QStringLiteral("onValue"), QStringLiteral("offValue"),
        QStringLiteral("offFeedbackValue"), QStringLiteral("currentCurve"), QStringLiteral("curveCount")};
    for (const PropDef &def : meta.properties) {
        const QString &name = def.name;
        if (firstSendFields.contains(name))
            continue;
        if (timing && (name == QLatin1String("currentCurve") || name == QLatin1String("curveCount") ||
                       curveFields().contains(name)) && !timingChannels)
            continue;
        if (timingChannels && name == QLatin1String("value"))
            continue;
        if ((hasCommands && (modeFields().contains(name) || name == QLatin1String("commandInputs") ||
                             name == QLatin1String("commands") ||
                             name.startsWith(QLatin1String("notify")))) || name.startsWith(QLatin1String("canTxInitial"))) {
            send.append(&def);
        } else if (feedbackFields.contains(name) || curveFields().contains(name)) {
            receive.append(&def);
        } else {
            general.append(&def);
        }
    }
    if (!general.isEmpty()) {
        auto *form = addSection(tr("常规"));
        addItemProperties(form, general);
        if (meta.typeName == QLatin1String("CanConfigCard") && m_project) {
            auto *save = new FluentQt::TransparentPushButton(tr("从 CAN 信号创建数据点…"), m_content);
            addRow(form, {}, {}, save);
            connect(save, &QPushButton::clicked, this, [this] {
                const auto source = m_item->widgetId();
                if (const auto picked = chooseCanSignal(m_project, currentWidgets(m_project, m_scene), source, {},
                                                        window(), true))
                    addDataPoint({{QStringLiteral("parseMode"), ParseMode::CanField},
                                  {QStringLiteral("sourceCardId"), picked->connectionId},
                                  {QStringLiteral("key"), picked->key}}, picked->name);
            });
        }
    }
    if (!send.isEmpty()) {
        auto *form = addSection(tr("操作与发送"));
        addItemProperties(form, send);
        if (meta.prop(QStringLiteral("canId")) && !meta.prop(QStringLiteral("commandInputs"))) {
            auto *signal = new FluentQt::PushButton(tr("选择写入信号…"), m_content);
            signal->setObjectName(QStringLiteral("pickCanWriteSignal"));
            addRow(form, QStringLiteral("canWriteSignal"), tr("CAN 信号"), signal);
            connect(signal, &QPushButton::clicked, this, [this] {
                const QPointer<CanvasItem> item = m_item;
                const auto props = item->properties();
                const auto choice = chooseCanSignal(m_project, currentWidgets(m_project, m_scene),
                    props.value(QStringLiteral("commandTargetId")).toString(), {}, window());
                if (!choice || !item || item != m_item)
                    return;
                if (item->typeName() == u"ToggleSwitch") {
                    AppDialog dialog(tr("开关写入值 · %1").arg(choice->key), window());
                    dialog.setPreferredSize({510, 280});
                    auto *form = new QFormLayout;
                    auto *on = new FluentQt::LineEdit(dialog.widget());
                    auto *off = new FluentQt::LineEdit(dialog.widget());
                    on->setText(QStringLiteral("1"));
                    off->setText(QStringLiteral("0"));
                    form->addRow(tr("开启值"), on);
                    form->addRow(tr("关闭值"), off);
                    dialog.viewLayout()->addLayout(form);
                    dialog.setValidator([&] { return !on->text().trimmed().isEmpty() && !off->text().trimmed().isEmpty(); });
                    if (dialog.exec() != QDialog::Accepted || !item || item != m_item)
                        return;
                    item->setPropertyValue(QStringLiteral("onCommand"), on->text().trimmed());
                    item->setPropertyValue(QStringLiteral("offCommand"), off->text().trimmed());
                }
                item->setPropertyValue(QStringLiteral("commandTargetId"), choice->connectionId);
                item->setPropertyValue(QStringLiteral("commandEndpoint"), QString());
                item->setPropertyValue(QStringLiteral("canId"), choice->key.section(u'.', 0, -2));
                item->setPropertyValue(QStringLiteral("commandTemplate"), choice->name + QStringLiteral("={value}"));
                item->setPropertyValue(FrameTemplates::SendValueField, QString());
                const int scroll = verticalScrollBar()->value();
                rebuild();
                verticalScrollBar()->setValue(scroll);
                emit modified({});
            });
        }
        if (m_project && (meta.prop(QStringLiteral("mbSlave")) || meta.prop(QStringLiteral("s7Command")))) {
            if (!meta.prop(QStringLiteral("commandInputs"))) {
                auto *point = new FluentQt::ComboBox(m_content);
                point->setObjectName(QStringLiteral("writeDevicePoint"));
                addRow(form, QStringLiteral("writeDevicePoint"), tr("写入对象"), point);
                connect(point, &FluentQt::ComboBox::currentIndexChanged, this, [this, point] {
                    const auto key = writePointKey(m_item->propertyValue(QStringLiteral("commandMode")).toString());
                    if (!key.isEmpty())
                        commitProperty(key, selectedValue(point));
                });
            }
            auto *summary = new FluentQt::BodyLabel(m_content);
            summary->setWordWrap(true);
            summary->setObjectName(QStringLiteral("effectiveWriteTarget"));
            addRow(form, QStringLiteral("effectiveWriteTarget"), tr("实际写入目标"), summary);
        }
        if (meta.typeName == QLatin1String("ToggleSwitch")) {
            auto *write = new FluentQt::BodyLabel(m_content);
            write->setWordWrap(true);
            addRow(form, QStringLiteral("modbusSwitchWrite"), tr("拨动时写入"), write);
        }
        if (hasCommands || meta.typeName == QLatin1String("DataSendCard")) {
            if (m_project)
                addTemplateChoiceRow(form, QStringLiteral("sendTemplate"), tr("发送帧模板"), [this](const QString &id) {
                    commitProperty(FrameTemplates::SendId, id);
                }, [this] {
                    const auto props = m_item->properties();
                    const QString mode = props.value(QStringLiteral("commandMode")).toString();
                    const auto parser = FrameTemplates::sendParser(mode);
                    QJsonObject config;
                    if (parser == QLatin1String("hex")) {
                        config = props.value(QStringLiteral("txFrame")).toObject();
                        if (mode == QLatin1String("HEX")) {
                            const auto head = props.value(QStringLiteral("frameHeader")).toString();
                            const auto tail = props.value(QStringLiteral("frameTail")).toString();
                            if (!config.isEmpty() || !head.isEmpty() || !tail.isEmpty()) {
                                config.insert(QStringLiteral("header"), head);
                                config.insert(QStringLiteral("footer"), tail);
                            }
                        }
                    }
                    const auto id = props.value(FrameTemplates::SendId).toString();
                    const auto entry = FrameTemplates::find(m_project->frameTemplates, id);
                    manageFrameTemplates({parser, config}, entry && entry->format.type == parser ? id : QString());
                });
            if (!meta.prop(QStringLiteral("commandInputs"))) {
                if (m_project && meta.typeName != QLatin1String("DataSendCard")) {
                    auto *field = new FluentQt::ComboBox(m_content);
                    field->setObjectName(QStringLiteral("sendValueTemplateField"));
                    addRow(form, FrameTemplates::SendValueField, tr("发送字段"), field);
                    connect(field, &FluentQt::ComboBox::currentIndexChanged, this, [this, field] {
                        commitProperty(FrameTemplates::SendValueField, selectedValue(field));
                    });
                }
                auto *format = new FluentQt::PushButton(tr("可视化编辑帧格式…"), m_content);
                format->setObjectName(QStringLiteral("editTransmitFrameFormat"));
                addRow(form, QStringLiteral("transmitFormat"), tr("发送帧格式"), format);
                connect(format, &QPushButton::clicked, this, &PropertyEditor::editTransmitFormat);
            }
            auto *preview = new FluentQt::PushButton(tr("发送预览"), m_content);
            preview->setObjectName(QStringLiteral("sendPreview"));
            addRow(form, QStringLiteral("sendPreview"), {}, preview);
            connect(preview, &QPushButton::clicked, this, &PropertyEditor::showSendPreview);
        }
    }
    const bool binds = !meta.bindProperty().isEmpty();
    if (binds || !receive.isEmpty()) {
        auto *form = addSection(meta.typeName == QLatin1String("PushButton") ? tr("标题绑定") : tr("接收与绑定"));
        if (binds && !timingChannels)
            buildBindingForm(form);
        addItemProperties(form, receive);
        if (meta.typeName == QLatin1String("RealtimeChart"))
            addReceiveEditorRow(form, QStringLiteral("curve.receiveFormat"), ReceiveTarget::Curve);
        else if (meta.typeName == QLatin1String("DataRecvCard") || meta.typeName == QLatin1String("LogBox") ||
                 meta.typeName == QLatin1String("DataTable"))
            addReceiveEditorRow(form, QStringLiteral("observer.receiveFormat"), ReceiveTarget::Observer);
    }
}

void PropertyEditor::showSendPreview()
{
    if (!m_item)
        return;
    const QString mode = m_item->propertyValue(QStringLiteral("commandMode")).toString();
    auto project = m_project ? *m_project : Project{};
    project.widgets = currentWidgets(m_project, m_scene);
    const auto &widgets = project.widgets;
    QString pointProblem;
    const auto props = m_item->properties();
    const auto effective = resolvePointWrite(props, project.logic, widgets, project.parserType(), &pointProblem,
                                             m_item->typeName(), ModbusParserChoice::OnConnect);
    const auto destination = effective ? previewCommandTarget(*effective, m_item->typeName(), project)
                                       : CommandTargetPreview{{}, {}, pointProblem, true};
    const auto resolved = destination.route.isBroadcast() ? QString() : destination.route.connectionId;
    const auto sources = mode == QLatin1String("CAN") ? canNames(m_project, widgets, resolved).sources
                                                     : QList<CanPreviewSource>{};
    const auto inputs = m_item->typeName() == QLatin1String("PushButton") &&
                        !CommandEncoder::commandInputIds(m_item->properties()).isEmpty()
        ? inputDefaults(widgets, m_project) : QMap<QString, CommandEncoder::InputValue>{};
    openCommandPreview(*m_item, sources, destination, inputs, window(), effective.value_or(props), project.frameTemplates,
                       project.enumTables);
}

void PropertyEditor::editTransmitFormat()
{
    const QPointer<CanvasItem> item = m_item;
    const auto properties = item->properties();
    const auto changes = editTransmitFrameFormat(properties,
        item->propertyValue(QStringLiteral("commandMode")).toString() == QLatin1String("CAN"), window());
    if (!item || !changes || changes->isEmpty())
        return;
    for (auto it = changes->cbegin(); it != changes->cend(); ++it)
        item->setPropertyValue(it.key(), it.value());
    emit modified({});
    const int scroll = verticalScrollBar()->value();
    QMetaObject::invokeMethod(this, [this, scroll] {
        rebuild();
        verticalScrollBar()->setValue(scroll);
    }, Qt::QueuedConnection);
}

void PropertyEditor::updateDefaultFrameTemplate(const QJsonObject &previous)
{
    using CommandEncoder::InputListMode;
    const auto properties = m_item->properties();
    const auto mode = CommandEncoder::inputListMode(properties);
    if (!CommandEncoder::usesTemplate(mode))
        return;

    const auto before = CommandEncoder::commandInputs(previous);
    const auto after = CommandEncoder::commandInputs(properties);
    if (before.isEmpty() && after.isEmpty())
        return;

    const bool usesCan = mode == InputListMode::Can || CommandEncoder::inputListMode(previous) == InputListMode::Can;
    const auto widgets = usesCan ? currentWidgets(m_project, m_scene) : QJsonArray{};
    const auto inputs = usesCan ? inputDefaults(widgets, m_project) : QMap<QString, CommandEncoder::InputValue>{};
    const QString key = QStringLiteral("commandTemplate");
    const QString current = m_item->propertyValue(key).toString();
    const QString generatedBefore = defaultFrameTemplate(previous, commandCanSources(m_project, widgets, previous), inputs);
    const bool usesValueVariable = std::any_of(before.cbegin(), before.cend(), [](const auto &input) {
        return input.variable == QLatin1String("value");
    });
    if (!current.trimmed().isEmpty() && !(current == QLatin1String("{value}") && !usesValueVariable) &&
        current != generatedBefore)
        return;

    const QString generated = defaultFrameTemplate(properties, commandCanSources(m_project, widgets, properties), inputs);
    const QString next = generated.isEmpty() ? QStringLiteral("{value}") : generated;
    if (next == current)
        return;
    m_item->setPropertyValue(key, next);
}

void PropertyEditor::editFrameTemplate()
{
    const QPointer<CanvasItem> item = m_item;
    const QPointer<DesignScene> scene = m_scene;
    const bool canCreate = scene && item->scene() == scene;
    const auto widgets = currentWidgets(m_project, m_scene);
    Project project = m_project ? *m_project : Project{};
    project.widgets = widgets;
    openFrameTemplateEditor(*item, project, commandCanSources(m_project, widgets, item->properties()), inputDefaults(widgets, m_project),
                            canCreate, window(), [self = QPointer<PropertyEditor>(this), item, scene](
                                const QJsonObject &draft, const QMap<QString, QString> &pending) {
        if (!self || !item || self->m_item != item)
            return;
        if (!pending.isEmpty() && (!scene || item->scene() != scene))
            return;
        QJsonObject changes = draft;
        QHash<QString, QString> created;
        const auto applyInput = [item, scene, &created, &pending](const QString &id) {
            if (!pending.contains(id))
                return id;
            if (!created.contains(id)) {
                const auto *input = scene->insertSibling(*item, QStringLiteral("LineEdit"),
                    {{QStringLiteral("title"), pending.value(id)}, {QStringLiteral("inputOnly"), true}});
                created.insert(id, input->widgetId());
            }
            return created.value(id);
        };
        if (!pending.isEmpty())
            CommandEncoder::mapInputIds(changes, applyInput);
        bool changed = false;
        for (auto it = changes.begin(); it != changes.end(); ++it) {
            if (item->propertyValue(it.key()) != it.value()) {
                item->setPropertyValue(it.key(), it.value());
                changed = true;
            }
        }
        if (changed) {
            const int scroll = self->verticalScrollBar()->value();
            self->rebuild();
            self->verticalScrollBar()->setValue(scroll);
            emit self->modified(QString());
        }
    });
}

void PropertyEditor::addItemProperties(QFormLayout *form, const QList<const PropDef *> &properties)
{
    for (const PropDef *def : properties) {
        const QString &name = def->name;
        if (name == QLatin1String("currentTab") || name == QLatin1String("bitProfile") ||
            (name == u"enumTable" && m_item->typeName() != u"ComboBox") || name == u"ledEffects" ||
            name.startsWith(QLatin1String("curveCan")))
            continue;
        if (name == QLatin1String("tabNames")) {
            addPageRows(form);
        } else if (name == u"enumTable") {
            auto *box = new FluentQt::ComboBox(m_content);
            box->addItem(tr("本地选项"), {}, QString());
            if (m_project)
                for (const auto &table : m_project->enumTables.keys())
                    box->addItem(table, {}, table);
            selectValue(box, itemValue(*def).toString());
            auto *row = new QWidget(m_content);
            auto *layout = new QHBoxLayout(row);
            layout->setContentsMargins(0, 0, 0, 0);
            layout->addWidget(box, 1);
            auto *edit = new FluentQt::TransparentToolButton(FluentQt::icon(FluentQt::FluentIcon::Edit), row);
            edit->setObjectName(QStringLiteral("editEnumMapping"));
            keepSquareBeside(edit, box);
            edit->setAccessibleName(tr("编辑工程枚举"));
            fluentToolTip(edit, edit->accessibleName());
            edit->setVisible(m_project != nullptr);
            layout->addWidget(edit);
            addRow(form, name, translated(def->label), row);
            connect(edit, &QToolButton::clicked, this, [this, name] {
                const QPointer<CanvasItem> item = m_item;
                const auto result = editEnumMapping(item->typeName(), item->properties(), m_project->enumTables, true, window());
                if (!result || !item || item != m_item)
                    return;
                const bool changed = result->tables != m_project->enumTables;
                m_project->enumTables = result->tables;
                commitProperty(name, result->tableName);
                if (changed)
                    emit projectSettingsChanged({});
            });
            connect(box, &FluentQt::ComboBox::currentIndexChanged, this, [this, box, name] {
                commitProperty(name, selectedValue(box));
            });
        } else if (name == u"textList") {
            auto *edit = new FluentQt::PushButton(m_content);
            edit->setObjectName(QStringLiteral("editEnumMapping"));
            const auto refresh = [this, edit] {
                const auto table = m_item->propertyValue(QStringLiteral("enumTable")).toString();
                edit->setText(table.isEmpty() ? tr("编辑枚举…") : tr("编辑枚举 · %1").arg(table));
            };
            refresh();
            addRow(form, name, translated(def->label), edit, def->helpKey);
            connect(edit, &QPushButton::clicked, this, [this, refresh] {
                const QPointer<CanvasItem> item = m_item;
                const QJsonObject tables = m_project ? m_project->enumTables : QJsonObject();
                const auto result = editEnumMapping(item->typeName(), item->properties(), tables, m_project != nullptr, window());
                if (!result || !item || item != m_item)
                    return;
                const auto set = [item](const QString &key, const QJsonValue &value) {
                    if (item->propertyValue(key) != value)
                        item->setPropertyValue(key, value);
                };
                set(QStringLiteral("textList"), result->localMapping);
                set(QStringLiteral("enumTable"), result->tableName);
                if (item->typeName() == u"LEDIndicator")
                    set(QStringLiteral("ledEffects"), result->effects);
                if (m_project && result->tables != tables) {
                    m_project->enumTables = result->tables;
                    emit projectSettingsChanged({});
                } else {
                    emit modified({});
                }
                refresh();
            });
        } else if (name == QLatin1String("canSignals")) {
            auto *edit = new FluentQt::PushButton(tr("可视化编辑…"), m_content);
            edit->setObjectName(QStringLiteral("editCanSignals"));
            addRow(form, name, translated(def->label), edit, def->helpKey);
            connect(edit, &QPushButton::clicked, this, [this, name] {
                const QPointer<CanvasItem> item = m_item;
                std::optional<Project> reviewed;
                const auto approve = [&](const CanSignalEdit &edit, QWidget *parent) {
                    if (!item || m_item != item)
                        return false;
                    if (!m_project || item->propertyValue(name).toString() == edit.text)
                        return true;
                    Project draft = *m_project;
                    draft.widgets = currentWidgets(m_project, m_scene);
                    const auto rows = stageCanSignalEdit(draft, item->widgetId(), edit);
                    if (!rows.isEmpty() && !confirmReferences(parent, tr("CAN 信号变更影响范围"),
                            tr("确认后会更新可唯一定位的引用。"), rows,
                            tr("继续保存"), tr("返回编辑"), QStringLiteral("canSignalImpact")))
                        return false;
                    reviewed = std::move(draft);
                    return true;
                };
                const auto edit = editCanSignals(m_project, item->properties(), window(), approve);
                if (!edit || !item || m_item != item || item->propertyValue(name).toString() == edit->text)
                    return;
                if (reviewed) {
                    applyProjectDraft(*reviewed);
                    emit projectSettingsChanged({});
                    QMetaObject::invokeMethod(this, [this] { rebuild(); }, Qt::QueuedConnection);
                } else {
                    commitProperty(name, edit->text);
                }
            });
        } else if (name == u"canTxInitialState" || name == u"canFilter") {
            const bool mapping = name == u"canTxInitialState";
            const auto summaryLabel = [mapping](const QString &val) {
                if (mapping) {
                    const int n = countNonEmptyLines(val);
                    return n > 0 ? tr("编辑初值映射（%1 条）…").arg(n) : tr("编辑初值映射…");
                }
                const QString trimmed = val.trimmed();
                if (trimmed.isEmpty())
                    return tr("编辑接收过滤…");
                const QString brief = trimmed.size() > 14 ? trimmed.left(13) + QChar(0x2026) : trimmed;
                return tr("编辑接收过滤 · %1").arg(brief);
            };
            auto *edit = new FluentQt::PushButton(summaryLabel(itemValue(*def).toString()), m_content);
            edit->setObjectName(mapping ? QStringLiteral("editCanInitialMapping") : QStringLiteral("editCanReceiveFilter"));
            addRow(form, name, translated(def->label), edit, def->helpKey);
            connect(edit, &QPushButton::clicked, this, [this, name, mapping, edit, summaryLabel] {
                const QPointer<CanvasItem> item = m_item;
                const auto result = mapping ? editCanInitialMapping(m_project, item->toJson(), window())
                                            : editCanReceiveFilter(m_project, item->toJson(), window());
                if (item && item == m_item && result) {
                    commitProperty(name, *result);
                    edit->setText(summaryLabel(*result));
                }
            });
        } else if (name == u"slaveInit") {
            const auto slaveInitLabel = [](const QString &val) {
                const int n = countSlaveInitEntries(val);
                return n > 0 ? tr("编辑寄存器初始值（%1 项）…").arg(n) : tr("编辑寄存器初始值…");
            };
            auto *edit = new FluentQt::PushButton(slaveInitLabel(itemValue(*def).toString()), m_content);
            edit->setObjectName(QStringLiteral("editSlaveInitialValues"));
            addRow(form, name, translated(def->label), edit, def->helpKey);
            connect(edit, &QPushButton::clicked, this, [this, name, edit, slaveInitLabel] {
                const QPointer<CanvasItem> item = m_item;
                const auto properties = item->properties();
                AppDialog dialog(tr("Modbus 从站初始值"), window());
                dialog.setObjectName(QStringLiteral("modbusInitialValuesDialog"));
                dialog.setPreferredSize({640, 535});
                auto *form = new QFormLayout;
                auto *area = new FluentQt::ComboBox(dialog.widget());
                addValueItems(area, ModbusArea::options());
                form->addRow(tr("寄存器区"), area);
                dialog.viewLayout()->addLayout(form);
                auto *hint = new FluentQt::CaptionLabel(tr("双击值可修改，保存后作为从站启动时的初始值。"), dialog.widget());
                hint->setWordWrap(true);
                dialog.viewLayout()->addWidget(hint);
                auto *table = new ModbusSlaveTableWidget(*WidgetRegistry::instance().find(QStringLiteral("ModbusSlaveTable")),
                                                         dialog.widget());
                const auto update = [table, area] {
                    table->setProperties({{QStringLiteral("mbArea"), selectedValue(area)}});
                };
                update();
                table->showDesign(&properties, false, true);
                connect(area, &FluentQt::ComboBox::currentIndexChanged, &dialog, update);
                dialog.viewLayout()->addWidget(table, 1);
                if (dialog.exec() == QDialog::Accepted && item && item == m_item) {
                    const QString values = table->initialValues();
                    commitProperty(name, values);
                    edit->setText(slaveInitLabel(values));
                }
            });
        } else if (name == QLatin1String("commandInputs")) {
            auto *edit = new FluentQt::PushButton(tr("编辑发送配置"), m_content);
            edit->setObjectName(QStringLiteral("editFrameTemplate"));
            addRow(form, name, tr("发送配置"), edit);
            connect(edit, &QPushButton::clicked, this, &PropertyEditor::editFrameTemplate);
        } else if (!def->columns.isEmpty()) {
            const auto rows = itemValue(*def).toArray();
            auto *edit = new FluentQt::PushButton(tr("编辑（%1 项）").arg(rows.size()), m_content);
            addRow(form, name, translated(def->label), edit, def->helpKey);
            connect(edit, &QPushButton::clicked, this, [this, def] { editPropertyRows(*def); });
        } else if (def->type == PropType::Object) {
            if (name == QLatin1String("s7Command") || name == QLatin1String("curveS7"))
                addS7Rows(form, *def);
        } else {
            auto *editor = createEditor(*def, itemValue(*def), [this, name](const QJsonValue &v) { commitProperty(name, v); });
            if (name == u"canId" && qobject_cast<FluentQt::LineEdit *>(editor)) {
                auto *line = static_cast<FluentQt::LineEdit *>(editor);
                auto *pick = new QAction(FluentQt::icon(FluentQt::FluentIcon::ChevronDownMed), tr("选择 CAN 报文"), line);
                line->addAction(pick, QLineEdit::TrailingPosition);
                connect(pick, &QAction::triggered, this, [this, line] {
                    const QPointer<CanvasItem> item = m_item;
                    const auto props = item->properties();
                    if (const auto frame = chooseCanMessage(m_project, currentWidgets(m_project, m_scene),
                            props.value(QStringLiteral("commandTargetId")).toString(), line->text(), window());
                        frame && item && item == m_item) {
                        item->setPropertyValue(QStringLiteral("commandTargetId"), frame->connectionId);
                        item->setPropertyValue(QStringLiteral("commandEndpoint"), QString());
                        const QSignalBlocker blocker(line);
                        line->setText(frame->key);
                        commitProperty(QStringLiteral("canId"), frame->key);
                        QMetaObject::invokeMethod(this, [this] { rebuild(); }, Qt::QueuedConnection);
                    }
                });
            }
            QString label = translated(def->label);
            if (name == QLatin1String("mbValue")) {
                const int function = CommandEncoder::functionCode(m_item->propertyValue(QStringLiteral("mbFunc")).toString());
                const auto argument = CommandEncoder::modbusFunctionSpec(function).argument;
                label = argument == CommandEncoder::ModbusArgument::ReadCount ? tr("读取数量")
                      : argument == CommandEncoder::ModbusArgument::Coil ? tr("写入状态") : tr("写入值");
            }
            addRow(form, name, label, editor, def->helpKey);
        }
        if (name == QLatin1String("defaultParser") && m_project) {
            addReceiveTemplateRow(form, QStringLiteral("connection.receiveTemplate"), ReceiveTarget::Connection);
            const auto chosen = FrameTemplates::receiveIds(m_item->properties());
            const bool inherited = chosen.isEmpty() && LinkParser::inheritsProject(m_item->propertyValue(name).toString());
            if (inherited && m_scene) {
                auto *settings = new FluentQt::TransparentPushButton(tr("工程解析设置"), m_content);
                addRow(form, {}, {}, settings);
                connect(settings, &QPushButton::clicked, this, [this] {
                    m_scene->clearSelection();
                    if (m_item)
                        showItem(nullptr);
                    ensureWidgetVisible(m_rows.value(QStringLiteral("parser")).second);
                });
            }
            if (!chosen.isEmpty())
                addReceivePreviewRow(form, QStringLiteral("templateParserPreview"));
            else if (inherited && LinkParser::hasEditableFraming(m_project->parserType()))
                addReceivePreviewRow(form, QStringLiteral("inheritedParserPreview"));
        }
        if (name == QLatin1String("curveKey"))
            addCanBindingRow(form, true);
        if (name == QLatin1String("highLimit"))
            addRow(form, QStringLiteral("limitsProblem"), {}, newProblemLabel(m_content));
    }
}

void PropertyEditor::addCanBindingRow(QFormLayout *form, bool curve)
{
    auto *row = new QWidget(m_content);
    auto *layout = new QVBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);
    auto *summary = new FluentQt::CaptionLabel(row);
    summary->setWordWrap(true);
    auto *edit = new FluentQt::PushButton(row);
    layout->addWidget(summary);
    layout->addWidget(edit);
    addRow(form, curve ? QStringLiteral("curve.canSignal") : QStringLiteral("binding.canSignal"), tr("CAN 信号"), row);
    if (curve)
        addRow(form, QStringLiteral("curveCanProblem"), {}, newProblemLabel(m_content));
    if (curve) {
        auto *batch = new FluentQt::PushButton(tr("批量添加 CAN 信号…"), m_content);
        batch->setObjectName(QStringLiteral("addCanSignalsToChart"));
        addRow(form, QStringLiteral("curve.canBatch"), {}, batch);
        connect(batch, &QPushButton::clicked, this, [this, batch] {
            const QPointer<CanvasItem> item = m_item;
            const auto props = item->properties();
            const int count = chartCurveCount(item->typeName(), props);
            const int limit = isTimingChart(item->typeName()) ? MaxTimingChannels : MaxRealtimeCurves;
            QList<int> free;
            QSet<QPair<QString, QString>> bound;
            for (int i = 0; i < limit; ++i) {
                const auto binding = chartCurveBinding(props, i);
                const auto mode = binding.value(QStringLiteral("parseMode")).toString();
                if (mode == ParseMode::CanField)
                    bound.insert({binding.value(QStringLiteral("sourceCardId")).toString(),
                                  binding.value(QStringLiteral("key")).toString()});
                if (mode == ParseMode::None || (mode == ParseMode::KeyValue && binding.value(QStringLiteral("key")).toString().isEmpty()))
                    free.append(i);
            }
            if (free.isEmpty()) {
                FluentQt::InfoBar::warning(tr("没有可用通道"), tr("请先解除一个通道的绑定。"), Qt::Horizontal,
                                          true, 3000, FluentQt::InfoBarPosition::TopRight, window());
                return;
            }
            const auto source = chartCurveBinding(props, editorCurve(*item)).value(QStringLiteral("sourceCardId")).toString();
            const auto selected = chooseCanSignals(m_project, currentWidgets(m_project, m_scene), source, free.size(), batch);
            if (selected.isEmpty() || !item || item != m_item)
                return;
            int firstAdded = -1;
            int newCount = count;
            for (const auto &signal : selected) {
                if (bound.contains({signal.connectionId, signal.key}))
                    continue;
                const int index = free.takeFirst();
                if (firstAdded < 0)
                    firstAdded = index;
                newCount = std::max(newCount, index + 1);
                const auto prefix = QStringLiteral("_curve_%1_").arg(index);
                item->setPropertyValue(prefix + QStringLiteral("name"), signal.name);
                item->setPropertyValue(prefix + QStringLiteral("parseMode"), ParseMode::CanField);
                item->setPropertyValue(prefix + QStringLiteral("sourceCardId"), signal.connectionId);
                item->setPropertyValue(prefix + QStringLiteral("sourceEndpoint"), QString());
                item->setPropertyValue(prefix + QStringLiteral("key"), signal.key);
                item->setPropertyValue(prefix + QStringLiteral("expression"), QString());
            }
            if (firstAdded < 0)
                return;
            item->setPropertyValue(QStringLiteral("curveCount"), newCount);
            item->setPropertyValue(QStringLiteral("currentCurve"), firstAdded);
            const int scroll = verticalScrollBar()->value();
            rebuild();
            verticalScrollBar()->setValue(scroll);
            emit modified({});
        });
    }
    connect(edit, &QPushButton::clicked, this, [this, curve, edit] {
        const QPointer<CanvasItem> item = m_item;
        const int index = curve ? editorCurve(*item) : -1;
        const auto binding = curve ? chartCurveBinding(item->properties(), index) : item->binding();
        const auto widgets = currentWidgets(m_project, m_scene);
        const bool named = binding.value(QStringLiteral("parseMode")).toString() == ParseMode::CanField;
        QJsonObject changes;
        if (named) {
            const auto picked = chooseCanSignal(m_project, widgets,
                                                binding.value(QStringLiteral("sourceCardId")).toString(),
                                                binding.value(QStringLiteral("key")).toString(), edit);
            if (!picked)
                return;
            changes = {{QStringLiteral("sourceCardId"), picked->connectionId},
                       {QStringLiteral("sourceEndpoint"), QString()},
                       {QStringLiteral("key"), picked->key}};
        } else {
            const DataRoute route{binding.value(QStringLiteral("sourceCardId")).toString(),
                                  binding.value(QStringLiteral("sourceEndpoint")).toString()};
            const auto source = resolveProjectRoute(widgets, m_project ? m_project->parserType() : QStringLiteral("line"),
                                                    RouteTransport::Can, route, RoutePurpose::DataPoint);
            QJsonObject card;
            if (source && !source->connectionId.isEmpty()) {
                visitWidgets(widgets, [&](const QJsonObject &widget) {
                    if (widget.value(QStringLiteral("id")).toString() != source->connectionId)
                        return true;
                    card = widget.value(QStringLiteral("properties")).toObject();
                    return false;
                });
            }
            const auto fields = editCanBitBinding(m_project, binding, card, edit);
            if (!fields)
                return;
            changes = *fields;
        }
        if (!item || m_item != item || (curve && editorCurve(*item) != index))
            return;
        if (curve) {
            const auto prefix = QStringLiteral("_curve_%1_").arg(index);
            for (auto it = changes.cbegin(); it != changes.cend(); ++it)
                item->setPropertyValue(prefix + it.key(), it.value());
        } else {
            auto updated = binding;
            for (auto it = changes.cbegin(); it != changes.cend(); ++it)
                updated.insert(it.key(), it.value());
            item->setBinding(updated);
        }
        const int scroll = verticalScrollBar()->value();
        rebuild();
        verticalScrollBar()->setValue(scroll);
        emit modified(QString());
    });
}

void PropertyEditor::updateCanBindingRow(const QJsonObject &binding, bool curve)
{
    const auto key = curve ? QStringLiteral("curve.canSignal") : QStringLiteral("binding.canSignal");
    const auto mode = binding.value(QStringLiteral("parseMode")).toString();
    const bool named = mode == ParseMode::CanField;
    setRowVisible(key, named || mode == ParseMode::CanSignal);
    if (!named && mode != ParseMode::CanSignal)
        return;
    auto *row = m_rows.value(key).second;
    auto *edit = row->findChild<QPushButton *>();
    auto *summary = row->findChild<QLabel *>();
    QString text;
    if (named) {
        text = binding.value(QStringLiteral("key")).toString();
    } else {
        const auto id = binding.value(QStringLiteral("canId")).toString();
        if (!id.isEmpty()) {
            const auto address = Can::parseId(id);
            const auto span = tr("起始位 %1 · %2 位").arg(binding.value(QStringLiteral("canStartBit")).toInt())
                                  .arg(binding.value(QStringLiteral("canBitLength")).toInt());
            text = QStringLiteral("%1 · %2 · %3").arg(address ? Can::idText(*address) : id, span,
                binding.value(QStringLiteral("canByteOrder")).toString(CanByteOrder::Intel));
        }
    }
    summary->setText(text);
    summary->setVisible(!text.isEmpty());
    edit->setText(named ? tr("可视化选信号…") : tr("可视化编辑…"));
    edit->setAccessibleName(edit->text());
    edit->setObjectName(named ? (curve ? QStringLiteral("pickCurveCanSignal") : QStringLiteral("pickBindingCanSignal"))
                              : (curve ? QStringLiteral("editCurveCanBits") : QStringLiteral("editBindingCanBits")));
}

void PropertyEditor::editPropertyRows(const PropDef &def)
{
    const QPointer<CanvasItem> item = m_item;
    const auto existing = itemValue(def).toArray();
    const bool editingOptions = def.name == QLatin1String("options");
    const bool ranges = def.name == u"colorRanges";
    const bool custom = def.name == u"actions";
    const bool recordColumns = def.name == QLatin1String("recordColumns");
    const bool pointColumns = recordColumns &&
                              item->propertyValue(QStringLiteral("recordSource")).toString() == QLatin1String("points");
    const auto pointFields = pointColumns ? dataPointNames(m_project) + QStringList{QStringLiteral("@time")} : QStringList{};
    const int previousIndex = editingOptions ? item->propertyValue(QStringLiteral("currentIndex")).toInt() : -1;
    auto *dialog = new AppDialog(translated(def.label), window());
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setPreferredSize(QSize(custom ? 820 : 560, 430));
    auto *table = new FluentQt::TableWidget(dialog);
    table->setObjectName(def.name + QStringLiteral("Rows"));
    const ActionTargets targets(custom ? currentWidgets(m_project, m_scene) : QJsonArray{},
                                custom && m_project ? m_project->logic : QJsonObject{});
    if (custom || ranges) {
        auto *hint = new FluentQt::BodyLabel(custom
            ? tr("按行执行。条件可用 value 或 T[\"点名\"]。发送成功后才继续下一行。配置操作后，仅执行表中操作。")
            : tr("下限包含本值，按升序填写。首行下限留空可覆盖更低的数值。"), dialog);
        hint->setWordWrap(true);
        dialog->viewLayout()->addWidget(hint);
    }
    if (custom)
        table->setItemDelegate(new ActionDelegate(item->typeName(), targets, table));
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setColumnCount(def.columns.size());
    QStringList labels;
    for (const auto &column : def.columns)
        labels.append(translated(column.second));
    if (pointColumns)
        labels[0] = tr("工程数据点");
    if (recordColumns) {
        const auto source = item->propertyValue(QStringLiteral("sourceCardId")).toString();
        const auto fields = pointColumns ? pointFields :
            receivedFieldNames(m_project, currentWidgets(m_project, m_scene), source) + QStringList{QStringLiteral("@time")};
        table->setItemDelegateForColumn(0, new RecordFieldDelegate(fields, table));
    }
    table->setHorizontalHeaderLabels(labels);
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    const auto add = [table, columns = def.columns](const QJsonObject &values, int originalIndex) {
        const int row = table->rowCount();
        table->insertRow(row);
        for (int column = 0; column < columns.size(); ++column) {
            auto *cell = new QTableWidgetItem(values.value(columns.at(column).first).toVariant().toString());
            cell->setData(Qt::UserRole, originalIndex);
            table->setItem(row, column, cell);
        }
    };
    for (int row = 0; row < existing.size(); ++row)
        add(existing.at(row).toObject(), row);
    dialog->viewLayout()->addWidget(table, 1);

    auto *actions = new QHBoxLayout;
    auto *append = new FluentQt::PushButton(tr("添加"), dialog);
    auto *remove = new FluentQt::PushButton(tr("删除所选"), dialog);
    auto *up = new FluentQt::PushButton(tr("上移"), dialog);
    auto *down = new FluentQt::PushButton(tr("下移"), dialog);
    for (auto *button : {append, remove, up, down})
        actions->addWidget(button);
    if (ranges) {
        auto *pick = new FluentQt::PushButton(tr("选择颜色"), dialog);
        actions->addWidget(pick);
        const auto swatch = [](QTableWidgetItem *cell) {
            if (cell->column() == 1) {
                const QColor color(cell->text());
                if (cell->data(Qt::DecorationRole).value<QColor>() != color)
                    cell->setData(Qt::DecorationRole, color);
            }
        };
        for (int row = 0; row < table->rowCount(); ++row)
            swatch(table->item(row, 1));
        connect(table, &QTableWidget::itemChanged, dialog, swatch);
        connect(pick, &QPushButton::clicked, dialog, [dialog, table] {
            if (auto *cell = table->item(table->currentRow(), 1)) {
                const QColor color = chooseColor(QColor(cell->text()), tr("选择颜色"), dialog);
                if (color.isValid())
                    cell->setText(color.name());
            }
        });
    }
    dialog->viewLayout()->addLayout(actions);
    auto *problem = newProblemLabel(dialog);
    problem->hide();
    dialog->viewLayout()->addWidget(problem);
    const QJsonObject defaults = custom
        ? QJsonObject{{"event", item->typeName() == u"ToggleSwitch" ? "on" : "click"}, {"type", "notify"}}
        : ranges ? QJsonObject{{"color", "#008040"}} : QJsonObject{};
    connect(append, &QPushButton::clicked, dialog, [table, add, defaults] {
        add(defaults, -1);
        const int row = table->rowCount() - 1;
        table->setCurrentCell(row, 0);
        table->editItem(table->item(row, 0));
    });
    connect(remove, &QPushButton::clicked, dialog, [table] {
        if (table->currentRow() >= 0)
            table->removeRow(table->currentRow());
    });
    const auto move = [table](int offset) {
        const int row = table->currentRow(), target = row + offset;
        if (row < 0 || target < 0 || target >= table->rowCount())
            return;
        for (int column = 0; column < table->columnCount(); ++column) {
            auto *originItem = table->takeItem(row, column);
            table->setItem(row, column, table->takeItem(target, column));
            table->setItem(target, column, originItem);
        }
        table->setCurrentCell(target, 0);
    };
    connect(up, &QPushButton::clicked, dialog, [move] { move(-1); });
    connect(down, &QPushButton::clicked, dialog, [move] { move(1); });
    const auto readRows = [table, columns = def.columns] {
        QJsonArray rows;
        for (int row = 0; row < table->rowCount(); ++row) {
            QJsonObject values;
            for (int column = 0; column < columns.size(); ++column)
                values.insert(columns.at(column).first, table->item(row, column)->text().trimmed());
            rows.append(values);
        }
        return rows;
    };
    dialog->setValidator([table, problem, pointFields, ranges, custom, type = item->typeName(), readRows, targets] {
        if (custom || ranges) {
            QString error;
            if (custom)
                error = widgetActionProblem(readRows(), type, targets);
            else
                parseColorRanges(readRows(), &error);
            problem->setProblem(error);
            problem->setVisible(!error.isEmpty());
            return error.isEmpty();
        }
        for (int row = 0; row < table->rowCount(); ++row) {
            const QString field = table->item(row, 0)->text().trimmed();
            if (field.isEmpty() || (!pointFields.isEmpty() && !pointFields.contains(field))) {
                problem->setProblem(pointFields.isEmpty() ? tr("请填写第 %1 行的名称或来源字段").arg(row + 1)
                                                         : tr("请为第 %1 行选择工程数据点或 @time").arg(row + 1));
                problem->show();
                table->setCurrentCell(row, 0);
                return false;
            }
        }
        return true;
    });
    connect(dialog, &QDialog::accepted, this, [this, item, table, def, previousIndex, editingOptions, readRows] {
        if (!item || item != m_item)
            return;
        int selected = -1;
        for (int row = 0; row < table->rowCount(); ++row) {
            if (previousIndex >= 0 && table->item(row, 0)->data(Qt::UserRole).toInt() == previousIndex)
                selected = row;
        }
        if (editingOptions)
            item->setPropertyValue(QStringLiteral("currentIndex"), std::max(0, selected));
        commitProperty(def.name, readRows());
    });
    dialog->open();
}

void PropertyEditor::addS7Rows(QFormLayout *form, const PropDef &def)
{
    const QJsonObject object = itemValue(def).toObject();
    QList<PropDef> fields{
        {QStringLiteral("address"), tr("S7 地址"), PropType::String, QStringLiteral("DB1.DBD4"), {},
         std::nullopt, std::nullopt, false, {}, QStringLiteral("s7.addressing")},
        {QStringLiteral("dataType"), tr("S7 数据类型"), PropType::Enum, QStringLiteral("REAL"), S7::dataTypes(),
         std::nullopt, std::nullopt, false, {}}};
    if (def.name == QLatin1String("curveS7")) {
        fields.append({QStringLiteral("period"), tr("采集周期(ms)"), PropType::Int, 0, {}, 0, 60000, false, {}});
    } else {
        fields.append({QStringLiteral("value"), tr("写入值"), PropType::String, QStringLiteral("1"), {}, std::nullopt, std::nullopt, false, {}});
        fields.append({QStringLiteral("onValue"), tr("开启写入值"), PropType::String, QStringLiteral("1"), {}, std::nullopt, std::nullopt, false, {}});
        fields.append({QStringLiteral("offValue"), tr("关闭写入值"), PropType::String, QStringLiteral("0"), {}, std::nullopt, std::nullopt, false, {}});
    }
    QLineEdit *addressEdit = nullptr;
    FluentQt::ComboBox *typeCombo = nullptr;
    QWidget *addressRow = nullptr;
    for (const PropDef &field : fields) {
        const auto current = object.value(field.name);
        auto *editor = createEditor(field, current.isUndefined() ? field.defaultValue : current,
                                    [this, root = def.name, name = field.name, defaults = def.defaultValue.toObject()](const QJsonValue &value) {
            auto settings = m_item->properties().value(storageKey(root)).toObject();
            if (settings.isEmpty())
                settings = defaults;
            settings.insert(name, value);
            commitProperty(root, settings, name);
        });
        if (field.name == QLatin1String("period")) {
            if (auto *spin = qobject_cast<QSpinBox *>(editor)) {
                spin->setSpecialValueText(tr("默认周期"));
            }
        }
        if (field.name == QLatin1String("address")) {
            addressEdit = qobject_cast<QLineEdit *>(editor);
            auto *row = new QWidget(m_content);
            auto *box = new QHBoxLayout(row);
            box->setContentsMargins(0, 0, 0, 0);
            box->setSpacing(4);
            box->addWidget(editor, 1);
            auto *build = new FluentQt::TransparentToolButton(FluentQt::icon(FluentQt::FluentIcon::Edit), row);
            build->setObjectName(QStringLiteral("s7AddressBuilderButton_%1").arg(def.name));
            keepSquareBeside(build, editor);
            fluentToolTip(build, tr("可视化构建 S7 地址"));
            box->addWidget(build);
            addressRow = row;
            addRow(form, def.name + QLatin1Char('.') + field.name, field.label, row, field.helpKey);
            continue;
        }
        if (field.name == QLatin1String("dataType")) {
            typeCombo = qobject_cast<FluentQt::ComboBox *>(editor);
        }
        addRow(form, def.name + QLatin1Char('.') + field.name, field.label, editor, field.helpKey);
    }
    if (addressRow && addressEdit && typeCombo) {
        auto *build = addressRow->findChild<QToolButton *>();
        const bool writeOnly = def.name == QLatin1String("s7Command");
        connect(build, &QToolButton::clicked, this, [this, addressEdit, typeCombo, writeOnly]() {
            openS7AddressBuilder(window(), addressEdit->text(), selectedValue(typeCombo), writeOnly,
                                 [addressEdit, typeCombo](const QString &addr, const QString &type) {
                                     addressEdit->setText(addr);
                                     selectValue(typeCombo, type);
                                 });
        });
    }
}

// ---------------------------------------------------------------------------
// Binding
// ---------------------------------------------------------------------------

void PropertyEditor::buildBindingForm(QFormLayout *form)
{
    const QJsonObject binding = m_item->binding();
    const auto value = [&binding](const char *key, const QJsonValue &fallback) {
        const QJsonValue v = binding.value(QLatin1String(key));
        return v.isUndefined() || v.isNull() ? fallback : v;
    };
    // Each editor commits under its own key, so typing in one field is one undo step.
    const auto add = [&](const char *key, const QString &label, QWidget *editor, const QString &helpKey = QString()) {
        m_bindingEditors.insert(QLatin1String(key), editor);
        addRow(form, QStringLiteral("binding.") + QLatin1String(key), label, editor, helpKey);
        const QString field = QLatin1String(key);
        if (auto *box = qobject_cast<FluentQt::ComboBox *>(editor)) {
            connect(box, &FluentQt::ComboBox::currentIndexChanged, this, [this, field]() { commitBinding(field); });
        } else if (auto *box = qobject_cast<FluentQt::EditableComboBox *>(editor)) {
            connect(box, &FluentQt::EditableComboBox::currentTextChanged, this,
                    [this, field]() { commitBinding(field); });
        } else if (auto *edit = qobject_cast<QLineEdit *>(editor)) {
            connect(edit, &QLineEdit::textChanged, this, [this, field]() { commitBinding(field); });
        } else if (auto *number = qobject_cast<QSpinBox *>(editor)) {
            connect(number, qOverload<int>(&QSpinBox::valueChanged), this, [this, field]() { commitBinding(field); });
        }
    };
    const auto combo = [this](const QStringList &values, const QString &current) {
        auto *box = new FluentQt::ComboBox(m_content);
        addValueItems(box, values);
        if (!selectValue(box, current)) {
            box->setCurrentIndex(0);
        }
        return box;
    };
    const auto line = [this](const QString &text, const QString &placeholder) {
        auto *edit = new FluentQt::LineEdit(m_content);
        edit->setText(text);
        edit->setPlaceholderText(placeholder);
        return edit;
    };
    const auto spin = [this](int lo, int hi, int current) {
        auto *box = new FluentQt::SpinBox(m_content);
        box->setRange(lo, hi);
        box->setValue(current);
        return box;
    };

    const QString mode = value("parseMode", binding.isEmpty() ? ParseMode::None : ParseMode::KeyValue).toString();
    QStringList modes{ParseMode::None, ParseMode::KeyValue, ParseMode::DataPoint, ParseMode::JsonPath,
                      ParseMode::HexBytes, ParseMode::Modbus, ParseMode::S7};
    const bool canAvailable = hasCanCard(currentWidgets(m_project, m_scene));
    for (const auto &canMode : {ParseMode::CanField, ParseMode::CanSignal})
        if (canAvailable || mode == canMode)
            modes.append(canMode);
    if (m_bindingOnly)
        modes.removeAll(ParseMode::DataPoint);
    modes.removeIf([this](const QString &candidate) { return !supportsBindingMode(m_item->typeName(), candidate); });
    auto *modePicker = new FluentQt::ComboBox(m_content);
    for (const auto &option : modes)
        modePicker->addItem(option == ParseMode::CanSignal ? tr("CAN 按位取值") : translated(option), {}, option);
    if (!selectValue(modePicker, mode))
        modePicker->setCurrentIndex(0);
    add("parseMode", tr("解析模式"), modePicker, QStringLiteral("data.binding"));
    const auto s7=binding.value(QStringLiteral("s7")).toObject();
    auto *source = new ConnectionSourceEdit(m_content, m_bindingOnly ? ConnectionSourceEdit::Purpose::DataPoint
                                                                    : ConnectionSourceEdit::Purpose::Binding);
    source->setSources(m_project, m_scene, bindingRouteTransport(mode), value("sourceCardId", QString()).toString());
    add("sourceCardId", tr("来源连接"), source, QStringLiteral("data.routing"));
    add("sourceEndpoint", tr("来源子通道"), line(value("sourceEndpoint", QString()).toString(), {}));
    addReceiveEditorRow(form, QStringLiteral("binding.receiveFormat"), ReceiveTarget::Binding);
    auto *s7AddressEdit = line(s7.value(QStringLiteral("address")).toString(QStringLiteral("DB1.DBD4")),
                               QStringLiteral("DB1.DBD4"));
    auto *s7AddressRow = new QWidget(m_content);
    auto *s7AddressLayout = new QHBoxLayout(s7AddressRow);
    s7AddressLayout->setContentsMargins(0, 0, 0, 0);
    s7AddressLayout->setSpacing(4);
    auto *s7BuildBtn = new FluentQt::TransparentToolButton(FluentQt::icon(FluentQt::FluentIcon::Edit), s7AddressRow);
    s7BuildBtn->setObjectName(QStringLiteral("s7AddressBuilderButton"));
    keepSquareBeside(s7BuildBtn, s7AddressEdit);
    fluentToolTip(s7BuildBtn, tr("可视化构建 S7 地址"));
    s7AddressLayout->addWidget(s7AddressEdit, 1);
    s7AddressLayout->addWidget(s7BuildBtn);
    m_bindingEditors.insert(QStringLiteral("s7Address"), s7AddressEdit);
    addRow(form, QStringLiteral("binding.s7Address"), tr("S7 地址"), s7AddressRow, QStringLiteral("s7.addressing"));
    connect(s7AddressEdit, &QLineEdit::textChanged, this, [this]() { commitBinding(QStringLiteral("s7Address")); });

    auto *s7TypeCombo = combo(S7::dataTypes(), s7.value(QStringLiteral("dataType")).toString(QStringLiteral("REAL")));
    add("s7DataType", tr("S7 数据类型"), s7TypeCombo);
    connect(s7BuildBtn, &QToolButton::clicked, this, [this, s7AddressEdit, s7TypeCombo]() {
        openS7AddressBuilder(window(), s7AddressEdit->text(), selectedValue(s7TypeCombo), false,
                             [s7AddressEdit, s7TypeCombo](const QString &addr, const QString &type) {
                                 s7AddressEdit->setText(addr);
                                 selectValue(s7TypeCombo, type);
                             });
    });
    auto *s7Period = spin(0, 60000, s7.value(QStringLiteral("period")).toInt(0));
    s7Period->setSpecialValueText(tr("默认周期"));
    add("s7Period", tr("S7 采集周期(ms)"), s7Period);
    const QString example = industrialCapabilities(m_item->typeName()).inputExample;
    if (!example.isEmpty()) {
        auto *hint = new FluentQt::CaptionLabel(tr("接收值示例 %1").arg(example), m_content);
        hint->setWordWrap(true);
        addRow(form, QStringLiteral("binding.example"), {}, hint);
    }

    auto *key = new BindingKeyEdit(m_content);
    key->setCurrentText(value("key", QString()).toString());
    add("key", tr("绑定字段"), key);
    addCanBindingRow(form, false);
    auto *path = new BindingKeyEdit(m_content);
    path->setCurrentText(value("jsonPath", QString()).toString());
    add("jsonPath", tr("JSON路径"), path);
    if (!m_bindingOnly && m_project) {
        auto *save = new FluentQt::TransparentPushButton(tr("存为工程数据点"), m_content);
        addRow(form, QStringLiteral("binding.savePoint"), {}, save);
        connect(save, &QPushButton::clicked, this, [this] {
            const auto binding = m_item->binding();
            const auto entry = BindingEntry::fromWidget(m_item->toJson());
            const auto fallback = entry && entry->s7 ? entry->s7->address.canonical : m_item->widgetId();
            const auto name = binding.value(QStringLiteral("key")).toString(
                binding.value(QStringLiteral("jsonPath")).toString(fallback));
            addDataPoint(binding, name, m_item);
        });
    }
    add("byteOffset", tr("字节偏移"), spin(0, 9999, value("byteOffset", 0).toInt()));
    add("dataType", tr("数据类型"),
        combo(rawBindingDataTypes(), value("dataType", QStringLiteral("uint16")).toString()));
    add("byteOrder", tr("字节序"),
        combo({QStringLiteral("大端(BE)"), QStringLiteral("小端(LE)")},    // i18n: value
              value("byteOrder", QStringLiteral("大端(BE)")).toString())); // i18n: value
    add("frameHeader", tr("帧头(HEX)"), line(value("frameHeader", QString()).toString(), tr("如 AA 55")));
    add("frameTail", tr("帧尾(HEX)"), line(value("frameTail", QString()).toString(), tr("如 0D 0A")));
    add("mbArea", tr("读取区"),
        combo(ModbusArea::options(), value("mbArea", ModbusArea::HoldingRegisters).toString()));
    add("mbRegIndex", tr("读取地址"), spin(0, 65535, value("mbRegIndex", 0).toInt()), QStringLiteral("modbus.addressing"));
    auto *mbBit = spin(-1, 15, value("mbBit", CommandEncoder::WholeRegister).toInt());
    mbBit->setSpecialValueText(tr("整个寄存器"));
    add("mbBit", tr("位号"), mbBit);
    add("mbDataType", tr("数据类型"),
        combo({QStringLiteral("uint16"), QStringLiteral("int16"), QStringLiteral("uint32"), QStringLiteral("int32"),
               QStringLiteral("float32")},
              value("mbDataType", QStringLiteral("uint16")).toString()));
    auto *mbSpan = new FluentQt::CaptionLabel(m_content);
    mbSpan->setObjectName(QStringLiteral("modbusRegisterSpanHint"));
    mbSpan->setWordWrap(true);
    addRow(form, QStringLiteral("binding.mbSpan"), {}, mbSpan);
    add("mbRegOrder", tr("寄存器序"),
        combo({QStringLiteral("大端(AB CD)"), QStringLiteral("小端(CD AB)")},  // i18n: value
              value("mbRegOrder", QStringLiteral("大端(AB CD)")).toString())); // i18n: value
    auto *function = makeModbusFcCombo(m_content, value("mbFcFilter", -1).toInt());
    function->setObjectName(QStringLiteral("bindingMbFcFilter"));
    add("mbFcFilter", tr("功能码过滤"), function);
    auto *station = spin(-1, 247, value("mbSlaveFilter", -1).toInt());
    station->setSpecialValueText(tr("连接默认从站"));
    add("mbSlaveFilter", tr("从站过滤"), station);
    auto *period = spin(0, 60000, value("mbPeriod", 0).toInt());
    period->setSpecialValueText(tr("默认周期"));
    add("mbPeriod", tr("采集周期(ms)"), period);
    add("problem", QString(), newProblemLabel(m_content));
    add("expression", tr("转换表达式"), line(value("expression", QString()).toString(), tr("如 value * 0.1")),
         QStringLiteral("data.expression"));
    if (!commandActivationProperty(m_item->typeName()).isEmpty() || m_item->typeName() == QLatin1String("ToggleSwitch")) {
        auto *local = new FluentQt::CaptionLabel(tr("本地从站表可读写，启用转换或对外下发时仅回读。"), m_content);
        local->setWordWrap(true);
        addRow(form, QStringLiteral("binding.localWrite"), {}, local);
    }
}

void PropertyEditor::commitBinding(const QString &field)
{
    if (m_loading || !m_item) {
        return;
    }
    if (field == QLatin1String("sourceCardId")) {
        auto *endpoint = static_cast<QLineEdit *>(m_bindingEditors.value(QStringLiteral("sourceEndpoint")));
        const QSignalBlocker blocker(endpoint);
        endpoint->clear();
    }
    const auto textOf = [this](const char *key) {
        QWidget *editor = m_bindingEditors.value(QLatin1String(key));
        if (auto *combo = qobject_cast<FluentQt::ComboBox *>(editor)) {
            return selectedValue(combo);
        }
        if (auto *edit = qobject_cast<QLineEdit *>(editor)) {
            return edit->text().trimmed();
        }
        return QString();
    };
    const auto intOf = [this](const char *key) {
        QWidget *editor = m_bindingEditors.value(QLatin1String(key));
        if (auto *spin = qobject_cast<QSpinBox *>(editor)) {
            return spin->value();
        }
        if (auto *box = qobject_cast<FluentQt::ComboBox *>(editor)) {
            return box->currentData().toInt();
        }
        return 0;
    };
    // Visual CAN settings live in the binding, alongside the fields in this form.
    auto binding = m_item->binding();
    binding.insert(QStringLiteral("parseMode"), textOf("parseMode"));
    for (const char *key : {"key", "jsonPath", "dataType", "byteOrder", "frameHeader", "frameTail",
                            "mbDataType", "mbRegOrder", "mbArea", "sourceCardId"})
        binding.insert(QLatin1String(key), textOf(key));
    for (const char *key : {"byteOffset", "mbRegIndex", "mbBit", "mbFcFilter", "mbSlaveFilter", "mbPeriod"})
        binding.insert(QLatin1String(key), intOf(key));
    const QString endpoint = textOf("sourceEndpoint");
    binding.insert(QStringLiteral("sourceEndpoint"), endpoint.isEmpty() ? QJsonValue(QJsonValue::Undefined) : QJsonValue(endpoint));
    binding.insert(QStringLiteral("s7"), QJsonObject{{QStringLiteral("address"), textOf("s7Address")},
                                                    {QStringLiteral("dataType"), textOf("s7DataType")},
                                                    {QStringLiteral("period"), intOf("s7Period")}});
    const QString expression = textOf("expression");
    binding.insert(QStringLiteral("expression"), expression.isEmpty() ? QJsonValue(QJsonValue::Undefined) : QJsonValue(expression));
    m_item->setBinding(binding);
    updateVisibility();
    emit modified(QStringLiteral("binding:%1:%2").arg(m_item->widgetId(), field));
}

// ---------------------------------------------------------------------------
// Visibility
// ---------------------------------------------------------------------------

void PropertyEditor::updateVisibility()
{
    if (!m_item) {
        if (m_project) {
            updateReceiveTemplateRow(QStringLiteral("project.receiveTemplate"), ReceiveTarget::Defaults);
            setRowVisible(QStringLiteral("parser"), m_project->serial.value(FrameTemplates::ReceiveId).toString().isEmpty());
            setFrameRowsVisible(QString(), m_project->parserType());
        }
        return;
    }
    const WidgetMeta &meta = m_item->meta();
    for (const PropDef &def : meta.properties) {
        if (!def.visibilityProperty.isEmpty()) {
            const bool matches = def.visibilityValues.contains(m_item->propertyValue(def.visibilityProperty));
            setRowVisible(def.name, matches != def.invertVisibility);
        }
    }
    if (meta.typeName == QLatin1String("LineEdit")) {
        const bool local = m_item->propertyValue(QStringLiteral("parameterName")).toString().isEmpty();
        setRowVisible(QStringLiteral("text"), local);
        setRowVisible(QStringLiteral("inputOnly"), local);
    }
    if (meta.typeName == QLatin1String("DataTable")) {
        const bool recording = m_item->propertyValue(QStringLiteral("recordMode")).toString() ==
                               QStringLiteral("逐行记录"); // i18n: value
        const bool points = recording && m_item->propertyValue(QStringLiteral("recordSource")).toString() == QLatin1String("points");
        setRowVisible(QStringLiteral("recordSource"), recording);
        setRowVisible(QStringLiteral("recordIntervalMs"), points);
        setRowVisible(QStringLiteral("recordTriggerPoint"),
                      points && m_item->propertyValue(QStringLiteral("recordIntervalMs")).toInt() == 0);
        setRowVisible(QStringLiteral("sourceCardId"), !points);
        updateRouteEndpoint(QStringLiteral("sourceCardId"), QStringLiteral("sourceEndpoint"), !points);
    }
    if (isIndustrialCategory(meta.category)) {
        const auto caps = industrialCapabilities(meta.typeName, m_item->propertyValue(QStringLiteral("feedbackMode")).toString());
        for (const char *field : {"minimum", "maximum"})
            setRowVisible(QLatin1String(field), caps.scale);
        for (const char *field : {"unit", "decimals"})
            setRowVisible(QLatin1String(field), caps.numericReadout);
        const bool showLimits = m_item->propertyValue(QStringLiteral("showLimits")).toBool();
        for (const char *field : {"lowLimit", "highLimit"})
            setRowVisible(QLatin1String(field), showLimits);
        if (auto *label = static_cast<ProblemLabel *>(m_rows.value(QStringLiteral("limitsProblem")).second)) {
            const bool problem = showLimits && !industrialLimitsValid(
                m_item->propertyValue(QStringLiteral("lowLimit")).toDouble(),
                m_item->propertyValue(QStringLiteral("highLimit")).toDouble());
            label->setProblem(problem ? tr("低限必须小于高限") : QString());
            setRowVisible(QStringLiteral("limitsProblem"), problem);
        }
    }
    if (meta.prop(QStringLiteral("textLayout"))) {
        const bool title = m_item->propertyValue(QStringLiteral("showTitle")).toBool();
        const bool value = m_item->propertyValue(QStringLiteral("showValue")).toBool();
        const bool offset =
            m_item->propertyValue(QStringLiteral("textLayout")).toString() == QStringLiteral("中心偏移"); // i18n: value
        setRowVisible(QStringLiteral("title"), title);
        setRowVisible(QStringLiteral("textLayout"), title || value);
        const bool locked = m_item->propertyValue(QStringLiteral("textPositionLocked")).toBool();
        for (const char *key : {"textLayout", "titleOffsetX", "titleOffsetY", "valueOffsetX", "valueOffsetY"}) {
            if (auto *editor = m_rows.value(QLatin1String(key)).second)
                editor->setEnabled(!locked);
        }
        for (const char *field : {"titleOffsetX", "titleOffsetY"}) {
            setRowVisible(QLatin1String(field), offset && title);
        }
        for (const char *field : {"valueOffsetX", "valueOffsetY"}) {
            setRowVisible(QLatin1String(field), offset && value);
        }
    }
    const QString format = m_item->propertyValue(QStringLiteral("displayFormat")).toString();
    const bool automatic = format == QStringLiteral("自动"); // i18n: value
    if (meta.typeName == u"ComboBox")
        setRowVisible(QStringLiteral("options"), m_item->propertyValue(QStringLiteral("enumTable")).toString().isEmpty());
    if (meta.typeName == u"LEDIndicator") {
        const bool multi = usesMultiStateLed(m_item->properties());
        for (const auto *key : {"color", "on", "onValue", "offValue", "blink"})
            setRowVisible(QLatin1String(key), !multi);
        setRowVisible(QStringLiteral("textList"), multi);
        setRowVisible(QStringLiteral("stateValue"), multi);
    }
    if (meta.typeName == QLatin1String("Label")) {
        const auto binding = m_item->binding();
        const bool bound = !binding.isEmpty() && binding.value(QStringLiteral("parseMode")).toString() != ParseMode::None;
        setRowVisible(QStringLiteral("displayFormat"), bound);
        setRowVisible(QStringLiteral("decimals"), bound && automatic);
    }
    if (meta.typeName == QLatin1String("NumericDisplay")) {
        const bool enumeration =
            m_item->propertyValue(QStringLiteral("displayFormat")).toString() == QStringLiteral("枚举"); // i18n: value
        for (const char *key : {"textList", "unknownText", "unknownColor"})
            setRowVisible(QLatin1String(key), enumeration);
        setRowVisible(QStringLiteral("decimals"), automatic);
        setRowVisible(QStringLiteral("unit"), !enumeration);
        setRowVisible(QStringLiteral("fontSize"),
                      m_item->propertyValue(QStringLiteral("displayStyle")).toString() != QStringLiteral("状态徽标")); // i18n: value
    }
    if (meta.typeName == QLatin1String("SpinBox")) {
        setRowVisible(QStringLiteral("decimals"),
                      m_item->propertyValue(QStringLiteral("numberMode")).toString() == QStringLiteral("小数")); // i18n: value
    }
    if (meta.typeName == QLatin1String("TractionBattery")) {
        for (const char *key :
             {"chargeLow", "chargeWarning", "chargeLowColor", "chargeWarningColor", "chargeNormalColor"})
            setRowVisible(QLatin1String(key), m_item->propertyValue(QStringLiteral("chargeColors")).toBool());
    }
    if (auto *help = m_helpButtons.value(QStringLiteral("commandTemplate"))) {
        help->setVisible(m_item->propertyValue(QStringLiteral("commandMode")).toString() == QLatin1String("CAN"));
    }
    if (meta.typeName == QLatin1String("ToggleSwitch")) {
        const bool exact = m_item->propertyValue(QStringLiteral("onCondition")).toString() ==
                           QStringLiteral("等于指定值"); // i18n: value
        setRowVisible(QStringLiteral("onValue"), exact);
        setRowVisible(QStringLiteral("offFeedbackValue"), exact);
    }
    if (meta.prop(QStringLiteral("parserConfig"))) {
        const QString parser = m_item->propertyValue(QStringLiteral("defaultParser")).toString();
        const bool manual = FrameTemplates::receiveIds(m_item->properties()).isEmpty();
        setRowVisible(QStringLiteral("defaultParser"), manual);
        updateReceiveTemplateRow(QStringLiteral("connection.receiveTemplate"), ReceiveTarget::Connection);
        setFrameRowsVisible(QStringLiteral("parserConfig."), parser);
        const QString effective = frameFormat(m_project, m_item->properties()).type;
        const bool modbus = isModbusParser(effective);
        const bool supportsSlave = LinkParser::supportsModbusSlave(effective);
        const bool slave = supportsSlave && m_item->propertyValue(QStringLiteral("modbusRole")).toString() == ModbusRole::Slave;
        setRowVisible(QStringLiteral("modbusRole"), supportsSlave);
        setRowVisible(QStringLiteral("modbusSlave"), modbus);
        for (const char *field : {"modbusPeriod", "modbusTimeout"}) {
            setRowVisible(QLatin1String(field), modbus && !slave);
        }
        for (const char *field :
             {"modbusDelay", "modbusExceptions", "slaveCoStart", "slaveCoCount", "slaveDiStart", "slaveDiCount",
              "slaveHrStart", "slaveHrCount", "slaveIrStart", "slaveIrCount", "slaveInit"}) {
            setRowVisible(QLatin1String(field), modbus && slave);
        }
    }
    if (meta.prop(QStringLiteral("commandMode"))) {
        const QString mode = m_item->propertyValue(QStringLiteral("commandMode")).toString();
        const QString target = m_item->propertyValue(QStringLiteral("commandTargetId")).toString();
        const auto props = m_item->properties();
        const auto pointKey = writePointKey(mode);
        const auto writePoint = pointKey.isEmpty() ? QString() : props.value(pointKey).toString();
        if (auto *box = static_cast<ConnectionSourceEdit *>(m_rows.value(QStringLiteral("commandTargetId")).second))
            box->setSources(m_project, m_scene, CommandEncoder::sendRouteFilter(props, meta.typeName), target);
        const bool sends = widgetSendsCommands(meta.typeName, props);
        setRowVisible(QStringLiteral("canWriteSignal"), sends && mode == u"CAN");
        const bool linked = sends && !writePoint.isEmpty();
        const auto widgets = m_project && (linked || (sends && !pointKey.isEmpty() && m_rows.contains(QStringLiteral("writeDevicePoint"))))
            ? currentWidgets(m_project, m_scene) : QJsonArray{};
        if (auto *picker = qobject_cast<FluentQt::ComboBox *>(m_rows.value(QStringLiteral("writeDevicePoint")).second)) {
            if (sends && !pointKey.isEmpty())
                fillWritePointChoices(picker, *m_project, widgets, props, meta.typeName);
            setRowVisible(QStringLiteral("writeDevicePoint"), sends && !pointKey.isEmpty() && picker->count() > 1);
        }
        auto effective = props;
        if (auto *summary = qobject_cast<QLabel *>(m_rows.value(QStringLiteral("effectiveWriteTarget")).second)) {
            if (linked) {
                QString error;
                const auto resolved = resolvePointWrite(props, m_project->logic, widgets, m_project->parserType(), &error,
                                                        meta.typeName, ModbusParserChoice::OnConnect);
                if (resolved) {
                    effective = *resolved;
                    summary->setText(writePointTargetText(*m_project, widgets, effective, writePoint));
                } else {
                    summary->setText(error);
                }
            }
            setRowVisible(QStringLiteral("effectiveWriteTarget"), linked);
        }
        setRowVisible(CommandEncoder::SendInterval, sends && CommandEncoder::supportsPeriodicSend(mode));
        bool editsTransmitFrame = sends && mode == QLatin1String("HEX");
        bool offersSendTemplate = sends && !FrameTemplates::sendParser(mode).isEmpty();
        const QSet<QString> shown = sends ? modeVisibility().value(mode) : QSet<QString>{};
        for (const QString &field : modeFields()) {
            setRowVisible(field, shown.contains(field));
        }
        using CommandEncoder::InputListMode;
        const auto listMode = sends && meta.prop(QStringLiteral("commandInputs")) ? CommandEncoder::inputListMode(props)
                                                                                 : InputListMode::None;
        const auto inputs = listMode == InputListMode::None ? QList<CommandEncoder::CommandInput>{}
                                                           : CommandEncoder::commandInputs(props);
        const bool batch = !inputs.isEmpty() || CommandEncoder::usesFrameFields(props);
        if (auto *interval = qobject_cast<QSpinBox *>(m_rows.value(CommandEncoder::SendInterval).second)) {
            const bool usesCanSettings = mode == QLatin1String("CAN") && meta.typeName != QLatin1String("DataSendCard") &&
                                         !batch;
            interval->setSpecialValueText(usesCanSettings ? tr("沿用 CAN 报文设置") : tr("单次发送"));
        }
        const bool unified = listMode != InputListMode::None;
        const bool sequence = unified && CommandEncoder::usesFrameSequence(props);
        offersSendTemplate = offersSendTemplate && !sequence;
        setRowVisible(QStringLiteral("commandInputs"), unified);
        if (batch)
            setRowVisible(QStringLiteral("command"), false);
        for(const QString &field:{QStringLiteral("address"),QStringLiteral("dataType"),QStringLiteral("value"),QStringLiteral("onValue"),QStringLiteral("offValue")}) {
            const bool toggle=meta.typeName==QLatin1String("ToggleSwitch");
            const bool shown=sends && mode==QLatin1String("S7") && (field==QLatin1String("address") || field==QLatin1String("dataType")
                || (toggle ? field==QLatin1String("onValue")||field==QLatin1String("offValue") : field==QLatin1String("value")&&meta.typeName==QLatin1String("PushButton")));
            setRowVisible(QStringLiteral("s7Command.")+field,shown);
        }
        const bool byteOrder = batch ? std::any_of(inputs.cbegin(), inputs.cend(), [](const auto &input) {
            return usesByteOrder(input.dataType);
        }) : usesByteOrder(m_item->propertyValue(QStringLiteral("dataType")).toString());
        if (sends && mode == QLatin1String("HEX")) {
            setRowVisible(QStringLiteral("dataType"), !batch);
            setRowVisible(QStringLiteral("byteOrder"), byteOrder);
        }
        if (sends && mode == QLatin1String("CAN") && meta.prop(QStringLiteral("canId"))) {
            // The link ignores these for a message with signals; show them only where they still matter.
            const auto names = canNames(m_project, currentWidgets(m_project, m_scene),
                                        target == DataRoute::Broadcast ? QString() : target);
            if (auto *edit = qobject_cast<FluentQt::LineEdit *>(m_rows.value(QStringLiteral("canId")).second))
                complete(edit, names.messages);
            const CanTargetKind kind = canTargetKind(names,
                                                     m_item->propertyValue(QStringLiteral("canId")).toString());
            editsTransmitFrame = kind.usesRawEncoding;
            offersSendTemplate = kind.usesRawEncoding;
            setRowVisible(QStringLiteral("dataType"), kind.usesRawEncoding && !batch);
            setRowVisible(QStringLiteral("byteOrder"), kind.usesRawEncoding && byteOrder);
            setRowVisible(QStringLiteral("canFd"), kind.usesFdSetting);
            setRowVisible(QStringLiteral("canCycle"), !batch && CommandEncoder::periodicSendInterval(props) == 0);
        }
        if (sends && mode == QLatin1String("Modbus") && meta.prop(QStringLiteral("mbFunc"))) {
            const int function = CommandEncoder::functionCode(m_item->propertyValue(QStringLiteral("mbFunc")).toString());
            const QString dataType = m_item->propertyValue(QStringLiteral("mbDataType")).toString();
            const auto spec = CommandEncoder::modbusFunctionSpec(function);
            const bool registers = spec.argument == CommandEncoder::ModbusArgument::Register;
            const bool isFc06 = function == 0x06;
            const bool writesBit = isFc06 && CommandEncoder::modbusBit(effective) != CommandEncoder::WholeRegister;
            if (auto *box = qobject_cast<FluentQt::ComboBox *>(m_rows.value(QStringLiteral("mbDataType")).second))
                replaceChoices(box, spec.dataTypes, dataType);
            const bool writeList = listMode == InputListMode::Registers && batch;
            setRowVisible(QStringLiteral("mbBit"), isFc06);
            setRowVisible(QStringLiteral("mbBitAction"), writesBit);
            setRowVisible(QStringLiteral("mbDataType"), registers && !writeList && !writesBit);
            const bool usesOrder = writeList ? std::any_of(inputs.cbegin(), inputs.cend(), [](const auto &input) {
                return CommandEncoder::usesRegisterOrder(input.dataType);
            }) : registers && spec.dataTypes.contains(dataType) && CommandEncoder::usesRegisterOrder(dataType);
            setRowVisible(QStringLiteral("mbRegOrder"), usesOrder && !writesBit);
            setRowVisible(QStringLiteral("mbValue"), !batch && !writesBit && spec.argument != CommandEncoder::ModbusArgument::Unsupported);
        }
        if (sends && mode == QLatin1String("Modbus") && meta.typeName == QLatin1String("ToggleSwitch")) {
            setRowVisible(QStringLiteral("mbBit"),
                          m_item->propertyValue(QStringLiteral("mbArea")).toString() == ModbusArea::HoldingRegisterBit);
        }
        if (unified) {
            for (const char *key : {"command", "commandTemplate", "frameHeader", "frameTail", "dataType", "byteOrder",
                                    "commandNewline", "commandJsonNewline", "s7Command.value", "mbValue"})
                setRowVisible(QLatin1String(key), false);
            if (listMode == InputListMode::Registers)
                setRowVisible(QStringLiteral("mbRegOrder"), false);
            if (listMode == InputListMode::S7Value) {
                setRowVisible(QStringLiteral("s7Command.address"), false);
                setRowVisible(QStringLiteral("s7Command.dataType"), false);
            }
            if (auto *edit = qobject_cast<QPushButton *>(m_rows.value(QStringLiteral("commandInputs")).second)) {
                edit->setText(!writePoint.isEmpty() ? tr("编辑发送配置 · %1").arg(writePoint)
                    : sequence ? tr("编辑混发配置（%1 帧）").arg(props.value(FrameTemplates::SendSequence).toArray().size())
                    : tr("编辑发送配置"));
            }
        }
        if (auto *row = m_rows.value(QStringLiteral("sendTemplate")).second; row && offersSendTemplate) {
            auto *picker = row->findChild<FluentQt::ComboBox *>();
            fillFrameTemplates(picker, m_project->frameTemplates, props.value(FrameTemplates::SendId).toString(),
                               tr("独立配置"), FrameTemplates::sendParser(mode));
            picker->setEnabled(picker->count() > 1);
        }
        setRowVisible(QStringLiteral("sendTemplate"), offersSendTemplate);
        const bool sharedTemplate = offersSendTemplate && m_project &&
                                    FrameTemplates::sendSettings(m_project->frameTemplates, props).has_value();
        if (auto *field = qobject_cast<FluentQt::ComboBox *>(m_rows.value(FrameTemplates::SendValueField).second)) {
            const auto name = props.value(FrameTemplates::SendValueField).toString();
            const bool supportsMapping = offersSendTemplate && FrameTemplates::isBinarySend(mode);
            const auto fields = supportsMapping ? FrameTemplates::sendFields(m_project->frameTemplates, props) : QList<HexField>{};
            const bool show = supportsMapping && (!fields.isEmpty() || !name.isEmpty());
            setRowVisible(FrameTemplates::SendValueField, show);
            if (show) {
                const QSignalBlocker blocker(field);
                field->clear();
                field->addItem(tr("整段 HEX"), {}, QString());
                const auto settings = FrameTemplates::sendSettings(m_project->frameTemplates, props);
                const auto required = settings ? FrameTemplates::requiredFields(*settings) : QStringList{};
                for (const auto &entry : fields) {
                    const auto label = required.contains(entry.name) ? tr("%1（必填）").arg(entry.name) : entry.name;
                    field->addItem(QStringLiteral("%1 · %2").arg(label, translated(entry.type)), {}, entry.name);
                }
                if (!name.isEmpty() && field->findData(name) < 0) {
                    field->addItem(tr("%1（字段不可用）").arg(name), {}, name);
                    field->setItemEnabled(field->count() - 1, false);
                }
                selectValue(field, name);
                if (!name.isEmpty())
                    for (const char *key : {"commandTemplate", "dataType", "byteOrder"})
                        setRowVisible(QLatin1String(key), false);
            }
        }
        if (sharedTemplate) {
            for (const char *key : {"frameHeader", "frameTail", "commandNewline", "commandJsonNewline"})
                setRowVisible(QLatin1String(key), false);
        }
        if (linked)
            for (const char *key : {"mbSlave", "mbAddress", "mbArea", "mbBit", "mbDataType", "mbRegOrder",
                                    "s7Command.address", "s7Command.dataType"})
                setRowVisible(QLatin1String(key), false);
        setRowVisible(QStringLiteral("commandTargetId"), sends && !linked);
        setRowVisible(QStringLiteral("confirmBeforeSend"), sends);
        if (!sends)
            setRowVisible(QStringLiteral("showSendConfirmationDetails"), false);
        setRowVisible(QStringLiteral("sendPreview"), sends);
        setRowVisible(QStringLiteral("transmitFormat"), editsTransmitFrame && !sharedTemplate);
        if (auto *write = qobject_cast<QLabel *>(m_rows.value(QStringLiteral("modbusSwitchWrite")).second)) {
            const bool modbus = mode == QLatin1String("Modbus");
            if (modbus)
                write->setText(modbusSwitchWriteSummary(effective));
            setRowVisible(QStringLiteral("modbusSwitchWrite"), modbus);
        }
        updateRouteEndpoint(QStringLiteral("commandTargetId"), QStringLiteral("commandEndpoint"), sends && !linked);
        setRowVisible(QStringLiteral("commandMode"), sends);
        setRowVisible(QStringLiteral("commands"), sends);
    } else {
        updateRouteEndpoint(QStringLiteral("commandTargetId"), QStringLiteral("commandEndpoint"));
    }
    if (meta.typeName != QLatin1String("DataTable"))
        updateRouteEndpoint(QStringLiteral("sourceCardId"), QStringLiteral("sourceEndpoint"));
    if (meta.typeName == QLatin1String("CanMonitor")) {
        setRowVisible(QStringLiteral("autoScroll"),
                      m_item->propertyValue(QStringLiteral("displayMode")).toString() == QStringLiteral("滚动日志")); // i18n: value
        setRowVisible(QStringLiteral("autoSaveRows"), m_item->propertyValue(QStringLiteral("autoSave")).toBool());
        setRowVisible(QStringLiteral("autoSaveDirectory"), m_item->propertyValue(QStringLiteral("autoSave")).toBool());
    }
    if (meta.typeName == QLatin1String("CanConfigCard")) {
        setRowVisible(QStringLiteral("canTxInitialMaxAge"),
                      !m_item->propertyValue(QStringLiteral("canTxInitialState")).toString().trimmed().isEmpty());
        // The ZLG fields for a ZLG adapter, the data phase for CAN FD.
        const QString adapter = m_item->propertyValue(QStringLiteral("defaultAdapter")).toString();
        const bool zlg = adapter == CanAdapter::Zlg;
        for (const char *field : {"zlgModel", "defaultDeviceIndex", "defaultChannel"}) {
            setRowVisible(QLatin1String(field), zlg);
        }
        const auto caps = CanAdapter::caps(adapter, m_item->propertyValue(QStringLiteral("zlgModel")).toString());
        if (auto *spin = qobject_cast<QSpinBox *>(m_rows.value(QStringLiteral("defaultChannel")).second)) {
            const QSignalBlocker blocker(spin);
            spin->setMaximum(caps.channelCount > 0 ? caps.channelCount - 1 : 7);
            spin->setValue(m_item->propertyValue(QStringLiteral("defaultChannel")).toInt());
        }
        const auto presets = [this, &meta](const QString &field, const QStringList &supported) {
            if (auto *box = qobject_cast<FluentQt::ComboBox *>(m_rows.value(field).second))
                replaceChoices(box, supported.isEmpty() ? meta.prop(field)->options : supported,
                               m_item->propertyValue(field).toString());
        };
        presets(QStringLiteral("defaultBitrate"), caps.nominalBitrates);
        presets(QStringLiteral("defaultDataBitrate"), caps.dataBitrates);
        const QString custom = m_item->propertyValue(zlg ? QStringLiteral("zlgCustomTiming")
                                                        : QStringLiteral("customTiming")).toString();
        const auto timing = CanAdapter::timingOverrides(adapter, caps.customTiming ? custom : QString());
        const bool fd = caps.fdEnabled(m_item->propertyValue(QStringLiteral("canFd")).toBool());
        const bool systemTiming = adapter == CanAdapter::SocketCan; // presets fill the ip link command
        setRowVisible(QStringLiteral("defaultBitrate"), (caps.bitrate || systemTiming) && !timing.nominal);
        setRowVisible(QStringLiteral("defaultSamplePoint"), !zlg && (caps.samplePoint || systemTiming) && !timing.nominal);
        setRowVisible(QStringLiteral("zlgSamplePoint"), zlg && caps.samplePoint && !timing.nominal);
        setRowVisible(QStringLiteral("customTiming"), !zlg && caps.customTiming);
        setRowVisible(QStringLiteral("zlgCustomTiming"), zlg && caps.customTiming);
        setRowVisible(QStringLiteral("canFd"), caps.fd);
        setRowVisible(QStringLiteral("defaultDataBitrate"), fd && (caps.bitrate || systemTiming) && !timing.data);
        setRowVisible(QStringLiteral("defaultDataSamplePoint"), fd && (caps.dataSamplePoint || systemTiming) && !timing.data);
        setRowVisible(QStringLiteral("canBrs"), fd);
        setRowVisible(QStringLiteral("listenOnly"), caps.listenOnly);
        setRowVisible(QStringLiteral("autoRecover"), caps.autoRecover);
        setRowVisible(QStringLiteral("termination"), caps.termination);
    }
    if (meta.prop(QStringLiteral("notify"))) {
        const bool notify = m_item->propertyValue(QStringLiteral("notify")).toBool();
        for (const char *field : {"notifyTitle", "notifyContent", "notifyType", "notifyDuration"}) {
            setRowVisible(QLatin1String(field), notify);
        }
    }
    if (usesChartSeries(meta.typeName, m_item->properties())) {
        const QString mode = itemValue(*meta.prop(QStringLiteral("curveParseMode"))).toString();
        auto *key = static_cast<BindingKeyEdit *>(m_rows.value(QStringLiteral("curveKey")).second);
        const QString source = m_item->properties().value(storageKey(QStringLiteral("curveSourceCardId"))).toString();
        const auto widgets = currentWidgets(m_project, m_scene);
        setRowVisible(QStringLiteral("curve.canBatch"), !routeConnections(widgets, RouteTransport::Can).isEmpty());
        if (auto *box = static_cast<ConnectionSourceEdit *>(m_rows.value(QStringLiteral("curveSourceCardId")).second))
            box->setSources(m_project, m_scene, bindingRouteTransport(mode), source);
        if (mode == ParseMode::DataPoint)
            key->setNames(dataPointNames(m_project), tr("选择或搜索工程数据点"));
        else if (mode == ParseMode::KeyValue)
            key->setNames(receivedFieldNames(m_project, widgets, source), tr("字段名 (如 temperature)"));
        else if (mode == ParseMode::JsonPath)
            if (auto *path = static_cast<BindingKeyEdit *>(m_rows.value(QStringLiteral("curveJsonPath")).second))
                path->setNames(receivedFieldNames(m_project, widgets, source, true), tr("路径 (如 data.sensors.0.temp)"));
        const bool usesSource = mode != ParseMode::DataPoint && mode != ParseMode::None;
        const bool visualValues = updateReceiveTemplateRow(
            QStringLiteral("curve.receiveFormat.template"), ReceiveTarget::Curve, usesFreeFrame(mode));
        setRowVisible(QStringLiteral("curve.receiveFormat"), visualValues);
        updateRouteEndpoint(QStringLiteral("curveSourceCardId"), QStringLiteral("curveSourceEndpoint"), usesSource);
        setRowVisible(QStringLiteral("curveSourceCardId"), usesSource);
        const auto shown = visualValues && mode == ParseMode::HexBytes ? QSet<QString>{} : chartParseVisibility().value(mode);
        for (const QSet<QString> &fields : chartParseVisibility()) {
            for (const QString &field : fields) {
                setRowVisible(field, shown.contains(field));
            }
        }
        const auto binding = chartCurveBinding(m_item->properties(), editorCurve(*m_item));
        updateCanBindingRow(binding, true);
        auto *modePicker = qobject_cast<FluentQt::ComboBox *>(m_rows.value(QStringLiteral("curveParseMode")).second);
        if (updateTemplateFieldPicker(QStringLiteral("curve.receiveFormat.field"), ReceiveTarget::Curve, modePicker, mode,
                                      m_item->properties().value(storageKey(QStringLiteral("curveKey"))).toString()))
            setRowVisible(QStringLiteral("curveKey"), false);
        if (mode == ParseMode::HexBytes)
            setRowVisible(QStringLiteral("curveByteOrder"),
                          !visualValues && usesByteOrder(m_item->properties().value(storageKey(QStringLiteral("curveDataType")))
                                            .toString(QStringLiteral("float32"))));
        setRowVisible(QStringLiteral("curveExpression"), mode != ParseMode::None && !visualValues);
        if (mode == ParseMode::Modbus)
            setModbusReadFields(binding, QStringLiteral("curveMbBit"), QStringLiteral("curveMbDataType"),
                                QStringLiteral("curveMbRegOrder"));
        for(const QString &field:{QStringLiteral("address"),QStringLiteral("dataType"),QStringLiteral("period")})
            setRowVisible(QStringLiteral("curveS7.")+field,mode==ParseMode::S7);
        // Only once the curve has an ID to judge: an empty one is a field still to fill, not a mistake.
        QString problem;
        if (mode == ParseMode::CanSignal && !binding.value(QStringLiteral("canId")).toString().isEmpty())
            problem = canSignalProblem(binding);
        if (const auto row = m_rows.constFind(QStringLiteral("curveCanProblem")); row != m_rows.constEnd()) {
            if (auto *label = dynamic_cast<ProblemLabel *>(row->second)) {
                label->setProblem(problem);
            }
        }
        setRowVisible(QStringLiteral("curveCanProblem"), !problem.isEmpty());
    }
    if (m_rows.contains(QStringLiteral("observer.receiveFormat"))) {
        const bool recordsPoints = meta.typeName == QLatin1String("DataTable") &&
            m_item->propertyValue(QStringLiteral("recordMode")).toString() == QStringLiteral("逐行记录") && // i18n: value
            m_item->propertyValue(QStringLiteral("recordSource")).toString() == QLatin1String("points");
        setRowVisible(QStringLiteral("observer.receiveFormat"), updateReceiveTemplateRow(
            QStringLiteral("observer.receiveFormat.template"), ReceiveTarget::Observer, !recordsPoints));
    }
    if (!m_bindingEditors.isEmpty()) {
        auto *combo = qobject_cast<FluentQt::ComboBox *>(m_bindingEditors.value(QStringLiteral("parseMode")));
        const QString mode = combo ? selectedValue(combo) : ParseMode::KeyValue;
        const auto binding = m_item->binding();
        auto *source = static_cast<ConnectionSourceEdit *>(m_bindingEditors.value(QStringLiteral("sourceCardId")));
        const QString sourceId = selectedValue(source);
        source->setSources(m_project, m_scene, bindingRouteTransport(mode), sourceId);
        auto *key = static_cast<BindingKeyEdit *>(m_bindingEditors.value(QStringLiteral("key")));
        if (mode == ParseMode::JsonPath) {
            auto *path = static_cast<BindingKeyEdit *>(m_bindingEditors.value(QStringLiteral("jsonPath")));
            path->setNames(receivedFieldNames(m_project, currentWidgets(m_project, m_scene), sourceId, true),
                           tr("路径 (如 data.sensors.0.temp)"));
        }
        key->setNames(mode == ParseMode::DataPoint ? dataPointNames(m_project)
                                                 : receivedFieldNames(m_project, currentWidgets(m_project, m_scene), sourceId),
                      mode == ParseMode::DataPoint ? tr("选择或搜索工程数据点") : tr("字段名 (如 temperature)"));
        const bool usesSource = mode != ParseMode::None && mode != ParseMode::DataPoint;
        updateRouteEndpoint(QStringLiteral("binding.sourceCardId"), QStringLiteral("binding.sourceEndpoint"), usesSource);
        for(const char *field:{"s7Address","s7DataType","s7Period"})
            setRowVisible(QStringLiteral("binding.")+QLatin1String(field),mode==ParseMode::S7);
        const bool usesTemplateField = updateTemplateFieldPicker(
            QStringLiteral("binding.receiveFormat.field"), ReceiveTarget::Binding, combo, mode,
            binding.value(QStringLiteral("key")).toString());
        setRowVisible(QStringLiteral("binding.key"), !usesTemplateField &&
                      (mode == ParseMode::KeyValue || mode == ParseMode::DataPoint));
        updateCanBindingRow(binding, false);
        setRowVisible(QStringLiteral("binding.sourceCardId"), usesSource);
        setRowVisible(QStringLiteral("binding.jsonPath"), mode == ParseMode::JsonPath);
        const bool visualValues = updateReceiveTemplateRow(
            QStringLiteral("binding.receiveFormat.template"), ReceiveTarget::Binding, usesFreeFrame(mode));
        setRowVisible(QStringLiteral("binding.receiveFormat"), visualValues);
        if (visualValues) {
            if (auto *editBtn = qobject_cast<QPushButton *>(m_rows.value(QStringLiteral("binding.receiveFormat")).second)) {
                QStringList parts;
                if (mode == ParseMode::HexBytes) {
                    parts.append(QStringLiteral("B%1").arg(binding.value(QStringLiteral("byteOffset")).toInt(0)));
                }
                const QString expr = binding.value(QStringLiteral("expression")).toString().trimmed();
                if (!expr.isEmpty()) {
                    parts.append(expr.size() > 14 ? expr.left(13) + QChar(0x2026) : expr);
                }
                editBtn->setText(parts.isEmpty() ? tr("可视化取值…")
                                                 : tr("可视化取值 · %1").arg(parts.join(QStringLiteral(" · "))));
            }
        }
        for (const char *field : {"byteOffset", "dataType", "byteOrder", "frameHeader", "frameTail"}) {
            setRowVisible(QStringLiteral("binding.") + QLatin1String(field), mode == ParseMode::HexBytes && !visualValues);
        }
        if (mode == ParseMode::HexBytes && !visualValues) {
            const auto *type = qobject_cast<FluentQt::ComboBox *>(m_bindingEditors.value(QStringLiteral("dataType")));
            setRowVisible(QStringLiteral("binding.byteOrder"), usesByteOrder(selectedValue(type)));
        }
        for (const char *field :
             {"mbArea", "mbRegIndex", "mbBit", "mbDataType", "mbRegOrder", "mbFcFilter", "mbSlaveFilter", "mbPeriod"}) {
            setRowVisible(QStringLiteral("binding.") + QLatin1String(field), mode == ParseMode::Modbus);
        }
        bool showMbSpan = false;
        if (mode == ParseMode::Modbus) {
            setModbusReadFields(m_item->binding(), QStringLiteral("binding.mbBit"), QStringLiteral("binding.mbDataType"),
                                QStringLiteral("binding.mbRegOrder"));
            const QString table = ModbusArea::prefix(binding.value(QStringLiteral("mbArea")).toString(ModbusArea::HoldingRegisters));
            const bool registers = table == QLatin1String("hr") || table == QLatin1String("ir");
            const int bit = binding.value(QStringLiteral("mbBit")).toInt(CommandEncoder::WholeRegister);
            const QString dataType = binding.value(QStringLiteral("mbDataType")).toString(QStringLiteral("uint16"));
            showMbSpan = registers && (bit < 0 || bit > 15) && CommandEncoder::usesRegisterOrder(dataType);
            if (showMbSpan) {
                const int reg = binding.value(QStringLiteral("mbRegIndex")).toInt(0);
                if (auto *span = qobject_cast<QLabel *>(m_rows.value(QStringLiteral("binding.mbSpan")).second)) {
                    span->setText(tr("占用 2 个寄存器：%1–%2").arg(reg).arg(reg + 1));
                }
            }
        }
        setRowVisible(QStringLiteral("binding.mbSpan"), showMbSpan);
        setRowVisible(QStringLiteral("binding.savePoint"), usesSource && BindingEntry::fromWidget(m_item->toJson()).has_value());
        setRowVisible(QStringLiteral("binding.example"), mode != ParseMode::None);
        if (m_rows.contains(QStringLiteral("binding.localWrite"))) {
            const auto links = routeConnections(currentWidgets(m_project, m_scene), RouteTransport::ModbusSlave, false,
                                                m_project ? m_project->parserType() : QStringLiteral("line"));
            const auto isLocal = [&sourceId](const auto &entry) { return entry.first == sourceId; };
            setRowVisible(QStringLiteral("binding.localWrite"), mode == ParseMode::Modbus &&
                          (sourceId.isEmpty() ? links.size() == 1 : std::any_of(links.cbegin(), links.cend(), isLocal)));
        }
        QString problem;
        if (mode == ParseMode::CanSignal)
            problem = canSignalProblem(binding);
        else if (mode == ParseMode::S7) {
            QString s7Error;
            if (!S7::point(binding.value(QStringLiteral("s7")).toObject(), &s7Error))
                problem = s7Error;
        } else if ((mode == ParseMode::KeyValue || mode == ParseMode::DataPoint || mode == ParseMode::CanField) &&
                 binding.value(QStringLiteral("key")).toString().isEmpty())
            problem = usesTemplateField ? tr("选择模板字段")
                : mode == ParseMode::CanField ? tr("选择 CAN 信号")
                : mode == ParseMode::DataPoint ? tr("请选择工程数据点") : tr("请填写绑定字段");
        else if (mode == ParseMode::JsonPath && binding.value(QStringLiteral("jsonPath")).toString().isEmpty())
            problem = tr("请填写 JSON 路径");
        else if (m_bindingOnly && usesSource && m_project)
            problem = dataPointSourceProblem(binding, m_project->widgets, m_project->parserType());
        setRowVisible(QStringLiteral("binding.expression"), mode != ParseMode::None && !visualValues &&
                      !industrialCapabilities(meta.typeName).structuredInput);
        if (auto *label = static_cast<ProblemLabel *>(m_bindingEditors.value(QStringLiteral("problem")))) {
            label->setProblem(problem);
        }
        setRowVisible(QStringLiteral("binding.problem"), !problem.isEmpty());
    }
}

} // namespace fhmi
