#include <QScopeGuard>
#include "router_win.h"

#include <cstring>
#include <string>

#include <QHostAddress>
#include <QProcess>

#include <core/networkUtilities.h>

// Helper: converts dotted IPv4 string to DWORD (network byte order), replacing deprecated inet_addr
static DWORD inetAddrFrom(const char* str) {
    IN_ADDR addr = {};
    InetPtonA(AF_INET, str, &addr);
    return addr.S_un.S_addr;
}

static QString savedRouteKey(const MIB_IPFORWARDROW &row) {
    return QString("%1/%2/%3/%4").arg(row.dwForwardDest).arg(row.dwForwardMask)
        .arg(row.dwForwardNextHop).arg(row.dwForwardIfIndex);
}


namespace {

enum class Ipv6RoutePresence {
    Unknown,
    Absent,
    Present,
};

struct LocalIpv4InterfaceMatch {
    bool found = false;
    NET_IFINDEX index = 0;
};

// Low interface metric so the VPN TUN wins both route tie-breaks and — more
// importantly — Windows DNS resolver ordering (resolvers are queried by
// ascending interface metric). The explicit per-interface DNS we set on tun2
// only takes effect first if tun2's metric is below the physical adapter's.
// Windows automatic metrics are 5 (fastest links) and up (25 Ethernet / 50
// Wi-Fi commonly), so a value below 5 guarantees the TUN is preferred. The old
// value 500 made the physical adapter win DNS; queries then hit the home
// resolver and were dropped by the killswitch — the classic "connected but
// sites don't open" symptom. (It was masked only because the metric set was
// failing with ERROR_INVALID_PARAMETER, see setInterfaceMetric.)
constexpr ULONG kTunInterfaceMetric = 3;

Ipv6RoutePresence getIpv6RoutePresence(const QNetworkInterface &iface, const QString &subnet)
{
    const auto parsedSubnet = QHostAddress::parseSubnet(subnet);
    if (parsedSubnet.first.protocol() != QAbstractSocket::IPv6Protocol || parsedSubnet.second < 0) {
        qWarning() << "RouterWin: invalid IPv6 subnet:" << subnet;
        return Ipv6RoutePresence::Unknown;
    }

    PMIB_IPFORWARD_TABLE2 table = nullptr;
    const DWORD result = GetIpForwardTable2(AF_INET6, &table);
    if (result != NO_ERROR || table == nullptr) {
        qWarning() << "RouterWin: failed to query IPv6 routing table:" << result;
        return Ipv6RoutePresence::Unknown;
    }

    const Q_IPV6ADDR routeAddress = parsedSubnet.first.toIPv6Address();
    Ipv6RoutePresence presence = Ipv6RoutePresence::Absent;

    for (ULONG i = 0; i < table->NumEntries; ++i) {
        const MIB_IPFORWARD_ROW2 &row = table->Table[i];
        if (row.InterfaceIndex != static_cast<NET_IFINDEX>(iface.index())) {
            continue;
        }

        if (row.DestinationPrefix.Prefix.si_family != AF_INET6 ||
            row.DestinationPrefix.PrefixLength != static_cast<UINT8>(parsedSubnet.second)) {
            continue;
        }

        if (memcmp(&row.DestinationPrefix.Prefix.Ipv6.sin6_addr,
                   &routeAddress,
                   sizeof(routeAddress)) == 0) {
            presence = Ipv6RoutePresence::Present;
            break;
        }
    }

    FreeMibTable(table);
    return presence;
}

bool runNetshIpv6RouteCommand(const QStringList &arguments, const QString &operation, const QString &subnet)
{
    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(QStringLiteral("netsh"), arguments);
    if (!process.waitForFinished()) {
        qWarning() << operation << "timed out for" << subnet;
        return false;
    }

    const QString output = QString::fromLocal8Bit(process.readAll()).trimmed();
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        qWarning().noquote()
            << QString("%1 failed for %2: %3").arg(operation, subnet, output);
        return false;
    }

    return true;
}

LocalIpv4InterfaceMatch findLocalIpv4InterfaceByAddress(const QString &ipv4)
{
    LocalIpv4InterfaceMatch match;

    for (const QNetworkInterface &iface : QNetworkInterface::allInterfaces()) {
        for (const QNetworkAddressEntry &entry : iface.addressEntries()) {
            if (entry.ip().protocol() != QAbstractSocket::IPv4Protocol) {
                continue;
            }

            if (entry.ip().toString() == ipv4) {
                match.found = true;
                match.index = static_cast<NET_IFINDEX>(iface.index());
                return match;
            }
        }
    }

    return match;
}

bool setInterfaceMetric(const NET_LUID &luid, ADDRESS_FAMILY family, ULONG metric, const QString &dev)
{
    MIB_IPINTERFACE_ROW row;
    InitializeIpInterfaceEntry(&row);
    row.InterfaceLuid = luid;
    row.Family = family;

    DWORD result = GetIpInterfaceEntry(&row);
    if (result != NO_ERROR) {
        qWarning() << "RouterWin: failed to query interface metric for" << dev << "error:" << result;
        return false;
    }

    if (!row.UseAutomaticMetric && row.Metric == metric) {
        return true;
    }

    row.UseAutomaticMetric = FALSE;
    row.Metric = metric;

    // SetIpInterfaceEntry fails with ERROR_INVALID_PARAMETER (87) unless these
    // members are reset after GetIpInterfaceEntry: for IPv4 the SitePrefixLength
    // must be 0 (otherwise the row read back from Get carries an invalid value),
    // and NlMtu / fields that the API treats as read-on-get must not be re-sent.
    // This is a documented WinAPI quirk — see MIB_IPINTERFACE_ROW remarks.
    if (family == AF_INET) {
        row.SitePrefixLength = 0;
    }

    result = SetIpInterfaceEntry(&row);
    if (result != NO_ERROR) {
        qWarning() << "RouterWin: failed to set interface metric for" << dev << "error:" << result;
        return false;
    }

    qDebug() << "RouterWin: set interface metric for" << dev << "to" << metric;
    return true;
}

}

QList<QString> RouterWin::kIpv6Subnets = { "fc00::/7", "2000::/4", "3000::/4" };

RouterWin &RouterWin::Instance()
{
    static RouterWin s;

    return s;
}

int RouterWin::routeAddList(const QString &gw, const QStringList &ips)
{
//    qDebug().noquote() << QString("ROUTE ADD List: IPs size:%1, GW: %2")
//                          .arg(ips.size())
//                          .arg(gw);

//    qDebug().noquote() << QString("ROUTE ADD List: IPs:\n%1")
//                          .arg(ips.join("\n"));


    if (!NetworkUtilities::checkIPv4Format(gw)) {
        qCritical().noquote() << "Trying to add invalid route, gw: " << gw;
        return 0;
    }

    PMIB_IPFORWARDTABLE pIpForwardTable = NULL;
    DWORD dwSize = 0;
    BOOL bOrder = FALSE;
    DWORD dwStatus = 0;


    // Find out how big our buffer needs to be.
    dwStatus = GetIpForwardTable(pIpForwardTable, &dwSize, bOrder);
    if (dwStatus == ERROR_INSUFFICIENT_BUFFER) {
        if (dwSize == 0 || dwSize > 10 * 1024 * 1024) {
            qWarning() << "routeAddList: invalid dwSize:" << dwSize;
            return 0;
        }
        // Allocate the memory for the table
        if (!(pIpForwardTable = (PMIB_IPFORWARDTABLE) malloc(dwSize))) {
            qDebug() << "Malloc failed. Out of memory.";
            return 0;
        }
        // Now get the table.
        dwStatus = GetIpForwardTable(pIpForwardTable, &dwSize, bOrder);
    }

    if (dwStatus != ERROR_SUCCESS) {
        qDebug() << "getIpForwardTable failed.";
        if (pIpForwardTable)
            free(pIpForwardTable);
        return 0;
    }

    const auto freeTable = qScopeGuard([&] { if (pIpForwardTable) free(pIpForwardTable); });
    int success_count = 0;
    MIB_IPFORWARDROW ipfrow;

    ipfrow.dwForwardPolicy = 0;
    ipfrow.dwForwardAge = INFINITE;

    const LocalIpv4InterfaceMatch localGwInterface = findLocalIpv4InterfaceByAddress(gw);
    const bool isOnLinkInterfaceRoute = localGwInterface.found;

    if (isOnLinkInterfaceRoute) {
        ipfrow.dwForwardNextHop = 0;
        ipfrow.dwForwardType = MIB_IPROUTE_TYPE_DIRECT;
        ipfrow.dwForwardIfIndex = localGwInterface.index;
        qDebug() << "Router::routeAddList: using on-link interface route for local gateway" << gw
                 << "ifIndex" << ipfrow.dwForwardIfIndex;
    } else {
        ipfrow.dwForwardNextHop = inetAddrFrom(gw.toStdString().c_str());
        ipfrow.dwForwardType = MIB_IPROUTE_TYPE_INDIRECT;	/* XXX - next hop != final dest */
    }
    ipfrow.dwForwardProto = MIB_IPPROTO_NETMGMT;	/* XXX - MIB_PROTO_NETMGMT */

    // Set iface for route
    if (!isOnLinkInterfaceRoute) {
        IPAddr dwGwAddr = inetAddrFrom(gw.toStdString().c_str());
        if (GetBestInterface(dwGwAddr, &ipfrow.dwForwardIfIndex) != NO_ERROR) {
            qDebug() << "Router::routeAddList : GetBestInterface failed";
            return false;
        }
    }

    // Get TAP iface metric to set it for new routes
    MIB_IPINTERFACE_ROW tap_iface;
    InitializeIpInterfaceEntry(&tap_iface);
    tap_iface.InterfaceIndex = ipfrow.dwForwardIfIndex;
    tap_iface.Family = AF_INET;
    dwStatus  = GetIpInterfaceEntry(&tap_iface);
    if (dwStatus == NO_ERROR){
        ipfrow.dwForwardMetric1 = tap_iface.Metric + 1;
    }
    else {
        qDebug() << "Router::routeAddList: failed GetIpInterfaceEntry(), Error:" << dwStatus;
        ipfrow.dwForwardMetric1 = 256;
    }
    ipfrow.dwForwardMetric2 = 0;
    ipfrow.dwForwardMetric3 = 0;
    ipfrow.dwForwardMetric4 = 0;
    ipfrow.dwForwardMetric5 = 0;

    for (int i = 0; i < ips.size(); ++i) {
        QString ipWithMask = ips.at(i);
        QString ip = NetworkUtilities::ipAddressFromIpWithSubnet(ipWithMask);

        if (!NetworkUtilities::checkIPv4Format(ip)) {
            qCritical().noquote() << "Critical, trying to add invalid route, ip: " << ip;
            continue;
        }

        QString mask = NetworkUtilities::netMaskFromIpWithSubnet(ipWithMask);

        // address
        ipfrow.dwForwardDest = inetAddrFrom(ip.toStdString().c_str());

        // mask
        in_addr maskAddr;
        inet_pton(AF_INET, mask.toStdString().c_str(), &maskAddr);
        ipfrow.dwForwardMask = maskAddr.S_un.S_addr;

        dwStatus = CreateIpForwardEntry(&ipfrow);
        if (dwStatus == NO_ERROR){
            m_ipForwardRows.insert(savedRouteKey(ipfrow), ipfrow);
            success_count++;
        }
        else if (dwStatus == ERROR_OBJECT_ALREADY_EXISTS) {
            // An existing OS route is not ours to delete. Verify its exact
            // path before treating it as the requested route.
            bool matches = false;
            for (DWORD row = 0; row < pIpForwardTable->dwNumEntries; ++row) {
                if (savedRouteKey(pIpForwardTable->table[row]) == savedRouteKey(ipfrow)) { matches = true; break; }
            }
            if (matches || m_ipForwardRows.contains(savedRouteKey(ipfrow))) ++success_count;
            else qWarning() << "Existing route does not match requested interface/gateway";
        }
        else {
            qDebug() << "Router::routeAdd: failed CreateIpForwardEntry(), Error:" << ip << gw << dwStatus;
        }
    }


    qDebug() << "Router::routeAddList finished, success: " << success_count << "/" << ips.size();

    // Never suspend Windows Connection Manager: large route lists must not
    // freeze unrelated downloads, connectivity checks or application startup.

    return success_count;
}

bool RouterWin::clearSavedRoutes()
{
    if (m_ipForwardRows.isEmpty()) return true;

    qDebug() << "RouterWin::clearSavedRoutes forward rows size:" << m_ipForwardRows.size();

    // Declare and initialize variables
    PMIB_IPFORWARDTABLE pIpForwardTable = NULL;
    DWORD dwSize = 0;
    BOOL bOrder = FALSE;
    DWORD dwStatus = 0;

    // Find out how big our buffer needs to be.
    dwStatus = GetIpForwardTable(pIpForwardTable, &dwSize, bOrder);
    if (dwStatus == ERROR_INSUFFICIENT_BUFFER) {
        // Allocate the memory for the table
        if (dwSize == 0 || dwSize > 10 * 1024 * 1024) {
            qWarning() << "clearSavedRoutes: invalid dwSize:" << dwSize;
            return false;
        }
        if (!(pIpForwardTable = (PMIB_IPFORWARDTABLE) malloc(dwSize))) {
            qDebug() << "Router::clearSavedRoutes : Malloc failed. Out of memory";
            return false;
        }
        // Now get the table.
        dwStatus = GetIpForwardTable(pIpForwardTable, &dwSize, bOrder);
    }

    if (dwStatus != ERROR_SUCCESS) {
        qDebug() << "Router::clearSavedRoutes : getIpForwardTable failed";
        if (pIpForwardTable)
            free(pIpForwardTable);

        return false;
    }

    int removed_count = 0;
    const int originalCount = m_ipForwardRows.size();
    for (auto i = m_ipForwardRows.begin(); i != m_ipForwardRows.end();) {
        dwStatus = DeleteIpForwardEntry(&i.value());
        if (dwStatus == ERROR_SUCCESS || dwStatus == ERROR_NOT_FOUND || dwStatus == ERROR_FILE_NOT_FOUND) {
            i = m_ipForwardRows.erase(i); ++removed_count;
        } else {
            qWarning() << "Router::clearSavedRoutes: deletion failed; retaining owned route" << dwStatus;
            ++i;
        }
    }

    if (pIpForwardTable)
        free(pIpForwardTable);

    qDebug() << "Router::clearSavedRoutes : removed routes:" << removed_count << "of" << originalCount;
    return m_ipForwardRows.isEmpty();
}

int RouterWin::routeDeleteList(const QString &gw, const QStringList &ips)
{
//    qDebug().noquote() << QString("ROUTE DELETE List: IPs size:%1, GW: %2")
//                          .arg(ips.size())
//                          .arg(gw);

//    qDebug().noquote() << QString("ROUTE DELETE List: IPs:\n%1")
//                          .arg(ips.join("\n"));

    PMIB_IPFORWARDTABLE pIpForwardTable = NULL;
    DWORD dwSize = 0;
    BOOL bOrder = FALSE;
    DWORD dwStatus = 0;
    ULONG gw_addr = inetAddrFrom(gw.toStdString().c_str());
    const LocalIpv4InterfaceMatch localGwInterface = findLocalIpv4InterfaceByAddress(gw);
    const bool isOnLinkInterfaceRoute = localGwInterface.found;

    // Find out how big our buffer needs to be.
    dwStatus = GetIpForwardTable(pIpForwardTable, &dwSize, bOrder);
    if (dwStatus == ERROR_INSUFFICIENT_BUFFER) {
        if (dwSize == 0 || dwSize > 10 * 1024 * 1024) {
            qWarning() << "routeDeleteList: invalid dwSize:" << dwSize;
            return 0;
        }
        // Allocate the memory for the table
        if (!(pIpForwardTable = (PMIB_IPFORWARDTABLE) malloc(dwSize))) {
            qDebug() << "Malloc failed. Out of memory.";
            return 0;
        }
        // Now get the table.
        dwStatus = GetIpForwardTable(pIpForwardTable, &dwSize, bOrder);
    }

    if (dwStatus != ERROR_SUCCESS) {
        qDebug() << "getIpForwardTable failed.";
        if (pIpForwardTable)
            free(pIpForwardTable);
        return 0;
    }

    int success_count = 0;

    QMap<ULONG, QPair<QString,ULONG>> ipMap;
    for (int i = 0; i < ips.size(); ++i) {
        QString ipMask = ips.at(i);
        if (ipMask.isEmpty()) continue;
        QString ip = NetworkUtilities::ipAddressFromIpWithSubnet(ipMask);
        QString mask = NetworkUtilities::netMaskFromIpWithSubnet(ipMask);

        if (ip.isEmpty()) continue;

        in_addr maskAddr;
        inet_pton(AF_INET, mask.toStdString().c_str(), &maskAddr);

        ipMap.insert(inetAddrFrom(ip.toStdString().c_str()), qMakePair(ipMask, maskAddr.S_un.S_addr));
    }

    for (int i = 0; i < (int) pIpForwardTable->dwNumEntries; i++) {
        MIB_IPFORWARDROW ipfrow = pIpForwardTable->table[i];

        const bool gatewayMatches = isOnLinkInterfaceRoute
                        ? (ipfrow.dwForwardIfIndex == localGwInterface.index &&
                           (ipfrow.dwForwardNextHop == 0 || ipfrow.dwForwardNextHop == gw_addr))
                        : (ipfrow.dwForwardNextHop == gw_addr);

        if(ipMap.contains(ipfrow.dwForwardDest) &&
                ipMap.value(ipfrow.dwForwardDest).second == ipfrow.dwForwardMask &&
            gatewayMatches) {
            dwStatus = DeleteIpForwardEntry(&pIpForwardTable->table[i]);
            if (dwStatus == ERROR_SUCCESS) {
                m_ipForwardRows.remove(savedRouteKey(ipfrow));
                success_count++;
            }
        }
    }

    // Free resources
    if (pIpForwardTable)
        free(pIpForwardTable);

    qDebug() << "Router::routeDeleteList finished, success: " << success_count << "/" << ips.size();

    return success_count;
}

bool RouterWin::flushDns()
{
    QProcess p;
    p.setProcessChannelMode(QProcess::MergedChannels);
    p.start(QStringLiteral("ipconfig"), QStringList() << QStringLiteral("/flushdns"));
    p.waitForFinished();
    if (p.exitStatus() != QProcess::NormalExit || p.exitCode() != 0) {
        qWarning() << "RouterWin::flushDns failed:" << p.readAll();
        return false;
    }
    return true;
}

void RouterWin::resetIpStack()
{
//    {
//        QProcess p;
//        QString command = QString("ipconfig /release");
//        p.start(command);
//    }
    {
        QProcess p;
        QString command = QString("netsh int ip reset");
        p.start(command);
        p.waitForFinished();
    }
    {
        QProcess p;
        QString command = QString("netsh winsock reset");
        p.start(command);
        p.waitForFinished();
    }
}

bool RouterWin::createTun(const QString &dev, const QString &subnet)
{
    NET_LUID luid;
    DWORD res = ConvertInterfaceAliasToLuid(reinterpret_cast<const wchar_t*>(dev.utf16()), &luid);
    if (res != NO_ERROR) {
        qCritical() << "Failed to convert luid: " << res;
        return false;
    }

    HANDLE hEvent = CreateEvent(nullptr, true, false, nullptr);
    if (!hEvent) {
        qCritical() << "Failed to allocate event object";
        return false;
    }
    auto _guardEvent = qScopeGuard([hEvent](){ CloseHandle(hEvent); });

    struct TunCallbackCtx {
        HANDLE hEvent;
        NET_LUID luid;
        QString subnet;   // copy, not reference — callback may fire after function returns
        bool found;
    };
    auto ctx = new TunCallbackCtx{ hEvent, luid, subnet, false };

    auto cb = [](void *priv, MIB_UNICASTIPADDRESS_ROW *row, MIB_NOTIFICATION_TYPE NotificationType) {
        auto* c = reinterpret_cast<TunCallbackCtx*>(priv);
        if (row != nullptr && row->InterfaceLuid.Value == c->luid.Value && row->Address.si_family == AF_INET) {
            char ip[INET_ADDRSTRLEN];
            inet_ntop(row->Address.Ipv4.sin_family, &row->Address.Ipv4.sin_addr, ip, INET_ADDRSTRLEN);
            if (c->subnet == ip) {
                c->found = true;
                SetEvent(c->hEvent);
            }
        }
    };

    HANDLE hNotif;
    res = NotifyUnicastIpAddressChange(AF_INET, cb, ctx, false, &hNotif);
    if (res != NO_ERROR) {
        qCritical() << "Failed to subscribe to interface change";
        delete ctx;
        return false;
    }
    auto _guardNotif = qScopeGuard([hNotif](){ CancelMibChangeNotify2(hNotif); });

    MIB_UNICASTIPADDRESS_ROW row;
    InitializeUnicastIpAddressEntry(&row);

    row.InterfaceLuid = luid;
    row.Address.si_family = AF_INET;

    inet_pton(AF_INET, subnet.toStdString().c_str(), &row.Address.Ipv4.sin_addr);

    row.OnLinkPrefixLength = 32;
    row.ValidLifetime = 0xffffffff;
    row.PreferredLifetime = 0xffffffff;
    row.DadState = IpDadStatePreferred;

    res = CreateUnicastIpAddressEntry(&row);
    if (res == ERROR_OBJECT_ALREADY_EXISTS) {
        ctx->found = true;
        SetEvent(hEvent);
    }

    if (res != NO_ERROR && res != ERROR_OBJECT_ALREADY_EXISTS) {
        qDebug() << "Failed to create IP address:" << res;
        CancelMibChangeNotify2(hNotif);
        _guardNotif.dismiss();
        delete ctx;
        return false;
    }

    res = WaitForSingleObject(hEvent, 10000);

    // Cancel notification BEFORE accessing ctx, so callback won't race
    CancelMibChangeNotify2(hNotif);
    _guardNotif.dismiss();

    bool found = ctx->found;
    delete ctx;

    setInterfaceMetric(luid, AF_INET, kTunInterfaceMetric, dev);

    if (res == WAIT_TIMEOUT) {
        qCritical() << "Timeout of waiting for IP assignment for " << dev << " device";
        return false;
    }

    return found;
}

bool RouterWin::updateResolvers(const QString& ifname, const QList<QHostAddress>& resolvers)
{
    return m_dnsUtil->updateResolvers(ifname, resolvers);
}

bool RouterWin::restoreResolvers() {
    return m_dnsUtil->restoreResolvers();
}

QNetworkInterface RouterWin::findLoopbackIface()
{
    for (auto iface : QNetworkInterface::allInterfaces()) {
        if (iface.flags() & QNetworkInterface::IsLoopBack) {
            return iface;
        }
    }
    return {};
}

bool RouterWin::StopRoutingIpv6()
{
    qDebug() << "RouterWin::StopRoutingIpv6";

    if (auto loopback = findLoopbackIface(); loopback.isValid()) {
        bool success = true;
        for (const QString &subnet : kIpv6Subnets) {
            if (getIpv6RoutePresence(loopback, subnet) == Ipv6RoutePresence::Present) {
                continue;
            }

            success = runNetshIpv6RouteCommand(
                          { "interface", "ipv6", "add", "route", subnet,
                            QString("interface=%1").arg(loopback.index()),
                            "metric=0", "store=active" },
                          QStringLiteral("RouterWin::StopRoutingIpv6"),
                          subnet) && success;
        }

        return success;
    }

    return false;
}

bool RouterWin::StartRoutingIpv6()
{
    qDebug() << "RouterWin::StartRoutingIpv6";

    if (auto loopback = findLoopbackIface(); loopback.isValid()) {
        bool success = true;
        for (const QString &subnet : kIpv6Subnets) {
            if (getIpv6RoutePresence(loopback, subnet) == Ipv6RoutePresence::Absent) {
                continue;
            }

            success = runNetshIpv6RouteCommand(
                          { "interface", "ipv6", "delete", "route", subnet,
                            QString("interface=%1").arg(loopback.index()) },
                          QStringLiteral("RouterWin::StartRoutingIpv6"),
                          subnet) && success;
        }

        return success;
    }

    return false;
}
