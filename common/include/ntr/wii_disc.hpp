#pragma once

// Reading the player's own dump of the Wii version of Echoes of Time (a WBFS file or a plain ISO) to get the files the
// frontend uses from it (the cutscenes, see game/src/wii_cutscenes.hpp). Nothing of the disc is kept: files are written to
// the player's data folder.
//
// A Wii disc's game partition is encrypted: its files are AES-128-CBC encrypted in clusters of 0x8000 bytes (0x400 of
// hashes, then 0x7C00 of data) with a title key, which the partition's ticket carries encrypted with the Wii's "common
// key". That key is not part of this program: the player supplies it (16 bytes, as 32 hex digits). A wrong key is told
// apart from a right one by the disc's own header, which reads as the game's id once decrypted.

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ntr {

using WiiKey = std::array<std::uint8_t, 16>;

// 32 hexadecimal digits (spaces, dashes and a 0x prefix are ignored). False, with `error` set, if it is not a 16-byte key.
bool parse_wii_key(const std::string& text, WiiKey& key, std::string& error);

struct WiiFile {
    std::string path;      // as on the disc: "movie/movie001.mods"
    std::uint64_t offset;  // in the partition's decrypted data
    std::uint64_t size;
};

class WiiDisc {
public:
    ~WiiDisc();
    WiiDisc(const WiiDisc&) = delete;
    WiiDisc& operator=(const WiiDisc&) = delete;

    // Opens a dump and checks it: a Wii disc (WBFS or ISO) of Echoes of Time that `key` decrypts. Null, with `error` set to
    // what to tell the player, otherwise.
    static std::unique_ptr<WiiDisc> open(const std::filesystem::path& path, const WiiKey& key, std::string& error);

    const std::string& game_id() const { return game_id_; }  // "RFFEGD"
    const std::string& region() const { return region_; }   // "USA" or "Europe"
    const std::vector<WiiFile>& files() const { return files_; }
    const WiiFile* find(const std::string& path) const;

    // Reads `size` bytes of a file's decrypted data at `pos`. False if it runs past the end of the file or the image.
    bool read(const WiiFile& file, std::uint64_t pos, void* out, std::size_t size);

    // Writes a file to `dest` (through a .part file that is renamed when it is whole). `progress` is told how many bytes were
    // written since its last call, and returns false to cancel. False, with `error` set, if it fails or is cancelled.
    bool extract(const WiiFile& file, const std::filesystem::path& dest, const std::function<bool(std::uint64_t)>& progress,
                 std::string& error);

private:
    WiiDisc() = default;
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::string game_id_, region_;
    std::vector<WiiFile> files_;
};

}  // namespace ntr
