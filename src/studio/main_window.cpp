#include "studio/main_window.h"

#include "runtime/runtime_context.h"
#include "studio/designer_page.h"
#include "studio/export_service.h"
#include "studio/preview_page.h"
#include "studio/settings_page.h"
#include "studio/update_controller.h"

#include <FluentQtWidgets/Config.h>
#include <FluentQtWidgets/Dialogs/Dialog.h>
#include <FluentQtWidgets/FluentIcon.h>
#include <FluentQtWidgets/Navigation/NavigationInterface.h>
#include <FluentQtWidgets/Widgets/Button.h>
#include <FluentQtWidgets/Widgets/Label.h>
#include <FluentQtWidgets/Widgets/ProgressBar.h>

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QMimeData>
#include <QtCore/QPointer>
#include <QtCore/QSettings>
#include <QtCore/QStandardPaths>
#include <QtCore/QThread>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtGui/QAction>
#include <QtGui/QCloseEvent>
#include <QtGui/QDesktopServices>
#include <QtGui/QDragEnterEvent>
#include <QtGui/QDropEvent>
#include <QtGui/QGuiApplication>
#include <QtGui/QScreen>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QVBoxLayout>

namespace fhmi {

void showInfoBar(QWidget *parent, const QString &title, const QString &content, const QString &type, int durationMs);

namespace {

const QString LastExportDirKey = QStringLiteral("studio/lastExportDir");

QString firstProjectUrl(const QMimeData *mime)
{
    if (!mime || !mime->hasUrls()) {
        return {};
    }
    for (const QUrl &url : mime->urls()) {
        if (!url.isLocalFile()) {
            continue;
        }
        const QString file = url.toLocalFile();
        if (file.endsWith(QLatin1String(".uidp"), Qt::CaseInsensitive) && QFileInfo(file).isFile()) {
            return file;
        }
    }
    return {};
}

class ExportProgressDialog : public FluentQt::MessageBoxBase
{
  public:
    ExportProgressDialog(const QString &appName, QWidget *parent) : FluentQt::MessageBoxBase(parent)
    {
        setClosableOnMaskClicked(false);
        auto *title = new FluentQt::SubtitleLabel(tr("正在导出应用"), this);
        auto *detail = new FluentQt::BodyLabel(tr("正在打包“%1”，请稍候…").arg(appName), this);
        detail->setWordWrap(true);
        auto *bar = new FluentQt::IndeterminateProgressBar(this);
        bar->setMinimumWidth(360);
        bar->start();
        viewLayout()->addWidget(title);
        viewLayout()->addWidget(detail);
        viewLayout()->addWidget(bar);
        buttonGroup()->hide();
    }

    void allowClose() { m_allowClose = true; }

  protected:
    void closeEvent(QCloseEvent *event) override
    {
        if (!m_allowClose) {
            event->ignore();
            return;
        }
        FluentQt::MessageBoxBase::closeEvent(event);
    }

    void keyPressEvent(QKeyEvent *event) override
    {
        if (!m_allowClose && event->key() == Qt::Key_Escape) {
            event->accept();
            return;
        }
        FluentQt::MessageBoxBase::keyPressEvent(event);
    }

  private:
    bool m_allowClose = false;
};

} // namespace

MainWindow::MainWindow(QWidget *parent) : FluentQt::FluentWindow(parent)
{
    setWindowIcon(QIcon(QStringLiteral(":/fluenthmi/logo.png")));
    setAcceptDrops(true);
    m_designer = new DesignerPage(this);
    m_preview = new PreviewPage(this);
    m_settings = new SettingsPage(this);

    addSubInterface(m_designer, FluentQt::icon(FluentQt::FluentIcon::Edit), tr("界面设计"), QStringLiteral("design"));
    addSubInterface(m_preview, FluentQt::icon(FluentQt::FluentIcon::Play), tr("预览运行"), QStringLiteral("preview"));
    if (auto *recentNav = navigationInterface()->addItem(QStringLiteral("recent"),
                                                         FluentQt::icon(FluentQt::FluentIcon::History),
                                                         tr("最近打开"), FluentQt::NavigationItemPosition::Bottom,
                                                         QString(), false)) {
        recentNav->setToolTip(tr("最近打开"));
        connect(recentNav, &FluentQt::NavigationWidget::clicked, this, [this, recentNav]() {
            m_designer->showRecentProjectsMenu(recentNav->mapToGlobal(QPoint(recentNav->width() + 4, 0)));
        });
    }
    if (auto *exportNav = navigationInterface()->addItem(QStringLiteral("export"),
                                                         FluentQt::icon(FluentQt::FluentIcon::Application),
                                                         tr("导出应用"), FluentQt::NavigationItemPosition::Bottom,
                                                         QString(), false)) {
        exportNav->setToolTip(QStringLiteral("%1 (%2)").arg(
            tr("导出应用"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_E).toString(QKeySequence::NativeText)));
        connect(exportNav, &FluentQt::NavigationWidget::clicked, this, &MainWindow::exportApplication);
    }
    addSubInterface(m_settings, FluentQt::icon(FluentQt::FluentIcon::Setting), tr("设置"), QStringLiteral("settings"),
                    FluentQt::NavigationItemPosition::Bottom);
    // Mica (Windows 11 only) is a setting, switched on the settings page.
    setMicaEffectEnabled(FluentQt::FluentConfig::instance()->isMicaEnabled());
    connect(FluentQt::FluentConfig::instance(), &FluentQt::FluentConfig::micaEnabledChanged, this,
            &MainWindow::setMicaEffectEnabled);

    connect(m_designer, &DesignerPage::projectChanged, this, [this](bool keepPreview) {
        if (!keepPreview) {
            m_preview->invalidate();
        }
        m_preview->setProject(m_designer->snapshot());
    });
    connect(m_designer, &DesignerPage::titleChanged, this, &MainWindow::updateTitle);
    connect(m_designer, &DesignerPage::notify, this, [this](const QString &title, const QString &content, bool error) {
        showInfoBar(this, title, content, error ? NotifyType::Error : NotifyType::Success, error ? 4000 : 1500);
    });
    connect(m_designer, &DesignerPage::warning, this, [this](const QString &title, const QString &content) {
        showInfoBar(this, title, content, NotifyType::Warning, 3000);
    });
    connect(m_preview, &PreviewPage::exportRequested, this, &MainWindow::exportApplication);
    connect(m_preview, &PreviewPage::endpointObserved, m_designer, &DesignerPage::rememberEndpoint);
    connect(m_preview, &PreviewPage::textSampleObserved, m_designer, &DesignerPage::rememberTextSample);
    connect(m_preview, &PreviewPage::receivePointRequested, m_designer, &DesignerPage::addPointFromPreview,
            Qt::QueuedConnection);

    m_updates = new UpdateController(this);
    connect(m_settings, &SettingsPage::checkUpdateRequested, m_updates, &UpdateController::checkNow);
    connect(m_updates, &UpdateController::checkingChanged, m_settings, &SettingsPage::setUpdateChecking);
    connect(m_updates, &UpdateController::restartRequested, this, [this]() {
        m_restartForUpdate = true;
        close();
    });
    // Once the window is up, so a notice has somewhere to appear.
#ifndef FHMI_CAN_DIAGNOSTICS
    QTimer::singleShot(1500, m_updates, &UpdateController::startupCheck);
#endif

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
#ifdef FHMI_CAN_DIAGNOSTICS
    setWindowTitle(QStringLiteral("%1 — FluentHMI Studio [CAN diagnostics]").arg(m_designer->displayName()));
#else
    setWindowTitle(QStringLiteral("%1 — FluentHMI Studio").arg(m_designer->displayName()));
#endif
}

void MainWindow::exportApplication()
{
    if (m_exporting) {
        return;
    }
    const Project project = m_designer->snapshot();
    const QString remembered = QSettings().value(LastExportDirKey).toString();
    QString start = !remembered.isEmpty() && QDir(remembered).exists()
                        ? remembered
                        : (project.filePath.isEmpty()
                               ? QStandardPaths::writableLocation(QStandardPaths::DesktopLocation)
                               : QFileInfo(project.filePath).absolutePath());
    const QString dir = QFileDialog::getExistingDirectory(this, tr("选择导出目录"), start);
    if (dir.isEmpty()) {
        return;
    }
    QSettings().setValue(LastExportDirKey, dir);

    const QString target = ExportService::targetPath(project, dir);
    bool overwrite = false;
    if (QFileInfo::exists(target)) {
        FluentQt::MessageBox confirm(tr("覆盖已有应用？"),
                                     tr("“%1”已存在，是否覆盖？").arg(QDir::toNativeSeparators(target)), this);
        confirm.yesButton()->setText(tr("覆盖"));
        confirm.cancelButton()->setText(tr("取消"));
        if (confirm.exec() != QDialog::Accepted) {
            return;
        }
        overwrite = true;
    }

    m_exporting = true;
    const auto appearance = ExportService::currentAppearance();
    auto *progress = new ExportProgressDialog(project.appName, this);
    progress->setAttribute(Qt::WA_DeleteOnClose);

    auto *thread = QThread::create([window = QPointer<MainWindow>(this), progress = QPointer<ExportProgressDialog>(progress),
                                    project, dir, overwrite, appearance]() {
        QString error;
        const QString created = ExportService::exportApplication(project, dir, &error, overwrite, appearance);
        if (!window) {
            return;
        }
        QMetaObject::invokeMethod(
            window,
            [window, progress, dir, created, error]() {
                if (progress) {
                    progress->allowClose();
                    progress->accept();
                }
                if (!window) {
                    return;
                }
                window->m_exporting = false;
                if (created.isEmpty()) {
                    showInfoBar(window, tr("导出失败"), error, NotifyType::Error, 5000);
                    return;
                }
                showInfoBar(window, tr("导出完成"), QDir::toNativeSeparators(created), NotifyType::Success, 4000);
                QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
            },
            Qt::QueuedConnection);
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
    progress->exec();
}

void MainWindow::dragEnterEvent(QDragEnterEvent *event)
{
    if (!firstProjectUrl(event->mimeData()).isEmpty()) {
        event->acceptProposedAction();
        return;
    }
    FluentQt::FluentWindow::dragEnterEvent(event);
}

void MainWindow::dropEvent(QDropEvent *event)
{
    const QString file = firstProjectUrl(event->mimeData());
    if (!file.isEmpty()) {
        event->acceptProposedAction();
        openProject(file);
        return;
    }
    FluentQt::FluentWindow::dropEvent(event);
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (m_exporting) {
        event->ignore();
        return;
    }
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
