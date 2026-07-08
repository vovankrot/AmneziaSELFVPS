/*
 * msvc_stdio_stubs.c
 *
 * Provides STRONG global definitions of fprintf and sscanf.
 *
 * PROBLEM: MSVC-compiled objects in libcrypto.lib (built with /MD, UCRT) emit
 * fprintf/sscanf as COMDAT(SELECTANY) inline wrappers that call
 * __stdio_common_vfprintf/__stdio_common_vsscanf.  GNU ld discards duplicate
 * COMDAT sections across objects, leaving some call sites with unresolved
 * "U fprintf"/"U sscanf" references.  Additionally, libmingw32.a's merr.o
 * references "U fprintf" directly as an external symbol.
 *
 * SOLUTION: Define strong (non-COMDAT) T fprintf and T sscanf in a regular
 * object file.  Object files are always linked before archives, so:
 *   1. These strong definitions are established first.
 *   2. All COMDAT copies in libcrypto.lib are discarded (strong wins).
 *   3. libmsvcrt.a's import-thunk fprintf is never extracted (already defined).
 *   4. merr.o's "U fprintf" is satisfied by our definition.
 *
 * We route calls through MinGW's __mingw_vfprintf/__mingw_vsscanf
 * (from libmingwex.a, auto-linked), which are the real implementations.
 *
 * IMPORTANT: Do NOT #include <stdio.h> in this file.  MinGW's stdio.h
 * defines fprintf as an inline function, which would conflict with our
 * definition here.  We declare only what we need from <stdarg.h>.
 */

#include <stdarg.h>

/* Opaque FILE - avoid pulling in <stdio.h> */
struct _iobuf;
typedef struct _iobuf FILE;

/* MinGW's real implementations, provided by libmingwex.a (auto-linked) */
extern int __cdecl __mingw_vfprintf(FILE *f, const char *fmt, va_list ap);
extern int __cdecl __mingw_vsscanf(const char *str, const char *fmt, va_list ap);

int __cdecl fprintf(FILE *f, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = __mingw_vfprintf(f, fmt, ap);
    va_end(ap);
    return r;
}

int __cdecl sscanf(const char *str, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = __mingw_vsscanf(str, fmt, ap);
    va_end(ap);
    return r;
}
