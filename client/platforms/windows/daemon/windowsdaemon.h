/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef WINDOWSDAEMON_H
#define WINDOWSDAEMON_H

#include <qpointer.h>
#include <QSet>

#include "daemon/daemon.h"
#include "dnsutilswindows.h"
#include "windowsfirewall.h"
#include "windowssplittunnel.h"
#include "windowstunnelservice.h"
#include "wireguardutilswindows.h"

#define TUNNEL_SERVICE_NAME L"AmneziaWGTunnel$AmneziaVPN"

class WindowsDaemon final : public Daemon {
  Q_DISABLE_COPY_MOVE(WindowsDaemon)

 public:
  WindowsDaemon();
  ~WindowsDaemon();

  void prepareActivation(const InterfaceConfig& config, int inetAdapterIndex = 0) override;
  void activateSplitTunnel(const InterfaceConfig& config, int vpnAdapterIndex = 0) override;
  
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

 private:
  enum State {
    Active,
    Inactive,
  };

  int m_inetAdapterIndex = -1;
  QSet<QString> m_siteExclusionRoutes;  // Track created exclusion routes
  QSet<QString> m_geoExclusionRoutes;   // Track geo (RU) exclusion routes

  std::unique_ptr<WireguardUtilsWindows> m_wgutils;
  DnsUtilsWindows* m_dnsutils = nullptr;
  std::unique_ptr<WindowsSplitTunnel> m_splitTunnelManager;
  QPointer<WindowsFirewall> m_firewallManager;
};

#endif  // WINDOWSDAEMON_H
