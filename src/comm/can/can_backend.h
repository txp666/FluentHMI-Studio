#pragma once

#include "core/can_frame.h"

#include <QtCore/QList>
#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QStringList>

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
    QString customTiming; // "BRP,TSEG1,TSEG2,SJW[;BRP,TSEG1,TSEG2,SJW]" in the adapter's clock, or empty
    bool listenOnly = false;
    bool termination = false;
};

// A channel the card offers for an adapter: the value stored in CanSettings::channel, and what the user sees.
struct CanChannel
{
    QString id;
    QString label;
};

// Which of the card's fields an adapter (and ZLG model) can set. A field it cannot set is disabled, with
// `reason` as its tooltip.
struct CanAdapterCaps
{
    bool supported = true; // built into this program on this OS
    bool channelList = true;
    bool bitrate = true;
    bool samplePoint = true;
    bool fd = true;
    bool dataSamplePoint = true;
    bool listenOnly = true;
    bool termination = false;
    bool customTiming = true;
    QString fixedTimingReason; // why bitrate/sample point fields are disabled
};

namespace CanAdapter {
inline const QString Pcan = QStringLiteral("PCAN");
inline const QString Zlg = QStringLiteral("ZLG");
inline const QString SocketCan = QStringLiteral("SocketCAN");
inline const QString Slcan = QStringLiteral("slcan");

CanAdapterCaps caps(const QString &adapter, const QString &zlgModel = QString());
// The channels to choose from now; *error says why there are none (a library that is not installed).
QList<CanChannel> channels(const QString &adapter, QString *error = nullptr);
// "500k", "1M": a bitrate as the card writes it.
QString bitrateText(quint32 bitrate);
// SocketCAN: the ip command that sets the interface's bitrate, which the card cannot.
QString socketCanCommand(const QString &interfaceName, const CanSettings &settings);
} // namespace CanAdapter

// One adapter channel. Received frames arrive on the GUI thread in batches (one delivery per event-loop pass at
// most), in order; the GUI thread never waits on the adapter except in open() and close().
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
    // Queues a frame for sending; false (with *error) when it cannot go out at all. framesSent() reports it once
    // the adapter took it.
    virtual bool send(const CanFrame &frame, QString *error) = 0;
    // "PCAN-USB FD 0x51 @ 500k/2M"
    virtual QString description() const = 0;

  signals:
    void framesReceived(const QList<fhmi::CanFrame> &frames);
    void framesSent(const QList<fhmi::CanFrame> &frames);
    void busStateChanged(fhmi::CanBusState state);
    // A frame the adapter would not take; the channel stays open.
    void sendFailed(const QString &message);
    // The adapter stopped working (unplugged); the link closes.
    void failed(const QString &message);
};

} // namespace fhmi

Q_DECLARE_METATYPE(fhmi::CanBusState)
