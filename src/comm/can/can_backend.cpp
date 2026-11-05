#include "comm/can/can_backend.h"

#include "comm/can/pcan_backend.h"
#include "comm/can/slcan_backend.h"
#include "comm/can/zlg_backend.h"
#ifdef Q_OS_LINUX
#include "comm/can/socketcan_backend.h"
#endif

#include <QtCore/QCoreApplication>

namespace fhmi {

namespace CanAdapter {

CanAdapterCaps caps(const QString &adapter, const QString &zlgModel)
{
    CanAdapterCaps caps;
    if (adapter == Pcan) {
#if !defined(Q_OS_WIN) && !defined(Q_OS_MACOS)
        caps.supported = false;
#endif
        return caps;
    }
    if (adapter == Zlg) {
        return ZlgBackend::caps(zlgModel);
    }
    if (adapter == SocketCan) {
#ifndef Q_OS_LINUX
        caps.supported = false;
#endif
        caps.bitrate = false;
        caps.samplePoint = false;
        caps.dataSamplePoint = false;
        caps.listenOnly = false;
        caps.customTiming = false;
        caps.fixedTimingReason = QCoreApplication::translate("fhmi", "SocketCAN 的波特率由系统设置（ip link）");
        return caps;
    }
    // slcan: S0–S8 only.
    caps.samplePoint = false;
    caps.fd = false;
    caps.dataSamplePoint = false;
    caps.customTiming = false;
    caps.fixedTimingReason = QCoreApplication::translate("fhmi", "采样点由适配器决定");
    return caps;
}

QList<CanChannel> channels(const QString &adapter, QString *error)
{
    if (adapter == Pcan) {
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
        return PcanBackend::channels(error);
#endif
    } else if (adapter == SocketCan) {
#ifdef Q_OS_LINUX
        return SocketCanBackend::channels();
#endif
    } else if (adapter == Slcan) {
        return SlcanBackend::channels();
    }
    return {};
}

QString bitrateText(quint32 bitrate)
{
    if (bitrate >= 1000000 && bitrate % 100000 == 0) {
        return QString::number(bitrate / 1e6) + QLatin1Char('M');
    }
    if (bitrate >= 1000) {
        return QString::number(bitrate / 1e3) + QLatin1Char('k');
    }
    return QString::number(bitrate);
}

QString socketCanCommand(const QString &interfaceName, const CanSettings &settings)
{
    QString command = QStringLiteral("sudo ip link set %1 up type can bitrate %2 sample-point %3")
                          .arg(interfaceName.isEmpty() ? QStringLiteral("can0") : interfaceName)
                          .arg(settings.bitrate)
                          .arg(settings.samplePoint);
    if (settings.fd) {
        command += QStringLiteral(" dbitrate %1 dsample-point %2 fd on")
                       .arg(settings.dataBitrate)
                       .arg(settings.dataSamplePoint);
    }
    return command;
}

} // namespace CanAdapter

std::unique_ptr<CanBackend> CanBackend::create(const QString &adapter, QString *error)
{
    const auto unsupported = [error](const QString &message) {
        if (error) {
            *error = message;
        }
        return nullptr;
    };
    if (adapter == CanAdapter::Pcan) {
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
        return std::make_unique<PcanBackend>();
#else
        return unsupported(QCoreApplication::translate("fhmi", "Linux 上请用 SocketCAN 连接 PEAK 适配器（内核自带 peak_usb 驱动）"));
#endif
    }
    if (adapter == CanAdapter::Zlg) {
#ifdef Q_OS_WIN
        return std::make_unique<ZlgBackend>();
#else
        return unsupported(QCoreApplication::translate("fhmi", "ZLG 适配器只支持 Windows"));
#endif
    }
    if (adapter == CanAdapter::SocketCan) {
#ifdef Q_OS_LINUX
        return std::make_unique<SocketCanBackend>();
#else
        return unsupported(QCoreApplication::translate("fhmi", "SocketCAN 只在 Linux 上可用"));
#endif
    }
    if (adapter == CanAdapter::Slcan) {
        return std::make_unique<SlcanBackend>();
    }
    return unsupported(QCoreApplication::translate("fhmi", "未知的 CAN 适配器: %1").arg(adapter));
}

} // namespace fhmi
