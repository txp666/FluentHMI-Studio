#pragma once

#include "core/project.h"

#include <FluentQtWidgets/Theme.h>

#include <QtCore/QString>
#include <QtGui/QColor>

#include <optional>

namespace fhmi {

// Turns a project into a standalone application without a compiler: a copy of the
// prebuilt FluentHMI Runtime carrying the project (and the images it uses), renamed
// after the project's app name, that starts with the studio's theme mode and colour.
class ExportService
{
  public:
    struct Appearance
    {
        FluentQt::Theme themeMode = FluentQt::Theme::Auto;
        QColor themeColor;
        QString localeName;
    };

    // The runtime this studio ships with: a bundle on macOS, an executable elsewhere.
    static QString runtimePath();

    // Reads the studio's current appearance settings on the GUI thread so export can run in a worker thread.
    static Appearance currentAppearance();

    // The platform-specific target path (<outputDir>/<safeName>(.app|.tar.gz|)).
    static QString targetPath(const Project &project, const QString &outputDir);

    // Creates <outputDir>/<appName>(.app), or on Linux <outputDir>/<appName>.tar.gz holding
    // <appName>/ with a start.sh. Returns the created path, or an empty string with *error set.
    static QString exportApplication(const Project &project, const QString &outputDir, QString *error,
                                     bool overwrite = false,
                                     const std::optional<Appearance> &appearance = std::nullopt);

    // A name every file system accepts, from the project's app name.
    static QString safeName(const QString &appName);
};

} // namespace fhmi
