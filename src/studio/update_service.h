#pragma once

#include "studio/update_release.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QFile>
#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QUrl>

class QNetworkAccessManager;
class QNetworkReply;

namespace fhmi {

// Asks GitHub for the latest release and downloads this platform's package, verified
// against the release's .sha256. One request at a time; a new one cancels the last.
class UpdateService : public QObject
{
    Q_OBJECT

  public:
    explicit UpdateService(QObject *parent = nullptr);
    ~UpdateService() override;

    // The latest-release API of this project; FHMI_UPDATE_URL replaces it, for testing an
    // update against a local server.
    static QUrl defaultReleaseUrl();
    void setReleaseUrl(const QUrl &url) { m_releaseUrl = url; }
    void setPackageSuffix(const QString &suffix) { m_packageSuffix = suffix; }

    // Answers with checked() or failed().
    void check();
    // Downloads the package into `dir`; answers with downloaded() and the file's path, or
    // failed(). A package whose size or SHA-256 does not match is deleted.
    void download(const ReleaseInfo &release, const QString &dir);
    void abort();
    bool isBusy() const { return !m_reply.isNull(); }

  signals:
    void checked(const fhmi::ReleaseInfo &release);
    void progress(qint64 received, qint64 total);
    void downloaded(const QString &path);
    void failed(const QString &message);

  private:
    QNetworkReply *get(const QUrl &url, int timeoutMs);
    QString replyError(QNetworkReply *reply) const;
    void fetchPackage(const QByteArray &expectedDigest);
    void finishPackage(QNetworkReply *reply, const QByteArray &expectedDigest);
    void discardPartial();

    QNetworkAccessManager *m_network = nullptr;
    QPointer<QNetworkReply> m_reply;
    QUrl m_releaseUrl;
    QString m_packageSuffix;
    ReleaseInfo m_release;
    QString m_target;
    QFile m_partial;
    QCryptographicHash m_hash{QCryptographicHash::Sha256};
};

} // namespace fhmi

Q_DECLARE_METATYPE(fhmi::ReleaseInfo)
