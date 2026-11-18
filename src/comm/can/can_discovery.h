#pragma once
#include <QtCore/QStringList>
namespace fhmi::CanAdapter {
// Resolve driver-provided model descriptions; never guess a compatible card from a shared USB chip ID.
QStringList zlgModelsFromDescriptions(const QStringList &descriptions);
QStringList detectedZlgModels(QString *error = nullptr);
} // namespace fhmi::CanAdapter
