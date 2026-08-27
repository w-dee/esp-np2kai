#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#include "np2opngen_fixture.h"

static uint64_t host_clock(void *context)
{
    struct timespec now;
    (void)context;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0;
    }
    return (uint64_t)now.tv_sec * UINT64_C(1000000) +
           (uint64_t)now.tv_nsec / UINT64_C(1000);
}

static void put_le16(uint8_t *out, uint16_t value)
{
    out[0] = (uint8_t)(value & 0xffU);
    out[1] = (uint8_t)((value >> 8) & 0xffU);
}

static void put_le32(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)(value & 0xffU);
    out[1] = (uint8_t)((value >> 8) & 0xffU);
    out[2] = (uint8_t)((value >> 16) & 0xffU);
    out[3] = (uint8_t)((value >> 24) & 0xffU);
}

static int write_bytes(FILE *file, const uint8_t *bytes, size_t byte_count)
{
    return fwrite(bytes, 1U, byte_count, file) == byte_count ? 0 : -1;
}

static int write_wav_sink(const uint8_t *canonical_pcm, size_t pcm_bytes,
                          uint32_t sample_rate_hz, uint16_t channels,
                          uint16_t bits_per_sample, void *context)
{
    const char *path = (const char *)context;
    const uint32_t block_align =
        (uint32_t)channels * (uint32_t)(bits_per_sample / 8U);
    const uint32_t byte_rate = sample_rate_hz * block_align;
    uint8_t header[44] = {0};
    FILE *file;
    int status = -1;

    if (canonical_pcm == 0 || path == 0 || path[0] == '\0' ||
        sample_rate_hz != 48000U || channels != 2U || bits_per_sample != 16U ||
        pcm_bytes != 115200U || block_align != 4U || byte_rate != 192000U ||
        pcm_bytes > UINT32_MAX - 36U) {
        return -1;
    }
    memcpy(header, "RIFF", 4U);
    put_le32(header + 4U, 36U + (uint32_t)pcm_bytes);
    memcpy(header + 8U, "WAVEfmt ", 8U);
    put_le32(header + 16U, 16U);
    put_le16(header + 20U, 1U);
    put_le16(header + 22U, channels);
    put_le32(header + 24U, sample_rate_hz);
    put_le32(header + 28U, byte_rate);
    put_le16(header + 32U, (uint16_t)block_align);
    put_le16(header + 34U, bits_per_sample);
    memcpy(header + 36U, "data", 4U);
    put_le32(header + 40U, (uint32_t)pcm_bytes);

    file = fopen(path, "wb");
    if (file == 0) {
        return -1;
    }
    if (write_bytes(file, header, sizeof(header)) == 0 &&
        write_bytes(file, canonical_pcm, pcm_bytes) == 0 &&
        fflush(file) == 0) {
        status = 0;
    }
    if (fclose(file) != 0) {
        status = -1;
    }
    return status;
}

static void print_usage(const char *program)
{
    fprintf(stderr, "Usage: %s [--wav PATH]\n", program);
}

int main(int argc, char **argv)
{
    const char *wav_path = 0;

    if (argc == 3 && strcmp(argv[1], "--wav") == 0 && argv[2][0] != '\0') {
        wav_path = argv[2];
    } else if (argc != 1) {
        print_usage(argv[0]);
        return 2;
    }
    if (wav_path == 0) {
        return np2opngen_fixture_run(host_clock, 0) == 0 ? 0 : 1;
    }
    return np2opngen_fixture_run_with_sink(host_clock, 0, write_wav_sink,
                                           (void *)wav_path) == 0
               ? 0
               : 1;
}
