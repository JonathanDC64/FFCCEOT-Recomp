#include "thp.hpp"

#include <cstring>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#include "stb_image.h"

namespace thp {

namespace {

uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

}  // namespace

std::vector<uint8_t> stuff_jpeg(const uint8_t* raw, size_t size) {
    // The segments run from the start-of-image marker to the start-of-scan.
    size_t i = 2;
    if (size < 4 || raw[0] != 0xFF || raw[1] != 0xD8) return {};
    for (;;) {
        if (i + 4 > size || raw[i] != 0xFF) return {};
        const uint8_t marker = raw[i + 1];
        const size_t length = size_t(raw[i + 2]) << 8 | raw[i + 3];
        i += 2 + length;
        if (marker == 0xDA) break;
    }
    // The entropy-coded data ends at the last end-of-image marker (after it comes padding).
    size_t end = size;
    while (end >= 2 && !(raw[end - 2] == 0xFF && raw[end - 1] == 0xD9)) end--;
    if (end < i + 2) return {};
    end -= 2;
    std::vector<uint8_t> out;
    out.reserve(end + 64 + (end - i) / 64);
    out.assign(raw, raw + i);
    for (size_t k = i; k < end; k++) {
        out.push_back(raw[k]);
        if (raw[k] == 0xFF) out.push_back(0x00);
    }
    out.push_back(0xFF);
    out.push_back(0xD9);
    return out;
}

bool Video::open(const std::filesystem::path& path, std::string& error) {
    close();
    file_ = std::fopen(path.string().c_str(), "rb");
    if (!file_) {
        error = "cannot open " + path.string();
        return false;
    }
    uint8_t h[0x50];
    if (std::fread(h, 1, sizeof h, file_) != sizeof h || std::memcmp(h, "THP\0", 4) != 0) {
        error = path.string() + " is not a THP video";
        close();
        return false;
    }
    // Header: magic, version, buffer size, audio buffer size, frame rate (float), frame count, first frame's size, data
    // size, where the component list is, where the frame table is, where the first frame is. Then the component list
    // (a count, 16 type bytes: 0 is video) and the video's size.
    const uint32_t frame_count = be32(h + 0x14), components = be32(h + 0x30), table = be32(h + 0x24), first = be32(h + 0x28);
    const uint32_t rate = be32(h + 0x10);
    std::memcpy(&fps_, &rate, 4);
    if (components != 1 || h[0x34] != 0 || frame_count == 0 || frame_count > 100000 || table < 0x50 ||
        first != table + 4 * frame_count) {
        error = path.string() + " is not a THP video with one video track";
        close();
        return false;
    }
    width_ = int(be32(h + 0x44));
    height_ = int(be32(h + 0x48));
    if (width_ <= 0 || height_ <= 0 || width_ > 4096 || height_ > 4096) {
        error = path.string() + ": bad picture size";
        close();
        return false;
    }
    base_ = first;
    std::vector<uint8_t> t(4 * size_t(frame_count));
    if (std::fseek(file_, long(table), SEEK_SET) != 0 || std::fread(t.data(), 1, t.size(), file_) != t.size()) {
        error = path.string() + ": the frame table is cut short";
        close();
        return false;
    }
    end_.resize(frame_count);
    for (uint32_t k = 0; k < frame_count; k++) end_[k] = be32(&t[4 * size_t(k)]);
    return true;
}

void Video::close() {
    if (file_) std::fclose(file_);
    file_ = nullptr;
    end_.clear();
    width_ = height_ = 0;
}

bool Video::decode(int index, std::vector<uint8_t>& rgb) {
    if (!file_ || index < 0 || index >= frames()) return false;
    const uint32_t from = index ? end_[size_t(index) - 1] : 0, to = end_[size_t(index)];
    if (to < from + 12 || to - from > (64u << 20)) return false;
    raw_.resize(to - from);
    if (std::fseek(file_, long(base_ + from), SEEK_SET) != 0 || std::fread(raw_.data(), 1, raw_.size(), file_) != raw_.size())
        return false;
    const uint32_t video = be32(&raw_[8]);
    if (video > raw_.size() - 12) return false;
    jpeg_ = stuff_jpeg(&raw_[12], video);
    if (jpeg_.empty()) return false;
    int w = 0, h = 0, n = 0;
    uint8_t* pixels = stbi_load_from_memory(jpeg_.data(), int(jpeg_.size()), &w, &h, &n, 3);
    if (!pixels) return false;
    const bool fits = w == width_ && h == height_;
    if (fits) rgb.assign(pixels, pixels + size_t(w) * size_t(h) * 3);
    stbi_image_free(pixels);
    return fits;
}

}  // namespace thp
