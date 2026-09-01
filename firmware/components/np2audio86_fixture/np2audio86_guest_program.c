#include "np2audio86_guest_program.h"

static int put8(uint8_t *out, size_t capacity, size_t *at, uint8_t value)
{
    if (*at >= capacity) return -1;
    out[(*at)++] = value;
    return 0;
}

static int put16(uint8_t *out, size_t capacity, size_t *at, uint16_t value)
{
    return put8(out, capacity, at, (uint8_t)value) ||
           put8(out, capacity, at, (uint8_t)(value >> 8));
}

static int mov_dx_out(uint8_t *out, size_t capacity, size_t *at,
                      uint16_t port, uint8_t value)
{
    return put8(out, capacity, at, 0xbau) || put16(out, capacity, at, port) ||
           put8(out, capacity, at, 0xb0u) || put8(out, capacity, at, value) ||
           put8(out, capacity, at, 0xeeu);
}

static int mov_dx_in_store(uint8_t *out, size_t capacity, size_t *at,
                           uint16_t port, uint16_t address)
{
    return put8(out, capacity, at, 0xbau) || put16(out, capacity, at, port) ||
           put8(out, capacity, at, 0xecu) || put8(out, capacity, at, 0xa2u) ||
           put16(out, capacity, at, address);
}

size_t np2audio86_guest_program_build(uint8_t *out, size_t capacity)
{
    size_t at = 0U;
    unsigned i;
    if (out == NULL) return 0U;
#define OUT(port, value) do { if (mov_dx_out(out, capacity, &at, port, value)) return 0U; } while (0)
#define IN(port, address) do { if (mov_dx_in_store(out, capacity, &at, port, address)) return 0U; } while (0)
    OUT(0x188, 0x24); OUT(0x18a, 0xff);
    OUT(0x188, 0x0f); OUT(0x18a, 0x55);
    OUT(0x188, 0x25); OUT(0x18a, 0x03);
    OUT(0x188, 0x27); OUT(0x18a, 0x05);
    OUT(0x188, 0x0f); IN(0x18a, 0x8000);
    OUT(0x188, 0x26); OUT(0x18a, 0xff);
    OUT(0x188, 0x27); OUT(0x18a, 0x0f);
    OUT(0x188, 0xa0); OUT(0x18a, 0x34);
    OUT(0x188, 0x28); OUT(0x18a, 0xf0);
    OUT(0x188, 0x10); OUT(0x18a, 0x7f);
    OUT(0x18c, 0x0f); IN(0x18c, 0x8009);
    OUT(0x18c, 0x0f); IN(0x18e, 0x800a);
    OUT(0xa460, 0x01);
    OUT(0x18c, 0x0f); OUT(0x18e, 0x5a);
    OUT(0x18c, 0x0f); IN(0x18e, 0x800b);
    OUT(0xa460, 0x00);
    OUT(0x18c, 0x0f); OUT(0x18e, 0xa5);
    OUT(0x18c, 0x0f); IN(0x18e, 0x800c);
    IN(0x18c, 0x800d);
    OUT(0xa460, 0x01);
    OUT(0xa46a, 0x00); OUT(0xa468, 0x11); OUT(0xa466, 0xa5);
    OUT(0xa46c, 0x10); OUT(0xa46c, 0x20); OUT(0xa46c, 0x30); OUT(0xa46c, 0x40);
    OUT(0xa46c, 0x50); OUT(0xa46c, 0x60); OUT(0xa46c, 0x70); OUT(0xa46c, 0x80);
    IN(0xa462, 0x8001); IN(0xa464, 0x8002); IN(0xa466, 0x8003);
    IN(0xa468, 0x8004); IN(0xa46a, 0x8005); IN(0xa46c, 0x8006); IN(0xa46e, 0x8007);
    for (i = 0U; i < 4500U; ++i) if (put8(out, capacity, &at, 0x90u)) return 0U;
    IN(0x188, 0x800e); OUT(0x188, 0x27); OUT(0x18a, 0x30); IN(0x188, 0x800f);
    for (i = 0U; i < 100U; ++i) if (put8(out, capacity, &at, 0x90u)) return 0U;
    IN(0x188, 0x8010);
    if (put8(out, capacity, &at, 0xf4u)) return 0U;
#undef OUT
#undef IN
    return at;
}
