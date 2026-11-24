#pragma once

#include "core/s7_address.h"
#include "core/s7_codec.h"
#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtCore/QDateTime>
#include <optional>

namespace fhmi::S7 {

struct Config {
    QString host = QStringLiteral("127.0.0.1");
    int port = 102, rack = 0, slot = 1, connectionType = 1;
    int period = 500, timeout = 1000, reconnectInterval = 1000;
    bool autoConnect = false, autoReconnect = true;
};

struct Point {
    Address address;
    DataType type;
    int period = 0;
    QString key() const { return address.canonical + QLatin1Char(':') + type.name; }
    bool operator==(const Point &other) const
    {
        return address.canonical == other.address.canonical && type.name == other.type.name && period == other.period;
    }
};

struct Need { QString sourceCardId; Point point; };
struct Command { QString targetId; Point point; QByteArray bytes; };
struct Sample {
    QString sourceCardId, address, dataType;
    QVariant value;
    QString quality = QStringLiteral("good");
    QDateTime timestamp;
    quint64 sessionGeneration = 0;
};

QStringList dataTypes();
std::optional<Config> config(const QJsonObject &properties, QString *error = nullptr);
std::optional<Point> point(const QJsonObject &object, QString *error = nullptr);
QJsonObject pointJson(const Point &point);
QString writeValueProblem(const QJsonObject &properties, const DataType &type, QStringView widgetType);
bool usesS7(const QJsonArray &widgets);
bool validateWidgets(const QJsonArray &widgets, QString *error = nullptr, bool allowExternalReferences = false);

} // namespace fhmi::S7
Q_DECLARE_METATYPE(fhmi::S7::Sample)
