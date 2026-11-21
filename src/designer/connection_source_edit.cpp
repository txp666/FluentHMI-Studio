#include "designer/connection_source_edit.h"

#include "core/binding.h"
#include "core/i18n.h"
#include "core/project.h"
#include "core/project_logic.h"
#include "core/mqtt_codec.h"
#include "core/widget_meta.h"
#include "designer/design_scene.h"
#include "runtime/ui_helpers.h"

#include <FluentQtWidgets/FluentIcon.h>
#include <FluentQtWidgets/Widgets/LineEdit.h>
#include <FluentQtWidgets/Widgets/Menu.h>
#include <QtCore/QCoreApplication>
#include <QtCore/QSet>
#include <QtCore/QSignalBlocker>
#include <QtCore/QUrl>
#include <QtNetwork/QHostAddress>

namespace fhmi {
namespace {
QString tr(const char *source) { return QCoreApplication::translate("fhmi", source); }
}

QJsonArray currentWidgets(const Project *project, const DesignScene *scene)
{
    // New and edited connections come from the live design before the project is saved.
    return scene ? scene->widgetsJson() : project ? project->widgets : QJsonArray();
}

void fillWritePointChoices(FluentQt::ComboBox *box, const Project &project, const QJsonArray &widgets,
                           const QJsonObject &properties, QStringView widgetType)
{
    const QSignalBlocker blocker(box);
    box->clear();
    box->addItem(tr("手动填写地址"), {}, QString());
    const auto key = writePointKey(properties.value(QStringLiteral("commandMode")).toString());
    if (key.isEmpty())
        return;
    const auto configured = properties.value(key).toString();
    auto candidate = properties;
    candidate.remove(QStringLiteral("commandInputs")); // The FC16 list may be edited after selecting its address.
    for (const auto &name : dataPointNames(project.logic)) {
        candidate.insert(key, name);
        if (resolvePointWrite(candidate, project.logic, widgets, project.parserType(), nullptr, widgetType,
                              ModbusParserChoice::OnConnect))
            box->addItem(name, {}, name);
    }
    if (!configured.isEmpty() && box->findData(configured) < 0)
        box->addItem(tr("%1（当前模式不可用）").arg(configured), {}, configured);
    selectValue(box, configured);
}

QString writePointTargetText(const Project &project, const QJsonArray &widgets,
                             const QJsonObject &resolved, const QString &point)
{
    auto binding = findDataPoint(project.logic, point).value(QStringLiteral("binding")).toObject();
    const auto target = resolved.value(QStringLiteral("commandTargetId"));
    binding.insert(QStringLiteral("sourceCardId"), target);
    auto endpoint = resolved.value(QStringLiteral("commandEndpoint"));
    if (binding.value(QStringLiteral("parseMode")).toString() == ParseMode::Modbus) {
        binding.insert(QStringLiteral("mbSlaveFilter"), resolved.value(QStringLiteral("mbSlave")));
        visitWidgets(widgets, [&](const QJsonObject &card) {
            if (card.value(QStringLiteral("id")) != target)
                return true;
            if (card.value(QStringLiteral("type")).toString() == QLatin1String("MqttConfigCard")) {
                const auto *meta = WidgetRegistry::instance().find(QStringLiteral("MqttConfigCard"));
                const auto key = QStringLiteral("defaultPublish");
                endpoint = card.value(QStringLiteral("properties")).toObject().value(key)
                    .toString(meta->prop(key)->defaultValue.toString());
            }
            return false;
        });
    }
    binding.insert(QStringLiteral("sourceEndpoint"), endpoint);
    return dataPointSourceText(binding);
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

QStringList ConnectionSourceEdit::endpointSuggestions() const
{
    const QString selected = selectedValue(this);
    QStringList options;
    visitWidgets(currentWidgets(m_project, m_scene), [&](const QJsonObject &widget) {
        const auto id = widget.value(QStringLiteral("id")).toString();
        const auto kind = m_endpoints.value(id, Endpoint::None);
        if (kind == Endpoint::None ||
            (!selected.isEmpty() && selected != DataRoute::Broadcast && selected != id))
            return true;
        const auto props = widget.value(QStringLiteral("properties")).toObject();
        const auto type = widget.value(QStringLiteral("type")).toString();
        if (kind == Endpoint::Topic) {
            const auto key = m_purpose == Purpose::Send ? QStringLiteral("defaultPublish")
                                                        : QStringLiteral("defaultSubscribe");
            const auto *meta = WidgetRegistry::instance().find(QStringLiteral("MqttConfigCard"));
            const auto topic = props.value(key).toString(meta->prop(key)->defaultValue.toString()).trimmed();
            const auto topics = m_purpose == Purpose::Send ? QStringList{topic} : Mqtt::topics(topic);
            for (const auto &candidate : topics)
                if (!candidate.isEmpty() && !candidate.contains(u'#') && !candidate.contains(u'+'))
                    options.append(candidate);
        } else if (type == u"UdpConfigCard") {
            const auto host = props.value(QStringLiteral("defaultHost")).toString(QStringLiteral("127.0.0.1"));
            const auto port = props.value(QStringLiteral("defaultPort")).toInt(8080);
            const QHostAddress address(host);
            if (!address.isNull() && port > 0)
                options.append(networkEndpoint(address.toString(), quint16(port)));
        }
        if (m_project) {
            const auto recent = m_project->recentEndpoints.value(id);
            if (type == u"TcpConfigCard" && m_purpose != Purpose::Send) {
                for (const auto &endpoint : recent) {
                    const auto address = QUrl(QStringLiteral("tcp://") + endpoint).host();
                    if (!address.isEmpty())
                        options.append(address);
                }
            }
            options.append(recent);
        }
        return true;
    });
    options.removeDuplicates();
    return options;
}

void ConnectionSourceEdit::bindEndpointEditor(FluentQt::LineEdit *editor)
{
    if (m_endpointEditor == editor)
        return;
    m_endpointEditor = editor;
    auto *action = new QAction(FluentQt::icon(FluentQt::FluentIcon::ChevronDownMed), fhmi::tr("选择子通道"), editor);
    action->setObjectName(QStringLiteral("chooseRouteEndpoint"));
    editor->addAction(action, QLineEdit::TrailingPosition);
    connect(action, &QAction::triggered, this, [this, editor] {
        auto *menu = new FluentQt::CheckableMenu(editor);
        menu->setAttribute(Qt::WA_DeleteOnClose);
        menu->setMaxVisibleItems(12);
        auto *automatic = menu->addCheckableAction(m_purpose == Purpose::Observe ? fhmi::tr("全部子通道")
                                                                                : fhmi::tr("自动匹配子通道"),
                                                   editor->text().isEmpty());
        connect(automatic, &QAction::triggered, editor, &QLineEdit::clear);
        const auto options = endpointSuggestions();
        menu->addSeparator();
        if (options.isEmpty())
            menu->addAction(fhmi::tr("暂无候选，可手动填写或先运行预览"))->setEnabled(false);
        for (const auto &endpoint : options) {
            auto *item = menu->addCheckableAction(endpoint, editor->text() == endpoint);
            connect(item, &QAction::triggered, editor, [editor, endpoint] { editor->setText(endpoint); });
        }
        menu->exec(editor->mapToGlobal(QPoint(0, editor->height())), true);
    });
}

QString ConnectionSourceEdit::selectionProblem() const
{
    if (!m_filter || m_filter->transport != RouteTransport::Modbus)
        return {};
    if (m_connections == 0 || (!m_unavailable.isEmpty() && m_unavailable == selectedValue(this)))
        return fhmi::tr("请选择启用 Modbus RTU、ASCII 或 TCP 的连接");
    return {};
}

void ConnectionSourceEdit::setSources(const Project *project, const DesignScene *scene, const RouteFilter &filter,
                                     const QString &current)
{
    if (m_filter == filter)
        return;
    m_project = project;
    m_scene = scene;
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
    QHash<QString, QJsonObject> modbusCards;
    if (allowsRouteEndpoint(transport)) {
        visitWidgets(widgets, [&](const QJsonObject &widget) {
            const auto id = widget.value(QStringLiteral("id")).toString();
            if (transport == RouteTransport::Modbus)
                modbusCards.insert(id, widget);
            if (widget.value(QStringLiteral("type")).toString() == QLatin1String("TcpConfigCard") &&
                widget.value(QStringLiteral("properties")).toObject().value(QStringLiteral("defaultMode")).toString() !=
                    QStringLiteral("服务端")) // i18n: value
                tcpClients.insert(id);
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
        auto label = transport == RouteTransport::Modbus && project
            ? modbusRouteDescription(modbusCards.value(id), *project) : translated(meta->displayName);
        if (transport == RouteTransport::Modbus && (m_purpose == Purpose::Binding || m_purpose == Purpose::DataPoint) &&
            label.startsWith(QLatin1String("Modbus")))
            label += QStringLiteral(" · ") + fhmi::tr("可轮询");
        addItem(id + QStringLiteral(" · ") + label, {}, id);
        if (!allowsRouteEndpoint(transport) || !supportsRouteEndpoint(type))
            continue;
        const auto kind = type == QLatin1String("MqttConfigCard") ? Endpoint::Topic : Endpoint::Address;
        m_endpoints.insert(id, kind);
        m_defaultEndpoint = combine(m_defaultEndpoint, kind);
        if (!tcpClients.contains(id) || (m_project && m_project->recentEndpoints.contains(id))) {
            m_offeredEndpoints.insert(id);
            m_defaultOfferedEndpoint = combine(m_defaultOfferedEndpoint, kind);
        }
    }
    m_unavailable.clear();
    if (!current.isEmpty() && findData(current) < 0) {
        m_unavailable = current;
        addItem(fhmi::tr("%1（当前模式不可用）").arg(current), {}, current);
        setItemEnabled(count() - 1, false);
    }
    selectValue(this, current);
}

} // namespace fhmi
