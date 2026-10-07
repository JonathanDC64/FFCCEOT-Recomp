#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include "ntr/rom.hpp"
#include "recomp/recomp.hpp"

namespace recomp {

uint32_t Module::read32(uint32_t addr) const {
    if (!contains(addr, 4)) throw std::out_of_range("read32 outside module " + name);
    uint32_t v;
    std::memcpy(&v, data.data() + (addr - base), 4);
    return v;
}

uint16_t Module::read16(uint32_t addr) const {
    if (!contains(addr, 2)) throw std::out_of_range("read16 outside module " + name);
    uint16_t v;
    std::memcpy(&v, data.data() + (addr - base), 2);
    return v;
}

const Module* Program::find_module(int id) const {
    for (const Module& m : modules)
        if (m.id == id) return &m;
    return nullptr;
}

const Module* Program::module_by_name(const std::string& name) const {
    for (const Module& m : modules)
        if (m.name == name) return &m;
    return nullptr;
}

Program load_program(const ntr::Rom& rom) {
    Program p;
    ntr::Arm9Image arm9 = rom.load_arm9();

    Module main;
    main.id = kModMain;
    main.name = "main";
    main.base = arm9.ram_addr;
    uint32_t static_size = arm9.params.autoload_start - arm9.ram_addr;
    main.data.assign(arm9.static_image.begin(), arm9.static_image.begin() + static_size);
    p.modules.push_back(std::move(main));

    for (const ntr::Autoload& a : arm9.autoloads) {
        Module m;
        // ITCM lives below main RAM; DTCM is placed by the game (0x027E0000 here).
        bool itcm = a.run_addr < 0x02000000;
        m.id = itcm ? kModItcm : kModDtcm;
        m.name = itcm ? "itcm" : "dtcm";
        m.base = a.run_addr;
        m.data = a.data;
        p.modules.push_back(std::move(m));
    }

    for (uint32_t i = 0; i < rom.overlays9().size(); i++) {
        ntr::Overlay ov = rom.load_overlay9(i);
        Module m;
        m.id = kModOverlayBase + int(ov.entry.id);
        char name[16];
        std::snprintf(name, sizeof name, "ov%03u", ov.entry.id);
        m.name = name;
        m.base = ov.entry.ram_addr;
        m.data = std::move(ov.data);
        p.modules.push_back(std::move(m));
    }
    return p;
}

static std::string trim(std::string s) {
    auto hash = s.find('#');
    if (hash != std::string::npos) s.erase(hash);
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

void load_regions(Program& program, const std::filesystem::path& dir) {
    std::ifstream f(dir / "regions.txt");
    std::string line;
    int lineno = 0, n = 0;
    while (f && std::getline(f, line)) {
        lineno++;
        line = trim(line);
        if (line.empty()) continue;
        std::istringstream ss(line);
        std::string name, src, addr, size, run, flag;
        ss >> name >> src >> addr >> size >> run;
        const Module* parent = program.module_by_name(src);
        if (!parent || run.empty())
            throw std::runtime_error("regions.txt:" + std::to_string(lineno) + ": expected '<name> <module> <addr> <size> <run_addr>'");
        Module m;
        m.id = kModRegionBase + n++;
        m.name = name;
        m.parent = parent->id;
        while (ss >> flag) {
            if (flag != "interpret") throw std::runtime_error("regions.txt:" + std::to_string(lineno) + ": unknown flag " + flag);
            m.interpret = true;
        }
        m.base = uint32_t(std::stoul(run, nullptr, 16));
        const uint32_t a = uint32_t(std::stoul(addr, nullptr, 16)), sz = uint32_t(std::stoul(size, nullptr, 16));
        if (!parent->contains(a, sz)) throw std::runtime_error("regions.txt:" + std::to_string(lineno) + ": source outside " + src);
        m.data.assign(parent->data.begin() + (a - parent->base), parent->data.begin() + (a - parent->base + sz));
        program.modules.push_back(std::move(m));
    }
}

Config load_config(const Program& program, const std::filesystem::path& dir) {
    Config cfg;

    std::ifstream f(dir / "functions.txt");
    if (!f) throw std::runtime_error("cannot open " + (dir / "functions.txt").string());
    std::string line;
    const Module* module = nullptr;
    int lineno = 0;
    while (std::getline(f, line)) {
        lineno++;
        line = trim(line);
        if (line.empty()) continue;
        if (line.front() == '[') {
            std::string name = line.substr(1, line.find(']') - 1);
            module = program.module_by_name(name);
            if (!module) throw std::runtime_error("functions.txt:" + std::to_string(lineno) + ": unknown module " + name);
            continue;
        }
        if (!module) throw std::runtime_error("functions.txt:" + std::to_string(lineno) + ": entry before module header");
        std::istringstream ss(line);
        std::string addr, size, mode, name, flag;
        ss >> addr >> size >> mode;
        FunctionSeed s;
        s.module = module->id;
        s.addr = uint32_t(std::stoul(addr, nullptr, 16));
        s.size = uint32_t(std::stoul(size, nullptr, 16));
        s.thumb = mode == "t";
        if (ss >> name && name != "-" && name[0] != '!') s.name = name;
        if (name == "!dsprot") s.dsprot = true;
        while (ss >> flag)
            if (flag == "!dsprot") s.dsprot = true;
        cfg.functions.push_back(std::move(s));
    }

    std::ifstream h(dir / "hooks.txt");
    lineno = 0;
    while (h && std::getline(h, line)) {
        lineno++;
        line = trim(line);
        if (line.empty()) continue;
        std::istringstream ss(line);
        std::string mod, addr, kind, repl;
        ss >> mod >> addr >> kind >> repl;
        const Module* m = program.module_by_name(mod);
        if (!m || kind != "replace" || repl.empty())
            throw std::runtime_error("hooks.txt:" + std::to_string(lineno) + ": expected '<module> <addr> replace <c_function>'");
        cfg.hooks.push_back({m->id, uint32_t(std::stoul(addr, nullptr, 16)), repl});
    }
    return cfg;
}

}  // namespace recomp
