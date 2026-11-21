#include "runtime/send_frame_fields.h"

#include "core/command_encoder.h"
#include "core/frame_templates.h"
#include "runtime/app_dialog.h"
#include "runtime/frame_bytes_view.h"

#include <FluentQtWidgets/Widgets/Button.h>
#include <FluentQtWidgets/Widgets/Label.h>
#include <FluentQtWidgets/Widgets/LineEdit.h>
#include <FluentQtWidgets/Widgets/ScrollArea.h>
#include <QtCore/QCoreApplication>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>

namespace fhmi {

std::optional<QJsonObject> editSendFrameFields(const QJsonObject &properties, const QJsonObject &library,
                                               const QJsonObject &values, QWidget *parent, SendFrameAction action)
{
    const auto tr = [](const char *message) { return QCoreApplication::translate("fhmi", message); };
    const auto entry = FrameTemplates::find(library, properties.value(FrameTemplates::SendId).toString());
    const auto fields = FrameTemplates::sendFields(library, properties);
    if (!entry || fields.isEmpty())
        return std::nullopt;
    const auto &settings = entry->format.settings;
    const auto config = hexFrameConfig(settings);
    const auto defaults = settings.value(FrameTemplates::SendDefaults).toObject();
    const auto required = FrameTemplates::requiredFields(settings);

    AppDialog dialog(tr("按模板字段填写 · %1").arg(entry->name), parent);
    dialog.setObjectName(QStringLiteral("manualSendFieldsDialog"));
    dialog.setPreferredSize({820, 398 + 38 * int(std::min<qsizetype>(fields.size(), 5))});
    switch (action) {
    case SendFrameAction::Preview:
        dialog.hideCancelButton();
        dialog.yesButton()->setText(tr("关闭"));
        break;
    case SendFrameAction::Send:
        dialog.yesButton()->setText(tr("发送"));
        break;
    case SendFrameAction::StartPeriodic:
        dialog.yesButton()->setText(tr("开始周期发送"));
        break;
    case SendFrameAction::ApplyPeriodic:
        dialog.yesButton()->setText(tr("应用"));
        break;
    }
    auto *scroll = new FluentQt::ScrollArea(dialog.widget());
    scroll->setWidgetResizable(true);
    scroll->enableTransparentBackground();
    scroll->setFrameShape(QFrame::NoFrame);
    auto *content = new QWidget(scroll);
    auto *body = new QVBoxLayout(content);
    body->setContentsMargins(0, 0, 8, 0);
    body->setSizeConstraint(QLayout::SetMinimumSize);
    auto *form = new QFormLayout;
    body->addLayout(form);
    scroll->setWidget(content);
    dialog.viewLayout()->addWidget(scroll, 1);

    auto *preview = new QWidget(content);
    auto *previewLayout = new QVBoxLayout(preview);
    previewLayout->setContentsMargins(0, 0, 0, 0);
    auto *modes = new QHBoxLayout;
    previewLayout->addLayout(modes);
    auto *diagram = new FrameDiagram(preview, modes);
    previewLayout->addWidget(diagram);
    auto *coverage = new FrameCoveragePreview(preview);
    previewLayout->addWidget(coverage);
    body->addWidget(preview);
    auto *status = new FluentQt::CaptionLabel(content);
    status->setWordWrap(true);
    body->addWidget(status);
    body->addStretch();

    QMap<QString, QLineEdit *> editors;
    QJsonObject result;
    const auto refresh = [&] {
        result = {};
        for (auto it = editors.cbegin(); it != editors.cend(); ++it)
            if (!it.value()->text().isEmpty())
                result.insert(it.key(), it.value()->text());
        CommandEncoder::BinaryFrameLayout layout;
        const auto command = CommandEncoder::frameFromFieldValues(properties, result, library, &layout);
        preview->setVisible(command.command.has_value());
        status->setText(command.command ? command.command->framingNotice : command.error);
        status->setVisible(!status->text().isEmpty());
        if (!command.command)
            return false;
        const auto &bytes = command.command->bytes;
        auto spans = frameEnvelopeSpans(layout.frame, bytes.size());
        for (qsizetype index = 0; index < layout.fields.size(); ++index) {
            const auto &field = layout.fields.at(index);
            spans.append({field.name, field.name, field.offset, field.size, FrameByteKind::Field, int(index)});
        }
        if (config && !config->matchBytes.isEmpty())
            spans.append({QStringLiteral("identifier"), tr("命令字"), config->matchOffset,
                          config->matchBytes.size(), FrameByteKind::Fixed});
        diagram->byteView()->setFrame(bytes, spans, layout.frame);
        coverage->setFrame(bytes, layout.frame);
        return true;
    };
    for (const auto &field : fields) {
        auto *edit = new FluentQt::LineEdit(content);
        edit->setText(values.value(field.name).toString(defaults.value(field.name).toString()));
        edit->setPlaceholderText(field.type);
        const auto caption = required.contains(field.name) ? tr("%1（必填）").arg(field.name) : field.name;
        edit->setAccessibleName(caption);
        form->addRow(caption, edit);
        editors.insert(field.name, edit);
        QObject::connect(edit, &QLineEdit::textChanged, &dialog, [&, name = field.name] {
            refresh();
            diagram->byteView()->selectPart(name);
        });
    }
    QObject::connect(diagram->byteView(), &FrameBytesView::partClicked, &dialog, [&](const QString &key) {
        if (auto *edit = editors.value(key))
            edit->setFocus();
    });
    dialog.setValidator([&] { return action == SendFrameAction::Preview || refresh(); });
    refresh();
    return dialog.exec() == QDialog::Accepted ? std::optional(result) : std::nullopt;
}

} // namespace fhmi
