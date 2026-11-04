#include "runtime/appearance.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>

namespace fhmi {

namespace {

// The names FluentConfig writes to its own file.
QString modeName(FluentQt::Theme mode)
{
    switch (mode) {
    case FluentQt::Theme::Dark:
        return QStringLiteral("Dark");
    case FluentQt::Theme::Auto:
        return QStringLiteral("Auto");
    case FluentQt::Theme::Light:
        break;
    }
    return QStringLiteral("Light");
}

} // namespace

bool writeAppearance(const QString &dir, FluentQt::Theme mode, const QColor &color, QString *error)
{
    const QString path = QDir(dir).filePath(QLatin1String(kAppearanceFile));
    QFile file(path);
    const QJsonObject appearance{{QStringLiteral("ThemeMode"), modeName(mode)},
                                 {QStringLiteral("ThemeColor"), color.name(QColor::HexRgb)}};
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) || file.write(QJsonDocument(appearance).toJson()) < 0) {
        if (error) {
            *error = QObject::tr("无法写入 %1").arg(QDir::toNativeSeparators(path));
        }
        return false;
    }
    return true;
}

bool applyAppearance(const QString &dir)
{
    QFile file(QDir(dir).filePath(QLatin1String(kAppearanceFile)));
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    const QJsonObject appearance = QJsonDocument::fromJson(file.readAll()).object();
    if (appearance.isEmpty()) {
        return false;
    }
    auto *theme = FluentQt::ThemeManager::instance();
    const QString mode = appearance.value(QStringLiteral("ThemeMode")).toString();
    for (const FluentQt::Theme candidate : {FluentQt::Theme::Light, FluentQt::Theme::Dark, FluentQt::Theme::Auto}) {
        if (mode.compare(modeName(candidate), Qt::CaseInsensitive) == 0) {
            theme->setTheme(candidate);
        }
    }
    const QColor color(appearance.value(QStringLiteral("ThemeColor")).toString());
    if (color.isValid()) {
        theme->setAccentColor(color);
    }
    return true;
}

} // namespace fhmi
