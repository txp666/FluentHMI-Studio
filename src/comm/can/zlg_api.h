#pragma once

// The part of ZLG's zlgcan library this program uses, copied from its headers zlgcan.h, canframe.h and typedef.h
// (the Windows x64 "zlgcan二次开发接口函数库" package; the headers carry no version line, the package on
// zlgcan.com is dated 2022.02.24). Device types are those of zlgcan.h; which product is which type follows ZLG's
// manual UM01010101 V3.15 (2021/01/21), table 2.1, and the USBCAN-I-mini manual (V1.02). Channel counts and the
// property names are those of the device property files ZLG ships in kerneldlls/devices_property (usbcan1.xml,
// usbcan2.xml, usbcan-e-u.xml, usbcan-2e-u.xml, usbcan-4e-u.xml, usbcan-8e-u.xml, usbcanfd-100u.xml,
// usbcanfd-200u.xml, usbcanfd-400u.xml, usbcanfd-800u.xml).
//
// The vendor headers and DLLs are not part of this repository; zlgcan.dll is loaded with QLibrary at connect time.

#include <QtCore/QtGlobal>

namespace fhmi::zlg {

// typedef.h
using BYTE = quint8;
using UINT = quint32;
using USHORT = quint16;
using UINT64 = quint64;

// zlgcan.h
using DEVICE_HANDLE = void *;
using CHANNEL_HANDLE = void *;
constexpr UINT STATUS_ERR = 0;
constexpr UINT STATUS_OK = 1;
constexpr UINT TYPE_CAN = 0;
constexpr UINT TYPE_CANFD = 1;

constexpr UINT ZCAN_USBCAN1 = 3;
constexpr UINT ZCAN_USBCAN2 = 4;
constexpr UINT ZCAN_USBCAN_E_U = 20;
constexpr UINT ZCAN_USBCAN_2E_U = 21;
constexpr UINT ZCAN_USBCAN_4E_U = 31;
constexpr UINT ZCAN_USBCAN_8E_U = 34;
constexpr UINT ZCAN_USBCANFD_200U = 41;
constexpr UINT ZCAN_USBCANFD_100U = 42;
constexpr UINT ZCAN_USBCANFD_MINI = 43;
constexpr UINT ZCAN_USBCANFD_800U = 59;
constexpr UINT ZCAN_USBCANFD_400U = 76;

constexpr UINT ZCAN_ERROR_CAN_OVERFLOW = 0x0001;
constexpr UINT ZCAN_ERROR_CAN_ERRALARM = 0x0002;
constexpr UINT ZCAN_ERROR_CAN_PASSIVE = 0x0004;
constexpr UINT ZCAN_ERROR_CAN_LOSE = 0x0008;
constexpr UINT ZCAN_ERROR_CAN_BUSERR = 0x0010;
constexpr UINT ZCAN_ERROR_CAN_BUSOFF = 0x0020;

// canframe.h
constexpr UINT CAN_EFF_FLAG = 0x80000000U;
constexpr UINT CAN_RTR_FLAG = 0x40000000U;
constexpr UINT CAN_ERR_FLAG = 0x20000000U;
constexpr UINT CAN_EFF_MASK = 0x1FFFFFFFU;
constexpr BYTE CANFD_BRS = 0x01;

struct can_frame
{
    UINT can_id; // 32 bit MAKE_CAN_ID + EFF/RTR/ERR flags
    BYTE can_dlc; // frame payload length in byte (0 .. CAN_MAX_DLEN)
    BYTE __pad;
    BYTE __res0;
    BYTE __res1;
    BYTE data[8];
};

struct canfd_frame
{
    UINT can_id; // 32 bit MAKE_CAN_ID + EFF/RTR/ERR flags
    BYTE len;    // frame payload length in byte
    BYTE flags;  // additional flags for CAN FD
    BYTE __res0;
    BYTE __res1;
    BYTE data[64];
};

// zlgcan.h declares these before its #pragma pack(push, 1), so they have the compiler's default layout.
struct ZCAN_CHANNEL_INIT_CONFIG
{
    UINT can_type; // TYPE_CAN or TYPE_CANFD
    union
    {
        struct
        {
            UINT acc_code;
            UINT acc_mask;
            UINT reserved;
            BYTE filter;
            BYTE timing0;
            BYTE timing1;
            BYTE mode;
        } can;
        struct
        {
            UINT acc_code;
            UINT acc_mask;
            UINT abit_timing;
            UINT dbit_timing;
            UINT brp;
            BYTE filter;
            BYTE mode;
            USHORT pad;
            UINT reserved;
        } canfd;
    } config;
};

struct ZCAN_CHANNEL_ERR_INFO
{
    UINT error_code;
    BYTE passive_ErrData[3];
    BYTE arLost_ErrData;
};

struct ZCAN_CHANNEL_STATUS
{
    BYTE errInterrupt;
    BYTE regMode;
    BYTE regStatus;
    BYTE regALCapture;
    BYTE regECCapture;
    BYTE regEWLimit;
    BYTE regRECounter;
    BYTE regTECounter;
    UINT Reserved;
};

struct ZCAN_Transmit_Data
{
    can_frame frame;
    UINT transmit_type;
};

struct ZCAN_TransmitFD_Data
{
    canfd_frame frame;
    UINT transmit_type;
};

struct ZCAN_Receive_Data
{
    can_frame frame;
    UINT64 timestamp; // us
};

struct ZCAN_ReceiveFD_Data
{
    canfd_frame frame;
    UINT64 timestamp; // us
};

// The sizes these member lists give with default alignment, so a slip in copying them cannot go unnoticed.
static_assert(sizeof(can_frame) == 16, "zlgcan can_frame");
static_assert(sizeof(canfd_frame) == 72, "zlgcan canfd_frame");
static_assert(sizeof(ZCAN_CHANNEL_INIT_CONFIG) == 32, "zlgcan ZCAN_CHANNEL_INIT_CONFIG");
static_assert(sizeof(ZCAN_Transmit_Data) == 20, "zlgcan ZCAN_Transmit_Data");
static_assert(sizeof(ZCAN_TransmitFD_Data) == 76, "zlgcan ZCAN_TransmitFD_Data");
static_assert(sizeof(ZCAN_Receive_Data) == 24, "zlgcan ZCAN_Receive_Data");
static_assert(sizeof(ZCAN_ReceiveFD_Data) == 80, "zlgcan ZCAN_ReceiveFD_Data");

// zlgcan.h: FUNC_CALL is __stdcall on Windows (meaningless on x64).
#ifdef Q_OS_WIN
#define FHMI_ZLG_CALL __stdcall
#else
#define FHMI_ZLG_CALL
#endif

using ZCAN_OpenDevice_t = DEVICE_HANDLE(FHMI_ZLG_CALL *)(UINT device_type, UINT device_index, UINT reserved);
using ZCAN_CloseDevice_t = UINT(FHMI_ZLG_CALL *)(DEVICE_HANDLE device_handle);
using ZCAN_InitCAN_t = CHANNEL_HANDLE(FHMI_ZLG_CALL *)(DEVICE_HANDLE device_handle, UINT can_index,
                                                       ZCAN_CHANNEL_INIT_CONFIG *pInitConfig);
using ZCAN_StartCAN_t = UINT(FHMI_ZLG_CALL *)(CHANNEL_HANDLE channel_handle);
using ZCAN_ResetCAN_t = UINT(FHMI_ZLG_CALL *)(CHANNEL_HANDLE channel_handle);
using ZCAN_ReadChannelErrInfo_t = UINT(FHMI_ZLG_CALL *)(CHANNEL_HANDLE channel_handle,
                                                        ZCAN_CHANNEL_ERR_INFO *pErrInfo);
using ZCAN_ReadChannelStatus_t = UINT(FHMI_ZLG_CALL *)(CHANNEL_HANDLE channel_handle,
                                                       ZCAN_CHANNEL_STATUS *pCANStatus);
using ZCAN_GetReceiveNum_t = UINT(FHMI_ZLG_CALL *)(CHANNEL_HANDLE channel_handle, BYTE type);
using ZCAN_Transmit_t = UINT(FHMI_ZLG_CALL *)(CHANNEL_HANDLE channel_handle, ZCAN_Transmit_Data *pTransmit, UINT len);
using ZCAN_Receive_t = UINT(FHMI_ZLG_CALL *)(CHANNEL_HANDLE channel_handle, ZCAN_Receive_Data *pReceive, UINT len,
                                             int wait_time);
using ZCAN_TransmitFD_t = UINT(FHMI_ZLG_CALL *)(CHANNEL_HANDLE channel_handle, ZCAN_TransmitFD_Data *pTransmit,
                                                UINT len);
using ZCAN_ReceiveFD_t = UINT(FHMI_ZLG_CALL *)(CHANNEL_HANDLE channel_handle, ZCAN_ReceiveFD_Data *pReceive, UINT len,
                                               int wait_time);
using ZCAN_SetValue_t = UINT(FHMI_ZLG_CALL *)(DEVICE_HANDLE device_handle, const char *path, const void *value);

// How a model is set up. The SJA1000 family (USBCAN-I/II) takes BTR0/BTR1 in the init config (ZLG's Timing0/
// Timing1, UM01010101 2.5.5) and also has a baud_rate property; the E-U family only the property "n/baud_rate";
// the CAN FD family the properties n/canfd_abit_baud_rate and n/canfd_dbit_baud_rate, and n/initenal_resistance
// (ZLG's spelling) for its terminal resistor.
enum class Family
{
    Sja1000,
    EU,
    CanFd,
};

struct Model
{
    const char *name; // the card's zlgModel value
    UINT deviceType;
    int channels; // 0: not known, any channel index is tried
    Family family;
    bool termination; // has n/initenal_resistance in its property file
};

// USBCAN-E-mini is not here: no ZLG document found says which device type it uses.
inline constexpr Model kModels[] = {
    {"USBCAN-I", ZCAN_USBCAN1, 1, Family::Sja1000, false},
    {"USBCAN-I+", ZCAN_USBCAN1, 1, Family::Sja1000, false},
    {"USBCAN-I-mini", ZCAN_USBCAN1, 1, Family::Sja1000, false},
    {"USBCAN-II", ZCAN_USBCAN2, 2, Family::Sja1000, false},
    {"USBCAN-II+", ZCAN_USBCAN2, 2, Family::Sja1000, false},
    {"USBCAN-E-U", ZCAN_USBCAN_E_U, 1, Family::EU, false},
    {"USBCAN-2E-U", ZCAN_USBCAN_2E_U, 2, Family::EU, false},
    {"USBCAN-4E-U", ZCAN_USBCAN_4E_U, 4, Family::EU, false},
    {"USBCAN-8E-U", ZCAN_USBCAN_8E_U, 8, Family::EU, false},
    // No property file for the mini was found: its channel count and terminal resistor are not known.
    {"USBCANFD-100U-mini", ZCAN_USBCANFD_MINI, 0, Family::CanFd, false},
    {"USBCANFD-100U", ZCAN_USBCANFD_100U, 1, Family::CanFd, true},
    {"USBCANFD-200U", ZCAN_USBCANFD_200U, 2, Family::CanFd, true},
    {"USBCANFD-400U", ZCAN_USBCANFD_400U, 4, Family::CanFd, true},
    {"USBCANFD-800U", ZCAN_USBCANFD_800U, 8, Family::CanFd, true},
};

// The bitrates each family's property file offers (n/baud_rate, n/canfd_abit_baud_rate; n/canfd_dbit_baud_rate).
// The CAN FD family's arbitration rates have a fixed 80 % sample point, its data rates 80 % (75 % at 5M).
inline constexpr quint32 kClassicRates[] = {1000000, 800000, 500000, 250000, 125000, 100000, 50000, 20000, 10000};
inline constexpr quint32 kFdArbitrationRates[] = {1000000, 800000, 500000, 250000, 125000, 100000, 50000};
inline constexpr quint32 kFdDataRates[] = {5000000, 4000000, 2000000, 1000000, 800000, 500000, 250000, 125000, 100000};

// Timing0/Timing1 (BTR0/BTR1) of the common bitrates, sample point 87.5 % (80 % at 800k): UM01010101 V3.15,
// 2.5.5 VCI_INIT_CONFIG.
struct Btr
{
    quint32 bitrate;
    BYTE timing0;
    BYTE timing1;
};
inline constexpr Btr kBtrTable[] = {{10000, 0x31, 0x1C},  {20000, 0x18, 0x1C},  {50000, 0x09, 0x1C},
                                    {100000, 0x04, 0x1C}, {125000, 0x03, 0x1C}, {250000, 0x01, 0x1C},
                                    {500000, 0x00, 0x1C}, {800000, 0x00, 0x16}, {1000000, 0x00, 0x14}};

inline const Model *findModel(const char *name, qsizetype length)
{
    for (const Model &model : kModels) {
        if (qstrlen(model.name) == size_t(length) && qstrncmp(model.name, name, size_t(length)) == 0) {
            return &model;
        }
    }
    return nullptr;
}

} // namespace fhmi::zlg
