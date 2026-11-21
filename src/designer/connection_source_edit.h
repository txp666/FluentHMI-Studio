#pragma once

#include "core/data_source.h"

#include <FluentQtWidgets/Widgets/ComboBox.h>
#include <QtCore/QHash>
#include <QtCore/QSet>
#include <QtCore/QPointer>

#include <optional>

class QJsonArray;
class QJsonObject;
namespace FluentQt { class LineEdit; }

namespace fhmi {

class DesignScene;
class Project;

QJsonArray currentWidgets(const Project *project, const DesignScene *scene);
void fillWritePointChoices(FluentQt::ComboBox *box, const Project &project, const QJsonArray &widgets,
                           const QJsonObject &properties, QStringView widgetType);
QString writePointTargetText(const Project &project, const QJsonArray &widgets,
                             const QJsonObject &resolved, const QString &point);

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
    void bindEndpointEditor(FluentQt::LineEdit *editor);
    QString selectionProblem() const;

  private:
    enum class Endpoint { None, Address, Topic, Either };
    Endpoint endpointKind() const;
    QStringList endpointSuggestions() const;

    Purpose m_purpose;
    const Project *m_project = nullptr;
    const DesignScene *m_scene = nullptr;
    QPointer<FluentQt::LineEdit> m_endpointEditor;
    qsizetype m_connections = 0;
    QString m_unavailable;
    std::optional<RouteFilter> m_filter;
    QHash<QString, Endpoint> m_endpoints;
    QSet<QString> m_offeredEndpoints;
    Endpoint m_defaultEndpoint = Endpoint::None;
    Endpoint m_defaultOfferedEndpoint = Endpoint::None;
};

} // namespace fhmi
