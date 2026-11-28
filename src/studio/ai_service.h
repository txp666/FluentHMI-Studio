#pragma once

#include "core/project.h"

#include <QtCore/QJsonObject>
#include <QtCore/QList>
#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QUrl>

class QNetworkAccessManager;
class QNetworkReply;

namespace fhmi {

struct AiConfig
{
    QString provider = QStringLiteral("deepseek");
    QString baseUrl;
    QString apiKey;
    QString model;

    static QStringList providerIds();
    static QString providerDisplayName(const QString &provider);
    static QString presetBaseUrl(const QString &provider);
    static QString presetModel(const QString &provider);
    static QStringList presetModels(const QString &provider);
    static bool requiresApiKey(const QString &provider);
    static QUrl chatCompletionsUrl(const QString &baseUrl);
    static QUrl modelsUrl(const QString &baseUrl);

    static AiConfig load();
    static void save(const AiConfig &config);
    static QString loadProviderApiKey(const QString &provider);
    static void saveProviderApiKey(const QString &provider, const QString &apiKey);
};

struct AiAttachment
{
    enum class Kind
    {
        Image,
        Pdf,
        TextFile,
    };

    Kind kind = Kind::Image;
    QString fileName;
    QString mimeType;
    QByteArray rawBytes;
    QString extractedText;

    static bool fromFile(const QString &filePath, AiAttachment *out, QString *error);
    static AiAttachment fromClipboardImage(const QImage &image, int index = 1);
};

struct AiChatMessage
{
    QString role;
    QString content;
    QList<AiAttachment> attachments;
};

struct AiPatchProposal
{
    bool hasPatch = false;
    bool valid = false;
    QString title;
    QStringList summaryLines;
    QStringList touchedWidgetIds;
    Project patchedProject;
    QString error;
    QString cleanText;
};

class AiService : public QObject
{
    Q_OBJECT

  public:
    explicit AiService(QObject *parent = nullptr);
    ~AiService() override;

    bool isBusy() const { return !m_reply.isNull(); }

    void sendChat(const AiConfig &config, const QList<AiChatMessage> &messages);
    void testConnection(const AiConfig &config);
    void fetchModels(const AiConfig &config);
    void abort();

    static QString buildSystemPrompt(const Project &project, const QStringList &selectedWidgetIds);
    static AiPatchProposal extractPatch(const QString &responseText, const Project &currentProject);
    static bool applyPatchObject(const Project &currentProject, const QJsonObject &patch, Project *outProject,
                                 QStringList *summaryLines, QStringList *touchedWidgetIds, QString *error);

  signals:
    void chunkReceived(const QString &delta);
    void finished(const QString &fullText);
    void failed(const QString &error);
    void testFinished(bool ok, const QString &message);
    void modelsFetched(bool ok, const QStringList &models, const QString &message);

  private:
    void processStreamBuffer(bool finalChunk);
    QString formatReplyError(QNetworkReply *reply, const QByteArray &body) const;

    QNetworkAccessManager *m_network = nullptr;
    QPointer<QNetworkReply> m_reply;
    QByteArray m_streamBuffer;
    QByteArray m_rawResponse;
    QString m_accumulatedText;
    bool m_sawSseData = false;
};

} // namespace fhmi
