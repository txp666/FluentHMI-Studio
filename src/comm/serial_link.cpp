#include "comm/serial_link.h"

#include <QtCore/QHash>

#ifdef Q_OS_LINUX
#include <QtCore/QFile>
#include <QtCore/QFileInfo>

#include <cerrno>
#include <unistd.h>
#endif

namespace fhmi {

namespace {

const QStringList kVirtualPorts{QStringLiteral("virtual:A"), QStringLiteral("virtual:B")};

} // namespace

#ifdef Q_OS_LINUX
namespace {

// Qt says only "Permission denied". On Linux that means this user is not in the group that owns the device node
// (dialout on Debian, Ubuntu and Fedora, uucp on Arch), which takes a usermod and a new login. A port another
// program holds fails the same way but passes access(), and keeps Qt's message. Empty when there is nothing to add.
QString permissionHint(const QString &portName)
{
    const QString device = portName.startsWith(QLatin1Char('/')) ? portName : QStringLiteral("/dev/") + portName;
    if (::access(QFile::encodeName(device).constData(), R_OK | W_OK) == 0 || errno != EACCES) {
        return QString();
    }
    const QFileInfo node(device);
    const QString group = node.group().isEmpty() ? QString::number(node.groupId()) : node.group();
    return SerialLink::tr("没有权限打开 %1。请把当前用户加入拥有它的用户组：sudo usermod -aG %2 $USER，然后注销并重新登录")
        .arg(device, group);
}

} // namespace
#endif

SerialLink::SerialLink(QObject *parent, VirtualSerialPair *virtualPair) : Link(parent), m_virtualPair(virtualPair)
{
    connect(&m_port, &QSerialPort::readyRead, this, &SerialLink::onReadyRead);
    connect(&m_port, &QSerialPort::errorOccurred, this, &SerialLink::onError);
}

SerialLink::~SerialLink()
{
    releaseVirtualPort();
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

QStringList SerialLink::selectablePorts()
{
    return availablePorts() + kVirtualPorts;
}

QString SerialLink::portLabel(const QString &name)
{
    const auto side = kVirtualPorts.indexOf(name);
    return side < 0 ? name : tr("虚拟串口 %1").arg(side == 0 ? QLatin1Char('A') : QLatin1Char('B'));
}

bool SerialLink::isVirtualPort(const QString &name)
{
    return kVirtualPorts.contains(name);
}

bool SerialLink::open(const SerialSettings &settings, QString *error)
{
    close();
    m_settings = settings;
    const auto side = kVirtualPorts.indexOf(settings.portName);
    if (side >= 0) {
        if (!m_virtualPair || m_virtualPair->m_sides.at(side)) {
            if (error)
                *error = m_virtualPair ? tr("%1 已由另一张连接卡片打开").arg(portLabel(settings.portName))
                                       : tr("虚拟串口需要工程内的连接卡片");
            return false;
        }
        m_virtualSide = int(side);
        m_virtualPair->m_sides.at(side) = this;
        resetDefaultParser();
        emit connectedChanged(true);
        if (auto *peer = virtualPeer())
            emit peer->canSendChanged();
        return true;
    }
    m_port.setPortName(settings.portName);

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
    const QSerialPort::FlowControl flow = settings.flowControl == QLatin1String("RTS/CTS")
                                              ? QSerialPort::HardwareControl
                                              : (settings.flowControl == QLatin1String("XON/XOFF")
                                                     ? QSerialPort::SoftwareControl
                                                     : QSerialPort::NoFlowControl);
    if (!m_port.setBaudRate(settings.baudRate) ||
        !m_port.setDataBits(dataBits.value(settings.dataBits, QSerialPort::Data8)) ||
        !m_port.setParity(parity.value(settings.parity, QSerialPort::NoParity)) ||
        !m_port.setStopBits(stopBits.value(settings.stopBits, QSerialPort::OneStop)) ||
        !m_port.setFlowControl(flow)) {
        if (error) {
            const QString detail = m_port.errorString();
            *error = detail.isEmpty() ? tr("串口参数不受支持") : tr("串口参数配置失败: %1").arg(detail);
        }
        return false;
    }

    if (!m_port.open(QIODevice::ReadWrite)) {
        if (error) {
            *error = m_port.errorString();
#ifdef Q_OS_LINUX
            const QString hint =
                m_port.error() == QSerialPort::PermissionError ? permissionHint(settings.portName) : QString();
            if (!hint.isEmpty()) {
                *error = hint;
            }
#endif
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
    if (!isConnected()) {
        return;
    }
    if (m_virtualSide >= 0)
        releaseVirtualPort();
    else
        m_port.close();
    resetDefaultParser();
    emit connectedChanged(false);
}

QString SerialLink::description() const
{
    if (isVirtualPort(m_settings.portName))
        return portLabel(m_settings.portName);
    return QStringLiteral("%1 @ %2").arg(m_settings.portName).arg(m_settings.baudRate);
}

bool SerialLink::isConnected() const
{
    return (m_virtualSide >= 0 && m_virtualPair) || m_port.isOpen();
}

bool SerialLink::canSend() const
{
    return m_virtualSide >= 0 ? virtualPeer() != nullptr : m_port.isOpen();
}

quint64 SerialLink::sendSession(const QString &endpoint) const
{
    if (m_virtualSide < 0)
        return Link::sendSession(endpoint);
    const auto *peer = virtualPeer();
    // Both ends use the same monotonically increasing generation counter.
    return peer && endpoint.isEmpty() ? qMax(Link::sendSession({}), peer->Link::sendSession({})) : 0;
}

SerialLink *SerialLink::virtualPeer() const
{
    return m_virtualPair && m_virtualSide >= 0 ? m_virtualPair->m_sides.at(1 - m_virtualSide).data() : nullptr;
}

void SerialLink::releaseVirtualPort()
{
    if (!m_virtualPair || m_virtualSide < 0)
        return;
    const QPointer<SerialLink> peer = virtualPeer();
    m_virtualPair->m_sides.at(m_virtualSide).clear();
    m_virtualSide = -1;
    if (peer) {
        peer->resetDefaultParser();
        if (peer)
            emit peer->canSendChanged();
    }
}

qint64 SerialLink::writeBytes(const QByteArray &bytes)
{
    if (m_virtualSide >= 0) {
        auto *peer = virtualPeer();
        if (!peer)
            return -1;
        // Deliver after the write returns, like readyRead on a physical port.
        const QPointer<SerialLink> sender(this);
        const auto session = sendSession({});
        QMetaObject::invokeMethod(peer, [peer, session, sender, bytes] {
            if (sender && sender->sendSession({}) == session && peer->sendSession({}) == session)
                peer->deliver(peer->defaultParser(), bytes);
        }, Qt::QueuedConnection);
        return bytes.size();
    }
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
