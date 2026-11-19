#pragma once

#include "core/data_source.h"

#include <FluentQtWidgets/Widgets/ComboBox.h>
#include <QtCore/QHash>
#include <QtCore/QSet>

#include <optional>

class QJsonArray;

namespace fhmi {

class DesignScene;
class Project;

QJsonArray currentWidgets(const Project *project, const DesignScene *scene);

class ConnectionSourceEdit : public FluentQt::ComboBox
{
  public:
    enum class Purpose { Binding, DataPoint, Send, Observe };
    explicit ConnectionSourceEdit(QWidget *parent, Purpose purpose = Purpose::Binding);

    void setSources(const Project *project, const DesignScene *scene, const RouteFilter &filter, const QString &current);
    bool ambiguous() const;
    bool supportsEndpoint() const;
    bool offersEndpoint() const;
    QString endpointPlaceholder() const;

  private:
    enum class Endpoint { None, Address, Topic, Either };
    Endpoint endpointKind() const;

    Purpose m_purpose;
    qsizetype m_connections = 0;
    std::optional<RouteFilter> m_filter;
    QHash<QString, Endpoint> m_endpoints;
    QSet<QString> m_offeredEndpoints;
    Endpoint m_defaultEndpoint = Endpoint::None;
    Endpoint m_defaultOfferedEndpoint = Endpoint::None;
};

} // namespace fhmi
