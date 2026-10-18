#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>
#include <array>
#include <memory>
#include <functional>
#include <span>
#include <optional>
#include <stdexcept>
#include <format>

namespace oldbox {
using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i8 = std::int8_t;
using i16 = std::int16_t;
using i32 = std::int32_t;
using i64 = std::int64_t;

constexpr u32 page_size = 4096;
constexpr u32 megabyte = 1024u * 1024u;

[[nodiscard]] constexpr u32 align_down(u32 v, u32 a) { return v & ~(a - 1); }
[[nodiscard]] constexpr u32 align_up(u32 v, u32 a) { return (v + a - 1) & ~(a - 1); }
[[nodiscard]] constexpr bool bit(u32 v, unsigned n) { return ((v >> n) & 1u) != 0; }
[[nodiscard]] constexpr u32 bits(u32 v, unsigned lo, unsigned hi) {
    return (v >> lo) & ((1u << (hi - lo + 1)) - 1u);
}
template <typename T> [[nodiscard]] constexpr T sign_extend(u32 v, unsigned bits_count) {
    const u32 m = 1u << (bits_count - 1);
    return static_cast<T>((v ^ m) - m);
}
[[nodiscard]] inline u16 le16(const u8* p) { return u16(p[0]) | (u16(p[1]) << 8); }
[[nodiscard]] inline u32 le32(const u8* p) { return u32(p[0]) | (u32(p[1]) << 8) | (u32(p[2]) << 16) | (u32(p[3]) << 24); }
inline void put_le16(u8* p, u16 v) { p[0] = u8(v); p[1] = u8(v >> 8); }
inline void put_le32(u8* p, u32 v) { p[0] = u8(v); p[1] = u8(v >> 8); p[2] = u8(v >> 16); p[3] = u8(v >> 24); }

enum class LogLevel { trace, debug, info, warn, error };
void log_set_level(LogLevel level);
LogLevel log_level();
void log(LogLevel level, std::string_view component, std::string_view message);
template <typename... Args>
void logf(LogLevel level, std::string_view component, std::format_string<Args...> fmt, Args&&... args) {
    if (level < log_level()) return;
    log(level, component, std::format(fmt, std::forward<Args>(args)...));
}

struct Error : std::runtime_error { using std::runtime_error::runtime_error; };

std::vector<u8> read_file(const std::string& path);
void write_file(const std::string& path, std::span<const u8> data);
std::string hex(u64 value, unsigned digits = 0);
std::string format_bytes(u64 bytes);

} // namespace oldbox
