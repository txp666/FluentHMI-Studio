#include "designer/receive_frame_editor.h"

#include "core/frame_templates.h"
#include "core/i18n.h"
#include "core/industrial_catalog.h"
#include "designer/receive_frame_fields.h"
#include "runtime/app_dialog.h"
#include "runtime/ui_helpers.h"

#include <FluentQtWidgets/Navigation/SegmentedWidget.h>
#include <FluentQtWidgets/Widgets/Button.h>
#include <FluentQtWidgets/Widgets/ComboBox.h>
#include <FluentQtWidgets/Widgets/Label.h>
#include <FluentQtWidgets/Widgets/LineEdit.h>
#include <FluentQtWidgets/Widgets/ScrollArea.h>
#include <FluentQtWidgets/Widgets/Selection.h>
#include <FluentQtWidgets/Widgets/SpinBox.h>
#include <QtCore/QCoreApplication>
#include <QtCore/QJsonArray>
#include <QtCore/QScopedValueRollback>
#include <QtCore/QSet>
#include <QtGui/QResizeEvent>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QSplitter>
#include <QtWidgets/QStackedWidget>
#include <QtWidgets/QVBoxLayout>
#include <FluentQtWidgets/Layout/FlowLayout.h>

#include <algorithm>

namespace fhmi {
namespace {
QString tr(const char *s) { return QCoreApplication::translate("fhmi", s); }

class ReceiveEditorSplitter : public QSplitter
{
  public:
    using QSplitter::QSplitter;

  protected:
    void resizeEvent(QResizeEvent *event) override
    {
        const auto direction = event->size().width() < 720 ? Qt::Vertical : Qt::Horizontal;
        if (direction != orientation()) {
            setOrientation(direction);
            const bool stacked = direction == Qt::Vertical;
            const int extent = stacked ? event->size().height() : event->size().width();
            const int previewExtent = stacked ? extent * 3 / 4 : extent * 2 / 3;
            setSizes({previewExtent, extent - previewExtent});
        }
        QSplitter::resizeEvent(event);
    }
};

class BindingForm : public QWidget
{
  public:
    BindingForm(const ReceiveValueSettings &valueSettings, bool editsTemplate, QWidget *parent)
        : QWidget(parent), m_typeName(valueSettings.typeName)
    {
        const auto &binding = valueSettings.binding;
        m_form = new QFormLayout(this);
        m_form->setContentsMargins(8, 10, 8, 0);
        m_form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        m_mode = new FluentQt::ComboBox(this);
        m_mode->setObjectName(QStringLiteral("receiveValueMode"));
        m_mode->addItem(editsTemplate ? fhmi::tr("按模板字段取值") : fhmi::tr("按字段取值"), {}, ParseMode::KeyValue);
        m_mode->addItem(translated(ParseMode::JsonPath), {}, ParseMode::JsonPath);
        if (supportsBindingMode(m_typeName, ParseMode::HexBytes))
            m_mode->addItem(fhmi::tr("手动按字节取值"), {}, ParseMode::HexBytes);
        selectValue(m_mode, binding.value(QStringLiteral("parseMode")).toString(ParseMode::KeyValue));
        m_form->addRow(fhmi::tr("取值方式"), m_mode);

        m_field = new FluentQt::EditableComboBox(this);
        m_field->setObjectName(QStringLiteral("receiveValueField"));
        m_field->setCurrentText(binding.value(QStringLiteral("key")).toString());
        m_field->setPlaceholderText(fhmi::tr("选择或填写字段名"));
        m_form->addRow(fhmi::tr("取值字段"), m_field);
        m_path = addText(binding, QStringLiteral("jsonPath"), QStringLiteral("receiveValuePath"),
                         translated(ParseMode::JsonPath), QStringLiteral("data.speed"));

        m_offset = new FluentQt::SpinBox(this);
        m_offset->setObjectName(QStringLiteral("receiveValueOffset"));
        m_offset->setRange(0, 9999);
        m_offset->setValue(binding.value(QStringLiteral("byteOffset")).toInt());
        m_form->addRow(fhmi::tr("字节偏移"), m_offset);
        m_type = new FluentQt::ComboBox(this);
        m_type->setObjectName(QStringLiteral("receiveValueType"));
        addValueItems(m_type, rawBindingDataTypes());
        selectValue(m_type, binding.value(QStringLiteral("dataType")).toString(QStringLiteral("uint16")));
        m_form->addRow(fhmi::tr("数据类型"), m_type);
        m_order = new FluentQt::ComboBox(this);
        m_order->setObjectName(QStringLiteral("receiveValueOrder"));
        addValueItems(m_order, HexOptions::byteOrders());
        selectValue(m_order, binding.value(QStringLiteral("byteOrder")).toString(HexOptions::byteOrders().first()));
        m_form->addRow(fhmi::tr("字节序"), m_order);
        m_header = addText(binding, QStringLiteral("frameHeader"), QStringLiteral("receiveValueHeader"),
                           fhmi::tr("剥除帧头"), fhmi::tr("如 AA 55"));
        m_tail = addText(binding, QStringLiteral("frameTail"), QStringLiteral("receiveValueTail"),
                         fhmi::tr("剥除帧尾"), fhmi::tr("如 0D 0A"));
        m_offsetHint = new FluentQt::CaptionLabel(fhmi::tr("偏移从剥除帧头后的第 0 字节开始。"), this);
        m_offsetHint->setWordWrap(true);
        m_form->addRow(m_offsetHint);
        m_expression = addText(binding, QStringLiteral("expression"), QStringLiteral("receiveValueExpression"),
                               fhmi::tr("转换表达式"), fhmi::tr("如 value * 0.1"));
        m_form->setRowVisible(m_expression, !industrialCapabilities(m_typeName).structuredInput);
        m_initial = values();
        updateRows();
    }

    QString parseMode() const { return selectedValue(m_mode); }
    bool readsBytes() const { return parseMode() == ParseMode::HexBytes; }

    void onChanged(const std::function<void()> &refresh)
    {
        const auto changed = [this, refresh] {
            if (m_updating)
                return;
            updateRows();
            refresh();
        };
        for (auto *choice : {m_mode, m_type, m_order})
            connect(choice, &FluentQt::ComboBox::currentIndexChanged, this, changed);
        connect(m_field, &FluentQt::EditableComboBox::currentTextChanged, this, changed);
        connect(m_offset, qOverload<int>(&QSpinBox::valueChanged), this, changed);
        for (auto *line : {m_path, m_header, m_tail, m_expression})
            connect(line, &QLineEdit::textChanged, this, changed);
    }

    void setFields(const QStringList &fields)
    {
        if (fields == m_fields)
            return;
        const QScopedValueRollback guard(m_updating, true);
        const auto value = m_field->text();
        m_fields = fields;
        m_field->clear();
        m_field->addItems(m_fields);
        m_field->setCurrentText(value);
    }

    void useField(const ReceivedField &field)
    {
        const QScopedValueRollback guard(m_updating, true);
        selectValue(m_mode, field.jsonPath ? ParseMode::JsonPath : ParseMode::KeyValue);
        if (field.jsonPath)
            m_path->setText(field.key);
        else
            m_field->setCurrentText(field.key);
        updateRows();
    }

    void renameField(const QString &from, const QString &to)
    {
        if (m_field->text().trimmed() == from) {
            const QScopedValueRollback guard(m_updating, true);
            m_field->setCurrentText(to);
        }
    }

    std::optional<BindingEntry> entry() const
    {
        return BindingEntry::fromWidget({{QStringLiteral("type"), m_typeName},
                                         {QStringLiteral("binding"), values()}});
    }

    QJsonObject changes() const
    {
        const auto current = values();
        QJsonObject result;
        for (auto it = current.cbegin(); it != current.cend(); ++it)
            if (m_initial.value(it.key()) != it.value())
                result.insert(it.key(), it.value());
        return result;
    }

    QString problem() const
    {
        if (readsBytes() && !changes().isEmpty() &&
            (!parseHex(m_header->text()) || !parseHex(m_tail->text())))
            return fhmi::tr("手动取值的帧头或帧尾不是有效的 HEX");
        return {};
    }

  private:
    FluentQt::LineEdit *addText(const QJsonObject &binding, const QString &key, const QString &name,
                               const QString &label, const QString &placeholder)
    {
        auto *input = new FluentQt::LineEdit(this);
        input->setObjectName(name);
        input->setText(binding.value(key).toString());
        input->setPlaceholderText(placeholder);
        m_form->addRow(label, input);
        return input;
    }

    void updateRows()
    {
        const bool raw = readsBytes();
        m_form->setRowVisible(m_field, parseMode() == ParseMode::KeyValue);
        m_form->setRowVisible(m_path, parseMode() == ParseMode::JsonPath);
        for (QWidget *control : QList<QWidget *>{m_offset, m_type, m_header, m_tail})
            m_form->setRowVisible(control, raw);
        m_form->setRowVisible(m_order, raw && HexOptions::fieldSize(selectedValue(m_type)) > 1);
        m_form->setRowVisible(m_offsetHint, raw && !m_header->text().trimmed().isEmpty());
    }

    QJsonObject values() const
    {
        return {{QStringLiteral("parseMode"), parseMode()}, {QStringLiteral("key"), m_field->text().trimmed()},
                {QStringLiteral("jsonPath"), m_path->text().trimmed()}, {QStringLiteral("byteOffset"), m_offset->value()},
                {QStringLiteral("dataType"), selectedValue(m_type)}, {QStringLiteral("byteOrder"), selectedValue(m_order)},
                {QStringLiteral("frameHeader"), m_header->text().trimmed()},
                {QStringLiteral("frameTail"), m_tail->text().trimmed()},
                {QStringLiteral("expression"), m_expression->text().trimmed()}};
    }

    QFormLayout *m_form;
    const QString m_typeName;
    FluentQt::ComboBox *m_mode;
    FluentQt::EditableComboBox *m_field;
    FluentQt::LineEdit *m_path;
    FluentQt::SpinBox *m_offset;
    FluentQt::ComboBox *m_type;
    FluentQt::ComboBox *m_order;
    FluentQt::LineEdit *m_header;
    FluentQt::LineEdit *m_tail;
    FluentQt::CaptionLabel *m_offsetHint;
    FluentQt::LineEdit *m_expression;
    QStringList m_fields;
    QJsonObject m_initial;
    bool m_updating = false;
};

void setSendDefaults(QJsonObject &config, const QJsonObject &defaults)
{
    if (defaults.isEmpty())
        config.remove(FrameTemplates::SendDefaults);
    else
        config.insert(FrameTemplates::SendDefaults, defaults);
}

void setSendRequired(QJsonObject &config, const QStringList &fields)
{
    if (fields.isEmpty())
        config.remove(FrameTemplates::SendRequired);
    else
        config.insert(FrameTemplates::SendRequired, QJsonArray::fromStringList(fields));
}

std::optional<QJsonObject> editField(const QJsonObject &settings, const QJsonArray &field, int edited, bool editsTemplate,
                                     QWidget *parent)
{
    const auto allFields = settings.value(QStringLiteral("fields")).toArray();
    const auto title = editsTemplate ? (edited >= 0 ? tr("编辑模板字段") : tr("添加模板字段"))
                                     : (edited >= 0 ? tr("编辑接收字段") : tr("添加接收字段"));
    AppDialog dialog(title, parent);
    dialog.setObjectName(QStringLiteral("receiveFieldEditor"));
    dialog.setPreferredSize(QSize(510, editsTemplate ? 505 : 414));
    auto *form = new QFormLayout;
    dialog.viewLayout()->addLayout(form);
    auto *name = new FluentQt::LineEdit(&dialog);
    name->setObjectName(QStringLiteral("receiveFieldName"));
    name->setText(field.at(0).toString());
    auto *type = new FluentQt::ComboBox(&dialog);
    type->setObjectName(QStringLiteral("receiveFieldType"));
    addValueItems(type, HexOptions::fieldTypes());
    selectValue(type, field.at(3).toString());
    auto *offset = new FluentQt::SpinBox(&dialog);
    offset->setObjectName(QStringLiteral("receiveFieldOffset"));
    offset->setRange(0, 65535);
    offset->setValue(field.at(1).toInt());
    auto *length = new FluentQt::SpinBox(&dialog);
    length->setRange(1, 65536);
    length->setValue(field.at(2).toInt());
    auto *order = new FluentQt::ComboBox(&dialog);
    addValueItems(order, HexOptions::byteOrders());
    selectValue(order, field.at(4).toString(HexOptions::byteOrders().first()));

    form->addRow(tr("字段名称"), name);
    form->addRow(tr("数据类型"), type);
    form->addRow(tr("帧头后的字节偏移"), offset);
    form->addRow(tr("字段长度"), length);
    form->addRow(tr("字节序"), order);
    FluentQt::LineEdit *defaultValue = nullptr;
    FluentQt::CheckBox *required = nullptr;
    if (editsTemplate) {
        required = new FluentQt::CheckBox(tr("发送时必填"), &dialog);
        required->setObjectName(QStringLiteral("sendFieldRequired"));
        required->setChecked(FrameTemplates::requiredFields(settings).contains(field.at(0).toString()));
        form->addRow(required);
        defaultValue = new FluentQt::LineEdit(&dialog);
        defaultValue->setObjectName(QStringLiteral("sendFieldDefaultValue"));
        defaultValue->setPlaceholderText(tr("选填"));
        defaultValue->setText(settings.value(FrameTemplates::SendDefaults).toObject().value(field.at(0).toString()).toString());
        form->addRow(tr("发送默认值"), defaultValue);
    }
    const auto update = [type, length, order, form] {
        const int width = HexOptions::fieldSize(selectedValue(type));
        if (width > 0)
            length->setValue(width);
        length->setReadOnly(width > 0);
        form->setRowVisible(order, width > 1);
    };
    QObject::connect(type, &FluentQt::ComboBox::currentIndexChanged, &dialog, update);
    update();

    auto *error = new FluentQt::CaptionLabel(&dialog);
    error->setWordWrap(true);
    error->setTextColor(QColor(0xC42B1C), QColor(0xFF99A4));
    dialog.viewLayout()->addWidget(error);
    dialog.viewLayout()->addStretch();
    const auto editedSettings = [&] {
        const QString dataType = selectedValue(type);
        const QString fieldName = name->text().trimmed();
        QJsonArray definition{fieldName, offset->value(), length->value(), dataType};
        if (HexOptions::fieldSize(dataType) > 1)
            definition.append(selectedValue(order));
        auto fields = allFields;
        if (edited >= 0)
            fields[edited] = definition;
        else
            fields.append(definition);
        auto result = settings;
        result.insert(QStringLiteral("fields"), fields);
        if (defaultValue) {
            auto defaults = settings.value(FrameTemplates::SendDefaults).toObject();
            if (edited >= 0)
                defaults.remove(field.at(0).toString());
            if (!defaultValue->text().isEmpty())
                defaults.insert(fieldName, defaultValue->text());
            setSendDefaults(result, defaults);

            auto requiredFields = FrameTemplates::requiredFields(settings);
            const auto position = requiredFields.indexOf(field.at(0).toString());
            if (position < 0) {
                if (required->isChecked())
                    requiredFields.append(fieldName);
            } else if (required->isChecked()) {
                requiredFields[position] = fieldName;
            } else {
                requiredFields.removeAt(position);
            }
            setSendRequired(result, requiredFields);
        }
        return result;
    };
    dialog.setValidator([&] {
        const QString key = name->text().trimmed();
        for (qsizetype i = 0; i < allFields.size(); ++i) {
            if (i != edited && allFields.at(i).toArray().at(0).toString() == key) {
                error->setText(tr("字段名称已存在"));
                return false;
            }
        }
        if (key.isEmpty()) {
            error->setText(tr("请填写字段名称"));
            return false;
        }
        if (editsTemplate) {
            const auto problem = FrameTemplates::sendFieldRulesProblem({QStringLiteral("hex"), editedSettings()});
            if (!problem.isEmpty()) {
                error->setText(problem);
                return false;
            }
        }
        return true;
    });
    if (dialog.exec() != QDialog::Accepted)
        return std::nullopt;
    return editedSettings();
}

QJsonArray newField(const QJsonObject &config, int selectedByte)
{
    const auto fields = config.value(QStringLiteral("fields")).toArray();
    QSet<QString> names;
    int offset = 0;
    const auto frame = hexFrameConfig(config);
    if (frame && frame->lengthOffset >= 0)
        offset = std::max(0, frame->lengthOffset + frame->lengthSize - int(frame->header.size()));
    for (const auto &entry : fields) {
        const auto field = entry.toArray();
        names.insert(field.at(0).toString());
        offset = std::max(offset, field.at(1).toInt() + field.at(2).toInt());
    }
    if (frame && selectedByte >= frame->header.size())
        offset = selectedByte - int(frame->header.size());
    int suffix = 1;
    while (names.contains(QStringLiteral("field%1").arg(suffix)))
        ++suffix;
    return {QStringLiteral("field%1").arg(suffix), offset, 1, QStringLiteral("uint8")};
}

} // namespace

std::optional<ReceiveFrameEdit> editReceiveFrameFormat(LinkParser::Format format, const QString &scope,
                                                       const std::optional<ReceiveValueSettings> &values, QWidget *parent,
                                                       const FrameEditorOptions &options)
{
    const bool editsTemplate = !options.templateName.isEmpty();
    AppDialog dialog(values ? tr("可视化接收与取值")
                     : editsTemplate ? tr("可视化帧格式模板") : tr("可视化接收帧格式"), parent);
    dialog.setObjectName(QStringLiteral("receiveFrameFormatEditor"));
    dialog.setPreferredSize(QSize(1110, editsTemplate && !values ? 820 : 745));
    dialog.yesButton()->setText(values ? tr("应用") : tr("应用帧格式"));
    auto *scopeLabel = new FluentQt::CaptionLabel(scope, &dialog);
    scopeLabel->setTextFormat(Qt::PlainText);
    scopeLabel->setWordWrap(true);
    dialog.viewLayout()->addWidget(scopeLabel);
    auto *name = new FluentQt::LineEdit(&dialog);
    name->setObjectName(QStringLiteral("frameTemplateName"));
    name->setText(options.templateName);
    if (!editsTemplate)
        name->hide();

    QJsonObject textConfig, hexConfig;
    if (format.type == QLatin1String("hex")) {
        hexConfig = format.settings;
    } else {
        textConfig = format.settings;
    }
    const auto scrollPane = [](QWidget *parent) {
        auto *scroll = new FluentQt::ScrollArea(parent);
        scroll->setWidgetResizable(true);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        scroll->enableTransparentBackground();
        return scroll;
    };
    auto *split = new ReceiveEditorSplitter(Qt::Horizontal, &dialog);
    split->setObjectName(QStringLiteral("receiveEditorPanes"));
    split->setChildrenCollapsible(false);
    dialog.viewLayout()->addWidget(split, 1);

    auto *left = new QWidget(split);
    auto *previewLayout = new QVBoxLayout(left);
    previewLayout->setContentsMargins(0, 0, 12, 0);
    auto *previewScroll = scrollPane(left);
    auto *previews = new QStackedWidget(previewScroll);
    auto *text = new ReceivePreview(false, previews);
    auto *hex = new ReceivePreview(true, previews);
    hex->showConfiguredFields();
    text->setSample(format.type == QLatin1String("line") && !options.sample.isEmpty() ? options.sample
                    : QStringLiteral("temperature=25.3,rpm=1500"));
    hex->setSettings(hexConfig);
    if (format.type == QLatin1String("hex") && !options.sample.isEmpty())
        hex->setSample(options.sample);
    else
        hex->generateSample();
    previews->addWidget(text);
    previews->addWidget(hex);
    previewScroll->setWidget(previews);
    previewLayout->addWidget(previewScroll, 1);
    auto *actions = new FluentQt::FlowLayout(0, 6, 4);
    previewLayout->addLayout(actions);
    auto *add = new FluentQt::PushButton(editsTemplate ? tr("添加模板字段") : tr("添加接收字段"), left);
    add->setObjectName(QStringLiteral("addReceiveFrameField"));
    auto *edit = new FluentQt::PushButton(tr("编辑所选"), left);
    auto *remove = new FluentQt::PushButton(tr("删除所选"), left);
    auto *sample = new FluentQt::TransparentPushButton(tr("生成示例"), left);
    for (auto *button : {add, edit, remove})
        actions->addWidget(button);
    auto *bind = new FluentQt::PushButton(tr("使用所选字段"), left);
    bind->setObjectName(QStringLiteral("applyReceiveField"));
    auto *matchSample = new FluentQt::PushButton(tr("用所选字节作命令字"), left);
    matchSample->setObjectName(QStringLiteral("useSampleMatchByte"));
    actions->addWidget(bind);
    hex->addSampleAction(sample);
    hex->addSampleAction(matchSample);

    auto *right = new QWidget(split);
    auto *rightLayout = new QVBoxLayout(right);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    auto *pages = new QStackedWidget(right);
    auto *scroll = scrollPane(pages);
    FluentQt::ScrollArea *bindingScroll = nullptr;
    BindingForm *bindingForm = nullptr;
    FluentQt::SegmentedWidget *tabs = nullptr;
    if (values) {
        tabs = new FluentQt::SegmentedWidget(right);
        tabs->setObjectName(QStringLiteral("receiveEditorTabs"));
        tabs->addItem(QStringLiteral("binding"), tr("当前取值"));
        tabs->addItem(QStringLiteral("format"), tr("帧格式"));
        tabs->setCurrentItem(QStringLiteral("binding"));
        rightLayout->addWidget(tabs, 0, Qt::AlignLeft);
        bindingScroll = scrollPane(pages);
        bindingForm = new BindingForm(*values, editsTemplate && format.type == QLatin1String("hex"), bindingScroll);
        bindingScroll->setWidget(bindingForm);
        pages->addWidget(bindingScroll);
    }
    pages->addWidget(scroll);
    rightLayout->addWidget(pages, 1);
    auto *settings = new QWidget(scroll);
    auto *settingsLayout = new QVBoxLayout(settings);
    settingsLayout->setContentsMargins(8, 0, 8, 0);
    auto *ruleForm = new QFormLayout;
    ruleForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    settingsLayout->addLayout(ruleForm);
    auto *mode = new FluentQt::ComboBox(settings);
    mode->setObjectName(QStringLiteral("receiveFrameMode"));
    mode->addItem(tr("文本 / JSON"), {}, QStringLiteral("line"));
    mode->addItem(QStringLiteral("HEX"), {}, QStringLiteral("hex"));
    mode->setEnabled(!options.lockParser);
    if (editsTemplate)
        ruleForm->addRow(tr("模板名称"), name);
    ruleForm->addRow(editsTemplate ? tr("模板类型") : tr("接收解析模式"), mode);
    auto *hint = new FluentQt::CaptionLabel(
        tr("粘贴示例并点击一个字节，再点「用所选字节作命令字」。字段偏移从帧头后的第 0 字节开始。"), settings);
    hint->setWordWrap(true);
    settingsLayout->addWidget(hint);
    settingsLayout->addStretch();
    scroll->setWidget(settings);
    split->setSizes({650, 370});
    split->setStretchFactor(0, 2);
    split->setStretchFactor(1, 1);

    const auto binary = [mode] { return selectedValue(mode) == QLatin1String("hex"); };
    const auto selectingValue = [&] { return bindingScroll && pages->currentWidget() == bindingScroll; };
    const auto rawSample = [&] { return selectingValue() && bindingForm->readsBytes() && !binary(); };
    const auto current = [&] { return binary() || rawSample() ? hex : text; };
    const auto readFormat = [&] {
        return LinkParser::Format{selectedValue(mode), binary() ? hexConfig : textConfig};
    };
    const auto syncActions = [&] {
        const bool show = binary();
        const bool row = show && hex->selectedHexField() >= 0;
        edit->setEnabled(row);
        remove->setEnabled(row);
        bind->setEnabled(current()->selectedField().has_value());
        matchSample->setEnabled(!hex->selectedSampleByte().isEmpty());
    };
    auto *problem = new FluentQt::CaptionLabel(&dialog);
    problem->setWordWrap(true);
    problem->setTextColor(QColor(0xC42B1C), QColor(0xFF99A4));
    problem->hide();
    dialog.viewLayout()->addWidget(problem);
    const auto refreshPreview = [&](bool locate) {
        if (bindingForm) {
            QStringList fields;
            if (binary())
                if (const auto config = hexFrameConfig(hexConfig))
                    for (const auto &field : config->fields)
                        fields.append(field.name);
            fields.removeDuplicates();
            bindingForm->setFields(fields);
        }
        auto *preview = current();
        previews->setCurrentWidget(preview);
        preview->setSettings(preview == hex ? hexConfig : textConfig,
                             bindingForm ? bindingForm->entry() : std::nullopt, values && values->strict, rawSample());
        if (locate)
            preview->locateBinding();
        for (auto *button : {add, edit, remove})
            button->setVisible(binary() && !selectingValue());
        matchSample->setVisible(binary() && !selectingValue());
        bind->setVisible(selectingValue());
        sample->setVisible(preview == hex);
        hint->setVisible(binary());
        problem->hide();
        syncActions();
    };
    const auto refresh = [&] { refreshPreview(false); };
    if (bindingForm) {
        bindingForm->onChanged([&] { refreshPreview(true); });
        QObject::connect(tabs, &FluentQt::SegmentedWidget::currentItemChanged, pages, [&](const QString &key) {
            pages->setCurrentWidget(key == QLatin1String("binding") ? bindingScroll : scroll);
            refreshPreview(selectingValue());
        });
    }
    // Construct each mode with its own draft so switches preserve in-progress settings.
    for (const auto &type : {QStringLiteral("line"), QStringLiteral("hex")}) {
        auto *modePane = new QWidget(settings);
        auto *form = new QFormLayout(modePane);
        form->setContentsMargins(0, 0, 0, 0);
        form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        auto *config = type == QLatin1String("hex") ? &hexConfig : &textConfig;
        QHash<QString, QWidget *> controls;
        addReceiveFrameFields(modePane, [config] { return *config; },
            [config, &refresh](const QJsonObject &values, const QString &) {
                *config = values;
                refresh();
            },
            [form, &controls](const QString &key, const QString &label, QWidget *control, const QString &) {
                control->setObjectName(QStringLiteral("receiveFrame_") + key);
                controls.insert(key, control);
                form->addRow(label, control);
            }, false, type);
        if (auto *algorithm = qobject_cast<FluentQt::ComboBox *>(controls.value(QStringLiteral("checksum"))))
            form->addRow(createChecksumRuleHint(algorithm, modePane));
        const auto visibility = [modePane, form, controls, mode, config, type] {
            modePane->setVisible(selectedValue(mode) == type);
            updateReceiveFrameFields({type, *config}, [form, &controls](const QString &key, bool visible) {
                if (auto *control = controls.value(key))
                    form->setRowVisible(control, visible);
            });
        };
        QObject::connect(mode, &FluentQt::ComboBox::currentIndexChanged, modePane, visibility);
        for (auto *control : std::as_const(controls))
            if (auto *choice = qobject_cast<FluentQt::ComboBox *>(control))
                QObject::connect(choice, &FluentQt::ComboBox::currentIndexChanged, modePane, visibility);
        if (auto *match = qobject_cast<QLineEdit *>(controls.value(QStringLiteral("matchBytes")))) {
            QObject::connect(match, &QLineEdit::textChanged, modePane, visibility);
            auto *offset = qobject_cast<QSpinBox *>(controls.value(QStringLiteral("matchOffset")));
            QObject::connect(matchSample, &QPushButton::clicked, modePane, [hex, match, offset] {
                const auto selected = hex->selectedSampleByte();
                if (selected.isEmpty())
                    return;
                offset->setValue(hex->selectedByte());
                match->setText(toHexString(selected));
            });
        }
        settingsLayout->insertWidget(settingsLayout->count() - 2, modePane);
        visibility();
    }
    QObject::connect(mode, &FluentQt::ComboBox::currentIndexChanged, &dialog, refresh);
    QObject::connect(hex, &ReceivePreview::selectionChanged, &dialog, syncActions);
    QObject::connect(text, &ReceivePreview::selectionChanged, &dialog, syncActions);
    auto templateSample = options.sample;
    const auto rememberTemplateSample = [&] {
        if (!selectingValue())
            templateSample = current()->savedSample();
    };
    QObject::connect(hex, &ReceivePreview::sampleEdited, &dialog, rememberTemplateSample);
    QObject::connect(text, &ReceivePreview::sampleEdited, &dialog, rememberTemplateSample);
    QObject::connect(sample, &QPushButton::clicked, &dialog, [&] {
        hex->generateSample();
        rememberTemplateSample();
    });
    QList<qsizetype> fieldOrigins;
    if (options.confirmChanges) {
        const auto count = hexConfig.value(QStringLiteral("fields")).toArray().size();
        for (qsizetype index = 0; index < count; ++index)
            fieldOrigins.append(index);
    }
    const auto changeField = [&](int index) {
        const auto fields = hexConfig.value(QStringLiteral("fields")).toArray();
        const auto original = index >= 0 ? fields.at(index).toArray() : newField(hexConfig, hex->selectedByte());
        if (const auto updated = editField(hexConfig, original, index, editsTemplate, &dialog)) {
            if (index < 0 && options.confirmChanges)
                fieldOrigins.append(-1);
            if (index >= 0 && bindingForm) {
                const auto renamed = updated->value(QStringLiteral("fields")).toArray().at(index).toArray();
                bindingForm->renameField(original.at(0).toString(), renamed.at(0).toString());
            }
            hexConfig = *updated;
            refresh();
            hex->selectHexField(index >= 0 ? index : fields.size());
        }
    };
    QObject::connect(add, &QPushButton::clicked, &dialog, [&] { changeField(-1); });
    QObject::connect(edit, &QPushButton::clicked, &dialog, [&] { changeField(hex->selectedHexField()); });
    QObject::connect(remove, &QPushButton::clicked, &dialog, [&] {
        auto fields = hexConfig.value(QStringLiteral("fields")).toArray();
        const int index = hex->selectedHexField();
        const auto fieldName = fields.at(index).toArray().at(0).toString();
        auto defaults = hexConfig.value(FrameTemplates::SendDefaults).toObject();
        defaults.remove(fieldName);
        auto requiredFields = FrameTemplates::requiredFields(hexConfig);
        requiredFields.removeAll(fieldName);
        if (options.confirmChanges)
            fieldOrigins.removeAt(index);
        fields.removeAt(index);
        hexConfig.insert(QStringLiteral("fields"), fields);
        setSendDefaults(hexConfig, defaults);
        setSendRequired(hexConfig, requiredFields);
        refresh();
    });

    selectValue(mode, format.type);
    refreshPreview(true);
    QObject::connect(bind, &QPushButton::clicked, &dialog, [&] {
        if (const auto field = current()->selectedField()) {
            bindingForm->useField(*field);
            refreshPreview(true);
        }
    });
    dialog.setValidator([&] {
        const auto config = readFormat();
        QString error;
        const bool invalidName = editsTemplate &&
            (name->text().trimmed().isEmpty() || options.takenNames.contains(name->text().trimmed()));
        if (invalidName)
            error = tr("帧格式模板的名称不能为空或重复");
        bool valid = !invalidName && bool(createParser(config.type, config.settings, &error));
        if (valid && editsTemplate) {
            error = FrameTemplates::sendFieldRulesProblem(config);
            valid = error.isEmpty();
        }
        if (valid && bindingForm) {
            error = bindingForm->problem();
            valid = error.isEmpty();
        }
        if (valid && options.formatProblem) {
            error = options.formatProblem(config);
            valid = error.isEmpty();
        }
        problem->setText(error);
        problem->setVisible(!valid);
        if (valid && options.confirmChanges)
            valid = options.confirmChanges(config, fieldOrigins, &dialog);
        return valid;
    });
    if (dialog.exec() != QDialog::Accepted)
        return std::nullopt;
    if (!bindingForm)
        templateSample = current()->savedSample();
    return ReceiveFrameEdit{readFormat(), bindingForm ? bindingForm->changes() : QJsonObject{},
                            name->text().trimmed(), templateSample};
}

} // namespace fhmi
