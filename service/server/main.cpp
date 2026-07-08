#include <QDir>

#include "version.h"
#include "localserver.h"
#include "logger.h"
#include "systemservice.h"
#include "utilities.h"

#ifdef Q_OS_WIN
#include <windows.h>
#include <dbghelp.h>
#include <signal.h>
#include <csignal>
#include <cstdlib>
#pragma comment(lib, "dbghelp.lib")
#include "platforms/windows/daemon/windowsdaemontunnel.h"

namespace {
int s_argc = 0;
char** s_argv = nullptr;
}  // namespace

static void writeText(HANDLE file, const char *text)
{
    if (file == INVALID_HANDLE_VALUE || text == nullptr) {
        return;
    }

    DWORD written = 0;
    WriteFile(file, text, static_cast<DWORD>(strlen(text)), &written, nullptr);
}

static void writeModuleOffsetLine(HANDLE file, const char *label, DWORD64 address)
{
    char buffer[2048] = {};

    if (address == 0) {
        wsprintfA(buffer, "%s: <null>\r\n", label);
        writeText(file, buffer);
        return;
    }

    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery(reinterpret_cast<LPCVOID>(address), &mbi, sizeof(mbi)) == 0 || mbi.AllocationBase == nullptr) {
        wsprintfA(buffer, "%s: 0x%p\r\n", label, reinterpret_cast<void *>(address));
        writeText(file, buffer);
        return;
    }

    wchar_t modulePathW[MAX_PATH] = {};
    GetModuleFileNameW(static_cast<HMODULE>(mbi.AllocationBase), modulePathW, MAX_PATH);

    char modulePathUtf8[MAX_PATH * 3] = {};
    if (modulePathW[0] != L'\0') {
        WideCharToMultiByte(CP_UTF8, 0, modulePathW, -1, modulePathUtf8, sizeof(modulePathUtf8), nullptr, nullptr);
    }

    const DWORD64 base = reinterpret_cast<DWORD64>(mbi.AllocationBase);
    const DWORD64 offset = address - base;

    if (modulePathUtf8[0] != '\0') {
        wsprintfA(buffer,
                  "%s: 0x%p [%s+0x%llX, base=0x%p]\r\n",
                  label,
                  reinterpret_cast<void *>(address),
                  modulePathUtf8,
                  offset,
                  reinterpret_cast<void *>(base));
    } else {
        wsprintfA(buffer,
                  "%s: 0x%p [module_base=0x%p, offset=0x%llX]\r\n",
                  label,
                  reinterpret_cast<void *>(address),
                  reinterpret_cast<void *>(base),
                  offset);
    }

    writeText(file, buffer);
}

static void writeStackTrace(HANDLE file, EXCEPTION_POINTERS *exInfo)
{
    if (file == INVALID_HANDLE_VALUE || exInfo == nullptr || exInfo->ContextRecord == nullptr) {
        return;
    }

    writeText(file, "Stack trace:\r\n");

    HANDLE process = GetCurrentProcess();
    HANDLE thread = GetCurrentThread();

    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME | SYMOPT_FAIL_CRITICAL_ERRORS);
    SymInitialize(process, nullptr, TRUE);

    CONTEXT context = *exInfo->ContextRecord;
    STACKFRAME64 frame = {};
    DWORD machineType = 0;

#if defined(_M_X64)
    machineType = IMAGE_FILE_MACHINE_AMD64;
    frame.AddrPC.Offset = context.Rip;
    frame.AddrFrame.Offset = context.Rbp;
    frame.AddrStack.Offset = context.Rsp;
#elif defined(_M_IX86)
    machineType = IMAGE_FILE_MACHINE_I386;
    frame.AddrPC.Offset = context.Eip;
    frame.AddrFrame.Offset = context.Ebp;
    frame.AddrStack.Offset = context.Esp;
#else
    writeText(file, "  <stack walking unsupported on this architecture>\r\n");
    SymCleanup(process);
    return;
#endif

    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Mode = AddrModeFlat;

    for (int index = 0; index < 32; ++index) {
        const BOOL walked = StackWalk64(machineType,
                                        process,
                                        thread,
                                        &frame,
                                        &context,
                                        nullptr,
                                        SymFunctionTableAccess64,
                                        SymGetModuleBase64,
                                        nullptr);
        if (!walked || frame.AddrPC.Offset == 0) {
            break;
        }

        char label[64] = {};
        wsprintfA(label, "  #%02d", index);
        writeModuleOffsetLine(file, label, frame.AddrPC.Offset);
    }

    SymCleanup(process);
}

static void writeCrashFile(DWORD code, void *addr, EXCEPTION_POINTERS *exInfo)
{
    // Write crash marker & minidump directly, bypassing Logger (may be broken)
    wchar_t dumpDir[MAX_PATH] = {};
    if (ExpandEnvironmentStringsW(L"%ProgramData%\\AmneziaVPN\\log", dumpDir, MAX_PATH) == 0)
        wcscpy_s(dumpDir, L"C:\\ProgramData\\AmneziaVPN\\log");
    CreateDirectoryW(dumpDir, nullptr);

    // 1) Crash text file
    wchar_t txtPath[MAX_PATH];
    wsprintfW(txtPath, L"%s\\service-crash.txt", dumpDir);
    HANDLE hFile = CreateFileW(txtPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile != INVALID_HANDLE_VALUE) {
        char buf[512];
        SYSTEMTIME st;
        GetLocalTime(&st);
        int len = wsprintfA(buf,
            "SERVICE CRASH at %04d-%02d-%02d %02d:%02d:%02d\r\n"
            "Exception code: 0x%08lX\r\n"
            "Exception address: 0x%p\r\n"
            "Process ID: %lu\r\n"
            "Thread ID: %lu\r\n",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
            code, addr, GetCurrentProcessId(), GetCurrentThreadId());
        writeText(hFile, buf);
        writeModuleOffsetLine(hFile, "Exception frame", reinterpret_cast<DWORD64>(addr));
        writeStackTrace(hFile, exInfo);
        CloseHandle(hFile);
    }

    // 2) Minidump for post-mortem debugging
    if (exInfo) {
        wchar_t dmpPath[MAX_PATH];
        wsprintfW(dmpPath, L"%s\\service-crash.dmp", dumpDir);
        HANDLE hDump = CreateFileW(dmpPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hDump != INVALID_HANDLE_VALUE) {
            MINIDUMP_EXCEPTION_INFORMATION mei;
            mei.ThreadId = GetCurrentThreadId();
            mei.ExceptionPointers = exInfo;
            mei.ClientPointers = FALSE;
            MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hDump,
                              MiniDumpWithDataSegs, &mei, nullptr, nullptr);
            CloseHandle(hDump);
        }
    }
}

LONG WINAPI serviceExceptionHandler(EXCEPTION_POINTERS *exInfo)
{
    DWORD code = exInfo ? exInfo->ExceptionRecord->ExceptionCode : 0;
    void *addr = exInfo ? exInfo->ExceptionRecord->ExceptionAddress : nullptr;

    // Write crash info directly to ProgramData (bypass Logger)
    writeCrashFile(code, addr, exInfo);

    // Also try Logger
    qCritical() << "SERVICE CRASH: Unhandled exception code=" << Qt::hex << code
                << "at address=" << addr;
    Logger::flush();

    return EXCEPTION_CONTINUE_SEARCH;
}

// Catches std::terminate() — called when C++ exception escapes noexcept, or double exception, etc.
// abort() and SEH do NOT catch this.
static void serviceTerminateHandler()
{
    Logger::writeEmergencyCrashFile("std::terminate",
        "Called from noexcept violation, uncaught exception in destructor, or double exception");
    qCritical() << "SERVICE CRASH: std::terminate() called";
    Logger::flush();
    _exit(4);
}

// Catches SIGABRT — raised by abort(), assert(), etc.
static void serviceAbortHandler(int)
{
    Logger::writeEmergencyCrashFile("SIGABRT", "abort() or assert() called");
    qCritical() << "SERVICE CRASH: SIGABRT received";
    Logger::flush();
    // Reset signal handler to default and re-raise to let OS handle it
    signal(SIGABRT, SIG_DFL);
    _exit(5);
}

// Catches pure virtual function calls
static void servicePurecallHandler()
{
    Logger::writeEmergencyCrashFile("purecall", "Pure virtual function call");
    qCritical() << "SERVICE CRASH: Pure virtual function call";
    Logger::flush();
    _exit(6);
}

void installAllCrashHandlers()
{
    SetUnhandledExceptionFilter(serviceExceptionHandler);
    std::set_terminate(serviceTerminateHandler);
    signal(SIGABRT, serviceAbortHandler);
    _set_purecall_handler(servicePurecallHandler);
}
#endif

int runApplication(int argc, char** argv)
{
    QCoreApplication app(argc,argv);

#ifdef Q_OS_WIN
    // Re-register after QCoreApplication (Qt may overwrite the SEH/terminate/abort handlers)
    installAllCrashHandlers();

    if(argc > 2){
        s_argc = argc;
        s_argv = argv;
        QStringList tokens;
        for (int i = 1; i < argc; ++i) {
            tokens.append(QString(argv[i]));
        }

        if (!tokens.empty() && tokens[0] == "tunneldaemon") {
            WindowsDaemonTunnel *daemon = new WindowsDaemonTunnel();
            daemon->run(tokens);
        }
    }
#endif

    LocalServer localServer;
    return app.exec();

}


int main(int argc, char **argv)
{
    Utils::initializePath(Logger::systemLogDir());

    // Always enable service file logging for crash diagnostics
    Logger::init(true);

#ifdef Q_OS_WIN
    installAllCrashHandlers();
#endif

    qInfo() << "======== AmneziaVPN service starting ========";

    try {
        if (argc >= 2) {
            qInfo() << "Started as console application";
            return runApplication(argc, argv);
        }
        else {
            qInfo() << "Started as system service";
#ifdef Q_OS_WIN
            SystemService systemService(argc, argv);
            return systemService.exec();
#else
            return runApplication(argc, argv);
#endif
        }
    } catch (const std::exception &ex) {
        qCritical() << "SERVICE CRASH: Unhandled C++ exception:" << ex.what();
        Logger::flush();
        return 1;
    } catch (...) {
        qCritical() << "SERVICE CRASH: Unknown unhandled exception";
        Logger::flush();
        return 1;
    }
}
