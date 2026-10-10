#include "ntr/wii_disc.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <system_error>

#include "ntr/aes.hpp"

namespace ntr {

namespace {

constexpr std::uint64_t kCluster = 0x8000;  // a partition's encrypted cluster
constexpr std::uint64_t kHashes = 0x400;    // of which the first part is hashes
constexpr std::uint64_t kClusterData = kCluster - kHashes;

std::uint32_t be32(const std::uint8_t* p) {
    return std::uint32_t(p[0]) << 24 | std::uint32_t(p[1]) << 16 | std::uint32_t(p[2]) << 8 | p[3];
}

bool seek(std::FILE* f, std::uint64_t pos) {
#ifdef _WIN32
    return _fseeki64(f, static_cast<long long>(pos), SEEK_SET) == 0;
#else
    return fseeko(f, static_cast<off_t>(pos), SEEK_SET) == 0;
#endif
}

}  // namespace

bool parse_wii_key(const std::string& text, WiiKey& key, std::string& error) {
    std::string digits;
    std::size_t i = 0;
    while (i < text.size() && (text[i] == ' ' || text[i] == '\t' || text[i] == '\r' || text[i] == '\n')) i++;
    if (i + 1 < text.size() && text[i] == '0' && (text[i + 1] == 'x' || text[i + 1] == 'X')) i += 2;
    for (; i < text.size(); i++) {
        const char c = text[i];
        if (c == ' ' || c == '-' || c == ':' || c == '\t' || c == '\r' || c == '\n') continue;
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
            error = "The key has to be 32 hexadecimal digits (0 to 9 and A to F).";
            return false;
        }
        digits.push_back(c);
    }
    if (digits.size() != 32) {
        error = "The key has to be 16 bytes: 32 hexadecimal digits, and this has " + std::to_string(digits.size()) + ".";
        return false;
    }
    const auto nibble = [](char c) { return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10; };
    for (std::size_t k = 0; k < 16; k++) key[k] = std::uint8_t(nibble(digits[2 * k]) << 4 | nibble(digits[2 * k + 1]));
    return true;
}

struct WiiDisc::Impl {
    std::FILE* file = nullptr;
    bool wbfs = false;
    std::uint64_t sector = 0;  // WBFS: bytes per block
    std::vector<std::uint16_t> map;  // WBFS: where each block of the disc is in the file, in blocks (0: not stored)
    std::uint64_t data_start = 0;    // where the partition's clusters begin, in the disc
    std::unique_ptr<Aes128> title;
    std::vector<std::uint8_t> clear;  // the cluster that is decrypted
    std::int64_t clear_index = -1;

    ~Impl() {
        if (file) std::fclose(file);
    }

    bool raw(std::uint64_t pos, void* out, std::size_t size) {
        auto* dst = static_cast<std::uint8_t*>(out);
        while (size) {
            std::uint64_t at = pos;
            std::size_t n = size;
            if (wbfs) {
                const std::uint64_t block = pos / sector, within = pos % sector;
                n = std::size_t(std::min<std::uint64_t>(size, sector - within));
                if (block >= map.size() || map[block] == 0) {
                    std::memset(dst, 0, n);  // a block the dump left out reads as zeros
                    dst += n, pos += n, size -= n;
                    continue;
                }
                at = std::uint64_t(map[block]) * sector + within;
            }
            if (!seek(file, at) || std::fread(dst, 1, n, file) != n) return false;
            dst += n, pos += n, size -= n;
        }
        return true;
    }

    bool load_cluster(std::int64_t index) {
        if (index == clear_index) return true;
        std::uint8_t enc[kCluster];
        if (!raw(data_start + std::uint64_t(index) * kCluster, enc, sizeof enc)) return false;
        std::array<std::uint8_t, 16> iv;
        std::memcpy(iv.data(), enc + 0x3D0, 16);  // (the hash block holds the data's IV)
        clear.resize(kClusterData);
        title->cbc_decrypt(iv, enc + kHashes, clear.data(), kClusterData);
        clear_index = index;
        return true;
    }

    bool partition(std::uint64_t pos, void* out, std::size_t size) {
        auto* dst = static_cast<std::uint8_t*>(out);
        while (size) {
            const std::uint64_t index = pos / kClusterData, within = pos % kClusterData;
            const std::size_t n = std::size_t(std::min<std::uint64_t>(size, kClusterData - within));
            if (!load_cluster(std::int64_t(index))) return false;
            std::memcpy(dst, clear.data() + within, n);
            dst += n, pos += n, size -= n;
        }
        return true;
    }
};

WiiDisc::~WiiDisc() = default;

std::unique_ptr<WiiDisc> WiiDisc::open(const std::filesystem::path& path, const WiiKey& key, std::string& error) {
    std::unique_ptr<WiiDisc> d(new WiiDisc);
    d->impl_ = std::make_unique<Impl>();
    Impl& m = *d->impl_;
    m.file = std::fopen(path.string().c_str(), "rb");
    if (!m.file) {
        error = "Cannot open " + path.string() + ".";
        return nullptr;
    }
    std::uint8_t head[0x200];
    if (std::fread(head, 1, sizeof head, m.file) != sizeof head) {
        error = "That file is too small to be a Wii disc.";
        return nullptr;
    }
    const auto is = [&](const char* tag) { return std::memcmp(head, tag, std::strlen(tag)) == 0; };
    if (is("WIA\x01") || is("RVZ\x01")) {
        error = "That is a compressed Dolphin image (WIA or RVZ). Convert it to ISO or WBFS first (Dolphin can: right-click the game, Convert).";
        return nullptr;
    }
    if (is("CISO") || is("WII5") || is("NKIT")) {
        error = "That disc image is in a format this reader does not know. Use a WBFS or ISO dump.";
        return nullptr;
    }
    m.wbfs = is("WBFS");
    if (m.wbfs) {
        if (head[9] < 15 || head[9] > 30) {
            error = "That WBFS file looks damaged.";
            return nullptr;
        }
        m.sector = std::uint64_t(1) << head[9];
        // A dual-layer disc is the most it can map: its table starts at 0x300 (after the copy of the disc's header).
        const std::size_t entries = std::size_t((std::uint64_t(143432) * 2 * kCluster + m.sector - 1) / m.sector);
        std::vector<std::uint8_t> raw(2 * entries);
        if (!seek(m.file, 0x300) || std::fread(raw.data(), 1, raw.size(), m.file) != raw.size()) {
            error = "That WBFS file looks damaged.";
            return nullptr;
        }
        m.map.resize(entries);
        for (std::size_t i = 0; i < entries; i++) m.map[i] = std::uint16_t(raw[2 * i] << 8 | raw[2 * i + 1]);
    }
    std::uint8_t disc[0x100];
    if (!m.raw(0, disc, sizeof disc) || be32(disc + 0x18) != 0x5D1C9EA3) {
        error = "That is not a Wii disc image.";
        return nullptr;
    }
    d->game_id_.assign(reinterpret_cast<const char*>(disc), 6);
    const std::string kind = d->game_id_.substr(0, 4);
    if (kind != "RFFE" && kind != "RFFP") {
        const std::string title(reinterpret_cast<const char*>(disc + 0x20), strnlen(reinterpret_cast<const char*>(disc + 0x20), 64));
        error = "That disc is " + title + " (" + d->game_id_ + "), not Final Fantasy Crystal Chronicles: Echoes of Time.";
        return nullptr;
    }
    d->region_ = kind == "RFFE" ? "USA" : "Europe";

    // The game partition (type 0) from the first group of the partition table at 0x40000.
    std::uint8_t group[8];
    if (!m.raw(0x40000, group, 8)) {
        error = "That disc image is cut short.";
        return nullptr;
    }
    const std::uint32_t count = be32(group);
    const std::uint64_t table = std::uint64_t(be32(group + 4)) << 2;
    std::uint64_t part = 0;
    bool found = false;
    for (std::uint32_t i = 0; i < std::min<std::uint32_t>(count, 8) && !found; i++) {
        std::uint8_t e[8];
        if (!m.raw(table + 8 * i, e, 8)) break;
        if (be32(e + 4) == 0) {
            part = std::uint64_t(be32(e)) << 2;
            found = true;
        }
    }
    std::uint8_t ticket[0x2A4], off[4];
    if (!found || !m.raw(part, ticket, sizeof ticket) || !m.raw(part + 0x2B8, off, 4)) {
        error = "That disc has no game partition this reader can find.";
        return nullptr;
    }
    // The title key: decrypted with the common key, the title id (and zeros) as the IV.
    std::array<std::uint8_t, 16> iv{}, title_key;
    std::memcpy(iv.data(), ticket + 0x1DC, 8);
    std::memcpy(title_key.data(), ticket + 0x1BF, 16);
    Aes128(key).cbc_decrypt(iv, title_key.data(), title_key.data(), 16);
    m.title = std::make_unique<Aes128>(title_key);
    m.data_start = part + (std::uint64_t(be32(off)) << 2);

    std::uint8_t boot[0x440];
    if (!m.partition(0, boot, sizeof boot) || std::memcmp(boot, disc, 6) != 0) {
        error = "The key does not decrypt this disc. Check that it is the Wii common key (16 bytes, 32 hex digits).";
        return nullptr;
    }
    const std::uint64_t fst_pos = std::uint64_t(be32(boot + 0x424)) << 2, fst_size = std::uint64_t(be32(boot + 0x428)) << 2;
    if (fst_size < 12 || fst_size > (64u << 20)) {
        error = "That disc's file table is damaged.";
        return nullptr;
    }
    std::vector<std::uint8_t> fst(fst_size);
    if (!m.partition(fst_pos, fst.data(), fst.size())) {
        error = "That disc image is cut short.";
        return nullptr;
    }
    const std::uint32_t entries = be32(fst.data() + 8);
    if (entries == 0 || std::uint64_t(entries) * 12 > fst.size()) {
        error = "That disc's file table is damaged.";
        return nullptr;
    }
    const std::uint8_t* names = fst.data() + std::size_t(entries) * 12;
    const std::size_t names_size = fst.size() - std::size_t(entries) * 12;
    std::vector<std::pair<std::string, std::uint32_t>> dirs{{"", entries}};  // (path so far, entry the folder ends at)
    for (std::uint32_t i = 1; i < entries; i++) {
        while (dirs.size() > 1 && i >= dirs.back().second) dirs.pop_back();
        const std::uint8_t* e = fst.data() + std::size_t(i) * 12;
        const std::size_t name_at = std::size_t(e[1]) << 16 | std::size_t(e[2]) << 8 | e[3];
        if (name_at >= names_size) continue;
        const std::string name(reinterpret_cast<const char*>(names + name_at), strnlen(reinterpret_cast<const char*>(names + name_at), names_size - name_at));
        if (e[0] == 1) {
            dirs.push_back({dirs.back().first + name + "/", be32(e + 8)});
        } else {
            d->files_.push_back({dirs.back().first + name, std::uint64_t(be32(e + 4)) << 2, be32(e + 8)});
        }
    }
    return d;
}

const WiiFile* WiiDisc::find(const std::string& path) const {
    for (const WiiFile& f : files_)
        if (f.path == path) return &f;
    return nullptr;
}

bool WiiDisc::read(const WiiFile& file, std::uint64_t pos, void* out, std::size_t size) {
    if (pos + size > file.size) return false;
    return impl_->partition(file.offset + pos, out, size);
}

bool WiiDisc::extract(const WiiFile& file, const std::filesystem::path& dest, const std::function<bool(std::uint64_t)>& progress,
                      std::string& error) {
    std::error_code ec;
    std::filesystem::create_directories(dest.parent_path(), ec);
    const std::filesystem::path part = dest.string() + ".part";
    std::FILE* out = std::fopen(part.string().c_str(), "wb");
    if (!out) {
        error = "Cannot write " + part.string() + ".";
        return false;
    }
    std::vector<std::uint8_t> buffer(1u << 20);
    bool ok = true, cancelled = false;
    for (std::uint64_t done = 0; done < file.size && ok;) {
        const std::size_t n = std::size_t(std::min<std::uint64_t>(buffer.size(), file.size - done));
        if (!read(file, done, buffer.data(), n)) {
            error = "The disc image is cut short or damaged (reading " + file.path + ").";
            ok = false;
        } else if (std::fwrite(buffer.data(), 1, n, out) != n) {
            error = "Cannot write " + part.string() + " (is the disk full?).";
            ok = false;
        } else {
            done += n;
            if (progress && !progress(n)) {
                cancelled = true;
                ok = false;
            }
        }
    }
    std::fclose(out);
    if (!ok) {
        std::filesystem::remove(part, ec);
        if (cancelled) error = "Cancelled.";
        return false;
    }
    std::filesystem::rename(part, dest, ec);
    if (ec) {
        std::filesystem::remove(dest, ec);
        std::filesystem::rename(part, dest, ec);
    }
    if (ec) {
        error = "Cannot write " + dest.string() + ".";
        return false;
    }
    return true;
}

}  // namespace ntr
