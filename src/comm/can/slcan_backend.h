#pragma once

#include "comm/can/can_backend.h"

#include <QtCore/QQueue>
#include <QtSerialPort/QSerialPort>

class QTimer;

namespace fhmi {

// Lawicel / CANable-style adapters that speak slcan over a (virtual) serial port: S0–S8 sets the bitrate, O opens
// (L listen-only), t/T/r/R carry frames, F reads the status flags once a second for the bus state. Classic CAN only.
// Uses QSerialPort on the backend's owning event loop.
class SlcanBackend : public CanBackend
{
    Q_OBJECT

  public:
    explicit SlcanBackend(QObject *parent = nullptr);
    ~SlcanBackend() override;

    static QList<CanChannel> channels();

    bool open(const CanSettings &settings, QString *error) override;
    void close() override;
    bool isOpen() const override { return m_open; }
    bool send(const CanFrame &frame, QString *error) override;
    QString description() const override { return m_description; }

  private:
    enum class SetupReply { Accepted, Rejected, Silent, WriteFailed };
    struct Request
    {
        QByteArray text;
        std::optional<CanFrame> frame; // absent for a status query
    };

    SetupReply setupCommand(const QByteArray &text);
    void sendNext();
    void finishReply();
    void onReadyRead();
    void failChannel(const QString &error);

    QSerialPort m_port;
    QTimer *m_statusTimer = nullptr;
    QTimer *m_replyTimer = nullptr;
    QByteArray m_line;
    QQueue<Request> m_outgoing;
    std::optional<Request> m_inFlight;
    bool m_waitForReplies = false;
    bool m_statusDue = false;
    bool m_open = false;
    quint64 m_generation = 0;
    QString m_description;
    CanBusState m_state = CanBusState::Normal;
};

} // namespace fhmi
