#include "studio/update_service.h"

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

namespace fhmi {

namespace {

constexpr int CheckTimeoutMs = 15000;
// Downloads may be slow; this is how long no data may arrive, not the whole transfer.
constexpr int DownloadStallMs = 60000;

} // namespace

UpdateService::UpdateService(QObject *parent)
    : QObject(parent), m_network(new QNetworkAccessManager(this)), m_releaseUrl(defaultReleaseUrl()),
      m_packageSuffix(UpdateRelease::packageSuffix())
{
    qRegisterMetaType<fhmi::ReleaseInfo>();
}

UpdateService::~UpdateService() { abort(); }

QUrl UpdateService::defaultReleaseUrl()
{
    const QString overridden = qEnvironmentVariable("FHMI_UPDATE_URL");
    return QUrl(overridden.isEmpty()
                    ? QStringLiteral("https://api.github.com/repos/txp666/FluentHMI-Studio/releases/latest")
                    : overridden);
}

QNetworkReply *UpdateService::get(const QUrl &url, int timeoutMs)
{
    QNetworkRequest request(url);
    request.setRawHeader("Accept", "application/vnd.github+json");
    // GitHub refuses API requests without one.
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("FluentHMI-Studio/" FHMI_VERSION));
    request.setTransferTimeout(timeoutMs);
    QNetworkReply *reply = m_network->get(request);
    m_reply = reply;
    return reply;
}

QString UpdateService::replyError(QNetworkReply *reply) const
{
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status == 404) {
        return tr("没有找到已发布的版本");
    }
    if (status == 403 && reply->rawHeader("x-ratelimit-remaining") == "0") {
        return tr("GitHub 请求次数已达上限，请稍后再试");
    }
    if (status >= 400) {
        return tr("服务器返回 HTTP %1").arg(status);
    }
    if (reply->error() == QNetworkReply::OperationCanceledError || reply->error() == QNetworkReply::TimeoutError) {
        return tr("连接超时");
    }
    return tr("网络错误: %1").arg(reply->errorString());
}

void UpdateService::check()
{
    abort();
    QNetworkReply *reply = get(m_releaseUrl, CheckTimeoutMs);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        m_reply.clear();
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            emit failed(replyError(reply));
            return;
        }
        ReleaseInfo release;
        QString error;
        if (!UpdateRelease::parse(reply->readAll(), m_packageSuffix, &release, &error)) {
            emit failed(error);
            return;
        }
        emit checked(release);
    });
}

void UpdateService::download(const ReleaseInfo &release, const QString &dir)
{
    abort();
    m_release = release;
    m_target = QDir(dir).filePath(release.package.name);
    if (!release.hasPackage()) {
        emit failed(tr("这个版本没有适用于本平台的安装包"));
        return;
    }
    // The checksum first: it is small, and without it the package cannot be trusted.
    QNetworkReply *reply = get(release.checksum.url, CheckTimeoutMs);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        m_reply.clear();
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            emit failed(replyError(reply));
            return;
        }
        const QByteArray digest = UpdateRelease::checksumFor(reply->readAll(), m_release.package.name);
        if (digest.isEmpty()) {
            emit failed(tr("校验文件中没有 %1 的 SHA-256").arg(m_release.package.name));
            return;
        }
        fetchPackage(digest);
    });
}

void UpdateService::fetchPackage(const QByteArray &expectedDigest)
{
    QDir().mkpath(QFileInfo(m_target).absolutePath());
    m_partial.setFileName(m_target + QStringLiteral(".part"));
    if (!m_partial.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        emit failed(tr("无法写入 %1").arg(QDir::toNativeSeparators(m_partial.fileName())));
        return;
    }
    m_hash.reset();
    QNetworkReply *reply = get(m_release.package.url, DownloadStallMs);
    connect(reply, &QNetworkReply::readyRead, this, [this, reply]() {
        const QByteArray chunk = reply->readAll();
        m_hash.addData(chunk);
        m_partial.write(chunk);
    });
    connect(reply, &QNetworkReply::downloadProgress, this, [this](qint64 received, qint64 total) {
        emit progress(received, total > 0 ? total : m_release.package.size);
    });
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, expectedDigest]() { finishPackage(reply, expectedDigest); });
}

void UpdateService::finishPackage(QNetworkReply *reply, const QByteArray &expectedDigest)
{
    m_reply.clear();
    reply->deleteLater();
    if (reply->error() != QNetworkReply::NoError) {
        discardPartial();
        emit failed(replyError(reply));
        return;
    }
    const QByteArray rest = reply->readAll();
    m_hash.addData(rest);
    m_partial.write(rest);
    const qint64 size = m_partial.size();
    m_partial.close();
    if (m_release.package.size > 0 && size != m_release.package.size) {
        discardPartial();
        emit failed(tr("下载不完整（%1 / %2 字节）").arg(size).arg(m_release.package.size));
        return;
    }
    if (m_hash.result().toHex() != expectedDigest) {
        discardPartial();
        emit failed(tr("安装包校验失败：SHA-256 与发布的校验文件不一致"));
        return;
    }
    QFile::remove(m_target);
    if (!m_partial.rename(m_target)) {
        discardPartial();
        emit failed(tr("无法写入 %1").arg(QDir::toNativeSeparators(m_target)));
        return;
    }
    emit downloaded(m_target);
}

void UpdateService::discardPartial()
{
    m_partial.close();
    m_partial.remove();
}

void UpdateService::abort()
{
    if (QNetworkReply *reply = m_reply.data()) {
        m_reply.clear();
        reply->disconnect(this);
        reply->abort();
        reply->deleteLater();
    }
    if (m_partial.isOpen()) {
        discardPartial();
    }
}

} // namespace fhmi
