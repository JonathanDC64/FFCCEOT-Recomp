// melonDS Platform interface: host services the emulation core calls back into.
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

#include "Net.h"
#if defined(_WIN32) && !defined(__WIN32__)
#define __WIN32__  // (Net_Slirp.h tells Windows by MinGW's macro)
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "Net_Slirp.h"
#include "Platform.h"
#include "SPI_Firmware.h"
#include "local_wireless.hpp"
#include "runtime_internal.hpp"

namespace melonDS::Platform {

void SignalStop(StopReason reason, void* userdata) { ntr::log("melonDS stop signal (%d)\n", int(reason)); }

// ---- files ----

struct FileHandle {
    std::FILE* f;
};

std::string GetLocalFilePath(const std::string& filename) { return filename; }

FileHandle* OpenFile(const std::string& path, FileMode mode) {
    if ((mode & FileMode::ReadWrite) == FileMode::None) return nullptr;
    bool exists = std::filesystem::exists(path);
    if ((mode & FileMode::NoCreate) && !exists) return nullptr;
    const char* m;
    if (mode & FileMode::Append)
        m = (mode & FileMode::Read) ? "a+b" : "ab";
    else if ((mode & FileMode::Write) && (mode & FileMode::Read))
        m = ((mode & FileMode::Preserve) && exists) ? "r+b" : "w+b";
    else if (mode & FileMode::Write)
        m = ((mode & FileMode::Preserve) && exists) ? "r+b" : "wb";
    else
        m = "rb";
    std::FILE* f = std::fopen(path.c_str(), m);
    if (!f) return nullptr;
    return new FileHandle{f};
}

FileHandle* OpenLocalFile(const std::string& path, FileMode mode) { return OpenFile(path, mode); }
bool FileExists(const std::string& name) { return std::filesystem::exists(name); }
bool LocalFileExists(const std::string& name) { return FileExists(name); }
bool CheckFileWritable(const std::string& filepath) {
    FileHandle* f = OpenFile(filepath, FileMode::Append);
    if (!f) return false;
    CloseFile(f);
    return true;
}
bool CheckLocalFileWritable(const std::string& filepath) { return CheckFileWritable(filepath); }
bool CloseFile(FileHandle* file) {
    bool ok = std::fclose(file->f) == 0;
    delete file;
    return ok;
}
bool IsEndOfFile(FileHandle* file) { return std::feof(file->f) != 0; }
bool FileReadLine(char* str, int count, FileHandle* file) { return std::fgets(str, count, file->f) != nullptr; }
u64 FilePosition(FileHandle* file) { return u64(std::ftell(file->f)); }
bool FileSeek(FileHandle* file, s64 offset, FileSeekOrigin origin) {
    int o = origin == FileSeekOrigin::Start ? SEEK_SET : origin == FileSeekOrigin::Current ? SEEK_CUR : SEEK_END;
    return std::fseek(file->f, long(offset), o) == 0;
}
void FileRewind(FileHandle* file) { std::rewind(file->f); }
u64 FileRead(void* data, u64 size, u64 count, FileHandle* file) { return std::fread(data, size_t(size), size_t(count), file->f); }
bool FileFlush(FileHandle* file) { return std::fflush(file->f) == 0; }
u64 FileWrite(const void* data, u64 size, u64 count, FileHandle* file) {
    return std::fwrite(data, size_t(size), size_t(count), file->f);
}
u64 FileWriteFormatted(FileHandle* file, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = std::vfprintf(file->f, fmt, ap);
    va_end(ap);
    return n < 0 ? 0 : u64(n);
}
u64 FileLength(FileHandle* file) {
    long pos = std::ftell(file->f);
    std::fseek(file->f, 0, SEEK_END);
    long len = std::ftell(file->f);
    std::fseek(file->f, pos, SEEK_SET);
    return u64(len);
}

// ---- logging ----

void Log(LogLevel level, const char* fmt, ...) {
    if (level == LogLevel::Debug) return;
    va_list ap;
    va_start(ap, fmt);
    std::fprintf(stderr, "[melonDS] ");
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
}

// ---- threads ----

struct Thread {
    std::thread t;
};
Thread* Thread_Create(std::function<void()> func) { return new Thread{std::thread(std::move(func))}; }
void Thread_Free(Thread* thread) {
    if (thread->t.joinable()) thread->t.detach();
    delete thread;
}
void Thread_Wait(Thread* thread) {
    if (thread->t.joinable()) thread->t.join();
}

struct Semaphore {
    std::mutex m;
    std::condition_variable cv;
    int count = 0;
};
Semaphore* Semaphore_Create() { return new Semaphore; }
void Semaphore_Free(Semaphore* sema) { delete sema; }
void Semaphore_Reset(Semaphore* sema) {
    std::lock_guard lock(sema->m);
    sema->count = 0;
}
void Semaphore_Wait(Semaphore* sema) {
    std::unique_lock lock(sema->m);
    sema->cv.wait(lock, [&] { return sema->count > 0; });
    sema->count--;
}
bool Semaphore_TryWait(Semaphore* sema, int timeout_ms) {
    std::unique_lock lock(sema->m);
    if (!sema->cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] { return sema->count > 0; })) return false;
    sema->count--;
    return true;
}
void Semaphore_Post(Semaphore* sema, int count) {
    {
        std::lock_guard lock(sema->m);
        sema->count += count;
    }
    sema->cv.notify_all();
}

struct Mutex {
    std::mutex m;
};
Mutex* Mutex_Create() { return new Mutex; }
void Mutex_Free(Mutex* mutex) { delete mutex; }
void Mutex_Lock(Mutex* mutex) { mutex->m.lock(); }
void Mutex_Unlock(Mutex* mutex) { mutex->m.unlock(); }
bool Mutex_TryLock(Mutex* mutex) { return mutex->m.try_lock(); }

void Sleep(u64 usecs) { std::this_thread::sleep_for(std::chrono::microseconds(usecs)); }
u64 GetMSCount() {
    using namespace std::chrono;
    return u64(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}
u64 GetUSCount() {
    using namespace std::chrono;
    return u64(duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
}

// ---- persistence ----

void WriteNDSSave(const u8* savedata, u32 savelen, u32 writeoffset, u32 writelen, void* userdata) {
    if (userdata) static_cast<ntr::RuntimeImpl*>(userdata)->host.on_save(savedata, savelen);
}
void WriteGBASave(const u8*, u32, u32, u32, void*) {}
void WriteFirmware(const Firmware&, u32, u32, void*) {}
void WriteDateTime(int, int, int, int, int, int, void*) {}

// ---- local wireless: with other instances on this PC (local_wireless.hpp) ----

namespace {
ntr::LocalWireless* wireless() { return ntr::LocalWireless::get(); }
}  // namespace

void MP_Begin(void*) {
    if (auto* w = wireless()) w->begin();
}
void MP_End(void*) {
    if (auto* w = wireless()) w->end();
}
int MP_SendPacket(u8* data, int len, u64 timestamp, void*) {
    auto* w = wireless();
    return w ? w->send_packet(data, len, timestamp) : 0;
}
int MP_RecvPacket(u8* data, u64* timestamp, void*) {
    auto* w = wireless();
    return w ? w->recv_packet(data, timestamp) : 0;
}
int MP_SendCmd(u8* data, int len, u64 timestamp, void*) {
    auto* w = wireless();
    return w ? w->send_cmd(data, len, timestamp) : 0;
}
int MP_SendReply(u8* data, int len, u64 timestamp, u16 aid, void*) {
    auto* w = wireless();
    return w ? w->send_reply(data, len, timestamp, aid) : 0;
}
int MP_SendAck(u8* data, int len, u64 timestamp, void*) {
    auto* w = wireless();
    return w ? w->send_ack(data, len, timestamp) : 0;
}
int MP_RecvHostPacket(u8* data, u64* timestamp, void*) {
    auto* w = wireless();
    return w ? w->recv_host_packet(data, timestamp) : 0;
}
u16 MP_RecvReplies(u8* data, u64 timestamp, u16 aidmask, void*) {
    auto* w = wireless();
    return w ? w->recv_replies(data, timestamp, aidmask) : 0;
}

// ---- internet: through melonDS's user-mode NAT (libslirp), started when the game first uses it ----

namespace {
melonDS::Net& internet() {
    static melonDS::Net net;
    static const bool started = [] {
        net.SetDriver(std::make_unique<melonDS::Net_Slirp>([](const u8* data, int len) { net.RXEnqueue(data, len); }));
        net.RegisterInstance(0);
        ntr::log("net: the console's Wi-Fi reaches the internet through a user-mode NAT\n");
        return true;
    }();
    (void)started;
    return net;
}

// --net-log: what an Ethernet frame to or from the console is, if worth a line (DNS, TCP connections).
void log_frame(const char* dir, const u8* f, int len) {
    if (len < 34 || f[12] != 0x08 || f[13] != 0x00) return;  // IPv4 only
    static const auto start = std::chrono::steady_clock::now();
    const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();  // seconds
    const u8* ip = f + 14;
    const int ihl = (ip[0] & 15) * 4, proto = ip[9];
    const u8* l4 = ip + ihl;
    const int l4_len = std::min(len - 14, (ip[2] << 8) | ip[3]) - ihl;  // (without Ethernet padding)
    char src[16], dst[16];
    std::snprintf(src, sizeof src, "%u.%u.%u.%u", ip[12], ip[13], ip[14], ip[15]);
    std::snprintf(dst, sizeof dst, "%u.%u.%u.%u", ip[16], ip[17], ip[18], ip[19]);
    const auto port = [&](int at) { return (l4[at] << 8) | l4[at + 1]; };
    if (proto == 17 && l4_len >= 8 && (port(0) == 53 || port(2) == 53)) {
        // DNS: the question's name, and the A records of an answer.
        const u8* d = l4 + 8;
        const int n = l4_len - 8;
        if (n < 12) return;
        std::string name;
        int at = 12;
        while (at < n && d[at] && d[at] < 64) {
            if (!name.empty()) name += '.';
            name.append(reinterpret_cast<const char*>(d + at + 1), std::min<int>(d[at], n - at - 1));
            at += d[at] + 1;
        }
        at += 5;
        const bool answer = d[2] & 0x80;
        std::string addrs;
        for (int i = 0, count = (d[6] << 8) | d[7]; answer && i < count && at + 12 <= n; i++) {
            while (at < n && d[at] && d[at] < 64) at += d[at] + 1;  // a name, or
            at += (at < n && d[at] >= 0xC0) ? 2 : 1;                // a pointer to one
            if (at + 10 > n) break;
            const int type = (d[at] << 8) | d[at + 1], rdlen = (d[at + 8] << 8) | d[at + 9];
            at += 10;
            if (type == 1 && rdlen == 4 && at + 4 <= n) {
                char a[20];
                std::snprintf(a, sizeof a, " %u.%u.%u.%u", d[at], d[at + 1], d[at + 2], d[at + 3]);
                addrs += a;
            }
            at += rdlen;
        }
        if (answer)
            ntr::log("net %s %.3f: DNS %s ->%s (rcode %d, from %s)\n", dir, t, name.c_str(), addrs.empty() ? " nothing" : addrs.c_str(),
                     d[3] & 15, src);
        else
            ntr::log("net %s %.3f: DNS lookup %s (server %s)\n", dir, t, name.c_str(), dst);
    } else if (proto == 6 && l4_len >= 20) {
        const int flags = l4[13];
        const int data_len = l4_len - ((l4[12] >> 4) * 4);
        if (flags & 0x02)
            ntr::log("net %s %.3f: TCP %s %s:%d -> %s:%d\n", dir, t, (flags & 0x10) ? "SYN-ACK" : "SYN", src, port(0), dst, port(2));
        else if (flags & 0x05)
            ntr::log("net %s %.3f: TCP %s %s:%d -> %s:%d\n", dir, t, (flags & 0x04) ? "RST" : "FIN", src, port(0), dst, port(2));
        else if (data_len == 0 && (port(0) == 80 || port(2) == 80))  // (acks of the connection test)
            ntr::log("net %s %.3f: TCP ACK %u %s:%d -> %s:%d\n", dir, t,
                     unsigned((l4[8] << 24) | (l4[9] << 16) | (l4[10] << 8) | l4[11]), src, port(0), dst, port(2));
        else if (data_len > 0) {
            // GameSpy's presence server (GPCM, port 29900) and plain HTTP (the connection test) are text: show it.
            std::string text;
            if (port(0) == 29900 || port(2) == 29900 || port(0) == 80 || port(2) == 80) {
                const u8* p = l4 + (l4[12] >> 4) * 4;
                for (int i = 0; i < data_len && i < 600; i++) text += (p[i] >= 0x20 && p[i] < 0x7F) ? char(p[i]) : '.';
            }
            ntr::log("net %s %.3f: TCP %d bytes %s:%d -> %s:%d %s\n", dir, t, data_len, src, port(0), dst, port(2), text.c_str());
        }
    } else if (proto == 17 && l4_len >= 8) {
        std::string text;  // GameSpy's query/report protocol (QR2, port 27900): mostly key/value text
        if (port(0) == 27900 || port(2) == 27900)
            for (int i = 8; i < l4_len && i < 608; i++) text += (l4[i] >= 0x20 && l4[i] < 0x7F) ? char(l4[i]) : '.';
        ntr::log("net %s %.3f: UDP %d bytes %s:%d -> %s:%d %s\n", dir, t, l4_len - 8, src, port(0), dst, port(2), text.c_str());
    }
}

bool net_log(void* userdata) { return userdata && static_cast<ntr::RuntimeImpl*>(userdata)->options.net_log; }
}  // namespace

// --net-log: HTTPS requests are encrypted on the wire, but the game builds them as text in main RAM first. On a
// TCP segment sent, log the HTTP requests and NAS answers (returncd=...) found there when they changed.
void log_https_request(ntr::RuntimeImpl& rt, const u8* f, int len) {
    if (len < 54 || f[23] != 6) return;
    static std::string last;
    const u8* ram = rt.nds->MainRAM;
    const size_t size = 4u << 20;
    std::string found;
    for (size_t i = 0; i + 5 < size; i++) {
        if (std::memcmp(ram + i, "POST ", 5) != 0 && std::memcmp(ram + i, "GET /", 5) != 0 &&
            std::memcmp(ram + i, "returncd=", 9) != 0)
            continue;
        size_t end = i;
        while (end < size && end - i < 1024 && ram[end] && (ram[end] >= 0x20 || ram[end] == '\r' || ram[end] == '\n'))
            end++;
        std::string text(reinterpret_cast<const char*>(ram + i), end - i);
        for (char& c : text)
            if (c == '\r') c = ' ';
        found += "  @" + std::to_string(i) + ": " + text + "\n";
        i = end;
    }
    if (found != last) ntr::log("net: HTTP request texts in RAM:\n%s", found.c_str());
    last = found;
}

int Net_SendPacket(u8* data, int len, void* userdata) {
    if (net_log(userdata)) {
        log_frame("out", data, len);
        log_https_request(*static_cast<ntr::RuntimeImpl*>(userdata), data, len);
    }
    return internet().SendPacket(data, len, 0);
}
int Net_RecvPacket(u8* data, void* userdata) {
    const int len = internet().RecvPacket(data, 0);
    if (len > 0 && net_log(userdata)) log_frame("in", data, len);
    return len;
}

// ---- peripherals the game does not use ----

void Camera_Start(int, void*) {}
void Camera_Stop(int, void*) {}
void Camera_CaptureFrame(int, u32*, int, int, bool, void*) {}
void Mic_Start(void*) {}
void Mic_Stop(void*) {}
int Mic_ReadInput(s16*, int, void*) { return 0; }
AACDecoder* AAC_Init() { return nullptr; }
void AAC_DeInit(AACDecoder*) {}
bool AAC_Configure(AACDecoder*, int, int) { return false; }
bool AAC_DecodeFrame(AACDecoder*, const void*, int, void*, int) { return false; }
bool Addon_KeyDown(KeyType, void*) { return false; }
void Addon_RumbleStart(u32, void*) {}
void Addon_RumbleStop(void*) {}
float Addon_MotionQuery(MotionQueryType, void*) { return 0.0f; }
DynamicLibrary* DynamicLibrary_Load(const char*) { return nullptr; }
void DynamicLibrary_Unload(DynamicLibrary*) {}
void* DynamicLibrary_LoadFunction(DynamicLibrary*, const char*) { return nullptr; }

}  // namespace melonDS::Platform
