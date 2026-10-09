#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>
#include <QScopeGuard>
#include <QMetaObject>
#include <limits>
#include <cstring>
#include <iostream>
#include <thread>
#define private public
#include "../../client/platforms/windows/daemon/windowssplittunnel.h"
#undef private
struct Log {
  Log &error() { return *this; } Log &warning() { return *this; }
  Log &info() { return *this; } Log &debug() { return *this; }
  template<class T> Log &operator<<(const T &) { return *this; }
} logger;
struct IP_ADDRESSES_CONFIG { IN_ADDR TunnelIpv4, InternetIpv4; IN6_ADDR TunnelIpv6, InternetIpv6; };
namespace {
#include "production-address-helpers.inc"
}
#include "production-address-context.inc"
namespace WindowsCommons { int VPNAdapterIndex() { return 28; } }
static bool poisoned = false, ioctlOk = true;
static int writes = 0, subscriptions = 0, cancellations = 0, notifyFailAt = 0;
static DWORD endpointError = 0, addressError = 0;
static NET_LUID endpointAdapter{2};
static ULONG endpointSource = htonl(0xc0a80002);
static QList<MIB_UNICASTIPADDRESS_ROW> addresses;
static QList<MIB_IPFORWARD_ROW2> routes;
static IP_ADDRESSES_CONFIG applied{};
void check(bool ok, const char *message) { if (!ok) { std::cerr << message << '\n'; std::exit(1); } }
template<class T, class R> T *makeTable(const QList<R> &rows) {
  auto table = reinterpret_cast<T *>(std::calloc(1, sizeof(T) + rows.size() * sizeof(R)));
  table->NumEntries = static_cast<ULONG>(rows.size());
  for (int i=0; i<rows.size(); ++i) table->Table[i] = rows[i];
  return table;
}
DWORD fakeAddresses(ADDRESS_FAMILY, PMIB_UNICASTIPADDRESS_TABLE *out) {
  if (addressError) return addressError;
  *out = makeTable<MIB_UNICASTIPADDRESS_TABLE>(addresses); return NO_ERROR;
}
DWORD fakeRoutes(ADDRESS_FAMILY family, PMIB_IPFORWARD_TABLE2 *out) {
  QList<MIB_IPFORWARD_ROW2> selected;
  for (auto row : routes) if (row.DestinationPrefix.Prefix.si_family == family) selected.append(row);
  *out = makeTable<MIB_IPFORWARD_TABLE2>(selected); return NO_ERROR;
}
void fakeFree(void *table) { std::free(table); }
void fakeInit(MIB_IPINTERFACE_ROW *row) { *row = {}; }
DWORD fakeInterface(MIB_IPINTERFACE_ROW *row) {
  row->Connected = row->InterfaceLuid.Value != 99; row->Metric = row->InterfaceLuid.Value == 3 ? 50 : 10; return NO_ERROR;
}
DWORD fakeLuid(NET_IFINDEX index, NET_LUID *out) { out->Value = index; return NO_ERROR; }
DWORD fakeBest(const NET_LUID *, NET_IFINDEX, const SOCKADDR_INET *, const SOCKADDR_INET *destination,
               ULONG, MIB_IPFORWARD_ROW2 *route, SOCKADDR_INET *source) {
  if (endpointError) return endpointError;
  route->InterfaceLuid = endpointAdapter;
  source->si_family = destination->si_family; source->Ipv4.sin_addr.s_addr = endpointSource;
  return NO_ERROR;
}
DWORD fakeNotify(ADDRESS_FAMILY, PIPFORWARD_CHANGE_CALLBACK, PVOID, BOOLEAN, HANDLE *handle) {
  ++subscriptions; if (subscriptions == notifyFailAt) return ERROR_ACCESS_DENIED;
  *handle = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(subscriptions)); return NO_ERROR;
}
DWORD fakeNotify(ADDRESS_FAMILY, PUNICAST_IPADDRESS_CHANGE_CALLBACK, PVOID, BOOLEAN, HANDLE *handle) {
  ++subscriptions; if (subscriptions == notifyFailAt) return ERROR_ACCESS_DENIED;
  *handle = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(subscriptions)); return NO_ERROR;
}
DWORD fakeNotify(ADDRESS_FAMILY, PIPINTERFACE_CHANGE_CALLBACK, PVOID, BOOLEAN, HANDLE *handle) {
  ++subscriptions; if (subscriptions == notifyFailAt) return ERROR_ACCESS_DENIED;
  *handle = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(subscriptions)); return NO_ERROR;
}
DWORD fakeCancel(HANDLE) { ++cancellations; return NO_ERROR; }
BOOL DeviceIoControlWithTimeout(HANDLE, DWORD, void *data, DWORD size, void *, DWORD, DWORD *, const char *) {
  ++writes; check(size == sizeof(applied), "wrong driver configuration size");
  if (ioctlOk) std::memcpy(&applied, data, size);
  return ioctlOk;
}
#define IOCTL_REGISTER_IP_ADDRESSES 4
#define IOCTL_REGISTER_PROCESSES 3
#define GetUnicastIpAddressTable fakeAddresses
#define GetIpForwardTable2 fakeRoutes
#define FreeMibTable fakeFree
#define InitializeIpInterfaceEntry fakeInit
#define GetIpInterfaceEntry fakeInterface
#define ConvertInterfaceIndexToLuid fakeLuid
#define GetBestRoute2 fakeBest
#define NotifyRouteChange2 fakeNotify
#define NotifyUnicastIpAddressChange fakeNotify
#define NotifyIpInterfaceChange fakeNotify
#define CancelMibChangeNotify2 fakeCancel
#include "production-address-refresh.inc"
#include "production-address-start.inc"
WindowsSplitTunnel::WindowsSplitTunnel(HANDLE driver) : QObject(nullptr), m_driver(driver), m_addressRefreshTimer(this) {
  m_addressRefreshTimer.setSingleShot(true); m_addressRefreshTimer.setInterval(1);
  connect(&m_addressRefreshTimer, &QTimer::timeout, this, &WindowsSplitTunnel::refreshAddresses);
}
WindowsSplitTunnel::~WindowsSplitTunnel() { stopAddressMonitoring(); }
bool WindowsSplitTunnel::isUnresponsive() const { return poisoned; }
WindowsSplitTunnel::DRIVER_STATE WindowsSplitTunnel::getState() { return STATE_READY; }
QString WindowsSplitTunnel::stateString() { return "test"; }
bool WindowsSplitTunnel::initDriver(HANDLE) { return true; }
std::vector<uint8_t> WindowsSplitTunnel::generateProcessBlob() { return {1}; }
void pump(int ms=300) {
  QElapsedTimer t; t.start(); do { QCoreApplication::processEvents(); QThread::msleep(1); } while (t.elapsed() < ms);
}
void addAddress(int adapter, const char *ip, NL_DAD_STATE dad = IpDadStatePreferred) {
  MIB_UNICASTIPADDRESS_ROW row{}; row.InterfaceLuid.Value=adapter; row.DadState=dad;
  row.Address.si_family = strchr(ip, ':') ? AF_INET6 : AF_INET;
  if (row.Address.si_family == AF_INET) InetPtonA(AF_INET,ip,&row.Address.Ipv4.sin_addr);
  else InetPtonA(AF_INET6,ip,&row.Address.Ipv6.sin6_addr);
  addresses.append(row);
}
void addRoute(int adapter, int metric, ADDRESS_FAMILY family = AF_INET) {
  MIB_IPFORWARD_ROW2 row{}; row.InterfaceLuid.Value=adapter; row.Metric=metric;
  row.ValidLifetime=100; row.DestinationPrefix.Prefix.si_family=family; routes.append(row);
}
int main(int argc, char **argv) {
  QCoreApplication app(argc,argv); WindowsSplitTunnel manager(reinterpret_cast<HANDLE>(1));
  addAddress(28,"10.33.0.2"); addAddress(2,"192.168.0.2"); addRoute(2,20);
  check(manager.start(QHostAddress("203.0.113.10"),2,28) && subscriptions == 3,"start subscribes to all network events");
  int failures=0;
  QObject::connect(&manager,&WindowsSplitTunnel::addressRefreshFailed,[&](quint64){ ++failures; });
  check(writes == 1,"initial configuration written once");
  check(applied.InternetIpv4.s_addr == endpointSource,"LAN physical source remains valid");
  int before=writes; manager.scheduleAddressRefresh(manager.m_addressGeneration); pump();
  check(writes == before,"unchanged network must not reauthorize flows");
  endpointSource=htonl(0xc0a80003);
  std::thread notification([&]{ WindowsSplitTunnel::dispatchAddressRefresh(manager.m_notificationContext); }); notification.join(); pump();
  check(writes == before+1 && applied.InternetIpv4.s_addr == endpointSource,"physical IP renewal");
  auto oldGeneration=manager.m_addressGeneration;
  manager.scheduleAddressRefresh(oldGeneration); manager.stopAddressMonitoring(); pump();
  check(writes == before+1 && cancellations == 3,"queued callback after stop must not write");
  check(manager.startAddressMonitoring(),"restart monitor"); manager.m_addressMonitoringActive=true;
  manager.scheduleAddressRefresh(oldGeneration); pump(); check(writes == before+1,"previous session event must not modify new session");
  endpointError=ERROR_NETWORK_UNREACHABLE; routes.clear();
  manager.scheduleAddressRefresh(manager.m_addressGeneration); pump();
  IP_ADDRESSES_CONFIG empty{}; check(std::memcmp(&applied,&empty,sizeof(empty)) == 0,"network removal must clear stale addresses");
  endpointError=0; addRoute(2,20); manager.scheduleAddressRefresh(manager.m_addressGeneration); pump();
  check(applied.InternetIpv4.s_addr == endpointSource,"network recovery without VPN restart");
  // Dual-stack updates exclude scoped link-local addresses and tentative DAD entries.
  addRoute(2,20,AF_INET6); addAddress(2,"fe80::1"); addAddress(2,"2001:db8::1",IpDadStateTentative);
  addAddress(2,"2001:db8::2"); addAddress(28,"fd00::2");
  auto blob=manager.generateIPConfiguration(); auto config=reinterpret_cast<const IP_ADDRESSES_CONFIG*>(blob.data());
  IN6_ADDR expected{}; InetPtonA(AF_INET6,"2001:db8::2",&expected);
  check(std::memcmp(&config->InternetIpv6,&expected,sizeof(expected)) == 0,"DAD and link-local selection");
  check(splittingMode(availability(*config)) == 1,"dual-stack driver mode");
  addRoute(28,0); addRoute(99,0); addRoute(3,0);
  check(manager.getBestDefaultRoute(AF_INET,NET_LUID{2})->Value == 2,"default route combines metric and excludes tunnel/down adapters");
  endpointAdapter.Value=28; check(manager.generateIPConfiguration().empty(),"endpoint inside tunnel must be rejected"); endpointAdapter.Value=2;
  // Timer retries are bounded; failure does not cache the rejected configuration.
  endpointSource=htonl(0xc0a80004); ioctlOk=false; before=writes;
  for (int i=0;i<4;++i) { manager.m_addressRefreshTimer.stop(); manager.refreshAddresses(); }
  manager.m_addressRefreshTimer.stop(); check(writes == before+4 && failures == 1,"bounded IOCTL failure retries");
  ioctlOk=true; manager.scheduleAddressRefresh(manager.m_addressGeneration); pump();
  check(applied.InternetIpv4.s_addr == endpointSource,"new event retries failed configuration");
  poisoned=true; before=writes; manager.refreshAddresses(); check(writes == before && failures == 2,"quarantined driver must not receive requests"); poisoned=false;
  manager.stopAddressMonitoring(); subscriptions=0; cancellations=0; notifyFailAt=2;
  check(!manager.startAddressMonitoring() && cancellations == 1 && manager.m_notificationContext == nullptr,"partial subscription rollback");
  notifyFailAt=0; addressError=ERROR_ACCESS_DENIED;
  check(!manager.start(QHostAddress("203.0.113.10"),2,28) && !manager.m_addressMonitoringActive &&
        manager.m_notificationContext == nullptr, "failed initial collection unregisters monitor");
  addressError=0;
  check(manager.start(QHostAddress("203.0.113.10"),2,28),"startup can recover after collection failure");
  // DAD settling cannot keep the retry timer alive indefinitely.
  addresses.clear(); addAddress(28,"10.33.0.2"); addAddress(28,"fd00::2");
  addAddress(2,"2001:db8::1",IpDadStateTentative);
  int previousFailures=failures;
  for (int i=0;i<4;++i) { manager.m_addressRefreshTimer.stop(); manager.refreshAddresses(); }
  check(failures == previousFailures+1 && !manager.m_addressRefreshTimer.isActive(),"DAD retries are bounded");
  std::cout << "Address refresh lifecycle and route selection passed\n";
}
