#include <functional>
#include <iostream>
#include <vector>

template<class T> using QList = std::vector<T>;
struct IPAddress { int id; };
constexpr int ERROR_SUCCESS = 0, LOW_WEIGHT = 0;
struct State { int beginError = 0, commitError = 0, failPrefix = -1, begun = 0, aborted = 0, committed = 0, allowed = 0, disabled = 0; } state;
int FwpmTransactionBegin(void *, void *) { ++state.begun; return state.beginError; }
void FwpmTransactionAbort0(void *) { ++state.aborted; }
int FwpmTransactionCommit0(void *) { ++state.committed; return state.commitError; }
class Guard {
public:
    std::function<void()> cleanup;
    ~Guard() { if (cleanup) cleanup(); }
    void dismiss() { cleanup = {}; }
};
template<class F> Guard qScopeGuard(F cleanup) { return {cleanup}; }
struct Log { Log &error() { return *this; } template<class T> Log &operator<<(const T &) { return *this; } } logger;
class WindowsFirewall {
public:
    void *m_sessionHandle = nullptr;
    std::vector<int> m_activeRules {41, 42};
    bool enableLanBypass(const QList<IPAddress> &ranges);
    bool allowTrafficTo(const IPAddress &prefix, int, const char *) {
        ++state.allowed;
        m_activeRules.push_back(100 + prefix.id * 2);
        if (prefix.id == state.failPrefix) return false;
        m_activeRules.push_back(101 + prefix.id * 2);
        return true;
    }
    void disableKillSwitch() { ++state.disabled; }
};
#include "production-lan-policy.inc"
int main()
{
    WindowsFirewall firewall;
    const auto originalRules = firewall.m_activeRules;
    const QList<IPAddress> ranges {{1}, {2}, {3}};
    state = {}; state.beginError = 5;
    if (firewall.enableLanBypass(ranges) || state.allowed || state.committed || state.aborted || state.disabled) return 1;
    if (firewall.m_activeRules != originalRules) return 5;
    state = {}; state.failPrefix = 2;
    if (firewall.enableLanBypass(ranges) || state.allowed != 2 || state.aborted != 1 || state.committed || state.disabled) return 2;
    if (firewall.m_activeRules != originalRules) return 6;
    state = {}; state.commitError = 5;
    if (firewall.enableLanBypass(ranges) || state.allowed != 3 || state.aborted != 1 || state.committed != 1 || state.disabled) return 3;
    if (firewall.m_activeRules != originalRules) return 7;
    state = {};
    if (!firewall.enableLanBypass(ranges) || state.allowed != 3 || state.aborted || state.committed != 1 || state.disabled) return 4;
    if (firewall.m_activeRules.size() != originalRules.size() + 6 || firewall.m_activeRules[0] != 41 || firewall.m_activeRules[1] != 42) return 8;
    std::cout << "PASS: LAN policy begin/prefix/commit failures preserve prior firewall rules; success commits atomically\n";
}
