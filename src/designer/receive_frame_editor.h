#pragma once

#include "core/protocol_parser.h"
#include "runtime/receive_frame_preview.h"

#include <functional>

namespace fhmi {

struct ReceiveFrameEdit
{
    LinkParser::Format format;
    QJsonObject bindingChanges;
    QString templateName;
    QString sample;
};

struct ReceiveValueSettings
{
    QJsonObject binding;
    QString typeName;
    bool strict = false;
};

struct FrameEditorOptions
{
    QString templateName;
    QStringList takenNames;
    QString sample;
    bool lockParser = false;
    std::function<QString(const LinkParser::Format &)> formatProblem;
    std::function<bool(const LinkParser::Format &, const QList<qsizetype> &, QWidget *)> confirmChanges;
};

std::optional<ReceiveFrameEdit> editReceiveFrameFormat(LinkParser::Format format, const QString &scope,
                                                       const std::optional<ReceiveValueSettings> &values, QWidget *parent,
                                                       const FrameEditorOptions &options = {});

} // namespace fhmi
