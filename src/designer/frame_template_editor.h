#pragma once

#include "designer/command_preview.h"

#include <functional>

namespace fhmi {

class CanvasItem;
class Project;

QString defaultFrameTemplate(const QJsonObject &properties, const QList<CanPreviewSource> &canSources,
                             const QMap<QString, CommandEncoder::InputValue> &inputs);
void openFrameTemplateEditor(const CanvasItem &item, const Project &project, QList<CanPreviewSource> canSources,
                             QMap<QString, CommandEncoder::InputValue> inputs,
                             bool canCreate, QWidget *parent,
                             std::function<void(const QJsonObject &, const QMap<QString, QString> &)> apply);

} // namespace fhmi
