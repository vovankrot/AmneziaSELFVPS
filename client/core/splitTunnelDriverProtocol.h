#pragma once

#include <Windows.h>
#include <winioctl.h>

namespace SplitTunnelDriverProtocol {
constexpr DWORD initialize = CTL_CODE(0x8000, 1, METHOD_BUFFERED, FILE_ANY_ACCESS);
constexpr DWORD initializeLegacy = CTL_CODE(0x8000, 1, METHOD_NEITHER, FILE_ANY_ACCESS);

struct SublayerGuids {
    GUID baseline;
    GUID dns;
};
static_assert(sizeof(SublayerGuids) == 32, "Mullvad 1.3 initialize ABI");

// CLEAR_CONFIGURATION leaves the driver READY (3), with process discovery
// still registered but splitting disabled. ENGAGED/RUNNING (4) is not passive.
constexpr bool isPassiveState(int state) { return state >= 0 && state <= 3; }

enum class Api { None, Legacy, SublayerGuids };
struct Result {
    bool ok;
    Api api;
    DWORD error;
};

// The loaded driver may still be the legacy version after a failed update or
// before reboot. Fall back only when the new IOCTL is explicitly unsupported;
// permission errors, timeouts and malformed configuration must remain failures.
template<class Ioctl>
Result initializeDriver(const GUID& baseline, const GUID& dns, Ioctl ioctl)
{
    SublayerGuids input{baseline, dns};
    if (ioctl(initialize, &input, sizeof(input)))
        return {true, Api::SublayerGuids, ERROR_SUCCESS};
    const auto error = GetLastError();
    if (error != ERROR_INVALID_FUNCTION && error != ERROR_NOT_SUPPORTED)
        return {false, Api::None, error};
    if (ioctl(initializeLegacy, nullptr, 0))
        return {true, Api::Legacy, ERROR_SUCCESS};
    return {false, Api::None, GetLastError()};
}
}
