#include "designer/command_preview.h"
#include "comm/can/can_initial_state.h"

#include "core/can_frame.h"
#include "core/command_encoder.h"
#include "core/i18n.h"
#include "core/manual_command.h"
#include "core/project.h"
#include "designer/canvas_item.h"
#include "runtime/app_dialog.h"

#include <FluentQtWidgets/Widgets/ComboBox.h>
#include <FluentQtWidgets/Widgets/Label.h>
#include <FluentQtWidgets/Widgets/LineEdit.h>
#include <FluentQtWidgets/Widgets/ScrollArea.h>
#include <FluentQtWidgets/Widgets/SpinBox.h>

#include <QtCore/QCoreApplication>
#include <QtCore/QLocale>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QVBoxLayout>
#include <QtCore/qalgorithms.h>

namespace fhmi {

namespace {

QString tr(const char *source) { return QCoreApplication::translate("fhmi", source); }

QString hexBytes(const QByteArray &bytes) { return QString::fromLatin1(bytes.toHex(' ').toUpper()); }

QString modbusFunction(int function) { return translated(CommandEncoder::functionOptions({function}).first()); }

QString previewCan(const CanCommand &command, const QList<CanPreviewSource> &configuredSources,
                   BinaryFrameExample *example)
{
    // A raw CAN ID can be previewed before the project has any CAN cards.
    const auto sources = configuredSources.isEmpty() ? QList<CanPreviewSource>{{{}, CanSignalSet{}, {}}}
                                                     : configuredSources;
    QStringList lines{tr("CAN 报文 %1").arg(command.target)};
    QByteArray rawFrame;
    bool onlyRaw = true;
    for (const auto &source : sources) {
        QString error = source.error;
        const auto payload = source.signalSet ? source.signalSet->preparePayload(command, &error) : std::nullopt;
        if (!payload) {
            return source.connectionId.isEmpty() ? error : source.connectionId + QStringLiteral(" · ") + error;
        }
        const CanId id{payload->frame.id, payload->frame.extended};
        const QString format = QStringLiteral("%1 · %2 · %3")
            .arg(Can::idText(id), payload->frame.fd ? QStringLiteral("CAN FD") : QStringLiteral("CAN"),
                 id.extended ? tr("扩展帧") : tr("标准帧"));
        const QString target = source.connectionId.isEmpty() ? format : source.connectionId + QStringLiteral(" · ") + format;
        if (!lines.contains(target))
            lines.append(target);
        const QString result = payload->signalMask.isEmpty() ? tr("数据 %1").arg(hexBytes(payload->frame.data))
                                                             : tr("信号赋值 %1").arg(command.assignments);
        onlyRaw &= payload->signalMask.isEmpty();
        rawFrame = payload->frame.data;
        if (!lines.contains(result))
            lines.append(result);
        if (!payload->signalMask.isEmpty()) {
            int bits = 0;
            for (quint8 byte : payload->signalMask)
                bits += qPopulationCount(byte);
            lines.append(tr("本次写入 %1/%2 位").arg(bits).arg(payload->frame.data.size() * 8));
            if (!source.initialState.isEmpty()) {
                const auto initial = CanInitialState::build(*source.signalSet, source.initialState, &error);
                if (!initial)
                    return error;
                if (initial->hasRule(id))
                    lines.append(tr("首次发送需要有效反馈"));
            }
        }
    }
    if (example && onlyRaw) {
        example->applicationSize = command.payload->size();
        example->bytes = rawFrame;
        example->caption = lines.size() > 1 ? lines.at(1) : lines.first();
    }
    return lines.join(QLatin1Char('\n'));
}

QString previewText(const CommandResult &result, const QString &mode, const QList<CanPreviewSource> &canSources,
                    bool showInputValues = true, BinaryFrameExample *example = nullptr)
{
    if (!result.command)
        return result.error.isEmpty() ? tr("未配置发送内容") : result.error;

    if (example && mode == QLatin1String("HEX")) {
        example->bytes = result.command->bytes;
        example->applicationSize = example->bytes.size();
    }
    QString text = result.command->kind == EncodedCommand::Kind::Can ? previewCan(result.command->can, canSources, example)
                                                                     : CommandEncoder::describe(*result.command, mode);
    if (showInputValues && !result.command->inputValues.isEmpty())
        text += QStringLiteral("\n\n") + CommandEncoder::inputValuesText(*result.command, true);
    if (!result.command->framingNotice.isEmpty())
        text += QStringLiteral("\n\n") + result.command->framingNotice;
    return text;
}

void addSampleField(QFormLayout *form, const QString &label, QWidget *editor)
{
    auto *caption = new FluentQt::BodyLabel(label, editor->parentWidget());
    caption->setBuddy(editor);
    editor->setAccessibleName(label);
    form->addRow(caption, editor);
}

} // namespace

CommandTargetPreview previewCommandTarget(const QJsonObject &properties, const QString &type, const Project &project)
{
    const auto &widgets = project.widgets;
    const auto projectParser = project.parserType();
    const QString mode = properties.value(QStringLiteral("commandMode")).toString();
    const DataRoute route{properties.value(QStringLiteral("commandTargetId")).toString(),
                          properties.value(QStringLiteral("commandEndpoint")).toString()};
    const auto filter = CommandEncoder::sendRouteFilter(properties, type);
    CommandTargetPreview result;
    const auto resolved = resolveProjectRoute(widgets, projectParser, filter, route, RoutePurpose::Send, &result.error);
    if (!resolved) {
        result.routeInvalid = true;
        return result;
    }
    result.route = *resolved;
    QStringList names;
    const auto targets = routeConnections(widgets, filter, !route.endpoint.isEmpty(), projectParser);
    visitWidgets(widgets, [&](const QJsonObject &card) {
        const QString id = card.value(QStringLiteral("id")).toString();
        if (!result.route.acceptsConnection(id) ||
            !std::any_of(targets.cbegin(), targets.cend(), [&](const auto &target) { return target.first == id; }))
            return true;
        QString name = id;
        if (mode == QLatin1String("Modbus"))
            name += QStringLiteral(" · ") + modbusRouteDescription(card, project);
        names.append(name);
        return true;
    });
    if (names.isEmpty()) {
        result.error = tr("尚未配置发送连接");
    } else {
        result.label = (result.route.isBroadcast() ? tr("广播到 %1") : tr("目标连接 %1")).arg(names.join(QStringLiteral(", ")));
        if (!route.endpoint.isEmpty())
            result.label += QStringLiteral(" · ") + route.endpoint;
    }
    return result;
}

QString modbusSwitchWriteSummary(const QJsonObject &properties)
{
    const auto on = CommandEncoder::modbusSwitchFromProperties(properties, true);
    const auto off = CommandEncoder::modbusSwitchFromProperties(properties, false);
    if (!on.command || !off.command)
        return on.command ? off.error : on.error;
    return tr("%1\n开 %2 · 关 %3").arg(modbusFunction(on.command->modbus.func),
                                     CommandEncoder::modbusValueText(on.command->modbus),
                                     CommandEncoder::modbusValueText(off.command->modbus));
}

QString previewInputCommand(const QJsonObject &properties, const QList<CanPreviewSource> &canSources,
                            const QMap<QString, CommandEncoder::InputValue> &inputs, bool showInputValues,
                            BinaryFrameExample *example, const QJsonObject &frameTemplates)
{
    const auto read = [&inputs](const QString &id) -> std::optional<CommandEncoder::InputValue> {
        const auto value = inputs.constFind(id);
        return value == inputs.cend() ? std::nullopt : std::optional(*value);
    };
    if (example)
        *example = {};
    return previewText(CommandEncoder::buttonFromProperties(properties, read, example ? &example->layout : nullptr,
                                                            frameTemplates),
                       properties.value(QStringLiteral("commandMode")).toString(), canSources, showInputValues, example);
}

void openCommandPreview(const CanvasItem &item, const QList<CanPreviewSource> &canSources,
                        const CommandTargetPreview &destination, const QMap<QString, CommandEncoder::InputValue> &inputDefaults,
                        QWidget *parent, const QJsonObject &frameTemplates)
{
    const QString type = item.typeName();
    const QString mode = item.propertyValue(QStringLiteral("commandMode")).toString();
    const QJsonObject properties = item.properties();
    auto *dialog = new AppDialog(tr("发送预览"), parent);
    dialog->setObjectName(QStringLiteral("commandPreviewDialog"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setPreferredSize(QSize(580, 390));
    dialog->hideCancelButton();
    dialog->yesButton()->setText(tr("关闭"));
    auto *layout = dialog->viewLayout();
    auto *target = new FluentQt::BodyLabel(destination.error.isEmpty() ? destination.label : destination.error, dialog);
    if (const int interval = CommandEncoder::periodicSendInterval(properties); interval > 0 && destination.error.isEmpty()) {
        target->setText(target->text() + QStringLiteral(" · ") + tr("手动周期 %1 ms").arg(interval));
        if (type == QLatin1String("DataSendCard") || CommandEncoder::supportsPeriodicInputs(properties))
            target->setText(target->text() + QStringLiteral(" · ") + tr("每轮取当前值"));
    }
    if (!destination.error.isEmpty())
        target->setTextColor(QColor(0xC42B1C), QColor(0xFF99A4));
    target->setWordWrap(true);
    layout->addWidget(target);
    if (destination.routeInvalid) {
        dialog->setPreferredSize(QSize(540, 215));
        layout->addStretch();
        dialog->open();
        return;
    }
    auto *sampleForm = new QFormLayout;
    sampleForm->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    auto *output = new FluentQt::PlainTextEdit(dialog);
    output->setObjectName(QStringLiteral("commandPreviewOutput"));
    output->setAccessibleName(tr("发送预览"));
    output->setReadOnly(true);
    const auto resultText = [mode, canSources](const CommandResult &result) { return previewText(result, mode, canSources); };
    const auto renderValue = [output, properties, resultText, frameTemplates](const QString &value) {
        output->setPlainText(value.isEmpty() ? tr("请输入示例值")
            : resultText(CommandEncoder::encode(properties, value, nullptr, frameTemplates)));
    };

    if (type == QLatin1String("DataSendCard")) {
        auto *sample = new FluentQt::LineEdit(dialog);
        addSampleField(sampleForm, tr("示例值"), sample);
        const auto update = [output, properties, resultText, mode, frameTemplates](const QString &input) {
            output->setPlainText(input.trimmed().isEmpty()
                ? (mode == QLatin1String("CAN") ? tr("CAN 链路请按 ID#数据 格式输入") : tr("请输入示例值"))
                : resultText(ManualCommand::encode(properties, input, frameTemplates)));
        };
        QObject::connect(sample, &QLineEdit::textChanged, dialog, update);
        update({});
    } else if (type == QLatin1String("ToggleSwitch")) {
        output->setPlainText(tr("拨到开\n%1\n\n拨到关\n%2")
                                 .arg(resultText(CommandEncoder::switchFromProperties(properties, true, frameTemplates)),
                                      resultText(CommandEncoder::switchFromProperties(properties, false, frameTemplates))));
    } else if (type == QLatin1String("PushButton")) {
        const auto inputs = CommandEncoder::commandInputIds(properties);
        if (inputs.isEmpty()) {
            output->setPlainText(resultText(CommandEncoder::buttonFromProperties(properties, {}, nullptr, frameTemplates)));
        } else {
            auto *scroll = new FluentQt::ScrollArea(dialog);
            scroll->setWidgetResizable(true);
            scroll->setFrameShape(QFrame::NoFrame);
            scroll->setMaximumHeight(150);
            scroll->enableTransparentBackground();
            auto *content = new QWidget(scroll);
            content->setLayout(sampleForm);
            QMap<QString, QLineEdit *> editors;
            for (const auto &id : inputs) {
                if (!inputDefaults.contains(id) || editors.contains(id))
                    continue;
                auto *editor = new FluentQt::LineEdit(content);
                editor->setPlaceholderText(tr("请输入示例值"));
                const auto sample = inputDefaults.value(id);
                editor->setText(sample.text());
                addSampleField(sampleForm, sample.title.isEmpty() ? id : sample.title, editor);
                editors.insert(id, editor);
            }
            scroll->setWidget(content);
            layout->addWidget(scroll);
            scroll->setVisible(!editors.isEmpty());
            sampleForm = nullptr;
            const auto update = [properties, editors, output, canSources, frameTemplates] {
                QMap<QString, CommandEncoder::InputValue> values;
                for (auto it = editors.cbegin(); it != editors.cend(); ++it)
                    values.insert(it.key(), {it.value()->text(), it.value()->accessibleName()});
                output->setPlainText(previewInputCommand(properties, canSources, values, true, nullptr, frameTemplates));
            };
            for (auto *editor : editors)
                QObject::connect(editor, &QLineEdit::textChanged, dialog, update);
            update();
        }
    } else if (type == QLatin1String("ComboBox")) {
        const auto options = CommandEncoder::selectionOptions(item.propertyValue(QStringLiteral("options")).toArray());
        auto *sample = new FluentQt::ComboBox(dialog);
        for (const auto &option : options)
            sample->addItem(option.label);
        const int index = item.propertyValue(QStringLiteral("currentIndex")).toInt();
        sample->setCurrentIndex(index >= 0 && index < options.size() ? index : options.isEmpty() ? -1 : 0);
        const auto update = [output, properties, options, resultText, frameTemplates](int index) {
            output->setPlainText(index < 0 ? tr("请先配置选项")
                : resultText(CommandEncoder::selectionFromProperties(properties, options.at(index), frameTemplates)));
        };
        addSampleField(sampleForm, tr("示例选项"), sample);
        QObject::connect(sample, &FluentQt::ComboBox::currentIndexChanged, dialog, update);
        update(sample->currentIndex());
    } else if (type == QLatin1String("LineEdit")) {
        auto *sample = new FluentQt::LineEdit(dialog);
        sample->setText(item.propertyValue(QStringLiteral("text")).toVariant().toString());
        addSampleField(sampleForm, tr("示例值"), sample);
        QObject::connect(sample, &QLineEdit::textChanged, dialog, [renderValue](const QString &text) { renderValue(text.trimmed()); });
        renderValue(sample->text().trimmed());
    } else {
        const double min = item.propertyValue(QStringLiteral("minimum")).toDouble();
        const double max = item.propertyValue(QStringLiteral("maximum")).toDouble(100);
        const double value = item.propertyValue(QStringLiteral("value")).toDouble();
        if (item.propertyValue(QStringLiteral("numberMode")).toString() == QStringLiteral("小数")) { // i18n: value
            auto *sample = new FluentQt::DoubleSpinBox(dialog);
            sample->setDecimals(item.propertyValue(QStringLiteral("decimals")).toInt(2));
            sample->setRange(min, max);
            sample->setValue(value);
            const auto update = [renderValue](double value) { renderValue(QString::number(value, 'g', QLocale::FloatingPointShortest)); };
            addSampleField(sampleForm, tr("示例值"), sample);
            QObject::connect(sample, qOverload<double>(&QDoubleSpinBox::valueChanged), dialog, update);
            update(sample->value());
        } else {
            auto *sample = new FluentQt::SpinBox(dialog);
            sample->setRange(qRound(min), qRound(max));
            sample->setValue(qRound(value));
            const auto update = [renderValue](int value) { renderValue(QString::number(value)); };
            addSampleField(sampleForm, tr("示例值"), sample);
            QObject::connect(sample, qOverload<int>(&QSpinBox::valueChanged), dialog, update);
            update(sample->value());
        }
    }
    if (sampleForm)
        layout->addLayout(sampleForm);
    layout->addWidget(output, 1);
    dialog->open();
}

} // namespace fhmi
