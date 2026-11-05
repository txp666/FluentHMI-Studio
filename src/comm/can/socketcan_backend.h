#pragma once

#include "comm/can/can_backend.h"

class QSocketNotifier;

namespace fhmi {

// Linux SocketCAN: a CAN_RAW socket on an interface such as can0, read through a QSocketNotifier on the GUI thread
// (no thread: reads never block). The bitrate is the interface's own, set with ip link; the card shows the command.
// Bus state comes from error frames (CAN_RAW_ERR_FILTER). Built on Linux only.
class SocketCanBackend : public CanBackend
{
    Q_OBJECT

  public:
    using CanBackend::CanBackend;
    ~SocketCanBackend() override;

    // The interfaces of type CAN (ARPHRD_CAN, 280) in /sys/class/net.
    static QList<CanChannel> channels();

    bool open(const CanSettings &settings, QString *error) override;
    void close() override;
    bool isOpen() const override { return m_socket >= 0; }
    bool send(const CanFrame &frame, QString *error) override;
    QString description() const override { return m_description; }

  private:
    void readAll();

    int m_socket = -1;
    bool m_fd = false;
    QSocketNotifier *m_notifier = nullptr;
    QString m_description;
    CanBusState m_state = CanBusState::Normal;
};

} // namespace fhmi
