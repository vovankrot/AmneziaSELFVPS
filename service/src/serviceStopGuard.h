#pragma once
#include <windows.h>

namespace ServiceStopGuard {
struct Work { HANDLE completed; DWORD timeout; };
inline DWORD WINAPI waitForShutdown(void *argument)
{
    auto *work = static_cast<Work *>(argument);
    const DWORD result = WaitForSingleObject(work->completed,work->timeout);
    CloseHandle(work->completed);
    delete work;
    // This must not depend on Qt's event loop, logger or DLL detach callbacks.
    if (result != WAIT_OBJECT_0) TerminateProcess(GetCurrentProcess(),0);
    return 0;
}
inline bool arm(HANDLE completed,DWORD timeout = 15000)
{
    HANDLE duplicate = nullptr;
    if (!completed || !DuplicateHandle(GetCurrentProcess(),completed,
        GetCurrentProcess(),&duplicate,SYNCHRONIZE,FALSE,0)) return false;
    auto *work = new Work{duplicate,timeout};
    HANDLE thread = CreateThread(nullptr,0,waitForShutdown,work,0,nullptr);
    if (!thread) { CloseHandle(duplicate); delete work; return false; }
    CloseHandle(thread);
    return true;
}
}
