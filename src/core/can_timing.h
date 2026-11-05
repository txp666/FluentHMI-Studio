#pragma once

#include <QtCore/QString>

#include <optional>

namespace fhmi {

// The register ranges of a CAN controller's bit timing, from the adapter's vendor documentation.
struct CanTimingLimits
{
    int brpMin = 1;
    int brpMax = 1;
    int tseg1Min = 1;
    int tseg1Max = 1;
    int tseg2Min = 1;
    int tseg2Max = 1;
    int sjwMax = 1;
    int minTq = 8; // fewer time quanta per bit are not considered
};

// One phase's bit timing in time quanta; brp 0 means none.
struct CanTiming
{
    int brp = 0;
    int tseg1 = 0;
    int tseg2 = 0;
    int sjw = 0;

    bool isValid() const { return brp > 0; }
    int tq() const { return 1 + tseg1 + tseg2; }
    double samplePoint() const { return tq() > 0 ? double(1 + tseg1) / tq() : 0; }
    quint32 bitrate(quint32 clockHz) const { return isValid() ? clockHz / (quint32(brp) * quint32(tq())) : 0; }
};

namespace CanBitTiming {

// SJA1000 BTR0/BTR1 (tq = 2 × BRP / f_osc, so the clock here is half the crystal: 8 MHz for 16 MHz).
inline constexpr CanTimingLimits Sja1000{1, 64, 1, 16, 1, 8, 4, 8};
// PEAK PCAN-Basic CAN FD bit rate strings (PCAN-Basic documentation, "FD Bit rate Parameter Definitions"):
// nom_brp 1..1024, nom_tseg1 1..256, nom_tseg2 1..128, nom_sjw 1..128; data_brp 1..1024, data_tseg1 1..32,
// data_tseg2 1..16, data_sjw 1..16.
inline constexpr CanTimingLimits PcanFdNominal{1, 1024, 1, 256, 1, 128, 128, 8};
inline constexpr CanTimingLimits PcanFdData{1, 1024, 1, 32, 1, 16, 16, 5};
// Preferred time quanta per bit: the nominal (arbitration) phase, the data phase.
constexpr int PreferredNominalTq = 16;
constexpr int PreferredDataTq = 8;

// The timing for a bitrate and sample point: total TQ = 1 + TSEG1 + TSEG2 = clock / (BRP × bitrate), exactly;
// sample point = (1 + TSEG1) / TQ. Among the BRPs that give the bitrate exactly and the TSEG1 that put the sample
// point at floor or ceil of samplePoint × TQ, within the limits, the best has the smallest sample-point error,
// then TQ closest to preferredTq, then the smaller BRP, then the later sample point. SJW = min(TSEG2, sjw limit).
// Invalid when no BRP gives the bitrate.
CanTiming compute(quint32 clockHz, quint32 bitrate, double samplePoint, const CanTimingLimits &limits, int preferredTq);

// SJA1000 BTR0 << 8 | BTR1: BTR0 = (SJW−1)<<6 | (BRP−1), BTR1 = (TSEG2−1)<<4 | (TSEG1−1), one sample per bit.
quint16 sja1000Btr(const CanTiming &timing);

// Whether a timing (from customTiming) lies within the limits; *error says which value does not.
bool check(const CanTiming &timing, const CanTimingLimits &limits, QString *error);

// The card's customTiming: "BRP,TSEG1,TSEG2,SJW" for the nominal phase, optionally ";BRP,TSEG1,TSEG2,SJW" for the
// data phase, in the adapter's own clock.
struct Custom
{
    CanTiming nominal;
    std::optional<CanTiming> data;
};
std::optional<Custom> parseCustom(const QString &text, QString *error = nullptr);

// "500k", "1M" and the like as bits per second; 0 when it is none. "87.5%" as 0.875; 0 when it is none.
quint32 bitrateValue(const QString &text);
double samplePointValue(const QString &text);

} // namespace CanBitTiming

} // namespace fhmi
