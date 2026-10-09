#include <Windows.h>
#include <QDebug>
#include <iostream>
#include <vector>
#include "../../client/platforms/windows/daemon/wintunDeviceDiagnostics.h"

QString output;
struct Log { QDebug info() { return QDebug(&output); } QDebug error() { return QDebug(&output); } } logger;
std::vector<SERVICE_STATUS> statuses;
int queries = 0, sleeps = 0;
BOOL fakeQueryServiceStatus(SC_HANDLE, SERVICE_STATUS* status) {
  *status = statuses[qMin(queries++, int(statuses.size()) - 1)]; return TRUE;
}
void fakeSleep(DWORD) { ++sleeps; }
namespace WindowsUtils { void windowsLog(const char*) {} }
#define QueryServiceStatus fakeQueryServiceStatus
#define Sleep fakeSleep
#include "production-service-wait.inc"

void check(bool ok, const char* why) { if (!ok) { std::cerr << why << '\n'; std::exit(1); } }
SERVICE_STATUS state(DWORD value, DWORD exit = 0, DWORD specific = 0) {
  SERVICE_STATUS result{}; result.dwCurrentState = value; result.dwWin32ExitCode = exit;
  result.dwServiceSpecificExitCode = specific; return result;
}
void reset(std::vector<SERVICE_STATUS> sequence) { statuses = std::move(sequence); queries = sleeps = 0; output.clear(); }
int main(int argc, char** argv) {
  if (argc == 2 && QString::fromLocal8Bit(argv[1]) == "--read-pnp") {
    for (const auto& line : WintunDeviceDiagnostics::recentFailures()) std::cout << line.toStdString() << '\n';
    return 0;
  }
  reset({state(SERVICE_START_PENDING), state(SERVICE_STOPPED, 1066, 3)});
  check(!waitForServiceStatus(nullptr, SERVICE_RUNNING) && queries == 2 && sleeps == 1,
      "terminated AWG must fail immediately rather than waiting the remaining 30 seconds");
  check(output.contains("1066") && output.contains("serviceExit= 3"), "preserve Win32 and tunnel-specific exit codes");
  reset({state(SERVICE_START_PENDING), state(SERVICE_RUNNING)});
  check(waitForServiceStatus(nullptr, SERVICE_RUNNING) && sleeps == 1, "pending to running succeeds");
  reset({state(SERVICE_STOP_PENDING), state(SERVICE_STOPPED)});
  check(waitForServiceStatus(nullptr, SERVICE_STOPPED), "normal stop still waits for completion");
  reset({state(SERVICE_START_PENDING)});
  check(!waitForServiceStatus(nullptr, SERVICE_RUNNING) && sleeps == 30 && output.contains("timed out"),
      "permanently pending service retains bounded timeout");
  const QString xml = QStringLiteral("<Event><EventData>"
      "<Data Name='DeviceInstanceId'>SWD\\Wintun\\{fixture}</Data>"
      "<Data Name='DriverName'>oem50.inf</Data><Data Name='ServiceName'>wintun</Data>"
      "<Data Name='Problem'>0x1f</Data><Data Name='Status'>0xc00002f0</Data>"
      "<Data Name='PrivateKey'>SECRET_MUST_NOT_APPEAR</Data>"
      "<Data Name='CommandLine'>PASSWORD_MUST_NOT_APPEAR</Data></EventData></Event>");
  const QString failure = WintunDeviceDiagnostics::failureFromXml(xml);
  check(failure.contains("oem50.inf") && failure.contains("0xc00002f0") && !failure.contains("MUST_NOT_APPEAR"),
      "PnP diagnostic extracts only device fields, never keys or command lines");
  QString unrelated = xml; unrelated.replace("SWD\\Wintun", "PCI\\OTHER");
  check(WintunDeviceDiagnostics::failureFromXml(unrelated).isEmpty(), "unrelated device failures excluded");
  check(WintunDeviceDiagnostics::failureFromXml(xml.left(xml.size() - 12)).isEmpty(), "malformed XML rejected");
  std::cout << "PASS: stopped startup, successful startup/stop, bounded timeout, PnP errors and secret exclusion\n";
}
