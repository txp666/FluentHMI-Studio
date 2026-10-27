#pragma once

#include <QtCore/QString>

namespace fhmi {

// The line in docs/uidp-format.md and docs/uidp-widgets.md after which the generated part starts.
inline const QString GeneratedMarker =
    QStringLiteral("<!-- 以下由 uidp_reference（tools/uidp_reference.cpp）从控件注册表生成，不要手改 -->");

// The widget list that ends docs/uidp-format.md as Markdown: one line per category, each widget type with its
// name and default size.
QString widgetIndex();

// The widget reference of docs/uidp-widgets.md as Markdown: every widget type with its default size, the
// property a binding writes and each property's default, range and options, strings written as a .uidp
// writes them. tools/uidp_reference.cpp puts both into the documents and tst_docs checks they are current.
QString widgetReference();

// `document` with everything after the marker line replaced by `generated`, or an empty string when the
// marker is missing.
QString withGenerated(const QString &document, const QString &generated);

} // namespace fhmi
