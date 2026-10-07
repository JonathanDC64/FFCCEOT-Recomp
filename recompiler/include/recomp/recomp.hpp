// ntr-recomp: static recompiler from DS ARM9 (ARMv5TE, ARM + Thumb) code to C.
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace ntr {
class Rom;
}

namespace recomp {

constexpr int kModMain = 0;
constexpr int kModItcm = 1;
constexpr int kModDtcm = 2;
constexpr int kModArm7 = 3;
constexpr int kModOverlayBase = 100;

inline bool is_overlay(int module) { return module >= kModOverlayBase; }

// A contiguous block of guest code/data at a fixed run address.
struct Module {
    int id = 0;
    std::string name;  // "main", "itcm", "ov023", ...
    uint32_t base = 0;
    std::vector<uint8_t> data;

    uint32_t end() const { return base + uint32_t(data.size()); }
    bool contains(uint32_t addr, uint32_t size = 1) const {
        return addr >= base && uint64_t(addr) + size <= uint64_t(base) + data.size();
    }
    uint32_t read32(uint32_t addr) const;
    uint16_t read16(uint32_t addr) const;
};

struct FunctionSeed {
    int module = 0;
    uint32_t addr = 0;
    uint32_t size = 0;
    bool thumb = false;
    std::string name;
    bool dsprot = false;
};

struct Hook {
    int module = 0;
    uint32_t addr = 0;
    std::string replacement;  // C function name taking (NtrCpu*)
};

struct Config {
    std::vector<FunctionSeed> functions;
    std::vector<Hook> hooks;
};

struct Program {
    std::vector<Module> modules;  // indexed by position; use find_module() for ids
    Config config;

    const Module* find_module(int id) const;
    const Module* module_by_name(const std::string& name) const;
};

// Build the module list from a ROM: ARM9 static ("main"), autoloads ("itcm"/"dtcm") and all ARM9 overlays.
Program load_program(const ntr::Rom& rom);
// Load functions.txt and hooks.txt from a per-game config directory.
Config load_config(const Program& program, const std::filesystem::path& dir);

// ---- analysis ----

enum class Flow : uint8_t {
    Normal,        // continues to the next instruction
    Jump,          // direct branch (possibly conditional)
    Call,          // direct BL/BLX to `target`
    IndirectJump,  // write to PC from a register/memory
    IndirectCall,  // BLX reg
    JumpTable,     // computed jump with known targets in `table`
    Undefined,     // not decodable; execution must not reach here
};

struct Instr {
    uint32_t addr = 0;
    uint32_t op = 0;  // ARM word; Thumb halfword; Thumb BL pair = prefix | (suffix << 16)
    uint8_t size = 4;
    bool thumb = false;
    Flow flow = Flow::Normal;
    uint8_t cond = 0xE;  // ARM condition (0xE = always). Thumb conditional branches also use it.
    uint32_t target = 0;
    bool target_thumb = false;
    bool link = false;            // Jump that also sets LR (Thumb BL used as a long branch)
    std::vector<uint32_t> table;  // JumpTable targets

    bool falls_through() const {
        if (cond != 0xE) return true;
        return flow == Flow::Normal || flow == Flow::Call || flow == Flow::IndirectCall;
    }
};

// Classify one instruction (no module needed except for the Thumb BL pair suffix).
Instr decode(const Module& m, uint32_t addr, bool thumb);
// Registers an instruction may write (bit n = rN). Conservative: unknown forms return 0xFFFF.
uint32_t regs_written(const Instr& in);

struct Function {
    int module = 0;
    uint32_t entry = 0;
    bool thumb = false;
    uint32_t extent = 0;  // size from the config seed (0 = unknown); BL inside it is a long branch
    std::string name;  // optional symbolic name
    std::string hook;  // non-empty: body replaced by a call to this C function
    bool encrypted = false;
    std::map<uint32_t, Instr> instrs;
    std::set<uint32_t> labels;
    std::vector<std::string> warnings;

    std::string c_name() const;
};

struct Analysis {
    std::vector<Function> functions;
    std::map<std::pair<int, uint32_t>, size_t> index;  // (module, entry) -> functions[]
    std::vector<std::string> warnings;
    size_t discovered = 0;  // functions found beyond the config seeds

    const Function* find(int module, uint32_t entry) const;
};

Analysis analyze(const Program& program);

// Module that owns code at `addr` when called from `from_module`. Returns nullopt for addresses
// that need runtime resolution (overlay regions seen from outside, or unknown memory).
std::optional<int> static_target_module(const Program& program, int from_module, uint32_t addr);

// ---- C emission ----

struct EmitOptions {
    // Fixed shard count so build systems know the output file names (funcs_000.c .. funcs_NNN.c) in advance.
    size_t shards = 64;
};

struct EmitStats {
    size_t files = 0;
    size_t functions = 0;
    size_t instructions = 0;
};

EmitStats emit_c(const Program& program, const Analysis& analysis, const std::filesystem::path& out_dir,
                 const EmitOptions& options = {});

// Generated sources start with this marker; the copyright guard refuses to commit them.
inline constexpr const char* kGeneratedMarker = "@ntr-recomp-generated";

}  // namespace recomp
