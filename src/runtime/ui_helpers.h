#pragma once

class QAbstractButton;
class QWidget;

namespace fhmi {

// Keeps an icon button square and exactly as tall as the field beside it (a refresh button
// next to a combo box, a browse button next to a path field), following the field's real
// height rather than a guess made before styles apply. Also drops Fluent's tool-button
// padding and 32 px minimum, which would push the icon off centre at that size.
void keepSquareBeside(QAbstractButton *button, QWidget *field);

} // namespace fhmi
