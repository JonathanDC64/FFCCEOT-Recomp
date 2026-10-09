// The game's icon: decoding the banner of a ROM (a made-up banner here, never the game's) and the .ico it is written
// into the exe as.
#include <SDL3/SDL.h>

#include <cstdio>
#include <cstring>
#include <vector>

#include "app_icon.hpp"

static int failures = 0;
#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                                   \
        }                                                                                 \
    } while (0)

namespace {

std::uint16_t crc16(const std::uint8_t* p, std::size_t n) {
    std::uint32_t crc = 0xFFFF;
    for (std::size_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++) crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : crc >> 1;
    }
    return std::uint16_t(crc);
}

// A banner whose pixel (x, y) is colour (x + y) % 16, and whose colour i is BGR555 (i, 2 i, 31 - i).
std::vector<std::uint8_t> make_banner() {
    std::vector<std::uint8_t> b(0x840, 0);
    b[0] = 1;
    for (int y = 0; y < 32; y++) {
        for (int x = 0; x < 32; x++) {
            const int tile = (y / 8) * 4 + (x / 8);
            std::uint8_t& byte = b[0x20 + tile * 32 + (y % 8) * 4 + (x % 8) / 2];
            const int index = (x + y) % 16;
            byte = std::uint8_t((x & 1) ? (byte & 0x0F) | (index << 4) : (byte & 0xF0) | index);
        }
    }
    for (int i = 0; i < 16; i++) {
        const unsigned c = unsigned(i) | (unsigned(2 * i) << 5) | (unsigned(31 - i) << 10);
        b[0x220 + i * 2] = std::uint8_t(c);
        b[0x221 + i * 2] = std::uint8_t(c >> 8);
    }
    const std::uint16_t crc = crc16(b.data() + 0x20, 0x820);
    b[2] = std::uint8_t(crc);
    b[3] = std::uint8_t(crc >> 8);
    return b;
}

void test_decode() {
    auto banner = make_banner();
    const auto icon = app_icon::from_banner(banner.data(), banner.size());
    CHECK(icon.has_value());
    if (!icon) return;
    for (int y : {0, 5, 13, 31}) {
        for (int x : {0, 1, 8, 9, 22, 31}) {
            const std::uint8_t* px = &icon->rgba[(y * 32 + x) * 4];
            const int index = (x + y) % 16;
            auto expand = [](unsigned v) { return std::uint8_t((v << 3) | (v >> 2)); };
            CHECK(px[0] == expand(unsigned(index)));
            CHECK(px[1] == expand(unsigned(2 * index)));
            CHECK(px[2] == expand(unsigned(31 - index)));
            CHECK(px[3] == (index == 0 ? 0 : 255));  // colour 0 is see-through
        }
    }
    auto wrong_crc = banner;
    wrong_crc[0x100] ^= 1;
    CHECK(!app_icon::from_banner(wrong_crc.data(), wrong_crc.size()).has_value());
    auto wrong_version = banner;
    wrong_version[0] = 9;
    CHECK(!app_icon::from_banner(wrong_version.data(), wrong_version.size()).has_value());
    CHECK(!app_icon::from_banner(banner.data(), 0x100).has_value());
}

void test_ico() {
    auto banner = make_banner();
    const auto icon = app_icon::from_banner(banner.data(), banner.size());
    CHECK(icon.has_value());
    if (!icon) return;
    const auto ico = app_icon::ico_file(*icon);
    CHECK(ico.size() > 6 + 16 * 6);
    auto u16 = [&](std::size_t o) { return unsigned(ico[o] | (ico[o + 1] << 8)); };
    auto u32 = [&](std::size_t o) { return u16(o) | (u16(o + 2) << 16); };
    CHECK(u16(0) == 0 && u16(2) == 1 && u16(4) == 6);
    for (int i = 0; i < 6; i++) {
        const std::size_t e = 6 + 16 * i;
        const int size = app_icon::kSizes[i];
        CHECK(ico[e] == (size >= 256 ? 0 : size));
        const std::size_t bytes = u32(e + 8), offset = u32(e + 12);
        CHECK(offset + bytes <= ico.size());
        CHECK(bytes > 8 && std::memcmp(&ico[offset], "\x89PNG", 4) == 0);
    }
    // the surface at 16 averages 2 x 2 pixels, at 64 repeats each pixel 2 x 2
    SDL_Surface* big = app_icon::surface(*icon, 64);
    CHECK(big != nullptr && big->w == 64);
    if (big) {
        const auto* p = static_cast<const std::uint8_t*>(big->pixels);
        CHECK(std::memcmp(p + 0, p + 4, 4) == 0);                       // (0,0) and (1,0) are one source pixel
        CHECK(std::memcmp(p + 0, p + big->pitch, 4) == 0);              // and (0,1)
        SDL_DestroySurface(big);
    }
}

}  // namespace

int main() {
    test_decode();
    test_ico();
    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::puts("icon: ok");
    return 0;
}
