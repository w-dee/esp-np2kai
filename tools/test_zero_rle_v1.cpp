#include "file_transfer/zero_rle_v1.hpp"

#include <array>
#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using file_transfer::zero_rle_v1::Decoder;
using file_transfer::zero_rle_v1::Result;

struct Sink {
    std::vector<std::uint8_t> output;
    bool fail = false;
};

bool emit(void *context, std::uint64_t offset, const std::uint8_t *data,
          std::size_t length)
{
    auto *sink = static_cast<Sink *>(context);
    if (sink->fail || offset != sink->output.size()) return false;
    sink->output.insert(sink->output.end(), data, data + length);
    return true;
}

void check(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}

std::vector<std::uint8_t> u32(std::uint32_t value)
{
    return {static_cast<std::uint8_t>(value),
            static_cast<std::uint8_t>(value >> 8),
            static_cast<std::uint8_t>(value >> 16),
            static_cast<std::uint8_t>(value >> 24)};
}

void append(std::vector<std::uint8_t> &out, std::uint8_t tag,
            std::uint32_t length, const std::vector<std::uint8_t> &data = {})
{
    out.push_back(tag);
    const auto bytes = u32(length);
    out.insert(out.end(), bytes.begin(), bytes.end());
    out.insert(out.end(), data.begin(), data.end());
}

std::vector<std::uint8_t> encode(const std::vector<std::uint8_t> &input,
                                 std::size_t *zero_records,
                                 std::size_t *literal_records,
                                 std::size_t *literal_bytes)
{
    std::vector<std::uint8_t> output;
    *zero_records = 0;
    *literal_records = 0;
    *literal_bytes = 0;
    std::size_t offset = 0;
    while (offset < input.size()) {
        const bool zero = input[offset] == 0;
        std::size_t end = offset + 1;
        while (end < input.size() && (input[end] == 0) == zero) ++end;
        append(output, zero ? 0x00 : 0x01,
               static_cast<std::uint32_t>(end - offset),
               zero ? std::vector<std::uint8_t>{} :
                      std::vector<std::uint8_t>(input.begin() + offset, input.begin() + end));
        if (zero) {
            ++*zero_records;
        } else {
            ++*literal_records;
            *literal_bytes += end - offset;
        }
        offset = end;
    }
    return output;
}

std::vector<std::uint8_t> decode(const std::vector<std::uint8_t> &encoded,
                                 std::uint64_t logical_size,
                                 std::uint64_t wire_size,
                                 std::size_t chunk_size,
                                 Result expected = Result::Ok)
{
    Sink sink;
    std::array<std::uint8_t, 4096> zero_buffer{};
    Decoder decoder;
    decoder.init(logical_size, wire_size, emit, &sink,
                 zero_buffer.data(), zero_buffer.size());
    std::size_t offset = 0;
    while (offset < encoded.size()) {
        const std::size_t length = std::min(chunk_size, encoded.size() - offset);
        const Result result = decoder.consume(offset, encoded.data() + offset, length);
        if (result != Result::Ok) {
            check(result == expected, "unexpected decoder consume result");
            return sink.output;
        }
        offset += length;
    }
    const Result result = decoder.finish();
    check(result == expected, "unexpected decoder finish result");
    return sink.output;
}

void test_valid_streams()
{
    const std::vector<std::vector<std::uint8_t>> inputs = {
        {}, {0}, {0, 0, 0, 0}, {1}, {1, 2, 3},
        {0, 1, 0, 2, 0, 3}, {0, 0, 1, 1, 0, 0},
    };
    for (std::size_t test_index = 0; test_index < inputs.size(); ++test_index) {
        const auto &input = inputs[test_index];
        std::size_t zero_records = 0;
        std::size_t literal_records = 0;
        std::size_t literal_bytes = 0;
        const auto encoded = encode(input, &zero_records, &literal_records, &literal_bytes);
        const auto decoded = decode(encoded, input.size(), encoded.size(), 1);
        if (decoded != input) {
            std::fprintf(stderr, "valid test %zu output mismatch\n", test_index);
            throw std::runtime_error("valid stream did not round-trip across byte boundaries");
        }
    }

    std::vector<std::uint8_t> adjacent;
    append(adjacent, 0x00, 2);
    append(adjacent, 0x00, 3);
    append(adjacent, 0x01, 3, {0, 1, 0});
    append(adjacent, 0x01, 1, {2});
    const auto decoded = decode(adjacent, 9, adjacent.size(), 7);
    check(decoded == std::vector<std::uint8_t>({0, 0, 0, 0, 0, 0, 1, 0, 2}),
          "adjacent records or literal zero bytes were rejected");
}

void test_invalid_streams()
{
    const std::vector<std::vector<std::uint8_t>> malformed = {
        {0x7f, 0, 0, 0, 1}, {0x00, 0, 0, 0, 0},
        {0x00, 0xff, 0xff, 0xff, 0x7f}, {0x00, 1, 0, 0},
        {0x01, 2, 0, 0, 0, 1}, {0x00, 1, 0, 0, 0},
    };
    const std::vector<std::uint64_t> logical = {1, 0, 1, 1, 2, 1};
    const std::vector<std::uint64_t> wire = {5, 5, 5, 4, 6, 6};
    const std::vector<std::size_t> chunks = {5, 5, 5, 4, 6, 5};
    for (std::size_t index = 0; index < malformed.size(); ++index) {
        try {
            decode(malformed[index], logical[index], wire[index], chunks[index], Result::Malformed);
        } catch (const std::exception &) {
            std::fprintf(stderr, "malformed test %zu failed\n", index);
            throw;
        }
    }

    Sink sink;
    std::array<std::uint8_t, 8> zeros{};
    Decoder decoder;
    decoder.init(1, 5, emit, &sink, zeros.data(), zeros.size());
    check(decoder.consume(1, reinterpret_cast<const std::uint8_t *>("\0"), 1) ==
              Result::Malformed,
          "wire offset mismatch was accepted");

    sink.fail = true;
    decoder.init(1, 5, emit, &sink, zeros.data(), zeros.size());
    const std::array<std::uint8_t, 5> zero_record{0, 1, 0, 0, 0};
    check(decoder.consume(0, zero_record.data(), zero_record.size()) == Result::OutputFailed,
          "output failure was not surfaced");
}

void test_np2_fixture(const std::string &fixture_path, const std::string &decoded_path)
{
    std::ifstream input(fixture_path, std::ios::binary);
    check(input.good(), "NP2TEST fixture could not be opened");
    std::vector<std::uint8_t> source((std::istreambuf_iterator<char>(input)), {});
    check(source.size() == 1261568, "NP2TEST fixture size changed");
    std::size_t zero_records = 0;
    std::size_t literal_records = 0;
    std::size_t literal_bytes = 0;
    const auto encoded = encode(source, &zero_records, &literal_records, &literal_bytes);
    check(zero_records == 96 && literal_records == 96 && literal_bytes == 852,
          "NP2TEST zero-rle record shape changed");
    check(encoded.size() == 1812, "NP2TEST zero-rle encoded size changed");
    const auto decoded = decode(encoded, source.size(), encoded.size(), 17);
    check(decoded == source, "NP2TEST zero-rle decode differs from source");
    std::ofstream output(decoded_path, std::ios::binary | std::ios::trunc);
    check(output.good(), "decoded output could not be opened");
    output.write(reinterpret_cast<const char *>(decoded.data()),
                 static_cast<std::streamsize>(decoded.size()));
    check(output.good(), "decoded output could not be written");
}

} // namespace

int main(int argc, char **argv)
{
    try {
        check(argc == 3, "usage: test_zero_rle_v1 <fixture> <decoded-output>");
        test_valid_streams();
        test_invalid_streams();
        test_np2_fixture(argv[1], argv[2]);
        return 0;
    } catch (const std::exception &error) {
        return (std::fprintf(stderr, "ERROR: %s\n", error.what()), 1);
    }
}
