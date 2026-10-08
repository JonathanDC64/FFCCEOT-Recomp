#include "instance_lan.hpp"

#include <algorithm>
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

#include "local_wireless.hpp"

namespace ntr {

namespace {

constexpr uint32_t kNatIp = 0x0A400010;  // the address melonDS's NAT (Net_Slirp) gives the console: 10.64.0.16
constexpr int kQueueFrames = 32;
constexpr int kMaxFrame = 1536;

uint32_t get32(const uint8_t* p) { return uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
void put32(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v >> 24);
    p[1] = uint8_t(v >> 16);
    p[2] = uint8_t(v >> 8);
    p[3] = uint8_t(v);
}
void replace32(uint8_t* p, uint32_t from, uint32_t to) {
    if (get32(p) == from) put32(p, to);
}

uint32_t sum16(const uint8_t* p, int n, uint32_t s = 0) {
    for (int i = 0; i + 1 < n; i += 2) s += uint32_t(p[i]) << 8 | p[i + 1];
    if (n & 1) s += uint32_t(p[n - 1]) << 8;
    return s;
}
uint16_t fold(uint32_t s) {
    while (s >> 16) s = (s & 0xFFFF) + (s >> 16);
    return uint16_t(~s);
}

// The MAC this LAN gives another instance's console in its ARP replies. Frames are routed by IP address, and the
// receiving instance puts its console's own MAC in.
void peer_mac(int slot, uint8_t* mac) {
    const uint8_t m[6] = {0x02, 'F', 'F', 'C', 'C', uint8_t(slot)};
    std::memcpy(mac, m, 6);
}

}  // namespace

// The shared memory: a queue of frames for each instance. Zero (as a new mapping is) is the empty state.
struct InstanceLan::Shared {
    struct Queue {
        uint32_t head, tail;  // frames are taken at head, added at tail
        struct {
            uint32_t len;
            uint8_t data[kMaxFrame];
        } frames[kQueueFrames];
    } queues[16];
};

InstanceLan* InstanceLan::get() {
    static std::unique_ptr<InstanceLan> lan = [] {
        LocalWireless* wireless = LocalWireless::get();
        std::unique_ptr<InstanceLan> l(new InstanceLan);
        if (!wireless || !l->open(wireless->slot())) l.reset();
        return l;
    }();
    return lan.get();
}

bool InstanceLan::is_peer(uint32_t ip) const {
    if (ip < kNatIp || ip >= kNatIp + 16 || ip == ip_) return false;
    LocalWireless* wireless = LocalWireless::get();
    return wireless && (wireless->running() & (1u << (ip - kNatIp)));
}

bool InstanceLan::outgoing(uint8_t* f, int len) {
    if (len < 42) return false;
    std::memcpy(mac_, f + 6, 6);
    const int type = f[12] << 8 | f[13];
    if (type == 0x0806 && (f[20] << 8 | f[21]) == 1 && is_peer(get32(f + 38))) {
        // An ARP request for another instance's console: answer it.
        std::vector<uint8_t> r(60, 0);
        std::memcpy(&r[0], f + 6, 6);
        peer_mac(int(get32(f + 38) - kNatIp), &r[6]);
        const uint8_t head[10] = {0x08, 0x06, 0x00, 0x01, 0x08, 0x00, 6, 4, 0x00, 0x02};
        std::memcpy(&r[12], head, 10);
        std::memcpy(&r[22], &r[6], 6);   // sender: that console
        std::memcpy(&r[28], f + 38, 4);
        std::memcpy(&r[32], f + 22, 10);  // target: ours (MAC and address)
        answers_.push_back(std::move(r));
        return true;
    }
    if (type == 0x0800) {
        const uint32_t dst = get32(f + 30);
        if (dst >= kNatIp && dst < kNatIp + 16 && dst != ip_) {
            // For another instance's console (dropped if there is none at that address).
            if (is_peer(dst)) push(int(dst - kNatIp), f, len);
            return true;
        }
    }
    if (ip_ != kNatIp) translate(f, len, ip_, kNatIp, true);
    return false;
}

void InstanceLan::from_nat(uint8_t* f, int len) {
    if (ip_ != kNatIp) translate(f, len, kNatIp, ip_, false);
}

// Replaces the console's address `from` with `to` where the NAT sees it (to_nat) or the console does: ARP addresses,
// IP source or destination, and the DHCP fields that carry it. Checksums are recomputed.
void InstanceLan::translate(uint8_t* f, int len, uint32_t from, uint32_t to, bool to_nat) {
    const int type = f[12] << 8 | f[13];
    if (type == 0x0806) {
        replace32(f + (to_nat ? 28 : 38), from, to);  // sender address going out, target address coming in
        return;
    }
    if (type != 0x0800 || len < 34) return;
    uint8_t* ip = f + 14;
    const int ihl = (ip[0] & 15) * 4;
    const int total = std::min(len - 14, ip[2] << 8 | ip[3]);
    if (ihl < 20 || total < ihl) return;
    replace32(ip + (to_nat ? 12 : 16), from, to);
    uint8_t* l4 = ip + ihl;
    const int l4_len = total - ihl;
    const int proto = ip[9];
    if (proto == 17 && l4_len >= 8) {
        const int sport = l4[0] << 8 | l4[1];
        uint8_t* dhcp = l4 + 8;
        const int n = l4_len - 8;
        if ((sport == 67 || sport == 68) && n >= 240) {
            replace32(dhcp + 12, from, to);  // ciaddr
            replace32(dhcp + 16, from, to);  // yiaddr
            for (int at = 240; at + 1 < n && dhcp[at] != 255;) {  // option 50: the requested address
                if (dhcp[at] == 0) {
                    at++;
                    continue;
                }
                if (dhcp[at] == 50 && dhcp[at + 1] == 4 && at + 6 <= n) replace32(dhcp + at + 2, from, to);
                at += 2 + dhcp[at + 1];
            }
        }
    }
    ip[10] = ip[11] = 0;
    const uint16_t ip_sum = fold(sum16(ip, ihl));
    ip[10] = uint8_t(ip_sum >> 8);
    ip[11] = uint8_t(ip_sum);
    const int sum_at = proto == 6 ? 16 : proto == 17 ? 6 : -1;
    if (sum_at < 0 || l4_len < sum_at + 2) return;
    if (proto == 17 && l4[6] == 0 && l4[7] == 0) return;  // (UDP without a checksum)
    l4[sum_at] = l4[sum_at + 1] = 0;
    uint32_t s = sum16(ip + 12, 8) + uint32_t(proto) + uint32_t(l4_len);
    uint16_t sum = fold(sum16(l4, l4_len, s));
    if (proto == 17 && sum == 0) sum = 0xFFFF;
    l4[sum_at] = uint8_t(sum >> 8);
    l4[sum_at + 1] = uint8_t(sum);
}

int InstanceLan::incoming(uint8_t* f) {
    if (!answers_.empty()) {
        const std::vector<uint8_t>& r = answers_.front();
        std::memcpy(f, r.data(), r.size());
        const int len = int(r.size());
        answers_.pop_front();
        return len;
    }
    Shared::Queue& q = shared_->queues[slot_];
    if (q.head == q.tail) return 0;
    lock();
    int len = 0;
    if (q.head != q.tail) {
        const auto& fr = q.frames[q.head % kQueueFrames];
        len = int(fr.len);
        std::memcpy(f, fr.data, size_t(len));
        q.head++;
    }
    unlock();
    if (len >= 14) std::memcpy(f, mac_, 6);  // to this console
    return len;
}

void InstanceLan::push(int slot, const uint8_t* f, int len) {
    if (len > kMaxFrame) return;
    lock();
    Shared::Queue& q = shared_->queues[slot];
    if (q.tail - q.head < uint32_t(kQueueFrames)) {  // (full: dropped, as on a busy network)
        auto& fr = q.frames[q.tail % kQueueFrames];
        fr.len = uint32_t(len);
        std::memcpy(fr.data, f, size_t(len));
        q.tail++;
    }
    unlock();
}

#ifdef _WIN32

namespace {
const wchar_t* const kName = L"Local\\FFCCEOT-Recomp-lan-1";
}

bool InstanceLan::open(int slot) {
    if (slot < 0 || slot >= 16) return false;
    mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Shared), kName);
    if (!mapping_) return false;
    shared_ = static_cast<Shared*>(MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared)));
    if (!shared_) return false;
    mutex_ = CreateMutexW(nullptr, FALSE, L"Local\\FFCCEOT-Recomp-lan-1-lock");
    if (!mutex_) return false;
    slot_ = slot;
    ip_ = kNatIp + uint32_t(slot);
    lock();
    shared_->queues[slot].head = shared_->queues[slot].tail;  // (whatever a previous instance in this slot left)
    unlock();
    return true;
}

InstanceLan::~InstanceLan() {
    if (mutex_) CloseHandle(mutex_);
    if (shared_) UnmapViewOfFile(shared_);
    if (mapping_) CloseHandle(mapping_);
}

void InstanceLan::lock() { WaitForSingleObject(mutex_, INFINITE); }
void InstanceLan::unlock() { ReleaseMutex(mutex_); }

#else  // not yet on other platforms

bool InstanceLan::open(int) { return false; }
InstanceLan::~InstanceLan() = default;
void InstanceLan::lock() {}
void InstanceLan::unlock() {}

#endif

}  // namespace ntr
