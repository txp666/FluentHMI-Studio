// FluentHMI Runtime: what every exported application runs. It looks for its project
// next to itself (or inside its .app bundle), so an export is just a copy of this
// program with a project.uidp beside it, and the studio's appearance.json.
#include "player/player_window.h"
#include "runtime/appearance.h"
#include "runtime/runtime_surface.h"

#include <FluentQtWidgets/Config.h>
#include <FluentQtWidgets/Dialogs/Dialog.h>
#include <FluentQtWidgets/Theme.h>

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QStandardPaths>
#include <QtCore/QTextStream>
#include <QtCore/QTimer>
#include <QtWidgets/QApplication>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QStyleFactory>

namespace {

QString projectArgument(const QStringList &args)
{
    for (int i = 1; i < args.size(); ++i) {
        if (args.at(i).endsWith(QLatin1String(".uidp"), Qt::CaseInsensitive) && QFileInfo(args.at(i)).isFile()) {
            return args.at(i);
        }
    }
    return QString();
}

// The project an exported application carries.
QString bundledProject()
{
    const QDir dir(QCoreApplication::applicationDirPath());
    for (const QString &candidate : {dir.filePath(QStringLiteral("project.uidp")),
                                     dir.filePath(QStringLiteral("../Resources/project.uidp"))}) {
        if (QFileInfo(candidate).isFile()) {
            return QDir::cleanPath(candidate);
        }
    }
    return QString();
}

} // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    Q_INIT_RESOURCE(app);
    Q_INIT_RESOURCE(fluentqtwidgets);
    app.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    QCoreApplication::setOrganizationName(QStringLiteral("FluentHMI"));
    QCoreApplication::setApplicationVersion(QStringLiteral(FHMI_VERSION));

    const QString configFile = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) +
                               QStringLiteral("/FluentHMI/fluent.json");
    if (QFileInfo::exists(configFile)) {
        FluentQt::FluentConfig::instance()->setFileName(configFile);
        FluentQt::FluentConfig::instance()->load();
    }
    FluentQt::ThemeManager::instance()->setTheme(FluentQt::FluentConfig::instance()->themeMode());

    // A project named on the command line comes first; otherwise this is an exported application,
    // which starts with the theme mode and colour and the language setting of the studio that exported it.
    QString path = projectArgument(app.arguments());
    const QString bundled = path.isEmpty() ? bundledProject() : QString();
    // The language goes first, before any widget exists: the exported one, else the studio's setting, else Auto
    // (FluentConfig's default when there is no fluent.json).
    QString language = bundled.isEmpty() ? QString() : fhmi::appearanceLanguage(QFileInfo(bundled).absolutePath());
    if (language.isEmpty()) {
        language = FluentQt::FluentConfig::instance()->localeName();
    }
    fhmi::installLanguage(&app, language);
    if (!bundled.isEmpty()) {
        path = bundled;
        fhmi::applyAppearance(QFileInfo(path).absolutePath());
#ifdef Q_OS_LINUX
        // Wayland docks show an application's icon only through its desktop entry, which they find by this id: an
        // entry named after the program (<App>.desktop, as the README shows) then matches the exported application.
        QGuiApplication::setDesktopFileName(QFileInfo(QCoreApplication::applicationFilePath()).fileName());
#endif
    }

    // Developer options: --theme light|dark overrides the saved theme, and
    // --screenshot <png> renders the whole surface to an image, then exits.
    const QStringList args = app.arguments();
    const auto optionValue = [&args](const QString &name) {
        const int at = int(args.indexOf(name));
        return at >= 0 && at + 1 < args.size() ? args.at(at + 1) : QString();
    };
    const QString theme = optionValue(QStringLiteral("--theme"));
    if (theme == QLatin1String("dark") || theme == QLatin1String("light")) {
        FluentQt::ThemeManager::instance()->setTheme(theme == QLatin1String("dark") ? FluentQt::Theme::Dark
                                                                                   : FluentQt::Theme::Light);
    }
    const QString screenshot = optionValue(QStringLiteral("--screenshot"));

    // --check: load the project and build its window, then exit. Packaging uses it to prove
    // an exported application starts with the Qt it carries.
    const bool check = app.arguments().contains(QStringLiteral("--check"));
    if (path.isEmpty() && check) {
        QTextStream(stderr) << "no project.uidp found\n";
        return 1;
    }
    if (path.isEmpty()) {
        path = QFileDialog::getOpenFileName(nullptr, QCoreApplication::translate("fhmi", "打开工程"), QString(),
                                            QCoreApplication::translate("fhmi", "FluentHMI 工程 (*.uidp)"));
        if (path.isEmpty()) {
            return 0;
        }
    }
    fhmi::Project project;
    QString error;
    if (!fhmi::Project::load(path, &project, &error)) {
        if (check) {
            QTextStream(stderr) << error << '\n';
            return 1;
        }
        FluentQt::MessageBox box(QCoreApplication::translate("fhmi", "无法加载工程"), error, nullptr);
        box.hideCancelButton();
        box.exec();
        return 1;
    }
    QCoreApplication::setApplicationName(project.appName);

    fhmi::PlayerWindow window(project);
    // The project's icon for the whole application too: the Dock and the taskbar show it.
    if (!project.icon.isEmpty()) {
        QApplication::setWindowIcon(window.windowIcon());
    }
    if (check) {
        QTextStream(stdout) << "ok: " << project.appName << '\n';
        return 0;
    }
    window.show();
    if (!screenshot.isEmpty()) {
        QTimer::singleShot(1000, &window, [&window, screenshot]() {
            const bool saved = window.surface()->grab().save(screenshot);
            QTextStream(saved ? stdout : stderr) << (saved ? "saved " : "cannot save ") << screenshot << '\n';
            QCoreApplication::exit(saved ? 0 : 1);
        });
    }
    return app.exec();
}
