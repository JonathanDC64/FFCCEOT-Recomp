// rominfo: print the layout of a DS ROM and whether it is a supported dump.
//
//   rominfo <rom.nds> [--files] [--overlays]
#include <cstdio>
#include <cstring>
#include <map>

#include "ntr/rom.hpp"

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: rominfo <rom.nds> [--files] [--overlays]\n");
        return 2;
    }
    bool show_files = false, show_overlays = false;
    for (int i = 2; i < argc; i++) {
        if (!std::strcmp(argv[i], "--files")) show_files = true;
        if (!std::strcmp(argv[i], "--overlays")) show_overlays = true;
    }

    try {
        ntr::Rom rom = ntr::Rom::open(argv[1]);
        const auto& h = rom.header();
        auto digest = rom.sha1();
        const ntr::KnownRom* known = ntr::find_known_rom(digest);

        std::printf("title      %s\n", h.title.c_str());
        std::printf("game code  %s (maker %s, rev %u)\n", h.game_code.c_str(), h.maker_code.c_str(), h.rom_version);
        std::printf("sha1       %s\n", ntr::to_hex(digest).c_str());
        std::printf("dump       %s\n", known ? (std::string(known->description) + (known->supported ? " [supported]" : " [recognised, unsupported]")).c_str()
                                             : "unknown (not a supported dump)");
        std::printf("arm9       rom 0x%08x size 0x%x -> ram 0x%08x entry 0x%08x\n", h.arm9_rom_offset, h.arm9_size, h.arm9_ram_addr, h.arm9_entry);
        std::printf("arm7       rom 0x%08x size 0x%x -> ram 0x%08x entry 0x%08x\n", h.arm7_rom_offset, h.arm7_size, h.arm7_ram_addr, h.arm7_entry);

        ntr::Arm9Image arm9 = rom.load_arm9();
        const auto& p = arm9.params;
        std::printf("arm9 img   0x%zx bytes decompressed (compressed_static_end 0x%08x), sdk 0x%08x\n",
                    arm9.static_image.size(), p.compressed_static_end, p.sdk_version);
        std::printf("arm9 bss   0x%08x-0x%08x\n", p.bss_start, p.bss_end);
        for (const auto& a : arm9.autoloads)
            std::printf("autoload   run 0x%08x size 0x%x bss 0x%x\n", a.run_addr, a.size, a.bss_size);

        std::map<uint32_t, int> regions;
        uint32_t total = 0;
        for (const auto& o : rom.overlays9()) {
            regions[o.ram_addr]++;
            total += o.ram_size;
        }
        std::printf("overlays9  %zu (0x%x bytes)\n", rom.overlays9().size(), total);
        for (auto [addr, n] : regions) std::printf("  region 0x%08x: %d overlay(s)\n", addr, n);
        if (show_overlays)
            for (const auto& o : rom.overlays9())
                std::printf("  ov%03u ram 0x%08x size 0x%06x bss 0x%05x sinit 0x%08x-0x%08x file %u%s\n", o.id, o.ram_addr, o.ram_size,
                            o.bss_size, o.sinit_start, o.sinit_end, o.file_id, o.compressed() ? " (compressed)" : "");

        std::printf("files      %zu named, %u FAT entries\n", rom.files().size(), rom.fat_count());
        if (show_files)
            for (const auto& f : rom.files()) std::printf("  [%4u] 0x%08x %10u  %s\n", f.id, f.offset, f.size, f.path.c_str());
        return known && known->supported ? 0 : 1;
    } catch (const ntr::RomError& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 3;
    }
}
