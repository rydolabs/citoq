#pragma once

#include <cstdint>
#include <string_view>

namespace cito {

inline std::uint64_t stable_name_id(
    std::string_view value) noexcept {
    std::uint64_t hash =
        14695981039346656037ull;

    for (const unsigned char byte :
         value) {
        hash ^= byte;
        hash *= 1099511628211ull;
    }

    return hash;
}

} // namespace cito
