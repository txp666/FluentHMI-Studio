#pragma once

#include "comm/can/can_polling_backend.h"
#include "comm/can/pcan_api.h"

namespace fhmi {

// PEAK adapters through PCAN-Basic (Windows: PCANBasic.dll from PEAK's driver) or MacCAN PCBUSB (macOS:
// libPCBUSB.dylib). A classic channel is initialised with SJA1000 BTR0/BTR1 (16 MHz crystal, so 8 MHz in the
// timing formula, as PEAK's own PCAN_BAUD_* codes are); a CAN FD channel with an FD bit rate string at an 80 MHz
// clock. The worker reads with CAN_Read / CAN_ReadFD; on Windows it sleeps on the receive event.
class PcanBackend : public CanPollingBackend
{
    Q_OBJECT

  public:
    using CanPollingBackend::CanPollingBackend;
    ~PcanBackend() override;

    // The attached channels (PCAN_ATTACHED_CHANNELS), or the PCAN-USB channels that answer
    // PCAN_CHANNEL_CONDITION when the library is too old for that.
    static QList<CanChannel> channels(QString *error);

    bool open(const CanSettings &settings, QString *error) override;
    void close() override;
    bool isOpen() const override { return m_open; }
    QString description() const override { return m_description; }

  protected:
    bool readFrames(QList<CanFrame> &out, QString *error) override;
    bool writeFrame(const CanFrame &frame, QString *error) override;
    CanBusState readBusState() override;
#ifdef Q_OS_WIN
    void waitForActivity(int ms) override;
    void wake() override;
#endif

  private:
    pcan::TPCANHandle m_handle = pcan::PCAN_NONEBUS;
    bool m_open = false;
    bool m_fd = false;
    QString m_description;
#ifdef Q_OS_WIN
    void *m_receiveEvent = nullptr; // HANDLE: set by the driver when a frame arrives
    void *m_wakeEvent = nullptr;    // HANDLE: set by wake()
#endif
};

} // namespace fhmi
