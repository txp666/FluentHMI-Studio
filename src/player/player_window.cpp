#include "player/player_window.h"

#include "runtime/periodic_task_view.h"
#include "runtime/project_data_view.h"
#include "runtime/runtime_context.h"
#include "runtime/runtime_surface.h"
#include "runtime/ui_helpers.h"

#include <FluentQtWidgets/Config.h>
#include <FluentQtWidgets/Dialogs/Dialog.h>
#include <FluentQtWidgets/FluentIcon.h>
#include <FluentQtWidgets/Widgets/Button.h>
#include <FluentQtWidgets/Widgets/InfoBar.h>
#include <FluentQtWidgets/Window/FluentTitleBar.h>

#include <QtCore/QJsonArray>
#include <QtGui/QGuiApplication>
#include <QtGui/QPainter>
#include <QtGui/QScreen>
#include <QtGui/QTransform>
#include <QtWidgets/QApplication>
#include <QtWidgets/QGraphicsScene>
#include <QtWidgets/QGraphicsView>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>

namespace fhmi {

namespace {

// The canvas is scaled, but its menus, dialogs and help bubbles stay ordinary windows.
// Set the flag when a child gets its parent, before Qt embeds it in the scene.
class CanvasPopupFilter : public QObject
{
  public:
    explicit CanvasPopupFilter(QWidget *canvas) : QObject(canvas), m_canvas(canvas) { qApp->installEventFilter(this); }

  protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (event->type() == QEvent::ParentChange || event->type() == QEvent::Polish) {
            auto *widget = qobject_cast<QWidget *>(watched);
            if (widget && widget != m_canvas && widget->isWindow() &&
                !widget->windowFlags().testFlag(Qt::BypassGraphicsProxyWidget)) {
                // QWidget::isAncestorOf stops at window boundaries, which popups introduce.
                for (QWidget *owner = widget->parentWidget(); owner; owner = owner->parentWidget()) {
                    if (owner == m_canvas) {
                        widget->setWindowFlag(Qt::BypassGraphicsProxyWidget);
                        break;
                    }
                }
            }
        }
        return false;
    }

  private:
    QWidget *m_canvas;
};

// Keep the design coordinates intact; the view scales painting and input together.
class CanvasView : public QGraphicsView
{
  public:
    CanvasView(QWidget *canvas, QWidget *parent) : QGraphicsView(parent), m_canvas(canvas)
    {
        setObjectName(QStringLiteral("canvasView"));
        auto *scene = new QGraphicsScene(this);
        setScene(scene);
        new CanvasPopupFilter(canvas);
        scene->addWidget(canvas);
        setFrameShape(QFrame::NoFrame);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setAlignment(Qt::AlignCenter);
        setViewportUpdateMode(QGraphicsView::SmartViewportUpdate);
        setOptimizationFlag(QGraphicsView::DontSavePainterState);
        setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing | QPainter::SmoothPixmapTransform);
        setStyleSheet(QStringLiteral("QGraphicsView { background: transparent; border: none; }"));
    }

  protected:
    void resizeEvent(QResizeEvent *event) override
    {
        QGraphicsView::resizeEvent(event);
        if (viewport()->width() <= 0 || viewport()->height() <= 0) {
            return;
        }
        const QRectF canvasRect(m_canvas->rect());
        setSceneRect(canvasRect);
        const qreal factor =
            std::min(viewport()->width() / canvasRect.width(), viewport()->height() / canvasRect.height());
        setTransform(QTransform::fromScale(factor, factor));
        centerOn(canvasRect.center());
    }

  private:
    QWidget *m_canvas;
};

} // namespace

PlayerWindow::PlayerWindow(const Project &project, QWidget *parent, bool startRuntime) : FluentQt::FluentWidget(parent)
{
    // FluentWidget enables Windows 11 Mica by default; follow the studio setting (default off)
    // so DWM transparent compositing does not stall QGraphicsView + dialog repaints.
    setMicaEffectEnabled(FluentQt::FluentConfig::instance()->isMicaEnabled());
#ifdef FHMI_CAN_DIAGNOSTICS
    setWindowTitle(project.appName + QStringLiteral(" [CAN diagnostics]"));
#else
    setWindowTitle(project.appName);
#endif
    const QIcon icon = iconFromFile(project.iconPath());
    setWindowIcon(icon.isNull() ? QIcon(QStringLiteral(":/fluenthmi/logo.png")) : icon);
    titleBar()->raise();

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, titleBar()->height(), 0, 0);
    m_surface = new RuntimeSurface;
    layout->addWidget(new CanvasView(m_surface, this), 1);

    // Place runtime utility buttons in the title bar beside window controls so they never steal space
    // from the scaled canvas or trigger a canvas rescale when periodic tasks start/stop.
    if (auto *barLayout = qobject_cast<QHBoxLayout *>(titleBar()->layout())) {
        auto *tools = new QWidget(titleBar());
        auto *toolsLayout = new QHBoxLayout(tools);
        toolsLayout->setContentsMargins(0, 0, 6, 0);
        toolsLayout->setSpacing(4);
        if (!project.logic.value(QStringLiteral("points")).toArray().isEmpty()) {
            auto *monitor = new FluentQt::TransparentPushButton(FluentQt::icon(FluentQt::FluentIcon::View),
                                                                tr("工程数据监视"), tools);
            monitor->setObjectName(QStringLiteral("playerDataMonitorButton"));
            monitor->setFixedHeight(26);
            const QJsonObject logic = project.logic;
            connect(monitor, &QPushButton::clicked, this, [this, logic]() {
                openProjectDataMonitor(logic, m_surface->context()->projectData(), this);
            });
            toolsLayout->addWidget(monitor);
        }
        QPushButton *periodic = createPeriodicTasksButton(m_surface->context()->periodicSender(), tools);
        periodic->setFixedHeight(26);
        toolsLayout->addWidget(periodic);
        const int insertAt = std::max(0, barLayout->count() - 1);
        barLayout->insertWidget(insertAt, tools, 0, Qt::AlignVCenter);
    }

    connect(m_surface, &RuntimeSurface::notificationRequested, this,
            [this](const QString &title, const QString &content, const QString &type, int duration) {
                using FluentQt::InfoBar;
                using FluentQt::InfoBarPosition;
                const auto show = type == NotifyType::Success   ? &InfoBar::success
                                  : type == NotifyType::Warning ? &InfoBar::warning
                                  : type == NotifyType::Error   ? &InfoBar::error
                                                                : &InfoBar::info;
                show(title, content, Qt::Horizontal, true, duration > 0 ? duration : 2000, InfoBarPosition::TopRight,
                     this);
            });
    m_surface->build(project, startRuntime);

    QSize size(project.canvasWidth + 24, project.canvasHeight + titleBar()->height() + 24);
    if (QScreen *screen = QGuiApplication::primaryScreen()) {
        size = size.boundedTo(screen->availableGeometry().size() * 0.95);
    }
    resize(size);
}

void PlayerWindow::resizeEvent(QResizeEvent *event)
{
    FluentQt::FluentWidget::resizeEvent(event);
    // FluentWidget::resizeEvent raises titleBar(); keep any active overlay dialog above it.
    for (auto *overlay : findChildren<FluentQt::MaskDialogBase *>(QString(), Qt::FindDirectChildrenOnly)) {
        if (overlay->isVisible()) {
            overlay->raise();
        }
    }
}

} // namespace fhmi
