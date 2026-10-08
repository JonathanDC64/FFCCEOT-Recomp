// Local wireless between instances of the game on this PC: the DS Wi-Fi frames that melonDS's Wi-Fi emulation sends
// and receives in multiplayer (Platform::MP_*) go through a block of shared memory, so two windows can play together.
// The protocol is melonDS's LocalMP (net/LocalMP.cpp), which does the same between consoles in one process: a ring
// of frames that every instance reads, a ring of replies to the host's commands, and a semaphore per instance and
// ring to wake it. Each instance takes a slot (0-15), which also gives its console a MAC address of its own.
#pragma once

#include <cstdint>

namespace ntr {

class LocalWireless {
public:
    // The process's link, or nullptr where there is none (other platforms, or the shared memory could not be made).
    static LocalWireless* get();

    int slot() const { return slot_; }
    uint32_t running() const;  // the slots taken by running instances (bit i: slot i)

    // melonDS's MP interface, for this instance.
    void begin();
    void end();
    int send_packet(uint8_t* data, int len, uint64_t timestamp) { return send(0, data, len, timestamp); }
    int recv_packet(uint8_t* data, uint64_t* timestamp) { return recv(data, false, timestamp); }
    int send_cmd(uint8_t* data, int len, uint64_t timestamp) { return send(1, data, len, timestamp); }
    int send_reply(uint8_t* data, int len, uint64_t timestamp, uint16_t aid) {
        return send(2 | (uint32_t(aid) << 16), data, len, timestamp);
    }
    int send_ack(uint8_t* data, int len, uint64_t timestamp) { return send(3, data, len, timestamp); }
    int recv_host_packet(uint8_t* data, uint64_t* timestamp);
    uint16_t recv_replies(uint8_t* data, uint64_t timestamp, uint16_t aidmask);

    ~LocalWireless();

private:
    LocalWireless() = default;
    bool open();
    int send(uint32_t type, uint8_t* data, int len, uint64_t timestamp);
    int recv(uint8_t* data, bool block, uint64_t* timestamp);
    void lock();
    void unlock();
    bool wait(int sem, int timeout_ms);
    void post(int sem);
    void reset(int sem);
    void read(int ring, void* buf, uint32_t len);
    void write(int ring, const void* buf, uint32_t len);

    struct Shared;
    Shared* shared_ = nullptr;
    void* mapping_ = nullptr;
    void* mutex_ = nullptr;
    void* sems_[32] = {};  // 0-15: frames for instance i; 16-31: replies for host i
    int slot_ = -1;
    uint32_t read_offset_[2] = {};  // where this instance reads each ring
    int last_host_ = -1;
};

}  // namespace ntr
