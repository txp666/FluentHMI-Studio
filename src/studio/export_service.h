#pragma once

#include "core/project.h"

#include <QtCore/QString>

namespace fhmi {

// Turns a project into a standalone application without a compiler: a copy of the
// prebuilt FluentHMI Runtime carrying the project (and the images it uses), renamed
// after the project's app name.
class ExportService
{
  public:
    // The runtime this studio ships with: a bundle on macOS, an executable elsewhere.
    static QString runtimePath();

    // Creates <outputDir>/<appName>(.app). Returns the created path, or an empty
    // string with *error set.
    static QString exportApplication(const Project &project, const QString &outputDir, QString *error);

    // A name every file system accepts, from the project's app name.
    static QString safeName(const QString &appName);
};

} // namespace fhmi
