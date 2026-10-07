// Plain SHA-1 (FIPS 180-1), used only to identify ROM dumps.
#include "ntr/rom.hpp"

#include <cstring>

namespace ntr {
namespace {

inline uint32_t rol(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

void sha1_block(uint32_t h[5], const uint8_t* p) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t(p[i * 4]) << 24) | (uint32_t(p[i * 4 + 1]) << 16) | (uint32_t(p[i * 4 + 2]) << 8) | p[i * 4 + 3];
    for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20)      { f = (b & c) | (~b & d);          k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d;                   k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
        else             { f = b ^ c ^ d;                   k = 0xCA62C1D6; }
        uint32_t t = rol(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rol(b, 30); b = a; a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}

}  // namespace

Sha1Digest sha1(std::span<const uint8_t> data) {
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    size_t full = data.size() / 64;
    for (size_t i = 0; i < full; i++) sha1_block(h, data.data() + i * 64);

    uint8_t tail[128] = {};
    size_t rem = data.size() % 64;
    if (rem) std::memcpy(tail, data.data() + full * 64, rem);
    tail[rem] = 0x80;
    size_t tail_len = rem < 56 ? 64 : 128;
    uint64_t bits = uint64_t(data.size()) * 8;
    for (int i = 0; i < 8; i++) tail[tail_len - 1 - i] = uint8_t(bits >> (i * 8));
    for (size_t off = 0; off < tail_len; off += 64) sha1_block(h, tail + off);

    Sha1Digest out;
    for (int i = 0; i < 5; i++)
        for (int j = 0; j < 4; j++) out[i * 4 + j] = uint8_t(h[i] >> (24 - j * 8));
    return out;
}

std::string to_hex(std::span<const uint8_t> bytes) {
    static const char digits[] = "0123456789abcdef";
    std::string s;
    s.reserve(bytes.size() * 2);
    for (uint8_t b : bytes) {
        s.push_back(digits[b >> 4]);
        s.push_back(digits[b & 15]);
    }
    return s;
}

}  // namespace ntr
