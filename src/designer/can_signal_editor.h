#pragma once

#include <QtCore/QHash>
#include <QtCore/QJsonObject>
#include <QtCore/QString>

#include <functional>
#include <memory>
#include <optional>

class QJsonArray;
class QWidget;

namespace fhmi {

class Project;
struct CanDatabase;

struct CanSignalSelection
{
    QString connectionId;
    QString key;
    QString name;
};

struct CanSignalEdit
{
    QString text;
    QHash<QString, QString> renamedKeys;
};

std::shared_ptr<const CanDatabase> designerCanDatabase(const Project *project, const QString &file,
                                                       QString *error = nullptr);
std::optional<CanSignalEdit> editCanSignals(const Project *project, const QJsonObject &properties, QWidget *parent,
                                           std::function<bool(const CanSignalEdit &, QWidget *)> approve);
std::optional<QJsonObject> editCanBitBinding(const Project *project, const QJsonObject &binding,
                                            const QJsonObject &card, QWidget *parent);
std::optional<CanSignalSelection> chooseCanSignal(const Project *project, const QJsonArray &widgets,
                                                  const QString &connection, const QString &key, QWidget *parent,
                                                  bool onlyCurrentConnection = false);
std::optional<CanSignalSelection> chooseCanMessage(const Project *project, const QJsonArray &widgets,
                                                  const QString &connection, const QString &key, QWidget *parent,
                                                  bool onlyCurrentConnection = false);
std::optional<QString> editCanInitialMapping(const Project *project, const QJsonObject &card, QWidget *parent);
std::optional<QString> editCanReceiveFilter(const Project *project, const QJsonObject &card, QWidget *parent);
QList<CanSignalSelection> chooseCanSignals(const Project *project, const QJsonArray &widgets,
                                           const QString &connection, int capacity, QWidget *parent);

} // namespace fhmi
