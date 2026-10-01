/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "windowssplittunnel.h"
#include "core/splitTunnelAddress.h"
#include "core/splitTunnelDriverProtocol.h"

#include <qassert.h>

#include <memory>

#include "../windowscommons.h"
#include "../windowsservicemanager.h"
#include "logger.h"
#include "platforms/windows/daemon/windowsfirewall.h"
#include "platforms/windows/daemon/windowssplittunnel.h"
#include "platforms/windows/windowsutils.h"
#include "windowsfirewall.h"

#define PSAPI_VERSION 2
#include <Windows.h>
#include <psapi.h>

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QNetworkInterface>
#include <QScopeGuard>
#include <QUrl>
#include <QThread>

#include <atomic>
#include <chrono>
#include <thread>
#include <mutex>
#include <set>
#include <cstring>

#pragma region

// Driver Configuration structures
using CONFIGURATION_ENTRY = struct {
  // Offset into buffer region that follows all entries.
  // The image name uses the device path.
  SIZE_T ImageNameOffset;
  // Length of the String
  USHORT ImageNameLength;
};

using CONFIGURATION_HEADER = struct {
  // Number of entries immediately following the header.
  SIZE_T NumEntries;

  // Total byte length: header + entries + string buffer.
  SIZE_T TotalLength;
};

// Used to Configure Which IP is network/vpn
using IP_ADDRESSES_CONFIG = struct {
  IN_ADDR TunnelIpv4;
  IN_ADDR InternetIpv4;

  IN6_ADDR TunnelIpv6;
  IN6_ADDR InternetIpv6;
};

// Used to Define Which Processes are alive on activation
using PROCESS_DISCOVERY_HEADER = struct {
  SIZE_T NumEntries;
  SIZE_T TotalLength;
};

using PROCESS_DISCOVERY_ENTRY = struct {
  HANDLE ProcessId;
  HANDLE ParentProcessId;

  SIZE_T ImageNameOffset;
  USHORT ImageNameLength;
};

using ProcessInfo = struct {
  DWORD ProcessId;
  DWORD ParentProcessId;
  FILETIME CreationTime;
  std::wstring DevicePath;
};

#ifndef CTL_CODE

#  define FILE_ANY_ACCESS 0x0000

#  define METHOD_BUFFERED 0
#  define METHOD_IN_DIRECT 1
#  define METHOD_NEITHER 3

#  define CTL_CODE(DeviceType, Function, Method, Access) \
    (((DeviceType) << 16) | ((Access) << 14) | ((Function) << 2) | (Method))
#endif

// Known ControlCodes
#define IOCTL_DEQUEUE_EVENT \
  CTL_CODE(0x8000, 2, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define IOCTL_REGISTER_PROCESSES \
  CTL_CODE(0x8000, 3, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define IOCTL_REGISTER_IP_ADDRESSES \
  CTL_CODE(0x8000, 4, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define IOCTL_GET_IP_ADDRESSES \
  CTL_CODE(0x8000, 5, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define IOCTL_SET_CONFIGURATION \
  CTL_CODE(0x8000, 6, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define IOCTL_GET_CONFIGURATION \
  CTL_CODE(0x8000, 7, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define IOCTL_CLEAR_CONFIGURATION \
  CTL_CODE(0x8000, 8, METHOD_NEITHER, FILE_ANY_ACCESS)

#define IOCTL_GET_STATE CTL_CODE(0x8000, 9, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define IOCTL_QUERY_PROCESS \
  CTL_CODE(0x8000, 10, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define IOCTL_ST_RESET CTL_CODE(0x8000, 11, METHOD_NEITHER, FILE_ANY_ACCESS)

constexpr static const auto DRIVER_SYMLINK = L"\\\\.\\MULLVADSPLITTUNNEL";
constexpr static const auto DRIVER_FILENAME = "mullvad-split-tunnel.sys";
constexpr static const auto DRIVER_SERVICE_NAME = L"AmneziaVPNSplitTunnel";
constexpr static const auto MV_SERVICE_NAME = L"MullvadVPN";

#pragma endregion

namespace {
Logger logger("WindowsSplitTunnel");

ProcessInfo getProcessInfo(HANDLE process, const PROCESSENTRY32W& processMeta) {
  ProcessInfo pi;
  pi.ParentProcessId = processMeta.th32ParentProcessID;
  pi.ProcessId = processMeta.th32ProcessID;
  pi.CreationTime = {0, 0};
  pi.DevicePath = L"";

  FILETIME creationTime, null_time;
  auto ok = GetProcessTimes(process, &creationTime, &null_time, &null_time,
                            &null_time);
  if (ok) {
    pi.CreationTime = creationTime;
  }
  wchar_t imagepath[MAX_PATH + 1];
  if (K32GetProcessImageFileNameW(
          process, imagepath, sizeof(imagepath) / sizeof(*imagepath)) != 0) {
    pi.DevicePath = imagepath;
  }
  return pi;
}

// Synchronous kernel requests can hang. Keep their buffers alive on the worker,
// bound the caller's wait below the client's 2s teardown timeout, and quarantine
// failed handles until process exit. Never queue additional requests after timeout.
constexpr int kIoctlTimeoutMs = 1500;
// Activating exclusions may reauthorize many existing flows. It has the
// normal 30s IPC budget; teardown keeps the short deadline above.
constexpr int kConfigurationIoctlTimeoutMs = 10000;
std::mutex failedDevicesMutex;
std::set<HANDLE> failedDevices;

bool driverFailed(HANDLE device) {
  std::lock_guard<std::mutex> lock(failedDevicesMutex);
  return failedDevices.count(device) != 0;
}

BOOL DeviceIoControlWithTimeout(HANDLE device, DWORD code, LPVOID inBuf, DWORD inSize,
                                LPVOID outBuf, DWORD outSize, DWORD* bytesReturned,
                                const char* opName, int timeoutMs = kIoctlTimeoutMs) {
  if (driverFailed(device)) {
    SetLastError(ERROR_DEVICE_NOT_CONNECTED);
    return FALSE;
  }
  struct Result {
    std::atomic<bool> done{false};
    BOOL ok = FALSE;
    DWORD err = 0;
    DWORD bytesReturned = 0;
    std::vector<unsigned char> input;
    std::vector<unsigned char> output;
  };
  auto result = std::make_shared<Result>();
  // Detached requests must never retain pointers into the caller's stack or vectors.
  result->input.resize(inSize);
  result->output.resize(outSize);
  if (inSize) std::memcpy(result->input.data(), inBuf, inSize);

  std::thread worker([device, code, inSize, outSize, result]() {
    DWORD bytes = 0;
    BOOL ok = DeviceIoControl(device, code,
        inSize ? result->input.data() : nullptr, inSize,
        outSize ? result->output.data() : nullptr, outSize, &bytes, nullptr);
    result->err = ok ? 0 : GetLastError();
    result->bytesReturned = bytes;
    result->ok = ok;
    result->done.store(true, std::memory_order_release);
  });
  worker.detach();

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  while (!result->done.load(std::memory_order_acquire)) {
    if (std::chrono::steady_clock::now() >= deadline) {
      {
        std::lock_guard<std::mutex> lock(failedDevicesMutex);
        failedDevices.insert(device);
      }
      logger.error() << "DeviceIoControl(" << opName << ") did not return within"
                     << timeoutMs << "ms -- request timed out; quarantining the handle";
      SetLastError(ERROR_TIMEOUT);
      return FALSE;
    }
    QThread::msleep(20);
  }

  if (result->ok && outSize) {
    if (result->bytesReturned > outSize) {
      SetLastError(ERROR_INVALID_DATA);
      return FALSE;
    }
    std::memcpy(outBuf, result->output.data(), result->bytesReturned);
  }
  if (bytesReturned) {
    *bytesReturned = result->bytesReturned;
  }
  if (!result->ok) {
    SetLastError(result->err);
  }
  return result->ok;
}

// App paths reach us from several places -- the QML file dialog hands back
// "file:///C:/..." URIs, the app-list provider hands back native paths, and an
// imported config can carry either separator style. Fold all of that into a
// plain native path before anything tries to resolve it, otherwise QueryDosDevice
// gets a drive letter of "file:" and the app silently never gets excluded.
// Ported from upstream 5.0.0.5. by vovankrot
QString normalizeExecutablePath(const QString& path) {
  QString normalized = path.trimmed();
  if (normalized.startsWith("file:", Qt::CaseInsensitive)) {
    const QString localPath = QUrl(normalized).toLocalFile();
    if (!localPath.isEmpty()) {
      normalized = localPath;
    }
  }
  normalized.replace('/', '\\');
  return normalized;
}

}  // namespace

std::unique_ptr<WindowsSplitTunnel> WindowsSplitTunnel::create(
    WindowsFirewall* fw) {
  if (fw == nullptr) {
    // Pre-Condition:
    // Make sure the Windows Firewall has created the sublayer
    // otherwise the driver will fail to initialize
    logger.error() << "Failed to did not pass a WindowsFirewall obj"
                   << "The Driver cannot work with the sublayer not created";
    return nullptr;
  }
  // 00: Check if we conflict with mullvad, if so.
  if (detectConflict()) {
    logger.error() << "Conflict detected, abort Split-Tunnel init.";
    return nullptr;
  }
  // 01: Check if the driver is installed, if not do so.
  if (!isInstalled()) {
    logger.debug() << "Driver is not Installed, doing so";
    auto handle = installDriver();
    if (handle == INVALID_HANDLE_VALUE) {
      WindowsUtils::windowsLog("Failed to install Driver");
      return nullptr;
    }
    logger.debug() << "Driver installed";
    CloseServiceHandle(handle);
  } else {
    logger.debug() << "Driver was installed";
  }
  // 02: Now check if the service is running
  auto driver_manager =
      WindowsServiceManager::open(QString::fromWCharArray(DRIVER_SERVICE_NAME));
  if (Q_UNLIKELY(driver_manager == nullptr)) {
    // Let's be fair if we end up here,
    // after checking it exists and installing it,
    // this is super unlikeley
    Q_ASSERT(false);
    logger.error()
        << "WindowsServiceManager was unable fo find Split Tunnel service?";
    return nullptr;
  }
  if (!driver_manager->isRunning()) {
    logger.debug() << "Driver is not running, starting it";
    // Start the service
    if (!driver_manager->startService()) {
      logger.error() << "Failed to start Split Tunnel Service";
      return nullptr;
    };
  }
  // 03: Open the Driver Symlink
  auto driverFile = CreateFileW(DRIVER_SYMLINK, GENERIC_READ | GENERIC_WRITE, 0,
                                nullptr, OPEN_EXISTING, 0, nullptr);
  ;
  if (driverFile == INVALID_HANDLE_VALUE) {
    WindowsUtils::windowsLog("Failed to open Driver: ");
    // Only once, if the opening did not work. Try to reboot it. #
    logger.info()
        << "Failed to open driver, attempting only once to reboot driver";
    if (!driver_manager->stopService()) {
      logger.error() << "Unable stop driver";
      return nullptr;
    };
    logger.info() << "Stopped driver, starting it again.";
    if (!driver_manager->startService()) {
      logger.error() << "Unable start driver";
      return nullptr;
    };
    logger.info() << "Opening again.";
    driverFile = CreateFileW(DRIVER_SYMLINK, GENERIC_READ | GENERIC_WRITE, 0,
                             nullptr, OPEN_EXISTING, 0, nullptr);
    if (driverFile == INVALID_HANDLE_VALUE) {
      logger.error() << "Opening Failed again, sorry!";
      return nullptr;
    }
  }
  if (!initDriver(driverFile)) {
    logger.error() << "Failed to init driver";
    if (!driverFailed(driverFile)) CloseHandle(driverFile);
    return nullptr;
  }
  // We're ready to talk to the driver, it's alive and setup.
  return std::make_unique<WindowsSplitTunnel>(driverFile);
}

bool WindowsSplitTunnel::initDriver(HANDLE driverIO) {
  // We need to now check the state and init it, if required
  auto state = getState(driverIO);
  if (state == STATE_UNKNOWN) {
    logger.debug() << "Cannot check if driver is initialized";
    return false;
  }
  if (state >= STATE_INITIALIZED) {
    logger.debug() << "Driver already initialized: " << state;
    // Reset Driver as it has wfp handles probably >:(
    if (!resetDriver(driverIO)) return false;

    auto newState = getState(driverIO);
    logger.debug() << "New state after reset:" << newState;
    if (newState != STATE_STARTED) {
      logger.debug() << "Reset unsuccesfull";
      return false;
    }
  }

  // This fork enforces both its DNS and baseline policy in the same WFP
  // sublayer. Supply that existing layer for both entries, rather than referring
  // to Mullvad's separate DNS sublayer which our firewall does not create.
  const auto& sublayer = WindowsFirewall::baselineSublayerKey();
  const auto result = SplitTunnelDriverProtocol::initializeDriver(sublayer, sublayer,
      [driverIO](DWORD code, const void* input, DWORD size) {
        DWORD bytesReturned = 0;
        return DeviceIoControlWithTimeout(driverIO, code, const_cast<void*>(input), size,
            nullptr, 0, &bytesReturned, "IOCTL_INITIALIZE");
      });
  if (!result.ok) {
    logger.error() << "Driver init failed err -" << result.error;
    const auto failedState = getState(driverIO);
    logger.error() << "State:" << failedState;

    return false;
  }
  const auto initializedState = getState(driverIO);
  logger.debug() << "Driver initialized" << initializedState << "API:"
                 << (result.api == SplitTunnelDriverProtocol::Api::Legacy ? "legacy" : "1.3 sublayer GUIDs");
  return initializedState == STATE_INITIALIZED;
}

WindowsSplitTunnel::WindowsSplitTunnel(HANDLE driverIO) : m_driver(driverIO) {
  logger.debug() << "Connected to the Driver";

  Q_ASSERT(getState() == STATE_INITIALIZED);
}

WindowsSplitTunnel::~WindowsSplitTunnel() {
  // CloseHandle / driver unload can block on an abandoned synchronous IRP.
  // A quarantined handle remains owned by this process until OS process cleanup.
  if (driverFailed(m_driver)) return;
  CloseHandle(m_driver);
  uninstallDriver();
}

bool WindowsSplitTunnel::excludeApps(const QStringList& appPaths) {
  auto state = getState();
  if (state != STATE_READY && state != STATE_RUNNING) {
    logger.warning() << "Driver is not in the right State to set Rules"
                     << state;
    return false;
  }

  logger.debug() << "Pushing new Ruleset for Split-Tunnel " << state;
  auto config = generateAppConfiguration(appPaths);
  if (config.empty()) {
    logger.warning() << "Split tunnel configuration is empty after path conversion";
    return false;
  }

  DWORD bytesReturned;
  auto ok = DeviceIoControlWithTimeout(m_driver, IOCTL_SET_CONFIGURATION, &config[0],
                                       (DWORD)config.size(), nullptr, 0, &bytesReturned,
                                       "IOCTL_SET_CONFIGURATION", kConfigurationIoctlTimeoutMs);
  if (!ok) {
    auto err = GetLastError();
    WindowsUtils::windowsLog("Set Config Failed:");
    logger.error() << "Failed to set Config err code " << err;
    return false;
  }
  const auto configuredState = getState();
  logger.debug() << "New Configuration applied: " << stateString();
  return configuredState == STATE_RUNNING;
}

bool WindowsSplitTunnel::start(int inetAdapterIndex, int vpnAdapterIndex) {
  // To Start we need to send 2 things:
  // Network info (what is vpn what is network)
  logger.debug() << "Starting SplitTunnel";
  DWORD bytesReturned;

  if (getState() == STATE_STARTED) {
    logger.debug() << "Driver needs Init Call";
    if (!initDriver(m_driver)) {
      logger.error() << "Driver init failed. Error:" << GetLastError();
      return false;
    }
  }

  // Process Info (what is running already)
  if (getState() == STATE_INITIALIZED) {
    logger.debug() << "State is Init, requires process config";
    auto config = generateProcessBlob();
    if (config.empty()) {
      logger.error() << "Failed to build process discovery blob";
      return false;
    }

    auto ok = DeviceIoControlWithTimeout(m_driver, IOCTL_REGISTER_PROCESSES, config.data(),
                                         (DWORD)config.size(), nullptr, 0, &bytesReturned,
                                         "IOCTL_REGISTER_PROCESSES");
    if (!ok) {
      logger.error() << "Failed to set Process Config. Error:" << GetLastError();
      return false;
    }
    logger.debug() << "Set Process Config ok || new State:" << stateString();
  }

  if (getState() == STATE_INITIALIZED) {
    logger.warning() << "Driver is still not ready after process list send";
    return false;
  }
  logger.debug() << "Driver is  ready || new State:" << stateString();

  std::vector<std::byte> config;
  constexpr int kAddressRetryCount = 10;

  for (int attempt = 1; attempt <= kAddressRetryCount; ++attempt) {
    config = generateIPConfiguration(inetAdapterIndex, vpnAdapterIndex);
    if (!config.empty()) {
      break;
    }

    if (attempt < kAddressRetryCount) {
      logger.warning() << "Split tunnel adapter addresses are not ready yet, retry"
                       << attempt << "of" << kAddressRetryCount
                       << "inetAdapterIndex:" << inetAdapterIndex
                       << "vpnAdapterIndex:" << vpnAdapterIndex;
      QThread::msleep(100);
    }
  }

  if (config.empty()) {
    logger.error() << "Failed to build Network Config for split tunnel";
    return false;
  }

  auto ok = DeviceIoControlWithTimeout(m_driver, IOCTL_REGISTER_IP_ADDRESSES, config.data(),
                                       (DWORD)config.size(), nullptr, 0, &bytesReturned,
                                       "IOCTL_REGISTER_IP_ADDRESSES");
  if (!ok) {
    logger.error() << "Failed to set Network Config. Error:" << GetLastError();
    return false;
  }
  const auto networkState = getState();
  logger.debug() << "New Network Config Applied || new State:" << stateString();
  return networkState == STATE_READY || networkState == STATE_RUNNING;
}

bool WindowsSplitTunnel::stop() {
  if (m_driver == INVALID_HANDLE_VALUE) {
    logger.warning() << "Split tunnel stop requested without a valid driver handle";
    return false;
  }

  const auto stateBeforeStop = getState();
  if (stateBeforeStop == STATE_UNKNOWN || stateBeforeStop == STATE_ZOMBIE) {
    logger.error() << "Cannot stop split tunnel: driver state is unavailable";
    return false;
  }
  if (stateBeforeStop != STATE_READY && stateBeforeStop != STATE_RUNNING) {
    logger.debug() << "Split tunnel already passive, current state:" << stateString();
    return true;
  }

  DWORD bytesReturned;
  auto ok = DeviceIoControlWithTimeout(m_driver, IOCTL_CLEAR_CONFIGURATION, nullptr, 0,
                                       nullptr, 0, &bytesReturned, "IOCTL_CLEAR_CONFIGURATION");
  const auto stateAfterClear = ok ? getState() : STATE_UNKNOWN;
  if (ok && (SplitTunnelDriverProtocol::isPassiveState(stateAfterClear))) {
    logger.debug() << "Stopping Split tunnel successfull, new state:" << stateString();
    return true;
  }

  if (!ok) {
    WindowsUtils::windowsLog("Stopping Split tunnel failed");
    logger.error() << "Stopping Split tunnel not successfull, attempting driver reset";
  } else {
    logger.warning() << "Split tunnel clear IOCTL returned success but driver is still running, attempting driver reset";
  }

  if (!resetDriver(m_driver)) {
    logger.error() << "Split tunnel reset failed, state:" << stateString();
    return false;
  }

  const auto stateAfterReset = getState();
  const bool stopped = SplitTunnelDriverProtocol::isPassiveState(stateAfterReset);
  if (!stopped) {
    logger.error() << "Split tunnel is still running after reset, state:" << stateString();
    return false;
  }

  logger.warning() << "Split tunnel required driver reset during stop, new state:" << stateString();
  return true;
}

bool WindowsSplitTunnel::resetDriver(HANDLE driverIO) {
  DWORD bytesReturned;
  auto ok = DeviceIoControlWithTimeout(driverIO, IOCTL_ST_RESET, nullptr, 0, nullptr, 0,
                                       &bytesReturned, "IOCTL_ST_RESET");
  if (!ok) {
    logger.error() << "Reset Split tunnel not successfull";
    return false;
  }
  logger.debug() << "Reset Split tunnel successfull";
  return true;
}

// static
WindowsSplitTunnel::DRIVER_STATE WindowsSplitTunnel::getState(HANDLE driverIO) {
  if (driverIO == INVALID_HANDLE_VALUE) {
    logger.debug() << "Can't query State from non Opened Driver";
    return STATE_UNKNOWN;
  }
  DWORD bytesReturned;
  SIZE_T outBuffer;
  bool ok = DeviceIoControlWithTimeout(driverIO, IOCTL_GET_STATE, nullptr, 0, &outBuffer,
                                       sizeof(outBuffer), &bytesReturned, "IOCTL_GET_STATE");
  if (!ok) {
    WindowsUtils::windowsLog("getState response failure");
    return STATE_UNKNOWN;
  }
  if (bytesReturned == 0) {
    WindowsUtils::windowsLog("getState response is empty");
    return STATE_UNKNOWN;
  }
  return static_cast<WindowsSplitTunnel::DRIVER_STATE>(outBuffer);
}
WindowsSplitTunnel::DRIVER_STATE WindowsSplitTunnel::getState() {
  m_lastState = getState(m_driver);
  return m_lastState;
}

std::vector<uint8_t> WindowsSplitTunnel::generateAppConfiguration(
    const QStringList& appPaths) {
  // Step 1: Calculate how much size the buffer will need
  size_t cummulated_string_size = 0;
  QStringList dosPaths;
  for (auto const& path : appPaths) {
    auto dosPath = convertPath(path);
    if (dosPath.isEmpty()) {
      logger.warning() << "Skipping invalid split tunnel path" << path;
      continue;
    }

    dosPaths.append(dosPath);
    cummulated_string_size += dosPath.toStdWString().size() * sizeof(wchar_t);
    logger.debug() << dosPath;
  }

  if (dosPaths.isEmpty()) {
    logger.warning() << "No valid application paths available for split tunnel configuration";
    return {};
  }

  size_t bufferSize = sizeof(CONFIGURATION_HEADER) +
                      (sizeof(CONFIGURATION_ENTRY) * dosPaths.size()) +
                      cummulated_string_size;
  std::vector<uint8_t> outBuffer(bufferSize);

  auto header = (CONFIGURATION_HEADER*)&outBuffer[0];
  auto entry = (CONFIGURATION_ENTRY*)(header + 1);

  auto stringDest = &outBuffer[0] + sizeof(CONFIGURATION_HEADER) +
                    (sizeof(CONFIGURATION_ENTRY) * dosPaths.size());

  SIZE_T stringOffset = 0;

  for (const QString& path : dosPaths) {
    auto wstr = path.toStdWString();
    auto cstr = wstr.c_str();
    auto stringLength = wstr.size() * sizeof(wchar_t);

    entry->ImageNameLength = (USHORT)stringLength;
    entry->ImageNameOffset = stringOffset;

    memcpy(stringDest, cstr, stringLength);

    ++entry;
    stringDest += stringLength;
    stringOffset += stringLength;
  }

  header->NumEntries = dosPaths.length();
  header->TotalLength = bufferSize;

  return outBuffer;
}

std::vector<std::byte> WindowsSplitTunnel::generateIPConfiguration(
    int inetAdapterIndex, int vpnAdapterIndex) {
  std::vector<std::byte> out(sizeof(IP_ADDRESSES_CONFIG));

  auto config = reinterpret_cast<IP_ADDRESSES_CONFIG*>(&out[0]);

  if (vpnAdapterIndex <= 0) {
    vpnAdapterIndex = WindowsCommons::VPNAdapterIndex();
  }

  if (vpnAdapterIndex <= 0) {
    logger.warning() << "Unable to resolve VPN adapter index:" << vpnAdapterIndex;
    return {};
  }

  if (inetAdapterIndex <= 0) {
    logger.warning() << "Unable to resolve internet adapter index:" << inetAdapterIndex;
    return {};
  }

  // Always the VPN
  if (!getAddress(vpnAdapterIndex, &config->TunnelIpv4,
                  &config->TunnelIpv6)) {
    return {};
  }
  // 2nd best route is usually the internet adapter
  if (!getAddress(inetAdapterIndex, &config->InternetIpv4,
                  &config->InternetIpv6)) {
    return {};
  };
  return out;
}
bool WindowsSplitTunnel::getAddress(int adapterIndex, IN_ADDR* out_ipv4,
                                    IN6_ADDR* out_ipv6) {
  QNetworkInterface target =
      QNetworkInterface::interfaceFromIndex(adapterIndex);
  if (!target.isValid()) {
    logger.debug() << "Adapter index is invalid:" << adapterIndex;
    return false;
  }

  logger.debug() << "Getting adapter info for:" << target.humanReadableName()
                 << "index:" << adapterIndex;

  QList<QHostAddress> addresses;
  for (const auto& entry : target.addressEntries()) addresses.append(entry.ip());
  const auto ipv4 = SplitTunnelAddress::ipv4(addresses);
  const auto ipv6 = SplitTunnelAddress::ipv6(addresses);
  std::memset(out_ipv4, 0, sizeof(*out_ipv4));
  std::memset(out_ipv6, 0, sizeof(*out_ipv6));
  if (ipv4.isNull()) {
    logger.warning() << "No usable IPv4 source for split tunnel adapter" << adapterIndex;
    return false;
  }
  out_ipv4->S_un.S_addr = htonl(ipv4.toIPv4Address());
  if (!ipv6.isNull()) {
    const auto raw = ipv6.toIPv6Address();
    static_assert(sizeof(raw.c) == sizeof(*out_ipv6));
    std::memcpy(out_ipv6, raw.c, sizeof(*out_ipv6));
  }
  return true;
}

std::vector<uint8_t> WindowsSplitTunnel::generateProcessBlob() {
  // Get a Snapshot of all processes that are running:
  HANDLE snapshot_handle = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot_handle == INVALID_HANDLE_VALUE) {
    WindowsUtils::windowsLog("Creating Process snapshot failed");
    return std::vector<uint8_t>(0);
  }
  auto cleanup = qScopeGuard([&] { CloseHandle(snapshot_handle); });
  // Load the First Entry, later iterate over all
  PROCESSENTRY32W currentProcess;
  currentProcess.dwSize = sizeof(PROCESSENTRY32W);

  if (FALSE == (Process32First(snapshot_handle, &currentProcess))) {
    WindowsUtils::windowsLog("Cant read first entry");
  }

  QMap<DWORD, ProcessInfo> processes;

  do {
    auto process_handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                      currentProcess.th32ProcessID);

    // OpenProcess returns NULL on failure, NOT INVALID_HANDLE_VALUE -- the old
    // check never fired, so a failed open fell through into getProcessInfo() and
    // CloseHandle() with a null handle. Upstream 5.0.0.5. by vovankrot
    if (process_handle == nullptr) {
      continue;
    }
    ProcessInfo info = getProcessInfo(process_handle, currentProcess);
    processes.insert(info.ProcessId, info);
    CloseHandle(process_handle);

  } while (FALSE != (Process32NextW(snapshot_handle, &currentProcess)));

  auto process_list = processes.values();
  if (process_list.isEmpty()) {
    logger.debug() << "Process Snapshot list was empty";
    return std::vector<uint8_t>(0);
  }

  logger.debug() << "Reading Processes NUM: " << process_list.size();
  // Determine the Size of the outBuffer:
  size_t totalStringSize = 0;

  for (const auto& process : process_list) {
    totalStringSize += (process.DevicePath.size() * sizeof(wchar_t));
  }
  auto bufferSize = sizeof(PROCESS_DISCOVERY_HEADER) +
                    (sizeof(PROCESS_DISCOVERY_ENTRY) * processes.size()) +
                    totalStringSize;

  std::vector<uint8_t> out(bufferSize);

  auto header = reinterpret_cast<PROCESS_DISCOVERY_HEADER*>(&out[0]);
  auto entry = reinterpret_cast<PROCESS_DISCOVERY_ENTRY*>(header + 1);
  auto stringBuffer = reinterpret_cast<uint8_t*>(entry + processes.size());

  SIZE_T currentStringOffset = 0;

  for (const auto& process : process_list) {
    // Wierd DWORD -> Handle Pointer magic.
    entry->ProcessId = (HANDLE)((size_t)process.ProcessId);
    entry->ParentProcessId = (HANDLE)((size_t)process.ParentProcessId);

    if (process.DevicePath.empty()) {
      entry->ImageNameOffset = 0;
      entry->ImageNameLength = 0;
    } else {
      const auto imageNameLength = process.DevicePath.size() * sizeof(wchar_t);

      entry->ImageNameOffset = currentStringOffset;
      entry->ImageNameLength = static_cast<USHORT>(imageNameLength);

      RtlCopyMemory(stringBuffer + currentStringOffset, &process.DevicePath[0],
                    imageNameLength);

      currentStringOffset += imageNameLength;
    }
    ++entry;
  }

  header->NumEntries = processes.size();
  header->TotalLength = bufferSize;

  return out;
}

// static
SC_HANDLE WindowsSplitTunnel::installDriver() {
  LPCWSTR displayName = L"Amnezia Split Tunnel Service";
  QFileInfo driver(qApp->applicationDirPath() + "/" + DRIVER_FILENAME);
  if (!driver.exists()) {
    logger.error() << "Split Tunnel Driver File not found "
                   << driver.absoluteFilePath();
    return (SC_HANDLE)INVALID_HANDLE_VALUE;
  }
  auto path = driver.absolutePath() + "/" + DRIVER_FILENAME;
  auto binPath = (const wchar_t*)path.utf16();
  auto scm_rights = SC_MANAGER_ALL_ACCESS;
  auto serviceManager = OpenSCManager(nullptr,  // local computer
                                      nullptr,  // servicesActive database
                                      scm_rights);
  auto service = CreateService(
      serviceManager, DRIVER_SERVICE_NAME, displayName, SERVICE_ALL_ACCESS,
      SERVICE_KERNEL_DRIVER, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL, binPath,
      nullptr, nullptr, nullptr, nullptr, nullptr);
  CloseServiceHandle(serviceManager);
  return service;
}
// static
bool WindowsSplitTunnel::uninstallDriver() {
  auto scm_rights = SC_MANAGER_ALL_ACCESS;
  auto serviceManager = OpenSCManager(NULL,  // local computer
                                      NULL,  // servicesActive database
                                      scm_rights);

  auto servicehandle =
      OpenService(serviceManager, DRIVER_SERVICE_NAME, GENERIC_READ);
  auto result = DeleteService(servicehandle);
  if (result) {
    logger.debug() << "Split Tunnel Driver Removed";
  }
  return result;
}
// static
bool WindowsSplitTunnel::isInstalled() {
  // Check if the Drivers I/O File is present
  auto symlink = QFileInfo(QString::fromWCharArray(DRIVER_SYMLINK));
  if (symlink.exists()) {
    return true;
  }
  // If not check with SCM, if the kernel service exists
  auto scm_rights = SC_MANAGER_ALL_ACCESS;
  auto serviceManager = OpenSCManager(NULL,  // local computer
                                      NULL,  // servicesActive database
                                      scm_rights);
  auto servicehandle =
      OpenService(serviceManager, DRIVER_SERVICE_NAME, GENERIC_READ);
  auto err = GetLastError();
  CloseServiceHandle(serviceManager);
  CloseServiceHandle(servicehandle);
  return err != ERROR_SERVICE_DOES_NOT_EXIST;
}

QString WindowsSplitTunnel::convertPath(const QString& path) {
  // Strip any file: URI wrapper first -- QFileInfo would otherwise treat the
  // whole "file:///C:/..." string as a relative name.
  QFileInfo fileInfo(QDir::fromNativeSeparators(normalizeExecutablePath(path)));
  QString normalizedPath = fileInfo.canonicalFilePath();
  if (normalizedPath.isEmpty()) {
    normalizedPath = fileInfo.absoluteFilePath();
  }
  if (normalizedPath.isEmpty()) {
    logger.error() << "Empty executable path for DOS device conversion";
    return "";
  }

  auto parts = QDir::fromNativeSeparators(QDir::cleanPath(normalizedPath))
                   .split("/", Qt::SkipEmptyParts);
  if (parts.isEmpty()) {
    logger.error() << "Invalid executable path for DOS device conversion:"
                   << normalizedPath;
    return "";
  }

  QString driveLetter = parts.takeFirst();
  if (!driveLetter.contains(":") || parts.size() == 0) {
    // device should contain : for e.g C:
    logger.error() << "Invalid executable path for DOS device conversion:"
                   << normalizedPath;
    return "";
  }

  // Grow the buffer until QueryDosDeviceW stops complaining rather than giving
  // up after a single retry -- a drive with many symlinked device names can
  // exceed 4 KB. Bounded at 4 attempts (2048 -> 16384 wchars). Upstream 5.0.0.5.
  QByteArray buffer(2048 * sizeof(wchar_t), 0);
  DWORD ok = 0;
  DWORD err = ERROR_SUCCESS;
  for (int attempt = 0; attempt < 4; ++attempt) {
    ok = QueryDosDeviceW(reinterpret_cast<LPCWSTR>(driveLetter.utf16()),
                         reinterpret_cast<LPWSTR>(buffer.data()),
                         buffer.size() / sizeof(wchar_t));
    if (ok != 0) {
      break;
    }
    err = GetLastError();
    if (err != ERROR_INSUFFICIENT_BUFFER) {
      WindowsUtils::windowsLog("Err fetching dos path");
      logger.error() << "QueryDosDeviceW failed for" << driveLetter
                     << "error:" << err;
      return "";
    }
    buffer.resize(buffer.size() * 2);
    buffer.fill(0);
  }
  if (ok == 0) {
    WindowsUtils::windowsLog("Err fetching dos path");
    logger.error() << "QueryDosDeviceW failed after buffer growth for"
                   << driveLetter << "error:" << err;
    return "";
  }
  QString deviceName;
  deviceName = QString::fromWCharArray((wchar_t*)buffer.data());
  parts.prepend(deviceName);

  return parts.join("\\");
}

// static
bool WindowsSplitTunnel::detectConflict() {
  auto scm_rights = SC_MANAGER_ENUMERATE_SERVICE;
  auto serviceManager = OpenSCManager(NULL,  // local computer
                                      NULL,  // servicesActive database
                                      scm_rights);
  auto cleanup = qScopeGuard([&] { CloseServiceHandle(serviceManager); });
  // Query for Mullvad Service.
  auto servicehandle =
      OpenService(serviceManager, MV_SERVICE_NAME, GENERIC_READ);
  auto err = GetLastError();
  CloseServiceHandle(servicehandle);
  if (err != ERROR_SERVICE_DOES_NOT_EXIST) {
    WindowsUtils::windowsLog("Mullvad Detected - Disabling SplitTunnel: ");
    // Mullvad is installed, so we would certainly break things.
    return true;
  }
  auto symlink = QFileInfo(QString::fromWCharArray(DRIVER_SYMLINK));
  if (!symlink.exists()) {
    // The driver is not loaded / installed.. MV is not installed, all good!
    logger.info() << "No Split-Tunnel Conflict detected, continue.";
    return false;
  }
  // The driver exists, so let's check if it has been created by us.
  // If our service is not present, it's has been created by
  // someone else so we should not use that :)
  servicehandle =
      OpenService(serviceManager, DRIVER_SERVICE_NAME, GENERIC_READ);
  err = GetLastError();
  CloseServiceHandle(servicehandle);
  return err == ERROR_SERVICE_DOES_NOT_EXIST;
}

bool WindowsSplitTunnel::isRunning() { return getState() == STATE_RUNNING; }
bool WindowsSplitTunnel::isUnresponsive() const { return driverFailed(m_driver); }
QString WindowsSplitTunnel::stateString() {
  // Formatting a log line must not issue IO or recursively enter the logger.
  switch (m_lastState) {
    case STATE_UNKNOWN:
      return "STATE_UNKNOWN";
    case STATE_NONE:
      return "STATE_NONE";
    case STATE_STARTED:
      return "STATE_STARTED";
    case STATE_INITIALIZED:
      return "STATE_INITIALIZED";
    case STATE_READY:
      return "STATE_READY";
    case STATE_RUNNING:
      return "STATE_RUNNING";
    case STATE_ZOMBIE:
      return "STATE_ZOMBIE";
      break;
  }
  return {};
}
