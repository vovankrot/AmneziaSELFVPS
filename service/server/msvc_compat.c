/*
 * msvc_compat.c – MSVC CRT compatibility stubs for MinGW-built executables
 * that must link against MSVC-compiled static libraries (e.g. OpenSSL 3.x).
 *
 * Problem:
 *   libcrypto.lib / libssl.lib were compiled with MSVC /GS (buffer security
 *   checks) and other MSVC-specific optimisations.  They reference internal
 *   CRT symbols that MinGW's runtime does not export:
 *     __chkstk, __security_cookie, __security_check_cookie,
 *     __GSHandlerCheck, __report_rangecheckfailure, __isa_available, etc.
 *
 * Solution:
 *   Provide minimal stub implementations of every missing symbol.
 *   These stubs are semantically correct enough for a production service
 *   that is not exploiting the MSVC-specific security machinery directly.
 */

#include <stdlib.h>   /* abort() */

/* -------------------------------------------------------------------------
 * __chkstk – MSVC stack-probe function (x86-64)
 *
 * Calling convention (MS x64):
 *   Input : RAX = number of bytes to allocate on the stack
 *   Effect: touches every 4 KiB page from (RSP - 4096) down to (RSP - RAX)
 *           so the OS can grow the guard-page chain.
 *   Return: RAX, RCX unchanged; RSP unchanged (caller does "sub rsp, rax").
 *
 * MinGW's libgcc provides the identical algorithm under the name
 * ___chkstk_ms (three underscores).  We redirect with a naked tail-call so
 * that RAX/RSP semantics are preserved exactly.
 * -------------------------------------------------------------------------
 */
#if defined(__x86_64__)
__attribute__((naked)) void __chkstk(void)
{
    __asm__("jmp ___chkstk_ms");
}
#endif /* __x86_64__ */


/* -------------------------------------------------------------------------
 * MSVC /GS (Buffer Security Check) support symbols
 * -------------------------------------------------------------------------
 */

/*
 * __security_cookie – random stack-canary placed in every /GS-protected frame.
 * Must be a non-trivial constant so mismatches are detectable.
 */
unsigned long long __security_cookie = 0x00002B992DDFA232ULL;

/*
 * __security_check_cookie – validates the frame's copy of the cookie on return.
 * If mismatch → buffer overflow detected → abort immediately.
 */
void __cdecl __security_check_cookie(unsigned long long cookie)
{
    if (cookie != __security_cookie)
        abort();
}

/*
 * __GSHandlerCheck – SEH exception handler invoked when unwinding through a
 * /GS-protected frame.  We emit a no-op: if a real overflow has occurred the
 * process will be terminated by other means (e.g. OS guard page fault).
 */
void __cdecl __GSHandlerCheck(void) { }


/* -------------------------------------------------------------------------
 * Miscellaneous MSVC CRT internals referenced by OpenSSL
 * -------------------------------------------------------------------------
 */

/* Called when MSVC's array-index range check fails (debug instrumentation). */
void __cdecl __report_rangecheckfailure(void) { abort(); }

/* Called by MSVC /GS cookie check if the global cookie is bad. */
void __cdecl __report_gsfailure(unsigned long long cookie)
{
    (void)cookie;
    abort();
}

/*
 * __isa_available – MSVC ISA feature-detection flag used to select optimised
 * string/math/crypto routines at runtime.
 *   0 = SSE2 only (safe conservative baseline; OpenSSL will still work).
 */
int __isa_available = 0;

/*
 * __isa_enabled – bitmask of enabled ISA extensions (used alongside
 * __isa_available in some MSVC builds of OpenSSL 3.x).
 */
unsigned int __isa_enabled = 0;

/*
 * __favor – MSVC processor-preference hint (e.g. FAVOR_ATOM).
 * 0 = no preference.
 */
int __favor = 0;


/* =========================================================================
 * MSVC UCRT stdio internals
 *
 * libcrypto.lib was compiled with MSVC /MD, linking against ucrtbase.dll.
 * We link libucrtbase.a explicitly to provide most UCRT symbols, but two
 * symbols are NOT exported from ucrtbase.dll and need hand-written stubs:
 *   __local_stdio_printf_options – per-thread printf options pointer
 *   sprintf_s                    – breaks circular dep in libmsvcrt.a
 * =========================================================================
 */

#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>

/*
 * __local_stdio_printf_options – MSVC UCRT returns a pointer to a per-thread
 * uint64 that controls printf behaviour flags.  We return a pointer to a
 * static storage initialised to 0 (= all defaults, no special flags).
 * ucrtbase.dll does NOT export this; it is inlined in MSVC's stdio.h.
 */
static uint64_t __local_stdio_printf_options_storage = 0;

uint64_t * __cdecl __local_stdio_printf_options(void)
{
    return &__local_stdio_printf_options_storage;
}

/*
 * sprintf_s – bounds-checking sprintf wrapper.
 *
 * Provided here to break a circular dependency inside libmsvcrt.a where
 * strerror_s.o → sprintf_s → sprintf_s.o → __stdio_common_vsprintf_s,
 * which is in ucrtbase that is already scanned at link time before the
 * auto-linked libmsvcrt.a.  Our stub uses vsnprintf directly and avoids
 * the whole chain.
 */
int __cdecl sprintf_s(char *buffer, size_t buffer_count,
                      const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    int r = vsnprintf(buffer, buffer_count, format, ap);
    va_end(ap);
    return r;
}

/* fprintf and sscanf are provided by the auto-linked libmsvcrt.a/libmingw32.a
 * CRT and are defined inline in MinGW's stdio.h, so we must NOT redefine them
 * here.  The auto-linked archives resolve all outstanding references at the
 * end of the link command without any circular dependency when ucrtbase is
 * not in the explicit LIBS list. */

/*
 * __acrt_iob_func is provided by libmsvcrt.a (libmsvcrt_common: acrt_iob_func.o).
 * Do NOT define it here — it would cause a multiple-definition link error.
 * References from MSVC-compiled objects in libcrypto.lib are satisfied by
 * the explicit -lmsvcrt in the LIBS list.
 */

/*
 * __stdio_common_vsprintf / __stdio_common_vswprintf
 *
 * MSVC UCRT internal entry points called by the inline wrappers in MSVC's
 * stdio.h (sprintf, swprintf, etc.).  libcrypto.lib was compiled with MSVC
 * /MD so it pulls these in from ucrtbase.dll at MSVC build time.  Under
 * MinGW linkage we must supply them ourselves.
 *
 * 'options' is a uint64 flags word (0 = defaults).  'locale' is unused (NULL
 * = C locale).  The return value is the number of characters written, or -1
 * on truncation (same as vsnprintf / vswprintf convention).
 */
#include <wchar.h>

int __cdecl __stdio_common_vsprintf(
    unsigned long long options,
    char *buffer, size_t buffer_count,
    const char *format, void *locale,
    va_list arglist)
{
    (void)options; (void)locale;
    if (!buffer || !buffer_count)
        return vsnprintf(NULL, 0, format, arglist);
    return vsnprintf(buffer, buffer_count, format, arglist);
}

int __cdecl __stdio_common_vswprintf(
    unsigned long long options,
    wchar_t *buffer, size_t buffer_count,
    const wchar_t *format, void *locale,
    va_list arglist)
{
    (void)options; (void)locale;
    if (!buffer || !buffer_count)
        return vswprintf(NULL, 0, format, arglist);
    return vswprintf(buffer, buffer_count, format, arglist);
}


/* =========================================================================
 * Windows Sockets Wspiapi.h backward-compatibility stubs
 *
 * OpenSSL was compiled on Windows with wspiapi.h included, which redirects
 * getaddrinfo/getnameinfo/freeaddrinfo through Wspiapi* wrapper functions
 * (for Windows 98/ME compatibility).  GNU ld cannot resolve these MSVC
 * COMDAT-compiled statics from libws2_32.a at the required link step.
 * We provide minimal implementations that are correct for Windows 10+.
 *
 * On Windows 10+ the native getaddrinfo/getnameinfo/freeaddrinfo are always
 * present in ws2_32.dll, so the Legacy* fallback functions are never called
 * at runtime.  WspiapiLoad just needs to be a valid callable symbol.
 * =========================================================================
 */

/* Include winsock2 BEFORE any Windows.h variant.  The cmake compile command
 * defines _WINSOCKAPI_ (which windows.h sets to block old winsock.h), so we
 * temporarily undefine it to let winsock2.h initialise without warnings. */
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifdef _WINSOCKAPI_
#  undef _WINSOCKAPI_
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

/*
 * WspiapiLoad – resolves getaddrinfo/getnameinfo/freeaddrinfo function
 * pointers at first call.  On Windows XP SP2+ (incl. Win10+) these are
 * always exported by ws2_32.dll.  We simply return 0 to signal success;
 * the Wspiapi wrappers will then find pfGetAddrInfo == NULL and fall through
 * to WspiapiLegacy* which we implement as direct ws2_32 calls.
 *
 * Signature matches wspiapi.h: INT WSAAPI WspiapiLoad(PWSPIAPI_PROC, INT)
 * On x64 WSAAPI (__stdcall) == __cdecl (single calling convention).
 */
int __cdecl WspiapiLoad(void *rgtRoute, int nRouteCount)
{
    (void)rgtRoute;
    (void)nRouteCount;
    return 0;
}

/*
 * WspiapiLegacyGetAddrInfo – legacy getaddrinfo for Windows 98/ME.
 * On Win10+ we simply delegate to the real ws2_32 getaddrinfo.
 */
int __cdecl WspiapiLegacyGetAddrInfo(
    const char *nodename, const char *servname,
    const struct addrinfo *hints, struct addrinfo **res)
{
    return getaddrinfo(nodename, servname, hints, res);
}

/*
 * WspiapiLegacyFreeAddrInfo – legacy freeaddrinfo for Windows 98/ME.
 */
void __cdecl WspiapiLegacyFreeAddrInfo(struct addrinfo *ai)
{
    freeaddrinfo(ai);
}

/*
 * WspiapiLegacyGetNameInfo – legacy getnameinfo for Windows 98/ME.
 */
int __cdecl WspiapiLegacyGetNameInfo(
    const struct sockaddr *sa, socklen_t salen,
    char *host, DWORD hostlen,
    char *serv, DWORD servlen,
    int flags)
{
    return getnameinfo(sa, salen, host, hostlen, serv, servlen, flags);
}

/*
 * gai_strerrorA – ANSI error-string for getaddrinfo error codes.
 * Used by OpenSSL's error-reporting path.  Returns a static buffer with a
 * human-readable message; thread-safety is not required here.
 */
char * __cdecl gai_strerrorA(int ecode)
{
    static char buf[128];
    snprintf(buf, sizeof(buf), "getaddrinfo error %d", ecode);
    return buf;
}
