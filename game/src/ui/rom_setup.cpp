// The prelaunch screen's ROM check and build windows. The flow and the windows follow Dusklight's disc image check
// (CC0, src/dusk/ui/prelaunch.cpp: DiscVerificationModal and the verification error Modals).
#include "rom_setup.hpp"

#include "app.hpp"
#include "file_dialog.hpp"
#include "modal.hpp"
#include "ui.hpp"
#include "window.hpp"

#include "../report.hpp"
#include "../setup.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

namespace fs = std::filesystem;

namespace ui::rom_setup {
namespace {

struct Check {
    explicit Check(std::string p) : path(std::move(p)) {
        worker = std::thread([this] {
            try {
                result = setup::verify_rom(fs::u8path(path));
            } catch (const std::exception& e) {
                result = {};
                result.message = std::string("The file could not be read: ") + e.what();
            }
            done.store(true, std::memory_order_release);
        });
    }
    ~Check() {
        if (worker.joinable()) worker.join();
    }
    bool finished() const { return done.load(std::memory_order_acquire); }

    std::string path;
    setup::RomCheck result;
    std::atomic_bool done = false;
    std::thread worker;
};

struct Build {
    Build(setup::Request request, std::string from) : source_name(std::move(from)) {
        worker = std::thread([this, request = std::move(request)] {
            setup::Callbacks callbacks;
            callbacks.progress = [this](float fraction, const std::string& step_text) {
                std::lock_guard lock(mutex);
                this->fraction = fraction;
                step = step_text;
            };
            callbacks.log = [this](const std::string& line) {
                std::lock_guard lock(mutex);
                log += line + "\n";
            };
            callbacks.cancel = &cancel;
            try {
                result = setup::run(request, callbacks);
            } catch (const std::exception& e) {
                result = {};
                result.error = std::string("Setup stopped unexpectedly: ") + e.what();
            }
            done.store(true, std::memory_order_release);
        });
    }
    ~Build() {
        cancel.store(true);
        if (worker.joinable()) worker.join();
    }
    bool finished() const { return done.load(std::memory_order_acquire); }

    std::string source_name;  // empty: from the kept copy
    std::mutex mutex;
    float fraction = 0.0f;
    std::string step = "Starting";
    std::string log;
    std::atomic<bool> cancel = false;
    std::atomic_bool done = false;
    setup::Result result;
    std::thread worker;
};

struct Mismatch {
    std::string path;
    setup::RomCheck check;
};

struct Failure {
    std::string message, log, source;  // source: the ROM it was built from (empty: the kept copy)
};

std::unique_ptr<Check> sCheck;
std::unique_ptr<Build> sBuild;
bool sCheckModalPushed = false;
bool sBuildModalPushed = false;
std::optional<Mismatch> sMismatch;
std::optional<Failure> sFailure;
bool sPlayWhenBuilt = false;
bool sBuilt = false;

void begin_check(std::string path) {
    sCheck.reset();
    sCheck = std::make_unique<Check>(std::move(path));
    sCheckModalPushed = false;
}

void begin_build(const std::string& source) {
    setup::Request request;
    request.data_dir = app().data_dir;
    request.rom_source = source.empty() ? fs::path{} : fs::u8path(source);
    request.toolchain_dir = app().toolchain_dir;
    sBuild.reset();
    sBuild = std::make_unique<Build>(std::move(request), source);
    sBuildModalPushed = false;
}

std::string u8(const fs::path& path) {
    const auto text = path.u8string();
    return {text.begin(), text.end()};
}

std::string file_name(const std::string& path) {
    return u8(fs::u8path(path).filename());
}

std::string size_text(uint64_t bytes) {
    if (bytes >= (1ull << 30)) return std::format("{:.2f} GiB", double(bytes) / double(1ull << 30));
    return std::format("{:.0f} MiB", double(bytes) / double(1ull << 20));
}

// Zips the setup's messages and the machine's description, and opens a filled-in issue form.
void report_failure(const Failure& failure) {
    const std::string system = report::redact(report::system_info());
    std::vector<report::File> files;
    files.push_back({"system.txt", report::bytes(system)});
    files.push_back({"setup.log", report::bytes(report::redact(failure.log + "\nerror: " + failure.message + "\n"))});
    const fs::path zip = report::write_bundle("setup-" + report::timestamp(), files);
    std::string body = "## What happened\nSetting up the game from my ROM failed.\n\n## Error\n```\n" +
                       report::redact(failure.message.substr(0, 600)) + "\n```\n\n## System\n```\n" + system +
                       "```\n\n## Attachments\n";
    body += zip.empty() ? "(The report files could not be saved.)\n"
                        : "Drag `" + u8(zip.filename()) + "` (its folder is open) into this issue: it has the setup's log.\n";
    report::open_url(report::issue_url("Setup failed", body));
    if (!zip.empty()) report::reveal(zip);
}

// Checking the ROM: the file name and what is being done, with a Cancel button (Dusklight's DiscVerificationModal).
class RomCheckModal : public WindowSmall {
public:
    RomCheckModal() : WindowSmall("modal") {
        auto* header = append(mDialog, "modal-header");
        append_text(append(header, "modal-title"), "Checking your ROM");
        append(header, "icon")->SetClass("verifying", true);

        auto* body = append(mDialog, "modal-body");
        auto* content = append(body, "verification-progress");
        mFileName = append(content, "file-path");
        mProgress = append(content, "progress");
        mProgress->SetClass("info", true);
        mProgress->SetAttribute("value", 0.f);
        mDetail = append(content, "small");
        set_text_content(mDetail, "Reading the file and working out its SHA-1 and CRC-32...");

        auto* actions = append(mDialog, "modal-actions");
        mCancelButton = std::make_unique<Button>(actions, "Cancel");
        mCancelButton->root()->SetClass("modal-btn", true);
        mCancelButton->on_pressed([this] { cancel(); });
        if (sCheck != nullptr) {
            set_text_content(mFileName, file_name(sCheck->path));
        }
    }

    void update() override {
        if (!mFinished) {
            if (sCheck == nullptr) {
                mFinished = true;
                pop();
            } else if (sCheck->finished()) {
                mFinished = true;
                finish();
            }
        }
        WindowSmall::update();
    }

    bool focus() override { return mCancelButton != nullptr && mCancelButton->focus(); }

protected:
    bool handle_nav_command(Rml::Event& event, NavCommand cmd) override {
        if (cmd == NavCommand::Cancel || cmd == NavCommand::Menu) {
            cancel();
            event.StopPropagation();
            return true;
        }
        if (cmd == NavCommand::Left || cmd == NavCommand::Right) {
            return true;
        }
        return false;
    }

private:
    void cancel() {
        if (mFinished) return;
        mFinished = true;
        sCheck.reset();  // joins: the check is a second or two at most
        pop();
    }

    void finish() {
        const std::string path = sCheck->path;
        const setup::RomCheck result = sCheck->result;
        sCheck.reset();
        if (result.ok) {
            begin_build(path);
        } else {
            sMismatch = Mismatch{path, result};
        }
        pop();
    }

    Rml::Element* mFileName = nullptr;
    Rml::Element* mProgress = nullptr;
    Rml::Element* mDetail = nullptr;
    std::unique_ptr<Button> mCancelButton;
    bool mFinished = false;
};

// Building the game: a bar and the current step, with a Cancel button.
class BuildModal : public WindowSmall {
public:
    BuildModal() : WindowSmall("modal") {
        auto* header = append(mDialog, "modal-header");
        append_text(append(header, "modal-title"),
            sBuild != nullptr && sBuild->source_name.empty() ? "Setting up again" : "Setting up the game");
        append(header, "icon")->SetClass("verifying", true);

        auto* body = append(mDialog, "modal-body");
        auto* content = append(body, "verification-progress");
        mFileName = append(content, "file-path");
        mProgress = append(content, "progress");
        mProgress->SetClass("info", true);
        mProgress->SetAttribute("value", 0.f);
        mDetail = append(content, "small");

        auto* actions = append(mDialog, "modal-actions");
        mCancelButton = std::make_unique<Button>(actions, "Cancel");
        mCancelButton->root()->SetClass("modal-btn", true);
        mCancelButton->on_pressed([this] { cancel(); });

        if (sBuild != nullptr) {
            set_text_content(mFileName,
                sBuild->source_name.empty() ?
                    std::string("Building the game again from your ROM. This takes about a minute.") :
                    "Building it from " + file_name(sBuild->source_name) +
                        ". This takes about a minute, and happens only once.");
        }
        refresh();
    }

    void update() override {
        if (!mFinished) {
            if (sBuild == nullptr) {
                mFinished = true;
                pop();
            } else if (sBuild->finished()) {
                mFinished = true;
                finish();
            } else {
                refresh();
            }
        }
        WindowSmall::update();
    }

    bool focus() override { return mCancelButton != nullptr && mCancelButton->focus(); }

protected:
    bool handle_nav_command(Rml::Event& event, NavCommand cmd) override {
        if (cmd == NavCommand::Cancel || cmd == NavCommand::Menu) {
            cancel();
            event.StopPropagation();
            return true;
        }
        if (cmd == NavCommand::Left || cmd == NavCommand::Right) {
            return true;
        }
        return false;
    }

private:
    void cancel() {
        if (sBuild == nullptr || mCancelRequested) return;
        mCancelRequested = true;
        sBuild->cancel.store(true);
        mCancelButton->set_text("Cancelling...");
        mCancelButton->set_disabled(true);
    }

    void refresh() {
        if (sBuild == nullptr) return;
        float fraction;
        std::string step;
        {
            std::lock_guard lock(sBuild->mutex);
            fraction = sBuild->fraction;
            step = sBuild->step;
        }
        fraction = std::clamp(fraction, 0.0f, 1.0f);
        mProgress->SetAttribute("value", fraction);
        set_text_content(mDetail, std::format("{} ({:.0f}%)", step, fraction * 100.0f));
    }

    void finish() {
        setup::Result result = sBuild->result;
        const std::string log = [&] {
            std::lock_guard lock(sBuild->mutex);
            return sBuild->log;
        }();
        const std::string source = sBuild->source_name;
        sBuild.reset();
        if (result.ok) {
            sBuilt = true;
            if (app().start_game) app().start_game();
        } else if (!result.cancelled) {
            sFailure = Failure{
                result.error.empty() ? std::string("Setup did not finish.") : result.error, log, source};
            sPlayWhenBuilt = false;
        } else {
            sPlayWhenBuilt = false;
        }
        pop();
    }

    Rml::Element* mFileName = nullptr;
    Rml::Element* mProgress = nullptr;
    Rml::Element* mDetail = nullptr;
    std::unique_ptr<Button> mCancelButton;
    bool mCancelRequested = false;
    bool mFinished = false;
};

void push_mismatch_modal(Document& host) {
    const Mismatch mismatch = *sMismatch;
    sMismatch.reset();
    const auto expected = setup::expected_rom();
    const bool isDs = !mismatch.check.sha1.empty();
    Rml::String body = escape(mismatch.check.message.empty() ?
                                  std::string("This is not the ROM this version builds the game from.") :
                                  mismatch.check.message);
    body += "<br/><br/>Expected: <b>" + escape(expected.name) + "</b><br/>SHA-1 " + escape(expected.sha1) +
            "<br/>CRC-32 " + escape(expected.crc32);
    body += "<br/><br/>Your file (" + escape(file_name(mismatch.path)) + ", " + size_text(mismatch.check.size) +
            "):<br/>SHA-1 " + escape(isDs ? mismatch.check.sha1 : std::string("(not a Nintendo DS game)")) +
            "<br/>CRC-32 " + escape(mismatch.check.crc32.empty() ? std::string("-") : mismatch.check.crc32);
    const auto dismiss = [](Modal& modal) { modal.pop(); };
    host.push(std::make_unique<Modal>(Modal::Props{
        .title = "This is not the right ROM",
        .bodyRml = body,
        .actions =
            {
                ModalAction{.label = "Close", .onPressed = dismiss},
                ModalAction{
                    .label = "Choose another",
                    .onPressed =
                        [](Modal& modal) {
                            modal.pop();
                            select_rom();
                        },
                },
            },
        .onDismiss = dismiss,
        .variant = "danger",
        .icon = "warning",
    }));
}

void push_failure_modal(Document& host) {
    const Failure failure = *sFailure;
    sFailure.reset();
    const auto dismiss = [](Modal& modal) { modal.pop(); };
    host.push(std::make_unique<Modal>(Modal::Props{
        .title = "Setup did not finish",
        .bodyRml = escape(failure.message),
        .actions =
            {
                ModalAction{.label = "Close", .onPressed = dismiss},
                ModalAction{
                    .label = "Report",
                    .onPressed = [failure](Modal&) { report_failure(failure); },
                },
                ModalAction{
                    .label = "Retry",
                    .onPressed =
                        [failure](Modal& modal) {
                            modal.pop();
                            begin_build(failure.source);
                        },
                },
            },
        .onDismiss = dismiss,
        .variant = "danger",
        .icon = "error",
    }));
}

}  // namespace

void select_rom() {
    if (busy()) return;
    open_file_dialog({{"Nintendo DS ROM", "nds"}, {"All files", "*"}}, [](const std::string& path) { use_rom(path); });
}

void use_rom(const std::string& path) {
    if (busy() || path.empty()) return;
    sFailure.reset();
    sMismatch.reset();
    sPlayWhenBuilt = false;
    begin_check(path);
}

void rebuild(bool play) {
    if (busy()) return;
    sFailure.reset();
    sMismatch.reset();
    sPlayWhenBuilt = play;
    begin_build({});
}

bool busy() {
    return sCheck != nullptr || sBuild != nullptr;
}

std::string busy_text() {
    if (sCheck != nullptr) return "Checking the ROM.";
    if (sBuild != nullptr) return "Building the game.";
    return {};
}

void update_modals(Document& host) {
    if (sCheck != nullptr && !sCheckModalPushed) {
        sCheckModalPushed = true;
        host.push(std::make_unique<RomCheckModal>());
        return;
    }
    if (sMismatch.has_value()) {
        push_mismatch_modal(host);
        return;
    }
    if (sBuild != nullptr && !sBuildModalPushed) {
        sBuildModalPushed = true;
        host.push(std::make_unique<BuildModal>());
        return;
    }
    if (sFailure.has_value()) {
        push_failure_modal(host);
    }
}

bool take_built(bool& play) {
    if (!sBuilt) return false;
    sBuilt = false;
    play = sPlayWhenBuilt;
    sPlayWhenBuilt = false;
    return true;
}

void shutdown() noexcept {
    sCheck.reset();
    sBuild.reset();
}

}  // namespace ui::rom_setup
