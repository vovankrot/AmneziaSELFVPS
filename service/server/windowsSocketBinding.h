#pragma once
#include <winsock2.h>
#include <ws2tcpip.h>

namespace WindowsSocketBinding {
enum class Status { Bound, DatagramUsesHostRoute, NoInterface, Failed };
struct Result { Status status; int family; int error; };

// mKCP datagrams retain the explicit server host route. Pin TCP/direct sockets
// only through their actual address family, rather than trying both protocols.
template<class GetOption, class SetOption, class GetError>
Result bindTcp(SOCKET socket, DWORD interfaceIndex, GetOption get, SetOption set, GetError error) {
    int type = 0, size = sizeof(type);
    if (get(socket, SOL_SOCKET, SO_TYPE, reinterpret_cast<char*>(&type), &size) != 0)
        return {Status::Failed, AF_UNSPEC, error()};
    if (type == SOCK_DGRAM) return {Status::DatagramUsesHostRoute, AF_UNSPEC, 0};
    if (!interfaceIndex) return {Status::NoInterface, AF_UNSPEC, 0};
    WSAPROTOCOL_INFOW protocol{}; size = sizeof(protocol);
    if (get(socket, SOL_SOCKET, SO_PROTOCOL_INFOW, reinterpret_cast<char*>(&protocol), &size) != 0)
        return {Status::Failed, AF_UNSPEC, error()};
    if (protocol.iAddressFamily != AF_INET && protocol.iAddressFamily != AF_INET6)
        return {Status::Failed, protocol.iAddressFamily, WSAEAFNOSUPPORT};
    const bool ipv4 = protocol.iAddressFamily == AF_INET;
    DWORD index = ipv4 ? htonl(interfaceIndex) : interfaceIndex;
    const int result = set(socket, ipv4 ? IPPROTO_IP : IPPROTO_IPV6,
        ipv4 ? IP_UNICAST_IF : IPV6_UNICAST_IF, reinterpret_cast<const char*>(&index), sizeof(index));
    return {result == 0 ? Status::Bound : Status::Failed, protocol.iAddressFamily, result == 0 ? 0 : error()};
}
}
