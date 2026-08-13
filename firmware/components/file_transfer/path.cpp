#include "file_transfer/path.hpp"

#include <cstdint>

#include "storage/storage.hpp"

namespace file_transfer::path {
namespace {

bool valid_utf8(std::string_view value)
{
    for (std::size_t i = 0; i < value.size();) {
        const unsigned char c = static_cast<unsigned char>(value[i]);
        std::size_t count = 0;
        std::uint32_t code = 0;
        if (c <= 0x7f) { count = 1; code = c; }
        else if (c >= 0xc2 && c <= 0xdf) { count = 2; code = c & 0x1f; }
        else if (c >= 0xe0 && c <= 0xef) { count = 3; code = c & 0x0f; }
        else if (c >= 0xf0 && c <= 0xf4) { count = 4; code = c & 0x07; }
        else return false;
        if (i + count > value.size()) return false;
        for (std::size_t j = 1; j < count; ++j) {
            const unsigned char next = static_cast<unsigned char>(value[i + j]);
            if ((next & 0xc0) != 0x80) return false;
            code = (code << 6) | (next & 0x3f);
        }
        if ((count == 2 && code < 0x80) ||
            (count == 3 && code < 0x800) ||
            (count == 4 && code < 0x10000) || code > 0x10ffff ||
            (code >= 0xd800 && code <= 0xdfff)) return false;
        i += count;
    }
    return true;
}

} // namespace

bool valid_component(std::string_view value)
{
    if (value.empty() || value.size() > storage::kMaxComponentBytes || !valid_utf8(value)) {
        return false;
    }
    if (value == "." || value == "..") return false;
    for (unsigned char c : value) {
        if (c == '/' || c == '\\' || c == 0x7f || c < 0x20) return false;
    }
    return true;
}

bool validate(std::string_view value, bool allow_root)
{
    if (value.empty() || value.size() > storage::kMaxPathBytes || value[0] != '/' ||
        !valid_utf8(value)) return false;
    if (value == "/") return allow_root;
    if (value.back() == '/') return false;
    std::size_t start = 1;
    while (start < value.size()) {
        const std::size_t end = value.find('/', start);
        const std::size_t length = end == std::string_view::npos ? value.size() - start : end - start;
        if (!valid_component(value.substr(start, length))) return false;
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return true;
}

} // namespace file_transfer::path
