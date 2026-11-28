#pragma once

#include "core/can_signal.h"
#include "core/command_encoder.h"

#include <QtCore/QMap>

class QJsonObject;
class QWidget;

namespace fhmi {

class CanvasItem;
class Project;

struct CanPreviewSource
{
    QString connectionId;
    std::optional<CanSignalSet> signalSet;
    QString error;
    QString initialState = {};
};

struct BinaryFrameExample
{
    QByteArray bytes;
    CommandEncoder::BinaryFrameLayout layout;
    qsizetype applicationSize = 0; // CAN FD can add bus padding after assembly
    QString caption;
};

struct CommandTargetPreview
{
    DataRoute route;
    QString label;
    QString error;
    bool routeInvalid = false;
};

CommandTargetPreview previewCommandTarget(const QJsonObject &properties, const QString &type, const Project &project);
QString modbusSwitchWriteSummary(const QJsonObject &properties);
QString previewInputCommand(const QJsonObject &properties, const QList<CanPreviewSource> &canSources,
                            const QMap<QString, CommandEncoder::InputValue> &inputs, bool showInputValues = true,
                            BinaryFrameExample *example = nullptr, const QJsonObject &frameTemplates = {});
void openCommandPreview(const CanvasItem &item, const QList<CanPreviewSource> &canSources,
                        const CommandTargetPreview &destination, const QMap<QString, CommandEncoder::InputValue> &inputDefaults,
                        QWidget *parent, const QJsonObject &properties, const QJsonObject &frameTemplates,
                        const QJsonObject &enumTables = {});

} // namespace fhmi
