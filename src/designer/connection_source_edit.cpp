#include "designer/connection_source_edit.h"

#include "core/i18n.h"
#include "core/project.h"
#include "core/widget_meta.h"
#include "designer/design_scene.h"
#include "runtime/ui_helpers.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QSet>
#include <QtCore/QSignalBlocker>

namespace fhmi {
namespace {
QString tr(const char *source) { return QCoreApplication::translate("fhmi", source); }
}

QJsonArray currentWidgets(const Project *project, const DesignScene *scene)
{
    // New and edited connections come from the live design before the project is saved.
    return scene ? scene->widgetsJson() : project ? project->widgets : QJsonArray();
}

ConnectionSourceEdit::ConnectionSourceEdit(QWidget *parent, Purpose purpose)
    : FluentQt::ComboBox(parent), m_purpose(purpose) {}

ConnectionSourceEdit::Endpoint ConnectionSourceEdit::endpointKind() const
{
    const QString id = selectedValue(this);
    if (id.isEmpty() || id == DataRoute::Broadcast)
        return m_defaultOfferedEndpoint == Endpoint::None ? m_defaultEndpoint : m_defaultOfferedEndpoint;
    return m_endpoints.value(id, Endpoint::None);
}

bool ConnectionSourceEdit::ambiguous() const
{
    return selectedValue(this).isEmpty() && m_connections > 1 &&
           (m_purpose != Purpose::Observe || (m_filter && m_filter->transport == RouteTransport::ModbusSlave));
}

bool ConnectionSourceEdit::supportsEndpoint() const { return endpointKind() != Endpoint::None; }

bool ConnectionSourceEdit::offersEndpoint() const
{
    const QString id = selectedValue(this);
    return id.isEmpty() || id == DataRoute::Broadcast ? m_defaultOfferedEndpoint != Endpoint::None
                                                     : m_offeredEndpoints.contains(id);
}

QString ConnectionSourceEdit::endpointPlaceholder() const
{
    const auto kind = endpointKind();
    if (kind == Endpoint::Topic)
        return fhmi::tr("如 device/data");
    if (kind == Endpoint::Address)
        return fhmi::tr("如 192.168.1.20:502");
    return fhmi::tr("地址:端口或 MQTT Topic");
}

void ConnectionSourceEdit::setSources(const Project *project, const DesignScene *scene, const RouteFilter &filter,
                                     const QString &current)
{
    if (m_filter == filter)
        return;
    m_filter = filter;
    const auto transport = filter.transport;
    const QSignalBlocker blocker(this);
    clear();
    m_endpoints.clear();
    m_offeredEndpoints.clear();
    m_defaultEndpoint = Endpoint::None;
    m_defaultOfferedEndpoint = Endpoint::None;
    const bool observesAll = m_purpose == Purpose::Observe && transport != RouteTransport::ModbusSlave;
    addItem(transport == RouteTransport::S7 ? fhmi::tr("请选择 S7 连接")
            : observesAll ? fhmi::tr("全部匹配连接") : fhmi::tr("自动匹配唯一连接"), {}, QString());
    if (!observesAll && m_purpose != Purpose::DataPoint && transport != RouteTransport::S7 &&
        transport != RouteTransport::ModbusSlave)
        addItem(m_purpose == Purpose::Send ? fhmi::tr("广播到全部匹配连接") : fhmi::tr("全部匹配连接"),
                {}, DataRoute::Broadcast);

    const auto widgets = currentWidgets(project, scene);
    QSet<QString> tcpClients;
    if (allowsRouteEndpoint(transport)) {
        visitWidgets(widgets, [&tcpClients](const QJsonObject &widget) {
            if (widget.value(QStringLiteral("type")).toString() == QLatin1String("TcpConfigCard") &&
                widget.value(QStringLiteral("properties")).toObject().value(QStringLiteral("defaultMode")).toString() !=
                    QStringLiteral("服务端")) // i18n: value
                tcpClients.insert(widget.value(QStringLiteral("id")).toString());
            return true;
        });
    }
    const auto connections = routeConnections(widgets, filter, false,
                                              project ? project->parserType() : QStringLiteral("line"));
    m_connections = connections.size();
    const auto combine = [](Endpoint previous, Endpoint next) {
        return previous == Endpoint::None || previous == next ? next : Endpoint::Either;
    };
    for (const auto &[id, type] : connections) {
        const auto *meta = WidgetRegistry::instance().find(type);
        addItem(id + QStringLiteral(" · ") + translated(meta->displayName), {}, id);
        if (!allowsRouteEndpoint(transport) || !supportsRouteEndpoint(type))
            continue;
        const auto kind = type == QLatin1String("MqttConfigCard") ? Endpoint::Topic : Endpoint::Address;
        m_endpoints.insert(id, kind);
        m_defaultEndpoint = combine(m_defaultEndpoint, kind);
        if (!tcpClients.contains(id)) {
            m_offeredEndpoints.insert(id);
            m_defaultOfferedEndpoint = combine(m_defaultOfferedEndpoint, kind);
        }
    }
    if (!current.isEmpty() && findData(current) < 0) {
        addItem(fhmi::tr("%1（当前模式不可用）").arg(current), {}, current);
        setItemEnabled(count() - 1, false);
    }
    selectValue(this, current);
}

} // namespace fhmi
