// GX command streams: everything the game sends the 3D geometry engine for one scene (up to a flush). They are
// recorded to interpolate the game's 30 Hz 3D to 60 Hz (docs/ARCHITECTURE.md §5.3): on a frame where a new scene
// arrives, the scene is replayed through melonDS's geometry engine with its matrices blended towards the previous
// scene, and that polygon list is rendered instead; the next frame (which would repeat the scene) shows the real one.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "GPU3D.h"
#include "gx_match.hpp"

namespace ntr::gx {

struct Scene {
    std::vector<Entry> entries;    // up to and including the flush (0x50)
    std::vector<uint8_t> start;    // the geometry engine's state when its first command ran (see save_state)
    uint32_t num_vertices = 0, num_polygons = 0;  // what it built
    uint64_t vblank = 0;           // the VBlank that swapped it in
};

class Streams {
public:
    explicit Streams(melonDS::GPU3D& gpu3d);
    ~Streams();
    Streams(const Streams&) = delete;
    Streams& operator=(const Streams&) = delete;

    bool probe = false;        // --gx-probe: log how well consecutive scenes match
    bool check = false;        // --gx-check: replay every scene unchanged and compare the polygon lists
    bool interpolate = false;  // show a blended scene on frames where a new one arrives
    uint64_t dump_at_vblank = 0;  // debugging: write the first scene swapped in at or after this VBlank to a file

    // Logs the statistics every 60 frames.
    void report(uint64_t frame);

private:
    static void command_hook(void* self, uint8_t op, uint32_t param);
    static void vblank_hook(void* self, bool flushed);
    void on_command(uint8_t op, uint32_t param);
    void on_vblank(bool flushed);

    void add_probe(const Scene& prev, const Scene& scene);
    void dump(const Scene& prev, const Scene& scene);
    void check_replay(const Scene& scene);
    void interpolate_scene(const Scene& prev, const Scene& scene);
    // Runs `entries` from `scene`'s start state through the geometry engine and makes the result the render list,
    // keeping the rest of the engine's state as it was. restore_real() brings back the real render list.
    void replay(const Scene& scene, const std::vector<Entry>& entries);
    void restore_real();

    melonDS::GPU3D& g_;
    Scene current_, prev_;
    std::vector<Scene> done_;
    bool have_prev_ = false;
    uint64_t vblanks_ = 0;

    // Saved around a replay: the engine state, and the real scene's part of vertex and polygon RAM.
    std::vector<uint8_t> post_;
    std::vector<melonDS::Vertex> real_vertices_;
    std::vector<melonDS::Polygon> real_polygons_;
    std::array<melonDS::Polygon*, 2048> real_list_{};
    uint32_t real_count_ = 0, real_bank_ = 0;
    bool restore_pending_ = false;

    // Statistics since the last report.
    std::string pattern_;
    int scenes_ = 0, identical_ = 0;
    uint64_t entries_ = 0, matrix_params_ = 0, vertex_params_ = 0;
    uint64_t same_matrix_ = 0, same_vertex_ = 0;                    // params in identical-opcode pairs
    uint64_t diff_matrix_ = 0, diff_vertex_ = 0, diff_other_ = 0;  // of which changed
    uint64_t diff_projection_ = 0;                                  // of which in the projection matrix
    uint64_t chunk_entries_ = 0, chunk_matched_ = 0;                // in the other pairs
    int checked_ = 0, check_failed_ = 0;
    int interpolated_ = 0, not_interpolated_ = 0, cuts_ = 0;
    double replay_ms_ = 0;
};

}  // namespace ntr::gx
