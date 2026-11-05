#pragma once

// The part of PEAK's PCAN-Basic API this program uses, copied from the official header PCANBasic.h
// ("PCAN-Basic API", Last change: 2022-11-17, (C) PEAK-System Technik GmbH) and checked against PEAK's
// PCANBasic.py (last change 2022-07-06). The vendor header itself is not part of this repository.
//
// On macOS the same API comes from MacCAN's PCBUSB library (libPCBUSB.dylib, mac-can.com), whose header is kept
// in step with PEAK's (PCBUSB 0.13: "synchronized with PCANBasic header from PEAK's version 4.9.0"). Its structures
// are declared with 32-bit IDs as here, as Qt's own PeakCAN plugin declares them for PCBUSB.
//
// The library is loaded with QLibrary at connect time; nothing links against it.

#include <QtCore/QtGlobal>

namespace fhmi::pcan {

// PCANBasic.h: WORD, DWORD, BYTE, UINT64 (fixed sizes on every platform the header supports).
using TPCANHandle = quint16;      // WORD
using TPCANStatus = quint32;      // DWORD
using TPCANParameter = quint8;    // BYTE
using TPCANDevice = quint8;       // BYTE
using TPCANMessageType = quint8;  // BYTE
using TPCANType = quint8;         // BYTE
using TPCANBaudrate = quint16;    // WORD
using TPCANTimestampFD = quint64; // UINT64

constexpr TPCANHandle PCAN_NONEBUS = 0x00;
// PCAN-USB interface, channels 1-8 then 9-16.
constexpr TPCANHandle PCAN_USBBUS1 = 0x51;
constexpr TPCANHandle PCAN_USBBUS8 = 0x58;
constexpr TPCANHandle PCAN_USBBUS9 = 0x509;
constexpr TPCANHandle PCAN_USBBUS16 = 0x510;

constexpr TPCANStatus PCAN_ERROR_OK = 0x00000;
constexpr TPCANStatus PCAN_ERROR_XMTFULL = 0x00001;
constexpr TPCANStatus PCAN_ERROR_OVERRUN = 0x00002;
constexpr TPCANStatus PCAN_ERROR_BUSLIGHT = 0x00004;
constexpr TPCANStatus PCAN_ERROR_BUSHEAVY = 0x00008;
constexpr TPCANStatus PCAN_ERROR_BUSWARNING = PCAN_ERROR_BUSHEAVY;
constexpr TPCANStatus PCAN_ERROR_BUSPASSIVE = 0x40000;
constexpr TPCANStatus PCAN_ERROR_BUSOFF = 0x00010;
constexpr TPCANStatus PCAN_ERROR_QRCVEMPTY = 0x00020;
constexpr TPCANStatus PCAN_ERROR_QOVERRUN = 0x00040;
constexpr TPCANStatus PCAN_ERROR_QXMTFULL = 0x00080;
constexpr TPCANStatus PCAN_ERROR_ILLHW = 0x01400;
constexpr TPCANStatus PCAN_ERROR_ILLNET = 0x01800;
constexpr TPCANStatus PCAN_ERROR_ILLCLIENT = 0x01C00;
constexpr TPCANStatus PCAN_ERROR_ILLHANDLE = PCAN_ERROR_ILLHW | PCAN_ERROR_ILLNET | PCAN_ERROR_ILLCLIENT;
constexpr TPCANStatus PCAN_ERROR_NODRIVER = 0x00200;
constexpr TPCANStatus PCAN_ERROR_INITIALIZE = 0x4000000;

constexpr TPCANMessageType PCAN_MESSAGE_STANDARD = 0x00;
constexpr TPCANMessageType PCAN_MESSAGE_RTR = 0x01;
constexpr TPCANMessageType PCAN_MESSAGE_EXTENDED = 0x02;
constexpr TPCANMessageType PCAN_MESSAGE_FD = 0x04;
constexpr TPCANMessageType PCAN_MESSAGE_BRS = 0x08;
constexpr TPCANMessageType PCAN_MESSAGE_ESI = 0x10;
constexpr TPCANMessageType PCAN_MESSAGE_ECHO = 0x20;
constexpr TPCANMessageType PCAN_MESSAGE_ERRFRAME = 0x40;
constexpr TPCANMessageType PCAN_MESSAGE_STATUS = 0x80;

constexpr TPCANParameter PCAN_RECEIVE_EVENT = 0x03;
constexpr TPCANParameter PCAN_LISTEN_ONLY = 0x08;
constexpr TPCANParameter PCAN_CHANNEL_CONDITION = 0x0D;
constexpr TPCANParameter PCAN_HARDWARE_NAME = 0x0E;
constexpr TPCANParameter PCAN_CHANNEL_FEATURES = 0x16;
constexpr TPCANParameter PCAN_ATTACHED_CHANNELS_COUNT = 0x2A;
constexpr TPCANParameter PCAN_ATTACHED_CHANNELS = 0x2B;

constexpr quint32 PCAN_PARAMETER_OFF = 0x00;
constexpr quint32 PCAN_PARAMETER_ON = 0x01;
constexpr quint32 PCAN_CHANNEL_AVAILABLE = 0x01;
constexpr quint32 PCAN_CHANNEL_OCCUPIED = 0x02;
constexpr quint32 FEATURE_FD_CAPABLE = 0x01;
constexpr int MAX_LENGTH_HARDWARE_NAME = 33;

struct TPCANMsg
{
    quint32 ID;               // 11/29-bit message identifier
    TPCANMessageType MSGTYPE; // Type of the message
    quint8 LEN;               // Data Length Code of the message (0..8)
    quint8 DATA[8];           // Data of the message (DATA[0]..DATA[7])
};

struct TPCANTimestamp
{
    quint32 millis;          // Base-value: milliseconds: 0.. 2^32-1
    quint16 millis_overflow; // Roll-arounds of millis
    quint16 micros;          // Microseconds: 0..999
};

struct TPCANMsgFD
{
    quint32 ID;               // 11/29-bit message identifier
    TPCANMessageType MSGTYPE; // Type of the message
    quint8 DLC;               // Data Length Code of the message (0..15)
    quint8 DATA[64];          // Data of the message (DATA[0]..DATA[63])
};

struct TPCANChannelInformation
{
    TPCANHandle channel_handle;                 // PCAN channel handle
    TPCANDevice device_type;                    // Kind of PCAN device
    quint8 controller_number;                   // CAN-Controller number
    quint32 device_features;                    // Device capabilities flag (see FEATURE_*)
    char device_name[MAX_LENGTH_HARDWARE_NAME]; // Device name
    quint32 device_id;                          // Device number
    quint32 channel_condition;                  // Availability status of a PCAN-Channel
};

// The header declares every function __stdcall on Windows (meaningless on x64) and plain C elsewhere.
#ifdef Q_OS_WIN
#define FHMI_PCAN_CALL __stdcall
#else
#define FHMI_PCAN_CALL
#endif

using CAN_Initialize_t = TPCANStatus(FHMI_PCAN_CALL *)(TPCANHandle Channel, TPCANBaudrate Btr0Btr1,
                                                       TPCANType HwType, quint32 IOPort, quint16 Interrupt);
using CAN_InitializeFD_t = TPCANStatus(FHMI_PCAN_CALL *)(TPCANHandle Channel, char *BitrateFD);
using CAN_Uninitialize_t = TPCANStatus(FHMI_PCAN_CALL *)(TPCANHandle Channel);
using CAN_GetStatus_t = TPCANStatus(FHMI_PCAN_CALL *)(TPCANHandle Channel);
using CAN_Read_t = TPCANStatus(FHMI_PCAN_CALL *)(TPCANHandle Channel, TPCANMsg *MessageBuffer,
                                                 TPCANTimestamp *TimestampBuffer);
using CAN_ReadFD_t = TPCANStatus(FHMI_PCAN_CALL *)(TPCANHandle Channel, TPCANMsgFD *MessageBuffer,
                                                   TPCANTimestampFD *TimestampBuffer);
using CAN_Write_t = TPCANStatus(FHMI_PCAN_CALL *)(TPCANHandle Channel, TPCANMsg *MessageBuffer);
using CAN_WriteFD_t = TPCANStatus(FHMI_PCAN_CALL *)(TPCANHandle Channel, TPCANMsgFD *MessageBuffer);
using CAN_GetValue_t = TPCANStatus(FHMI_PCAN_CALL *)(TPCANHandle Channel, TPCANParameter Parameter, void *Buffer,
                                                     quint32 BufferLength);
using CAN_SetValue_t = TPCANStatus(FHMI_PCAN_CALL *)(TPCANHandle Channel, TPCANParameter Parameter, void *Buffer,
                                                     quint32 BufferLength);
using CAN_GetErrorText_t = TPCANStatus(FHMI_PCAN_CALL *)(TPCANStatus Error, quint16 Language, char *Buffer);

} // namespace fhmi::pcan
