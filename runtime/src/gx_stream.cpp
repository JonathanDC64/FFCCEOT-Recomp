#include "gx_stream.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <utility>

#include "gx_hook.hpp"
#include "runtime_internal.hpp"

namespace ntr::gx {

namespace {

using melonDS::GPU3D;

// The geometry engine's state is plain data after its two references, in two parts around vertex and polygon RAM
// (which are large, so only the parts in use are copied, by the caller): from the command FIFO up to VertexRAM, and
// from CurVertexRAM to the end. Pointers in it point into the same object, so a copy is only ever loaded back into
// the engine it came from.
std::pair<char*, size_t> head(GPU3D& g) {
    char* b = reinterpret_cast<char*>(&g.CmdFIFO);
    return {b, size_t(reinterpret_cast<char*>(&g.VertexRAM) - b)};
}
std::pair<char*, size_t> tail(GPU3D& g) {
    char* b = reinterpret_cast<char*>(&g.CurVertexRAM);
    return {b, size_t(reinterpret_cast<char*>(&g.FlushAttributes + 1) - b)};
}

void save_state(GPU3D& g, std::vector<uint8_t>& out) {
    const auto [hb, hn] = head(g);
    const auto [tb, tn] = tail(g);
    out.resize(hn + tn);
    std::memcpy(out.data(), hb, hn);
    std::memcpy(out.data() + hn, tb, tn);
}

void load_state(GPU3D& g, const std::vector<uint8_t>& in) {
    const auto [hb, hn] = head(g);
    const auto [tb, tn] = tail(g);
    std::memcpy(hb, in.data(), hn);
    std::memcpy(tb, in.data() + hn, tn);
}

// The type of a GX FIFO entry (private to GPU3D, but its FIFOs are public).
template <typename T, melonDS::u32 N>
T fifo_entry_type(const melonDS::FIFO<T, N>&);
using CmdEntry = decltype(fifo_entry_type(std::declval<GPU3D&>().CmdPIPE));

// A hash of the render list: everything the renderers read from the polygons and their vertices.
uint64_t hash_render_list(const GPU3D& g) {
    uint64_t h = 1469598103934665603ull;
    const auto mix = [&](const void* p, size_t n) {
        const auto* b = static_cast<const uint8_t*>(p);
        for (size_t i = 0; i < n; i++) {
            h ^= b[i];
            h *= 1099511628211ull;
        }
    };
    const auto val = [&](auto v) { mix(&v, sizeof v); };
    val(g.RenderNumPolygons);
    for (uint32_t i = 0; i < g.RenderNumPolygons; i++) {
        const melonDS::Polygon& p = *g.RenderPolygonRAM[i];
        val(p.NumVertices);
        mix(p.FinalZ, sizeof(int32_t) * std::min<uint32_t>(p.NumVertices, 10));
        mix(p.FinalW, sizeof(int32_t) * std::min<uint32_t>(p.NumVertices, 10));
        val(p.WBuffer), val(p.Attr), val(p.TexParam), val(p.TexPalette), val(p.Degenerate), val(p.FacingView);
        val(p.Translucent), val(p.IsShadowMask), val(p.IsShadow), val(p.Type), val(p.VTop), val(p.VBottom);
        val(p.YTop), val(p.YBottom), val(p.XTop), val(p.XBottom), val(p.SortKey);
        for (uint32_t v = 0; v < p.NumVertices && v < 10; v++) {
            const melonDS::Vertex& x = *p.Vertices[v];
            mix(x.Position, sizeof x.Position), mix(x.Color, sizeof x.Color), mix(x.TexCoords, sizeof x.TexCoords);
            val(x.Clipped);
            mix(x.FinalPosition, sizeof x.FinalPosition), mix(x.FinalColor, sizeof x.FinalColor);
            mix(x.HiresPosition, sizeof x.HiresPosition);
        }
    }
    return h;
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------------
// Recording

Streams::Streams(melonDS::GPU3D& gpu3d) : g_(gpu3d) {
    const char* obj = reinterpret_cast<const char*>(&g_);
    const auto [hb, hn] = head(g_);
    const auto [tb, tn] = tail(g_);
    if (hb < obj || hb + hn != reinterpret_cast<const char*>(&g_.VertexRAM) ||
        reinterpret_cast<const char*>(&g_.PolygonRAM) + sizeof g_.PolygonRAM > tb || tb + tn > obj + sizeof g_)
        fatal("melonDS's GPU3D layout changed: update runtime/src/gx_stream.cpp");
    melonDS::FfccGxCommandHookContext = this;
    melonDS::FfccGxCommandHook = &Streams::command_hook;
    melonDS::FfccGxVBlankHook = &Streams::vblank_hook;
}

Streams::~Streams() {
    if (restore_pending_) restore_real();
    melonDS::FfccGxCommandHook = nullptr;
    melonDS::FfccGxVBlankHook = nullptr;
    melonDS::FfccGxCommandHookContext = nullptr;
}

void Streams::command_hook(void* self, uint8_t op, uint32_t param) { static_cast<Streams*>(self)->on_command(op, param); }
void Streams::vblank_hook(void* self, bool flushed) { static_cast<Streams*>(self)->on_vblank(flushed); }

void Streams::on_command(uint8_t op, uint32_t param) {
    if (current_.entries.empty() && (check || interpolate)) save_state(g_, current_.start);
    current_.entries.push_back({op, param});
    if (op != 0x50) return;
    // (The flush itself has not run yet; everything before it has.)
    current_.num_vertices = g_.NumVertices;
    current_.num_polygons = g_.NumPolygons;
    done_.push_back(std::move(current_));
    current_ = {};
    current_.entries.reserve(done_.back().entries.size());
}

void Streams::on_vblank(bool flushed) {
    ++vblanks_;
    // The frame after an interpolated one shows the real scene, unless a new scene replaced it already.
    if (restore_pending_) {
        if (!flushed) restore_real();
        restore_pending_ = false;
    }
    if (done_.empty()) return;
    Scene scene = std::move(done_.back());
    done_.clear();
    scene.vblank = vblanks_;
    if (have_prev_) {
        if (probe) add_probe(prev_, scene);
        if (dump_at_vblank && scene.vblank >= dump_at_vblank) {
            dump(prev_, scene);
            dump_at_vblank = 0;
        }
        if (flushed && check && !scene.start.empty()) check_replay(scene);
        else if (flushed && interpolate && !scene.start.empty()) interpolate_scene(prev_, scene);
    }
    prev_ = std::move(scene);
    have_prev_ = true;
}

// ---------------------------------------------------------------------------------------------------------------
// Replay

void Streams::replay(const Scene& scene, const std::vector<Entry>& entries) {
    const auto t0 = std::chrono::steady_clock::now();
    // Keep what the replay overwrites: the engine state, and the real scene in vertex/polygon RAM (it was built in
    // the bank the engine is no longer building in).
    save_state(g_, post_);
    real_bank_ = g_.CurRAMBank ? 0 : 1;
    melonDS::Vertex* vram = &g_.VertexRAM[real_bank_ ? 6144 : 0];
    melonDS::Polygon* pram = &g_.PolygonRAM[real_bank_ ? 2048 : 0];
    real_vertices_.assign(vram, vram + std::min<uint32_t>(scene.num_vertices, 6144));
    real_polygons_.assign(pram, pram + std::min<uint32_t>(scene.num_polygons, 2048));
    real_list_ = g_.RenderPolygonRAM;
    real_count_ = g_.RenderNumPolygons;

    // Run the commands from the scene's start, without the console seeing any of it, and swap the result in as the
    // VBlank after a flush does.
    load_state(g_, scene.start);
    g_.CmdFIFO.Clear();
    g_.CmdPIPE.Clear();
    g_.CmdStallQueue.Clear();
    g_.ExecParamCount = 0;
    melonDS::FfccGxReplaying = true;
    for (const Entry& e : entries) {
        CmdEntry c{};
        c.Command = e.op;
        c.Param = e.param;
        g_.CmdPIPE.Write(c);
        g_.ExecuteCommand();
    }
    g_.VBlank();
    melonDS::FfccGxReplaying = false;
    const std::array<melonDS::Polygon*, 2048> list = g_.RenderPolygonRAM;
    const uint32_t count = g_.RenderNumPolygons;

    load_state(g_, post_);
    g_.RenderPolygonRAM = list;
    g_.RenderNumPolygons = count;
    g_.RenderFrameIdentical = false;
    restore_pending_ = true;
    replay_ms_ += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

void Streams::restore_real() {
    std::copy(real_vertices_.begin(), real_vertices_.end(), &g_.VertexRAM[real_bank_ ? 6144 : 0]);
    std::copy(real_polygons_.begin(), real_polygons_.end(), &g_.PolygonRAM[real_bank_ ? 2048 : 0]);
    g_.RenderPolygonRAM = real_list_;
    g_.RenderNumPolygons = real_count_;
    g_.RenderFrameIdentical = false;  // render it again: the last frame showed the replayed list
}

void Streams::check_replay(const Scene& scene) {
    const uint64_t real = hash_render_list(g_);
    const bool identical = g_.RenderFrameIdentical;
    replay(scene, scene.entries);
    const uint64_t replayed = hash_render_list(g_);
    restore_real();
    restore_pending_ = false;
    g_.RenderFrameIdentical = identical;
    ++checked_;
    if (replayed != real) ++check_failed_;
}

// ---------------------------------------------------------------------------------------------------------------
// Interpolation

void Streams::interpolate_scene(const Scene& prev, const Scene& scene) {
    // One frame of latency: this frame shows the time one frame before the new scene, between the two scenes. With
    // the game's usual two-frame step that is the midpoint. (A one-frame step is a catch-up after a late scene, and
    // long gaps are loading or pauses: those show the real scene.)
    const int gap = int(scene.vblank - prev.vblank);
    if (gap < 2 || gap > 4) {
        ++not_interpolated_;
        return;
    }
    std::vector<Entry> entries = scene.entries;
    const Blend b = blend(prev.entries, scene.entries, entries, gap - 1, gap);
    cuts_ += b.cut;
    snapped_ += int(b.snapped);
    if (b.blended == 0) {
        ++not_interpolated_;
        return;
    }
    replay(scene, entries);
    ++interpolated_;
}

// ---------------------------------------------------------------------------------------------------------------
// Statistics

void Streams::add_probe(const Scene& a_scene, const Scene& scene) {
    ++scenes_;
    entries_ += scene.entries.size();
    for (const Entry& e : scene.entries) {
        matrix_params_ += is_matrix_param_op(e.op);
        vertex_params_ += is_vertex_op(e.op);
    }
    const auto& a = a_scene.entries;
    const auto& b = scene.entries;
    if (same_opcodes(a_scene.entries, scene.entries)) {
        ++identical_;
        pattern_ += '=';
        uint32_t mode = 0;
        for (size_t i = 0; i < b.size(); i++) {
            if (b[i].op == 0x10) mode = b[i].param & 3;
            same_matrix_ += is_matrix_param_op(b[i].op);
            same_vertex_ += is_vertex_op(b[i].op);
            if (a[i].param == b[i].param) continue;
            if (is_matrix_param_op(b[i].op)) {
                ++diff_matrix_;
                diff_projection_ += mode == 0;
            } else if (is_vertex_op(b[i].op)) {
                ++diff_vertex_;
            } else {
                ++diff_other_;
            }
        }
    } else {
        const std::vector<Chunk> ca = chunks(a_scene.entries), cb = chunks(scene.entries);
        size_t matched = 0;
        for (const auto& [i, j] : align(ca, cb)) matched += cb[j].entries;
        chunk_entries_ += b.size();
        chunk_matched_ += matched;
        const size_t tenth = b.empty() ? 0 : std::min<size_t>(matched * 10 / b.size(), 9);
        pattern_ += char('0' + tenth);
    }
}

// One command per line, with the matrix mode and the parameters that differ from the previous scene marked.
void Streams::dump(const Scene& prev, const Scene& scene) {
    FILE* f = std::fopen("gx_scene.txt", "w");
    if (!f) return;
    const bool same = same_opcodes(prev.entries, scene.entries);
    std::fprintf(f, "# scene at VBlank %llu (previous at %llu), %zu entries, %s opcodes as the previous\n",
                 (unsigned long long)scene.vblank, (unsigned long long)prev.vblank, scene.entries.size(),
                 same ? "same" : "different");
    uint32_t mode = 0;
    for (size_t i = 0; i < scene.entries.size();) {
        const uint8_t op = scene.entries[i].op;
        if (op == 0x10) mode = scene.entries[i].param & 3;
        const size_t n = std::min<size_t>(entry_count(op), scene.entries.size() - i);
        std::fprintf(f, "%6zu m%u %02x", i, mode, op);
        for (size_t k = 0; k < n; k++) {
            const uint32_t p = scene.entries[i + k].param;
            const bool changed = same && prev.entries[i + k].param != p;
            std::fprintf(f, " %s%08x", changed ? "*" : "", p);
        }
        std::fprintf(f, "\n");
        i += n;
    }
    std::fclose(f);
    log("gx: wrote gx_scene.txt (VBlank %llu)\n", (unsigned long long)scene.vblank);
}

void Streams::report(uint64_t frame) {
    if (frame % 60) return;
    const auto pct = [](uint64_t a, uint64_t b) { return b ? 100.0 * double(a) / double(b) : 0.0; };
    if (probe) {
        const double s = scenes_ ? double(scenes_) : 1.0;
        log("gx-probe: frame %llu: scenes %d, same opcodes as the previous %d, others chunk-matched %.0f%%; per scene "
            "%.0f entries (%.0f matrix, %.0f vertex); changed params: %.0f%% matrix (%llu projection), %.0f%% vertex, "
            "%llu other  %s\n",
            (unsigned long long)frame, scenes_, identical_, pct(chunk_matched_, chunk_entries_), double(entries_) / s,
            double(matrix_params_) / s, double(vertex_params_) / s, pct(diff_matrix_, same_matrix_),
            (unsigned long long)diff_projection_, pct(diff_vertex_, same_vertex_), (unsigned long long)diff_other_,
            pattern_.c_str());
    }
    if (check)
        log("gx-check: frame %llu: replayed %d scenes, %d differ from the real polygon list, %.2f ms per replay\n",
            (unsigned long long)frame, checked_, check_failed_, checked_ ? replay_ms_ / checked_ : 0.0);
    if (interpolate && probe)
        log("gx-interp: frame %llu: interpolated %d scenes, %d not (%d camera cuts), %d matrices snapped, %.2f ms per "
            "replay\n",
            (unsigned long long)frame, interpolated_, not_interpolated_, cuts_, snapped_,
            interpolated_ ? replay_ms_ / interpolated_ : 0.0);
    pattern_.clear();
    scenes_ = identical_ = checked_ = check_failed_ = interpolated_ = not_interpolated_ = cuts_ = snapped_ = 0;
    entries_ = matrix_params_ = vertex_params_ = diff_matrix_ = diff_vertex_ = diff_other_ = diff_projection_ = 0;
    chunk_entries_ = chunk_matched_ = same_matrix_ = same_vertex_ = 0;
    replay_ms_ = 0;
}

}  // namespace ntr::gx
