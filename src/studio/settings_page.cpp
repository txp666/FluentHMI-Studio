#include "studio/settings_page.h"

#include "core/i18n.h"
#include "runtime/ui_helpers.h"
#include "studio/ai_service.h"
#include "studio/update_controller.h"

#include <FluentQtWidgets/Config.h>
#include <FluentQtWidgets/ConfigItem.h>
#include <FluentQtWidgets/FluentIcon.h>
#include <FluentQtWidgets/Settings/SettingCard.h>
#include <FluentQtWidgets/Theme.h>
#include <FluentQtWidgets/Widgets/Button.h>
#include <FluentQtWidgets/Widgets/ComboBox.h>
#include <FluentQtWidgets/Widgets/InfoBar.h>
#include <FluentQtWidgets/Widgets/Label.h>
#include <FluentQtWidgets/Widgets/LineEdit.h>
#include <FluentQtWidgets/Widgets/ScrollArea.h>
#include <FluentQtWidgets/Window/FluentWindow.h>

#include <QtCore/QSignalBlocker>
#include <QtCore/QUrl>
#include <QtGui/QDesktopServices>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QVBoxLayout>

namespace fhmi {

namespace {

FluentQt::PushSettingCard *createLinkCard(const QString &title, const QString &description, const QString &url,
                                        const QString &objectName, FluentQt::FluentIcon icon, QWidget *parent)
{
    auto *card = new FluentQt::PushSettingCard(SettingsPage::tr("打开"), icon, title, description, parent);
    card->setObjectName(objectName + QStringLiteral("Card"));
    card->setProperty("targetUrl", url);
    card->button()->setObjectName(objectName + QStringLiteral("Button"));
    card->button()->setProperty("targetUrl", url);
    fluentToolTip(card->button(), url);
    QObject::connect(card, &FluentQt::PushSettingCard::clicked, card,
                     [url]() { QDesktopServices::openUrl(QUrl(url)); });
    return card;
}

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

    m_aiTestService = new AiService(this);
    auto *aiGroup = new FluentQt::SettingCardGroup(tr("AI 助手"), content);
    aiGroup->setObjectName(QStringLiteral("aiSettingGroup"));
    const AiConfig initialAi = AiConfig::load();

    auto *providerCard = new FluentQt::SettingCard(
        FluentQt::FluentIcon::Robot, tr("服务提供商"),
        tr("选择云端 OpenAI 兼容服务或本地离线 Ollama 模型"), aiGroup);
    providerCard->setObjectName(QStringLiteral("aiProviderCard"));
    auto *providerCombo = new FluentQt::ComboBox(providerCard);
    providerCombo->setObjectName(QStringLiteral("aiProviderCombo"));
    providerCombo->setMinimumWidth(220);
    for (const QString &id : AiConfig::providerIds()) {
        providerCombo->addItem(AiConfig::providerDisplayName(id), {}, id);
    }
    const int initialProviderIdx = providerCombo->findData(initialAi.provider);
    providerCombo->setCurrentIndex(initialProviderIdx >= 0 ? initialProviderIdx : 0);
    providerCard->actionLayout()->addWidget(providerCombo);
    aiGroup->addSettingCard(providerCard);

    auto *baseUrlCard = new FluentQt::SettingCard(
        FluentQt::FluentIcon::Cloud, tr("接口地址 (Base URL)"),
        tr("OpenAI 兼容接口地址，例如 https://api.deepseek.com/v1 或 http://127.0.0.1:11434/v1"), aiGroup);
    baseUrlCard->setObjectName(QStringLiteral("aiBaseUrlCard"));
    auto *baseUrlEdit = new FluentQt::LineEdit(initialAi.baseUrl, baseUrlCard);
    baseUrlEdit->setObjectName(QStringLiteral("aiBaseUrlEdit"));
    baseUrlEdit->setMinimumWidth(300);
    baseUrlEdit->setClearButtonEnabled(true);
    baseUrlEdit->setPlaceholderText(AiConfig::presetBaseUrl(initialAi.provider));
    baseUrlCard->actionLayout()->addWidget(baseUrlEdit);
    aiGroup->addSettingCard(baseUrlCard);

    auto *apiKeyCard = new FluentQt::SettingCard(
        FluentQt::FluentIcon::Certificate, tr("API 密钥"),
        tr("仅保存在本机配置中，不会写入 .uidp 工程文件（本地 Ollama 可留空）"), aiGroup);
    apiKeyCard->setObjectName(QStringLiteral("aiApiKeyCard"));
    auto *apiKeyEdit = new FluentQt::PasswordLineEdit(apiKeyCard);
    apiKeyEdit->setObjectName(QStringLiteral("aiApiKeyEdit"));
    apiKeyEdit->setMinimumWidth(300);
    apiKeyEdit->setClearButtonEnabled(true);
    apiKeyEdit->setText(initialAi.apiKey);
    apiKeyEdit->setPlaceholderText(!AiConfig::requiresApiKey(initialAi.provider) ? tr("本地模型服务无需密钥") : QStringLiteral("sk-..."));
    apiKeyCard->actionLayout()->addWidget(apiKeyEdit);
    aiGroup->addSettingCard(apiKeyCard);

    auto *modelCard = new FluentQt::SettingCard(
        FluentQt::FluentIcon::Code, tr("模型名称"),
        tr("可从下拉列表选择常用模型，也可直接输入自定义模型标识"), aiGroup);
    modelCard->setObjectName(QStringLiteral("aiModelCard"));
    auto *modelCombo = new FluentQt::EditableComboBox(modelCard);
    modelCombo->setObjectName(QStringLiteral("aiModelEdit"));
    modelCombo->setMinimumWidth(240);
    modelCombo->setMaxVisibleItems(10);
    modelCombo->addItems(AiConfig::presetModels(initialAi.provider));
    modelCombo->setCurrentText(initialAi.model);
    modelCombo->setPlaceholderText(AiConfig::presetModel(initialAi.provider));
    auto *refreshModelsBtn = new FluentQt::TransparentToolButton(FluentQt::icon(FluentQt::FluentIcon::Sync), modelCard);
    refreshModelsBtn->setObjectName(QStringLiteral("aiRefreshModelsButton"));
    refreshModelsBtn->setFixedSize(32, 32);
    fluentToolTip(refreshModelsBtn, tr("从接口在线拉取可用模型列表"));
    auto *testBtn = new FluentQt::PushButton(tr("测试连接"), modelCard);
    testBtn->setObjectName(QStringLiteral("aiTestConnectionButton"));
    modelCard->actionLayout()->addWidget(modelCombo);
    modelCard->actionLayout()->addSpacing(4);
    modelCard->actionLayout()->addWidget(refreshModelsBtn);
    modelCard->actionLayout()->addSpacing(6);
    modelCard->actionLayout()->addWidget(testBtn);
    aiGroup->addSettingCard(modelCard);

    const auto currentAiConfig = [providerCombo, baseUrlEdit, apiKeyEdit, modelCombo]() {
        AiConfig cfg;
        cfg.provider = providerCombo->currentData().toString();
        cfg.baseUrl = baseUrlEdit->text().trimmed();
        if (cfg.baseUrl.isEmpty()) {
            cfg.baseUrl = AiConfig::presetBaseUrl(cfg.provider);
        }
        cfg.apiKey = apiKeyEdit->text().trimmed();
        cfg.model = modelCombo->currentText().trimmed();
        if (cfg.model.isEmpty()) {
            cfg.model = AiConfig::presetModel(cfg.provider);
        }
        return cfg;
    };

    const auto persistAiConfig = [currentAiConfig]() { AiConfig::save(currentAiConfig()); };

    connect(providerCombo, &FluentQt::ComboBox::currentIndexChanged, this,
            [providerCombo, baseUrlEdit, apiKeyEdit, modelCombo, persistAiConfig]() {
                const QString provider = providerCombo->currentData().toString();
                const QSignalBlocker b1(baseUrlEdit);
                const QSignalBlocker b2(apiKeyEdit);
                const QSignalBlocker b3(modelCombo);
                baseUrlEdit->setText(AiConfig::presetBaseUrl(provider));
                baseUrlEdit->setPlaceholderText(AiConfig::presetBaseUrl(provider));
                modelCombo->clear();
                modelCombo->addItems(AiConfig::presetModels(provider));
                modelCombo->setCurrentText(AiConfig::presetModel(provider));
                modelCombo->setPlaceholderText(AiConfig::presetModel(provider));
                apiKeyEdit->setText(AiConfig::loadProviderApiKey(provider));
                apiKeyEdit->setPlaceholderText(!AiConfig::requiresApiKey(provider)
                                                   ? SettingsPage::tr("本地模型服务无需密钥")
                                                   : QStringLiteral("sk-..."));
                persistAiConfig();
            });
    connect(baseUrlEdit, &QLineEdit::editingFinished, this, persistAiConfig);
    connect(apiKeyEdit, &QLineEdit::textChanged, this, persistAiConfig);
    connect(modelCombo, &FluentQt::EditableComboBox::currentTextChanged, this, persistAiConfig);
    connect(modelCombo, &QLineEdit::editingFinished, this, persistAiConfig);

    connect(testBtn, &QPushButton::clicked, this, [this, testBtn, currentAiConfig]() {
        const AiConfig cfg = currentAiConfig();
        AiConfig::save(cfg);
        testBtn->setEnabled(false);
        testBtn->setText(tr("测试中…"));
        m_aiTestService->testConnection(cfg);
    });
    connect(refreshModelsBtn, &QToolButton::clicked, this, [this, refreshModelsBtn, currentAiConfig]() {
        const AiConfig cfg = currentAiConfig();
        AiConfig::save(cfg);
        refreshModelsBtn->setEnabled(false);
        m_aiTestService->fetchModels(cfg);
    });
    connect(m_aiTestService, &AiService::modelsFetched, this,
            [this, refreshModelsBtn, modelCombo, persistAiConfig](bool ok, const QStringList &models, const QString &message) {
                refreshModelsBtn->setEnabled(true);
                if (ok && !models.isEmpty()) {
                    const QString current = modelCombo->currentText().trimmed();
                    const QSignalBlocker blocker(modelCombo);
                    modelCombo->clear();
                    modelCombo->addItems(models);
                    modelCombo->setCurrentText(models.contains(current) ? current : models.first());
                    persistAiConfig();
                    FluentQt::InfoBar::success(tr("模型列表已更新"), message, Qt::Horizontal, true, 3000,
                                               FluentQt::InfoBarPosition::Top, window());
                } else {
                    FluentQt::InfoBar::error(tr("获取模型列表失败"), message, Qt::Horizontal, true, 4000,
                                             FluentQt::InfoBarPosition::Top, window());
                }
            });
    connect(m_aiTestService, &AiService::testFinished, this, [this, testBtn](bool ok, const QString &message) {
        testBtn->setEnabled(true);
        testBtn->setText(tr("测试连接"));
        if (ok) {
            FluentQt::InfoBar::success(tr("AI 连接成功"), message, Qt::Horizontal, true, 3000,
                                       FluentQt::InfoBarPosition::Top, window());
        } else {
            FluentQt::InfoBar::error(tr("AI 连接失败"), message, Qt::Horizontal, true, 5000,
                                     FluentQt::InfoBarPosition::Top, window());
        }
    });

    layout->addWidget(aiGroup);

    auto *about = new FluentQt::SettingCardGroup(tr("关于"), content);
    about->setObjectName(QStringLiteral("aboutGroup"));
    auto *projectInfo = new FluentQt::SettingCard(
        FluentQt::FluentIcon::Application, QStringLiteral("FluentHMI Studio"),
        tr("原生 C++/Qt 6 上位机设计器，支持实时预览和独立应用导出。"), about);
    projectInfo->setObjectName(QStringLiteral("projectInfoCard"));
    auto *license = new FluentQt::SettingCard(FluentQt::FluentIcon::Document, tr("开源许可"),
                                               QStringLiteral("GPL-3.0"), about);
    license->setObjectName(QStringLiteral("projectLicenseCard"));
    m_checkUpdate = new FluentQt::PrimaryPushSettingCard(tr("检查更新"), FluentQt::FluentIcon::Info, tr("版本"),
                                                         tr("当前版本 v%1").arg(QStringLiteral(FHMI_VERSION)), about);
    m_checkUpdate->setObjectName(QStringLiteral("checkUpdateCard"));
    m_checkUpdate->button()->setObjectName(QStringLiteral("checkUpdateButton"));
    connect(m_checkUpdate, &FluentQt::PushSettingCard::clicked, this, &SettingsPage::checkUpdateRequested);
    auto *checkAtStartup = new FluentQt::SwitchSettingCard(FluentQt::FluentIcon::Update, tr("启动时检查更新"),
                                                           tr("启动后在后台检查 GitHub 上是否有新版本"), about);
    checkAtStartup->setChecked(UpdateController::checkAtStartup());
    connect(checkAtStartup, &FluentQt::SwitchSettingCard::checkedChanged, this, &UpdateController::setCheckAtStartup);
    about->addSettingCard(projectInfo);
    about->addSettingCard(license);
    about->addSettingCard(m_checkUpdate);
    about->addSettingCard(checkAtStartup);
    layout->addWidget(about);

    auto *repository = new FluentQt::SettingCardGroup(tr("代码仓库"), content);
    repository->setObjectName(QStringLiteral("repositoryGroup"));
    // Fixed HTTPS destinations are opened only after a user clicks a card's
    // button. Repository information never triggers a request during startup.
    repository->addSettingCard(createLinkCard(
        tr("GitHub 仓库"), QStringLiteral("https://github.com/txp666/FluentHMI-Studio"),
        QStringLiteral("https://github.com/txp666/FluentHMI-Studio"), QStringLiteral("repository"),
        FluentQt::FluentIcon::GitHub, repository));
    repository->addSettingCard(createLinkCard(
        tr("问题反馈"), tr("报告问题或提交功能建议"),
        QStringLiteral("https://github.com/txp666/FluentHMI-Studio/issues"), QStringLiteral("issues"),
        FluentQt::FluentIcon::Question, repository));
    repository->addSettingCard(createLinkCard(
        tr("发布版本"), tr("查看发布说明和下载安装包"),
        QStringLiteral("https://github.com/txp666/FluentHMI-Studio/releases"), QStringLiteral("releases"),
        FluentQt::FluentIcon::Download, repository));
    layout->addWidget(repository);

    auto *related = new FluentQt::SettingCardGroup(tr("相关工具"), content);
    related->setObjectName(QStringLiteral("relatedToolsGroup"));
    related->addSettingCard(createLinkCard(
        QStringLiteral("PyQt UI Designer"), tr("Python/PyQt5 设计器，支持源码导出和 Nuitka 打包。"),
        QStringLiteral("https://github.com/txp666/PyQt-UI-Designer"), QStringLiteral("pyqtDesignerRepository"),
        FluentQt::FluentIcon::Code, related));
    related->addSettingCard(createLinkCard(
        QStringLiteral("modusignal"), tr("浏览器设备调试与 .uidp 自定义设备编辑。"),
        QStringLiteral("https://github.com/txp666/modusignal"), QStringLiteral("modusignalRepository"),
        FluentQt::FluentIcon::Globe, related));
    related->addSettingCard(createLinkCard(
        QStringLiteral("FluentPLC Simulator"), tr("桌面与命令行经典 S7comm 测试服务端。"),
        QStringLiteral("https://github.com/txp666/FluentPLC-Simulator"), QStringLiteral("plcSimulatorRepository"),
        FluentQt::FluentIcon::Application, related));
    layout->addWidget(related);
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
