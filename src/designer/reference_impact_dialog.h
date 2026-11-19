#pragma once

#include <QtCore/QList>
#include <QtCore/QString>

class QJsonObject;
class QWidget;

namespace fhmi {

struct ReferenceRow
{
    QString category;
    QString name;
    QString detail;
};

QString widgetLabel(const QJsonObject &widget);
bool confirmReferences(QWidget *parent, const QString &title, const QString &message,
                       const QList<ReferenceRow> &rows, const QString &continueText, const QString &cancelText,
                       const QString &objectName = QStringLiteral("frameTemplateImpact"));

} // namespace fhmi
