// Nintendo DS ROM access: header, NitroFS, overlays, ARM9/ARM7 program images.
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace ntr {

class RomError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

using Sha1Digest = std::array<uint8_t, 20>;

Sha1Digest sha1(std::span<const uint8_t> data);
std::string to_hex(std::span<const uint8_t> bytes);

// Nintendo "backwards LZ" used for the ARM9 static module and (optionally) overlays.
// `in` is the complete compressed region; the footer is at its end.
std::vector<uint8_t> blz_decompress(std::span<const uint8_t> in);

struct RomHeader {
    std::string title;       // 12 chars, trailing NULs stripped
    std::string game_code;   // e.g. "CFIE"
    std::string maker_code;  // e.g. "GD"
    uint8_t unit_code = 0;
    uint8_t device_capacity = 0;
    uint8_t rom_version = 0;

    uint32_t arm9_rom_offset = 0, arm9_entry = 0, arm9_ram_addr = 0, arm9_size = 0;
    uint32_t arm7_rom_offset = 0, arm7_entry = 0, arm7_ram_addr = 0, arm7_size = 0;
    uint32_t fnt_offset = 0, fnt_size = 0, fat_offset = 0, fat_size = 0;
    uint32_t ovt9_offset = 0, ovt9_size = 0, ovt7_offset = 0, ovt7_size = 0;
    uint32_t banner_offset = 0;
    uint32_t rom_used_size = 0;
    uint16_t header_crc = 0;
};

struct OverlayEntry {
    uint32_t id = 0;
    uint32_t ram_addr = 0;
    uint32_t ram_size = 0;   // size of the loaded (decompressed) image
    uint32_t bss_size = 0;
    uint32_t sinit_start = 0;
    uint32_t sinit_end = 0;
    uint32_t file_id = 0;
    uint32_t flags = 0;      // bits 0-23: compressed size, bit 24: compressed, bit 25: verified

    bool compressed() const { return (flags & 0x01000000) != 0; }
};

struct NitroFile {
    std::string path;  // "dir/name.ext"
    uint16_t id = 0;
    uint32_t offset = 0;
    uint32_t size = 0;
};

// SDK module parameters found in the ARM9 static image (crt0 "_start_ModuleParams").
struct Arm9ModuleParams {
    uint32_t offset = 0;  // offset of the params struct within the ARM9 image
    uint32_t autoload_list = 0;
    uint32_t autoload_list_end = 0;
    uint32_t autoload_start = 0;
    uint32_t bss_start = 0;
    uint32_t bss_end = 0;
    uint32_t compressed_static_end = 0;  // 0 when the image is not compressed
    uint32_t sdk_version = 0;
};

// A block the ARM9 crt0 copies to its run address at boot (ITCM, DTCM, ...).
struct Autoload {
    uint32_t run_addr = 0;
    uint32_t size = 0;
    uint32_t bss_size = 0;
    std::vector<uint8_t> data;
};

struct Arm9Image {
    uint32_t ram_addr = 0;
    uint32_t entry = 0;
    Arm9ModuleParams params;
    std::vector<uint8_t> static_image;  // decompressed; [ram_addr, autoload_start) is the static module
    std::vector<Autoload> autoloads;
};

struct Overlay {
    OverlayEntry entry;
    std::vector<uint8_t> data;  // decompressed, ram_size bytes
};

class Rom {
public:
    static Rom open(const std::filesystem::path& path);
    static Rom from_bytes(std::vector<uint8_t> bytes);

    const RomHeader& header() const { return header_; }
    std::span<const uint8_t> bytes() const { return data_; }
    Sha1Digest sha1() const;

    const std::vector<OverlayEntry>& overlays9() const { return overlays9_; }
    const std::vector<NitroFile>& files() const { return files_; }
    uint32_t fat_count() const { return header_.fat_size / 8; }

    // Raw bytes of a FAT entry (file or overlay).
    std::span<const uint8_t> fat_file(uint32_t id) const;
    std::optional<NitroFile> find_file(std::string_view path) const;

    Arm9Image load_arm9() const;
    Overlay load_overlay9(uint32_t index) const;
    std::span<const uint8_t> arm7_binary() const;

private:
    void parse();
    void parse_fnt(uint32_t dir_id, const std::string& prefix, int depth);
    uint32_t u32(uint32_t off) const;
    uint16_t u16(uint32_t off) const;
    std::span<const uint8_t> slice(uint32_t off, uint32_t size) const;

    std::vector<uint8_t> data_;
    RomHeader header_;
    std::vector<OverlayEntry> overlays9_;
    std::vector<NitroFile> files_;
};

// ROM dumps this project knows how to set up.
struct KnownRom {
    const char* sha1_hex;
    const char* game_code;
    const char* region;
    const char* description;
    bool supported;
};

std::span<const KnownRom> known_roms();
const KnownRom* find_known_rom(const Sha1Digest& digest);

}  // namespace ntr
