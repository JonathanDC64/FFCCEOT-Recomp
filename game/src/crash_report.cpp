#include "crash_report.hpp"

#ifdef _WIN32

#include <windows.h>
// (after windows.h)
#include <dbghelp.h>

#include <shellapi.h>

#include <algorithm>
#include <csignal>
#include <cstdarg>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "crash_dialog.hpp"
#include "report.hpp"

namespace crash_report {

namespace {

HMODULE g_module = nullptr;  // ffcc.exe: crashes outside it (e.g. inside a GPU driver) may be handled there
wchar_t g_dir[MAX_PATH];     // where the reports go (with a trailing backslash; empty: the working directory)
char g_stem[64];
char g_what[256];            // what the report says happened
wchar_t g_txt_path[MAX_PATH];
bool g_dialog = false;       // show the crash dialog (a second process) when the game ends
volatile LONG g_reported = 0, g_dialog_shown = 0;

std::wstring path_of(const char* extension) {
    return std::wstring(g_dir) + std::wstring(g_stem, g_stem + std::strlen(g_stem)) +
           std::wstring(extension, extension + std::strlen(extension));
}

void open_stem() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    std::strftime(g_stem, sizeof g_stem, "crash-%Y%m%d-%H%M%S", &tm);
}

// Both to stderr and to the report file.
struct Report {
    FILE* file = nullptr;
    void operator()(const char* fmt, ...) {
        va_list ap, again;
        va_start(ap, fmt);
        va_copy(again, ap);
        std::vfprintf(stderr, fmt, ap);
        if (file) std::vfprintf(file, fmt, again);
        va_end(again);
        va_end(ap);
    }
};

void frame_line(Report& out, HANDLE process, DWORD64 address) {
    alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256];
    auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = 255;
    DWORD64 displacement = 0;
    IMAGEHLP_LINE64 line{};
    line.SizeOfStruct = sizeof line;
    DWORD line_displacement = 0;
    char module[MAX_PATH] = "?";
    const DWORD64 base = SymGetModuleBase64(process, address);
    if (base) {
        GetModuleFileNameA(HMODULE(base), module, sizeof module);
        if (const char* slash = std::strrchr(module, '\\')) std::memmove(module, slash + 1, std::strlen(slash));
    }
    if (SymFromAddr(process, address, &displacement, symbol)) {
        if (SymGetLineFromAddr64(process, address, &line_displacement, &line))
            out("  %016llx %s!%s+0x%llx  (%s:%lu)\n", (unsigned long long)address, module, symbol->Name,
                (unsigned long long)displacement, line.FileName, line.LineNumber);
        else
            out("  %016llx %s!%s+0x%llx\n", (unsigned long long)address, module, symbol->Name,
                (unsigned long long)displacement);
    } else {
        out("  %016llx %s+0x%llx\n", (unsigned long long)address, module, (unsigned long long)(address - base));
    }
}

void write_dump(EXCEPTION_POINTERS* info) {
    const HANDLE file = CreateFileW(path_of(".dmp").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    MINIDUMP_EXCEPTION_INFORMATION exception{GetCurrentThreadId(), info, FALSE};
    MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file,
                      MINIDUMP_TYPE(MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithDataSegs),
                      info ? &exception : nullptr, nullptr, nullptr);
    CloseHandle(file);
}

// Writes the report: what happened, then the stack from `context`.
void report(const char* what, CONTEXT* context, EXCEPTION_POINTERS* info) {
    if (InterlockedExchange(&g_reported, 1)) return;  // one report per run
    open_stem();
    std::snprintf(g_what, sizeof g_what, "%s", what);
    const std::wstring txt = path_of(".txt");
    std::wcsncpy(g_txt_path, txt.c_str(), MAX_PATH - 1);
    Report out;
    _wfopen_s(&out.file, txt.c_str(), L"w");
    {  // (what to start again with: the restart button)
        FILE* cmd = nullptr;
        _wfopen_s(&cmd, path_of(".cmdline").c_str(), L"wb");
        if (cmd) {
            const wchar_t* line = GetCommandLineW();
            std::fwrite(line, sizeof(wchar_t), std::wcslen(line), cmd);
            std::fclose(cmd);
        }
    }
    out("\n=== CRASH: %s ===\n", what);
    const HANDLE process = GetCurrentProcess();
    CONTEXT ctx = *context;
    out("rip %016llx rsp %016llx rbp %016llx\nrax %016llx rbx %016llx rcx %016llx rdx %016llx\n"
        "rsi %016llx rdi %016llx r8  %016llx r9  %016llx\n",
        ctx.Rip, ctx.Rsp, ctx.Rbp, ctx.Rax, ctx.Rbx, ctx.Rcx, ctx.Rdx, ctx.Rsi, ctx.Rdi, ctx.R8, ctx.R9);
    out("stack:\n");
    STACKFRAME64 frame{};
    frame.AddrPC.Offset = ctx.Rip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrStack.Offset = ctx.Rsp;
    frame.AddrStack.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = ctx.Rbp;
    frame.AddrFrame.Mode = AddrModeFlat;
    for (int i = 0; i < 64; i++) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, GetCurrentThread(), &frame, &ctx, nullptr,
                         SymFunctionTableAccess64, SymGetModuleBase64, nullptr) ||
            frame.AddrPC.Offset == 0)
            break;
        frame_line(out, process, frame.AddrPC.Offset);
    }
    write_dump(info);
    out("report: %s.txt, minidump: %s.dmp (in %ls)\n", g_stem, g_stem, g_dir[0] ? g_dir : L"the working directory");
    std::fflush(stderr);
    if (out.file) std::fclose(out.file);
}

// Starts the crash dialog: a second process, since this one is in no state to show a window. It waits for this one to end.
void show_dialog() {
    if (!g_dialog || !g_txt_path[0] || InterlockedExchange(&g_dialog_shown, 1)) return;
    Sleep(150);  // (lets the log writer catch up with the last messages)
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring data = report::data_dir().wstring();
    while (!data.empty() && (data.back() == L'\\' || data.back() == L'/')) data.pop_back();
    std::wstring command = L"\"" + std::wstring(exe) + L"\" --crash-dialog \"" + g_txt_path + L"\" " +
                           std::to_wstring(GetCurrentProcessId()) + L" \"" + data + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof startup;
    PROCESS_INFORMATION process{};
    if (CreateProcessW(exe, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process)) {
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    }
}

const char* describe(DWORD code) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION: return "access violation";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "illegal instruction";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "integer divide by zero";
    case EXCEPTION_PRIV_INSTRUCTION: return "privileged instruction";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "array bounds exceeded";
    case EXCEPTION_IN_PAGE_ERROR: return "in-page error";
    case EXCEPTION_DATATYPE_MISALIGNMENT: return "datatype misalignment";
    default: return nullptr;
    }
}

LONG CALLBACK on_exception(EXCEPTION_POINTERS* info) {
    const EXCEPTION_RECORD& e = *info->ExceptionRecord;
    const char* what = describe(e.ExceptionCode);
    if (!what) return EXCEPTION_CONTINUE_SEARCH;
    // Only faults in our own code: drivers and system DLLs raise and handle some internally.
    HMODULE module = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       static_cast<LPCSTR>(e.ExceptionAddress), &module);
    if (module != g_module) return EXCEPTION_CONTINUE_SEARCH;
    char text[160];
    if (e.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && e.NumberParameters >= 2)
        std::snprintf(text, sizeof text, "%s %s %016llx", what,
                      e.ExceptionInformation[0] == 0 ? "reading" : e.ExceptionInformation[0] == 1 ? "writing" : "executing",
                      (unsigned long long)e.ExceptionInformation[1]);
    else
        std::snprintf(text, sizeof text, "%s (code %08lx)", what, e.ExceptionCode);
    report(text, info->ContextRecord, info);
    return EXCEPTION_CONTINUE_SEARCH;  // the process ends as it would have
}

LONG WINAPI on_unhandled(EXCEPTION_POINTERS* info) {
    char text[64];
    std::snprintf(text, sizeof text, "unhandled exception %08lx", info->ExceptionRecord->ExceptionCode);
    report(text, info->ContextRecord, info);
    show_dialog();
    return EXCEPTION_CONTINUE_SEARCH;
}

void on_abort(int) {
    CONTEXT context{};
    RtlCaptureContext(&context);
    report("abort (see the message above, e.g. \"fatal: ...\")", &context, nullptr);
    show_dialog();
}

}  // namespace

void install(const std::filesystem::path& crash_dir, bool dialog) {
    g_module = GetModuleHandleA(nullptr);
    g_dialog = dialog;
    if (!crash_dir.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(crash_dir, ec);
        std::wstring dir = crash_dir.wstring();
        if (!dir.empty() && dir.back() != L'\\' && dir.back() != L'/') dir += L'\\';
        std::wcsncpy(g_dir, dir.c_str(), MAX_PATH - 1);
    }
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    SymInitialize(GetCurrentProcess(), nullptr, TRUE);
    AddVectoredExceptionHandler(1, on_exception);
    SetUnhandledExceptionFilter(on_unhandled);
    std::signal(SIGABRT, on_abort);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);  // no "abort() has been called" dialog
}

namespace {

struct Dialog {
    std::filesystem::path txt, data;
    DWORD pid = 0;
    std::string what;
    std::filesystem::path zip;
    bool restart = false;
};

std::string read_text(const std::filesystem::path& path) {
    const auto b = report::read_file(path);
    std::string text(b.begin(), b.end());
    text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());  // (the report was written in text mode)
    return text;
}

std::wstring widen(const std::string& s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n : 1, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    w.resize(std::wcslen(w.c_str()));
    return w;
}

// "access violation reading 0000000000000010", from the first line of the report; with the message of a runtime error
// ("fatal: no recompiled function at ...") from the log, if that is what ended the game.
std::string what_happened(const Dialog& d, const std::string& txt) {
    std::string what = "The game stopped unexpectedly.";
    const size_t a = txt.find("=== CRASH: ");
    if (a != std::string::npos) {
        const size_t b = txt.find(" ===", a);
        if (b != std::string::npos) what = txt.substr(a + 11, b - a - 11);
    }
    const std::string log = "\n" + read_text(d.data / "logs" / "ffcc.log");
    const size_t f = log.rfind("\nfatal: ");  // (at the start of a line: the report quotes it too)
    if (f != std::string::npos && what.rfind("abort", 0) == 0) {
        const size_t end = log.find('\n', f + 1);
        what = log.substr(f + 8, end == std::string::npos ? std::string::npos : end - f - 8);
    }
    return what;
}

// The first `n` lines of the stack in the report.
std::string top_of_stack(const std::string& txt, int n) {
    const size_t a = txt.find("stack:\n");
    if (a == std::string::npos) return {};
    std::string out;
    size_t at = a + 7;
    for (int i = 0; i < n && at < txt.size(); i++) {
        const size_t e = txt.find('\n', at);
        out += txt.substr(at, e == std::string::npos ? std::string::npos : e - at + 1);
        if (e == std::string::npos) break;
        at = e + 1;
    }
    return out;
}

// Zips what a bug report needs and opens the issue form, filled in, and the folder with the zip to attach to it.
// The text to show the player afterwards.
std::string make_report(Dialog& d) {
    const std::string txt = report::redact(read_text(d.txt));
    std::vector<report::File> files;
    files.push_back({"crash.txt", report::bytes(txt)});
    files.push_back({"system.txt", report::bytes(report::redact(report::system_info()))});
    if (auto ini = report::read_file(d.data / "settings.ini"); !ini.empty())  // (their game settings)
        files.push_back({"settings.ini", report::bytes(report::redact(std::string(ini.begin(), ini.end())))});
    files.push_back({"ffcc.log", report::bytes(report::redact(report::read_lines(d.data / "logs" / "ffcc.log", 80, 600)))});
    d.zip = report::write_bundle(d.txt.stem().string(), files);

    std::string body = "## What happened\n(Please say what you were doing when the game crashed.)\n\n## Crash\n" + d.what +
                       "\n\n```\n" + top_of_stack(txt, 20) + "```\n\n## System\n```\n" +
                       report::redact(report::system_info()) + "```\n\n## Attachments\n";
    body += d.zip.empty() ? "(The report files could not be saved.)\n"
                          : "Drag `" + d.zip.filename().string() + "` (its folder is open) into this issue: it has the full "
                            "stack, the game settings and the game's recent messages.\n";
    report::open_url(report::issue_url("Crash: " + d.what.substr(0, 80), body));
    if (!d.zip.empty()) report::reveal(d.zip);
    return d.zip.empty() ? "The report files could not be saved. The issue form is open in your browser."
                         : "The issue form is open in your browser, with the folder holding " + d.zip.filename().string() +
                               ". Drag that file into the issue to attach the details: the full stack, your game settings "
                               "and the game's recent messages. Thank you!";
}

void restart(const Dialog& d) {
    if (HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, d.pid)) {  // (the crashed process must be gone first)
        WaitForSingleObject(h, 10000);
        CloseHandle(h);
    }
    auto path = d.txt;
    path.replace_extension(".cmdline");
    const auto b = report::read_file(path);
    if (b.size() < sizeof(wchar_t)) return;
    std::wstring command(reinterpret_cast<const wchar_t*>(b.data()), b.size() / sizeof(wchar_t));
    STARTUPINFOW startup{};
    startup.cb = sizeof startup;
    PROCESS_INFORMATION process{};
    if (CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process)) {
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    }
}

}  // namespace

int run_dialog() {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv || argc < 5) return 2;
    Dialog d;
    d.txt = argv[2];
    d.pid = DWORD(std::wcstoul(argv[3], nullptr, 10));
    d.data = argv[4];
    report::set_data_dir(d.data);
    const std::string txt = read_text(d.txt);
    d.what = what_happened(d, txt);

    crash_dialog::Content content{d.what, top_of_stack(txt, 14)};
    crash_dialog::Actions actions;
    actions.report = [&d] { return make_report(d); };
    actions.restart = [&d] { restart(d); };
    if (!crash_dialog::show(content, actions)) {
        // (no window to be had: a plain message box)
        const std::wstring text = widen(d.what) + L"\n\nA report was saved in " + d.txt.parent_path().wstring() +
                                  L".\nTo report it, attach the files in that folder to a new issue at " +
                                  widen(report::kRepositoryUrl);
        MessageBoxW(nullptr, text.c_str(), L"Echoes of Time has crashed", MB_OK | MB_ICONERROR);
    }
    return 0;
}

}  // namespace crash_report

#else

namespace crash_report {
void install(const std::filesystem::path&, bool) {}
int run_dialog() { return 0; }
}  // namespace crash_report

#endif
