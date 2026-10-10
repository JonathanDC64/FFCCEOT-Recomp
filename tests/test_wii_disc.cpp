// Unit tests for the Wii disc reader (ntr_common): AES against the published test vectors, and a small disc of our own
// (made up here, with a key of our own) in both the plain ISO and the WBFS layout.
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "ntr/aes.hpp"
#include "ntr/wii_disc.hpp"

static int failures = 0;
#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                                \
        }                                                                              \
    } while (0)

using Bytes = std::vector<std::uint8_t>;

static Bytes hex(const char* s) {
    Bytes b;
    for (; s[0] && s[1]; s += 2) b.push_back(std::uint8_t(std::stoi(std::string(s, 2), nullptr, 16)));
    return b;
}

static ntr::WiiKey key_of(const Bytes& b) {
    ntr::WiiKey k;
    std::copy(b.begin(), b.begin() + 16, k.begin());
    return k;
}

static void test_aes() {
    // FIPS 197, appendix C.1.
    const ntr::Aes128 aes(key_of(hex("000102030405060708090a0b0c0d0e0f")));
    const Bytes plain = hex("00112233445566778899aabbccddeeff"), cipher = hex("69c4e0d86a7b0430d8cdb78070b4c55a");
    std::uint8_t out[16];
    aes.encrypt_block(plain.data(), out);
    CHECK(Bytes(out, out + 16) == cipher);
    aes.decrypt_block(cipher.data(), out);
    CHECK(Bytes(out, out + 16) == plain);

    // NIST SP 800-38A, F.2.1 and F.2.2: CBC, two blocks.
    const ntr::Aes128 cbc(key_of(hex("2b7e151628aed2a6abf7158809cf4f3c")));
    const Bytes pt = hex("6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51");
    const Bytes ct = hex("7649abac8119b246cee98e9b12e9197d5086cb9b507219ee95db113a917678b2");
    std::array<std::uint8_t, 16> iv;
    std::memcpy(iv.data(), hex("000102030405060708090a0b0c0d0e0f").data(), 16);
    Bytes buf(32);
    cbc.cbc_encrypt(iv, pt.data(), buf.data(), 32);
    CHECK(buf == ct);
    std::memcpy(iv.data(), hex("000102030405060708090a0b0c0d0e0f").data(), 16);
    cbc.cbc_decrypt(iv, ct.data(), buf.data(), 32);
    CHECK(buf == pt);
    // In place, and in two pieces.
    Bytes inplace = ct;
    std::memcpy(iv.data(), hex("000102030405060708090a0b0c0d0e0f").data(), 16);
    cbc.cbc_decrypt(iv, inplace.data(), inplace.data(), 16);
    cbc.cbc_decrypt(iv, inplace.data() + 16, inplace.data() + 16, 16);
    CHECK(inplace == pt);
}

static void test_key_text() {
    ntr::WiiKey k;
    std::string error;
    CHECK(ntr::parse_wii_key("00 11 22 33 44 55 66 77 88 99 aa bb cc dd ee ff", k, error));
    CHECK(k[0] == 0 && k[1] == 0x11 && k[15] == 0xFF);
    CHECK(ntr::parse_wii_key("0x00112233445566778899AABBCCDDEEFF\n", k, error));
    CHECK(k[10] == 0xAA);
    CHECK(!ntr::parse_wii_key("0011", k, error));
    CHECK(!ntr::parse_wii_key("zz112233445566778899aabbccddeeff", k, error));
}

// A disc of our own: a game id, a partition whose data holds one small file and one that crosses a cluster.
static const char* kCommonKey = "0f1e2d3c4b5a69788796a5b4c3d2e1f0";

static void put32(Bytes& b, size_t at, std::uint32_t v) {
    b[at] = std::uint8_t(v >> 24), b[at + 1] = std::uint8_t(v >> 16), b[at + 2] = std::uint8_t(v >> 8), b[at + 3] = std::uint8_t(v);
}

static Bytes make_iso(const char* id, Bytes& small_file, Bytes& big_file) {
    constexpr size_t part = 0x50000, data = 0x20000, clusters = 3;
    Bytes iso(part + data + clusters * 0x8000, 0);
    std::memcpy(&iso[0], id, 6);
    put32(iso, 0x18, 0x5D1C9EA3);
    std::memcpy(&iso[0x20], "A MADE UP DISC", 14);
    put32(iso, 0x40000, 1);
    put32(iso, 0x40004, 0x40020 >> 2);
    put32(iso, 0x40020, part >> 2);
    put32(iso, 0x40024, 0);

    // The clear partition data: the boot header, the file table, the files.
    Bytes clear(clusters * 0x7C00, 0);
    std::memcpy(&clear[0], id, 6);
    put32(clear, 0x424, 0x1000 >> 2);
    put32(clear, 0x428, 0x100 >> 2);
    // Entries: root (folder, 4 entries), "movie" folder (ends at 4), two files.
    Bytes fst(0x100, 0);
    fst[0] = 1, put32(fst, 8, 4);
    fst[12] = 1, fst[13] = 0, fst[14] = 0, fst[15] = 0, put32(fst, 12 + 4, 0), put32(fst, 12 + 8, 4);   // "movie", name at 0
    fst[24] = 0, put32(fst, 24 + 4, 0x2000 >> 2), put32(fst, 24 + 8, 100);                           // "a.mods", name at 6
    fst[27] = 6;
    fst[36] = 0, put32(fst, 36 + 4, 0x7000 >> 2), put32(fst, 36 + 8, 0x4000);                        // "b.mods", name at 13
    fst[39] = 13;
    const char names[] = "movie\0a.mods\0b.mods";
    std::memcpy(&fst[48], names, sizeof names);
    std::memcpy(&clear[0x1000], fst.data(), fst.size());
    small_file.resize(100);
    for (size_t i = 0; i < small_file.size(); i++) small_file[i] = std::uint8_t(i * 7 + 1);
    std::memcpy(&clear[0x2000], small_file.data(), small_file.size());
    big_file.resize(0x4000);
    for (size_t i = 0; i < big_file.size(); i++) big_file[i] = std::uint8_t((i * 31) ^ (i >> 8));
    std::memcpy(&clear[0x7000], big_file.data(), big_file.size());

    // The ticket: the title key (ours) encrypted with the common key, the title id as the IV.
    const Bytes title_key = hex("a1a2a3a4a5a6a7a8a9aaabacadaeafb0");
    Bytes ticket(0x2A4, 0);
    for (int i = 0; i < 8; i++) ticket[0x1DC + size_t(i)] = std::uint8_t(0x10 + i);
    std::array<std::uint8_t, 16> iv{};
    std::memcpy(iv.data(), &ticket[0x1DC], 8);
    ntr::Aes128(key_of(hex(std::string(kCommonKey).c_str()))).cbc_encrypt(iv, title_key.data(), &ticket[0x1BF], 16);
    std::memcpy(&iso[part], ticket.data(), ticket.size());
    put32(iso, part + 0x2B8, data >> 2);

    // The clusters: a hash block (here only the IV is set), then the data encrypted with the title key.
    const ntr::Aes128 title(key_of(title_key));
    for (size_t c = 0; c < clusters; c++) {
        std::uint8_t* cluster = &iso[part + data + c * 0x8000];
        for (int i = 0; i < 16; i++) cluster[0x3D0 + i] = std::uint8_t(c * 16 + size_t(i) + 3);
        std::array<std::uint8_t, 16> civ;
        std::memcpy(civ.data(), cluster + 0x3D0, 16);
        title.cbc_encrypt(civ, &clear[c * 0x7C00], cluster + 0x400, 0x7C00);
    }
    return iso;
}

static Bytes make_wbfs(const Bytes& iso) {
    constexpr size_t block = size_t(1) << 21;
    Bytes w(block + (iso.size() + block - 1) / block * block, 0);
    std::memcpy(&w[0], "WBFS", 4);
    w[8] = 9, w[9] = 21;
    std::memcpy(&w[0x200], iso.data(), 0x100);
    for (size_t b = 0; b * block < iso.size(); b++) {
        w[0x300 + 2 * b] = std::uint8_t((b + 1) >> 8), w[0x300 + 2 * b + 1] = std::uint8_t(b + 1);  // block b is at file block b + 1
        std::memcpy(&w[(b + 1) * block], &iso[b * block], std::min(block, iso.size() - b * block));
    }
    return w;
}

static void write_file(const std::filesystem::path& p, const Bytes& b) {
    std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
}

static void check_disc(const std::filesystem::path& path, const Bytes& small_file, const Bytes& big_file) {
    ntr::WiiKey key;
    std::string error;
    CHECK(ntr::parse_wii_key(kCommonKey, key, error));
    auto disc = ntr::WiiDisc::open(path, key, error);
    CHECK(disc != nullptr);
    if (!disc) {
        std::fprintf(stderr, "  (%s)\n", error.c_str());
        return;
    }
    CHECK(disc->game_id() == "RFFEGD" && disc->region() == "USA");
    CHECK(disc->files().size() == 2);
    const ntr::WiiFile* a = disc->find("movie/a.mods");
    const ntr::WiiFile* b = disc->find("movie/b.mods");
    CHECK(a && a->size == 100);
    CHECK(b && b->size == 0x4000);
    if (!a || !b) return;
    Bytes got(100);
    CHECK(disc->read(*a, 0, got.data(), got.size()) && got == small_file);
    Bytes big(0x4000);
    CHECK(disc->read(*b, 0, big.data(), big.size()) && big == big_file);  // (crosses two clusters)
    Bytes part(10);
    CHECK(disc->read(*b, 0x3000, part.data(), part.size()) && std::equal(part.begin(), part.end(), big_file.begin() + 0x3000));
    CHECK(!disc->read(*a, 95, got.data(), 10));  // (past the end of the file)

    const std::filesystem::path out = std::filesystem::temp_directory_path() / "ffcc_wii_test" / "movie" / "b.thp";
    std::filesystem::remove_all(out.parent_path().parent_path());
    std::uint64_t total = 0;
    CHECK(disc->extract(*b, out, [&](std::uint64_t n) { total += n; return true; }, error));
    CHECK(total == 0x4000);
    Bytes back(0x4000);
    std::ifstream(out, std::ios::binary).read(reinterpret_cast<char*>(back.data()), std::streamsize(back.size()));
    CHECK(back == big_file);
    CHECK(!std::filesystem::exists(out.string() + ".part"));
    CHECK(!disc->extract(*b, out.parent_path() / "c.thp", [](std::uint64_t) { return false; }, error));  // cancelled
    CHECK(!std::filesystem::exists(out.parent_path() / "c.thp") && !std::filesystem::exists(out.parent_path() / "c.thp.part"));
    std::filesystem::remove_all(out.parent_path().parent_path());

    // The wrong key is told from the right one.
    ntr::WiiKey wrong = key;
    wrong[3] ^= 1;
    CHECK(ntr::WiiDisc::open(path, wrong, error) == nullptr);
    CHECK(error.find("does not decrypt") != std::string::npos);
}

static void test_disc() {
    Bytes small_file, big_file;
    const Bytes iso = make_iso("RFFEGD", small_file, big_file);
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "ffcc_wii_test_images";
    std::filesystem::create_directories(dir);
    write_file(dir / "disc.iso", iso);
    write_file(dir / "disc.wbfs", make_wbfs(iso));
    check_disc(dir / "disc.iso", small_file, big_file);
    check_disc(dir / "disc.wbfs", small_file, big_file);

    // Other things are turned away with a reason.
    ntr::WiiKey key;
    std::string error;
    ntr::parse_wii_key(kCommonKey, key, error);
    Bytes other, ignored;
    write_file(dir / "other.iso", make_iso("RSBE01", other, ignored));
    CHECK(ntr::WiiDisc::open(dir / "other.iso", key, error) == nullptr && error.find("RSBE01") != std::string::npos);
    write_file(dir / "junk.bin", Bytes(0x1000, 0x55));
    CHECK(ntr::WiiDisc::open(dir / "junk.bin", key, error) == nullptr && error.find("not a Wii disc") != std::string::npos);
    Bytes rvz(0x1000, 0);
    std::memcpy(rvz.data(), "RVZ\x01", 4);
    write_file(dir / "x.rvz", rvz);
    CHECK(ntr::WiiDisc::open(dir / "x.rvz", key, error) == nullptr && error.find("RVZ") != std::string::npos);
    CHECK(ntr::WiiDisc::open(dir / "missing.iso", key, error) == nullptr);
    std::filesystem::remove_all(dir);
}

int main() {
    test_aes();
    test_key_text();
    test_disc();
    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("ok\n");
    return 0;
}
