#include "local_wireless.hpp"

#include <cstdio>
#include <cstring>
#include <memory>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "runtime_internal.hpp"

namespace ntr {

namespace {

constexpr uint32_t kRingSize = 0x10000;
constexpr int kMaxFrameSize = 0x948;
constexpr uint32_t kMagic = 0x4946494E;
constexpr int kRecvTimeoutMs = 25;  // as melonDS: how long to wait for the host's frames, or for replies

struct Header {
    uint32_t magic, sender, type, length;  // type: 0 frame, 1 command, 2 reply (| aid << 16), 3 ack
    uint64_t timestamp;
};

}  // namespace

// The shared memory. Zero (as a new mapping is) is the empty state.
struct LocalWireless::Shared {
    uint32_t claimed;       // slots taken by a running instance
    uint32_t pid[16];       // and its process, to free the slots of instances that quit without saying so
    uint32_t connected;     // instances in a multiplayer session (between MP_Begin and MP_End)
    uint32_t write_offset[2];
    uint32_t host;          // the instance that sent the last command
    uint32_t reply_mask;
    uint8_t ring[2][kRingSize];  // 0: frames, commands and acks for everyone; 1: replies for the host
};

LocalWireless* LocalWireless::get() {
    static std::unique_ptr<LocalWireless> link = [] {
        std::unique_ptr<LocalWireless> l(new LocalWireless);
        if (!l->open()) l.reset();
        return l;
    }();
    return link.get();
}

uint32_t LocalWireless::running() const { return shared_->claimed; }

void LocalWireless::begin() {
    lock();
    read_offset_[0] = shared_->write_offset[0];
    read_offset_[1] = shared_->write_offset[1];
    reset(slot_);
    reset(16 + slot_);
    shared_->connected |= 1u << slot_;
    unlock();
    log("wireless: instance %d joined local wireless (others in it: %04x)\n", slot_,
        shared_->connected & ~(1u << slot_));
}

void LocalWireless::end() {
    lock();
    shared_->connected &= ~(1u << slot_);
    unlock();
    log("wireless: instance %d left local wireless\n", slot_);
}

void LocalWireless::read(int ring, void* buf, uint32_t len) {
    uint32_t& off = read_offset_[ring];
    const uint32_t first = len < kRingSize - off ? len : kRingSize - off;
    std::memcpy(buf, &shared_->ring[ring][off], first);
    std::memcpy(static_cast<uint8_t*>(buf) + first, shared_->ring[ring], len - first);
    off = (off + len) % kRingSize;
}

void LocalWireless::write(int ring, const void* buf, uint32_t len) {
    uint32_t& off = shared_->write_offset[ring];
    const uint32_t first = len < kRingSize - off ? len : kRingSize - off;
    std::memcpy(&shared_->ring[ring][off], buf, first);
    std::memcpy(shared_->ring[ring], static_cast<const uint8_t*>(buf) + first, len - first);
    off = (off + len) % kRingSize;
}

int LocalWireless::send(uint32_t type, uint8_t* data, int len, uint64_t timestamp) {
    if (len > kMaxFrameSize) {
        log("wireless: frame too big (%d bytes)\n", len);
        return 0;
    }
    lock();
    const uint32_t mask = shared_->connected;
    const Header h{kMagic, uint32_t(slot_), type, uint32_t(len), timestamp};
    const int ring = (type & 0xFFFF) == 2 ? 1 : 0;
    write(ring, &h, sizeof h);
    if (len) write(ring, data, uint32_t(len));
    if ((type & 0xFFFF) == 1) {  // a command: replies to it start now
        shared_->host = uint32_t(slot_);
        shared_->reply_mask = 0;
        read_offset_[1] = shared_->write_offset[1];
        reset(16 + slot_);
    } else if ((type & 0xFFFF) == 2) {
        shared_->reply_mask |= 1u << slot_;
    }
    const uint32_t host = shared_->host;
    unlock();
    if ((type & 0xFFFF) == 2) {
        post(16 + int(host));
    } else {
        for (int i = 0; i < 16; i++)
            if (mask & (1u << i)) post(i);
    }
    return len;
}

int LocalWireless::recv(uint8_t* data, bool block, uint64_t* timestamp) {
    for (;;) {
        if (!wait(slot_, block ? kRecvTimeoutMs : 0)) return 0;
        lock();
        Header h{};
        read(0, &h, sizeof h);
        if (h.magic != kMagic) {
            log("wireless: frame ring overflow\n");
            read_offset_[0] = shared_->write_offset[0];
            reset(slot_);
            unlock();
            return 0;
        }
        if (h.sender == uint32_t(slot_)) {  // our own
            read_offset_[0] = (read_offset_[0] + h.length) % kRingSize;
            unlock();
            continue;
        }
        if (h.length) {
            read(0, data, h.length);
            if (h.type == 1) last_host_ = int(h.sender);
        }
        if (timestamp) *timestamp = h.timestamp;
        unlock();
        return int(h.length);
    }
}

int LocalWireless::recv_host_packet(uint8_t* data, uint64_t* timestamp) {
    if (last_host_ != -1 && !(shared_->connected & (1u << last_host_))) return -1;  // the host left
    return recv(data, true, timestamp);
}

uint16_t LocalWireless::recv_replies(uint8_t* data, uint64_t timestamp, uint16_t aidmask) {
    uint16_t ret = 0;
    uint32_t replied = 1u << slot_;
    const uint32_t connected = shared_->connected;
    if ((replied & connected) == connected) return 0;  // nobody else is there
    for (;;) {
        if (!wait(16 + slot_, kRecvTimeoutMs)) return ret;
        lock();
        Header h{};
        read(1, &h, sizeof h);
        if (h.magic != kMagic) {
            log("wireless: reply ring overflow\n");
            read_offset_[1] = shared_->write_offset[1];
            reset(16 + slot_);
            unlock();
            return 0;
        }
        if (h.sender == uint32_t(slot_) || h.timestamp < timestamp - 32) {  // our own, or stale
            read_offset_[1] = (read_offset_[1] + h.length) % kRingSize;
            unlock();
            continue;
        }
        if (h.length) {
            const uint32_t aid = h.type >> 16;
            read(1, &data[(aid - 1) * 1024], h.length);
            ret |= uint16_t(1u << aid);
        }
        replied |= 1u << h.sender;
        unlock();
        if ((replied & connected) == connected || (ret & aidmask) == aidmask) return ret;
    }
}

#ifdef _WIN32

namespace {
const wchar_t* const kName = L"Local\\FFCCEOT-Recomp-wireless-1";

bool process_alive(uint32_t pid) {
    HANDLE p = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!p) return GetLastError() != ERROR_INVALID_PARAMETER;  // (no such process; access denied: it exists)
    const bool alive = WaitForSingleObject(p, 0) == WAIT_TIMEOUT;
    CloseHandle(p);
    return alive;
}
}  // namespace

bool LocalWireless::open() {
    mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Shared), kName);
    if (!mapping_) return false;
    shared_ = static_cast<Shared*>(MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared)));
    if (!shared_) return false;
    wchar_t name[96];
    swprintf(name, 96, L"%ls-lock", kName);
    mutex_ = CreateMutexW(nullptr, FALSE, name);
    if (!mutex_) return false;
    for (int i = 0; i < 32; i++) {
        swprintf(name, 96, L"%ls-sem-%d", kName, i);
        sems_[i] = CreateSemaphoreW(nullptr, 0, 0x7FFFFFFF, name);
        if (!sems_[i]) return false;
    }
    // Take the first free slot; slots of instances that are gone are free.
    lock();
    for (int i = 0; i < 16; i++)
        if ((shared_->claimed & (1u << i)) && !process_alive(shared_->pid[i])) {
            shared_->claimed &= ~(1u << i);
            shared_->connected &= ~(1u << i);
        }
    for (int i = 0; i < 16 && slot_ < 0; i++)
        if (!(shared_->claimed & (1u << i))) {
            slot_ = i;
            shared_->claimed |= 1u << i;
            shared_->pid[i] = GetCurrentProcessId();
        }
    const uint32_t others = shared_->claimed & ~(slot_ >= 0 ? 1u << slot_ : 0u);
    unlock();
    if (slot_ >= 0) log("wireless: instance %d on this PC (others running: %04x)\n", slot_, others);
    return slot_ >= 0;
}

LocalWireless::~LocalWireless() {
    if (shared_ && slot_ >= 0) {
        lock();
        shared_->claimed &= ~(1u << slot_);
        shared_->connected &= ~(1u << slot_);
        unlock();
    }
    for (void* s : sems_)
        if (s) CloseHandle(s);
    if (mutex_) CloseHandle(mutex_);
    if (shared_) UnmapViewOfFile(shared_);
    if (mapping_) CloseHandle(mapping_);
}

void LocalWireless::lock() { WaitForSingleObject(mutex_, INFINITE); }  // (abandoned by a crashed instance: ours now)
void LocalWireless::unlock() { ReleaseMutex(mutex_); }
bool LocalWireless::wait(int sem, int timeout_ms) {
    return WaitForSingleObject(sems_[sem], DWORD(timeout_ms)) == WAIT_OBJECT_0;
}
void LocalWireless::post(int sem) { ReleaseSemaphore(sems_[sem], 1, nullptr); }
void LocalWireless::reset(int sem) {
    while (WaitForSingleObject(sems_[sem], 0) == WAIT_OBJECT_0) {
    }
}

#else  // not yet on other platforms: no link

bool LocalWireless::open() { return false; }
LocalWireless::~LocalWireless() = default;
void LocalWireless::lock() {}
void LocalWireless::unlock() {}
bool LocalWireless::wait(int, int) { return false; }
void LocalWireless::post(int) {}
void LocalWireless::reset(int) {}

#endif

}  // namespace ntr
