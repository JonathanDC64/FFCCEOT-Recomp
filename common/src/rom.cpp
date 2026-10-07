#include "ntr/rom.hpp"

#include <cstring>
#include <fstream>

namespace ntr {

namespace {
constexpr uint32_t kNitroCode0 = 0xDEC00621;  // bytes 21 06 C0 DE
constexpr uint32_t kNitroCode1 = 0x2106C0DE;  // bytes DE C0 06 21
constexpr uint32_t kMaxRomSize = 512u * 1024 * 1024;

std::string fixed_string(const uint8_t* p, size_t n) {
    std::string s(reinterpret_cast<const char*>(p), n);
    s.erase(s.find_last_not_of('\0') + 1);
    return s;
}

uint32_t rd32(std::span<const uint8_t> b, size_t off) {
    uint32_t v;
    std::memcpy(&v, b.data() + off, 4);
    return v;
}
}  // namespace

Rom Rom::open(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) throw RomError("cannot open " + path.string());
    auto size = static_cast<uint64_t>(f.tellg());
    if (size < 0x200 || size > kMaxRomSize) throw RomError("file size is not plausible for a DS ROM");
    std::vector<uint8_t> bytes(size);
    f.seekg(0);
    f.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
    if (!f) throw RomError("failed to read " + path.string());
    return from_bytes(std::move(bytes));
}

Rom Rom::from_bytes(std::vector<uint8_t> bytes) {
    Rom rom;
    rom.data_ = std::move(bytes);
    rom.parse();
    return rom;
}

uint32_t Rom::u32(uint32_t off) const {
    if (uint64_t(off) + 4 > data_.size()) throw RomError("read past end of ROM");
    return rd32(data_, off);
}

uint16_t Rom::u16(uint32_t off) const {
    if (uint64_t(off) + 2 > data_.size()) throw RomError("read past end of ROM");
    return uint16_t(data_[off] | (data_[off + 1] << 8));
}

std::span<const uint8_t> Rom::slice(uint32_t off, uint32_t size) const {
    if (uint64_t(off) + size > data_.size()) throw RomError("region past end of ROM");
    return std::span<const uint8_t>(data_).subspan(off, size);
}

void Rom::parse() {
    if (data_.size() < 0x200) throw RomError("not a DS ROM (too small)");
    const uint8_t* h = data_.data();
    if (u16(0x15C) != 0xCF56) throw RomError("not a DS ROM (Nintendo logo checksum missing)");

    header_.title = fixed_string(h, 12);
    header_.game_code = fixed_string(h + 0x0C, 4);
    header_.maker_code = fixed_string(h + 0x10, 2);
    header_.unit_code = h[0x12];
    header_.device_capacity = h[0x14];
    header_.rom_version = h[0x1E];
    header_.arm9_rom_offset = u32(0x20);
    header_.arm9_entry = u32(0x24);
    header_.arm9_ram_addr = u32(0x28);
    header_.arm9_size = u32(0x2C);
    header_.arm7_rom_offset = u32(0x30);
    header_.arm7_entry = u32(0x34);
    header_.arm7_ram_addr = u32(0x38);
    header_.arm7_size = u32(0x3C);
    header_.fnt_offset = u32(0x40);
    header_.fnt_size = u32(0x44);
    header_.fat_offset = u32(0x48);
    header_.fat_size = u32(0x4C);
    header_.ovt9_offset = u32(0x50);
    header_.ovt9_size = u32(0x54);
    header_.ovt7_offset = u32(0x58);
    header_.ovt7_size = u32(0x5C);
    header_.banner_offset = u32(0x68);
    header_.rom_used_size = u32(0x80);
    header_.header_crc = u16(0x15E);

    for (uint32_t i = 0; i < header_.ovt9_size / 32; i++) {
        uint32_t o = header_.ovt9_offset + i * 32;
        OverlayEntry e;
        e.id = u32(o);
        e.ram_addr = u32(o + 4);
        e.ram_size = u32(o + 8);
        e.bss_size = u32(o + 12);
        e.sinit_start = u32(o + 16);
        e.sinit_end = u32(o + 20);
        e.file_id = u32(o + 24);
        e.flags = u32(o + 28);
        overlays9_.push_back(e);
    }

    if (header_.fnt_size) parse_fnt(0xF000, "", 0);
}

void Rom::parse_fnt(uint32_t dir_id, const std::string& prefix, int depth) {
    if (depth > 32) throw RomError("NitroFS: directory nesting too deep");
    uint32_t base = header_.fnt_offset + (dir_id & 0xFFF) * 8;
    uint32_t pos = header_.fnt_offset + u32(base);
    uint16_t file_id = u16(base + 4);
    const uint32_t fnt_end = header_.fnt_offset + header_.fnt_size;

    while (pos < fnt_end) {
        uint8_t type = data_[pos++];
        if (type == 0) break;
        uint32_t len = type & 0x7F;
        std::string name(reinterpret_cast<const char*>(slice(pos, len).data()), len);
        pos += len;
        if (type & 0x80) {
            uint16_t sub = u16(pos);
            pos += 2;
            parse_fnt(sub, prefix + name + "/", depth + 1);
        } else {
            uint32_t start = u32(header_.fat_offset + file_id * 8);
            uint32_t end = u32(header_.fat_offset + file_id * 8 + 4);
            files_.push_back({prefix + name, file_id, start, end - start});
            file_id++;
        }
    }
}

Sha1Digest Rom::sha1() const { return ntr::sha1(data_); }

std::span<const uint8_t> Rom::fat_file(uint32_t id) const {
    if (id >= fat_count()) throw RomError("FAT id out of range");
    uint32_t start = u32(header_.fat_offset + id * 8);
    uint32_t end = u32(header_.fat_offset + id * 8 + 4);
    if (end < start) throw RomError("FAT entry has negative size");
    return slice(start, end - start);
}

std::optional<NitroFile> Rom::find_file(std::string_view path) const {
    for (const NitroFile& f : files_)
        if (f.path == path) return f;
    return std::nullopt;
}

Arm9Image Rom::load_arm9() const {
    Arm9Image img;
    img.ram_addr = header_.arm9_ram_addr;
    img.entry = header_.arm9_entry;
    auto raw = slice(header_.arm9_rom_offset, header_.arm9_size);

    // Locate the module params by the "nitrocode" pair that follows them.
    const size_t search_end = std::min<size_t>(raw.size(), 0x4000);
    size_t found = 0;
    for (size_t off = 0x1C; off + 8 <= search_end; off += 4) {
        if (rd32(raw, off) == kNitroCode0 && rd32(raw, off + 4) == kNitroCode1) {
            found = off - 0x1C;
            break;
        }
    }
    if (!found) throw RomError("ARM9 module params not found");
    Arm9ModuleParams& p = img.params;
    p.offset = uint32_t(found);
    p.autoload_list = rd32(raw, found + 0x00);
    p.autoload_list_end = rd32(raw, found + 0x04);
    p.autoload_start = rd32(raw, found + 0x08);
    p.bss_start = rd32(raw, found + 0x0C);
    p.bss_end = rd32(raw, found + 0x10);
    p.compressed_static_end = rd32(raw, found + 0x14);
    p.sdk_version = rd32(raw, found + 0x18);

    if (p.compressed_static_end) {
        uint32_t clen = p.compressed_static_end - img.ram_addr;
        if (clen > raw.size()) throw RomError("ARM9 compressed size exceeds binary");
        img.static_image = blz_decompress(raw.first(clen));
        img.static_image.insert(img.static_image.end(), raw.begin() + clen, raw.end());
    } else {
        img.static_image.assign(raw.begin(), raw.end());
    }

    auto in_image = [&](uint32_t addr, uint32_t size) {
        return addr >= img.ram_addr && uint64_t(addr - img.ram_addr) + size <= img.static_image.size();
    };
    if (!in_image(p.autoload_list, p.autoload_list_end - p.autoload_list))
        throw RomError("ARM9 autoload list outside image");

    uint32_t data_addr = p.autoload_start;
    for (uint32_t e = p.autoload_list; e < p.autoload_list_end; e += 12) {
        uint32_t off = e - img.ram_addr;
        Autoload a;
        a.run_addr = rd32(img.static_image, off);
        a.size = rd32(img.static_image, off + 4);
        a.bss_size = rd32(img.static_image, off + 8);
        if (!in_image(data_addr, a.size)) throw RomError("ARM9 autoload data outside image");
        auto src = img.static_image.begin() + (data_addr - img.ram_addr);
        a.data.assign(src, src + a.size);
        data_addr += a.size;
        img.autoloads.push_back(std::move(a));
    }
    return img;
}

Overlay Rom::load_overlay9(uint32_t index) const {
    if (index >= overlays9_.size()) throw RomError("overlay index out of range");
    Overlay ov;
    ov.entry = overlays9_[index];
    auto raw = fat_file(ov.entry.file_id);
    if (ov.entry.compressed()) {
        uint32_t clen = ov.entry.flags & 0xFFFFFF;
        if (clen > raw.size()) throw RomError("overlay compressed size exceeds file");
        ov.data = blz_decompress(raw.first(clen));
    } else {
        ov.data.assign(raw.begin(), raw.end());
    }
    if (ov.data.size() < ov.entry.ram_size) throw RomError("overlay smaller than its declared size");
    ov.data.resize(ov.entry.ram_size);
    return ov;
}

std::span<const uint8_t> Rom::arm7_binary() const {
    return slice(header_.arm7_rom_offset, header_.arm7_size);
}

}  // namespace ntr
