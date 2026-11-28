#pragma once

#include <QtCore/QHash>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtCore/QSet>

namespace fhmi {

struct ColorRange
{
    double minimum;
    QString color;
};

QList<ColorRange> parseColorRanges(const QJsonValue &rows, QString *error = nullptr);
QString rangeColor(const QList<ColorRange> &ranges, double value);
QStringList tabNames(const QString &text);

struct ActionTargets
{
    QHash<QString, QStringList> pages;
    QSet<QString> buttons;
    QSet<QString> points;
    QHash<QString, QJsonValue> parameters;
    ActionTargets(const QJsonArray &widgets, const QJsonObject &logic);
};

QString widgetActionProblem(const QJsonValue &actions, const QString &widgetType, const ActionTargets &targets,
                            bool allowExternal = false);

template <typename Edit> void editWidgetActions(QJsonObject &properties, Edit edit)
{
    auto rows = properties.value(QStringLiteral("actions")).toArray();
    if (rows.isEmpty())
        return;
    for (qsizetype index = 0; index < rows.size(); ++index) {
        auto row = rows[index].toObject();
        edit(row);
        rows[index] = row;
    }
    properties.insert(QStringLiteral("actions"), rows);
}

} // namespace fhmi
