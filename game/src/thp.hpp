// THP video: the format of the Wii version's cutscenes (movie/movieNNN.mods on its disc are THP files). A THP file is a
// header, a table of where each frame ends, and then the frames, each one a JPEG picture. The JPEGs leave out the byte
// stuffing (a 0x00 after every 0xFF in the entropy-coded data) that ordinary decoders expect, so it is put back before a
// frame is decoded.
#pragma once

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace thp {

class Video {
public:
    Video() = default;
    ~Video() { close(); }
    Video(const Video&) = delete;
    Video& operator=(const Video&) = delete;

    // Opens a file and reads its header and frame table. False, with `error` set, if it is not a THP video this reader
    // understands (one video component, no sound).
    bool open(const std::filesystem::path& path, std::string& error);
    void close();
    bool is_open() const { return file_ != nullptr; }

    int width() const { return width_; }
    int height() const { return height_; }
    int frames() const { return int(end_.size()); }
    float fps() const { return fps_; }

    // Decodes frame `index` (0 is the first) to `rgb` (width * height * 3 bytes, top row first). False if it cannot.
    bool decode(int index, std::vector<uint8_t>& rgb);

private:
    std::FILE* file_ = nullptr;
    int width_ = 0, height_ = 0;
    float fps_ = 0;
    uint32_t base_ = 0;           // where the first frame starts in the file
    std::vector<uint32_t> end_;   // where each frame ends, from base_
    std::vector<uint8_t> raw_, jpeg_;
};

// A frame's JPEG data with its byte stuffing put back (see above): the header segments up to the scan are kept, then every
// 0xFF of the entropy-coded data is followed by a 0x00, and the data ends with the end-of-image marker. Empty if `raw` is
// not a JPEG this understands.
std::vector<uint8_t> stuff_jpeg(const uint8_t* raw, size_t size);

}  // namespace thp
