#include "report.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <regex>
#include <sstream>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
// (after windows.h)
#include <dxgi.h>
#include <fcntl.h>
#include <io.h>
#include <shellapi.h>
#include <share.h>
#else
#include <sys/utsname.h>
#include <unistd.h>
#endif

#ifndef FFCC_VERSION
#define FFCC_VERSION "unknown"
#endif
#ifndef FFCC_GIT_REV
#define FFCC_GIT_REV ""
#endif

namespace report {

namespace {

std::filesystem::path g_data_dir;
std::filesystem::path g_log_path;

#ifdef _WIN32
std::string narrow(const wchar_t* w, int n = -1) {
    const int size = WideCharToMultiByte(CP_UTF8, 0, w, n, nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string s(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, n, s.data(), size, nullptr, nullptr);
    if (n == -1 && !s.empty()) s.pop_back();  // (the terminator)
    return s;
}

std::wstring widen(const std::string& s) {
    const int size = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (size <= 0) return {};
    std::wstring w(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), size);
    w.pop_back();
    return w;
}
#endif

// Replaces every occurrence of `what` in `text` (ignoring case) with `with`.
void replace_all(std::string& text, const std::string& what, const std::string& with) {
    if (what.size() < 3) return;
    std::string lower = text, needle = what;
    for (char& c : lower) c = char(std::tolower((unsigned char)c));
    for (char& c : needle) c = char(std::tolower((unsigned char)c));
    std::string out;
    size_t at = 0;
    for (size_t f; (f = lower.find(needle, at)) != std::string::npos; at = f + needle.size()) {
        out.append(text, at, f - at);
        out += with;
    }
    out.append(text, at, std::string::npos);
    text = std::move(out);
}

std::string env(const char* name) {
    const char* v = std::getenv(name);
    return v ? v : "";
}

uint32_t crc32(const uint8_t* data, size_t n) {
    static uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        ready = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) c = table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

void put16(std::string& s, uint32_t v) {
    s += char(v & 0xFF);
    s += char((v >> 8) & 0xFF);
}
void put32(std::string& s, uint32_t v) {
    put16(s, v & 0xFFFF);
    put16(s, v >> 16);
}

bool write_zip(const std::filesystem::path& path, const std::vector<File>& files) {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    const uint32_t dos_time = uint32_t(tm.tm_hour << 11 | tm.tm_min << 5 | tm.tm_sec / 2);
    const uint32_t dos_date = uint32_t(std::max(0, tm.tm_year - 80) << 9 | (tm.tm_mon + 1) << 5 | tm.tm_mday);
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    std::string central;
    uint32_t offset = 0;
    for (const File& f : files) {
        const uint32_t crc = crc32(f.data.data(), f.data.size());
        std::string head;
        put32(head, 0x04034B50);
        put16(head, 20);       // version needed
        put16(head, 0x0800);   // names are UTF-8
        put16(head, 0);        // stored
        put16(head, dos_time);
        put16(head, dos_date);
        put32(head, crc);
        put32(head, uint32_t(f.data.size()));
        put32(head, uint32_t(f.data.size()));
        put16(head, uint32_t(f.name.size()));
        put16(head, 0);
        head += f.name;
        out.write(head.data(), std::streamsize(head.size()));
        out.write(reinterpret_cast<const char*>(f.data.data()), std::streamsize(f.data.size()));
        put32(central, 0x02014B50);
        put16(central, 20);
        put16(central, 20);
        put16(central, 0x0800);
        put16(central, 0);
        put16(central, dos_time);
        put16(central, dos_date);
        put32(central, crc);
        put32(central, uint32_t(f.data.size()));
        put32(central, uint32_t(f.data.size()));
        put16(central, uint32_t(f.name.size()));
        put16(central, 0);  // extra
        put16(central, 0);  // comment
        put16(central, 0);  // disk
        put16(central, 0);  // internal attributes
        put32(central, 0);  // external attributes
        put32(central, offset);
        central += f.name;
        offset += uint32_t(head.size() + f.data.size());
    }
    std::string end;
    put32(end, 0x06054B50);
    put16(end, 0);
    put16(end, 0);
    put16(end, uint32_t(files.size()));
    put16(end, uint32_t(files.size()));
    put32(end, uint32_t(central.size()));
    put32(end, offset);
    put16(end, 0);
    out.write(central.data(), std::streamsize(central.size()));
    out.write(end.data(), std::streamsize(end.size()));
    return bool(out);
}

std::string url_encode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += char(c);
        else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

}  // namespace

void set_data_dir(const std::filesystem::path& dir) { g_data_dir = dir; }
const std::filesystem::path& data_dir() { return g_data_dir; }
std::filesystem::path log_path() { return g_log_path; }

std::string version() {
    std::string v = FFCC_VERSION;
    if (FFCC_GIT_REV[0]) v += std::string(" (") + FFCC_GIT_REV + ")";
    return v;
}

std::string timestamp() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y%m%d-%H%M%S", &tm);
    return buf;
}

std::string redact(std::string text) {
    std::string home = env("USERPROFILE");
    if (home.empty()) home = env("HOME");
    replace_all(text, home, "%USERPROFILE%");
    std::string user = env("USERNAME");
    if (user.empty()) user = env("USER");
    replace_all(text, user, "<user>");
    std::string pc = env("COMPUTERNAME");
    if (pc.empty()) pc = env("HOSTNAME");
    replace_all(text, pc, "<pc>");
    // The path of the ROM the player picked (their own folders), and the console name they typed.
    static const std::regex rom_path(R"(([A-Za-z]:[\\/]|/)[^\r\n"<>|]*\.nds)", std::regex::icase);
    text = std::regex_replace(text, rom_path, "<rom path>");
    static const std::regex console_name(R"((^|\n)online\.console_name[^\n]*)");
    text = std::regex_replace(text, console_name, "$1online.console_name = <removed>");
    return text;
}

std::string system_info() {
    std::ostringstream o;
    o << "ffcc " << version() << "\n";
#ifdef _WIN32
    {
        using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
        OSVERSIONINFOW v{};
        v.dwOSVersionInfoSize = sizeof v;
        if (auto fn = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlGetVersion")))
            fn(&v);
        o << "OS: Windows " << v.dwMajorVersion << "." << v.dwMinorVersion << " build " << v.dwBuildNumber << "\n";
    }
    {
        char cpu[128] = "?";
        DWORD size = sizeof cpu;
        RegGetValueA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", "ProcessorNameString",
                     RRF_RT_REG_SZ, nullptr, cpu, &size);
        SYSTEM_INFO si;
        GetNativeSystemInfo(&si);
        o << "CPU: " << cpu << " (" << si.dwNumberOfProcessors << " threads)\n";
        MEMORYSTATUSEX ms{};
        ms.dwLength = sizeof ms;
        if (GlobalMemoryStatusEx(&ms)) o << "Memory: " << (ms.ullTotalPhys >> 20) << " MB\n";
    }
    {
        IDXGIFactory1* factory = nullptr;
        if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) {
            IDXGIAdapter1* adapter = nullptr;
            for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++) {
                DXGI_ADAPTER_DESC1 d{};
                adapter->GetDesc1(&d);
                if (!(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
                    o << "GPU: " << narrow(d.Description) << ", " << (d.DedicatedVideoMemory >> 20) << " MB";
                    LARGE_INTEGER driver{};
                    if (SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &driver)))
                        o << ", driver " << HIWORD(driver.HighPart) << "." << LOWORD(driver.HighPart) << "."
                          << HIWORD(driver.LowPart) << "." << LOWORD(driver.LowPart);
                    o << "\n";
                }
                adapter->Release();
            }
            factory->Release();
        }
    }
    {
        struct Monitors {
            std::ostringstream* o;
            int n = 0;
        } m{&o};
        EnumDisplayMonitors(
            nullptr, nullptr,
            [](HMONITOR mon, HDC, LPRECT, LPARAM p) -> BOOL {
                auto& m = *reinterpret_cast<Monitors*>(p);
                MONITORINFOEXW info{};
                info.cbSize = sizeof info;
                if (!GetMonitorInfoW(mon, &info)) return TRUE;
                DEVMODEW mode{};
                mode.dmSize = sizeof mode;
                EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode);
                *m.o << "Display " << ++m.n << ": " << (info.rcMonitor.right - info.rcMonitor.left) << "x"
                     << (info.rcMonitor.bottom - info.rcMonitor.top) << " at " << mode.dmDisplayFrequency << " Hz"
                     << ((info.dwFlags & MONITORINFOF_PRIMARY) ? " (primary)" : "") << "\n";
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&m));
    }
#else
    {
        // The text after `sep` on the first line of `file` that starts with `key` ("PRETTY_NAME=", "model name").
        const auto field = [](const char* file, const char* key, char sep) -> std::string {
            std::ifstream f(file);
            std::string line;
            while (std::getline(f, line)) {
                if (line.rfind(key, 0) != 0) continue;
                const size_t at = line.find(sep);
                if (at == std::string::npos) continue;
                std::string v = line.substr(at + 1);
                v.erase(0, v.find_first_not_of(" \t\"'"));
                v.erase(v.find_last_not_of(" \t\"'\r") + 1);
                return v;
            }
            return {};
        };
        std::string os = field("/etc/os-release", "PRETTY_NAME", '=');
        utsname u{};
        if (uname(&u) == 0) os += std::string(os.empty() ? "" : ", ") + u.sysname + " " + u.release;
        o << "OS: " << (os.empty() ? "Linux" : os) << "\n";
        if (std::getenv("APPIMAGE")) o << "Package: AppImage\n";
        o << "CPU: " << field("/proc/cpuinfo", "model name", ':') << " (" << std::thread::hardware_concurrency()
          << " threads)\n";
        const std::string mem = field("/proc/meminfo", "MemTotal", ':');
        if (!mem.empty()) o << "Memory: " << (std::strtoull(mem.c_str(), nullptr, 10) >> 10) << " MB\n";
        // (the graphics card and driver are in the log: the program prints them when it opens its window)
    }
#endif
    return o.str();
}

std::vector<uint8_t> read_file(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    return f ? std::vector<uint8_t>(std::istreambuf_iterator<char>(f), {}) : std::vector<uint8_t>{};
}

std::string read_lines(const std::filesystem::path& path, size_t head, size_t tail) {
    std::ifstream f(path);
    std::vector<std::string> lines;
    for (std::string line; std::getline(f, line);) lines.push_back(line);
    std::string out;
    for (size_t i = 0; i < lines.size(); i++) {
        if (lines.size() > head + tail && i == head) {
            out += "[... " + std::to_string(lines.size() - head - tail) + " lines left out ...]\n";
            i = lines.size() - tail - 1;
            continue;
        }
        out += lines[i] + "\n";
    }
    return out;
}

std::filesystem::path write_bundle(const std::string& stem, const std::vector<File>& files) {
    if (g_data_dir.empty()) return {};
    std::error_code ec;
    const auto dir = g_data_dir / "reports";
    std::filesystem::create_directories(dir, ec);
    const auto path = dir / (stem + ".zip");
    return write_zip(path, files) ? path : std::filesystem::path{};
}

std::string issue_url(const std::string& title, const std::string& body) {
    // (GitHub refuses very long links, and browsers cut them: keep the encoded text under about 7000 bytes)
    std::string text = body;
    while (url_encode(text).size() > 6500 && text.size() > 200) {
        text.resize(text.size() * 9 / 10);
        text += "\n[...cut: the attached report has the rest]";
    }
    return std::string(kRepositoryUrl) + "/issues/new?title=" + url_encode(title) + "&body=" + url_encode(text);
}

void open_url(const std::string& url) {
#ifdef _WIN32
    ShellExecuteW(nullptr, L"open", widen(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
    std::system(("xdg-open '" + url + "' >/dev/null 2>&1 &").c_str());
#endif
}

void reveal(const std::filesystem::path& path) {
#ifdef _WIN32
    const std::wstring args = L"/select,\"" + path.wstring() + L"\"";
    ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
#else
    open_url("file://" + path.parent_path().string());
#endif
}

#ifdef _WIN32
namespace {

HANDLE g_console_out = nullptr;  // where the output was going
FILE* g_log_file = nullptr;

DWORD WINAPI tee_thread(LPVOID pipe) {
    char buf[4096];
    DWORD n;
    while (ReadFile(static_cast<HANDLE>(pipe), buf, sizeof buf, &n, nullptr) && n) {
        DWORD written;
        if (g_console_out && g_console_out != INVALID_HANDLE_VALUE) WriteFile(g_console_out, buf, n, &written, nullptr);
        if (g_log_file) {
            std::fwrite(buf, 1, n, g_log_file);
            std::fflush(g_log_file);
        }
    }
    return 0;
}

}  // namespace

void attach_parent_console() {
    auto valid = [](DWORD id) {
        const HANDLE h = GetStdHandle(id);
        return h && h != INVALID_HANDLE_VALUE;
    };
    // Redirected to a file or a pipe (tests, scripts): the handles are there already.
    if (valid(STD_OUTPUT_HANDLE) && valid(STD_ERROR_HANDLE)) return;
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) return;  // started from the desktop: no console, and none wanted
    FILE* f = nullptr;
    if (!valid(STD_OUTPUT_HANDLE) && freopen_s(&f, "CONOUT$", "w", stdout) == 0)
        SetStdHandle(STD_OUTPUT_HANDLE, reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(stdout))));
    if (!valid(STD_ERROR_HANDLE) && freopen_s(&f, "CONOUT$", "w", stderr) == 0)
        SetStdHandle(STD_ERROR_HANDLE, reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(stderr))));
    if (!valid(STD_INPUT_HANDLE) && freopen_s(&f, "CONIN$", "r", stdin) == 0)
        SetStdHandle(STD_INPUT_HANDLE, reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(stdin))));
}

void start_log() {
    if (g_data_dir.empty()) return;
    std::error_code ec;
    const auto dir = g_data_dir / "logs";
    std::filesystem::create_directories(dir, ec);
    auto path = dir / "ffcc.log";
    std::filesystem::rename(path, dir / "ffcc.prev.log", ec);
    g_log_file = _wfsopen(path.c_str(), L"wb", _SH_DENYWR);
    if (!g_log_file) {
        // Another instance running at the same time keeps ffcc.log: this one writes its own, which its problem report
        // quotes. (Those of finished instances are cleared away after a day.)
        const auto now = std::filesystem::file_time_type::clock::now();
        for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
            const std::wstring name = e.path().filename().wstring();
            if (name.rfind(L"ffcc.", 0) == 0 && name != L"ffcc.log" && name != L"ffcc.prev.log" &&
                now - e.last_write_time(ec) > std::chrono::hours(24))
                std::filesystem::remove(e.path(), ec);
        }
        path = dir / (L"ffcc." + std::to_wstring(GetCurrentProcessId()) + L".log");
        g_log_file = _wfsopen(path.c_str(), L"wb", _SH_DENYWR);
    }
    if (!g_log_file) return;
    HANDLE read = nullptr, write = nullptr;
    if (!CreatePipe(&read, &write, nullptr, 0)) {
        std::fclose(g_log_file);
        g_log_file = nullptr;
        return;
    }
    HANDLE console = GetStdHandle(STD_ERROR_HANDLE);
    if (!console || console == INVALID_HANDLE_VALUE) console = GetStdHandle(STD_OUTPUT_HANDLE);
    // A copy of its own: the _dup2 calls below close the C runtime's descriptors, and with them the handle that was
    // here, whose number the next duplicate of the pipe's end can take (the tee would then write into its own pipe, and
    // read it again, without end).
    g_console_out = nullptr;
    if (console && console != INVALID_HANDLE_VALUE &&
        !DuplicateHandle(GetCurrentProcess(), console, GetCurrentProcess(), &g_console_out, 0, FALSE, DUPLICATE_SAME_ACCESS))
        g_console_out = nullptr;
    const int fd = _open_osfhandle(reinterpret_cast<intptr_t>(write), _O_BINARY);
    if (fd < 0) return;
    std::fflush(stdout);
    std::fflush(stderr);
    _dup2(fd, 1);
    _dup2(fd, 2);
    SetStdHandle(STD_OUTPUT_HANDLE, write);
    SetStdHandle(STD_ERROR_HANDLE, write);
    // A program without a console (started from the desktop: a windowed program) has no stdout and stderr to begin with:
    // the C runtime marks the streams as closed (file descriptor -2), and printf writes nothing even with descriptors 1
    // and 2 on the pipe. Open the streams (on NUL) so that they have descriptors, and put the pipe on those.
    if (_fileno(stdout) < 0) std::freopen("NUL", "w", stdout);
    if (_fileno(stderr) < 0) std::freopen("NUL", "w", stderr);
    if (_fileno(stdout) >= 0) _dup2(fd, _fileno(stdout));
    if (_fileno(stderr) >= 0) _dup2(fd, _fileno(stderr));
    // Unbuffered: the C runtime on Windows treats _IOLBF as fully buffered, so with 4 KB of buffer nothing the program
    // prints ever reached the log file (a problem report quoted an empty log) until 4 KB had been written.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    CloseHandle(CreateThread(nullptr, 0, tee_thread, read, 0, nullptr));
    g_log_path = path;
}
#else
namespace {

int g_console_fd = -1;  // where the output was going
FILE* g_log_file = nullptr;

void tee_thread(int pipe_fd) {
    char buf[4096];
    for (ssize_t n; (n = read(pipe_fd, buf, sizeof buf)) > 0;) {
        for (ssize_t done = 0; done < n;) {
            const ssize_t w = write(g_console_fd, buf + done, size_t(n - done));
            if (w <= 0) break;
            done += w;
        }
        if (g_log_file) {
            std::fwrite(buf, 1, size_t(n), g_log_file);
            std::fflush(g_log_file);
        }
    }
}

}  // namespace

void attach_parent_console() {}  // (a terminal is the console already)

void start_log() {
    if (g_data_dir.empty()) return;
    std::error_code ec;
    const auto dir = g_data_dir / "logs";
    std::filesystem::create_directories(dir, ec);
    const auto path = dir / "ffcc.log";
    std::filesystem::rename(path, dir / "ffcc.prev.log", ec);
    g_log_file = std::fopen(path.string().c_str(), "wb");
    if (!g_log_file) return;
    int fds[2];
    if (pipe(fds) != 0) {
        std::fclose(g_log_file);
        g_log_file = nullptr;
        return;
    }
    g_console_fd = dup(2);
    std::fflush(stdout);
    std::fflush(stderr);
    dup2(fds[1], 1);
    dup2(fds[1], 2);
    close(fds[1]);
    std::setvbuf(stdout, nullptr, _IOLBF, 4096);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    std::thread(tee_thread, fds[0]).detach();
    g_log_path = path;
}
#endif

}  // namespace report
