#include "runtime/runtime_surface.h"

#include "runtime/hmi_widget.h"
#include "runtime/runtime_context.h"
#include "runtime/widgets/container_widgets.h"
#include "runtime/widgets/display_widgets.h"

#include <FluentQtWidgets/Theme.h>

#include <QtCore/QHash>
#include <QtGui/QPainter>

namespace fhmi {

RuntimeSurface::RuntimeSurface(QWidget *parent) : QWidget(parent), m_context(new RuntimeContext(this))
{
    connect(m_context, &RuntimeContext::notificationRequested, this, &RuntimeSurface::notificationRequested);
    connect(FluentQt::ThemeManager::instance(), &FluentQt::ThemeManager::effectiveThemeChanged, this,
            [this]() { update(); });
}

RuntimeSurface::~RuntimeSurface()
{
    clear();
}

QList<HmiWidget *> RuntimeSurface::widgets() const
{
    QList<HmiWidget *> out;
    for (const QPointer<HmiWidget> &widget : m_widgets) {
        if (widget) {
            out.append(widget.data());
        }
    }
    return out;
}

void RuntimeSurface::clear()
{
    m_context->setActive(false);
    // Detach first so every link closes while the hub still exists.
    for (const QPointer<HmiWidget> &widget : std::as_const(m_widgets)) {
        if (widget) {
            widget->detach();
        }
    }
    m_context->hub()->disconnectAll();
    for (const QPointer<HmiWidget> &widget : std::as_const(m_widgets)) {
        if (widget && widget->parentWidget() == this) {
            delete widget.data();
        }
    }
    m_widgets.clear();
    m_context->clear();
}

void RuntimeSurface::build(const Project &project)
{
    clear();
    m_assetBaseDir = project.assetBaseDir();
    m_context->hub()->configure(project.serial);
    setFixedSize(project.canvasWidth, project.canvasHeight);
    for (const QJsonValue &value : project.widgets) {
        buildWidget(value.toObject(), nullptr);
    }
    // Attach once every widget exists, so cards register links before anything sends.
    for (const QPointer<HmiWidget> &widget : std::as_const(m_widgets)) {
        if (widget) {
            widget->attach(m_context);
        }
    }
    m_context->setActive(true);
}

void RuntimeSurface::buildWidget(const QJsonObject &data, HmiWidget *container)
{
    const QString type = data.value(QStringLiteral("type")).toString();
    HmiWidget *widget = createHmiWidget(type, data.value(QStringLiteral("properties")).toObject(), nullptr,
                                        m_assetBaseDir);
    if (!widget) {
        return;
    }
    const WidgetMeta &meta = widget->meta();
    const QJsonObject geo = data.value(QStringLiteral("geometry")).toObject();
    const QRect rect(int(geo.value(QStringLiteral("x")).toDouble(0)), int(geo.value(QStringLiteral("y")).toDouble(0)),
                     int(geo.value(QStringLiteral("w")).toDouble(meta.defaultWidth)),
                     int(geo.value(QStringLiteral("h")).toDouble(meta.defaultHeight)));

    if (container) {
        QWidget *host = container->childHost(data.value(QStringLiteral("tabIndex")).toInt(0));
        auto *keeper = container->findChild<ChildGeometryKeeper *>(QString(), Qt::FindDirectChildrenOnly);
        if (!keeper) {
            keeper = new ChildGeometryKeeper(container);
        }
        keeper->place(widget, host ? host : container, rect);
    } else {
        widget->setParent(this);
        widget->setGeometry(rect);
    }
    widget->show();
    m_widgets.append(widget);

    const QString id = data.value(QStringLiteral("id")).toString();
    m_context->registerWidget(id, widget);
    if (auto binding = BindingEntry::fromWidget(data)) {
        m_context->addBinding(std::move(*binding));
    }
    for (const QJsonValue &child : data.value(QStringLiteral("children")).toArray()) {
        buildWidget(child.toObject(), widget);
    }
}

void RuntimeSurface::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    const bool dark = isDarkTheme();
    painter.fillRect(rect(), dark ? QColor(0x2b, 0x2b, 0x2b) : QColor(0xfb, 0xfb, 0xfb));
}

} // namespace fhmi
