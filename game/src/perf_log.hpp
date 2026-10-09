#pragma once

// Testing: the machine-readable logs of a run (docs/TESTING.md).
//   --perf-log <file.csv>   one row per emulated frame (kind E) and one per picture presented (kind P)
//   --frame-hash <file>     one line per emulated frame: its number, a hash of the emulated memory, a hash of the picture
// tools/scenario.py reads both.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <filesystem>

class PerfLog {
public:
    // Rows before `from_frame` (the boot, say) are left out.
    bool open(const std::filesystem::path& path, std::uint64_t from_frame) {
        f_ = std::fopen(path.string().c_str(), "w");
        if (!f_) return false;
        from_ = from_frame;
        start_ = std::chrono::steady_clock::now();
        std::fprintf(f_, "frame,kind,k,t_ms,emu_ms,present_ms,interp_ms,gpu_ms,late_ms,steps\n");
        return true;
    }
    ~PerfLog() {
        if (!f_) return;
        write(true);
        std::fclose(f_);
    }
    bool is_open() const { return f_ != nullptr; }

    // An emulated frame: the host time its emulation took, the instructions the interpreter ran, and how late the pacing
    // let the frame start (paced runs).
    void emulated(std::uint64_t frame, double emu_ms, std::uint64_t steps, double late_ms) {
        Row r;
        r.frame = frame;
        r.kind = 'E';
        r.emu_ms = emu_ms;
        r.late_ms = late_ms;
        r.steps = steps;
        add(r);
    }
    // A picture presented for it (the k-th of the emulated frame: display-rate 3D presents several). present_ms is the
    // time present() took, which includes the wait for the refresh with vsync; interp_ms the time to blend the 3D;
    // late_ms how late the picture came against the refresh (or the timer) it was meant for. `serial` ties the GPU time
    // that arrives later (gpu()) to the row.
    void presented(std::uint64_t frame, int k, double present_ms, double interp_ms, double late_ms, std::uint64_t serial) {
        Row r;
        r.frame = frame;
        r.kind = 'P';
        r.k = k;
        r.present_ms = present_ms;
        r.interp_ms = interp_ms;
        r.late_ms = late_ms;
        r.serial = serial;
        add(r);
    }
    void gpu(std::uint64_t serial, double ms) {
        for (auto it = rows_.rbegin(); it != rows_.rend(); ++it)
            if (it->serial == serial) {
                it->gpu_ms = ms;
                return;
            }
    }

private:
    struct Row {
        std::uint64_t frame = 0, steps = 0, serial = 0;
        char kind = 'E';
        int k = 0;
        double t_ms = 0, emu_ms = -1, present_ms = -1, interp_ms = -1, gpu_ms = -1, late_ms = -1;
    };

    void add(Row r) {
        if (!f_ || r.frame < from_) return;
        r.t_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start_).count();
        rows_.push_back(r);
        write(false);
    }
    static void number(std::FILE* f, double v) {
        if (v < 0)
            std::fputc(',', f);
        else
            std::fprintf(f, "%.3f,", v);
    }
    // Rows leave a few at a time, late enough for their GPU time to have come in.
    void write(bool all) {
        const std::size_t keep = all ? 0 : 12;
        while (rows_.size() > keep) {
            const Row& r = rows_.front();
            std::fprintf(f_, "%llu,%c,%d,%.3f,", (unsigned long long)r.frame, r.kind, r.k, r.t_ms);
            number(f_, r.emu_ms);
            number(f_, r.present_ms);
            number(f_, r.interp_ms);
            number(f_, r.gpu_ms);
            if (r.late_ms < 0) std::fputc(',', f_);
            else std::fprintf(f_, "%.3f,", r.late_ms);
            std::fprintf(f_, "%llu\n", (unsigned long long)r.steps);
            rows_.pop_front();
        }
        std::fflush(f_);
    }

    std::FILE* f_ = nullptr;
    std::uint64_t from_ = 0;
    std::chrono::steady_clock::time_point start_{};
    std::deque<Row> rows_;
};

class HashLog {
public:
    bool open(const std::filesystem::path& path) {
        f_ = std::fopen(path.string().c_str(), "w");
        if (f_) std::fprintf(f_, "# frame state_hash picture_hash\n");
        return f_ != nullptr;
    }
    ~HashLog() {
        if (f_) std::fclose(f_);
    }
    bool is_open() const { return f_ != nullptr; }
    void add(std::uint64_t frame, std::uint64_t state, std::uint64_t picture) {
        if (!f_) return;
        std::fprintf(f_, "%llu %016llx %016llx\n", (unsigned long long)frame, (unsigned long long)state,
                     (unsigned long long)picture);
        std::fflush(f_);
    }

private:
    std::FILE* f_ = nullptr;
};
