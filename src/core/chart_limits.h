#pragma once

#include <QtCore/QtTypes>
#include <limits>

namespace fhmi {

using TimingMask = quint32;
inline constexpr int MaxTimingChannels = std::numeric_limits<TimingMask>::digits;
inline constexpr TimingMask AllTimingChannels = std::numeric_limits<TimingMask>::max();
inline constexpr int MaxRealtimeCurves = 8;

} // namespace fhmi
