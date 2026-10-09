#pragma once

// A fast 64-bit hash for comparing runs (--frame-hash): the emulated memory and the picture of every frame. Not
// cryptographic and not a stable format between versions: only equal inputs giving equal outputs matters.

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace ntr {

inline std::uint64_t hash64(const void* data, std::size_t size, std::uint64_t seed = 0) {
    constexpr std::uint64_t k1 = 0x9E3779B97F4A7C15ull, k2 = 0xC2B2AE3D27D4EB4Full;
    const auto* p = static_cast<const unsigned char*>(data);
    // Four lanes, so that the multiplies of one lane overlap the others'.
    std::uint64_t h[4] = {seed + 1 * k1, seed + 2 * k1, seed + 3 * k1, seed + 4 * k1};
    while (size >= 32) {
        for (int i = 0; i < 4; i++) {
            std::uint64_t w;
            std::memcpy(&w, p + 8 * i, 8);
            h[i] = (h[i] ^ w) * k2;
            h[i] ^= h[i] >> 29;
        }
        p += 32;
        size -= 32;
    }
    std::uint64_t tail = 0;
    for (std::size_t i = 0; i < size; i++) tail = (tail << 8) | p[i];
    std::uint64_t r = (h[0] ^ (h[1] << 1) ^ (h[2] << 2) ^ (h[3] << 3)) + tail * k1 + size;
    r ^= r >> 32;
    r *= k2;
    r ^= r >> 29;
    return r;
}

}  // namespace ntr
