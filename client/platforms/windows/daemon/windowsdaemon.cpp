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
  
  quint32 gatewayIp = 0;
  quint64 ifLuid = 0;
  if (!getDefaultGateway(gatewayIp, ifLuid)) {
    logger.error() << "Cannot add geo exclusion routes: no default gateway found";
    return;
  }
  
  logger.info() << "Adding" << cidrs.size() << "geo exclusion routes via"
                << QHostAddress(gatewayIp).toString();
  
  int added = 0;
  int failed = 0;
  for (const QString& cidr : cidrs) {
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
