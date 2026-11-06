// Widescreen, and the frame pass's overscan, through a renderer extension.
//
// Widescreen: on a 16:9 output the console frame is shown 16:9, and every draw into the back buffer is squeezed to
// 3/4 of its width around the centre, so nothing is stretched. The game projects with a PS2 style view screen matrix
// (c68..c71, or c85..c88 with the world matrix in it) into PS2 screen space, which c94 and c95 then scale to console
// pixels. Squeezing a projected draw is the same as narrowing the camera's focal length, so the 3D view shows more
// at the sides (Hor+). 2D draws (HUD, menus, text, backdrops, movies) keep their 4:3 shape in the middle. Draws over
// the whole width that are no picture of their own stay whole: fades and tints, panels whose texture does not change
// across them (a message's dimmed panel), and passes that draw the frame itself back (which already holds the
// squeezed scene).
//
// Backdrops: the menus, the map and the save and load screens draw theirs in pieces, none over the whole width. The
// pieces at the frame's left and right edges have those edges pulled out to the screen's, so the backdrop reaches
// the sides, its outer pieces that much wider. Only for a backdrop of its own, drawn before the 3D world and any 4:3
// picture of the frame: the world's effects touch the edges too, and the title's logo is in pieces at the edges over
// its castle.
//
// Overscan: the last pass of each frame draws a copy of the frame back over it with rows -16 to 496 of the copy
// across rows 0 to 480, the PS2's 448 lines and their border shrunk into the NTSC frame; the European release's rows
// -16 to 490, and its columns -20 to 660 across the 640, the picture narrowed by 640/680. A TV's overscan hid the
// rows and columns at the edges, which read past the copy and smear its first and last ones into streaks, and the
// copy's first row and column, which the game's 2D draws leave out (on the PS2's grid, a pixel right and down: a fade
// misses them). The pass is drawn where its texels are instead, the copy's pixel at the frame's a pixel up and left of
// it, so the copy fills the frame pixel for pixel, as square as the scene was drawn.
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <unordered_map>

#include "app/app.h"
#include "core/log.h"
#include "gpu/ext.h"

uint64_t TexturePackReplace(const gpu::TextureDesc& d, const uint8_t* guest, const uint32_t* palette);  // texpack.cpp
std::shared_ptr<const gpu::Replacement> TexturePackReplaced(uint64_t id);                              // texpack.cpp

namespace {

constexpr float kSqueeze = (4.0f / 3.0f) / (16.0f / 9.0f);

// hardware constant order: D3D register + 96
constexpr uint32_t kCamera = 68 + 96;       // view screen matrix
constexpr uint32_t kCameraWorld = 85 + 96;  // world view screen matrix
constexpr uint32_t kScreenScale = 94 + 96;  // PS2 screen to console pixels
constexpr uint32_t kScreenOffset = 95 + 96;
constexpr int kFrameTexCoord = 2;  // the frame pass's texture coordinates, in texels
constexpr int kTexCoord = 2;       // a 2D draw's, as the game's programs take them

struct Program {
    bool camera = false;  // projects with the game's camera
    bool screen = false;  // takes PS2 screen coordinates: x * c94.x / w + c95.x
};

struct Span {
    float lo = 1e30f, hi = -1e30f;
    void Add(float v) {
        lo = std::fmin(lo, v);
        hi = std::fmax(hi, v);
    }
    bool Empty() const { return lo > hi; }
};

class Hooks : public gpu::ShaderHooks {
public:
    // ext.xy scale across and down, ext.zw then move, in clip space; ext.x negative: a vertex on the console frame's
    // left or right edge goes out to the screen's instead
    std::string Position() const override {
        return "    o.pos.x = ext.x < 0 && abs(abs(o.pos.x / o.pos.w) - 1) < 0.006 ? o.pos.x\n"
               "            : o.pos.x * abs(ext.x) + ext.z * o.pos.w;\n"
               "    o.pos.y = o.pos.y * abs(ext.y) + ext.w * o.pos.w;\n";
    }
    // ext.y negative: only the texture's own columns are drawn (stage 0's, in texels: a linear texture), not its edge
    // ones repeated past them
    std::string Output() const override {
        return gpu::ShaderHooks::Output() + "if (ext.y < 0 && (tc0.x < 0 || tc0.x > texsize[0].x)) discard;\n";
    }
};

class Widescreen : public gpu::Extension {
public:
    const gpu::ShaderHooks* Shaders() const override { return &hooks_; }

    void BeginDraw(const gpu::State&) override { d_ = {}; }

    void Vertices(const gpu::State& s, const gpu::Assembled& a) override {
        d_.squeeze = true;
        bool prog = (s.vs_handle & 1) && s.vs && !s.vs->program.empty();
        d_.textured = s.textures[0] != 0;
        d_.count = a.vertex_count;
        // fixed function transform: a camera of its own
        d_.world = !prog && !a.pretransformed;
        if (d_.world) return;
        Program p = prog ? Classify(*s.vs) : Program{};
        d_.world = p.camera;
        if (p.camera) return;
        // on the console frame
        auto screen = [&](int axis) {
            return Read(a, 0, [&](const float* v, uint32_t comps) {
                if (!p.screen) return v[axis];
                uint32_t decode = a.decode & 0xF;
                float w = comps == 4 && decode != gpu::kDecodeW1 ? v[3] : decode == gpu::kDecodeFloat2H ? v[2] : 1.0f;
                return v[axis] * s.vsc[kScreenScale][axis] * (w != 0 ? 1 / w : 1) + s.vsc[kScreenOffset][axis];
            });
        };
        d_.x = screen(0);
        d_.y = screen(1);
        const Span& x = d_.x;
        const Span& y = d_.y;
        float gw = float(s.viewport.w ? s.viewport.w : 640), gh = float(s.viewport.h ? s.viewport.h : 480);
        d_.whole = !x.Empty() && x.lo <= 1 && x.hi >= gw - 1;
        bool inside = !x.Empty() && !y.Empty() && x.lo >= -2 && x.hi <= gw + 2 && y.lo >= -2 && y.hi <= gh + 2;
        // a backdrop's piece at the frame's left or right edge (on the PS2's grid, a pixel to the right), no speck
        d_.edge = inside && !d_.whole && x.hi - x.lo >= 64 &&
                  (std::fabs(x.lo - 0.5f) <= 1.5f || std::fabs(x.hi - gw - 0.5f) <= 1.5f);
        // a quad on the frame: across it, how far its texture coordinates go
        if (inside && a.vertex_count == 4) d_.u = Read(a, kTexCoord, [](const float* v, uint32_t) { return v[0]; });
        // the frame pass: screen positions, its copy's rows read over a taller span than it covers (a picture of the
        // frame's size drawn as it is). Each corner goes to its texel a pixel up and left, in clip space (y up there).
        if (prog && !p.screen && d_.whole && a.vertex_count == 4) {
            Span u = Read(a, kFrameTexCoord, [](const float* v, uint32_t) { return v[0]; });
            Span t = d_.v = Read(a, kFrameTexCoord, [](const float* v, uint32_t) { return v[1]; });
            if (!u.Empty() && !t.Empty() && x.hi > x.lo && y.hi > y.lo && t.lo < y.lo - 0.5f && t.hi > y.hi + 0.5f) {
                d_.frame = true;
                d_.gw = uint32_t(gw);
                d_.gh = uint32_t(gh);
                d_.scale[0] = (u.hi - u.lo) / (x.hi - x.lo);
                d_.scale[1] = (t.hi - t.lo) / (y.hi - y.lo);
                d_.move[0] = d_.scale[0] - 1 + 2 * (u.lo - d_.scale[0] * x.lo - 1) / gw;
                d_.move[1] = 1 - d_.scale[1] - 2 * (t.lo - d_.scale[1] * y.lo - 1) / gh;
            }
        }
    }

    bool Bound(gpu::Context& c, const gpu::State& s, gpu::Target* rt, gpu::Target*, bool) override {
        uint32_t w = 0, h = 0;
        c.GuestSize(w, h);
        // the device's own: the game also composes in a target the size of it and copies pieces out (the name card
        // of an area: CopyRects from 640x480 to its texture), which a squeeze there would cut at the left
        back_buffer_ = rt && s.rt_back_buffer && rt->gw == w && rt->gh == h;
        trace_rt_ = rt ? rt->data : 0;
        trace_device_ = s.rt_back_buffer;
        if (c.Presents() != frame_) {  // the frame's first draw
            frame_ = c.Presents();
            world_ = 0;
            picture_ = false;
        }
        return false;
    }

    void Sampled(const gpu::TextureDesc& d, const gpu::Target* t, uint8_t, gpu::TexInfo& info) override {
        // a target the GPU drew: the frame, or a pass over it. One whose picture the CPU wrote is a picture (the
        // European release's intro decodes every other frame of its movie into a target of the game's).
        d_.target |= t != nullptr && !t->cpu;
        if (!d_.sampled) {
            d_.tex = d;
            // The full frame has a known screen-to-texture mapping. CPU uploads
            // (including the PAL intro movie) still need interpolated coordinates.
            d_.direct_copy = FrameExtent() && d_.textured && t && t->drawn && !t->cpu &&
                             !t->depth && d.linear && info.kind == gpu::kTex2D &&
                             t->gw == d_.gw && t->gh == d_.gh &&
                             d_.x.lo >= -1 && d_.x.hi <= d_.gw + 1 &&
                             d_.y.lo >= -1 && d_.y.lo <= 1 &&
                             d_.y.hi >= d_.gh - 1 && d_.y.hi <= d_.gh + 1;
            if (d_.direct_copy) info.flags |= gpu::kTexFrameCopy;
        }
        d_.sampled = true;
    }

    // HD texture packs (texpack.cpp)
    uint64_t Replace(const gpu::TextureDesc& d, const uint8_t* guest, const uint32_t* palette) override {
        return TexturePackReplace(d, guest, palette);
    }
    std::shared_ptr<const gpu::Replacement> Replaced(uint64_t id) override { return TexturePackReplaced(id); }

    void Constants(float (*)[4], float* ext, const gpu::Target*, uint32_t src_blend) override {
        bool squeeze = d_.squeeze && back_buffer_ && app::Widescreen();
        // whole width: an untextured fade or tint, a panel whose texture does not change across it (a few texels
        // stretched over the frame: a message's dimmed panel), or the frame drawn back over itself
        float texels = (d_.u.hi - d_.u.lo) * (d_.tex.linear ? 1.0f : float(d_.tex.width));
        bool flat = !d_.textured || (d_.sampled && !d_.u.Empty() && texels <= 4);
        if (d_.whole && (flat || d_.target)) squeeze = false;
        // a backdrop's edge pieces reach the screen's edges on a screen of their own: not over the 3D world or a
        // 4:3 picture drawn before them in the frame
        bool edges = squeeze && d_.edge && !d_.target && world_ < kWorld && !picture_;
        if (back_buffer_) {
            world_ += d_.world ? d_.count : 0;
            picture_ |= squeeze && d_.whole && d_.textured && !d_.target;
        }
        // the frame pass, or a picture of the frame's size drawn the way it is (the European release's intro movie),
        // each texel on a pixel. The picture squeezed has the columns it reads past its texture at its sides, on the
        // screen: left out.
        bool placed = FrameExtent();
        float across = squeeze ? kSqueeze : 1.0f;
        ext[0] = across * (placed ? d_.scale[0] : 1.0f) * (edges ? -1.0f : 1.0f);
        ext[1] = (placed ? d_.scale[1] : 1.0f) * (placed && squeeze && d_.tex.linear ? -1.0f : 1.0f);
        ext[2] = placed ? across * d_.move[0] : 0.0f;
        ext[3] = placed ? d_.move[1] : 0.0f;
        // CV_WIDE_TRACE=<n>: every draw of a frame in n, the target it goes into ("device": the back buffer)
        if (kTrace && frame_ % kTrace == 0 && d_.world)
            LOGR("wide %llu: into %08x%s 3D, n %u", (unsigned long long)frame_, trace_rt_, trace_device_ ? " device" : "",
                 d_.count);
        else if (kTrace && frame_ % kTrace == 0 && d_.squeeze)
            LOGR("wide %llu: into %08x%s n %u x %.0f..%.0f y %.0f..%.0f tex %ux%u at %08x u %g..%g v %g..%g%s blend %u: "
                 "%s%s%s%s",
                 (unsigned long long)frame_, trace_rt_, trace_device_ ? " device" : "", d_.count, d_.x.lo, d_.x.hi,
                 d_.y.lo, d_.y.hi, d_.tex.width, d_.tex.height, d_.tex.data, d_.u.lo, d_.u.hi, d_.v.lo, d_.v.hi,
                 d_.target ? " target" : "", src_blend, d_.whole ? "whole, " : "",
                 edges ? "edges out" : squeeze ? "squeezed" : "kept", placed ? ", placed" : "",
                 d_.direct_copy ? ", direct copy" : "");
    }

private:
    Program Classify(const gpu::VertexShader& vs) {
        uint64_t key = gpu::Hash(vs.program.data(), vs.program.size() * 4);
        auto it = programs_.find(key);
        if (it != programs_.end()) return it->second;
        Program p;
        for (uint32_t r = 0; r < 4; r++)
            p.camera |= gpu::ReadsConstant(vs.program, kCamera + r) || gpu::ReadsConstant(vs.program, kCameraWorld + r);
        p.screen = gpu::ReadsConstant(vs.program, kScreenScale) && gpu::ReadsConstant(vs.program, kScreenOffset);
        programs_.emplace(key, p);
        return p;
    }

    // f(element, components) over the draw's vertices of input register reg, when it is 2 to 4 floats
    template <typename F>
    static Span Read(const gpu::Assembled& a, int reg, F f) {
        Span span;
        const gpu::VertexElement& e = a.elements[size_t(reg)];
        if (!(a.regs >> reg & 1) || e.buffer >= a.buffer_count) return span;
        const gpu::VertexBuffer& b = a.buffers[e.buffer];
        uint32_t comps = e.format == DXGI_FORMAT_R32G32B32A32_FLOAT ? 4
                         : e.format == DXGI_FORMAT_R32G32B32_FLOAT  ? 3
                         : e.format == DXGI_FORMAT_R32G32_FLOAT     ? 2
                                                                    : 0;
        if (!comps || !b.src) return span;
        for (uint32_t v = 0; v < a.vertex_count; v++) {
            uint64_t at = uint64_t(v) * b.stride + e.offset;
            if (at + comps * 4 > b.bytes) break;
            span.Add(f(reinterpret_cast<const float*>(b.src + at), comps));
        }
        return span;
    }

    static inline const uint64_t kTrace =
        std::getenv("CV_WIDE_TRACE") ? std::max(1, std::atoi(std::getenv("CV_WIDE_TRACE"))) : 0;

    // the draw at hand
    bool FrameExtent() const {
        return back_buffer_ && d_.frame && d_.tex.width == d_.gw && d_.tex.height == d_.gh;
    }

    struct Draw {
        bool squeeze = false, world = false, whole = false, edge = false, textured = false, target = false;
        bool sampled = false, direct_copy = false;
        bool frame = false;  // the frame pass, its scale and move across and down in clip space, for a picture of
        float scale[2] = {1, 1}, move[2] = {0, 0};  // gw by gh
        uint32_t gw = 0, gh = 0;
        uint32_t count = 0;
        Span x, y, u, v;  // on the console frame; the first texture coordinate, for one quad on it (v: whole)
        gpu::TextureDesc tex;
    };
    Draw d_;
    bool back_buffer_ = false;
    // the frame so far: vertices of 3D drawn (the map screen's shapes are some 150, the world's thousands), a 4:3
    // picture over the whole frame
    static constexpr uint32_t kWorld = 1000;
    uint64_t frame_ = ~0ull;
    uint32_t trace_rt_ = 0;  // the target's address, for the trace
    bool trace_device_ = false;
    uint32_t world_ = 0;
    bool picture_ = false;
    Hooks hooks_;
    std::unordered_map<uint64_t, Program> programs_;
};

}  // namespace

// GameInfo::init: before the renderer is made
void WidescreenInit() { gpu::SetExtension(std::make_unique<Widescreen>()); }
