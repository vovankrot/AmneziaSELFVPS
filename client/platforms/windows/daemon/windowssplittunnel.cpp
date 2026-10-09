/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifdef AMNEZIA_EXPERIMENTAL_CDN_RECOVERY
#include "cdnRecoveryWfp.h"
#endif
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
#include <QMetaObject>
#include <limits>
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

constexpr int AddressRefreshRetryIntervalMs = 1000;

enum class DadState { Other, Tentative, Deprecated, Preferred };

struct AddressAvailability {
  bool internetIpv4;
  bool tunnelIpv4;
  bool internetIpv6;
  bool tunnelIpv6;
};

AddressAvailability availability(const IP_ADDRESSES_CONFIG& addresses);

constexpr int dadAddressScore(DadState state) {
  return state == DadState::Preferred    ? 2
         : state == DadState::Deprecated ? 1
                                         : 0;
}

constexpr bool hasInternetAddress(const AddressAvailability& addresses) {
  return addresses.internetIpv4 || addresses.internetIpv6;
}

constexpr bool hasTunnelAddress(const AddressAvailability& addresses) {
  return addresses.tunnelIpv4 || addresses.tunnelIpv6;
}

constexpr int splittingMode(const AddressAvailability& addresses) {
  if (addresses.internetIpv4 && addresses.tunnelIpv4 &&
      addresses.internetIpv6 && addresses.tunnelIpv6) {
    return 1;
  }
  if (addresses.internetIpv4 && addresses.tunnelIpv4 &&
      !addresses.internetIpv6 && !addresses.tunnelIpv6) {
    return 2;
  }
  if (addresses.internetIpv4 && addresses.tunnelIpv4 &&
      addresses.internetIpv6 && !addresses.tunnelIpv6) {
    return 3;
  }
  if (addresses.internetIpv4 && addresses.tunnelIpv4 &&
      !addresses.internetIpv6 && addresses.tunnelIpv6) {
    return 4;
  }
  if (!addresses.internetIpv4 && !addresses.tunnelIpv4 &&
      addresses.internetIpv6 && addresses.tunnelIpv6) {
    return 5;
  }
  if (addresses.internetIpv4 && !addresses.tunnelIpv4 &&
      addresses.internetIpv6 && addresses.tunnelIpv6) {
    return 6;
  }
  if (!addresses.internetIpv4 && addresses.tunnelIpv4 &&
      addresses.internetIpv6 && addresses.tunnelIpv6) {
    return 7;
  }
  if (!addresses.internetIpv4 && addresses.tunnelIpv4 &&
      addresses.internetIpv6 && !addresses.tunnelIpv6) {
    return 8;
  }
  if (addresses.internetIpv4 && !addresses.tunnelIpv4 &&
      !addresses.internetIpv6 && addresses.tunnelIpv6) {
    return 9;
  }
  return 0;
}

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

struct WindowsSplitTunnel::NotificationContext {
  SRWLOCK lock = SRWLOCK_INIT;
  WindowsSplitTunnel* target = nullptr;
  quint64 generation = 0;
};

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
#ifdef AMNEZIA_EXPERIMENTAL_CDN_RECOVERY
  const DWORD layerError = CdnRecoveryWfp::prepareDriverLayer();
  if (layerError != ERROR_SUCCESS) {
    logger.error() << "Experimental CDN driver baseline unavailable:" << layerError;
    return false;
  }
#endif
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
#ifdef AMNEZIA_EXPERIMENTAL_CDN_RECOVERY
  const auto& driverLayer = CdnRecoveryWfp::DriverLayer;
#else
  const auto& driverLayer = sublayer;
#endif
  const auto result = SplitTunnelDriverProtocol::initializeDriver(driverLayer, sublayer,
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

WindowsSplitTunnel::WindowsSplitTunnel(HANDLE driverIO)
    : QObject(nullptr), m_driver(driverIO), m_addressRefreshTimer(this) {
  logger.debug() << "Connected to the Driver";

  m_addressRefreshTimer.setSingleShot(true);
  m_addressRefreshTimer.setInterval(250);
  connect(&m_addressRefreshTimer, &QTimer::timeout, this,
          &WindowsSplitTunnel::refreshAddresses);
  Q_ASSERT(getState() == STATE_INITIALIZED);
}

WindowsSplitTunnel::~WindowsSplitTunnel() {
  stopAddressMonitoring();
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

bool WindowsSplitTunnel::start(const QHostAddress& endpoint, int inetAdapterIndex, int vpnAdapterIndex) {
  stopAddressMonitoring();
  m_endpoint = endpoint;
  m_lastIPConfiguration.clear();
  if (!updateAdapterLuids(inetAdapterIndex, vpnAdapterIndex)) return false;
  // Subscribe before reading addresses, so a renewal during startup is not lost.
  if (!startAddressMonitoring()) return false;
  auto failedStart = qScopeGuard([this] { stopAddressMonitoring(); });
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
    config = generateIPConfiguration();
    if (!config.empty() &&
        splittingMode(availability(*reinterpret_cast<const IP_ADDRESSES_CONFIG*>(config.data()))) != 0) {
      break;
    }
    config.clear();

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
  if (networkState != STATE_READY && networkState != STATE_RUNNING) return false;
  m_lastIPConfiguration = std::move(config);
  m_addressMonitoringActive = true;
  failedStart.dismiss();
  if (m_addressStabilizationPending) m_addressRefreshTimer.start(250);
  return true;
}

bool WindowsSplitTunnel::stop() {
  stopAddressMonitoring();
  m_lastIPConfiguration.clear();
  if (m_driver == INVALID_HANDLE_VALUE || isUnresponsive()) return false;
  const auto state = getState();
  if (state == STATE_STARTED) return true;
  if (state == STATE_UNKNOWN || state == STATE_ZOMBIE) return false;
  // CLEAR_CONFIGURATION leaves the process monitor alive in INITIALIZED/READY.
  // RESET tears it down and drains its event queue. start() initializes again.
  if (!resetDriver(m_driver)) return false;
  const auto resetState = getState();
  if (resetState != STATE_STARTED) {
    logger.error() << "Split tunnel monitor did not stop, state:" << resetState;
    return false;
  }
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

bool WindowsSplitTunnel::updateAdapterLuids(int inetAdapterIndex,
                                            int vpnAdapterIndex) {
  if (vpnAdapterIndex == 0) {
    vpnAdapterIndex = WindowsCommons::VPNAdapterIndex();
  }
  if (vpnAdapterIndex <= 0 ||
      ConvertInterfaceIndexToLuid(vpnAdapterIndex, &m_vpnAdapterLuid) !=
          NO_ERROR) {
    logger.error() << "Failed to resolve VPN adapter LUID:" << vpnAdapterIndex;
    m_vpnAdapterLuid.Value = 0;
    return false;
  }

  m_internetHintLuid.Value = 0;
  if (inetAdapterIndex > 0 &&
      ConvertInterfaceIndexToLuid(inetAdapterIndex, &m_internetHintLuid) !=
          NO_ERROR) {
    logger.warning() << "Failed to resolve internet adapter LUID:"
                     << inetAdapterIndex;
    m_internetHintLuid.Value = 0;
  }
  return true;
}

namespace {

bool isEmpty(const IN_ADDR& address) {
  return address.S_un.S_addr == INADDR_ANY;
}

bool isEmpty(const IN6_ADDR& address) {
  return IN6_IS_ADDR_UNSPECIFIED(&address);
}

bool isUsable(const IN_ADDR& address) {
  const std::uint32_t hostAddress = ntohl(address.S_un.S_addr);
  const std::uint8_t firstOctet =
      static_cast<std::uint8_t>(hostAddress >> 24);
  return hostAddress != INADDR_ANY && firstOctet != 127 && firstOctet < 224 &&
         (hostAddress & 0xffff0000u) != 0xa9fe0000u;
}

bool isUsable(const IN6_ADDR& address) {
  return !IN6_IS_ADDR_UNSPECIFIED(&address) &&
         !IN6_IS_ADDR_LOOPBACK(&address) && !IN6_IS_ADDR_LINKLOCAL(&address) &&
         !IN6_IS_ADDR_MULTICAST(&address);
}

AddressAvailability availability(
    const IP_ADDRESSES_CONFIG& addresses) {
  return {
      !isEmpty(addresses.InternetIpv4),
      !isEmpty(addresses.TunnelIpv4),
      !isEmpty(addresses.InternetIpv6),
      !isEmpty(addresses.TunnelIpv6),
  };
}

}  // namespace

std::vector<std::byte> WindowsSplitTunnel::generateIPConfiguration() {
  std::vector<std::byte> out(sizeof(IP_ADDRESSES_CONFIG));
  auto config = reinterpret_cast<IP_ADDRESSES_CONFIG*>(out.data());
  m_addressStabilizationPending = false;
  m_addressCollectionIncomplete = false;

  if (!getAddresses(m_vpnAdapterLuid, &config->TunnelIpv4,
                    &config->TunnelIpv6)) {
    logger.error() << "Failed to collect VPN interface addresses";
    return {};
  }

  NET_LUID preferredAdapter = m_internetHintLuid;
  const auto endpointRoute = getEndpointRoute();
  if (endpointRoute) {
    preferredAdapter = endpointRoute->adapter;
    m_internetHintLuid = endpointRoute->adapter;
  }

  if (m_endpoint.protocol() == QAbstractSocket::IPv4Protocol &&
      endpointRoute) {
    if (endpointRoute->source.si_family == AF_INET &&
        isUsable(endpointRoute->source.Ipv4.sin_addr)) {
      config->InternetIpv4 = endpointRoute->source.Ipv4.sin_addr;
    } else if (!getAddresses(endpointRoute->adapter, &config->InternetIpv4,
                             nullptr)) {
      logger.error() << "Failed to collect fallback internet IPv4 address";
      return {};
    }
  } else if (auto adapter = getBestDefaultRoute(AF_INET, preferredAdapter)) {
    if (!getAddresses(*adapter, &config->InternetIpv4, nullptr)) {
      logger.error() << "Failed to collect internet IPv4 address";
      return {};
    }
  }

  if (m_endpoint.protocol() == QAbstractSocket::IPv6Protocol &&
      endpointRoute) {
    if (endpointRoute->source.si_family == AF_INET6 &&
        isUsable(endpointRoute->source.Ipv6.sin6_addr)) {
      config->InternetIpv6 = endpointRoute->source.Ipv6.sin6_addr;
    } else if (!getAddresses(endpointRoute->adapter, nullptr,
                             &config->InternetIpv6)) {
      logger.error() << "Failed to collect fallback internet IPv6 address";
      return {};
    }
  } else if (auto adapter = getBestDefaultRoute(AF_INET6, preferredAdapter)) {
    if (!getAddresses(*adapter, nullptr, &config->InternetIpv6)) {
      logger.error() << "Failed to collect internet IPv6 address";
      return {};
    }
  }

  if (m_addressCollectionIncomplete) {
    return {};
  }

  auto addressState = availability(*config);
  if (!hasInternetAddress(addressState)) {
    std::memset(config, 0, sizeof(*config));
    addressState = availability(*config);
  }

  const int mode = splittingMode(addressState);
  if (hasTunnelAddress(addressState) && mode == 0) {
    logger.error() << "Unsupported split-tunnel address combination";
    return {};
  }
  return out;
}

bool WindowsSplitTunnel::registerIPConfiguration(bool force,
                                                 bool requireActiveMode) {
  auto config = generateIPConfiguration();
  if (config.empty()) {
    return false;
  }

  const auto* addresses =
      reinterpret_cast<const IP_ADDRESSES_CONFIG*>(config.data());
  const auto addressState = availability(*addresses);
  const bool activeMode =
      hasInternetAddress(addressState) &&
      hasTunnelAddress(addressState) &&
      splittingMode(addressState) != 0;
  if (requireActiveMode && !activeMode && !m_addressStabilizationPending) {
    logger.error() << "No usable tunnel/internet address pair";
    return false;
  }
  if (!activeMode && m_addressStabilizationPending) {
    std::memset(config.data(), 0, config.size());
  }
  if (!force && config == m_lastIPConfiguration) {
    return true;
  }

  DWORD bytesReturned = 0;
  const auto ok = DeviceIoControlWithTimeout(
      m_driver, IOCTL_REGISTER_IP_ADDRESSES, config.data(),
      static_cast<DWORD>(config.size()), nullptr, 0, &bytesReturned,
      "IOCTL_REGISTER_IP_ADDRESSES refresh");
  if (!ok) {
    logger.error() << "Failed to set Network Config. Error:" << GetLastError();
    return false;
  }
  m_lastIPConfiguration = std::move(config);
  logger.info() << "Split-tunnel addresses updated; mode:" << splittingMode(addressState)
                << "active:" << activeMode;
  emit addressConfigurationChanged(activeMode, m_addressGeneration);
  return true;
}

bool WindowsSplitTunnel::getAddresses(const NET_LUID& adapter,
                                      IN_ADDR* outIpv4, IN6_ADDR* outIpv6) {
  if (outIpv4 != nullptr) {
    std::memset(outIpv4, 0, sizeof(*outIpv4));
  }
  if (outIpv6 != nullptr) {
    std::memset(outIpv6, 0, sizeof(*outIpv6));
  }
  if (adapter.Value == 0) {
    return true;
  }

  PMIB_UNICASTIPADDRESS_TABLE table = nullptr;
  const DWORD result = GetUnicastIpAddressTable(AF_UNSPEC, &table);
  if (result == ERROR_NOT_FOUND) {
    return true;
  }
  if (result != NO_ERROR) {
    logger.error() << "Failed to retrieve unicast address table:" << result;
    m_addressCollectionIncomplete = true;
    return false;
  }
  auto guard = qScopeGuard([&] { FreeMibTable(table); });

  int ipv4Score = 0;
  int ipv6Score = 0;
  bool ipv4Tentative = false;
  bool ipv6Tentative = false;
  for (ULONG i = 0; i < table->NumEntries; ++i) {
    const MIB_UNICASTIPADDRESS_ROW& row = table->Table[i];
    if (row.InterfaceLuid.Value != adapter.Value || row.SkipAsSource) {
      continue;
    }

    DadState dadState =
        DadState::Other;
    if (row.DadState == IpDadStatePreferred) {
      dadState = DadState::Preferred;
    } else if (row.DadState == IpDadStateDeprecated) {
      dadState = DadState::Deprecated;
    } else if (row.DadState == IpDadStateTentative) {
      dadState = DadState::Tentative;
      const bool requestedUsableIpv4 =
          outIpv4 != nullptr && row.Address.si_family == AF_INET &&
          isUsable(row.Address.Ipv4.sin_addr);
      const bool requestedUsableIpv6 =
          outIpv6 != nullptr && row.Address.si_family == AF_INET6 &&
          isUsable(row.Address.Ipv6.sin6_addr);
      ipv4Tentative = ipv4Tentative || requestedUsableIpv4;
      ipv6Tentative = ipv6Tentative || requestedUsableIpv6;
    }
    const int score = dadAddressScore(dadState);
    if (score == 0) {
      continue;
    }
    if (outIpv4 != nullptr && row.Address.si_family == AF_INET &&
        score > ipv4Score && isUsable(row.Address.Ipv4.sin_addr)) {
      *outIpv4 = row.Address.Ipv4.sin_addr;
      ipv4Score = score;
    }
    if (outIpv6 != nullptr && row.Address.si_family == AF_INET6 &&
        score > ipv6Score && isUsable(row.Address.Ipv6.sin6_addr)) {
      *outIpv6 = row.Address.Ipv6.sin6_addr;
      ipv6Score = score;
    }
  }
  m_addressStabilizationPending =
      m_addressStabilizationPending ||
      (ipv4Tentative && ipv4Score == 0) ||
      (ipv6Tentative && ipv6Score == 0);
  return true;
}

std::optional<NET_LUID> WindowsSplitTunnel::getBestDefaultRoute(
    ADDRESS_FAMILY family, const NET_LUID& preferredAdapter) {
  PMIB_IPFORWARD_TABLE2 table = nullptr;
  const DWORD result = GetIpForwardTable2(family, &table);
  if (result == ERROR_NOT_FOUND || result == ERROR_NOT_SUPPORTED) {
    return std::nullopt;
  }
  if (result != NO_ERROR) {
    logger.error() << "Failed to retrieve route table for family" << family
                   << ":" << result;
    m_addressCollectionIncomplete = true;
    return std::nullopt;
  }
  auto guard = qScopeGuard([&] { FreeMibTable(table); });

  std::optional<NET_LUID> bestAdapter;
  std::uint64_t bestMetric = (std::numeric_limits<std::uint64_t>::max)();
  bool bestIsPreferred = false;
  for (ULONG i = 0; i < table->NumEntries; ++i) {
    const MIB_IPFORWARD_ROW2& route = table->Table[i];
    if (route.DestinationPrefix.PrefixLength != 0 || route.Loopback ||
        route.ValidLifetime == 0 ||
        route.InterfaceLuid.Value == m_vpnAdapterLuid.Value) {
      continue;
    }

    MIB_IPINTERFACE_ROW interfaceRow = {};
    InitializeIpInterfaceEntry(&interfaceRow);
    interfaceRow.Family = family;
    interfaceRow.InterfaceLuid = route.InterfaceLuid;
    if (GetIpInterfaceEntry(&interfaceRow) != NO_ERROR ||
        !interfaceRow.Connected) {
      continue;
    }

    const std::uint64_t metric = static_cast<std::uint64_t>(route.Metric) +
                                 static_cast<std::uint64_t>(interfaceRow.Metric);
    const bool isPreferred =
        route.InterfaceLuid.Value == preferredAdapter.Value;
    if (metric > bestMetric ||
        (metric == bestMetric && (bestIsPreferred || !isPreferred))) {
      continue;
    }
    bestAdapter = route.InterfaceLuid;
    bestMetric = metric;
    bestIsPreferred = isPreferred;
  }
  return bestAdapter;
}

std::optional<WindowsSplitTunnel::EndpointRoute>
WindowsSplitTunnel::getEndpointRoute() {
  SOCKADDR_INET destination = {};
  if (m_endpoint.protocol() == QAbstractSocket::IPv4Protocol) {
    destination.Ipv4.sin_family = AF_INET;
    destination.Ipv4.sin_addr.S_un.S_addr = htonl(m_endpoint.toIPv4Address());
  } else if (m_endpoint.protocol() == QAbstractSocket::IPv6Protocol) {
    destination.Ipv6.sin6_family = AF_INET6;
    const Q_IPV6ADDR address = m_endpoint.toIPv6Address();
    std::memcpy(&destination.Ipv6.sin6_addr, address.c, sizeof(address.c));
  } else {
    return std::nullopt;
  }

  MIB_IPFORWARD_ROW2 route = {};
  SOCKADDR_INET source = {};
  const DWORD result =
      GetBestRoute2(nullptr, 0, nullptr, &destination, 0, &route, &source);
  if (result != NO_ERROR) {
    logger.warning() << "Failed to resolve current route to VPN endpoint:"
                     << result;
    m_addressCollectionIncomplete = result != ERROR_NETWORK_UNREACHABLE &&
                                    result != ERROR_NOT_FOUND;
    return std::nullopt;
  }
  if (route.InterfaceLuid.Value == m_vpnAdapterLuid.Value) {
    logger.warning() << "VPN endpoint route points into the VPN interface";
    m_addressCollectionIncomplete = true;
    return std::nullopt;
  }
  return EndpointRoute{route.InterfaceLuid, source};
}

void WindowsSplitTunnel::refreshAddresses() {
  // Timer and all public driver operations run on the owning service thread.
  // Never pump events during IOCTL waits or reuse a quarantined handle.
  if (!m_addressMonitoringActive) return;
  const bool ok = !isUnresponsive() && registerIPConfiguration(false);
  if (ok && !m_addressStabilizationPending) {
    m_addressRefreshRetryAttempts = 0;
    return;
  }
  if (!isUnresponsive() && m_addressRefreshRetryAttempts < 3) {
    ++m_addressRefreshRetryAttempts;
    m_addressRefreshTimer.start(AddressRefreshRetryIntervalMs);
    return;
  }
  logger.error() << "Split-tunnel address refresh exhausted retries; unresponsive:"
                 << isUnresponsive();
  emit addressRefreshFailed(m_addressGeneration);
  // A fresh network notification may retry a transient collection failure.
  // A poisoned handle is only handled by the daemon's queued failure handler.
}

bool WindowsSplitTunnel::startAddressMonitoring() {
  if (m_notificationContext != nullptr) {
    return true;
  }

  auto context = std::make_unique<NotificationContext>();
  context->target = this;
  context->generation = m_addressGeneration;
  m_notificationContext = context.release();

  DWORD result = NotifyRouteChange2(AF_UNSPEC, routeChangeCallback,
                                    m_notificationContext, FALSE,
                                    &m_routeChangeHandle);
  if (result == NO_ERROR) {
    result = NotifyUnicastIpAddressChange(
        AF_UNSPEC, addressChangeCallback, m_notificationContext, FALSE,
        &m_addressChangeHandle);
  }
  if (result == NO_ERROR) {
    result = NotifyIpInterfaceChange(AF_UNSPEC, interfaceChangeCallback,
                                     m_notificationContext, FALSE,
                                     &m_interfaceChangeHandle);
  }
  if (result != NO_ERROR) {
    logger.error() << "Failed to monitor network address changes:" << result;
    stopAddressMonitoring();
    return false;
  }
  return true;
}

void WindowsSplitTunnel::stopAddressMonitoring() {
  m_addressMonitoringActive = false;
  ++m_addressGeneration;
  m_addressRefreshTimer.stop();
  m_addressRefreshRetryAttempts = 0;

  NotificationContext* context = m_notificationContext;
  if (context != nullptr) {
    AcquireSRWLockExclusive(&context->lock);
    context->target = nullptr;
    ReleaseSRWLockExclusive(&context->lock);
  }

  bool allCancelled = true;
  auto cancel = [&allCancelled](HANDLE& handle) {
    if (handle == nullptr) {
      return;
    }
    const DWORD result = CancelMibChangeNotify2(handle);
    if (result != NO_ERROR) {
      logger.error() << "Failed to cancel network change notification:"
                     << result;
      allCancelled = false;
    }
    handle = nullptr;
  };
  cancel(m_interfaceChangeHandle);
  cancel(m_addressChangeHandle);
  cancel(m_routeChangeHandle);

  m_notificationContext = nullptr;
  if (allCancelled) {
    delete context;
  } else if (context != nullptr) {
    logger.error()
        << "Retaining disabled network callback context after cancel failure";
  }
}

void WindowsSplitTunnel::scheduleAddressRefresh(quint64 generation) {
  QMetaObject::invokeMethod(
      this,
      [this, generation]() {
        if (generation != m_addressGeneration || !m_addressMonitoringActive) return;
        m_addressRefreshRetryAttempts = 0;
        if (m_addressMonitoringActive && !m_addressRefreshTimer.isActive()) {
          m_addressRefreshTimer.start(250);
        }
      },
      Qt::QueuedConnection);
}

void WindowsSplitTunnel::dispatchAddressRefresh(PVOID context) {
  auto notification = static_cast<NotificationContext*>(context);
  if (notification == nullptr) {
    return;
  }
  AcquireSRWLockShared(&notification->lock);
  if (notification->target != nullptr) {
    notification->target->scheduleAddressRefresh(notification->generation);
  }
  ReleaseSRWLockShared(&notification->lock);
}

void CALLBACK WindowsSplitTunnel::routeChangeCallback(
    PVOID context, PMIB_IPFORWARD_ROW2 row, MIB_NOTIFICATION_TYPE type) {
  Q_UNUSED(row);
  Q_UNUSED(type);
  dispatchAddressRefresh(context);
}

void CALLBACK WindowsSplitTunnel::addressChangeCallback(
    PVOID context, PMIB_UNICASTIPADDRESS_ROW row, MIB_NOTIFICATION_TYPE type) {
  Q_UNUSED(row);
  Q_UNUSED(type);
  dispatchAddressRefresh(context);
}

void CALLBACK WindowsSplitTunnel::interfaceChangeCallback(
    PVOID context, PMIB_IPINTERFACE_ROW row, MIB_NOTIFICATION_TYPE type) {
  Q_UNUSED(row);
  Q_UNUSED(type);
  dispatchAddressRefresh(context);
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
  PROCESSENTRY32W currentProcess = {};
  currentProcess.dwSize = sizeof(PROCESSENTRY32W);

  if (FALSE == (Process32First(snapshot_handle, &currentProcess))) {
    WindowsUtils::windowsLog("Cant read first entry");
    return {};
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

  // Toolhelp's parent PID can refer to a terminated process, or a different
  // process which has since reused that PID. Never give the driver a false
  // inheritance relationship: it can apply another app's bypass to a child.
  int staleParents = 0;
  for (auto it = processes.begin(); it != processes.end(); ++it) {
    auto& process = it.value();
    const auto parent = processes.constFind(process.ParentProcessId);
    const auto asTime = [](FILETIME time) {
      return (quint64(time.dwHighDateTime) << 32) | time.dwLowDateTime;
    };
    if (process.ParentProcessId &&
        (parent == processes.constEnd() || asTime(parent->CreationTime) == 0
         || asTime(process.CreationTime) == 0
         || asTime(parent->CreationTime) >= asTime(process.CreationTime))) {
      process.ParentProcessId = 0;
      ++staleParents;
    }
  }
  logger.info() << "Process discovery: stale or unverifiable parent links removed=" << staleParents;
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
