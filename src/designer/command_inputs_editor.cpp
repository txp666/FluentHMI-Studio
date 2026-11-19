#include "designer/command_inputs_editor.h"
#include "core/project.h"
#include "core/frame_templates.h"
#include "runtime/frame_bytes_view.h"

#include "core/i18n.h"
#include "core/project_logic.h"
#include "runtime/ui_helpers.h"

#include <FluentQtWidgets/FluentIcon.h>
#include <FluentQtWidgets/Views/ItemViews.h>
#include <FluentQtWidgets/Widgets/Button.h>
#include <FluentQtWidgets/Widgets/ComboBox.h>
#include <FluentQtWidgets/Widgets/Label.h>
#include <FluentQtWidgets/Widgets/LineEdit.h>
#include <FluentQtWidgets/Widgets/Menu.h>

#include <QtCore/QCoreApplication>
#include <QtCore/QSet>
#include <QtCore/QSignalBlocker>
#include <QtGui/QAction>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>

namespace fhmi {
namespace {

QString label(const CommandInputsEditor::InputValues::const_iterator &input)
{
    if (const auto point = pointNameForInput(input.key()); !point.isEmpty())
        return QCoreApplication::translate("fhmi", "%1 · 工程数据").arg(point);
    const auto widgetId = widgetIdForInput(input.key());
    return input->title.isEmpty() || input->title == widgetId ? widgetId
        : input->title + QStringLiteral(" · ") + widgetId;
}

} // namespace

CommandInputsEditor::CommandInputsEditor(const QJsonObject &properties, InputValues values, const QStringList &dataTypes,
                                         const QJsonArray &widgets, bool canCreate, QWidget *parent)
    : QWidget(parent), m_existing(std::move(values)), m_rows(CommandEncoder::commandInputs(properties)),
      m_dataTypes(dataTypes), m_hasVariables(CommandEncoder::usesTemplate(CommandEncoder::inputListMode(properties))),
      m_single(!CommandEncoder::singleInputProperty(CommandEncoder::inputListMode(properties)).isEmpty()),
      m_registers(CommandEncoder::inputListMode(properties) == CommandEncoder::InputListMode::Registers),
      m_registerAddress(properties.value(QStringLiteral("mbAddress")).toInt())
{
    if (m_hasVariables && !m_dataTypes.isEmpty()) {
        visitWidgets(widgets, [this](const QJsonObject &widget) {
            const auto props = widget.value(QStringLiteral("properties")).toObject();
            const QString type = props.value(QStringLiteral("dataType")).toString();
            if (widget.value(QStringLiteral("type")).toString() == QLatin1String("LineEdit") &&
                props.value(QStringLiteral("commandMode")).toString() == QLatin1String("HEX") &&
                m_dataTypes.contains(type))
                m_sourceTypes.insert(widget.value(QStringLiteral("id")).toString(), type);
            return true;
        });
    }
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(7);
    if (!m_single)
        layout->addWidget(new FluentQt::StrongBodyLabel(tr("输入列表"), this));

    auto *actions = new QHBoxLayout;
    actions->setSpacing(4);
    m_create = new FluentQt::PushButton(tr("新建输入框"), this);
    m_create->setObjectName(QStringLiteral("newCommandInput"));
    m_create->setVisible(canCreate);
    connect(m_create, &QPushButton::clicked, this, [this] {
        QString id;
        do {
            id = QStringLiteral("@input%1").arg(++m_newInput);
        } while (m_existing.contains(id));
        m_pending.insert(id, tr("参数 %1").arg(m_rows.size() + 1));
        append(id);
    });
    if (m_single) {
        m_singleSource = new FluentQt::ComboBox(this);
        m_singleSource->setObjectName(QStringLiteral("singleCommandSource"));
        auto *caption = new FluentQt::BodyLabel(tr("值来源"), this);
        caption->setBuddy(m_singleSource);
        actions->addWidget(caption);
        actions->addWidget(m_singleSource, 1);
        actions->addWidget(m_create);
        layout->addLayout(actions);
        populate();
        connect(m_singleSource, &FluentQt::ComboBox::currentIndexChanged, this, [this] {
            const auto id = selectedValue(m_singleSource);
            m_rows.clear();
            if (!id.isEmpty())
                m_rows.append({id, {}, {}});
            emit changed({}, {});
        });
        return;
    }
    actions->addWidget(m_create);
    m_choose = new FluentQt::DropDownPushButton(tr("选择已有"), this);
    auto *menu = new FluentQt::RoundMenu(m_choose);
    for (auto input = m_existing.cbegin(); input != m_existing.cend(); ++input) {
        auto *action = new QAction(label(input), menu);
        action->setData(input.key());
        menu->addAction(action);
        connect(action, &QAction::triggered, this, [this, id = input.key()] { append(id); });
    }
    m_choose->setMenu(menu);
    menu->setObjectName(QStringLiteral("existingCommandInputs"));
    actions->addWidget(m_choose);
    actions->addStretch();
    const auto tool = [this, actions](FluentQt::FluentIcon icon, const QString &name) {
        auto *button = new FluentQt::TransparentToolButton(FluentQt::icon(icon), this);
        button->setAccessibleName(name);
        button->setToolTip(name);
        actions->addWidget(button);
        return button;
    };
    m_up = tool(FluentQt::FluentIcon::Up, tr("上移输入"));
    m_down = tool(FluentQt::FluentIcon::Down, tr("下移输入"));
    m_remove = tool(FluentQt::FluentIcon::Delete, tr("移除此输入"));
    connect(m_up, &QToolButton::clicked, this, [this] { move(-1); });
    connect(m_down, &QToolButton::clicked, this, [this] { move(1); });
    connect(m_remove, &QToolButton::clicked, this, [this] {
        const int row = m_table->currentRow();
        if (row < 0)
            return;
        m_rows.removeAt(row);
        populate();
        m_table->setCurrentCell(std::min(row, m_table->rowCount() - 1), 0);
        emit changed({}, {});
    });
    layout->addLayout(actions);
    m_table = new FluentQt::TableWidget(this);
    m_table->setObjectName(QStringLiteral("commandInputsTable"));
    m_table->setBorderVisible(true);
    m_table->setBorderRadius(6);
    m_table->setShowGrid(false);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->verticalHeader()->hide();
    m_table->verticalHeader()->setDefaultSectionSize(38);
    m_table->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    QStringList columns{tr("输入来源")};
    if (m_hasVariables)
        columns.append(tr("变量"));
    if (!m_dataTypes.isEmpty())
        columns.append(tr("数据类型"));
    if (m_registers)
        columns.append({tr("寄存器范围"), tr("示例值")});
    m_table->setColumnCount(columns.size());
    m_table->setHorizontalHeaderLabels(columns);
    auto *header = m_table->horizontalHeader();
    header->setSectionResizeMode(0, QHeaderView::Stretch);
    if (m_hasVariables) {
        header->setSectionResizeMode(1, QHeaderView::Fixed);
        header->resizeSection(1, 64);
    }
    if (!m_dataTypes.isEmpty()) {
        const int typeColumn = m_hasVariables ? 2 : 1;
        header->setSectionResizeMode(typeColumn, QHeaderView::Fixed);
        header->resizeSection(typeColumn, 105);
    }
    if (m_registers) {
        for (int col : {2, 3}) {
            header->setSectionResizeMode(col, QHeaderView::Fixed);
            header->resizeSection(col, 116);
        }
    }
    connect(m_table, &QTableWidget::currentCellChanged, this, [this] { updateButtons(); });
    layout->addWidget(m_table, 1);
    populate();
}

void CommandInputsEditor::append(const QString &id)
{
    if (m_single) {
        m_rows = {{id, {}, {}}};
        populate();
        emit changed({}, {});
        return;
    }
    QSet<QString> names;
    for (const auto &entry : m_rows)
        names.insert(entry.variable);
    qsizetype next = m_rows.size();
    while (names.contains(CommandEncoder::defaultInputVariable(next)))
        ++next;
    m_rows.append({id, m_sourceTypes.value(id, QStringLiteral("uint16")), CommandEncoder::defaultInputVariable(next)});
    populate();
    m_table->setCurrentCell(m_rows.size() - 1, 0);
    m_table->scrollTo(m_table->currentIndex());
    emit changed({}, {});
}

void CommandInputsEditor::move(int step)
{
    const int row = m_table->currentRow();
    const int next = row + step;
    if (row < 0 || next < 0 || next >= m_rows.size())
        return;
    m_rows.swapItemsAt(row, next);
    populate();
    m_table->setCurrentCell(next, 0);
    m_table->scrollTo(m_table->currentIndex());
    emit changed({}, {});
}

void CommandInputsEditor::populate()
{
    if (m_singleSource) {
        fillSources(m_singleSource, m_rows.isEmpty() ? QString() : m_rows.first().inputId);
        return;
    }
    for (auto *editor : m_table->findChildren<QLineEdit *>())
        editor->disconnect(this);
    m_table->setRowCount(0);
    for (int row = 0; row < m_rows.size(); ++row) {
        const auto &entry = m_rows.at(row);
        m_table->insertRow(row);
        const auto add = [this, row](int column, QWidget *editor) {
            auto *cell = new QWidget(m_table);
            auto *layout = new QHBoxLayout(cell);
            layout->setContentsMargins(4, 2, 4, 2);
            layout->addWidget(editor);
            m_table->setCellWidget(row, column, cell);
        };
        auto *source = new FluentQt::ComboBox(m_table);
        source->setAccessibleName(tr("输入框"));
        fillSources(source, entry.inputId);
        add(0, source);
        connect(source, &FluentQt::ComboBox::currentIndexChanged, this, [this, row, source] {
            m_rows[row].inputId = selectedValue(source);
            m_table->setCurrentCell(row, 0);
            updateRegisterCells();
            emit changed({}, {});
        });
        if (m_hasVariables) {
            auto *name = new FluentQt::LineEdit(m_table);
            name->setAccessibleName(tr("模板变量"));
            name->setText(entry.variable);
            add(1, name);
            connect(name, &QLineEdit::editingFinished, this, [this, row, name] {
                const QString value = name->text().trimmed();
                const QString previous = m_rows.at(row).variable;
                if (value == previous)
                    return;
                m_rows[row].variable = value;
                m_table->setCurrentCell(row, 1);
                emit changed(previous, value);
            });
        }
        if (!m_dataTypes.isEmpty()) {
            const int column = m_hasVariables ? 2 : 1;
            auto *type = new FluentQt::ComboBox(m_table);
            type->setAccessibleName(tr("数据类型"));
            addValueItems(type, m_dataTypes);
            if (!selectValue(type, entry.dataType)) {
                type->addItem(translated(entry.dataType), {}, entry.dataType);
                type->setCurrentIndex(type->count() - 1);
            }
            add(column, type);
            connect(type, &FluentQt::ComboBox::currentIndexChanged, this, [this, row, column, type] {
                m_rows[row].dataType = selectedValue(type);
                m_table->setCurrentCell(row, column);
                updateRegisterCells();
                emit changed({}, {});
            });
        }
    }
    if (!m_rows.isEmpty())
        m_table->setCurrentCell(0, 0);
    updateRegisterCells();
    updateButtons();
}

void CommandInputsEditor::fillSources(FluentQt::ComboBox *source, const QString &current)
{
    const QSignalBlocker blocker(source);
    source->clear();
    if (m_single)
        source->addItem(tr("固定值"), {}, QString());
    for (auto input = m_existing.cbegin(); input != m_existing.cend(); ++input)
        source->addItem(label(input), {}, input.key());
    for (auto it = m_pending.cbegin(); it != m_pending.cend(); ++it)
        source->addItem(tr("新建 · %1").arg(it.value()), {}, it.key());
    if (!selectValue(source, current)) {
        source->addItem(current, {}, current);
        source->setCurrentIndex(source->count() - 1);
    }
}

void CommandInputsEditor::setRegisterAddress(int address)
{
    m_registerAddress = address;
    updateRegisterCells();
}

void CommandInputsEditor::updateRegisterCells()
{
    if (!m_registers)
        return;
    const auto samples = values();
    qsizetype address = m_registerAddress;
    for (int row = 0; row < m_rows.size(); ++row) {
        const auto &input = m_rows.at(row);
        const int registers = CommandEncoder::usesRegisterOrder(input.dataType) ? 2 : 1;
        const QString range = address + registers > 65536 ? tr("地址超出范围")
            : registers == 2 ? QStringLiteral("%1–%2").arg(address).arg(address + 1) : QString::number(address);
        const QString sample = samples.value(input.inputId).text();
        const QStringList values{range, sample.isEmpty() ? QStringLiteral("—") : sample};
        for (int i = 0; i < values.size(); ++i) {
            auto *cell = m_table->item(row, 2 + i);
            if (!cell) {
                cell = new QTableWidgetItem;
                cell->setFlags(cell->flags() & ~Qt::ItemIsEditable);
                m_table->setItem(row, 2 + i, cell);
            }
            cell->setText(values.at(i));
        }
        address += registers;
    }
}

void CommandInputsEditor::updateButtons()
{
    const int row = m_table->currentRow();
    m_choose->setEnabled(!m_existing.isEmpty());
    m_up->setEnabled(row > 0);
    m_down->setEnabled(row >= 0 && row + 1 < m_rows.size());
    m_remove->setEnabled(row >= 0);
}

QJsonArray CommandInputsEditor::toJson() const
{
    QJsonArray rows;
    for (const auto &input : m_rows) {
        QJsonObject row{{QStringLiteral("inputId"), input.inputId}, {QStringLiteral("dataType"), input.dataType}};
        if (!input.variable.isEmpty())
            row.insert(QStringLiteral("variable"), input.variable);
        rows.append(row);
    }
    return rows;
}

CommandInputsEditor::InputValues CommandInputsEditor::values() const
{
    auto result = m_existing;
    const auto pending = pendingInputs();
    for (auto it = pending.cbegin(); it != pending.cend(); ++it)
        result.insert(it.key(), {{}, it.value()});
    return result;
}

QMap<QString, QString> CommandInputsEditor::pendingInputs() const
{
    QMap<QString, QString> result;
    for (const auto &input : m_rows)
        if (m_pending.contains(input.inputId))
            result.insert(input.inputId, m_pending.value(input.inputId));
    return result;
}

QString CommandInputsEditor::validate() const
{
    QSet<QString> names;
    int registerCount = 0;
    for (qsizetype i = 0; i < m_rows.size(); ++i) {
        const auto &input = m_rows.at(i);
        const auto errorAtRow = [this, i](const QString &error) {
            if (m_table) {
                m_table->setCurrentCell(int(i), 0);
                m_table->scrollTo(m_table->currentIndex());
            }
            return error;
        };
        if (!m_existing.contains(input.inputId) && !m_pending.contains(input.inputId)) {
            return errorAtRow(tr("请选择第 %1 行的输入来源").arg(i + 1));
        }
        if (!m_dataTypes.isEmpty() && !m_dataTypes.contains(input.dataType))
            return errorAtRow(tr("第 %1 行的数据类型不适用于当前模式").arg(i + 1));
        if (m_hasVariables && (!CommandEncoder::validInputVariable(input.variable) || names.contains(input.variable)))
            return errorAtRow(tr("第 %1 行的模板变量须以英文字母或下划线开头，只能包含字母、数字和下划线，且不能重复").arg(i + 1));
        if (m_registers) {
            registerCount += CommandEncoder::usesRegisterOrder(input.dataType) ? 2 : 1;
            if (registerCount > 123)
                return errorAtRow(tr("FC16 每帧最多写入 123 个寄存器"));
            if (m_registerAddress + registerCount > 65536)
                return errorAtRow(tr("第 %1 行的寄存器地址超出范围").arg(i + 1));
        }
        names.insert(input.variable);
    }
    return {};
}

void CommandInputsEditor::useRows(const QList<CommandEncoder::CommandInput> &rows)
{
    m_rows = rows;
    populate();
    emit changed({}, {});
}

namespace {

QString fieldSample(const HexField &field)
{
    if (field.type == QLatin1String("ASCII"))
        return {};
    if (field.type == QLatin1String("HEX"))
        return QString(field.length * 2, u'0');
    return QStringLiteral("0");
}

} // namespace

FrameFieldInputsEditor::FrameFieldInputsEditor(const QJsonArray &bindings, InputValues inputs, bool canCreate,
                                               QWidget *parent)
    : QWidget(parent), m_existing(std::move(inputs))
{
    for (const auto &binding : bindings) {
        const auto row = binding.toObject();
        m_bindings.insert(row.value(QStringLiteral("field")).toString(), row);
    }
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    auto *actions = new QHBoxLayout;
    m_create = new FluentQt::PushButton(tr("为所选字段新建输入框"), this);
    m_create->setObjectName(QStringLiteral("newFrameFieldInput"));
    m_create->setVisible(canCreate);
    actions->addWidget(m_create);
    auto *fill = new FluentQt::CaptionLabel(tr("字段沿用模板默认值，未填的命令字自动补齐，其他字节填 00"), this);
    fill->setWordWrap(true);
    fill->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    actions->addWidget(fill, 1);
    layout->addLayout(actions);
    m_table = new FluentQt::TableWidget(this);
    m_table->setObjectName(QStringLiteral("frameFieldInputs"));
    m_table->setColumnCount(5);
    m_table->setHorizontalHeaderLabels({tr("模板字段"), tr("整帧字节"), tr("数据类型"), tr("值来源"), tr("固定值 / 示例")});
    m_table->setBorderVisible(true);
    m_table->setShowGrid(false);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->verticalHeader()->hide();
    m_table->verticalHeader()->setDefaultSectionSize(40);
    auto *header = m_table->horizontalHeader();
    header->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    header->setSectionResizeMode(QHeaderView::ResizeToContents);
    header->setSectionResizeMode(3, QHeaderView::Stretch);
    header->setSectionResizeMode(4, QHeaderView::Fixed);
    header->resizeSection(4, 180);
    layout->addWidget(m_table, 1);
    connect(m_table, &QTableWidget::currentCellChanged, this, [this](int row) {
        const QString name = m_fields.value(row);
        m_create->setEnabled(m_definitions.contains(name));
        emit fieldSelected(name);
    });
    connect(m_create, &QPushButton::clicked, this, [this] {
        const QString name = m_fields.value(m_table->currentRow());
        QString id;
        do {
            id = QStringLiteral("@frameInput%1").arg(++m_nextInput);
        } while (m_existing.contains(id));
        m_pending.insert(id, name);
        m_bindings.insert(name, {{QStringLiteral("field"), name}, {QStringLiteral("inputId"), id}});
        populate();
        selectField(name);
        emit changed();
    });
}

void FrameFieldInputsEditor::setFormat(const std::optional<HexFrameConfig> &format, const QJsonObject &settings)
{
    m_fields.clear();
    m_definitions.clear();
    m_defaults = settings.value(FrameTemplates::SendDefaults).toObject();
    m_required = FrameTemplates::requiredFields(settings);
    m_headerSize = format ? format->header.size() : 0;
    if (format) {
        for (const auto &field : FrameTemplates::writableFields(*format)) {
            m_fields.append(field.name);
            m_definitions.insert(field.name, field);
        }
    }
    for (auto it = m_bindings.cbegin(); it != m_bindings.cend(); ++it)
        if (!m_definitions.contains(it.key()))
            m_fields.append(it.key());
    populate();
}

void FrameFieldInputsEditor::populate()
{
    const QSignalBlocker blocker(m_table);
    m_table->setRowCount(0);
    const auto samples = sampleValues();
    for (const auto &name : m_fields) {
        const int row = m_table->rowCount();
        m_table->insertRow(row);
        const auto field = m_definitions.constFind(name);
        const auto binding = m_bindings.value(name);
        auto *key = new QTableWidgetItem(m_required.contains(name) ? tr("%1（必填）").arg(name) : name);
        key->setBackground(frameByteColor(FrameByteKind::Field, row));
        m_table->setItem(row, 0, key);
        QString span, type;
        if (field == m_definitions.cend()) {
            type = tr("字段不可用");
        } else {
            const auto first = m_headerSize + field->offset;
            span = field->length == 1 ? QString::number(first)
                    : QStringLiteral("%1–%2").arg(first).arg(first + field->length - 1);
            type = translated(field->type);
            if (HexOptions::fieldSize(field->type) > 1)
                type += QStringLiteral(" · ") + translated(HexOptions::byteOrders().at(field->littleEndian ? 1 : 0));
        }
        m_table->setItem(row, 1, new QTableWidgetItem(span));
        m_table->setItem(row, 2, new QTableWidgetItem(type));
        auto *source = new FluentQt::ComboBox(m_table);
        source->setAccessibleName(tr("%1 的值来源").arg(name));
        source->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        source->addItem(m_defaults.contains(name) ? tr("模板默认值") : tr("未设置"));
        source->addItem(tr("固定值"));
        for (auto input = m_existing.cbegin(); input != m_existing.cend(); ++input)
            source->addItem(label(input), {}, input.key());
        for (auto input = m_pending.cbegin(); input != m_pending.cend(); ++input)
            source->addItem(tr("新建 · %1").arg(input.value()), {}, input.key());
        const auto inputId = binding.value(QStringLiteral("inputId")).toString();
        if (!inputId.isEmpty() && source->findData(inputId) < 0)
            source->addItem(inputId, {}, inputId);
        source->setCurrentIndex(binding.contains(QStringLiteral("value")) ? 1
                                 : inputId.isEmpty() ? 0 : source->findData(inputId));
        m_table->setCellWidget(row, 3, source);
        auto *value = new FluentQt::LineEdit(m_table);
        value->setAccessibleName(tr("%1 的固定值或示例值").arg(name));
        m_table->setCellWidget(row, 4, value);
        const auto updateValue = [value, source, this, name](const InputValues &examples) {
            const QSignalBlocker blocker(value);
            value->setReadOnly(source->currentIndex() != 1);
            value->setText(source->currentIndex() == 1 ? m_bindings.value(name).value(QStringLiteral("value")).toString()
                          : source->currentIndex() == 0 ? m_defaults.value(name).toString()
                                                       : examples.value(selectedValue(source)).text());
        };
        updateValue(samples);
        connect(source, &FluentQt::ComboBox::currentIndexChanged, this, [this, row, name, source, updateValue](int index) {
            if (index == 0)
                m_bindings.remove(name);
            else {
                QJsonObject choice{{QStringLiteral("field"), name}};
                if (index == 1)
                    choice.insert(QStringLiteral("value"), m_defaults.value(name).toString(fieldSample(m_definitions.value(name))));
                else
                    choice.insert(QStringLiteral("inputId"), selectedValue(source));
                m_bindings.insert(name, choice);
            }
            updateValue(sampleValues());
            m_table->setCurrentCell(row, 0);
            emit changed();
        });
        connect(value, &QLineEdit::textChanged, this, [this, name](const QString &text) {
            m_bindings[name].insert(QStringLiteral("value"), text);
            emit changed();
        });
    }
    if (!m_fields.isEmpty())
        m_table->setCurrentCell(0, 0);
    m_create->setEnabled(m_definitions.contains(m_fields.value(m_table->currentRow())));
}

QJsonArray FrameFieldInputsEditor::bindings() const
{
    QJsonArray rows;
    for (const auto &name : m_fields)
        if (m_bindings.contains(name))
            rows.append(m_bindings.value(name));
    return rows;
}

FrameFieldInputsEditor::InputValues FrameFieldInputsEditor::sampleValues() const
{
    auto values = m_existing;
    for (auto it = m_bindings.cbegin(); it != m_bindings.cend(); ++it) {
        const auto id = it.value().value(QStringLiteral("inputId")).toString();
        if (m_pending.contains(id))
            values.insert(id, {{}, m_pending.value(id)});
        if (auto value = values.find(id); value != values.end() && value->text().isEmpty())
            value->value = fieldSample(m_definitions.value(it.key()));
    }
    return values;
}

QMap<QString, QString> FrameFieldInputsEditor::pendingInputs() const
{
    QMap<QString, QString> used;
    for (const auto &binding : m_bindings) {
        const auto id = binding.value(QStringLiteral("inputId")).toString();
        if (m_pending.contains(id))
            used.insert(id, m_pending.value(id));
    }
    return used;
}

QString FrameFieldInputsEditor::validate() const
{
    for (auto it = m_bindings.cbegin(); it != m_bindings.cend(); ++it) {
        const auto id = it.value().value(QStringLiteral("inputId")).toString();
        if (!id.isEmpty() && !m_existing.contains(id) && !m_pending.contains(id))
            return tr("字段 %1 的输入来源 %2 没有有效值").arg(it.key(), id);
    }
    return {};
}

void FrameFieldInputsEditor::selectField(const QString &name)
{
    if (const int row = fieldRow(name); row >= 0) {
        m_table->setCurrentCell(row, 0);
        m_table->scrollTo(m_table->currentIndex());
    }
}

} // namespace fhmi
