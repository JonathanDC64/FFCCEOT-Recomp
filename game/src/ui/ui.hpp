// Adapted from Dusklight (CC0), src/dusk/ui/ui.hpp
#pragma once

#include <RmlUi/Core.h>
#include <SDL3/SDL_events.h>

#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "nav_types.hpp"
#include "sound.hpp"

namespace ui {
class Document;

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using clock = std::chrono::steady_clock;

enum class DocumentScope : u8 {
    None,
    Prelaunch,
    Window,
    MenuBar,
    Overlay,
};

struct Toast {
    Rml::String type;
    Rml::String title;
    Rml::String content;
    clock::duration duration;
};

struct Insets {
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;
    float left = 0.0f;

    bool operator==(const Insets& other) const noexcept {
        return top == other.top && right == other.right && bottom == other.bottom &&
               left == other.left;
    }
};

// Loads nothing the host has not: the context must exist (ui::host::init).
bool initialize() noexcept;
void shutdown() noexcept;

// Gives the UI an event of the window: keyboard, text, mouse and gamepad. The program calls it for every event first.
void handle_event(const SDL_Event& event) noexcept;
void update() noexcept;

// While set, every event goes to `capture` before anything else; when it returns true the event is used up (the binding
// controls take the next key or button this way).
using EventCapture = std::function<bool(const SDL_Event&)>;
void set_event_capture(EventCapture capture) noexcept;

Document& push_document(
    std::unique_ptr<Document> doc, bool show = true, bool passive = false) noexcept;

namespace detail {
Document& pop_to_or_push_document(bool (*matches)(Document&),
    const std::function<std::unique_ptr<Document>()>& create,
    const std::function<void(Document&)>& configure);
}

template <typename T, typename Configure, typename... Args>
T& pop_to_or_push(Configure&& configure, Args&&... args) {
    return static_cast<T&>(detail::pop_to_or_push_document(
        [](Document& document) { return dynamic_cast<T*>(&document) != nullptr; },
        [&]() -> std::unique_ptr<Document> {
            return std::make_unique<T>(std::forward<Args>(args)...);
        },
        [&](Document& document) {
            std::invoke(std::forward<Configure>(configure), static_cast<T&>(document));
        }));
}

void bring_document_to_front(Document& doc) noexcept;
void uncover_top_document() noexcept;
Document* find_document(DocumentScope scope) noexcept;
void close_all_documents() noexcept;
bool any_document_visible() noexcept;
bool is_prelaunch_open() noexcept;
bool game_obscured_below(const Document& doc) noexcept;
Document* top_document() noexcept;

std::string escape(std::string_view str) noexcept;
Rml::Element* append(Rml::Element* parent, const Rml::String& tag) noexcept;
Rml::Element* append_text(Rml::Element* parent, const Rml::String& text) noexcept;
Rml::Element* append_text_element(
    Rml::Element* parent, const Rml::String& tag, const Rml::String& text) noexcept;
void clear_children(Rml::Element* parent) noexcept;
void set_text_content(Rml::Element* parent, const Rml::String& text) noexcept;
void set_display(Rml::Element* element, Rml::Style::Display display) noexcept;

NavCommand map_nav_event(const Rml::Event& event) noexcept;
Insets safe_area_insets(Rml::Context* context) noexcept;

void push_toast(Toast toast) noexcept;

// Asks "Quit?" in a window of its own (the menu's Quit tab, the start screen's Quit and the window's close button).
// Does nothing if it is already asking.
void request_quit_confirm();
std::deque<Toast>& get_toasts() noexcept;
void show_menu_notification() noexcept;
bool consume_menu_notification_request() noexcept;

// The spell radial (see spell_ring.hpp), drawn by the overlay: whether it is up, which of its six sectors is picked (-1
// none), and where it goes: its centre in window pixels and the height of the screen it sits on (it scales with it).
struct SpellRingView {
    bool open = false;
    int sector = -1;
    bool top = false;  // the Top orientation (see spell_ring.hpp)
    float x = 0, y = 0, size = 0;
};
void set_spell_ring(const SpellRingView& view) noexcept;
const SpellRingView& get_spell_ring() noexcept;

// The UI's size from the settings (Auto, or 50-200%).
void apply_scale() noexcept;

}  // namespace ui
