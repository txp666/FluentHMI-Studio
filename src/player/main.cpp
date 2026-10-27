// FluentHMI Runtime: what every exported application runs. It looks for its project
// next to itself (or inside its .app bundle), so an export is just a copy of this
// program with a project.uidp beside it.
#include "player/player_window.h"
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

QString findProject(const QStringList &args)
{
    for (int i = 1; i < args.size(); ++i) {
        if (args.at(i).endsWith(QLatin1String(".uidp"), Qt::CaseInsensitive) && QFileInfo(args.at(i)).isFile()) {
            return args.at(i);
        }
    }
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
    QString path = findProject(app.arguments());
    if (path.isEmpty() && check) {
        QTextStream(stderr) << "no project.uidp found\n";
        return 1;
    }
    if (path.isEmpty()) {
        path = QFileDialog::getOpenFileName(nullptr, QObject::tr("打开工程"), QString(),
                                            QObject::tr("FluentHMI 工程 (*.uidp)"));
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
        FluentQt::MessageBox box(QObject::tr("无法加载工程"), error, nullptr);
        box.hideCancelButton();
        box.exec();
        return 1;
    }
    QCoreApplication::setApplicationName(project.appName);

    fhmi::PlayerWindow window(project);
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
