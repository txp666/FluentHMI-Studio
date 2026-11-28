#pragma once

#include "core/binding.h"
#include "core/widget_meta.h"
#include "runtime/runtime_context.h"

#include <QtCore/QJsonObject>
#include <QtCore/QPointer>
#include <QtGui/QIcon>
#include <QtWidgets/QWidget>

class QAbstractButton;
class QSpinBox;

namespace FluentQt {
class ComboBox;
class InfoBadge;
}

namespace fhmi {

// Base of every designable widget. The same object is shown on the design canvas
// (inert, inside a graphics proxy) and in a running surface (attached to a context
// that delivers bound data and carries its commands to the device).
class HmiWidget : public QWidget
{
    Q_OBJECT

  public:
    enum class BindingStatus { Unbound, Waiting, Live, Stale };

    explicit HmiWidget(const WidgetMeta &meta, QWidget *parent = nullptr);

    const WidgetMeta &meta() const { return *m_meta; }
    QString typeName() const { return m_meta->typeName; }
    const QJsonObject &properties() const { return m_props; }
    DataRoute receiveRoute() const { return {text("sourceCardId"), text("sourceEndpoint")}; }
    DataRoute commandRoute() const { return {text("commandTargetId"), text("commandEndpoint")}; }

    // Stores the properties and refreshes the widget from them.
    void setProperties(const QJsonObject &props);
    void setEnumTables(const QJsonObject &tables);
    void setAssetBaseDir(const QString &dir);
    QString assetBaseDir() const { return m_assetBaseDir; }

    // Device data arriving through a binding, already transformed and formatted.
    // Implementations update the display only; device feedback never sends a command.
    virtual void applyBoundValue(const QVariant &value, const BindingEntry &binding);
    void setBindingStatus(BindingStatus status);
    void setBindingProblem(const QString &status, const QString &detail = {});
    BindingStatus bindingStatus() const { return m_bindingStatus; }
    bool isBound() const { return m_bindingStatus != BindingStatus::Unbound; }

    virtual std::optional<QVariant> inputValue() const { return std::nullopt; }

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

    // Connection cards fit their visible rows. Designer and runtime measure at the requested width
    // before applying geometry, so an old narrow-layout minimum cannot enlarge a wide card.
    virtual bool fitsContentHeight() const { return false; }
    QSize sizeForGeometry(const QSize &requested);

  protected:
    virtual void apply() = 0;
    virtual void onAttached() {}
    virtual void onDetached() {}
    virtual void bindingStatusChanged(BindingStatus) {}
    virtual void arrangeForWidth(int) {}
    void resizeEvent(QResizeEvent *event) override;
    void updateMinimumHeight();
    // A connection that only exists while attached; detach() disconnects it.
    void live(const QMetaObject::Connection &connection) { m_liveConnections.append(connection); }

    // Restore from local settings after apply() sets the project's defaults.
    // Combo boxes use stable item data, independent of the display language.
    void rememberViewOption(FluentQt::ComboBox *control, const QString &key);
    void rememberViewOption(QAbstractButton *control, const QString &key);
    void rememberViewOption(QSpinBox *control, const QString &key);
    QVariant viewPreference(const QString &key, const QVariant &fallback) const;
    void saveViewPreference(const QString &key, const QVariant &value);

    // Property readers that fall back to the metadata default.
    QJsonValue prop(const char *name) const;
    QString text(const char *name) const;
    int integer(const char *name) const;
    double real(const char *name) const;
    bool flag(const char *name) const;

    bool confirmsSend() const { return flag("confirmBeforeSend"); }
    const QJsonObject &frameTemplates() const;
    const QJsonObject &enumTables() const { return m_enumTables; }
    void requestSend(const CommandResult &command, std::function<void()> onSent = {}, const QString &mode = {},
                     int intervalMs = 0, CommandReader readInputs = {});
    bool isPeriodicSending() const;
    bool isPeriodicStarting() const;
    bool stopPeriodicSend();
    virtual void periodicSendChanged(bool) {}

  private:
    friend class RuntimeContext;
    int contentMinimumHeight(int width);
    void positionBindingBadge();
    const WidgetMeta *m_meta;
    QJsonObject m_props;
    QJsonObject m_enumTables;
    QString m_assetBaseDir;
    QPointer<RuntimeContext> m_context;
    QList<QMetaObject::Connection> m_liveConnections;
    BindingStatus m_bindingStatus = BindingStatus::Unbound;
    FluentQt::InfoBadge *m_bindingBadge = nullptr;
};

// Creates the widget for a project type, or a labelled placeholder for a type the
// C++ runtime does not implement yet.
HmiWidget *createHmiWidget(const QString &typeName, const QJsonObject &props, QWidget *parent = nullptr,
                           const QString &assetBaseDir = QString());

// The icon a widget's metadata names, one of the palette's own or a FluentIcon; it follows the theme.
QIcon widgetIcon(const QString &iconName);

// Tab names of a TabWidget's "tabNames" text; never empty.
QStringList tabNames(const QString &text);
int clampTabIndex(int index, int count);

} // namespace fhmi
