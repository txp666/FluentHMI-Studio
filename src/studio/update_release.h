#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QString>
#include <QtCore/QUrl>
#include <QtCore/QVersionNumber>

namespace fhmi {

// A file attached to a release.
struct ReleaseAsset
{
    QString name;
    QUrl url;
    qint64 size = 0;
};

// The latest published release: its version, notes and page, and the package built for
// this platform with its .sha256, when the release has them.
struct ReleaseInfo
{
    QString tag;
    QVersionNumber version;
    QString notes;
    QUrl page;
    ReleaseAsset package;
    ReleaseAsset checksum;

    bool hasPackage() const { return package.url.isValid() && checksum.url.isValid(); }
};

namespace UpdateRelease {

// How this platform's package name ends ("-macos-universal.zip", "-windows-x64.zip"), or
// empty where no package is built.
QString packageSuffix();

// Reads GitHub's "latest release" JSON. The package is the asset whose name ends with
// `packageSuffix`; its checksum is the asset named like it plus ".sha256".
bool parse(const QByteArray &json, const QString &packageSuffix, ReleaseInfo *release, QString *error);

// Whether `release` is newer than the running version, given as "0.1.1".
bool isNewer(const ReleaseInfo &release, const QString &current);

// The part of bilingual release notes (Chinese, then a line "## English" and the English notes) for one
// language: what follows that line in English, what precedes it in Chinese; all of it without the line.
QString notesFor(const QString &notes, bool english);

// The SHA-256 that a "<hex>  <file name>" checksum file gives for `fileName`, in lower-case
// hex; empty when the file names another file or is malformed.
QByteArray checksumFor(const QByteArray &checksumFile, const QString &fileName);

} // namespace UpdateRelease
} // namespace fhmi
