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

int main() {
    test_sha1();
    test_blz();
    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::puts("all tests passed");
    return 0;
}
