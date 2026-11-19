#include "comm/can/zlg_timing.h"

#include "core/can_timing.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QRegularExpression>

#include <algorithm>
#include <cmath>
#include <tuple>

namespace fhmi::zlg {
namespace {

constexpr quint32 kSjaClock = 8000000; // 16 MHz crystal / 2, per SJA1000 BTR0/BTR1

CanTiming decodeBtr(quint16 value)
{
    const int btr0 = value >> 8;
    const int btr1 = value & 0xff;
    return {(btr0 & 0x3f) + 1, (btr1 & 0xf) + 1, ((btr1 >> 4) & 7) + 1, (btr0 >> 6) + 1};
}

std::optional<quint16> standardBtr(quint32 bitrate)
{
    // Keep the vendor timings used by the original USBCAN-I/II backend.
    static constexpr struct { quint32 rate; quint16 btr; } timings[] = {
        {10000, 0x311c}, {20000, 0x181c}, {50000, 0x091c}, {100000, 0x041c}, {125000, 0x031c},
        {250000, 0x011c}, {500000, 0x001c}, {800000, 0x0016}, {1000000, 0x0014},
    };
    const auto preset = std::find_if(std::begin(timings), std::end(timings),
                                     [bitrate](const auto &item) { return item.rate == bitrate; });
    if (preset == std::end(timings))
        return {};
    return preset->btr;
}

QString tr(const char *text) { return QCoreApplication::translate("ZlgTiming", text); }

QString percent(double samplePoint)
{
    return QString::number(samplePoint * 100, 'f', 2)
               .remove(QRegularExpression(QStringLiteral("0+$")))
               .remove(QRegularExpression(QStringLiteral("\\.$"))) +
           QLatin1Char('%');
}

template <size_t Size> bool contains(const quint32 (&rates)[Size], quint32 value)
{
    return std::find(std::begin(rates), std::end(rates), value) != std::end(rates);
}

// Only the calculator's envelope is interpreted for non-SJA cards. Its registers and clock are private to the
// selected model; passing the original ASCII string lets that model's vendor driver validate them.
bool calculatorString(const QString &text, bool fd, int parameters)
{
    static const QRegularExpression expression(
        QStringLiteral("^([0-9]+(?:\\.[0-9]+)?\\s*[kKmM]?[bB][pP][sS]\\s*\\(\\s*"
                       "[0-9]+(?:\\.[0-9]+)?%\\s*\\)\\s*,\\s*)"
                       "(?:([0-9]+(?:\\.[0-9]+)?\\s*[kKmM]?[bB][pP][sS]\\s*\\(\\s*"
                       "[0-9]+(?:\\.[0-9]+)?%\\s*\\)\\s*,\\s*))?"
                       "\\(\\s*((?:0[xX])?[0-9a-fA-F]{1,8}(?:\\s*,\\s*(?:0[xX])?[0-9a-fA-F]{1,8})*)\\s*\\)$"));
    const auto match = expression.match(text);
    return match.hasMatch() && (!match.captured(2).isEmpty()) == fd &&
           match.captured(3).split(QLatin1Char(',')).size() == parameters;
}

} // namespace

QList<SjaTimingCandidate> sjaTimingCandidates(double bitrate, double samplePoint, int sjw, int maxErrorPpm)
{
    QList<SjaTimingCandidate> results;
    if (!std::isfinite(bitrate) || bitrate < 1 || bitrate > 1000000 || !std::isfinite(samplePoint) ||
        samplePoint <= 0 || samplePoint >= 1 || sjw < 1 || sjw > 4 || maxErrorPpm < 0 || maxErrorPpm > 10000) {
        return results;
    }
    const auto &limits = CanBitTiming::Sja1000;
    for (int brp = limits.brpMin; brp <= limits.brpMax; ++brp) {
        for (int tseg1 = limits.tseg1Min; tseg1 <= limits.tseg1Max; ++tseg1) {
            for (int tseg2 = std::max(sjw, limits.tseg2Min); tseg2 <= limits.tseg2Max; ++tseg2) {
                const CanTiming timing{brp, tseg1, tseg2, sjw};
                if (timing.tq() < limits.minTq) {
                    continue;
                }
                const double actual = double(kSjaClock) / (brp * timing.tq());
                const double errorPpm = std::abs(actual - bitrate) / bitrate * 1000000;
                if (actual <= 1000000 && errorPpm <= maxErrorPpm + 1e-9) {
                    results.append({timing, CanBitTiming::sja1000Btr(timing), actual, errorPpm});
                }
            }
        }
    }
    const auto rank = [samplePoint](const SjaTimingCandidate &candidate) {
        return std::make_tuple(qint64(std::llround(candidate.bitrateErrorPpm * 1000000)),
                               qint64(std::llround(std::abs(candidate.timing.samplePoint() - samplePoint) * 1e9)),
                               std::abs(candidate.timing.tq() - CanBitTiming::PreferredNominalTq), candidate.timing.brp,
                               -candidate.timing.samplePoint());
    };
    std::sort(results.begin(), results.end(), [&](const auto &a, const auto &b) { return rank(a) < rank(b); });
    return results;
}

QString customTimingPlaceholder(const Model &model)
{
    if (model.family == Family::Sja1000) {
        return QStringLiteral("00,1C / 1,13,2,1");
    }
    if (model.deviceType == ZCAN_USBCAN_4E_U) {
        return QStringLiteral("200000");
    }
    return tr("粘贴此型号的 ZCANPRO 计算结果");
}

std::optional<ChannelTiming> channelTiming(const Model &model, const CanSettings &settings, QString *error)
{
    const auto fail = [error](const QString &text) -> std::optional<ChannelTiming> {
        if (error) {
            *error = text;
        }
        return std::nullopt;
    };
    if (settings.fd && model.family != Family::CanFd) {
        return fail(tr("%1 不支持 CAN FD").arg(settings.zlgModel));
    }
    const QString custom = settings.customTiming.trimmed();
    ChannelTiming result;
    if (model.family == Family::Sja1000) {
        CanTiming timing;
        quint16 btr = 0;
        if (custom.isEmpty() && settings.standardZlgTiming) {
            const auto preset = standardBtr(settings.bitrate);
            if (!preset)
                return fail(tr("%1 不支持波特率 %2").arg(settings.zlgModel, CanAdapter::bitrateText(settings.bitrate)));
            btr = *preset;
            timing = decodeBtr(btr);
        } else if (custom.isEmpty()) {
            if (!std::isfinite(settings.samplePoint) || settings.samplePoint <= 0 || settings.samplePoint >= 1 ||
                settings.bitrate == 0 || settings.bitrate > 1000000) {
                return fail(tr("ZLG 波特率或采样点无效"));
            }
            timing = CanBitTiming::compute(kSjaClock, settings.bitrate, settings.samplePoint, CanBitTiming::Sja1000,
                                           CanBitTiming::PreferredNominalTq);
            if (!timing.isValid()) {
                return fail(tr("此适配器无法实现波特率 %1").arg(CanAdapter::bitrateText(settings.bitrate)));
            }
            timing.sjw = 1; // preserve ZLG's standard 500k/87.5% value 00,1C
            btr = CanBitTiming::sja1000Btr(timing);
        } else {
            // Accept both the raw two-byte value and the complete classic ZCANPRO calculator result.
            static const QRegularExpression raw(QStringLiteral("^\\s*((?:0[xX])?[0-9a-fA-F]{1,2})"
                                                               "\\s*,\\s*((?:0[xX])?[0-9a-fA-F]{1,2})\\s*$"));
            QString bytes = custom;
            if (calculatorString(custom, false, 2)) {
                bytes = custom.mid(custom.lastIndexOf(QLatin1Char('(')));
            }
            if (bytes.startsWith(QLatin1Char('(')) && bytes.endsWith(QLatin1Char(')'))) {
                bytes = bytes.mid(1, bytes.size() - 2);
            }
            const auto match = raw.match(bytes);
            if (match.hasMatch()) {
                const int btr0 = match.captured(1).toInt(nullptr, 16);
                const int btr1 = match.captured(2).toInt(nullptr, 16);
                btr = quint16((btr0 << 8) | btr1);
                timing = decodeBtr(btr);
            } else {
                const auto parsed = CanBitTiming::parseCustom(custom, error);
                if (!parsed) {
                    return fail(tr("ZLG 位时序格式无效：请填 BTR0,BTR1、BRP,TSEG1,TSEG2,SJW 或 ZCANPRO 结果"));
                }
                if (parsed->data) {
                    return fail(tr("此 ZLG 型号只有经典 CAN 位时序，不能填写数据域"));
                }
                timing = parsed->nominal;
                if (!CanBitTiming::check(timing, CanBitTiming::Sja1000, error)) {
                    return std::nullopt;
                }
                btr = CanBitTiming::sja1000Btr(timing);
            }
            if (!CanBitTiming::check(timing, CanBitTiming::Sja1000, error)) {
                return std::nullopt;
            }
            if (quint64(timing.brp) * timing.tq() < 8) {
                return fail(tr("ZLG 经典 CAN 自定义波特率不能超过 1M"));
            }
        }
        result.btr = btr;
        result.description = QStringLiteral("%1 · %2 · BTR %3,%4")
                                 .arg(CanAdapter::bitrateText(timing.bitrate(kSjaClock)), percent(timing.samplePoint()),
                                      QStringLiteral("%1").arg(btr >> 8, 2, 16, QLatin1Char('0')).toUpper(),
                                      QStringLiteral("%1").arg(btr & 0xFF, 2, 16, QLatin1Char('0')).toUpper());
        return result;
    }
    if (!custom.isEmpty()) {
        if (custom.size() > 512 || std::any_of(custom.begin(), custom.end(),
                                               [](QChar ch) { return ch.unicode() < 32 || ch.unicode() > 126; })) {
            return fail(tr("ZLG 自定义位时序格式无效，请粘贴此型号的 ZCANPRO 完整计算结果"));
        }
        if (model.deviceType == ZCAN_USBCAN_4E_U) {
            bool ok = false;
            const quint32 rate = custom.toUInt(&ok, 10);
            if (!ok || rate == 0 || rate > 1000000) {
                return fail(tr("USBCAN-4E-U 自定义位时序须为 1–1000000 的十进制波特率（bps）"));
            }
            result.description = CanAdapter::bitrateText(rate);
        } else {
            const int parameters = model.family == Family::CanFd ? (model.deviceType == ZCAN_USBCANFD_800U ? 8 : 3)
                                                                 : (model.deviceType == ZCAN_USBCAN_8E_U ? 2 : 1);
            if (!calculatorString(custom, model.family == Family::CanFd, parameters)) {
                return fail(
                    tr("ZLG 自定义位时序格式与 %1 不符，请粘贴此型号的 ZCANPRO 完整计算结果").arg(settings.zlgModel));
            }
            result.description = custom.left(custom.lastIndexOf(QLatin1Char('('))).trimmed();
            if (result.description.endsWith(QLatin1Char(','))) {
                result.description.chop(1);
            }
        }
        result.properties.append({QByteArrayLiteral("baud_rate_custom"), custom});
        return result;
    }
    if (model.family == Family::CanFd) {
        if (!contains(kFdArbitrationRates, settings.bitrate)) {
            return fail(tr("%1 不支持波特率 %2").arg(settings.zlgModel, CanAdapter::bitrateText(settings.bitrate)));
        }
        if (settings.fd && !contains(kFdDataRates, settings.dataBitrate)) {
            return fail(
                tr("%1 不支持数据域波特率 %2").arg(settings.zlgModel, CanAdapter::bitrateText(settings.dataBitrate)));
        }
        result.properties = {{QByteArrayLiteral("canfd_abit_baud_rate"), QString::number(settings.bitrate)},
                             {QByteArrayLiteral("canfd_dbit_baud_rate"), QString::number(settings.dataBitrate)}};
    } else {
        if (!contains(kClassicRates, settings.bitrate)) {
            return fail(tr("%1 不支持波特率 %2").arg(settings.zlgModel, CanAdapter::bitrateText(settings.bitrate)));
        }
        result.properties = {{QByteArrayLiteral("baud_rate"), QString::number(settings.bitrate)}};
    }
    result.description = CanAdapter::bitrateText(settings.bitrate);
    if (settings.fd) {
        result.description += QLatin1Char('/') + CanAdapter::bitrateText(settings.dataBitrate);
    }
    return result;
}

} // namespace fhmi::zlg
