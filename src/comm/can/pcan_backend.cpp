#include "comm/can/pcan_backend.h"

#include "core/can_timing.h"

#include <QtCore/QLibrary>

#include <algorithm>
#include <cstring>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX // keep std::min / std::max usable
#endif
#include <windows.h>
#endif

namespace fhmi {

using namespace pcan;

namespace {

// PCAN-USB (classic, SJA1000-compatible): BTR0/BTR1 at a 16 MHz crystal, i.e. 8 MHz in the timing formula; the
// PCAN_BAUD_* codes of PCANBasic.h are exactly that (PCAN_BAUD_500K = 0x001C, PCAN_BAUD_1M = 0x0014).
constexpr quint32 kClassicClock = 8000000;
// CAN FD: the f_clock_mhz of the bit rate string; 80 MHz is one of the clocks PCAN-Basic accepts (20, 24, 30, 40,
// 60, 80) and the PCAN-USB FD's own.
constexpr quint32 kFdClock = 80000000;
constexpr int kReadBatch = 256;

struct Api
{
    QLibrary library;
    bool loaded = false;
    CAN_Initialize_t initialize = nullptr;
    CAN_InitializeFD_t initializeFd = nullptr;
    CAN_Uninitialize_t uninitialize = nullptr;
    CAN_GetStatus_t getStatus = nullptr;
    CAN_Read_t read = nullptr;
    CAN_ReadFD_t readFd = nullptr;
    CAN_Write_t write = nullptr;
    CAN_WriteFD_t writeFd = nullptr;
    CAN_GetValue_t getValue = nullptr;
    CAN_SetValue_t setValue = nullptr;
    CAN_GetErrorText_t getErrorText = nullptr;
};

Api &api()
{
    static Api instance;
    return instance;
}

// Loads the library once, on the GUI thread, the first time a PCAN card lists channels or connects.
bool loadApi(QString *error)
{
    Api &a = api();
    if (a.loaded) {
        return true;
    }
#ifdef Q_OS_WIN
    const QStringList candidates{QStringLiteral("PCANBasic")};
    const QString missing = PcanBackend::tr("找不到 PCANBasic.dll：请安装 PEAK 的设备驱动（包含 PCAN-Basic）");
#else
    // MacCAN's installer puts the library in /usr/local/lib, which the loader does not search by itself.
    const QStringList candidates{QStringLiteral("/usr/local/lib/libPCBUSB.dylib"), QStringLiteral("PCBUSB")};
    const QString missing =
        PcanBackend::tr("找不到 libPCBUSB.dylib：请从 mac-can.com 下载并安装 PCBUSB 库（安装到 /usr/local/lib）");
#endif
    bool loaded = false;
    for (const QString &candidate : candidates) {
        a.library.setFileName(candidate);
        if (a.library.load()) {
            loaded = true;
            break;
        }
    }
    if (!loaded) {
        if (error) {
            *error = missing;
        }
        return false;
    }
    QString absent;
    const auto resolve = [&a, &absent](auto &function, const char *name) {
        function = reinterpret_cast<std::remove_reference_t<decltype(function)>>(a.library.resolve(name));
        if (!function && absent.isEmpty()) {
            absent = QLatin1String(name);
        }
    };
    resolve(a.initialize, "CAN_Initialize");
    resolve(a.initializeFd, "CAN_InitializeFD");
    resolve(a.uninitialize, "CAN_Uninitialize");
    resolve(a.getStatus, "CAN_GetStatus");
    resolve(a.read, "CAN_Read");
    resolve(a.readFd, "CAN_ReadFD");
    resolve(a.write, "CAN_Write");
    resolve(a.writeFd, "CAN_WriteFD");
    resolve(a.getValue, "CAN_GetValue");
    resolve(a.setValue, "CAN_SetValue");
    resolve(a.getErrorText, "CAN_GetErrorText");
    if (!absent.isEmpty()) {
        a.library.unload();
        if (error) {
            *error = PcanBackend::tr("%1 缺少函数 %2，请更新 PEAK 驱动").arg(a.library.fileName(), absent);
        }
        return false;
    }
    a.loaded = true;
    return true;
}

QString errorText(TPCANStatus status)
{
    char buffer[512] = {};
    // Language 0: the system's (PCAN-Basic offers neutral, German, English, …).
    if (api().getErrorText && api().getErrorText(status, 0, buffer) == PCAN_ERROR_OK && buffer[0]) {
        return QStringLiteral("%1 (0x%2)").arg(QString::fromLocal8Bit(buffer)).arg(status, 0, 16);
    }
    return QStringLiteral("0x%1").arg(status, 0, 16);
}

QString handleText(TPCANHandle handle)
{
    return QStringLiteral("0x") + QStringLiteral("%1").arg(handle, 2, 16, QLatin1Char('0')).toUpper();
}

} // namespace

PcanBackend::~PcanBackend()
{
    close();
}

QList<CanChannel> PcanBackend::channels(QString *error)
{
    QList<CanChannel> out;
    if (!loadApi(error)) {
        return out;
    }
    const Api &a = api();
    quint32 count = 0;
    if (a.getValue(PCAN_NONEBUS, PCAN_ATTACHED_CHANNELS_COUNT, &count, sizeof count) == PCAN_ERROR_OK) {
        QList<TPCANChannelInformation> list(static_cast<qsizetype>(count), TPCANChannelInformation{});
        if (count > 0 &&
            a.getValue(PCAN_NONEBUS, PCAN_ATTACHED_CHANNELS, list.data(),
                       quint32(sizeof(TPCANChannelInformation) * count)) == PCAN_ERROR_OK) {
            for (const TPCANChannelInformation &info : std::as_const(list)) {
                char name[MAX_LENGTH_HARDWARE_NAME + 1] = {};
                std::memcpy(name, info.device_name, MAX_LENGTH_HARDWARE_NAME);
                QString label = QStringLiteral("%1 %2").arg(QString::fromLatin1(name), handleText(info.channel_handle));
                if ((info.channel_condition & PCAN_CHANNEL_OCCUPIED) &&
                    !(info.channel_condition & PCAN_CHANNEL_AVAILABLE)) {
                    label += tr("（已占用）");
                }
                out.append({handleText(info.channel_handle), label});
            }
        }
        return out;
    }
    // Older libraries: ask each PCAN-USB channel whether it is there.
    QList<TPCANHandle> handles;
    for (TPCANHandle h = PCAN_USBBUS1; h <= PCAN_USBBUS8; ++h) {
        handles << h;
    }
    for (TPCANHandle h = PCAN_USBBUS9; h <= PCAN_USBBUS16; ++h) {
        handles << h;
    }
    for (int i = 0; i < handles.size(); ++i) {
        quint32 condition = 0;
        if (a.getValue(handles.at(i), PCAN_CHANNEL_CONDITION, &condition, sizeof condition) == PCAN_ERROR_OK &&
            (condition & (PCAN_CHANNEL_AVAILABLE | PCAN_CHANNEL_OCCUPIED))) {
            out.append({handleText(handles.at(i)), QStringLiteral("PCAN-USB %1 %2").arg(i + 1).arg(handleText(handles.at(i)))});
        }
    }
    return out;
}

bool PcanBackend::open(const CanSettings &settings, QString *error)
{
    close();
    const auto fail = [error](const QString &message) {
        if (error) {
            *error = message;
        }
        return false;
    };
    if (!loadApi(error)) {
        return false;
    }
    bool ok = false;
    const uint handle = settings.channel.trimmed().toUInt(&ok, 0);
    if (!ok || handle == 0 || handle > 0xFFFF) {
        return fail(tr("请先选择 PCAN 通道"));
    }
    std::optional<CanBitTiming::Custom> custom;
    if (!settings.customTiming.trimmed().isEmpty()) {
        custom = CanBitTiming::parseCustom(settings.customTiming, error);
        if (!custom) {
            return false;
        }
    }
    const Api &a = api();
    m_handle = TPCANHandle(handle);
    m_fd = settings.fd;
    // Listen-only is a pre-initialisation parameter; it is set either way, as a handle keeps it.
    quint32 listen = settings.listenOnly ? PCAN_PARAMETER_ON : PCAN_PARAMETER_OFF;
    const TPCANStatus listenStatus = a.setValue(m_handle, PCAN_LISTEN_ONLY, &listen, sizeof listen);
    TPCANStatus status = PCAN_ERROR_OK;
    QString rates;
    if (!settings.fd) {
        CanTiming timing = custom ? custom->nominal
                                  : CanBitTiming::compute(kClassicClock, settings.bitrate, settings.samplePoint,
                                                          CanBitTiming::Sja1000, CanBitTiming::PreferredNominalTq);
        if (!timing.isValid()) {
            return fail(tr("此适配器无法实现波特率 %1").arg(CanAdapter::bitrateText(settings.bitrate)));
        }
        QString why;
        if (custom && !CanBitTiming::check(timing, CanBitTiming::Sja1000, &why)) {
            return fail(why);
        }
        status = a.initialize(m_handle, CanBitTiming::sja1000Btr(timing), 0, 0, 0);
        rates = CanAdapter::bitrateText(timing.bitrate(kClassicClock));
    } else {
        const CanTiming nominal =
            custom ? custom->nominal
                   : CanBitTiming::compute(kFdClock, settings.bitrate, settings.samplePoint, CanBitTiming::PcanFdNominal,
                                           CanBitTiming::PreferredNominalTq);
        const CanTiming data =
            custom && custom->data ? *custom->data
                                   : CanBitTiming::compute(kFdClock, settings.dataBitrate, settings.dataSamplePoint,
                                                           CanBitTiming::PcanFdData, CanBitTiming::PreferredDataTq);
        if (!nominal.isValid()) {
            return fail(tr("此适配器无法实现波特率 %1").arg(CanAdapter::bitrateText(settings.bitrate)));
        }
        if (!data.isValid()) {
            return fail(tr("此适配器无法实现数据域波特率 %1").arg(CanAdapter::bitrateText(settings.dataBitrate)));
        }
        QString why;
        if (custom && (!CanBitTiming::check(nominal, CanBitTiming::PcanFdNominal, &why) ||
                       !CanBitTiming::check(data, CanBitTiming::PcanFdData, &why))) {
            return fail(why);
        }
        QByteArray bitrate = QStringLiteral("f_clock_mhz=%1,nom_brp=%2,nom_tseg1=%3,nom_tseg2=%4,nom_sjw=%5,"
                                            "data_brp=%6,data_tseg1=%7,data_tseg2=%8,data_sjw=%9")
                                 .arg(kFdClock / 1000000)
                                 .arg(nominal.brp)
                                 .arg(nominal.tseg1)
                                 .arg(nominal.tseg2)
                                 .arg(nominal.sjw)
                                 .arg(data.brp)
                                 .arg(data.tseg1)
                                 .arg(data.tseg2)
                                 .arg(data.sjw)
                                 .toLatin1();
        status = a.initializeFd(m_handle, bitrate.data());
        rates = CanAdapter::bitrateText(nominal.bitrate(kFdClock)) + QLatin1Char('/') +
                CanAdapter::bitrateText(data.bitrate(kFdClock));
    }
    if (status != PCAN_ERROR_OK) {
        return fail(tr("PCAN 通道 %1 打开失败: %2").arg(handleText(m_handle), errorText(status)));
    }
    if (settings.listenOnly && listenStatus != PCAN_ERROR_OK &&
        a.setValue(m_handle, PCAN_LISTEN_ONLY, &listen, sizeof listen) != PCAN_ERROR_OK) {
        a.uninitialize(m_handle);
        return fail(tr("此 PCAN 设备不支持只听模式"));
    }
#ifdef Q_OS_WIN
    m_receiveEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    m_wakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HANDLE event = static_cast<HANDLE>(m_receiveEvent);
    if (!m_receiveEvent || !m_wakeEvent ||
        a.setValue(m_handle, PCAN_RECEIVE_EVENT, &event, sizeof event) != PCAN_ERROR_OK) {
        // Without the event the worker polls, as on macOS.
        if (m_receiveEvent) {
            CloseHandle(static_cast<HANDLE>(m_receiveEvent));
        }
        m_receiveEvent = nullptr;
    }
#endif
    char name[MAX_LENGTH_HARDWARE_NAME + 1] = {};
    QString device = QStringLiteral("PCAN");
    if (a.getValue(m_handle, PCAN_HARDWARE_NAME, name, MAX_LENGTH_HARDWARE_NAME) == PCAN_ERROR_OK && name[0]) {
        device = QString::fromLatin1(name);
    }
    m_description = QStringLiteral("%1 %2 @ %3").arg(device, handleText(m_handle), rates);
    if (settings.listenOnly) {
        m_description += tr(" 只听");
    }
    m_open = true;
    startWorker();
    return true;
}

void PcanBackend::close()
{
    stopWorker();
    if (m_open) {
        api().uninitialize(m_handle);
        m_open = false;
    }
#ifdef Q_OS_WIN
    for (void **event : {&m_receiveEvent, &m_wakeEvent}) {
        if (*event) {
            CloseHandle(static_cast<HANDLE>(*event));
            *event = nullptr;
        }
    }
#endif
}

bool PcanBackend::readFrames(QList<CanFrame> &out, QString *error)
{
    const Api &a = api();
    for (int i = 0; i < kReadBatch; ++i) {
        CanFrame frame;
        TPCANStatus status = PCAN_ERROR_OK;
        TPCANMessageType type = 0;
        if (m_fd) {
            TPCANMsgFD message{};
            TPCANTimestampFD stamp = 0;
            status = a.readFd(m_handle, &message, &stamp);
            type = message.MSGTYPE;
            frame.id = message.ID;
            frame.data = QByteArray(reinterpret_cast<const char *>(message.DATA), Can::dlcToLength(message.DLC));
        } else {
            TPCANMsg message{};
            TPCANTimestamp stamp{};
            status = a.read(m_handle, &message, &stamp);
            type = message.MSGTYPE;
            frame.id = message.ID;
            frame.data = QByteArray(reinterpret_cast<const char *>(message.DATA), std::min<int>(message.LEN, 8));
        }
        if (status & PCAN_ERROR_QRCVEMPTY) {
            return true;
        }
        if (status & (PCAN_ERROR_ILLHANDLE | PCAN_ERROR_INITIALIZE | PCAN_ERROR_NODRIVER)) {
            if (error) {
                *error = tr("PCAN 通道已断开: %1").arg(errorText(status));
            }
            return false;
        }
        if (status != PCAN_ERROR_OK) {
            continue; // bus errors and overruns: the bus state shows them
        }
        if (type & (PCAN_MESSAGE_STATUS | PCAN_MESSAGE_ERRFRAME | PCAN_MESSAGE_ECHO)) {
            continue;
        }
        frame.extended = type & PCAN_MESSAGE_EXTENDED;
        frame.rtr = type & PCAN_MESSAGE_RTR;
        frame.fd = type & PCAN_MESSAGE_FD;
        frame.brs = type & PCAN_MESSAGE_BRS;
        out.append(frame);
    }
    return true;
}

bool PcanBackend::writeFrame(const CanFrame &frame, QString *error)
{
    TPCANMessageType type = frame.extended ? PCAN_MESSAGE_EXTENDED : PCAN_MESSAGE_STANDARD;
    if (frame.rtr) {
        type |= PCAN_MESSAGE_RTR;
    }
    TPCANStatus status = PCAN_ERROR_OK;
    if (m_fd) {
        TPCANMsgFD message{};
        message.ID = frame.id;
        message.MSGTYPE = type | (frame.fd ? PCAN_MESSAGE_FD : 0) | (frame.fd && frame.brs ? PCAN_MESSAGE_BRS : 0);
        message.DLC = quint8(Can::lengthToDlc(int(frame.data.size())));
        if (!frame.rtr) {
            std::memcpy(message.DATA, frame.data.constData(), std::min<size_t>(frame.data.size(), 64));
        }
        status = api().writeFd(m_handle, &message);
    } else {
        if (frame.fd) {
            if (error) {
                *error = tr("通道是经典 CAN，不能发送 CAN FD 报文（勾选 CAN FD 后重新连接）");
            }
            return false;
        }
        TPCANMsg message{};
        message.ID = frame.id;
        message.MSGTYPE = type;
        message.LEN = quint8(std::min<qsizetype>(frame.data.size(), 8));
        if (!frame.rtr) {
            std::memcpy(message.DATA, frame.data.constData(), message.LEN);
        }
        status = api().write(m_handle, &message);
    }
    if (status != PCAN_ERROR_OK) {
        if (error) {
            *error = tr("PCAN 发送失败: %1").arg(errorText(status));
        }
        return false;
    }
    return true;
}

CanBusState PcanBackend::readBusState()
{
    const TPCANStatus status = api().getStatus(m_handle);
    if (status & PCAN_ERROR_BUSOFF) {
        return CanBusState::BusOff;
    }
    if (status & PCAN_ERROR_BUSPASSIVE) {
        return CanBusState::Passive;
    }
    if (status & (PCAN_ERROR_BUSWARNING | PCAN_ERROR_BUSLIGHT)) {
        return CanBusState::Warning;
    }
    return CanBusState::Normal;
}

#ifdef Q_OS_WIN
void PcanBackend::waitForActivity(int ms)
{
    if (!m_receiveEvent) {
        CanPollingBackend::waitForActivity(ms);
        return;
    }
    // Woken by a frame arriving or by send(); the timeout only paces the bus-state check.
    HANDLE events[2] = {static_cast<HANDLE>(m_receiveEvent), static_cast<HANDLE>(m_wakeEvent)};
    WaitForMultipleObjects(2, events, FALSE, DWORD(std::max(ms, 50)));
}

void PcanBackend::wake()
{
    if (m_wakeEvent) {
        SetEvent(static_cast<HANDLE>(m_wakeEvent));
    }
}
#endif

} // namespace fhmi
