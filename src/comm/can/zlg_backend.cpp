#include "comm/can/zlg_backend.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QHash>
#include <QtCore/QLibrary>

#include <algorithm>
#include <cstring>

namespace fhmi {

using namespace zlg;

namespace {

constexpr UINT kReadBatch = 100;
// ZCAN_Receive's wait_time (ms) when frames are already waiting; -1 would block until some arrive.
constexpr int kReceiveWaitMs = 10;

struct Api
{
    QLibrary library;
    bool loaded = false;
    QString folder; // where zlgcan.dll and its kerneldlls folder are
    ZCAN_OpenDevice_t openDevice = nullptr;
    ZCAN_CloseDevice_t closeDevice = nullptr;
    ZCAN_InitCAN_t initCan = nullptr;
    ZCAN_StartCAN_t startCan = nullptr;
    ZCAN_ResetCAN_t resetCan = nullptr;
    ZCAN_ReadChannelErrInfo_t readErrInfo = nullptr;
    ZCAN_ReadChannelStatus_t readStatus = nullptr;
    ZCAN_GetReceiveNum_t receiveNum = nullptr;
    ZCAN_Transmit_t transmit = nullptr;
    ZCAN_Receive_t receive = nullptr;
    ZCAN_TransmitFD_t transmitFd = nullptr;
    ZCAN_ReceiveFD_t receiveFd = nullptr;
    ZCAN_SetValue_t setValue = nullptr;
};

Api &api()
{
    static Api instance;
    return instance;
}

bool loadApi(QString *error)
{
    Api &a = api();
    if (a.loaded) {
        return true;
    }
    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList candidates{QDir(appDir).filePath(QStringLiteral("zlgcan.dll")),
                                 QDir(appDir).filePath(QStringLiteral("zlgcan/zlgcan.dll")),
                                 QStringLiteral("zlgcan")};
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
            // clang-format off
            *error = ZlgBackend::tr("找不到 zlgcan.dll：请从致远电子官网下载 64 位的 CAN 接口卡二次开发接口函数库，把 zlgcan.dll 和 kerneldlls 文件夹放到 %1 或其中的 zlgcan 文件夹")
                         .arg(QDir::toNativeSeparators(appDir));
            // clang-format on
        }
        return false;
    }
    a.folder = QFileInfo(a.library.fileName()).absolutePath();
    QString absent;
    const auto resolve = [&a, &absent](auto &function, const char *name) {
        function = reinterpret_cast<std::remove_reference_t<decltype(function)>>(a.library.resolve(name));
        if (!function && absent.isEmpty()) {
            absent = QLatin1String(name);
        }
    };
    resolve(a.openDevice, "ZCAN_OpenDevice");
    resolve(a.closeDevice, "ZCAN_CloseDevice");
    resolve(a.initCan, "ZCAN_InitCAN");
    resolve(a.startCan, "ZCAN_StartCAN");
    resolve(a.resetCan, "ZCAN_ResetCAN");
    resolve(a.readErrInfo, "ZCAN_ReadChannelErrInfo");
    resolve(a.readStatus, "ZCAN_ReadChannelStatus");
    resolve(a.receiveNum, "ZCAN_GetReceiveNum");
    resolve(a.transmit, "ZCAN_Transmit");
    resolve(a.receive, "ZCAN_Receive");
    resolve(a.transmitFd, "ZCAN_TransmitFD");
    resolve(a.receiveFd, "ZCAN_ReceiveFD");
    resolve(a.setValue, "ZCAN_SetValue");
    if (!absent.isEmpty()) {
        a.library.unload();
        if (error) {
            *error = ZlgBackend::tr("zlgcan.dll 缺少函数 %1，请换用新版接口函数库").arg(absent);
        }
        return false;
    }
    a.loaded = true;
    return true;
}

// The devices open now, by type and index, with how many channels use each: a device is opened once however
// many cards use its channels (GUI thread only).
struct SharedDevice
{
    DEVICE_HANDLE handle = nullptr;
    int users = 0;
};

QHash<quint64, SharedDevice> &openDevices()
{
    static QHash<quint64, SharedDevice> devices;
    return devices;
}

quint64 deviceKey(UINT type, int index)
{
    return (quint64(type) << 32) | quint32(index);
}

bool contains(const quint32 *begin, const quint32 *end, quint32 value)
{
    return std::find(begin, end, value) != end;
}

const Model *modelNamed(const QString &name)
{
    const QByteArray latin = name.toLatin1();
    return findModel(latin.constData(), latin.size());
}

} // namespace

ZlgBackend::~ZlgBackend()
{
    close();
}

CanAdapterCaps ZlgBackend::caps(const QString &model)
{
    CanAdapterCaps caps;
    caps.channelList = false; // no device listing: the model, device index and channel are chosen
    caps.samplePoint = false;
    caps.dataSamplePoint = false;
    caps.customTiming = false;
    const Model *found = modelNamed(model);
    const Family family = found ? found->family : Family::CanFd;
    caps.fd = family == Family::CanFd;
    caps.termination = found && found->termination;
    caps.fixedTimingReason = family == Family::CanFd
                                 ? tr("采样点由 ZLG 设备决定（仲裁域 80%，数据域 80%，5M 时 75%）")
                                 : tr("采样点由 ZLG 设备决定");
#ifndef Q_OS_WIN
    caps.supported = false;
#endif
    return caps;
}

bool ZlgBackend::open(const CanSettings &settings, QString *error)
{
    close();
    const auto fail = [error](const QString &message) {
        if (error) {
            *error = message;
        }
        return false;
    };
    const Model *model = modelNamed(settings.zlgModel);
    if (!model) {
        return fail(tr("未知的 ZLG 型号: %1").arg(settings.zlgModel));
    }
    if (model->channels > 0 && settings.channelIndex >= model->channels) {
        return fail(tr("%1 只有 %2 路 CAN（通道 0–%3）").arg(settings.zlgModel).arg(model->channels).arg(model->channels - 1));
    }
    if (settings.fd && model->family != Family::CanFd) {
        return fail(tr("%1 不支持 CAN FD").arg(settings.zlgModel));
    }
    // The bitrates the model's property file offers; anything else the device cannot be set to.
    if (model->family == Family::CanFd) {
        if (!contains(std::begin(kFdArbitrationRates), std::end(kFdArbitrationRates), settings.bitrate)) {
            return fail(tr("%1 不支持波特率 %2").arg(settings.zlgModel, CanAdapter::bitrateText(settings.bitrate)));
        }
        if (settings.fd && !contains(std::begin(kFdDataRates), std::end(kFdDataRates), settings.dataBitrate)) {
            return fail(
                tr("%1 不支持数据域波特率 %2").arg(settings.zlgModel, CanAdapter::bitrateText(settings.dataBitrate)));
        }
    } else if (!contains(std::begin(kClassicRates), std::end(kClassicRates), settings.bitrate)) {
        return fail(tr("%1 不支持波特率 %2").arg(settings.zlgModel, CanAdapter::bitrateText(settings.bitrate)));
    }
    if (!loadApi(error)) {
        return false;
    }
    Api &a = api();
    const quint64 key = deviceKey(model->deviceType, settings.deviceIndex);
    SharedDevice &shared = openDevices()[key];
    if (!shared.handle) {
        // zlgcan.dll loads the kernel DLL of the device from its kerneldlls folder; ZLG's demos run from the folder
        // that holds both, and the manuals do not say whether it looks beside itself or in the working directory.
        const QString previous = QDir::currentPath();
        QDir::setCurrent(a.folder);
        shared.handle = a.openDevice(model->deviceType, UINT(settings.deviceIndex), 0);
        QDir::setCurrent(previous);
        if (!shared.handle) {
            openDevices().remove(key);
            return fail(tr("无法打开 %1（设备 %2）：设备未连接、驱动未装或被其他程序占用")
                            .arg(settings.zlgModel)
                            .arg(settings.deviceIndex));
        }
    }
    ++shared.users;
    m_model = model;
    m_device = shared.handle;
    m_deviceIndex = settings.deviceIndex;
    const auto setValue = [this, &a, &settings](const char *name, const QString &value) {
        const QByteArray path = QByteArray::number(settings.channelIndex) + '/' + name;
        const QByteArray text = value.toLatin1();
        return a.setValue(m_device, path.constData(), text.constData()) == STATUS_OK;
    };
    ZCAN_CHANNEL_INIT_CONFIG config;
    std::memset(&config, 0, sizeof config);
    const BYTE mode = settings.listenOnly ? 1 : 0; // 0 normal, 1 listen only (work_mode)
    if (model->family == Family::CanFd) {
        if (!setValue("canfd_abit_baud_rate", QString::number(settings.bitrate)) ||
            !setValue("canfd_dbit_baud_rate", QString::number(settings.dataBitrate))) {
            releaseDevice();
            return fail(tr("%1 拒绝了波特率设置").arg(settings.zlgModel));
        }
        config.can_type = TYPE_CANFD;
        config.config.canfd.mode = mode;
    } else {
        const bool rateSet = setValue("baud_rate", QString::number(settings.bitrate));
        if (model->family == Family::EU && !rateSet) {
            releaseDevice();
            return fail(tr("%1 拒绝了波特率设置").arg(settings.zlgModel));
        }
        config.can_type = TYPE_CAN;
        // ZLG recommends the mask 0xFFFFFFFF, everything received (UM01010101, VCI_INIT_CONFIG).
        config.config.can.acc_code = 0;
        config.config.can.acc_mask = 0xFFFFFFFF;
        config.config.can.filter = 0;
        config.config.can.mode = mode;
        if (model->family == Family::Sja1000) {
            for (const Btr &btr : kBtrTable) {
                if (btr.bitrate == settings.bitrate) {
                    config.config.can.timing0 = btr.timing0;
                    config.config.can.timing1 = btr.timing1;
                }
            }
        }
    }
    m_channel = a.initCan(m_device, UINT(settings.channelIndex), &config);
    if (!m_channel) {
        releaseDevice();
        return fail(tr("%1 通道 %2 初始化失败").arg(settings.zlgModel).arg(settings.channelIndex));
    }
    QStringList notes;
    if (model->termination && !setValue("initenal_resistance", settings.termination ? QStringLiteral("1")
                                                                                     : QStringLiteral("0"))) {
        notes << tr("终端电阻设置失败");
    }
    if (a.startCan(m_channel) != STATUS_OK) {
        a.resetCan(m_channel);
        m_channel = nullptr;
        releaseDevice();
        return fail(tr("%1 通道 %2 启动失败").arg(settings.zlgModel).arg(settings.channelIndex));
    }
    QString rates = CanAdapter::bitrateText(settings.bitrate);
    if (settings.fd) {
        rates += QLatin1Char('/') + CanAdapter::bitrateText(settings.dataBitrate);
    }
    m_description = QStringLiteral("%1 #%2 CAN%3 @ %4").arg(settings.zlgModel).arg(settings.deviceIndex).arg(
        settings.channelIndex).arg(rates);
    if (settings.listenOnly) {
        m_description += tr(" 只听");
    }
    startWorker();
    if (!notes.isEmpty()) {
        emit sendFailed(notes.join(QStringLiteral("; ")));
    }
    return true;
}

void ZlgBackend::close()
{
    stopWorker();
    if (m_channel) {
        api().resetCan(m_channel);
        m_channel = nullptr;
    }
    releaseDevice();
}

void ZlgBackend::releaseDevice()
{
    if (!m_device || !m_model) {
        return;
    }
    const quint64 key = deviceKey(m_model->deviceType, m_deviceIndex);
    auto it = openDevices().find(key);
    if (it != openDevices().end() && --it->users <= 0) {
        api().closeDevice(it->handle);
        openDevices().erase(it);
    }
    m_device = nullptr;
}

bool ZlgBackend::readFrames(QList<CanFrame> &out, QString *)
{
    const Api &a = api();
    if (UINT count = a.receiveNum(m_channel, BYTE(TYPE_CAN)); count > 0) {
        ZCAN_Receive_Data buffer[kReadBatch];
        const UINT read = a.receive(m_channel, buffer, std::min(count, kReadBatch), kReceiveWaitMs);
        for (UINT i = 0; i < read && i < kReadBatch; ++i) {
            const can_frame &raw = buffer[i].frame;
            if (raw.can_id & CAN_ERR_FLAG) {
                continue;
            }
            CanFrame frame;
            frame.id = raw.can_id & CAN_EFF_MASK;
            frame.extended = raw.can_id & CAN_EFF_FLAG;
            frame.rtr = raw.can_id & CAN_RTR_FLAG;
            frame.data = QByteArray(reinterpret_cast<const char *>(raw.data), std::min<int>(raw.can_dlc, 8));
            out.append(frame);
        }
    }
    if (m_model->family != Family::CanFd) {
        return true;
    }
    if (UINT count = a.receiveNum(m_channel, BYTE(TYPE_CANFD)); count > 0) {
        ZCAN_ReceiveFD_Data buffer[kReadBatch];
        const UINT read = a.receiveFd(m_channel, buffer, std::min(count, kReadBatch), kReceiveWaitMs);
        for (UINT i = 0; i < read && i < kReadBatch; ++i) {
            const canfd_frame &raw = buffer[i].frame;
            if (raw.can_id & CAN_ERR_FLAG) {
                continue;
            }
            CanFrame frame;
            frame.id = raw.can_id & CAN_EFF_MASK;
            frame.extended = raw.can_id & CAN_EFF_FLAG;
            frame.fd = true;
            frame.brs = raw.flags & CANFD_BRS;
            frame.data = QByteArray(reinterpret_cast<const char *>(raw.data), std::min<int>(raw.len, 64));
            out.append(frame);
        }
    }
    return true;
}

bool ZlgBackend::writeFrame(const CanFrame &frame, QString *error)
{
    // canframe.h MAKE_CAN_ID(id, eff, rtr, err)
    const UINT id = frame.id | (frame.extended ? CAN_EFF_FLAG : 0) | (frame.rtr ? CAN_RTR_FLAG : 0);
    UINT sent = 0;
    if (frame.fd) {
        ZCAN_TransmitFD_Data data;
        std::memset(&data, 0, sizeof data);
        data.frame.can_id = id;
        data.frame.len = BYTE(std::min<qsizetype>(frame.data.size(), 64));
        data.frame.flags = frame.brs ? CANFD_BRS : 0;
        std::memcpy(data.frame.data, frame.data.constData(), data.frame.len);
        data.transmit_type = 0; // normal sending, retried on failure (UM01010101, SendType 0)
        sent = api().transmitFd(m_channel, &data, 1);
    } else {
        ZCAN_Transmit_Data data;
        std::memset(&data, 0, sizeof data);
        data.frame.can_id = id;
        data.frame.can_dlc = BYTE(std::min<qsizetype>(frame.data.size(), 8));
        if (!frame.rtr) {
            std::memcpy(data.frame.data, frame.data.constData(), data.frame.can_dlc);
        }
        data.transmit_type = 0;
        sent = api().transmit(m_channel, &data, 1);
    }
    if (sent != 1) {
        if (error) {
            *error = tr("ZLG 发送失败（%1）").arg(Can::idText(frame.id, frame.extended));
        }
        return false;
    }
    return true;
}

CanBusState ZlgBackend::readBusState()
{
    const Api &a = api();
    ZCAN_CHANNEL_ERR_INFO info;
    std::memset(&info, 0, sizeof info);
    const bool hasInfo = a.readErrInfo(m_channel, &info) == STATUS_OK;
    if (hasInfo && (info.error_code & ZCAN_ERROR_CAN_BUSOFF)) {
        return CanBusState::BusOff;
    }
    ZCAN_CHANNEL_STATUS status;
    std::memset(&status, 0, sizeof status);
    if (a.readStatus(m_channel, &status) == STATUS_OK) {
        // The error counters, as the CAN standard draws the lines.
        const int counter = std::max(status.regRECounter, status.regTECounter);
        return counter >= 128 ? CanBusState::Passive : counter >= 96 ? CanBusState::Warning : CanBusState::Normal;
    }
    if (hasInfo && (info.error_code & ZCAN_ERROR_CAN_PASSIVE)) {
        return CanBusState::Passive;
    }
    if (hasInfo && (info.error_code & ZCAN_ERROR_CAN_ERRALARM)) {
        return CanBusState::Warning;
    }
    return CanBusState::Normal;
}

} // namespace fhmi
