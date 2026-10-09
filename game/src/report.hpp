// Problem reports: what a bug report needs (version, system, settings, recent log, a screenshot or a crash trace),
// zipped for attaching to a GitHub issue, and the issue form opened with its text filled in. Used by the crash
// dialog (crash_report.cpp) and the settings menu's "Report an issue" button.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace report {

constexpr const char* kRepositoryUrl = "https://github.com/JonathanDC64/FFCCEOT-Recomp";

struct File {
    std::string name;  // in the zip
    std::vector<uint8_t> data;
};

// The folder of this player's data (settings.ini, saves, logs, reports); set once at start.
void set_data_dir(const std::filesystem::path& dir);
const std::filesystem::path& data_dir();

// "0.1.0 (a1b2c3d)".
std::string version();
// The operating system, processor, memory, graphics adapters and displays, one fact per line.
std::string system_info();
// Hides the user's name, profile folder and computer name from text that is going to be posted.
std::string redact(std::string text);

// The program is a windowed one (no console window of its own when started from the desktop). Started from a terminal, this
// ties its output to that terminal again, so the options and messages still show there. Windows only; call first.
void attach_parent_console();

// Sends this process's standard output and error to ffcc.log in the data dir as well as where they went (the previous
// run's log is kept as ffcc.prev.log). Windows only.
void start_log();
std::filesystem::path log_path();
// The head (`head` lines) and tail (`tail` lines) of a text file, with a note where lines were left out.
std::string read_lines(const std::filesystem::path& path, size_t head, size_t tail);
std::vector<uint8_t> read_file(const std::filesystem::path& path);
inline std::vector<uint8_t> bytes(const std::string& s) { return {s.begin(), s.end()}; }

// Writes `files` to a new zip in the data dir's reports folder, named `<stem>.zip`; the path, or empty on failure.
std::filesystem::path write_bundle(const std::string& stem, const std::vector<File>& files);
std::string timestamp();  // "20261008-153012", for file names

// The new-issue page of the repository with the title and text filled in (cut to what a link can hold).
std::string issue_url(const std::string& title, const std::string& body);
void open_url(const std::string& url);
// Shows a file in the system's file manager.
void reveal(const std::filesystem::path& path);

}  // namespace report
