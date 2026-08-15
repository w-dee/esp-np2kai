/*
 * Project-owned Ubuntu/headless compiler contract for the portable probe.
 *
 * This header deliberately does not select NP2_WIN, USE_SDL, or
 * __LIBRETRO__, and it does not include compiler_base.h or a frontend
 * compiler.h.  Definitions below are limited to the scalar/compiler surface
 * directly consumed by the retained portable sources.
 */
#ifndef NP2_HOST_COMPILER_H
#define NP2_HOST_COMPILER_H

#include <assert.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <wchar.h>

typedef int INT;
typedef INT SINT;
typedef unsigned int UINT;

typedef int8_t INT8;
typedef INT8 SINT8;
typedef uint8_t UINT8;
typedef int16_t INT16;
typedef INT16 SINT16;
typedef uint16_t UINT16;
typedef int32_t INT32;
typedef INT32 SINT32;
typedef uint32_t UINT32;
typedef int64_t INT64;
typedef INT64 SINT64;
typedef uint64_t UINT64;

typedef size_t SIZET;
typedef intptr_t INTPTR;
typedef uintptr_t UINTPTR;
typedef intptr_t INT_PTR;
typedef uintptr_t UINT_PTR;
typedef intmax_t INTMAX;
typedef uintmax_t UINTMAX;

typedef bool BOOL;
#ifndef TRUE
#define TRUE ((BOOL)1)
#endif
#ifndef FALSE
#define FALSE ((BOOL)0)
#endif

typedef uint8_t BYTE;
typedef uint16_t WORD;
typedef uint32_t DWORD;
typedef wchar_t TCHAR;

typedef union {
    struct {
        UINT32 LowPart;
        SINT32 HighPart;
    } u;
    SINT64 QuadPart;
} LARGE_INTEGER;

#define OEMNEWLINE "\n"
#define OEMPATHDIV "/"
#define OEMPATHDIVC '/'
#define OEMSLASH "/"
#define OEMSLASHC '/'
#define OEMCHAR char
#define OEMTEXT(string) string
#define OEMSTRNLEN strnlen
#define OEMSTRNLENS strnlen
#define OEMSTRLEN strlen
#define OEMSNPRINTF snprintf
#define OEMSPRINTF sprintf
#define OEMSTRCPY strcpy
#define OEMPRINTFSTR(s) printf("%s", (s))
#define STRNLEN OEMSTRNLEN
#define STRNLENS OEMSTRNLENS
#define STRLEN OEMSTRLEN
#define SNPRINTF OEMSNPRINTF
#define SPRINTF OEMSPRINTF
#define STRCALL

#define _T(string) string
#define _tcscpy OEMSTRCPY
#define _tcsicmp strcasecmp
#define _tcsnicmp strncasecmp

#define CDECL
#define STDCALL
#define CLRCALL
#define FASTCALL
#define SAFECALL
#define VECTORCALL
#define THISCALL
#define WINAPI

#define CPUCALL FASTCALL
#define MEMCALL FASTCALL
#define DMACCALL FASTCALL
#define IOOUTCALL FASTCALL
#define IOINPCALL FASTCALL
#define SOUNDCALL FASTCALL
#define VRAMCALL FASTCALL
#define SCRNCALL FASTCALL
#define VERMOUTHCL FASTCALL
#define PARTSCALL FASTCALL

#define UNUSED(value) ((void)(value))

/* Retained portable code emits TRACEOUT directly; headless probing has no
 * trace sink, so the contract consumes the argument without evaluating it. */
#define TRACEOUT(arguments) ((void)0)
#define __ASSERT(condition) assert(condition)

/* GETTICK is a host timing contract; implementation is intentionally absent
 * from this syntax-only subphase. */
UINT32 np2_host_gettick(void);
#define GETTICK() np2_host_gettick()

#ifndef ZeroMemory
#define ZeroMemory(destination, size) memset((destination), 0, (size))
#endif
#ifndef CopyMemory
#define CopyMemory(destination, source, size) \
    memcpy((destination), (source), (size))
#endif
#ifndef FillMemory
#define FillMemory(destination, size, value) \
    memset((destination), (value), (size))
#endif

#ifndef MAX_PATH
#define MAX_PATH 4096
#endif
#ifndef MAX
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#endif
#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif
#if defined(SUPPORT_LARGE_HDD)
typedef int64_t FILEPOS;
typedef int64_t FILELEN;
#else
typedef int32_t FILEPOS;
typedef int32_t FILELEN;
#endif

/* Reuse the existing portable endian/load/store and scalar helper contract. */
#include <common.h>
#include <common/_memory.h>

#endif /* NP2_HOST_COMPILER_H */
