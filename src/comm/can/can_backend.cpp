#include "comm/can/can_backend.h"

#include "comm/can/pcan_backend.h"
#include "comm/can/slcan_backend.h"
#include "comm/can/zlg_backend.h"
#include "core/can_timing.h"
#ifdef Q_OS_LINUX
#include "comm/can/socketcan_backend.h"
#endif

#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtCore/QPointer>

namespace fhmi {
namespace {

class LoopbackCanBackend final : public CanBackend
{
  public:
    bool open(const CanSettings &, QString *) override
    {
        m_open = true;
        return true;
    }
    void close() override { m_open = false; }
    bool isOpen() const override { return m_open; }
    QString description() const override
    {
        return QCoreApplication::translate("fhmi", "虚拟 CAN 回环");
    }
    bool send(const CanFrame &frame, QString *error) override
    {
        if (!m_open) {
            if (error)
                *error = QCoreApplication::translate("fhmi", "CAN 未连接");
            return false;
        }
        CanFrame sent = frame;
        sent.timestampMs = QDateTime::currentMSecsSinceEpoch();
        sent.receivedAtMs = Can::monotonicMs();
        CanFrame received = sent;
        received.submissionId = 0;
        const QPointer<LoopbackCanBackend> alive(this);
        emit framesSent({sent});
        if (alive && m_open)
            emit framesReceived({received});
        return true;
    }

  private:
    bool m_open = false;
};

} // namespace

namespace CanAdapter {

CanAdapterCaps caps(const QString &adapter, const QString &zlgModel)
{
    CanAdapterCaps caps;
    if (adapter == Pcan) {
        caps.autoRecover = true;
#if !defined(Q_OS_WIN) && !defined(Q_OS_MACOS)
        caps.supported = false;
#endif
        return caps;
    }
    if (adapter == Zlg) {
        return ZlgBackend::caps(zlgModel);
    }
    if (adapter == SocketCan || adapter == Virtual) {
        const bool systemChannel = adapter == SocketCan;
        caps.channelList = systemChannel;
        caps.autoRecover = systemChannel;
#ifndef Q_OS_LINUX
        caps.supported = !systemChannel;
#endif
        caps.bitrate = false;
        caps.samplePoint = false;
        caps.dataSamplePoint = false;
        caps.listenOnly = false;
        caps.customTiming = false;
        return caps;
    }
    // slcan: S0–S8 only.
    caps.samplePoint = false;
    caps.fd = false;
    caps.dataSamplePoint = false;
    caps.customTiming = false;
    return caps;
}

TimingOverrides timingOverrides(const QString &adapter, const QString &customTiming)
{
    if (customTiming.trimmed().isEmpty())
        return {};
    if (adapter == Zlg)
        return {true, true};
    if (adapter == Pcan) {
        const auto parsed = CanBitTiming::parseCustom(customTiming);
        return {true, parsed && parsed->data.has_value()};
    }
    return {};
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
    if (settings.autoRecover)
        command += QStringLiteral(" restart-ms 1000");
    return command;
}

} // namespace CanAdapter

QString CanBackend::busOffSendError()
{
    return tr("CAN 总线关闭，未发送报文");
}

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
    if (adapter == CanAdapter::Virtual)
        return std::make_unique<LoopbackCanBackend>();
    return unsupported(QCoreApplication::translate("fhmi", "未知的 CAN 适配器: %1").arg(adapter));
}

} // namespace fhmi
