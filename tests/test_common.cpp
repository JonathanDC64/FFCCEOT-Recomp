// Unit tests for ntr_common that need no game data.
#include <cstdio>
#include <cstring>
#include <string>

#include "ntr/rom.hpp"

static int failures = 0;
#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                   \
        }                                                                 \
    } while (0)

static std::string sha1_hex(const char* s) {
    return ntr::to_hex(ntr::sha1({reinterpret_cast<const uint8_t*>(s), std::strlen(s)}));
}

static void test_sha1() {
    CHECK(sha1_hex("") == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    CHECK(sha1_hex("abc") == "a9993e364706816aba3e25717850c26c9cd0d89d");
    CHECK(sha1_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
          "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
    std::string million(1000000, 'a');
    CHECK(sha1_hex(million.c_str()) == "34aa973cd4c4daa4f61eeb2bdbad27316534016f");
}

static void test_crc32() {
    const auto crc = [](const char* s) {
        return ntr::crc32_hex(ntr::crc32({reinterpret_cast<const uint8_t*>(s), std::strlen(s)}));
    };
    CHECK(crc("") == "00000000");
    CHECK(crc("123456789") == "cbf43926");  // the standard check value
}

static void test_blz() {
    // Region = uncompressed prefix "XY" + compressed stream + footer. The stream is read backwards:
    // flag byte 0x08 = four literals (D, C, B, A) then one reference (len 18, disp 4).
    const uint8_t compressed[] = {
        'X', 'Y',                 // uncompressed prefix (left untouched)
        0x01, 0xF0,               // reference: v = 0xF001 -> len 15+3, disp 1+3
        'A', 'B', 'C', 'D',       // literals, consumed from the end
        0x08,                     // flags
        0xFF,                     // padding covered by the footer length
        0x10, 0x00, 0x00, 0x09,   // compressed length 0x10, footer length 9
        0x06, 0x00, 0x00, 0x00,   // output grows by 6 bytes
    };
    auto out = ntr::blz_decompress(compressed);
    CHECK(std::string(out.begin(), out.end()) == "XYCDABCDABCDABCDABCDABCD");
}

static ntr::Sha1Digest digest_of(const std::string& hex) {
    ntr::Sha1Digest d{};
    for (size_t i = 0; i < d.size(); i++) d[i] = uint8_t(std::stoul(hex.substr(i * 2, 2), nullptr, 16));
    return d;
}

static void test_known_roms() {
    const ntr::KnownRom* usa = ntr::find_known_rom(digest_of("1df23d5a3a655bdd587c360b581202931d0f66e1"));
    const ntr::KnownRom* eur = ntr::find_known_rom(digest_of("18f7004f74c83feba40f003641ec00571e52a301"));
    CHECK(usa && eur);
    if (usa && eur) {
        CHECK(std::string(usa->game_code) == "CFIE" && usa->supported);
        CHECK(std::string(eur->game_code) == "CFIP" && eur->supported);
        CHECK(std::string(eur->crc32_hex) == "2dbad135");
    }
    CHECK(!ntr::find_known_rom(digest_of("0000000000000000000000000000000000000000")));
    // Every supported release has a game code of its own (the configuration and the hooks are chosen by it).
    for (const ntr::KnownRom& a : ntr::known_roms())
        for (const ntr::KnownRom& b : ntr::known_roms())
            if (&a != &b) CHECK(std::strcmp(a.game_code, b.game_code) != 0 || std::strcmp(a.sha1_hex, b.sha1_hex) != 0);
}

int main() {
    test_known_roms();
    test_sha1();
    test_crc32();
    test_blz();
    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::puts("all tests passed");
    return 0;
}
