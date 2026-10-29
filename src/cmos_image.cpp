#include "oldbox/cmos_image.hpp"
#include "oldbox/rtc.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

namespace oldbox {
namespace {
std::vector<u8> read_all(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    return std::vector<u8>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}
} // namespace

bool load_cmos_image(const std::string& path, Rtc& rtc) {
    const std::vector<u8> data = read_all(path);
    if (data.empty()) return false;
    if (data.size() == Rtc::kBankBytes) {
        // A plain 128-byte CMOS dump (for example from --cmos): keep it
        // compatible and restore the extended bank as erased.
        std::array<u8, Rtc::kNvramBytes> full{};
        std::copy(data.begin(), data.end(), full.begin());
        std::fill(full.begin() + Rtc::kBankBytes, full.end(), 0xff);
        if (!rtc.load_nvram(full)) return false;
        logf(LogLevel::info, "cmos", "loaded 128-byte CMOS image {}", path);
        return true;
    }
    if (data.size() != Rtc::kNvramBytes) {
        logf(LogLevel::warn, "cmos", "ignoring {}: expected {} or {} bytes, got {}", path,
             Rtc::kBankBytes, Rtc::kNvramBytes, data.size());
        return false;
    }
    if (!rtc.load_nvram(data)) return false;
    logf(LogLevel::info, "cmos", "loaded battery-backed NVRAM from {}", path);
    return true;
}

bool save_cmos_image(const std::string& path, const Rtc& rtc) {
    const auto data = rtc.nvram();
    std::error_code ec;
    const std::filesystem::path p(path);
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        logf(LogLevel::warn, "cmos", "cannot write CMOS image {}", path);
        return false;
    }
    out.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
    if (!out) {
        logf(LogLevel::warn, "cmos", "failed writing CMOS image {}", path);
        return false;
    }
    logf(LogLevel::info, "cmos", "saved battery-backed NVRAM to {}", path);
    return true;
}

} // namespace oldbox
