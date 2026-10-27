#include "studio/preview_page.h"

#include "runtime/runtime_context.h"
#include "runtime/runtime_surface.h"

#include <FluentQtWidgets/FluentIcon.h>
#include <FluentQtWidgets/Widgets/Button.h>
#include <FluentQtWidgets/Widgets/InfoBar.h>
#include <FluentQtWidgets/Widgets/Label.h>
#include <FluentQtWidgets/Widgets/ScrollArea.h>

#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QVBoxLayout>

namespace fhmi {

void showInfoBar(QWidget *parent, const QString &title, const QString &content, const QString &type, int durationMs)
{
    using FluentQt::InfoBar;
    using FluentQt::InfoBarPosition;
    const int duration = durationMs > 0 ? durationMs : 2000;
    if (type == NotifyType::Success) {
        InfoBar::success(title, content, Qt::Horizontal, true, duration, InfoBarPosition::TopRight, parent);
    } else if (type == NotifyType::Warning) {
        InfoBar::warning(title, content, Qt::Horizontal, true, duration, InfoBarPosition::TopRight, parent);
    } else if (type == NotifyType::Error) {
        InfoBar::error(title, content, Qt::Horizontal, true, duration, InfoBarPosition::TopRight, parent);
    } else {
        InfoBar::info(title, content, Qt::Horizontal, true, duration, InfoBarPosition::TopRight, parent);
    }
}

PreviewPage::PreviewPage(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("previewPage"));
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 8, 16, 16);
    layout->setSpacing(10);

    auto *bar = new QHBoxLayout;
    m_run = new FluentQt::PrimaryPushButton(this);
    m_run->setIcon(FluentQt::icon(FluentQt::FluentIcon::Play));
    m_run->setText(tr("运行预览"));
    m_export = new FluentQt::PushButton(this);
    m_export->setIcon(FluentQt::icon(FluentQt::FluentIcon::Application));
    m_export->setText(tr("导出应用"));
    m_status = new FluentQt::CaptionLabel(tr("已停止"), this);
    bar->addWidget(m_run);
    bar->addWidget(m_export);
    bar->addSpacing(12);
    bar->addWidget(m_status);
    bar->addStretch();

    auto *scroll = new FluentQt::ScrollArea(this);
    scroll->setWidgetResizable(false);
    scroll->setAlignment(Qt::AlignCenter);
    scroll->setFrameShape(QFrame::NoFrame);
    m_surface = new RuntimeSurface;
    scroll->setWidget(m_surface);
    scroll->enableTransparentBackground();

    layout->addLayout(bar);
    layout->addWidget(scroll, 1);

    connect(m_run, &QPushButton::clicked, this, [this]() { m_running ? stop() : start(); });
    connect(m_export, &QPushButton::clicked, this, &PreviewPage::exportRequested);
    connect(m_surface, &RuntimeSurface::notificationRequested, this, &PreviewPage::showNotification);
}

void PreviewPage::setProject(const Project &project)
{
    m_project = project;
    if (!m_running) {
        m_surface->setFixedSize(project.canvasWidth, project.canvasHeight);
    }
}

void PreviewPage::invalidate()
{
    if (m_running) {
        stop();
        m_status->setText(tr("设计已修改，预览已停止"));
    }
}

void PreviewPage::start()
{
    m_surface->build(m_project);
    m_running = true;
    m_run->setText(tr("停止预览"));
    m_run->setIcon(FluentQt::icon(FluentQt::FluentIcon::Pause));
    m_status->setText(tr("运行中 · 用链路卡片连接设备"));
}

void PreviewPage::stop()
{
    m_surface->clear();
    m_running = false;
    m_run->setText(tr("运行预览"));
    m_run->setIcon(FluentQt::icon(FluentQt::FluentIcon::Play));
    m_status->setText(tr("已停止"));
}

void PreviewPage::showNotification(const QString &title, const QString &content, const QString &type, int durationMs)
{
    showInfoBar(window(), title, content, type, durationMs);
}

} // namespace fhmi
