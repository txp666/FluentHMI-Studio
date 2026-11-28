#pragma once

#include "core/project.h"
#include "studio/ai_service.h"

#include <QtCore/QList>
#include <QtCore/QStringList>
#include <QtWidgets/QWidget>

#include <functional>

class QLabel;
class QVBoxLayout;

namespace FluentQt {
class BodyLabel;
class CaptionLabel;
class CardWidget;
class PlainTextEdit;
class PrimaryPushButton;
class PushButton;
class ScrollArea;
} // namespace FluentQt

namespace fhmi {

class CanvasItem;

class AiAssistantPanel : public QWidget
{
    Q_OBJECT

  public:
    explicit AiAssistantPanel(QWidget *parent = nullptr);

    void setProjectProvider(std::function<Project()> provider);
    void setSelectedItems(const QList<CanvasItem *> &items);
    void refreshModelIndicator();

  signals:
    void applyProjectRequested(const fhmi::Project &patchedProject, const QStringList &selectIds);
    void notify(const QString &title, const QString &content, bool error);

  protected:
    void showEvent(QShowEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

  private:
    void chooseAttachments();
    bool addAttachmentFile(const QString &filePath);
    void rebuildAttachmentBar();
    void sendPrompt(const QString &text);
    void clearConversation();
    void updateBusyState(bool busy);
    void scrollToBottom();
    void appendUserBubble(const QString &text, const QList<AiAttachment> &attachments);
    void startAssistantBubble();
    void finalizeAssistantBubble(const QString &fullText);
    void failAssistantBubble(const QString &errorText);

    AiService *m_service = nullptr;
    std::function<Project()> m_projectProvider;
    QStringList m_selectedIds;
    QStringList m_selectedSummaries;
    QList<AiChatMessage> m_history;
    QList<AiAttachment> m_pendingAttachments;
    int m_clipboardImageCounter = 0;

    FluentQt::CaptionLabel *m_contextLabel = nullptr;
    FluentQt::CaptionLabel *m_modelLabel = nullptr;
    FluentQt::ScrollArea *m_scroll = nullptr;
    QWidget *m_messagesContainer = nullptr;
    QVBoxLayout *m_messagesLayout = nullptr;
    FluentQt::CardWidget *m_welcomeCard = nullptr;

    FluentQt::CardWidget *m_activeBubble = nullptr;
    FluentQt::BodyLabel *m_activeTextLabel = nullptr;
    QVBoxLayout *m_activeBubbleLayout = nullptr;
    QString m_streamingText;

    QWidget *m_attachmentBar = nullptr;
    QVBoxLayout *m_attachmentLayout = nullptr;
    FluentQt::PlainTextEdit *m_input = nullptr;
    FluentQt::PrimaryPushButton *m_sendButton = nullptr;
    FluentQt::PushButton *m_stopButton = nullptr;
};

} // namespace fhmi
