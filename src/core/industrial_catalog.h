#pragma once

#include "core/widget_meta.h"
#include <QtCore/QJsonArray>

namespace fhmi {
// Shared offline catalog and SVG templates are embedded in every player and designer build.
const QJsonArray &industrialCatalog();
QJsonObject industrialDefinition(const QString &type);
QList<WidgetMeta> industrialWidgets();
bool isIndustrialCategory(WidgetCategory category);
} // namespace fhmi
