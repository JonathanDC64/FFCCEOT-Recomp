// Extracting the Wii cutscenes from the player's disc. The windows follow rom_setup.cpp's (Dusklight's verification modal).
#include "wii_setup.hpp"

#include "app.hpp"
#include "file_dialog.hpp"
#include "modal.hpp"
#include "ui.hpp"
#include "window.hpp"

#include "../wii_cutscenes.hpp"

#include <ntr/wii_disc.hpp>

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

namespace ui::wii_setup {
namespace {

std::string sKeyText;

struct Job {
    Job(std::string disc, ntr::WiiKey wii_key, fs::path dest_dir) : disc_name(std::move(disc)) {
        const fs::path disc_path = fs::u8path(disc_name);
        worker = std::thread([this, disc_path, wii_key, dest_dir] {
            try {
                run(disc_path, wii_key, dest_dir);
            } catch (const std::exception& e) {
                std::lock_guard lock(mutex);
                error = std::string("Unexpected error: ") + e.what();
            }
            done.store(true, std::memory_order_release);
        });
    }
    ~Job() {
        cancel.store(true);
        if (worker.joinable()) worker.join();
    }
    bool finished() const { return done.load(std::memory_order_acquire); }

    void run(const fs::path& disc_path, const ntr::WiiKey& key, const fs::path& dest_dir) {
        std::string err;
        auto disc = ntr::WiiDisc::open(disc_path, key, err);
        if (!disc) {
            std::lock_guard lock(mutex);
            error = err;
            return;
        }
        std::vector<std::pair<const ntr::WiiFile*, fs::path>> todo;
        std::uint64_t total = 0;
        for (const auto& f : disc->files()) {
            int id = 0;
            char tail[8] = {};
            if (std::sscanf(f.path.c_str(), "movie/movie%3d.%7s", &id, tail) == 2 && std::string(tail) == "mods" && id > 0) {
                todo.emplace_back(&f, dest_dir / std::format("movie{:03d}.thp", id));
                total += f.size;
            }
        }
        if (todo.empty()) {
            std::lock_guard lock(mutex);
            error = "This disc has no cutscenes (movie/movieNNN files). Is it the Wii version of Echoes of Time?";
            return;
        }
        std::error_code ec;
        fs::create_directories(dest_dir, ec);
        std::uint64_t written = 0;
        int index = 0;
        for (const auto& [file, dest] : todo) {
            {
                std::lock_guard lock(mutex);
                step = std::format("Cutscene {} of {}", ++index, todo.size());
            }
            const bool ok = disc->extract(
                *file, dest,
                [&](std::uint64_t bytes) {
                    written += bytes;
                    std::lock_guard lock(mutex);
                    fraction = total ? float(double(written) / double(total)) : 1.0f;
                    return !cancel.load();
                },
                err);
            if (!ok) {
                std::lock_guard lock(mutex);
                if (cancel.load()) cancelled = true;
                else error = err;
                return;
            }
        }
        std::lock_guard lock(mutex);
        extracted = int(todo.size());
        fraction = 1.0f;
    }

    std::string disc_name;
    std::mutex mutex;
    float fraction = 0.0f;
    std::string step = "Reading the disc";
    std::string error;
    int extracted = 0;
    bool cancelled = false;
    std::atomic<bool> cancel = false;
    std::atomic_bool done = false;
    std::thread worker;
};

std::unique_ptr<Job> sJob;
bool sJobModalPushed = false;
std::optional<std::string> sMessage;  // a window to show: what went wrong, or that it worked
bool sMessageIsError = true;

std::string u8(const fs::path& path) {
    const auto text = path.u8string();
    return {text.begin(), text.end()};
}

fs::path dest_dir() {
    return wii::movie_dir(app().data_dir);
}

void begin(const std::string& disc_path) {
    ntr::WiiKey key;
    std::string error;
    if (!ntr::parse_wii_key(sKeyText, key, error)) {
        sMessage = error;
        sMessageIsError = true;
        return;
    }
    sJob.reset();
    sJob = std::make_unique<Job>(disc_path, key, dest_dir());
    sJobModalPushed = false;
}

class JobModal : public WindowSmall {
public:
    JobModal() : WindowSmall("modal") {
        auto* header = append(mDialog, "modal-header");
        append_text(append(header, "modal-title"), "Setting up the Wii cutscenes");
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

        if (sJob != nullptr) {
            set_text_content(mFileName, "Copying the cutscenes from " + u8(fs::u8path(sJob->disc_name).filename()) +
                                            ". This takes a few minutes, and happens only once.");
        }
        refresh();
    }

    void update() override {
        if (!mFinished) {
            if (sJob == nullptr) {
                mFinished = true;
                pop();
            } else if (sJob->finished()) {
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
        if (sJob == nullptr || mCancelRequested) return;
        mCancelRequested = true;
        sJob->cancel.store(true);
        mCancelButton->set_text("Cancelling...");
        mCancelButton->set_disabled(true);
    }

    void refresh() {
        if (sJob == nullptr) return;
        float fraction;
        std::string step;
        {
            std::lock_guard lock(sJob->mutex);
            fraction = sJob->fraction;
            step = sJob->step;
        }
        fraction = std::clamp(fraction, 0.0f, 1.0f);
        mProgress->SetAttribute("value", fraction);
        set_text_content(mDetail, std::format("{} ({:.0f}%)", step, fraction * 100.0f));
    }

    void finish() {
        std::string error;
        int extracted = 0;
        bool cancelled = false;
        {
            std::lock_guard lock(sJob->mutex);
            error = sJob->error;
            extracted = sJob->extracted;
            cancelled = sJob->cancelled;
        }
        sJob.reset();
        if (!error.empty()) {
            sMessage = error;
            sMessageIsError = true;
        } else if (!cancelled) {
            sMessage = std::format("{} cutscenes are ready. They are used the next time the game starts.", extracted);
            sMessageIsError = false;
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

void push_message_modal(Document& host) {
    const std::string text = *sMessage;
    const bool isError = sMessageIsError;
    sMessage.reset();
    const auto dismiss = [](Modal& modal) { modal.pop(); };
    host.push(std::make_unique<Modal>(Modal::Props{
        .title = isError ? "The Wii cutscenes were not set up" : "Wii cutscenes ready",
        .bodyRml = escape(text),
        .actions = {ModalAction{.label = "Close", .onPressed = dismiss}},
        .onDismiss = dismiss,
        .variant = isError ? "danger" : "",
        .icon = isError ? "error" : "",
    }));
}

}  // namespace

void set_key_text(std::string text) {
    sKeyText = std::move(text);
}

const std::string& key_text() {
    return sKeyText;
}

void select_disc() {
    if (busy()) return;
    // Say so before opening the file dialog if the key is not usable.
    ntr::WiiKey key;
    std::string error;
    if (!ntr::parse_wii_key(sKeyText, key, error)) {
        sMessage = "Enter the Wii common key first (32 hexadecimal digits). " + error;
        sMessageIsError = true;
        return;
    }
    open_file_dialog({{"Wii disc image", "wbfs;iso"}, {"All files", "*"}}, [](const std::string& path) { begin(path); });
}

bool busy() {
    return sJob != nullptr;
}

int installed_count() {
    int count = 0;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dest_dir(), ec)) {
        const std::string name = e.path().filename().string();
        if (name.size() == 12 && name.compare(0, 5, "movie") == 0 && name.compare(8, 4, ".thp") == 0) count++;
    }
    return count;
}

void remove_installed() {
    if (busy()) return;
    std::error_code ec;
    fs::remove_all(app().data_dir / "wii", ec);
}

void update_modals(Document& host) {
    if (sJob != nullptr && !sJobModalPushed) {
        sJobModalPushed = true;
        host.push(std::make_unique<JobModal>());
        return;
    }
    if (sMessage.has_value()) {
        push_message_modal(host);
    }
}

void shutdown() noexcept {
    sJob.reset();
    sKeyText.clear();
}

}  // namespace ui::wii_setup
