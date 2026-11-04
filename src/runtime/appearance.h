#pragma once

#include <FluentQtWidgets/Theme.h>

#include <QtCore/QString>
#include <QtGui/QColor>

namespace fhmi {

// An exported application looks like the studio that exported it: the theme mode and colour set
// at export time go into appearance.json beside the exported project (the .uidp stays as it is),
// and the runtime applies them at start-up over whatever it has saved itself.
inline constexpr char kAppearanceFile[] = "appearance.json";

bool writeAppearance(const QString &dir, FluentQt::Theme mode, const QColor &color, QString *error);

// Applies the appearance.json in dir, if there is one, and returns whether it did.
bool applyAppearance(const QString &dir);

} // namespace fhmi
