#include "windowsSocketBinding.h"
#include <iostream>
#include <cstdlib>
#include <cstring>
void check(bool ok) { if (!ok) std::abort(); }
int main() {
    using namespace WindowsSocketBinding;
    for (int family : {AF_INET, AF_INET6}) {
        int writes=0;
        auto get = [family](SOCKET, int, int option, char* out, int*) {
            if(option==SO_TYPE) *reinterpret_cast<int*>(out)=SOCK_STREAM;
            else reinterpret_cast<WSAPROTOCOL_INFOW*>(out)->iAddressFamily=family;
            return 0;
        };
        auto set = [&](SOCKET, int level, int option, const char* input, int size) {
            ++writes; check(size==4);
            check(level==(family==AF_INET?IPPROTO_IP:IPPROTO_IPV6));
            check(option==(family==AF_INET?IP_UNICAST_IF:IPV6_UNICAST_IF));
            DWORD index; std::memcpy(&index,input,4); check(index==(family==AF_INET?htonl(7):7));
            return 0;
        };
        auto result=bindTcp(1,7,get,set,[]{return 123;});
        check(result.status==Status::Bound && writes==1 && result.error==0);
        auto failed=bindTcp(1,7,get,[](SOCKET,int,int,const char*,int){return -1;},[]{return 123;});
        check(failed.status==Status::Failed && failed.error==123);
    }
    int writes=0;
    auto udpGet=[](SOCKET,int,int option,char* out,int*) { check(option==SO_TYPE); *reinterpret_cast<int*>(out)=SOCK_DGRAM; return 0; };
    auto udp=bindTcp(1,7,udpGet,[&](SOCKET,int,int,const char*,int){++writes;return 0;},[]{return 0;});
    check(udp.status==Status::DatagramUsesHostRoute && writes==0);
    auto error=bindTcp(1,7,[](SOCKET,int,int,char*,int*){return -1;},[&](SOCKET,int,int,const char*,int){++writes;return 0;},[]{return WSAENOTSOCK;});
    check(error.status==Status::Failed && error.error==WSAENOTSOCK && writes==0);
    std::cout << "PASS: TCP family/index byte order, single binding, OS errors, UDP host-route preservation\n";
}
