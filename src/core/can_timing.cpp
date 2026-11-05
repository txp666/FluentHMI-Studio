#include "core/can_timing.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QStringList>

#include <cmath>
#include <tuple>

namespace fhmi::CanBitTiming {

CanTiming compute(quint32 clockHz, quint32 bitrate, double samplePoint, const CanTimingLimits &limits, int preferredTq)
{
    CanTiming best;
    // Compared as a whole: the error in units of 1e-9 (so equal errors computed two ways compare equal), the
    // distance from the preferred TQ, the BRP, and the negated sample point (a later one wins a tie).
    std::tuple<qint64, int, int, double> bestKey;
    if (bitrate == 0 || clockHz == 0) {
        return best;
    }
    const int fewest = std::max(limits.minTq, 1 + limits.tseg1Min + limits.tseg2Min);
    const int most = 1 + limits.tseg1Max + limits.tseg2Max;
    for (int brp = limits.brpMin; brp <= limits.brpMax; ++brp) {
        const quint64 divisor = quint64(brp) * bitrate;
        if (clockHz % divisor != 0) {
            continue;
        }
        const quint64 total = clockHz / divisor;
        if (total < quint64(fewest) || total > quint64(most)) {
            continue;
        }
        const int tq = int(total);
        const double ideal = samplePoint * tq;
        const int low = int(std::floor(ideal + 1e-9));
        const int high = int(std::ceil(ideal - 1e-9));
        // When both are the same candidate, the second one never beats the first.
        for (const int position : {low, high}) {
            const int tseg1 = position - 1;
            const int tseg2 = tq - 1 - tseg1;
            if (tseg1 < limits.tseg1Min || tseg1 > limits.tseg1Max || tseg2 < limits.tseg2Min ||
                tseg2 > limits.tseg2Max) {
                continue;
            }
            const double actual = double(1 + tseg1) / tq;
            const auto key = std::make_tuple(qint64(std::llround(std::fabs(actual - samplePoint) * 1e9)),
                                             std::abs(tq - preferredTq), brp, -actual);
            if (!best.isValid() || key < bestKey) {
                best = {brp, tseg1, tseg2, std::min(tseg2, limits.sjwMax)};
                bestKey = key;
            }
        }
    }
    return best;
}

quint16 sja1000Btr(const CanTiming &timing)
{
    const int btr0 = ((timing.sjw - 1) << 6) | (timing.brp - 1);
    const int btr1 = ((timing.tseg2 - 1) << 4) | (timing.tseg1 - 1);
    return quint16(((btr0 & 0xFF) << 8) | (btr1 & 0xFF));
}

bool check(const CanTiming &timing, const CanTimingLimits &limits, QString *error)
{
    const auto outside = [error](const char *name, int value, int lo, int hi) {
        if (value >= lo && value <= hi) {
            return false;
        }
        if (error) {
            *error = QCoreApplication::translate("fhmi", "自定义位时序 %1=%2 超出此适配器的范围 %3–%4")
                         .arg(QLatin1String(name))
                         .arg(value)
                         .arg(lo)
                         .arg(hi);
        }
        return true;
    };
    return !outside("BRP", timing.brp, limits.brpMin, limits.brpMax) &&
           !outside("TSEG1", timing.tseg1, limits.tseg1Min, limits.tseg1Max) &&
           !outside("TSEG2", timing.tseg2, limits.tseg2Min, limits.tseg2Max) &&
           !outside("SJW", timing.sjw, 1, std::min(limits.sjwMax, timing.tseg2));
}

std::optional<Custom> parseCustom(const QString &text, QString *error)
{
    const auto invalid = [error, &text]() {
        if (error) {
            *error = QCoreApplication::translate("fhmi", "自定义位时序格式错误: %1（应为 BRP,TSEG1,TSEG2,SJW）")
                         .arg(text.trimmed());
        }
        return std::nullopt;
    };
    const QStringList phases = text.split(QLatin1Char(';'));
    if (phases.isEmpty() || phases.size() > 2) {
        return invalid();
    }
    Custom custom;
    for (int i = 0; i < phases.size(); ++i) {
        const QStringList values = phases.at(i).split(QLatin1Char(','));
        if (values.size() != 4) {
            return invalid();
        }
        int numbers[4] = {};
        for (int v = 0; v < 4; ++v) {
            bool ok = false;
            numbers[v] = values.at(v).trimmed().toInt(&ok);
            if (!ok) {
                return invalid();
            }
        }
        const CanTiming timing{numbers[0], numbers[1], numbers[2], numbers[3]};
        if (i == 0) {
            custom.nominal = timing;
        } else {
            custom.data = timing;
        }
    }
    return custom;
}

quint32 bitrateValue(const QString &text)
{
    QString value = text.trimmed();
    double scale = 1;
    if (value.endsWith(QLatin1Char('k'), Qt::CaseInsensitive)) {
        scale = 1e3;
        value.chop(1);
    } else if (value.endsWith(QLatin1Char('M'), Qt::CaseInsensitive)) {
        scale = 1e6;
        value.chop(1);
    }
    bool ok = false;
    const double number = value.toDouble(&ok);
    return ok && number > 0 ? quint32(std::llround(number * scale)) : 0;
}

double samplePointValue(const QString &text)
{
    QString value = text.trimmed();
    if (value.endsWith(QLatin1Char('%'))) {
        value.chop(1);
    }
    bool ok = false;
    const double number = value.toDouble(&ok);
    return ok && number > 0 && number < 100 ? number / 100 : 0;
}

} // namespace fhmi::CanBitTiming
