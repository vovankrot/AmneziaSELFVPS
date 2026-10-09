#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <QHostAddress>
#include <QHash>
#include <QJsonObject>
#include <QStringList>
#include <QScopeGuard>
#include <QCoreApplication>
#include <QTimer>
#include <QElapsedTimer>
#include <QThread>
#include <iostream>
#include <memory>
#include <cstring>
#include "ipaddress.h"
struct Log {
    Log &error() { return *this; } Log &warning() { return *this; } Log &debug() { return *this; }
    template<class T> Log &operator<<(const T &) { return *this; }
} logger;
void check(bool ok, const char *message) { if (!ok) { std::cerr << message << '\n'; std::exit(1); } }
class WindowsSplitTunnel {
public:
    enum State { STATE_UNKNOWN, STATE_STARTED, STATE_INITIALIZED, STATE_READY, STATE_RUNNING, STATE_ZOMBIE };
    HANDLE m_driver = reinterpret_cast<HANDLE>(1);
    bool unresponsive = false, resetOk = true, resetReachesStarted = true, excludeOk = true;
    State state = STATE_RUNNING;
    int resets = 0, starts = 0, exclusions = 0;
    bool isUnresponsive() const { return unresponsive; }
    State getState() { return state; }
    bool resetDriver(HANDLE) { ++resets; if (resetOk && resetReachesStarted) state = STATE_STARTED; return resetOk; }
    void stopAddressMonitoring() {}
    std::vector<std::byte> m_lastIPConfiguration;
    bool stop();
    bool start(const QHostAddress &, int, int = -1) { ++starts; state = STATE_RUNNING; return true; }
    bool excludeApps(const QStringList &) { ++exclusions; return excludeOk; }
    bool isRunning() const { return state == STATE_RUNNING; }
};
#include "production-driver-stop.inc"
struct InterfaceConfig { QStringList m_vpnDisabledApps; };
namespace DaemonError { enum { ERROR_SPLIT_TUNNEL_START_FAILURE, ERROR_SPLIT_TUNNEL_EXCLUDE_FAILURE }; }
QStringList sanitizeSplitTunnelApps(const QStringList &paths) {
    QStringList result; for (const auto &p : paths) if (p.endsWith(".exe")) result.append(p); return result;
}
class WindowsDaemon {
public:
    enum Op { Up, Down, Switch };
    QHostAddress m_serverEndpoint;
    void prepareActivation(const InterfaceConfig &) {}
    std::unique_ptr<WindowsSplitTunnel> m_splitTunnelManager;
    bool m_appBypassActive = false;
    QStringList m_activeAppBypassPaths;
    int m_inetAdapterIndex = 2, creates = 0, failures = 0;
    void tryRestoreSplitTunnelManager() { ++creates; m_splitTunnelManager = std::make_unique<WindowsSplitTunnel>(); }
    bool fallBackFromUnresponsiveSplitTunnel(const char *) { return false; }
    void backendFailure(int) { ++failures; } void networkPolicyWarning(const QString &) {}
    static QString tr(const char *s) { return QString::fromUtf8(s); }
    bool activateSplitTunnel(const InterfaceConfig &, int); bool run(Op, const InterfaceConfig &);
};
#include "production-lazy-driver.inc"
int getInterfaceError = 0, setInterfaceError = 0, createError = 0, deleteError = 0, tableError = 0;
int sets = 0, creates = 0, frees = 0, reads = 0;
MIB_IPINTERFACE_ROW lastInterface{};
QList<MIB_IPFORWARD_ROW2> tableRows;
QByteArray tableStorage;
void fakeInitializeIpInterfaceEntry(MIB_IPINTERFACE_ROW *row) { std::memset(row, 0, sizeof(*row)); }
DWORD fakeGetIpInterfaceEntry(MIB_IPINTERFACE_ROW *row) {
    row->DisableDefaultRoutes = TRUE; row->UseAutomaticMetric = TRUE; row->Metric = 4230; row->SitePrefixLength = 16;
    row->NlMtu = 1400; return getInterfaceError;
}
DWORD fakeSetIpInterfaceEntry(MIB_IPINTERFACE_ROW *row) { ++sets; lastInterface = *row; return setInterfaceError; }
void fakeInitializeIpForwardEntry(MIB_IPFORWARD_ROW2 *row) { std::memset(row, 0, sizeof(*row)); }
DWORD fakeCreateIpForwardEntry2(MIB_IPFORWARD_ROW2 *) { ++creates; return createError; }
DWORD fakeDeleteIpForwardEntry2(MIB_IPFORWARD_ROW2 *) { return deleteError; }
DWORD fakeGetIpForwardTable2(int, PMIB_IPFORWARD_TABLE2 *table) {
    ++reads; if (tableError) return tableError;
    tableStorage.resize(int(sizeof(MIB_IPFORWARD_TABLE2) + tableRows.size() * sizeof(MIB_IPFORWARD_ROW2)));
    *table = reinterpret_cast<PMIB_IPFORWARD_TABLE2>(tableStorage.data()); (*table)->NumEntries = ULONG(tableRows.size());
    for (int i=0; i<tableRows.size(); ++i) (*table)->Table[i] = tableRows[i]; return NO_ERROR;
}
void fakeFreeMibTable(void *) { ++frees; }
#define InitializeIpInterfaceEntry fakeInitializeIpInterfaceEntry
#define GetIpInterfaceEntry fakeGetIpInterfaceEntry
#define SetIpInterfaceEntry fakeSetIpInterfaceEntry
#define InitializeIpForwardEntry fakeInitializeIpForwardEntry
#define CreateIpForwardEntry2 fakeCreateIpForwardEntry2
#define DeleteIpForwardEntry2 fakeDeleteIpForwardEntry2
#define GetIpForwardTable2 fakeGetIpForwardTable2
#define FreeMibTable fakeFreeMibTable
constexpr ULONG EXCLUSION_ROUTE_METRIC = 0x5e72;
#include "production-local-prefix.inc"
class WindowsRouteMonitor {
public:
    quint64 m_luid = 77;
    bool m_defaultRouteCapture = true, m_allowLocalNetwork = true;
    QHash<IPAddress, MIB_IPFORWARD_ROW2*> m_clonedRoutes, m_exclusionRoutes;
    ~WindowsRouteMonitor() { qDeleteAll(m_clonedRoutes); qDeleteAll(m_exclusionRoutes); }
    bool addExclusionRoute(const IPAddress &prefix);
    void updateInterfaceMetrics(int) {}
    void updateExclusionRoute(MIB_IPFORWARD_ROW2*, void*) {}
    bool setDetaultRouteCapture(bool enabled);
    bool flushRouteTable(QHash<IPAddress, MIB_IPFORWARD_ROW2*> &);
    bool isRouteExcluded(const IP_ADDRESS_PREFIX *prefix) const {
        for (auto it = m_exclusionRoutes.cbegin(); it != m_exclusionRoutes.cend(); ++it)
            if (it.key().address() == prefixToAddress(prefix)
                && it.key().prefixLength() == prefix->PrefixLength) return true;
        return false;
    }
    static QHostAddress prefixToAddress(const IP_ADDRESS_PREFIX *p) {
        return p->Prefix.si_family == AF_INET ? QHostAddress(ntohl(p->Prefix.Ipv4.sin_addr.s_addr))
            : QHostAddress(p->Prefix.Ipv6.sin6_addr.s6_addr);
    }
    void updateCapturedRoutes(int); void updateCapturedRoutes(int, void *);
    bool deleteExclusionRoute(const IPAddress &);
};
#include "production-route-capture.inc"
#include "production-delete-exclusion.inc"
#include "production-flush-routes.inc"
class WireguardUtilsWindows {
public:
    quint64 m_luid = 77;
    WindowsRouteMonitor *m_routeMonitor = nullptr;
    static void buildMibForwardRow(const IPAddress &, MIB_IPFORWARD_ROW2 *entry) { std::memset(entry,0,sizeof(*entry)); }
    bool updateRoutePrefix(const IPAddress &);
    bool deleteRoutePrefix(const IPAddress &);
};
#include "production-default-route.inc"
#include "production-delete-default-route.inc"
struct Autostart {
    static inline bool enabled = false; static inline int repairs = 0;
    static bool isAutostart() { return enabled; } static void setAutostart(bool) { ++repairs; }
};
struct SettingsController { bool automatic=true; bool isAutoConnectEnabled() const { return automatic; } };
struct ServersModel { int index=0; int getDefaultServerIndex() const { return index; } };
struct ConnectionController {
    bool connected=false, connecting=false; int opens=0;
    bool isConnected() const { return connected; } bool isConnectionInProgress() const { return connecting; }
    void openConnection() { ++opens; connecting=true; }
};
struct FakeReplica { bool valid=false; bool isReplicaValid() const { return valid; } };
struct IpcClient {
    static inline std::shared_ptr<FakeReplica> iface = std::make_shared<FakeReplica>();
    static auto InterfaceWithoutWait() { return iface; }
};
class CoreController : public QObject {
public:
    std::unique_ptr<SettingsController> m_settingsController = std::make_unique<SettingsController>();
    std::unique_ptr<ServersModel> m_serversModel = std::make_unique<ServersModel>();
    std::unique_ptr<ConnectionController> m_connectionController = std::make_unique<ConnectionController>();
    void initAutoConnectHandler();
};
#include "production-autoconnect.inc"
void eventsFor(int ms) {
    QElapsedTimer timer; timer.start();
    while(timer.elapsed()<ms) { QCoreApplication::processEvents(); QThread::msleep(1); }
}
MIB_IPFORWARD_ROW2 row(const QString &network, int prefix, const QString &gateway = "192.0.2.1") {
    MIB_IPFORWARD_ROW2 r{}; r.InterfaceLuid.Value=12;
    QHostAddress address(network), hop(gateway);
    if (address.protocol() == QAbstractSocket::IPv4Protocol) {
        r.DestinationPrefix.Prefix.Ipv4.sin_family=AF_INET; r.DestinationPrefix.Prefix.Ipv4.sin_addr.s_addr=htonl(address.toIPv4Address());
        r.NextHop.Ipv4.sin_family=AF_INET; r.NextHop.Ipv4.sin_addr.s_addr=htonl(hop.toIPv4Address());
    } else {
        auto bytes=address.toIPv6Address(), next=hop.toIPv6Address();
        r.DestinationPrefix.Prefix.Ipv6.sin6_family=AF_INET6; std::memcpy(&r.DestinationPrefix.Prefix.Ipv6.sin6_addr,&bytes,16);
        r.NextHop.Ipv6.sin6_family=AF_INET6; std::memcpy(&r.NextHop.Ipv6.sin6_addr,&next,16);
    }
    r.DestinationPrefix.PrefixLength=UCHAR(prefix); return r;
}
#include "production-route-add-exclusion.inc"
int main(int argc, char **argv) {
    QCoreApplication app(argc,argv);
    {
        CoreController startup; startup.initAutoConnectHandler(); eventsFor(25);
        check(!startup.m_connectionController->opens,"autoconnect must await service readiness");
        IpcClient::iface->valid=true; eventsFor(35);
        check(startup.m_connectionController->opens==1,"autoconnect must dial only once after readiness");
    }
    {
        CoreController startup; startup.initAutoConnectHandler(); startup.m_connectionController->connecting=true; eventsFor(25);
        startup.m_connectionController->connecting=false; eventsFor(25);
        check(!startup.m_connectionController->opens,"manual connection must cancel delayed autoconnect");
    }
    {
        CoreController startup; startup.initAutoConnectHandler(); startup.m_settingsController->automatic=false; eventsFor(25);
        check(!startup.m_connectionController->opens,"changed startup preference must cancel dial");
        check(!Autostart::repairs,"disabled autostart must not write registry");
    }
    for (auto s : {WindowsSplitTunnel::STATE_INITIALIZED, WindowsSplitTunnel::STATE_READY, WindowsSplitTunnel::STATE_RUNNING}) {
        WindowsSplitTunnel driver; driver.state=s; check(driver.stop() && driver.resets==1 && driver.state==WindowsSplitTunnel::STATE_STARTED,"monitor must reset fully");
        check(driver.stop() && driver.resets==1,"stopped monitor must not reset twice");
    }
    WindowsSplitTunnel driver; driver.unresponsive=true; check(!driver.stop() && !driver.resets,"quarantined driver cannot receive IOCTL");
    driver.unresponsive=false; driver.resetOk=false; check(!driver.stop(),"reset rejection cannot be hidden");
    driver.resetOk=true; driver.resetReachesStarted=false; check(!driver.stop(),"incomplete reset cannot be hidden");
    for (auto s : {WindowsSplitTunnel::STATE_UNKNOWN, WindowsSplitTunnel::STATE_ZOMBIE}) {
        WindowsSplitTunnel invalid; invalid.state=s; check(!invalid.stop() && !invalid.resets,"unknown driver state cannot be treated as stopped");
    }
    WindowsDaemon daemon; InterfaceConfig config;
    check(daemon.activateSplitTunnel(config, 7) && !daemon.creates,"disabled app bypass cannot initialize monitor");
    config.m_vpnDisabledApps={"bad-path"}; check(daemon.run(WindowsDaemon::Up,config) && !daemon.creates,"invalid list cannot initialize monitor");
    config.m_vpnDisabledApps={"fixture.exe", "bad-path"}; check(daemon.activateSplitTunnel(config,7) && daemon.creates==1 && daemon.m_appBypassActive,"valid bypass creates once");
    check(daemon.m_activeAppBypassPaths == QStringList{"fixture.exe"}, "firewall must receive only accepted driver paths");
    daemon.m_splitTunnelManager->excludeOk = false;
    check(!daemon.activateSplitTunnel(config,7) && daemon.m_activeAppBypassPaths.isEmpty()
          && !daemon.m_appBypassActive,"failed reconfiguration must not publish previous paths as accepted");
    daemon.m_splitTunnelManager->excludeOk = true;
    check(daemon.run(WindowsDaemon::Down,config) && daemon.m_splitTunnelManager->resets==1,"native shutdown must reset monitor");
    check(daemon.activateSplitTunnel({},7) && daemon.m_activeAppBypassPaths.isEmpty()
          && !daemon.m_appBypassActive,"disabled bypass must forget previous accepted paths");
    WindowsRouteMonitor monitor; monitor.m_defaultRouteCapture=false;
    WireguardUtilsWindows vpn; vpn.m_routeMonitor=&monitor;
    getInterfaceError=ERROR_ACCESS_DENIED; check(!vpn.updateRoutePrefix(IPAddress("0.0.0.0/0")) && !sets && !creates,"read failure cannot write routes");
    getInterfaceError=0; setInterfaceError=ERROR_ACCESS_DENIED;
    check(!vpn.updateRoutePrefix(IPAddress("0.0.0.0/0")) && !creates,"interface write failure cannot create route");
    setInterfaceError=0; createError=ERROR_ACCESS_DENIED;
    check(!vpn.updateRoutePrefix(IPAddress("0.0.0.0/0")) && !monitor.m_defaultRouteCapture,"failed default route cannot enable capture");
    createError=0; check(vpn.updateRoutePrefix(IPAddress("0.0.0.0/0")),"default route success");
    check(lastInterface.InterfaceLuid.Value==77 && lastInterface.Family==AF_INET && !lastInterface.DisableDefaultRoutes
        && !lastInterface.UseAutomaticMetric && !lastInterface.Metric && !lastInterface.SitePrefixLength && lastInterface.NlMtu==1400,"AWG must adjust only tunnel routing fields");
    tableRows={row("10.1.0.0",16),row("192.168.1.0",24,"0.0.0.0"),row("100.64.0.0",10),row("203.0.113.0",24,"0.0.0.0"),row("198.51.100.0",24),row("fd00::",64,"fe80::1"),row("2001:db8:1::",64,"::")};
    auto unspecified = row("192.0.2.0",24); unspecified.NextHop.si_family=AF_UNSPEC;
    tableRows.append(unspecified);
    monitor.updateCapturedRoutes(AF_UNSPEC);
    check(frees==1 && monitor.m_clonedRoutes.size()==1 && monitor.m_clonedRoutes.contains(IPAddress("198.51.100.0/24")),"private/corporate/public on-link LAN must not be cloned");
    tableError=ERROR_ACCESS_DENIED; monitor.updateCapturedRoutes(AF_UNSPEC); check(frees==1,"failed table read must not dereference/free uninitialized table");
    tableError=0; tableRows.clear(); deleteError=ERROR_ACCESS_DENIED; monitor.updateCapturedRoutes(AF_UNSPEC);
    check(monitor.m_clonedRoutes.size()==1,"failed deletion must retain captured route ownership");
    deleteError=0; monitor.updateCapturedRoutes(AF_UNSPEC); check(monitor.m_clonedRoutes.isEmpty(),"retry must clean captured route");
    tableRows={row("198.51.100.0",24)};
    monitor.updateCapturedRoutes(AF_INET);
    check(monitor.m_clonedRoutes.size()==1,"routed prefix must be captured before exclusion");
    const IPAddress newExclusion("198.51.100.0/24");
    check(monitor.addExclusionRoute(newExclusion) && monitor.m_clonedRoutes.isEmpty(),
          "new exclusion must remove a competing captured route in the same pass");
    check(monitor.deleteExclusionRoute(newExclusion),"new exclusion can be removed");
    tableRows.clear(); monitor.updateCapturedRoutes(AF_UNSPEC);
    auto prefix=IPAddress("192.0.2.1/32"); auto *owned=new MIB_IPFORWARD_ROW2{}; owned->DestinationPrefix.Prefix.si_family=AF_INET;
    monitor.m_exclusionRoutes.insert(prefix,owned); deleteError=ERROR_ACCESS_DENIED;
    check(!monitor.deleteExclusionRoute(prefix) && monitor.m_exclusionRoutes.contains(prefix),"failed exclusion removal must retain ownership");
    deleteError=0; check(monitor.deleteExclusionRoute(prefix) && monitor.m_exclusionRoutes.isEmpty(),"exclusion removal retry");
    monitor.m_clonedRoutes.insert(prefix,new MIB_IPFORWARD_ROW2{});
    deleteError=ERROR_ACCESS_DENIED;
    check(!monitor.setDetaultRouteCapture(false) && monitor.m_clonedRoutes.size()==1 && !monitor.m_defaultRouteCapture,
        "failed flush must retain ledger and disable further capture");
    deleteError=ERROR_FILE_NOT_FOUND;
    check(monitor.setDetaultRouteCapture(false) && monitor.m_clonedRoutes.isEmpty(),"Windows error 2 must release absent captured route");
    monitor.m_clonedRoutes.insert(prefix,new MIB_IPFORWARD_ROW2{});
    deleteError=ERROR_NOT_FOUND;
    check(monitor.setDetaultRouteCapture(false) && monitor.m_clonedRoutes.isEmpty(),"absent route completes retry");
    monitor.m_exclusionRoutes.insert(prefix,new MIB_IPFORWARD_ROW2{});
    const int previousReads=reads;
    check(monitor.addExclusionRoute(prefix) && monitor.m_exclusionRoutes.size()==1 && reads==previousReads,
        "endpoint already installed through config exclusions must not reject peer creation or allocate another row");
    deleteError=ERROR_FILE_NOT_FOUND;
    check(monitor.deleteExclusionRoute(prefix) && monitor.m_exclusionRoutes.isEmpty(),"Windows error 2 must release absent exclusion");
    deleteError=ERROR_FILE_NOT_FOUND;
    check(vpn.deleteRoutePrefix(prefix),"Windows error 2 must allow cleanup of an absent interface route");
    deleteError=ERROR_ACCESS_DENIED;
    check(!vpn.deleteRoutePrefix(prefix),"interface route access failure cannot be hidden");
    std::cout << "PASS: lazy driver/reset, AWG default-route failures, LAN preservation and native route ownership\n";
}
