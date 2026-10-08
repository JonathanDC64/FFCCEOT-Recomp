// Game-specific glue for hooks listed in game/config/cfie/hooks.txt.
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
