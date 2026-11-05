#pragma once

#include "core/can_frame.h"

#include <QtCore/QByteArray>

#include <optional>

// The Lawicel / CANable ASCII protocol ("slcan"), as the LAWICEL CANUSB User Manual (Rev. 1.1) describes it:
// every command and reply ends in CR; OK is a bare CR (z/Z CR after a transmit), an error is BELL (0x07).
namespace fhmi::Slcan {

// The Sn index of a standard bitrate (S0 10k … S8 1M); -1 for any other, which slcan cannot set.
int bitrateCode(quint32 bitrate);

// A frame as its transmit command, CR included: t<iii><l><dd…>, T<iiiiiiii><l><dd…>, r<iii><l>, R<iiiiiiii><l>.
// nullopt for a frame slcan cannot carry (FD, more than 8 bytes).
std::optional<QByteArray> encodeFrame(const CanFrame &frame);

// A received frame line without its CR, with or without the 4-digit time stamp (Z1) after the data.
std::optional<CanFrame> decodeFrame(const QByteArray &line);

// The reply to F: "F" and two hex digits of status flags.
std::optional<quint8> decodeStatus(const QByteArray &line);

// Status flag bits (CANUSB manual 2.9): bit 2 error warning (EI), bit 5 error passive (EPI).
constexpr quint8 ErrorWarningFlag = 0x04;
constexpr quint8 ErrorPassiveFlag = 0x20;

} // namespace fhmi::Slcan
