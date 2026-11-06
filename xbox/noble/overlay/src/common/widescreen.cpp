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
// HUD at the edges (hud_edges): the HUD's draws, from the first of the frame that takes one of its textures (kHud: the
// sheet of the health bar's frame, digits and icons, the bar's fill, the icon at the bottom left) to the frame pass, go
// to the screen's left or right edge, rather than the 4:3 middle's: as on a 4:3 screen, the same way from the screen's
// corner. They go by the piece of the HUD they are part of, the draws that touch one another (the health bar's frame,
// its fill, its digits that reach past the middle of the frame; a message's box at the bottom right and its text),
// taken from the frame before: a piece at least twice as far from one side of the frame as from the other goes to
// that side's screen edge, one about the middle stays, and so does one with none of the HUD's textures. Before the
// HUD in the frame are the world's 2D effects (glows, sparks), drawn at the points they light, which stay; so does any
// 2D draw placed at a point of the world (its vertices carry the point's depth, w, where the HUD's have 1), whatever
// its texture: the lock-on marker is drawn from the HUD's sheet over the enemy locked on, and went to the screen's edge
// with the HUD once the enemy was off the middle. Nothing moves while the pause menu or a message box is up, or in a
// story scene.
//
// Skies past the fog: the game builds its skies from painted strips, a moon and clouds placed far out in the world,
// past a room's far plane where its fog has hidden everything (the rampart of 5.7: a ring of sky strips up to 62000
// out, the moon at 45000, with fog from 14000 to 39000 and the far plane at 39000). With the fog and the far plane
// pushed out (game/view.cpp) they show as the cards they are, their edges and overlaps never meant to be seen. The
// draws of a sky's textures (kSky) get the room's own fog back (the coefficients in c91, as the game set them) and
// are not drawn where wholly past the room's own far plane; everything else is drawn as far as set. A texture is
// listed by hand: level geometry lies past a room's far plane too, and only the sky's cards are held back.
// CV_SKY_TRACE logs the candidates, CV_SKY_HIDE hides them, CV_BACKDROPS=0 draws the skies pushed out too.
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
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "app/app.h"
#include "core/log.h"
#include "gpu/ext.h"

uint64_t TexturePackReplace(const gpu::TextureDesc& d, const uint8_t* guest, const uint32_t* palette);  // texpack.cpp
extern uint32_t g_button_layout;
extern bool g_hd_movies;
void MoviesFrame(uint64_t presents);
uint64_t MoviesReplace(const gpu::TextureDesc&, const uint8_t*);
bool MoviesOwn(uint64_t id);
std::shared_ptr<const gpu::Replacement> MoviesReplaced(uint64_t id);
bool MoviesScale(const gpu::TextureDesc&, float squeeze, float qx, float qy, float& sx, float& sy);
extern bool g_hud_edges;  // menu.cpp
bool PauseMenuUp();       // telemetry.cpp
bool ViewBackdrop(float game[2], float drawn[2], float& far_z, uint32_t& room);  // game/view.cpp
bool ModalUp();           // telemetry.cpp
uint32_t SceneWord();     // telemetry.cpp
std::shared_ptr<const gpu::Replacement> TexturePackReplaced(uint64_t id);                              // texpack.cpp

namespace {

// for tests (telemetry.cpp: "wide"), as last drawn: the lock-on marker (the HUD's sheet at a point of the world) and
// the HUD (its sheet's last draw of a frame, the health bar's), each on the console frame, its move across in clip
// space and the frame it was drawn in
struct Seen2D {
    uint64_t frame = 0;
    float x0 = 0, x1 = 0, move = 0;
};
std::mutex g_seen_m;
Seen2D g_marker, g_hud;
uint64_t g_frame = 0;

// hardware constant order: D3D register + 96
constexpr uint32_t kCamera = 68 + 96;       // view screen matrix
constexpr uint32_t kCameraWorld = 85 + 96;  // world view screen matrix
constexpr uint32_t kScreenScale = 94 + 96;  // PS2 screen to console pixels
constexpr uint32_t kScreenOffset = 95 + 96;
constexpr uint32_t kFog = 91 + 96;          // the fog's coefficients
// the game's skies by their textures' Name: painted cards it placed past a room's far plane, in its fog
constexpr uint64_t kSky[] = {
    0xa13869efdd70cb49ull,  // 5.7, the Abandoned Castle's rampart: its ring of sky strips
    0x8713e8116bb8057bull,  // its moon
    0x2d6da7cc8094311cull,  // its clouds
};
constexpr int kFrameTexCoord = 2;  // the frame pass's texture coordinates, in texels
constexpr int kTexCoord = 2;       // a 2D draw's, as the game's programs take them
// the HUD's textures by their Name (CV_WIDE_TRACE shows them): its sheet, the health bar's fill, the icon at the
// bottom left
constexpr uint64_t kHud[] = {0x3ff08a280ddd7171ull, 0x9d83fce80f5f78f6ull, 0x8d0b7015b1ee08baull};

struct Program {
    bool camera = false;  // projects with the game's camera
    bool screen = false;  // takes PS2 screen coordinates: x * c94.x / w + c95.x
    bool world = false;   // with the world matrix too (c85)
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
    // ext.xy scale across and down, ext.zw then move, in clip space (the frame pass's placing, the HUD's move to the
    // screen's edge); ext.x negative: a vertex on the console frame's left or right edge goes out to the screen's
    // instead
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
        if (p.camera) {
            Distance(s, a, p);
            return;
        }
        // on the console frame
        uint32_t decode = a.decode & 0xF;
        auto vertex_w = [decode](const float* v, uint32_t comps) {
            return comps == 4 && decode != gpu::kDecodeW1 ? v[3] : decode == gpu::kDecodeFloat2H ? v[2] : 1.0f;
        };
        auto screen = [&](int axis) {
            return Read(a, 0, [&](const float* v, uint32_t comps) {
                if (!p.screen) return v[axis];
                float w = vertex_w(v, comps);
                return v[axis] * s.vsc[kScreenScale][axis] * (w != 0 ? 1 / w : 1) + s.vsc[kScreenOffset][axis];
            });
        };
        d_.x = screen(0);
        d_.y = screen(1);
        // a point of the world's: its vertices carry the point's depth (the HUD's have w 1)
        Span pw = Read(a, 0, vertex_w);
        d_.at_point = !pw.Empty() && (pw.lo < 0.999f || pw.hi > 1.001f);
        const Span& x = d_.x;
        const Span& y = d_.y;
        float gw = float(s.viewport.w ? s.viewport.w : 640), gh = float(s.viewport.h ? s.viewport.h : 480);
        d_.gw = uint32_t(gw);
        d_.gh = uint32_t(gh);
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
            MoviesFrame(frame_);
            squeeze_ = (4.0f / 3.0f) / app::WideAspect();
            world_ = 0;
            picture_ = false;
            hud_ = false;
            modal_ = ModalUp();
            hud_free_ = g_hud_edges && !PauseMenuUp() && !(SceneWord() & 0x20000);
            HudPieces();
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
            auto it = t ? names_.end() : names_.find(d.data);
            d_.name = it != names_.end() ? it->second : 0;
        }
        d_.sampled = true;
    }

    uint32_t ReplaceEpoch() override { return (g_button_layout & 1) | (g_hd_movies ? 2 : 0); }

    // HD texture packs (texpack.cpp)
    uint64_t Replace(const gpu::TextureDesc& d, const uint8_t* guest, const uint32_t* palette) override {
        if (guest) names_[d.data] = Name(d, guest);
        if (uint64_t id = MoviesReplace(d, guest)) return id;
        return TexturePackReplace(d, guest, palette);
    }
    std::shared_ptr<const gpu::Replacement> Replaced(uint64_t id) override { return MoviesOwn(id) ? MoviesReplaced(id) : TexturePackReplaced(id); }

    void Constants(float (*vsc)[4], float* ext, const gpu::Target*, uint32_t src_blend) override {
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
        // the HUD, from its first draw to the frame pass, to the screen's edges
        float shift = 0;
        if (back_buffer_ && d_.target && d_.whole) hud_ = false;
        if (squeeze && hud_free_ && !d_.world && !d_.whole && !d_.target && !edges && !d_.at_point) {
            bool named = std::find(std::begin(kHud), std::end(kHud), d_.name) != std::end(kHud);
            hud_ |= named;
            bool inside = !d_.x.Empty() && !d_.y.Empty() && d_.x.lo >= -2 && d_.x.hi <= float(d_.gw) + 2;
            if (hud_ && inside && !BossBar(named)) shift = HudShift({d_.x.lo, d_.x.hi, d_.y.lo, d_.y.hi, named, 0}, d_.count == 4);
        }
        // the frame pass, or a picture of the frame's size drawn the way it is (the European release's intro movie),
        // each texel on a pixel. The picture squeezed has the columns it reads past its texture at its sides, on the
        // screen: left out.
        bool placed = FrameExtent();
        float across = squeeze ? squeeze_ : 1.0f;
        ext[0] = across * (placed ? d_.scale[0] : 1.0f) * (edges ? -1.0f : 1.0f);
        ext[1] = (placed ? d_.scale[1] : 1.0f) * (placed && squeeze && d_.tex.linear ? -1.0f : 1.0f);
        ext[2] = placed ? across * d_.move[0] : shift;
        ext[3] = placed ? d_.move[1] : 0.0f;
        // A decoded 640x480 movie frame can fill or fit the wide output.
        float mx = 1, my = 1;
        if (back_buffer_ && d_.whole && d_.sampled && !d_.target &&
            MoviesScale(d_.tex, across, 1.0f, 1.0f, mx, my)) {
            ext[0] = across * mx;
            ext[1] = my;
            ext[2] = ext[3] = 0;
        }
        if (Backdrop(vsc) || SkyCard()) ext[0] = ext[1] = ext[2] = ext[3] = 0;  // not drawn: every vertex at the centre
        if (back_buffer_ && d_.name == kHud[0] && !d_.x.Empty()) {
            std::lock_guard lock(g_seen_m);
            (d_.at_point ? g_marker : g_hud) = {frame_, d_.x.lo, d_.x.hi, ext[2]};
            g_frame = frame_;
        }
        // CV_WIDE_TRACE=<n>: every draw of a frame in n, the target it goes into ("device": the back buffer)
        if (kTrace && frame_ % kTrace == 0 && d_.world)
            LOGR("wide %llu: into %08x%s 3D, n %u", (unsigned long long)frame_, trace_rt_, trace_device_ ? " device" : "",
                 d_.count);
        else if (kTrace && frame_ % kTrace == 0 && d_.squeeze)
            LOGR("wide %llu: into %08x%s n %u x %.0f..%.0f y %.0f..%.0f tex %ux%u at %08x %016llx u %g..%g v %g..%g%s "
                 "blend %u: %s%s%s%s%s",
                 (unsigned long long)frame_, trace_rt_, trace_device_ ? " device" : "", d_.count, d_.x.lo, d_.x.hi,
                 d_.y.lo, d_.y.hi, d_.tex.width, d_.tex.height, d_.tex.data, (unsigned long long)d_.name, d_.u.lo,
                 d_.u.hi, d_.v.lo, d_.v.hi, d_.target ? " target" : "", src_blend, d_.whole ? "whole, " : "",
                 edges ? "edges out" : shift < 0 ? "hud left" : shift > 0 ? "hud right" : squeeze ? "squeezed" : "kept",
                 placed ? ", placed" : "", d_.at_point ? ", at a point" : "", d_.direct_copy ? ", direct copy" : "");
    }

private:
    // a HUD draw on the console frame, and where it went: a part of the frame's width
    struct Rect {
        float x0, x1, y0, y1;
        bool named;  // with one of the HUD's textures
        float shift;
    };
    static bool Touch(const Rect& a, const Rect& b) {
        constexpr float kGap = 2;
        return a.x0 <= b.x1 + kGap && b.x0 <= a.x1 + kGap && a.y0 <= b.y1 + kGap && b.y0 <= a.y1 + kGap;
    }
    // a piece of the HUD at least twice as far from one side of the frame as from the other, to that side
    float Side(const Rect& r) const {
        float left = std::max(r.x0, 0.0f), right = std::max(gw_ - r.x1, 0.0f);
        return left * 2 < right ? squeeze_ - 1 : right * 2 < left ? 1 - squeeze_ : 0.0f;
    }

    // at a frame's start: the frame before's HUD draws, gathered into the pieces they touch one another in
    void HudPieces() {
        pieces_.clear();
        size_t n = rects_.size();
        std::vector<size_t> up(n);
        for (size_t i = 0; i < n; i++) up[i] = i;
        auto root = [&](size_t i) {
            while (up[i] != i) i = up[i] = up[up[i]];
            return i;
        };
        for (size_t i = 0; i < n; i++)
            for (size_t j = i + 1; j < n; j++)
                if (Touch(rects_[i], rects_[j])) up[root(i)] = root(j);
        std::vector<int> piece(n, -1);
        for (size_t i = 0; i < n; i++) {
            size_t r = root(i);
            if (piece[r] < 0) {
                piece[r] = int(pieces_.size());
                pieces_.push_back(rects_[i]);
            }
            Rect& p = pieces_[size_t(piece[r])];
            p.x0 = std::min(p.x0, rects_[i].x0);
            p.x1 = std::max(p.x1, rects_[i].x1);
            p.y0 = std::min(p.y0, rects_[i].y0);
            p.y1 = std::max(p.y1, rects_[i].y1);
            p.named |= rects_[i].named;
        }
        for (Rect& p : pieces_) p.shift = p.named ? Side(p) : 0.0f;
        rects_.clear();
        if (modal_) pieces_ = known_;
        else known_ = pieces_;
    }

    // a HUD draw's move: as the piece of the frame before it is in, else as a draw of this frame it touches, else a
    // quad of the HUD's own textures by itself (a piece's first frame)
    float HudShift(Rect r, bool quad) {
        gw_ = d_.gw;
        r.shift = 0;
        bool found = false;
        for (const Rect& p : pieces_)
            if (Touch(p, r)) {
                r.shift = p.shift;
                found = true;
                break;
            }
        for (size_t i = 0; !found && i < rects_.size(); i++)
            if (Touch(rects_[i], r)) {
                r.shift = rects_[i].shift;
                found = true;
            }
        if (!found && r.named && quad && !modal_) r.shift = Side(r);
        if (rects_.size() < 256) rects_.push_back(r);
        return r.shift;
    }

    // The boss label identifies the HUD sheet row that must stay centered.
    bool BossBar(bool named) {
        if (!named) return false;
        const float w = d_.x.hi - d_.x.lo, h = d_.y.hi - d_.y.lo;
        if (d_.name == kHud[0] && !d_.u.Empty() && std::fabs(d_.u.lo - 0.8125f) < 0.004f &&
            d_.u.hi > 0.996f && w > 50 && w < 70 && h > 13 && h < 22 && d_.y.lo > d_.gh * 0.6f) {
            boss_row_ = {d_.x.lo, d_.x.hi, d_.y.lo, d_.y.hi, true, 0};
            boss_frame_ = frame_;
            return true;
        }
        return boss_frame_ != ~0ull && frame_ - boss_frame_ <= 1 &&
               d_.y.lo >= boss_row_.y0 - 2 && d_.y.hi <= boss_row_.y1 + 2 && d_.x.lo >= boss_row_.x0 - 2;
    }

    // a texture by its first bytes (its indices), size and format: the same picture wherever the game loads it
    static uint64_t Name(const gpu::TextureDesc& d, const uint8_t* guest) {
        uint32_t n = std::min<uint32_t>(gpu::LevelSize(d, 0), 16384);
        uint64_t shape[2] = {uint64_t(d.width) << 32 | d.height, d.format};
        return gpu::Hash(guest, n, gpu::Hash(shape, sizeof(shape)));
    }

    // a 3D draw's depth through the camera, while the view is pushed out (Backdrop)
    void Distance(const gpu::State& s, const gpu::Assembled& a, const Program& p) {
        uint32_t room = 0;
        backdrop_ = kBackdrops && ViewBackdrop(fog_game_, fog_drawn_, far_z_, room);
        if (!backdrop_) return;
        if (room != room_) {
            SkyCandidates();
            room_ = room;
            room_far_ = far_z_;
            room_frame_ = frame_;
        }
        uint32_t m = p.world ? kCameraWorld : kCamera;
        const auto& c = s.vsc[m + 3];
        uint32_t ahead = 0, past = 0;
        d_.w = Read(a, 0, [&](const float* v, uint32_t comps) {
            float w = v[0] * c[0] + v[1] * c[1] + v[2] * c[2] + (comps == 4 ? v[3] : 1.0f) * c[3];
            ahead += w > 0;
            past += w > far_z_;
            return w;
        });
        d_.past = ahead ? float(past) / float(ahead) : 0.0f;
    }

    // CV_SKY_TRACE: the textures of a room none of whose draws came nearer than mostly past its far plane, the
    // candidates for kSky, logged as the next room starts
    void SkyCandidates() {
        if (kSkyTrace && room_)
            for (const auto& [name, seen] : seen_)
                if (seen.min_past >= 0.3f && !Sky(name))
                    LOGR("wide: sky candidate in room %u (from frame %llu, far %.0f): %016llx, %u draws, all %.0f%% or "
                         "more past it, out to %.0f",
                         room_, (unsigned long long)room_frame_, room_far_, (unsigned long long)name, seen.draws,
                         seen.min_past * 100, seen.max_w);
        seen_.clear();
    }

    static bool Sky(uint64_t name) { return std::find(std::begin(kSky), std::end(kSky), name) != std::end(kSky); }

    bool SkyCard() {
        // The rampart's moon glows are normally beyond the room's far plane.
        if (!d_.world || d_.name != 0xc26555aa0ffd586dull) return false;
        float game[2], drawn[2], far_z = 0;
        uint32_t room = 0;
        return kBackdrops && ViewBackdrop(game, drawn, far_z, room);
    }

    // A sky's draw (kSky) gets the room's own fog, and one wholly past the room's own far plane is not drawn (true):
    // the sky is as the game shows it, and the rest of the room has the fog and far plane as set.
    bool Backdrop(float (*vsc)[4]) {
        if (!backdrop_ || !vsc || !d_.world || d_.w.Empty() || !d_.name) return false;
        if (kSkyTrace && d_.past > 0) {
            Seen& seen = seen_[d_.name];
            seen.draws++;
            seen.min_past = std::min(seen.min_past, d_.past);
            seen.max_w = std::max(seen.max_w, d_.w.hi);
        } else if (kSkyTrace) {
            seen_[d_.name].min_past = 0;
        }
        // CV_SKY_HIDE: every candidate's draw hidden, to see what they are
        if (kSkyHide) return d_.past >= 0.3f;
        if (!Sky(d_.name)) return false;
        auto same = [](float a, float b) { return b != 0 && std::fabs(a - b) <= 1e-4f * std::fabs(b); };
        float* fog = vsc[kFog];
        // c91 holds them over 128 (the PS2's fog units): (A / 128, B / 128, 0, 0)
        for (int i = 0; i < 4; i++)
            for (int k = 0; k < 2; k++)
                if (same(fog[i], fog_drawn_[k] / 128)) {
                    fog[i] = fog_game_[k] / 128;
                    break;
                }
        return d_.w.lo > far_z_;
    }

    Program Classify(const gpu::VertexShader& vs) {
        uint64_t key = gpu::Hash(vs.program.data(), vs.program.size() * 4);
        auto it = programs_.find(key);
        if (it != programs_.end()) return it->second;
        Program p;
        for (uint32_t r = 0; r < 4; r++)
            p.camera |= gpu::ReadsConstant(vs.program, kCamera + r) || gpu::ReadsConstant(vs.program, kCameraWorld + r);
        p.world = gpu::ReadsConstant(vs.program, kCameraWorld);
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
        uint32_t gw = 640, gh = 480;  // the console frame's size
        uint64_t name = 0;  // the first texture's (Name), 0 for a target or one not seen uploaded
        Span w;             // a 3D draw's depth, while the view is pushed out
        float past = 0;     // the part of its vertices ahead past the room's own far plane
        uint32_t count = 0;
        Span x, y, u, v;  // on the console frame; the first texture coordinate, for one quad on it (v: whole)
        bool at_point = false;  // a 2D draw placed at a point of the world, by that point's depth
        gpu::TextureDesc tex;
    };
    Draw d_;
    float squeeze_ = 0.75f;
    bool back_buffer_ = false;
    // the frame so far: vertices of 3D drawn (the map screen's shapes are some 150, the world's thousands), a 4:3
    // picture over the whole frame
    static constexpr uint32_t kWorld = 1000;
    uint64_t frame_ = ~0ull;
    uint32_t trace_rt_ = 0;  // the target's address, for the trace
    bool trace_device_ = false;
    uint32_t world_ = 0;
    bool picture_ = false;
    bool hud_ = false;       // the frame's HUD has begun
    bool modal_ = false;
    std::vector<Rect> known_;
    Rect boss_row_{};
    uint64_t boss_frame_ = ~0ull;
    bool hud_free_ = false;  // the HUD may go to the edges this frame
    std::vector<Rect> rects_, pieces_;  // this frame's HUD draws; the pieces of the frame before's
    float gw_ = 640;
    // backdrops (Backdrop): the room's own fog coefficients and far plane, the fog drawn, the backdrops' textures
    bool backdrop_ = false;
    float fog_game_[2] = {}, fog_drawn_[2] = {}, far_z_ = 0;
    uint32_t room_ = 0;
    float room_far_ = 0;
    uint64_t room_frame_ = 0;
    struct Seen {
        uint32_t draws = 0;
        float min_past = 1, max_w = 0;
    };
    std::unordered_map<uint64_t, Seen> seen_;  // CV_SKY_TRACE: each texture's draws in the room
    static inline const bool kBackdrops = !std::getenv("CV_BACKDROPS") || std::atoi(std::getenv("CV_BACKDROPS")) != 0;
    static inline const bool kSkyTrace = std::getenv("CV_SKY_TRACE") != nullptr;
    static inline const bool kSkyHide = std::getenv("CV_SKY_HIDE") != nullptr;
    Hooks hooks_;
    std::unordered_map<uint64_t, Program> programs_;
    std::unordered_map<uint32_t, uint64_t> names_;  // each texture's Name, by its guest address, as last uploaded
};

}  // namespace

// "wide" for telemetry.cpp: {"marker": {"x": [lo, hi], "move": m, "ago": frames since drawn}, "hud": {...}}, each
// once drawn
std::string WideJson() {
    std::lock_guard lock(g_seen_m);
    std::string out;
    for (auto [name, seen] : {std::pair{"marker", &g_marker}, std::pair{"hud", &g_hud}}) {
        if (!seen->frame) continue;
        char b[160];
        std::snprintf(b, sizeof(b), "%s\"%s\":{\"x\":[%.1f,%.1f],\"move\":%.4f,\"ago\":%llu}",
                      out.empty() ? "" : ",", name, seen->x0, seen->x1, seen->move,
                      (unsigned long long)(g_frame - seen->frame));
        out += b;
    }
    return out.empty() ? out : "{" + out + "}";
}

// GameInfo::init: before the renderer is made
void WidescreenInit() { gpu::SetExtension(std::make_unique<Widescreen>()); }
