// Game-specific glue for hooks listed in game/config/cfie/hooks.txt.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cmath>
#include <cstring>

#include "cfie_game.hpp"
#include "ntr/nitro.h"
#include "ntr/recomp.h"

namespace cfie {
GameEvents events;
OnlineBuffer online_buffer = OnlineBuffer::Low;
bool fast_saving = true;
AnalogMovement analog_movement = AnalogMovement::SinglePlayer;
uint16_t aspect = 0x1555;
Stick stick;
bool log_sounds = false;
}  // namespace cfie

namespace {
constexpr uint32_t kOsIrqTable = 0x027E0000;            // OS_IRQTable (DTCM)
constexpr uint32_t kOsIrqHandlerThreadSwitch = 0x01FF8190;  // OS_IrqHandler_ThreadSwitch (ITCM)
constexpr uint32_t kErrno = 0x02103AFC;                     // the C library's errno
constexpr uint32_t kGuestSendAllowance = 0x0212F8A8 + 68;  // how many more inputs the host lets a guest send, less one
constexpr uint32_t kOnlineTiming = 0x0212FD28;              // the online timing table (see cfie_set_online_timing)
constexpr uint32_t kNetwork = 0x0212F700;                   // the game's network state (+2: 0 alone, 2 online)
constexpr uint32_t kNetworkIsOnline = 0x020AD998;           // (network): the Wi-Fi Connection is up
constexpr uint32_t kCardCommon = 0x020FEE40;                // the card library's common state (cardi_common)
constexpr uint32_t kPlayers = 0x02122E98;                   // the players (572 bytes each; +328: their character)
constexpr uint32_t kPlayerPresent = 0x02099780;             // (players, n): player n is in the game
constexpr uint32_t kMoveYaw = 0x02057220;                   // (character): the camera's yaw its movement turns by
constexpr uint32_t kSinCosTable = 0x020CD780;               // FX_SinCosTable_: sin, cos (4.12) per 16 angle units
constexpr uint32_t kAnimationStep = 0x0206F1C8;             // (character): 1, or 1.5 and 0.5 under Haste and Slow
constexpr uint32_t kRunAnimation = 0x52554E20;              // 'RUN ', at a character's +364 while it plays
constexpr uint32_t kStartSound = 0x020B2B10;                // (_, id): the sound manager starts sound effect `id`
constexpr uint32_t kBlankInputCall = 0x020ADF81;            // return address: a slot nobody's records came in for

// Menu sounds: a small ring of requests from the frontend, started by cfie_sound_update.
struct SoundRequest {
    std::atomic<uint16_t> id{0};
    std::atomic<int64_t> at{0};  // steady-clock milliseconds
};
constexpr uint32_t kSoundQueue = 16;
constexpr int64_t kSoundMaxAgeMs = 300;
SoundRequest sound_queue[kSoundQueue];
std::atomic<uint32_t> sound_head{0}, sound_tail{0};

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// Analog movement: the stick each player slot's input records carry (see cfie_pack_input), the same on every console.
struct SlotStick {
    bool marked = false;  // the records come from this build
    uint8_t angle = 0;    // 256 steps, 0 = down, 64 = right
    uint8_t push = 0;     // how far it is pushed, 0 (at rest) to kFullPush
};
constexpr uint8_t kFullPush = 63;
SlotStick slot_sticks[4];

// The stick of player slot `player` (a character's +172), or null if the D-pad moves it as on the DS.
const SlotStick* moving_stick(int32_t player) {
    if (player < 0 || player >= 4 || slot_sticks[player].push == 0) return nullptr;
    return &slot_sticks[player];
}

// Whether this console sends its stick: as set, and in a network session only while every player's console reads it
// (a DS or another emulator would move the character by the D-pad bits and fall out of step).
bool send_stick(NtrCpu* c) {
    if (cfie::analog_movement == cfie::AnalogMovement::Off) return false;
    if (ntr_rd8(c, kNetwork + 2) == 0) return true;
    if (cfie::analog_movement != cfie::AnalogMovement::Multiplayer) return false;
    const uint32_t lr = c->r[14], r0 = c->r[0], r1 = c->r[1], r2 = c->r[2], r3 = c->r[3];
    bool all = true;
    for (uint32_t n = 0; n < 4 && all; n++) {
        c->r[0] = kPlayers;
        c->r[1] = n;
        ntr_call(c, kPlayerPresent | 1);
        all = c->r[0] == 0 || slot_sticks[n].marked;
    }
    c->r[0] = r0, c->r[1] = r1, c->r[2] = r2, c->r[3] = r3, c->r[14] = lr;
    return all;
}
}  // namespace

extern "C" void cfie_os_irq_handler(NtrCpu* c) { ntr::nitro::irq_handler(c, kOsIrqTable, kOsIrqHandlerThreadSwitch); }

namespace cfie {
void queue_sound(uint16_t id) {
    const uint32_t head = sound_head.load(std::memory_order_relaxed);
    if (head - sound_tail.load(std::memory_order_acquire) >= kSoundQueue) return;  // full: this one is dropped
    sound_queue[head % kSoundQueue].id.store(id, std::memory_order_relaxed);
    sound_queue[head % kSoundQueue].at.store(now_ms(), std::memory_order_relaxed);
    sound_head.store(head + 1, std::memory_order_release);
}
}  // namespace cfie

// 0x02076A94 (sound manager): its update, once per pass of the main loop. Starts the queued menu sounds.
extern "C" void cfie_sound_update(NtrCpu* c) {
    uint32_t tail = sound_tail.load(std::memory_order_relaxed);
    if (tail == sound_head.load(std::memory_order_acquire)) return;
    const uint32_t r0 = c->r[0], r1 = c->r[1], r2 = c->r[2], r3 = c->r[3], lr = c->r[14];
    const int64_t now = now_ms();
    while (tail != sound_head.load(std::memory_order_acquire)) {
        const SoundRequest& q = sound_queue[tail % kSoundQueue];
        const uint16_t id = q.id.load(std::memory_order_relaxed);
        const bool fresh = now - q.at.load(std::memory_order_relaxed) <= kSoundMaxAgeMs;
        sound_tail.store(++tail, std::memory_order_release);
        if (!fresh || id == 0) continue;
        c->r[0] = 0;
        c->r[1] = id;
        ntr_call(c, kStartSound | 1);
    }
    c->r[0] = r0, c->r[1] = r1, c->r[2] = r2, c->r[3] = r3, c->r[14] = lr;
}

// 0x02076FD0: the sound manager starts a sound effect (r1 = the manager, r2 = its id).
extern "C" void cfie_on_sound_start(NtrCpu* c) {
    if (cfie::log_sounds) std::fprintf(stderr, "sound: start %u\n", c->r[2]);
}

// 0x0207713C: start background music (r1 = track id, 0 = stop).
extern "C" void cfie_on_bgm_play(NtrCpu* c) {
    if (c->r[1] != 0) cfie::events.title_bgm_started = true;
}

// 0x02082A40: pack a tick's input record (out, buttons, touch: x, y, touching): buttons (bit 13: touching), then the
// touch x and y. While the screen isn't touched no console reads x and y (0x02077CB4 keeps the last touched point),
// and a DS never sends y over 191 (TP_GetCalibratedPoint clamps it), so this build sends its stick there: x = its
// angle, y = 192 + how far it is pushed. A DS still sees the D-pad bits; y = 192 alone marks a console that reads them.
extern "C" int cfie_pack_input(NtrCpu* c) {
    const uint32_t out = c->r[0], touch = c->r[2];
    const bool touching = ntr_rd8(c, touch + 2) != 0;
    const uint32_t buttons = ntr_rd16(c, c->r[1]) | (touching ? 0x2000 : 0);
    uint8_t x = ntr_rd8(c, touch), y = ntr_rd8(c, touch + 1);
    if (!touching) {
        x = 0;
        y = 192;
        const cfie::Stick s = cfie::stick;
        if (const float length = std::hypot(s.x, s.y); length > 0 && send_stick(c)) {
            const long push = std::lround(std::min(length, 1.0f) * kFullPush);
            x = uint8_t(std::lround(std::atan2(s.x, s.y) * (128 / 3.14159265358979)) & 0xFF);
            y = uint8_t(192 + push);
        }
    }
    ntr_wr8(c, out, uint8_t(buttons));
    ntr_wr8(c, out + 1, uint8_t(buttons >> 8));
    ntr_wr8(c, out + 2, x);
    ntr_wr8(c, out + 3, y);
    return 1;
}

// 0x020AF120: apply player slot r1's input record (r0) for this tick, on every console for every slot. Keeps the
// stick it carries; while the player touches the screen, the last one.
extern "C" void cfie_apply_input(NtrCpu* c) {
    const uint32_t slot = c->r[1], record = c->r[0];
    if (slot >= 4) return;
    SlotStick& s = slot_sticks[slot];
    if (c->r[14] == kBlankInputCall) {
        s = {};
        return;
    }
    if (ntr_rd8(c, record + 1) & 0x20) return;
    const uint8_t y = ntr_rd8(c, record + 3);
    s.marked = y >= 192;
    s.angle = s.marked ? ntr_rd8(c, record + 2) : 0;
    s.push = s.marked ? uint8_t(std::min(y - 192, int(kFullPush))) : 0;
}

// 0x02056498: a character's movement (velocity out, buttons, speed, player slot): the D-pad's direction (one of
// eight, length 1), turned by the camera's yaw, times the speed. Before it, the game speeds the character up over 8
// ticks while the D-pad is held. With a stick in the player's records the character goes the stick's way instead, at
// the speed times how far it is pushed. Fixed point only, so every console moves it the same.
extern "C" int cfie_analog_move(NtrCpu* c) {
    const int32_t player = int8_t(c->r[3]);
    const SlotStick* s = moving_stick(player);
    if (!s || !(c->r[1] & 0xF0)) return 0;
    const uint32_t out = c->r[0];
    const int64_t speed = int64_t(int32_t(c->r[2])) * s->push / kFullPush;
    // The stick's direction (x = sin, z = cos of its angle), z towards the bottom of the screen (the D-pad's Down).
    const uint32_t a = uint32_t(s->angle) << 4;
    const int64_t x = int16_t(ntr_rd16(c, kSinCosTable + a * 4)), z = int16_t(ntr_rd16(c, kSinCosTable + a * 4 + 2));

    const uint32_t lr = c->r[14];
    c->r[0] = ntr_rd32(c, kPlayers + 572 * uint32_t(player) + 328);
    ntr_call(c, kMoveYaw | 1);
    c->r[14] = lr;
    const uint32_t i = (c->r[0] & 0xFFFF) >> 4;
    const int64_t sin = int16_t(ntr_rd16(c, kSinCosTable + i * 4)), cos = int16_t(ntr_rd16(c, kSinCosTable + i * 4 + 2));
    // As MTX_RotY33 and MTX_MultVec33, then FX_Mul by the speed.
    const int64_t vx = (x * cos + z * sin) >> 12, vz = (z * cos - x * sin) >> 12;
    ntr_wr32(c, out, uint32_t((vx * speed + 0x800) >> 12));
    ntr_wr32(c, out + 4, 0);
    ntr_wr32(c, out + 8, uint32_t((vz * speed + 0x800) >> 12));
    c->r[0] = 1;
    return 1;
}

// 0x0206F1BC: how far a player character's animation (r0, +8 +20: the character) goes in a tick (4.12). The run has
// one speed on the DS, so with the stick pushed part way its feet would slide: it plays slower, as much as the
// character moves slower.
// The perspective setup (G3 perspective: r0 = sin and r1 = cos of the half field of view, r2 = the aspect ratio):
// widescreen is the aspect ratio. Its only callers pass the DS's 4:3.
// The geometry engine (third_party/CMakeLists.txt) widens a viewport only while this is counting down (per VBlank), so
// scenes that do not use this projection (the title) are not stretched.
// A scene of orthographic geometry only (the title) stays 4:3 (ffcc_wide_block counts frames).
extern "C" int ffcc_wide_scene;
extern "C" int ffcc_wide_block;
extern "C" void cfie_perspective_aspect(NtrCpu* c) {
    if (c->r[2] == 0x1555 && cfie::aspect != 0x1555 && ffcc_wide_block == 0) {
        c->r[2] = cfie::aspect;
        ffcc_wide_scene = 8;
    }
}

extern "C" int cfie_player_animation_step(NtrCpu* c) {
    const uint32_t character = ntr_rd32(c, ntr_rd32(c, c->r[0] + 8) + 20);
    if (ntr_rd32(c, character + 364) != kRunAnimation) return 0;
    const int32_t player = int8_t(ntr_rd8(c, character + 172));
    const SlotStick* s = moving_stick(player);
    if (!s || s->push >= kFullPush || character != ntr_rd32(c, kPlayers + 572 * uint32_t(player) + 328)) return 0;
    const uint32_t lr = c->r[14];
    c->r[0] = character;
    ntr_call(c, kAnimationStep | 1);
    c->r[14] = lr;
    c->r[0] = uint32_t(int64_t(int32_t(c->r[0])) * s->push / kFullPush);
    return 1;
}

// 0x02025260: CARDi_RequestStreamCommand. The game's save thread reads and writes the save 1 KB at a time, and the
// card library hands each 256-byte page to the ARM7: a save of 25 KB took 3.2 s. Fast saving does it at once.
extern "C" int cfie_card_backup_request(NtrCpu* c) {
    return cfie::fast_saving && ntr::nitro::card_backup_request(c, kCardCommon);
}

// 0x02005BBC: the Wi-Fi Connection test (DWC netcheck). It converts the HTTP status with atoi and fails if errno is
// ERANGE, but errno is never cleared: the stats server's session key (\sesskey\, read with atoi at 0x02040D02)
// overflows on today's revival servers, and every connection after the first then fails with error 52200.
extern "C" void cfie_on_netcheck(NtrCpu* c) { ntr_wr32(c, kErrno, 0); }

// 0x020ADDBC: how many of the guest's queued inputs (one per game frame, 30 a second) to send to the host now: at most
// 3, and no more than the host allows. Online (Wi-Fi Connection) the game sends nothing until it has 3, so an input
// waits up to 100 ms before it leaves, and the guest sees the result only with the host's reply to it. Over local
// wireless it sends whatever it has every frame; this does the same online (unless the game's own timing is chosen).
extern "C" void cfie_guest_inputs_to_send(NtrCpu* c) {
    int32_t n = std::min(int32_t(ntr_rd32(c, c->r[0] + 108)), 3);
    n = std::min(n, int32_t(ntr_rd32(c, kGuestSendAllowance)) + 1);
    if (cfie::online_buffer == cfie::OnlineBuffer::Original && ntr_rd8(c, kNetwork + 2) == 2 && n != 3) {
        const uint32_t lr = c->r[14];
        c->r[0] = kNetwork;
        ntr_call(c, kNetworkIsOnline | 1);
        c->r[14] = lr;
        if (c->r[0] != 0) n = 0;
    }
    c->r[0] = uint32_t(n);
}

// 0x020AE314: fills the timing table for online play (r1: one of two presets, 0 or 1). Over local wireless the host
// holds each guest's inputs 1-2 game frames and runs one frame at a time. Online it holds them (ping / 30 ms) + 5
// frames, at least 5 (3 in preset 1), trimmed only 2 past that, and runs and replies 3 frames at a time, so a guest sees
// its own moves some 200 ms late. Low: ping + 1 frame, trimmed 1 past it, one frame at a time. The buffer still grows
// with the ping, as on a slow connection it must; between two instances on a PC it holds 0-1 inputs. Medium: twice the
// margins, for connections whose packets come late. Original: the game's own table.
extern "C" void cfie_set_online_timing(NtrCpu* c) {
    const uint32_t preset = c->r[1];
    const bool second = preset == 1;
    const int32_t game = second ? 3 : 5;
    const int32_t margin = cfie::online_buffer == cfie::OnlineBuffer::Low ? 1 : 2;
    const bool original = cfie::online_buffer == cfie::OnlineBuffer::Original;
    const int32_t fields[] = {
        original ? 2 : margin,     // +84: the host trims a guest's queued inputs once they are this many over the target
        original ? -2 : -margin,   // +88: down to this many under it
        second ? 2 : 0,            // +92, +96: when to tell guests to wait (unchanged)
        second ? 0 : -2,           //
        original ? 3 : 1,          // +100: frames run (and replied) at a time
        original ? game : margin,  // +104: the smallest target
        20,                        // +108: the largest target
        original ? game : margin,  // +112: the target over the ping
    };
    for (int i = 0; i < 8; i++) ntr_wr32(c, kOnlineTiming + 84 + 4 * i, uint32_t(fields[i]));
    ntr_wr32(c, kOnlineTiming + 116, preset);
}

void* cfie::find_hook(const char* name) {
    static const struct {
        const char* name;
        void* fn;
    } kHooks[] = {
        {"cfie_os_irq_handler", reinterpret_cast<void*>(&cfie_os_irq_handler)},
        {"cfie_on_bgm_play", reinterpret_cast<void*>(&cfie_on_bgm_play)},
        {"cfie_sound_update", reinterpret_cast<void*>(&cfie_sound_update)},
        {"cfie_on_sound_start", reinterpret_cast<void*>(&cfie_on_sound_start)},
        {"cfie_pack_input", reinterpret_cast<void*>(&cfie_pack_input)},
        {"cfie_apply_input", reinterpret_cast<void*>(&cfie_apply_input)},
        {"cfie_analog_move", reinterpret_cast<void*>(&cfie_analog_move)},
        {"cfie_perspective_aspect", reinterpret_cast<void*>(&cfie_perspective_aspect)},
        {"cfie_player_animation_step", reinterpret_cast<void*>(&cfie_player_animation_step)},
        {"cfie_card_backup_request", reinterpret_cast<void*>(&cfie_card_backup_request)},
        {"cfie_on_netcheck", reinterpret_cast<void*>(&cfie_on_netcheck)},
        {"cfie_guest_inputs_to_send", reinterpret_cast<void*>(&cfie_guest_inputs_to_send)},
        {"cfie_set_online_timing", reinterpret_cast<void*>(&cfie_set_online_timing)},
        {"ntr_nitro_save_context", reinterpret_cast<void*>(&ntr_nitro_save_context)},
        {"ntr_nitro_load_context", reinterpret_cast<void*>(&ntr_nitro_load_context)},
        {"ntr_nitro_init_context", reinterpret_cast<void*>(&ntr_nitro_init_context)},
        {"ntr_dsprot_not_detected", reinterpret_cast<void*>(&ntr_dsprot_not_detected)},
    };
    for (const auto& h : kHooks)
        if (std::strcmp(h.name, name) == 0) return h.fn;
    return nullptr;
}
