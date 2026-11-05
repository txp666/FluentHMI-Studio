#include "comm/can/socketcan_backend.h"

#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QSocketNotifier>

#include <algorithm>
#include <cerrno>
#include <cstring>

#include <fcntl.h>
#include <linux/can.h>
#include <linux/can/error.h>
#include <linux/can/raw.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace fhmi {

namespace {

constexpr int kArphrdCan = 280; // <linux/if_arp.h> ARPHRD_CAN, as /sys/class/net/<if>/type shows it
constexpr int kReadBatch = 512; // per notification, so a busy bus cannot starve the event loop

QString systemError()
{
    return QString::fromLocal8Bit(std::strerror(errno));
}

} // namespace

SocketCanBackend::~SocketCanBackend()
{
    close();
}

QList<CanChannel> SocketCanBackend::channels()
{
    QList<CanChannel> out;
    const QDir net(QStringLiteral("/sys/class/net"));
    for (const QString &name : net.entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::System, QDir::Name)) {
        QFile type(net.filePath(name + QStringLiteral("/type")));
        if (type.open(QIODevice::ReadOnly) && type.readAll().trimmed().toInt() == kArphrdCan) {
            out.append({name, name});
        }
    }
    return out;
}

bool SocketCanBackend::open(const CanSettings &settings, QString *error)
{
    close();
    const auto fail = [this, error](const QString &message) {
        if (m_socket >= 0) {
            ::close(m_socket);
            m_socket = -1;
        }
        if (error) {
            *error = message;
        }
        return false;
    };
    const QByteArray name = settings.channel.trimmed().toLocal8Bit();
    if (name.isEmpty()) {
        return fail(tr("请先选择 CAN 接口"));
    }
    if (name.size() >= IFNAMSIZ) {
        return fail(tr("接口名过长: %1").arg(settings.channel));
    }
    const QString command = CanAdapter::socketCanCommand(settings.channel, settings);
    m_socket = ::socket(PF_CAN, SOCK_RAW | SOCK_NONBLOCK | SOCK_CLOEXEC, CAN_RAW);
    if (m_socket < 0) {
        return fail(tr("无法创建 CAN 套接字: %1").arg(systemError()));
    }
    ifreq request{};
    std::memcpy(request.ifr_name, name.constData(), size_t(name.size()));
    if (::ioctl(m_socket, SIOCGIFINDEX, &request) < 0) {
        return fail(tr("没有 CAN 接口 %1: %2").arg(settings.channel, systemError()));
    }
    const int index = request.ifr_ifindex;
    if (::ioctl(m_socket, SIOCGIFFLAGS, &request) == 0 && !(request.ifr_flags & IFF_UP)) {
        return fail(tr("%1 未启用，请先执行: %2").arg(settings.channel, command));
    }
    m_fd = settings.fd;
    if (m_fd) {
        if (::ioctl(m_socket, SIOCGIFMTU, &request) < 0 || request.ifr_mtu != int(CANFD_MTU)) {
            return fail(tr("%1 没有开启 CAN FD，请执行: %2").arg(settings.channel, command));
        }
        const int on = 1;
        if (::setsockopt(m_socket, SOL_CAN_RAW, CAN_RAW_FD_FRAMES, &on, sizeof on) < 0) {
            return fail(tr("无法开启 CAN FD: %1").arg(systemError()));
        }
    }
    // Error frames carry the controller state: warning / passive / bus off and the restart after it.
    const can_err_mask_t errors = CAN_ERR_CRTL | CAN_ERR_BUSOFF | CAN_ERR_RESTARTED;
    ::setsockopt(m_socket, SOL_CAN_RAW, CAN_RAW_ERR_FILTER, &errors, sizeof errors);
    sockaddr_can address{};
    address.can_family = AF_CAN;
    address.can_ifindex = index;
    if (::bind(m_socket, reinterpret_cast<sockaddr *>(&address), sizeof address) < 0) {
        return fail(tr("无法打开 %1: %2").arg(settings.channel, systemError()));
    }
    m_state = CanBusState::Normal;
    m_description = QStringLiteral("SocketCAN %1").arg(settings.channel);
    m_notifier = new QSocketNotifier(m_socket, QSocketNotifier::Read, this);
    connect(m_notifier, &QSocketNotifier::activated, this, &SocketCanBackend::readAll);
    return true;
}

void SocketCanBackend::close()
{
    delete m_notifier;
    m_notifier = nullptr;
    if (m_socket >= 0) {
        ::close(m_socket);
        m_socket = -1;
    }
}

bool SocketCanBackend::send(const CanFrame &frame, QString *error)
{
    if (m_socket < 0) {
        if (error) {
            *error = tr("CAN 通道未打开");
        }
        return false;
    }
    canfd_frame raw{};
    raw.can_id = frame.id | (frame.extended ? CAN_EFF_FLAG : 0) | (frame.rtr ? CAN_RTR_FLAG : 0);
    raw.len = quint8(std::min<qsizetype>(frame.data.size(), frame.fd ? CANFD_MAX_DLEN : CAN_MAX_DLEN));
    if (!frame.rtr) {
        std::memcpy(raw.data, frame.data.constData(), raw.len);
    }
    size_t size = CAN_MTU;
    if (frame.fd) {
        if (!m_fd) {
            if (error) {
                *error = tr("通道是经典 CAN，不能发送 CAN FD 报文（勾选 CAN FD 后重新连接）");
            }
            return false;
        }
        raw.flags = frame.brs ? CANFD_BRS : 0;
        size = CANFD_MTU;
    }
    // can_frame and canfd_frame share their first CAN_MTU bytes (can_dlc is len).
    if (::write(m_socket, &raw, size) != ssize_t(size)) {
        if (error) {
            *error = errno == ENOBUFS || errno == EAGAIN ? tr("发送缓冲区已满（总线上没有其他节点或总线关闭？）")
                                                         : tr("发送失败: %1").arg(systemError());
        }
        return false;
    }
    CanFrame sent = frame;
    sent.timestampMs = QDateTime::currentMSecsSinceEpoch();
    emit framesSent({sent});
    return true;
}

void SocketCanBackend::readAll()
{
    QList<CanFrame> frames;
    CanBusState state = m_state;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (int i = 0; i < kReadBatch && m_socket >= 0; ++i) {
        canfd_frame raw{};
        const ssize_t got = ::read(m_socket, &raw, sizeof raw);
        if (got < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                const QString message = tr("CAN 接口读取失败: %1").arg(systemError());
                close();
                emit failed(message);
                return;
            }
            break;
        }
        if (got != ssize_t(CAN_MTU) && got != ssize_t(CANFD_MTU)) {
            continue;
        }
        if (raw.can_id & CAN_ERR_FLAG) {
            // linux/can/error.h: data[1] has the controller's state for CAN_ERR_CRTL.
            if (raw.can_id & CAN_ERR_BUSOFF) {
                state = CanBusState::BusOff;
            } else if (raw.can_id & CAN_ERR_RESTARTED) {
                state = CanBusState::Normal;
            } else if (raw.can_id & CAN_ERR_CRTL) {
                if (raw.data[1] & (CAN_ERR_CRTL_RX_PASSIVE | CAN_ERR_CRTL_TX_PASSIVE)) {
                    state = CanBusState::Passive;
                } else if (raw.data[1] & (CAN_ERR_CRTL_RX_WARNING | CAN_ERR_CRTL_TX_WARNING)) {
                    state = CanBusState::Warning;
                } else if (raw.data[1] & CAN_ERR_CRTL_ACTIVE) {
                    state = CanBusState::Normal;
                }
            }
            continue;
        }
        CanFrame frame;
        frame.extended = raw.can_id & CAN_EFF_FLAG;
        frame.id = raw.can_id & (frame.extended ? CAN_EFF_MASK : CAN_SFF_MASK);
        frame.rtr = raw.can_id & CAN_RTR_FLAG;
        frame.fd = got == ssize_t(CANFD_MTU);
        frame.brs = frame.fd && (raw.flags & CANFD_BRS);
        frame.data = QByteArray(reinterpret_cast<const char *>(raw.data), raw.len);
        frame.timestampMs = now;
        frames.append(frame);
    }
    if (!frames.isEmpty()) {
        emit framesReceived(frames);
    }
    if (state != m_state) {
        m_state = state;
        emit busStateChanged(state);
    }
}

} // namespace fhmi
