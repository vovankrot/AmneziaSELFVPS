/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "windowstunnelservice.h"

#include <Windows.h>

#include <QDateTime>
#include <QElapsedTimer>
#include <QScopeGuard>
#include <QFileInfo>
#include <QSysInfo>
#include <QUuid>

#include "wintunDeviceDiagnostics.h"

#include "leakdetector.h"
#include "logger.h"
#include "platforms/windows/windowscommons.h"
#include "platforms/windows/windowsutils.h"
#include "windowsdaemon.h"

#define TUNNEL_NAMED_PIPE \
  "\\\\."                 \
  "\\pipe\\ProtectedPrefix\\Administrators\\AmneziaWG\\AmneziaVPN"

constexpr uint32_t WINDOWS_TUNNEL_MONITOR_TIMEOUT_MSEC = 2000;

namespace {
Logger logger("WindowsTunnelService");
}  // namespace

static bool stopAndDeleteTunnelService(SC_HANDLE service);
static bool waitForServiceStatus(SC_HANDLE service, DWORD expectedStatus);

static void logServiceSnapshot(SC_HANDLE service, const char* phase, const QString& attempt) {
  SERVICE_STATUS_PROCESS status{};
  DWORD bytes = 0;
  if (!QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO,
      reinterpret_cast<LPBYTE>(&status), sizeof(status), &bytes)) {
    logger.error() << "Tunnel attempt" << attempt << phase << "status query failed; win32=" << GetLastError();
    return;
  }
  logger.info() << "Tunnel attempt" << attempt << phase
      << "state=" << status.dwCurrentState << "pid=" << status.dwProcessId
      << "win32Exit=" << status.dwWin32ExitCode << "serviceExit=" << status.dwServiceSpecificExitCode
      << "checkpoint=" << status.dwCheckPoint << "waitHintMs=" << status.dwWaitHint
      << "acceptedControls=" << status.dwControlsAccepted;
}

WindowsTunnelService::WindowsTunnelService(QObject* parent) : QObject(parent) {
  MZ_COUNT_CTOR(WindowsTunnelService);
  logger.debug() << "WindowsTunnelService created.";

  m_scm = OpenSCManager(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
  if (m_scm == nullptr) {
    WindowsUtils::windowsLog("Failed to open SCManager");
  }

  // Is the service already running? Terminate it.
  SC_HANDLE service =
      OpenService((SC_HANDLE)m_scm, TUNNEL_SERVICE_NAME, SERVICE_ALL_ACCESS);
  if (service != nullptr) {
    logger.info() << "Tunnel already exists. Terminating it.";
    if (stopAndDeleteTunnelService(service)) CloseServiceHandle(service);
    else m_service = service;
  }

  connect(&m_timer, &QTimer::timeout, this, &WindowsTunnelService::timeout);
}

WindowsTunnelService::~WindowsTunnelService() {
  MZ_COUNT_CTOR(WindowsTunnelService);
  stop();
  CloseServiceHandle((SC_HANDLE)m_scm);
}

bool WindowsTunnelService::stop() {
  SC_HANDLE service = (SC_HANDLE)m_service;
  if (service) {
    if (!stopAndDeleteTunnelService(service)) return false;
    CloseServiceHandle(service);
    m_service = nullptr;
  }

  m_timer.stop();

  if (m_logworker) {
    m_logthread.quit();
    m_logthread.wait();
    m_logworker = nullptr;
  }
  return true;
}

bool WindowsTunnelService::isRunning() {
  if (m_service == nullptr) {
    return false;
  }

  SERVICE_STATUS status;
  if (!QueryServiceStatus((SC_HANDLE)m_service, &status)) {
    return false;
  }

  return status.dwCurrentState == SERVICE_RUNNING;
}

bool WindowsTunnelService::isStopped() {
  if (!m_service) return true;
  SERVICE_STATUS status{};
  return QueryServiceStatus((SC_HANDLE)m_service, &status) && status.dwCurrentState == SERVICE_STOPPED;
}

void WindowsTunnelService::timeout() {
  if (m_service == nullptr) {
    logger.error() << "The service doesn't exist";
    emit backendFailure();
    return;
  }

  SERVICE_STATUS status;
  if (!QueryServiceStatus((SC_HANDLE)m_service, &status)) {
    WindowsUtils::windowsLog("Failed to retrieve the service status");
    emit backendFailure();
    return;
  }

  if (status.dwCurrentState == SERVICE_RUNNING) {
    // The service is active
    return;
  }

  logger.debug() << "The service is not active";
  logServiceSnapshot((SC_HANDLE)m_service, "runtime-failure", m_attemptId);
  for (const auto& detail : WintunDeviceDiagnostics::recentFailures()) logger.error() << detail;
  // Report this failure once, rather than repeating it every two seconds.
  m_timer.stop();
  emit backendFailure();
}

bool WindowsTunnelService::start(const QString& configData) {
  if (m_service && !stop()) return false;
  m_attemptId = QUuid::createUuid().toString(QUuid::WithoutBraces);
  QElapsedTimer attemptTime;
  attemptTime.start();
  logger.debug() << "Starting the tunnel service";
  logger.info() << "Tunnel attempt" << m_attemptId << "begin; OS=" << QSysInfo::prettyProductName()
                << "kernel=" << QSysInfo::kernelVersion() << "arch=" << QSysInfo::currentCpuArchitecture();
  for (const auto& filename : {QStringLiteral("tunnel.dll"), QStringLiteral("wintun.dll")}) {
    const QFileInfo file(qApp->applicationDirPath() + "/" + filename);
    logger.info() << "Tunnel attempt" << m_attemptId << "binary=" << filename
                  << "exists=" << file.exists() << "size=" << file.size()
                  << "modifiedUtc=" << file.lastModified().toUTC().toString(Qt::ISODate);
  }

  m_logworker = new WindowsTunnelLogger(WindowsCommons::tunnelLogFile(), nullptr, m_attemptId);
  m_logworker->moveToThread(&m_logthread);
  connect(&m_logthread, &QThread::finished, m_logworker, &QObject::deleteLater);
  m_logthread.start();

  SC_HANDLE scm = (SC_HANDLE)m_scm;
  SC_HANDLE service = nullptr;
  bool created = false;
  auto guard = qScopeGuard([&] {
    if (service) {
      if (created && !stopAndDeleteTunnelService(service)) {
        // Preserve the handle so cleanup can retry a partially started service.
        m_service = service;
      } else {
        CloseServiceHandle(service);
      }
    }
    m_logthread.quit();
    m_logthread.wait();
    // QThread::finished owns deletion via deleteLater, including start failure.
    m_logworker = nullptr;
  });

  // Let's see if we have to delete a previous instance.
  service = OpenService(scm, TUNNEL_SERVICE_NAME, SERVICE_ALL_ACCESS);
  if (service) {
    logger.debug() << "An existing service has been detected. Let's close it.";
    if (!stopAndDeleteTunnelService(service)) {
      m_service = service;
      service = nullptr;
      return false;
    }
    CloseServiceHandle(service);
    service = nullptr;
  }

  QString serviceCmdline;
  {
    QTextStream out(&serviceCmdline);
    out << "\"" << qApp->applicationFilePath() << "\" tunneldaemon \""
        << configData << "\"";
  }

  logger.debug() << "Service:" << qApp->applicationFilePath();

  service = CreateService(scm, TUNNEL_SERVICE_NAME, L"Amnezia VPN (tunnel)",
                          SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS,
                          SERVICE_DEMAND_START, SERVICE_ERROR_NORMAL,
                          (const wchar_t*)serviceCmdline.utf16(), nullptr, 0,
                          TEXT("Nsi\0TcpIp\0"), nullptr, nullptr);
  if (!service) {
    WindowsUtils::windowsLog("Failed to create the tunnel service");
    return false;
  }
  created = true;

  SERVICE_DESCRIPTION sd = {
      (wchar_t*)L"Manages the Amnezia VPN tunnel connection"};

  if (!ChangeServiceConfig2(service, SERVICE_CONFIG_DESCRIPTION, &sd)) {
    WindowsUtils::windowsLog(
        "Failed to set the description to the tunnel service");
    return false;
  }

  SERVICE_SID_INFO ssi;
  ssi.dwServiceSidType = SERVICE_SID_TYPE_UNRESTRICTED;
  if (!ChangeServiceConfig2(service, SERVICE_CONFIG_SERVICE_SID_INFO, &ssi)) {
    WindowsUtils::windowsLog("Failed to set the SID to the tunnel service");
    return false;
  }

  if (!StartService(service, 0, nullptr)) {
    WindowsUtils::windowsLog("Failed to start the service");
    return false;
  }
  logServiceSnapshot(service, "start-accepted", m_attemptId);

  if (waitForServiceStatus(service, SERVICE_RUNNING)) {
    logger.debug() << "The tunnel service is up and running";
    logServiceSnapshot(service, "running", m_attemptId);
    logger.info() << "Tunnel attempt" << m_attemptId << "ready; elapsedMs=" << attemptTime.elapsed();
    guard.dismiss();
    m_service = service;
    m_timer.start(WINDOWS_TUNNEL_MONITOR_TIMEOUT_MSEC);
    return true;
  }

  logger.error() << "Failed to run the tunnel service";
  logServiceSnapshot(service, "start-failure", m_attemptId);
  logger.error() << "Tunnel attempt" << m_attemptId << "failed; elapsedMs=" << attemptTime.elapsed();
  for (const auto& detail : WintunDeviceDiagnostics::recentFailures()) {
    logger.error() << "Tunnel attempt" << m_attemptId << detail;
  }

  SERVICE_STATUS status;
  if (!QueryServiceStatus(service, &status)) {
    WindowsUtils::windowsLog("Failed to retrieve the service status");
    return false;
  }

  logger.debug() << "The tunnel service exited with status:"
                 << status.dwWin32ExitCode << "-" << exitCodeToFailure(&status);

  emit backendFailure();
  return false;
}

static bool stopAndDeleteTunnelService(SC_HANDLE service) {
  SERVICE_STATUS status;
  if (!QueryServiceStatus(service, &status)) {
    WindowsUtils::windowsLog("Failed to retrieve the service status");
    return false;
  }

  logger.debug() << "The current service is stopped:"
                 << (status.dwCurrentState == SERVICE_STOPPED);

  if (status.dwCurrentState != SERVICE_STOPPED) {
    logger.debug() << "The service is not stopped yet.";
    if (!ControlService(service, SERVICE_CONTROL_STOP, &status)) {
      WindowsUtils::windowsLog("Failed to control the service");
      return false;
    }

    if (!waitForServiceStatus(service, SERVICE_STOPPED)) {
      logger.error() << "Unable to stop the service";
      return false;
    }
  }

  logger.debug() << "Proceeding with the deletion";

  if (!DeleteService(service) && GetLastError() != ERROR_SERVICE_MARKED_FOR_DELETE) {
    WindowsUtils::windowsLog("Failed to delete the service");
    return false;
  }

  return true;
}

QString WindowsTunnelService::uapiCommand(const QString& command) {
  // Create a pipe to the tunnel service.
  LPTSTR tunnelName = (LPTSTR)TEXT(TUNNEL_NAMED_PIPE);
  if (!WaitNamedPipe(tunnelName, 1000)) {
    WindowsUtils::windowsLog("Failed to wait for named pipes");
    return {};
  }
  HANDLE pipe = CreateFile(tunnelName, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                           OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
  if (pipe == INVALID_HANDLE_VALUE) {
    return QString();
  }

  auto guard = qScopeGuard([&] { CloseHandle(pipe); });
  DWORD mode = PIPE_READMODE_BYTE;
  if (!SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr)) {
    WindowsUtils::windowsLog("Failed to set the read-mode on pipe");
    return QString();
  }

  // Bound the entire exchange, including a service that accepts the command
  // but never completes its response. Never leave stack buffers in pending IO.
  HANDLE event = CreateEvent(nullptr, TRUE, FALSE, nullptr);
  if (!event) return {};
  auto eventGuard = qScopeGuard([&] { CloseHandle(event); });
  QElapsedTimer deadline;
  deadline.start();
  const auto transfer = [&](bool writing, void* buffer, DWORD size, DWORD& count) {
    if (deadline.elapsed() >= 5000) return false;
    OVERLAPPED operation{};
    operation.hEvent = event;
    ResetEvent(event);
    count = 0;
    const BOOL completed = writing ? WriteFile(pipe, buffer, size, &count, &operation)
                                   : ReadFile(pipe, buffer, size, &count, &operation);
    if (!completed && GetLastError() != ERROR_IO_PENDING) return false;
    const qint64 remaining = 5000 - deadline.elapsed();
    if (!completed && (remaining <= 0 || WaitForSingleObject(event, DWORD(remaining)) != WAIT_OBJECT_0)) {
      CancelIoEx(pipe, &operation);
      GetOverlappedResult(pipe, &operation, &count, TRUE);
      return false;
    }
    return GetOverlappedResult(pipe, &operation, &count, FALSE) && count > 0;
  };

  // Write the UAPI command into the pipe.
  QByteArray message = command.toLocal8Bit();
  while (!message.endsWith("\n\n")) {
    message.append('\n');
  }
  DWORD offset = 0;
  while (offset < DWORD(message.size())) {
    DWORD written = 0;
    if (!transfer(true, message.data() + offset, DWORD(message.size()) - offset, written)) return {};
    offset += written;
  }

  // Receive the response from the pipe.
  QByteArray reply;
  while (!reply.contains("\n\n")) {
    char buffer[512];
    DWORD read = 0;
    if (!transfer(false, buffer, sizeof(buffer), read)) return {};
    reply.append(buffer, read);
    if (reply.size() > 1024 * 1024) return {};
  }

  return QString::fromUtf8(reply).trimmed();
}

// static
static bool waitForServiceStatus(SC_HANDLE service, DWORD expectedStatus) {
  int tries = 0;
  DWORD previousState = MAXDWORD;
  DWORD previousCheckpoint = MAXDWORD;
  while (tries < 30) {
    SERVICE_STATUS status{};
    if (!QueryServiceStatus(service, &status)) {
      WindowsUtils::windowsLog("Failed to retrieve the service status");
      return false;
    }

    if (status.dwCurrentState != previousState || status.dwCheckPoint != previousCheckpoint) {
      logger.info() << "Tunnel service wait; expected=" << expectedStatus << "state=" << status.dwCurrentState
                    << "elapsedMs=" << tries * 1000 << "win32Exit=" << status.dwWin32ExitCode
                    << "serviceExit=" << status.dwServiceSpecificExitCode
                    << "checkpoint=" << status.dwCheckPoint << "waitHintMs=" << status.dwWaitHint;
      previousState = status.dwCurrentState;
      previousCheckpoint = status.dwCheckPoint;
    }
    if (status.dwCurrentState == expectedStatus) {
      return true;
    }
    if (expectedStatus == SERVICE_RUNNING && status.dwCurrentState == SERVICE_STOPPED) {
      logger.error() << "Tunnel startup ended before SERVICE_RUNNING; win32Exit=" << status.dwWin32ExitCode
                     << "serviceExit=" << status.dwServiceSpecificExitCode;
      return false;
    }

    Sleep(1000);
    ++tries;
  }

  logger.error() << "Tunnel service wait timed out; expected=" << expectedStatus
                 << "lastState=" << previousState << "elapsedMs=" << tries * 1000;
  return false;
}

// static
QString WindowsTunnelService::exitCodeToFailure(const void* status) {
  const SERVICE_STATUS* st = static_cast<const SERVICE_STATUS*>(status);
  if (st->dwWin32ExitCode != ERROR_SERVICE_SPECIFIC_ERROR) {
    return WindowsUtils::getErrorMessage(st->dwWin32ExitCode);
  }

  // The order of this error code is taken from wireguard.
  switch (st->dwServiceSpecificExitCode) {
    case 0:
      return "No error";
    case 1:
      return "Error when opening the ringlogger log file";
    case 2:
      return "Error while loading the WireGuard configuration file from "
             "path.";
    case 3:
      return "Error while creating a WinTun device.";
    case 4:
      return "Error while listening on a named pipe.";
    case 5:
      return "Error while resolving DNS hostname endpoints.";
    case 6:
      return "Error while manipulating firewall rules.";
    case 7:
      return "Error while setting the device configuration.";
    case 8:
      return "Error while binding sockets to default routes.";
    case 9:
      return "Unable to set interface addresses, routes, dns, and/or "
             "interface settings.";
    case 10:
      return "Error while determining current executable path.";
    case 11:
      return "Error while opening the NUL file.";
    case 12:
      return "Error while attempting to track tunnels.";
    case 13:
      return "Error while attempting to enumerate current sessions.";
    case 14:
      return "Error while dropping privileges.";
    case 15:
      return "Windows internal error.";
    default:
      return QString("Unknown error (%1)").arg(st->dwServiceSpecificExitCode);
  }
}
