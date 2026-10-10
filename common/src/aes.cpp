#include "ntr/aes.hpp"

#include <cstring>

namespace ntr {

namespace {

// The tables are computed on first use (from the field arithmetic that defines them) rather than typed in.
struct Tables {
    std::uint8_t sbox[256], inv[256];
    std::uint32_t td[4][256];  // the inverse cipher's round: InvSubBytes, InvShiftRows and InvMixColumns together

    static std::uint8_t xtime(std::uint8_t x) { return std::uint8_t((x << 1) ^ ((x & 0x80) ? 0x1B : 0)); }
    static std::uint8_t mul(std::uint8_t a, std::uint8_t b) {
        std::uint8_t r = 0;
        while (b) {
            if (b & 1) r ^= a;
            a = xtime(a);
            b >>= 1;
        }
        return r;
    }

    Tables() {
        // The S-box: the multiplicative inverse in GF(2^8) (3 generates its non-zero elements), then an affine map.
        std::uint8_t p = 1, q = 1;
        do {
            p = std::uint8_t(p ^ xtime(p));  // p *= 3
            q ^= std::uint8_t(q << 1);       // q /= 3
            q ^= std::uint8_t(q << 2);
            q ^= std::uint8_t(q << 4);
            if (q & 0x80) q ^= 0x09;
            const auto rot = [](std::uint8_t x, int n) { return std::uint8_t((x << n) | (x >> (8 - n))); };
            const std::uint8_t x = std::uint8_t(q ^ rot(q, 1) ^ rot(q, 2) ^ rot(q, 3) ^ rot(q, 4));
            sbox[p] = std::uint8_t(x ^ 0x63);
        } while (p != 1);
        sbox[0] = 0x63;
        for (int i = 0; i < 256; i++) inv[sbox[i]] = std::uint8_t(i);
        for (int i = 0; i < 256; i++) {
            const std::uint8_t s = inv[i];
            const std::uint32_t w = std::uint32_t(mul(s, 14)) << 24 | std::uint32_t(mul(s, 9)) << 16 |
                                    std::uint32_t(mul(s, 13)) << 8 | mul(s, 11);
            td[0][i] = w;
            td[1][i] = w >> 8 | w << 24;
            td[2][i] = w >> 16 | w << 16;
            td[3][i] = w >> 24 | w << 8;
        }
    }
};

const Tables& tables() {
    static const Tables t;
    return t;
}

std::uint32_t load32(const std::uint8_t* p) {
    return std::uint32_t(p[0]) << 24 | std::uint32_t(p[1]) << 16 | std::uint32_t(p[2]) << 8 | p[3];
}
void store32(std::uint8_t* p, std::uint32_t v) {
    p[0] = std::uint8_t(v >> 24);
    p[1] = std::uint8_t(v >> 16);
    p[2] = std::uint8_t(v >> 8);
    p[3] = std::uint8_t(v);
}

}  // namespace

Aes128::Aes128(const std::array<std::uint8_t, 16>& key) {
    const Tables& t = tables();
    for (int i = 0; i < 4; i++) enc_[i] = load32(&key[size_t(4 * i)]);
    std::uint8_t rcon = 1;
    for (int i = 4; i < 44; i++) {
        std::uint32_t w = enc_[i - 1];
        if (i % 4 == 0) {
            w = w << 8 | w >> 24;  // RotWord, SubWord, and the round constant
            w = std::uint32_t(t.sbox[w >> 24]) << 24 | std::uint32_t(t.sbox[(w >> 16) & 255]) << 16 |
                std::uint32_t(t.sbox[(w >> 8) & 255]) << 8 | t.sbox[w & 255];
            w ^= std::uint32_t(rcon) << 24;
            rcon = Tables::xtime(rcon);
        }
        enc_[i] = enc_[i - 4] ^ w;
    }
    // The equivalent inverse cipher: the round keys in reverse order, InvMixColumns applied to all but the first and last.
    for (int r = 0; r <= 10; r++) {
        for (int c = 0; c < 4; c++) {
            std::uint32_t w = enc_[4 * (10 - r) + c];
            if (r != 0 && r != 10)
                w = t.td[0][t.sbox[w >> 24]] ^ t.td[1][t.sbox[(w >> 16) & 255]] ^ t.td[2][t.sbox[(w >> 8) & 255]] ^
                    t.td[3][t.sbox[w & 255]];
            dec_[4 * r + c] = w;
        }
    }
}

void Aes128::decrypt_block(const std::uint8_t in[16], std::uint8_t out[16]) const {
    const Tables& t = tables();
    std::uint32_t s0 = load32(in) ^ dec_[0], s1 = load32(in + 4) ^ dec_[1], s2 = load32(in + 8) ^ dec_[2],
                  s3 = load32(in + 12) ^ dec_[3];
    for (int r = 1; r < 10; r++) {
        const std::uint32_t t0 = t.td[0][s0 >> 24] ^ t.td[1][(s3 >> 16) & 255] ^ t.td[2][(s2 >> 8) & 255] ^ t.td[3][s1 & 255] ^ dec_[4 * r];
        const std::uint32_t t1 = t.td[0][s1 >> 24] ^ t.td[1][(s0 >> 16) & 255] ^ t.td[2][(s3 >> 8) & 255] ^ t.td[3][s2 & 255] ^ dec_[4 * r + 1];
        const std::uint32_t t2 = t.td[0][s2 >> 24] ^ t.td[1][(s1 >> 16) & 255] ^ t.td[2][(s0 >> 8) & 255] ^ t.td[3][s3 & 255] ^ dec_[4 * r + 2];
        const std::uint32_t t3 = t.td[0][s3 >> 24] ^ t.td[1][(s2 >> 16) & 255] ^ t.td[2][(s1 >> 8) & 255] ^ t.td[3][s0 & 255] ^ dec_[4 * r + 3];
        s0 = t0, s1 = t1, s2 = t2, s3 = t3;
    }
    const auto last = [&](std::uint32_t a, std::uint32_t b, std::uint32_t c, std::uint32_t d, std::uint32_t k) {
        return (std::uint32_t(t.inv[a >> 24]) << 24 | std::uint32_t(t.inv[(b >> 16) & 255]) << 16 |
                std::uint32_t(t.inv[(c >> 8) & 255]) << 8 | t.inv[d & 255]) ^ k;
    };
    store32(out, last(s0, s3, s2, s1, dec_[40]));
    store32(out + 4, last(s1, s0, s3, s2, dec_[41]));
    store32(out + 8, last(s2, s1, s0, s3, dec_[42]));
    store32(out + 12, last(s3, s2, s1, s0, dec_[43]));
}

void Aes128::encrypt_block(const std::uint8_t in[16], std::uint8_t out[16]) const {
    const Tables& t = tables();
    std::uint8_t s[16];
    for (int i = 0; i < 16; i++) s[i] = in[i] ^ std::uint8_t(enc_[i / 4] >> (24 - 8 * (i % 4)));
    for (int r = 1; r <= 10; r++) {
        std::uint8_t b[16];
        for (int i = 0; i < 16; i++) b[i] = t.sbox[s[i]];
        // ShiftRows: the byte at column c, row k comes from column (c + k) mod 4 (the state is stored column by column).
        std::uint8_t m[16];
        for (int c = 0; c < 4; c++)
            for (int k = 0; k < 4; k++) m[4 * c + k] = b[4 * ((c + k) % 4) + k];
        if (r != 10) {
            for (int c = 0; c < 4; c++) {
                const std::uint8_t a0 = m[4 * c], a1 = m[4 * c + 1], a2 = m[4 * c + 2], a3 = m[4 * c + 3];
                b[4 * c] = std::uint8_t(Tables::xtime(a0) ^ (Tables::xtime(a1) ^ a1) ^ a2 ^ a3);
                b[4 * c + 1] = std::uint8_t(a0 ^ Tables::xtime(a1) ^ (Tables::xtime(a2) ^ a2) ^ a3);
                b[4 * c + 2] = std::uint8_t(a0 ^ a1 ^ Tables::xtime(a2) ^ (Tables::xtime(a3) ^ a3));
                b[4 * c + 3] = std::uint8_t((Tables::xtime(a0) ^ a0) ^ a1 ^ a2 ^ Tables::xtime(a3));
            }
            std::memcpy(m, b, 16);
        }
        for (int i = 0; i < 16; i++) s[i] = m[i] ^ std::uint8_t(enc_[4 * r + i / 4] >> (24 - 8 * (i % 4)));
    }
    std::memcpy(out, s, 16);
}

void Aes128::cbc_decrypt(std::array<std::uint8_t, 16>& iv, const std::uint8_t* in, std::uint8_t* out, std::size_t size) const {
    std::uint8_t prev[16], next[16];
    std::memcpy(prev, iv.data(), 16);
    for (std::size_t i = 0; i + 16 <= size; i += 16) {
        std::memcpy(next, in + i, 16);  // (kept: `in` may be `out`)
        decrypt_block(next, out + i);
        for (int k = 0; k < 16; k++) out[i + size_t(k)] ^= prev[k];
        std::memcpy(prev, next, 16);
    }
    std::memcpy(iv.data(), prev, 16);
}

void Aes128::cbc_encrypt(std::array<std::uint8_t, 16>& iv, const std::uint8_t* in, std::uint8_t* out, std::size_t size) const {
    std::uint8_t block[16];
    std::memcpy(block, iv.data(), 16);
    for (std::size_t i = 0; i + 16 <= size; i += 16) {
        for (int k = 0; k < 16; k++) block[k] ^= in[i + size_t(k)];
        encrypt_block(block, block);
        std::memcpy(out + i, block, 16);
    }
    std::memcpy(iv.data(), block, 16);
}

}  // namespace ntr
