// Pieces of RmlUi's setup shared by the settings menu and the crash dialog: the embedded files, a system interface that
// prints RmlUi's warnings, and the fonts.
#pragma once

#include <RmlUi/Core.h>
#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "RmlUi_Platform_SDL.h"
#include "ui_assets.hpp"

namespace ui {

// Serves the embedded UI files to RmlUi.
class EmbeddedFiles final : public Rml::FileInterface {
    struct Open {
        const Asset* asset;
        size_t pos;
    };

public:
    Rml::FileHandle Open(const Rml::String& path) override {
        const Asset* a = find_asset(path);
        if (!a) return 0;
        return reinterpret_cast<Rml::FileHandle>(new struct Open{a, 0});
    }
    void Close(Rml::FileHandle file) override { delete reinterpret_cast<struct Open*>(file); }
    size_t Read(void* buffer, size_t size, Rml::FileHandle file) override {
        auto* f = reinterpret_cast<struct Open*>(file);
        const size_t n = std::min(size, f->asset->size - f->pos);
        std::memcpy(buffer, f->asset->data + f->pos, n);
        f->pos += n;
        return n;
    }
    bool Seek(Rml::FileHandle file, long offset, int origin) override {
        auto* f = reinterpret_cast<struct Open*>(file);
        const long base = origin == SEEK_SET ? 0 : origin == SEEK_CUR ? long(f->pos) : long(f->asset->size);
        const long p = base + offset;
        if (p < 0 || size_t(p) > f->asset->size) return false;
        f->pos = size_t(p);
        return true;
    }
    size_t Tell(Rml::FileHandle file) override { return reinterpret_cast<struct Open*>(file)->pos; }
};

// RmlUi's SDL system interface, with its log messages printed.
class System final : public SystemInterface_SDL {
public:
    using SystemInterface_SDL::SystemInterface_SDL;
    bool LogMessage(Rml::Log::Type type, const Rml::String& message) override {
        if (type <= Rml::Log::LT_WARNING) std::fprintf(stderr, "ui: %s\n", message.c_str());
        return true;
    }
};

// The menu's fonts, from the embedded files.
inline void load_fonts() {
    struct Font {
        const char* file;
        const char* family;
        Rml::Style::FontWeight weight;
    };
    for (const Font& f : {Font{"fonts/Cinzel.ttf", "Cinzel", Rml::Style::FontWeight::Auto},
                          Font{"fonts/AlegreyaSans-Regular.ttf", "Alegreya Sans", Rml::Style::FontWeight::Normal},
                          Font{"fonts/AlegreyaSans-Bold.ttf", "Alegreya Sans", Rml::Style::FontWeight::Bold}}) {
        const Asset* a = find_asset(f.file);
        if (!a || !Rml::LoadFontFace({reinterpret_cast<const Rml::byte*>(a->data), a->size}, f.family,
                                     Rml::Style::FontStyle::Normal, f.weight))
            std::fprintf(stderr, "ui: cannot load font %s\n", f.file);
    }
}

}  // namespace ui
