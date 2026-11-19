#pragma once

#include "core/project.h"
#include "designer/receive_frame_editor.h"

namespace FluentQt {
class ComboBox;
}

namespace fhmi {

void fillFrameTemplates(FluentQt::ComboBox *box, const QJsonObject &library, const QString &selected,
                        const QString &automaticLabel, QStringView parser = {});
bool hasMultipleHexTemplates(const QJsonObject &library);
void editReceiveTemplateSelection(const QJsonObject &library, const QStringList &selected, QWidget *parent,
                                  const std::function<bool(const QStringList &)> &apply,
                                  const QString &connectionId = {});
bool confirmReceiveTemplateSelection(const Project &before, Project &draft, const QString &connectionId,
                                     QWidget *parent);
QHash<QString, QString> mergeFrameTemplateLibrary(QJsonObject &library, const QJsonObject &incoming);
QJsonObject importClipboardFrameTemplates(Project &clipboard, QJsonObject library);
QString chooseReceiveTemplate(const QJsonObject &group, const QString &fieldKey, QWidget *anchor);
std::optional<ReceiveFrameEdit> editProjectFrameTemplate(Project &draft, const QString &id, LinkParser::Format seed,
    const QString &scope, const std::optional<ReceiveValueSettings> &values, QWidget *parent);
QWidget *createFrameTemplateLibrary(Project *draft, QWidget *parent, LinkParser::Format seed = {QStringLiteral("hex"), {}},
                                   const QString &selected = {}, std::function<void()> onSaved = {});
bool editFrameTemplateLibrary(Project *project, QWidget *parent, const LinkParser::Format &seed,
                              const QString &selected = {});

} // namespace fhmi
