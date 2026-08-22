#include "file_transfer/zero_rle_v1.hpp"

#include <algorithm>

namespace file_transfer::zero_rle_v1 {
namespace {

constexpr std::uint8_t kZeroRun = 0x00;
constexpr std::uint8_t kLiteral = 0x01;

} // namespace

void Decoder::init(std::uint64_t logical_size, std::uint64_t wire_size,
                   Emit emit, void *emit_context,
                   const std::uint8_t *zero_buffer, std::size_t zero_buffer_bytes)
{
    *this = Decoder{};
    logical_size_ = logical_size;
    wire_size_ = wire_size;
    emit_ = emit;
    emit_context_ = emit_context;
    zero_buffer_ = zero_buffer;
    zero_buffer_bytes_ = zero_buffer_bytes;
}

Result Decoder::fail(Error error)
{
    state_ = State::Failed;
    error_ = error;
    return Result::Malformed;
}

Result Decoder::emit(const std::uint8_t *data, std::size_t bytes)
{
    if (bytes == 0 || emit_ == nullptr || logical_produced_ > logical_size_ ||
        static_cast<std::uint64_t>(bytes) > logical_size_ - logical_produced_) {
        return fail(Error::LogicalOverrun);
    }
    if (!emit_(emit_context_, logical_produced_, data, bytes)) {
        state_ = State::Failed;
        error_ = Error::None;
        return Result::OutputFailed;
    }
    logical_produced_ += bytes;
    return Result::Ok;
}

Result Decoder::emit_zeroes()
{
    if (zero_buffer_ == nullptr || zero_buffer_bytes_ == 0) {
        state_ = State::Failed;
        error_ = Error::None;
        return Result::OutputFailed;
    }
    while (record_remaining_ != 0) {
        const std::size_t count = static_cast<std::size_t>(std::min<std::uint64_t>(
            record_remaining_, zero_buffer_bytes_));
        const Result result = emit(zero_buffer_, count);
        if (result != Result::Ok) return result;
        record_remaining_ -= count;
    }
    state_ = State::ExpectTag;
    return Result::Ok;
}

Result Decoder::consume(std::uint64_t wire_offset, const std::uint8_t *encoded,
                        std::size_t encoded_bytes)
{
    if (state_ == State::Failed) {
        return error_ == Error::None ? Result::OutputFailed : Result::Malformed;
    }
    if (wire_offset != wire_consumed_) return fail(Error::OffsetMismatch);
    if (encoded_bytes != 0 && encoded == nullptr) return fail(Error::WireOverrun);
    if (wire_consumed_ > wire_size_ ||
        static_cast<std::uint64_t>(encoded_bytes) > wire_size_ - wire_consumed_) {
        return fail(Error::WireOverrun);
    }

    std::size_t index = 0;
    while (index < encoded_bytes) {
        if (state_ == State::EmitZero) {
            const Result result = emit_zeroes();
            if (result != Result::Ok) return result;
            continue;
        }
        if (state_ == State::ExpectTag) {
            tag_ = encoded[index++];
            ++wire_consumed_;
            if (tag_ != kZeroRun && tag_ != kLiteral) {
                return fail(Error::UnknownTag);
            }
            record_length_ = 0;
            length_bytes_ = 0;
            state_ = State::ReadLength;
            continue;
        }
        if (state_ == State::ReadLength) {
            record_length_ |= static_cast<std::uint32_t>(encoded[index++]) <<
                (length_bytes_ * 8);
            ++wire_consumed_;
            ++length_bytes_;
            if (length_bytes_ != 4) continue;
            if (record_length_ == 0) return fail(Error::ZeroLength);
            if (logical_produced_ > logical_size_ ||
                static_cast<std::uint64_t>(record_length_) > logical_size_ - logical_produced_) {
                return fail(Error::LogicalOverrun);
            }
            record_remaining_ = record_length_;
            state_ = tag_ == kZeroRun ? State::EmitZero : State::ReadLiteral;
            continue;
        }
        if (state_ == State::ReadLiteral) {
            const std::size_t count = static_cast<std::size_t>(std::min<std::uint64_t>(
                record_remaining_, encoded_bytes - index));
            const Result result = emit(encoded + index, count);
            if (result != Result::Ok) return result;
            index += count;
            wire_consumed_ += count;
            record_remaining_ -= count;
            if (record_remaining_ == 0) state_ = State::ExpectTag;
            continue;
        }
        return error_ == Error::None ? Result::OutputFailed : Result::Malformed;
    }
    if (state_ == State::EmitZero) return emit_zeroes();
    return Result::Ok;
}

Result Decoder::finish()
{
    if (state_ == State::Failed) {
        return error_ == Error::None ? Result::OutputFailed : Result::Malformed;
    }
    if (state_ == State::EmitZero) {
        const Result result = emit_zeroes();
        if (result != Result::Ok) return result;
    }
    if (wire_consumed_ != wire_size_) return fail(Error::Truncated);
    if (state_ != State::ExpectTag) return fail(Error::Truncated);
    if (logical_produced_ != logical_size_) return fail(Error::TrailingBytes);
    return Result::Ok;
}

} // namespace file_transfer::zero_rle_v1
