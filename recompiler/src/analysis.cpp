// Function body discovery by reachability from each entry point.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <deque>

#include "recomp/recomp.hpp"

namespace recomp {

std::string Function::c_name() const {
    char buf[64];
    const char* mod = module == kModMain ? "main" : module == kModItcm ? "itcm" : module == kModDtcm ? "dtcm" : nullptr;
    if (mod)
        std::snprintf(buf, sizeof buf, "fn_%s_%08x", mod, entry);
    else if (is_region(module))
        std::snprintf(buf, sizeof buf, "fn_rg%d_%08x", module - kModRegionBase, entry);
    else
        std::snprintf(buf, sizeof buf, "fn_ov%03d_%08x", module - kModOverlayBase, entry);
    return buf;
}

const Function* Analysis::find(int module, uint32_t entry) const {
    auto it = index.find({module, entry});
    return it == index.end() ? nullptr : &functions[it->second];
}

std::optional<int> static_target_module(const Program& program, int from_module, uint32_t addr) {
    // Same overlay: always resident together with the caller.
    if (is_overlay(from_module)) {
        const Module* self = program.find_module(from_module);
        if (self && self->contains(addr)) return from_module;
    }
    // A code region called from the module that carries it (or from a sibling region).
    const Module* from = program.find_module(from_module);
    const int family = from && from->parent >= 0 ? from->parent : from_module;
    for (const Module& m : program.modules)
        if (is_region(m.id) && m.parent == family && m.contains(addr)) return m.id;
    // A region's calls back into its parent overlay.
    if (from && from->parent >= 0) {
        const Module* p = program.find_module(from->parent);
        if (p && p->contains(addr)) return p->id;
    }
    for (const Module& m : program.modules) {
        if (is_overlay(m.id) || !m.contains(addr)) continue;
        return m.id;
    }
    return std::nullopt;
}

namespace {

std::string hex(uint32_t v) {
    char b[16];
    std::snprintf(b, sizeof b, "%08x", v);
    return b;
}

struct Builder {
    const Program& program;
    Analysis& out;
    std::deque<size_t> queue;

    size_t add_function(int module, uint32_t entry, bool thumb, std::string name = {}) {
        if (const Module* m = program.find_module(module); m && m->interpret) return SIZE_MAX;
        auto key = std::make_pair(module, entry);
        auto it = out.index.find(key);
        if (it != out.index.end()) {
            Function& f = out.functions[it->second];
            if (f.thumb != thumb)
                out.warnings.push_back("mode conflict for " + f.c_name() + ": called as " + (thumb ? "Thumb" : "ARM"));
            return it->second;
        }
        Function f;
        f.module = module;
        f.entry = entry;
        f.thumb = thumb;
        f.name = std::move(name);
        out.functions.push_back(std::move(f));
        out.index[key] = out.functions.size() - 1;
        queue.push_back(out.functions.size() - 1);
        return out.functions.size() - 1;
    }

    bool is_entry(int module, uint32_t addr) const { return out.index.count({module, addr}) != 0; }

    void explore(size_t fi) {
        // Copy what we need: add_function() may reallocate out.functions.
        const int module = out.functions[fi].module;
        const uint32_t entry = out.functions[fi].entry;
        const bool thumb = out.functions[fi].thumb;
        const uint32_t extent = out.functions[fi].extent;
        if (!out.functions[fi].hook.empty() || out.functions[fi].encrypted) return;

        const Module* m = program.find_module(module);
        std::map<uint32_t, Instr> instrs;
        std::set<uint32_t> labels;
        std::vector<std::string> warnings;
        std::vector<uint32_t> work{entry};

        auto enqueue_local = [&](uint32_t target) {
            // Branches to another function's entry are tail calls, not part of this body.
            if (target != entry && is_entry(module, target)) return;
            if (!m->contains(target, thumb ? 2 : 4)) {
                warnings.push_back("branch outside module to " + hex(target));
                return;
            }
            labels.insert(target);
            work.push_back(target);
        };

        while (!work.empty()) {
            uint32_t a = work.back();
            work.pop_back();
            if (instrs.count(a)) continue;
            if (a != entry && is_entry(module, a)) continue;  // fell into the next function: tail call
            Instr in = decode(*m, a, thumb);
            // mwcc emits Thumb BL to a label of the same function as a long-range branch.
            if (in.flow == Flow::Call && in.target_thumb == thumb && in.target > entry && in.target < entry + extent) {
                in.flow = Flow::Jump;
                in.link = true;
            }
            // ARMv4-style indirect call: `mov lr, pc` or `add lr, pc, #0` followed by `bx rX` (NitroSDK IRQ thread
            // switch, hand-written ITCM code). Targets loaded just before with `adr rX` become functions.
            if (!thumb && in.flow == Flow::IndirectJump && (in.op & 0x0FFFFFF0) == 0x012FFF10 && a >= m->base + 4 &&
                (m->read32(a - 4) == ((in.op & 0xF0000000) | 0x01A0E00F) ||
                 m->read32(a - 4) == ((in.op & 0xF0000000) | 0x028FE000))) {
                in.flow = Flow::IndirectCall;
                const uint32_t rx = in.op & 0xF;
                for (uint32_t b = a - 8; b >= m->base && b + 32 >= a; b -= 4) {
                    const uint32_t op = m->read32(b);
                    if ((op & 0x0FFFF000) != (0x028F0000u | (rx << 12))) continue;
                    const uint32_t rot = ((op >> 8) & 0xF) * 2, imm = op & 0xFF;
                    const uint32_t t = b + 8 + (rot ? (imm >> rot) | (imm << (32 - rot)) : imm);
                    if (m->contains(t, 4)) add_function(module, t, false);
                }
            }
            // Unbounded `add pc, pc, rX, lsl #2`: a computed jump into straight-line code (the unrolled loops of
            // the mwcc division helpers). Any later word of the function is a possible target.
            if (!thumb && in.flow == Flow::IndirectJump && (in.op & 0x0FFFFFF0) == 0x008FF100) {
                // A secondary entry inside a seeded function (no size of its own) ends where that one ends.
                uint32_t end = extent ? entry + extent : 0;
                for (const FunctionSeed& sd : program.config.functions)
                    if (!end && sd.module == module && sd.size && sd.addr < entry && entry < sd.addr + sd.size)
                        end = sd.addr + sd.size;
                for (uint32_t t = a + 8; t < end; t += 4) in.table.push_back(t);
                if (!in.table.empty()) in.flow = Flow::JumpTable;
            }
            instrs[a] = in;
            if (in.flow == Flow::Undefined) {
                warnings.push_back("undefined instruction at " + hex(a));
                continue;
            }
            switch (in.flow) {
            case Flow::Jump:
                if (in.target_thumb == thumb) enqueue_local(in.target);
                break;
            case Flow::Call: {
                auto tm = static_target_module(program, module, in.target);
                if (tm) {
                    const Module* tmod = program.find_module(*tm);
                    if (tmod->contains(in.target, in.target_thumb ? 2 : 4))
                        add_function(*tm, in.target, in.target_thumb);
                }
                break;
            }
            case Flow::JumpTable:
                for (uint32_t t : in.table) enqueue_local(t);
                break;
            default:
                break;
            }
            if (in.falls_through()) work.push_back(a + in.size);
        }

        // A fallthrough that is not the next emitted instruction needs a label.
        uint32_t prev_end = 0;
        bool prev_falls = false;
        for (auto& [a, in] : instrs) {
            if (prev_falls && prev_end != a && instrs.count(prev_end)) labels.insert(prev_end);
            prev_end = a + in.size;
            prev_falls = in.falls_through();
        }
        for (auto it = labels.begin(); it != labels.end();)
            it = instrs.count(*it) ? std::next(it) : labels.erase(it);
        // Code shared with a lower-addressed function (mwcc's 64-bit division helpers) puts the body
        // before the entry; the emitter then starts with a jump to the entry label.
        if (!instrs.empty() && instrs.begin()->first != entry) labels.insert(entry);

        Function& f = out.functions[fi];
        f.instrs = std::move(instrs);
        f.labels = std::move(labels);
        f.warnings = std::move(warnings);
    }
};

}  // namespace

Analysis analyze(const Program& program) {
    Analysis out;
    Builder b{program, out, {}};

    // Seeds first so every known entry is registered before bodies are explored.
    for (const FunctionSeed& s : program.config.functions) {
        size_t i = b.add_function(s.module, s.addr, s.thumb, s.name);
        if (i == SIZE_MAX) continue;
        out.functions[i].encrypted = s.dsprot;
        out.functions[i].extent = s.size;
    }
    for (const Hook& h : program.config.hooks) {
        auto it = out.index.find({h.module, h.addr});
        if (it == out.index.end()) {
            out.warnings.push_back("hook target is not a known function: " + hex(h.addr));
            continue;
        }
        out.functions[it->second].hook = h.replacement;
    }
    size_t seeds = out.functions.size();

    auto drain = [&] {
        while (!b.queue.empty()) {
            size_t fi = b.queue.front();
            b.queue.pop_front();
            b.explore(fi);
        }
    };
    drain();
    // Functions explored early did not know entries discovered later; redo them so their bodies
    // end at every known entry (tail call) instead of duplicating the following function.
    size_t known = out.functions.size();
    for (size_t i = 0; i < known; i++) b.explore(i);
    drain();
    out.discovered = out.functions.size() - seeds;
    return out;
}

}  // namespace recomp
