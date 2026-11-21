#include "designer/frame_template_editor.h"

#include "core/binding.h"
#include "core/frame_templates.h"
#include "core/hex_utils.h"
#include "core/i18n.h"
#include "core/project.h"
#include "core/project_logic.h"
#include "core/protocol_parser.h"
#include "designer/canvas_item.h"
#include "designer/command_inputs_editor.h"
#include "designer/connection_source_edit.h"
#include "designer/project_frame_templates.h"
#include "designer/transmit_frame_editor.h"
#include "runtime/frame_bytes_view.h"
#include "runtime/app_dialog.h"
#include "runtime/ui_helpers.h"

#include <FluentQtWidgets/FluentIcon.h>
#include <FluentQtWidgets/Navigation/SegmentedWidget.h>
#include <FluentQtWidgets/Views/ItemViews.h>
#include <FluentQtWidgets/Widgets/Button.h>
#include <FluentQtWidgets/Widgets/ComboBox.h>
#include <FluentQtWidgets/Widgets/Label.h>
#include <FluentQtWidgets/Widgets/LineEdit.h>
#include <FluentQtWidgets/Widgets/Menu.h>
#include <FluentQtWidgets/Widgets/ScrollArea.h>
#include <FluentQtWidgets/Widgets/SpinBox.h>

#include <QtCore/QCoreApplication>
#include <QtCore/QRegularExpression>
#include <QtCore/QSet>
#include <QtCore/QSignalBlocker>
#include <QtGui/QAction>
#include <QtGui/QClipboard>
#include <QtGui/QFontDatabase>
#include <QtWidgets/QApplication>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QStackedWidget>
#include <QtWidgets/QSplitter>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <numeric>

namespace fhmi {

namespace {

using CommandEncoder::InputListMode;
using InputValues = QMap<QString, CommandEncoder::InputValue>;

const QString RawPayload = QStringLiteral("raw");
const QString FieldPayload = QStringLiteral("fields");

QString tr(const char *source) { return QCoreApplication::translate("fhmi", source); }

QString dialogTitle(InputListMode mode)
{
    if (mode == InputListMode::Text)
        return tr("文本模板");
    if (mode == InputListMode::Json)
        return tr("JSON 模板");
    if (mode == InputListMode::Hex)
        return tr("HEX 帧格式");
    if (mode == InputListMode::Can)
        return tr("CAN 帧格式");
    if (mode == InputListMode::Registers)
        return tr("Modbus 写入列表");
    if (mode == InputListMode::S7Value)
        return tr("S7 写入配置");
    if (mode == InputListMode::ModbusValue)
        return tr("Modbus 操作配置");
    return tr("发送配置");
}

QStringList signalNames(const QJsonObject &properties, const QList<CanPreviewSource> &sources)
{
    if (CommandEncoder::inputListMode(properties) != InputListMode::Can)
        return {};
    const QString id = properties.value(QStringLiteral("canId")).toString();
    QStringList names;
    bool resolved = false;
    for (const auto &source : sources) {
        const auto target = source.signalSet ? source.signalSet->target(id) : std::nullopt;
        if (!target)
            continue;
        if (!target->hasSignals)
            return {};
        QStringList onCard;
        for (const auto *signal : target->signalDefs)
            onCard.append(signal->name);
        if (resolved)
            names.removeIf([&onCard](const QString &name) { return !onCard.contains(name); });
        else
            names = onCard;
        resolved = true;
    }
    return names;
}

struct FrameField
{
    QString signal;
    QString variable;
    QString content;
};

QString frameFieldKey(int row) { return QStringLiteral("field:%1").arg(row); }

QList<FrameField> frameFields(const QString &text, bool useSignals)
{
    QList<FrameField> fields;
    if (useSignals) {
        // Keep unfinished assignments in the editor, including rows without a chosen signal.
        static const QRegularExpression separator(QStringLiteral("[,;\\n]"));
        for (const auto &assignment : text.split(separator, Qt::SkipEmptyParts)) {
            if (assignment.trimmed().isEmpty())
                continue;
            const qsizetype equals = assignment.indexOf(u'=');
            const QString name = equals < 0 ? QString() : assignment.left(equals).trimmed();
            const QString value = assignment.mid(equals + 1).trimmed();
            const auto parts = CommandEncoder::templateParts(value);
            if (parts.size() == 1 && parts.first().isInput)
                fields.append({name, parts.first().text, {}});
            else
                fields.append({name, {}, value});
        }
    } else {
        for (const auto &part : CommandEncoder::templateParts(text)) {
            if (part.isInput)
                fields.append({{}, part.text, {}});
            else if (!part.text.trimmed().isEmpty())
                fields.append({{}, {}, part.text.trimmed()});
        }
    }
    return fields;
}

class FrameSequenceEditor : public QWidget
{
  public:
    FrameSequenceEditor(const QJsonObject &library, const QJsonArray &sequence, const InputValues &inputs,
                        bool canCreate, QWidget *parent)
        : QWidget(parent), m_library(library), m_inputs(inputs), m_canCreate(canCreate)
    {
        auto *layout = new QHBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        auto *left = new QVBoxLayout;
        layout->addLayout(left);
        auto *add = new FluentQt::ComboBox(this);
        add->setObjectName(QStringLiteral("addSequenceFrame"));
        fillFrameTemplates(add, library, {}, fhmi::tr("添加 HEX 帧模板…"), QStringLiteral("hex"));
        add->setEnabled(add->count() > 1);
        left->addWidget(add);
        m_order = new FluentQt::ListWidget(this);
        m_order->setObjectName(QStringLiteral("sendFrameSequence"));
        m_order->setAccessibleName(fhmi::tr("发送顺序"));
        m_order->setFixedWidth(230);
        m_order->setSelectionMode(QAbstractItemView::SingleSelection);
        left->addWidget(m_order, 1);
        auto *edit = new QHBoxLayout;
        left->addLayout(edit);
        const auto action = [&](FluentQt::FluentIcon icon, const QString &text) {
            auto *control = new FluentQt::TransparentToolButton(FluentQt::icon(icon), this);
            fluentToolTip(control, text);
            control->setEnabled(false);
            edit->addWidget(control);
            return control;
        };
        auto *up = action(FluentQt::FluentIcon::Up, fhmi::tr("上移"));
        auto *down = action(FluentQt::FluentIcon::Down, fhmi::tr("下移"));
        auto *remove = action(FluentQt::FluentIcon::Delete, fhmi::tr("删除所选"));
        edit->addStretch();
        m_fields = new QStackedWidget(this);
        auto *hint = new FluentQt::BodyLabel(fhmi::tr("按发送顺序添加模板，再为各帧设置字段值。"), m_fields);
        hint->setWordWrap(true);
        m_fields->addWidget(hint);
        layout->addWidget(m_fields, 1);
        connect(m_order, &QListWidget::currentRowChanged, this, [this, hint, up, down, remove](int row) {
            const bool selected = row >= 0 && row < m_entries.size();
            m_fields->setCurrentWidget(selected ? static_cast<QWidget *>(m_entries.at(row).fields) : hint);
            up->setEnabled(selected && row > 0);
            down->setEnabled(selected && row + 1 < m_entries.size());
            remove->setEnabled(selected);
            if (changed)
                changed();
        });
        connect(add, &FluentQt::ComboBox::currentIndexChanged, this, [this, add](int index) {
            if (index <= 0)
                return;
            append(selectedValue(add), {});
            refreshOrder(m_entries.size() - 1);
            const QSignalBlocker blocker(add);
            add->setCurrentIndex(0);
        });
        const auto move = [this](int delta) {
            const int row = m_order->currentRow();
            m_entries.swapItemsAt(row, row + delta);
            refreshOrder(row + delta);
        };
        connect(up, &QToolButton::clicked, this, [move] { move(-1); });
        connect(down, &QToolButton::clicked, this, [move] { move(1); });
        connect(remove, &QToolButton::clicked, this, [this] {
            const int row = m_order->currentRow();
            auto *fields = m_entries.takeAt(row).fields;
            m_fields->removeWidget(fields);
            fields->deleteLater();
            refreshOrder(std::min(row, int(m_entries.size()) - 1));
        });
        for (const auto &value : sequence) {
            const auto frame = value.toObject();
            append(frame.value(FrameTemplates::SendId).toString(), frame.value(FrameTemplates::SendBindings).toArray());
        }
        refreshOrder(m_entries.isEmpty() ? -1 : 0);
    }

    std::function<void()> changed;
    std::function<void(const QString &)> selectedField;

    QJsonArray frames() const
    {
        QJsonArray frames;
        for (const auto &entry : m_entries)
            frames.append(config(entry));
        return frames;
    }
    FrameFieldInputsEditor *activeFields() const
    {
        return qobject_cast<FrameFieldInputsEditor *>(m_fields->currentWidget());
    }
    QJsonObject activeFrame() const
    {
        const int row = m_order->currentRow();
        return row < 0 ? QJsonObject{} : config(m_entries.at(row));
    }
    InputValues sampleValues() const
    {
        auto values = m_inputs;
        for (const auto &entry : m_entries) {
            const auto samples = entry.fields->sampleValues();
            for (auto it = samples.cbegin(); it != samples.cend(); ++it)
                if (!values.contains(it.key()) || values.value(it.key()).text().isEmpty())
                    values.insert(it.key(), it.value());
        }
        return values;
    }
    QMap<QString, QString> pendingInputs() const
    {
        QMap<QString, QString> inputs;
        for (const auto &entry : m_entries)
            inputs.insert(entry.fields->pendingInputs());
        return inputs;
    }

  private:
    struct Entry { QString id; QString name; FrameFieldInputsEditor *fields; };

    static QJsonObject config(const Entry &entry)
    {
        return {{FrameTemplates::SendId, entry.id}, {FrameTemplates::SendBindings, entry.fields->bindings()}};
    }

    void append(const QString &id, const QJsonArray &bindings)
    {
        const auto preset = FrameTemplates::find(m_library, id);
        auto *fields = new FrameFieldInputsEditor(bindings, m_inputs, m_canCreate, m_fields);
        const auto settings = preset ? preset->format.settings : QJsonObject();
        fields->setFormat(preset ? hexFrameConfig(settings) : std::nullopt, settings);
        m_entries.append({id, preset ? preset->name : id, fields});
        m_fields->addWidget(fields);
        connect(fields, &FrameFieldInputsEditor::changed, this, [this] {
            if (changed)
                changed();
        });
        connect(fields, &FrameFieldInputsEditor::fieldSelected, this, [this, fields](const QString &name) {
            if (fields == activeFields() && selectedField)
                selectedField(name);
        });
    }

    void refreshOrder(int selected)
    {
        m_order->clear();
        for (int index = 0; index < m_entries.size(); ++index)
            m_order->addItem(QStringLiteral("%1. %2").arg(index + 1).arg(m_entries.at(index).name));
        m_order->setCurrentRow(selected);
    }

    QJsonObject m_library;
    InputValues m_inputs;
    bool m_canCreate;
    QList<Entry> m_entries;
    FluentQt::ListWidget *m_order;
    QStackedWidget *m_fields;
};

class FrameTemplateEditor : public AppDialog
{
  public:
    FrameTemplateEditor(const CanvasItem &item, const Project &project, QList<CanPreviewSource> sources, InputValues values,
                        bool canCreate, QWidget *parent)
        : AppDialog(dialogTitle(CommandEncoder::inputListMode(item.properties())), parent),
          m_properties(item.properties()), m_project(project), m_sources(std::move(sources)),
          m_signalNames(signalNames(m_properties, m_sources)),
          m_mode(CommandEncoder::inputListMode(m_properties)),
          m_byteFrame(m_mode == InputListMode::Hex || (m_mode == InputListMode::Can && m_signalNames.isEmpty()))
    {
        setObjectName(QStringLiteral("frameTemplateEditor"));
        setAttribute(Qt::WA_DeleteOnClose);
        const bool hasTemplate = CommandEncoder::usesTemplate(m_mode);
        setPreferredSize(hasTemplate ? QSize(1070, m_mode == InputListMode::Hex ? 765 : m_byteFrame ? 710 : 680) :
                         m_mode == InputListMode::Registers ? QSize(760, 740) : QSize(620, 510));
        viewLayout()->setSpacing(9);

        const QStringList dataTypes = m_byteFrame ? CommandEncoder::hexDataTypes()
            : m_mode == InputListMode::Registers ? CommandEncoder::dataTypes() : QStringList{};
        auto *inputPane = new QWidget(this);
        auto *inputLayout = new QVBoxLayout(inputPane);
        inputLayout->setContentsMargins(0, 0, 0, 0);
        inputLayout->setSpacing(8);
        m_inputEditor = new CommandInputsEditor(m_properties, values, dataTypes, m_project.widgets, canCreate, inputPane);
        if (!hasTemplate)
            buildAddressSettings(inputLayout, item);
        inputLayout->addWidget(m_inputEditor, m_mode == InputListMode::Registers || hasTemplate ? 1 : 0);
        m_staticSettings = new QWidget(inputPane);
        auto *fixedLayout = new QVBoxLayout(m_staticSettings);
        fixedLayout->setContentsMargins(0, 0, 0, 0);
        auto *fixedValue = new QHBoxLayout;
        fixedLayout->addLayout(fixedValue);
        if (m_mode == InputListMode::S7Value) {
            auto settings = item.propertyValue(QStringLiteral("s7Command")).toObject();
            auto *label = new FluentQt::BodyLabel(fhmi::tr("写入值"), m_staticSettings);
            auto *edit = new FluentQt::LineEdit(m_staticSettings);
            edit->setText(settings.value(QStringLiteral("value")).toVariant().toString());
            label->setBuddy(edit);
            fixedValue->addWidget(label);
            fixedValue->addWidget(edit, 1);
            connect(edit, &QLineEdit::textChanged, this, [this](const QString &text) {
                setS7Setting(QStringLiteral("value"), text);
            });
        } else if (hasTemplate) {
            addSetting(fixedValue, item, QStringLiteral("command"));
        } else {
            const auto argument = CommandEncoder::modbusFunctionSpec(
                CommandEncoder::functionCode(item.propertyValue(QStringLiteral("mbFunc")).toString())).argument;
            addSetting(fixedValue, item, QStringLiteral("mbValue"),
                       argument == CommandEncoder::ModbusArgument::ReadCount ? fhmi::tr("读取数量") : fhmi::tr("写入值"));
        }
        if (m_byteFrame) {
            auto *dataType = new QHBoxLayout;
            addSetting(dataType, item, QStringLiteral("dataType"));
            dataType->addStretch();
            fixedLayout->addLayout(dataType);
        }
        if (hasTemplate)
            inputLayout->insertWidget(0, m_staticSettings);
        else
            inputLayout->addWidget(m_staticSettings);
        if (!hasTemplate && m_mode != InputListMode::Registers)
            inputLayout->addStretch();
        if (m_mode == InputListMode::Registers) {
            m_registerOrder = new QWidget(inputPane);
            auto *order = new QHBoxLayout(m_registerOrder);
            order->setContentsMargins(0, 0, 0, 0);
            addSetting(order, item, QStringLiteral("mbRegOrder"));
            order->addStretch();
            inputLayout->addWidget(m_registerOrder);
        } else if (m_mode == InputListMode::Json && !m_properties.value(QStringLiteral("jsonUseInputs")).toBool()) {
            auto shared = m_properties;
            shared.insert(QStringLiteral("jsonUseInputs"), true);
            const auto rows = CommandEncoder::commandInputs(shared);
            if (!rows.isEmpty()) {
                auto *reuse = new FluentQt::TransparentPushButton(fhmi::tr("沿用已有输入列表"), inputPane);
                inputLayout->insertWidget(1, reuse, 0, Qt::AlignLeft);
                connect(reuse, &QPushButton::clicked, this, [this, reuse, rows] {
                    m_inputEditor->useRows(rows);
                    reuse->hide();
                });
            }
        }
        if (hasTemplate) {
            m_payloadEditors = new QStackedWidget(this);
            auto *split = new QSplitter(Qt::Horizontal, m_payloadEditors);
            split->setChildrenCollapsible(false);
            split->addWidget(inputPane);
            auto *format = new QWidget(split);
            auto *layout = new QVBoxLayout(format);
            layout->setContentsMargins(4, 0, 0, 0);
            layout->setSpacing(8);
            buildTemplatePane(layout, item);
            split->addWidget(format);
            split->setSizes({368, 632});
            split->setStretchFactor(0, 0);
            split->setStretchFactor(1, 1);
            m_payloadEditors->addWidget(split);
            if (m_byteFrame) {
                m_namedFields = new FrameFieldInputsEditor(m_properties.value(FrameTemplates::SendBindings).toArray(),
                                                            values, canCreate, m_payloadEditors);
                m_payloadEditors->addWidget(m_namedFields);
                connect(m_namedFields, &FrameFieldInputsEditor::changed, this, &FrameTemplateEditor::refreshPreview);
            }
            if (m_mode == InputListMode::Hex && !m_project.frameTemplates.isEmpty()) {
                m_sequence = new FrameSequenceEditor(m_project.frameTemplates,
                    m_properties.value(FrameTemplates::SendSequence).toArray(), values, canCreate, m_payloadEditors);
                m_payloadEditors->addWidget(m_sequence);
                auto *modes = new FluentQt::SegmentedWidget(this);
                modes->setObjectName(QStringLiteral("sendTemplateSequenceMode"));
                modes->addItem(QStringLiteral("single"), fhmi::tr("单帧"));
                modes->addItem(QStringLiteral("sequence"), fhmi::tr("多模板混发"));
                m_sequenceEnabled = CommandEncoder::usesFrameSequence(m_properties);
                modes->setCurrentItem(m_sequenceEnabled ? QStringLiteral("sequence") : QStringLiteral("single"));
                viewLayout()->addWidget(modes, 0, Qt::AlignLeft);
                m_sequence->changed = [this] { refreshPreview(); };
                connect(modes, &FluentQt::SegmentedWidget::currentItemChanged, this, [this](const QString &key) {
                    m_sequenceEnabled = key == QLatin1String("sequence");
                    refreshFieldChoices();
                    refreshPreview();
                });
            }
            addPayloadChoices();
            viewLayout()->addWidget(m_payloadEditors, 1);
        } else {
            viewLayout()->addWidget(inputPane, 1);
        }

        auto *previewLayout = new QVBoxLayout;
        previewLayout->setSpacing(4);
        auto *previewHeading = new QHBoxLayout;
        auto *previewLabel = new FluentQt::BodyLabel(fhmi::tr("发送示例"), this);
        previewHeading->addWidget(previewLabel);
        previewHeading->addStretch();
        previewLayout->addLayout(previewHeading);
        m_target = new FluentQt::CaptionLabel(this);
        m_target->setWordWrap(true);
        m_target->setObjectName(QStringLiteral("sendConfigurationTarget"));
        previewLayout->addWidget(m_target);
        m_preview = new FluentQt::PlainTextEdit(this);
        m_preview->setObjectName(QStringLiteral("frameTemplatePreview"));
        m_preview->setAccessibleName(previewLabel->text());
        m_preview->setReadOnly(true);
        m_preview->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        m_preview->setFixedHeight(m_mode == InputListMode::Can || !hasTemplate ? 74 : 52);
        previewLayout->addWidget(m_preview);
        if (m_byteFrame)
            buildBytePreview(previewLayout, previewHeading);
        viewLayout()->addLayout(previewLayout);
        auto *problem = new FluentQt::BodyLabel(this);
        problem->setWordWrap(true);
        problem->hide();
        viewLayout()->addWidget(problem);
        setValidator([this, problem] {
            QString pointProblem;
            const auto configured = draft();
            const auto effective = resolvePointWrite(configured, m_project.logic, m_project.widgets, m_project.parserType(),
                                                     &pointProblem, u"PushButton", ModbusParserChoice::OnConnect);
            const auto props = effective.value_or(configured);
            QString error = m_sequenceEnabled
                ? (m_sequence->frames().isEmpty() ? fhmi::tr("请添加至少一个发送帧模板") : QString())
                : usesMappedFields() ? m_namedFields->validate() : m_inputEditor->validate();
            if (error.isEmpty())
                error = pointProblem;
            if (error.isEmpty() && editsFixedCan() && !templateText().trimmed().isEmpty())
                parseAssignments(templateText(), &error);
            if (error.isEmpty() && (usesMappedFields() || m_sequenceEnabled))
                error = CommandEncoder::frameFieldConfigurationProblem(props, m_project.frameTemplates);
            const auto target = previewCommandTarget(props, QStringLiteral("PushButton"), m_project);
            if (error.isEmpty() && target.routeInvalid)
                error = target.error;
            if (error.isEmpty() && m_mode == InputListMode::S7Value) {
                const auto point = S7::point(props.value(QStringLiteral("s7Command")).toObject(), &error);
                if (point && point->address.area == S7::MemoryArea::Input)
                    error = fhmi::tr("S7 输入区 I 只读，请选择 DB、M 或 Q 写入");
                else if (point)
                    error = S7::writeValueProblem(props, point->type, u"PushButton");
            }
            if (error.isEmpty() && !m_sequenceEnabled && !usesMappedFields() && m_expression && !m_hasInputs &&
                !props.value(QStringLiteral("command")).toString().trimmed().isEmpty()) {
                for (const auto &part : CommandEncoder::templateParts(templateText())) {
                    if (part.isInput && part.text != QLatin1String("value") && m_linkedVariables.contains(part.text)) {
                        error = fhmi::tr("发送模板仍引用已移除的变量 %1").arg(CommandEncoder::inputToken(part.text));
                        break;
                    }
                }
            }
            problem->setText(error);
            problem->setVisible(!error.isEmpty());
            return error.isEmpty();
        });
        connect(m_inputEditor, &CommandInputsEditor::changed, this, &FrameTemplateEditor::inputChanged);
        m_hasInputs = !m_inputEditor->rows().isEmpty();
        refreshSamples();
        m_staticSettings->setVisible(!m_hasInputs && !editsFixedCan());
        refreshFieldChoices();
        if (m_expression) {
            m_generated = generated();
            const QString current = item.propertyValue(QStringLiteral("commandTemplate")).toString();
            if (editsFixedCan()) {
                const auto result = CommandEncoder::buttonFromProperties(m_properties, {}, nullptr, m_project.frameTemplates);
                if (result.command && result.command->kind == EncodedCommand::Kind::Can) {
                    rebuildVariableMenu();
                    setTemplate(result.command->can.assignments);
                    return;
                }
            }
            const bool legacy = current.trimmed().isEmpty() || (current == QLatin1String("{value}") &&
                std::none_of(m_inputEditor->rows().cbegin(), m_inputEditor->rows().cend(), [](const auto &input) {
                    return input.variable == QLatin1String("value");
                }));
            rebuildVariableMenu();
            setTemplate(editsFixedCan() ? item.propertyValue(QStringLiteral("command")).toString()
                                        : legacy ? m_generated : current);
        } else {
            refreshPreview();
        }
    }

    QJsonObject changes() const
    {
        QJsonObject changes = m_editedSettings;
        if (m_sequenceEnabled) {
            changes.insert(FrameTemplates::SendSequence, m_sequence->frames());
            return changes;
        }
        if (m_mode == InputListMode::Hex && m_properties.contains(FrameTemplates::SendSequence))
            changes.insert(FrameTemplates::SendSequence, QJsonArray{});
        if (usesMappedFields()) {
            changes.insert(FrameTemplates::SendFieldsEnabled, true);
            changes.insert(FrameTemplates::SendBindings, m_namedFields->bindings());
            return changes;
        }
        if (m_payloadMode && (m_payloadModeChosen || m_properties.contains(FrameTemplates::SendFieldsEnabled)))
            changes.insert(FrameTemplates::SendFieldsEnabled, false);
        const auto rows = m_inputEditor->toJson();
        const QString key = CommandEncoder::singleInputProperty(m_mode);
        if (!key.isEmpty()) {
            if (!rows.isEmpty() || m_properties.contains(key))
                changes.insert(key, rows.isEmpty() ? QString() : m_inputEditor->rows().first().inputId);
        } else if (m_mode != InputListMode::Json || !rows.isEmpty() || m_properties.value(QStringLiteral("jsonUseInputs")).toBool()) {
            if (!rows.isEmpty() || m_properties.contains(QStringLiteral("commandInputs")))
                changes.insert(QStringLiteral("commandInputs"), rows);
        }
        if (m_mode == InputListMode::Json && (!rows.isEmpty() || m_properties.contains(QStringLiteral("jsonUseInputs"))))
            changes.insert(QStringLiteral("jsonUseInputs"), !rows.isEmpty());
        if (editsFixedCan()) {
            changes.insert(QStringLiteral("command"), templateText());
            changes.insert(QStringLiteral("commandTemplate"), QStringLiteral("{value}"));
        } else if (m_expression) {
            changes.insert(QStringLiteral("commandTemplate"), templateText());
        }
        return changes;
    }

    QMap<QString, QString> pendingInputs() const
    {
        if (m_sequenceEnabled)
            return m_sequence->pendingInputs();
        return usesMappedFields() ? m_namedFields->pendingInputs() : m_inputEditor->pendingInputs();
    }

  private:
    bool usesMappedFields() const { return !m_sequenceEnabled && m_payloadMode && m_payloadMode->currentItem() == FieldPayload; }
    bool editsFixedCan() const { return !m_signalNames.isEmpty() && !m_hasInputs; }

    void addPayloadChoices()
    {
        m_payloadChoices = new QWidget(this);
        auto *row = new QHBoxLayout(m_payloadChoices);
        row->setContentsMargins(0, 0, 0, 0);
        if (m_mode != InputListMode::Can || m_byteFrame) {
            auto *choice = new FluentQt::ComboBox(this);
            choice->setObjectName(QStringLiteral("sendConfigurationFrameTemplate"));
            fillFrameTemplates(choice, m_project.frameTemplates, m_properties.value(FrameTemplates::SendId).toString(),
                               fhmi::tr("独立配置"),
                               FrameTemplates::sendParser(m_properties.value(QStringLiteral("commandMode")).toString()));
            if (choice->count() > 1) {
                auto *label = new FluentQt::BodyLabel(fhmi::tr("发送帧模板"), this);
                label->setBuddy(choice);
                row->addWidget(label);
                row->addWidget(choice, 1);
                connect(choice, &FluentQt::ComboBox::currentIndexChanged, this, [this, choice] {
                    m_editedSettings.insert(FrameTemplates::SendId, selectedValue(choice));
                    refreshFieldChoices();
                    refreshPreview();
                });
            } else {
                choice->hide();
            }
        }
        if (m_namedFields) {
            m_payloadMode = new FluentQt::SegmentedWidget(this);
            m_payloadMode->setObjectName(QStringLiteral("sendFramePayloadMode"));
            m_payloadMode->addItem(FieldPayload, fhmi::tr("按字段填写"));
            m_payloadMode->addItem(RawPayload, fhmi::tr("整段 HEX"));
            m_payloadMode->setCurrentItem(CommandEncoder::usesFrameFields(m_properties) ? FieldPayload : RawPayload);
            m_payloadModeLabel = new FluentQt::BodyLabel(fhmi::tr("载荷来源"), this);
            m_payloadMode->setAccessibleName(m_payloadModeLabel->text());
            row->addWidget(m_payloadModeLabel);
            row->addWidget(m_payloadMode);
            connect(m_payloadMode, &FluentQt::SegmentedWidget::currentItemChanged, this, [this](const QString &key) {
                m_payloadModeChosen = true;
                m_payloadEditors->setCurrentIndex(key == FieldPayload ? 1 : 0);
                refreshPreview();
            });
        }
        viewLayout()->addWidget(m_payloadChoices);
        m_payloadChoices->setVisible(row->count() && !m_sequenceEnabled);
        if (m_payloadMode) {
            m_payloadHint = new FluentQt::CaptionLabel(fhmi::tr("发送模板负责帧头、长度、校验和帧尾。"), this);
            m_payloadHint->setWordWrap(true);
            m_payloadHint->hide();
            viewLayout()->addWidget(m_payloadHint);
        }
    }

    void refreshFieldChoices()
    {
        if (!m_namedFields)
            return;
        if (m_sequence) {
            m_payloadChoices->setVisible(!m_sequenceEnabled);
            if (m_sequenceEnabled) {
                m_payloadEditors->setCurrentWidget(m_sequence);
                return;
            }
        }
        auto properties = m_properties;
        if (m_editedSettings.contains(FrameTemplates::SendId))
            properties.insert(FrameTemplates::SendId, m_editedSettings.value(FrameTemplates::SendId));
        const auto settings = FrameTemplates::sendSettings(m_project.frameTemplates, properties);
        const auto format = settings ? hexFrameConfig(*settings) : std::nullopt;
        m_namedFields->setFormat(format, settings.value_or(QJsonObject()));
        const bool hasFields = format && !FrameTemplates::writableFields(*format).isEmpty();
        if (!m_payloadModeChosen && !m_properties.contains(FrameTemplates::SendFieldsEnabled)) {
            const QString commandKey = QStringLiteral("command");
            const auto command = m_editedSettings.contains(commandKey) ? m_editedSettings.value(commandKey)
                                                                       : m_properties.value(commandKey);
            const QString expression = (m_generated.isEmpty() ? m_properties.value(QStringLiteral("commandTemplate")).toString()
                                                               : templateText()).trimmed();
            const bool hasRawPayload = !m_inputEditor->rows().isEmpty() || !command.toString().trimmed().isEmpty() ||
                                       (!expression.isEmpty() && expression != QLatin1String("{value}"));
            const QSignalBlocker blocker(m_payloadMode);
            m_payloadMode->setCurrentItem(hasFields && !hasRawPayload ? FieldPayload : RawPayload);
        }
        const bool showMode = hasFields || usesMappedFields();
        m_payloadMode->setVisible(showMode);
        m_payloadModeLabel->setVisible(showMode);
        m_payloadMode->item(FieldPayload)->setEnabled(hasFields);
        m_payloadEditors->setCurrentIndex(usesMappedFields() ? 1 : 0);
    }

    void setS7Setting(const QString &key, const QString &value)
    {
        constexpr auto settings = QLatin1String("s7Command");
        auto current = m_editedSettings.value(settings).toObject();
        if (!m_editedSettings.contains(settings))
            current = m_properties.value(settings).toObject();
        current.insert(key, value);
        m_editedSettings.insert(settings, current);
        refreshPreview();
    }

    void buildAddressSettings(QVBoxLayout *layout, const CanvasItem &item)
    {
        const bool isS7 = m_mode == InputListMode::S7Value;
        const auto key = writePointKey(m_properties.value(QStringLiteral("commandMode")).toString());
        auto *point = new FluentQt::ComboBox(this);
        point->setObjectName(QStringLiteral("writeDevicePoint"));
        fillWritePointChoices(point, m_project, m_project.widgets, m_properties, u"PushButton");
        if (point->count() > 1) {
            auto *source = new QFormLayout;
            source->addRow(fhmi::tr("写入对象"), point);
            layout->addLayout(source);
            connect(point, &FluentQt::ComboBox::currentIndexChanged, this, [this, point, key] {
                m_editedSettings.insert(key, selectedValue(point));
                refreshPreview();
            });
        } else {
            point->hide();
        }

        m_manualAddress = new QWidget(this);
        auto *form = new QFormLayout(m_manualAddress);
        form->setContentsMargins(0, 0, 0, 0);
        layout->addWidget(m_manualAddress);
        auto *target = new ConnectionSourceEdit(m_manualAddress, ConnectionSourceEdit::Purpose::Send);
        target->setObjectName(QStringLiteral("sendConfigurationConnection"));
        target->setSources(&m_project, nullptr, CommandEncoder::sendRouteFilter(m_properties, QStringLiteral("PushButton")),
                           m_properties.value(QStringLiteral("commandTargetId")).toString());
        form->addRow(fhmi::tr("发送连接"), target);
        if (isS7) {
            const auto settings = m_properties.value(QStringLiteral("s7Command")).toObject();
            auto *address = new FluentQt::LineEdit(m_manualAddress);
            address->setObjectName(QStringLiteral("s7WriteAddress"));
            address->setText(settings.value(QStringLiteral("address")).toString());
            address->setPlaceholderText(QStringLiteral("DB1.DBD0"));
            form->addRow(fhmi::tr("S7 写入地址"), address);
            auto *type = new FluentQt::ComboBox(m_manualAddress);
            addValueItems(type, S7::dataTypes());
            selectValue(type, settings.value(QStringLiteral("dataType")).toString(QStringLiteral("REAL")));
            form->addRow(fhmi::tr("S7 数据类型"), type);
            connect(address, &QLineEdit::textChanged, this, [this](const QString &text) {
                setS7Setting(QStringLiteral("address"), text);
            });
            connect(type, &FluentQt::ComboBox::currentIndexChanged, this, [this, type] {
                setS7Setting(QStringLiteral("dataType"), selectedValue(type));
            });
        } else {
            auto *endpoint = new FluentQt::LineEdit(m_manualAddress);
            endpoint->setObjectName(QStringLiteral("sendConfigurationEndpoint"));
            endpoint->setText(m_properties.value(QStringLiteral("commandEndpoint")).toString());
            target->bindEndpointEditor(endpoint);
            auto *endpointLabel = new FluentQt::BodyLabel(fhmi::tr("发送子通道"), m_manualAddress);
            endpointLabel->setBuddy(endpoint);
            form->addRow(endpointLabel, endpoint);
            const auto update = [target, endpoint, form] {
                endpoint->setPlaceholderText(target->endpointPlaceholder());
                form->setRowVisible(endpoint, target->offersEndpoint() || !endpoint->text().isEmpty());
            };
            update();
            connect(endpoint, &QLineEdit::textChanged, this, [this, update](const QString &text) {
                m_editedSettings.insert(QStringLiteral("commandEndpoint"), text.trimmed());
                update();
                refreshPreview();
            });
            connect(target, &FluentQt::ComboBox::currentIndexChanged, endpoint, [update, endpoint] {
                endpoint->clear();
                update();
            });
            const auto number = [&](const char *field, const QString &label, int minimum, int maximum) {
                auto *edit = new FluentQt::SpinBox(m_manualAddress);
                const auto name = QLatin1String(field);
                edit->setRange(minimum, maximum);
                edit->setValue(item.propertyValue(name).toInt());
                form->addRow(label, edit);
                connect(edit, qOverload<int>(&QSpinBox::valueChanged), this, [this, name, target, update](int value) {
                    m_editedSettings.insert(name, value);
                    if (name == QLatin1String("mbSlave")) {
                        const auto props = draft();
                        target->setSources(&m_project, nullptr, CommandEncoder::sendRouteFilter(props, QStringLiteral("PushButton")),
                                           props.value(QStringLiteral("commandTargetId")).toString());
                        update();
                    }
                    refreshPreview();
                });
            };
            number("mbSlave", fhmi::tr("从站地址"), 0, 247);
            number("mbAddress", fhmi::tr("起始地址"), 0, 65535);
        }
        connect(target, &FluentQt::ComboBox::currentIndexChanged, this, [this, target] {
            m_editedSettings.insert(QStringLiteral("commandTargetId"), selectedValue(target));
            refreshPreview();
        });
    }

    void buildTemplatePane(QVBoxLayout *layout, const CanvasItem &item)
    {
        const bool text = m_mode == InputListMode::Text || m_mode == InputListMode::Json;
        m_editors = new QStackedWidget(this);
        m_expression = new FluentQt::PlainTextEdit(m_editors);
        m_expression->setObjectName(QStringLiteral("frameTemplateExpression"));
        m_expression->setAccessibleName(fhmi::tr("发送模板"));
        m_expression->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        m_expression->setTabChangesFocus(true);
        m_editors->addWidget(m_expression);
        if (!text)
            buildFieldTable();

        if (m_byteFrame) {
            auto *tabs = new FluentQt::SegmentedWidget(this);
            m_frameTabs = tabs;
            tabs->addItem(QStringLiteral("fields"), fhmi::tr("帧内容"));
            tabs->addItem(QStringLiteral("options"), fhmi::tr("长度与校验"))
                ->setObjectName(QStringLiteral("editFrameOptions"));
            layout->addWidget(tabs, 0, Qt::AlignLeft);
            auto *pages = new QStackedWidget(this);
            auto *fields = new QWidget(pages);
            pages->addWidget(fields);
            auto *options = buildFrameOptions(pages);
            pages->addWidget(options);
            layout->addWidget(pages, 1);
            connect(tabs, &FluentQt::SegmentedWidget::currentItemChanged, pages,
                    [pages, fields, options](const QString &key) {
                        pages->setCurrentWidget(key == QLatin1String("options") ? options : fields);
                    });
            tabs->setCurrentItem(QStringLiteral("fields"));
            layout = new QVBoxLayout(fields);
            layout->setContentsMargins(0, 0, 0, 0);
            layout->setSpacing(8);
            auto *settings = new QHBoxLayout;
            settings->setSpacing(8);
            if (m_mode == InputListMode::Hex) {
                m_manualFraming = new QWidget(this);
                auto *envelope = new QHBoxLayout(m_manualFraming);
                envelope->setContentsMargins(0, 0, 0, 0);
                addSetting(envelope, item, QStringLiteral("frameHeader"));
                addSetting(envelope, item, QStringLiteral("frameTail"));
                settings->addWidget(m_manualFraming, 1);
            }
            m_byteOrderSetting = new QWidget(this);
            m_byteOrderSetting->setObjectName(QStringLiteral("frameTemplateByteOrderRow"));
            auto *order = new QHBoxLayout(m_byteOrderSetting);
            order->setContentsMargins(0, 0, 0, 0);
            addSetting(order, item, QStringLiteral("byteOrder"));
            settings->addWidget(m_byteOrderSetting, 1);
            layout->addLayout(settings);
        }

        auto *toolbar = new QHBoxLayout;
        toolbar->setSpacing(6);
        m_insert = new FluentQt::DropDownPushButton(fhmi::tr("插入变量"), this);
        m_insert->setObjectName(QStringLiteral("insertFrameVariable"));
        if (!text)
            toolbar->addWidget(buildFieldActions());
        toolbar->addWidget(m_insert);

        auto *generate = new FluentQt::TransparentPushButton(fhmi::tr("自动生成"), this);
        generate->setObjectName(QStringLiteral("generateFrameTemplate"));
        connect(generate, &QPushButton::clicked, this, [this] { setTemplate(m_generated); });
        if (text) {
            toolbar->addWidget(generate);
            toolbar->addStretch();
            m_manualFraming = new QWidget(this);
            auto *ending = new QHBoxLayout(m_manualFraming);
            ending->setContentsMargins(0, 0, 0, 0);
            addSetting(ending, item, m_mode == InputListMode::Json ? QStringLiteral("commandJsonNewline")
                                                                  : QStringLiteral("commandNewline"));
            toolbar->addWidget(m_manualFraming);
        } else {
            toolbar->addStretch();
            toolbar->addWidget(generate);
            auto *toggle = new FluentQt::TransparentPushButton(fhmi::tr("模板文本"), this);
            toggle->setObjectName(QStringLiteral("toggleFrameTemplateText"));
            toolbar->addWidget(toggle);
            m_insert->hide();
            connect(toggle, &QPushButton::clicked, this, [this, toggle] {
                const QString current = templateText();
                const bool showText = !editsText();
                m_editors->setCurrentWidget(showText ? static_cast<QWidget *>(m_expression) : m_table);
                m_fieldActions->setVisible(!showText);
                m_insert->setVisible(showText);
                toggle->setText(showText ? fhmi::tr("字段编辑") : fhmi::tr("模板文本"));
                setTemplate(current);
                if (showText)
                    m_expression->setFocus();
            });
        }
        layout->addLayout(toolbar);
        layout->addWidget(m_editors, 1);
        connect(m_expression, &QPlainTextEdit::textChanged, this, [this] { refreshPreview(); });
    }

    void buildBytePreview(QVBoxLayout *layout, QHBoxLayout *heading)
    {
        m_byteInfo = new FluentQt::CaptionLabel(this);
        heading->addWidget(m_byteInfo);
        m_copyBytes = new FluentQt::TransparentToolButton(FluentQt::icon(FluentQt::FluentIcon::Copy), this);
        m_copyBytes->setAccessibleName(fhmi::tr("复制 HEX"));
        m_copyBytes->setToolTip(m_copyBytes->accessibleName());
        heading->addWidget(m_copyBytes);
        m_diagramControls = new QWidget(this);
        auto *modes = new QHBoxLayout(m_diagramControls);
        modes->setContentsMargins(0, 0, 0, 0);
        heading->insertWidget(1, m_diagramControls);
        m_frameDiagram = new FrameDiagram(this, modes);
        m_frameDiagram->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);
        m_byteView = m_frameDiagram->byteView();
        layout->addWidget(m_frameDiagram);
        m_frameCoverage = new FrameCoveragePreview(this);
        layout->addWidget(m_frameCoverage);
        connect(m_copyBytes, &QToolButton::clicked, this, [this] {
            QGuiApplication::clipboard()->setText(toHexString(m_byteView->bytes()));
        });
        connect(m_table, &QTableWidget::currentCellChanged, this, [this](int row) {
            if (row >= 0)
                m_byteView->selectPart(frameFieldKey(row));
        });
        connect(m_namedFields, &FrameFieldInputsEditor::fieldSelected, this, [this](const QString &name) {
            m_byteView->selectPart(QStringLiteral("mapped:") + name);
        });
        if (m_sequence)
            m_sequence->selectedField = [this](const QString &name) { m_byteView->selectPart(QStringLiteral("mapped:") + name); };
        connect(qApp, &QApplication::focusChanged, this, [this](QWidget *, QWidget *focused) {
            if (!focused || !isAncestorOf(focused))
                return;
            for (QWidget *control = focused; control != this; control = control->parentWidget()) {
                const QString key = control->property("framePart").toString();
                if (!key.isEmpty()) {
                    m_byteView->selectPart(key);
                    break;
                }
            }
        });
        connect(m_byteView, &FrameBytesView::partClicked, this, [this](const QString &key) {
            if (usesMappedFields() || m_sequenceEnabled) {
                auto *fields = m_sequenceEnabled ? m_sequence->activeFields() : m_namedFields;
                if (fields && key.startsWith(QLatin1String("mapped:")))
                    fields->selectField(key.mid(7));
            } else if (auto *editor = m_frameControls.value(key)) {
                if (m_frameTabs->isHidden())
                    return;
                const bool options = key == QLatin1String("length") || key == QLatin1String("checksum");
                m_frameTabs->setCurrentItem(options ? QStringLiteral("options") : QStringLiteral("fields"));
                editor->setFocus();
            } else if (!editsText() && key.startsWith(QLatin1String("field:"))) {
                const int row = key.mid(6).toInt();
                m_frameTabs->setCurrentItem(QStringLiteral("fields"));
                m_table->setCurrentCell(row, sourceColumn());
                m_table->scrollTo(m_table->currentIndex());
            }
        });
    }

    void addByteControl(const QString &part, QWidget *control)
    {
        control->setProperty("framePart", part);
        if (!m_frameControls.contains(part))
            m_frameControls.insert(part, control);
    }

    bool editsText() const { return m_editors->currentWidget() == m_expression; }
    int sourceColumn() const { return m_signalNames.isEmpty() ? 0 : 1; }
    int contentColumn() const { return sourceColumn() + 1; }

    QList<CommandEncoder::CommandInput> availableInputs() const
    {
        if (!m_inputEditor->rows().isEmpty())
            return m_inputEditor->rows();
        const QString rawType = m_properties.value(QStringLiteral("dataType")).toString(
            CommandEncoder::hexDataTypes().first());
        return {{{}, m_editedSettings.value(QStringLiteral("dataType")).toString(rawType), QStringLiteral("value")}};
    }

    QString inputLabel(const CommandEncoder::CommandInput &input) const
    {
        if (input.inputId.isEmpty())
            return fhmi::tr("发送内容") + QStringLiteral(" · {value}");
        const QString title = m_samples.value(input.inputId).title;
        const QString token = CommandEncoder::inputToken(input.variable);
        return title.isEmpty() ? token + QStringLiteral(" · ") + input.inputId : title + QStringLiteral(" · ") + token;
    }

    void rebuildVariableMenu()
    {
        auto *oldMenu = m_insert->menu();
        auto *menu = new FluentQt::RoundMenu(m_insert);
        menu->setObjectName(QStringLiteral("frameTemplateVariables"));
        for (const auto &input : availableInputs()) {
            auto *action = new QAction(inputLabel(input), menu);
            const QString token = CommandEncoder::inputToken(input.variable);
            action->setData(token);
            menu->addAction(action);
            connect(action, &QAction::triggered, this, [this, token] {
                m_expression->insertPlainText(token);
                m_expression->setFocus();
            });
        }
        m_insert->setMenu(menu);
        if (oldMenu)
            oldMenu->deleteLater();
    }

    void refreshSamples()
    {
        m_samples = m_inputEditor->values();
        const auto &inputs = m_inputEditor->rows();
        for (qsizetype i = 0; i < inputs.size(); ++i) {
            auto sample = m_samples.find(inputs.at(i).inputId);
            if (sample != m_samples.end() && sample->text().trimmed().isEmpty())
                sample->value = QString::number(m_byteFrame || m_mode == InputListMode::Can ? 0 : i + 1);
            m_linkedVariables.insert(inputs.at(i).variable);
        }
    }

    QJsonObject draft() const
    {
        auto sample = m_properties;
        const auto edited = changes();
        for (auto it = edited.begin(); it != edited.end(); ++it)
            sample.insert(it.key(), it.value());
        return sample;
    }

    QString generated() const
    {
        const QString text = defaultFrameTemplate(draft(), m_sources, m_inputEditor->values());
        return text.isEmpty() ? QStringLiteral("{value}") : text;
    }

    void inputChanged(const QString &previous, const QString &name)
    {
        const bool firstLink = !m_hasInputs && !m_inputEditor->rows().isEmpty();
        m_hasInputs = !m_inputEditor->rows().isEmpty();
        refreshSamples();
        m_staticSettings->setVisible(!m_hasInputs && !editsFixedCan());
        if (!m_expression) {
            refreshPreview();
            return;
        }
        const QString current = templateText();
        const QString next = generated();
        QString edited = current;
        if (current == m_generated || current.trimmed().isEmpty()) {
            edited = next;
        } else {
            const QString from = firstLink ? QStringLiteral("value") : previous;
            const QString to = firstLink ? m_inputEditor->rows().first().variable : name;
            if (!from.isEmpty() && CommandEncoder::validInputVariable(to)) {
                edited.clear();
                for (const auto &part : CommandEncoder::templateParts(current))
                    edited += part.isInput ? CommandEncoder::inputToken(part.text == from ? to : part.text) : part.text;
            }
        }
        m_generated = next;
        rebuildVariableMenu();
        setTemplate(edited);
    }

    void buildFieldTable()
    {
        m_table = new FluentQt::TableWidget(m_editors);
        m_table->setObjectName(QStringLiteral("frameTemplateTable"));
        QStringList columns;
        if (m_byteFrame)
            columns = {fhmi::tr("字段来源"), fhmi::tr("字节 / 类型")};
        else
            columns = {fhmi::tr("信号"), fhmi::tr("值来源"), fhmi::tr("固定值 / 示例")};
        m_table->setColumnCount(columns.size());
        m_table->setHorizontalHeaderLabels(columns);
        m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
        m_table->setSelectionMode(QAbstractItemView::SingleSelection);
        m_table->setShowGrid(false);
        m_table->setBorderVisible(true);
        m_table->setBorderRadius(6);
        m_table->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
        m_table->verticalHeader()->setDefaultSectionSize(38);
        m_table->verticalHeader()->hide();
        m_editors->addWidget(m_table);
        m_editors->setCurrentWidget(m_table);
        connect(qApp, &QApplication::focusChanged, this, [this](QWidget *, QWidget *focused) {
            if (focused && m_table->viewport()->isAncestorOf(focused)) {
                const auto index = m_table->indexAt(focused->mapTo(m_table->viewport(), focused->rect().center()));
                if (index.isValid())
                    m_table->setCurrentCell(index.row(), index.column());
            }
        });
    }

    QWidget *buildFieldActions()
    {
        m_fieldActions = new QWidget(this);
        auto *actions = new QHBoxLayout(m_fieldActions);
        actions->setContentsMargins(0, 0, 0, 0);
        actions->setSpacing(4);
        auto *addLiteral = new FluentQt::PushButton(m_byteFrame ? fhmi::tr("添加固定段") : fhmi::tr("添加固定值"),
                                                    m_fieldActions);
        addLiteral->setObjectName(QStringLiteral("addFrameLiteral"));
        auto *addInput = new FluentQt::PushButton(fhmi::tr("添加输入项"), m_fieldActions);
        const auto tool = [this, actions](FluentQt::FluentIcon icon, const QString &label) {
            auto *button = new FluentQt::TransparentToolButton(FluentQt::icon(icon), m_fieldActions);
            button->setAccessibleName(label);
            button->setToolTip(label);
            actions->addWidget(button);
            return button;
        };
        actions->addWidget(addLiteral);
        actions->addWidget(addInput);
        actions->addSpacing(4);
        auto *up = tool(FluentQt::FluentIcon::Up, fhmi::tr("上移"));
        auto *down = tool(FluentQt::FluentIcon::Down, fhmi::tr("下移"));
        auto *remove = tool(FluentQt::FluentIcon::Delete, fhmi::tr("删除所选"));
        const auto updateActions = [this, up, down, remove] {
            const int row = m_table->currentRow();
            up->setEnabled(row > 0);
            down->setEnabled(row >= 0 && row + 1 < m_table->rowCount());
            remove->setEnabled(row >= 0);
        };
        connect(m_table, &QTableWidget::currentCellChanged, this, updateActions);
        connect(addLiteral, &QPushButton::clicked, this, [this] {
            const int row = appendField({});
            m_table->setCurrentCell(row, contentColumn());
            literal(row)->setFocus();
            refreshPreview();
        });
        connect(addInput, &QPushButton::clicked, this, [this] {
            QSet<QString> used;
            for (int row = 0; row < m_table->rowCount(); ++row)
                used.insert(selectedValue(combo(row, sourceColumn())));
            const auto inputs = availableInputs();
            QString name = inputs.first().variable;
            for (const auto &input : inputs) {
                if (!used.contains(input.variable)) {
                    name = input.variable;
                    break;
                }
            }
            m_table->setCurrentCell(appendField({{}, name, {}}), sourceColumn());
            refreshPreview();
        });
        connect(up, &QToolButton::clicked, this, [this] { moveField(-1); });
        connect(down, &QToolButton::clicked, this, [this] { moveField(1); });
        connect(remove, &QToolButton::clicked, this, [this, updateActions] {
            if (m_table->currentRow() >= 0)
                m_table->removeRow(m_table->currentRow());
            updateActions();
            refreshPreview();
        });
        updateActions();
        return m_fieldActions;
    }

    QString templateText() const
    {
        if (editsText())
            return m_expression->toPlainText();
        QStringList parts;
        for (const auto &field : fields()) {
            QString value = field.variable.isEmpty() ? field.content : CommandEncoder::inputToken(field.variable);
            if (m_byteFrame && field.variable.isEmpty()) {
                if (const auto bytes = parseHex(value))
                    value = toHexString(*bytes, true);
            }
            parts.append(field.signal.isEmpty() ? value : field.signal + u'=' + value);
        }
        return parts.join(m_byteFrame ? u' ' : u',');
    }

    void setTemplate(const QString &text)
    {
        if (editsText()) {
            const QSignalBlocker blocker(m_expression);
            m_expression->setPlainText(text);
        } else {
            showFields(frameFields(text, !m_signalNames.isEmpty()));
        }
        refreshPreview();
    }

    QWidget *buildFrameOptions(QWidget *parent)
    {
        const auto header = m_mode == InputListMode::Hex
            ? parseHex(m_properties.value(QStringLiteral("frameHeader")).toString()) : std::nullopt;
        return createTransmitFrameRules(parent, m_properties.value(QStringLiteral("txFrame")).toObject(),
            header ? int(header->size()) : 0, [this](const QJsonObject &config) {
                m_editedSettings.insert(QStringLiteral("txFrame"), config);
                refreshPreview();
            }, [this](const QString &kind, QWidget *control) { addByteControl(kind, control); });
    }

    void addSetting(QHBoxLayout *layout, const CanvasItem &item, const QString &name, const QString &caption = {})
    {
        const auto *definition = item.meta().prop(name);
        const bool header = name == QLatin1String("frameHeader");
        const bool footer = name == QLatin1String("frameTail");
        if (header || footer)
            layout->addWidget(frameByteMarker(this, header ? FrameByteKind::Header : FrameByteKind::Footer));
        auto *label = new FluentQt::BodyLabel(caption.isEmpty() ? translated(definition->label) : caption, this);
        layout->addWidget(label);
        const auto update = [this, name](const QString &text) {
            m_editedSettings.insert(name, text);
            if (name == QLatin1String("dataType") && m_table && !editsText())
                setTemplate(templateText());
            else
                refreshPreview();
        };
        const QString current = item.propertyValue(name).toVariant().toString();
        if (definition->options.isEmpty()) {
            auto *edit = new FluentQt::LineEdit(this);
            edit->setObjectName(QStringLiteral("frameTemplate_%1").arg(name));
            edit->setText(current);
            edit->setMaximumWidth(154);
            if (header || footer)
                addByteControl(header ? QStringLiteral("header") : QStringLiteral("footer"), edit);
            label->setBuddy(edit);
            layout->addWidget(edit, 1);
            connect(edit, &QLineEdit::textChanged, this, update);
        } else {
            auto *box = new FluentQt::ComboBox(this);
            addValueItems(box, definition->options);
            selectValue(box, current);
            box->setMinimumWidth(108);
            label->setBuddy(box);
            layout->addWidget(box);
            connect(box, &FluentQt::ComboBox::currentIndexChanged, this, [box, update] { update(selectedValue(box)); });
        }
    }

    FluentQt::ComboBox *combo(int row, int column) const
    {
        return m_table->cellWidget(row, column)->findChild<FluentQt::ComboBox *>();
    }

    QLineEdit *literal(int row) const
    {
        return m_table->cellWidget(row, contentColumn())->findChild<QLineEdit *>();
    }

    void setCellEditor(int row, int column, QWidget *editor, QLabel *marker = nullptr)
    {
        auto *cell = new QWidget(m_table);
        auto *layout = new QHBoxLayout(cell);
        layout->setContentsMargins(6, 2, 6, 2);
        if (marker)
            layout->addWidget(marker);
        layout->addWidget(editor);
        m_table->setCellWidget(row, column, cell);
    }

    QList<FrameField> fields() const
    {
        QList<FrameField> result;
        for (int row = 0; row < m_table->rowCount(); ++row)
            result.append({m_signalNames.isEmpty() ? QString() : selectedValue(combo(row, 0)),
                           selectedValue(combo(row, sourceColumn())), literal(row)->text()});
        return result;
    }

    int appendField(const FrameField &field)
    {
        const int row = m_table->rowCount();
        m_table->insertRow(row);
        const auto selectCell = [this](QWidget *editor, int column) {
            const int index = m_table->rowAt(editor->mapTo(m_table->viewport(), editor->rect().center()).y());
            if (index >= 0)
                m_table->setCurrentCell(index, column);
        };
        if (!m_signalNames.isEmpty()) {
            auto *signal = new FluentQt::ComboBox(m_table);
            signal->setAccessibleName(fhmi::tr("信号"));
            signal->setPlaceholderText(fhmi::tr("选择 CAN 信号"));
            for (const auto &name : m_signalNames)
                signal->addItem(name, {}, name);
            if (!field.signal.isEmpty() && !m_signalNames.contains(field.signal))
                signal->addItem(field.signal, {}, field.signal);
            if (!selectValue(signal, field.signal))
                signal->setCurrentIndex(-1);
            setCellEditor(row, 0, signal);
            connect(signal, &FluentQt::ComboBox::currentIndexChanged, this, [this, signal, selectCell] {
                selectCell(signal, 0);
                refreshPreview();
            });
        }
        auto *source = new FluentQt::ComboBox(m_table);
        source->setAccessibleName(fhmi::tr("字段来源"));
        source->addItem(fhmi::tr("固定内容"));
        for (const auto &input : availableInputs())
            source->addItem(inputLabel(input), {}, input.variable);
        if (!field.variable.isEmpty() && !selectValue(source, field.variable)) {
            source->addItem(CommandEncoder::inputToken(field.variable), {}, field.variable);
            selectValue(source, field.variable);
        }
        auto *marker = m_byteFrame ? frameByteMarker(m_table, FrameByteKind::Field, row) : nullptr;
        setCellEditor(row, sourceColumn(), source, marker);
        source->setProperty("framePart", frameFieldKey(row));
        auto *stack = new QStackedWidget(m_table);
        auto *content = new FluentQt::LineEdit(stack);
        content->setProperty("framePart", frameFieldKey(row));
        content->setAccessibleName(fhmi::tr("固定内容"));
        content->setText(field.content);
        if (m_byteFrame)
            content->setPlaceholderText(fhmi::tr("例如 AA 55"));
        auto *inputDetail = new FluentQt::BodyLabel(stack);
        inputDetail->setTextFormat(Qt::PlainText);
        inputDetail->setContentsMargins(4, 0, 4, 0);
        stack->addWidget(content);
        stack->addWidget(inputDetail);
        setCellEditor(row, contentColumn(), stack);
        const auto showContent = [this, source, stack, content, inputDetail, marker, row] {
            const QString variable = selectedValue(source);
            if (marker)
                setFrameByteMarker(marker, variable.isEmpty() ? FrameByteKind::Fixed : FrameByteKind::Field, row);
            if (variable.isEmpty()) {
                stack->setCurrentWidget(content);
                return;
            }
            const auto inputs = availableInputs();
            const auto input = std::find_if(inputs.begin(), inputs.end(), [&](const auto &entry) {
                return entry.variable == variable;
            });
            if (input == inputs.end())
                inputDetail->setText(CommandEncoder::inputToken(variable));
            else
                inputDetail->setText(m_byteFrame ? translated(input->dataType) : m_samples.value(input->inputId).text());
            stack->setCurrentWidget(inputDetail);
        };
        showContent();
        connect(source, &FluentQt::ComboBox::currentIndexChanged, this, [this, source, selectCell, showContent] {
            showContent();
            selectCell(source, sourceColumn());
            refreshPreview();
        });
        connect(content, &QLineEdit::textChanged, this, [this, content, selectCell] {
            selectCell(content, contentColumn());
            refreshPreview();
        });
        return row;
    }

    void showFields(const QList<FrameField> &fields)
    {
        m_table->setRowCount(0);
        for (const auto &field : fields)
            appendField(field);
        m_table->setCurrentCell(0, sourceColumn());
    }

    void moveField(int offset)
    {
        const int row = m_table->currentRow();
        const int next = row + offset;
        if (row < 0 || next < 0 || next >= m_table->rowCount())
            return;
        auto current = fields();
        current.swapItemsAt(row, next);
        showFields(current);
        m_table->setCurrentCell(next, sourceColumn());
        refreshPreview();
    }

    void refreshPreview()
    {
        const auto configured = draft();
        QString pointProblem;
        const auto effective = resolvePointWrite(configured, m_project.logic, m_project.widgets, m_project.parserType(),
                                                 &pointProblem, u"PushButton", ModbusParserChoice::OnConnect);
        const auto props = effective.value_or(configured);
        const auto pointName = configured.value(writePointKey(configured.value(QStringLiteral("commandMode")).toString())).toString();
        if (m_manualAddress)
            m_manualAddress->setVisible(pointName.isEmpty());
        const bool shared = FrameTemplates::sendSettings(m_project.frameTemplates, props).has_value();
        if (m_payloadHint)
            m_payloadHint->setVisible(!m_sequenceEnabled && shared && !usesMappedFields());
        if (m_manualFraming)
            m_manualFraming->setVisible(!shared);
        if (m_byteOrderSetting) {
            const auto &inputs = m_inputEditor->rows();
            const bool needsOrder = inputs.isEmpty()
                ? HexOptions::fieldSize(props.value(QStringLiteral("dataType")).toString()) > 1
                : std::any_of(inputs.cbegin(), inputs.cend(), [](const auto &input) {
                      return HexOptions::fieldSize(input.dataType) > 1;
                  });
            m_byteOrderSetting->setVisible(needsOrder);
        }
        if (m_frameTabs) {
            if (shared)
                m_frameTabs->setCurrentItem(QStringLiteral("fields"));
            m_frameTabs->setVisible(!shared);
        }
        const auto target = effective ? previewCommandTarget(props, QStringLiteral("PushButton"), m_project)
                                      : CommandTargetPreview{{}, {}, pointProblem, true};
        m_target->setText(!target.error.isEmpty() ? target.error
            : pointName.isEmpty() ? target.label
            : pointName + QStringLiteral(" · ") + writePointTargetText(m_project, m_project.widgets, props, pointName));
        if (target.error.isEmpty())
            m_target->setTextColor(QColor(0x404040), QColor(0xD0D0D0));
        else
            m_target->setTextColor(QColor(0xC42B1C), QColor(0xFF99A4));
        if (m_registerOrder) {
            m_inputEditor->setRegisterAddress(props.value(QStringLiteral("mbAddress")).toInt());
            const auto &rows = m_inputEditor->rows();
            m_registerOrder->setVisible(pointName.isEmpty() && (rows.isEmpty()
                ? CommandEncoder::usesRegisterOrder(props.value(QStringLiteral("mbDataType")).toString())
                : std::any_of(rows.cbegin(), rows.cend(), [](const auto &row) {
                    return CommandEncoder::usesRegisterOrder(row.dataType);
                })));
        }
        BinaryFrameExample example;
        QString preview;
        if (!target.routeInvalid) {
            if (m_sequenceEnabled) {
                if (!m_sequence->frames().isEmpty())
                    preview = previewInputCommand(props, m_sources, m_sequence->sampleValues(), false, nullptr,
                                                  m_project.frameTemplates);
                if (const auto *fields = m_sequence->activeFields()) {
                    auto frame = m_sequence->activeFrame();
                    frame.insert(QStringLiteral("commandMode"), QStringLiteral("HEX"));
                    frame.insert(FrameTemplates::SendFieldsEnabled, true);
                    previewInputCommand(frame, {}, fields->sampleValues(), false, &example, m_project.frameTemplates);
                    if (const auto entry = FrameTemplates::find(m_project.frameTemplates, frame.value(FrameTemplates::SendId).toString()))
                        example.caption = entry->name;
                }
            } else {
                preview = previewInputCommand(props, m_sources, usesMappedFields() ? m_namedFields->sampleValues() : m_samples,
                                              false, m_byteView ? &example : nullptr, m_project.frameTemplates);
            }
        }
        m_preview->setPlainText(preview);
        if (m_sequence) {
            const int rows = int(std::clamp<qsizetype>(props.value(FrameTemplates::SendSequence).toArray().size(), 1, 4));
            m_preview->setFixedHeight(m_sequenceEnabled ? rows * m_preview->fontMetrics().lineSpacing() + 22 : 52);
        }
        if (!m_byteView)
            return;
        const bool hasBytes = !example.bytes.isEmpty();
        m_preview->setVisible(m_sequenceEnabled ? !preview.isEmpty() : !hasBytes);
        m_diagramControls->setVisible(hasBytes);
        m_frameDiagram->setVisible(hasBytes);
        m_byteInfo->setVisible(hasBytes);
        m_copyBytes->setVisible(hasBytes);
        const auto &layout = example.layout.frame;
        m_frameCoverage->setFrame(example.bytes.left(example.applicationSize), layout);
        if (hasBytes) {
            const QString size = fhmi::tr("%1 字节").arg(example.bytes.size());
            m_byteInfo->setText(example.caption.isEmpty() ? size : example.caption + QStringLiteral(" · ") + size);
            m_byteView->setFrame(example.bytes, previewSpans(example), layout, example.applicationSize);
        }
    }

    QList<FrameByteSpan> previewSpans(const BinaryFrameExample &example) const
    {
        const auto &layout = example.layout.frame;
        auto spans = frameEnvelopeSpans(layout, example.applicationSize);
        if (example.bytes.size() > example.applicationSize)
            spans.append({{}, fhmi::tr("补齐"), example.applicationSize, example.bytes.size() - example.applicationSize,
                          FrameByteKind::Padding});
        if (usesMappedFields() || m_sequenceEnabled) {
            const auto *fields = m_sequenceEnabled ? m_sequence->activeFields() : m_namedFields;
            for (const auto &field : example.layout.fields)
                spans.append({QStringLiteral("mapped:") + field.name, field.name, field.offset, field.size,
                              FrameByteKind::Field, fields->fieldRow(field.name)});
            return spans;
        }

        const auto &encoded = example.layout.partSizes;
        const auto parts = CommandEncoder::templateParts(templateText());
        if (parts.size() != encoded.size())
            return spans;
        QList<QString> variables;
        QList<qsizetype> sizes;
        if (editsText()) {
            for (const auto &part : parts)
                variables.append(part.isInput ? part.text : QString());
            sizes = encoded;
        } else {
            QHash<QString, qsizetype> inputSizes;
            for (qsizetype i = 0; i < parts.size(); ++i)
                if (parts.at(i).isInput)
                    inputSizes.insert(parts.at(i).text, encoded.at(i));
            for (const auto &field : fields()) {
                variables.append(field.variable);
                sizes.append(field.variable.isEmpty() ? parseHexOrEmpty(field.content).size()
                                                      : inputSizes.value(field.variable));
            }
        }
        const qsizetype bodySize = example.applicationSize - layout.headerSize - layout.lengthSize -
                                   layout.checksumSize - layout.footerSize;
        if (std::accumulate(sizes.begin(), sizes.end(), qsizetype(0)) != bodySize)
            return spans;

        qsizetype offset = layout.headerSize;
        for (int row = 0; row < sizes.size(); ++row) {
            if (sizes.at(row) == 0)
                continue;
            const QString &variable = variables.at(row);
            const auto kind = variable.isEmpty() ? FrameByteKind::Fixed : FrameByteKind::Field;
            const QString label = variable.isEmpty() ? fhmi::tr("固定") : CommandEncoder::inputToken(variable);
            const auto add = [&](qsizetype at, qsizetype length) {
                spans.append({frameFieldKey(row), label, at, length, kind, row});
            };
            qsizetype remaining = sizes.at(row);
            if (offset <= layout.lengthOffset && layout.lengthOffset < offset + remaining) {
                const qsizetype prefix = layout.lengthOffset - offset;
                if (prefix > 0)
                    add(offset, prefix);
                remaining -= prefix;
                offset = layout.lengthOffset + layout.lengthSize;
            }
            add(offset, remaining);
            offset += remaining;
        }
        return spans;
    }

    const QJsonObject m_properties;
    const Project m_project;
    const QList<CanPreviewSource> m_sources;
    const QStringList m_signalNames;
    const InputListMode m_mode;
    const bool m_byteFrame;
    InputValues m_samples;
    CommandInputsEditor *m_inputEditor = nullptr;
    FrameFieldInputsEditor *m_namedFields = nullptr;
    FrameSequenceEditor *m_sequence = nullptr;
    bool m_sequenceEnabled = false;
    QWidget *m_payloadChoices = nullptr;
    QWidget *m_manualAddress = nullptr;
    QStackedWidget *m_payloadEditors = nullptr;
    FluentQt::SegmentedWidget *m_payloadMode = nullptr;
    FluentQt::BodyLabel *m_payloadModeLabel = nullptr;
    FluentQt::CaptionLabel *m_payloadHint = nullptr;
    bool m_payloadModeChosen = false;
    QWidget *m_staticSettings = nullptr;
    QWidget *m_registerOrder = nullptr;
    QWidget *m_manualFraming = nullptr;
    QWidget *m_byteOrderSetting = nullptr;
    FluentQt::DropDownPushButton *m_insert = nullptr;
    QString m_generated;
    bool m_hasInputs = false;
    QSet<QString> m_linkedVariables;
    QStackedWidget *m_editors = nullptr;
    FluentQt::SegmentedWidget *m_frameTabs = nullptr;
    QMap<QString, QWidget *> m_frameControls;
    FluentQt::PlainTextEdit *m_expression = nullptr;
    QWidget *m_fieldActions = nullptr;
    FluentQt::TableWidget *m_table = nullptr;
    FluentQt::PlainTextEdit *m_preview = nullptr;
    FluentQt::CaptionLabel *m_target = nullptr;
    FrameBytesView *m_byteView = nullptr;
    FrameDiagram *m_frameDiagram = nullptr;
    QWidget *m_diagramControls = nullptr;
    FrameCoveragePreview *m_frameCoverage = nullptr;
    FluentQt::CaptionLabel *m_byteInfo = nullptr;
    FluentQt::TransparentToolButton *m_copyBytes = nullptr;
    QJsonObject m_editedSettings;
};

} // namespace

QString defaultFrameTemplate(const QJsonObject &properties, const QList<CanPreviewSource> &canSources,
                             const InputValues &inputs)
{
    const auto mode = CommandEncoder::inputListMode(properties);
    if (!CommandEncoder::usesTemplate(mode))
        return {};
    const QStringList names = signalNames(properties, canSources);
    QStringList parts;
    QSet<QString> assigned;
    for (const auto &input : CommandEncoder::commandInputs(properties)) {
        const QString token = CommandEncoder::inputToken(input.variable);
        if (mode == InputListMode::Json) {
            parts.append(u'"' + input.variable + QStringLiteral("\":") + token);
        } else if (mode == InputListMode::Text) {
            parts.append(input.variable + u'=' + token);
        } else if (names.isEmpty()) {
            parts.append(token);
        } else {
            QString signal;
            for (const auto &name : {input.variable, inputs.value(input.inputId).title, input.inputId}) {
                if (names.contains(name) && !assigned.contains(name)) {
                    signal = name;
                    assigned.insert(name);
                    break;
                }
            }
            parts.append(signal.isEmpty() ? token : signal + u'=' + token);
        }
    }
    if (mode == InputListMode::Json && !parts.isEmpty())
        return u'{' + parts.join(u',') + u'}';
    return parts.join(mode == InputListMode::Text || !names.isEmpty() ? u',' : u' ');
}

void openFrameTemplateEditor(const CanvasItem &item, const Project &project, QList<CanPreviewSource> canSources, InputValues inputs,
                             bool canCreate, QWidget *parent,
                             std::function<void(const QJsonObject &, const QMap<QString, QString> &)> apply)
{
    auto *dialog = new FrameTemplateEditor(item, project, std::move(canSources), std::move(inputs), canCreate, parent);
    QObject::connect(dialog, &QDialog::accepted, dialog, [dialog, apply = std::move(apply)] {
        apply(dialog->changes(), dialog->pendingInputs());
    });
    dialog->open();
}

} // namespace fhmi
