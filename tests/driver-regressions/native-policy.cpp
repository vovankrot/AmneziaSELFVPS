#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <QHostAddress>
#include <QJsonObject>
#include <QJsonArray>
#include "client/protocols/protocols_defs.h"
#include <QJsonValue>
#include <QTextStream>
#include <iostream>
#include "client/daemon/interfaceconfig.h"
struct Log {
    Log &debug(){return *this;}Log &error(){return *this;}
    template<class T> Log &operator<<(const T&){return *this;}
} logger;
void check(bool ok,const char *message){if(!ok){std::cerr<<message<<'\n';std::exit(1);}}
struct Monitor {bool ok=true;int calls=0;bool deleteExclusionRoute(const IPAddress&){++calls;return ok;}};
struct Firewall {bool ok=true;int calls=0;bool disablePeerTraffic(const QString&){++calls;return ok;}};
struct Tunnel {bool stopped=false;int calls=0;QString reply="errno=0";
    bool isStopped(){return stopped;}QString uapiCommand(const QString&){++calls;return reply;}};
class WireguardUtilsWindows {
public:
    Monitor *m_routeMonitor=nullptr;Firewall *m_firewall=nullptr;Tunnel m_tunnel;
    bool deletePeer(const InterfaceConfig&);
};
#include "production-native-peer-cleanup.inc"
#include "production-interface-conf.inc"
void parseAwgFields(const QJsonObject &obj, InterfaceConfig &config) {
#include "production-awg-fields.inc"
}
struct LocalSocketController {
    QString m_deviceIpv4;
    QJsonObject activation;
    void activate(const QJsonObject &rawConfig);
    void sendActivation(const QJsonObject &json) { activation=json; }
};
#include "production-native-activate-fields.inc"
DWORD lookupError=0, dnsV4Error=0, dnsV6Error=0;int dnsCalls=0,lookups=0;
DWORD fakeGetIfEntry2(MIB_IF_ROW2 *row){++lookups;row->InterfaceIndex=7;return lookupError;}
#define GetIfEntry2 fakeGetIfEntry2
DWORD fakeSetDns(GUID,const void *data){
    ++dnsCalls;const auto *settings=static_cast<const DNS_INTERFACE_SETTINGS*>(data);
    check(settings->Version==DNS_INTERFACE_SETTINGS_VERSION1,"DNS settings version");
    check((settings->Flags&DNS_SETTING_NAMESERVER)!=0,"DNS nameserver flag");
    return settings->Flags&DNS_SETTING_IPV6?dnsV6Error:dnsV4Error;
}
constexpr uint32_t WINDOWS_NETSH_TIMEOUT_MSEC=2000;
struct FakeProcess {
    enum {NormalExit,CrashExit};
    static inline int failedCommand=0,timeoutCommand=0,crashCommand=0,kills=0;
    static inline QList<QStringList> commands;
    int number=0;
    void start(const QString&,const QStringList &args){commands.append(args);number=commands.size();}
    bool waitForStarted(int){return true;}
    bool waitForFinished(int){return number!=timeoutCommand;}
    void kill(){++kills;}
    int exitStatus(){return number==crashCommand?CrashExit:NormalExit;}
    int exitCode(){return number==failedCommand?1:0;}
};
#define QProcess FakeProcess
class DnsUtilsWindows {
public:
    quint64 m_luid=77;
    DWORD (*m_setInterfaceDnsSettingsProcAddr)(GUID,const void*)=fakeSetDns;
    bool updateResolversWin32(GUID,const QList<QHostAddress>&);
    bool updateResolversNetsh(int,const QList<QHostAddress>&);
    bool restoreResolvers();
};
#include "production-native-dns-settings.inc"
#include "production-native-netsh.inc"
#include "production-native-dns-restore.inc"
int main(){
    InterfaceConfig config;check(config.m_hopType==InterfaceConfig::SingleHop&&!config.m_killSwitchEnabled,"deterministic config defaults");
    config.m_deviceIpv4Address="10.0.0.2";config.m_serverIpv4AddrIn="192.0.2.1";
    check(!config.toWgConf().isEmpty()&&!config.toWgConf().contains("RandomTrailers")&&!config.toWgConf().contains("DisableCookies"),"legacy configs omit AWG31 options");
    parseAwgFields(QJsonObject{{"RandomTrailers",true},{"DisableCookies",false}}, config);
    check(config.m_randomTrailers=="on"&&config.m_disableCookies=="off","JSON booleans survive daemon AWG parsing");
    const auto awg31=config.toWgConf();
    check(awg31.contains("RandomTrailers = on\n")&&awg31.contains("DisableCookies = off\n"),"AWG31 flags survive native config generation");
    for (const auto &protocol : {QString("awg"), QString("wireguard")}) {
        for (const auto &toggle : {QJsonValue(true), QJsonValue(false), QJsonValue("on"), QJsonValue("off")}) {
            const QJsonObject wg{{"client_ip","10.0.0.2"},{"hostName","192.0.2.1"},
                {"HeaderProtectionKey","fixture-header-key"},{"RandomTrailers",toggle},{"DisableCookies",toggle}};
            LocalSocketController controller;
            controller.activate(QJsonObject{{"protocol",protocol},{protocol+"_config_data",wg}});
            check(controller.activation.value("RandomTrailers")==toggle&&controller.activation.value("DisableCookies")==toggle,
                  "AWG31 flags survive the actual client activation IPC serialization");
            InterfaceConfig emitted;emitted.m_deviceIpv4Address="10.0.0.2";emitted.m_serverIpv4AddrIn="192.0.2.1";
            parseAwgFields(controller.activation,emitted);
            const QString expected=toggle.isBool()?(toggle.toBool()?"on":"off"):toggle.toString();
            check(emitted.toWgConf().contains("RandomTrailers = "+expected+"\n")
                  &&emitted.toWgConf().contains("DisableCookies = "+expected+"\n"),
                  "AWG31 flags survive activation IPC, daemon parsing and tunnel config");
        }
    }
    LocalSocketController legacy;legacy.activate(QJsonObject{{"protocol","wireguard"},{"wireguard_config_data",QJsonObject()}});
    check(!legacy.activation.contains("RandomTrailers")&&!legacy.activation.contains("DisableCookies"),"plain WG does not gain AWG31 parameters");
    config.m_serverPublicKey="dGVzdA==";config.m_serverIpv4AddrIn="192.0.2.1";
    Monitor monitor;Firewall firewall;WireguardUtilsWindows vpn;vpn.m_routeMonitor=&monitor;vpn.m_firewall=&firewall;
    vpn.m_tunnel.stopped=true;
    check(vpn.deletePeer(config)&&!vpn.m_tunnel.calls&&monitor.calls==1&&firewall.calls==1,"stopped tunnel still cleans routes and WFP without UAPI");
    monitor.ok=false;check(!vpn.deletePeer(config),"failed endpoint cleanup cannot be hidden by stopped tunnel");
    monitor.ok=true;firewall.ok=false;check(!vpn.deletePeer(config),"failed WFP cleanup cannot be hidden by stopped tunnel");
    firewall.ok=true;vpn.m_tunnel.stopped=false;
    for(const auto &reply:{QString(),QString("errno=5"),QString("errno=0\nerrno=5"),QString("unexpected\nerrno=0")}){
        vpn.m_tunnel.reply=reply;check(!vpn.deletePeer(config),"empty, error or contradictory UAPI reply cannot confirm cleanup");
    }
    vpn.m_tunnel.reply="errno=0";check(vpn.deletePeer(config),"confirmed UAPI deletion");
    DnsUtilsWindows dns;
    lookupError=ERROR_ACCESS_DENIED;check(!dns.restoreResolvers()&&dns.m_luid==77&&!dnsCalls,"lookup failure preserves DNS owner");
    lookupError=0;dnsV6Error=ERROR_ACCESS_DENIED;
    check(!dns.restoreResolvers()&&dns.m_luid==77&&dnsCalls==2,"partial DNS restore must retain owner");
    dnsV6Error=0;check(dns.restoreResolvers()&&!dns.m_luid&&dnsCalls==4,"DNS retry clears owner only after both families succeed");
    const int previousLookups=lookups;check(dns.restoreResolvers()&&lookups==previousLookups&&dnsCalls==4,"successful DNS restoration must not repeat");
    dns.m_luid=88;lookupError=ERROR_FILE_NOT_FOUND;check(dns.restoreResolvers()&&!dns.m_luid,"removed interface releases DNS owner");
    lookupError=0;dns.m_luid=99;dns.m_setInterfaceDnsSettingsProcAddr=nullptr;FakeProcess::failedCommand=1;
    check(!dns.restoreResolvers()&&dns.m_luid==99,"netsh failure preserves owner");
    FakeProcess::failedCommand=0;check(dns.restoreResolvers()&&!dns.m_luid,"netsh retry releases owner");
    for (int failed=1;failed<=4;++failed) {
        FakeProcess::commands.clear();FakeProcess::failedCommand=failed;
        check(!dns.updateResolversNetsh(7,{QHostAddress("192.0.2.53"),QHostAddress("2001:db8::53")})
            &&FakeProcess::commands.size()==failed,"each netsh command failure must stop the sequence");
    }
    FakeProcess::failedCommand=0;FakeProcess::commands.clear();FakeProcess::timeoutCommand=1;
    check(!dns.updateResolversNetsh(7,{})&&FakeProcess::kills==1,"netsh timeout must stop child and fail");
    FakeProcess::timeoutCommand=0;FakeProcess::commands.clear();FakeProcess::crashCommand=1;
    check(!dns.updateResolversNetsh(7,{}),"crashed netsh cannot succeed");
    FakeProcess::crashCommand=0;FakeProcess::commands.clear();
    check(!dns.updateResolversNetsh(7,{QHostAddress()})&&FakeProcess::commands.isEmpty(),"invalid DNS must not flush existing settings");
    check(dns.updateResolversNetsh(7,{QHostAddress("192.0.2.53"),QHostAddress("2001:db8::53")})
        &&FakeProcess::commands.size()==4&&FakeProcess::commands[0].contains("validate=no")
        &&FakeProcess::commands[0].contains("source=static")&&FakeProcess::commands[1].contains("source=static")
        &&FakeProcess::commands[3][1]=="ipv6","successful netsh arguments and address families");
    std::cout<<"PASS: crashed peer cleanup, strict UAPI acknowledgements and DNS ownership on failure/retry\n";
}
