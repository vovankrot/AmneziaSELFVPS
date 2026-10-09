#pragma once

#include <cstdint>
#include <optional>
#include <tuple>
#include <vector>

namespace DefaultRouteSelection {
struct Route {
    std::uint32_t interfaceIndex = 0;
    std::uint32_t gateway = 0;
    std::uint32_t routeMetric = 0;
    std::uint32_t interfaceMetric = 0;
    unsigned prefixLength = 0;
    bool up = false;
    bool loopback = false;
    bool connected = false;
    bool valid = false;
};

inline std::optional<Route> select(const std::vector<Route> &routes)
{
    std::optional<Route> best;
    const auto rank = [](const Route &route) {
        // Windows combines these two metrics. Use 64 bits to avoid overflow.
        return std::make_tuple(std::uint64_t(route.routeMetric) + route.interfaceMetric,
                               route.interfaceIndex, route.gateway);
    };
    for (const auto &route : routes) {
        // An on-link/loopback route cannot be used as an external gateway for
        // the server's /32 exclusion. Ignore stale and non-default entries.
        if (route.prefixLength || !route.interfaceIndex || !route.gateway || !route.up
            || route.loopback || !route.connected || !route.valid) continue;
        if (!best || rank(route) < rank(*best)) best = route;
    }
    return best;
}
}
