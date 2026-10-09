#include <winsock2.h>
#include <windows.h>
#include <iphlpapi.h>
#include <QMap>
#include <QString>
#include <QDebug>
#include <cstdlib>
#include <iostream>

bool rejectSecond = true;
DWORD fakeGetIpForwardTable(PMIB_IPFORWARDTABLE table, PDWORD size, BOOL) {
    if (!table) { *size = sizeof(MIB_IPFORWARDTABLE); return ERROR_INSUFFICIENT_BUFFER; }
    table->dwNumEntries = 0; return ERROR_SUCCESS;
}
DWORD fakeDeleteIpForwardEntry(PMIB_IPFORWARDROW row) {
    if (row->dwForwardDest == 2 && rejectSecond) return ERROR_ACCESS_DENIED;
    if (row->dwForwardDest == 3) return ERROR_NOT_FOUND;
    return ERROR_SUCCESS;
}
class RouterWin {
public:
    QMap<QString, MIB_IPFORWARDROW> m_ipForwardRows;
    bool clearSavedRoutes();
};
#include "production-route-cleanup.inc"
int main() {
    RouterWin router;
    for (int i : {1, 2, 3}) { MIB_IPFORWARDROW row{}; row.dwForwardDest = i; router.m_ipForwardRows.insert(QString::number(i), row); }
    if (router.clearSavedRoutes() || router.m_ipForwardRows.size() != 1 || !router.m_ipForwardRows.contains("2")) return 1;
    rejectSecond = false;
    if (!router.clearSavedRoutes() || !router.m_ipForwardRows.isEmpty()) return 2;
    std::cout << "PASS: failed owned route survives for retry; absent route is idempotent; cleanup reports actual failure\n";
}
