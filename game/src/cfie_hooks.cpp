// Game-specific glue for hooks listed in game/config/cfie/hooks.txt.
#include <algorithm>

#include "cfie_game.hpp"
#include "ntr/nitro.h"
#include "ntr/recomp.h"

namespace cfie {
GameEvents events;
}  // namespace cfie

namespace {
constexpr uint32_t kOsIrqTable = 0x027E0000;            // OS_IRQTable (DTCM)
constexpr uint32_t kOsIrqHandlerThreadSwitch = 0x01FF8190;  // OS_IrqHandler_ThreadSwitch (ITCM)
constexpr uint32_t kErrno = 0x02103AFC;                     // the C library's errno
constexpr uint32_t kGuestSendAllowance = 0x0212F8A8 + 68;  // how many more inputs the host lets a guest send, less one
constexpr uint32_t kOnlineTiming = 0x0212FD28;              // the online timing table (see cfie_set_online_timing)
}  // namespace

extern "C" void cfie_os_irq_handler(NtrCpu* c) { ntr::nitro::irq_handler(c, kOsIrqTable, kOsIrqHandlerThreadSwitch); }

// 0x0207713C: start background music (r1 = track id, 0 = stop).
extern "C" void cfie_on_bgm_play(NtrCpu* c) {
    if (c->r[1] != 0) cfie::events.title_bgm_started = true;
}

// 0x02005BBC: the Wi-Fi Connection test (DWC netcheck). It converts the HTTP status with atoi and fails if errno is
// ERANGE, but errno is never cleared: the stats server's session key (\sesskey\, read with atoi at 0x02040D02)
// overflows on today's revival servers, and every connection after the first then fails with error 52200.
extern "C" void cfie_on_netcheck(NtrCpu* c) { ntr_wr32(c, kErrno, 0); }

// 0x020ADDBC: how many of the guest's queued inputs (one per game frame, 30 a second) to send to the host now: at most
// 3, and no more than the host allows. Online (Wi-Fi Connection) the game sends nothing until it has 3, so an input
// waits up to 100 ms before it leaves, and the guest sees the result only with the host's reply to it. Over local
// wireless it sends whatever it has every frame; this does the same online.
extern "C" void cfie_guest_inputs_to_send(NtrCpu* c) {
    int32_t n = std::min(int32_t(ntr_rd32(c, c->r[0] + 108)), 3);
    n = std::min(n, int32_t(ntr_rd32(c, kGuestSendAllowance)) + 1);
    c->r[0] = uint32_t(n);
}

// 0x020AE314: fills the timing table for online play (r1: one of two presets, 0 or 1). Over local wireless the host
// holds each guest's inputs 1-2 game frames and runs one frame at a time. Online it holds them (ping / 30 ms) + 5
// frames, at least 5 (3 in preset 1), trimmed only 2 past that, and runs and replies 3 frames at a time, so a guest sees
// its own moves some 200 ms late. Here: ping + 1 frame, trimmed 1 past it, one frame at a time. The buffer still grows
// with the ping, as on a slow connection it must; between two instances on a PC it holds 0-1 inputs.
extern "C" void cfie_set_online_timing(NtrCpu* c) {
    const uint32_t preset = c->r[1];
    const bool second = preset == 1;
    const int32_t fields[] = {
        1,                // +84: the host trims a guest's queued inputs once they are this many over the target
        -1,               // +88: down to this many under it
        second ? 2 : 0,   // +92, +96: when to tell guests to wait (unchanged)
        second ? 0 : -2,  //
        1,                // +100: frames run (and replied) at a time; the game's own value is 3
        1,                // +104: the smallest target; 5 (3 in preset 1)
        20,               // +108: the largest target
        1,                // +112: the target over the ping; 5 (3 in preset 1)
    };
    for (int i = 0; i < 8; i++) ntr_wr32(c, kOnlineTiming + 84 + 4 * i, uint32_t(fields[i]));
    ntr_wr32(c, kOnlineTiming + 116, preset);
}

