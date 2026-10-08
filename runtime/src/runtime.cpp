// Core of the runtime: guest memory, the system scheduler, IRQ delivery, CPU modes and guest threads.
//
// Execution model: the recompiled ARM9 code is the master. It counts ARM9 cycles in NtrCpu::cycles (the same
// time base as melonDS's ARM9Timestamp) and calls ntr_poll() once that passes the next scheduled event. poll()
// then lets melonDS catch up: timers, the 3D engine, the ARM7 interpreter and every scheduled event up to now.
// IRQs are delivered by emulating the BIOS IRQ entry at a high level on top of the recompiled game handler.
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "Args.h"
#include "GPU.h"
#include "GPU_OpenGL.h"
#include "GPU_Soft.h"
#include "NDSCart.h"
#include "SPU.h"
#include "ntr/rom.hpp"
#include "runtime_internal.hpp"

namespace ntr {

using melonDS::CPUExecuteMode;

void log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
    std::fflush(stderr);
}

[[noreturn]] void fatal(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::fprintf(stderr, "fatal: ");
    std::vfprintf(stderr, fmt, ap);
    std::fprintf(stderr, "\n");
    va_end(ap);
    std::fflush(stderr);
    std::abort();
}

static bool is_token(u32 pc) { return (pc & kFiberTokenMask) == kFiberTokenBase; }

// ---------------------------------------------------------------------------------------------------------------
// Setup

std::unique_ptr<Runtime> Runtime::create(std::vector<uint8_t> rom, const GameCode& code, Host& host,
                                         RuntimeOptions options) {
    return std::make_unique<RuntimeImpl>(std::move(rom), code, host, std::move(options));
}

RuntimeImpl::RuntimeImpl(std::vector<uint8_t> rom, const GameCode& code, Host& h, RuntimeOptions opts)
    : host(h), options(std::move(opts)) {
    build_function_index(code);
    if (options.trace_files) {
        const Rom r = Rom::from_bytes(rom);
        for (const NitroFile& f : r.files()) rom_spans_.push_back({f.offset, f.offset + f.size, f.path});
        for (const OverlayEntry& ov : r.overlays9()) {
            const auto bytes = r.fat_file(ov.file_id);
            const u32 off = u32(bytes.data() - r.bytes().data());
            rom_spans_.push_back({off, off + u32(bytes.size()), "overlay " + std::to_string(ov.id)});
        }
    }

    melonDS::NDSArgs args;
    args.JIT = std::nullopt;
    args.OutputSampleRate = 48000.0;
    nds = std::make_unique<RecompNDS>(std::move(args), this);

    melonDS::NDSCart::NDSCartArgs cart_args;
    if (!options.save_data.empty()) {
        cart_args.SRAMLength = u32(options.save_data.size());
        cart_args.SRAM = std::make_unique<u8[]>(options.save_data.size());
        std::memcpy(cart_args.SRAM.get(), options.save_data.data(), options.save_data.size());
    }
    auto cart = melonDS::NDSCart::ParseROM(rom.data(), u32(rom.size()), this, std::move(cart_args));
    if (!cart) fatal("melonDS could not parse the ROM");
    nds->SetNDSCart(std::move(cart));
    nds->Reset();
    nds->SetupDirectBoot("game.nds");
    nds->Start();
    apply_video(options.video);

    read_pages_.assign(1u << 20, nullptr);
    write_pages_.assign(1u << 20, nullptr);
    cpu.read_pages = read_pages_.data();
    cpu.write_pages = write_pages_.data();
    cpu.runtime = this;
    map_pages();

    // ARM9 state after the firmware hands over to the cartridge (see melonDS NDS::SetupDirectBoot).
    const u32 entry = nds->NDSCartSlot.GetCart()->GetHeader().ARM9EntryAddress;
    cpu.cpsr_ctrl = 0xD3;
    cpu.r[12] = entry;
    cpu.r[13] = 0x03002F7C;
    cpu.r[14] = entry;
    bank_usr_[0] = 0x03003FC0;
    bank_irq_[0] = 0x03003F80;
    bank_svc_[2] = bank_irq_[2] = bank_abt_[2] = bank_und_[2] = bank_fiq_[7] = 0x10;

    cpu.cycles = int64_t(nds->ARM9Timestamp);
    nds->GPU.StartFrame();
    nds->ARM9Target = nds->next_target() << nds->ARM9ClockShift;
    cpu.next_event = int64_t(nds->ARM9Target);

    fibers_.push_back({nullptr, entry, true, false});
}

RuntimeImpl::~RuntimeImpl() {
    if (!options.profile_path.empty()) {
        if (FILE* f = std::fopen(options.profile_path.c_str(), "w")) {
            std::fprintf(f, "# address first_frame last_frame calls (%llu frames)\n", (unsigned long long)frames_);
            std::vector<std::pair<u32, FuncProfile>> v(profile_.begin(), profile_.end());
            std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.first < b.first; });
            for (auto& [a, p] : v)
                std::fprintf(f, "%08x %llu %llu %llu\n", a, (unsigned long long)p.first, (unsigned long long)p.last,
                             (unsigned long long)p.count);
            std::fclose(f);
        }
    }
    if (!interp_stats_.empty()) {
        std::vector<std::pair<u32, InterpStat>> v(interp_stats_.begin(), interp_stats_.end());
        std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second.steps > b.second.steps; });
        log("interpreter coverage: %zu entry points, %llu calls, %llu instructions\n", v.size(),
            (unsigned long long)interp_calls_, (unsigned long long)interp_steps_);
        for (auto& [a, st] : v)
            log("  %08x  %10llu calls  %12llu instructions\n", a, (unsigned long long)st.calls, (unsigned long long)st.steps);
    }
    for (GuestFiber& f : fibers_)
        if (f.fiber) fiber_destroy(f.fiber);
}

void RuntimeImpl::build_function_index(const GameCode& code) {
    for (size_t i = 0; i < code.function_count; i++) functions_[code.functions[i].addr].push_back(&code.functions[i]);
    for (size_t i = 0; i < code.overlay_count; i++) {
        const NtrOverlayInfo& ov = code.overlays[i];
        if (ov.id >= overlays_by_id_.size()) overlays_by_id_.resize(ov.id + 1, nullptr);
        overlays_by_id_[ov.id] = &ov;
        if (ov.flags & NTR_OVERLAY_INTERPRET) interpreted_.push_back(&ov);
    }
}

void RuntimeImpl::map_pages() {
    std::fill(read_pages_.begin(), read_pages_.end(), nullptr);
    melonDS::NDS& n = *nds;
    // Main RAM, mirrored across 0x02000000-0x02FFFFFF.
    for (u32 a = 0x02000000; a < 0x03000000; a += 0x1000) read_pages_[a >> 12] = n.MainRAM + (a & n.MainRAMMask);
    // DTCM and ITCM take priority over the bus (as in ARMv5::DataRead32).
    melonDS::ARMv5& arm9 = n.ARM9;
    if (arm9.DTCMMask) {
        u32 size = ~arm9.DTCMMask + 1;
        for (u32 off = 0; off < size; off += 0x1000)
            read_pages_[(arm9.DTCMBase + off) >> 12] = arm9.DTCM + (off & (melonDS::DTCMPhysicalSize - 1));
    }
    for (u32 a = 0; a < std::min<u32>(arm9.ITCMSize, 0x02000000); a += 0x1000)
        read_pages_[a >> 12] = arm9.ITCM + (a & (melonDS::ITCMPhysicalSize - 1));
    write_pages_ = read_pages_;
    cpu.read_pages = read_pages_.data();
    cpu.write_pages = write_pages_.data();
}

// ---------------------------------------------------------------------------------------------------------------
// Memory slow paths (I/O, VRAM, WRAM, BIOS, ...)

u32 RuntimeImpl::bus_read32(u32 a) {
    nds->ARM9Timestamp = u64(cpu.cycles);
    return nds->ARM9Read32(a);
}
u32 RuntimeImpl::bus_read16(u32 a) {
    nds->ARM9Timestamp = u64(cpu.cycles);
    return nds->ARM9Read16(a);
}
u32 RuntimeImpl::bus_read8(u32 a) {
    nds->ARM9Timestamp = u64(cpu.cycles);
    return nds->ARM9Read8(a);
}
void RuntimeImpl::bus_write32(u32 a, u32 v) {
    if (options.trace_files && (a & ~0xFu) == 0x040001A0u) trace_cart_write(a, v, 4);
    nds->ARM9Timestamp = u64(cpu.cycles);
    nds->ARM9Write32(a, v);
    sync_after_io(a);
}
void RuntimeImpl::bus_write16(u32 a, u32 v) {
    if (options.trace_files && (a & ~0xFu) == 0x040001A0u) trace_cart_write(a, v, 2);
    nds->ARM9Timestamp = u64(cpu.cycles);
    nds->ARM9Write16(a, u16(v));
    sync_after_io(a);
}
void RuntimeImpl::bus_write8(u32 a, u32 v) {
    if (options.trace_files && (a & ~0xFu) == 0x040001A0u) trace_cart_write(a, v, 1);
    nds->ARM9Timestamp = u64(cpu.cycles);
    nds->ARM9Write8(a, u8(v));
    sync_after_io(a);
}

void RuntimeImpl::trace_cart_write(u32 a, u32 v, u32 size) {
    bool start = false;
    for (u32 i = 0; i < size; i++) {
        const u32 b = a + i;
        const u8 byte = u8(v >> (8 * i));
        if (b >= 0x040001A8u && b < 0x040001B0u) cart_cmd_[b - 0x040001A8u] = byte;
        if (b == 0x040001A7u && (byte & 0x80)) start = true;  // ROMCTRL bit 31: start the transfer
    }
    if (!start || cart_cmd_[0] != 0xB7) return;
    const u32 off = u32(cart_cmd_[1]) << 24 | u32(cart_cmd_[2]) << 16 | u32(cart_cmd_[3]) << 8 | cart_cmd_[4];
    std::string name = "?";
    for (const RomSpan& s : rom_spans_)
        if (off >= s.begin && off < s.end) {
            name = s.name;
            break;
        }
    if (name == last_traced_file_) return;
    last_traced_file_ = name;
    log("frame %llu: cart read %08x  %s\n", (unsigned long long)frames_, off, name.c_str());
}

void RuntimeImpl::sync_after_io(u32 addr) {
    melonDS::NDS& n = *nds;
    // A started DMA or a full GX FIFO stalls the ARM9 until it is done: run the system until then.
    if (n.CPUStop & (melonDS::CPUStop_DMA9 | melonDS::CPUStop_GXStall)) {
        cpu.next_event = 0;
        advance();
    }
    cpu.cycles = int64_t(n.ARM9Timestamp);
    // Writes may have scheduled an earlier event (melonDS lowers ARM9Target) or unmasked an IRQ.
    cpu.next_event = std::min<int64_t>(cpu.next_event, int64_t(n.ARM9Target));
    if ((addr & 0xFFFFF000) == 0x04000000 && (addr & 0xFFF) >= 0x208 && (addr & 0xFFF) < 0x218) cpu.next_event = 0;
}

// ---------------------------------------------------------------------------------------------------------------
// Scheduler

void RuntimeImpl::advance() {
    melonDS::NDS& n = *nds;
    RecompNDS& r = *nds;
    if (u64(cpu.cycles) > n.ARM9Timestamp) n.ARM9Timestamp = u64(cpu.cycles);
    const u32 arm9_stall = melonDS::CPUStop_DMA9 | melonDS::CPUStop_GXStall;
    while (n.ARM9Timestamp >= n.ARM9Target || (n.CPUStop & arm9_stall)) {
        if (n.CPUStop & melonDS::CPUStop_GXStall) {
            int32_t cycles = n.GPU.GPU3D.CyclesToRunFor();
            n.ARM9Timestamp = std::min(n.ARM9Target, n.ARM9Timestamp + (u64(cycles) << n.ARM9ClockShift));
        } else if (n.CPUStop & melonDS::CPUStop_DMA9) {
            for (int i = 0; i < 4; i++)
                if (!(n.CPUStop & melonDS::CPUStop_GXStall)) r.dma(i).Run();
        }
        n.RunTimers(0);
        n.GPU.GPU3D.Run();

        u64 target = n.ARM9Timestamp >> n.ARM9ClockShift;
        n.CurCPU = 1;
        while (n.ARM7Timestamp < target) {
            n.ARM7Target = target;
            if (n.CPUStop & melonDS::CPUStop_DMA7) {
                for (int i = 4; i < 8; i++) r.dma(i).Run();
            } else {
                n.ARM7.Execute<CPUExecuteMode::Interpreter>();
            }
            n.RunTimers(1);
        }
        r.run_system(target);
        n.CurCPU = 0;

        if (n.GPU.TotalScanlines != 0) {
            end_frame();
            n.GPU.StartFrame();
        }
        n.ARM9Target = r.next_target() << n.ARM9ClockShift;
        // While stalled the ARM9 makes no progress, so time jumps to the next event.
        if ((n.CPUStop & melonDS::CPUStop_GXStall) == 0 && (n.CPUStop & melonDS::CPUStop_DMA9) == 0) break;
    }
    cpu.cycles = int64_t(n.ARM9Timestamp);
    cpu.next_event = int64_t(n.ARM9Target);
}

void RuntimeImpl::end_frame() {
    melonDS::NDS& n = *nds;
    frames_++;
    n.SetKeyMask(~buttons_ & 0xFFF);
    if (touch_x_ >= 0)
        n.TouchScreen(u16(touch_x_), u16(touch_y_));
    else
        n.ReleaseScreen();

    int avail = n.SPU.GetOutputSize();
    if (avail > 0) {
        audio_buf_.resize(size_t(avail) * 2);
        int got = n.SPU.ReadOutput(audio_buf_.data(), avail);
        if (got > 0) host.on_audio(audio_buf_.data(), size_t(got));
    }

    if (options.perf) report_perf();
    if (options.dump_at_frame && frames_ == options.dump_at_frame) dump_state();

    Frame frame;
    void* top = nullptr;
    void* bottom = nullptr;
    if (n.GPU.GetFramebuffers(&top, &bottom)) {
        frame.top = static_cast<const uint32_t*>(top);
        frame.bottom = static_cast<const uint32_t*>(bottom);
    } else {
        frame.gl_texture = *static_cast<const GLuint*>(top);
        frame.scale = video_.scale;
    }
    if (!host.on_frame(frame)) {
        quit_ = true;
        fiber_switch(fibers_[current_fiber_].fiber, root_fiber_);
    }
    if (video_dirty_) {
        video_dirty_ = false;
        apply_video(pending_video_);
    }
    perf_mark_ = std::chrono::steady_clock::now();  // the frontend's frame pacing is not emulation time
}

void RuntimeImpl::report_perf() {
    const auto now = std::chrono::steady_clock::now();
    if (perf_mark_ != std::chrono::steady_clock::time_point{})
        perf_host_s_ += std::chrono::duration<double>(now - perf_mark_).count();
    constexpr u64 kFrames = 60;
    if (frames_ % kFrames) return;
    const double ms = perf_host_s_ * 1000.0 / kFrames;
    log("perf: frame %llu: %.2f ms/frame emulating (%.0f%% of a DS frame), interpreter %llu calls, %llu instructions/frame\n",
        (unsigned long long)frames_, ms, ms / (1000.0 / 59.8261) * 100.0,
        (unsigned long long)(interp_calls_ - perf_calls_mark_), (unsigned long long)((interp_steps_ - perf_steps_mark_) / kFrames));
    perf_host_s_ = 0;
    perf_calls_mark_ = interp_calls_;
    perf_steps_mark_ = interp_steps_;
}

void RuntimeImpl::poll() {
    advance();
    if (irq_pending()) take_irq();
}

void RuntimeImpl::halt() {
    // CP15 wait-for-interrupt: sleep until an enabled IRQ is raised (IME is not required to wake up).
    melonDS::NDS& n = *nds;
    while (!(n.IE[0] & n.IF[0])) {
        cpu.cycles = std::max<int64_t>(cpu.cycles, int64_t(n.ARM9Target));
        advance();
    }
    cpu.next_event = 0;
}

bool RuntimeImpl::irq_pending() const {
    const melonDS::NDS& n = *nds;
    return (n.IME[0] & 1) && (n.IE[0] & n.IF[0]) && !(cpu.cpsr_ctrl & 0x80);
}

// BIOS IRQ entry at a high level:
//   stmfd sp!, {r0-r3, r12, lr}; r0 = DTCM end; lr = return; ldr pc, [r0, #-4]
//   ldmfd sp!, {r0-r3, r12, lr}; subs pc, lr, #4
// The interrupted "PC" is the current fiber's token, so if the game's handler switches threads (it rewrites
// this frame), the PC it returns to names the fiber to resume.
void RuntimeImpl::take_irq() {
    const u32 old = cpsr();
    swap_banks(old & 0x1F, 0x12);
    cpu.cpsr_ctrl = 0x92 | (old & 0x40);
    bank_irq_[2] = old;
    fibers_[current_fiber_].saved = true;
    cpu.r[14] = current_token() + 4;

    u32 sp = cpu.r[13] - 24;
    const u32 frame[6] = {cpu.r[0], cpu.r[1], cpu.r[2], cpu.r[3], cpu.r[12], cpu.r[14]};
    for (int i = 0; i < 6; i++) ntr_wr32(&cpu, sp + 4 * i, frame[i]);
    cpu.r[13] = sp;

    const u32 dtcm_end = (nds->ARM9.DTCMSetting & 0xFFFFF000) + 0x4000;
    cpu.r[0] = dtcm_end;
    cpu.r[14] = kBiosIrqReturn;
    call(ntr_rd32(&cpu, dtcm_end - 4));

    if ((cpu.cpsr_ctrl & 0x1F) != 0x12) fatal("IRQ handler returned in mode %02x", cpu.cpsr_ctrl & 0x1F);
    sp = cpu.r[13];
    cpu.r[0] = ntr_rd32(&cpu, sp);
    cpu.r[1] = ntr_rd32(&cpu, sp + 4);
    cpu.r[2] = ntr_rd32(&cpu, sp + 8);
    cpu.r[3] = ntr_rd32(&cpu, sp + 12);
    cpu.r[12] = ntr_rd32(&cpu, sp + 16);
    cpu.r[14] = ntr_rd32(&cpu, sp + 20);
    cpu.r[13] = sp + 24;
    const u32 pc = cpu.r[14] - 4;
    const u32 spsr = bank_irq_[2];
    set_cpsr(spsr, 0xFFFFFFFF);
    resume_at(pc | ((spsr >> 5) & 1));
}

// ---------------------------------------------------------------------------------------------------------------
// CPU modes

void RuntimeImpl::swap_banks(u32 old_mode, u32 new_mode) {
    old_mode &= 0x1F;
    new_mode &= 0x1F;
    if (old_mode == new_mode) return;
    auto bank = [&](u32 mode) -> u32* {
        switch (mode) {
        case 0x11: return &bank_fiq_[5];
        case 0x12: return bank_irq_;
        case 0x13: return bank_svc_;
        case 0x17: return bank_abt_;
        case 0x1B: return bank_und_;
        default: return bank_usr_;
        }
    };
    if (old_mode == 0x11)
        for (int i = 0; i < 5; i++) {
            bank_fiq_[i] = cpu.r[8 + i];
            cpu.r[8 + i] = fiq_usr_[i];
        }
    u32* ob = bank(old_mode);
    ob[0] = cpu.r[13];
    ob[1] = cpu.r[14];
    u32* nb = bank(new_mode);
    cpu.r[13] = nb[0];
    cpu.r[14] = nb[1];
    if (new_mode == 0x11)
        for (int i = 0; i < 5; i++) {
            fiq_usr_[i] = cpu.r[8 + i];
            cpu.r[8 + i] = bank_fiq_[i];
        }
}

void RuntimeImpl::set_cpsr(u32 value, u32 mask) {
    const u32 old = cpsr();
    const u32 nv = (old & ~mask) | (value & mask);
    cpu.n = nv >> 31;
    cpu.z = (nv >> 30) & 1;
    cpu.c = (nv >> 29) & 1;
    cpu.v = (nv >> 28) & 1;
    cpu.q = (nv >> 27) & 1;
    swap_banks(old & 0x1F, nv & 0x1F);
    cpu.cpsr_ctrl = nv & 0xFF;
    if ((old & 0x80) && !(nv & 0x80)) cpu.next_event = 0;  // IRQs unmasked: check for pending ones
}

u32* RuntimeImpl::spsr_slot() {
    static u32 dummy;
    switch (cpu.cpsr_ctrl & 0x1F) {
    case 0x11: return &bank_fiq_[7];
    case 0x12: return &bank_irq_[2];
    case 0x13: return &bank_svc_[2];
    case 0x17: return &bank_abt_[2];
    case 0x1B: return &bank_und_[2];
    default: dummy = cpsr(); return &dummy;
    }
}

u32 RuntimeImpl::user_reg(u32 r) {
    const u32 mode = cpu.cpsr_ctrl & 0x1F;
    if (mode == 0x10 || mode == 0x1F) return cpu.r[r];
    if (r >= 13 && r <= 14) return bank_usr_[r - 13];
    if (mode == 0x11 && r >= 8 && r <= 12) return fiq_usr_[r - 8];
    return cpu.r[r];
}

void RuntimeImpl::set_user_reg(u32 r, u32 v) {
    const u32 mode = cpu.cpsr_ctrl & 0x1F;
    if (mode == 0x10 || mode == 0x1F)
        cpu.r[r] = v;
    else if (r >= 13 && r <= 14)
        bank_usr_[r - 13] = v;
    else if (mode == 0x11 && r >= 8 && r <= 12)
        fiq_usr_[r - 8] = v;
    else
        cpu.r[r] = v;
}

void RuntimeImpl::exception_return(u32 target) {
    fatal("exception return to %08x from mode %02x is not supported", target, cpu.cpsr_ctrl & 0x1F);
}

// ---------------------------------------------------------------------------------------------------------------
// Calls

bool RuntimeImpl::overlay_resident(const NtrOverlayInfo& ov) {
    for (u32 i = 0; i < ov.nprobes; i++)
        if (ntr_rd32(&cpu, ov.probe_addr[i]) != ov.probe_val[i]) return false;
    return true;
}

const NtrFuncEntry* RuntimeImpl::lookup(u32 target) {
    for (const NtrOverlayInfo* ov : interpreted_)
        if ((target & ~1u) - ov->base < ov->size && overlay_resident(*ov)) return nullptr;
    auto it = functions_.find(target);
    if (it == functions_.end()) return nullptr;
    const auto& cands = it->second;
    if (cands.size() == 1) return cands[0];
    // A resident overlay (or code region copied over static code) wins over the static module beneath it.
    const NtrFuncEntry* fixed = nullptr;
    for (const NtrFuncEntry* e : cands) {
        if (e->module < NTR_MODULE_OVERLAY(0)) {
            fixed = e;
            continue;
        }
        u32 id = e->module - NTR_MODULE_OVERLAY(0);
        if (id < overlays_by_id_.size() && overlays_by_id_[id] && overlay_resident(*overlays_by_id_[id])) return e;
    }
    return fixed;
}

void RuntimeImpl::call(u32 target) {
    if (is_token(target)) fatal("call to suspended-thread token %08x (lr %08x)", target, cpu.r[14]);
    const NtrFuncEntry* e = lookup(target);
    if (!e) {
        if (target == kBiosIrqReturn || target == kHleCallReturn)
            fatal("unbalanced return to %08x (sp %08x)", target, cpu.r[13]);
        if (!can_interpret(target)) fatal("no recompiled function at %08x (lr %08x, sp %08x)", target, cpu.r[14], cpu.r[13]);
        interpret(target, cpu.r[14]);
        return;
    }
    if (options.trace_calls) log("call %08x\n", target);
    e->fn(&cpu);
}

namespace {
// Function entries recorded by generated code built with NTR_TRACE_ENTRY (fiber, address, lr, sp).
struct EntryRecord { u32 fiber, addr, lr, sp; };
EntryRecord g_entries[64];
u32 g_entry_pos = 0;
}  // namespace

void RuntimeImpl::dump_state() {
    log("thread %u, cpsr %08x, cycles %lld, frame %llu\n", current_fiber_, cpsr(), (long long)cpu.cycles, (unsigned long long)frames_);
    for (int i = 0; i < 16; i += 4)
        log("  r%-2d %08x  r%-2d %08x  r%-2d %08x  r%-2d %08x\n", i, cpu.r[i], i + 1, cpu.r[i + 1], i + 2, cpu.r[i + 2], i + 3, cpu.r[i + 3]);
    log("  banks: usr sp/lr %08x %08x  irq %08x %08x spsr %08x  svc %08x %08x spsr %08x\n", bank_usr_[0], bank_usr_[1],
        bank_irq_[0], bank_irq_[1], bank_irq_[2], bank_svc_[0], bank_svc_[1], bank_svc_[2]);
    u32 sp = cpu.r[13] & ~3u;
    for (int i = -8; i < 24; i += 4) {
        log("  [sp%+4d]", i * 4);
        for (int j = 0; j < 4; j++) log(" %08x", ntr_rd32(&cpu, sp + (i + j) * 4));
        log("\n");
    }
    if (g_entry_pos) {
        log("  last function entries (oldest first):\n");
        for (u32 i = g_entry_pos > 64 ? g_entry_pos - 64 : 0; i < g_entry_pos; i++) {
            const EntryRecord& e = g_entries[i % 64];
            log("    thread %u  %08x  lr %08x  sp %08x\n", e.fiber, e.addr, e.lr, e.sp);
        }
    }
}

void RuntimeImpl::jump(u32 target, u32 from, u32 entry_lr) {
    if (options.trace_calls) log("jump %08x -> %08x\n", from, target);
    if (lookup(target)) {
        call(target);
        return;
    }
    if (!can_interpret(target)) {
        dump_state();
        fatal("jump from %08x to %08x: no recompiled function there (lr %08x, sp %08x)", from, target, cpu.r[14], cpu.r[13]);
    }
    interpret(target, entry_lr);
}

bool RuntimeImpl::can_interpret(u32 target) const {
    // Guest code with no recompiled function: hand-written code reached through computed jumps and
    // continuation-style returns (the ITCM code that overlay 24 installs). Anywhere in RAM, ITCM or DTCM.
    const u32 a = target & ~1u;
    return a >= 0x01000000 && a < 0x03000000 && !is_token(a);
}

void RuntimeImpl::interpret(u32 target, u32 stop) {
    melonDS::ARMv5& arm = nds->ARM9;
    const u32 ctrl = cpu.cpsr_ctrl;
    const u32 stop_sp = cpu.r[13];
    stop &= ~1u;
    interp_calls_++;
    InterpStat& stat = interp_stats_[target];
    if (stat.calls++ == 0) log("interpreter: first entry at %08x (returns to %08x)\n", target, stop);
    const u64 steps_before = interp_steps_;
    if (options.trace_calls) log("interpret %08x until %08x\n", target, stop);

    auto load = [&](u32 pc) {
        for (int i = 0; i < 15; i++) arm.R[i] = cpu.r[i];
        // IRQs stay masked inside the interpreter; they are delivered by poll() through the HLE BIOS path.
        arm.CPSR = (cpsr() & ~0x20u) | 0x80;
        nds->ARM9Timestamp = u64(cpu.cycles);
        arm.Cycles = 0;
        arm.JumpTo(pc);
    };
    auto store = [&] {
        for (int i = 0; i < 15; i++) cpu.r[i] = arm.R[i];
        cpu.n = arm.CPSR >> 31 & 1;
        cpu.z = arm.CPSR >> 30 & 1;
        cpu.c = arm.CPSR >> 29 & 1;
        cpu.v = arm.CPSR >> 28 & 1;
        cpu.q = arm.CPSR >> 27 & 1;
        cpu.cpsr_ctrl = ctrl;
        cpu.cycles = int64_t(nds->ARM9Timestamp);
    };
    auto pc_of = [&] { return arm.R[15] - ((arm.CPSR & 0x20) ? 2 : 4); };

    load(target);
    const u32 stall = melonDS::CPUStop_DMA9 | melonDS::CPUStop_GXStall;
    for (;;) {
        const u32 pc = pc_of();
        if (pc == stop && arm.R[13] >= stop_sp) break;
        if ((arm.CPSR & 0x1F) != (ctrl & 0x1F) || pc >= 0xFFFF0000u) {
            store();
            dump_state();
            log("  cp15 control %08x, PU data rw %08x, regions:", arm.CP15Control, arm.PU_DataRW);
            for (u32 r : arm.PU_Region) log(" %08x", r);
            log("\n");
            fatal("interpreter left guest code at %08x (cpsr %08x), started at %08x", pc, arm.CPSR, target);
        }
        const u64 saved_target = nds->ARM9Target;
        nds->ARM9Target = nds->ARM9Timestamp + 1;
        arm.Execute<melonDS::CPUExecuteMode::Interpreter>();
        nds->ARM9Target = saved_target;
        interp_steps_++;
        if (arm.Halted) fatal("interpreted code halted the CPU at %08x", pc);
        if (int64_t(nds->ARM9Timestamp) >= cpu.next_event || (nds->CPUStop & stall)) {
            store();
            const u32 resume = pc_of() | ((arm.CPSR >> 5) & 1);
            poll();  // may run IRQ handlers and switch guest threads; `cpu` holds this thread's state again after
            load(resume);
        }
    }
    stat.steps += interp_steps_ - steps_before;  // unordered_map references stay valid across inserts
    store();
}

void RuntimeImpl::call_guest(u32 target) {
    const u32 lr = cpu.r[14];
    cpu.r[14] = kHleCallReturn;
    call(target);
    cpu.r[14] = lr;
}

// ---------------------------------------------------------------------------------------------------------------
// Guest threads

void RuntimeImpl::fiber_main(void* arg) {
    RuntimeImpl* self = static_cast<RuntimeImpl*>(arg);
    self->cleanup_dead_fibers();
    const u32 pc = self->fibers_[self->current_fiber_].start_pc;
    const u32 lr = self->cpu.r[14];
    if (self->options.trace_threads)
        log("thread %u starts at %08x (lr %08x, sp %08x, cpsr %08x)\n", self->current_fiber_, pc, lr, self->cpu.r[13], self->cpsr());
    self->call(pc);
    if (self->options.trace_threads) log("thread %u returned to %08x\n", self->current_fiber_, lr);
    // The thread function returned to its initial LR (OS_ExitThread for NitroSDK threads).
    self->call(lr);
    fatal("guest thread started at %08x ran past its exit function %08x", pc, lr);
}

void RuntimeImpl::resume_at(u32 pc) {
    u32 k;
    if (is_token(pc & ~1u)) {
        k = ((pc & ~1u) - kFiberTokenBase) / 4;
        if (k == current_fiber_) {
            fibers_[k].saved = false;
            return;
        }
        if (k >= fibers_.size() || !fibers_[k].alive) fatal("resume of a dead guest thread (token %08x)", pc);
    } else {
        k = u32(fibers_.size());
        fibers_.push_back({fiber_create(1u << 20, &RuntimeImpl::fiber_main, this), pc, true, false});
    }
    const u32 prev = current_fiber_;
    if (!fibers_[prev].saved) fibers_[prev].alive = false;  // nothing can resume it any more
    if (options.trace_threads)
        log("switch thread %u -> %u (%s%08x)%s\n", prev, k, is_token(pc & ~1u) ? "resume " : "new at ", pc,
            fibers_[prev].alive ? "" : ", previous one ended");
    current_fiber_ = k;
    fibers_[k].saved = false;
    fiber_switch(fibers_[prev].fiber, fibers_[k].fiber);
    cleanup_dead_fibers();
}

void RuntimeImpl::discard_token(u32 pc) {
    if (!is_token(pc & ~1u)) return;
    u32 k = ((pc & ~1u) - kFiberTokenBase) / 4;
    if (k < fibers_.size() && k != current_fiber_) fibers_[k].alive = false;
    cleanup_dead_fibers();
}

void RuntimeImpl::cleanup_dead_fibers() {
    for (u32 i = 0; i < fibers_.size(); i++) {
        GuestFiber& f = fibers_[i];
        if (!f.alive && f.fiber && i != current_fiber_) {
            fiber_destroy(f.fiber);
            f.fiber = nullptr;
        }
    }
}

int RuntimeImpl::run() {
    root_fiber_ = fiber_init_thread();
    fibers_[0].fiber = fiber_create(1u << 20, &RuntimeImpl::fiber_main, this);
    current_fiber_ = 0;
    fiber_switch(root_fiber_, fibers_[0].fiber);
    return exit_code_;
}

void RuntimeImpl::set_buttons(uint32_t pressed) { buttons_ = pressed; }

void RuntimeImpl::set_video(const VideoSettings& settings) {
    pending_video_ = settings;
    video_dirty_ = true;
}

// Switches melonDS's renderer (between frames). Falls back to OpenGL when the context lacks compute shaders, and to
// Software when there is no usable context at all.
void RuntimeImpl::apply_video(VideoSettings s) {
    melonDS::NDS& n = *nds;
    s.scale = std::clamp(s.scale, 1, 16);
    if (s.renderer != Renderer::Software && !glEnable) {
        log("video: no OpenGL context, using the software renderer\n");
        s.renderer = Renderer::Software;
    }
    if (s.renderer == Renderer::Compute && !GLAD_GL_VERSION_4_3) {
        log("video: OpenGL 4.3 is not available for the compute renderer, using OpenGL\n");
        s.renderer = Renderer::OpenGL;
    }
    if (!video_applied_ || s.renderer != video_.renderer) {
        if (s.renderer == Renderer::Software)
            n.SetRenderer(std::make_unique<melonDS::SoftRenderer>(n));
        else
            n.SetRenderer(std::make_unique<melonDS::GLRenderer>(n, s.renderer == Renderer::Compute));
        if (s.renderer != Renderer::Software && !dynamic_cast<melonDS::GLRenderer*>(&n.GetRenderer())) {
            log("video: the %s renderer failed to start, using the software renderer\n", renderer_name(s.renderer));
            s.renderer = Renderer::Software;
        }
    }
    melonDS::RendererSettings rs{s.scale, s.threaded, s.hires_coordinates, s.better_polygons};
    n.GetRenderer().SetRenderSettings(rs);
    int current = 0, count = 0;
    while (n.GetRenderer().NeedsShaderCompile()) n.GetRenderer().ShaderCompileStep(current, count);
    if (s.renderer == Renderer::Software) s.scale = 1;
    video_ = s;
    video_applied_ = true;
    log("video: %s renderer, %dx (%dx%d per screen)\n", renderer_name(s.renderer), s.scale, 256 * s.scale, 192 * s.scale);
}

const char* renderer_name(Renderer r) {
    switch (r) {
    case Renderer::Software: return "software";
    case Renderer::OpenGL: return "OpenGL";
    case Renderer::Compute: return "compute";
    }
    return "?";
}
void RuntimeImpl::touch(int x, int y) {
    touch_x_ = std::clamp(x, 0, 255);
    touch_y_ = std::clamp(y, 0, 191);
}
void RuntimeImpl::release_touch() { touch_x_ = touch_y_ = -1; }

// ---------------------------------------------------------------------------------------------------------------
// Coprocessor

u32 RuntimeImpl::mrc(u32 cp, u32 op1, u32 crn, u32 crm, u32 op2) {
    if (cp != 15) fatal("mrc p%u", cp);
    return nds->ARM9.CP15Read((crn << 8) | (crm << 4) | op2);
}

void RuntimeImpl::mcr(u32 cp, u32 op1, u32 crn, u32 crm, u32 op2, u32 v) {
    if (cp != 15) fatal("mcr p%u", cp);
    const u32 id = (crn << 8) | (crm << 4) | op2;
    if (id == 0x704 || id == 0x782) {  // wait for interrupt
        halt();
        return;
    }
    nds->ARM9.CP15Write(id, v);
    if (id == 0x100 || id == 0x910 || id == 0x911) map_pages();
}

}  // namespace ntr

// ---------------------------------------------------------------------------------------------------------------
// C services called by recompiled code (ntr/recomp.h)

using namespace ntr;

extern "C" {

void ntr_trace_enter(NtrCpu* c, uint32_t addr) {
    g_entries[g_entry_pos++ % 64] = {(rt(c).current_token() - kFiberTokenBase) >> 2, addr, c->r[14], c->r[13]};
    rt(c).note_entry(addr);
}

uint32_t ntr_read32_slow(NtrCpu* c, uint32_t a) { return rt(c).bus_read32(a); }
uint32_t ntr_read16_slow(NtrCpu* c, uint32_t a) { return rt(c).bus_read16(a); }
uint32_t ntr_read8_slow(NtrCpu* c, uint32_t a) { return rt(c).bus_read8(a); }
void ntr_write32_slow(NtrCpu* c, uint32_t a, uint32_t v) { rt(c).bus_write32(a, v); }
void ntr_write16_slow(NtrCpu* c, uint32_t a, uint32_t v) { rt(c).bus_write16(a, v); }
void ntr_write8_slow(NtrCpu* c, uint32_t a, uint32_t v) { rt(c).bus_write8(a, v); }

void ntr_call(NtrCpu* c, uint32_t target) { rt(c).call(target); }
void ntr_jump(NtrCpu* c, uint32_t target, uint32_t from, uint32_t entry_lr) { rt(c).jump(target, from, entry_lr); }
void ntr_swi(NtrCpu* c, uint32_t number, uint32_t pc) { rt(c).swi(number, pc); }
void ntr_poll(NtrCpu* c) { rt(c).poll(); }
uint32_t ntr_get_spsr(NtrCpu* c) { return *rt(c).spsr_slot(); }
void ntr_set_cpsr(NtrCpu* c, uint32_t value, uint32_t mask) { rt(c).set_cpsr(value, mask); }
void ntr_set_spsr(NtrCpu* c, uint32_t value, uint32_t mask) {
    uint32_t* s = rt(c).spsr_slot();
    *s = (*s & ~mask) | (value & mask);
}
void ntr_exception_return(NtrCpu* c, uint32_t target) { rt(c).exception_return(target); }
uint32_t ntr_get_user_reg(NtrCpu* c, uint32_t r) { return rt(c).user_reg(r); }
void ntr_set_user_reg(NtrCpu* c, uint32_t r, uint32_t v) { rt(c).set_user_reg(r, v); }
uint32_t ntr_mrc(NtrCpu* c, uint32_t cp, uint32_t op1, uint32_t crn, uint32_t crm, uint32_t op2) {
    return rt(c).mrc(cp, op1, crn, crm, op2);
}
void ntr_mcr(NtrCpu* c, uint32_t cp, uint32_t op1, uint32_t crn, uint32_t crm, uint32_t op2, uint32_t v) {
    rt(c).mcr(cp, op1, crn, crm, op2, v);
}
void ntr_undefined(NtrCpu* c, uint32_t pc, uint32_t opcode) { fatal("undefined instruction %08x at %08x", opcode, pc); }
void ntr_bad_jump(NtrCpu* c, uint32_t pc, uint32_t target) { fatal("jump table at %08x: unexpected target %08x", pc, target); }
void ntr_unimplemented(NtrCpu* c, uint32_t addr) {
    fatal("function %08x is not recompiled (DS Protect or a missing hook), lr %08x", addr, c->r[14]);
}

}  // extern "C"
