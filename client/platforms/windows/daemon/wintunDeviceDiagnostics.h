#pragma once

#include <Windows.h>
#include <winevt.h>
#include <string>
#include <QStringList>
#include <QXmlStreamReader>

namespace WintunDeviceDiagnostics {
// Only explicitly selected device fields may reach the VPN log. Never log
// arbitrary Event Viewer XML: other event types can contain service arguments.
inline QString failureFromXml(const QString& xml) {
  QXmlStreamReader reader(xml);
  QString device, driver, problem, status, service;
  while (!reader.atEnd()) {
    reader.readNext();
    if (!reader.isStartElement() || reader.name() != QStringLiteral("Data")) continue;
    const QString name = reader.attributes().value(QStringLiteral("Name")).toString();
    const QString value = reader.readElementText();
    if (name == "DeviceInstanceId") device = value;
    else if (name == "DriverName") driver = value;
    else if (name == "Problem") problem = value;
    else if (name == "Status") status = value;
    else if (name == "ServiceName") service = value;
  }
  if (reader.hasError() || !device.startsWith(QStringLiteral("SWD\\Wintun\\"), Qt::CaseInsensitive)
      || service.compare(QStringLiteral("wintun"), Qt::CaseInsensitive) != 0
      || problem.isEmpty() || status.isEmpty()) return {};
  return QStringLiteral("device=%1 driver=%2 problem=%3 ntstatus=%4")
      .arg(device.left(256), driver.left(128), problem.left(32), status.left(32));
}

inline QStringList recentFailures() {
  QStringList result;
  // System DLL only, and no new linker dependency in the client/service.
  HMODULE module = LoadLibraryExW(L"wevtapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (!module) return {QStringLiteral("PnP diagnostics unavailable: load error=%1").arg(GetLastError())};
  const auto query = reinterpret_cast<decltype(&EvtQuery)>(GetProcAddress(module, "EvtQuery"));
  const auto next = reinterpret_cast<decltype(&EvtNext)>(GetProcAddress(module, "EvtNext"));
  const auto render = reinterpret_cast<decltype(&EvtRender)>(GetProcAddress(module, "EvtRender"));
  const auto close = reinterpret_cast<decltype(&EvtClose)>(GetProcAddress(module, "EvtClose"));
  if (!query || !next || !render || !close) { FreeLibrary(module); return {QStringLiteral("PnP diagnostics API unavailable")}; }
  EVT_HANDLE events = query(nullptr, L"Microsoft-Windows-Kernel-PnP/Configuration",
      L"*[System[(EventID=411) and TimeCreated[timediff(@SystemTime) <= 60000]]]",
      EvtQueryChannelPath | EvtQueryReverseDirection);
  if (!events) {
    result << QStringLiteral("PnP diagnostics query failed: error=%1").arg(GetLastError());
  } else {
    // No subprocess, no waiting for new events; cap both event count and XML size.
    for (int i = 0; i < 16; ++i) {
      EVT_HANDLE event = nullptr;
      DWORD returned = 0;
      if (!next(events, 1, &event, 0, 0, &returned)) {
        const DWORD error = GetLastError();
        if (error != ERROR_NO_MORE_ITEMS) result << QStringLiteral("PnP diagnostics read failed: error=%1").arg(error);
        break;
      }
      DWORD bytes = 0, properties = 0;
      render(nullptr, event, EvtRenderEventXml, 0, nullptr, &bytes, &properties);
      if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && bytes > 0 && bytes <= 65536) {
        std::wstring xml(bytes / sizeof(wchar_t) + 1, L'\0');
        if (render(nullptr, event, EvtRenderEventXml, bytes, xml.data(), &bytes, &properties)) {
          const auto failure = failureFromXml(QString::fromWCharArray(xml.c_str()));
          if (!failure.isEmpty()) result << QStringLiteral("Recent Wintun PnP failure (last 60s): ") + failure;
        }
      }
      close(event);
    }
    close(events);
    if (result.isEmpty()) result << QStringLiteral("No recent Wintun PnP failure recorded");
  }
  FreeLibrary(module);
  return result;
}
} // namespace WintunDeviceDiagnostics
