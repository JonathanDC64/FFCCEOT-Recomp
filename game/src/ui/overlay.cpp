// Adapted from Dusklight (CC0), src/dusk/ui/overlay.cpp
#include "overlay.hpp"

#include "app.hpp"
#include "host.hpp"
#include "ui.hpp"

#include "../input.hpp"
#include "../spell_ring.hpp"

#include <SDL3/SDL_gamepad.h>

#include <algorithm>
#include <cmath>
#include <string>

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

// The radial: a backdrop disc and six slots (a holder pointing at the centre, rotated, and the element's orb on it, not
// rotated), laid out around the stick's screen. The root's `open` attribute grows it into view and shrinks it away.
void Overlay::update_spell_ring() {
    const SpellRingView& view = get_spell_ring();
    if (mSpellRing == nullptr) {
        if (!view.open) {
            return;
        }
        mSpellRing = append(mDocument, "div");
        mSpellRing->SetClass("spell-ring", true);
        append(mSpellRing, "div")->SetClass("ring-backdrop", true);
        for (int s = 0; s < ring::kSectors; s++) {
            auto* slot = append(mSpellRing, "div");
            slot->SetClass("ring-slot", true);
            append(slot, "div")->SetClass("ring-holder", true);
            auto* orb = append(slot, "div");
            orb->SetClass("ring-orb", true);
            mSpellRingSlots[s] = slot;
        }
    }
    // Which element each sector shows depends on the orientation.
    for (int s = 0; s < ring::kSectors; s++) {
        auto* orb = mSpellRingSlots[s]->GetLastChild();
        const int element = ring::element_of(s, view.top ? ring::Orientation::Top : ring::Orientation::Sides);
        for (int e = 0; e < 6; e++) orb->SetClass("element" + std::to_string(e), e == element);
    }
    // Where it goes: sized by the screen's height, so it scales with the window.
    if (view.size > 0) {
        const float size = view.size;
        const float radius = size * ring::kRadius;  // from the centre to a holder's centre
        const float holder_w = size * ring::kHolderWidth, holder_h = holder_w * 160 / 192;  // (the holders' art is 192 x 160)
        const float orb = holder_w * 88 / 192;
        const float orb_offset = holder_w * 22 / 192;                     // (the orb sits in the holder's wide part)
        const auto px = [](float v) { return std::to_string(std::lround(v)) + "px"; };
        mSpellRing->SetProperty("transform-origin", px(view.x) + " " + px(view.y) + " 0px");
        auto* backdrop = mSpellRing->GetFirstChild();
        const float disc = size * ring::kDiscRadius * 2;
        backdrop->SetProperty("left", px(view.x - disc / 2));
        backdrop->SetProperty("top", px(view.y - disc / 2));
        backdrop->SetProperty("width", px(disc));
        backdrop->SetProperty("height", px(disc));
        for (int s = 0; s < ring::kSectors; s++) {
            const float degrees = ring::sector_angle(s, view.top ? ring::Orientation::Top : ring::Orientation::Sides);
            const float a = degrees * 3.14159265f / 180.0f;
            const float dx = std::cos(a), dy = -std::sin(a);  // outward, on the screen
            auto* slot = mSpellRingSlots[s];
            slot->SetProperty("left", px(view.x + dx * radius - holder_w / 2));
            slot->SetProperty("top", px(view.y + dy * radius - holder_h / 2));
            slot->SetProperty("width", px(holder_w));
            slot->SetProperty("height", px(holder_h));
            auto* holder = slot->GetFirstChild();
            holder->SetProperty("transform", "rotate(" + std::to_string(-degrees) + "deg)");
            auto* orb_element = slot->GetLastChild();
            orb_element->SetProperty("left", px(holder_w / 2 + dx * orb_offset - orb / 2));
            orb_element->SetProperty("top", px(holder_h / 2 + dy * orb_offset - orb / 2));
            orb_element->SetProperty("width", px(orb));
            orb_element->SetProperty("height", px(orb));
        }
    }
    for (int s = 0; s < ring::kSectors; s++) {
        mSpellRingSlots[s]->SetClass("selected", view.open && s == view.sector);
    }
    if (view.open) {
        mSpellRing->SetAttribute("open", "");
    } else {
        mSpellRing->RemoveAttribute("open");
    }
}

void Overlay::update() {
    Document::update();
    if (mDocument == nullptr) {
        return;
    }
    update_spell_ring();
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
