#pragma once

#include <cstddef>
#include <string_view>

namespace file_transfer::path {

bool validate(std::string_view value, bool allow_root);
bool valid_component(std::string_view value);

} // namespace file_transfer::path
