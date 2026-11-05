#include "comm/can/slcan_backend.h"

#include "comm/can/slcan_protocol.h"
#include "comm/serial_link.h"

#include <QtCore/QDateTime>
#include <QtCore/QElapsedTimer>
#include <QtCore/QTimer>

namespace fhmi {

namespace {

constexpr char kBell = 0x07;
// CANUSB manual 1.5: start a session with a few CRs to flush anything left in the adapter.
const QByteArray kFlush = QByteArrayLiteral("\r\r\r");
constexpr int kStatusIntervalMs = 1000;
// A USB adapter ignores the baud rate; a real serial one (CAN232) is usually set to this.
constexpr int kSerialBaud = 115200;

} // namespace

SlcanBackend::SlcanBackend(QObject *parent) : CanBackend(parent)
{
    connect(&m_port, &QSerialPort::readyRead, this, &SlcanBackend::onReadyRead);
    connect(&m_port, &QSerialPort::errorOccurred, this, [this](QSerialPort::SerialPortError error) {
        if (m_open && (error == QSerialPort::ResourceError || error == QSerialPort::PermissionError)) {
            const QString message = tr("slcan 串口出错: %1").arg(m_port.errorString());
            close();
            emit failed(message);
        }
    });
    m_statusTimer = new QTimer(this);
    m_statusTimer->setInterval(kStatusIntervalMs);
    connect(m_statusTimer, &QTimer::timeout, this, [this]() {
        if (m_open && m_statusPending < 3) {
            ++m_statusPending;
            m_port.write("F\r");
        }
    });
}

SlcanBackend::~SlcanBackend()
{
    close();
}

QList<CanChannel> SlcanBackend::channels()
{
    QList<CanChannel> out;
    for (const QString &port : SerialLink::availablePorts()) {
        out.append({port, port});
    }
    return out;
}

bool SlcanBackend::command(const QByteArray &text, int timeoutMs)
{
    m_port.write(text);
    m_port.waitForBytesWritten(timeoutMs);
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < timeoutMs) {
        if (!m_port.bytesAvailable() && !m_port.waitForReadyRead(int(timeoutMs - clock.elapsed()))) {
            break;
        }
        const QByteArray reply = m_port.readAll();
        if (reply.contains(kBell)) {
            return false;
        }
        if (reply.contains('\r')) {
            return true;
        }
    }
    return true; // no answer: some firmware does not acknowledge
}

bool SlcanBackend::open(const CanSettings &settings, QString *error)
{
    close();
    const auto fail = [this, error](const QString &message) {
        if (m_port.isOpen()) {
            m_port.close();
        }
        if (error) {
            *error = message;
        }
        return false;
    };
    if (settings.fd) {
        return fail(tr("slcan 不支持 CAN FD"));
    }
    const int code = Slcan::bitrateCode(settings.bitrate);
    if (code < 0) {
        return fail(tr("slcan 只能设置 10k、20k、50k、100k、125k、250k、500k、800k、1M"));
    }
    if (settings.channel.trimmed().isEmpty()) {
        return fail(tr("请先选择串口"));
    }
    m_port.setPortName(settings.channel.trimmed());
    m_port.setBaudRate(kSerialBaud);
    if (!m_port.open(QIODevice::ReadWrite)) {
        return fail(tr("无法打开串口 %1: %2").arg(settings.channel, m_port.errorString()));
    }
    // Flush, then close a channel left open (its BELL when none was open does not matter), set the bitrate, open.
    // The CRs and C answer once each, maybe in several reads: all of that is drained before S, whose answer counts.
    for (const QByteArray &text : {kFlush, QByteArrayLiteral("C\r")}) {
        m_port.write(text);
        m_port.waitForBytesWritten(100);
        while (m_port.waitForReadyRead(100)) {
            m_port.readAll();
        }
        m_port.readAll();
    }
    if (!command(QByteArrayLiteral("S") + QByteArray::number(code) + '\r', 500)) {
        return fail(tr("slcan 适配器拒绝了波特率设置"));
    }
    if (!command(settings.listenOnly ? QByteArrayLiteral("L\r") : QByteArrayLiteral("O\r"), 500)) {
        return fail(settings.listenOnly ? tr("slcan 适配器不支持只听模式（L 命令）") : tr("slcan 适配器无法打开 CAN 通道"));
    }
    m_line.clear();
    m_statusPending = 0;
    m_state = CanBusState::Normal;
    m_open = true;
    m_description = QStringLiteral("slcan %1 @ %2").arg(settings.channel.trimmed(), CanAdapter::bitrateText(settings.bitrate));
    if (settings.listenOnly) {
        m_description += tr(" 只听");
    }
    m_statusTimer->start();
    return true;
}

void SlcanBackend::close()
{
    m_statusTimer->stop();
    if (m_port.isOpen()) {
        if (m_open) {
            m_port.write("C\r");
            m_port.waitForBytesWritten(100);
        }
        m_port.close();
    }
    m_open = false;
}

bool SlcanBackend::send(const CanFrame &frame, QString *error)
{
    const auto line = Slcan::encodeFrame(frame);
    if (!line) {
        if (error) {
            *error = frame.fd ? tr("slcan 不支持 CAN FD") : tr("slcan 无法发送此报文");
        }
        return false;
    }
    if (!m_open || m_port.write(*line) != line->size()) {
        if (error) {
            *error = tr("slcan 写入失败: %1").arg(m_port.errorString());
        }
        return false;
    }
    CanFrame sent = frame;
    sent.timestampMs = QDateTime::currentMSecsSinceEpoch();
    emit framesSent({sent});
    return true;
}

void SlcanBackend::onReadyRead()
{
    // While open() runs, waitForReadyRead() emits this too: its replies are command()'s to read.
    if (!m_open) {
        return;
    }
    const QByteArray bytes = m_port.readAll();
    QList<CanFrame> frames;
    std::optional<CanBusState> state;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    int refused = 0;
    for (const char c : bytes) {
        if (c == kBell) {
            // The answer to an F the adapter could not serve, or a frame it would not send.
            if (m_statusPending > 0) {
                --m_statusPending;
            } else {
                ++refused;
            }
            continue;
        }
        if (c != '\r' && c != '\n') {
            if (m_line.size() < 256) {
                m_line += c;
            }
            continue;
        }
        const QByteArray line = m_line;
        m_line.clear();
        if (line.isEmpty()) {
            continue; // OK
        }
        if (auto frame = Slcan::decodeFrame(line)) {
            frame->timestampMs = now;
            frames.append(*frame);
        } else if (const auto flags = Slcan::decodeStatus(line)) {
            m_statusPending = std::max(0, m_statusPending - 1);
            state = (*flags & Slcan::ErrorPassiveFlag)   ? CanBusState::Passive
                    : (*flags & Slcan::ErrorWarningFlag) ? CanBusState::Warning
                                                         : CanBusState::Normal;
        }
        // z / Z after a transmit and any other reply need nothing.
    }
    if (!frames.isEmpty()) {
        emit framesReceived(frames);
    }
    if (refused > 0) {
        emit sendFailed(tr("slcan 适配器拒绝发送报文"));
    }
    if (state && *state != m_state) {
        m_state = *state;
        emit busStateChanged(*state);
    }
}

} // namespace fhmi
