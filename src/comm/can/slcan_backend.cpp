#include "comm/can/slcan_backend.h"

#include "comm/can/slcan_protocol.h"
#include "comm/serial_link.h"

#include <QtCore/QDateTime>
#include <QtCore/QElapsedTimer>
#include <QtCore/QPointer>
#include <QtCore/QTimer>

namespace fhmi {

namespace {

constexpr char kBell = 0x07;
// CANUSB manual 1.5: start a session with a few CRs to flush anything left in the adapter.
const QByteArray kFlush = QByteArrayLiteral("\r\r\r");
constexpr int kStatusIntervalMs = 1000;
constexpr int kReplyTimeoutMs = 500;
constexpr qsizetype kMaxQueue = 4096;
// A USB adapter ignores the baud rate; a real serial one (CAN232) is usually set to this.
constexpr int kSerialBaud = 115200;

} // namespace

SlcanBackend::SlcanBackend(QObject *parent) : CanBackend(parent)
{
    connect(&m_port, &QSerialPort::readyRead, this, &SlcanBackend::onReadyRead);
    connect(&m_port, &QSerialPort::errorOccurred, this, [this](QSerialPort::SerialPortError error) {
        if (m_open && (error == QSerialPort::ResourceError || error == QSerialPort::PermissionError)) {
            failChannel(tr("slcan 串口出错: %1").arg(m_port.errorString()));
        }
    });
    m_statusTimer = new QTimer(this);
    m_statusTimer->setInterval(kStatusIntervalMs);
    connect(m_statusTimer, &QTimer::timeout, this, [this]() {
        m_statusDue = true;
        sendNext();
    });
    m_replyTimer = new QTimer(this);
    m_replyTimer->setSingleShot(true);
    connect(m_replyTimer, &QTimer::timeout, this, [this]() {
        // The protocol's CR/BELL replies carry no request ID. After a missing reply,
        // sending anything else could let a late response acknowledge the wrong frame.
        failChannel(tr("slcan 适配器应答超时，请重新连接"));
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

SlcanBackend::SetupReply SlcanBackend::setupCommand(const QByteArray &text)
{
    if (m_port.write(text) != text.size() ||
        (m_port.bytesToWrite() && !m_port.waitForBytesWritten(kReplyTimeoutMs))) {
        return SetupReply::WriteFailed;
    }
    QElapsedTimer clock;
    clock.start();
    bool hasContent = false;
    for (qint64 remaining = kReplyTimeoutMs; remaining > 0; remaining = kReplyTimeoutMs - clock.elapsed()) {
        if (!m_port.bytesAvailable() && !m_port.waitForReadyRead(int(remaining))) {
            break;
        }
        const QByteArray reply = m_port.readAll();
        for (const char c : reply) {
            if (c == kBell)
                return SetupReply::Rejected;
            if (c == '\r') {
                if (!hasContent)
                    return SetupReply::Accepted;
                hasContent = false; // unsolicited CAN traffic is not the command acknowledgement
            } else if (c != '\n') {
                hasContent = true;
            }
        }
    }
    return SetupReply::Silent;
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
    m_port.setDataBits(QSerialPort::Data8);
    m_port.setParity(QSerialPort::NoParity);
    m_port.setStopBits(QSerialPort::OneStop);
    m_port.setFlowControl(QSerialPort::NoFlowControl);
    if (!m_port.open(QIODevice::ReadWrite)) {
        return fail(tr("无法打开串口 %1: %2").arg(settings.channel, m_port.errorString()));
    }
    // Flush, then close a channel left open (its BELL when none was open does not matter), set the bitrate, open.
    // Bound each drain even when a channel left open is forwarding continuous CAN traffic.
    for (const QByteArray &text : {kFlush, QByteArrayLiteral("C\r")}) {
        m_port.write(text);
        m_port.waitForBytesWritten(100);
        QElapsedTimer drain;
        drain.start();
        for (qint64 remaining = 100; remaining > 0 && m_port.waitForReadyRead(int(remaining));
             remaining = 100 - drain.elapsed()) {
            m_port.readAll();
        }
        m_port.readAll();
    }
    const SetupReply bitrate = setupCommand(QByteArrayLiteral("S") + QByteArray::number(code) + '\r');
    if (bitrate == SetupReply::WriteFailed) {
        return fail(tr("slcan 写入失败: %1").arg(m_port.errorString()));
    }
    if (bitrate == SetupReply::Rejected) {
        return fail(tr("slcan 适配器拒绝了波特率设置"));
    }
    const SetupReply opened = setupCommand(settings.listenOnly ? QByteArrayLiteral("L\r") : QByteArrayLiteral("O\r"));
    if (opened == SetupReply::WriteFailed) {
        return fail(tr("slcan 写入失败: %1").arg(m_port.errorString()));
    }
    if (opened == SetupReply::Rejected) {
        return fail(settings.listenOnly ? tr("slcan 适配器不支持只听模式（L 命令）") : tr("slcan 适配器无法打开 CAN 通道"));
    }
    // Stock CANable and similar firmware can respond to neither setup command.
    // Keep their existing raw/ordinary TX path, with no F commands in flight.
    m_waitForReplies = bitrate == SetupReply::Accepted || opened == SetupReply::Accepted;
    m_state = CanBusState::Normal;
    m_open = true;
    m_description = QStringLiteral("slcan %1 @ %2").arg(settings.channel.trimmed(), CanAdapter::bitrateText(settings.bitrate));
    if (settings.listenOnly) {
        m_description += tr(" 只听");
    }
    if (m_waitForReplies)
        m_statusTimer->start();
    return true;
}

void SlcanBackend::close()
{
    const bool wasOpen = std::exchange(m_open, false);
    ++m_generation;
    m_statusTimer->stop();
    finishReply();
    m_statusDue = false;
    m_outgoing.clear();
    m_line.clear();
    if (m_port.isOpen()) {
        if (wasOpen) {
            m_port.write("C\r");
            m_port.waitForBytesWritten(100);
        }
        m_port.close();
    }
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
    if (!m_open || transmitState()->stopping) {
        if (error)
            *error = tr("CAN 通道未打开");
        return false;
    }
    if (m_waitForReplies) {
        if (m_outgoing.size() >= kMaxQueue) {
            if (error)
                *error = tr("CAN 发送队列已满");
            return false;
        }
        m_outgoing.enqueue({*line, frame});
        sendNext();
        return true;
    }
    if (frame.requireAdapterReply) {
        if (error)
            *error = tr("slcan 发送初值映射需要能应答的适配器");
        return false;
    }
    if (m_port.write(*line) != line->size()) {
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

void SlcanBackend::sendNext()
{
    if (!m_open || m_inFlight || transmitState()->stopping)
        return;
    if (m_statusDue) {
        m_statusDue = false;
        m_inFlight = Request{QByteArrayLiteral("F\r"), std::nullopt};
    } else if (!m_outgoing.isEmpty()) {
        m_inFlight = m_outgoing.dequeue();
    } else {
        return;
    }
    const QByteArray text = m_inFlight->text;
    const bool accepted = m_port.write(text) == text.size();
    if (!m_open)
        return; // errorOccurred() may have closed the port
    if (!accepted) {
        failChannel(tr("slcan 写入失败: %1").arg(m_port.errorString()));
        return;
    }
    m_replyTimer->start(kReplyTimeoutMs);
}

void SlcanBackend::finishReply()
{
    m_replyTimer->stop();
    m_inFlight.reset();
}

void SlcanBackend::failChannel(const QString &error)
{
    close();
    emit failed(error);
}

void SlcanBackend::onReadyRead()
{
    // During open(), waitForReadyRead() emits this too. setupCommand() handles those responses.
    if (!m_open) {
        return;
    }
    const QByteArray bytes = m_port.readAll();
    QList<CanFrame> frames;
    std::optional<CanFrame> submitted;
    std::optional<CanFrame> rejected;
    std::optional<CanBusState> state;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    bool legacyRefused = false;
    for (const char c : bytes) {
        if (c == kBell) {
            m_line.clear();
            if (m_inFlight) {
                if (m_inFlight->frame) {
                    rejected = *m_inFlight->frame;
                } else {
                    // An adapter that explicitly rejects F need not be polled again.
                    m_statusTimer->stop();
                    m_statusDue = false;
                }
                finishReply();
            } else if (!m_waitForReplies) {
                legacyRefused = true;
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
        if (auto frame = Slcan::decodeFrame(line)) {
            frame->timestampMs = now;
            frame->receivedAtMs = Can::monotonicMs();
            frames.append(*frame);
        } else if (m_inFlight && m_inFlight->frame && c == '\r' &&
                   (line.isEmpty() || line == (m_inFlight->frame->extended ? "Z" : "z"))) {
            submitted = *m_inFlight->frame;
            submitted->timestampMs = now;
            finishReply();
        } else if (m_inFlight && !m_inFlight->frame) {
            if (const auto flags = Slcan::decodeStatus(line)) {
                state = (*flags & Slcan::ErrorPassiveFlag)   ? CanBusState::Passive
                        : (*flags & Slcan::ErrorWarningFlag) ? CanBusState::Warning
                                                             : CanBusState::Normal;
                finishReply();
            }
        }
    }
    // Wait until the entire buffer is parsed before advancing the command queue. A
    // second delimiter or another already buffered response cannot apply to the next request.
    const QPointer<SlcanBackend> alive(this);
    const quint64 generation = m_generation;
    const auto current = [this, alive, generation] { return alive && m_generation == generation; };
    if (submitted)
        emit framesSent({*submitted});
    if (current() && rejected)
        emit frameSendFailed(*rejected, tr("slcan 适配器拒绝发送报文"));
    if (current() && !frames.isEmpty()) {
        emit framesReceived(frames);
    }
    if (current() && legacyRefused) {
        emit sendFailed(tr("slcan 适配器拒绝发送报文"));
    }
    if (current() && state && *state != m_state) {
        m_state = *state;
        emit busStateChanged(*state);
    }
    if (current())
        sendNext();
}

} // namespace fhmi
