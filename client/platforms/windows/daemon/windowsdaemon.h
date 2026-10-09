/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef WINDOWSDAEMON_H
#define WINDOWSDAEMON_H

#include <qpointer.h>
#include <QSet>
#include <QHash>

#include "daemon/daemon.h"
#include "dnsutilswindows.h"
#include "windowsfirewall.h"
#include "windowssplittunnel.h"
#include "windowstunnelservice.h"
#include "wireguardutilswindows.h"
#include <netioapi.h>

#define TUNNEL_SERVICE_NAME L"AmneziaWGTunnel$AmneziaVPN"

class WindowsDaemon final : public Daemon {
  Q_DISABLE_COPY_MOVE(WindowsDaemon)

 public:
  WindowsDaemon();
  ~WindowsDaemon();

  void prepareActivation(const InterfaceConfig& config, int inetAdapterIndex = 0) override;
  bool activateSplitTunnel(const InterfaceConfig& config, int vpnAdapterIndex = 0) override;
  bool appBypassActive() const { return m_appBypassActive; }
  const QStringList& activeAppBypassPaths() const { return m_activeAppBypassPaths; }
  
  // Site-based split tunneling: add exclusion routes for specified addresses
  void activateSiteExclusionRoutes(const QStringList& excludedAddresses);
  void deactivateSiteExclusionRoutes();
  
  // Geo-based exclusion routes: bypass VPN for Russian IP ranges
  void activateGeoExclusionRoutes(const QStringList& cidrs);
  void deactivateGeoExclusionRoutes();

 protected:
  bool run(Op op, const InterfaceConfig& config) override;
  WireguardUtils* wgutils() const override { return m_wgutils.get(); }
  DnsUtils* dnsutils() override { return m_dnsutils; }

 private:
  void monitorBackendFailure();
  bool addExclusionRoute(const QString& ipRange);
  bool deleteExclusionRoute(const QString& ipRange);
  bool getDefaultGateway(quint32& gatewayIp, quint64& interfaceLuid);
  bool fallBackFromUnresponsiveSplitTunnel(const char* operation);
  void tryRestoreSplitTunnelManager();

 private:
  enum State {
    Active,
    Inactive,
  };

  bool m_splitTunnelQuarantined = false;
  bool m_appBypassActive = false;
  QStringList m_activeAppBypassPaths;
  int m_inetAdapterIndex = -1;
  QHostAddress m_serverEndpoint;
  QSet<QString> m_siteExclusionRoutes;  // Track created exclusion routes
  QSet<QString> m_geoExclusionRoutes;   // Track geo (RU) exclusion routes
  QHash<quint64, MIB_IPFORWARD_ROW2> m_ownedExclusionRoutes;

  std::unique_ptr<WireguardUtilsWindows> m_wgutils;
  DnsUtilsWindows* m_dnsutils = nullptr;
  std::unique_ptr<WindowsSplitTunnel> m_splitTunnelManager;
  QPointer<WindowsFirewall> m_firewallManager;
};

#endif  // WINDOWSDAEMON_H
