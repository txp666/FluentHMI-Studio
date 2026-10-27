#include "studio/settings_page.h"

#include "studio/update_controller.h"

#include <FluentQtWidgets/Config.h>
#include <FluentQtWidgets/FluentIcon.h>
#include <FluentQtWidgets/Settings/SettingCard.h>
#include <FluentQtWidgets/Theme.h>
#include <FluentQtWidgets/Widgets/ComboBox.h>
#include <FluentQtWidgets/Widgets/Label.h>
#include <FluentQtWidgets/Widgets/ScrollArea.h>

#include <QtCore/QUrl>
#include <QtGui/QDesktopServices>
#include <QtWidgets/QVBoxLayout>

namespace fhmi {

SettingsPage::SettingsPage(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("settingsPage"));
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    auto *scroll = new FluentQt::ScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto *content = new QWidget;
    content->setAttribute(Qt::WA_TranslucentBackground);
    auto *layout = new QVBoxLayout(content);
    layout->setContentsMargins(36, 16, 36, 24);
    layout->setSpacing(12);
    layout->addWidget(new FluentQt::TitleLabel(tr("设置"), content));

    auto *appearance = new FluentQt::SettingCardGroup(tr("外观"), content);
    auto *theme = new FluentQt::ComboBoxSettingCard(
        QStringList{tr("浅色"), tr("深色"), tr("跟随系统")}, FluentQt::FluentIcon::Brush, tr("主题"),
        tr("设计画布、预览和导出的应用都跟随这里的选择"), appearance);
    const FluentQt::Theme current = FluentQt::FluentConfig::instance()->themeMode();
    theme->comboBox()->setCurrentIndex(current == FluentQt::Theme::Dark ? 1 : current == FluentQt::Theme::Auto ? 2 : 0);
    connect(theme->comboBox(), &FluentQt::ComboBox::currentIndexChanged, this, [](int index) {
        const FluentQt::Theme mode = index == 1   ? FluentQt::Theme::Dark
                                     : index == 2 ? FluentQt::Theme::Auto
                                                  : FluentQt::Theme::Light;
        FluentQt::FluentConfig::instance()->setThemeMode(mode);
        FluentQt::FluentConfig::instance()->save();
        FluentQt::ThemeManager::instance()->setTheme(mode);
    });
    appearance->addSettingCard(theme);
    layout->addWidget(appearance);

    auto *about = new FluentQt::SettingCardGroup(tr("关于"), content);
    auto *repo = new FluentQt::PushSettingCard(tr("打开"), FluentQt::FluentIcon::GitHub, tr("项目主页"),
                                               QStringLiteral("github.com/txp666/FluentHMI-Studio"), about);
    connect(repo, &FluentQt::PushSettingCard::clicked, this, []() {
        QDesktopServices::openUrl(QUrl(QStringLiteral("https://github.com/txp666/FluentHMI-Studio")));
    });
    m_checkUpdate = new FluentQt::PrimaryPushSettingCard(tr("检查更新"), FluentQt::FluentIcon::Info, tr("版本"),
                                                         tr("当前版本 v%1").arg(QStringLiteral(FHMI_VERSION)), about);
    connect(m_checkUpdate, &FluentQt::PushSettingCard::clicked, this, &SettingsPage::checkUpdateRequested);
    auto *checkAtStartup = new FluentQt::SwitchSettingCard(FluentQt::FluentIcon::Update, tr("启动时检查更新"),
                                                           tr("启动后在后台检查 GitHub 上是否有新版本"), about);
    checkAtStartup->setChecked(UpdateController::checkAtStartup());
    connect(checkAtStartup, &FluentQt::SwitchSettingCard::checkedChanged, this, &UpdateController::setCheckAtStartup);
    about->addSettingCard(m_checkUpdate);
    about->addSettingCard(checkAtStartup);
    about->addSettingCard(repo);
    layout->addWidget(about);
    layout->addStretch();

    scroll->setWidget(content);
    scroll->enableTransparentBackground();
    outer->addWidget(scroll);
}

void SettingsPage::setUpdateChecking(bool checking)
{
    m_checkUpdate->button()->setEnabled(!checking);
    m_checkUpdate->button()->setText(checking ? tr("检查中…") : tr("检查更新"));
}

} // namespace fhmi
