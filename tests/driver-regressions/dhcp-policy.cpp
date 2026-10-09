#include <winsock2.h>
#include <windows.h>
#include <fwpmu.h>
#include <QString>
#include <QList>
#include <iostream>
const GUID ST_FW_WINFW_BASELINE_SUBLAYER_KEY{};
struct Rule {GUID layer;uint16_t local=0,remote=0;uint8_t protocol=0,weight=0;uint32_t flags=0;FWP_MATCH_TYPE match=FWP_MATCH_EQUAL;};
class WindowsFirewall {
public:
    QList<Rule> rules;int calls=0,failAt=0;
    bool allowDHCPTraffic(uint8_t,const QString&);
    bool allowLoopbackTraffic(uint8_t,const QString&);
    bool enableFilter(FWPM_FILTER0 *filter,const QString&,const QString&) {
        if(++calls==failAt)return false;
        Rule row;row.layer=filter->layerKey;row.weight=filter->weight.uint8;
        for(unsigned i=0;i<filter->numFilterConditions;++i){
            const auto &c=filter->filterCondition[i];
            if(IsEqualGUID(c.fieldKey,FWPM_CONDITION_IP_LOCAL_PORT))row.local=c.conditionValue.uint16;
            if(IsEqualGUID(c.fieldKey,FWPM_CONDITION_IP_REMOTE_PORT))row.remote=c.conditionValue.uint16;
            if(IsEqualGUID(c.fieldKey,FWPM_CONDITION_IP_PROTOCOL))row.protocol=c.conditionValue.uint8;
            if(IsEqualGUID(c.fieldKey,FWPM_CONDITION_FLAGS)){row.flags=c.conditionValue.uint32;row.match=c.matchType;}
        }
        rules.append(row);return true;
    }
};
#include "production-dhcp.inc"
#include "production-loopback.inc"
void check(bool ok,const char *message){if(!ok){std::cerr<<message<<'\n';std::exit(1);}}
int main(){
    WindowsFirewall fw;check(fw.allowDHCPTraffic(7,"DHCP")&&fw.rules.size()==4,"all four DHCP layers");
    const GUID layers[]={FWPM_LAYER_ALE_AUTH_CONNECT_V4,FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V4,
        FWPM_LAYER_ALE_AUTH_CONNECT_V6,FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V6};
    for(int i=0;i<4;++i){
        const auto &rule=fw.rules[i];
        check(IsEqualGUID(rule.layer,layers[i])&&rule.protocol==IPPROTO_UDP&&rule.weight==7,"DHCP layer, UDP and weight");
        check(rule.local==(i<2?68:546)&&rule.remote==(i<2?67:547),"DHCP client/server ports must match IP family");
    }
    for(int fail=1;fail<=4;++fail){WindowsFirewall bad;bad.failAt=fail;
        check(!bad.allowDHCPTraffic(7,"DHCP")&&bad.calls==fail,"failed filter insertion must propagate");}
    WindowsFirewall local;check(local.allowLoopbackTraffic(7,"Local %1")&&local.rules.size()==4,"all four loopback layers");
    for(int i=0;i<4;++i){const auto &rule=local.rules[i];
        check(IsEqualGUID(rule.layer,layers[i])&&rule.weight==7&&rule.flags==FWP_CONDITION_FLAG_IS_LOOPBACK
            &&rule.match==FWP_MATCH_FLAGS_ALL_SET,"loopback policy must match only local flows, without an interface index");}
    for(int fail=1;fail<=4;++fail){WindowsFirewall bad;bad.failAt=fail;
        check(!bad.allowLoopbackTraffic(7,"Local %1")&&bad.calls==fail,"loopback insertion failure propagates to baseline rollback");}
    std::cout<<"PASS: DHCP and loopback IPv4/IPv6, both directions, scoped conditions and insertion failures\n";
}
