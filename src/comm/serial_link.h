#pragma once

#include "comm/link.h"

#include <QtCore/QPointer>
#include <QtSerialPort/QSerialPort>
#include <QtSerialPort/QSerialPortInfo>

#include <array>

namespace fhmi {

class SerialLink;

// One app-local cable. Each side behaves as an independent serial link.
class VirtualSerialPair : public QObject
{
  public:
    using QObject::QObject;

  private:
    friend class SerialLink;
    std::array<QPointer<SerialLink>, 2> m_sides;
};

struct SerialSettings
{
    QString portName;
    int baudRate = 115200;
    QString dataBits = QStringLiteral("8");
    QString parity = QStringLiteral("None");  // None, Even, Odd, Mark, Space
    QString stopBits = QStringLiteral("1");   // 1, 1.5, 2
    QString flowControl = QStringLiteral("无"); // i18n: value; 无, RTS/CTS, XON/XOFF, DTR/DSR
};

class SerialLink : public Link
{
    Q_OBJECT

  public:
    explicit SerialLink(QObject *parent = nullptr, VirtualSerialPair *virtualPair = nullptr);
    ~SerialLink() override;

    // Ports as shown to the user; device paths keep their case on macOS.
    static QStringList availablePorts();
    static QStringList selectablePorts();
    static QString portLabel(const QString &name);
    static bool isVirtualPort(const QString &name);

    bool open(const SerialSettings &settings, QString *error = nullptr);
    void close() override;

    QString description() const override;
    bool isConnected() const override;
    bool canSend() const override;
    quint64 sendSession(const QString &endpoint) const override;
    bool hasPendingWrite(const QString &) const override { return m_port.bytesToWrite() > 0; }

  protected:
    qint64 writeBytes(const QByteArray &bytes) override;

  private:
    void onReadyRead();
    void onError(QSerialPort::SerialPortError error);
    SerialLink *virtualPeer() const;
    void releaseVirtualPort();

    QSerialPort m_port;
    SerialSettings m_settings;
    QPointer<VirtualSerialPair> m_virtualPair;
    int m_virtualSide = -1;
};

} // namespace fhmi
