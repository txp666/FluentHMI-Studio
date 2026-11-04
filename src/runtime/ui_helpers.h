#pragma once

#include <QtGui/QIcon>

class QAbstractButton;
class QWidget;

namespace fhmi {

// Keeps an icon button square and exactly as tall as the field beside it (a refresh button
// next to a combo box, a browse button next to a path field), following the field's real
// height rather than a guess made before styles apply. Also drops Fluent's tool-button
// padding and 32 px minimum, which would push the icon off centre at that size.
void keepSquareBeside(QAbstractButton *button, QWidget *field);

// Shows the widget's tooltip as a Fluent one rather than the native one. Call it once, where the widget is
// created: the text is read from toolTip() when the pointer enters, so a later setToolTip() changes it and an
// empty one shows nothing. Sets the tooltip too when text is given.
void fluentToolTip(QWidget *widget, const QString &text = QString());

// An image file as an icon, every size an .ico holds included, or a null icon when it cannot be read.
// QIcon(path) decodes a JPEG at the size asked for, squashing a photo that is not square.
QIcon iconFromFile(const QString &path);

} // namespace fhmi
