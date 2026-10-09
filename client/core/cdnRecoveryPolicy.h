#pragma once

#include <QHostAddress>
#include <QString>
#include <array>

namespace CdnRecovery {

// One instance belongs to one (session, numeric IP, TCP port). The hostname is
// used for authenticated TLS probes, never as a wildcard routing exception.
enum class Probe { Success, Timeout, OtherFailure };
enum class Change { None, Install, Remove };

inline bool publicIpv4(const QHostAddress &address)
{
    if (address.protocol() != QAbstractSocket::IPv4Protocol) return false;
    const quint32 ip = address.toIPv4Address();
    struct Network { quint32 prefix; int bits; };
    // Keep LAN, CGNAT, loopback, documentation, benchmarking, multicast and
    // reserved ranges out of adaptive routing, regardless of DNS answers.
    constexpr std::array<Network, 15> excluded {{
        {0x00000000, 8}, {0x0a000000, 8}, {0x64400000, 10},
        {0x7f000000, 8}, {0xa9fe0000, 16}, {0xac100000, 12},
        {0xc0000000, 24}, {0xc0000200, 24}, {0xc0a80000, 16},
        {0xc6120000, 15}, {0xc6336400, 24}, {0xcb007100, 24},
        {0xe0000000, 4}, {0xf0000000, 4}, {0xc0586300, 24}
    }};
    for (const auto &network : excluded) {
        const quint32 mask = 0xffffffffu << (32 - network.bits);
        if ((ip & mask) == network.prefix) return false;
    }
    return true;
}

class Policy {
public:
    static constexpr qint64 SampleIntervalMs = 10000;
    static constexpr qint64 EvidenceWindowMs = 60000;
    static constexpr qint64 MinimumHoldMs = 120000;
    static constexpr qint64 LeaseMs = 600000;

    Policy(quint64 session, const QHostAddress &address, quint16 port)
        : m_session(session), m_valid(session && publicIpv4(address) && port == 443) {}

    // Times are monotonic and results must belong to the original session. A
    // verified VPN response to the SAME numeric destination is required for
    // every timeout counted towards installation. Certificate failures do not
    // justify automatic policy changes.
    Change observe(quint64 session, qint64 now, Probe direct, Probe vpn)
    {
        if (!m_valid || session != m_session || m_stopping || m_pending != Change::None
            || now < 0 || (m_lastSample >= 0 && now - m_lastSample < SampleIntervalMs))
            return Change::None;
        if (m_lastSample >= 0 && now - m_lastSample > EvidenceWindowMs) resetEvidence();
        m_lastSample = now;
        if (m_active) {
            m_successes = direct == Probe::Success ? m_successes + 1 : 0;
            if (now >= m_appliedAt + LeaseMs
                || (now >= m_appliedAt + MinimumHoldMs && m_successes >= 3))
                return request(Change::Remove);
        } else {
            m_failures = direct == Probe::Timeout && vpn == Probe::Success ? m_failures + 1 : 0;
            if (m_failures >= 3) return request(Change::Install);
        }
        return Change::None;
    }

    Change expire(quint64 session, qint64 now)
    {
        if (session == m_session && m_active && m_pending == Change::None
            && now >= m_appliedAt + LeaseMs) return request(Change::Remove);
        return Change::None;
    }

    Change stop()
    {
        m_stopping = true;
        // An in-flight installation must finish first; acknowledge() then
        // requests cleanup. A failed cleanup retains ownership for retry.
        if (m_active && m_pending == Change::None) return request(Change::Remove);
        return Change::None;
    }

    Change acknowledge(quint64 session, Change change, bool success, qint64 now)
    {
        if (session != m_session || change == Change::None || change != m_pending)
            return Change::None;
        m_pending = Change::None;
        resetEvidence();
        if (success) {
            m_active = change == Change::Install;
            if (m_active) m_appliedAt = now;
        }
        if (m_stopping && m_active && success && change == Change::Install)
            return request(Change::Remove);
        return Change::None;
    }

    bool active() const { return m_active; }
    bool drained() const { return !m_active && m_pending == Change::None; }
    Change pending() const { return m_pending; }

private:
    Change request(Change change) { m_pending = change; return change; }
    void resetEvidence() { m_failures = m_successes = 0; }
    quint64 m_session;
    bool m_valid = false, m_active = false, m_stopping = false;
    Change m_pending = Change::None;
    int m_failures = 0, m_successes = 0;
    qint64 m_lastSample = -1, m_appliedAt = 0;
};
}
