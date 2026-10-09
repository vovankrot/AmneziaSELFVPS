#include <QCoreApplication>
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QHash>
#include <QMap>
#include <QSet>
#include <QLocalServer>
#include <QLocalSocket>
#include <QTimer>
#include <QDebug>
#include <functional>
#include <iostream>
#include "client/daemon/daemonerrors.h"
struct Log {
    Log &debug(){return *this;} Log &error(){return *this;} Log &warning(){return *this;}
    template<class T> Log &operator<<(const T &){return *this;}
} logger;
void check(bool ok,const char *message){if(!ok){std::cerr<<message<<'\n';std::exit(1);}}
constexpr int MAX_CONNECTION_RETRY=10, CONNECTION_RETRY_TIMER_MSEC=500;
// Production methods with only IPC transport and signal recipients substituted.
class LocalSocketController {
public:
    enum {eUnknown,eInitializing,eReady,eDisconnected}; int m_daemonState=eReady;
    QLocalSocket *m_socket=nullptr; QTimer m_initializingTimer;
    uint32_t m_initializingRetry=0; bool m_deactivatePending=false;
    QString m_deviceIpv4; std::function<void(const QString&)> m_logCallback;
    QJsonObject m_pendingActivation;
    int stopped=0,failures=0,requests=0,reconnects=0,initializations=0,activations=0;
    void disconnected(){++stopped;} void backendFailed(){++failures;}
    void initialized(bool,bool,const QDateTime&){++initializations;}
    void connected(const QString&){ } void statusUpdated(const QString&,const QString&,uint64_t,uint64_t){ }
    void initializeInternal(){m_daemonState=eInitializing;++reconnects;}
    void write(const QJsonObject &o){if(o.value("type")=="deactivate")++requests;if(o.value("type")=="activate")++activations;}
    void checkStatus(){ }
    void errorOccurred(QLocalSocket::LocalSocketError); void disconnectInternal();
    void deactivate();void sendActivation(const QJsonObject&); void parseCommand(const QByteArray&);
};
#include "production-native-socket-errors.inc"
#include "production-native-stop.inc"
#include "production-native-activation.inc"
#include "production-native-replies.inc"
using IPAddress=int;
struct InterfaceConfig {int m_hopType=0; QList<IPAddress> m_allowedIPAddressRanges{1,2};};
struct ConnectionState {InterfaceConfig m_config;bool m_peerDeleted=false;};
struct WgUtils {
    int fail=0, interfaceDeletes=0;bool peerDeleted=false;
    QSet<int> deletedRoutes;
    bool deleteRoutePrefix(int ip){
        if(fail==ip || deletedRoutes.contains(ip))return false;
        deletedRoutes.insert(ip);return true;
    }
    bool deletePeer(const InterfaceConfig&){
        if(fail==3 || peerDeleted)return false;
        peerDeleted=true;return true;
    }
    bool deleteExclusionRoute(int){return fail!=4;}
    bool deleteInterface(){++interfaceDeletes;return fail!=5;}
};
struct DnsUtils {bool succeeds=true;bool restoreResolvers(){return succeeds;}};
class Daemon {
public:
    enum {Down}; WgUtils wg; DnsUtils dns; bool downOk=true; int stopped=0;
    QMap<int,ConnectionState> m_connections{{0,ConnectionState{}}};
    QHash<IPAddress,int> m_excludedAddrSet{{8,1}}; QTimer m_handshakeTimer; bool m_cleanupPending=false;
    WgUtils *wgutils(){return &wg;} DnsUtils *dnsutils(){return &dns;}
    bool run(int,const InterfaceConfig&){return downOk;}
    void disconnected(){++stopped;} bool deactivate(bool);
};
#include "production-native-cleanup.inc"
int main(int argc,char **argv) {
    QCoreApplication app(argc,argv);
    QLocalServer server; check(server.listen("amnezia-native-test-"+QString::number(QCoreApplication::applicationPid())),"local test server");
    QLocalSocket socket; socket.connectToServer(server.serverName());check(socket.waitForConnected(1000),"local test socket");
    LocalSocketController controller;controller.m_socket=&socket;
    {
        LocalSocketController startup;startup.m_socket=&socket;startup.m_daemonState=LocalSocketController::eInitializing;
        startup.sendActivation(QJsonObject{{"type","activate"}});
        check(!startup.activations&&!startup.m_pendingActivation.isEmpty(),"activation must await initial service status");
        startup.parseCommand(R"({"type":"status","connected":false})");
        check(startup.activations==1&&startup.m_pendingActivation.isEmpty(),"ready service receives queued activation exactly once");
        startup.m_daemonState=LocalSocketController::eInitializing;
        startup.sendActivation(QJsonObject{{"type","activate"}});startup.deactivate();
        startup.parseCommand(R"({"type":"status","connected":false})");
        check(startup.activations==1&&startup.requests==1&&startup.m_pendingActivation.isEmpty(),"stop cancels queued activation");
    }
    controller.deactivate();check(controller.requests==1&&!controller.stopped,"command is not a stop acknowledgement");
    controller.parseCommand(R"({"type":"backendFailure","errorCode":1})");
    check(controller.failures==1&&!controller.stopped,"backend failure cannot confirm cleanup");
    controller.deactivate();controller.parseCommand(R"({"type":"disconnected"})");
    check(controller.stopped==1&&!controller.m_deactivatePending,"service acknowledgement completes stop");
    controller.errorOccurred(QLocalSocket::PeerClosedError);
    check(controller.failures==2&&controller.stopped==1&&controller.m_daemonState==LocalSocketController::eDisconnected,
        "lost socket cannot report disconnection");
    controller.deactivate();check(controller.reconnects==1&&controller.m_deactivatePending,"cleanup retry reconnects");
    controller.parseCommand(R"({"type":"status","connected":false})");
    check(controller.requests==3&&!controller.initializations&&controller.m_deactivatePending,"reconnected status must send pending cleanup");
    for (const auto &reply : {R"({"type":"status"})", R"({"type":"status","connected":true})",
        R"({"type":"status","connected":true,"date":"invalid"})"}) {
        LocalSocketController invalid;invalid.m_socket=&socket;invalid.m_daemonState=LocalSocketController::eInitializing;
        invalid.m_deactivatePending=true;invalid.parseCommand(reply);
        check(invalid.failures==1&&!invalid.stopped&&!invalid.requests&&invalid.m_daemonState==LocalSocketController::eDisconnected,
            "malformed initial status cannot leave a ready controller or confirm cleanup");
    }
    for(int failure=0;failure<=7;++failure){
        Daemon daemon;daemon.wg.fail=failure;daemon.dns.succeeds=failure!=6;daemon.downOk=failure!=7;
        bool ok=daemon.deactivate(true);
        if(!failure)check(ok&&daemon.stopped==1&&daemon.m_connections.isEmpty()&&daemon.m_excludedAddrSet.isEmpty(),"complete cleanup acknowledges once");
        else {
            check(!ok&&!daemon.stopped&&!daemon.m_connections.isEmpty()&&daemon.m_cleanupPending,"failed cleanup must retain session and suppress acknowledgement");
            if(failure==4)check(daemon.m_excludedAddrSet.contains(8),"failed exclusion remains owned");
            daemon.wg.fail=0;daemon.dns.succeeds=daemon.downOk=true;
            check(daemon.deactivate(true)&&daemon.stopped==1&&daemon.m_connections.isEmpty(),"retry completes cleanup");
        }
    }
    std::cout<<"PASS: delayed stop acknowledgement, lost IPC, pending retry and seven cleanup failure points\n";
}
