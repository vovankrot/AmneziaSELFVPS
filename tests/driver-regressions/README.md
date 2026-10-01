These tests compile the IO timeout helper extracted directly from the production
source. The OS call is replaced with a controllable delayed response. No VPN
driver or network configuration is touched.

Build with CMake/MSVC and run driver-regressions.exe. Tests cover bounded timeout,
quarantine, late completion after caller storage changes, valid output copying,
output length validation and preservation of the Windows error code.

They do not prove recovery of an already wedged kernel driver. That can require
a Windows restart; the service must report failure rather than claim recovery.
