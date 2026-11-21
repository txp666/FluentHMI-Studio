#pragma once

#include <QtCore/QJsonObject>
#include <optional>

class QWidget;

namespace fhmi {

enum class SendFrameAction { Preview, Send, StartPeriodic, ApplyPeriodic };

std::optional<QJsonObject> editSendFrameFields(const QJsonObject &properties, const QJsonObject &library,
                                               const QJsonObject &values, QWidget *parent, SendFrameAction action);

} // namespace fhmi
