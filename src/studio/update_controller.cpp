#include "studio/update_controller.h"

#include "runtime/runtime_context.h"
#include "studio/update_dialog.h"
#include "studio/update_installer.h"
#include "studio/update_service.h"

#include <QtCore/QEventLoop>
#include <QtCore/QSettings>
#include <QtCore/QTextStream>
#include <QtWidgets/QWidget>

namespace fhmi {

void showInfoBar(QWidget *parent, const QString &title, const QString &content, const QString &type, int durationMs);

namespace {

const QString CheckAtStartupKey = QStringLiteral("update/checkAtStartup");
const QString SkippedVersionKey = QStringLiteral("update/skippedVersion");

} // namespace

UpdateController::UpdateController(QWidget *window)
    : QObject(window), m_window(window), m_service(new UpdateService(this)), m_installer(new UpdateInstaller(this))
{
    connect(m_service, &UpdateService::checked, this, [this](const ReleaseInfo &release) {
        if (!m_checking) {
            return;
        }
        m_checking = false;
        emit checkingChanged(false);
        offer(release, m_manual);
    });
    // Download failures go to the dialog; only a failed check is reported here.
    connect(m_service, &UpdateService::failed, this, [this](const QString &message) {
        if (!m_checking) {
            return;
        }
        m_checking = false;
        emit checkingChanged(false);
        if (m_manual) {
            showInfoBar(m_window, tr("检查更新失败"), message, NotifyType::Error, 5000);
        }
    });
}

bool UpdateController::checkAtStartup() { return QSettings().value(CheckAtStartupKey, true).toBool(); }

void UpdateController::setCheckAtStartup(bool enabled) { QSettings().setValue(CheckAtStartupKey, enabled); }

void UpdateController::startupCheck()
{
    const QString report = UpdateInstaller::takeFailureReport();
    if (!report.isEmpty()) {
        showInfoBar(m_window, tr("上次更新没有完成，仍在使用原来的版本"), report, NotifyType::Warning, 8000);
    }
    if (checkAtStartup()) {
        check(false);
    }
}

void UpdateController::checkNow() { check(true); }

void UpdateController::check(bool manual)
{
    if (m_checking) {
        return;
    }
    if (!m_preparedTag.isEmpty()) {
        if (manual) {
            showInfoBar(m_window, tr("新版本 %1 已下载").arg(m_preparedTag), tr("退出 Studio 时会自动完成更新"),
                        NotifyType::Info, 4000);
        }
        return;
    }
    m_checking = true;
    m_manual = manual;
    emit checkingChanged(true);
    m_service->check();
}

void UpdateController::offer(const ReleaseInfo &release, bool manual)
{
    if (!UpdateRelease::isNewer(release, QStringLiteral(FHMI_VERSION))) {
        if (manual) {
            showInfoBar(m_window, tr("已是最新版本"), tr("当前版本 v%1").arg(QStringLiteral(FHMI_VERSION)),
                        NotifyType::Success, 3000);
        }
        return;
    }
    if (!manual && QSettings().value(SkippedVersionKey).toString() == release.tag) {
        return;
    }
    UpdateDialog dialog(release, m_service, m_installer, !manual, m_window);
    connect(&dialog, &UpdateDialog::skipRequested, this,
            [&release]() { QSettings().setValue(SkippedVersionKey, release.tag); });
    const bool restart = dialog.exec() == QDialog::Accepted;
    if (!m_installer->isPrepared()) {
        // Closed before the update was ready: drop whatever was under way.
        m_service->abort();
        m_installer->discard();
        return;
    }
    m_preparedTag = release.tag;
    if (restart) {
        emit restartRequested();
    } else {
        showInfoBar(m_window, tr("新版本 %1 已就绪").arg(release.tag), tr("退出 Studio 时会自动完成更新"),
                    NotifyType::Info, 4000);
    }
}

bool UpdateController::finishOnExit(bool relaunch, QString *error)
{
    if (!m_installer->isPrepared()) {
        return true;
    }
    return m_installer->launch(relaunch, error);
}

int UpdateController::runHeadless()
{
    QTextStream out(stdout);
    QTextStream err(stderr);
    UpdateService service;
    UpdateInstaller installer;
    QEventLoop loop;
    const auto failWith = [&](const QString &message) {
        err << message << Qt::endl;
        installer.discard();
        loop.exit(1);
    };
    QObject::connect(&service, &UpdateService::failed, &loop, failWith);
    QObject::connect(&installer, &UpdateInstaller::failed, &loop, failWith);
    QObject::connect(&service, &UpdateService::checked, &loop, [&](const ReleaseInfo &release) {
        if (!UpdateRelease::isNewer(release, QStringLiteral(FHMI_VERSION))) {
            out << "FluentHMI Studio " FHMI_VERSION " is up to date (latest " << release.tag << ")" << Qt::endl;
            loop.exit(0);
            return;
        }
        if (!release.hasPackage()) {
            failWith(tr("%1 没有适用于本平台的安装包").arg(release.tag));
            return;
        }
        QString reason;
        if (!UpdateInstaller::canInstall(&reason)) {
            failWith(reason);
            return;
        }
        out << "downloading " << release.package.name << Qt::endl;
        service.download(release, UpdateInstaller::stagingDir());
    });
    QObject::connect(&service, &UpdateService::downloaded, &loop, [&](const QString &path) {
        out << "checksum verified, unpacking" << Qt::endl;
        installer.prepare(path);
    });
    QObject::connect(&installer, &UpdateInstaller::prepared, &loop, [&]() {
        QString error;
        if (!installer.launch(false, &error)) {
            failWith(error);
            return;
        }
        out << "installing: the new version replaces this one once this process has exited" << Qt::endl;
        loop.exit(0);
    });
    service.check();
    return loop.exec();
}

} // namespace fhmi
