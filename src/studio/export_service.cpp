#include "studio/export_service.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QDirIterator>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QProcess>
#include <QtCore/QSet>
#include <QtCore/QStandardPaths>

namespace fhmi {

namespace {

bool fail(QString *error, const QString &message)
{
    if (error) {
        *error = message;
    }
    return false;
}

// Recursive copy that keeps symbolic links as links (macOS frameworks rely on them).
bool copyTree(const QString &source, const QString &target, QString *error)
{
    const QFileInfo info(source);
    if (info.isSymLink()) {
        return QFile::link(info.symLinkTarget().startsWith(info.absolutePath())
                               ? QDir(info.absolutePath()).relativeFilePath(info.symLinkTarget())
                               : info.symLinkTarget(),
                           target) ||
               fail(error, QObject::tr("无法创建链接: %1").arg(target));
    }
    if (info.isDir()) {
        if (!QDir().mkpath(target)) {
            return fail(error, QObject::tr("无法创建目录: %1").arg(QDir::toNativeSeparators(target)));
        }
        const QStringList entries =
            QDir(source).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
        for (const QString &entry : entries) {
            if (!copyTree(QDir(source).filePath(entry), QDir(target).filePath(entry), error)) {
                return false;
            }
        }
        return true;
    }
    if (!QFile::copy(source, target)) {
        return fail(error, QObject::tr("无法复制: %1").arg(QDir::toNativeSeparators(source)));
    }
    QFile::setPermissions(target, QFile::permissions(source));
    return true;
}

// Copies every image the project uses into assets/ and points the widgets at the copies.
bool bundleImages(QJsonArray &widgets, const QDir &base, const QDir &assets, QSet<QString> &used, QString *error)
{
    for (int i = 0; i < widgets.size(); ++i) {
        QJsonObject widget = widgets.at(i).toObject();
        QJsonObject props = widget.value(QStringLiteral("properties")).toObject();
        const QString image = props.value(QStringLiteral("imagePath")).toString();
        if (widget.value(QStringLiteral("type")).toString() == QLatin1String("ImageDisplay") && !image.isEmpty()) {
            const QString source = QDir::isRelativePath(image) ? base.absoluteFilePath(image) : image;
            if (!QFileInfo(source).isReadable()) {
                return fail(error, QObject::tr("图片不存在或不可读: %1").arg(QDir::toNativeSeparators(source)));
            }
            QString name = QFileInfo(source).fileName();
            for (int n = 1; used.contains(name); ++n) {
                name = QStringLiteral("%1_%2.%3").arg(QFileInfo(source).completeBaseName()).arg(n).arg(QFileInfo(source).suffix());
            }
            used.insert(name);
            if (!assets.mkpath(QStringLiteral(".")) || !QFile::copy(source, assets.filePath(name))) {
                return fail(error, QObject::tr("无法复制图片: %1").arg(QDir::toNativeSeparators(source)));
            }
            props.insert(QStringLiteral("imagePath"), QStringLiteral("assets/") + name);
            widget.insert(QStringLiteral("properties"), props);
        }
        QJsonArray children = widget.value(QStringLiteral("children")).toArray();
        if (!children.isEmpty()) {
            if (!bundleImages(children, base, assets, used, error)) {
                return false;
            }
            widget.insert(QStringLiteral("children"), children);
        }
        widgets[i] = widget;
    }
    return true;
}

#ifdef Q_OS_MACOS
QString plistEscape(const QString &text)
{
    return text.toHtmlEscaped();
}

// Sets the name the Finder and Dock show, and an identifier of the app's own.
bool rebrandInfoPlist(const QString &plistPath, const QString &appName, QString *error)
{
    QFile file(plistPath);
    if (!file.open(QIODevice::ReadOnly)) {
        return fail(error, QObject::tr("无法读取 Info.plist"));
    }
    QString plist = QString::fromUtf8(file.readAll());
    file.close();
    const auto setKey = [&plist](const QString &key, const QString &value) {
        const QString tag = QStringLiteral("<key>%1</key>").arg(key);
        const int at = plist.indexOf(tag);
        const QString entry = QStringLiteral("<string>%1</string>").arg(plistEscape(value));
        if (at < 0) {
            plist.replace(QStringLiteral("</dict>\n</plist>"), QStringLiteral("\t%1\n\t%2\n</dict>\n</plist>").arg(tag, entry));
            return;
        }
        const int start = plist.indexOf(QStringLiteral("<string>"), at);
        const int end = plist.indexOf(QStringLiteral("</string>"), start);
        if (start >= 0 && end > start) {
            plist.replace(start, end + 9 - start, entry);
        }
    };
    const QByteArray hash = QCryptographicHash::hash(appName.toUtf8(), QCryptographicHash::Sha1).toHex().left(10);
    setKey(QStringLiteral("CFBundleName"), appName);
    setKey(QStringLiteral("CFBundleDisplayName"), appName);
    setKey(QStringLiteral("CFBundleIdentifier"), QStringLiteral("tech.zhangshu.fluenthmi.app") + QString::fromLatin1(hash));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return fail(error, QObject::tr("无法写入 Info.plist"));
    }
    file.write(plist.toUtf8());
    return true;
}

#endif
} // namespace

QString ExportService::safeName(const QString &appName)
{
    QString name = appName.trimmed();
    static const QString forbidden = QStringLiteral("\\/:*?\"<>|");
    for (QChar &c : name) {
        if (forbidden.contains(c) || c.unicode() < 32) {
            c = QLatin1Char('_');
        }
    }
    while (name.endsWith(QLatin1Char('.'))) {
        name.chop(1);
    }
    return name.isEmpty() ? QStringLiteral("FluentHMI App") : name;
}

QString ExportService::runtimePath()
{
    const QDir appDir(QCoreApplication::applicationDirPath());
    QStringList candidates;
#ifdef Q_OS_MACOS
    // Packaged: FluentHMI Studio.app/Contents/Helpers/FluentHMIRuntime.app
    candidates << appDir.absoluteFilePath(QStringLiteral("../Helpers/FluentHMIRuntime.app"));
    // Build tree: both bundles side by side.
    candidates << appDir.absoluteFilePath(QStringLiteral("../../../FluentHMIRuntime.app"));
#ifdef FHMI_RUNTIME_BUILD_PATH
    candidates << QDir(QStringLiteral(FHMI_RUNTIME_BUILD_PATH)).absoluteFilePath(QStringLiteral("../../.."));
#endif
#else
#ifdef Q_OS_WIN
    const QString exe = QStringLiteral("FluentHMIRuntime.exe");
#else
    const QString exe = QStringLiteral("FluentHMIRuntime");
#endif
    // Packaged: beside the studio, sharing its Qt libraries.
    candidates << appDir.absoluteFilePath(exe);
#ifdef FHMI_RUNTIME_BUILD_PATH
    candidates << QStringLiteral(FHMI_RUNTIME_BUILD_PATH);
#endif
#endif
    for (const QString &candidate : std::as_const(candidates)) {
        const QString clean = QDir::cleanPath(candidate);
        if (QFileInfo::exists(clean)) {
            return clean;
        }
    }
    return QString();
}

QString ExportService::exportApplication(const Project &project, const QString &outputDir, QString *error)
{
    const QString runtime = runtimePath();
    if (runtime.isEmpty()) {
        fail(error, QObject::tr("找不到 FluentHMI Runtime，请确认它与 Studio 一同安装"));
        return QString();
    }
    const QString name = safeName(project.appName);
    QJsonArray widgets = project.widgets;

#ifdef Q_OS_MACOS
    const QString target = QDir(outputDir).filePath(name + QStringLiteral(".app"));
    const QString resources = QDir(target).filePath(QStringLiteral("Contents/Resources"));
#else
    const QString target = QDir(outputDir).filePath(name);
    const QString resources = target;
#endif
    if (QFileInfo::exists(target)) {
        fail(error, QObject::tr("目标已存在: %1").arg(QDir::toNativeSeparators(target)));
        return QString();
    }

    // Stage beside the target, so a failure never leaves a half-built application.
    const QString staging = target + QStringLiteral(".partial");
    QDir(staging).removeRecursively();
    const auto abort = [&staging](const QString &) {
        QDir(staging).removeRecursively();
        return QString();
    };
    (void)abort;

#ifdef Q_OS_MACOS
    if (!copyTree(runtime, staging, error)) {
        return abort(QString());
    }
    const QString stagedResources = QDir(staging).filePath(QStringLiteral("Contents/Resources"));
    if (!rebrandInfoPlist(QDir(staging).filePath(QStringLiteral("Contents/Info.plist")), project.appName, error)) {
        return abort(QString());
    }
#else
    // A packaged runtime shares its folder, and Qt, with the studio; runtime-files.txt beside
    // it names the files that are the runtime's. A build tree holds everything else too, so
    // there only the executable is taken. Any other folder is the runtime's own; take all of it.
    const QFileInfo runtimeInfo(runtime);
    // Multi-config generators put the executable one level below the cache (build/Release).
    const QDir runtimeDir(runtimeInfo.absolutePath());
    const bool buildTree = QFileInfo::exists(runtimeDir.filePath(QStringLiteral("CMakeCache.txt"))) ||
                           QFileInfo::exists(runtimeDir.filePath(QStringLiteral("../CMakeCache.txt")));
    QStringList files;
    QFile list(runtimeDir.filePath(QStringLiteral("runtime-files.txt")));
    if (list.open(QIODevice::ReadOnly | QIODevice::Text)) {
        const QStringList lines = QString::fromUtf8(list.readAll()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        for (const QString &line : lines) {
            if (!line.trimmed().isEmpty()) {
                files << line.trimmed();
            }
        }
    } else if (buildTree) {
        files << runtimeInfo.fileName();
    }
    if (files.isEmpty()) {
        if (!copyTree(runtimeDir.absolutePath(), staging, error)) {
            return abort(QString());
        }
        // Drop the studio itself if it shares the folder.
        for (const QString &studio : {QStringLiteral("FluentHMIStudio"), QStringLiteral("FluentHMIStudio.exe")}) {
            QFile::remove(QDir(staging).filePath(studio));
        }
    }
    for (const QString &file : std::as_const(files)) {
        const QString target = QDir(staging).filePath(file);
        if (!QDir().mkpath(QFileInfo(target).absolutePath())) {
            fail(error, QObject::tr("无法创建目录: %1").arg(QDir::toNativeSeparators(QFileInfo(target).absolutePath())));
            return abort(QString());
        }
        if (!copyTree(runtimeDir.filePath(file), target, error)) {
            return abort(QString());
        }
    }
    const QString stagedResources = staging;
    const QString exe = QDir(staging).filePath(runtimeInfo.fileName());
    const QString renamed = QDir(staging).filePath(name + (runtimeInfo.suffix().isEmpty() ? QString() : QStringLiteral(".") + runtimeInfo.suffix()));
    if (exe != renamed && !QFile::rename(exe, renamed)) {
        fail(error, QObject::tr("无法重命名程序"));
        return abort(QString());
    }
#endif
    Q_UNUSED(resources);

    if (!QDir().mkpath(stagedResources)) {
        fail(error, QObject::tr("无法创建目录: %1").arg(QDir::toNativeSeparators(stagedResources)));
        return abort(QString());
    }
    QSet<QString> used;
    if (!bundleImages(widgets, QDir(project.assetBaseDir()), QDir(QDir(stagedResources).filePath(QStringLiteral("assets"))),
                      used, error)) {
        return abort(QString());
    }
    Project exported = project;
    exported.widgets = widgets;
    if (!exported.save(QDir(stagedResources).filePath(QStringLiteral("project.uidp")), error)) {
        return abort(QString());
    }

#ifdef Q_OS_MACOS
    // Editing the bundle invalidates its signature; re-sign ad hoc so it still launches.
    const QString codesign = QStandardPaths::findExecutable(QStringLiteral("codesign"));
    if (!codesign.isEmpty()) {
        QProcess sign;
        sign.start(codesign, {QStringLiteral("--force"), QStringLiteral("--deep"), QStringLiteral("--sign"),
                              QStringLiteral("-"), staging});
        sign.waitForFinished(60000);
    }
#endif
    if (!QDir().rename(staging, target)) {
        fail(error, QObject::tr("无法写入 %1").arg(QDir::toNativeSeparators(target)));
        return abort(QString());
    }
    return target;
}

} // namespace fhmi
