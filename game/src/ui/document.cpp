// Adapted from Dusklight (CC0), src/dusk/ui/document.cpp
#include "document.hpp"

#include "host.hpp"
#include "ui.hpp"

#include <algorithm>

namespace ui {
namespace {

Rml::ElementDocument* load_document(const Rml::String& source) {
    auto* context = host::context();
    if (context == nullptr) {
        return nullptr;
    }
    auto* document = context->LoadDocumentFromMemory(source);
    if (document != nullptr) {
        if (auto global = Rml::Factory::InstanceStyleSheetFile("res/rml/global.rcss")) {
            if (const auto* local = document->GetStyleSheetContainer()) {
                global = global->CombineStyleSheetContainer(*local);
            }
            document->SetStyleSheetContainer(std::move(global));
        }
    }
    return document;
}

}  // namespace

Document::Document(const Rml::String& source, bool passive, DocumentScope scope)
    : mDocument(load_document(source)), mScope(scope), mPassive(passive) {
    // Block events while hidden (except for Menu command); play nav sounds when visible
    listen(
        Rml::EventId::Keydown,
        [this](Rml::Event& event) {
            if (mPassive) {
                return;
            }
            const auto cmd = map_nav_event(event);
            if (cmd != NavCommand::Menu && (!visible() || !active())) {
                event.StopImmediatePropagation();
            }
        },
        true);
    const auto blockUnlessActive = [this](Rml::Event& event) {
        if (!visible() || !active()) {
            event.StopImmediatePropagation();
        }
    };
    listen(Rml::EventId::Mouseover, blockUnlessActive, true);
    listen(Rml::EventId::Click, blockUnlessActive, true);
    listen(Rml::EventId::Scroll, blockUnlessActive, true);

    listen(Rml::EventId::Keydown, [this](Rml::Event& event) {
        if (mPassive) {
            auto* doc = top_document();
            if (doc != nullptr && doc->handle_nav_event(event)) {
                event.StopPropagation();
            }
            return;
        }
        if (handle_nav_event(event)) {
            event.StopPropagation();
        }
    });
}

Document::~Document() {
    mListeners.clear();
    if (mDocument != nullptr) {
        mDocument->Close();
        mDocument = nullptr;
    }
}

void Document::show() {
    if (mDocument != nullptr) {
        // Attempt to preserve the previously focused element
        mDocument->Show(Rml::ModalFlag::None, Rml::FocusFlag::Keep, Rml::ScrollFlag::None);
        // If nothing is focused, let the document decide the initial focus
        auto* leaf = mDocument->GetFocusLeafNode();
        if (leaf == nullptr || leaf == mDocument) {
            focus();
        }
    }
    mPendingClose = false;
}

void Document::hide(bool close) {
    if (mDocument != nullptr) {
        mDocument->Hide();
    }
    if (close) {
        mClosed = true;
    }
}

void Document::update() {}

bool Document::focus() {
    return false;
}

bool Document::has_focus() const {
    if (mDocument == nullptr) {
        return false;
    }
    auto* context = mDocument->GetContext();
    const auto* focused = context != nullptr ? context->GetFocusElement() : nullptr;
    return focused != nullptr && focused->GetOwnerDocument() == mDocument;
}

void Document::listen(Rml::Element* element, Rml::EventId event,
    ScopedEventListener::Callback callback, bool capture) {
    if (element == nullptr) {
        element = mDocument;
    }
    if (element == nullptr || !callback) {
        return;
    }
    mListeners.emplace_back(
        std::make_unique<ScopedEventListener>(element, event, std::move(callback), capture));
}

void Document::listen(Rml::Element* element, const Rml::String& event,
    ScopedEventListener::Callback callback, bool capture) {
    if (element == nullptr) {
        element = mDocument;
    }
    if (element == nullptr || event.empty() || !callback) {
        return;
    }
    mListeners.emplace_back(
        std::make_unique<ScopedEventListener>(element, event, std::move(callback), capture));
}

bool Document::visible() const {
    if (mDocument == nullptr) {
        return false;
    }
    return *mDocument->GetProperty(Rml::PropertyId::Visibility) == Rml::Style::Visibility::Visible;
}

bool Document::active() const {
    return !mClosed && !mPendingClose;
}

bool Document::handle_nav_event(Rml::Event& event) {
    if (!active()) {
        return false;
    }
    const auto cmd = map_nav_event(event);
    if (cmd == NavCommand::None || (cmd != NavCommand::Menu && !visible())) {
        return false;
    }
    return handle_nav_command(event, cmd);
}

bool Document::handle_nav_command(Rml::Event& event, NavCommand cmd) {
    if (cmd == NavCommand::Menu) {
        if (game_obscured_below(*this)) {
            return true;
        }
        play_sound(visible() ? kSoundMenuClose : kSoundMenuOpen);
        toggle();
        return true;
    }
    return false;
}

}  // namespace ui
