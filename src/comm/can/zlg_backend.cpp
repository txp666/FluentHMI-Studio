#include "comm/can/zlg_backend.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QHash>
#include <QtCore/QLibrary>

#ifdef Q_OS_WIN
#include <QtCore/qt_windows.h>
#endif

#include <algorithm>
#include <cstring>
#include <optional>

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

// What a PE file says about itself: the CPU it is built for and the DLLs it imports.
struct PeInfo
{
    quint16 machine = 0;
    QStringList imports;
};

constexpr quint16 kMachineX86 = 0x014C;
constexpr quint16 kMachineX64 = 0x8664;
constexpr quint16 kMachineArm64 = 0xAA64;

// The machine this build expects of ZLG's DLLs; 0 skips the check (ZLG ships x86 and x64 only, so on any other
// build there is nothing to compare with).
#if defined(Q_PROCESSOR_X86_64)
constexpr quint16 kBuildMachine = kMachineX64;
#else
constexpr quint16 kBuildMachine = 0;
#endif

quint32 readLe(const QByteArray &data, qsizetype offset, int size)
{
    if (offset < 0 || offset + size > data.size()) {
        return 0;
    }
    quint32 value = 0;
    for (int i = size - 1; i >= 0; --i) {
        value = (value << 8) | quint8(data[offset + i]);
    }
    return value;
}

// Reads the PE header and the import table by hand: this must work on a DLL that Windows refuses to load, and
// on the platforms where the checks are developed. Anything that does not fit the file gives std::nullopt, or
// an empty import list.
std::optional<PeInfo> readPe(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return std::nullopt;
    }
    const QByteArray data = file.readAll();
    if (data.size() < 0x40 || data[0] != 'M' || data[1] != 'Z') {
        return std::nullopt;
    }
    const qsizetype pe = readLe(data, 0x3C, 4);
    if (pe <= 0 || pe + 24 > data.size() || data.mid(pe, 4) != QByteArray("PE\0\0", 4)) {
        return std::nullopt;
    }
    PeInfo info;
    info.machine = quint16(readLe(data, pe + 4, 2));

    const int sectionCount = int(readLe(data, pe + 6, 2));
    const int optionalSize = int(readLe(data, pe + 20, 2));
    const qsizetype optional = pe + 24;
    const quint32 magic = readLe(data, optional, 2);
    // The import directory is the second data directory, after the optional header's fixed fields.
    const qsizetype directory = optional + (magic == 0x20B ? 112 : 96) + 8;
    if ((magic != 0x10B && magic != 0x20B) || directory + 8 > optional + optionalSize) {
        return info;
    }
    const quint32 importRva = readLe(data, directory, 4);
    if (importRva == 0) {
        return info;
    }
    const qsizetype sections = optional + optionalSize;
    const auto toOffset = [&](quint32 rva) -> qsizetype {
        for (int i = 0; i < sectionCount; ++i) {
            const qsizetype header = sections + qsizetype(i) * 40;
            const quint32 virtualSize = readLe(data, header + 8, 4);
            const quint32 virtualAddress = readLe(data, header + 12, 4);
            const quint32 rawSize = readLe(data, header + 16, 4);
            const quint32 rawPointer = readLe(data, header + 20, 4);
            if (rva >= virtualAddress && rva < virtualAddress + std::max(virtualSize, rawSize)) {
                return qsizetype(rawPointer) + (rva - virtualAddress);
            }
        }
        return -1;
    };
    const qsizetype table = toOffset(importRva);
    for (qsizetype entry = table; table >= 0 && entry + 20 <= data.size(); entry += 20) {
        const quint32 nameRva = readLe(data, entry + 12, 4);
        if (nameRva == 0) {
            break;
        }
        const qsizetype name = toOffset(nameRva);
        if (name < 0 || name >= data.size()) {
            break;
        }
        const qsizetype end = data.indexOf('\0', name);
        info.imports << QString::fromLatin1(data.mid(name, (end < 0 ? data.size() : end) - name));
    }
    return info;
}

// How a message names a file: relative to the program's folder (kerneldlls\USBCANFD.dll, zlgcan.dll), since the
// card shows the message in a few wrapped lines; a file elsewhere keeps its whole path.
QString shownPath(const QString &path)
{
    const QString relative = QDir(QCoreApplication::applicationDirPath()).relativeFilePath(path);
    return QDir::toNativeSeparators(relative.startsWith(QLatin1String("..")) ? path : relative);
}

QString machineText(quint16 machine)
{
    switch (machine) {
    case kMachineX86:
        return ZlgBackend::tr("32 位");
    case kMachineX64:
        return ZlgBackend::tr("64 位 x64");
    case kMachineArm64:
        return ZlgBackend::tr("64 位 ARM64");
    default:
        return ZlgBackend::tr("未知架构 0x%1").arg(machine, 4, 16, QLatin1Char('0'));
    }
}

// The runtime an imported DLL name points at, empty when it is not one ZLG's DLLs need.
QString runtimeHint(const QStringList &imports)
{
    for (const QString &name : imports) {
        const QString lower = name.toLower();
        if (lower.startsWith(QLatin1String("msvcr120")) || lower.startsWith(QLatin1String("msvcp120"))) {
            return ZlgBackend::tr("Visual C++ 2013 运行库 (x64)");
        }
        if (lower.startsWith(QLatin1String("msvcr90")) || lower.startsWith(QLatin1String("msvcp90")) ||
            lower.startsWith(QLatin1String("mfc90"))) {
            return ZlgBackend::tr("Visual C++ 2008 SP1 运行库 (x64，含 MFC)");
        }
    }
    return {};
}

// The kernel DLL file name that kerneldlls/dll_cfg.ini gives a device type, empty when none does. Lines look like
// "2=USBCANFD.dll 41 32 1" (index, dll, device type, max devices, flag) and may sit in any section.
QString kernelDllFor(const QString &iniPath, quint32 deviceType)
{
    QFile file(iniPath);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    const QStringList lines = QString::fromLocal8Bit(file.readAll()).split(QLatin1Char('\n'));
    for (const QString &raw : lines) {
        const QString line = raw.trimmed();
        const qsizetype equals = line.indexOf(QLatin1Char('='));
        if (equals < 0 || line.startsWith(QLatin1Char(';')) || line.startsWith(QLatin1Char('#'))) {
            continue;
        }
        const QStringList fields = line.mid(equals + 1).simplified().split(QLatin1Char(' '));
        bool numeric = false;
        if (fields.size() >= 2 && fields[1].toUInt(&numeric) == deviceType && numeric) {
            return fields[0];
        }
    }
    return {};
}

// What kerneldlls/dll_cfg.ini of the loaded zlgcan.dll says about a model, looked at before ZCAN_OpenDevice
// reports only a null handle. Needs no Windows API.
struct KernelDll
{
    QString problem; // the first problem found, empty when there is none
    QString name;    // the kernel DLL the ini gives the model's device type
    QString path;    // that DLL in kerneldlls, empty when it is not there
};

KernelDll findKernelDll(const QString &folder, const QString &model, quint32 deviceType)
{
    KernelDll dll;
    const QDir kernelDir(QDir(folder).filePath(QStringLiteral("kerneldlls")));
    const QString ini = kernelDir.filePath(QStringLiteral("dll_cfg.ini"));
    if (!QFileInfo::exists(ini)) {
        // clang-format off
        dll.problem = ZlgBackend::tr("zlgcan.dll 旁边没有 kerneldlls\\dll_cfg.ini：请把接口函数库 zlgcan_x64 文件夹里的整个 kerneldlls 文件夹放到 %1")
            .arg(QDir::toNativeSeparators(folder));
        // clang-format on
        return dll;
    }
    dll.name = kernelDllFor(ini, deviceType);
    if (dll.name.isEmpty()) {
        // clang-format off
        dll.problem = ZlgBackend::tr("这版 zlgcan.dll 不认识 %1（设备类型 %2）：kerneldlls\\dll_cfg.ini 里没有它，请换用新版接口函数库")
            .arg(model)
            .arg(deviceType);
        // clang-format on
        return dll;
    }
    const QStringList files = kernelDir.entryList(QDir::Files);
    for (const QString &file : files) {
        if (file.compare(dll.name, Qt::CaseInsensitive) == 0) {
            dll.path = kernelDir.filePath(file);
            break;
        }
    }
    return dll;
}

// Why the kerneldlls copy of a kernel DLL cannot load here: it is built for another CPU. Empty when it is not;
// expectedMachine 0 skips the check.
QString machineProblem(const QString &path, quint16 expectedMachine)
{
    const std::optional<PeInfo> pe = readPe(path);
    if (expectedMachine == 0 || !pe || pe->machine == expectedMachine) {
        return {};
    }
    return ZlgBackend::tr("%1 为 %2，本程序为 %3：kerneldlls 里的文件必须来自接口函数库的 zlgcan_x64 文件夹")
        .arg(shownPath(path), machineText(pe->machine), machineText(expectedMachine));
}

#ifdef Q_OS_WIN
QString modulePath(HMODULE module)
{
    wchar_t buffer[4096];
    const DWORD length = GetModuleFileNameW(module, buffer, DWORD(std::size(buffer)));
    return QDir::fromNativeSeparators(QString::fromWCharArray(buffer, int(length)));
}

// Loads the model's kernel DLL as zlgcan.dll does (read off its code): LoadLibraryA with the bare name first,
// which finds a DLL a device driver installed in System32 or one beside the program, then the kerneldlls copy.
// Returns the problem, or empty with *used set to the file that loaded. The module stays loaded, so zlgcan.dll's
// own LoadLibraryA gets this same one.
QString loadKernelDll(const KernelDll &dll, const QString &model, QString *used)
{
    if (HMODULE module = LoadLibraryA(dll.name.toLocal8Bit().constData())) {
        *used = modulePath(module);
        return {};
    }
    if (dll.path.isEmpty()) {
        // clang-format off
        return ZlgBackend::tr("找不到 %1，%2 需要它：接口函数库里没有这个文件，系统里也没有。请确认已安装 %2 的设备驱动，或把它放进 kerneldlls 文件夹")
            .arg(dll.name, model);
        // clang-format on
    }
    if (const QString problem = machineProblem(dll.path, kBuildMachine); !problem.isEmpty()) {
        return problem;
    }
    if (HMODULE module = LoadLibraryA(QDir::toNativeSeparators(dll.path).toLocal8Bit().constData())) {
        *used = modulePath(module);
        return {};
    }
    const QString reason = qt_error_string(int(GetLastError()));
    const std::optional<PeInfo> pe = readPe(dll.path);
    const QString runtime = pe ? runtimeHint(pe->imports) : QString();
    if (!runtime.isEmpty()) {
        return ZlgBackend::tr("无法加载 %1：电脑缺少 %2，请从微软官网安装后重试").arg(shownPath(dll.path), runtime);
    }
    return ZlgBackend::tr("无法加载 %1：%2").arg(shownPath(dll.path), reason);
}
#endif

// Why a zlgcan.dll that is there will not load, from its own header.
QString loadFailure(const QString &path, const QString &reason)
{
    const std::optional<PeInfo> pe = readPe(path);
    if (pe && kBuildMachine != 0 && pe->machine != kBuildMachine) {
        // clang-format off
        return ZlgBackend::tr("找到了 %1 但无法加载：它为 %2，本程序为 %3，请换用接口函数库 zlgcan_x64 文件夹里的 zlgcan.dll 和 kerneldlls")
            .arg(shownPath(path), machineText(pe->machine), machineText(kBuildMachine));
        // clang-format on
    }
    const QString runtime = pe ? runtimeHint(pe->imports) : QString();
    if (!runtime.isEmpty()) {
        return ZlgBackend::tr("找到了 %1 但无法加载：电脑缺少 %2，请从微软官网安装后重试")
            .arg(shownPath(path), runtime);
    }
    return ZlgBackend::tr("找到了 %1 但无法加载：%2").arg(shownPath(path), reason);
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
    QString present;       // the first candidate that is a file, which then failed to load
    QString presentReason; // and what QLibrary said about it
    for (const QString &candidate : candidates) {
        a.library.setFileName(candidate);
        if (a.library.load()) {
            loaded = true;
            break;
        }
        if (present.isEmpty() && QFileInfo::exists(candidate)) {
            present = candidate;
            presentReason = a.library.errorString();
        }
    }
    if (!loaded) {
        if (!present.isEmpty()) {
            if (error) {
                *error = loadFailure(present, presentReason);
            }
            return false;
        }
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
        // zlgcan.dll finds kerneldlls beside itself (GetModuleFileNameA on its own module), whatever the working
        // directory is. ZCAN_OpenDevice only returns null when something goes wrong, so look first and name the cause.
        const KernelDll dll = findKernelDll(a.folder, settings.zlgModel, model->deviceType);
        QString problem = dll.problem;
        QString used = dll.path.isEmpty() ? dll.name : dll.path; // the kernel DLL that will be used
#ifdef Q_OS_WIN
        if (problem.isEmpty()) {
            problem = loadKernelDll(dll, settings.zlgModel, &used);
        }
#else
        if (problem.isEmpty() && !dll.path.isEmpty()) {
            problem = machineProblem(dll.path, kBuildMachine);
        }
#endif
        if (!problem.isEmpty()) {
            openDevices().remove(key);
            return fail(problem);
        }
        shared.handle = a.openDevice(model->deviceType, UINT(settings.deviceIndex), 0);
        if (!shared.handle) {
            openDevices().remove(key);
            // clang-format off
            // The kernel DLL that loaded is named: an old one in System32 is used before the kerneldlls copy.
            return fail(tr("无法打开 %1（设备 %2）：zlgcan.dll 和 %3 都已加载，请检查设备是否已连接、是否被 ZCANPRO 等其他程序占用、驱动是否与这版接口函数库匹配")
                            .arg(settings.zlgModel)
                            .arg(settings.deviceIndex)
                            .arg(shownPath(used)));
            // clang-format on
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
