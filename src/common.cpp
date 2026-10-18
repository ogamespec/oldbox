#include "oldbox/common.hpp"
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <mutex>

namespace oldbox {
namespace {
LogLevel g_level = LogLevel::info;
std::mutex g_log_mutex;
const char* level_name(LogLevel l) {
    switch (l) {
    case LogLevel::trace: return "TRACE";
    case LogLevel::debug: return "DEBUG";
    case LogLevel::info: return "INFO";
    case LogLevel::warn: return "WARN";
    case LogLevel::error: return "ERROR";
    }
    return "?";
}
}

void log_set_level(LogLevel level) { g_level = level; }
LogLevel log_level() { return g_level; }

void log(LogLevel level, std::string_view component, std::string_view message) {
    if (level < g_level) return;
    std::lock_guard lock(g_log_mutex);
    std::fprintf(stderr, "[%-5s] %-8.*s %.*s\n", level_name(level),
                 int(component.size()), component.data(), int(message.size()), message.data());
    std::fflush(stderr);
}

std::vector<u8> read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw Error("cannot open file: " + path);
    f.seekg(0, std::ios::end);
    const auto n = f.tellg();
    if (n < 0) throw Error("cannot size file: " + path);
    f.seekg(0, std::ios::beg);
    std::vector<u8> data(static_cast<size_t>(n));
    if (!data.empty()) f.read(reinterpret_cast<char*>(data.data()), std::streamsize(data.size()));
    if (!f && !f.eof()) throw Error("cannot read file: " + path);
    return data;
}

void write_file(const std::string& path, std::span<const u8> data) {
    std::ofstream f(path, std::ios::binary);
    if (!f) throw Error("cannot create file: " + path);
    f.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
    if (!f) throw Error("cannot write file: " + path);
}

std::string hex(u64 value, unsigned digits) {
    char buf[32];
    if (digits == 0) std::snprintf(buf, sizeof buf, "%llx", static_cast<unsigned long long>(value));
    else std::snprintf(buf, sizeof buf, "%0*llx", int(digits), static_cast<unsigned long long>(value));
    return buf;
}

std::string format_bytes(u64 bytes) {
    if (bytes >= 1024ull * 1024 * 1024) return std::format("{:.1f} GiB", double(bytes) / (1024.0 * 1024 * 1024));
    if (bytes >= 1024ull * 1024) return std::format("{:.1f} MiB", double(bytes) / (1024.0 * 1024));
    if (bytes >= 1024) return std::format("{:.1f} KiB", double(bytes) / 1024.0);
    return std::format("{} B", bytes);
}
}
