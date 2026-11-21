#pragma once

#include <QtCore/QMetaType>
#include <QtCore/QString>

#include <atomic>
#include <optional>

namespace fhmi {

// Transport metadata stays outside the device's decoded fields. A stream is renewed on reconnect;
// TCP clients, UDP senders and MQTT topics each have their own stream within a connection card.
struct DataSource
{
    QString connectionId;
    quint64 stream = 0;
    QString endpoint;
    QString encoding = QStringLiteral("utf-8");
    bool canText = false;
    bool slaveData = false;
    QString device = {}; // Stable transport peer, independent of a TCP client's temporary source port.
    bool modbus = false;

    QStringView deviceKey() const { return device.isEmpty() ? QStringView(endpoint) : QStringView(device); }
};

struct DataRoute
{
    inline static const QString Broadcast = QStringLiteral("@all");

    QString connectionId; // empty: automatic for actions and bindings
    QString endpoint;     // empty: the connection's normal destination / all incoming streams

    bool isBroadcast() const { return connectionId == Broadcast; }
    bool acceptsConnection(QStringView id) const { return connectionId.isEmpty() || isBroadcast() || connectionId == id; }

    bool accepts(const DataSource &source) const
    {
        return acceptsConnection(source.connectionId) &&
               (endpoint.isEmpty() || endpoint == source.endpoint || (!source.endpoint.isEmpty() && endpoint == source.device));
    }
    bool operator==(const DataRoute &other) const
    {
        return connectionId == other.connectionId && endpoint == other.endpoint;
    }
};

enum class RouteTransport { Any, Bytes, Can, S7, Modbus, ModbusSlave };
enum class ModbusParserChoice { Project, OnConnect };

struct ModbusDestination
{
    int slave;
    int function;

    bool acceptsLocalSlave(int unit) const
    {
        return slave == unit && (function == 0x05 || function == 0x06 || function == 0x10);
    }
    bool operator==(const ModbusDestination &other) const { return slave == other.slave && function == other.function; }
};

struct RouteFilter
{
    RouteTransport transport;
    std::optional<ModbusDestination> modbus;

    RouteFilter(RouteTransport transport) : transport(transport) {}
    RouteFilter(ModbusDestination modbus) : transport(RouteTransport::Modbus), modbus(modbus) {}
    bool operator==(const RouteFilter &other) const { return transport == other.transport && modbus == other.modbus; }
};

inline RouteTransport widgetReceiveTransport(QStringView widgetType)
{
    if (widgetType == QLatin1String("CanMonitor"))
        return RouteTransport::Can;
    if (widgetType == QLatin1String("ModbusSlaveTable"))
        return RouteTransport::ModbusSlave;
    return RouteTransport::Any;
}

inline bool allowsRouteEndpoint(RouteTransport transport)
{
    return transport == RouteTransport::Any || transport == RouteTransport::Bytes || transport == RouteTransport::Modbus;
}

inline bool supportsRouteTransport(QStringView cardType, RouteTransport transport)
{
    const bool can = cardType == QLatin1String("CanConfigCard");
    const bool localSlave = cardType == QLatin1String("SerialConfigCard") ||
                            cardType == QLatin1String("TcpConfigCard") ||
                            cardType == QLatin1String("UdpConfigCard");
    const bool bytes = localSlave || cardType == QLatin1String("MqttConfigCard");
    switch (transport) {
    case RouteTransport::Any: return can || bytes;
    case RouteTransport::Bytes: return bytes;
    case RouteTransport::Modbus: return bytes;
    case RouteTransport::Can: return can;
    case RouteTransport::S7: return cardType == QLatin1String("S7ConfigCard");
    case RouteTransport::ModbusSlave: return localSlave;
    }
    return false;
}

inline bool isConnectionCard(QStringView cardType)
{
    return supportsRouteTransport(cardType, RouteTransport::Any) || supportsRouteTransport(cardType, RouteTransport::S7);
}

inline bool supportsRouteEndpoint(QStringView cardType)
{
    return cardType == QLatin1String("TcpConfigCard") || cardType == QLatin1String("UdpConfigCard") ||
           cardType == QLatin1String("MqttConfigCard");
}

inline QString networkEndpoint(const QString &address, quint16 port)
{
    return QStringLiteral("%1:%2")
        .arg(address.contains(QLatin1Char(':')) ? QLatin1Char('[') + address + QLatin1Char(']') : address)
        .arg(port);
}

inline quint64 nextDataStream()
{
    static std::atomic<quint64> next{0};
    return ++next;
}

} // namespace fhmi

Q_DECLARE_METATYPE(fhmi::DataSource)
