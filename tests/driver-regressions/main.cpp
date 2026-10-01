#include <Windows.h>
#include <atomic>
#include <chrono>
#include <thread>
#include <mutex>
#include <set>
#include <memory>
#include <vector>
#include <cstring>
#include <iostream>
#include <cstdlib>
#include "splitTunnelDriverProtocol.h"
struct Log { template<class T> Log &operator<<(T const &) { return *this; } };
struct Logger { Log error() { return {}; } } logger;
struct QThread { static void msleep(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); } };
std::atomic<int> calls{0};
std::atomic<bool> completed{false};
BOOL fakeIo(HANDLE, DWORD code, LPVOID input, DWORD inSize, LPVOID output, DWORD outSize, DWORD *bytes, void *) {
    ++calls;
    if (code == 2) QThread::msleep(150);
    if (code == 3) { *bytes = outSize + 1; return TRUE; }
    if (code == 4) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
    if (inSize && static_cast<unsigned char *>(input)[0] != 42) std::abort();
    if (outSize) static_cast<unsigned char *>(output)[0] = 99;
    *bytes = outSize ? 1 : 0;
    completed = true;
    return TRUE;
}
#define DeviceIoControl fakeIo
#include "production-helper.h"
#undef DeviceIoControl
void check(bool ok, const char *message) { if (!ok) { std::cerr << message << '\n'; std::exit(1); } }
int main() {
    unsigned char in=42, out=0;
    DWORD bytes=0;
    auto handle = reinterpret_cast<HANDLE>(1);
    check(DeviceIoControlWithTimeout(handle,1,&in,1,&out,1,&bytes,"success") && out==99 && bytes==1,"copy output");
    out=0;
    completed=false;
    auto start=std::chrono::steady_clock::now();
    check(!DeviceIoControlWithTimeout(handle,2,&in,1,&out,1,&bytes,"timeout"),"must timeout");
    check(GetLastError()==ERROR_TIMEOUT,"timeout error");
    in=0; // The caller's storage can now be changed/freed.
    check(std::chrono::steady_clock::now()-start < std::chrono::milliseconds(120),"bounded caller");
    int count=calls;
    check(!DeviceIoControlWithTimeout(handle,1,nullptr,0,nullptr,0,&bytes,"quarantine"),"quarantine");
    check(calls==count,"must not queue more IRPs on a failed handle");
    QThread::msleep(180);
    check(completed && out==0,"late completion must not touch caller memory");
    handle=reinterpret_cast<HANDLE>(2);
    check(!DeviceIoControlWithTimeout(handle,3,nullptr,0,&out,1,&bytes,"invalid size") && GetLastError()==ERROR_INVALID_DATA,"size validation");
    check(!DeviceIoControlWithTimeout(handle,4,nullptr,0,nullptr,0,&bytes,"failure") && GetLastError()==ERROR_ACCESS_DENIED,"OS error preservation");
    handle=reinterpret_cast<HANDLE>(3);
    in=42; out=0; completed=false;
    check(DeviceIoControlWithTimeout(handle,2,&in,1,&out,1,&bytes,"slow activation",300) && out==99,
          "Slow healthy activation must complete within its separate budget");
    check(!driverFailed(handle),"Healthy slow request must not quarantine the handle");
    for(int state=0;state<=3;++state) check(SplitTunnelDriverProtocol::isPassiveState(state),"Passive state rejected");
    check(!SplitTunnelDriverProtocol::isPassiveState(-1) && !SplitTunnelDriverProtocol::isPassiveState(4) &&
          !SplitTunnelDriverProtocol::isPassiveState(5),"Unknown or engaged/zombie state accepted");
    std::cout << "PASS: slow activation budget and READY cleanup semantics\n";
    std::cout << "PASS: success, timeout, quarantine, late completion, output bounds, OS error\n";
    const GUID baseline{0xc78056ff,0x2bc1,0x4211,{0xaa,0xdd,0x7f,0x35,0x8d,0xef,0x20,0x2d}};
    using namespace SplitTunnelDriverProtocol;
    static_assert(initialize == 0x80000004 && initializeLegacy == 0x80000007);
    int probes=0;
    auto modern=initializeDriver(baseline,baseline,[&](DWORD code,const void* data,DWORD size) {
        ++probes;
        check(code==initialize && size==32 && data,"Modern initialize request ABI");
        auto input=static_cast<const SublayerGuids*>(data);
        check(IsEqualGUID(input->baseline,baseline) && IsEqualGUID(input->dns,baseline),"Wrong policy sublayer GUIDs");
        return TRUE;
    });
    check(modern.ok && modern.api==Api::SublayerGuids && probes==1,"Modern driver initialization");
    probes=0;
    auto legacy=initializeDriver(baseline,baseline,[&](DWORD code,const void* data,DWORD size) {
        ++probes;
        if(code==initialize) { SetLastError(ERROR_INVALID_FUNCTION); return FALSE; }
        check(code==initializeLegacy && !data && !size,"Legacy initialize request ABI");
        return TRUE;
    });
    check(legacy.ok && legacy.api==Api::Legacy && probes==2,"Legacy driver fallback");
    for(DWORD error:{ERROR_ACCESS_DENIED,ERROR_TIMEOUT,ERROR_INVALID_PARAMETER}) {
        probes=0;
        auto failed=initializeDriver(baseline,baseline,[&](DWORD,const void*,DWORD) { ++probes; SetLastError(error); return FALSE; });
        check(!failed.ok && failed.error==error && probes==1,"Unsafe fallback after real initialize failure");
    }
    auto bothFailed=initializeDriver(baseline,baseline,[](DWORD code,const void*,DWORD) { SetLastError(code==initialize?ERROR_NOT_SUPPORTED:ERROR_ACCESS_DENIED); return FALSE; });
    check(!bothFailed.ok && bothFailed.error==ERROR_ACCESS_DENIED,"Legacy initialization failure hidden");
    std::cout << "PASS: driver 1.3 ABI, legacy transition, failure preservation and no retry on timeout\n";
}
