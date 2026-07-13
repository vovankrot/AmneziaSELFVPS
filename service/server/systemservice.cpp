#include "version.h"
#include "localserver.h"
#include "systemservice.h"
#include "logger.h"


#ifdef Q_OS_WIN
#include <windows.h>
#include <tlhelp32.h>
#include <atomic>
#include <thread>
#include "platforms/windows/daemon/windowsdaemontunnel.h"
#include "platforms/windows/daemon/windowsdaemon.h"

// Defined in main.cpp
extern LONG WINAPI serviceExceptionHandler(EXCEPTION_POINTERS *exInfo);
extern void installAllCrashHandlers();

namespace {
int s_argc = 0;
char** s_argv = nullptr;

// Force-terminate every direct child process of the service.
// Used as a watchdog fallback when graceful shutdown stalls
// (xray.exe / tun2socks.exe / ss-local.exe sometimes block QProcess::~QProcess
// for the full 30s SCM timeout, producing Event 7011 and "VPN broken until reboot").
void killAllChildProcessesWin()
{
    const DWORD myPid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        qWarning() << "shutdown watchdog: CreateToolhelp32Snapshot failed:" << GetLastError();
        return;
    }
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    int killed = 0;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (pe.th32ParentProcessID != myPid || pe.th32ProcessID == myPid)
                continue;
            HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
            if (!h)
                continue;
            if (TerminateProcess(h, 0)) {
                ++killed;
                qInfo().noquote() << "shutdown watchdog: TerminateProcess"
                                  << QString::fromWCharArray(pe.szExeFile)
                                  << "(PID:" << pe.th32ProcessID << ")";
            }
            CloseHandle(h);
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    if (killed > 0)
        qInfo() << "shutdown watchdog: killed" << killed << "child process(es)";
}
}  // namespace
#endif

SystemService::SystemService(int argc, char **argv)
    : QtService<QCoreApplication>(argc, argv, SERVICE_NAME)
{
    setServiceDescription("Service for AmneziaVPN");

#ifdef Q_OS_WIN
    if(argc > 2){
        s_argc = argc;
        s_argv = argv;
        QStringList tokens;

        for (int i = 1; i < argc; ++i) {
            tokens.append(QString(argv[i]));
        }

        if (!tokens.empty() && tokens[0] == "tunneldaemon") {
            // Same guard as in main.cpp: a tunneldaemon process must terminate
            // right after the tunnel stops. Falling through here would let it
            // continue as a regular service instance (second daemon) — see the
            // comment in main.cpp runApplication().
            WindowsDaemonTunnel daemon;
            const int exitCode = daemon.run(tokens);
            Logger::flush();
            ::ExitProcess(static_cast<UINT>(exitCode));
        }

    }
#endif

}

void SystemService::start()
{
#ifdef Q_OS_WIN
    // Re-register all crash handlers: QtService creates QCoreApplication internally, which may overwrite them
    installAllCrashHandlers();
#endif

    qInfo() << "SystemService::start() — creating LocalServer";
    QCoreApplication* app = application();
    m_localServer  = new LocalServer();
    qInfo() << "SystemService::start() — LocalServer created OK";
}

void SystemService::stop()
{
#ifdef Q_OS_WIN
    qInfo() << "SystemService::stop() — beginning shutdown (watchdog: 5s child-kill, 8s hard-exit)";

    // Watchdog thread: two phases.
    //   Phase 1 (5 s): force-kill child processes (xray.exe / tun2socks.exe /
    //                  ss-local.exe) so that any QProcess::waitForFinished()
    //                  inside deactivate() unblocks quickly.
    //   Phase 2 (+3 s = 8 s total): if the main thread is still stuck in a
    //                  blocking kernel call (e.g. DeviceIoControl to the
    //                  AmneziaVPNSplitTunnel driver) that the child-kill did
    //                  not unblock, cancel that I/O with CancelSynchronousIo
    //                  and then force-exit the process.  This keeps us well
    //                  within the SCM's 90-second stop timeout (Event 7011).
    HANDLE doneEvent = CreateEventW(nullptr, TRUE /*manual reset*/, FALSE, nullptr);

    // Duplicate a handle to the current (main) thread so the watchdog can
    // cancel any blocking synchronous I/O it is stuck in.
    HANDLE mainThread = nullptr;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(),
                    GetCurrentProcess(), &mainThread,
                    THREAD_TERMINATE, FALSE, 0);

    std::thread watchdog;
    if (doneEvent) {
        watchdog = std::thread([doneEvent, mainThread]() {
            // Phase 1: wait 5 s, then kill child processes.
            DWORD r = WaitForSingleObject(doneEvent, 5000);
            if (r == WAIT_TIMEOUT) {
                qWarning() << "SystemService::stop() watchdog phase-1 — force-killing children";
                killAllChildProcessesWin();

                // Phase 2: wait another 3 s, then cancel stuck kernel I/O and exit.
                r = WaitForSingleObject(doneEvent, 3000);
                if (r == WAIT_TIMEOUT) {
                    qWarning() << "SystemService::stop() watchdog phase-2 — "
                                  "cancelling stuck I/O and force-exiting";
                    if (mainThread)
                        CancelSynchronousIo(mainThread);
                    // Give the main thread one more second to unwind.
                    r = WaitForSingleObject(doneEvent, 1000);
                    if (r == WAIT_TIMEOUT) {
                        Logger::flush();
                        ::ExitProcess(0);
                    }
                }
            }
            if (mainThread)
                CloseHandle(mainThread);
        });
    } else {
        qWarning() << "SystemService::stop() — CreateEvent failed:" << GetLastError();
        if (mainThread)
            CloseHandle(mainThread);
    }

    if (auto *d = WindowsDaemon::instance())
        d->deactivate();

    delete m_localServer;
    m_localServer = nullptr;

    if (doneEvent) {
        SetEvent(doneEvent);
        if (watchdog.joinable())
            watchdog.join();
        CloseHandle(doneEvent);
    }
    qInfo() << "SystemService::stop() — completed";
#else
    delete m_localServer;
    m_localServer = nullptr;
#endif
}
