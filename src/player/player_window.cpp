#include "player/player_window.h"

#include "runtime/runtime_context.h"
#include "runtime/runtime_surface.h"
#include "runtime/ui_helpers.h"

#include <FluentQtWidgets/Widgets/InfoBar.h>
#include <FluentQtWidgets/Window/FluentTitleBar.h>

#include <QtGui/QGuiApplication>
#include <QtGui/QPainter>
#include <QtGui/QScreen>
#include <QtGui/QTransform>
#include <QtWidgets/QGraphicsScene>
#include <QtWidgets/QGraphicsView>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>

namespace fhmi {

namespace {

// Keep the design coordinates intact; the view scales painting and input together.
class CanvasView : public QGraphicsView
{
  public:
    CanvasView(QWidget *canvas, QWidget *parent) : QGraphicsView(parent), m_canvas(canvas)
    {
        setObjectName(QStringLiteral("canvasView"));
        auto *scene = new QGraphicsScene(this);
        setScene(scene);
        scene->addWidget(canvas);
        setFrameShape(QFrame::NoFrame);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setAlignment(Qt::AlignCenter);
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

PlayerWindow::PlayerWindow(const Project &project, QWidget *parent) : FluentQt::FluentWidget(parent)
{
    setWindowTitle(project.appName);
    const QIcon icon = iconFromFile(project.iconPath());
    setWindowIcon(icon.isNull() ? QIcon(QStringLiteral(":/fluenthmi/logo.png")) : icon);
    titleBar()->raise();

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, titleBar()->height(), 0, 0);
    m_surface = new RuntimeSurface;
    layout->addWidget(new CanvasView(m_surface, this));

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
    m_surface->build(project);

    QSize size(project.canvasWidth + 24, project.canvasHeight + titleBar()->height() + 24);
    if (QScreen *screen = QGuiApplication::primaryScreen()) {
        size = size.boundedTo(screen->availableGeometry().size() * 0.95);
    }
    resize(size);
}

} // namespace fhmi
