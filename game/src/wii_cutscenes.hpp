// The Wii version's cutscenes, shown in place of the DS's. The Wii disc has the same movies (movie001 ... movie038) as 384x192
// THP video instead of the DS's 256x128 Mobiclip: more detail, with the same picture. The player extracts them from their own
// disc at setup (see wii_disc.hpp); the game keeps playing its own movie (its sound, its timing), and this supplies the
// picture for the frame it is on.
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <vector>

#include "thp.hpp"

namespace wii {

// The feature is switched off for now (its pieces stay in the build): nothing of it shows in the menus and no Wii picture is
// drawn. Set to true to bring it back.
constexpr bool kEnabled = false;

// The folder the movies are extracted to, under the data folder: <data>/wii/movie/movieNNN.thp.
std::filesystem::path movie_dir(const std::filesystem::path& data_dir);

// Which Wii frame goes with the DS's frame `frame` of movie `id`: the Wii cut of a movie can start earlier or run longer.
int wii_frame(int id, int frame);

struct Picture {
    const uint8_t* rgb = nullptr;  // width * height * 3 bytes
    int width = 0, height = 0;
    uint64_t serial = 0;  // changes with the picture
};

class Cutscenes {
public:
    // The folder to read the extracted movies from. Movies that are not there are simply not available.
    void set_dir(const std::filesystem::path& dir);
    bool any() const { return !files_.empty(); }
    bool has(int id) const { return files_.count(id) != 0; }

    // The picture for DS frame `frame` of movie `id`; empty (rgb null) if there is none (no file, or it cannot be read).
    Picture picture(int id, int frame);

private:
    std::map<int, std::filesystem::path> files_;
    int open_id_ = -1;
    bool open_failed_ = false;
    thp::Video video_;
    int decoded_ = -1;
    std::vector<uint8_t> rgb_;
    uint64_t serial_ = 0;
};

}  // namespace wii
