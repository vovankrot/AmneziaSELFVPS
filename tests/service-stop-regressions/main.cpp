#include "service/src/serviceStopGuard.h"
#include <cstdio>
#include <string>

int main(int argc,char **argv)
{
    if (argc > 1) {
        HANDLE done = CreateEventW(nullptr,TRUE,FALSE,nullptr);
        if (!ServiceStopGuard::arm(done,300)) return 9;
        if (std::string(argv[1]) == "normal") {
            SetEvent(done); CloseHandle(done); Sleep(600); return 7;
        }
        // Neither an event loop nor a logger is available to complete shutdown.
        Sleep(INFINITE);
        return 99;
    }
    wchar_t path[MAX_PATH];
    if (!GetModuleFileNameW(nullptr,path,MAX_PATH)) return 1;
    for (const auto *mode : {L"normal",L"blocked"}) {
        std::wstring command=L"\"" + std::wstring(path) + L"\" " + mode;
        STARTUPINFOW startup{}; startup.cb=sizeof(startup);
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(path,command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,
            nullptr,nullptr,&startup,&process)) return 2;
        const auto wait=WaitForSingleObject(process.hProcess,2500);
        DWORD code=99; GetExitCodeProcess(process.hProcess,&code);
        if (wait != WAIT_OBJECT_0) TerminateProcess(process.hProcess,99);
        CloseHandle(process.hThread); CloseHandle(process.hProcess);
        if (wait != WAIT_OBJECT_0 || code != (std::wstring(mode)==L"normal" ? 7u : 0u)) return 3;
    }
    std::puts("PASS: normal shutdown survives watchdog; blocked shutdown exits without Qt or logging");
    return 0;
}
