#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <QByteArray>
#include <QHash>
#include <QHostAddress>
#include <QScopeGuard>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <vector>
#include "defaultRouteSelection.h"

struct Log {
    Log& debug() { return *this; }
    Log& error() { return *this; }
    QString sensitive(const QString& text) { return text; }
    template<class T> Log& operator<<(const T&) { return *this; }
} logger;
constexpr ULONG SITE_EXCLUSION_ROUTE_METRIC = 0x5e73;
struct Link { ULONG metric = 25; bool up = true, connected = true, loopback = false, queryFails = false; };
QHash<quint64, Link> links;
std::vector<MIB_IPFORWARD_ROW2> rows;
QByteArray storage;
DWORD tableError = NO_ERROR;
int releases = 0;
DWORD fakeGetIpForwardTable2(ADDRESS_FAMILY, PMIB_IPFORWARD_TABLE2* table) {
    if (tableError) return tableError;
    storage.resize(int(sizeof(MIB_IPFORWARD_TABLE2) + rows.size() * sizeof(MIB_IPFORWARD_ROW2)));
    *table = reinterpret_cast<PMIB_IPFORWARD_TABLE2>(storage.data());
    (*table)->NumEntries = ULONG(rows.size());
    std::copy(rows.begin(), rows.end(), (*table)->Table);
    return NO_ERROR;
}
DWORD fakeGetIfEntry2(MIB_IF_ROW2* row) {
    const auto link = links.value(row->InterfaceLuid.Value);
    if (link.queryFails) return ERROR_ACCESS_DENIED;
    row->OperStatus = link.up ? IfOperStatusUp : IfOperStatusDown;
    row->Type = link.loopback ? IF_TYPE_SOFTWARE_LOOPBACK : IF_TYPE_ETHERNET_CSMACD;
    return NO_ERROR;
}
void fakeInitializeIpInterfaceEntry(MIB_IPINTERFACE_ROW* row) { *row = {}; }
DWORD fakeGetIpInterfaceEntry(MIB_IPINTERFACE_ROW* row) {
    const auto link = links.value(row->InterfaceLuid.Value);
    row->Metric = link.metric;
    row->Connected = link.connected;
    return NO_ERROR;
}
void fakeFreeMibTable(void*) { ++releases; }
#define GetIpForwardTable2 fakeGetIpForwardTable2
#define GetIfEntry2 fakeGetIfEntry2
#define InitializeIpInterfaceEntry fakeInitializeIpInterfaceEntry
#define GetIpInterfaceEntry fakeGetIpInterfaceEntry
#define FreeMibTable fakeFreeMibTable
struct Wireguard { quint64 getLuid() const { return 77; } };
struct WindowsDaemon {
    std::unique_ptr<Wireguard> m_wgutils = std::make_unique<Wireguard>();
    bool getDefaultGateway(quint32&, quint64&);
};
#include "production-exclusion-gateway.inc"

void check(bool ok, const char* message) { if (!ok) { std::cerr << message << '\n'; std::exit(1); } }
MIB_IPFORWARD_ROW2 route(ULONG index, ULONG metric, quint32 gateway = 0xc0000201) {
    MIB_IPFORWARD_ROW2 row{};
    row.InterfaceIndex = index;
    row.InterfaceLuid.Value = index;
    row.DestinationPrefix.Prefix.si_family = AF_INET;
    row.NextHop.Ipv4.sin_family = AF_INET;
    row.NextHop.Ipv4.sin_addr.s_addr = htonl(gateway);
    row.Metric = metric;
    row.ValidLifetime = 3600;
    return row;
}
int main() {
    WindowsDaemon daemon;
    quint32 gateway = 0;
    quint64 luid = 0;
    links[2] = {25}; links[5] = {50}; links[77] = {0};
    rows = {route(5, 250), route(77, 0), route(2, 256)};
    check(daemon.getDefaultGateway(gateway, luid) && luid == 2 && gateway == 0xc0000201,
          "public exclusions must use the combined metric and exclude the VPN");
    std::reverse(rows.begin(), rows.end());
    check(daemon.getDefaultGateway(gateway, luid) && luid == 2, "table order cannot choose the gateway");
    for (int invalid = 0; invalid < 7; ++invalid) {
        auto bad = route(5, 0);
        links[5] = {0};
        switch (invalid) {
        case 0: bad.NextHop.Ipv4.sin_addr.s_addr = 0; break;
        case 1: bad.ValidLifetime = 0; break;
        case 2: links[5].up = false; break;
        case 3: links[5].connected = false; break;
        case 4: links[5].loopback = true; break;
        case 5: links[5].queryFails = true; break;
        case 6: bad.DestinationPrefix.PrefixLength = 24; break;
        }
        rows = {bad, route(2,256)};
        check(daemon.getDefaultGateway(gateway,luid) && luid == 2, "invalid gateway must be skipped");
        rows = {bad};
        check(!daemon.getDefaultGateway(gateway,luid) && !gateway && !luid,
              "missing usable gateway cannot return stale output values");
    }
    const int freed = releases;
    tableError = ERROR_ACCESS_DENIED;
    gateway = 123; luid = 456;
    check(!daemon.getDefaultGateway(gateway,luid) && !gateway && !luid && releases == freed,
          "table read failure must not expose stale gateway or free an invalid table");
    std::cout << "PASS: production exclusion gateway uses combined metrics, live links and valid next hops\n";
}
