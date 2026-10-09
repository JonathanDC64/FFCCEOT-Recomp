// Adapted from Dusklight (CC0), src/dusk/ui/overlay.cpp
#include "overlay.hpp"

#include "app.hpp"
#include "host.hpp"
#include "ui.hpp"

#include "../input.hpp"

#include <SDL3/SDL_gamepad.h>

#include <algorithm>

namespace ui {
namespace {

const Rml::String kDocumentSource = R"RML(
<rml>
<head>
    <link type="text/rcss" href="res/rml/theme.rcss" />
    <link type="text/rcss" href="res/rml/global.rcss" />
    <link type="text/rcss" href="res/rml/overlay.rcss" />
</head>
<body>
</body>
</rml>
)RML";

constexpr auto kMenuNotificationDuration = std::chrono::milliseconds(2500);

Rml::Element* create_toast(Rml::Element* parent, const Toast& toast) {
    auto* elem = append(parent, "toast");
    if (!toast.type.empty()) {
        elem->SetClass(toast.type, true);
    }
    {
        auto* heading = append(elem, "heading");
        if (toast.title.starts_with("<")) {
            heading->SetInnerRML(toast.title);
        } else {
            append_text(append(heading, "toast-title"), toast.title);
        }
        if (toast.type == "warning") {
            auto* icon = append(heading, "icon");
            icon->SetClass("warning", true);
        }
    }
    {
        auto* message = append(elem, "message");
        if (toast.content.starts_with("<")) {
            message->SetInnerRML(toast.content);
        } else {
            append_text(append(message, "toast-message-text"), toast.content);
        }
    }
    {
        auto* progress = append(elem, "progress");
        progress->SetAttribute("value", 1.f);
    }
    return elem;
}

// "Press F1 or (gamepad) R + Start to open menu", or the gamepad button chosen for "Open menu".
Rml::Element* create_menu_notification(Rml::Element* parent) {
    auto* elem = append(parent, "toast");
    elem->SetClass("menu-notification", true);

    Rml::String padButton = "R + Start";
    if (const auto* bindings = app().bindings) {
        const auto& pad = bindings->pad[int(::input::Action::OpenMenu)];
        if (pad.kind != ::input::PadInput::Kind::None) {
            padButton = ::input::pad_label(pad, SDL_GAMEPAD_TYPE_STANDARD);
        }
    }

    auto* message = append(elem, "message");
    auto* row = append(message, "row");
    auto* prefix = append(row, "notification-prefix");
    append_text(prefix, "Press ");
    append_text(append(prefix, "b"), "F1");
    append_text(prefix, " or");
    auto* icon = append(row, "icon");
    icon->SetClass("controller", true);
    append_text(append(append(row, "notification-button"), "b"), padButton);
    append_text(append(row, "notification-action"), "to open menu");

    return elem;
}

void remove_element(Rml::Element*& elem) noexcept {
    if (elem == nullptr) {
        return;
    }
    if (auto* parent = elem->GetParentNode()) {
        parent->RemoveChild(elem);
    }
    elem = nullptr;
}

}  // namespace

Overlay::Overlay() : Document(kDocumentSource, true, DocumentScope::Overlay) {
    listen(mDocument, Rml::EventId::Transitionend, [this](Rml::Event& event) {
        if (event.GetTargetElement() == mCurrentToast) {
            if (get_toasts().empty() ||
                clock::now() >= mCurrentToastStartTime + get_toasts().front().duration)
            {
                mCurrentToast->SetPseudoClass("done", true);
            }
        } else if (mMenuNotification != nullptr && event.GetTargetElement() == mMenuNotification &&
                   !mMenuNotification->HasAttribute("open"))
        {
            mMenuNotification->SetPseudoClass("done", true);
        }
    });
}

void Overlay::show() {
    if (mDocument != nullptr) {
        mDocument->Show(Rml::ModalFlag::None, Rml::FocusFlag::None, Rml::ScrollFlag::None);
    }
}

void Overlay::update() {
    Document::update();
    if (mDocument == nullptr) {
        return;
    }
    // Without transitions (tests) nothing would mark an element done; treat it as done once closed.
    const bool animated = host::animations_enabled();

    if (mMenuNotification != nullptr) {
        if (clock::now() >= mMenuNotificationStartTime + kMenuNotificationDuration) {
            if (!animated || mMenuNotification->IsPseudoClassSet("done") ||
                !mMenuNotification->IsPseudoClassSet("opened"))
            {
                remove_element(mMenuNotification);
            } else {
                mMenuNotification->RemoveAttribute("open");
            }
        } else {
            mMenuNotification->SetAttribute("open", "");
            mMenuNotification->SetPseudoClass("opened", true);
            mMenuNotification->SetPseudoClass("done", false);
        }
    }
    if (consume_menu_notification_request()) {
        if (mMenuNotification == nullptr) {
            mMenuNotification = create_menu_notification(mDocument);
        }
        mMenuNotificationStartTime = clock::now();
    }

    auto& toasts = get_toasts();
    if (mCurrentToast == nullptr) {
        if (!toasts.empty()) {
            const auto& toast = toasts.front();
            mCurrentToast = create_toast(mDocument, toast);
            mCurrentToastStartTime = clock::now();
        }
    } else if (!toasts.empty()) {
        const auto& toast = toasts.front();
        const float duration = std::chrono::duration<float>(toast.duration).count();
        const float elapsed =
            std::chrono::duration<float>(clock::now() - mCurrentToastStartTime).count();
        const float ratio = duration > 0.0f ? std::clamp(elapsed / duration, 0.0f, 1.0f) : 1.0f;
        const auto remaining = 1.f - ratio;
        if (auto* progress = mCurrentToast->QuerySelector("progress")) {
            progress->SetAttribute("value", remaining);
        }
        if (remaining == 0.f) {
            if (!animated || mCurrentToast->IsPseudoClassSet("done") ||
                // Fallback for large gaps in time where we never actually opened it
                !mCurrentToast->IsPseudoClassSet("opened"))
            {
                remove_element(mCurrentToast);
                toasts.pop_front();
            } else {
                mCurrentToast->RemoveAttribute("open");
            }
        } else {
            mCurrentToast->SetAttribute("open", "");
            mCurrentToast->SetPseudoClass("opened", true);
        }
    }
}

bool Overlay::handle_nav_command(Rml::Event&, NavCommand) {
    return false;
}

}  // namespace ui
