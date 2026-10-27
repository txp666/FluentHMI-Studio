#include "player/player_window.h"

#include "runtime/runtime_context.h"
#include "runtime/runtime_surface.h"

#include <FluentQtWidgets/Widgets/InfoBar.h>
#include <FluentQtWidgets/Widgets/ScrollArea.h>
#include <FluentQtWidgets/Window/FluentTitleBar.h>

#include <QtGui/QGuiApplication>
#include <QtGui/QScreen>
#include <QtWidgets/QVBoxLayout>

namespace fhmi {

PlayerWindow::PlayerWindow(const Project &project, QWidget *parent) : FluentQt::FluentWidget(parent)
{
    setWindowTitle(project.appName);
    setWindowIcon(QIcon(QStringLiteral(":/fluenthmi/logo.png")));
    titleBar()->raise();

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, titleBar()->height(), 0, 0);
    auto *scroll = new FluentQt::ScrollArea(this);
    scroll->setWidgetResizable(false);
    scroll->setAlignment(Qt::AlignCenter);
    scroll->setFrameShape(QFrame::NoFrame);
    m_surface = new RuntimeSurface;
    scroll->setWidget(m_surface);
    scroll->enableTransparentBackground();
    layout->addWidget(scroll);

    connect(m_surface, &RuntimeSurface::notificationRequested, this,
            [this](const QString &title, const QString &content, const QString &type, int duration) {
                using FluentQt::InfoBar;
                using FluentQt::InfoBarPosition;
                const auto show = type == NotifyType::Success   ? &InfoBar::success
                                  : type == NotifyType::Warning ? &InfoBar::warning
                                  : type == NotifyType::Error   ? &InfoBar::error
                                                                : &InfoBar::info;
                show(title, content, Qt::Horizontal, true, duration > 0 ? duration : 2000,
                     InfoBarPosition::TopRight, this);
            });
    m_surface->build(project);

    QSize size(project.canvasWidth + 24, project.canvasHeight + titleBar()->height() + 24);
    if (QScreen *screen = QGuiApplication::primaryScreen()) {
        size = size.boundedTo(screen->availableGeometry().size() * 0.95);
    }
    resize(size);
}

} // namespace fhmi
