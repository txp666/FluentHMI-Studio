#pragma once

#include "comm/can/can_backend.h"
#include "comm/can/zlg_api.h"

#include <algorithm>

namespace fhmi::zlg::detail {

// Kept internal so the production decision can be exercised without loading a vendor DLL.
template <class ReadError, class ReadStatus>
CanBusState readBusState(Family family, ReadError &&readError, ReadStatus &&readStatus)
{
    ZCAN_CHANNEL_ERR_INFO info{};
    const bool hasInfo = readError(&info) == STATUS_OK;
    if (hasInfo && (info.error_code & ZCAN_ERROR_CAN_BUSOFF)) {
        return CanBusState::BusOff;
    }

    // Some USBCAN-I/II compatible drivers block for seconds in ReadChannelStatus, even on an idle bus.
    // Their quick ReadChannelErrInfo still supplies bus-off, passive and warning flags.
    if (family != Family::Sja1000) {
        ZCAN_CHANNEL_STATUS status{};
        if (readStatus(&status) == STATUS_OK) {
            const int counter = std::max(status.regRECounter, status.regTECounter);
            return counter >= 128 ? CanBusState::Passive : counter >= 96 ? CanBusState::Warning : CanBusState::Normal;
        }
    }
    if (hasInfo && (info.error_code & ZCAN_ERROR_CAN_PASSIVE)) {
        return CanBusState::Passive;
    }
    if (hasInfo && (info.error_code & ZCAN_ERROR_CAN_ERRALARM)) {
        return CanBusState::Warning;
    }
    return CanBusState::Normal;
}

} // namespace fhmi::zlg::detail
