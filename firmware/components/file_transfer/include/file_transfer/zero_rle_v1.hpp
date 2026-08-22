#pragma once

#include <cstddef>
#include <cstdint>

namespace file_transfer::zero_rle_v1 {

enum class Result : std::uint8_t {
    Ok,
    Malformed,
    OutputFailed,
};

enum class Error : std::uint8_t {
    None,
    OffsetMismatch,
    WireOverrun,
    UnknownTag,
    ZeroLength,
    LogicalOverrun,
    Truncated,
    TrailingBytes,
};

using Emit = bool (*)(void *, std::uint64_t, const std::uint8_t *, std::size_t);

class Decoder {
public:
    void init(std::uint64_t logical_size, std::uint64_t wire_size,
              Emit emit, void *emit_context,
              const std::uint8_t *zero_buffer, std::size_t zero_buffer_bytes);
    Result consume(std::uint64_t wire_offset, const std::uint8_t *encoded,
                   std::size_t encoded_bytes);
    Result finish();

    std::uint64_t logical_produced() const { return logical_produced_; }
    std::uint64_t wire_consumed() const { return wire_consumed_; }
    Error error() const { return error_; }

private:
    enum class State : std::uint8_t {
        ExpectTag,
        ReadLength,
        EmitZero,
        ReadLiteral,
        Failed,
    };

    Result fail(Error error);
    Result emit_zeroes();
    Result emit(const std::uint8_t *data, std::size_t bytes);

    std::uint64_t logical_size_ = 0;
    std::uint64_t wire_size_ = 0;
    std::uint64_t logical_produced_ = 0;
    std::uint64_t wire_consumed_ = 0;
    std::uint64_t record_remaining_ = 0;
    std::uint32_t record_length_ = 0;
    std::uint8_t length_bytes_ = 0;
    std::uint8_t tag_ = 0;
    State state_ = State::ExpectTag;
    Error error_ = Error::None;
    Emit emit_ = nullptr;
    void *emit_context_ = nullptr;
    const std::uint8_t *zero_buffer_ = nullptr;
    std::size_t zero_buffer_bytes_ = 0;
};

} // namespace file_transfer::zero_rle_v1
