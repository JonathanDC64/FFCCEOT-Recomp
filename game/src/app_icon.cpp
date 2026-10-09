#include "app_icon.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstring>
#include <fstream>

#ifdef _WIN32
#include <windows.h>

#include <shlobj.h>
#endif

namespace app_icon {
namespace {

constexpr std::size_t kBannerBytes = 0x840;  // what the banner's checksum covers (0x20..0x840) and the picture lives in

std::uint16_t crc16(const std::uint8_t* p, std::size_t n) {  // the DS banner's checksum: CRC-16/MODBUS
    std::uint32_t crc = 0xFFFF;
    for (std::size_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++) crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : crc >> 1;
    }
    return std::uint16_t(crc);
}

std::uint16_t le16(const std::uint8_t* p) { return std::uint16_t(p[0] | (p[1] << 8)); }

void put16(std::vector<std::uint8_t>& v, std::uint32_t x) {
    v.push_back(std::uint8_t(x));
    v.push_back(std::uint8_t(x >> 8));
}
void put32(std::vector<std::uint8_t>& v, std::uint32_t x) {
    put16(v, x & 0xFFFF);
    put16(v, x >> 16);
}

}  // namespace

std::optional<Icon> from_banner(const std::uint8_t* banner, std::size_t size) {
    if (banner == nullptr || size < kBannerBytes) return std::nullopt;
    const unsigned version = le16(banner);
    if (version != 1 && version != 2 && version != 3 && version != 0x103) return std::nullopt;
    if (le16(banner + 2) != crc16(banner + 0x20, kBannerBytes - 0x20)) return std::nullopt;
    Icon icon;
    const std::uint8_t* bitmap = banner + 0x20;
    const std::uint8_t* palette = banner + 0x220;
    for (int y = 0; y < kBannerSize; y++) {
        for (int x = 0; x < kBannerSize; x++) {
            // 4 x 4 tiles of 8 x 8 pixels, 4 bits a pixel, the left pixel in the low half of the byte
            const int tile = (y / 8) * 4 + (x / 8);
            const std::uint8_t byte = bitmap[tile * 32 + (y % 8) * 4 + (x % 8) / 2];
            const int index = (x & 1) ? byte >> 4 : byte & 15;
            std::uint8_t* px = &icon.rgba[(y * kBannerSize + x) * 4];
            const unsigned c = le16(palette + index * 2);  // BGR555
            const unsigned r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
            px[0] = std::uint8_t((r << 3) | (r >> 2));
            px[1] = std::uint8_t((g << 3) | (g >> 2));
            px[2] = std::uint8_t((b << 3) | (b >> 2));
            px[3] = index == 0 ? 0 : 255;  // colour 0 is see-through
        }
    }
    return icon;
}

std::optional<Icon> from_rom(const std::filesystem::path& rom) {
    std::ifstream f(rom, std::ios::binary);
    if (!f) return std::nullopt;
    std::uint8_t header[0x70];
    if (!f.read(reinterpret_cast<char*>(header), sizeof header)) return std::nullopt;
    const std::uint32_t offset = header[0x68] | (header[0x69] << 8) | (header[0x6A] << 16) | (std::uint32_t(header[0x6B]) << 24);
    if (offset == 0) return std::nullopt;
    std::vector<std::uint8_t> banner(kBannerBytes);
    f.seekg(offset);
    if (!f.read(reinterpret_cast<char*>(banner.data()), std::streamsize(banner.size()))) return std::nullopt;
    return from_banner(banner.data(), banner.size());
}

SDL_Surface* surface(const Icon& icon, int size) {
    SDL_Surface* s = SDL_CreateSurface(size, size, SDL_PIXELFORMAT_RGBA32);
    if (s == nullptr) return nullptr;
    auto* dst = static_cast<std::uint8_t*>(s->pixels);
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            std::uint8_t* d = dst + y * s->pitch + x * 4;
            if (size < kBannerSize) {
                // Averaged (colours weighted by their alpha, so see-through pixels do not darken the edges).
                const int f = kBannerSize / size;
                int sum[4] = {0, 0, 0, 0};
                for (int j = 0; j < f; j++) {
                    for (int i = 0; i < f; i++) {
                        const std::uint8_t* p = &icon.rgba[((y * f + j) * kBannerSize + x * f + i) * 4];
                        for (int c = 0; c < 3; c++) sum[c] += p[c] * p[3];
                        sum[3] += p[3];
                    }
                }
                for (int c = 0; c < 3; c++) d[c] = std::uint8_t(sum[3] ? sum[c] / sum[3] : 0);
                d[3] = std::uint8_t(sum[3] / (f * f));
            } else {
                std::memcpy(d, &icon.rgba[((y * kBannerSize / size) * kBannerSize + x * kBannerSize / size) * 4], 4);
            }
        }
    }
    return s;
}

void apply_to_window(SDL_Window* window, const Icon& icon) {
    if (window == nullptr) return;
    SDL_Surface* base = surface(icon, 32);
    if (base == nullptr) return;
    for (const int size : {64, 128, 256}) {
        if (SDL_Surface* big = surface(icon, size)) {
            SDL_AddSurfaceAlternateImage(base, big);
            SDL_DestroySurface(big);  // (the base keeps its own reference)
        }
    }
    SDL_SetWindowIcon(window, base);
    SDL_DestroySurface(base);
}

std::vector<std::uint8_t> png(const Icon& icon, int size) {
    std::vector<std::uint8_t> out;
    SDL_Surface* s = surface(icon, size);
    if (s == nullptr) return out;
    if (SDL_IOStream* io = SDL_IOFromDynamicMem()) {
        if (SDL_SavePNG_IO(s, io, false)) {
            const auto* data = static_cast<const std::uint8_t*>(
                SDL_GetPointerProperty(SDL_GetIOProperties(io), SDL_PROP_IOSTREAM_DYNAMIC_MEMORY_POINTER, nullptr));
            const Sint64 n = SDL_GetIOSize(io);
            if (data != nullptr && n > 0) out.assign(data, data + n);
        }
        SDL_CloseIO(io);
    }
    SDL_DestroySurface(s);
    return out;
}

std::vector<std::uint8_t> ico_file(const Icon& icon) {
    std::vector<std::vector<std::uint8_t>> images;
    for (const int size : kSizes) images.push_back(png(icon, size));
    std::vector<std::uint8_t> out;
    put16(out, 0);
    put16(out, 1);  // an icon
    put16(out, std::uint32_t(images.size()));
    std::uint32_t offset = 6 + 16 * std::uint32_t(images.size());
    for (std::size_t i = 0; i < images.size(); i++) {
        const int size = kSizes[i];
        out.push_back(std::uint8_t(size >= 256 ? 0 : size));
        out.push_back(std::uint8_t(size >= 256 ? 0 : size));
        out.push_back(0);  // colours in the palette: none
        out.push_back(0);
        put16(out, 1);   // planes
        put16(out, 32);  // bits per pixel
        put32(out, std::uint32_t(images[i].size()));
        put32(out, offset);
        offset += std::uint32_t(images[i].size());
    }
    for (const auto& image : images) out.insert(out.end(), image.begin(), image.end());
    return out;
}

#ifdef _WIN32
namespace {

std::filesystem::path exe_path() {
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, path.data(), DWORD(path.size()));
        if (n == 0) return {};
        if (n < path.size()) {
            path.resize(n);
            return path;
        }
        path.resize(path.size() * 2);
    }
}

// (RT_ICON and RT_GROUP_ICON are narrow-string macros here)
const wchar_t* const kRtIcon = MAKEINTRESOURCEW(3);
const wchar_t* const kRtGroupIcon = MAKEINTRESOURCEW(14);

// The icon resource id (1..) of a size: the order of kSizes, as game/ffcc.rc's icon has them.
const wchar_t* icon_id(int index) { return MAKEINTRESOURCEW(index + 1); }

// True when the running exe already holds this very picture at 32 pixels.
bool exe_has(const Icon& icon) {
    const auto wanted = png(icon, 32);
    HRSRC res = FindResourceW(nullptr, icon_id(1), kRtIcon);
    if (res == nullptr || wanted.empty()) return false;
    HGLOBAL mem = LoadResource(nullptr, res);
    const void* data = mem != nullptr ? LockResource(mem) : nullptr;
    return data != nullptr && SizeofResource(nullptr, res) == wanted.size() &&
           std::memcmp(data, wanted.data(), wanted.size()) == 0;
}

std::string last_error(const char* what) { return std::string(what) + " (error " + std::to_string(GetLastError()) + ")"; }

}  // namespace

bool patch_exe(const Icon& icon, std::string& error) {
    const std::filesystem::path exe = exe_path();
    if (exe.empty()) {
        error = "cannot find the program's own file";
        return false;
    }
    if (exe_has(icon)) return true;
    std::filesystem::path fresh = exe, old = exe;
    fresh += L".new";
    old += L".old";
    if (!CopyFileW(exe.c_str(), fresh.c_str(), FALSE)) {
        error = last_error("cannot copy the program");
        return false;
    }
    bool ok = false;
    if (HANDLE update = BeginUpdateResourceW(fresh.c_str(), FALSE)) {
        ok = true;
        std::vector<std::uint8_t> group;  // the icon directory of the resource: an .ico's, with ids in place of offsets
        put16(group, 0);
        put16(group, 1);
        put16(group, sizeof kSizes / sizeof kSizes[0]);
        for (int i = 0; i < int(sizeof kSizes / sizeof kSizes[0]); i++) {
            const auto image = png(icon, kSizes[i]);
            ok = ok && !image.empty() &&
                 UpdateResourceW(update, kRtIcon, icon_id(i), 0, const_cast<std::uint8_t*>(image.data()), DWORD(image.size()));
            const int size = kSizes[i];
            group.push_back(std::uint8_t(size >= 256 ? 0 : size));
            group.push_back(std::uint8_t(size >= 256 ? 0 : size));
            group.push_back(0);
            group.push_back(0);
            put16(group, 1);
            put16(group, 32);
            put32(group, std::uint32_t(image.size()));
            put16(group, std::uint32_t(i + 1));
        }
        ok = ok && UpdateResourceW(update, kRtGroupIcon, MAKEINTRESOURCEW(1), 0, group.data(), DWORD(group.size()));
        if (!EndUpdateResourceW(update, !ok)) ok = false;
    }
    if (!ok) {
        error = last_error("cannot write the icon into the program");
        DeleteFileW(fresh.c_str());
        return false;
    }
    // A running exe can be renamed but not overwritten: the old one steps aside, the new one takes its name.
    if (!MoveFileExW(exe.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        error = last_error("cannot move the program aside");
        DeleteFileW(fresh.c_str());
        return false;
    }
    if (!MoveFileExW(fresh.c_str(), exe.c_str(), 0)) {
        error = last_error("cannot put the new program in place");
        MoveFileExW(old.c_str(), exe.c_str(), MOVEFILE_REPLACE_EXISTING);
        DeleteFileW(fresh.c_str());
        return false;
    }
    SHChangeNotify(SHCNE_UPDATEITEM, SHCNF_PATHW, exe.c_str(), nullptr);
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return true;
}

void clean_up_exe() {
    const std::filesystem::path exe = exe_path();
    if (exe.empty()) return;
    for (const wchar_t* suffix : {L".old", L".new"}) {
        std::filesystem::path p = exe;
        p += suffix;
        DeleteFileW(p.c_str());
    }
}
#endif

}  // namespace app_icon
