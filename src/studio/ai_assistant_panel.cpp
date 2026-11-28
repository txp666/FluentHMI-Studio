#include "studio/ai_assistant_panel.h"

#include "core/i18n.h"
#include "core/widget_meta.h"
#include "designer/canvas_item.h"
#include "runtime/ui_helpers.h"

#include <FluentQtWidgets/FluentIcon.h>
#include <FluentQtWidgets/Theme.h>
#include <FluentQtWidgets/Widgets/Button.h>
#include <FluentQtWidgets/Widgets/CardWidget.h>
#include <FluentQtWidgets/Widgets/Label.h>
#include <FluentQtWidgets/Widgets/LineEdit.h>
#include <FluentQtWidgets/Widgets/ScrollArea.h>

#include <QtCore/QEvent>
#include <QtCore/QFileInfo>
#include <QtCore/QMimeData>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtGui/QClipboard>
#include <QtGui/QDragEnterEvent>
#include <QtGui/QDropEvent>
#include <QtGui/QGuiApplication>
#include <QtGui/QImage>
#include <QtGui/QKeyEvent>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QFrame>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QScrollBar>
#include <QtWidgets/QVBoxLayout>

namespace fhmi {

AiAssistantPanel::AiAssistantPanel(QWidget *parent) : QWidget(parent), m_service(new AiService(this))
{
    setObjectName(QStringLiteral("aiAssistantPanel"));
    setAcceptDrops(true);
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(10, 8, 10, 8);
    root->setSpacing(6);

    // Top context bar + clear button
    auto *topRow = new QHBoxLayout;
    topRow->setContentsMargins(0, 0, 0, 0);
    topRow->setSpacing(6);
    m_contextLabel = new FluentQt::CaptionLabel(tr("上下文：全工程（未选中控件）"), this);
    m_contextLabel->setObjectName(QStringLiteral("aiContextLabel"));
    m_contextLabel->setWordWrap(true);
    m_contextLabel->setMinimumWidth(0);
    m_contextLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    auto *clearBtn = new FluentQt::TransparentToolButton(FluentQt::icon(FluentQt::FluentIcon::Broom), this);
    clearBtn->setObjectName(QStringLiteral("aiClearChatButton"));
    clearBtn->setFixedSize(28, 28);
    fluentToolTip(clearBtn, tr("清空对话记录"));
    connect(clearBtn, &QToolButton::clicked, this, &AiAssistantPanel::clearConversation);
    topRow->addWidget(m_contextLabel, 1);
    topRow->addWidget(clearBtn);
    root->addLayout(topRow);

    // Quick action chips
    auto *quickRow = new QHBoxLayout;
    quickRow->setContentsMargins(0, 0, 0, 0);
    quickRow->setSpacing(4);
    const auto makeChip = [this, quickRow](const QString &label, const QString &tooltip, const std::function<void()> &onClick) {
        auto *btn = new FluentQt::PushButton(label, this);
        btn->setFixedHeight(26);
        fluentToolTip(btn, tooltip);
        connect(btn, &QPushButton::clicked, this, onClick);
        quickRow->addWidget(btn);
    };
    makeChip(tr("生成界面"), tr("根据自然语言描述在画布上生成控件与布局"), [this]() {
        m_input->setPlainText(tr("帮我设计一个监控面板：包含电流仪表盘（0~2000A）、电压数值显示、运行状态指示灯和实时曲线，并合理排布在画布上。"));
        m_input->setFocus();
    });
    makeChip(tr("解析协议"), tr("根据 HEX 报文、寄存器表或 CAN 信号描述生成接收模板与绑定"), [this]() {
        m_input->setPlainText(tr("请帮我解析以下通讯协议并配置接收帧模板与控件绑定：\n帧格式：帧头 AA 55，字节2-3为电压(uint16大端，0.1V)，字节4-5为电流(int16大端，0.1A)，帧尾 0D 0A。"));
        m_input->setFocus();
    });
    makeChip(tr("配置绑定"), tr("为当前选中的控件配置数据源绑定、量程或换算公式"), [this]() {
        if (!m_selectedIds.isEmpty()) {
            m_input->setPlainText(tr("针对当前选中的控件 (%1)，帮我检查它的量程与属性设置，并配置合适的数据绑定：")
                                      .arg(m_selectedIds.join(QStringLiteral(", "))));
        } else {
            m_input->setPlainText(tr("帮我将画布上的显示控件绑定到对应的连接卡片数据字段，并添加工程数据点与越限报警规则。"));
        }
        m_input->setFocus();
    });
    makeChip(tr("诊断工程"), tr("立即检查当前工程中的控件属性、绑定路由、协议模板与报警配置"), [this]() {
        if (!m_service->isBusy()) {
            sendPrompt(tr("请全面检查当前工程的控件属性、量程设置、数据绑定路由、接收帧模板以及工程数据点/报警配置，指出潜在问题或优化建议。"));
        }
    });
    root->addLayout(quickRow);

    // Conversation scroll area
    m_scroll = new FluentQt::ScrollArea(this);
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_messagesContainer = new QWidget(m_scroll);
    m_messagesContainer->setAttribute(Qt::WA_TranslucentBackground);
    m_messagesLayout = new QVBoxLayout(m_messagesContainer);
    m_messagesLayout->setContentsMargins(0, 4, 4, 4);
    m_messagesLayout->setSpacing(8);

    m_welcomeCard = new FluentQt::CardWidget(m_messagesContainer);
    m_welcomeCard->setObjectName(QStringLiteral("aiWelcomeCard"));
    auto *welcomeLayout = new QVBoxLayout(m_welcomeCard);
    welcomeLayout->setContentsMargins(12, 10, 12, 10);
    welcomeLayout->setSpacing(6);
    auto *welcomeTitle = new FluentQt::StrongBodyLabel(tr("FluentHMI AI 助手"), m_welcomeCard);
    welcomeTitle->setWordWrap(true);
    auto *welcomeBody = new FluentQt::CaptionLabel(
        tr("我已连接当前画布与工程上下文，你可以随时问我：\n• 界面生成：用自然语言批量创建、排布或修改仪表盘、曲线、工业控件\n• 协议解析：粘贴 HEX 帧、Modbus 寄存器表、S7 地址或 CAN 信号说明，自动生成模板与绑定\n• 联调排障：选中任意控件询问量程、居中原因、表达式换算或通讯排障\n• 安全可控：所有工程修改均会先生成变更预览卡片，点击“应用到工程”才生效，并支持 Ctrl+Z 撤销"),
        m_welcomeCard);
    welcomeBody->setWordWrap(true);
    welcomeLayout->addWidget(welcomeTitle);
    welcomeLayout->addWidget(welcomeBody);
    m_messagesLayout->addWidget(m_welcomeCard);
    m_messagesLayout->addStretch(1);

    m_scroll->setWidget(m_messagesContainer);
    m_scroll->enableTransparentBackground();
    root->addWidget(m_scroll, 1);

    // Pending attachments strip
    m_attachmentBar = new QWidget(this);
    m_attachmentBar->setObjectName(QStringLiteral("aiAttachmentBar"));
    m_attachmentLayout = new QVBoxLayout(m_attachmentBar);
    m_attachmentLayout->setContentsMargins(0, 2, 0, 2);
    m_attachmentLayout->setSpacing(4);
    m_attachmentBar->hide();
    root->addWidget(m_attachmentBar);

    // Input editor
    m_input = new FluentQt::PlainTextEdit(this);
    m_input->setObjectName(QStringLiteral("aiPromptInput"));
    m_input->setPlaceholderText(tr("输入问题、粘贴截图或拖入图片/PDF协议文档… (Ctrl+Enter 发送)"));
    m_input->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_input->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    m_input->setFixedHeight(78);
    m_input->installEventFilter(this);
    root->addWidget(m_input);

    // Bottom bar: attach button + model indicator + stop/send buttons
    auto *bottomRow = new QHBoxLayout;
    bottomRow->setContentsMargins(0, 0, 0, 0);
    bottomRow->setSpacing(6);
    auto *attachBtn = new FluentQt::TransparentToolButton(FluentQt::icon(FluentQt::FluentIcon::FolderAdd), this);
    attachBtn->setObjectName(QStringLiteral("aiAttachFileButton"));
    attachBtn->setFixedSize(28, 28);
    fluentToolTip(attachBtn, tr("添加图片、PDF 或协议文档附件（也支持 Ctrl+V 粘贴截图或拖拽文件）"));
    connect(attachBtn, &QToolButton::clicked, this, &AiAssistantPanel::chooseAttachments);

    m_modelLabel = new FluentQt::CaptionLabel(this);
    m_modelLabel->setObjectName(QStringLiteral("aiModelStatusLabel"));
    m_modelLabel->setMinimumWidth(0);
    m_modelLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_stopButton = new FluentQt::PushButton(FluentQt::icon(FluentQt::FluentIcon::Cancel), tr("停止"), this);
    m_stopButton->setObjectName(QStringLiteral("aiStopButton"));
    m_stopButton->hide();
    m_sendButton = new FluentQt::PrimaryPushButton(FluentQt::icon(FluentQt::FluentIcon::Send), tr("发送"), this);
    m_sendButton->setObjectName(QStringLiteral("aiSendButton"));

    connect(m_stopButton, &QPushButton::clicked, this, [this]() {
        m_service->abort();
        updateBusyState(false);
        if (m_activeTextLabel && m_streamingText.isEmpty()) {
            m_activeTextLabel->setText(tr("已取消生成"));
        }
        m_activeBubble = nullptr;
        m_activeTextLabel = nullptr;
        m_activeBubbleLayout = nullptr;
    });
    connect(m_sendButton, &QPushButton::clicked, this, [this]() {
        sendPrompt(m_input->toPlainText());
    });

    bottomRow->addWidget(attachBtn);
    bottomRow->addWidget(m_modelLabel, 1);
    bottomRow->addWidget(m_stopButton);
    bottomRow->addWidget(m_sendButton);
    root->addLayout(bottomRow);

    connect(m_service, &AiService::chunkReceived, this, [this](const QString &delta) {
        m_streamingText.append(delta);
        if (m_activeTextLabel) {
            m_activeTextLabel->setText(m_streamingText);
            scrollToBottom();
        }
    });
    connect(m_service, &AiService::finished, this, [this](const QString &fullText) {
        updateBusyState(false);
        finalizeAssistantBubble(fullText);
    });
    connect(m_service, &AiService::failed, this, [this](const QString &error) {
        updateBusyState(false);
        failAssistantBubble(error);
    });

    refreshModelIndicator();
}

void AiAssistantPanel::setProjectProvider(std::function<Project()> provider)
{
    m_projectProvider = std::move(provider);
}

void AiAssistantPanel::setSelectedItems(const QList<CanvasItem *> &items)
{
    m_selectedIds.clear();
    m_selectedSummaries.clear();
    for (const CanvasItem *item : items) {
        if (!item) {
            continue;
        }
        m_selectedIds.append(item->widgetId());
        m_selectedSummaries.append(QStringLiteral("%1 (%2)").arg(translated(item->meta().displayName), item->widgetId()));
    }
    if (m_selectedSummaries.isEmpty()) {
        m_contextLabel->setText(tr("上下文：全工程（未选中控件）"));
    } else if (m_selectedSummaries.size() == 1) {
        m_contextLabel->setText(tr("当前选中：%1").arg(m_selectedSummaries.first()));
    } else {
        m_contextLabel->setText(tr("当前选中：%1 个控件 (%2)").arg(m_selectedSummaries.size()).arg(m_selectedIds.join(QStringLiteral(", "))));
    }
}

void AiAssistantPanel::refreshModelIndicator()
{
    const AiConfig cfg = AiConfig::load();
    const QString model = cfg.model.isEmpty() ? AiConfig::presetModel(cfg.provider) : cfg.model;
    m_modelLabel->setText(QStringLiteral("%1 · %2").arg(AiConfig::providerDisplayName(cfg.provider), model));
}

void AiAssistantPanel::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    refreshModelIndicator();
}

void AiAssistantPanel::dragEnterEvent(QDragEnterEvent *event)
{
    if (event->mimeData() && (event->mimeData()->hasUrls() || event->mimeData()->hasImage())) {
        event->acceptProposedAction();
        return;
    }
    QWidget::dragEnterEvent(event);
}

void AiAssistantPanel::dropEvent(QDropEvent *event)
{
    const QMimeData *mime = event->mimeData();
    if (!mime) {
        return;
    }
    bool added = false;
    if (mime->hasUrls()) {
        for (const QUrl &url : mime->urls()) {
            if (url.isLocalFile()) {
                added |= addAttachmentFile(url.toLocalFile());
            }
        }
    } else if (mime->hasImage()) {
        const QImage img = qvariant_cast<QImage>(mime->imageData());
        if (!img.isNull()) {
            m_pendingAttachments.append(AiAttachment::fromClipboardImage(img, ++m_clipboardImageCounter));
            rebuildAttachmentBar();
            added = true;
        }
    }
    if (added) {
        event->acceptProposedAction();
    }
}

bool AiAssistantPanel::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_input && event->type() == QEvent::KeyPress) {
        auto *keyEvent = static_cast<QKeyEvent *>(event);
        if ((keyEvent->key() == Qt::Key_Return || keyEvent->key() == Qt::Key_Enter) &&
            (keyEvent->modifiers() & (Qt::ControlModifier | Qt::MetaModifier))) {
            if (!m_service->isBusy()) {
                sendPrompt(m_input->toPlainText());
            }
            return true;
        }
        if (keyEvent->matches(QKeySequence::Paste)) {
            if (const QMimeData *mime = QGuiApplication::clipboard()->mimeData()) {
                if (mime->hasImage()) {
                    const QImage img = qvariant_cast<QImage>(mime->imageData());
                    if (!img.isNull()) {
                        m_pendingAttachments.append(AiAttachment::fromClipboardImage(img, ++m_clipboardImageCounter));
                        rebuildAttachmentBar();
                        return true;
                    }
                }
                if (mime->hasUrls()) {
                    bool added = false;
                    for (const QUrl &url : mime->urls()) {
                        if (url.isLocalFile()) {
                            added |= addAttachmentFile(url.toLocalFile());
                        }
                    }
                    if (added) {
                        return true;
                    }
                }
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

void AiAssistantPanel::chooseAttachments()
{
    const QString filter = tr("图片与文档 (*.png *.jpg *.jpeg *.webp *.bmp *.gif *.pdf *.dbc *.txt *.md *.csv *.json);;所有文件 (*)");
    const QStringList files = QFileDialog::getOpenFileNames(this, tr("选择图片或协议文档"), QString(), filter);
    for (const QString &file : files) {
        addAttachmentFile(file);
    }
}

bool AiAssistantPanel::addAttachmentFile(const QString &filePath)
{
    AiAttachment att;
    QString error;
    if (!AiAttachment::fromFile(filePath, &att, &error)) {
        emit notify(tr("无法添加附件"), error, true);
        return false;
    }
    m_pendingAttachments.append(std::move(att));
    rebuildAttachmentBar();
    return true;
}

void AiAssistantPanel::rebuildAttachmentBar()
{
    while (QLayoutItem *item = m_attachmentLayout->takeAt(0)) {
        if (QWidget *w = item->widget()) {
            w->deleteLater();
        }
        delete item;
    }
    if (m_pendingAttachments.isEmpty()) {
        m_attachmentBar->hide();
        return;
    }
    for (int i = 0; i < m_pendingAttachments.size(); ++i) {
        const AiAttachment &att = m_pendingAttachments.at(i);
        auto *rowWidget = new QWidget(m_attachmentBar);
        auto *rowLayout = new QHBoxLayout(rowWidget);
        rowLayout->setContentsMargins(6, 2, 4, 2);
        rowLayout->setSpacing(6);
        const QString kindTag = att.kind == AiAttachment::Kind::Image ? tr("[图片]")
                                : att.kind == AiAttachment::Kind::Pdf ? QStringLiteral("[PDF]")
                                                                      : tr("[文档]");
        auto *label = new FluentQt::CaptionLabel(QStringLiteral("%1 %2").arg(kindTag, att.fileName), rowWidget);
        label->setWordWrap(true);
        auto *removeBtn = new FluentQt::TransparentToolButton(FluentQt::icon(FluentQt::FluentIcon::Close), rowWidget);
        removeBtn->setFixedSize(20, 20);
        connect(removeBtn, &QToolButton::clicked, this, [this, i]() {
            if (i >= 0 && i < m_pendingAttachments.size()) {
                m_pendingAttachments.removeAt(i);
                rebuildAttachmentBar();
            }
        });
        rowLayout->addWidget(label, 1);
        rowLayout->addWidget(removeBtn);
        m_attachmentLayout->addWidget(rowWidget);
    }
    m_attachmentBar->show();
}

void AiAssistantPanel::clearConversation()
{
    m_service->abort();
    updateBusyState(false);
    m_input->clear();
    m_requestProject = {};
    m_history.clear();
    m_pendingAttachments.clear();
    m_clipboardImageCounter = 0;
    rebuildAttachmentBar();
    m_activeBubble = nullptr;
    m_activeTextLabel = nullptr;
    m_activeBubbleLayout = nullptr;
    m_streamingText.clear();

    while (m_messagesLayout->count() > 2) {
        QLayoutItem *item = m_messagesLayout->takeAt(1);
        delete item->widget();
        delete item;
    }
    m_welcomeCard->show();
}

void AiAssistantPanel::updateBusyState(bool busy)
{
    m_sendButton->setEnabled(!busy);
    m_stopButton->setVisible(busy);
    m_input->setReadOnly(busy);
}

void AiAssistantPanel::scrollToBottom()
{
    QTimer::singleShot(0, this, [this]() {
        if (QScrollBar *bar = m_scroll->verticalScrollBar()) {
            bar->setValue(bar->maximum());
        }
    });
}

void AiAssistantPanel::appendUserBubble(const QString &text, const QList<AiAttachment> &attachments)
{
    m_welcomeCard->hide();
    auto *card = new FluentQt::CardWidget(m_messagesContainer);
    auto *layout = new QVBoxLayout(card);
    layout->setContentsMargins(10, 8, 10, 8);
    layout->setSpacing(4);
    auto *header = new FluentQt::CaptionLabel(tr("我"), card);
    header->setTextColor(QColor(QStringLiteral("#0078d4")), QColor(QStringLiteral("#4cc2ff")));
    layout->addWidget(header);
    if (!attachments.isEmpty()) {
        QStringList attLabels;
        for (const AiAttachment &att : attachments) {
            const QString kindTag = att.kind == AiAttachment::Kind::Image ? tr("[图片]")
                                    : att.kind == AiAttachment::Kind::Pdf ? QStringLiteral("[PDF]")
                                                                          : tr("[文档]");
            attLabels.append(QStringLiteral("%1 %2").arg(kindTag, att.fileName));
        }
        auto *attCaption = new FluentQt::CaptionLabel(attLabels.join(QStringLiteral("  ")), card);
        attCaption->setWordWrap(true);
        layout->addWidget(attCaption);
    }
    if (!text.isEmpty()) {
        auto *body = new FluentQt::BodyLabel(text, card);
        body->setWordWrap(true);
        body->setTextInteractionFlags(Qt::TextSelectableByMouse);
        layout->addWidget(body);
    }
    m_messagesLayout->insertWidget(m_messagesLayout->count() - 1, card);
    scrollToBottom();
}

void AiAssistantPanel::startAssistantBubble()
{
    m_streamingText.clear();
    m_activeBubble = new FluentQt::CardWidget(m_messagesContainer);
    m_activeBubbleLayout = new QVBoxLayout(m_activeBubble);
    m_activeBubbleLayout->setContentsMargins(10, 8, 10, 8);
    m_activeBubbleLayout->setSpacing(6);
    auto *header = new FluentQt::CaptionLabel(tr("AI 助手"), m_activeBubble);
    header->setTextColor(QColor(QStringLiteral("#606060")), QColor(QStringLiteral("#b3b3b3")));
    m_activeTextLabel = new FluentQt::BodyLabel(tr("思考中…"), m_activeBubble);
    m_activeTextLabel->setWordWrap(true);
    m_activeTextLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_activeBubbleLayout->addWidget(header);
    m_activeBubbleLayout->addWidget(m_activeTextLabel);
    m_messagesLayout->insertWidget(m_messagesLayout->count() - 1, m_activeBubble);
    scrollToBottom();
}

void AiAssistantPanel::sendPrompt(const QString &rawText)
{
    QString text = rawText.trimmed();
    if ((text.isEmpty() && m_pendingAttachments.isEmpty()) || m_service->isBusy()) {
        return;
    }
    if (text.isEmpty() && !m_pendingAttachments.isEmpty()) {
        text = tr("请解析附件中的内容，并协助配置当前 HMI 工程。");
    }
    const QList<AiAttachment> attachments = m_pendingAttachments;
    m_pendingAttachments.clear();
    rebuildAttachmentBar();

    refreshModelIndicator();
    m_input->clear();
    appendUserBubble(text, attachments);
    startAssistantBubble();
    updateBusyState(true);

    m_requestProject = m_projectProvider ? m_projectProvider() : Project{};
    QList<AiChatMessage> payload;
    payload.append({QStringLiteral("system"), AiService::buildSystemPrompt(m_requestProject, m_selectedIds), {}});
    // Keep up to the last 8 turns of history so context stays focused and token-efficient
    const qsizetype startIdx = std::max<qsizetype>(0, m_history.size() - 8);
    for (qsizetype i = startIdx; i < m_history.size(); ++i) {
        payload.append(m_history.at(i));
    }
    payload.append({QStringLiteral("user"), text, attachments});
    m_history.append({QStringLiteral("user"), text, attachments});

    m_service->sendChat(AiConfig::load(), payload);
}

void AiAssistantPanel::finalizeAssistantBubble(const QString &fullText)
{
    if (!m_activeBubble || !m_activeTextLabel || !m_activeBubbleLayout) {
        return;
    }
    const QString response = fullText.isEmpty() ? m_streamingText : fullText;
    m_history.append({QStringLiteral("assistant"), response, {}});
    const AiPatchProposal proposal = AiService::extractPatch(response, m_requestProject);

    const QString shownText = proposal.cleanText.isEmpty()
                                  ? (proposal.hasPatch ? tr("已为你生成如下工程变更方案：") : tr("（无文本回复）"))
                                  : proposal.cleanText;
    m_activeTextLabel->setText(shownText);

    if (proposal.hasPatch) {
        auto *patchCard = new FluentQt::CardWidget(m_activeBubble);
        patchCard->setObjectName(QStringLiteral("aiPatchProposalCard"));
        auto *patchLayout = new QVBoxLayout(patchCard);
        patchLayout->setContentsMargins(10, 8, 10, 8);
        patchLayout->setSpacing(6);

        auto *titleLabel = new FluentQt::StrongBodyLabel(proposal.title, patchCard);
        titleLabel->setWordWrap(true);
        patchLayout->addWidget(titleLabel);

        if (proposal.valid) {
            QStringList bullets;
            for (const QString &line : proposal.summaryLines) {
                bullets.append(QStringLiteral("• %1").arg(line));
            }
            auto *summaryLabel = new FluentQt::CaptionLabel(bullets.join(QLatin1Char('\n')), patchCard);
            summaryLabel->setWordWrap(true);
            patchLayout->addWidget(summaryLabel);

            auto *applyBtn = new FluentQt::PrimaryPushButton(FluentQt::icon(FluentQt::FluentIcon::Accept), tr("应用到工程"), patchCard);
            applyBtn->setObjectName(QStringLiteral("aiApplyPatchButton"));
            connect(applyBtn, &QPushButton::clicked, this, [this, applyBtn, patch = proposal.patch, base = m_requestProject]() {
                const Project latest = m_projectProvider ? m_projectProvider() : Project{};
                Project target;
                QStringList touchedIds;
                QString error;
                if (!AiService::applyPatchObject(latest, patch, &target, nullptr, &touchedIds, &error, &base)) {
                    emit notify(tr("无法应用 AI 变更"), error, true);
                    return;
                }
                applyBtn->setText(tr("已应用（可 Ctrl+Z 撤销）"));
                applyBtn->setEnabled(false);
                emit applyProjectRequested(target, touchedIds);
            });
            patchLayout->addWidget(applyBtn);
        } else {
            auto *errLabel = new FluentQt::CaptionLabel(tr("变更未通过工程校验：%1").arg(proposal.error), patchCard);
            errLabel->setWordWrap(true);
            errLabel->setTextColor(QColor(0xC4, 0x2B, 0x1C), QColor(0xFF, 0x99, 0xA4));
            patchLayout->addWidget(errLabel);
        }
        m_activeBubbleLayout->addWidget(patchCard);
    }

    m_activeBubble = nullptr;
    m_activeTextLabel = nullptr;
    m_activeBubbleLayout = nullptr;
    scrollToBottom();
}

void AiAssistantPanel::failAssistantBubble(const QString &errorText)
{
    if (!m_history.isEmpty() && m_history.last().role == QLatin1String("user")) {
        m_history.removeLast();
    }
    if (m_activeTextLabel) {
        m_activeTextLabel->setText(tr("请求失败：%1").arg(errorText));
        m_activeTextLabel->setTextColor(QColor(0xC4, 0x2B, 0x1C), QColor(0xFF, 0x99, 0xA4));
    }
    m_activeBubble = nullptr;
    m_activeTextLabel = nullptr;
    m_activeBubbleLayout = nullptr;
    scrollToBottom();
}

} // namespace fhmi
