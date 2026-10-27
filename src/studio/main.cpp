#include "studio/export_service.h"
#include "studio/main_window.h"
#include "studio/update_controller.h"

#include <FluentQtWidgets/Config.h>
#include <FluentQtWidgets/Theme.h>

#include <QtCore/QCommandLineParser>
#include <QtCore/QDir>
#include <QtCore/QTextStream>
#include <QtCore/QStandardPaths>
#include <QtWidgets/QApplication>
#include <QtWidgets/QStyleFactory>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    Q_INIT_RESOURCE(app);
    Q_INIT_RESOURCE(fluentqtwidgets);
    app.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    QCoreApplication::setOrganizationName(QStringLiteral("FluentHMI"));
    QCoreApplication::setApplicationName(QStringLiteral("FluentHMI Studio"));
    QCoreApplication::setApplicationVersion(QStringLiteral(FHMI_VERSION));

    // Exported applications read the same file, so they follow the studio's theme.
    const QString configDir = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) +
                              QStringLiteral("/FluentHMI");
    QDir().mkpath(configDir);
    FluentQt::FluentConfig::instance()->setFileName(QDir(configDir).filePath(QStringLiteral("fluent.json")));
    FluentQt::FluentConfig::instance()->load();
    FluentQt::ThemeManager::instance()->setTheme(FluentQt::FluentConfig::instance()->themeMode());

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("FluentHMI Studio"));
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption exportOption(
        QStringLiteral("export"), QStringLiteral("Export <project> as a standalone application into <dir>, then exit."),
        QStringLiteral("dir"));
    parser.addOption(exportOption);
    const QCommandLineOption updateOption(
        QStringLiteral("update"),
        QStringLiteral("Install the latest release if it is newer, then exit. The installation is replaced once "
                       "this process has ended."));
    parser.addOption(updateOption);
    parser.addPositionalArgument(QStringLiteral("project"), QStringLiteral("A .uidp project to open."));
    parser.process(app);
    const QStringList projects = parser.positionalArguments();

    if (parser.isSet(updateOption)) {
        return fhmi::UpdateController::runHeadless();
    }
    if (parser.isSet(exportOption)) {
        // Headless export, for scripts and CI.
        QTextStream err(stderr);
        if (projects.isEmpty()) {
            err << "--export needs a project file\n";
            return 2;
        }
        fhmi::Project project;
        QString error;
        if (!fhmi::Project::load(projects.first(), &project, &error)) {
            err << error << '\n';
            return 1;
        }
        const QString created = fhmi::ExportService::exportApplication(project, parser.value(exportOption), &error);
        if (created.isEmpty()) {
            err << error << '\n';
            return 1;
        }
        QTextStream(stdout) << QDir::toNativeSeparators(created) << '\n';
        return 0;
    }

    fhmi::MainWindow window;
    window.show();
    if (!projects.isEmpty()) {
        window.openProject(projects.first());
    }
    return app.exec();
}
