#pragma once

#include "comm/can/can_backend.h"

#include <QtSerialPort/QSerialPort>

class QTimer;

namespace fhmi {

// Lawicel / CANable-style adapters that speak slcan over a (virtual) serial port: S0–S8 sets the bitrate, O opens
// (L listen-only), t/T/r/R carry frames, F reads the status flags once a second for the bus state. Classic CAN only.
// Everything runs on the GUI thread through QSerialPort's signals.
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
    // A command during open(): true for its CR (or for silence, which some firmware answers with), false for BELL.
    bool command(const QByteArray &text, int timeoutMs);
    void onReadyRead();

    QSerialPort m_port;
    QTimer *m_statusTimer = nullptr;
    QByteArray m_line;
    int m_statusPending = 0; // F commands not answered yet: a BELL may be their answer
    bool m_open = false;
    QString m_description;
    CanBusState m_state = CanBusState::Normal;
};

} // namespace fhmi
