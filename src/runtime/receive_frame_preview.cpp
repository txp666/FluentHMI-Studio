#include "runtime/receive_frame_preview.h"
#include "core/command_encoder.h"
#include "core/frame_templates.h"

#include "core/binding.h"
#include "core/i18n.h"
#include "core/protocol_parser.h"
#include "runtime/frame_bytes_view.h"
#include "runtime/app_dialog.h"

#include <FluentQtWidgets/Views/ItemViews.h>
#include <FluentQtWidgets/Widgets/Button.h>
#include <FluentQtWidgets/Widgets/ComboBox.h>
#include <FluentQtWidgets/Widgets/Label.h>
#include <FluentQtWidgets/Widgets/LineEdit.h>
#include <FluentQtWidgets/Widgets/ScrollArea.h>

#include <QtCore/QCoreApplication>
#include <QtCore/QJsonArray>
#include <QtCore/QMimeData>
#include <QtCore/QSignalBlocker>
#include <QtCore/QtEndian>
#include <QtGui/QFontDatabase>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <memory>

namespace fhmi {
namespace {

QString tr(const char *source) { return QCoreApplication::translate("fhmi", source); }

class ReceiveSampleEdit : public FluentQt::PlainTextEdit
{
  public:
    explicit ReceiveSampleEdit(ReceivePreview *preview) : FluentQt::PlainTextEdit(preview), m_preview(preview) {}

  protected:
    void insertFromMimeData(const QMimeData *data) override
    {
        const auto previous = m_preview->savedSample();
        FluentQt::PlainTextEdit::insertFromMimeData(data);
        if (data->hasText())
            emit m_preview->samplePasted(previous);
    }

  private:
    ReceivePreview *m_preview;
};

QString frameProblem(HexFrameParser::Issue issue, qsizetype size)
{
    using Issue = HexFrameParser::Issue;
    switch (issue) {
    case Issue::Header: return tr("帧头不匹配");
    case Issue::Identifier: return tr("命令字不匹配");
    case Issue::Length: return tr("整帧长度与配置不符");
    case Issue::Footer: return tr("帧尾不匹配");
    case Issue::ChecksumRange: return tr("校验起点超出了数据范围");
    case Issue::Checksum: return tr("解析未通过 · %1 字节").arg(size);
    case Issue::None: return {};
    }
    return {};
}

QByteArray sampleFrame(const HexFrameConfig &config, const QJsonObject &settings)
{
    const auto layout = config.layout();
    qsizetype bodySize = std::max<qsizetype>({3, layout.lengthOffset - layout.headerSize,
                                           config.checksumStart - layout.headerSize - layout.lengthSize});
    for (const auto &field : config.fields)
        bodySize = std::max(bodySize, qsizetype(field.offset) + field.length - layout.lengthSize);
    bodySize = std::max(bodySize, config.identificationEnd() - layout.headerSize - layout.lengthSize);
    if (config.fixedLength)
        bodySize = config.fixedLength - layout.headerSize - layout.footerSize - layout.checksumSize;
    // Offer a short sample. Large frames can be pasted from an actual capture.
    if (bodySize < 0 || bodySize > 256)
        return {};
    if (!settings.value(FrameTemplates::SendDefaults).toObject().isEmpty()) {
        const auto result = CommandEncoder::frameFromTemplateDefaults(settings);
        return result.command ? result.command->bytes : QByteArray();
    }
    QByteArray body(bodySize + layout.lengthSize, '\0');
    for (qsizetype i = 0; i < bodySize; ++i)
        body[i] = char(i + 1);
    FrameTemplates::fillUnassignedMatchBytes(body, {}, config);
    if (layout.lengthSize)
        body.remove(layout.lengthOffset - layout.headerSize, layout.lengthSize);
    return buildHexFrame(body, config.header, config.footer, settings).value_or(QByteArray{});
}

struct DecodedSpan
{
    FrameByteSpan bytes;
    QString type;
    QString value;
    int fieldIndex = -1;
    int fieldWidth = 0;
    QString fieldName = {};
};

enum TableRole { SpanKeyRole = Qt::UserRole, BindableFieldRole, JsonPathRole, HexFieldIndexRole };

QList<DecodedSpan> decodeSpans(const HexFrameConfig &config, const QByteArray &frame, const Message &values,
                               HexFrameParser::Issue issue, bool showConfiguredFields)
{
    QList<DecodedSpan> rows;
    const auto layout = config.layout();
    const bool valid = issue == HexFrameParser::Issue::None;
    const bool complete = valid || issue == HexFrameParser::Issue::Checksum;
    qsizetype lengthRow = -1;
    for (const auto &span : frame.isEmpty() ? QList<FrameByteSpan>{} : frameEnvelopeSpans(layout, frame.size())) {
        if (span.size == 0 || span.offset < 0 || span.offset + span.size > frame.size())
            continue;
        if (!complete && span.kind != FrameByteKind::Header && span.kind != FrameByteKind::Length)
            continue;
        QString type;
        QString value;
        if (span.kind == FrameByteKind::Length) {
            type = config.lengthCovers == HexFrameConfig::LengthCovers::Data
                ? tr("数据长度") : translated(HexOptions::lengthCovers().at(int(config.lengthCovers)));
            const auto *start = frame.constData() + span.offset;
            const quint16 size = span.size == 1 ? quint8(*start)
                : config.lengthLittleEndian ? qFromLittleEndian<quint16>(start) : qFromBigEndian<quint16>(start);
            value = tr("%1 字节").arg(size);
            lengthRow = rows.size();
        } else if (span.kind == FrameByteKind::Checksum) {
            type = translated(HexOptions::checksums().at(int(config.checksum)));
            value = valid ? tr("校验通过") : tr("校验值不匹配");
        } else {
            type = tr("固定内容");
        }
        rows.append({span, type, value});
    }
    const auto dataEnd = complete ? layout.checksumOffset(frame.size()) : frame.size();
    if (!config.matchBytes.isEmpty() && config.matchOffset + config.matchBytes.size() <= dataEnd) {
        const FrameByteSpan span{QStringLiteral("identifier"), tr("命令字"), config.matchOffset,
                                 config.matchBytes.size(), FrameByteKind::Fixed};
        rows.append({span, tr("匹配 %1").arg(toHexString(config.matchBytes)),
                     frame.mid(span.offset, span.size) == config.matchBytes ? QString() : tr("命令字不匹配")});
    }
    const qsizetype fieldCount = complete || showConfiguredFields ? config.fields.size() : 0;
    const auto lengthType = layout.lengthSize == 1 ? QLatin1String("uint8") : QLatin1String("uint16");
    for (int i = 0; i < fieldCount; ++i) {
        const auto &field = config.fields.at(i);
        const qsizetype offset = layout.headerSize + field.offset;
        const bool inRange = offset + field.length <= dataEnd;
        const bool sameLength = lengthRow >= 0 && inRange && !field.name.isEmpty() && offset == layout.lengthOffset &&
                                field.length == layout.lengthSize && field.type == lengthType &&
                                (layout.lengthSize == 1 || field.littleEndian == config.lengthLittleEndian);
        if (sameLength && std::count_if(config.fields.cbegin(), config.fields.cend(), [&field](const HexField &other) {
                return other.name == field.name;
            }) == 1) {
            auto &row = rows[lengthRow];
            row.type = field.type + QStringLiteral(" · ") + row.type;
            row.fieldIndex = i;
            row.fieldWidth = field.length;
            row.fieldName = field.name;
            lengthRow = -1;
            continue;
        }
        const bool decoded = complete && inRange;
        const FrameByteSpan span{QStringLiteral("rx:%1").arg(i), field.name, offset, decoded ? field.length : 0,
                                 FrameByteKind::Field, i};
        const QString value = !decoded && !showConfiguredFields ? tr("超出数据范围")
            : valid && values.contains(field.name) ? displayString(values.value(field.name)) : QString();
        rows.append({span, field.type, value, i, field.length, field.name});
    }
    std::stable_sort(rows.begin(), rows.end(), [](const auto &first, const auto &second) {
        return first.bytes.offset < second.bytes.offset;
    });
    return rows;
}

void displayRows(FluentQt::TableWidget *table, const QList<DecodedSpan> &rows, const QByteArray &frame, bool valid)
{
    const auto *current = table->item(table->currentRow(), 0);
    const QString selected = current ? current->data(SpanKeyRole).toString() : QString();
    table->setRowCount(0);
    for (const auto &row : rows) {
        const int index = table->rowCount();
        table->insertRow(index);
        const auto &span = row.bytes;
        QString offset = QString::number(span.offset);
        const auto width = row.fieldIndex >= 0 ? row.fieldWidth : span.size;
        if (width > 1)
            offset += QStringLiteral("–%1").arg(span.offset + width - 1);
        const bool hasAlias = span.kind == FrameByteKind::Length && !row.fieldName.isEmpty() && row.fieldName != span.label;
        auto *name = new QTableWidgetItem(hasAlias ? tr("%1（%2）").arg(span.label, row.fieldName) : span.label);
        name->setData(SpanKeyRole, span.key);
        if (valid && span.size > 0 && !row.value.isEmpty())
            name->setData(BindableFieldRole, row.fieldName);
        name->setData(HexFieldIndexRole, row.fieldIndex);
        const QStringList texts{row.type, offset, toHexString(frame.mid(span.offset, span.size)), row.value};
        table->setItem(index, 0, name);
        for (int col = 0; col < texts.size(); ++col)
            table->setItem(index, col + 1, new QTableWidgetItem(texts.at(col)));
        table->item(index, 3)->setBackground(frameByteColor(span.kind, span.tint));
        if (selected == span.key)
            table->setCurrentCell(index, 0);
    }
}

void addSaveAction(AppDialog *dialog, ReceivePreview *preview, SaveReceivedField saveField)
{
    if (!saveField)
        return;
    auto *save = new FluentQt::PushButton(tr("将所选字段保存为数据点"), dialog->buttonGroup());
    save->setObjectName(QStringLiteral("saveReceivedField"));
    dialog->buttonLayout()->insertWidget(0, save, 0, Qt::AlignVCenter);
    const auto update = [preview, save] { save->setEnabled(preview->selectedField().has_value()); };
    update();
    QObject::connect(preview, &ReceivePreview::selectionChanged, save, update);
    QObject::connect(save, &QPushButton::clicked, dialog, [preview, saveField = std::move(saveField)] {
        if (const auto field = preview->selectedField())
            saveField(field->key, field->jsonPath);
    });
}

ReceivePreview *addScrollablePreview(AppDialog *dialog, bool binary)
{
    auto *scroll = new FluentQt::ScrollArea(dialog->widget());
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->enableTransparentBackground();
    auto *preview = new ReceivePreview(binary, scroll);
    scroll->setWidget(preview);
    dialog->viewLayout()->addWidget(scroll, 1);
    return preview;
}

void appendTextFields(QTableWidget *table, const QString &path, const QVariant &value, bool json)
{
    const int row = table->rowCount();
    table->insertRow(row);
    auto *key = new QTableWidgetItem(path);
    key->setData(BindableFieldRole, path);
    key->setData(JsonPathRole, json);
    table->setItem(row, 0, key);
    table->setItem(row, 1, new QTableWidgetItem(displayString(value)));
    if (value.typeId() == QMetaType::QVariantMap) {
        const auto fields = value.toMap();
        for (auto it = fields.cbegin(); it != fields.cend(); ++it)
            if (!it.key().isEmpty() && !it.key().contains(u'.'))
                appendTextFields(table, path + u'.' + it.key(), it.value(), true);
    } else if (value.typeId() == QMetaType::QVariantList) {
        const auto fields = value.toList();
        for (qsizetype i = 0; i < fields.size(); ++i)
            appendTextFields(table, path + u'.' + QString::number(i), fields.at(i), true);
    }
}

} // namespace

ReceivePreview::ReceivePreview(bool binary, QWidget *parent) : QWidget(parent), m_binary(binary)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);
    auto *label = new FluentQt::BodyLabel(binary ? fhmi::tr("示例报文（HEX）") : fhmi::tr("示例报文"), this);
    m_sampleActions = new QHBoxLayout;
    m_sampleActions->addWidget(label);
    m_sampleActions->addStretch();
    layout->addLayout(m_sampleActions);
    m_input = new ReceiveSampleEdit(this);
    m_input->setObjectName(binary ? QStringLiteral("receiveFrameSample") : QStringLiteral("receiveTextSample"));
    m_input->setAccessibleName(label->text());
    m_input->setPlaceholderText(binary ? fhmi::tr("粘贴一帧 HEX 报文") : fhmi::tr("粘贴文本或 JSON 样例"));
    m_input->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_input->setFixedHeight(binary ? 38 : 78);
    layout->addWidget(m_input);

    m_status = new FluentQt::BodyLabel(this);
    m_status->setObjectName(QStringLiteral("receiveFrameSampleStatus"));
    m_status->setWordWrap(true);
    if (binary) {
        auto *statusRow = new QHBoxLayout;
        statusRow->addWidget(m_status, 1);
        m_diagram = new FrameDiagram(this, statusRow);
        layout->addLayout(statusRow);
        m_bytes = m_diagram->byteView();
        layout->addWidget(m_diagram);
        m_frameCoverage = new FrameCoveragePreview(this);
        layout->addWidget(m_frameCoverage);
    } else {
        layout->addWidget(m_status);
    }
    m_bindingValue = new FluentQt::BodyLabel(this);
    m_bindingValue->setObjectName(QStringLiteral("receiveBindingSampleValue"));
    m_bindingValue->setTextFormat(Qt::PlainText);
    m_bindingValue->setWordWrap(true);
    m_bindingValue->hide();
    layout->addWidget(m_bindingValue);

    m_table = new FluentQt::TableWidget(this);
    const QStringList headers = binary
        ? QStringList{fhmi::tr("字段"), fhmi::tr("类型"), fhmi::tr("整帧偏移"), QStringLiteral("HEX"), fhmi::tr("解析值")}
        : QStringList{fhmi::tr("字段或 JSON 路径"), fhmi::tr("解析值")};
    m_table->setObjectName(binary ? QStringLiteral("receiveFrameFields") : QStringLiteral("receiveTextFields"));
    m_table->setColumnCount(headers.size());
    m_table->setHorizontalHeaderLabels(headers);
    m_table->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    if (binary)
        m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setShowGrid(false);
    m_table->verticalHeader()->hide();
    m_table->verticalHeader()->setDefaultSectionSize(28);
    m_table->setMinimumHeight(116);
    layout->addWidget(m_table, 1);
    layout->addStretch();

    connect(m_table, &QTableWidget::currentCellChanged, this, [this](int row) {
        if (m_bytes) {
            const auto *cell = m_table->item(row, 0);
            m_bytes->selectPart(cell ? cell->data(SpanKeyRole).toString() : QString());
        }
        emit selectionChanged();
    });
    if (m_bytes) {
        connect(m_bytes, &FrameBytesView::byteClicked, this, [this](int offset) {
            m_selectedByte = offset;
            if (m_showConfiguredFields && !m_binding)
                m_bytes->markRange(offset, 1);
            emit selectionChanged();
        });
        connect(m_bytes, &FrameBytesView::partClicked, this, [this](const QString &key) {
            for (int row = 0; row < m_table->rowCount(); ++row) {
                if (m_table->item(row, 0)->data(SpanKeyRole).toString() == key) {
                    m_table->setCurrentCell(row, 0);
                    m_table->scrollTo(m_table->currentIndex(), QAbstractItemView::PositionAtCenter);
                    for (auto *parent = parentWidget(); parent; parent = parent->parentWidget()) {
                        if (auto *scroll = qobject_cast<QScrollArea *>(parent)) {
                            const auto cell = m_table->visualRect(m_table->currentIndex());
                            const auto point = m_table->viewport()->mapTo(scroll->widget(), cell.center());
                            scroll->ensureVisible(point.x(), point.y(), 0, cell.height() / 2 + 2);
                            break;
                        }
                    }
                    break;
                }
            }
        });
    }
    connect(m_input, &QPlainTextEdit::textChanged, this, [this] {
        m_generated = false;
        refresh();
        emit sampleEdited();
    });
}

void ReceivePreview::addSampleAction(QWidget *action) { m_sampleActions->addWidget(action); }

void ReceivePreview::setSettings(const QJsonObject &settings, std::optional<BindingEntry> binding, bool strict,
                                 bool rawSample)
{
    m_binding = std::move(binding);
    m_strict = strict;
    m_rawSample = rawSample;
    const bool settingsChanged = m_settings != settings;
    m_settings = settings;
    if (m_generated && settingsChanged)
        generateSample();
    else
        refresh();
}

void ReceivePreview::setSample(const QString &sample)
{
    m_generated = false;
    m_input->setPlainText(sample);
}

void ReceivePreview::loadFrameSample(const QJsonObject &settings, const std::optional<QString> &sample)
{
    const QSignalBlocker blocker(m_input);
    m_table->setCurrentCell(-1, -1);
    m_settings = settings;
    m_generated = false;
    m_input->setPlainText(sample.value_or(QString()));
    if (sample)
        refresh();
    else
        generateSample();
}

QString ReceivePreview::savedSample() const
{
    return m_generated ? QString() : m_input->toPlainText();
}

QByteArray ReceivePreview::selectedSampleByte() const
{
    return m_bytes && !m_generated && m_selectedByte >= 0 ? m_bytes->bytes().mid(m_selectedByte, 1) : QByteArray();
}

void ReceivePreview::generateSample()
{
    if (!m_binary)
        return;
    const auto config = hexFrameConfig(m_settings);
    if (!config) {
        refresh();
        return;
    }
    const QSignalBlocker blocker(m_input);
    m_generated = true;
    m_input->setPlainText(toHexString(sampleFrame(*config, m_settings)));
    refresh();
}

void ReceivePreview::locateBinding()
{
    if (m_bindingRow >= 0) {
        m_table->setCurrentCell(m_bindingRow, 0);
        m_table->scrollTo(m_table->currentIndex());
    } else if (m_bytes) {
        m_bytes->selectPart({});
        m_bytes->locateMarkedRange();
    }
}

void ReceivePreview::showConfiguredFields()
{
    m_showConfiguredFields = true;
}

std::optional<ReceivedField> ReceivePreview::selectedField() const
{
    const auto *item = m_table->item(m_table->currentRow(), 0);
    const QString field = item ? item->data(BindableFieldRole).toString() : QString();
    if (field.isEmpty())
        return std::nullopt;
    return ReceivedField{field, item->data(JsonPathRole).toBool()};
}

void ReceivePreview::selectOnlyDecodedField()
{
    int decoded = -1;
    for (int row = 0; row < m_table->rowCount(); ++row) {
        if (m_table->item(row, 0)->data(BindableFieldRole).toString().isEmpty())
            continue;
        if (decoded >= 0)
            return;
        decoded = row;
    }
    if (decoded >= 0)
        m_table->selectRow(decoded);
}

int ReceivePreview::selectedHexField() const
{
    const auto *item = m_table->item(m_table->currentRow(), 0);
    return m_binary && item ? item->data(HexFieldIndexRole).toInt() : -1;
}

void ReceivePreview::selectHexField(int index)
{
    if (index < 0)
        return;
    for (int row = 0; row < m_table->rowCount(); ++row) {
        if (m_table->item(row, 0)->data(HexFieldIndexRole).toInt() == index) {
            m_table->setCurrentCell(row, 0);
            m_table->scrollTo(m_table->currentIndex());
            break;
        }
    }
}

void ReceivePreview::refresh()
{
    m_selectedByte = -1;
    m_bindingRow = -1;
    if (m_binary)
        refreshHex();
    else
        refreshText();
    emit selectionChanged();
}

void ReceivePreview::markBindingRow(int row)
{
    m_bindingRow = row;
    auto *item = m_table->item(row, 0);
    auto font = item->font();
    font.setBold(true);
    item->setFont(font);
    item->setData(Qt::AccessibleDescriptionRole, fhmi::tr("当前取值字段"));
}

void ReceivePreview::showBindingValue(const QString &selection, const QVariant &value)
{
    m_bindingValue->setVisible(m_binding.has_value());
    if (!m_binding)
        return;
    QString result = fhmi::tr("样例没有有效取值");
    if (value.isValid() && !value.isNull()) {
        const auto original = displayString(value);
        const auto converted = m_binding->evaluateStrict(value);
        if (m_binding->expression.isEmpty())
            result = fhmi::tr("样例值 %1").arg(original);
        else if (converted.isValid())
            result = fhmi::tr("样例值 %1 → %2").arg(original, displayString(converted));
        else
            result = (m_strict ? fhmi::tr("样例值 %1 · 换算失败") : fhmi::tr("样例值 %1 · 换算失败，显示原值")).arg(original);
    }
    m_bindingValue->setText(fhmi::tr("当前取值 · %1 · %2").arg(selection, result));
}

void ReceivePreview::refreshHex()
{
    QString result;
    const auto config = m_rawSample ? std::nullopt : hexFrameConfig(m_settings, &result);
    QList<DecodedSpan> rows;
    Message values;
    const auto sample = parseHex(m_input->toPlainText());
    const auto frame = sample.value_or(QByteArray{});
    QByteArray calculated;
    auto issue = HexFrameParser::Issue::None;
    bool valid = false;
    if (m_rawSample) {
        result = !sample ? fhmi::tr("示例中含有无效的 HEX 字符") : frame.isEmpty() ? fhmi::tr("输入一帧报文以查看解析结果")
            : fhmi::tr("样例报文 · %1 字节").arg(frame.size());
    } else if (config) {
        if (!sample)
            result = fhmi::tr("示例中含有无效的 HEX 字符");
        else if (frame.isEmpty())
            result = fhmi::tr("输入一帧报文以查看解析结果");
        else {
            HexFrameParser parser(*config);
            issue = parser.sampleIssue(frame, &calculated);
            valid = issue == HexFrameParser::Issue::None;
            if (valid) {
                const auto messages = parser.parse(frame);
                if (!messages.isEmpty())
                    values = messages.first();
                result = fhmi::tr("解析通过 · %1 字节").arg(frame.size());
            } else {
                result = frameProblem(issue, frame.size());
                if (issue == HexFrameParser::Issue::Length) {
                    const int expected = parser.declaredLength(frame);
                    if (expected > config->maxFrameLength)
                        result = fhmi::tr("整帧长度超限 · 声明 %1 B · 模板上限 %2 B").arg(expected).arg(config->maxFrameLength);
                    else if (expected > 0 && expected != frame.size())
                        result = fhmi::tr("整帧长度不符 · 收到 %1 B · 应为 %2 B").arg(frame.size()).arg(expected);
                }
            }
            if (valid || issue == HexFrameParser::Issue::Checksum || issue == HexFrameParser::Issue::Length)
                rows = decodeSpans(*config, frame, values, issue, m_showConfiguredFields);
        }
        if (rows.isEmpty() && m_showConfiguredFields)
            rows = decodeSpans(*config, {}, {}, HexFrameParser::Issue::Length, true);
    }
    QList<FrameByteSpan> spans;
    for (const auto &row : rows)
        if (row.bytes.kind != FrameByteKind::Field)
            spans.append(row.bytes);
    for (const auto &row : rows)
        if (row.bytes.kind == FrameByteKind::Field)
            spans.append(row.bytes);
    // A bad checksum still has known frame boundaries. A wrong length does not.
    const auto layout = config && (valid || issue == HexFrameParser::Issue::Checksum)
        ? config->layout() : HexFrameLayout{};
    m_bytes->setFrame(frame, spans, layout);
    m_diagram->setVisible(!frame.isEmpty());
    m_frameCoverage->setFrame(frame, layout, calculated);
    m_bytes->markRange(0, 0);
    displayRows(m_table, rows, frame, valid);
    const int visibleRows = std::clamp(m_table->rowCount(), 3, 6);
    m_table->setMinimumHeight(m_table->horizontalHeader()->sizeHint().height() +
                              visibleRows * m_table->verticalHeader()->defaultSectionSize() + 2 * m_table->frameWidth());
    m_table->setVisible(!m_rawSample && !rows.isEmpty());
    if (issue == HexFrameParser::Issue::Checksum || issue == HexFrameParser::Issue::Length) {
        const auto invalidPart = issue == HexFrameParser::Issue::Length ? FrameByteKind::Length : FrameByteKind::Checksum;
        for (int row = 0; row < rows.size(); ++row)
            if (rows.at(row).bytes.kind == invalidPart) {
                m_table->setCurrentCell(row, 0);
                break;
            }
    }
    if (!m_binding) {
        showBindingValue({}, {});
    } else if (m_binding->usesRawData()) {
        const auto range = frame.isEmpty() ? std::nullopt : m_binding->rawByteRange(frame);
        QString location = fhmi::tr("手动取值");
        if (range && range->size > 0) {
            m_bytes->markRange(range->offset, range->size);
            location = range->size == 1 ? fhmi::tr("整帧字节 %1").arg(range->offset)
                : fhmi::tr("整帧字节 %1–%2").arg(range->offset).arg(range->offset + range->size - 1);
        }
        showBindingValue(location, range ? m_binding->extractFromRaw(frame) : QVariant());
    } else {
        const QString field = m_binding->parseMode == ParseMode::JsonPath ? m_binding->jsonPath : m_binding->key;
        for (int row = 0; row < rows.size(); ++row) {
            const auto &decoded = rows.at(row);
            if (decoded.fieldIndex >= 0 && decoded.fieldName == field) {
                markBindingRow(row);
                m_bytes->markRange(decoded.bytes.offset, decoded.bytes.size);
                break;
            }
        }
        showBindingValue(field, valid ? m_binding->extractFromParsed(values) : QVariant());
    }
    if (valid)
        m_status->setTextColor(QColor(0x0F6D3C), QColor(0x72D499));
    else if (sample && (m_rawSample || (config && frame.isEmpty())))
        m_status->setTextColor(QColor(0x545454), QColor(0xC4C4C4));
    else
        m_status->setTextColor(QColor(0xC42B1C), QColor(0xFF99A4));
    m_status->setText(result);
}

void ReceivePreview::refreshText()
{
    auto utf8 = m_settings;
    utf8.insert(QStringLiteral("encoding"), QStringLiteral("utf-8"));
    const auto terminator = lineTerminator(utf8);
    auto bytes = m_input->toPlainText().toUtf8();
    if (!bytes.endsWith(terminator))
        bytes += terminator;
    m_table->setRowCount(0);
    QString error;
    QVariant value;
    if (const auto parser = createParser(QStringLiteral("line"), utf8, &error)) {
        for (const auto &message : parser->feed(bytes)) {
            if (m_binding && m_binding->usesParsedData()) {
                const auto decoded = m_binding->extractFromParsed(message);
                if (decoded.isValid())
                    value = decoded;
            }
            for (auto field = message.constBegin(); field != message.constEnd(); ++field)
                appendTextFields(m_table, field.key(), field.value(), false);
        }
    }
    m_status->setText(!error.isEmpty() ? error : m_table->rowCount() > 0
        ? fhmi::tr("解析出 %1 个字段或路径").arg(m_table->rowCount()) : fhmi::tr("未解析出字段"));
    if (!m_binding) {
        showBindingValue({}, {});
        return;
    }
    const bool path = m_binding->parseMode == ParseMode::JsonPath;
    const auto field = path ? m_binding->jsonPath : m_binding->key;
    for (int row = 0; row < m_table->rowCount(); ++row) {
        const auto *item = m_table->item(row, 0);
        if (item->text() == field && (path || !item->data(JsonPathRole).toBool())) {
            markBindingRow(row);
            break;
        }
    }
    showBindingValue(field, value);
}

namespace {

struct PreviewFrame
{
    QString id;
    QString name;
    QJsonObject settings;
    std::optional<QString> sample;
};

void openHexPreview(QList<PreviewFrame> frames, QWidget *parent, SaveReceivedField saveField,
                    QList<HexFrameVariant> variants = {}, const QByteArray &initialSample = {})
{
    const auto matchingIndex = [variants = std::move(variants)](QByteArrayView sample) {
        for (int i = 0; i < variants.size(); ++i)
            if (matchHexFrame(sample, variants.at(i).frame) == HexFrameMatch::Yes)
                return i;
        return -1;
    };
    const bool grouped = frames.size() > 1;
    const int initialIndex = initialSample.isEmpty() ? 0 : std::max(0, matchingIndex(initialSample));
    if (!initialSample.isEmpty())
        frames[initialIndex].sample = toHexString(initialSample);
    const auto &initial = frames.at(initialIndex);
    auto *dialog = new AppDialog(grouped || initial.name.isEmpty() ? tr("接收帧预览")
        : tr("接收帧预览 · %1").arg(initial.name), parent);
    dialog->setObjectName(QStringLiteral("receiveFramePreview"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setPreferredSize(QSize(980, grouped ? 700 : 640));
    dialog->viewLayout()->setSpacing(8);
    dialog->hideCancelButton();
    dialog->yesButton()->setText(tr("关闭"));
    FluentQt::ComboBox *choice = nullptr;
    if (grouped) {
        choice = new FluentQt::ComboBox(dialog->widget());
        choice->setObjectName(QStringLiteral("receivePreviewTemplate"));
        for (const auto &frame : frames)
            choice->addItem(frame.name, {}, frame.id);
        choice->setCurrentIndex(initialIndex);
        auto *row = new QHBoxLayout;
        auto *label = new FluentQt::BodyLabel(tr("接收模板"), dialog->widget());
        label->setBuddy(choice);
        choice->setAccessibleName(label->text());
        row->addWidget(label);
        row->addWidget(choice, 1);
        dialog->viewLayout()->addLayout(row);
    }
    auto *preview = addScrollablePreview(dialog, true);
    const bool selectsField = bool(saveField);
    if (saveField) {
        addSaveAction(dialog, preview, [choice, save = std::move(saveField)](const QString &field, bool json) {
            const auto id = choice ? choice->currentData().toString() : QString();
            save(id.isEmpty() ? field : hexTemplateFieldKey(id, field), json);
        });
    }
    const auto load = [preview, selectsField](const PreviewFrame &frame) {
        preview->loadFrameSample(frame.settings, frame.sample);
        if (selectsField)
            preview->selectOnlyDecodedField();
    };
    load(initial);
    if (choice) {
        const auto samples = std::make_shared<QList<PreviewFrame>>(std::move(frames));
        QObject::connect(choice, &FluentQt::ComboBox::currentIndexChanged, dialog,
                         [samples, load](int index) { load(samples->at(index)); });
        QObject::connect(preview, &ReceivePreview::sampleEdited, dialog, [choice, preview, samples] {
            (*samples)[choice->currentIndex()].sample = preview->savedSample();
        });
        QObject::connect(preview, &ReceivePreview::samplePasted, dialog,
                         [choice, preview, samples, matchingIndex, selectsField](const QString &previous) {
            const auto sample = preview->savedSample();
            const auto bytes = parseHex(sample);
            const int selected = choice->currentIndex();
            const int matched = bytes ? matchingIndex(*bytes) : -1;
            if (matched < 0 || matched == selected)
                return;
            (*samples)[selected].sample = previous.isNull() ? std::nullopt : std::optional(previous);
            auto &frame = (*samples)[matched];
            frame.sample = sample;
            const QSignalBlocker blocked(choice);
            choice->setCurrentIndex(matched);
            preview->setSettings(frame.settings);
            if (selectsField)
                preview->selectOnlyDecodedField();
        });
    }
    dialog->open();
}

} // namespace

void openReceiveFramePreview(const QJsonObject &settings, QWidget *parent, const QByteArray &sample,
                             SaveReceivedField saveField, const QString &templateName)
{
    openHexPreview({{{}, templateName, settings, sample.isEmpty() ? std::nullopt
                   : std::optional(toHexString(sample))}}, parent, std::move(saveField));
}

void openReceiveGroupPreview(const QJsonObject &settings, QWidget *parent, SaveReceivedField saveField,
                             const QByteArray &sample)
{
    QList<PreviewFrame> frames;
    for (const auto &value : settings.value(LinkParser::GroupTemplates).toArray()) {
        const auto entry = value.toObject();
        const auto example = entry.value(QStringLiteral("sample")).toString();
        frames.append({entry.value(QStringLiteral("id")).toString(), entry.value(QStringLiteral("name")).toString(),
                       entry.value(QStringLiteral("parserConfig")).toObject(),
                       example.isEmpty() ? std::nullopt : std::optional(example)});
    }
    if (!frames.isEmpty())
        openHexPreview(std::move(frames), parent, std::move(saveField),
                       hexFrameVariants(settings).value_or(QList<HexFrameVariant>{}), sample);
}

void openReceiveTextPreview(const QJsonObject &settings, QWidget *parent, const QString &sample,
                            SaveReceivedField saveField)
{
    auto *dialog = new AppDialog(tr("文本与 JSON 解析预览"), parent);
    dialog->setObjectName(QStringLiteral("receiveTextPreview"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setPreferredSize(QSize(740, 610));
    dialog->hideCancelButton();
    dialog->yesButton()->setText(tr("关闭"));
    auto *preview = addScrollablePreview(dialog, false);
    preview->setSettings(settings);
    preview->setSample(sample);
    addSaveAction(dialog, preview, std::move(saveField));
    dialog->open();
}

} // namespace fhmi
