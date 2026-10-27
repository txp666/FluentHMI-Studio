#pragma once

#include "core/binding.h"
#include "core/widget_meta.h"
#include "runtime/runtime_context.h"

#include <QtCore/QJsonObject>
#include <QtCore/QPointer>
#include <QtGui/QIcon>
#include <QtWidgets/QWidget>

namespace fhmi {

// Base of every designable widget. The same object is shown on the design canvas
// (inert, inside a graphics proxy) and in a running surface (attached to a context
// that delivers bound data and carries its commands to the device).
class HmiWidget : public QWidget
{
    Q_OBJECT

  public:
    explicit HmiWidget(const WidgetMeta &meta, QWidget *parent = nullptr);

    const WidgetMeta &meta() const { return *m_meta; }
    QString typeName() const { return m_meta->typeName; }
    const QJsonObject &properties() const { return m_props; }

    // Stores the properties and refreshes the widget from them.
    void setProperties(const QJsonObject &props);
    void setAssetBaseDir(const QString &dir);
    QString assetBaseDir() const { return m_assetBaseDir; }

    // Device data arriving through a binding, already transformed and formatted.
    // Implementations update the display only; device feedback never sends a command.
    virtual void applyBoundValue(const QVariant &value, const BindingEntry &binding);

    void attach(RuntimeContext *context);
    void detach();
    RuntimeContext *context() const { return m_context; }
    bool isLive() const { return !m_context.isNull(); }

    // Containers: the widget children of a page are placed in. A child's stored
    // geometry is relative to this widget, whatever page it is on.
    virtual QWidget *childHost(int tabIndex);
    // Containers: the page shown on the design canvas. -1 for widgets without pages.
    virtual int currentPage() const { return -1; }
    // Height of a container's header above its child area, for design-time layout.
    virtual int contentTop() const { return 0; }

  protected:
    virtual void apply() = 0;
    virtual void onAttached() {}
    virtual void onDetached() {}
    // A connection that only exists while attached; detach() disconnects it.
    void live(const QMetaObject::Connection &connection) { m_liveConnections.append(connection); }

    // Property readers that fall back to the metadata default.
    QJsonValue prop(const char *name) const;
    QString text(const char *name) const;
    int integer(const char *name) const;
    double real(const char *name) const;
    bool flag(const char *name) const;

  private:
    const WidgetMeta *m_meta;
    QJsonObject m_props;
    QString m_assetBaseDir;
    QPointer<RuntimeContext> m_context;
    QList<QMetaObject::Connection> m_liveConnections;
};

// Creates the widget for a project type, or a labelled placeholder for a type the
// C++ runtime does not implement yet.
HmiWidget *createHmiWidget(const QString &typeName, const QJsonObject &props, QWidget *parent = nullptr,
                           const QString &assetBaseDir = QString());

// The FluentIcon a widget's metadata names, for palettes and placeholders.
QIcon widgetIcon(const QString &iconName);

// Tab names of a TabWidget's "tabNames" text; never empty.
QStringList tabNames(const QString &text);
int clampTabIndex(int index, int count);

} // namespace fhmi
