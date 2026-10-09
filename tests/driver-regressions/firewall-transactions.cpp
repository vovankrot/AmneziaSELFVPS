#include <QHostAddress>
#include <QList>
#include <QMultiMap>
#include <QScopeGuard>
#include <QStringList>
#include <functional>
#include <algorithm>
#include <QJsonObject>
#include "ipaddress.h"
#include <iostream>

constexpr int FWP_E_FILTER_NOT_FOUND = 9;
constexpr int ERROR_SUCCESS = 0, LOW_WEIGHT = 0, MED_WEIGHT = 7, HIGH_WEIGHT = 13, MAX_WEIGHT = 15;
struct InterfaceConfig {
    QString m_serverPublicKey, m_primaryDnsServer, m_secondaryDnsServer;
    QString m_serverIpv4Gateway, m_serverIpv6Gateway;
    QList<IPAddress> m_allowedIPAddressRanges;
    QStringList m_allowedDnsServers, m_excludedAddresses;
};
struct Log {
    Log &error() { return *this; }
    Log &info() { return *this; }
    Log &debug() { return *this; }
    template<class T> Log &operator<<(const T &) { return *this; }
} logger;
struct WfpState {
    QList<quint64> installed {41, 42, 43}, pending;
    bool inTransaction = false, rejectBegin = false, rejectCommit = false;
    int begins = 0, commits = 0, aborts = 0, adds = 0, failAt = -1, disables = 0, deletes = 0, failDeleteAt = -1;
    QStringList addedPrefixes;
} state;
int FwpmTransactionBegin(void *, void *) {
    ++state.begins;
    if (state.rejectBegin || state.inTransaction) return 5;
    state.inTransaction = true; state.pending = state.installed; return 0;
}
int FwpmTransactionBegin0(void *handle, int) { return FwpmTransactionBegin(handle, nullptr); }
int FwpmTransactionAbort0(void *) {
    ++state.aborts; state.pending.clear(); state.inTransaction = false; return 0;
}
int FwpmTransactionCommit0(void *) {
    ++state.commits;
    if (state.rejectCommit || !state.inTransaction) return 5;
    state.installed = state.pending; state.pending.clear(); state.inTransaction = false; return 0;
}
int FwpmFilterDeleteById0(void *, quint64 id) {
    if (++state.deletes == state.failDeleteAt) return 5;
    if (!state.pending.removeOne(id)) return FWP_E_FILTER_NOT_FOUND;
    return 0;
}
class WindowsFirewall {
public:
    void *m_sessionHandle = nullptr;
    // Include a prior LAN permit and a different peer's policy.
    QList<quint64> m_activeRules {41, 42};
    QMultiMap<QString, quint64> m_peerRules {{"existing-peer", 43}};
    static inline WindowsFirewall *fixture = nullptr;
    static WindowsFirewall *create(void *) { return fixture; }
    bool enableInterface(int, bool replaceExistingRules = false, const QStringList & = {});
    bool allowTrafficRange(const QStringList &, bool replaceExistingRanges = false);
    bool enablePeerTraffic(const InterfaceConfig &);
    bool disablePeerTraffic(const QString &);
    bool allowAllTraffic();
    bool enableIpv6AppBypass(const QStringList &);
    void disableKillSwitch() { ++state.disables; state.installed.clear(); }
    QString getCurrentPath() { return "fixture.exe"; }
    bool add(const QString &peer, int count = 2) {
        // Rule helpers may succeed on the outbound layer then fail inbound.
        for (int i = 0; i < count; ++i) {
            if (++state.adds == state.failAt) return false;
            const quint64 id = 1000 + state.adds;
            (state.inTransaction ? state.pending : state.installed).append(id);
            if (peer.isEmpty()) m_activeRules.append(id); else m_peerRules.insert(peer, id);
        }
        return true;
    }
    bool allowTrafficTo(const IPAddress &ip, int, const QString &, const QString &peer = {}) {
        state.addedPrefixes.append(ip.toString()); return add(peer);
    }
    bool allowTrafficTo(const QHostAddress &, uint, int, const QString &, const QString &peer = {}) { return add(peer, 4); }
    bool blockTrafficTo(const IPAddress &, int, const QString &, const QString &peer = {}) { return add(peer); }
    bool blockTrafficTo(const QList<IPAddress> &ips, int weight, const QString &title, const QString &peer = {}) {
        for (const auto &ip : ips) if (!blockTrafficTo(ip, weight, title, peer)) return false;
        return true;
    }
    bool allowTrafficOfAdapter(int, int, const QString &) { return add({}); }
    bool allowDHCPTraffic(int, const QString &) { return add({}, 4); }
    bool allowHyperVTraffic(int, const QString &) { return add({}); }
    bool allowTrafficForAppOnAll(const QString &, int, const QString &) { return add({}, 4); }
    bool allowTrafficForAppOnAll(const QString &, int, const QString &, bool, const QString &peer) { return add(peer, 2); }
    bool blockTrafficOnPort(uint, int, const QString &) { return add({}, 4); }
    bool allowLoopbackTraffic(int, const QString &) { return add({}); }
};
#include "production-firewall-transactions.inc"

bool exercise(const char *name, const std::function<bool(WindowsFirewall &)> &operation, bool peer = false, bool replacement = false) {
    const QList<quint64> original {41, 42, 43};
    WindowsFirewall successful; state = {};
    if (!operation(successful) || state.begins != 1 || state.commits != 1 || state.aborts || state.disables || state.inTransaction) return false;
    const int totalAdds = state.adds;
    if (totalAdds == 0 || state.installed.size() != (replacement ? 0 : original.size()) + totalAdds) return false;
    if (peer) {
        for (const auto &prefix : {"192.168.0.0/16", "10.0.0.0/8", "172.16.0.0/12", "169.254.0.0/16"})
            if (!state.addedPrefixes.contains(prefix)) return false;
    }
    // Every individual insertion, including intermediate layers, can fail.
    for (int failure = -2; failure <= totalAdds; ++failure) {
        if (failure == 0) continue;
        WindowsFirewall firewall; const auto active = firewall.m_activeRules; const auto peers = firewall.m_peerRules;
        state = {}; state.rejectBegin = failure == -2; state.rejectCommit = failure == -1; state.failAt = failure;
        if (operation(firewall) || state.installed != original || firewall.m_activeRules != active
            || firewall.m_peerRules != peers || state.disables || state.inTransaction) return false;
        if (state.aborts != (failure == -2 ? 0 : 1)) return false;
        if (state.commits != (failure == -1 ? 1 : 0)) return false;
        // A retry must start a fresh transaction and commit without stale IDs.
        state.rejectBegin = state.rejectCommit = false; state.failAt = -1;
        if (!operation(firewall) || state.inTransaction || state.disables) return false;
    }
    std::cout << "PASS: " << name << " preserves prior LAN/peer filters at begin, all " << totalAdds
              << " insertions and commit; retry succeeds\n";
    return true;
}
const QString ORGANIZATION_NAME="fixture", APPLICATION_NAME="fixture";
struct FakeSettings {
    enum { NativeFormat, NoError };
    static inline bool failWrite=false, mode=false;
    FakeSettings(const QString &, int) {}
    void setValue(const char *, bool enabled) { if (!failWrite) mode=enabled; }
    void sync() {}
    int status() const { return failWrite ? 5 : NoError; }
};
#define QSettings FakeSettings
class KillSwitch {
public:
    QStringList m_allowedRanges {"192.168.0.0/16"};
    bool strict = false;
    bool isStrictKillSwitchEnabled() const { return strict; }
    bool disableAllTraffic();
    bool resetAllowedRange(const QStringList &);
    bool addAllowedRange(const QStringList &);
    bool enableKillSwitch(const QJsonObject &, int);
    bool refresh(bool, bool);
    bool disableKillSwitch() { return WindowsFirewall::fixture->allowAllTraffic(); }
};
bool isValidIpOrCidr(const QString &value) { return IPAddress(value).isValid(); }
#include "production-firewall-callers.inc"
bool verifyCallers() {
    WindowsFirewall fw; WindowsFirewall::fixture = &fw;
    KillSwitch ks; const auto original = ks.m_allowedRanges;
    state = {}; state.rejectCommit = true;
    if (ks.addAllowedRange({"192.0.2.1"}) || ks.m_allowedRanges != original) return false;
    state = {}; state.rejectBegin = true;
    if (ks.disableAllTraffic() || ks.m_allowedRanges != original) return false;
    if (ks.resetAllowedRange({"192.0.2.1"}) || ks.m_allowedRanges != original) return false;
    state = {}; state.failAt = 2;
    if (ks.enableKillSwitch(QJsonObject{{"splitTunnelType", 2}}, 17)
        || state.installed != QList<quint64>{41,42,43} || state.disables) return false;
    state = {};
    if (!ks.refresh(true,true) || !FakeSettings::mode || state.begins || state.installed != QList<quint64>{41,42,43}) return false;
    if (!ks.refresh(false,true) || FakeSettings::mode || state.begins) return false;
    FakeSettings::failWrite=true;
    if (ks.refresh(true,true) || state.begins) return false;
    FakeSettings::failWrite=false; state.rejectBegin=true;
    if (ks.refresh(true,false) || FakeSettings::mode) return false;
    state = {};
    if (!ks.addAllowedRange({"192.0.2.1"}) || !ks.m_allowedRanges.contains("192.0.2.1")) return false;
    std::cout << "PASS: callers report failure and retain range bookkeeping; CIDR prefixes preserved\n";
    return true;
}
int main() {
    InterfaceConfig config;
    config.m_serverPublicKey = "new-peer";
    config.m_allowedIPAddressRanges = {IPAddress("0.0.0.0/0"), IPAddress("::/0")};
    config.m_primaryDnsServer = config.m_serverIpv4Gateway = "192.0.2.1";
    config.m_secondaryDnsServer = "198.51.100.1";
    config.m_serverIpv6Gateway = "2001:db8::1";
    config.m_allowedDnsServers = {"203.0.113.1"}; config.m_excludedAddresses = {"192.0.2.2", "198.51.100.2"};
    if (!exercise("allowTrafficRange", [](auto &fw) { return fw.allowTrafficRange({"192.0.2.1", "2001:db8::1"}); })) return 1;
    if (!exercise("enablePeerTraffic", [&](auto &fw) { return fw.enablePeerTraffic(config); }, true)) return 2;
    {
        WindowsFirewall target; state={};
        if (!target.enablePeerTraffic(config)) return 22;
        auto installed=state.installed; auto peers=target.m_peerRules;
        state.failDeleteAt=1;
        if (target.enablePeerTraffic(config) || state.installed!=installed || target.m_peerRules!=peers) return 23;
        state.failDeleteAt=-1;
        if (!target.enablePeerTraffic(config) || state.installed.size()!=installed.size()
            || !target.m_peerRules.contains("existing-peer")) return 24;
    }
    if (!exercise("enableInterface(adapter)", [](auto &fw) { return fw.enableInterface(17); })) return 3;
    if (!exercise("enableInterface(no adapter)", [](auto &fw) { return fw.enableInterface(-1); })) return 4;
    if (!exercise("replace interface", [](auto &fw) { return fw.enableInterface(17, true); }, false, true)) return 5;
    if (!exercise("replace no-adapter policy", [](auto &fw) { return fw.enableInterface(-1, true); }, false, true)) return 6;
    if (!exercise("atomic strict baseline and ranges", [](auto &fw) { return fw.enableInterface(-1, true, {"192.0.2.1"}); }, false, true)) return 14;
    {
        WindowsFirewall target; WindowsFirewall::fixture=&target; KillSwitch ks; ks.strict=true; state={};
        if (!ks.resetAllowedRange({"192.0.2.1"})) return 15;
        const int count=state.installed.size();
        const auto oldRules=target.m_peerRules.values("killswitch-range");
        if (!ks.resetAllowedRange({"198.51.100.1"}) || state.installed.size()!=count) return 16;
        for (auto id:oldRules) if (state.installed.contains(id)) return 17;
        if (!ks.disableAllTraffic() || target.m_peerRules.contains("killswitch-range")
            || target.m_peerRules.contains("existing-peer")) return 18;
    }
    for (int failure=-2; failure<=2; ++failure) {
        WindowsFirewall target; state={};
        if (!target.allowTrafficRange({"192.0.2.1"},true)) return 19;
        auto original=state.installed; auto active=target.m_activeRules; auto peers=target.m_peerRules;
        state.rejectBegin=failure==-2; state.rejectCommit=failure==-1; state.failAt=state.adds+failure;
        if (!failure) {
            if (!target.allowTrafficRange({"198.51.100.1"},true) || state.installed.size()!=original.size()
                || !target.m_peerRules.contains("existing-peer")) return 20;
        } else if (target.allowTrafficRange({"198.51.100.1"},true) || state.installed!=original
            || target.m_activeRules!=active || target.m_peerRules!=peers) return 21;
    }
    for (int failure = 1; failure <= 3; ++failure) {
        WindowsFirewall fw; const auto active = fw.m_activeRules; const auto peers = fw.m_peerRules;
        state = {}; state.failDeleteAt = failure;
        if (fw.enableInterface(17, true) || state.installed != QList<quint64>{41, 42, 43}
            || fw.m_activeRules != active || fw.m_peerRules != peers || state.aborts != 1 || state.inTransaction) return 7;
    }
    // CIDRs must retain their prefix instead of becoming an invalid QHostAddress.
    WindowsFirewall fw; state = {};
    if (!fw.allowTrafficRange({"192.168.0.0/16", "2001:db8::/32"})
        || state.addedPrefixes != QStringList{"192.168.0.0/16", "2001:db8::/32"}) return 8;
    if (!verifyCallers()) return 9;
    for (bool all : {false, true}) {
        for (int failure = -2; failure <= (all ? 3 : 1); ++failure) {
            WindowsFirewall target; const auto active = target.m_activeRules, installed = state.installed;
            const auto peers = target.m_peerRules;
            state = {}; state.rejectBegin = failure == -2; state.rejectCommit = failure == -1;
            state.failDeleteAt = failure;
            const bool ok = all ? target.allowAllTraffic() : target.disablePeerTraffic("existing-peer");
            if (failure == 0) {
                if (!ok || state.inTransaction || !target.m_peerRules.isEmpty()
                    || (all && !target.m_activeRules.isEmpty()) || (!all && target.m_activeRules != active)) return 10;
            } else if (ok || target.m_activeRules != active || target.m_peerRules != peers
                       || state.installed != QList<quint64>{41,42,43} || state.inTransaction) return 11;
        }
    }
    for (int failure = -3; failure <= 6; ++failure) {
        WindowsFirewall target;
        target.m_peerRules.insert("selfvps-app-bypass-ipv6", 44);
        const auto peers = target.m_peerRules;
        state = {}; state.installed.append(44);
        state.rejectBegin = failure == -3; state.rejectCommit = failure == -2;
        state.failDeleteAt = failure == -1 ? 1 : -1; state.failAt = failure;
        const bool ok = target.enableIpv6AppBypass({"excluded.exe"});
        if (failure == 0) { if (!ok || state.installed.contains(44) || !state.installed.contains(43)) return 12; }
        else if (ok || target.m_peerRules != peers || state.installed != QList<quint64>{41,42,43,44}
                 || state.inTransaction) return 13;
    }
    std::cout << "PASS: peer/all-policy deletion and IPv6 replacement retain prior filters on every failure\n";
    return 0;
}
