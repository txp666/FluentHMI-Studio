#include "designer/transmit_frame_editor.h"

#include "core/can_frame.h"
#include "core/protocol_parser.h"
#include "designer/receive_frame_fields.h"
#include "runtime/app_dialog.h"
#include "runtime/frame_bytes_view.h"
#include "runtime/ui_helpers.h"

#include <FluentQtWidgets/Widgets/Button.h>
#include <FluentQtWidgets/Widgets/ComboBox.h>
#include <FluentQtWidgets/Widgets/Label.h>
#include <FluentQtWidgets/Widgets/LineEdit.h>
#include <FluentQtWidgets/Widgets/ScrollArea.h>
#include <FluentQtWidgets/Widgets/SpinBox.h>
#include <QtCore/QCoreApplication>
#include <QtGui/QFontDatabase>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>

namespace fhmi {
namespace {
QString tr(const char *s) { return QCoreApplication::translate("fhmi", s); }
} // namespace

QWidget *createTransmitFrameRules(QWidget *parent, const QJsonObject &config, int defaultOffset,
                                  const TransmitFrameChanged &changed, const RegisterTransmitControl &registerControl)
{
    auto *scroll = new FluentQt::ScrollArea(parent);
    scroll->setWidgetResizable(true);
    scroll->enableTransparentBackground();
    auto *panel = new QWidget(scroll);
    auto *body = new QVBoxLayout(panel);
    body->setContentsMargins(0, 0, 8, 0);
    body->setSpacing(8);
    auto *columns = new QHBoxLayout;
    columns->setSpacing(14);
    body->addLayout(columns);
    auto *lengthForm = new QFormLayout;
    auto *checkForm = new QFormLayout;
    lengthForm->setFormAlignment(Qt::AlignTop);
    checkForm->setFormAlignment(Qt::AlignTop);
    columns->addLayout(lengthForm, 1);
    columns->addLayout(checkForm, 1);
    auto *hint = new FluentQt::BodyLabel(tr("偏移从帧首的第 0 字节开始。"), panel);
    hint->setWordWrap(true);
    body->addWidget(hint);
    body->addStretch();

    const auto number = [panel, config](const QString &key, int minimum, int maximum, int fallback) {
        auto *edit = new FluentQt::SpinBox(panel);
        edit->setObjectName(QStringLiteral("txFrame_%1").arg(key));
        edit->setRange(minimum, maximum);
        edit->setValue(config.value(key).toInt(fallback));
        return edit;
    };
    const auto choice = [panel, config](const QString &key, const QStringList &choices) {
        auto *edit = new FluentQt::ComboBox(panel);
        edit->setObjectName(QStringLiteral("txFrame_%1").arg(key));
        addValueItems(edit, choices);
        selectValue(edit, config.value(key).toString(choices.first()));
        return edit;
    };
    auto *lengthMode = new FluentQt::ComboBox(panel);
    lengthMode->setObjectName(QStringLiteral("txFrame_lengthMode"));
    lengthMode->addItems({tr("无"), tr("固定帧长"), tr("长度字段")});
    lengthMode->setCurrentIndex(config.value(QStringLiteral("fixedLength")).toInt() > 0 ? 1
        : config.value(QStringLiteral("lengthOffset")).toInt(-1) >= 0 ? 2 : 0);
    auto *fixed = number(QStringLiteral("fixedLength"), 1, 65536, 8);
    auto *offset = number(QStringLiteral("lengthOffset"), 0, 65535, defaultOffset);
    auto *width = new FluentQt::ComboBox(panel);
    width->setObjectName(QStringLiteral("txFrame_lengthSize"));
    for (int size : {1, 2})
        width->addItem(tr("%1 字节").arg(size), {}, QString::number(size));
    selectValue(width, QString::number(config.value(QStringLiteral("lengthSize")).toInt(1)));
    auto *order = choice(QStringLiteral("lengthEndian"), HexOptions::byteOrders());
    auto *covers = choice(QStringLiteral("lengthCovers"), HexOptions::lengthCovers());
    auto *checksum = choice(QStringLiteral("checksum"), HexOptions::checksums());
    auto *start = number(QStringLiteral("checksumStart"), 0, 65535, 0);
    auto *checkOrder = choice(QStringLiteral("checksumEndian"), HexOptions::byteOrders());
    const auto markedLabel = [panel](const QString &label, FrameByteKind kind, QWidget *buddy) {
        auto *row = new QWidget(panel);
        auto *line = new QHBoxLayout(row);
        line->setContentsMargins(0, 0, 0, 0);
        line->setSpacing(5);
        line->addWidget(frameByteMarker(row, kind));
        auto *caption = new FluentQt::BodyLabel(label, row);
        caption->setBuddy(buddy);
        line->addWidget(caption);
        line->addStretch();
        return row;
    };
    lengthForm->addRow(markedLabel(tr("长度规则"), FrameByteKind::Length, lengthMode), lengthMode);
    lengthForm->addRow(tr("固定帧长"), fixed);
    lengthForm->addRow(tr("长度插入偏移"), offset);
    lengthForm->addRow(tr("长度字段字节数"), width);
    lengthForm->addRow(tr("长度字节序"), order);
    lengthForm->addRow(tr("长度含义"), covers);
    checkForm->addRow(markedLabel(tr("校验算法"), FrameByteKind::Checksum, checksum), checksum);
    checkForm->addRow(tr("校验起始字节"), start);
    checkForm->addRow(tr("校验值字节序"), checkOrder);
    checkForm->addRow(createChecksumRuleHint(checksum, panel));
    if (registerControl) {
        for (auto *control : QList<QWidget *>{lengthMode, offset, width, order, covers})
            registerControl(QStringLiteral("length"), control);
        for (auto *control : QList<QWidget *>{checksum, start, checkOrder})
            registerControl(QStringLiteral("checksum"), control);
    }
    const auto update = [changed, lengthForm, checkForm, lengthMode, fixed, offset, width, order, covers,
                         checksum, start, checkOrder](bool save) {
        const int mode = lengthMode->currentIndex();
        const bool field = mode == 2;
        const auto check = HexChecksum(checksum->currentIndex());
        lengthForm->setRowVisible(fixed, mode == 1);
        for (QWidget *edit : QList<QWidget *>{offset, width, covers})
            lengthForm->setRowVisible(edit, field);
        lengthForm->setRowVisible(order, field && width->currentIndex() == 1);
        checkForm->setRowVisible(start, check != HexChecksum::None);
        checkForm->setRowVisible(checkOrder, hexChecksumSize(check) == 2);
        if (!save)
            return;
        QJsonObject values;
        if (mode == 1)
            values.insert(QStringLiteral("fixedLength"), fixed->value());
        if (field) {
            values.insert(QStringLiteral("lengthOffset"), offset->value());
            values.insert(QStringLiteral("lengthSize"), selectedValue(width).toInt());
            values.insert(QStringLiteral("lengthEndian"), selectedValue(order));
            values.insert(QStringLiteral("lengthCovers"), selectedValue(covers));
        }
        if (check != HexChecksum::None) {
            values.insert(QStringLiteral("checksum"), selectedValue(checksum));
            values.insert(QStringLiteral("checksumStart"), start->value());
            values.insert(QStringLiteral("checksumEndian"), selectedValue(checkOrder));
        }
        changed(values);
    };
    for (auto *edit : {lengthMode, width, order, covers, checksum, checkOrder})
        QObject::connect(edit, &FluentQt::ComboBox::currentIndexChanged, panel, [update] { update(true); });
    for (auto *edit : {fixed, offset, start})
        QObject::connect(edit, &QSpinBox::valueChanged, panel, [update] { update(true); });
    update(false);
    scroll->setWidget(panel);
    return scroll;
}

std::optional<QJsonObject> editTransmitFrameFormat(const QJsonObject &properties, bool can, QWidget *parent)
{
    AppDialog dialog(tr("可视化发送帧格式"), parent);
    dialog.setObjectName(QStringLiteral("transmitFrameFormatEditor"));
    dialog.setPreferredSize(QSize(970, 680));
    dialog.yesButton()->setText(tr("应用帧格式"));
    auto *form = new QFormLayout;
    dialog.viewLayout()->addLayout(form);
    auto *head = new FluentQt::LineEdit(&dialog);
    auto *tail = new FluentQt::LineEdit(&dialog);
    if (can) {
        head->hide();
        tail->hide();
    } else {
        head->setText(properties.value(QStringLiteral("frameHeader")).toString());
        tail->setText(properties.value(QStringLiteral("frameTail")).toString());
        form->addRow(tr("帧头(HEX)"), head);
        form->addRow(tr("帧尾(HEX)"), tail);
    }
    auto *payload = new FluentQt::LineEdit(&dialog);
    payload->setText(QStringLiteral("01 02 03"));
    payload->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    form->addRow(tr("示例载荷（HEX）"), payload);
    auto *status = new FluentQt::CaptionLabel(&dialog);
    status->setWordWrap(true);
    dialog.viewLayout()->addWidget(status);
    auto *diagram = new FrameDiagram(&dialog);
    auto *bytes = diagram->byteView();
    dialog.viewLayout()->addWidget(diagram);
    auto *coveragePreview = new FrameCoveragePreview(&dialog);
    dialog.viewLayout()->addWidget(coveragePreview);
    auto *problem = new FluentQt::CaptionLabel(&dialog);
    problem->setWordWrap(true);
    problem->setTextColor(QColor(0xC42B1C), QColor(0xFF99A4));
    problem->hide();
    auto config = properties.value(QStringLiteral("txFrame")).toObject();
    const bool fd = properties.value(QStringLiteral("canFd")).toBool();
    const auto assemble = [&](const std::optional<QByteArray> &body, HexFrameLayout *layout, QString *error) -> std::optional<QByteArray> {
        const auto header = parseHex(head->text());
        const auto footer = parseHex(tail->text());
        if (!body || !header || !footer) {
            *error = tr("示例载荷、帧头或帧尾含有无效的 HEX 字符");
            return std::nullopt;
        }
        auto frame = buildHexFrame(*body, *header, *footer, config, error, layout);
        if (frame && can && (fd ? !Can::padFdPayload(*frame) : frame->size() > 8)) {
            *error = tr("发送数据超出 CAN 帧的载荷上限");
            return std::nullopt;
        }
        return frame;
    };
    const auto refresh = [&] {
        HexFrameLayout layout;
        QString error;
        const auto frame = assemble(parseHex(payload->text()), &layout, &error);
        status->setText(frame ? tr("组帧结果 · %1 字节").arg(frame->size()) : error);
        status->setTextColor(frame ? QColor(0x16663B) : QColor(0xC42B1C),
                             frame ? QColor(0x72D499) : QColor(0xFF99A4));
        if (frame)
            bytes->setFrame(*frame, frameEnvelopeSpans(layout, frame->size()), layout);
        coveragePreview->setFrame(frame.value_or(QByteArray{}), layout);
        diagram->setVisible(frame.has_value());
        problem->hide();
    };
    auto *rules = createTransmitFrameRules(&dialog, config, int(parseHex(head->text()).value_or(QByteArray{}).size()),
        [&](const QJsonObject &edited) {
            config = edited;
            refresh();
        });
    dialog.viewLayout()->addWidget(rules, 1);
    dialog.viewLayout()->addWidget(problem);
    for (auto *entry : {head, tail, payload})
        QObject::connect(entry, &QLineEdit::textChanged, &dialog, refresh);
    refresh();
    dialog.setValidator([&] {
        const auto header = parseHex(head->text());
        const auto footer = parseHex(tail->text());
        const int lengthOffset = config.value(QStringLiteral("lengthOffset")).toInt(-1);
        const int lengthWidth = lengthOffset >= 0 ? config.value(QStringLiteral("lengthSize")).toInt(1) : 0;
        const auto checksum = HexChecksum(std::max(0, int(HexOptions::checksums().indexOf(config.value(QStringLiteral("checksum")).toString()))));
        const int fixed = config.value(QStringLiteral("fixedLength")).toInt();
        const int start = config.value(QStringLiteral("checksumStart")).toInt();
        const int headSize = int(header.value_or(QByteArray{}).size());
        const int tailSize = int(footer.value_or(QByteArray{}).size());
        const int required = fixed > 0 ? fixed - headSize - tailSize - hexChecksumSize(checksum) :
            std::max({0, lengthOffset - headSize, start - headSize - lengthWidth});
        QString error;
        const auto probe = assemble(QByteArray(std::max(0, required), '\x01'), nullptr, &error);
        problem->setText(error);
        problem->setVisible(!probe);
        return bool(probe);
    });
    if (dialog.exec() != QDialog::Accepted)
        return std::nullopt;
    QJsonObject changes;
    if (properties.value(QStringLiteral("txFrame")).toObject() != config)
        changes.insert(QStringLiteral("txFrame"), config);
    if (!can && head->text() != properties.value(QStringLiteral("frameHeader")).toString())
        changes.insert(QStringLiteral("frameHeader"), head->text());
    if (!can && tail->text() != properties.value(QStringLiteral("frameTail")).toString())
        changes.insert(QStringLiteral("frameTail"), tail->text());
    return changes;
}

} // namespace fhmi
