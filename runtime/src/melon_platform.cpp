// melonDS Platform interface: host services the emulation core calls back into.
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <thread>

#include "Platform.h"
#include "SPI_Firmware.h"
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

// ---- local wireless (not connected yet) ----

void MP_Begin(void*) {}
void MP_End(void*) {}
int MP_SendPacket(u8*, int, u64, void*) { return 0; }
int MP_RecvPacket(u8*, u64*, void*) { return 0; }
int MP_SendCmd(u8*, int, u64, void*) { return 0; }
int MP_SendReply(u8*, int, u64, u16, void*) { return 0; }
int MP_SendAck(u8*, int, u64, void*) { return 0; }
int MP_RecvHostPacket(u8*, u64*, void*) { return 0; }
u16 MP_RecvReplies(u8*, u64, u16, void*) { return 0; }

// ---- internet (not connected yet) ----

int Net_SendPacket(u8*, int, void*) { return 0; }
int Net_RecvPacket(u8*, void*) { return 0; }

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
