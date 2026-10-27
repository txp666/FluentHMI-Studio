#include "comm/serial_link.h"

#include <QtCore/QHash>

namespace fhmi {

SerialLink::SerialLink(QObject *parent) : Link(parent)
{
    connect(&m_port, &QSerialPort::readyRead, this, &SerialLink::onReadyRead);
    connect(&m_port, &QSerialPort::errorOccurred, this, &SerialLink::onError);
}

SerialLink::~SerialLink()
{
    m_port.close();
}

QStringList SerialLink::availablePorts()
{
    QStringList names;
    for (const QSerialPortInfo &info : QSerialPortInfo::availablePorts()) {
#ifdef Q_OS_MACOS
        // Callout devices are the ones to open on macOS; skip the tty.* duplicates.
        if (info.portName().startsWith(QLatin1String("tty."))) {
            continue;
        }
        names.append(info.systemLocation());
#else
        names.append(info.portName());
#endif
    }
    return names;
}

bool SerialLink::open(const SerialSettings &settings, QString *error)
{
    close();
    m_settings = settings;
    m_port.setPortName(settings.portName);
    m_port.setBaudRate(settings.baudRate);

    static const QHash<QString, QSerialPort::DataBits> dataBits = {{QStringLiteral("5"), QSerialPort::Data5},
                                                                   {QStringLiteral("6"), QSerialPort::Data6},
                                                                   {QStringLiteral("7"), QSerialPort::Data7},
                                                                   {QStringLiteral("8"), QSerialPort::Data8}};
    static const QHash<QString, QSerialPort::Parity> parity = {{QStringLiteral("None"), QSerialPort::NoParity},
                                                               {QStringLiteral("Even"), QSerialPort::EvenParity},
                                                               {QStringLiteral("Odd"), QSerialPort::OddParity},
                                                               {QStringLiteral("Mark"), QSerialPort::MarkParity},
                                                               {QStringLiteral("Space"), QSerialPort::SpaceParity}};
    static const QHash<QString, QSerialPort::StopBits> stopBits = {
        {QStringLiteral("1"), QSerialPort::OneStop},
        {QStringLiteral("1.5"), QSerialPort::OneAndHalfStop},
        {QStringLiteral("2"), QSerialPort::TwoStop}};
    m_port.setDataBits(dataBits.value(settings.dataBits, QSerialPort::Data8));
    m_port.setParity(parity.value(settings.parity, QSerialPort::NoParity));
    m_port.setStopBits(stopBits.value(settings.stopBits, QSerialPort::OneStop));
    if (settings.flowControl == QLatin1String("RTS/CTS")) {
        m_port.setFlowControl(QSerialPort::HardwareControl);
    } else if (settings.flowControl == QLatin1String("XON/XOFF")) {
        m_port.setFlowControl(QSerialPort::SoftwareControl);
    } else {
        m_port.setFlowControl(QSerialPort::NoFlowControl);
    }

    if (!m_port.open(QIODevice::ReadWrite)) {
        if (error) {
            *error = m_port.errorString();
        }
        return false;
    }
    if (settings.flowControl == QLatin1String("DTR/DSR")) {
        // Qt has no DTR/DSR handshake; assert DTR so devices waiting for it start talking.
        m_port.setDataTerminalReady(true);
        emit infoMessage(tr("DTR/DSR 流控以常开 DTR 方式提供"));
    }
    resetDefaultParser();
    emit connectedChanged(true);
    return true;
}

void SerialLink::close()
{
    if (!m_port.isOpen()) {
        return;
    }
    m_port.close();
    resetDefaultParser();
    emit connectedChanged(false);
}

QString SerialLink::description() const
{
    return QStringLiteral("%1 @ %2").arg(m_settings.portName).arg(m_settings.baudRate);
}

bool SerialLink::isConnected() const
{
    return m_port.isOpen();
}

qint64 SerialLink::writeBytes(const QByteArray &bytes)
{
    return m_port.write(bytes);
}

void SerialLink::onReadyRead()
{
    deliver(defaultParser(), m_port.readAll());
}

void SerialLink::onError(QSerialPort::SerialPortError error)
{
    if (error == QSerialPort::NoError) {
        return;
    }
    emit errorOccurred(m_port.errorString());
    // A device that vanished leaves the handle open but useless.
    if (error == QSerialPort::ResourceError || error == QSerialPort::PermissionError) {
        close();
    }
}

} // namespace fhmi
