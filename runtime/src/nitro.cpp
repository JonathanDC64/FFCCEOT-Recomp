// Replacements for NitroSDK routines that cannot run as recompiled code: thread context switches and the
// IRQ dispatcher. They keep the guest-visible effects of the originals (the OSContext contents, registers,
// I/O writes) but resume guest threads by switching host fibers.
//
// OSContext layout (NitroSDK): +0 cpsr, +4 r0-r12, +0x38 sp, +0x3C lr, +0x40 pc+4, +0x44 sp_svc, +0x48 CPContext.
#include "ntr/nitro.h"

#include "runtime_internal.hpp"

using namespace ntr;

namespace {

constexpr u32 kCtxCpsr = 0x00, kCtxR0 = 0x04, kCtxPcPlus4 = 0x40, kCtxSpSvc = 0x44, kCtxCp = 0x48;

// CP_SaveContext: divider and square-root unit state.
void cp_save(NtrCpu* c, u32 cp) {
    for (u32 i = 0; i < 4; i++) ntr_wr32(c, cp + i * 4, ntr_rd32(c, 0x04000290 + i * 4));
    ntr_wr32(c, cp + 0x10, ntr_rd32(c, 0x040002B8));
    ntr_wr32(c, cp + 0x14, ntr_rd32(c, 0x040002BC));
    ntr_wr16(c, cp + 0x18, ntr_rd16(c, 0x04000280) & 3);
    ntr_wr16(c, cp + 0x1A, ntr_rd16(c, 0x040002B0) & 1);
}

// CPi_RestoreContext (same write order as the original).
void cp_restore(NtrCpu* c, u32 cp) {
    for (u32 i = 0; i < 4; i++) ntr_wr32(c, 0x04000290 + i * 4, ntr_rd32(c, cp + i * 4));
    ntr_wr16(c, 0x04000280, ntr_rd16(c, cp + 0x18));
    ntr_wr16(c, 0x040002B0, ntr_rd16(c, cp + 0x1A));
    ntr_wr32(c, 0x040002B8, ntr_rd32(c, cp + 0x10));
    ntr_wr32(c, 0x040002BC, ntr_rd32(c, cp + 0x14));
}

}  // namespace

extern "C" {

// BOOL OS_SaveContext(OSContext* context): returns 0 now, and "returns 1" when the context is loaded again.
// The saved PC is the current fiber's token; resuming it continues this fiber where it suspended (inside
// ntr_nitro_load_context), which mwcc's OSi_RescheduleThread follows with the same epilogue as the
// `if (OS_SaveContext(...)) return;` path.
void ntr_nitro_save_context(NtrCpu* c) {
    RuntimeImpl& r = rt(c);
    const u32 ctx = c->r[0];
    cp_save(c, ctx + kCtxCp);
    ntr_wr32(c, ctx + kCtxCpsr, r.cpsr() & ~0x20u);
    const bool svc = (c->cpsr_ctrl & 0x1F) == 0x13;
    u32 sp_svc = svc ? c->r[13] : 0;
    if (!svc) {
        // Read the banked SVC stack pointer the way the original does (switch to SVC and back).
        const u32 saved = r.cpsr();
        r.set_cpsr(0xD3, 0xFF);
        sp_svc = c->r[13];
        r.set_cpsr(saved, 0xFF);
    }
    ntr_wr32(c, ctx + kCtxSpSvc, sp_svc);
    ntr_wr32(c, ctx + kCtxR0, 1);
    for (u32 i = 1; i < 15; i++) ntr_wr32(c, ctx + kCtxR0 + i * 4, c->r[i]);
    ntr_wr32(c, ctx + kCtxPcPlus4, r.current_token() + 4);
    c->r[0] = 0;
    r.mark_saved();
}

// void OS_LoadContext(OSContext* context): continues the thread saved in `context`.
void ntr_nitro_load_context(NtrCpu* c) {
    RuntimeImpl& r = rt(c);
    const u32 ctx = c->r[0];
    cp_restore(c, ctx + kCtxCp);
    const u32 cpsr = ntr_rd32(c, ctx + kCtxCpsr);
    r.set_cpsr((r.cpsr() & ~0x1Fu) | 0xD3, 0xFF);
    *r.spsr_slot() = cpsr;
    c->r[13] = ntr_rd32(c, ctx + kCtxSpSvc);
    c->r[14] = ntr_rd32(c, ctx + kCtxPcPlus4);
    const u32 pc = c->r[14] - 4;
    for (u32 i = 0; i < 15; i++) r.set_user_reg(i, ntr_rd32(c, ctx + kCtxR0 + i * 4));
    r.set_cpsr(cpsr, 0xFFFFFFFF);
    r.resume_at(pc | ((cpsr >> 5) & 1));
}

// void OS_InitContext(OSContext* context, u32 newpc, u32 newsp)
void ntr_nitro_init_context(NtrCpu* c) {
    RuntimeImpl& r = rt(c);
    const u32 ctx = c->r[0], pc = c->r[1], stack = c->r[2];
    r.discard_token(ntr_rd32(c, ctx + kCtxPcPlus4) - 4);  // a thread that was suspended here is gone
    ntr_wr32(c, ctx + kCtxPcPlus4, pc + 4);
    ntr_wr32(c, ctx + kCtxSpSvc, stack);
    u32 sp = stack - 0x40;
    if (sp & 4) sp -= 4;
    ntr_wr32(c, ctx + 0x38, sp);
    ntr_wr32(c, ctx + kCtxCpsr, (pc & 1) ? 0x3F : 0x1F);
    for (u32 i = 0; i < 13; i++) ntr_wr32(c, ctx + kCtxR0 + i * 4, 0);
    ntr_wr32(c, ctx + 0x3C, 0);
    c->r[1] = (pc & 1) ? 0x3F : 0x1F;
    c->r[2] = sp;
}

void ntr_dsprot_not_detected(NtrCpu* c) { c->r[0] = 0; }

}  // extern "C"

namespace ntr::nitro {

// CARDi_RequestStreamCommand(src, dst, len, callback, arg, is_async, req, retry, mode). The ARM9 hands each backup
// request to the ARM7's card code, one page (256 bytes of EEPROM) at a time, each taking about a tick:
// a few seconds for a save of tens of kilobytes. Here a synchronous read, write or verify is done on the save memory
// itself, as on a save chip that takes no time.
bool card_backup_request(NtrCpu* c, u32 common) {
    enum : u32 { kReadBackup = 6, kWriteBackup = 7, kProgramBackup = 8, kVerifyBackup = 9 };
    constexpr u32 kCommonFlags = 69 * 4, kFlagBusy = 4;  // a request in progress (an asynchronous one)
    const u32 sp = c->r[13], src = c->r[0], dst = c->r[1], len = c->r[2];
    const u32 is_async = ntr_rd32(c, sp + 4), req = ntr_rd32(c, sp + 8);
    if (is_async || (ntr_rd32(c, common + kCommonFlags) & kFlagBusy)) return false;

    RuntimeImpl& r = rt(c);
    u8* save = r.nds->NDSCartSlot.GetSaveMemory();
    const u32 size = r.nds->NDSCartSlot.GetSaveMemoryLength();
    // Backup offset and RAM address.
    const u32 offset = req == kReadBackup ? src : dst, ram = req == kReadBackup ? dst : src;
    if (!save || offset > size || len > size - offset) return false;
    bool same = true;
    switch (req) {
    case kReadBackup:
        for (u32 i = 0; i < len; i++) ntr_wr8(c, ram + i, save[offset + i]);
        break;
    case kWriteBackup:
    case kProgramBackup:
        for (u32 i = 0; i < len; i++) save[offset + i] = u8(ntr_rd8(c, ram + i));
        r.host.on_save(save, size);
        break;
    case kVerifyBackup:
        for (u32 i = 0; i < len && same; i++) same = save[offset + i] == ntr_rd8(c, ram + i);
        break;
    default:
        return false;
    }
    // The command block shared with the ARM7 (first word of the common state) starts with the result.
    constexpr u32 kResultSuccess = 0, kResultFailure = 1;
    ntr_wr32(c, ntr_rd32(c, common), same ? kResultSuccess : kResultFailure);
    c->r[0] = same ? 1 : 0;
    return true;
}

// OS_IrqHandler: acknowledges the lowest pending enabled IRQ, calls its handler from OS_IRQTable, then
// runs OS_IrqHandler_ThreadSwitch (which the original enters by setting LR before jumping to the handler).
void irq_handler(NtrCpu* c, uint32_t irq_table, uint32_t thread_switch) {
    RuntimeImpl& r = rt(c);
    const u32 entry_lr = c->r[14];
    c->r[13] -= 4;
    ntr_wr32(c, c->r[13], entry_lr);  // stmdb sp!, {lr}
    c->r[12] = 0x04000210;
    const u32 ime = ntr_rd32(c, 0x04000208);
    const u32 ie = ntr_rd32(c, 0x04000210), iflags = ntr_rd32(c, 0x04000214);
    const u32 pending = ie & iflags;
    if (!ime || !pending) {
        c->r[13] += 4;  // ldmfd sp!, {pc}
        return;
    }
    u32 index = 0;
    while (!((pending >> index) & 1)) index++;
    ntr_wr32(c, 0x04000214, 1u << index);
    const u32 handler = ntr_rd32(c, irq_table + index * 4);
    c->r[0] = handler;
    c->r[1] = irq_table;
    c->r[2] = iflags;
    c->r[3] = 0x80000000u;
    c->r[14] = thread_switch;
    r.call(handler);
    // The handler "returned" to OS_IrqHandler_ThreadSwitch, which ends by popping the BIOS return address.
    c->r[14] = entry_lr;
    r.call(thread_switch);
}

}  // namespace ntr::nitro
