/* Ubuntu/headless syntax-probe contract for the retained portable sources. */
#ifndef NP2_HOST_COMPILER_H
#define NP2_HOST_COMPILER_H

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* Scalar names used by the current retained source list. */
typedef int INT;
typedef unsigned int UINT;
typedef int8_t SINT8;
typedef uint8_t UINT8;
typedef int16_t SINT16;
typedef uint16_t UINT16;
typedef int32_t SINT32;
typedef uint32_t UINT32;
typedef int64_t SINT64;
typedef uint64_t UINT64;
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

/* The retained tree uses single-byte OEM strings on this host. */
#define OEMCHAR char
#define OEMTEXT(string) string
#define OEMSTRLEN strlen
#define OEMSNPRINTF snprintf
#define OEMSPRINTF sprintf
#define _T(string) string
#define _tcsicmp strcasecmp
#define _tcsnicmp strncasecmp

/* These entry points are ordinary C functions on the non-i386 probe host. */
#define CPUCALL
#define MEMCALL
#define DMACCALL
#define IOOUTCALL
#define IOINPCALL
#define SOUNDCALL
#define VRAMCALL
#define SCRNCALL
#define PARTSCALL

/* Release-like lineage behavior: tracing and assertions are inactive. */
#define TRACEOUT(arguments) ((void)0)
#define __ASSERT(condition) ((void)0)

/* The host timing implementation is intentionally deferred beyond syntax probing. */
UINT32 np2_host_gettick(void);
#define GETTICK() np2_host_gettick()

#define ZeroMemory(destination, size) memset((destination), 0, (size))
#define CopyMemory(destination, source, size) \
    memcpy((destination), (source), (size))
#define FillMemory(destination, size, value) \
    memset((destination), (value), (size))

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

/* Existing vendor helpers supply endian operations, REG aliases, BRESULT,
 * NELEMENTS, and the memory-allocation macros used by retained sources. */
#include <common.h>
#include <common/_memory.h>

#endif /* NP2_HOST_COMPILER_H */
