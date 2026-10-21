#pragma once
#include "oldbox/common.hpp"
#include <deque>
#include <vector>

namespace oldbox {

class Machine;

// Text command console for a running machine ("CLI").  It is deliberately
// frontend-independent: the SDL window renders log() in an overlay and feeds
// typed lines to execute(), while the same object is usable from tests or a
// script.  Commands only drive the existing device models (media insertion is
// a regular attach/mount, ejection only latches the media-change lines), so no
// BIOS service is intercepted.
class CommandConsole {
public:
    explicit CommandConsole(Machine& machine, bool echo_to_stdout = true);

    // Executes one command line.  Output is appended to log() (and echoed to
    // stdout when enabled).  Returns false when the emulator should quit.
    bool execute(const std::string& line);

    // Appends a line to the console log without echoing it to stdout (used
    // for the typed command echo and for frontend notices).
    void note(std::string_view text);

    // Tab completion: completes the last token in place (command or path).
    void complete(std::string& line);

    [[nodiscard]] bool paused() const { return paused_; }
    [[nodiscard]] bool quit_requested() const { return quit_; }
    [[nodiscard]] const std::deque<std::string>& log() const { return log_; }
    void clear_log() { log_.clear(); }

    [[nodiscard]] static const std::vector<std::string>& commands();
    [[nodiscard]] static std::vector<std::string> tokenize(std::string_view line);

private:
    Machine& machine_;
    bool echo_ = true;
    bool paused_ = false;
    bool quit_ = false;
    std::deque<std::string> log_;

    void emit(std::string_view text); // log + stdout
    void append_line(std::string line);

    void help(const std::vector<std::string>& args);
    void floppy_command(const std::vector<std::string>& args);
    void cdrom_command(const std::vector<std::string>& args);
    void insert_auto(const std::vector<std::string>& args);
    void eject_auto(const std::vector<std::string>& args);
    void images(const std::vector<std::string>& args);
    void key_command(const std::vector<std::string>& args);
    void type_command(const std::string& line);
    void screenshot(const std::vector<std::string>& args);
    void text_dump(const std::vector<std::string>& args);
    void cmos_command(const std::vector<std::string>& args);

    void floppy_status(int drive);
    void insert_floppy(int drive, const std::string& path);
    void eject_floppy(int drive);
    void cdrom_status();
    void insert_cdrom(const std::string& path);
    void eject_cdrom();
    void press_combo(std::string_view combo);
    void boot_reset();
};

} // namespace oldbox
