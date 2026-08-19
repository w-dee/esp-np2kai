#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "binary_data_plane/binary_data_plane.hpp"
#include "binary_codec.hpp"
#include "control_plane/control_plane.hpp"
#include "control_stream/control_stream.hpp"

namespace {

std::size_t g_ping_count = 0;
std::size_t g_hello_count = 0;
std::size_t g_binary_count = 0;

void reset_observations()
{
    g_ping_count = 0;
    g_hello_count = 0;
    g_binary_count = 0;
}

std::vector<std::uint8_t> bytes(std::string_view text)
{
    return std::vector<std::uint8_t>(text.begin(), text.end());
}

std::vector<std::uint8_t> ping_frame()
{
    return bytes("@ESP-NP2 {\"v\":1,\"id\":1,\"cmd\":\"system.ping\"}\n");
}

std::vector<std::uint8_t> hello_frame()
{
    return bytes("@ESP-NP2 {\"v\":1,\"id\":2,\"cmd\":\"protocol.hello\"}\n");
}

void append(std::vector<std::uint8_t> *target, const std::vector<std::uint8_t> &source)
{
    target->insert(target->end(), source.begin(), source.end());
}

void feed(control_stream::ControlStream *stream, const std::vector<std::uint8_t> &data)
{
    control_stream::feed(stream, data.data(), data.size(), 1234);
}

void feed_chunks(control_stream::ControlStream *stream,
                 const std::vector<std::uint8_t> &data,
                 const std::vector<std::size_t> &chunk_lengths)
{
    std::size_t offset = 0;
    for (const std::size_t chunk_length : chunk_lengths) {
        assert(chunk_length > 0);
        assert(offset + chunk_length <= data.size());
        control_stream::feed(stream,
                             data.data() + offset,
                             chunk_length,
                             1234);
        offset += chunk_length;
    }
    assert(offset == data.size());
}

void expect_one_ping_after_sync(const std::vector<std::uint8_t> &prefix)
{
    control_plane::ControlPlane control{};
    binary_data_plane::TransferManager binary{};
    control_stream::ControlStream stream{};
    control_stream::init(&stream, &control, &binary);

    std::vector<std::uint8_t> input = prefix;
    input.insert(input.end(), control_stream::kTransportSyncLength, 0);
    append(&input, ping_frame());
    feed(&stream, input);

    assert(g_ping_count == 1);
    assert(stream.state == control_stream::State::Text);
    assert(stream.encoded_length == 0);
    assert(control.frame_length == 0);
}

std::vector<std::uint8_t> valid_ack_wire(std::uint32_t sequence)
{
    std::array<std::uint8_t, binary_data_plane::kMaxDecodedFrameBytes> decoded{};
    std::array<std::uint8_t, binary_data_plane::kMaxEncodedBodyBytes> encoded{};
    std::array<std::uint8_t, binary_data_plane::kMaxWireFrameBytes> wire{};
    std::size_t wire_length = 0;
    const bool ok = binary_data_plane::codec::encode_frame(
        binary_data_plane::FrameType::Ack,
        0,
        1,
        sequence,
        0,
        0,
        nullptr,
        0,
        decoded.data(), decoded.size(),
        encoded.data(), encoded.size(),
        wire.data(), wire.size(),
        &wire_length);
    assert(ok);
    assert(wire_length >= 3);
    assert(wire[0] == 0 && wire[1] == 0 && wire[wire_length - 1] == 0);
    assert(std::count(wire.begin() + 2, wire.begin() + wire_length - 1, 0) == 0);
    return std::vector<std::uint8_t>(wire.begin(), wire.begin() + wire_length);
}

void test_detector_boundaries()
{
    control_stream::TransportSyncDetector detector{};
    for (std::size_t index = 0; index < 3; ++index) {
        assert(!detector.feed(0));
        assert(!detector.active());
    }
    assert(detector.feed(0));
    assert(detector.active());
    assert(!detector.feed(0));
    assert(detector.active());
    assert(!detector.feed(0x7f));
    assert(!detector.active());
}

void test_text_recovery_cases()
{
    reset_observations();
    expect_one_ping_after_sync({0xff});

    reset_observations();
    expect_one_ping_after_sync(bytes("arbitrary garbage without a frame"));

    reset_observations();
    expect_one_ping_after_sync(bytes("@ESP-NP"));

    reset_observations();
    control_plane::ControlPlane control{};
    binary_data_plane::TransferManager binary{};
    control_stream::ControlStream stream{};
    control_stream::init(&stream, &control, &binary);
    feed(&stream, {0});
    assert(stream.state == control_stream::State::StartZero);
    std::vector<std::uint8_t> start_zero = {0, 0, 0, 0};
    append(&start_zero, ping_frame());
    feed(&stream, start_zero);
    assert(g_ping_count == 1);

    reset_observations();
    control_stream::init(&stream, &control, &binary);
    feed(&stream, {0, 0, 1, 2, 3});
    assert(stream.state == control_stream::State::BinaryCollect);
    std::vector<std::uint8_t> partial_binary = {0, 0, 0, 0};
    append(&partial_binary, ping_frame());
    feed(&stream, partial_binary);
    assert(g_ping_count == 1);

    reset_observations();
    control_stream::init(&stream, &control, &binary);
    std::vector<std::uint8_t> oversized = {0, 0};
    oversized.insert(oversized.end(),
                     binary_data_plane::kMaxEncodedBodyBytes + 1,
                     0x55);
    feed(&stream, oversized);
    assert(stream.state == control_stream::State::BinaryDiscard);
    append(&oversized, std::vector<std::uint8_t>(control_stream::kTransportSyncLength, 0));
    append(&oversized, ping_frame());
    // Only feed the suffix: the state was deliberately established above.
    const std::size_t suffix_offset = 2 + binary_data_plane::kMaxEncodedBodyBytes + 1;
    std::vector<std::uint8_t> suffix(oversized.begin() + suffix_offset, oversized.end());
    feed(&stream, suffix);
    assert(g_ping_count == 1);
    assert(stream.state == control_stream::State::Text);
}

void test_three_zeros_and_adjacent_binary()
{
    reset_observations();
    control_plane::ControlPlane control{};
    binary_data_plane::TransferManager binary{};
    control_stream::ControlStream stream{};
    control_stream::init(&stream, &control, &binary);
    feed(&stream, {0, 0, 0});
    assert(stream.state == control_stream::State::Text);
    assert(!stream.transport_sync.active());
    assert(g_binary_count == 0);

    // A non-NUL byte ends the three-zero run before the next independent
    // synchronized binary stream begins.
    feed(&stream, {0x7f});
    const std::vector<std::uint8_t> first = valid_ack_wire(0);
    const std::vector<std::uint8_t> second = valid_ack_wire(1);
    std::vector<std::uint8_t> adjacent = first;
    append(&adjacent, second);
    feed(&stream, adjacent);
    assert(g_binary_count == 2);
    assert(stream.state == control_stream::State::Text);
}

void expect_one_ping_after_chunks(const std::vector<std::size_t> &chunk_lengths)
{
    reset_observations();
    control_plane::ControlPlane control{};
    binary_data_plane::TransferManager binary{};
    control_stream::ControlStream stream{};
    control_stream::init(&stream, &control, &binary);

    std::vector<std::uint8_t> input(control_stream::kTransportSyncLength, 0);
    append(&input, ping_frame());
    feed_chunks(&stream, input, chunk_lengths);

    assert(g_ping_count == 1);
    assert(stream.state == control_stream::State::Text);
    assert(stream.encoded_length == 0);
    assert(control.frame_length == 0);
}

void test_sync_chunk_boundaries()
{
    const std::size_t ping_length = ping_frame().size();

    // The token must be recognized when one control_stream::feed() call
    // contains 1+3, 2+2, or 3+1 of its four NUL bytes.
    expect_one_ping_after_chunks({1, 3, ping_length});
    expect_one_ping_after_chunks({2, 2, 1, ping_length - 1});
    expect_one_ping_after_chunks({3, 1, 2, ping_length - 2});

    // A split token followed by a valid protocol frame split at an arbitrary
    // boundary must recover the frame without requiring a single large feed.
    expect_one_ping_after_chunks({1, 3, 5, ping_length - 5});

    // The complete synchronization token and frame must also work one byte at
    // a time, matching the smallest possible UART read chunk.
    std::vector<std::size_t> one_byte_chunks(
        control_stream::kTransportSyncLength + ping_length,
        1);
    expect_one_ping_after_chunks(one_byte_chunks);
}

void test_long_sync_run()
{
    reset_observations();
    control_plane::ControlPlane control{};
    binary_data_plane::TransferManager binary{};
    control_stream::ControlStream stream{};
    control_stream::init(&stream, &control, &binary);
    std::vector<std::uint8_t> input(9, 0);
    append(&input, ping_frame());
    feed(&stream, input);
    assert(g_ping_count == 1);
    assert(stream.state == control_stream::State::Text);

    reset_observations();
    control_stream::init(&stream, &control, &binary);
    std::vector<std::uint8_t> hello = {0xff, 0xfe, 0xfd};
    hello.insert(hello.end(), control_stream::kTransportSyncLength, 0);
    append(&hello, hello_frame());
    feed(&stream, hello);
    assert(g_hello_count == 1);
    assert(stream.state == control_stream::State::Text);
}

void test_randomized_recovery()
{
    std::mt19937 generator(0x504e3253u);
    std::uniform_int_distribution<int> length_distribution(0, 96);
    std::uniform_int_distribution<int> byte_distribution(1, 255);
    std::uniform_int_distribution<int> prefix_kind_distribution(0, 4);

    for (int iteration = 0; iteration < 1000; ++iteration) {
        reset_observations();
        control_plane::ControlPlane control{};
        binary_data_plane::TransferManager binary{};
        control_stream::ControlStream stream{};
        control_stream::init(&stream, &control, &binary);

        std::vector<std::uint8_t> input;
        for (int index = 0; index < length_distribution(generator); ++index) {
            int value = byte_distribution(generator);
            if (value == '\n') value = 0x7f;
            input.push_back(static_cast<std::uint8_t>(value));
        }

        switch (prefix_kind_distribution(generator)) {
        case 0:
            break;
        case 1:
            input.push_back(0);
            break;
        case 2:
            input.insert(input.end(), {0, 0});
            break;
        case 3:
            input.insert(input.end(), {0, 0, 0x01, 0x02, 0x03});
            break;
        case 4:
            append(&input, bytes("@ESP-NP"));
            break;
        }

        const int zero_run = iteration % 4;
        input.insert(input.end(), static_cast<std::size_t>(zero_run), 0);
        input.insert(input.end(), control_stream::kTransportSyncLength, 0);
        append(&input, ping_frame());
        feed(&stream, input);

        assert(g_ping_count == 1);
        assert(stream.state == control_stream::State::Text);
        assert(stream.encoded_length == 0);
    }
}

} // namespace

namespace control_plane {

void reset_input(ControlPlane *control)
{
    if (control == nullptr) return;
    control->frame_length = 0;
    control->discarding = false;
    control->prefix_candidate = true;
    control->frame[0] = '\0';
}

void feed(ControlPlane *control, const std::uint8_t *data, std::size_t length)
{
    if (control == nullptr || data == nullptr) return;
    for (std::size_t index = 0; index < length; ++index) {
        const std::uint8_t byte = data[index];
        if (byte == '\n') {
            const std::string line(control->frame, control->frame_length);
            if (line.find("\"cmd\":\"system.ping\"") != std::string::npos) {
                ++g_ping_count;
            }
            if (line.find("\"cmd\":\"protocol.hello\"") != std::string::npos) {
                ++g_hello_count;
            }
            reset_input(control);
        } else if (control->frame_length < control_plane::kMaxFrameBytes) {
            control->frame[control->frame_length++] = static_cast<char>(byte);
        } else {
            control->discarding = true;
        }
    }
}

} // namespace control_plane

namespace binary_data_plane {

void handle_decoded_frame(TransferManager *, const std::uint8_t *, std::size_t, std::uint32_t)
{
    ++g_binary_count;
}

} // namespace binary_data_plane

int main()
{
    test_detector_boundaries();
    test_text_recovery_cases();
    test_three_zeros_and_adjacent_binary();
    test_sync_chunk_boundaries();
    test_long_sync_run();
    test_randomized_recovery();
    return 0;
}
