#include "wii_cutscenes.hpp"

#include <algorithm>
#include <cstdio>
#include <string>

namespace wii {

namespace {

// The DS shows its movie two frames behind the frame count the game reports (its decoder runs ahead of the picture), so
// the picture on screen when the game reports frame k is frame k - 2 of the movie. The Wii cut matches the DS's frame for
// frame from the first to the last, in all 38 movies (measured by matching pictures, docs/WII_ASSETS.md); where it is
// longer, the extra frames are at the end.
constexpr int kDisplayDelay = 2;

}  // namespace

std::filesystem::path movie_dir(const std::filesystem::path& data_dir) { return data_dir / "wii" / "movie"; }

int wii_frame(int, int frame) { return frame - kDisplayDelay; }

void Cutscenes::set_dir(const std::filesystem::path& dir) {
    files_.clear();
    video_.close();
    open_id_ = -1;
    decoded_ = -1;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        const std::string name = e.path().filename().string();
        int id = 0;
        char tail[8] = {};
        if (std::sscanf(name.c_str(), "movie%3d.%7s", &id, tail) == 2 && std::string(tail) == "thp") files_[id] = e.path();
    }
}

Picture Cutscenes::picture(int id, int frame) {
    const auto file = files_.find(id);
    if (file == files_.end()) return {};
    if (id != open_id_) {
        std::string error;
        video_.close();
        open_id_ = id;
        decoded_ = -1;
        open_failed_ = !video_.open(file->second, error);
        if (open_failed_) std::fprintf(stderr, "wii cutscenes: %s\n", error.c_str());
    }
    if (open_failed_ || video_.frames() == 0) return {};
    const int index = std::clamp(wii_frame(id, frame), 0, video_.frames() - 1);
    if (index != decoded_) {
        if (!video_.decode(index, rgb_)) return {};
        decoded_ = index;
        serial_++;
    }
    return {rgb_.data(), video_.width(), video_.height(), serial_};
}

}  // namespace wii
