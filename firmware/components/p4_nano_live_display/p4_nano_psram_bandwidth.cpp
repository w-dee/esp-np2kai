/*
 * SPDX-FileCopyrightText: 2026 esp-np2kai contributors
 * SPDX-License-Identifier: MIT
 */

#include "p4_nano_live_display/p4_nano_psram_bandwidth.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace {

constexpr std::size_t kBufferBytes = 4U * 1024U * 1024U;
constexpr std::size_t kProxySourceBytes = 512'000U;
constexpr std::size_t kProxyDestinationBytes = 2'048'000U;
constexpr std::size_t kRowCount = 400U;
constexpr std::size_t kRowBytes = 1'280U;

__attribute__((noinline)) std::uint32_t sequential_read(
    const std::uint8_t *source) noexcept
{
    const volatile std::uint32_t *input =
        reinterpret_cast<const volatile std::uint32_t *>(source);
    std::uint32_t a0 = 0x13579bdfU;
    std::uint32_t a1 = 0x2468ace0U;
    std::uint32_t a2 = 0x9e3779b9U;
    std::uint32_t a3 = 0x7f4a7c15U;
    std::uint32_t a4 = 0x6a09e667U;
    std::uint32_t a5 = 0xbb67ae85U;
    std::uint32_t a6 = 0x3c6ef372U;
    std::uint32_t a7 = 0xa54ff53aU;
    for (std::size_t index = 0; index < kBufferBytes / sizeof(std::uint32_t);
         index += 8U) {
        a0 ^= input[index + 0U];
        a1 ^= input[index + 1U];
        a2 ^= input[index + 2U];
        a3 ^= input[index + 3U];
        a4 ^= input[index + 4U];
        a5 ^= input[index + 5U];
        a6 ^= input[index + 6U];
        a7 ^= input[index + 7U];
    }
    return a0 ^ a1 ^ a2 ^ a3 ^ a4 ^ a5 ^ a6 ^ a7;
}

__attribute__((noinline)) void sequential_write16(
    std::uint8_t *destination, std::uint32_t pattern) noexcept
{
    volatile std::uint16_t *output =
        reinterpret_cast<volatile std::uint16_t *>(destination);
    for (std::size_t index = 0; index < kBufferBytes / sizeof(std::uint16_t);
         ++index) {
        output[index] = static_cast<std::uint16_t>(
            pattern ^ static_cast<std::uint32_t>(index));
    }
}

__attribute__((noinline)) void sequential_write32(
    std::uint8_t *destination, std::uint32_t pattern) noexcept
{
    volatile std::uint32_t *output =
        reinterpret_cast<volatile std::uint32_t *>(destination);
    for (std::size_t index = 0; index < kBufferBytes / sizeof(std::uint32_t);
         ++index) {
        const std::uint32_t pixel =
            pattern ^ static_cast<std::uint32_t>(index);
        output[index] = pixel | (pixel << 16U);
    }
}

using MemcpyFunction = void *(*)(void *, const void *, std::size_t);

__attribute__((noinline)) void contiguous_memcpy(
    std::uint8_t *source, std::uint8_t *destination) noexcept
{
    volatile MemcpyFunction copy = &std::memcpy;
    (void)copy(destination, source, kBufferBytes);
}

__attribute__((noinline)) void row_memcpy(std::uint8_t *source,
                                          std::uint8_t *destination) noexcept
{
    volatile MemcpyFunction copy = &std::memcpy;
    for (std::size_t row = 0; row < kRowCount; ++row) {
        (void)copy(destination + row * kRowBytes, source + row * kRowBytes,
                   kRowBytes);
    }
}

__attribute__((noinline)) std::uint32_t transform_like_proxy(
    std::uint8_t *source, std::uint8_t *destination) noexcept
{
    const volatile std::uint16_t *input =
        reinterpret_cast<const volatile std::uint16_t *>(source);
    volatile std::uint16_t *output =
        reinterpret_cast<volatile std::uint16_t *>(destination);
    constexpr std::size_t source_pixels = kProxySourceBytes / sizeof(std::uint16_t);
    for (std::size_t index = 0; index < source_pixels; ++index) {
        const std::uint16_t pixel = input[index];
        const std::size_t output_index = index * 4U;
        output[output_index + 0U] = pixel;
        output[output_index + 1U] = pixel;
        output[output_index + 2U] = pixel;
        output[output_index + 3U] = pixel;
    }
    return static_cast<std::uint32_t>(output[source_pixels * 4U - 1U]);
}

} // namespace

namespace p4_nano_psram_bandwidth {

const char *operation_name(Operation operation) noexcept
{
    switch (operation) {
    case Operation::Read:
        return "read";
    case Operation::Write16:
        return "write16";
    case Operation::Write32:
        return "write32";
    case Operation::Memcpy:
        return "memcpy";
    case Operation::RowMemcpy:
        return "row_memcpy";
    case Operation::Proxy:
        return "proxy";
    }
    return "unknown";
}

std::uint32_t run_kernel(Operation operation, std::uint8_t *source,
                         std::uint8_t *destination,
                         std::uint32_t pattern) noexcept
{
    switch (operation) {
    case Operation::Read:
        return sequential_read(source);
    case Operation::Write16:
        sequential_write16(destination, pattern);
        return 0U;
    case Operation::Write32:
        sequential_write32(destination, pattern);
        return 0U;
    case Operation::Memcpy:
        contiguous_memcpy(source, destination);
        return 0U;
    case Operation::RowMemcpy:
        row_memcpy(source, destination);
        return 0U;
    case Operation::Proxy:
        return transform_like_proxy(source, destination);
    }
    return 0U;
}

std::size_t read_bytes(Operation operation) noexcept
{
    switch (operation) {
    case Operation::Read:
        return kBufferBytes;
    case Operation::Write16:
    case Operation::Write32:
        return 0U;
    case Operation::Memcpy:
        return kBufferBytes;
    case Operation::RowMemcpy:
        return kRowCount * kRowBytes;
    case Operation::Proxy:
        return kProxySourceBytes;
    }
    return 0U;
}

std::size_t write_bytes(Operation operation) noexcept
{
    switch (operation) {
    case Operation::Read:
        return 0U;
    case Operation::Write16:
    case Operation::Write32:
    case Operation::Memcpy:
        return kBufferBytes;
    case Operation::RowMemcpy:
        return kRowCount * kRowBytes;
    case Operation::Proxy:
        return kProxyDestinationBytes;
    }
    return 0U;
}

} // namespace p4_nano_psram_bandwidth
