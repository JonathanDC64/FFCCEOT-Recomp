#include "setup.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>

#include "ntr/recomp.h"
#include "ntr/rom.hpp"
#include "process.hpp"
#include "recomp/recomp.hpp"
#include "ui_assets.hpp"

namespace fs = std::filesystem;

namespace setup {

namespace {

#ifdef _WIN32
constexpr const char* kModuleName = "cfie_game.dll";
constexpr const char* kClangName = "clang.exe";
constexpr const char* kLinkerName = "lld-link.exe";
#else
constexpr const char* kModuleName = "libcfie_game.so";
constexpr const char* kClangName = "clang";
constexpr const char* kLinkerName = "ld.lld";
#endif

constexpr int kManifestFormat = 1;
constexpr uint64_t kMinFreeBytes = 700ull << 20;  // the ROM copy, the generated C and the objects, with room to spare

// The files embedded from the source tree (game/CMakeLists.txt): the game's configuration and the module's header.
const char* const kConfigFiles[] = {"functions.txt", "hooks.txt", "regions.txt"};
std::string config_asset(const char* name) { return std::string("../config/cfie/") + name; }
constexpr const char* kHeaderAsset = "../../runtime/include/ntr/recomp.h";

std::vector<uint8_t> read_all(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), {});
}

bool write_all(const fs::path& p, const void* data, size_t size) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(static_cast<const char*>(data), std::streamsize(size));
    return bool(f);
}

// What a built game depends on besides the ROM: this program's ABI, the recompiler's revision, the game's
// configuration and the module's header. Any change makes the cache stale, so no revision has to be remembered for
// those; only a change inside the recompiler itself (kRecompilerRevision) does.
std::string generator_id() {
    std::vector<uint8_t> bytes;
    const std::string head = "abi " + std::to_string(NTR_ABI_VERSION) + " rev " + std::to_string(kRecompilerRevision) + "\n";
    bytes.insert(bytes.end(), head.begin(), head.end());
    auto add = [&](const char* path) {
        if (const ui::Asset* a = ui::find_asset(path)) bytes.insert(bytes.end(), a->data, a->data + a->size);
    };
    for (const char* f : kConfigFiles) add(config_asset(f).c_str());
    add(kHeaderAsset);
    return ntr::to_hex(ntr::sha1(bytes)).substr(0, 16);
}

using Manifest = std::map<std::string, std::string>;

Manifest read_manifest(const fs::path& p) {
    Manifest m;
    std::ifstream f(p);
    std::string line;
    while (std::getline(f, line)) {
        const size_t eq = line.find('=');
        if (eq == std::string::npos || line[0] == '#') continue;
        auto trim = [](std::string s) {
            while (!s.empty() && (s.back() == ' ' || s.back() == '\r')) s.pop_back();
            size_t i = 0;
            while (i < s.size() && s[i] == ' ') i++;
            return s.substr(i);
        };
        m[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
    }
    return m;
}

uint64_t to_u64(const std::string& s) { return s.empty() ? 0 : std::strtoull(s.c_str(), nullptr, 10); }

std::string lower_ext(const fs::path& p) {
    std::string e = p.extension().string();
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return e;
}

// The header fields of a DS ROM that tell what it is.
struct Header {
    std::string title, code;
    bool plausible = false;
};
Header parse_header(const uint8_t* b, size_t n) {
    Header h;
    if (n < 0x200) return h;
    auto text = [&](size_t off, size_t len) {
        std::string s(reinterpret_cast<const char*>(b + off), len);
        while (!s.empty() && (s.back() == '\0' || s.back() == ' ')) s.pop_back();
        return s;
    };
    h.title = text(0, 12);
    h.code = text(0x0C, 4);
    bool printable = h.code.size() == 4;
    for (char c : h.code) printable = printable && c >= 0x20 && c < 0x7F;
    for (char c : h.title) printable = printable && c >= 0x20 && c < 0x7F;
    // The header's unit code and the fixed byte at 0x15C-0x15D (the logo CRC) separate a ROM from other files.
    h.plausible = printable && !h.code.empty() && b[0x15C] == 0x56 && b[0x15D] == 0xCF;
    return h;
}

// Serialises the callbacks, which the compile workers call.
class Reporter {
public:
    explicit Reporter(const Callbacks& cb) : cb_(cb) {}
    void progress(float f, const std::string& step) {
        std::lock_guard<std::mutex> lock(m_);
        if (cb_.progress) cb_.progress(std::clamp(f, 0.0f, 1.0f), step);
    }
    void log(const std::string& line) {
        std::lock_guard<std::mutex> lock(m_);
        std::fprintf(stderr, "setup: %s\n", line.c_str());
        if (cb_.log) cb_.log(line);
    }
    bool cancelled() const { return cb_.cancel && cb_.cancel->load(); }
    const std::atomic<bool>* cancel_flag() const { return cb_.cancel; }

private:
    const Callbacks& cb_;
    std::mutex m_;
};

// "free space" in bytes at `p` (or the nearest folder above that exists).
uint64_t free_space(fs::path p) {
    std::error_code ec;
    while (!p.empty() && !fs::exists(p, ec)) p = p.parent_path();
    const auto info = fs::space(p, ec);
    return ec ? ~0ull : uint64_t(info.available);
}

std::string tail(const std::string& s, size_t n) { return s.size() <= n ? s : "..." + s.substr(s.size() - n); }

// Builds the module from the generated sources in `gen` with the bundled compiler. Returns an error text, or empty.
std::string build_module(const ToolchainPaths& tc, const fs::path& gen, const fs::path& include, const fs::path& obj_dir,
                         const fs::path& out_module, unsigned jobs, Reporter& rep, float from, float to) {
    std::vector<fs::path> sources;
    for (const auto& e : fs::directory_iterator(gen))
        if (e.path().extension() == ".c") sources.push_back(e.path());
    std::sort(sources.begin(), sources.end());
    if (sources.empty()) return "the recompiler produced no source files";
    fs::create_directories(obj_dir);

    std::vector<std::string> common = {"-c", "-O1", "-w", "-ffreestanding", "-fno-stack-protector", "-nostdlibinc",
                                       "-DNTR_GAME_MODULE=1", "-I" + gen.string(), "-I" + include.string()};
#ifdef _WIN32
    common.insert(common.begin(), "--target=x86_64-pc-windows-msvc");
#else
    common.insert(common.begin(), "--target=x86_64-unknown-linux-gnu");
    common.push_back("-fPIC");
    common.push_back("-fvisibility=hidden");
#endif

    std::atomic<size_t> next{0}, done{0};
    std::atomic<bool> failed{false};
    std::mutex err_mutex;
    std::string error;
    std::vector<fs::path> objects(sources.size());
    const unsigned n = std::max(1u, std::min(jobs ? jobs : std::thread::hardware_concurrency(), unsigned(sources.size())));
    std::vector<std::thread> workers;
    const std::atomic<bool>* cancel = rep.cancel_flag();
    for (unsigned w = 0; w < n; w++)
        workers.emplace_back([&] {
            for (;;) {
                const size_t i = next++;
                if (i >= sources.size() || failed.load() || rep.cancelled()) return;
                objects[i] = obj_dir / (sources[i].filename().string() + ".o");
                std::vector<std::string> args = common;
                args.push_back(sources[i].string());
                args.push_back("-o");
                args.push_back(objects[i].string());
                const proc::Result r = proc::run(tc.clang, args, cancel);
                if (!r.started || r.exit_code != 0) {
                    std::lock_guard<std::mutex> lock(err_mutex);
                    if (!failed.exchange(true)) error = "the compiler failed on " + sources[i].filename().string() + ":\n" + tail(r.output, 1500);
                    return;
                }
                const size_t d = ++done;
                rep.progress(from + (to - from) * 0.95f * float(d) / float(sources.size()),
                             "Building the game (" + std::to_string(d) + " of " + std::to_string(sources.size()) + ")");
            }
        });
    for (std::thread& t : workers) t.join();
    if (failed) return error;
    if (rep.cancelled()) return "cancelled";

    rep.progress(from + (to - from) * 0.96f, "Linking the game");
    std::vector<std::string> link;
#ifdef _WIN32
    link = {"/dll", "/noentry", "/nodefaultlib", "/noimplib", "/out:" + out_module.string()};
#else
    link = {"-shared", "-o", out_module.string()};
#endif
    for (const fs::path& o : objects) link.push_back(o.string());
    const proc::Result r = proc::run(tc.linker, link);
    if (!r.started || r.exit_code != 0) return "the linker failed:\n" + tail(r.output, 1500);
    return {};
}

}  // namespace

fs::path game_dir(const fs::path& data_dir) { return data_dir / "game"; }

ToolchainPaths find_toolchain(const fs::path& override_dir) {
    ToolchainPaths tc;
    std::vector<fs::path> dirs;
    if (!override_dir.empty()) dirs.push_back(override_dir);
    if (const char* base = SDL_GetBasePath()) dirs.push_back(fs::path(base) / "toolchain");
    std::error_code ec;
    for (const fs::path& d : dirs) {
        const fs::path clang = d / "bin" / kClangName, linker = d / "bin" / kLinkerName;
        if (fs::exists(clang, ec) && fs::exists(linker, ec)) {
            tc.clang = clang;
            tc.linker = linker;
            return tc;
        }
    }
    if (override_dir.empty()) {
        // Development: an installed LLVM.
        std::vector<fs::path> bins;
        if (const char* path = std::getenv("PATH")) {
            std::stringstream ss(path);
            std::string part;
#ifdef _WIN32
            const char sep = ';';
#else
            const char sep = ':';
#endif
            while (std::getline(ss, part, sep)) bins.push_back(part);
        }
#ifdef _WIN32
        bins.push_back("C:/Program Files/LLVM/bin");
#endif
        for (const fs::path& b : bins) {
            if (fs::exists(b / kClangName, ec) && fs::exists(b / kLinkerName, ec)) {
                tc.clang = b / kClangName;
                tc.linker = b / kLinkerName;
                return tc;
            }
        }
    }
    tc.error = "The compiler that comes with the game was not found (a \"toolchain\" folder next to the program).";
    return tc;
}

ExpectedRom expected_rom() {
    for (const ntr::KnownRom& k : ntr::known_roms())
        if (k.supported) return {k.description, k.sha1_hex, k.crc32_hex};
    return {};
}

RomCheck verify_rom(const fs::path& path) {
    RomCheck r;
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) {
        r.message = "That file cannot be found.";
        return r;
    }
    r.size = fs::file_size(path, ec);
    const std::string ext = lower_ext(path);
    if (ext == ".zip" || ext == ".7z" || ext == ".rar" || ext == ".gz" || ext == ".xz") {
        r.message = "That is an archive. Extract the .nds file from it first.";
        return r;
    }
    const std::vector<uint8_t> bytes = read_all(path);
    if (bytes.empty()) {
        r.message = "That file cannot be read.";
        return r;
    }
    if (bytes.size() >= 4 && ((bytes[0] == 'P' && bytes[1] == 'K') || (bytes[0] == '7' && bytes[1] == 'z') ||
                              (bytes[0] == 'R' && bytes[1] == 'a' && bytes[2] == 'r'))) {
        r.message = "That is an archive. Extract the .nds file from it first.";
        return r;
    }
    const Header h = parse_header(bytes.data(), bytes.size());
    if (!h.plausible) {
        r.message = "That is not a Nintendo DS game dump (.nds).";
        return r;
    }
    const ntr::Sha1Digest digest = ntr::sha1(bytes);
    r.sha1 = ntr::to_hex(digest);
    r.crc32 = ntr::crc32_hex(ntr::crc32(bytes));
    if (const ntr::KnownRom* known = ntr::find_known_rom(digest)) {
        if (known->supported) {
            r.ok = true;
            r.message = std::string(known->description);
            return r;
        }
        r.message = std::string("That is ") + known->description + ", which is not supported yet. Only the USA release works for now.";
        return r;
    }
    if (h.code == "CFIE") {
        r.message = "This is the USA release of Echoes of Time, but it is not the original dump: it has been modified, trimmed or badly dumped. "
                    "Dump your cartridge again, or use an untouched copy.";
        return r;
    }
    if (h.code.rfind("CFI", 0) == 0) {
        r.message = "That looks like Echoes of Time (" + h.code + "), but not the USA release this version supports (CFIE). "
                    "Its SHA-1 is " + r.sha1 + ".";
        return r;
    }
    r.message = "That is " + (h.title.empty() ? std::string("another game") : h.title) + " (" + h.code + "), not Echoes of Time.";
    return r;
}

std::vector<fs::path> find_roms() {
    std::vector<fs::path> folders;
    for (SDL_Folder f : {SDL_FOLDER_DOWNLOADS, SDL_FOLDER_DOCUMENTS, SDL_FOLDER_DESKTOP}) {
        if (const char* p = SDL_GetUserFolder(f)) folders.push_back(p);
    }
    if (const char* home = SDL_GetUserFolder(SDL_FOLDER_HOME)) {
        for (const char* sub : {"ROMs", "roms", "Games", "games", "Emulation/roms/nds"}) folders.push_back(fs::path(home) / sub);
    }
    std::vector<fs::path> found;
    size_t scanned = 0;
    std::error_code ec;
    auto scan = [&](const fs::path& dir, auto&& self, int depth) -> void {
        for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
            if (++scanned > 4000) return;
            if (it->is_directory(ec)) {
                if (depth < 2) self(it->path(), self, depth + 1);
            } else if (lower_ext(it->path()) == ".nds") {
                std::ifstream f(it->path(), std::ios::binary);
                uint8_t head[0x200] = {};
                if (!f.read(reinterpret_cast<char*>(head), sizeof head)) continue;
                const Header h = parse_header(head, sizeof head);
                if (h.plausible && h.code == "CFIE") found.push_back(it->path());
            }
        }
    };
    for (const fs::path& f : folders)
        if (fs::is_directory(f, ec)) scan(f, scan, 0);
    std::sort(found.begin(), found.end());
    found.erase(std::unique(found.begin(), found.end()), found.end());
    return found;
}

Status check(const fs::path& data_dir) {
    Status s;
    const fs::path dir = game_dir(data_dir);
    std::error_code ec;
    s.rom = dir / "cfie.nds";
    s.have_rom_copy = fs::is_regular_file(s.rom, ec);
    const Manifest m = read_manifest(dir / "manifest.ini");
    if (m.empty()) {
        s.state = State::Missing;
        s.reason = "The game has not been set up yet.";
        return s;
    }
    auto get = [&](const char* k) {
        auto it = m.find(k);
        return it == m.end() ? std::string() : it->second;
    };
    if (to_u64(get("format")) != kManifestFormat || to_u64(get("abi")) != NTR_ABI_VERSION || get("generator") != generator_id()) {
        s.state = State::Outdated;
        s.reason = "This version changes how the game is built from your ROM, so it has to be set up again.";
        return s;
    }
    s.module = dir / get("module");
    if (get("module").empty() || !fs::is_regular_file(s.module, ec) || !s.have_rom_copy ||
        fs::file_size(s.rom, ec) != to_u64(get("rom_size"))) {
        s.state = State::Damaged;
        s.reason = "Some of the game's files are missing, so it has to be set up again.";
        return s;
    }
    s.state = State::Ready;
    return s;
}

Result run(const Request& req, const Callbacks& callbacks) {
    Result res;
    Reporter rep(callbacks);
    const fs::path dir = game_dir(req.data_dir);
    std::error_code ec;
    auto fail = [&](const std::string& why) {
        // Leave nothing half-built behind (the ROM copy stays: it is what a retry builds from).
        fs::remove_all(dir / "work", ec);
        for (const auto& e : fs::directory_iterator(dir, ec))
            if (e.is_directory(ec) && e.path().filename().string().rfind("build-", 0) == 0 && !fs::exists(e.path() / kModuleName, ec))
                fs::remove_all(e.path(), ec);
        res.ok = false;
        res.cancelled = rep.cancelled();
        res.error = res.cancelled ? "Cancelled." : why;
        return res;
    };
    try {
        fs::create_directories(dir, ec);
        if (free_space(dir) < kMinFreeBytes)
            return fail("There is not enough free disk space (about 700 MB are needed in " + dir.string() + ").");

        const ToolchainPaths tc = find_toolchain(req.toolchain_dir);
        if (!tc.ok()) return fail(tc.error);
        rep.log("compiler: " + tc.clang.string());

        // 1. The ROM.
        rep.progress(0.0f, "Checking your ROM");
        const fs::path rom_copy = dir / "cfie.nds";
        fs::path rom_path = rom_copy;
        std::string rom_sha1;
        if (!req.rom_source.empty()) {
            const RomCheck check = verify_rom(req.rom_source);
            rep.log("rom: " + check.message + (check.sha1.empty() ? "" : " (sha1 " + check.sha1 + ")"));
            if (!check.ok) return fail(check.message);
            rom_path = req.rom_source;
            rom_sha1 = check.sha1;
        } else {
            const RomCheck check = verify_rom(rom_copy);
            if (!check.ok) return fail("The copy of your ROM that was kept is no longer usable. Choose your ROM again. (" + check.message + ")");
            rom_sha1 = check.sha1;
        }
        if (rep.cancelled()) return fail("");

        // 2. Keep a copy, so the game keeps working when the original is moved and so a later update can rebuild.
        if (!req.rom_source.empty() && !(fs::exists(rom_copy, ec) && fs::equivalent(rom_copy, rom_path, ec))) {
            rep.progress(0.05f, "Copying your ROM");
            const fs::path tmp = dir / "cfie.nds.tmp";
            fs::copy_file(rom_path, tmp, fs::copy_options::overwrite_existing, ec);
            if (ec) return fail("Cannot copy the ROM into " + dir.string() + ": " + ec.message());
            fs::rename(tmp, rom_copy, ec);
            if (ec) {
                fs::remove(rom_copy, ec);
                fs::rename(tmp, rom_copy, ec);
                if (ec) return fail("Cannot save the ROM copy: " + ec.message());
            }
            rom_path = rom_copy;
        }
        if (rep.cancelled()) return fail("");

        // 3. Work area: the configuration and header the program carries, and the recompiler's output.
        const fs::path work = dir / "work";
        fs::remove_all(work, ec);
        const fs::path config = work / "config", gen = work / "gen", include = work / "include" / "ntr", obj = work / "obj";
        fs::create_directories(config);
        fs::create_directories(gen);
        fs::create_directories(include);
        for (const char* f : kConfigFiles) {
            const ui::Asset* a = ui::find_asset(config_asset(f));
            if (!a || !write_all(config / f, a->data, a->size)) return fail(std::string("This program lacks its own file ") + f + ".");
        }
        const ui::Asset* header = ui::find_asset(kHeaderAsset);
        if (!header || !write_all(include / "recomp.h", header->data, header->size)) return fail("This program lacks its own recomp.h.");

        // 4. Recompile: ARM code to C.
        rep.progress(0.10f, "Translating the game");
        {
            ntr::Rom rom = ntr::Rom::open(rom_path);
            recomp::Program program = recomp::load_program(rom);
            recomp::load_regions(program, config);
            program.config = recomp::load_config(program, config);
            if (rep.cancelled()) return fail("");
            rep.progress(0.14f, "Analysing the game");
            recomp::Analysis analysis = recomp::analyze(program);
            if (rep.cancelled()) return fail("");
            rep.progress(0.24f, "Writing the game's code");
            const recomp::EmitStats stats = recomp::emit_c(program, analysis, gen);
            rep.log("recompiled " + std::to_string(stats.functions) + " functions (" + std::to_string(stats.instructions) + " instructions)");
        }
        if (rep.cancelled()) return fail("");

        // 5. Build the module.
        const std::string key = "build-" + generator_id();
        const fs::path out_dir = dir / key;
        fs::create_directories(out_dir, ec);
        const fs::path module = out_dir / kModuleName;
        const std::string err = build_module(tc, gen, include.parent_path(), obj, module, req.jobs, rep, 0.28f, 0.98f);
        if (!err.empty()) return fail(err);

        // 6. Record it, and tidy up what an earlier build left.
        rep.progress(0.99f, "Finishing");
        {
            std::ostringstream m;
            m << "# Written by setup. Delete this file to run setup again.\n"
              << "format = " << kManifestFormat << "\nabi = " << NTR_ABI_VERSION << "\ngenerator = " << generator_id()
              << "\nrom_sha1 = " << rom_sha1 << "\nrom_size = " << fs::file_size(rom_copy, ec)
              << "\nmodule = " << key << "/" << kModuleName << "\n";
            const std::string text = m.str();
            if (!write_all(dir / "manifest.ini", text.data(), text.size())) return fail("Cannot write the manifest.");
        }
        fs::remove_all(work, ec);
        for (const auto& e : fs::directory_iterator(dir, ec))
            if (e.is_directory(ec) && e.path().filename().string().rfind("build-", 0) == 0 && e.path().filename() != key)
                fs::remove_all(e.path(), ec);
        res.ok = true;
        res.rom = rom_copy;
        res.module = module;
        rep.progress(1.0f, "Done");
        return res;
    } catch (const std::exception& e) {
        return fail(std::string("Setup failed: ") + e.what());
    }
}

}  // namespace setup
