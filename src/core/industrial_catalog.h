#pragma once

#include "core/widget_meta.h"
#include <QtCore/QJsonArray>

namespace fhmi {
// Shared offline catalog and SVG templates are embedded in every player and designer build.
const QJsonArray &industrialCatalog();
QJsonObject industrialDefinition(const QString &type);
QList<WidgetMeta> industrialWidgets();
bool isIndustrialCategory(WidgetCategory category);

struct IndustrialCapabilities
{
    bool structuredInput = false;
    bool scale = false;
    bool numericReadout = false;
    QStringList states; // Localized names indexed by the received state code.
    int sampleMaximum = -1; // Non-negative for a bounded unsigned integer sample.
    double sampleDefault = 0;
    QString inputExample; // A complete JSON message for composite bindings.
};

IndustrialCapabilities industrialCapabilities(const QString &type, QStringView feedbackMode = {});
bool industrialLimitsValid(double low, double high);
} // namespace fhmi
