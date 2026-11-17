#pragma once

#include "comm/can/can_backend.h"
#include "comm/can/zlg_api.h"

#include <optional>

namespace fhmi::zlg {

struct TimingProperty
{
    QByteArray name; // relative to the channel, e.g. baud_rate_custom
    QString value;
};

// Prepared before loading the driver. SJA1000 cards take BTR0/BTR1 directly, including compatible drivers
// without SetValue support. Other models take the calculator's own, model-specific string before InitCAN.
struct ChannelTiming
{
    std::optional<quint16> btr;
    QList<TimingProperty> properties;
    QString description;
};

std::optional<ChannelTiming> channelTiming(const Model &model, const CanSettings &settings, QString *error = nullptr);
QString customTimingHint(const Model &model);
QString customTimingPlaceholder(const Model &model);

// The same configuration path is used with the real setter and in driver-call regression tests. A rejected
// custom value stops opening the channel; a subsequent standard-rate call must never overwrite it.
template <class Setter>
bool applyChannelTiming(const ChannelTiming &timing, ZCAN_CHANNEL_INIT_CONFIG &config, Setter &&setValue)
{
    if (timing.btr) {
        config.config.can.timing0 = BYTE(*timing.btr >> 8);
        config.config.can.timing1 = BYTE(*timing.btr);
    }
    for (const TimingProperty &property : timing.properties) {
        if (!setValue(property.name.constData(), property.value)) {
            return false;
        }
    }
    return true;
}

} // namespace fhmi::zlg
