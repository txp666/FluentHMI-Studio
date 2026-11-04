#include "studio/settings_page.h"

#include "core/i18n.h"
#include "studio/update_controller.h"

#include <FluentQtWidgets/Config.h>
#include <FluentQtWidgets/ConfigItem.h>
#include <FluentQtWidgets/FluentIcon.h>
#include <FluentQtWidgets/Settings/SettingCard.h>
#include <FluentQtWidgets/Theme.h>
#include <FluentQtWidgets/Widgets/InfoBar.h>
#include <FluentQtWidgets/Widgets/Label.h>
#include <FluentQtWidgets/Widgets/ScrollArea.h>
#include <FluentQtWidgets/Window/FluentWindow.h>

#include <QtCore/QUrl>
#include <QtGui/QDesktopServices>
#include <QtWidgets/QVBoxLayout>

namespace fhmi {

namespace {

#ifdef Q_OS_WIN
// Mica needs Windows 11, so on Windows 10 the switch stays off and disabled.
FluentQt::SwitchSettingCard *createMicaCard(QWidget *parent)
{
    const bool available = FluentQt::isMicaEffectAvailable();
    auto *card = new FluentQt::SwitchSettingCard(
        FluentQt::FluentIcon::Transparent, SettingsPage::tr("云母效果"),
        available ? SettingsPage::tr("窗口和表面显示半透明") : SettingsPage::tr("需要 Windows 11"), parent);
    card->setObjectName(QStringLiteral("micaEffectCard"));
    card->setEnabled(available);
    if (!available) {
        return card;
    }

    // The main window follows the setting.
    auto *config = FluentQt::FluentConfig::instance();
    card->setChecked(config->isMicaEnabled());
    QObject::connect(card, &FluentQt::SwitchSettingCard::checkedChanged, card, [config](bool checked) {
        config->setMicaEnabled(checked);
        config->save();
    });
    QObject::connect(config, &FluentQt::FluentConfig::micaEnabledChanged, card,
                     &FluentQt::SwitchSettingCard::setChecked);
    return card;
}
#endif

} // namespace

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
    // FluentConfig keeps the theme; this item only feeds the card, in the order of its texts.
    auto *themeMode = new FluentQt::OptionsConfigItem(
        QStringLiteral("Studio"), QStringLiteral("ThemeMode"),
        static_cast<int>(FluentQt::FluentConfig::instance()->themeMode()),
        QVariantList{static_cast<int>(FluentQt::Theme::Light), static_cast<int>(FluentQt::Theme::Dark),
                     static_cast<int>(FluentQt::Theme::Auto)},
        this);
    auto *theme = new FluentQt::ComboBoxSettingCard(themeMode, FluentQt::FluentIcon::Brush, tr("主题"),
                                                    tr("设计画布、预览和导出的应用都跟随这里的选择"),
                                                    {tr("浅色"), tr("深色"), tr("跟随系统")}, appearance);
    connect(themeMode, &FluentQt::ConfigItem::valueChanged, this, [](const QVariant &value) {
        const auto mode = static_cast<FluentQt::Theme>(value.toInt());
        FluentQt::FluentConfig::instance()->setThemeMode(mode);
        FluentQt::FluentConfig::instance()->save();
        FluentQt::ThemeManager::instance()->setTheme(mode);
    });
    appearance->addSettingCard(theme);

    auto *themeColor = new FluentQt::CustomColorSettingCard(
        QColor(QStringLiteral("#009faa")), FluentQt::FluentConfig::instance()->themeColor(),
        FluentQt::FluentIcon::Palette, tr("主题色"), tr("选择 Fluent 控件的强调色"), appearance);
    connect(themeColor, &FluentQt::CustomColorSettingCard::colorChanged, this, [](const QColor &color) {
        FluentQt::FluentConfig::instance()->setThemeColor(color);
        FluentQt::FluentConfig::instance()->save();
        FluentQt::ThemeManager::instance()->setAccentColor(color);
    });
    connect(FluentQt::ThemeManager::instance(), &FluentQt::ThemeManager::accentColorChanged, this,
            [themeColor](const QColor &color) {
                if (themeColor->color() != color) {
                    themeColor->setColor(color);
                }
            });
    appearance->addSettingCard(themeColor);

    // FluentConfig keeps the language too (localeName); it takes effect at the next start.
    const QVariantList languages{kLanguageAuto, kLanguageChinese, kLanguageEnglish};
    const QString savedLanguage = FluentQt::FluentConfig::instance()->localeName();
    auto *languageItem = new FluentQt::OptionsConfigItem(
        QStringLiteral("Studio"), QStringLiteral("Language"),
        languages.contains(savedLanguage) ? savedLanguage : kLanguageAuto, languages, this);
    // Each language's own name reads the same in every language.
    const QStringList languageTexts{tr("跟随系统"),
                                    QStringLiteral("简体中文"), // i18n: ignore
                                    QStringLiteral("English")};
    auto *language =
        new FluentQt::ComboBoxSettingCard(languageItem, FluentQt::FluentIcon::Language, tr("语言"),
                                          tr("界面和导出应用的语言，重启后生效"), languageTexts, appearance);
    language->setObjectName(QStringLiteral("languageCard"));
    connect(languageItem, &FluentQt::ConfigItem::valueChanged, this, [this](const QVariant &value) {
        FluentQt::FluentConfig::instance()->setLocaleName(value.toString());
        FluentQt::FluentConfig::instance()->save();
        FluentQt::InfoBar::success(tr("语言已更改"), tr("重启软件后生效"), Qt::Horizontal, true, 3000,
                                   FluentQt::InfoBarPosition::Top, window());
    });
    appearance->addSettingCard(language);
#ifdef Q_OS_WIN
    appearance->addSettingCard(createMicaCard(appearance));
#endif
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
