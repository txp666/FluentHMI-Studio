#include "studio/export_service.h"

#include "runtime/appearance.h"

#include <FluentQtWidgets/Config.h>

#include <QtCore/QBuffer>
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
#include <QtCore/QTemporaryDir>
#include <QtCore/QtEndian>
#include <QtGui/QImage>
#include <QtGui/QImageReader>
#include <QtGui/QImageWriter>
#include <QtGui/QPainter>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

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

// A name in assets/ that no other copied file has taken.
QString assetName(const QString &source, QSet<QString> &used)
{
    const QFileInfo info(source);
    QString name = info.fileName();
    for (int n = 1; used.contains(name); ++n) {
        name = QStringLiteral("%1_%2.%3").arg(info.completeBaseName()).arg(n).arg(info.suffix());
    }
    used.insert(name);
    return name;
}

// The largest image in the file (an .ico holds several), or a null image when it cannot be read.
QImage largestImage(const QString &path)
{
    QImageReader reader(path);
    QImage largest;
    for (int i = 0; i < std::max(1, reader.imageCount()); ++i) {
        if (i > 0 && !reader.jumpToImage(i)) {
            break;
        }
        const QImage image = reader.read();
        if (image.width() * image.height() > largest.width() * largest.height()) {
            largest = image;
        }
    }
    return largest;
}

#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
// The image scaled into a square of `side` pixels, centred on a transparent background.
QImage squared(const QImage &image, int side)
{
    QImage canvas(side, side, QImage::Format_ARGB32_Premultiplied);
    canvas.fill(Qt::transparent);
    const QImage scaled = image.scaled(side, side, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    QPainter painter(&canvas);
    painter.drawImage((side - scaled.width()) / 2, (side - scaled.height()) / 2, scaled);
    return canvas;
}
#endif

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
            const QString name = assetName(source, used);
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

// Sets the name the Finder and Dock show, an identifier of the app's own and, if given, its icon file.
bool rebrandInfoPlist(const QString &plistPath, const QString &appName, const QString &iconFile, QString *error)
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
    setKey(QStringLiteral("CFBundleIdentifier"), QStringLiteral("io.github.txp666.fluenthmi.app") + QString::fromLatin1(hash));
    if (!iconFile.isEmpty()) {
        setKey(QStringLiteral("CFBundleIconFile"), iconFile);
    }
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return fail(error, QObject::tr("无法写入 Info.plist"));
    }
    file.write(plist.toUtf8());
    return true;
}

// The bundle's icon, made by iconutil (part of every macOS) from an iconset of the image.
bool writeIcns(const QImage &image, const QString &target, QString *error)
{
    const QString cannot = QObject::tr("无法生成应用图标");
    QTemporaryDir dir;
    const QString iconset = dir.filePath(QStringLiteral("icon.iconset"));
    if (!dir.isValid() || !QDir().mkpath(iconset)) {
        return fail(error, cannot);
    }
    for (const int side : {16, 32, 128, 256, 512}) {
        if (!squared(image, side).save(QStringLiteral("%1/icon_%2x%2.png").arg(iconset).arg(side), "PNG") ||
            !squared(image, side * 2).save(QStringLiteral("%1/icon_%2x%2@2x.png").arg(iconset).arg(side), "PNG")) {
            return fail(error, cannot);
        }
    }
    QProcess iconutil;
    iconutil.start(QStringLiteral("/usr/bin/iconutil"),
                   {QStringLiteral("-c"), QStringLiteral("icns"), QStringLiteral("-o"), target, iconset});
    if (!iconutil.waitForFinished(60000) || iconutil.exitStatus() != QProcess::NormalExit || iconutil.exitCode() != 0 ||
        !QFileInfo(target).isFile()) {
        return fail(error, QStringLiteral("%1: %2").arg(
                               cannot, QString::fromLocal8Bit(iconutil.readAllStandardError()).trimmed()));
    }
    return true;
}

#endif

#ifdef Q_OS_WIN
// The exported program's icon on Windows. Explorer shows the first icon group in an .exe, and Qt's windows
// start with the one named IDI_ICON1 (resources/app_icon.rc.in); the runtime's groups and their images are
// replaced by one group of the project's icon, under the name and language the runtime's first group had.

// One image of an icon: its directory entry and its data (a DIB or a PNG), as an RT_ICON resource holds it.
struct IconImage
{
    quint8 width = 0; // 0 means 256
    quint8 height = 0;
    quint8 colors = 0;
    quint16 planes = 1;
    quint16 bitCount = 32;
    QByteArray data;
};

// The images of an .ico file, or none if it is not one.
QList<IconImage> icoImages(const QByteArray &ico)
{
    if (ico.size() < 6 || qFromLittleEndian<quint16>(ico.constData()) != 0 ||
        qFromLittleEndian<quint16>(ico.constData() + 2) != 1) {
        return {};
    }
    QList<IconImage> images;
    const int count = qFromLittleEndian<quint16>(ico.constData() + 4);
    for (int i = 0; i < count; ++i) {
        if (6 + 16 * (i + 1) > ico.size()) {
            return {};
        }
        const char *entry = ico.constData() + 6 + 16 * i;
        const quint32 size = qFromLittleEndian<quint32>(entry + 8);
        const quint32 offset = qFromLittleEndian<quint32>(entry + 12);
        if (size == 0 || quint64(offset) + size > quint64(ico.size())) {
            return {};
        }
        IconImage image;
        image.width = quint8(entry[0]);
        image.height = quint8(entry[1]);
        image.colors = quint8(entry[2]);
        // Some writers leave these 0 for PNG images.
        image.planes = std::max<quint16>(1, qFromLittleEndian<quint16>(entry + 4));
        image.bitCount = qFromLittleEndian<quint16>(entry + 6) ? qFromLittleEndian<quint16>(entry + 6) : quint16(32);
        image.data = ico.mid(offset, size);
        images.append(image);
    }
    return images;
}

// The image at the usual icon sizes, each written by Qt's ICO writer.
QList<IconImage> iconImagesOf(const QImage &image)
{
    QList<IconImage> images;
    for (const int side : {16, 24, 32, 48, 64, 128, 256}) {
        QBuffer buffer;
        buffer.open(QIODevice::WriteOnly);
        QImageWriter writer(&buffer, "ico");
        const QList<IconImage> one = writer.write(squared(image, side)) ? icoImages(buffer.data()) : QList<IconImage>();
        if (one.size() != 1) {
            return {};
        }
        images.append(one.first());
    }
    return images;
}

// A group icon resource of the program, with each language it exists in and the RT_ICON ids it uses.
struct IconGroup
{
    std::wstring name; // empty for a numeric id
    WORD id = 0;
    WORD language = 0;
    QList<WORD> icons;

    LPCWSTR key() const { return name.empty() ? MAKEINTRESOURCEW(id) : name.c_str(); }
};

const LPCWSTR kIconType = MAKEINTRESOURCEW(3);       // RT_ICON
const LPCWSTR kGroupIconType = MAKEINTRESOURCEW(14); // RT_GROUP_ICON

BOOL CALLBACK collectGroupName(HMODULE, LPCWSTR, LPWSTR name, LONG_PTR groups)
{
    IconGroup group;
    if (IS_INTRESOURCE(name)) {
        group.id = WORD(reinterpret_cast<ULONG_PTR>(name));
    } else {
        group.name = name;
    }
    reinterpret_cast<QList<IconGroup> *>(groups)->append(group);
    return TRUE;
}

BOOL CALLBACK collectLanguage(HMODULE, LPCWSTR, LPCWSTR, WORD language, LONG_PTR languages)
{
    reinterpret_cast<QList<WORD> *>(languages)->append(language);
    return TRUE;
}

bool replaceProgramIcon(const QString &exe, const QList<IconImage> &images, QString *error)
{
    const QString cannot = QObject::tr("无法写入程序图标: %1").arg(QDir::toNativeSeparators(exe));
    const std::wstring path = QDir::toNativeSeparators(exe).toStdWString();
    QList<IconGroup> groups;
    HMODULE module = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (!module) {
        return fail(error, cannot);
    }
    QList<IconGroup> named;
    EnumResourceNamesW(module, kGroupIconType, collectGroupName, reinterpret_cast<LONG_PTR>(&named));
    for (const IconGroup &name : std::as_const(named)) {
        QList<WORD> languages;
        EnumResourceLanguagesW(module, kGroupIconType, name.key(), collectLanguage,
                               reinterpret_cast<LONG_PTR>(&languages));
        for (const WORD language : std::as_const(languages)) {
            IconGroup group = name;
            group.language = language;
            // GRPICONDIR: 6 bytes, then 14 per image, each ending in its RT_ICON id.
            HRSRC found = FindResourceExW(module, kGroupIconType, group.key(), language);
            HGLOBAL loaded = found ? LoadResource(module, found) : nullptr;
            const auto *data = loaded ? static_cast<const uchar *>(LockResource(loaded)) : nullptr;
            const DWORD size = found ? SizeofResource(module, found) : 0;
            const int count = data && size >= 6 ? qFromLittleEndian<quint16>(data + 4) : 0;
            for (int i = 0; i < count && DWORD(6 + 14 * (i + 1)) <= size; ++i) {
                group.icons.append(qFromLittleEndian<quint16>(data + 6 + 14 * i + 12));
            }
            groups.append(group);
        }
    }
    FreeLibrary(module); // a mapped file cannot be rewritten

    HANDLE update = BeginUpdateResourceW(path.c_str(), FALSE);
    if (!update) {
        return fail(error, cannot);
    }
    // Removing what is not there only fails; the writes below are what must succeed.
    for (const IconGroup &group : std::as_const(groups)) {
        UpdateResourceW(update, kGroupIconType, group.key(), group.language, nullptr, 0);
        for (const WORD icon : group.icons) {
            UpdateResourceW(update, kIconType, MAKEINTRESOURCEW(icon), group.language, nullptr, 0);
        }
    }
    const IconGroup target = groups.isEmpty()
                                 ? IconGroup{L"IDI_ICON1", 0, WORD(MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL)), {}}
                                 : groups.first();
    QByteArray directory(6, '\0');
    qToLittleEndian<quint16>(1, directory.data() + 2);
    qToLittleEndian<quint16>(quint16(images.size()), directory.data() + 4);
    bool ok = true;
    for (int i = 0; i < images.size(); ++i) {
        const IconImage &image = images.at(i);
        const WORD id = WORD(i + 1);
        ok = ok && UpdateResourceW(update, kIconType, MAKEINTRESOURCEW(id), target.language,
                                   const_cast<char *>(image.data.constData()), DWORD(image.data.size()));
        char entry[14] = {};
        entry[0] = char(image.width);
        entry[1] = char(image.height);
        entry[2] = char(image.colors);
        qToLittleEndian<quint16>(image.planes, entry + 4);
        qToLittleEndian<quint16>(image.bitCount, entry + 6);
        qToLittleEndian<quint32>(quint32(image.data.size()), entry + 8);
        qToLittleEndian<quint16>(id, entry + 12);
        directory.append(entry, sizeof entry);
    }
    ok = ok && UpdateResourceW(update, kGroupIconType, target.key(), target.language, directory.data(),
                               DWORD(directory.size()));
    // On a failed write, discard every change rather than leave the program without an icon.
    const bool written = EndUpdateResourceW(update, ok ? FALSE : TRUE) && ok;
    return written || fail(error, cannot);
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
    // Read before anything is copied: a set icon that cannot be read fails the export, as images do.
    const QString iconSource = project.iconPath();
    const QImage icon = iconSource.isEmpty() ? QImage() : largestImage(iconSource);
    if (!iconSource.isEmpty() && icon.isNull()) {
        fail(error, QObject::tr("图标不存在或不可读: %1").arg(QDir::toNativeSeparators(iconSource)));
        return QString();
    }

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
    const QString icnsName = QStringLiteral("AppIcon.icns");
    if (!rebrandInfoPlist(QDir(staging).filePath(QStringLiteral("Contents/Info.plist")), project.appName,
                          icon.isNull() ? QString() : icnsName, error)) {
        return abort(QString());
    }
    if (!icon.isNull()) {
        if (!writeIcns(icon, QDir(stagedResources).filePath(icnsName), error)) {
            return abort(QString());
        }
        // The runtime's own icon, which the bundle no longer names.
        QFile::remove(QDir(stagedResources).filePath(QStringLiteral("app_icon.icns")));
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
#ifdef Q_OS_WIN
    if (!icon.isNull()) {
        // An .ico goes in as it is, with all its sizes; anything else (or an .ico that is not one) as made here.
        QList<IconImage> images;
        QFile iconFile(iconSource);
        if (iconSource.endsWith(QLatin1String(".ico"), Qt::CaseInsensitive) && iconFile.open(QIODevice::ReadOnly)) {
            images = icoImages(iconFile.readAll());
        }
        if (images.isEmpty()) {
            images = iconImagesOf(icon);
        }
        if (images.isEmpty()) {
            fail(error, QObject::tr("无法生成应用图标"));
            return abort(QString());
        }
        if (!replaceProgramIcon(renamed, images, error)) {
            return abort(QString());
        }
    }
#endif
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
    // A copy beside the project, for the runtime's window icon.
    if (!icon.isNull()) {
        const QString copy = assetName(iconSource, used);
        const QDir assets(QDir(stagedResources).filePath(QStringLiteral("assets")));
        if (!assets.mkpath(QStringLiteral(".")) || !QFile::copy(iconSource, assets.filePath(copy))) {
            fail(error, QObject::tr("无法复制图标: %1").arg(QDir::toNativeSeparators(iconSource)));
            return abort(QString());
        }
        exported.icon = QStringLiteral("assets/") + copy;
    }
    if (!exported.save(QDir(stagedResources).filePath(QStringLiteral("project.uidp")), error)) {
        return abort(QString());
    }
    // The studio's theme mode and colour, as its preview shows them.
    if (!writeAppearance(stagedResources, FluentQt::FluentConfig::instance()->themeMode(),
                         FluentQt::FluentConfig::instance()->themeColor(), error)) {
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
