#pragma once

#include "core/can_frame.h"

#include <QtCore/QList>
#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QStringList>

#include <atomic>
#include <memory>

namespace fhmi {

// The CAN controller's error state, as the adapter reports it.
enum class CanBusState
{
    Normal,
    Warning, // an error counter reached 96
    Passive, // error passive: a counter above 127
    BusOff,
};

// Shared by the session and the physical backend. A bus-off transition invalidates work
// already queued above the adapter even if the controller recovers before that work runs.
struct CanTransmitState
{
    std::atomic<bool> stopping{false};
    std::atomic<bool> busOff{false};
    std::atomic<quint64> generation{0};

    void observe(CanBusState state)
    {
        const bool off = state == CanBusState::BusOff;
        const bool wasOff = busOff.exchange(off);
        if (off && !wasOff)
            ++generation;
    }
};

// What a CAN card connects with: the card's fields at the moment 连接 is clicked.
struct CanSettings
{
    QString adapter;      // CanAdapter::Pcan, Zlg, SocketCan or Slcan
    QString channel;      // PCAN: the channel handle ("0x51"); SocketCAN: the interface; slcan: the serial port
    QString zlgModel;     // ZLG only
    int deviceIndex = 0;  // ZLG only
    int channelIndex = 0; // ZLG only
    quint32 bitrate = 500000;
    double samplePoint = 0.875;
    bool fd = false;
    quint32 dataBitrate = 2000000;
    double dataSamplePoint = 0.75;
    bool brs = true;
    QString customTiming; // PCAN: BRP,TSEG1,TSEG2,SJW[;...]; ZLG: SJA BTR/BRP, or model-specific ZCANPRO result
    bool autoRecover = false; // Opt-in; old projects retain manual recovery.
    bool listenOnly = false;
    bool termination = false;
    QString txInitialState; // explicit target=feedback mappings for first signal-based transmission only
    int txInitialMaxAgeMs = 2000;
    bool standardZlgTiming = false; // classic ZLG vendor presets instead of computed sample point
};

// A channel the card offers for an adapter: the value stored in CanSettings::channel, and what the user sees.
struct CanChannel
{
    QString id;
    QString label;
};

// Capabilities used by the designer, connection card and effective connection settings.
struct CanAdapterCaps
{
    bool autoRecover = false;
    bool supported = true; // built into this program on this OS
    bool channelList = true;
    bool bitrate = true;
    bool samplePoint = true;
    bool fd = true;
    bool dataSamplePoint = true;
    bool listenOnly = true;
    bool termination = false;
    bool customTiming = true;
    bool fdEnabled(bool requested) const { return fd && requested; }
};

namespace CanAdapter {
inline const QString Pcan = QStringLiteral("PCAN");
inline const QString Zlg = QStringLiteral("ZLG");
inline const QString SocketCan = QStringLiteral("SocketCAN");
inline const QString Slcan = QStringLiteral("slcan");

CanAdapterCaps caps(const QString &adapter, const QString &zlgModel = QString());
struct TimingOverrides
{
    bool nominal = false;
    bool data = false;
};
TimingOverrides timingOverrides(const QString &adapter, const QString &customTiming);
// The channels to choose from now; *error says why there are none (a library that is not installed).
QList<CanChannel> channels(const QString &adapter, QString *error = nullptr);
// "500k", "1M": a bitrate as the card writes it.
QString bitrateText(quint32 bitrate);
// SocketCAN: the ip command that sets the interface's bitrate, which the card cannot.
QString socketCanCommand(const QString &interfaceName, const CanSettings &settings);
} // namespace CanAdapter

// One adapter channel. Received frames arrive on the owning thread in batches (one delivery per event-loop pass at
// most), in order; receivedAtMs is stamped when read, before any queued delivery or status query.
// CanSession owns production backends on a worker thread, including blocking native open() and close().
class CanBackend : public QObject
{
    Q_OBJECT

  public:
    using QObject::QObject;

    // Creates the backend for an adapter; nullptr (with *error) when it is not built for this OS.
    static std::unique_ptr<CanBackend> create(const QString &adapter, QString *error = nullptr);

    // Opens and starts the channel; false with an actionable message in *error.
    virtual bool open(const CanSettings &settings, QString *error) = 0;
    virtual void close() = 0;
    virtual bool isOpen() const = 0;
    // False reports an immediate rejection. After a queue accepts a frame, framesSent() or
    // frameSendFailed() reports the adapter result. A disconnect discards pending work.
    virtual bool send(const CanFrame &frame, QString *error) = 0;
    // "PCAN-USB FD 0x51 @ 500k/2M"
    virtual QString description() const = 0;

    const std::shared_ptr<CanTransmitState> &transmitState() const { return m_transmitState; }
    void setTransmitState(std::shared_ptr<CanTransmitState> state) { m_transmitState = std::move(state); }
    static QString busOffSendError();

  signals:
    void framesReceived(const QList<fhmi::CanFrame> &frames);
    void framesSent(const QList<fhmi::CanFrame> &frames);
    void busStateChanged(fhmi::CanBusState state);
    void frameSendFailed(const fhmi::CanFrame &frame, const QString &message);
    // An adapter warning or failure without an associated submission.
    void sendFailed(const QString &message);
    // The adapter stopped working (unplugged); the link closes.
    void failed(const QString &message);

  private:
    std::shared_ptr<CanTransmitState> m_transmitState = std::make_shared<CanTransmitState>();
};

} // namespace fhmi

Q_DECLARE_METATYPE(fhmi::CanBusState)
