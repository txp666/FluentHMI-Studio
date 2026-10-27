#include "studio/update_dialog.h"

#include "studio/update_installer.h"
#include "studio/update_service.h"

#include <FluentQtWidgets/Widgets/Button.h>
#include <FluentQtWidgets/Widgets/Label.h>
#include <FluentQtWidgets/Widgets/LineEdit.h>
#include <FluentQtWidgets/Widgets/ProgressBar.h>

#include <QtGui/QDesktopServices>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QVBoxLayout>

namespace fhmi {

UpdateDialog::UpdateDialog(const ReleaseInfo &release, UpdateService *service, UpdateInstaller *installer,
                           bool offerSkip, QWidget *parent)
    : FluentQt::MessageBoxBase(parent), m_release(release), m_service(service), m_installer(installer)
{
    if (!release.hasPackage()) {
        m_cannotInstall = tr("这个版本没有适用于本平台的安装包");
    } else {
        UpdateInstaller::canInstall(&m_cannotInstall);
    }

    auto *title = new FluentQt::SubtitleLabel(tr("发现新版本 %1").arg(release.tag), this);
    auto *versions = new FluentQt::BodyLabel(tr("当前版本 v%1").arg(QStringLiteral(FHMI_VERSION)), this);
    auto *notes = new FluentQt::TextBrowser(this);
    notes->setMarkdown(release.notes.isEmpty() ? tr("（这个版本没有更新说明）") : release.notes);
    notes->setOpenExternalLinks(true);
    notes->setMinimumWidth(460);
    notes->setFixedHeight(220);
    auto *page = new FluentQt::HyperlinkButton(release.page.toString(), tr("在浏览器中查看发布页"), this);
    m_progress = new FluentQt::ProgressBar(this);
    m_progress->setRange(0, 1000);
    m_progress->hide();
    m_status = new FluentQt::CaptionLabel(this);
    m_status->setWordWrap(true);

    viewLayout()->addWidget(title);
    viewLayout()->addWidget(versions);
    viewLayout()->addWidget(notes);
    viewLayout()->addWidget(page, 0, Qt::AlignLeft);
    viewLayout()->addWidget(m_progress);
    viewLayout()->addWidget(m_status);

    if (offerSkip) {
        m_skip = new FluentQt::PushButton(tr("跳过此版本"), buttonGroup());
        buttonLayout()->insertWidget(1, m_skip);
        connect(m_skip, &QPushButton::clicked, this, [this]() {
            emit skipRequested();
            reject();
        });
    }
    // The yes button drives the update instead of closing the dialog straight away.
    disconnect(yesButton(), &QPushButton::clicked, this, nullptr);
    connect(yesButton(), &QPushButton::clicked, this, &UpdateDialog::proceed);

    connect(m_service, &UpdateService::progress, this, [this](qint64 received, qint64 total) {
        if (total > 0) {
            m_progress->setValue(int(received * 1000 / total));
            m_status->setText(tr("正在下载 %1 / %2 MB")
                                  .arg(double(received) / (1 << 20), 0, 'f', 1)
                                  .arg(double(total) / (1 << 20), 0, 'f', 1));
        }
    });
    connect(m_service, &UpdateService::downloaded, this, [this](const QString &path) {
        setState(State::Working, tr("校验通过，正在解压…"));
        m_installer->prepare(path);
    });
    connect(m_service, &UpdateService::failed, this,
            [this](const QString &message) { setState(State::Failed, tr("下载失败：%1").arg(message)); });
    connect(m_installer, &UpdateInstaller::prepared, this, [this]() {
        m_progress->setValue(1000);
        setState(State::Ready, tr("新版本已就绪。重启 Studio 即完成更新；也可以稍后，退出时会自动更新。"));
    });
    connect(m_installer, &UpdateInstaller::failed, this,
            [this](const QString &message) { setState(State::Failed, message); });

    setState(State::Offer, m_cannotInstall.isEmpty() ? QString() : tr("无法自动更新：%1").arg(m_cannotInstall));
}

void UpdateDialog::proceed()
{
    switch (m_state) {
    case State::Offer:
    case State::Failed:
        if (!m_cannotInstall.isEmpty()) {
            QDesktopServices::openUrl(m_release.page);
            reject();
            return;
        }
        m_progress->setValue(0);
        setState(State::Working, tr("正在连接…"));
        m_service->download(m_release, UpdateInstaller::stagingDir());
        break;
    case State::Ready:
        accept();
        break;
    case State::Working:
        break;
    }
}

void UpdateDialog::setState(State state, const QString &status)
{
    m_state = state;
    m_status->setText(status);
    m_status->setVisible(!status.isEmpty());
    m_progress->setVisible(state == State::Working || state == State::Ready);
    m_progress->setError(false);
    if (m_skip) {
        m_skip->setVisible(state == State::Offer);
    }
    yesButton()->setEnabled(state != State::Working);
    switch (state) {
    case State::Offer:
        yesButton()->setText(m_cannotInstall.isEmpty() ? tr("立即更新") : tr("前往下载"));
        cancelButton()->setText(tr("以后再说"));
        break;
    case State::Working:
        yesButton()->setText(tr("正在更新…"));
        cancelButton()->setText(tr("取消"));
        break;
    case State::Ready:
        yesButton()->setText(tr("重启并更新"));
        cancelButton()->setText(tr("稍后"));
        break;
    case State::Failed:
        yesButton()->setText(tr("重试"));
        cancelButton()->setText(tr("关闭"));
        break;
    }
}

} // namespace fhmi
