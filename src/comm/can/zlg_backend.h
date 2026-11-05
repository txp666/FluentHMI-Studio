#pragma once

#include "comm/can/can_polling_backend.h"
#include "comm/can/zlg_api.h"

namespace fhmi {

// ZLG adapters through zlgcan.dll (Windows x64 only; CanBackend::create() refuses it elsewhere, but it builds
// everywhere). The DLL and its kerneldlls folder are not bundled: they are looked for beside the program, in its
// zlgcan subfolder, then on the system path. Several cards may use channels of one device: the device is opened
// once and closed with its last channel.
class ZlgBackend : public CanPollingBackend
{
    Q_OBJECT

  public:
    using CanPollingBackend::CanPollingBackend;
    ~ZlgBackend() override;

    // What the card can set for a model: its family decides the bitrate fields and the terminal resistor.
    static CanAdapterCaps caps(const QString &model);

    bool open(const CanSettings &settings, QString *error) override;
    void close() override;
    bool isOpen() const override { return m_channel != nullptr; }
    QString description() const override { return m_description; }

  protected:
    bool readFrames(QList<CanFrame> &out, QString *error) override;
    bool writeFrame(const CanFrame &frame, QString *error) override;
    CanBusState readBusState() override;

  private:
    void releaseDevice();

    const zlg::Model *m_model = nullptr;
    zlg::DEVICE_HANDLE m_device = nullptr;
    zlg::CHANNEL_HANDLE m_channel = nullptr;
    int m_deviceIndex = 0;
    QString m_description;
};

} // namespace fhmi
