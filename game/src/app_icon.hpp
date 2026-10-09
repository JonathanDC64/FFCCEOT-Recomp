#pragma once

// The game's own icon: the 32x32 picture a DS shows for the game, kept in the ROM's banner. It is Square Enix artwork, so it
// is never shipped or committed: it is read from the player's ROM (the copy setup keeps) when the program starts and used
//   - as the icon of the program's windows (and so the taskbar's), and
//   - on Windows, written into the player's own copy of ffcc.exe, so that Explorer and a pinned taskbar entry show it.
// Until then the program carries an original icon (game/res/ffcc.ico, drawn by tools/gen_app_icon.py).

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

struct SDL_Surface;
struct SDL_Window;

namespace app_icon {

inline constexpr int kBannerSize = 32;
// The sizes of the icon in an .ico (the order is the icon ids 1..6 of the exe's resource, game/ffcc.rc).
inline constexpr int kSizes[] = {16, 32, 48, 64, 128, 256};

struct Icon {
    std::array<std::uint8_t, kBannerSize * kBannerSize * 4> rgba{};  // R, G, B, A bytes, row by row
};

// Decodes the banner icon of a ROM file; nullopt if the file has no valid banner (wrong version or CRC).
std::optional<Icon> from_rom(const std::filesystem::path& rom);
// The same from the bytes of a banner (0x840 or more bytes starting at the banner).
std::optional<Icon> from_banner(const std::uint8_t* banner, std::size_t size);

// The icon at a size (nearest neighbour, so the pixels stay sharp; 16 is averaged). The caller destroys the surface.
SDL_Surface* surface(const Icon& icon, int size);
// The window's icon, with larger versions for high-DPI displays. Call it on the thread that owns the window.
void apply_to_window(SDL_Window* window, const Icon& icon);

// One size as PNG file bytes, and the whole icon as the bytes of an .ico file (a PNG at every size of kSizes).
std::vector<std::uint8_t> png(const Icon& icon, int size);
std::vector<std::uint8_t> ico_file(const Icon& icon);

#ifdef _WIN32
// Windows: writes the icon into this program's own exe (a copy is patched, then swapped in: a running exe can be renamed
// but not written). Does nothing when the exe already has it. Returns false and says why in `error` when it could not
// (a folder that is not writable, say). Slow (copies the exe): call it from a thread of its own.
bool patch_exe(const Icon& icon, std::string& error);
// Deletes what a patch left behind (ffcc.exe.old, ffcc.exe.new); ignores a file that is still in use.
void clean_up_exe();
#endif

}  // namespace app_icon
