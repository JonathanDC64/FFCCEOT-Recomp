#include "crash_report.hpp"

#ifdef _WIN32

#include <windows.h>
// (after windows.h)
#include <dbghelp.h>

#include <csignal>
#include <cstdarg>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace crash_report {

namespace {

HMODULE g_module = nullptr;  // ffcc.exe: crashes outside it (e.g. inside a GPU driver) may be handled there
char g_stem[64];
volatile LONG g_reported = 0;

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
    char path[96];
    std::snprintf(path, sizeof path, "%s.dmp", g_stem);
    const HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
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
    char path[96];
    std::snprintf(path, sizeof path, "%s.txt", g_stem);
    Report out;
    fopen_s(&out.file, path, "w");
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
    out("report: %s.txt, minidump: %s.dmp\n", g_stem, g_stem);
    std::fflush(stderr);
    if (out.file) std::fclose(out.file);
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
    return EXCEPTION_CONTINUE_SEARCH;
}

void on_abort(int) {
    CONTEXT context{};
    RtlCaptureContext(&context);
    report("abort (see the message above, e.g. \"fatal: ...\")", &context, nullptr);
}

}  // namespace

void install() {
    g_module = GetModuleHandleA(nullptr);
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    SymInitialize(GetCurrentProcess(), nullptr, TRUE);
    AddVectoredExceptionHandler(1, on_exception);
    SetUnhandledExceptionFilter(on_unhandled);
    std::signal(SIGABRT, on_abort);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);  // no "abort() has been called" dialog
}

}  // namespace crash_report

#else

namespace crash_report {
void install() {}
}  // namespace crash_report

#endif
