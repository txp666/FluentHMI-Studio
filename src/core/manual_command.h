#pragma once

#include "core/command_encoder.h"
#include "core/frame_templates.h"

#include <QtCore/QCoreApplication>

namespace fhmi::ManualCommand {

inline const QString Text = QStringLiteral("文本"); // i18n: value
inline const QString Hex = QStringLiteral("HEX");
inline const QString Can = QStringLiteral("CAN");

inline QStringList modes() { return {Text, Hex, Can}; }

inline bool hasFraming(const QJsonObject &properties)
{
    return !properties.value(QStringLiteral("txFrame")).toObject().isEmpty() ||
           !properties.value(QStringLiteral("frameHeader")).toString().trimmed().isEmpty() ||
           !properties.value(QStringLiteral("frameTail")).toString().trimmed().isEmpty();
}

inline QStringList newlines()
{
    return {QStringLiteral("无"), QStringLiteral("\\r\\n"), QStringLiteral("\\n"), QStringLiteral("\\r")}; // i18n: value
}

inline CommandResult encode(const QJsonObject &props, const QString &input, const QJsonObject &frameTemplates = {})
{
    const QString value = input.trimmed();
    if (value.isEmpty())
        return CommandResult::failure(QCoreApplication::translate("fhmi", "请输入数据"));
    const QString mode = props.value(QStringLiteral("commandMode")).toString(Text);
    const QString target = props.value(QStringLiteral("commandTargetId")).toString();
    const QString endpoint = props.value(QStringLiteral("commandEndpoint")).toString();
    if (mode == Can) {
        QString error;
        QString framingNotice;
        const auto settings = FrameTemplates::sendSettings(frameTemplates, props);
        auto frame = fhmi::Can::parseSendText(value, &error, !settings);
        if (!frame)
            return CommandResult::failure(error);
        if (settings && !frame->rtr) {
            auto payload = FrameTemplates::buildSendFrame(frame->data, *settings, &error);
            if (!payload)
                return CommandResult::failure(error);
            if (frame->fd ? !fhmi::Can::padFdPayload(*payload) : payload->size() > 8)
                return CommandResult::failure(QCoreApplication::translate("fhmi", "发送数据超出 CAN 帧的载荷上限"));
            framingNotice = FrameTemplates::sendMatchNotice(*payload, *settings);
            frame->data = std::move(*payload);
        }
        EncodedCommand command;
        command.kind = EncodedCommand::Kind::CanFrame;
        command.frame = *frame;
        command.route = {target, endpoint};
        command.framingNotice = std::move(framingNotice);
        return CommandResult::ok(std::move(command));
    }
    if (mode != Text && mode != Hex)
        return CommandResult::failure(QCoreApplication::translate("fhmi", "未知发送模式：%1").arg(mode));

    QJsonObject encoding{{QStringLiteral("commandMode"), mode},
                         {QStringLiteral("commandNewline"), props.value(QStringLiteral("commandNewline"))},
                         {FrameTemplates::SendId, props.value(FrameTemplates::SendId)},
                         {QStringLiteral("commandTargetId"), target},
                         {QStringLiteral("commandEndpoint"), endpoint}};
    if (mode == Hex)
        for (const auto &key : {QStringLiteral("frameHeader"), QStringLiteral("frameTail"), QStringLiteral("txFrame")})
            encoding.insert(key, props.value(key));
    auto result = CommandEncoder::encode(encoding, value, nullptr, frameTemplates);
    if (result.command && result.command->bytes.isEmpty())
        return CommandResult::failure(QCoreApplication::translate("fhmi", "HEX格式错误"));
    return result;
}

} // namespace fhmi::ManualCommand
