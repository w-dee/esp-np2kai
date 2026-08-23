#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef uint8_t UINT8;
typedef uint16_t UINT16;
typedef uint32_t UINT32;
typedef UINT8 REG8;
typedef UINT16 REG16;

#define INLINE inline
#define I286_MEMREADMAX 0xa4000U
#define I286_MEMWRITEMAX 0xa0000U
#define CONTRACT_MEM_BYTES (I286_MEMREADMAX + 2U)
#define LOADINTELWORD(address) \
    ((UINT16)((UINT8 *)(address))[0] | \
     ((UINT16)((UINT8 *)(address))[1] << 8))
#define STOREINTELWORD(address, value) \
    (((UINT8 *)(address))[0] = (UINT8)(value), \
     ((UINT8 *)(address))[1] = (UINT8)((value) >> 8))

UINT8 mem[CONTRACT_MEM_BYTES];

static unsigned read8_fallback_calls;
static unsigned read16_fallback_calls;
static unsigned write8_fallback_calls;
static unsigned write16_fallback_calls;
static unsigned address_evaluations;
static unsigned value_evaluations;
static unsigned tramupdate_calls;
static unsigned mapped_side_effect_calls;
static unsigned high_mask_fallback_calls;

static void fail(const char *message) {
    fprintf(stderr, "FAIL mode=%d: %s\n", NP2_I286C_INLINE_MEM_FASTPATH, message);
    exit(EXIT_FAILURE);
}

#define CHECK(condition, message) \
    do { \
        if (!(condition)) { \
            fail(message); \
        } \
    } while (0)

REG8 memp_read8(UINT32 address) {
    ++read8_fallback_calls;
    if (address < sizeof(mem)) {
        return mem[address];
    }
    ++high_mask_fallback_calls;
    return 0xe1U;
}

REG16 memp_read16(UINT32 address) {
    ++read16_fallback_calls;
    if (address + 1U >= sizeof(mem)) {
        ++high_mask_fallback_calls;
        return 0xe2U;
    }
    return (REG16)(mem[address] | ((REG16)mem[address + 1] << 8));
}

static void tramupdate(UINT32 address) {
    (void)address;
    ++tramupdate_calls;
}

static void mapped_memory_side_effect(UINT32 address) {
    (void)address;
    ++mapped_side_effect_calls;
}

void memp_write8(UINT32 address, REG8 value) {
    ++write8_fallback_calls;
    if (address >= I286_MEMWRITEMAX && address < I286_MEMREADMAX) {
        tramupdate(address);
    }
    if (address >= 0x200U && address < 0x100000U) {
        mapped_memory_side_effect(address);
    }
    if (address < sizeof(mem)) {
        mem[address] = value;
    } else {
        ++high_mask_fallback_calls;
    }
}

void memp_write16(UINT32 address, REG16 value) {
    ++write16_fallback_calls;
    if (address >= I286_MEMWRITEMAX - 1U && address < I286_MEMREADMAX) {
        tramupdate(address);
    }
    if (address >= 0x200U && address < 0x100000U) {
        mapped_memory_side_effect(address);
    }
    if (address + 1U < sizeof(mem)) {
        mem[address] = (UINT8)value;
        mem[address + 1] = (UINT8)(value >> 8);
    } else {
        ++high_mask_fallback_calls;
    }
}

static UINT32 address_once(UINT32 address) {
    ++address_evaluations;
    return address;
}

static REG8 value_once(REG8 value) {
    ++value_evaluations;
    return value;
}

#include "np2_i286_inline_mem_fastpath.h"

static void reset_observations(void) {
    read8_fallback_calls = 0;
    read16_fallback_calls = 0;
    write8_fallback_calls = 0;
    write16_fallback_calls = 0;
    address_evaluations = 0;
    value_evaluations = 0;
    tramupdate_calls = 0;
    mapped_side_effect_calls = 0;
    high_mask_fallback_calls = 0;
}

int main(void) {
    unsigned expected_low_fallback = NP2_I286C_INLINE_MEM_FASTPATH ? 0U : 1U;

    for (unsigned index = 0; index < sizeof(mem); ++index) {
        mem[index] = (UINT8)(index ^ 0x5aU);
    }

    reset_observations();
    CHECK(NP2_I286_MEMORYREAD8(address_once(I286_MEMREADMAX - 1U)) ==
              mem[I286_MEMREADMAX - 1U], "read8 low boundary value");
    CHECK(address_evaluations == 1U, "read8 low address evaluated once");
    CHECK(read8_fallback_calls == expected_low_fallback,
          "read8 low boundary fallback selection");

    reset_observations();
    CHECK(NP2_I286_MEMORYREAD8(address_once(I286_MEMREADMAX)) ==
              mem[I286_MEMREADMAX], "read8 high boundary value");
    CHECK(address_evaluations == 1U, "read8 high address evaluated once");
    CHECK(read8_fallback_calls == 1U, "read8 high boundary fallback");

    reset_observations();
    CHECK(NP2_I286_MEMORYREAD8(address_once(0x100000U)) == 0xe1U,
          "high masked read8 fallback value");
    CHECK(address_evaluations == 1U, "high masked read8 address evaluated once");
    CHECK(read8_fallback_calls == 1U, "high masked read8 fallback");
    CHECK(high_mask_fallback_calls == 1U, "high masked read8 side effect");

    reset_observations();
    CHECK(NP2_I286_MEMORYREAD16(address_once(I286_MEMREADMAX - 2U)) ==
              (REG16)(mem[I286_MEMREADMAX - 2U] |
                      ((REG16)mem[I286_MEMREADMAX - 1U] << 8)),
          "read16 low boundary value");
    CHECK(address_evaluations == 1U, "read16 low address evaluated once");
    CHECK(read16_fallback_calls == expected_low_fallback,
          "read16 low boundary fallback selection");

    reset_observations();
    CHECK(NP2_I286_MEMORYREAD16(address_once(I286_MEMREADMAX - 1U)) ==
              (REG16)(mem[I286_MEMREADMAX - 1U] |
                      ((REG16)mem[I286_MEMREADMAX] << 8)),
          "read16 high boundary value");
    CHECK(address_evaluations == 1U, "read16 high address evaluated once");
    CHECK(read16_fallback_calls == 1U, "read16 high boundary fallback");

    mem[I286_MEMWRITEMAX - 1U] = 0;
    reset_observations();
    NP2_I286_MEMORYWRITE8(address_once(I286_MEMWRITEMAX - 1U),
                          value_once((REG8)0xa5U));
    CHECK(mem[I286_MEMWRITEMAX - 1U] == 0xa5U, "write8 low boundary value");
    CHECK(address_evaluations == 1U, "write8 low address evaluated once");
    CHECK(value_evaluations == 1U, "write8 low value evaluated once");
    CHECK(write8_fallback_calls == expected_low_fallback,
          "write8 low boundary fallback selection");
    CHECK(tramupdate_calls == 0U,
          "write8 low boundary keeps text side effect dormant");

    mem[I286_MEMWRITEMAX] = 0;
    reset_observations();
    NP2_I286_MEMORYWRITE8(address_once(I286_MEMWRITEMAX),
                          value_once((REG8)0x3cU));
    CHECK(mem[I286_MEMWRITEMAX] == 0x3cU, "write8 high boundary value");
    CHECK(address_evaluations == 1U, "write8 high address evaluated once");
    CHECK(value_evaluations == 1U, "write8 high value evaluated once");
    CHECK(write8_fallback_calls == 1U, "write8 high boundary fallback");
    CHECK(tramupdate_calls == 1U, "write8 text fallback preserves side effect");

    mem[I286_MEMWRITEMAX - 2U] = 0;
    mem[I286_MEMWRITEMAX - 1U] = 0;
    reset_observations();
    NP2_I286_MEMORYWRITE16(address_once(I286_MEMWRITEMAX - 2U),
                           (REG16)value_once((REG8)0x7bU));
    CHECK(LOADINTELWORD(mem + I286_MEMWRITEMAX - 2U) == 0x007bU,
          "write16 low boundary value");
    CHECK(address_evaluations == 1U, "write16 low address evaluated once");
    CHECK(value_evaluations == 1U, "write16 low value evaluated once");
    CHECK(write16_fallback_calls == expected_low_fallback,
          "write16 low boundary fallback selection");
    CHECK(tramupdate_calls == 0U,
          "write16 low boundary keeps text side effect dormant");

    mem[I286_MEMWRITEMAX - 1U] = 0;
    mem[I286_MEMWRITEMAX] = 0;
    reset_observations();
    NP2_I286_MEMORYWRITE16(address_once(I286_MEMWRITEMAX - 1U),
                           (REG16)value_once((REG8)0x6dU));
    CHECK(LOADINTELWORD(mem + I286_MEMWRITEMAX - 1U) == 0x006dU,
          "write16 high boundary value");
    CHECK(address_evaluations == 1U, "write16 high address evaluated once");
    CHECK(value_evaluations == 1U, "write16 high value evaluated once");
    CHECK(write16_fallback_calls == 1U, "write16 high boundary fallback");
    CHECK(tramupdate_calls == 1U, "write16 text fallback preserves side effect");

    reset_observations();
    NP2_I286_MEMORYWRITE8(address_once(0xa5000U), value_once((REG8)0x42U));
    CHECK(address_evaluations == 1U, "mapped write address evaluated once");
    CHECK(value_evaluations == 1U, "mapped write value evaluated once");
    CHECK(write8_fallback_calls == 1U, "mapped write fallback");
    CHECK(mapped_side_effect_calls == 1U, "mapped write side effect");

    printf("I286_INLINE_MEM_FASTPATH_CONTRACT_PASS mode=%d\n",
           NP2_I286C_INLINE_MEM_FASTPATH);
    return EXIT_SUCCESS;
}
