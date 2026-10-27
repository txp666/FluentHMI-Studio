#pragma once

#include "comm/link.h"

#include <QtSerialPort/QSerialPort>
#include <QtSerialPort/QSerialPortInfo>

namespace fhmi {

struct SerialSettings
{
    QString portName;
    int baudRate = 115200;
    QString dataBits = QStringLiteral("8");
    QString parity = QStringLiteral("None");  // None, Even, Odd, Mark, Space
    QString stopBits = QStringLiteral("1");   // 1, 1.5, 2
    QString flowControl = QStringLiteral("无"); // 无, RTS/CTS, XON/XOFF, DTR/DSR
};

class SerialLink : public Link
{
    Q_OBJECT

  public:
    explicit SerialLink(QObject *parent = nullptr);
    ~SerialLink() override;

    // Ports as shown to the user; device paths keep their case on macOS.
    static QStringList availablePorts();

    bool open(const SerialSettings &settings, QString *error = nullptr);
    void close() override;

    QString description() const override;
    bool isConnected() const override;

  protected:
    qint64 writeBytes(const QByteArray &bytes) override;

  private:
    void onReadyRead();
    void onError(QSerialPort::SerialPortError error);

    QSerialPort m_port;
    SerialSettings m_settings;
};

} // namespace fhmi
