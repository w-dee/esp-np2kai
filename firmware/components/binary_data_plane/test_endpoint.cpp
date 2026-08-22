#include "binary_data_plane/binary_data_plane.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace binary_data_plane {
namespace {

constexpr std::string_view kPatternPrefix = "@ESP-NP2 ";
constexpr std::string_view kPatternLog = "I (123) ESP-IDF log text\n";

struct PatternContext {
    Direction direction = Direction::HostToDevice;
    std::uint64_t size = 0;
    std::uint64_t consumed = 0;
    std::uint64_t produced = 0;
};
PatternContext s_pattern_context{};

std::uint8_t pattern_value(std::uint64_t offset)
{
    const std::size_t block = static_cast<std::size_t>(offset % 1024);
    if (block >= 256 && block < 256 + kPatternPrefix.size()) {
        return static_cast<std::uint8_t>(kPatternPrefix[block - 256]);
    }
    if (block >= 320 && block < 320 + kPatternLog.size()) {
        return static_cast<std::uint8_t>(kPatternLog[block - 320]);
    }
    if (block == 384) return 0x00;
    if (block == 385) return 0x0a;
    if (block == 386) return 0x0d;
    if (block == 387) return 0xff;
    if (block == 388) return 0x00;
    return static_cast<std::uint8_t>(block & 0xff);
}

EndpointResult begin(void *context, Direction direction, std::uint64_t size)
{
    auto *pattern = static_cast<PatternContext *>(context);
    if (pattern == nullptr || size == 0) return EndpointResult::Failed;
    pattern->direction = direction;
    pattern->size = size;
    pattern->consumed = 0;
    pattern->produced = 0;
    return EndpointResult::Ok;
}

EndpointResult consume(void *context, std::uint64_t offset, const std::uint8_t *data,
                       std::size_t length)
{
    auto *pattern = static_cast<PatternContext *>(context);
    if (pattern == nullptr || data == nullptr || length == 0 ||
        offset != pattern->consumed ||
        length > pattern->size - pattern->consumed) {
        return EndpointResult::Failed;
    }
    for (std::size_t i = 0; i < length; ++i) {
        if (data[i] != pattern_value(offset + i)) return EndpointResult::Failed;
    }
    pattern->consumed += length;
    return EndpointResult::Ok;
}

EndpointResult produce(void *context, std::uint64_t offset, std::uint8_t *out,
                       std::size_t requested, std::size_t *produced)
{
    auto *pattern = static_cast<PatternContext *>(context);
    if (pattern == nullptr || out == nullptr || produced == nullptr || requested == 0 ||
        requested > kMaxPayloadBytes || offset != pattern->produced ||
        requested > pattern->size - pattern->produced) {
        return EndpointResult::Failed;
    }
    for (std::size_t i = 0; i < requested; ++i) out[i] = pattern_value(offset + i);
    *produced = requested;
    pattern->produced += requested;
    return EndpointResult::Ok;
}

EndpointResult finish(void *context)
{
    auto *pattern = static_cast<PatternContext *>(context);
    if (pattern == nullptr) return EndpointResult::Failed;
    return (pattern->direction == Direction::HostToDevice ?
            pattern->consumed : pattern->produced) == pattern->size ?
        EndpointResult::Ok : EndpointResult::Failed;
}
void abort_endpoint(void *, TerminalReason) {}
void terminal(void *, TerminalReason) {}

TransferEndpoint endpoint()
{
    return TransferEndpoint{
        &s_pattern_context,
        begin,
        consume,
        produce,
        finish,
        abort_endpoint,
        terminal,
    };
}

} // namespace

ManagerError begin_test_rx(TransferManager *manager, std::uint64_t size,
                           TransferInfo *info)
{
    return begin_rx(manager, size, endpoint(), info);
}

ManagerError begin_test_tx(TransferManager *manager, std::uint64_t size,
                           TransferInfo *info)
{
    return begin_tx(manager, size, endpoint(), info);
}

std::uint8_t test_pattern_byte(std::uint64_t offset)
{
    return pattern_value(offset);
}

} // namespace binary_data_plane
