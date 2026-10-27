#include "studio/update_release.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QRegularExpression>

namespace fhmi::UpdateRelease {

QString packageSuffix()
{
#if defined(Q_OS_MACOS)
    return QStringLiteral("-macos-universal.zip");
#elif defined(Q_OS_WIN) && defined(Q_PROCESSOR_X86_64)
    return QStringLiteral("-windows-x64.zip");
#else
    return QString();
#endif
}

bool parse(const QByteArray &json, const QString &packageSuffix, ReleaseInfo *release, QString *error)
{
    const auto fail = [error](const QString &message) {
        if (error) {
            *error = message;
        }
        return false;
    };
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(json, &parseError);
    if (!document.isObject()) {
        return fail(QStringLiteral("发布信息不是有效的 JSON: %1").arg(parseError.errorString()));
    }
    const QJsonObject root = document.object();
    ReleaseInfo info;
    info.tag = root.value(QStringLiteral("tag_name")).toString();
    QString number = info.tag;
    if (number.startsWith(QLatin1Char('v')) || number.startsWith(QLatin1Char('V'))) {
        number.remove(0, 1);
    }
    qsizetype parsedUpTo = 0;
    info.version = QVersionNumber::fromString(number, &parsedUpTo);
    if (info.version.isNull() || parsedUpTo != number.size()) {
        return fail(QStringLiteral("无法识别的版本号: %1").arg(info.tag));
    }
    info.notes = root.value(QStringLiteral("body")).toString();
    info.page = QUrl(root.value(QStringLiteral("html_url")).toString());

    const QJsonArray assets = root.value(QStringLiteral("assets")).toArray();
    const auto assetAt = [](const QJsonObject &object) {
        ReleaseAsset asset;
        asset.name = object.value(QStringLiteral("name")).toString();
        asset.url = QUrl(object.value(QStringLiteral("browser_download_url")).toString());
        asset.size = object.value(QStringLiteral("size")).toInteger();
        return asset;
    };
    if (!packageSuffix.isEmpty()) {
        for (const QJsonValue &value : assets) {
            const ReleaseAsset asset = assetAt(value.toObject());
            if (asset.name.endsWith(packageSuffix) && asset.url.isValid()) {
                info.package = asset;
                break;
            }
        }
    }
    if (!info.package.name.isEmpty()) {
        for (const QJsonValue &value : assets) {
            const ReleaseAsset asset = assetAt(value.toObject());
            if (asset.name == info.package.name + QStringLiteral(".sha256") && asset.url.isValid()) {
                info.checksum = asset;
                break;
            }
        }
    }
    *release = info;
    return true;
}

bool isNewer(const ReleaseInfo &release, const QString &current)
{
    return QVersionNumber::compare(release.version, QVersionNumber::fromString(current)) > 0;
}

QByteArray checksumFor(const QByteArray &checksumFile, const QString &fileName)
{
    // "<64 hex digits>  <name>", or "<hex> *<name>" for the binary marker some tools write.
    static const QRegularExpression line(QStringLiteral("^([0-9A-Fa-f]{64}) [ *](.+)$"));
    const QStringList lines = QString::fromUtf8(checksumFile).split(QLatin1Char('\n'));
    for (const QString &text : lines) {
        const QRegularExpressionMatch match = line.match(text.trimmed());
        if (match.hasMatch() && match.captured(2).trimmed() == fileName) {
            return match.captured(1).toLower().toLatin1();
        }
    }
    return QByteArray();
}

} // namespace fhmi::UpdateRelease
