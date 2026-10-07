// ntr-recomp CLI: recompile the ARM9 code of a user-supplied ROM to C.
//
//   ntr-recomp --rom game.nds --config game/config/cfie --out build/recomp_generated [--report report.txt]
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <string>

#include "ntr/rom.hpp"
#include "recomp/recomp.hpp"

// Test mode: recompile a raw code blob (used by the differential tests).
// --raw-funcs lists one "<hex addr> <a|t>" entry per line.
static int run_raw(const std::string& blob, uint32_t base, const std::string& funcs, const std::string& out_dir) {
    std::ifstream b(blob, std::ios::binary);
    std::ifstream f(funcs);
    if (!b || !f || out_dir.empty()) {
        std::fprintf(stderr, "raw mode needs --raw <bin> --raw-base <hex> --raw-funcs <file> --out <dir>\n");
        return 2;
    }
    recomp::Program program;
    recomp::Module m;
    m.id = recomp::kModMain;
    m.name = "main";
    m.base = base;
    m.data.assign(std::istreambuf_iterator<char>(b), {});
    program.modules.push_back(std::move(m));
    std::string addr, mode;
    while (f >> addr >> mode) {
        recomp::FunctionSeed s;
        s.module = recomp::kModMain;
        s.addr = uint32_t(std::stoul(addr, nullptr, 16));
        s.thumb = mode == "t";
        program.config.functions.push_back(s);
    }
    recomp::Analysis analysis = recomp::analyze(program);
    recomp::EmitOptions opt;
    opt.shards = 1;
    recomp::EmitStats st = recomp::emit_c(program, analysis, out_dir, opt);
    std::printf("raw: %zu functions, %zu instructions\n", st.functions, st.instructions);
    return 0;
}

int main(int argc, char** argv) {
    std::string rom_path, config_dir, out_dir, report_path, raw_path, raw_funcs;
    uint32_t raw_base = 0;
    bool allow_unknown = false;
    for (int i = 1; i < argc; i++) {
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (!std::strcmp(argv[i], "--rom")) rom_path = next();
        else if (!std::strcmp(argv[i], "--config")) config_dir = next();
        else if (!std::strcmp(argv[i], "--out")) out_dir = next();
        else if (!std::strcmp(argv[i], "--report")) report_path = next();
        else if (!std::strcmp(argv[i], "--allow-unknown-rom")) allow_unknown = true;
        else if (!std::strcmp(argv[i], "--raw")) raw_path = next();
        else if (!std::strcmp(argv[i], "--raw-base")) raw_base = uint32_t(std::stoul(next(), nullptr, 16));
        else if (!std::strcmp(argv[i], "--raw-funcs")) raw_funcs = next();
    }
    if (!raw_path.empty()) return run_raw(raw_path, raw_base, raw_funcs, out_dir);
    if (rom_path.empty() || config_dir.empty() || out_dir.empty()) {
        std::fprintf(stderr, "usage: ntr-recomp --rom <rom.nds> --config <game/config/xxxx> --out <dir> [--report <file>]\n");
        return 2;
    }

    try {
        auto t0 = std::chrono::steady_clock::now();
        ntr::Rom rom = ntr::Rom::open(rom_path);
        const ntr::KnownRom* known = ntr::find_known_rom(rom.sha1());
        if (!known || !known->supported) {
            std::fprintf(stderr, "%s: not a supported dump (sha1 %s)\n", rom_path.c_str(), ntr::to_hex(rom.sha1()).c_str());
            if (!allow_unknown) return 1;
        }

        recomp::Program program = recomp::load_program(rom);
        program.config = recomp::load_config(program, config_dir);
        recomp::Analysis analysis = recomp::analyze(program);

        size_t warn_fns = 0, undefined = 0, tables = 0;
        for (const auto& f : analysis.functions) {
            if (!f.warnings.empty()) warn_fns++;
            for (const auto& [a, in] : f.instrs) {
                if (in.flow == recomp::Flow::Undefined) undefined++;
                if (in.flow == recomp::Flow::JumpTable) tables++;
            }
        }

        recomp::EmitStats stats = recomp::emit_c(program, analysis, out_dir);
        double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("functions: %zu (%zu seeded, %zu discovered), instructions: %zu, jump tables: %zu\n",
                    stats.functions, program.config.functions.size(), analysis.discovered, stats.instructions, tables);
        std::printf("functions with warnings: %zu, undefined instructions reached: %zu, global warnings: %zu\n", warn_fns,
                    undefined, analysis.warnings.size());
        std::printf("wrote %zu files to %s in %.2fs\n", stats.files, out_dir.c_str(), secs);

        if (!report_path.empty()) {
            std::ofstream r(report_path);
            for (const auto& w : analysis.warnings) r << "global: " << w << "\n";
            for (const auto& f : analysis.functions)
                for (const auto& w : f.warnings) r << f.c_name() << ": " << w << "\n";
        }
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 3;
    }
}
