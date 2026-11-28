#pragma once

#include <QtCore/QJsonObject>
#include <optional>

class QWidget;

namespace fhmi {

struct EnumMappingEdit
{
    QString localMapping;
    QString tableName;
    QJsonObject tables;
    QJsonObject effects;
};

std::optional<EnumMappingEdit> editEnumMapping(const QString &type, const QJsonObject &properties,
                                               const QJsonObject &tables, bool canShare, QWidget *parent);

} // namespace fhmi
