// Adapted from Dusklight (CC0), src/dusk/ui/prelaunch.cpp. Dusklight's disc image becomes the player's ROM (see
// rom_setup.cpp); its game modes, mods and update check are not part of this game.
#include "prelaunch.hpp"

#include "app.hpp"
#include "host.hpp"
#include "modal.hpp"
#include "rom_setup.hpp"
#include "settings_window.hpp"
#include "ui.hpp"

#include "../report.hpp"
#include "../setup.hpp"
#include "../update_check.hpp"

namespace ui {
namespace {

const Rml::String kDocumentSource = R"RML(
<rml>
<head>
    <link type="text/rcss" href="res/rml/theme.rcss" />
    <link type="text/rcss" href="res/rml/controls.rcss" />
    <link type="text/rcss" href="res/rml/prelaunch.rcss" />
</head>
<body>
    <prelaunch-gradient />
    <prelaunch-background />
    <content id="root" open>
        <menu>
            <hero class="intro-item delay-0">
                <eyebrow>Final Fantasy Crystal Chronicles</eyebrow>
                <title-text>Echoes of Time</title-text>
            </hero>
            <menu-list id="menu-list" />
        </menu>
        <rom-info class="intro-item delay-5">
            <rom-status id="rom-status">
                <icon />
                <rom-status-label id="rom-status-label" />
            </rom-status>
            <rom-version id="rom-version" />
        </rom-info>
        <update-banner id="update-banner" class="intro-item delay-6">
            <update-title id="update-title" />
            <update-notes id="update-notes" />
        </update-banner>
        <version-info class="intro-item delay-6">
            <version-label>Version <version-number id="version-text"></version-number></version-label>
        </version-info>
    </content>
</body>
</rml>
)RML";

void apply_intro_animation(Rml::Element* element, const char* delay_class) {
    if (element == nullptr || delay_class == nullptr) {
        return;
    }
    element->SetClass("intro-item", true);
    element->SetClass(delay_class, true);
}

bool game_running() {
    return app().game_running && app().game_running();
}

// The ROM can be rebuilt without asking for it again when a copy of it was kept.
bool can_rebuild() {
    const auto& status = app().status;
    return status.state != setup::State::Ready && status.have_rom_copy;
}

}  // namespace

Prelaunch::Prelaunch() : Document(kDocumentSource, false, DocumentScope::Prelaunch) {
    mRoot = mDocument->GetElementById("root");

    build_menu_buttons();

    mRomStatus = mDocument->GetElementById("rom-status");
    mRomStatusLabel = mDocument->GetElementById("rom-status-label");
    mRomDetail = mDocument->GetElementById("rom-version");
    mVersion = mDocument->GetElementById("version-text");
    mUpdateTitle = mDocument->GetElementById("update-title");
    mUpdateNotes = mDocument->GetElementById("update-notes");

    listen(mDocument, Rml::EventId::Transitionend, [this](Rml::Event& event) {
        auto* target = event.GetTargetElement();
        if (target == nullptr) {
            return;
        }
        if (target == mDocument && !mDocument->HasAttribute("open")) {
            Document::hide(true);
        } else if (target->GetTagName() == "button" && !target->IsClassSet("anim-done")) {
            target->SetClass("anim-done", true);
        }
    });
}

void Prelaunch::build_menu_buttons() {
    auto* menuList = mDocument->GetElementById("menu-list");
    if (menuList == nullptr) {
        return;
    }

    auto playButton = std::make_unique<Button>(menuList, "Play");
    playButton->on_pressed([this] { play(); });
    apply_intro_animation(playButton->root(), "delay-1");
    mPlayButton = playButton.get();
    mMenuButtons.push_back(std::move(playButton));

    mMenuButtons.push_back(std::make_unique<Button>(menuList, "Settings"));
    mMenuButtons.back()->on_pressed([this] { push(std::make_unique<SettingsWindow>(true)); });
    apply_intro_animation(mMenuButtons.back()->root(), "delay-2");

    mMenuButtons.push_back(std::make_unique<Button>(menuList, "Quit"));
    mMenuButtons.back()->on_pressed([] { request_quit_confirm(); });
    apply_intro_animation(mMenuButtons.back()->root(), "delay-3");
}

// Play: starts the game, which is already running behind this screen when it is built. Without a built game it asks
// for the ROM (Select ROM), or builds again from the copy of it that was kept.
void Prelaunch::play() {
    if (rom_setup::busy()) {
        return;
    }
    if (app().status.state != setup::State::Ready) {
        if (can_rebuild()) {
            mPlayPending = true;
            rom_setup::rebuild(true);
        } else {
            rom_setup::select_rom();
        }
        return;
    }
    if (!game_running()) {
        mPlayPending = true;  // still starting
        return;
    }
    mPlayPending = false;
    play_sound(kSoundPlay);
    show_menu_notification();
    hide(true);
}

// A newer release was found on GitHub (see update_check.hpp): the notes in a panel, and a Download button that opens the
// release's page. Built when the answer arrives, which can be after the screen is up.
void Prelaunch::show_update() {
    if (mUpdateShown) {
        return;
    }
    update::Available newer;
    if (!update::available(newer)) {
        return;
    }
    mUpdateShown = true;
    set_text_content(mUpdateTitle, "Version " + newer.version + " is available");
    mUpdateNotes->SetInnerRML(escape_lines(newer.notes));
    mDocument->SetClass("has-update", true);

    auto* menuList = mDocument->GetElementById("menu-list");
    if (menuList == nullptr) {
        return;
    }
    mMenuButtons.push_back(std::make_unique<Button>(menuList, "Download"));
    mMenuButtons.back()->on_pressed([url = newer.url] {
        play_sound(kSoundClick);
        report::open_url(url);
    });
    apply_intro_animation(mMenuButtons.back()->root(), "delay-4");
}

void Prelaunch::show() {
    Document::show();
    mDocument->SetAttribute("open", "");
    mRoot->SetAttribute("open", "");
}

void Prelaunch::hide(bool close) {
    if (close) {
        if (!mEntranceAnimationStarted || !host::animations_enabled()) {
            // Close document immediately
            Document::hide(true);
        } else {
            mPendingClose = true;
        }
        mDocument->RemoveAttribute("open");
    } else {
        mRoot->RemoveAttribute("open");
    }
}

void Prelaunch::update() {
    if (top_document() == this) {
        rom_setup::update_modals(*this);
    }

    bool playAfterBuild = false;
    if (rom_setup::take_built(playAfterBuild) && playAfterBuild) {
        mPlayPending = true;
    }
    if (mPlayPending && !rom_setup::busy() && app().status.state == setup::State::Ready && game_running() &&
        top_document() == this)
    {
        play();
    }

    const bool running = game_running();
    mDocument->SetClass("rom-ready", running);

    if (!mEntranceAnimationStarted && mDocument != nullptr) {
        mDocument->SetClass("animate-in", true);
        mEntranceAnimationStarted = true;
    }

    // The first button is Select ROM until there is a game to play.
    if (mPlayButton != nullptr) {
        const bool needsRom = app().status.state != setup::State::Ready && !can_rebuild();
        mPlayButton->set_text(needsRom ? "Select ROM" : "Play");
        mPlayButton->set_disabled(rom_setup::busy());
    }
    for (const auto& button : mMenuButtons) {
        button->update();
    }

    const auto& status = app().status;
    if (mRomStatus != nullptr && mRomStatusLabel != nullptr) {
        const std::string busy = rom_setup::busy_text();
        if (!busy.empty()) {
            mRomStatus->SetAttribute("status", "verifying");
            set_text_content(mRomStatusLabel, busy);
        } else if (status.state == setup::State::Ready && running) {
            mRomStatus->SetAttribute("status", "good");
            set_text_content(mRomStatusLabel, "ROM ready.");
        } else if (status.state == setup::State::Ready) {
            mRomStatus->SetAttribute("status", "pending");
            set_text_content(mRomStatusLabel, "Starting the game.");
        } else if (status.state == setup::State::Outdated) {
            mRomStatus->SetAttribute("status", "pending");
            set_text_content(mRomStatusLabel, "Needs setting up again.");
        } else if (status.state == setup::State::Damaged) {
            mRomStatus->SetAttribute("status", "bad");
            set_text_content(mRomStatusLabel, can_rebuild() ? "Game files missing." : "ROM unavailable.");
        } else {
            mRomStatus->RemoveAttribute("status");
            set_text_content(mRomStatusLabel, "No ROM selected.");
        }
    }
    if (mRomDetail != nullptr) {
        if (status.state == setup::State::Ready || can_rebuild()) {
            set_display(mRomDetail, Rml::Style::Display::Block);
            set_text_content(mRomDetail, !status.rom_name.empty() ? status.rom_name : setup::expected_roms().front().name);
        } else {
            set_display(mRomDetail, Rml::Style::Display::None);
        }
    }
    if (mVersion != nullptr) {
        set_text_content(mVersion, report::version());
    }
    show_update();

    Document::update();
}

bool Prelaunch::focus() {
    if (mMenuButtons.empty()) {
        return false;
    }
    return mMenuButtons.front()->focus();
}

bool Prelaunch::visible() const {
    return mDocument->HasAttribute("open") && mRoot->HasAttribute("open");
}

bool Prelaunch::handle_nav_command(Rml::Event& event, NavCommand cmd) {
    int direction = 0;
    if (cmd == NavCommand::Down) {
        direction = 1;
    } else if (cmd == NavCommand::Up) {
        direction = -1;
    } else {
        return false;
    }
    auto* target = event.GetTargetElement();
    int focusedButton = -1;
    for (int i = 0; i < mMenuButtons.size(); ++i) {
        if (mMenuButtons[i]->contains(target)) {
            focusedButton = i;
            break;
        }
    }
    const auto n = static_cast<int>(mMenuButtons.size());
    int i = ((focusedButton + direction) % n + n) % n;
    while (i >= 0 && i < mMenuButtons.size()) {
        if (mMenuButtons[i]->focus()) {
            play_sound(kSoundItemFocus);
            event.StopPropagation();
            return true;
        }
        i += direction;
    }
    return false;
}

}  // namespace ui
