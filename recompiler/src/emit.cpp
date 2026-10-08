// C code generation for ARMv5TE (ARM + Thumb) guest functions.
//
// Conventions in generated code (see runtime/include/ntr/recomp.h):
//   R(n)            guest register n
//   c->n/z/c/v/q    condition flags (0/1)
//   entry_lr        LR on entry; an indirect jump to it is a function return
//   ntr_call()      runtime dispatch for calls/jumps the recompiler cannot bind statically
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <fstream>
#include <stdexcept>

#include "recomp/recomp.hpp"

namespace recomp {

namespace {

std::string fmt(const char* f, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    return buf;
}

std::string hexc(uint32_t v) { return fmt("0x%08Xu", v); }

const char* cond_expr(uint32_t cond) {
    switch (cond) {
    case 0x0: return "c->z";
    case 0x1: return "!c->z";
    case 0x2: return "c->c";
    case 0x3: return "!c->c";
    case 0x4: return "c->n";
    case 0x5: return "!c->n";
    case 0x6: return "c->v";
    case 0x7: return "!c->v";
    case 0x8: return "c->c && !c->z";
    case 0x9: return "!c->c || c->z";
    case 0xA: return "c->n == c->v";
    case 0xB: return "c->n != c->v";
    case 0xC: return "!c->z && c->n == c->v";
    case 0xD: return "c->z || c->n != c->v";
    default: return "1";
    }
}

inline uint32_t bits(uint32_t v, int hi, int lo) { return (v >> lo) & ((1u << (hi - lo + 1)) - 1); }
inline uint32_t rotr(uint32_t v, uint32_t s) { s &= 31; return s ? (v >> s) | (v << (32 - s)) : v; }

struct Operand {
    std::string val;    // C expression
    std::string carry;  // C expression for the shifter carry-out
    std::string setup;  // statements to run first (register-specified shifts)
};

class FunctionEmitter {
public:
    FunctionEmitter(const Program& p, const Analysis& a, const Function& f)
        : prog_(p), an_(a), fn_(f), mod_(*p.find_module(f.module)) {}

    std::string run(size_t& instr_count) {
        out_ += "void " + fn_.c_name() + "(NtrCpu* c) {\n";
        if (!fn_.name.empty()) out_ = "/* " + fn_.name + " */\n" + out_;
        if (!fn_.hook.empty()) {
            out_ += "    " + fn_.hook + "(c);\n}\n\n";
            return out_;
        }
        if (fn_.encrypted || fn_.instrs.empty()) {
            out_ += "    ntr_unimplemented(c, " + hexc(fn_.entry) + ");\n}\n\n";
            return out_;
        }
        out_ += "    const uint32_t entry_lr = R(14);\n    (void)entry_lr;\n    NTR_ENTER(" + hexc(fn_.entry | (fn_.thumb ? 1u : 0u)) + ");\n    POLL();\n";
        if (!fn_.hook_before.empty()) out_ += "    " + fn_.hook_before + "(c);\n";
        for (const std::string& w : fn_.warnings) out_ += "    /* warning: " + w + " */\n";
        if (!fn_.instrs.empty() && fn_.instrs.begin()->first != fn_.entry) line(fmt("goto L_%08x;", fn_.entry));

        std::vector<const Instr*> list;
        for (auto& [a, in] : fn_.instrs) list.push_back(&in);
        for (size_t i = 0; i < list.size(); i++) {
            const Instr& in = *list[i];
            if (fn_.labels.count(in.addr) || i == 0) {
                if (fn_.labels.count(in.addr)) line(fmt("L_%08x:;", in.addr), 0);
                // Approximate cycle cost: one per instruction up to the next label.
                size_t n = 0;
                for (size_t j = i; j < list.size() && (j == i || !fn_.labels.count(list[j]->addr)); j++) n++;
                line(fmt("c->cycles += %zu;", n));
            }
            if (fn_.labels.count(in.addr)) known_.clear();
            cur_ = &in;
            if (in.thumb)
                emit_thumb(in);
            else
                emit_arm(in);
            update_known(in);
            instr_count++;

            // Fallthrough handling when the next address is not emitted next.
            if (in.falls_through()) {
                uint32_t next = in.addr + in.size;
                bool next_is_following = i + 1 < list.size() && list[i + 1]->addr == next;
                if (!next_is_following) {
                    if (fn_.instrs.count(next))
                        line(fmt("goto L_%08x;", next));
                    else
                        line(tail_call(next, fn_.thumb));
                }
            }
        }
        out_ += "}\n\n";
        return out_;
    }

private:
    const Program& prog_;
    const Analysis& an_;
    const Function& fn_;
    const Module& mod_;
    const Instr* cur_ = nullptr;
    std::string out_;
    std::map<int, uint32_t> known_;  // registers holding a known constant (literal loads) in this block

    void line(const std::string& s, int indent = 1) {
        out_.append(size_t(indent) * 4, ' ');
        out_ += s;
        out_ += '\n';
    }

    // ---- control flow helpers ----

    // Code for transferring control to a static target (bit 0 of target ignored; `thumb` gives the mode).
    std::string tail_call(uint32_t target, bool thumb) {
        if (thumb == fn_.thumb && fn_.instrs.count(target)) return fmt("goto L_%08x;", target);
        auto tm = static_target_module(prog_, fn_.module, target);
        if (tm) {
            if (const Function* f = an_.find(*tm, target); f && f->thumb == thumb)
                return f->c_name() + "(c); return;";
        }
        return fmt("ntr_call(c, %s); return;", hexc(target | (thumb ? 1 : 0)).c_str());
    }

    std::string call(uint32_t target, bool thumb) {
        auto tm = static_target_module(prog_, fn_.module, target);
        if (tm) {
            if (const Function* f = an_.find(*tm, target); f && f->thumb == thumb) return f->c_name() + "(c);";
        }
        return fmt("ntr_call(c, %s);", hexc(target | (thumb ? 1 : 0)).c_str());
    }

    // Jump to a computed address held in C variable `t`. `interwork`: bit 0 selects Thumb (BX/LDR pc on ARMv5).
    std::string indirect_jump(const std::string& t_expr, bool interwork) {
        std::string s = "{ uint32_t t = " + t_expr + "; ";
        if (!interwork) s += fn_.thumb ? "t |= 1u; " : "t &= ~3u; ";
        s += "if (t == entry_lr) return; ntr_jump(c, t, " + hexc(cur_->addr) + ", entry_lr); return; }";
        return s;
    }

    std::string branch_to(uint32_t target, bool thumb) {
        std::string s;
        if (target <= cur_->addr && thumb == fn_.thumb && fn_.instrs.count(target)) s = "POLL(); ";
        return s + tail_call(target, thumb);
    }

    void emit_jump(const Instr& in) {
        std::string body = branch_to(in.target, in.target_thumb);
        if (in.link) body = "R(14) = " + hexc((in.addr + in.size) | (in.thumb ? 1 : 0)) + "; " + body;
        if (in.cond != 0xE)
            line(std::string("if (") + cond_expr(in.cond) + ") { " + body + " }");
        else
            line(body);
    }

    void emit_jump_table(const Instr& in, const std::string& target_expr) {
        std::string s = "{ uint32_t t = " + target_expr + "; switch (t) {";
        std::set<uint32_t> seen;
        for (uint32_t t : in.table) {
            if (!seen.insert(t).second) continue;
            s += fmt(" case %s: ", hexc(t).c_str()) + tail_call(t, fn_.thumb);
        }
        s += " default: ntr_bad_jump(c, " + hexc(in.addr) + ", t); return; } }";
        line(s);
    }

    void update_known(const Instr& in) {
        uint32_t w = regs_written(in);
        for (int r = 0; r < 16; r++)
            if (w & (1u << r)) known_.erase(r);
        if (in.thumb && (in.op & 0xF800) == 0x4800) {  // ldr rd, [pc, #imm]
            uint32_t ea = ((in.addr + 4) & ~3u) + (in.op & 0xFF) * 4;
            if (mod_.contains(ea, 4)) known_[int((in.op >> 8) & 7)] = mod_.read32(ea);
        }
    }

    // ---- ARM ----

    std::string reg(uint32_t r, uint32_t pc_value) { return r == 15 ? hexc(pc_value) : fmt("R(%u)", r); }

    Operand shift_imm(uint32_t rm, uint32_t type, uint32_t amt, uint32_t pc_value) {
        std::string v = reg(rm, pc_value);
        switch (type) {
        case 0:
            if (amt == 0) return {v, "c->c", ""};
            return {fmt("(%s << %u)", v.c_str(), amt), fmt("((%s >> %u) & 1)", v.c_str(), 32 - amt), ""};
        case 1:
            if (amt == 0) return {"0u", fmt("(%s >> 31)", v.c_str()), ""};
            return {fmt("(%s >> %u)", v.c_str(), amt), fmt("((%s >> %u) & 1)", v.c_str(), amt - 1), ""};
        case 2:
            if (amt == 0) return {fmt("(uint32_t)((int32_t)%s >> 31)", v.c_str()), fmt("(%s >> 31)", v.c_str()), ""};
            return {fmt("(uint32_t)((int32_t)%s >> %u)", v.c_str(), amt), fmt("((%s >> %u) & 1)", v.c_str(), amt - 1), ""};
        default:
            if (amt == 0) return {fmt("((c->c << 31) | (%s >> 1))", v.c_str()), fmt("(%s & 1)", v.c_str()), ""};
            return {fmt("ntr_ror(%s, %u)", v.c_str(), amt), fmt("((%s >> %u) & 1)", v.c_str(), amt - 1), ""};
        }
    }

    Operand shift_reg(uint32_t rm, uint32_t type, uint32_t rs, uint32_t pc_value) {
        static const char* fns[] = {"ntr_lsl_reg", "ntr_lsr_reg", "ntr_asr_reg", "ntr_ror_reg"};
        std::string setup = fmt("uint32_t sc = c->c; uint32_t op2 = %s(%s, R(%u), &sc); ", fns[type],
                                reg(rm, pc_value).c_str(), rs);
        return {"op2", "sc", setup};
    }

    // Data-processing operand 2 for the instruction at `a`.
    Operand dp_operand(uint32_t op, uint32_t a) {
        if (op & (1u << 25)) {
            uint32_t rot = bits(op, 11, 8) * 2;
            uint32_t v = rotr(op & 0xFF, rot);
            return {hexc(v), rot ? fmt("%uu", v >> 31) : "c->c", ""};
        }
        uint32_t rm = op & 15, type = bits(op, 6, 5);
        if (op & 0x10) return shift_reg(rm, type, bits(op, 11, 8), a + 12);
        return shift_imm(rm, type, bits(op, 11, 7), a + 8);
    }

    void emit_arm(const Instr& in) {
        uint32_t op = in.op, a = in.addr;
        std::string body;
        bool cond = in.cond != 0xE;

        if (in.flow == Flow::Jump) { emit_jump(in); return; }
        if (in.flow == Flow::Call) {
            body = fmt("R(14) = %s; ", hexc(a + 4).c_str()) + call(in.target, in.target_thumb);
        } else if (in.flow == Flow::Undefined) {
            body = fmt("ntr_undefined(c, %s, %s); return;", hexc(a).c_str(), hexc(op).c_str());
        } else if (in.flow == Flow::JumpTable) {
            if (cond) out_ += std::string("    if (") + cond_expr(in.cond) + ")\n";
            emit_jump_table(in, fmt("%s + (R(%u) << 2)", hexc(a + 8).c_str(), op & 15));
            return;
        } else {
            body = arm_body(op, a);
        }
        if (cond)
            line(std::string("if (") + cond_expr(in.cond) + ") { " + body + " }");
        else
            line(body);
    }

    std::string arm_body(uint32_t op, uint32_t a) {
        if ((op >> 28) == 0xF) return "/* pld */";
        uint32_t rd = bits(op, 15, 12), rn = bits(op, 19, 16);
        switch (bits(op, 27, 25)) {
        case 0:
        case 1: {
            if ((op & 0x0FFFFFF0) == 0x012FFF10) {
                if (cur_->flow == Flow::IndirectCall) return fmt("ntr_call(c, R(%u));", op & 15);  // after mov lr, pc
                return indirect_jump(fmt("R(%u)", op & 15), true);
            }
            if ((op & 0x0FFFFFF0) == 0x012FFF30)
                return fmt("{ uint32_t t = R(%u); R(14) = %s; ntr_call(c, t); }", op & 15, hexc(a + 4).c_str());
            bool imm = op & (1u << 25);
            if (!imm && (op & 0x90) == 0x90) return arm_extra(op, a);
            uint32_t opcode = bits(op, 24, 21);
            bool s = op & (1u << 20);
            if (opcode >= 8 && opcode <= 11 && !s) return arm_misc(op, a);
            return arm_dp(op, a);
        }
        case 2:
        case 3: return arm_ldr_str(op, a);
        case 4: return arm_ldm_stm(op, a);
        case 7:
            if (op & (1u << 24)) return fmt("ntr_swi(c, %uu, %s);", bits(op, 23, 16), hexc(a).c_str());
            if (op & 0x10) {
                uint32_t cp = bits(op, 11, 8), op1 = bits(op, 23, 21), crn = rn, crm = op & 15, op2 = bits(op, 7, 5);
                if (op & (1u << 20)) {
                    if (rd == 15)
                        return fmt("{ uint32_t v = ntr_mrc(c, %u, %u, %u, %u, %u); c->n = v >> 31; c->z = (v >> 30) & 1; "
                                   "c->c = (v >> 29) & 1; c->v = (v >> 28) & 1; }", cp, op1, crn, crm, op2);
                    return fmt("R(%u) = ntr_mrc(c, %u, %u, %u, %u, %u);", rd, cp, op1, crn, crm, op2);
                }
                return fmt("ntr_mcr(c, %u, %u, %u, %u, %u, %s);", cp, op1, crn, crm, op2, reg(rd, a + 12).c_str());
            }
            break;
        default: break;
        }
        return fmt("ntr_undefined(c, %s, %s); return;", hexc(a).c_str(), hexc(op).c_str());
    }

    std::string arm_dp(uint32_t op, uint32_t a) {
        uint32_t opcode = bits(op, 24, 21), rn = bits(op, 19, 16), rd = bits(op, 15, 12);
        bool s = op & (1u << 20);
        Operand o2 = dp_operand(op, a);
        uint32_t pc_n = (op & (1u << 25)) || !(op & 0x10) ? a + 8 : a + 12;
        std::string n = reg(rn, pc_n);
        std::string b = "{ " + o2.setup;
        if (!o2.setup.empty() || o2.val.find("R(") != std::string::npos) {
            if (o2.setup.empty()) b += "uint32_t op2 = " + o2.val + "; ";
            o2.val = "op2";
        }
        bool flags = s && rd != 15;
        bool logical = false;
        std::string res;
        switch (opcode) {
        case 0x0: res = n + " & " + o2.val; logical = true; break;
        case 0x1: res = n + " ^ " + o2.val; logical = true; break;
        case 0x2: res = flags ? "ntr_sub_flags(c, " + n + ", " + o2.val + ")" : n + " - " + o2.val; break;
        case 0x3: res = flags ? "ntr_sub_flags(c, " + o2.val + ", " + n + ")" : o2.val + " - " + n; break;
        case 0x4: res = flags ? "ntr_add_flags(c, " + n + ", " + o2.val + ")" : n + " + " + o2.val; break;
        case 0x5: res = flags ? "ntr_adc_flags(c, " + n + ", " + o2.val + ", c->c)" : n + " + " + o2.val + " + c->c"; break;
        case 0x6: res = flags ? "ntr_sbc_flags(c, " + n + ", " + o2.val + ", c->c)" : n + " - " + o2.val + " - (c->c ^ 1u)"; break;
        case 0x7: res = flags ? "ntr_sbc_flags(c, " + o2.val + ", " + n + ", c->c)" : o2.val + " - " + n + " - (c->c ^ 1u)"; break;
        case 0x8: b += "uint32_t res = " + n + " & " + o2.val + "; ntr_nz(c, res); c->c = " + o2.carry + "; }"; return b;
        case 0x9: b += "uint32_t res = " + n + " ^ " + o2.val + "; ntr_nz(c, res); c->c = " + o2.carry + "; }"; return b;
        case 0xA: b += "ntr_sub_flags(c, " + n + ", " + o2.val + "); }"; return b;
        case 0xB: b += "ntr_add_flags(c, " + n + ", " + o2.val + "); }"; return b;
        case 0xC: res = n + " | " + o2.val; logical = true; break;
        case 0xD: res = o2.val; logical = true; break;
        case 0xE: res = n + " & ~" + o2.val; logical = true; break;
        case 0xF: res = "~" + o2.val; logical = true; break;
        }
        b += "uint32_t res = " + res + "; ";
        if (flags && logical) {
            b += "ntr_nz(c, res); ";
            if (o2.carry != "c->c") b += "c->c = " + o2.carry + "; ";
        }
        if (rd == 15) {
            if (s) b += "ntr_exception_return(c, res); return; ";
            else b += indirect_jump("res", false) + " ";
        } else {
            b += fmt("R(%u) = res; ", rd);
        }
        return b + "}";
    }

    std::string arm_misc(uint32_t op, uint32_t a) {
        uint32_t rd = bits(op, 15, 12), rn = bits(op, 19, 16), rm = op & 15, rs = bits(op, 11, 8);
        if ((op & 0x0FBF0FFF) == 0x010F0000)  // MRS
            return op & (1u << 22) ? fmt("R(%u) = ntr_get_spsr(c);", rd) : fmt("R(%u) = ntr_get_cpsr(c);", rd);
        if ((op & 0x0FB0FFF0) == 0x0120F000 || (op & 0x0FB0F000) == 0x0320F000) {  // MSR
            std::string v = (op & (1u << 25)) ? hexc(rotr(op & 0xFF, bits(op, 11, 8) * 2)) : fmt("R(%u)", rm);
            uint32_t mask = 0;
            if (op & (1u << 16)) mask |= 0x000000FF;
            if (op & (1u << 17)) mask |= 0x0000FF00;
            if (op & (1u << 18)) mask |= 0x00FF0000;
            if (op & (1u << 19)) mask |= 0xFF000000;
            if (op & (1u << 22)) return fmt("ntr_set_spsr(c, %s, %s);", v.c_str(), hexc(mask).c_str());
            if (mask == 0xFF000000)
                return "{ uint32_t v = " + v + "; c->n = v >> 31; c->z = (v >> 30) & 1; c->c = (v >> 29) & 1; "
                       "c->v = (v >> 28) & 1; c->q = (v >> 27) & 1; }";
            return fmt("ntr_set_cpsr(c, %s, %s);", v.c_str(), hexc(mask).c_str());
        }
        if ((op & 0x0FFF0FF0) == 0x016F0F10) return fmt("R(%u) = ntr_clz(R(%u));", rd, rm);
        if ((op & 0x0F9000F0) == 0x01000050) {  // QADD/QSUB/QDADD/QDSUB: Rd = Rm op Rn
            switch (bits(op, 22, 21)) {
            case 0: return fmt("R(%u) = ntr_qadd(c, R(%u), R(%u));", rd, rm, rn);
            case 1: return fmt("R(%u) = ntr_qsub(c, R(%u), R(%u));", rd, rm, rn);
            case 2: return fmt("R(%u) = ntr_qadd(c, R(%u), ntr_qadd(c, R(%u), R(%u)));", rd, rm, rn, rn);
            default: return fmt("R(%u) = ntr_qsub(c, R(%u), ntr_qadd(c, R(%u), R(%u)));", rd, rm, rn, rn);
            }
        }
        if ((op & 0x0F900090) == 0x01000080) {  // signed halfword multiplies
            // Here bits 19:16 = Rd (or RdHi), 15:12 = Rn (or RdLo)
            uint32_t d = rn, acc = rd;
            auto half = [](uint32_t r, bool top) {
                return top ? fmt("(int32_t)(int16_t)(R(%u) >> 16)", r) : fmt("(int32_t)(int16_t)R(%u)", r);
            };
            bool x = op & 0x20, y = op & 0x40;
            switch (bits(op, 22, 21)) {
            case 0:  // SMLAxy
                return fmt("R(%u) = ntr_add_q(c, (uint32_t)(%s * %s), R(%u));", d, half(rm, x).c_str(), half(rs, y).c_str(), acc);
            case 1:
                if (x)  // SMULWy
                    return fmt("R(%u) = (uint32_t)(((int64_t)(int32_t)R(%u) * %s) >> 16);", d, rm, half(rs, y).c_str());
                return fmt("R(%u) = ntr_add_q(c, (uint32_t)(((int64_t)(int32_t)R(%u) * %s) >> 16), R(%u));", d, rm,
                           half(rs, y).c_str(), acc);  // SMLAWy
            case 2:  // SMLALxy: RdHi = bits 19:16, RdLo = bits 15:12
                return fmt("{ int64_t acc = (int64_t)(((uint64_t)R(%u) << 32) | R(%u)); acc += (int64_t)(%s * %s); "
                           "R(%u) = (uint32_t)acc; R(%u) = (uint32_t)((uint64_t)acc >> 32); }",
                           d, acc, half(rm, x).c_str(), half(rs, y).c_str(), acc, d);
            default:  // SMULxy
                return fmt("R(%u) = (uint32_t)(%s * %s);", d, half(rm, x).c_str(), half(rs, y).c_str());
            }
        }
        return fmt("ntr_undefined(c, %s, %s); return;", hexc(a).c_str(), hexc(op).c_str());
    }

    // Multiplies, SWP and halfword/doubleword transfers.
    std::string arm_extra(uint32_t op, uint32_t a) {
        uint32_t rn = bits(op, 19, 16), rd = bits(op, 15, 12), rs = bits(op, 11, 8), rm = op & 15;
        uint32_t sh = bits(op, 6, 5);
        bool s = op & (1u << 20);
        if (sh == 0) {
            if ((op & 0x0FC000F0) == 0x00000090) {  // MUL/MLA: Rd = bits 19:16, Rn = 15:12
                std::string e = fmt("R(%u) * R(%u)", rm, rs);
                if (op & (1u << 21)) e += fmt(" + R(%u)", rd);
                std::string b = fmt("{ uint32_t res = %s; R(%u) = res; ", e.c_str(), rn);
                if (s) b += "ntr_nz(c, res); ";
                return b + "}";
            }
            if ((op & 0x0F8000F0) == 0x00800090) {  // UMULL/UMLAL/SMULL/SMLAL: RdHi = 19:16, RdLo = 15:12
                bool sign = op & (1u << 22), acc = op & (1u << 21);
                std::string prod = sign ? fmt("(uint64_t)((int64_t)(int32_t)R(%u) * (int32_t)R(%u))", rm, rs)
                                        : fmt("((uint64_t)R(%u) * R(%u))", rm, rs);
                std::string b = "{ uint64_t res = " + prod;
                if (acc) b += fmt(" + (((uint64_t)R(%u) << 32) | R(%u))", rn, rd);
                b += fmt("; R(%u) = (uint32_t)res; R(%u) = (uint32_t)(res >> 32); ", rd, rn);
                if (s) b += "c->n = (uint32_t)(res >> 63); c->z = res == 0; ";
                return b + "}";
            }
            if ((op & 0x0FB00FF0) == 0x01000090) {  // SWP/SWPB
                if (op & (1u << 22))
                    return fmt("{ uint32_t ea = R(%u); uint32_t v = ntr_rd8(c, ea); ntr_wr8(c, ea, R(%u)); R(%u) = v; }", rn, rm, rd);
                return fmt("{ uint32_t ea = R(%u); uint32_t v = ntr_ldr(c, ea); ntr_wr32(c, ea, R(%u)); R(%u) = v; }", rn, rm, rd);
            }
            return fmt("ntr_undefined(c, %s, %s); return;", hexc(a).c_str(), hexc(op).c_str());
        }
        bool p = op & (1u << 24), u = op & (1u << 23), immf = op & (1u << 22), w = op & (1u << 21), l = s;
        std::string off = immf ? fmt("%uu", (bits(op, 11, 8) << 4) | (op & 15)) : fmt("R(%u)", rm);
        std::string base = reg(rn, a + 8);
        const char* sign = u ? "+" : "-";
        std::string b = "{ uint32_t base = " + base + "; uint32_t moved = base " + sign + " " + off + "; ";
        b += std::string("uint32_t ea = ") + (p ? "moved" : "base") + "; ";
        bool wb = (!p || w) && rn != 15;
        if (l) {
            std::string v;
            if (sh == 1) v = "ntr_rd16(c, ea)";
            else if (sh == 2) v = "(uint32_t)(int32_t)(int8_t)ntr_rd8(c, ea)";
            else v = "(uint32_t)(int32_t)(int16_t)ntr_rd16(c, ea)";
            b += "uint32_t v = " + v + "; ";
            if (wb) b += fmt("R(%u) = moved; ", rn);
            if (rd == 15) b += indirect_jump("v", true) + " ";
            else b += fmt("R(%u) = v; ", rd);
        } else if (sh == 1) {  // STRH
            b += fmt("ntr_wr16(c, ea, %s); ", reg(rd, a + 12).c_str());
            if (wb) b += fmt("R(%u) = moved; ", rn);
        } else if (sh == 2) {  // LDRD
            b += fmt("uint32_t v0 = ntr_rd32(c, ea), v1 = ntr_rd32(c, ea + 4); ");
            if (wb) b += fmt("R(%u) = moved; ", rn);
            b += fmt("R(%u) = v0; R(%u) = v1; ", rd, (rd + 1) & 15);
        } else {  // STRD
            b += fmt("ntr_wr32(c, ea, R(%u)); ntr_wr32(c, ea + 4, R(%u)); ", rd, (rd + 1) & 15);
            if (wb) b += fmt("R(%u) = moved; ", rn);
        }
        return b + "}";
    }

    std::string arm_ldr_str(uint32_t op, uint32_t a) {
        bool regoff = op & (1u << 25), p = op & (1u << 24), u = op & (1u << 23), byte = op & (1u << 22);
        bool w = op & (1u << 21), l = op & (1u << 20);
        uint32_t rn = bits(op, 19, 16), rd = bits(op, 15, 12);
        std::string off = regoff ? shift_imm(op & 15, bits(op, 6, 5), bits(op, 11, 7), a + 8).val : fmt("%uu", op & 0xFFF);

        // PC-relative literal load: resolve at recompile time.
        if (l && !byte && rn == 15 && p && !w && !regoff) {
            uint32_t ea = a + 8 + (u ? (op & 0xFFF) : uint32_t(-int32_t(op & 0xFFF)));
            if (mod_.contains(ea, 4) && !(ea & 3)) {
                uint32_t v = mod_.read32(ea);
                if (rd == 15) return indirect_jump(hexc(v), true);
                return fmt("R(%u) = %s;", rd, hexc(v).c_str());
            }
        }
        std::string b = "{ uint32_t base = " + reg(rn, a + 8) + "; uint32_t moved = base " + (u ? "+ " : "- ") + off + "; ";
        b += std::string("uint32_t ea = ") + (p ? "moved" : "base") + "; ";
        bool wb = (!p || w) && rn != 15;
        if (l) {
            b += std::string("uint32_t v = ") + (byte ? "ntr_rd8(c, ea)" : "ntr_ldr(c, ea)") + "; ";
            if (wb) b += fmt("R(%u) = moved; ", rn);
            if (rd == 15) b += indirect_jump("v", true) + " ";
            else b += fmt("R(%u) = v; ", rd);
        } else {
            b += std::string(byte ? "ntr_wr8" : "ntr_wr32") + "(c, ea, " + reg(rd, a + 12) + "); ";
            if (wb) b += fmt("R(%u) = moved; ", rn);
        }
        return b + "}";
    }

    std::string arm_ldm_stm(uint32_t op, uint32_t a) {
        bool p = op & (1u << 24), u = op & (1u << 23), s = op & (1u << 22), w = op & (1u << 21), l = op & (1u << 20);
        uint32_t rn = bits(op, 19, 16), list = op & 0xFFFF;
        int n = 0;
        for (int i = 0; i < 16; i++) n += (list >> i) & 1;
        if (n == 0) return fmt("ntr_undefined(c, %s, %s); return;", hexc(a).c_str(), hexc(op).c_str());
        bool user = s && !(l && (list & 0x8000));
        std::string b = fmt("{ uint32_t base = R(%u); uint32_t ea = base", rn);
        if (u) b += p ? " + 4" : "";
        else b += fmt(" - %d", 4 * n) + (p ? "" : " + 4");
        b += "; ";
        std::string wbv = fmt("base %s %du", u ? "+" : "-", 4 * n);
        if (l) {
            if (w && !(list & (1u << rn))) b += fmt("R(%u) = %s; ", rn, wbv.c_str());
            for (int i = 0; i < 16; i++) {
                if (!(list & (1u << i))) continue;
                if (i == 15) {
                    b += "uint32_t pcv = ntr_rd32(c, ea); ";
                } else if (user) {
                    b += fmt("ntr_set_user_reg(c, %d, ntr_rd32(c, ea)); ea += 4; ", i);
                } else {
                    b += fmt("R(%d) = ntr_rd32(c, ea); ea += 4; ", i);
                }
            }
            if (list & 0x8000) {
                if (s) b += "ntr_exception_return(c, pcv); return; ";
                else b += indirect_jump("pcv", true) + " ";
            }
        } else {
            for (int i = 0; i < 16; i++) {
                if (!(list & (1u << i))) continue;
                std::string v = i == 15 ? hexc(a + 12) : user ? fmt("ntr_get_user_reg(c, %d)", i) : fmt("R(%d)", i);
                if (i == int(rn)) v = "base";
                b += fmt("ntr_wr32(c, ea, %s); ea += 4; ", v.c_str());
            }
            if (w) b += fmt("R(%u) = %s; ", rn, wbv.c_str());
        }
        return b + "}";
    }

    // ---- Thumb ----

    void emit_thumb(const Instr& in) {
        uint32_t op = in.op & 0xFFFF, a = in.addr;
        if (in.flow == Flow::Jump) { emit_jump(in); return; }
        if (in.flow == Flow::Call) {
            line(fmt("R(14) = %s; ", hexc((a + 4) | 1).c_str()) + call(in.target, in.target_thumb));
            return;
        }
        if (in.flow == Flow::Undefined) {
            line(fmt("ntr_undefined(c, %s, %s); return;", hexc(a).c_str(), hexc(in.op).c_str()));
            return;
        }
        if (in.flow == Flow::JumpTable) {
            emit_jump_table(in, fmt("(%s + R(%u)) & ~1u", hexc(a + 4).c_str(), (op >> 3) & 15));
            return;
        }
        line(thumb_body(op, a));
    }

    std::string thumb_body(uint32_t op, uint32_t a) {
        uint32_t rd = op & 7, rs = (op >> 3) & 7;
        switch (op >> 13) {
        case 0: {
            uint32_t sub = bits(op, 12, 11);
            if (sub == 3) {  // add/sub register or imm3
                std::string rhs = (op & 0x400) ? fmt("%uu", bits(op, 8, 6)) : fmt("R(%u)", bits(op, 8, 6));
                return fmt("R(%u) = %s(c, R(%u), %s);", rd, (op & 0x200) ? "ntr_sub_flags" : "ntr_add_flags", rs, rhs.c_str());
            }
            Operand o = shift_imm(rs, sub, bits(op, 10, 6), a + 4);
            std::string b = fmt("{ uint32_t res = %s; ", o.val.c_str());
            if (o.carry != "c->c") b += "c->c = " + o.carry + "; ";
            return b + fmt("R(%u) = res; ntr_nz(c, res); }", rd);
        }
        case 1: {
            uint32_t r = bits(op, 10, 8), imm = op & 0xFF;
            switch (bits(op, 12, 11)) {
            case 0: return fmt("R(%u) = %uu; ntr_nz(c, R(%u));", r, imm, r);
            case 1: return fmt("ntr_sub_flags(c, R(%u), %uu);", r, imm);
            case 2: return fmt("R(%u) = ntr_add_flags(c, R(%u), %uu);", r, r, imm);
            default: return fmt("R(%u) = ntr_sub_flags(c, R(%u), %uu);", r, r, imm);
            }
        }
        case 2:
            if ((op & 0xFC00) == 0x4000) return thumb_alu(op);
            if ((op & 0xFC00) == 0x4400) return thumb_hi(op, a);
            if ((op & 0xF800) == 0x4800) {
                uint32_t ea = ((a + 4) & ~3u) + (op & 0xFF) * 4;
                if (mod_.contains(ea, 4)) return fmt("R(%u) = %s;", bits(op, 10, 8), hexc(mod_.read32(ea)).c_str());
                return fmt("R(%u) = ntr_rd32(c, %s);", bits(op, 10, 8), hexc(ea).c_str());
            }
            {
                uint32_t ro = bits(op, 8, 6);
                std::string ea = fmt("R(%u) + R(%u)", rs, ro);
                switch (bits(op, 11, 9)) {
                case 0: return fmt("ntr_wr32(c, %s, R(%u));", ea.c_str(), rd);
                case 1: return fmt("ntr_wr16(c, %s, R(%u));", ea.c_str(), rd);
                case 2: return fmt("ntr_wr8(c, %s, R(%u));", ea.c_str(), rd);
                case 3: return fmt("R(%u) = (uint32_t)(int32_t)(int8_t)ntr_rd8(c, %s);", rd, ea.c_str());
                case 4: return fmt("R(%u) = ntr_ldr(c, %s);", rd, ea.c_str());
                case 5: return fmt("R(%u) = ntr_rd16(c, %s);", rd, ea.c_str());
                case 6: return fmt("R(%u) = ntr_rd8(c, %s);", rd, ea.c_str());
                default: return fmt("R(%u) = (uint32_t)(int32_t)(int16_t)ntr_rd16(c, %s);", rd, ea.c_str());
                }
            }
        case 3: {
            uint32_t imm = bits(op, 10, 6);
            bool byte = op & 0x1000, load = op & 0x800;
            std::string ea = fmt("R(%u) + %uu", rs, byte ? imm : imm * 4);
            if (load) return fmt("R(%u) = %s(c, %s);", rd, byte ? "ntr_rd8" : "ntr_ldr", ea.c_str());
            return fmt("%s(c, %s, R(%u));", byte ? "ntr_wr8" : "ntr_wr32", ea.c_str(), rd);
        }
        case 4:
            if (op & 0x1000) {  // sp-relative
                uint32_t r = bits(op, 10, 8);
                std::string ea = fmt("R(13) + %uu", (op & 0xFF) * 4);
                if (op & 0x800) return fmt("R(%u) = ntr_ldr(c, %s);", r, ea.c_str());
                return fmt("ntr_wr32(c, %s, R(%u));", ea.c_str(), r);
            } else {
                std::string ea = fmt("R(%u) + %uu", rs, bits(op, 10, 6) * 2);
                if (op & 0x800) return fmt("R(%u) = ntr_rd16(c, %s);", rd, ea.c_str());
                return fmt("ntr_wr16(c, %s, R(%u));", ea.c_str(), rd);
            }
        case 5:
            if (!(op & 0x1000)) {
                uint32_t r = bits(op, 10, 8), imm = (op & 0xFF) * 4;
                if (op & 0x800) return fmt("R(%u) = R(13) + %uu;", r, imm);
                return fmt("R(%u) = %s;", r, hexc(((a + 4) & ~3u) + imm).c_str());
            }
            if ((op & 0xFF00) == 0xB000)
                return fmt("R(13) %s= %uu;", (op & 0x80) ? "-" : "+", (op & 0x7F) * 4);
            if ((op & 0xF600) == 0xB400) return thumb_push_pop(op);
            break;
        case 6:
            if (!(op & 0x1000)) return thumb_ldm_stm(op);
            if (bits(op, 11, 8) == 15) return fmt("ntr_swi(c, %uu, %s);", op & 0xFF, hexc(a).c_str());
            break;
        default: break;
        }
        return fmt("ntr_undefined(c, %s, %s); return;", hexc(a).c_str(), hexc(op).c_str());
    }

    std::string thumb_alu(uint32_t op) {
        uint32_t rd = op & 7, rs = (op >> 3) & 7;
        std::string d = fmt("R(%u)", rd), s = fmt("R(%u)", rs);
        switch (bits(op, 9, 6)) {
        case 0x0: return d + " &= " + s + "; ntr_nz(c, " + d + ");";
        case 0x1: return d + " ^= " + s + "; ntr_nz(c, " + d + ");";
        case 0x2: return "{ uint32_t sc = c->c; " + d + " = ntr_lsl_reg(" + d + ", " + s + ", &sc); c->c = sc; ntr_nz(c, " + d + "); }";
        case 0x3: return "{ uint32_t sc = c->c; " + d + " = ntr_lsr_reg(" + d + ", " + s + ", &sc); c->c = sc; ntr_nz(c, " + d + "); }";
        case 0x4: return "{ uint32_t sc = c->c; " + d + " = ntr_asr_reg(" + d + ", " + s + ", &sc); c->c = sc; ntr_nz(c, " + d + "); }";
        case 0x5: return d + " = ntr_adc_flags(c, " + d + ", " + s + ", c->c);";
        case 0x6: return d + " = ntr_sbc_flags(c, " + d + ", " + s + ", c->c);";
        case 0x7: return "{ uint32_t sc = c->c; " + d + " = ntr_ror_reg(" + d + ", " + s + ", &sc); c->c = sc; ntr_nz(c, " + d + "); }";
        case 0x8: return "ntr_nz(c, " + d + " & " + s + ");";
        case 0x9: return d + " = ntr_sub_flags(c, 0u, " + s + ");";
        case 0xA: return "ntr_sub_flags(c, " + d + ", " + s + ");";
        case 0xB: return "ntr_add_flags(c, " + d + ", " + s + ");";
        case 0xC: return d + " |= " + s + "; ntr_nz(c, " + d + ");";
        case 0xD: return d + " *= " + s + "; ntr_nz(c, " + d + ");";
        case 0xE: return d + " &= ~" + s + "; ntr_nz(c, " + d + ");";
        default: return d + " = ~" + s + "; ntr_nz(c, " + d + ");";
        }
    }

    std::string thumb_hi(uint32_t op, uint32_t a) {
        uint32_t rd = (op & 7) | ((op >> 4) & 8), rm = (op >> 3) & 15;
        std::string m = reg(rm, a + 4);
        switch (bits(op, 9, 8)) {
        case 0:
            if (rd == 15) return indirect_jump(fmt("%s + %s", hexc(a + 4).c_str(), m.c_str()), false);
            return fmt("R(%u) = %s + %s;", rd, reg(rd, a + 4).c_str(), m.c_str());
        case 1: return fmt("ntr_sub_flags(c, %s, %s);", reg(rd, a + 4).c_str(), m.c_str());
        case 2:
            if (rd == 15) return indirect_jump(m, false);
            return fmt("R(%u) = %s;", rd, m.c_str());
        default:
            if (op & 0x80) {  // BLX reg
                if (auto k = known_.find(int(rm)); k != known_.end() && rm != 14)
                    return fmt("R(14) = %s; ", hexc((a + 2) | 1).c_str()) + call(k->second & ~1u, k->second & 1);
                return fmt("{ uint32_t t = %s; R(14) = %s; ntr_call(c, t); }", m.c_str(), hexc((a + 2) | 1).c_str());
            }
            // BX: a constant target loaded from a literal pool is a static tail call (thunks).
            if (auto k = known_.find(int(rm)); k != known_.end() && rm != 14) return tail_call(k->second & ~1u, k->second & 1);
            return indirect_jump(m, true);
        }
    }

    std::string thumb_push_pop(uint32_t op) {
        bool load = op & 0x800, extra = op & 0x100;
        uint32_t list = op & 0xFF;
        int n = extra;
        for (int i = 0; i < 8; i++) n += (list >> i) & 1;
        std::string b = "{ ";
        if (!load) {
            b += fmt("uint32_t ea = R(13) - %du; R(13) = ea; ", 4 * n);
            for (int i = 0; i < 8; i++)
                if (list & (1u << i)) b += fmt("ntr_wr32(c, ea, R(%d)); ea += 4; ", i);
            if (extra) b += "ntr_wr32(c, ea, R(14)); ";
            return b + "}";
        }
        b += "uint32_t ea = R(13); ";
        for (int i = 0; i < 8; i++)
            if (list & (1u << i)) b += fmt("R(%d) = ntr_rd32(c, ea); ea += 4; ", i);
        if (extra) {
            b += "{ uint32_t pcv = ntr_rd32(c, ea); R(13) = ea + 4; " + indirect_jump("pcv", true) + " } ";
        } else {
            b += "R(13) = ea; ";
        }
        return b + "}";
    }

    std::string thumb_ldm_stm(uint32_t op) {
        uint32_t rb = bits(op, 10, 8), list = op & 0xFF;
        bool load = op & 0x800;
        if (!list) return fmt("ntr_undefined(c, %s, %s); return;", hexc(cur_->addr).c_str(), hexc(op).c_str());
        int n = 0;
        for (int i = 0; i < 8; i++) n += (list >> i) & 1;
        std::string b = fmt("{ uint32_t base = R(%u); uint32_t ea = base; ", rb);
        if (load) {
            if (!(list & (1u << rb))) b += fmt("R(%u) = base + %du; ", rb, 4 * n);
            for (int i = 0; i < 8; i++)
                if (list & (1u << i)) b += fmt("R(%d) = ntr_rd32(c, ea); ea += 4; ", i);
        } else {
            for (int i = 0; i < 8; i++)
                if (list & (1u << i)) b += fmt("ntr_wr32(c, ea, %s); ea += 4; ", i == int(rb) ? "base" : fmt("R(%d)", i).c_str());
            b += fmt("R(%u) = base + %du; ", rb, 4 * n);
        }
        return b + "}";
    }
};

}  // namespace

EmitStats emit_c(const Program& program, const Analysis& analysis, const std::filesystem::path& out_dir,
                 const EmitOptions& options) {
    namespace fs = std::filesystem;
    fs::create_directories(out_dir);
    EmitStats stats;

    const std::string banner = std::string("/* ") + kGeneratedMarker +
                               " -- generated from the user's ROM. Do not commit or distribute. */\n";

    // Order: by module, then address.
    std::vector<const Function*> order;
    for (const Function& f : analysis.functions) order.push_back(&f);
    std::sort(order.begin(), order.end(), [](const Function* x, const Function* y) {
        return x->module != y->module ? x->module < y->module : x->entry < y->entry;
    });

    {
        std::ofstream h(out_dir / "funcs.h", std::ios::binary);
        h << banner << "#ifndef NTR_RECOMP_FUNCS_H\n#define NTR_RECOMP_FUNCS_H\n#include \"ntr/recomp.h\"\n\n";
        std::set<std::string> hooks;
        for (const Function* f : order) {
            h << "void " << f->c_name() << "(NtrCpu* c);\n";
            if (!f->hook.empty()) hooks.insert(f->hook);
            if (!f->hook_before.empty()) hooks.insert(f->hook_before);
        }
        for (const std::string& hk : hooks) h << "void " << hk << "(NtrCpu* c);\n";
        h << "\n#endif\n";
    }

    const size_t per_shard = (order.size() + options.shards - 1) / options.shards;
    for (size_t shard = 0; shard < options.shards; shard++) {
        char name[32];
        std::snprintf(name, sizeof name, "funcs_%03zu.c", shard);
        std::ofstream f(out_dir / name, std::ios::binary);
        f << banner << "#include \"funcs.h\"\n\n";
        for (size_t j = shard * per_shard; j < std::min(order.size(), (shard + 1) * per_shard); j++) {
            FunctionEmitter e(program, analysis, *order[j]);
            f << e.run(stats.instructions);
            stats.functions++;
        }
        stats.files++;
    }

    {
        std::ofstream t(out_dir / "func_table.c", std::ios::binary);
        t << banner << "#include \"funcs.h\"\n#include <stddef.h>\n\nconst NtrFuncEntry ntr_recomp_functions[] = {\n";
        for (const Function* f : order)
            t << "    {0x" << std::hex << (f->entry | (f->thumb ? 1 : 0)) << std::dec << "u, " << f->module << ", "
              << f->c_name() << "},\n";
        t << "};\nconst size_t ntr_recomp_function_count = " << order.size() << ";\n\n";

        // Overlays share run addresses, so the runtime identifies the resident one by up to four probe words: it
        // is resident when they all match. Every other module covering the same addresses must fail at least one,
        // except byte-identical copies (several of the small scene overlays are), which run the same code, so
        // either may be taken for the other. Without that exception such copies got no probes at all and counted
        // as resident whatever was loaded, and a call into a different overlay at the same address ran their
        // code instead.
        std::vector<const Module*> ovs;
        for (const Module& m : program.modules)
            if (is_overlay(m.id)) ovs.push_back(&m);
        t << "const NtrOverlayInfo ntr_recomp_overlays[] = {\n";
        for (const Module* o : ovs) {
            std::vector<const Module*> others;  // overlapping, not identical
            for (const Module& pm : program.modules) {
                const Module* p = &pm;
                if (p == o || p->end() <= o->base || o->end() <= p->base) continue;
                if (p->base == o->base && p->data == o->data) continue;
                others.push_back(p);
            }
            const uint32_t words = uint32_t(o->data.size() / 4);
            const auto differs = [&](const Module* p, uint32_t addr, uint32_t val) {
                return p->contains(addr, 4) && p->read32(addr) != val;
            };
            // Greedy: the word that tells the most remaining modules apart, until every one is told apart. Then
            // again from the start with the words left, so that up to four probes give a second, independent check.
            std::vector<std::pair<uint32_t, uint32_t>> probes;
            std::vector<const Module*> left = others;
            while (probes.size() < 4 && !others.empty()) {
                if (left.empty()) left = others;
                uint32_t best_addr = 0, best_val = 0;
                size_t best = 0;
                for (uint32_t w = 0; w < words; w++) {
                    const uint32_t addr = o->base + w * 4, val = o->read32(addr);
                    if (val == 0) continue;  // could be cleared memory
                    bool taken = false;
                    for (auto& pr : probes) taken |= pr.first == addr;
                    if (taken) continue;
                    size_t n = 0;
                    for (const Module* p : left) n += differs(p, addr, val);
                    if (n > best) {
                        best = n;
                        best_addr = addr;
                        best_val = val;
                    }
                }
                if (!best) break;
                probes.emplace_back(best_addr, best_val);
                std::erase_if(left, [&](const Module* p) { return differs(p, best_addr, best_val); });
            }
            // (Empty overlays, all zeros, have no code to call and need no probes.)
            const bool empty = std::all_of(o->data.begin(), o->data.end(), [](uint8_t b) { return b == 0; });
            for (const Module* p : empty ? std::vector<const Module*>{} : others) {
                bool told = false;
                for (auto& pr : probes) told |= differs(p, pr.first, pr.second);
                if (!told)
                    std::fprintf(stderr, "warning: overlay %s cannot be told apart from %s by its probe words\n",
                                 o->name.c_str(), p->name.c_str());
            }
            std::sort(probes.begin(), probes.end());
            probes.erase(std::unique(probes.begin(), probes.end()), probes.end());
            t << "    {" << (o->id - kModOverlayBase) << ", 0x" << std::hex << o->base << "u, 0x" << o->data.size()
              << "u, " << std::dec << (o->interpret ? "NTR_OVERLAY_INTERPRET" : "0") << ", " << probes.size() << ", {";
            for (auto& pr : probes) t << "0x" << std::hex << pr.first << "u, ";
            if (probes.empty()) t << "0";
            t << "}, {";
            for (auto& pr : probes) t << "0x" << std::hex << pr.second << "u, ";
            if (probes.empty()) t << "0";
            t << std::dec << "}},\n";
        }
        if (ovs.empty()) t << "    {0, 0, 0, 0, 0, {0}, {0}},\n";
        t << "};\nconst size_t ntr_recomp_overlay_count = " << ovs.size() << ";\n";
        stats.files++;
    }
    return stats;
}

}  // namespace recomp
