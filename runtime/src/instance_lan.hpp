// A LAN between instances of the game on this PC, for the Wi-Fi Connection. Each instance's console reaches the
// internet through its own user-mode NAT, and those all hand out the same address (10.64.0.16). Two consoles behind one
// public address connect to each other's private address (DWC: "behind same NAT as me"), so each instance's console
// gets one of its own, 10.64.0.(16 + slot), and frames for another instance's address go to it through shared memory.
//
// Toward the NAT, the console's address is translated to the one the NAT gave out (ARP, IP, DHCP), so the NAT works
// as before; instance 0 needs no translation. ARP requests for another instance's address are answered here.
#pragma once

#include <cstdint>
#include <deque>
#include <vector>

namespace ntr {

class InstanceLan {
public:
    // The process's LAN, or nullptr where there is none (other platforms, or no local wireless slot).
    static InstanceLan* get();

    // A frame the console sends. True if it was taken here (for another instance, or answered); otherwise it goes to
    // the NAT, and has been translated to the NAT's view of the console.
    bool outgoing(uint8_t* frame, int len);
    // A frame for the console from another instance or from here: its length, or 0 if there is none.
    int incoming(uint8_t* frame);
    // A frame from the NAT: translated to the console's address.
    void from_nat(uint8_t* frame, int len);

    ~InstanceLan();

private:
    InstanceLan() = default;
    bool open(int slot);
    bool is_peer(uint32_t ip) const;
    void translate(uint8_t* frame, int len, uint32_t from, uint32_t to, bool to_nat);
    void push(int slot, const uint8_t* frame, int len);
    void lock();
    void unlock();

    struct Shared;
    Shared* shared_ = nullptr;
    void* mapping_ = nullptr;
    void* mutex_ = nullptr;
    int slot_ = -1;
    uint32_t ip_ = 0;                            // the console's address
    uint8_t mac_[6] = {};                        // and MAC (as its frames show it)
    std::deque<std::vector<uint8_t>> answers_;  // ARP replies made here
};

}  // namespace ntr
