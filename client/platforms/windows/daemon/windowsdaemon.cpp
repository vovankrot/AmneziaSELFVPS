/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "windowsdaemon.h"

#include <Windows.h>
#include <netioapi.h>
#include <ws2tcpip.h>
#include <qassert.h>

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLocalSocket>
#include <QNetworkInterface>
#include <QTextStream>
#include <QtGlobal>

#include "daemon/daemonerrors.h"
#include "dnsutilswindows.h"
#include "leakdetector.h"
#include "logger.h"
#include "platforms/windows/daemon/windowsfirewall.h"
#include "platforms/windows/daemon/windowssplittunnel.h"
#include "windowsfirewall.h"

#include "core/networkUtilities.h"

// Metric for exclusion routes - high enough to not interfere with normal routing
constexpr const ULONG SITE_EXCLUSION_ROUTE_METRIC = 0x5e73;

namespace {
Logger logger("WindowsDaemon");

quint64 exclusionRouteKey(quint32 address, quint8 prefixLen) {
  return (static_cast<quint64>(prefixLen) << 32) | address;
}

bool parseIpv4RouteKey(const QString& ipRange, quint64& routeKey) {
  const QStringList parts = ipRange.split('/');
  const QString ip = parts[0];
  const int prefixLen = (parts.size() > 1) ? parts[1].toInt() : 32;

  QHostAddress addr(ip);
  if (addr.protocol() != QAbstractSocket::IPv4Protocol || prefixLen < 0 || prefixLen > 32) {
    return false;
  }

  routeKey = exclusionRouteKey(addr.toIPv4Address(), static_cast<quint8>(prefixLen));
  return true;
}

// Fold overlapping/adjacent IPv4 CIDRs into the smallest equivalent set, and
// drop anything that would touch a private / reserved / multicast range.
//
// Two problems this fixes at once:
//
// 1) A raw GeoIP-RU list is ~8600 lines. Every one of those becomes a
//    CreateIpForwardEntry2() call and lives in the OS route table forever
//    while the tunnel is up. Windows handles it, but userland UI on top of
//    a route table that big feels visibly laggy -- reported by the user on
//    2026-07-26 as "система начала тормозить" after enabling GeoIP bypass on
//    a non-XRay protocol. Merging adjacent /24s into their covering /23 or
//    /22 typically cuts the count roughly in half without changing coverage.
//
// 2) The list source is configurable. A malicious or broken feed could ship
//    192.168.0.0/16, 127.0.0.0/8, etc. -- those would land in the OS route
//    table with a low metric and silently redirect LAN / loopback traffic
//    through the physical gateway. The user was explicit: "только не убей
//    локалку". So we drop those unconditionally, regardless of what the feed
//    says.
namespace geoCidr {

struct Range {
  quint32 start;
  quint32 end;
};

bool isPrivateOrReservedV4(quint32 addr) {
  // Octets, MSB first.
  const quint8 a = static_cast<quint8>((addr >> 24) & 0xFF);
  const quint8 b = static_cast<quint8>((addr >> 16) & 0xFF);

  if (a == 0) return true;                          // 0.0.0.0/8
  if (a == 10) return true;                         // RFC1918
  if (a == 127) return true;                        // loopback
  if (a == 169 && b == 254) return true;            // link-local
  if (a == 172 && b >= 16 && b <= 31) return true;  // RFC1918
  if (a == 192 && b == 168) return true;            // RFC1918
  if (a >= 224) return true;                        // multicast + reserved (240-255) + broadcast
  return false;
}

bool parseCidr(const QString& cidr, Range& out) {
  const QStringList parts = cidr.split('/');
  if (parts.size() != 2) return false;

  QHostAddress addr(parts[0]);
  if (addr.protocol() != QAbstractSocket::IPv4Protocol) return false;

  bool ok = false;
  const int prefixLen = parts[1].toInt(&ok);
  if (!ok || prefixLen < 0 || prefixLen > 32) return false;

  const quint32 base = addr.toIPv4Address();
  const quint32 mask = (prefixLen == 0) ? 0u : (~0u << (32 - prefixLen));
  out.start = base & mask;
  out.end = out.start | ~mask;
  return true;
}

// Emit the minimal set of CIDRs that exactly covers [start, end], no overshoot.
// Standard range-to-CIDR: at each step take the largest block whose alignment
// (bits set in `start`) fits and whose size doesn't overshoot `end`.
QStringList rangeToCidrs(quint32 start, quint32 end) {
  QStringList out;
  while (true) {
    // Largest prefix length permitted by alignment of `start` -- limited by
    // how many low-order zero bits it has. A start of 0 can align to /0.
    int alignMaxSize = 32;
    if (start != 0) {
      // count trailing zeros in start
      int tz = 0;
      quint32 s = start;
      while ((s & 1) == 0) { s >>= 1; ++tz; }
      alignMaxSize = tz;  // block size in bits (0..32), block covers 2^tz addresses
    }

    // Largest prefix length permitted by remaining span.
    quint64 remaining = static_cast<quint64>(end) - static_cast<quint64>(start) + 1;
    int spanMaxSize = 0;
    quint64 v = 1;
    while ((v << 1) <= remaining && spanMaxSize < 32) { v <<= 1; ++spanMaxSize; }

    const int blockBits = qMin(alignMaxSize, spanMaxSize);
    const int prefixLen = 32 - blockBits;

    out.append(QStringLiteral("%1/%2")
                   .arg(QHostAddress(start).toString())
                   .arg(prefixLen));

    const quint64 blockSize = static_cast<quint64>(1) << blockBits;
    if (static_cast<quint64>(start) + blockSize > 0xFFFFFFFFull) break;  // wrapped
    start += static_cast<quint32>(blockSize);
    if (start > end) break;
  }
  return out;
}

// Public entry point.
QStringList aggregateAndSanitize(const QStringList& in) {
  QList<Range> ranges;
  ranges.reserve(in.size());

  int filteredPrivate = 0;
  int filteredMalformed = 0;

  for (const QString& raw : in) {
    Range r{};
    if (!parseCidr(raw, r)) {
      ++filteredMalformed;
      continue;
    }
    // Drop the entire block if ANY address in it is private/reserved -- we do
    // not slice around private ranges, we just refuse to touch a feed that
    // spills into them.
    if (isPrivateOrReservedV4(r.start) || isPrivateOrReservedV4(r.end)) {
      ++filteredPrivate;
      continue;
    }
    ranges.append(r);
  }

  std::sort(ranges.begin(), ranges.end(),
            [](const Range& a, const Range& b) { return a.start < b.start; });

  // Merge overlapping / adjacent (end+1 == next.start).
  QList<Range> merged;
  merged.reserve(ranges.size());
  for (const Range& r : ranges) {
    if (!merged.isEmpty() &&
        static_cast<quint64>(merged.last().end) + 1 >= static_cast<quint64>(r.start)) {
      if (r.end > merged.last().end) merged.last().end = r.end;
    } else {
      merged.append(r);
    }
  }

  QStringList out;
  out.reserve(merged.size());
  for (const Range& r : merged) {
    out.append(rangeToCidrs(r.start, r.end));
  }

  logger.info() << "GeoIP CIDR aggregation:" << in.size() << "in ->" << out.size()
                << "out (dropped" << filteredPrivate << "private/reserved,"
                << filteredMalformed << "malformed)";
  return out;
}

}  // namespace geoCidr

QStringList sanitizeSplitTunnelApps(const QStringList& appPaths) {
  QStringList sanitizedApps;
  QSet<QString> seenEntries;

  for (const QString& rawPath : appPaths) {
    const QString cleanedPath = QDir::fromNativeSeparators(QDir::cleanPath(rawPath.trimmed()));
    if (cleanedPath.isEmpty()) {
      continue;
    }

    QFileInfo fileInfo(cleanedPath);
    if (!fileInfo.exists() || !fileInfo.isFile()) {
      logger.warning() << "Skipping missing split tunnel path" << rawPath;
      continue;
    }

    if (fileInfo.suffix().compare("exe", Qt::CaseInsensitive) != 0) {
      logger.warning() << "Skipping non-executable split tunnel path" << fileInfo.absoluteFilePath();
      continue;
    }

    const QString canonicalPath = fileInfo.canonicalFilePath();
    const QString normalizedPath = QDir::fromNativeSeparators(
        canonicalPath.isEmpty() ? fileInfo.absoluteFilePath() : canonicalPath);
    const QString dedupeKey = normalizedPath.toLower();
    if (seenEntries.contains(dedupeKey)) {
      continue;
    }

    seenEntries.insert(dedupeKey);
    sanitizedApps.append(normalizedPath);
  }

  return sanitizedApps;
}
}

WindowsDaemon::WindowsDaemon() : Daemon(nullptr) {
  MZ_COUNT_CTOR(WindowsDaemon);
  m_firewallManager = WindowsFirewall::create(this);
  if (!m_firewallManager) {
    logger.error() << "WindowsFirewall::create() returned nullptr — firewall management disabled";
  }

  m_wgutils = WireguardUtilsWindows::create(m_firewallManager, this);
  m_dnsutils = new DnsUtilsWindows(this);
  m_splitTunnelManager = WindowsSplitTunnel::create(m_firewallManager);

  connect(m_wgutils.get(), &WireguardUtilsWindows::backendFailure, this,
          &WindowsDaemon::monitorBackendFailure);
  connect(this, &WindowsDaemon::activationFailure,
          [this]() {
              if (m_firewallManager) {
                  m_firewallManager->disableKillSwitch();
              }
          });
}

WindowsDaemon::~WindowsDaemon() {
  MZ_COUNT_DTOR(WindowsDaemon);
  logger.debug() << "Daemon released";
}

void WindowsDaemon::prepareActivation(const InterfaceConfig& config, int inetAdapterIndex) {
  // Before creating the interface we need to check which adapter
  // routes to the server endpoint
  if (inetAdapterIndex == 0) {
      auto serveraddr = QHostAddress(config.m_serverIpv4AddrIn);
      m_inetAdapterIndex = NetworkUtilities::AdapterIndexTo(serveraddr);
  } else {
      m_inetAdapterIndex = inetAdapterIndex;
  }
}

void WindowsDaemon::activateSplitTunnel(const InterfaceConfig& config, int vpnAdapterIndex) {
  if (m_splitTunnelManager == nullptr)
    return;

  const QStringList sanitizedApps = sanitizeSplitTunnelApps(config.m_vpnDisabledApps);

  if (!sanitizedApps.isEmpty()) {
    if (!m_splitTunnelManager->start(m_inetAdapterIndex, vpnAdapterIndex)) {
      emit backendFailure(DaemonError::ERROR_SPLIT_TUNNEL_START_FAILURE);
      return;
    }
    if (!m_splitTunnelManager->excludeApps(sanitizedApps)) {
      emit backendFailure(DaemonError::ERROR_SPLIT_TUNNEL_EXCLUDE_FAILURE);
    }
  } else {
    if (!config.m_vpnDisabledApps.isEmpty()) {
      logger.warning() << "Skipping app split tunnel activation: no valid executable paths remain after sanitization";
    }

    if (!m_splitTunnelManager->stop() && m_splitTunnelManager->isRunning()) {
      emit backendFailure(DaemonError::ERROR_SPLIT_TUNNEL_START_FAILURE);
    }
  }
}

bool WindowsDaemon::run(Op op, const InterfaceConfig& config) {
  if (!m_splitTunnelManager) {
    if (config.m_vpnDisabledApps.length() > 0) {
      // The Client has sent us a list of disabled apps, but we failed
      // to init the the split tunnel driver.
      // So let the client know this was not possible
      emit backendFailure(DaemonError::ERROR_SPLIT_TUNNEL_INIT_FAILURE);
    }
    return true;
  }

  if (op == Down) {
    if (!m_splitTunnelManager->stop()) {
      emit backendFailure(DaemonError::ERROR_SPLIT_TUNNEL_START_FAILURE);
      return false;
    }
    return true;
  }
  const QStringList sanitizedApps = sanitizeSplitTunnelApps(config.m_vpnDisabledApps);

  if (!sanitizedApps.isEmpty()) {
    if (!m_splitTunnelManager->start(m_inetAdapterIndex)) {
      emit backendFailure(DaemonError::ERROR_SPLIT_TUNNEL_START_FAILURE);
    };
    if (!m_splitTunnelManager->excludeApps(sanitizedApps)) {
      emit backendFailure(DaemonError::ERROR_SPLIT_TUNNEL_EXCLUDE_FAILURE);
    };
    // Now the driver should be running (State == 4)
    if (!m_splitTunnelManager->isRunning()) {
      emit backendFailure(DaemonError::ERROR_SPLIT_TUNNEL_START_FAILURE);
    }
    return true;
  }

  if (!config.m_vpnDisabledApps.isEmpty()) {
    logger.warning() << "Skipping app split tunnel activation in run(): no valid executable paths remain after sanitization";
  }

  if (!m_splitTunnelManager->stop()) {
    emit backendFailure(DaemonError::ERROR_SPLIT_TUNNEL_START_FAILURE);
    return false;
  }

  return true;
}

void WindowsDaemon::monitorBackendFailure() {
  logger.warning() << "Tunnel service is down";

  emit backendFailure();
  deactivate();
}

bool WindowsDaemon::getDefaultGateway(quint32& gatewayIp, quint64& interfaceLuid) {
  PMIB_IPFORWARD_TABLE2 table = nullptr;
  DWORD result = GetIpForwardTable2(AF_INET, &table);
  if (result != NO_ERROR) {
    logger.error() << "Failed to get routing table:" << result;
    return false;
  }

  bool found = false;
  ULONG bestMetric = ULONG_MAX;
  
  for (ULONG i = 0; i < table->NumEntries; i++) {
    MIB_IPFORWARD_ROW2* row = &table->Table[i];
    
    // Skip routes on the VPN interface (WireGuard LUID)
    if (m_wgutils && row->InterfaceLuid.Value == m_wgutils->getLuid()) {
      continue;
    }
    
    // Skip routes with our exclusion metric
    if (row->Protocol == MIB_IPPROTO_NETMGMT && row->Metric == SITE_EXCLUSION_ROUTE_METRIC) {
      continue;
    }
    
    // Look for default route (0.0.0.0/0)
    if (row->DestinationPrefix.PrefixLength == 0 &&
        row->DestinationPrefix.Prefix.Ipv4.sin_family == AF_INET) {
      
      // Prefer route with lower metric
      if (row->Metric < bestMetric) {
        gatewayIp = ntohl(row->NextHop.Ipv4.sin_addr.s_addr);
        interfaceLuid = row->InterfaceLuid.Value;
        bestMetric = row->Metric;
        found = true;
      }
    }
  }
  
  FreeMibTable(table);
  
  if (found) {
    logger.debug() << "Found default gateway:" << QHostAddress(gatewayIp).toString();
  }
  return found;
}

bool WindowsDaemon::addExclusionRoute(const QString& ipRange) {
  QStringList parts = ipRange.split('/');
  QString ip = parts[0];
  int prefixLen = (parts.size() > 1) ? parts[1].toInt() : 32;
  
  QHostAddress addr(ip);
  if (addr.protocol() != QAbstractSocket::IPv4Protocol) {
    logger.warning() << "Site exclusion routes only support IPv4:" << ipRange;
    return false;
  }
  
  quint32 gatewayIp = 0;
  quint64 ifLuid = 0;
  if (!getDefaultGateway(gatewayIp, ifLuid)) {
    logger.error() << "Cannot add exclusion route: no default gateway found";
    return false;
  }
  
  MIB_IPFORWARD_ROW2 row;
  InitializeIpForwardEntry(&row);
  
  // Set destination
  row.DestinationPrefix.Prefix.Ipv4.sin_family = AF_INET;
  row.DestinationPrefix.Prefix.Ipv4.sin_addr.s_addr = htonl(addr.toIPv4Address());
  row.DestinationPrefix.PrefixLength = prefixLen;
  
  // Set next hop (gateway)
  row.NextHop.Ipv4.sin_family = AF_INET;
  row.NextHop.Ipv4.sin_addr.s_addr = htonl(gatewayIp);
  
  // Set interface
  row.InterfaceLuid.Value = ifLuid;
  
  // Set route properties
  row.ValidLifetime = 0xffffffff;
  row.PreferredLifetime = 0xffffffff;
  row.Metric = SITE_EXCLUSION_ROUTE_METRIC;
  row.Protocol = static_cast<NL_ROUTE_PROTOCOL>(MIB_IPPROTO_NETMGMT);
  row.Loopback = FALSE;
  row.AutoconfigureAddress = FALSE;
  row.Publish = FALSE;
  row.Immortal = FALSE;
  
  DWORD result = CreateIpForwardEntry2(&row);
  if (result != NO_ERROR && result != ERROR_OBJECT_ALREADY_EXISTS) {
    logger.error() << "Failed to create exclusion route for" << ipRange << "error:" << result;
    return false;
  }
  
  logger.info() << "Added site exclusion route:" << ipRange << "via" << QHostAddress(gatewayIp).toString();
  return true;
}

bool WindowsDaemon::deleteExclusionRoute(const QString& ipRange) {
  QStringList parts = ipRange.split('/');
  QString ip = parts[0];
  int prefixLen = (parts.size() > 1) ? parts[1].toInt() : 32;
  
  QHostAddress addr(ip);
  if (addr.protocol() != QAbstractSocket::IPv4Protocol) {
    return false;
  }
  
  quint32 gatewayIp = 0;
  quint64 ifLuid = 0;
  if (!getDefaultGateway(gatewayIp, ifLuid)) {
    // If we can't find gateway, try to delete route anyway by scanning the table
    PMIB_IPFORWARD_TABLE2 table = nullptr;
    if (GetIpForwardTable2(AF_INET, &table) == NO_ERROR) {
      for (ULONG i = 0; i < table->NumEntries; i++) {
        MIB_IPFORWARD_ROW2* row = &table->Table[i];
        if (row->Protocol == MIB_IPPROTO_NETMGMT &&
            row->Metric == SITE_EXCLUSION_ROUTE_METRIC &&
            row->DestinationPrefix.PrefixLength == prefixLen &&
            row->DestinationPrefix.Prefix.Ipv4.sin_addr.s_addr == htonl(addr.toIPv4Address())) {
          DeleteIpForwardEntry2(row);
          logger.info() << "Deleted site exclusion route:" << ipRange;
        }
      }
      FreeMibTable(table);
    }
    return true;
  }
  
  MIB_IPFORWARD_ROW2 row;
  InitializeIpForwardEntry(&row);
  
  row.DestinationPrefix.Prefix.Ipv4.sin_family = AF_INET;
  row.DestinationPrefix.Prefix.Ipv4.sin_addr.s_addr = htonl(addr.toIPv4Address());
  row.DestinationPrefix.PrefixLength = prefixLen;
  row.NextHop.Ipv4.sin_family = AF_INET;
  row.NextHop.Ipv4.sin_addr.s_addr = htonl(gatewayIp);
  row.InterfaceLuid.Value = ifLuid;
  row.Protocol = static_cast<NL_ROUTE_PROTOCOL>(MIB_IPPROTO_NETMGMT);
  row.Metric = SITE_EXCLUSION_ROUTE_METRIC;
  
  DWORD result = DeleteIpForwardEntry2(&row);
  if (result != NO_ERROR && result != ERROR_NOT_FOUND) {
    logger.warning() << "Failed to delete exclusion route for" << ipRange << "error:" << result;
    return false;
  }
  
  logger.info() << "Deleted site exclusion route:" << ipRange;
  return true;
}

void WindowsDaemon::activateSiteExclusionRoutes(const QStringList& excludedAddresses) {
  // First, deactivate any existing routes
  deactivateSiteExclusionRoutes();
  
  if (excludedAddresses.isEmpty()) {
    return;
  }
  
  logger.info() << "Activating site exclusion routes for" << excludedAddresses.size() << "addresses";
  
  for (const QString& ipRange : excludedAddresses) {
    if (addExclusionRoute(ipRange)) {
      m_siteExclusionRoutes.insert(ipRange);
    }
  }
}

void WindowsDaemon::deactivateSiteExclusionRoutes() {
  if (m_siteExclusionRoutes.isEmpty()) {
    return;
  }
  
  logger.info() << "Deactivating" << m_siteExclusionRoutes.size() << "site exclusion routes";
  
  for (const QString& ipRange : m_siteExclusionRoutes) {
    deleteExclusionRoute(ipRange);
  }
  m_siteExclusionRoutes.clear();
}

void WindowsDaemon::activateGeoExclusionRoutes(const QStringList& cidrs) {
  deactivateGeoExclusionRoutes();

  if (cidrs.isEmpty()) {
    return;
  }

  // Fold overlapping / adjacent networks and strip anything that would touch
  // LAN, loopback, or multicast BEFORE we start writing rows into the OS route
  // table. Also cuts route count roughly in half for the standard RU GeoIP
  // feed, which measurably reduces UI-side lag on the WG/Hysteria2 paths.
  const QStringList aggregated = geoCidr::aggregateAndSanitize(cidrs);

  if (aggregated.isEmpty()) {
    logger.warning() << "Geo exclusion routes: aggregation dropped everything, nothing to add";
    return;
  }

  quint32 gatewayIp = 0;
  quint64 ifLuid = 0;
  if (!getDefaultGateway(gatewayIp, ifLuid)) {
    logger.error() << "Cannot add geo exclusion routes: no default gateway found";
    return;
  }

  logger.info() << "Adding" << aggregated.size() << "geo exclusion routes via"
                << QHostAddress(gatewayIp).toString();

  int added = 0;
  int failed = 0;
  for (const QString& cidr : aggregated) {
    QStringList parts = cidr.split('/');
    if (parts.size() != 2) continue;
    
    QHostAddress addr(parts[0]);
    if (addr.protocol() != QAbstractSocket::IPv4Protocol) continue;
    int prefixLen = parts[1].toInt();
    
    MIB_IPFORWARD_ROW2 row;
    InitializeIpForwardEntry(&row);
    
    row.DestinationPrefix.Prefix.Ipv4.sin_family = AF_INET;
    row.DestinationPrefix.Prefix.Ipv4.sin_addr.s_addr = htonl(addr.toIPv4Address());
    row.DestinationPrefix.PrefixLength = prefixLen;
    row.NextHop.Ipv4.sin_family = AF_INET;
    row.NextHop.Ipv4.sin_addr.s_addr = htonl(gatewayIp);
    row.InterfaceLuid.Value = ifLuid;
    row.ValidLifetime = 0xffffffff;
    row.PreferredLifetime = 0xffffffff;
    row.Metric = SITE_EXCLUSION_ROUTE_METRIC;
    row.Protocol = static_cast<NL_ROUTE_PROTOCOL>(MIB_IPPROTO_NETMGMT);
    row.Loopback = FALSE;
    row.AutoconfigureAddress = FALSE;
    row.Publish = FALSE;
    row.Immortal = FALSE;
    
    DWORD result = CreateIpForwardEntry2(&row);
    if (result == NO_ERROR || result == ERROR_OBJECT_ALREADY_EXISTS) {
      m_geoExclusionRoutes.insert(cidr);
      added++;
    } else {
      failed++;
    }
  }
  
  logger.info() << "Geo exclusion routes: added" << added << ", failed" << failed;
}

void WindowsDaemon::deactivateGeoExclusionRoutes() {
  if (m_geoExclusionRoutes.isEmpty()) {
    return;
  }

  const auto geoRoutes = m_geoExclusionRoutes;
  const int count = geoRoutes.size();
  logger.info() << "Deactivating" << count << "geo exclusion routes (tracked batch scan)";

  QSet<quint64> siteRouteKeys;
  for (const QString& ipRange : m_siteExclusionRoutes) {
    quint64 routeKey = 0;
    if (parseIpv4RouteKey(ipRange, routeKey)) {
      siteRouteKeys.insert(routeKey);
    }
  }

  QSet<quint64> geoRouteKeys;
  for (const QString& cidr : geoRoutes) {
    quint64 routeKey = 0;
    if (parseIpv4RouteKey(cidr, routeKey) && !siteRouteKeys.contains(routeKey)) {
      geoRouteKeys.insert(routeKey);
    }
  }

  m_geoExclusionRoutes.clear();
  if (geoRouteKeys.isEmpty()) {
    logger.info() << "Geo exclusion routes cleanup: nothing to remove";
    return;
  }

  PMIB_IPFORWARD_TABLE2 table = nullptr;
  if (GetIpForwardTable2(AF_INET, &table) != NO_ERROR) {
    logger.warning() << "Failed to get routing table for geo route cleanup";
    return;
  }

  int deleted = 0;
  for (ULONG i = 0; i < table->NumEntries; i++) {
    MIB_IPFORWARD_ROW2* r = &table->Table[i];
    if (r->Protocol == MIB_IPPROTO_NETMGMT &&
        r->Metric == SITE_EXCLUSION_ROUTE_METRIC &&
        geoRouteKeys.contains(exclusionRouteKey(ntohl(r->DestinationPrefix.Prefix.Ipv4.sin_addr.s_addr),
                                                r->DestinationPrefix.PrefixLength))) {
      if (DeleteIpForwardEntry2(r) == NO_ERROR) {
        deleted++;
      }
    }
  }
  FreeMibTable(table);

  logger.info() << "Geo exclusion routes cleanup: removed" << deleted << "of" << geoRouteKeys.size();
}
