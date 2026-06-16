#pragma once

#include <array>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s32 = std::int32_t;
using s64 = std::int64_t;

constexpr u32 AlignDown(const u32 value, const u32 alignment) {
    return value & ~(alignment - 1);
}

constexpr u32 AlignUp(const u32 value, const u32 alignment) {
    return AlignDown(value + alignment - 1, alignment);
}

inline std::string Hex32(const u32 value) {
    std::ostringstream stream;
    stream << "0x" << std::hex << std::uppercase << value;
    return stream.str();
}

inline std::string JoinPathForGuest(const std::string_view base, const std::string_view leaf) {
    std::string out(base);
    if (!out.empty() && out.back() != '/') {
        out.push_back('/');
    }
    out.append(leaf);
    return out;
}

template<typename T>
inline T BitCastFromU64(const u64 value) {
    static_assert(sizeof(T) == sizeof(value));
    return std::bit_cast<T>(value);
}

template<typename T>
inline u64 BitCastToU64(const T value) {
    static_assert(sizeof(T) == sizeof(u64));
    return std::bit_cast<u64>(value);
}
