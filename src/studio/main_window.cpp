#include "studio/main_window.h"

#include "runtime/runtime_context.h"
#include "studio/designer_page.h"
#include "studio/export_service.h"
#include "studio/preview_page.h"
#include "studio/settings_page.h"
#include "studio/update_controller.h"

#include <FluentQtWidgets/FluentIcon.h>
#include <FluentQtWidgets/Navigation/NavigationInterface.h>

#include <QtCore/QDir>
#include <QtCore/QStandardPaths>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtGui/QAction>
#include <QtGui/QCloseEvent>
#include <QtGui/QDesktopServices>
#include <QtGui/QGuiApplication>
#include <QtGui/QScreen>
#include <QtWidgets/QFileDialog>

namespace fhmi {

void showInfoBar(QWidget *parent, const QString &title, const QString &content, const QString &type, int durationMs);

MainWindow::MainWindow(QWidget *parent) : FluentQt::FluentWindow(parent)
{
    setWindowIcon(QIcon(QStringLiteral(":/fluenthmi/logo.png")));
    m_designer = new DesignerPage(this);
    m_preview = new PreviewPage(this);
    m_settings = new SettingsPage(this);

    addSubInterface(m_designer, FluentQt::icon(FluentQt::FluentIcon::Edit), tr("界面设计"), QStringLiteral("design"));
    addSubInterface(m_preview, FluentQt::icon(FluentQt::FluentIcon::Play), tr("预览运行"), QStringLiteral("preview"));
    addSubInterface(m_settings, FluentQt::icon(FluentQt::FluentIcon::Setting), tr("设置"), QStringLiteral("settings"),
                    FluentQt::NavigationItemPosition::Bottom);

    connect(m_designer, &DesignerPage::projectChanged, this, [this]() {
        m_preview->invalidate();
        m_preview->setProject(m_designer->snapshot());
    });
    connect(m_designer, &DesignerPage::titleChanged, this, &MainWindow::updateTitle);
    connect(m_designer, &DesignerPage::notify, this, [this](const QString &title, const QString &content, bool error) {
        showInfoBar(this, title, content, error ? NotifyType::Error : NotifyType::Success, error ? 4000 : 1500);
    });
    connect(m_preview, &PreviewPage::exportRequested, this, &MainWindow::exportApplication);

    m_updates = new UpdateController(this);
    connect(m_settings, &SettingsPage::checkUpdateRequested, m_updates, &UpdateController::checkNow);
    connect(m_updates, &UpdateController::checkingChanged, m_settings, &SettingsPage::setUpdateChecking);
    connect(m_updates, &UpdateController::restartRequested, this, [this]() {
        m_restartForUpdate = true;
        close();
    });
    // Once the window is up, so a notice has somewhere to appear.
    QTimer::singleShot(1500, m_updates, &UpdateController::startupCheck);

    auto *exportAction = new QAction(this);
    exportAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_E));
    connect(exportAction, &QAction::triggered, this, &MainWindow::exportApplication);
    addAction(exportAction);

    if (QScreen *screen = QGuiApplication::primaryScreen()) {
        const QRect available = screen->availableGeometry();
        resize(QSize(1440, 900).boundedTo(available.size() * 0.92));
        move(available.center() - rect().center());
    }
    setMinimumSize(QSize(1000, 640).boundedTo(size()));
    m_preview->setProject(m_designer->snapshot());
    updateTitle();
}

void MainWindow::openProject(const QString &path)
{
    m_designer->openProject(path);
}

void MainWindow::updateTitle()
{
    setWindowTitle(QStringLiteral("%1 — FluentHMI Studio").arg(m_designer->displayName()));
}

void MainWindow::exportApplication()
{
    const Project project = m_designer->snapshot();
    const QString start = project.filePath.isEmpty()
                              ? QStandardPaths::writableLocation(QStandardPaths::DesktopLocation)
                              : QFileInfo(project.filePath).absolutePath();
    const QString dir = QFileDialog::getExistingDirectory(this, tr("选择导出目录"), start);
    if (dir.isEmpty()) {
        return;
    }
    QString error;
    const QString created = ExportService::exportApplication(project, dir, &error);
    if (created.isEmpty()) {
        showInfoBar(this, tr("导出失败"), error, NotifyType::Error, 5000);
        return;
    }
    showInfoBar(this, tr("导出完成"), QDir::toNativeSeparators(created), NotifyType::Success, 4000);
    QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (!m_designer->maybeSave()) {
        m_restartForUpdate = false;
        event->ignore();
        return;
    }
    // A downloaded update is installed whenever the studio exits; it restarts only when the
    // user asked to restart into it.
    QString error;
    if (!m_updates->finishOnExit(m_restartForUpdate, &error) && m_restartForUpdate) {
        m_restartForUpdate = false;
        showInfoBar(this, tr("无法安装更新"), error, NotifyType::Error, 6000);
        event->ignore();
        return;
    }
    m_preview->invalidate();
    FluentQt::FluentWindow::closeEvent(event);
}

} // namespace fhmi
