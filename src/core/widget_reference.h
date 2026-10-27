#pragma once

#include <QtCore/QString>

namespace fhmi {

// The line in docs/uidp-format.md after which the generated widget reference starts.
inline const QString WidgetReferenceMarker =
    QStringLiteral("<!-- 以下由 uidp_reference（tools/uidp_reference.cpp）从控件注册表生成，不要手改 -->");

// The widget reference of docs/uidp-format.md as Markdown: every widget type with its default size, the
// property a binding writes and each property's default, range and options, strings written as a .uidp
// writes them. tools/uidp_reference.cpp puts it into the document and tst_docs checks it is current.
QString widgetReference();

// `document` with everything after the marker line replaced by the current reference, or an empty
// string when the marker is missing.
QString withWidgetReference(const QString &document);

} // namespace fhmi
