// Instruction classification for control-flow analysis (ARMv5TE ARM + Thumb).
#include "recomp/recomp.hpp"

namespace recomp {

namespace {

inline int32_t sext(uint32_t v, int bits) {
    uint32_t m = 1u << (bits - 1);
    v &= (1u << bits) - 1;
    return int32_t((v ^ m) - m);
}

// mwcc Thumb switch:  [cmp rX,#n; bhi def;] add rN,rX,rX; add rN,pc; ldrh rN,[rN,#k]; lsl rN,#16; asr rN,#16; add pc,rN
bool thumb_jump_table(const Module& m, uint32_t addr, uint32_t rn, std::vector<uint32_t>& out) {
    if (addr < m.base + 10 || !m.contains(addr - 10, 10)) return false;
    uint16_t i_add2 = m.read16(addr - 10), i_addpc = m.read16(addr - 8), i_ldrh = m.read16(addr - 6);
    uint16_t i_lsl = m.read16(addr - 4), i_asr = m.read16(addr - 2);
    // add rN, rX, rX (index doubled; rX may differ from rN)
    if ((i_add2 & 0xFE00) != 0x1800 || (i_add2 & 7) != rn || ((i_add2 >> 3) & 7) != ((i_add2 >> 6) & 7)) return false;
    if (i_addpc != (0x4478 | rn)) return false;                          // add rN, pc
    if ((i_ldrh & 0xF800) != 0x8800 || (i_ldrh & 7) != rn || ((i_ldrh >> 3) & 7) != rn) return false;
    if (i_lsl != (0x0400 | (rn << 3) | rn)) return false;   // lsl rN, rN, #16
    if (i_asr != (0x1400 | (rn << 3) | rn)) return false;   // asr rN, rN, #16

    uint32_t k = ((i_ldrh >> 6) & 31) * 2;
    uint32_t table = (addr - 8) + 4 + k;  // entry i is at table + 2*i
    uint32_t base_pc = addr + 4;          // PC value read by `add pc, rN`

    // Bound: the nearest preceding "cmp idx, #n" followed by a bhi/bgt to the default case or a bls/ble
    // to the table (signed forms come with a separate "cmp idx, #0; blt" lower check).
    int count = -1;
    uint32_t idx = (i_add2 >> 3) & 7;
    for (uint32_t back = 14; back <= 30 && addr >= m.base + back; back += 2) {
        uint16_t i_cmp = m.read16(addr - back), i_b = m.read16(addr - back + 2);
        uint32_t bc = (i_b >> 8) & 0xFF;
        bool upper = bc == 0xD8 || bc == 0xDC || bc == 0xD9 || bc == 0xDD;  // bhi / bgt / bls / ble
        if (upper && i_cmp == (0x2800 | (idx << 8) | (i_cmp & 0xFF))) {
            count = (i_cmp & 0xFF) + 1;
            break;
        }
    }

    uint32_t min_target = 0xFFFFFFFF;
    for (int i = 0; count < 0 ? i < 1024 : i < count; i++) {
        uint32_t ea = table + 2 * i;
        if (!m.contains(ea, 2) || (count < 0 && ea >= min_target)) break;
        uint32_t t = (base_pc + uint32_t(sext(m.read16(ea), 16))) & ~1u;
        if (!m.contains(t, 2)) {
            if (count < 0) break;
            return false;
        }
        out.push_back(t);
        if (t < min_target && t > table) min_target = t;
    }
    return !out.empty();
}

// mwcc ARM switch:  cmp rX,#n; addls pc,pc,rX,lsl #2; b default; b case0; b case1; ...
bool arm_jump_table(const Module& m, uint32_t addr, uint32_t op, std::vector<uint32_t>& out) {
    uint32_t rm = op & 15;
    int count = -1;
    for (uint32_t back = 4; back <= 12 && addr >= m.base + back; back += 4) {
        uint32_t p = m.read32(addr - back);
        if ((p & 0x0FFFF000) == (0x03500000 | (rm << 16))) {  // cmp rm, #imm
            uint32_t imm = p & 0xFF, rot = ((p >> 8) & 15) * 2;
            imm = rot ? (imm >> rot) | (imm << (32 - rot)) : imm;
            count = int(imm) + 1;
            break;
        }
    }
    if (count <= 0 || count > 4096) return false;
    for (int i = 0; i < count; i++) {
        uint32_t t = addr + 8 + 4 * i;
        if (!m.contains(t, 4)) return false;
        out.push_back(t);
    }
    return true;
}

Instr decode_arm(const Module& m, uint32_t addr) {
    Instr in;
    in.addr = addr;
    in.thumb = false;
    in.size = 4;
    if (!m.contains(addr, 4)) {
        in.flow = Flow::Undefined;
        return in;
    }
    uint32_t op = m.read32(addr);
    in.op = op;
    uint32_t cond = op >> 28;
    in.cond = uint8_t(cond == 0xF ? 0xE : cond);

    if (cond == 0xF) {
        if ((op & 0x0E000000) == 0x0A000000) {  // BLX imm
            in.flow = Flow::Call;
            in.target = addr + 8 + uint32_t(sext(op, 24) << 2) + ((op >> 23) & 2);
            in.target_thumb = true;
        } else if ((op & 0x0D70F000) == 0x0550F000) {  // PLD
            in.flow = Flow::Normal;
        } else {
            in.flow = Flow::Undefined;
        }
        return in;
    }

    uint32_t rd = (op >> 12) & 15;
    switch ((op >> 25) & 7) {
    case 0:
    case 1: {
        if ((op & 0x0FFFFFF0) == 0x012FFF10) { in.flow = Flow::IndirectJump; return in; }  // BX
        if ((op & 0x0FFFFFF0) == 0x012FFF30) { in.flow = Flow::IndirectCall; return in; }  // BLX reg
        bool imm = (op >> 25) & 1;
        // Multiplies, swaps and halfword transfers live in the register form with bit7 = bit4 = 1.
        if (!imm && (op & 0x90) == 0x90) {
            uint32_t sh = (op >> 5) & 3;
            bool load = (op >> 20) & 1;
            if (sh != 0 && load && rd == 15) { in.flow = Flow::IndirectJump; return in; }  // LDRH pc (odd)
            if (sh == 2 && !load && rd == 15) { in.flow = Flow::Undefined; return in; }   // LDRD into pc
            return in;
        }
        uint32_t opcode = (op >> 21) & 15;
        bool s = (op >> 20) & 1;
        if (opcode >= 8 && opcode <= 11 && !s) {
            // Misc instructions (MRS/MSR/CLZ/QADD/SMLA/BKPT); none write PC except MRS pc (unpredictable).
            if ((op & 0x0FF000F0) == 0x01200070) in.flow = Flow::Undefined;  // BKPT
            return in;
        }
        if (rd == 15 && !(opcode >= 8 && opcode <= 11)) {
            uint32_t rn = (op >> 16) & 15;
            if (!imm && opcode == 4 && rn == 15 && !s && (op & 0xFF0) == 0x100) {  // add pc, pc, rX, lsl #2
                if (arm_jump_table(m, addr, op, in.table)) {
                    in.flow = Flow::JumpTable;
                    return in;
                }
            }
            in.flow = Flow::IndirectJump;
        }
        return in;
    }
    case 2:
    case 3:
        if (((op >> 25) & 7) == 3 && (op & 0x10)) { in.flow = Flow::Undefined; return in; }
        if ((op >> 20) & 1 && rd == 15) in.flow = Flow::IndirectJump;
        return in;
    case 4:
        if ((op >> 20) & 1 && (op & 0x8000)) in.flow = Flow::IndirectJump;
        return in;
    case 5:
        in.flow = (op >> 24) & 1 ? Flow::Call : Flow::Jump;
        in.target = addr + 8 + uint32_t(sext(op, 24) << 2);
        in.target_thumb = false;
        return in;
    case 6:
        in.flow = Flow::Undefined;  // LDC/STC not used on the DS
        return in;
    case 7:
        if ((op >> 24) & 1) return in;  // SWI
        if ((op & 0x10) && ((op >> 8) & 15) == 15) return in;  // MCR/MRC p15
        in.flow = Flow::Undefined;
        return in;
    }
    return in;
}

Instr decode_thumb(const Module& m, uint32_t addr) {
    Instr in;
    in.addr = addr;
    in.thumb = true;
    in.size = 2;
    if (!m.contains(addr, 2)) {
        in.flow = Flow::Undefined;
        return in;
    }
    uint32_t op = m.read16(addr);
    in.op = op;

    if ((op & 0xF800) == 0xF000) {  // BL/BLX prefix
        if (!m.contains(addr + 2, 2)) { in.flow = Flow::Undefined; return in; }
        uint32_t suffix = m.read16(addr + 2);
        uint32_t hi = addr + 4 + uint32_t(sext(op, 11) << 12);
        if ((suffix & 0xF800) == 0xF800) {
            in.flow = Flow::Call;
            in.target = hi + ((suffix & 0x7FF) << 1);
            in.target_thumb = true;
        } else if ((suffix & 0xF801) == 0xE800) {
            in.flow = Flow::Call;
            in.target = (hi + ((suffix & 0x7FF) << 1)) & ~3u;
            in.target_thumb = false;
        } else {
            in.flow = Flow::Undefined;
            return in;
        }
        in.op = op | (suffix << 16);
        in.size = 4;
        return in;
    }
    if ((op & 0xF800) == 0xE800 || (op & 0xF800) == 0xF800) { in.flow = Flow::Undefined; return in; }  // lone BL suffix
    if ((op & 0xF800) == 0xE000) {  // B
        in.flow = Flow::Jump;
        in.target = addr + 4 + uint32_t(sext(op, 11) << 1);
        in.target_thumb = true;
        return in;
    }
    if ((op & 0xF000) == 0xD000) {
        uint32_t cond = (op >> 8) & 15;
        if (cond == 15) return in;  // SWI
        if (cond == 14) { in.flow = Flow::Undefined; return in; }
        in.flow = Flow::Jump;
        in.cond = uint8_t(cond);
        in.target = addr + 4 + uint32_t(sext(op, 8) << 1);
        in.target_thumb = true;
        return in;
    }
    if ((op & 0xFF00) == 0xBE00) { in.flow = Flow::Undefined; return in; }  // BKPT
    if ((op & 0xFF00) == 0xBD00) { in.flow = Flow::IndirectJump; return in; }  // POP {.., pc}
    if ((op & 0xFC00) == 0x4400) {  // hi register ops / BX / BLX
        uint32_t sub = (op >> 8) & 3;
        uint32_t rd = (op & 7) | ((op >> 4) & 8);
        uint32_t rm = (op >> 3) & 15;
        if (sub == 3) {
            in.flow = (op & 0x80) ? Flow::IndirectCall : Flow::IndirectJump;
            return in;
        }
        if (sub != 1 && rd == 15) {  // ADD/MOV to pc
            if (sub == 0 && rm != 15 && thumb_jump_table(m, addr, rm, in.table)) {
                in.flow = Flow::JumpTable;
                return in;
            }
            in.flow = Flow::IndirectJump;
        }
        return in;
    }
    return in;
}

}  // namespace

Instr decode(const Module& m, uint32_t addr, bool thumb) {
    return thumb ? decode_thumb(m, addr) : decode_arm(m, addr);
}

uint32_t regs_written(const Instr& in) {
    uint32_t op = in.op;
    if (in.flow == Flow::Call || in.flow == Flow::IndirectCall) return 0xFFFF;  // callee may clobber anything
    if (!in.thumb) {
        // Only used for constant tracking; be conservative.
        uint32_t cls = (op >> 25) & 7;
        if (cls == 4) return (op >> 20) & 1 ? (op & 0xFFFF) | (1u << ((op >> 16) & 15)) : 1u << ((op >> 16) & 15);
        if (cls == 5) return 0;
        return 0xFFFF;
    }
    uint32_t r0 = op & 7;
    switch (op >> 13) {
    case 0: return 1u << r0;                                   // shifts, add/sub
    case 1: return ((op >> 11) & 3) == 1 ? 0 : 1u << ((op >> 8) & 7);  // mov/cmp/add/sub imm
    case 2:
        if ((op & 0xFC00) == 0x4000) {                          // ALU
            uint32_t alu = (op >> 6) & 15;
            return (alu == 8 || alu == 10 || alu == 11) ? 0 : 1u << r0;
        }
        if ((op & 0xFC00) == 0x4400) {                          // hi ops
            uint32_t sub = (op >> 8) & 3;
            if (sub == 1 || sub == 3) return sub == 3 && (op & 0x80) ? 0xFFFF : 0;
            return 1u << ((op & 7) | ((op >> 4) & 8));
        }
        if ((op & 0xF800) == 0x4800) return 1u << ((op >> 8) & 7);  // ldr pc-rel
        return (op & 0x0800) || ((op >> 9) & 7) >= 3 ? 1u << r0 : 0;  // reg-offset load/store
    case 3: return (op & 0x0800) ? 1u << r0 : 0;                 // imm5 load/store
    case 4:
        if (op & 0x1000) return (op & 0x0800) ? 1u << ((op >> 8) & 7) : 0;  // sp-relative
        return (op & 0x0800) ? 1u << r0 : 0;                     // halfword
    case 5:
        if (!(op & 0x1000)) return 1u << ((op >> 8) & 7);        // add rd, pc/sp
        if ((op & 0xFF00) == 0xB000) return 1u << 13;
        if ((op & 0xF600) == 0xB400) return (1u << 13) | ((op & 0x0800) ? (op & 0xFF) | ((op & 0x100) << 7) : 0);
        return 0xFFFF;
    case 6:
        if (!(op & 0x1000)) return (1u << ((op >> 8) & 7)) | ((op & 0x0800) ? (op & 0xFF) : 0);  // ldmia/stmia
        return 0;                                                 // cond branch / swi
    default: return 0;
    }
}

}  // namespace recomp
