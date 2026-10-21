#include "oldbox/console.hpp"
#include "oldbox/machine.hpp"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace oldbox {
namespace {

constexpr std::size_t kMaxLogLines = 1000;

std::string lower_ascii(std::string_view s) {
    std::string out(s);
    for (char& c : out)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return out;
}

std::string trim(std::string_view s) {
    std::size_t b = 0, e = s.size();
    auto space = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (b < e && space(s[b])) ++b;
    while (e > b && space(s[e - 1])) --e;
    return std::string(s.substr(b, e - b));
}

bool starts_with_ci(std::string_view text, std::string_view prefix) {
    if (prefix.size() > text.size()) return false;
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        char a = text[i], b = prefix[i];
        if (a >= 'A' && a <= 'Z') a = char(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = char(b - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

u32 parse_number(std::string_view s, std::string_view what) {
    int base = 10;
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s.remove_prefix(2);
        base = 16;
    }
    u32 v = 0;
    auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v, base);
    if (s.empty() || ec != std::errc{} || p != s.data() + s.size())
        throw Error(std::format("invalid {} '{}'", what, s));
    return v;
}

struct KeyDef {
    const char* name;
    u8 code;
    bool extended;
};

// PC/AT set-1 make codes.  The E0 prefix is added by the 8042 model for the
// extended keys, exactly as for a real keyboard.
constexpr KeyDef kKeys[] = {
    {"esc", 0x01, false},        {"escape", 0x01, false},
    {"enter", 0x1c, false},      {"return", 0x1c, false},
    {"backspace", 0x0e, false},  {"tab", 0x0f, false},
    {"space", 0x39, false},      {"capslock", 0x3a, false},
    {"numlock", 0x45, false},    {"scrolllock", 0x46, false},
    {"up", 0x48, true},          {"down", 0x50, true},
    {"left", 0x4b, true},        {"right", 0x4d, true},
    {"insert", 0x52, true},      {"ins", 0x52, true},
    {"delete", 0x53, true},      {"del", 0x53, true},
    {"home", 0x47, true},        {"end", 0x4f, true},
    {"pageup", 0x49, true},      {"pgup", 0x49, true},
    {"pagedown", 0x51, true},    {"pgdn", 0x51, true},
    {"f1", 0x3b, false},         {"f2", 0x3c, false},
    {"f3", 0x3d, false},         {"f4", 0x3e, false},
    {"f5", 0x3f, false},         {"f6", 0x40, false},
    {"f7", 0x41, false},         {"f8", 0x42, false},
    {"f9", 0x43, false},         {"f10", 0x44, false},
    {"f11", 0x57, false},        {"f12", 0x58, false},
    {"ctrl", 0x1d, false},       {"lctrl", 0x1d, false},
    {"rctrl", 0x1d, true},       {"alt", 0x38, false},
    {"lalt", 0x38, false},       {"ralt", 0x38, true},
    {"shift", 0x2a, false},      {"lshift", 0x2a, false},
    {"rshift", 0x36, false},     {"minus", 0x0c, false},
    {"equal", 0x0d, false},      {"equals", 0x0d, false},
    {"comma", 0x33, false},      {"period", 0x34, false},
    {"slash", 0x35, false},      {"backslash", 0x2b, false},
    {"semicolon", 0x27, false},  {"apostrophe", 0x28, false},
    {"grave", 0x29, false},      {"bracketleft", 0x1a, false},
    {"bracketright", 0x1b, false},
};

struct KeyPress {
    u8 code = 0;
    bool extended = false;
};

std::optional<KeyPress> named_key(std::string_view name_in) {
    const std::string name = lower_ascii(name_in);
    for (const KeyDef& k : kKeys)
        if (name == k.name) return KeyPress{k.code, k.extended};

    static constexpr u8 letters[26] = {
        0x1e, 0x30, 0x2e, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32,
        0x31, 0x18, 0x19, 0x10, 0x13, 0x1f, 0x14, 0x16, 0x2f, 0x11, 0x2d, 0x15, 0x2c};
    if (name.size() == 1) {
        const char c = name[0];
        if (c >= 'a' && c <= 'z') return KeyPress{letters[c - 'a'], false};
        if (c >= '1' && c <= '9') return KeyPress{u8(0x01 + (c - '1')), false};
        if (c == '0') return KeyPress{0x0b, false};
        switch (c) {
        case '-': return KeyPress{0x0c, false};
        case '=': return KeyPress{0x0d, false};
        case '[': return KeyPress{0x1a, false};
        case ']': return KeyPress{0x1b, false};
        case ';': return KeyPress{0x27, false};
        case '\'': return KeyPress{0x28, false};
        case '`': return KeyPress{0x29, false};
        case '\\': return KeyPress{0x2b, false};
        case ',': return KeyPress{0x33, false};
        case '.': return KeyPress{0x34, false};
        case '/': return KeyPress{0x35, false};
        default: break;
        }
    }
    return std::nullopt;
}

bool is_floppy_extension(const std::string& ext) {
    return ext == ".img" || ext == ".ima" || ext == ".vfd" || ext == ".dsk" ||
           ext == ".144" || ext == ".360" || ext == ".720" || ext == ".120";
}
bool is_cdrom_extension(const std::string& ext) {
    return ext == ".iso" || ext == ".cdr" || ext == ".udf";
}
bool is_hdd_extension(const std::string& ext) {
    return ext == ".raw" || ext == ".vhd" || ext == ".vdi" || ext == ".qcow2";
}

// A floppy image is at most a 2.88 MB extended-density disk.
constexpr u64 kMaxFloppyBytes = 2949120;

struct CommandDoc {
    const char* name;
    const char* usage;
    const char* summary;
};

constexpr CommandDoc kDocs[] = {
    {"help", "help [COMMAND]", "list commands or help for one"},
    {"status", "status", "machine, CPU and device status"},
    {"devices", "devices", "PCI and memory map"},
    {"floppy", "floppy [a|b] status|insert PATH|eject", "insert, eject or show a floppy"},
    {"cdrom", "cdrom status|insert PATH|eject", "insert, eject or show the CD"},
    {"insert", "insert PATH", "insert media by extension"},
    {"eject", "eject [floppy|cdrom|a|b]", "eject media"},
    {"images", "images [DIR]", "list disk images in a directory"},
    {"key", "key NAME[+NAME...]", "press guest keys"},
    {"type", "type TEXT", "type text into the guest"},
    {"screenshot", "screenshot [FILE]", "save the current frame as PPM"},
    {"text", "text [FILE]", "dump the VGA text screen"},
    {"cmos", "cmos dump|get IDX|set IDX VAL", "read or patch CMOS bytes"},
    {"pause", "pause", "stop advancing emulated time"},
    {"resume", "resume", "resume emulation"},
    {"step", "step [SECONDS]", "advance emulation while paused"},
    {"reset", "reset", "hard reset, restart the BIOS POST"},
    {"flush", "flush", "write dirty images back to the host"},
    {"version", "version", "print the OldBox version"},
    {"quit", "quit", "close the emulator"},
};

std::size_t token_start(const std::string& line) {
    std::size_t start = 0;
    bool quoted = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (c == '"') quoted = !quoted;
        else if (!quoted && (c == ' ' || c == '\t')) start = i + 1;
    }
    return start;
}

#ifdef _WIN32
bool path_prefix_matches(std::string_view name, std::string_view prefix) {
    return starts_with_ci(name, prefix);
}
#else
bool path_prefix_matches(std::string_view name, std::string_view prefix) {
    return name.size() >= prefix.size() && name.substr(0, prefix.size()) == prefix;
}
#endif

std::vector<std::string> path_candidates(const std::string& token) {
    std::string text = token;
    bool quoted = false;
    if (!text.empty() && text.front() == '"') {
        quoted = true;
        text.erase(text.begin());
    }
    std::filesystem::path p(text);
    const std::filesystem::path dir = p.has_parent_path() ? p.parent_path() : std::filesystem::path(".");
    const std::string prefix = p.filename().string();
    std::vector<std::string> out;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (!path_prefix_matches(name, prefix)) continue;
        std::string candidate;
        if (p.has_parent_path()) {
            const std::string parent = p.parent_path().string();
            candidate = parent;
            if (!parent.empty() && parent.back() != '/' && parent.back() != '\\') {
                // Keep the separator the user typed: forward slashes stay
                // forward slashes, otherwise use the native one.
                const bool forward = parent.find('/') != std::string::npos;
                candidate += forward ? '/' : std::filesystem::path::preferred_separator;
            }
        }
        candidate += name;
        std::error_code dir_ec;
        if (it->is_directory(dir_ec)) candidate += std::filesystem::path::preferred_separator;
        if (quoted || candidate.find(' ') != std::string::npos) candidate = "\"" + candidate + "\"";
        out.push_back(std::move(candidate));
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::string common_prefix(const std::vector<std::string>& values) {
    if (values.empty()) return {};
    std::string prefix = values.front();
    for (const std::string& v : values) {
        std::size_t n = 0;
        while (n < prefix.size() && n < v.size() && prefix[n] == v[n]) ++n;
        prefix.resize(n);
    }
    return prefix;
}

} // namespace

CommandConsole::CommandConsole(Machine& machine, bool echo_to_stdout)
    : machine_(machine), echo_(echo_to_stdout) {}

void CommandConsole::append_line(std::string line) {
    log_.push_back(std::move(line));
    while (log_.size() > kMaxLogLines) log_.pop_front();
}

void CommandConsole::emit(std::string_view text) {
    if (!text.empty() && text.back() == '\n') text.remove_suffix(1);
    std::size_t start = 0;
    for (;;) {
        const std::size_t nl = text.find('\n', start);
        std::string line(text.substr(start, nl == std::string_view::npos
                                                ? std::string_view::npos : nl - start));
        if (echo_) std::printf("%s\n", line.c_str());
        append_line(std::move(line));
        if (nl == std::string_view::npos) break;
        start = nl + 1;
    }
}

void CommandConsole::note(std::string_view text) {
    if (!text.empty() && text.back() == '\n') text.remove_suffix(1);
    std::size_t start = 0;
    for (;;) {
        const std::size_t nl = text.find('\n', start);
        append_line(std::string(text.substr(start, nl == std::string_view::npos
                                                       ? std::string_view::npos : nl - start)));
        if (nl == std::string_view::npos) break;
        start = nl + 1;
    }
}

const std::vector<std::string>& CommandConsole::commands() {
    static const std::vector<std::string> names = [] {
        std::vector<std::string> v;
        for (const CommandDoc& d : kDocs) v.emplace_back(d.name);
        return v;
    }();
    return names;
}

std::vector<std::string> CommandConsole::tokenize(std::string_view line) {
    std::vector<std::string> out;
    std::string current;
    bool quoted = false;
    bool has_token = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (quoted) {
            if (c == '"') {
                quoted = false;
            } else if (c == '\\' && i + 1 < line.size() && line[i + 1] == '"') {
                current += '"';
                ++i;
            } else {
                current += c;
            }
        } else if (c == '"') {
            quoted = true;
            has_token = true;
        } else if (c == ' ' || c == '\t') {
            if (has_token) {
                out.push_back(current);
                current.clear();
                has_token = false;
            }
        } else {
            current += c;
            has_token = true;
        }
    }
    if (has_token) out.push_back(current);
    return out;
}

bool CommandConsole::execute(const std::string& line) {
    const std::vector<std::string> args = tokenize(line);
    if (args.empty()) return !quit_;
    note(std::format("> {}", line));
    const std::string cmd = lower_ascii(args[0]);
    try {
        if (cmd == "help" || cmd == "?") help(args);
        else if (cmd == "status") emit(machine_.status_text());
        else if (cmd == "devices") emit(machine_.device_list());
        else if (cmd == "floppy") floppy_command(args);
        else if (cmd == "cdrom") cdrom_command(args);
        else if (cmd == "insert" || cmd == "mount") insert_auto(args);
        else if (cmd == "eject" || cmd == "unmount") eject_auto(args);
        else if (cmd == "images" || cmd == "ls") images(args);
        else if (cmd == "key" || cmd == "keys" || cmd == "press") key_command(args);
        else if (cmd == "type") type_command(line);
        else if (cmd == "screenshot") screenshot(args);
        else if (cmd == "text" || cmd == "screen") text_dump(args);
        else if (cmd == "cmos") cmos_command(args);
        else if (cmd == "pause") {
            paused_ = true;
            emit("emulation paused ('resume' continues, 'step' advances one slice)");
        } else if (cmd == "resume" || cmd == "continue") {
            paused_ = false;
            emit("emulation resumed");
        } else if (cmd == "step") {
            double seconds = 1.0 / 60.0;
            if (args.size() > 1) {
                try {
                    seconds = std::stod(args[1]);
                } catch (const std::exception&) {
                    throw Error("step expects a number of seconds");
                }
                if (seconds <= 0.0) throw Error("step expects a positive number of seconds");
            }
            paused_ = true;
            machine_.run_slice(seconds);
            emit(std::format("stepped {:.3f} s (now {:.3f} s emulated)", seconds,
                             machine_.emulated_seconds()));
        } else if (cmd == "reset" || cmd == "reboot") boot_reset();
        else if (cmd == "flush" || cmd == "sync") {
            machine_.floppy().flush();
            machine_.ata().flush_all();
            emit("dirty disk images flushed");
        } else if (cmd == "version") {
            emit(std::format("OldBox {} - 1998 Pentium II reference machine", OLDBOX_VERSION));
            emit(std::format("CPU: {}, RAM: {} MiB", cpu_name(machine_.config().cpu),
                             machine_.config().memory.size_mb));
        } else if (cmd == "quit" || cmd == "exit" || cmd == "stop") {
            quit_ = true;
            emit("closing OldBox");
        } else {
            emit(std::format("unknown command '{}' (type 'help' for the command list)", args[0]));
        }
    } catch (const std::exception& e) {
        emit(std::format("error: {}", e.what()));
    }
    return !quit_;
}

void CommandConsole::help(const std::vector<std::string>& args) {
    if (args.size() > 1) {
        const std::string want = lower_ascii(args[1]);
        for (const CommandDoc& d : kDocs) {
            if (want == d.name) {
                emit(std::format("usage: {}", d.usage));
                emit(d.summary);
                return;
            }
        }
        emit(std::format("no help for '{}'", args[1]));
        return;
    }
    emit("OldBox command console (F2 opens/closes it in the emulator window)");
    for (const CommandDoc& d : kDocs) emit(std::format("  {:<42} {}", d.usage, d.summary));
    emit("Tab completes commands and paths; Up/Down browse the history.");
}

void CommandConsole::floppy_command(const std::vector<std::string>& args) {
    int drive = 0;
    std::vector<std::string> rest;
    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string t = lower_ascii(args[i]);
        if (rest.empty() && (t == "a" || t == "a:" || t == "drivea")) {
            drive = 0;
        } else if (rest.empty() && (t == "b" || t == "b:" || t == "driveb")) {
            drive = 1;
        } else {
            rest.push_back(args[i]);
        }
    }
    if (rest.empty() || lower_ascii(rest[0]) == "status") {
        floppy_status(drive);
        return;
    }
    const std::string op = lower_ascii(rest[0]);
    if (op == "insert" || op == "mount" || op == "load" || op == "attach") {
        if (rest.size() < 2) throw Error("usage: floppy [a|b] insert PATH");
        insert_floppy(drive, rest[1]);
    } else if (op == "eject" || op == "remove" || op == "unmount" || op == "detach") {
        eject_floppy(drive);
    } else {
        throw Error(std::format("unknown floppy action '{}' (status, insert, eject)", rest[0]));
    }
}

void CommandConsole::floppy_status(int drive) {
    const FloppyController& f = machine_.floppy();
    const bool present = drive == 0 ? f.present() : f.present_b();
    const char letter = drive == 0 ? 'A' : 'B';
    if (!present) {
        emit(std::format("{}: no disk", letter));
        return;
    }
    const std::string& path = drive == 0 ? f.path() : f.path_b();
    const u64 bytes = drive == 0 ? f.media_bytes() : f.media_bytes_b();
    const u32 tracks = drive == 0 ? f.tracks() : f.tracks_b();
    const u32 heads = drive == 0 ? f.heads() : f.heads_b();
    const u32 spt = drive == 0 ? f.sectors_per_track() : f.sectors_per_track_b();
    const bool ro = drive == 0 ? f.write_protected() : f.write_protected_b();
    emit(std::format("{}: {} {} {}/{}/{} {}", letter, path.empty() ? "(memory image)" : path,
                     format_bytes(bytes), tracks, heads, spt,
                     ro ? "write-protected" : "read-write"));
}

void CommandConsole::insert_floppy(int drive, const std::string& path) {
    const bool ok = drive == 0 ? machine_.floppy().attach(path) : machine_.floppy().attach_b(path);
    if (!ok)
        throw Error(std::format("cannot attach floppy image '{}' (missing, empty or smaller than one sector)",
                                path));
    const FloppyController& f = machine_.floppy();
    const u32 tracks = drive == 0 ? f.tracks() : f.tracks_b();
    const u32 heads = drive == 0 ? f.heads() : f.heads_b();
    const u32 spt = drive == 0 ? f.sectors_per_track() : f.sectors_per_track_b();
    const u64 bytes = drive == 0 ? f.media_bytes() : f.media_bytes_b();
    emit(std::format("{}: inserted {} ({}, {}/{}/{})", drive == 0 ? 'A' : 'B', path,
                     format_bytes(bytes), tracks, heads, spt));
}

void CommandConsole::eject_floppy(int drive) {
    FloppyController& f = machine_.floppy();
    if (drive == 0) {
        if (!f.present()) {
            emit("A: the drive is already empty");
            return;
        }
        f.eject();
        emit("A: disk ejected");
    } else {
        if (!f.present_b()) {
            emit("B: the drive is already empty");
            return;
        }
        f.eject_b();
        emit("B: disk ejected");
    }
}

void CommandConsole::cdrom_command(const std::vector<std::string>& args) {
    std::vector<std::string> rest(args.begin() + 1, args.end());
    if (rest.empty() || lower_ascii(rest[0]) == "status") {
        cdrom_status();
        return;
    }
    const std::string op = lower_ascii(rest[0]);
    if (op == "insert" || op == "mount" || op == "load" || op == "attach") {
        if (rest.size() < 2) throw Error("usage: cdrom insert PATH");
        insert_cdrom(rest[1]);
    } else if (op == "eject" || op == "remove" || op == "unmount" || op == "detach") {
        eject_cdrom();
    } else {
        throw Error(std::format("unknown cdrom action '{}' (status, insert, eject)", rest[0]));
    }
}

void CommandConsole::cdrom_status() {
    const AtaController& ata = machine_.ata();
    if (!ata.cdrom_media_present()) {
        emit("D: no CD in the ATAPI drive");
        return;
    }
    emit(std::format("D: {} ({}, {} x 2048-byte sectors)", ata.cdrom_path(),
                     format_bytes(ata.cdrom_bytes()), ata.cdrom_sectors()));
}

void CommandConsole::insert_cdrom(const std::string& path) {
    std::error_code ec;
    const u64 bytes = std::filesystem::file_size(path, ec);
    if (ec) throw Error(std::format("cannot open '{}': {}", path, ec.message()));
    if (!machine_.ata().mount_cdrom(path))
        throw Error(std::format("cannot mount CD image '{}'", path));
    emit(std::format("D: inserted {} ({}, {} sectors)", path, format_bytes(bytes),
                     machine_.ata().cdrom_sectors()));
    if (bytes % 2048 != 0)
        emit("warning: the image size is not a multiple of 2048 bytes (ISO logical sectors)");
}

void CommandConsole::eject_cdrom() {
    if (!machine_.ata().cdrom_media_present()) {
        emit("D: the CD drive is already empty");
        return;
    }
    if (!machine_.ata().eject_cdrom()) throw Error("the secondary master is not an ATAPI drive");
    emit("D: CD ejected");
}

void CommandConsole::insert_auto(const std::vector<std::string>& args) {
    if (args.size() < 2) throw Error("usage: insert PATH");
    const std::string& path = args[1];
    std::error_code ec;
    const u64 bytes = std::filesystem::file_size(path, ec);
    if (ec) throw Error(std::format("cannot open '{}': {}", path, ec.message()));
    const std::string ext = lower_ascii(std::filesystem::path(path).extension().string());
    if (is_cdrom_extension(ext)) {
        insert_cdrom(path);
    } else if (is_floppy_extension(ext)) {
        if (bytes > kMaxFloppyBytes)
            throw Error(std::format("'{}' is {}: too large for a floppy; "
                                    "use 'cdrom insert' for a CD image", path, format_bytes(bytes)));
        insert_floppy(0, path);
    } else if (is_hdd_extension(ext)) {
        throw Error(std::format("'{}' looks like a hard-disk image; only floppy and CD media "
                                "can be changed at run time", path));
    } else {
        throw Error(std::format("cannot guess the media type of '{}'; "
                                "use 'floppy insert PATH' or 'cdrom insert PATH'", path));
    }
}

void CommandConsole::eject_auto(const std::vector<std::string>& args) {
    const std::string what = args.size() > 1 ? lower_ascii(args[1]) : "a";
    if (what == "a" || what == "a:" || what == "floppy" || what == "fdd") eject_floppy(0);
    else if (what == "b" || what == "b:" || what == "floppy-b") eject_floppy(1);
    else if (what == "cd" || what == "cdrom" || what == "d" || what == "d:" || what == "dvd")
        eject_cdrom();
    else
        throw Error(std::format("unknown media '{}' (use floppy, a, b or cdrom)", args[1]));
}

void CommandConsole::images(const std::vector<std::string>& args) {
    const std::string dir = args.size() > 1 ? args[1] : ".";
    std::error_code ec;
    std::filesystem::directory_iterator it(dir, ec);
    const std::filesystem::directory_iterator end;
    if (ec) throw Error(std::format("cannot list '{}': {}", dir, ec.message()));
    struct Entry {
        std::string path;
        u64 bytes = 0;
        std::string kind;
    };
    std::vector<Entry> entries;
    for (; it != end; it.increment(ec)) {
        if (ec) break;
        std::error_code file_ec;
        if (!it->is_regular_file(file_ec)) continue;
        const std::string ext = lower_ascii(it->path().extension().string());
        std::string kind;
        if (is_cdrom_extension(ext)) kind = "cdrom";
        else if (is_floppy_extension(ext)) kind = "floppy";
        else if (is_hdd_extension(ext)) kind = "hdd";
        else continue;
        entries.push_back({it->path().string(), u64(it->file_size(file_ec)), std::move(kind)});
    }
    std::sort(entries.begin(), entries.end(),
              [](const Entry& a, const Entry& b) { return a.path < b.path; });
    if (entries.empty()) {
        emit(std::format("no .img/.ima/.iso images in {}", dir));
        return;
    }
    const std::string attached_a = machine_.floppy().path();
    const std::string attached_cd = machine_.ata().cdrom_path();
    for (const Entry& e : entries) {
        const std::string marker = (e.path == attached_a || e.path == attached_cd) ? "*" : " ";
        emit(std::format("{} {:>9}  {:<6}  {}", marker, format_bytes(e.bytes), e.kind, e.path));
    }
    emit("(* = currently inserted)");
}

void CommandConsole::key_command(const std::vector<std::string>& args) {
    if (args.size() < 2) throw Error("usage: key NAME[+NAME...] (e.g. key enter, key ctrl+alt+del)");
    for (std::size_t i = 1; i < args.size(); ++i) press_combo(args[i]);
}

void CommandConsole::press_combo(std::string_view combo) {
    std::vector<KeyPress> keys;
    std::size_t start = 0;
    for (;;) {
        const std::size_t plus = combo.find('+', start);
        const std::string part = trim(combo.substr(
            start, plus == std::string_view::npos ? std::string_view::npos : plus - start));
        if (!part.empty()) {
            const auto key = named_key(part);
            if (!key) throw Error(std::format("unknown key '{}'", part));
            keys.push_back(*key);
        }
        if (plus == std::string_view::npos) break;
        start = plus + 1;
    }
    if (keys.empty()) throw Error("key expects at least one key name");
    for (const KeyPress& k : keys) machine_.set1_key_event(k.code, true, k.extended);
    for (auto it = keys.rbegin(); it != keys.rend(); ++it)
        machine_.set1_key_event(it->code, false, it->extended);
    emit(std::format("pressed {}", combo));
}

void CommandConsole::type_command(const std::string& line) {
    std::size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
    while (i < line.size() && line[i] != ' ' && line[i] != '\t') ++i;
    std::string text = i < line.size() ? line.substr(i + 1) : std::string();
    if (text.empty()) throw Error("usage: type TEXT");
    machine_.type_text(text);
    emit(std::format("typing into the guest: {}", text));
}

void CommandConsole::screenshot(const std::vector<std::string>& args) {
    const std::string path = args.size() > 1 ? args[1] : "oldbox-console.ppm";
    const VideoFrame frame = machine_.display_frame();
    if (!frame.valid()) throw Error("no video frame is available yet");
    std::ofstream out(path, std::ios::binary);
    if (!out) throw Error(std::format("cannot create '{}'", path));
    out << "P6\n" << frame.width << " " << frame.height << "\n255\n";
    for (int y = 0; y < frame.height; ++y) {
        for (int x = 0; x < frame.width; ++x) {
            const u32 p = frame.pixels[std::size_t(y) * std::size_t(frame.stride) + std::size_t(x)];
            const char rgb[3] = {char((p >> 16) & 0xff), char((p >> 8) & 0xff), char(p & 0xff)};
            out.write(rgb, 3);
        }
    }
    if (!out) throw Error(std::format("failed while writing '{}'", path));
    emit(std::format("wrote {} ({}x{})", path, frame.width, frame.height));
}

void CommandConsole::text_dump(const std::vector<std::string>& args) {
    std::string screen;
    for (int row = 0; row < 25; ++row) {
        std::string line;
        for (int col = 0; col < 80; ++col) {
            const u8 ch = machine_.bus().read8(0xb8000u + u32((row * 80 + col) * 2));
            line.push_back(ch >= 32 && ch < 127 ? char(ch) : ' ');
        }
        while (!line.empty() && line.back() == ' ') line.pop_back();
        screen += line;
        screen += '\n';
    }
    if (args.size() > 1) {
        std::ofstream out(args[1], std::ios::binary);
        if (!out) throw Error(std::format("cannot create '{}'", args[1]));
        out << screen;
        emit(std::format("wrote {}", args[1]));
    } else {
        emit(screen);
    }
}

void CommandConsole::cmos_command(const std::vector<std::string>& args) {
    const std::string op = args.size() > 1 ? lower_ascii(args[1]) : "dump";
    if (op == "dump" || op == "show") {
        emit("--- CMOS (128 bytes) ---");
        for (int i = 0; i < 128; i += 16) {
            std::string line = std::format("{:02x}:", i);
            for (int j = 0; j < 16; ++j)
                line += std::format(" {:02x}", machine_.cmos(u8(i + j)));
            emit(line);
        }
        u32 sum = 0;
        for (int i = 0x10; i <= 0x2d; ++i) sum += machine_.cmos(u8(i));
        emit(std::format("Award checksum 0x10-0x2d sum={:04x}, stored 0x2e/0x2f={:02x} {:02x}", sum,
                         machine_.cmos(0x2e), machine_.cmos(0x2f)));
        return;
    }
    if (op == "get") {
        if (args.size() < 3) throw Error("usage: cmos get IDX");
        const u32 index = parse_number(args[2], "CMOS index");
        if (index > 127) throw Error("CMOS index must be 0..127");
        emit(std::format("CMOS[{:02x}] = {:02x}", index, machine_.cmos(u8(index))));
        return;
    }
    if (op == "set") {
        if (args.size() < 4) throw Error("usage: cmos set IDX VALUE");
        const u32 index = parse_number(args[2], "CMOS index");
        const u32 value = parse_number(args[3], "CMOS value");
        if (index > 127) throw Error("CMOS index must be 0..127");
        if (value > 0xff) throw Error("CMOS value must be 0..255");
        machine_.set_cmos(u8(index), u8(value));
        machine_.piix4().rtc().refresh_checksum();
        emit(std::format("CMOS[{:02x}] := {:02x} (checksum refreshed)", index, value));
        return;
    }
    throw Error(std::format("unknown cmos action '{}' (dump, get, set)", args[1]));
}

void CommandConsole::boot_reset() {
    machine_.floppy().flush();
    machine_.ata().flush_all();
    machine_.reset();
    emit("machine reset; the BIOS POST restarts");
}

void CommandConsole::complete(std::string& line) {
    const std::size_t start = token_start(line);
    const std::string prefix = line.substr(0, start);
    const std::string token = line.substr(start);
    const std::vector<std::string> before = tokenize(prefix);
    const bool first_word = before.empty();
    std::vector<std::string> candidates;

    if (first_word) {
        for (const std::string& name : commands())
            if (starts_with_ci(name, token)) candidates.push_back(name);
    } else {
        const std::string cmd = lower_ascii(before[0]);
        bool path_arg = false;
        if (cmd == "insert" || cmd == "images" || cmd == "ls" || cmd == "screenshot" ||
            cmd == "text" || cmd == "screen") {
            path_arg = true;
        } else if (cmd == "floppy" || cmd == "cdrom") {
            std::size_t action = 1;
            if (cmd == "floppy" && before.size() >= 2) {
                const std::string drive = lower_ascii(before[1]);
                if (drive == "a" || drive == "a:" || drive == "b" || drive == "b:") action = 2;
            }
            if (before.size() <= action) {
                static const char* const kActions[] = {"status", "insert", "eject"};
                for (const char* a : kActions)
                    if (starts_with_ci(a, token)) candidates.emplace_back(a);
            } else if (lower_ascii(before[action]) == "insert") {
                path_arg = true;
            }
        }
        if (path_arg) candidates = path_candidates(token);
    }

    if (candidates.empty()) return;
    if (candidates.size() == 1) {
        const std::string& candidate = candidates.front();
        line = prefix + candidate;
        if (!candidate.empty() &&
            candidate.back() != std::filesystem::path::preferred_separator &&
            candidate.back() != '/')
            line += ' ';
        return;
    }
    const std::string common = common_prefix(candidates);
    if (common.size() > token.size()) line = prefix + common;
    emit(std::format("{} completions:", candidates.size()));
    std::string row;
    for (const std::string& c : candidates) {
        if (!row.empty() && row.size() + c.size() + 2 > 96) {
            emit(row);
            row.clear();
        }
        if (!row.empty()) row += "  ";
        row += c;
    }
    if (!row.empty()) emit(row);
}

} // namespace oldbox
