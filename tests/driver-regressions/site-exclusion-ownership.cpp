#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <QHostAddress>
#include <QHash>
#include <QSet>
#include <QStringList>
#include <QByteArray>
#include <algorithm>
#include <iostream>
#include <cstdlib>
#include <vector>
struct Log {
 Log& info(){return *this;} Log& warning(){return *this;} Log& error(){return *this;}
 template<class T> Log& operator<<(const T&){return *this;}
} logger;
constexpr ULONG SITE_EXCLUSION_ROUTE_METRIC=0x5e73;
#include "production-site-route-key.inc"
namespace geoCidr { QStringList aggregateAndSanitize(const QStringList& v){return v;} }
std::vector<MIB_IPFORWARD_ROW2> routes;
quint32 gateway=0xc0000201; quint64 luid=2;
DWORD deletionError=NO_ERROR; int gatewayReads=0, tableReads=0;
bool same(const MIB_IPFORWARD_ROW2& a,const MIB_IPFORWARD_ROW2& b){
 return a.DestinationPrefix.Prefix.Ipv4.sin_addr.s_addr==b.DestinationPrefix.Prefix.Ipv4.sin_addr.s_addr
 && a.DestinationPrefix.PrefixLength==b.DestinationPrefix.PrefixLength
 && a.NextHop.Ipv4.sin_addr.s_addr==b.NextHop.Ipv4.sin_addr.s_addr && a.InterfaceLuid.Value==b.InterfaceLuid.Value;
}
void init(MIB_IPFORWARD_ROW2* p){*p={};}
DWORD create(const MIB_IPFORWARD_ROW2* row){
 if(std::any_of(routes.begin(),routes.end(),[&](const auto& r){return same(r,*row);}))return ERROR_OBJECT_ALREADY_EXISTS;
 routes.push_back(*row);return NO_ERROR;
}
DWORD removeRow(const MIB_IPFORWARD_ROW2* row){
 if(deletionError)return deletionError;
 auto it=std::find_if(routes.begin(),routes.end(),[&](const auto& r){return same(r,*row);});
 if(it==routes.end())return ERROR_NOT_FOUND;
 routes.erase(it);return NO_ERROR;
}
QByteArray tableStorage;
DWORD readTable(ADDRESS_FAMILY,PMIB_IPFORWARD_TABLE2* p){
 ++tableReads;tableStorage.resize(sizeof(MIB_IPFORWARD_TABLE2)+routes.size()*sizeof(MIB_IPFORWARD_ROW2));
 *p=reinterpret_cast<PMIB_IPFORWARD_TABLE2>(tableStorage.data());(*p)->NumEntries=routes.size();
 std::copy(routes.begin(),routes.end(),(*p)->Table);return NO_ERROR;
}
#define InitializeIpForwardEntry init
#define CreateIpForwardEntry2 create
#define DeleteIpForwardEntry2 removeRow
#define GetIpForwardTable2 readTable
#define FreeMibTable(x) ((void)0)
struct WindowsDaemon {
 QSet<QString> m_siteExclusionRoutes,m_geoExclusionRoutes;
 QHash<quint64,MIB_IPFORWARD_ROW2> m_ownedExclusionRoutes;
 bool getDefaultGateway(quint32& g,quint64& l){++gatewayReads;g=gateway;l=luid;return g!=0;}
 bool addExclusionRoute(const QString&);
 bool deleteExclusionRoute(const QString&);
 void activateSiteExclusionRoutes(const QStringList&);
 void deactivateSiteExclusionRoutes();
 void activateGeoExclusionRoutes(const QStringList&);
 void deactivateGeoExclusionRoutes();
};
#include "production-site-routes.inc"
void check(bool ok,const char* message){if(!ok){std::cerr<<message<<'\n';std::exit(1);}}
void reset(){routes.clear();gateway=0xc0000201;luid=2;deletionError=0;tableReads=0;gatewayReads=0;}
int main(){
 const QString ip="8.8.8.8/32";
 reset();{WindowsDaemon d;d.activateSiteExclusionRoutes({ip});check(routes.size()==1,"setup route");
 gateway=0xc0000202;luid=5;d.deactivateSiteExclusionRoutes();check(routes.empty(),"old gateway route leaked after gateway change");}
 reset();{WindowsDaemon d;d.activateSiteExclusionRoutes({ip});deletionError=ERROR_ACCESS_DENIED;d.deactivateSiteExclusionRoutes();
 check(d.m_siteExclusionRoutes.contains(ip),"failed cleanup lost ownership");
 d.activateSiteExclusionRoutes({"9.9.9.9/32"});check(routes.size()==1,"replacement proceeded with failed old cleanup");
 deletionError=0;d.deactivateSiteExclusionRoutes();check(routes.empty(),"cleanup retry failed");}
 reset();{WindowsDaemon original;original.activateSiteExclusionRoutes({ip});WindowsDaemon borrower;borrower.activateSiteExclusionRoutes({ip});
 borrower.deactivateSiteExclusionRoutes();check(routes.size()==1,"borrower deleted pre-existing route");original.deactivateSiteExclusionRoutes();check(routes.empty(),"owner cleanup failed");}
 reset();{WindowsDaemon d;d.activateSiteExclusionRoutes({ip});d.activateGeoExclusionRoutes({ip});d.deactivateSiteExclusionRoutes();
 check(routes.size()==1,"site cleanup removed active geo route");gateway=0;luid=0;d.deactivateGeoExclusionRoutes();check(routes.empty(),"shared route last owner cleanup failed");}
 reset();{WindowsDaemon d;d.activateGeoExclusionRoutes({ip});d.activateSiteExclusionRoutes({ip});d.deactivateGeoExclusionRoutes();
 check(routes.size()==1,"geo cleanup removed active site route");d.deactivateSiteExclusionRoutes();check(routes.empty(),"reverse shared owner cleanup failed");}
 reset();{WindowsDaemon d;d.activateGeoExclusionRoutes({ip});deletionError=ERROR_ACCESS_DENIED;d.deactivateGeoExclusionRoutes();
 check(d.m_geoExclusionRoutes.contains(ip),"failed geo cleanup lost ownership");deletionError=0;d.deactivateGeoExclusionRoutes();check(routes.empty(),"geo retry failed");}
 reset();{WindowsDaemon d;check(!d.addExclusionRoute("8.8.8.8/not-a-prefix")&&!d.addExclusionRoute("8.8.8.8/24/32"),"invalid prefix accepted");}
 reset();{WindowsDaemon d;d.activateSiteExclusionRoutes({ip});routes.clear();d.deactivateSiteExclusionRoutes();
 check(d.m_siteExclusionRoutes.isEmpty()&&d.m_ownedExclusionRoutes.isEmpty(),"already removed row must release ownership");}
 reset();{WindowsDaemon d;d.activateSiteExclusionRoutes({"8.8.8.42/24"});d.activateGeoExclusionRoutes({"8.8.8.0/24"});
 check(routes.size()==1&&ntohl(routes[0].DestinationPrefix.Prefix.Ipv4.sin_addr.s_addr)==0x08080800,"canonical subnet ownership");
 d.deactivateSiteExclusionRoutes();check(routes.size()==1,"canonical shared subnet removed early");d.deactivateGeoExclusionRoutes();check(routes.empty(),"canonical subnet final cleanup");}
 reset();{WindowsDaemon owner;owner.activateGeoExclusionRoutes({ip});WindowsDaemon borrower;borrower.activateGeoExclusionRoutes({ip});
 borrower.deactivateGeoExclusionRoutes();check(routes.size()==1,"geo borrower deleted pre-existing route");owner.deactivateGeoExclusionRoutes();check(routes.empty(),"geo owner cleanup failed");}
 std::cout<<"PASS site and geo route ownership, gateway changes, shared references and cleanup retry\n";
}
