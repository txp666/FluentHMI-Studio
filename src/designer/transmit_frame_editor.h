#pragma once

#include <QtCore/QJsonObject>
#include <functional>
#include <optional>

class QWidget;

namespace fhmi {

using TransmitFrameChanged = std::function<void(const QJsonObject &)>;
using RegisterTransmitControl = std::function<void(const QString &span, QWidget *control)>;

QWidget *createTransmitFrameRules(QWidget *parent, const QJsonObject &config, int defaultOffset,
                                  const TransmitFrameChanged &changed, const RegisterTransmitControl &registerControl = {});
std::optional<QJsonObject> editTransmitFrameFormat(const QJsonObject &properties, bool can, QWidget *parent);

} // namespace fhmi
