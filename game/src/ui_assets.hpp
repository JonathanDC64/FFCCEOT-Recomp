// The settings UI's files (markup, styles, fonts), embedded in the executable from game/ui by cmake/EmbedFiles.cmake.
#pragma once

#include <cstddef>
#include <string_view>

namespace ui {

struct Asset {
    const char* path;  // relative to game/ui, e.g. "menu.rml"
    const unsigned char* data;
    size_t size;
};

extern const Asset kAssets[];
extern const size_t kAssetCount;

inline const Asset* find_asset(std::string_view path) {
    for (size_t i = 0; i < kAssetCount; i++)
        if (path == kAssets[i].path) return &kAssets[i];
    return nullptr;
}

}  // namespace ui
