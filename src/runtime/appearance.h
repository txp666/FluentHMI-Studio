#pragma once

#include <FluentQtWidgets/Theme.h>

#include <QtCore/QString>
#include <QtGui/QColor>

class QCoreApplication;

namespace fhmi {

// An exported application looks like the studio that exported it: the theme mode and colour and the
// language setting at export time go into appearance.json beside the exported project (the .uidp stays as it
// is), and the runtime applies them at start-up over whatever it has saved itself.
inline constexpr char kAppearanceFile[] = "appearance.json";

bool writeAppearance(const QString &dir, FluentQt::Theme mode, const QColor &color, const QString &language,
                     QString *error);

// Applies the theme of the appearance.json in dir, if there is one, and returns whether it did.
bool applyAppearance(const QString &dir);

// The language setting ("Auto", "zh_CN", "en_US") of the appearance.json in dir, or an empty string when it has
// none. Read before any widget exists, unlike the theme.
QString appearanceLanguage(const QString &dir);

// Installs the language a setting stands for, before the first widget is made: English installs the
// translation table, Simplified Chinese FluentQtWidgets' own Chinese texts (its sources are English).
// Returns the resolved language.
QString installLanguage(QCoreApplication *app, const QString &setting);

} // namespace fhmi
