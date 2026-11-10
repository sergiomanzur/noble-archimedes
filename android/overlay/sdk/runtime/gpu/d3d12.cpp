// The title's Direct3D 8 calls drawn with Direct3D 12
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <ctime>
#include <memory>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <imgui.h>
#include <backends/imgui_impl_dx12.h>
#ifdef __ANDROID__
#include "noble_android.h"
#endif
#ifdef _WIN32
#include <backends/imgui_impl_win32.h>
#else
// Linux: D3D12 is vkd3d's on Vulkan, the swap chain and ImGui's input SDL's (linux/)
#include "linux/dlss_vk.h"
#include "linux/fsr_vk.h"
#include "linux/vk_images.h"
#include "linux/present.h"
#include "linux/sdl_app.h"
#endif

#include "app/app.h"
#include "core/diag.h"
#include "core/input.h"
#include "core/log.h"
#include "gpu/backend.h"
#include "gpu/ext.h"
#include "gpu/framegen.h"
#include "gpu/pipeline_cache.h"
#include "gpu/raytrace.h"
#include "gpu/shaders.h"
#include "gpu/texture.h"
#include "gpu/texturedumper.h"
#include "gpu/vertex.h"
#include "kernel/kernel.h"

using Microsoft::WRL::ComPtr;

// the first n times, at the report level (LOG_FIRST's are informational, left out of the log by default)
#define LOGR_FIRST(n, ...)                                                       \
    do {                                                                         \
        static std::atomic<int> _c{0};                                           \
        if (_c.fetch_add(1, std::memory_order_relaxed) < (n)) LOGR(__VA_ARGS__); \
    } while (0)

namespace gpu {

namespace {

constexpr int kFrames = 3;
constexpr uint64_t kStaleFrames = 8;  // frames a target may go unused and still be read back
constexpr uint64_t kUploadSize = 64ull << 20;
// a draw starting with less than this left in the frame's ring waits for the GPU and starts the ring over
constexpr uint64_t kUploadReserve = 8ull << 20;
// Adreno's descriptor limits are much smaller than a desktop GPU's. Virtual heaps take ordinary
// descriptor sets; 3 * 8192 keeps every per-frame ring comfortably within mobile limits.
constexpr uint32_t kSrvCount = 3 * 8192;
constexpr uint32_t kMaterialViews = 6;  // a draw's view table: four stages, a material's two maps
constexpr uint32_t kSamplerCount = 2048;
constexpr uint32_t kRtvCount = 1024;
constexpr uint32_t kDsvCount = 256;
constexpr DXGI_FORMAT kBgra = DXGI_FORMAT_B8G8R8A8_UNORM;
constexpr DXGI_FORMAT kHdr = DXGI_FORMAT_R16G16B16A16_FLOAT;  // HDR output: scRGB, linear, 1.0 is 80 nits
// HDR output for a frame generation that takes no scRGB (DLSS-G): HDR10, the same light encoded with PQ in BT.2020
constexpr DXGI_FORMAT kHdr10 = DXGI_FORMAT_R10G10B10A2_UNORM;
constexpr float kScrgbNits = 80;
// HDR: below this in linear light the picture is as SDR shows it at scene brightness; above it the brightest parts rise
// smoothly to the peak at full white
constexpr float kHdrKnee = 0.4f;
constexpr uint32_t kUiRtv = kRtvCount + kFrames;  // the UI layer's view, after the back buffers'
constexpr uint32_t kFgRtv = kUiRtv + 1;  // frame generation's depth, motion and HUD-less copies, after it
// the 3D draws' own motion (PrevPositionHlsl, MotionOut), after the HUD-less frames
constexpr uint32_t kFgObjRtv = kFgRtv + 2 + kFrames;
constexpr uint32_t kFgRtvCount = 3 + kFrames;
constexpr uint32_t kAaRtv = kFgRtv + kFgRtvCount;  // anti-aliasing's depth and motion at the frame's size, after them
constexpr uint32_t kAaRxRtv = kAaRtv + 2;            // its reactive mask (AntiAlias::Frame::reactive)
constexpr uint32_t kAoRtv = kAaRxRtv + 1;            // ambient occlusion's (AmbientOcclusion)
constexpr uint32_t kGiRtv = kAoRtv + 1;              // ray traced bounce lighting's (Lighting)
constexpr uint32_t kGuideRtv = kGiRtv + 1;           // ray tracing's four guides (Guides), last
constexpr uint32_t kGuides = 4;
// after them their two surfaces (this frame's and the frame before's) and the eye's hits (rt_guide.hlsl's sixth target:
// what the eye's ray through each pixel met, which the bounce lighting and ambient occlusion take rather than tracing it
// again)
constexpr uint32_t kGuideExtra = 3;
constexpr uint32_t kGuideHit = kGuides + 2;
// the bounce lighting's own denoiser's (Lighting): its history, moments and surfaces twice over (this frame's and the
// frame before's) and its smoothing's two, last
constexpr uint32_t kDnRtv = kGuideRtv + kGuides + kGuideExtra;  // after the guides, their two surfaces and the hits
constexpr uint32_t kDn = 8;
// the reflections' and ambient occlusion's own denoiser's (Reflections, AmbientOcclusion), last: the reflections alone,
// their history and frames twice over, their smoothing's; the ambient occlusion's history twice over
constexpr uint32_t kRdRtv = kDnRtv + kDn;
constexpr uint32_t kRd = 8;
// the bounce lighting's room average (Lighting, rt_gimean.hlsl), last: its four levels, each 1/8 of the one before,
// and its eased value twice over (this frame's and the frame before's)
constexpr uint32_t kMeanRtv = kRdRtv + kRd;
constexpr uint32_t kMean = 9;  // the room's average bounce's levels and eased pair; the sky's (SkyLight), held pair
// the light shafts' (Shafts, rt_shafts.hlsl), last: this frame's march, its gathered twice over
constexpr uint32_t kShaftRtv = kMeanRtv + kMean;
constexpr uint32_t kShafts = 3;
// the lights' points kept for each bounce texel from frame to frame (Lighting, rt_gi.hlsl: ReSTIR's reuse over time),
// this frame's and the frame before's, by the frame's number, last
constexpr uint32_t kRsRtv = kShaftRtv + kShafts;
constexpr uint32_t kRs = 2;
// the glows remembered off screen (Lighting, rt_glowmem.hlsl): the listed lights' summary, and the slots twice over
// (this frame's and the frame before's, by the frame's number), last
constexpr uint32_t kGlowRtv = kRsRtv + kRs;
constexpr uint32_t kGlow = 5;           // the glows known frame to frame (GlowMemory): the summaries' pair (0, 4), the slots' pair (1, 2), their first pass (3)
constexpr uint32_t kGlowSlots = 128;    // the slots (as many as the lights listed: each light steady has one)
constexpr uint32_t kMaxLights = 128;  // the lights the bounce lighting and the light shafts sample (rt_gi.hlsl)
constexpr uint32_t kTaaRtv = kGlowRtv + kGlow;   // the own TAA's two histories (TaaFrame), last
// the motion target's clear and MotionOut's value where a pixel has no motion of its own: the camera's from depth
constexpr float kNoMotion = 1024;
constexpr uint32_t kQueries = 8192;  // occlusion queries in flight, several frames of counted draws
constexpr uint32_t kNoQuery = ~0u;

// perf diagnostic: where a frame's time goes, logged once a second with diag::Perf
using PerfClock = std::chrono::steady_clock;
double PerfMs(PerfClock::time_point t) {
    return std::chrono::duration<double, std::milli>(PerfClock::now() - t).count();
}
struct Perf {
    // milliseconds
    double draw = 0;
    double assemble = 0;
    double bind = 0;
    double sig = 0;
    double texup = 0;
    double compile = 0;
    double pso = 0;
    double clear = 0;
    double copy = 0;
    double present = 0;
    double wait = 0;
    double swap = 0;
    double sync = 0;
    // counts and bytes
    uint64_t draws = 0;
    uint64_t verts = 0;
    uint64_t upload = 0;
    uint64_t overflow = 0;
    uint64_t sigs = 0;
    uint64_t sig_bytes = 0;
    uint64_t texups = 0;
    uint64_t texup_bytes = 0;
    uint64_t newtex = 0;
    uint64_t targets = 0;
    uint64_t rtups = 0;
    uint64_t shrinks = 0;
    uint64_t syncs = 0;
    uint64_t psos = 0;
    uint64_t shaders = 0;
    uint64_t shaders_kept = 0;  // read from the disk cache instead
    uint64_t raw = 0;
    uint64_t lates = 0;
    // the GPU's milliseconds by part of the frame (GpuMark), summed over gpu_frames, and the whole's
    double gpu[10] = {};  // the GPU parts (GpuMarkIndex), their sum last
    uint64_t gpu_frames = 0;
    // a draw's steps on the render thread (Draw, DrawLap): targets and the extension's first look, shaders and their
    // keys, the pipeline (with the anti-aliasing's passes as the UI starts), constants, views and samplers, geometry
    double lap[6] = {};
    // UploadConstants' steps: the frame before's constants found, the vertex constants uploaded, the extension's
    // edits, kept for the next frame, the rest
    double clap[5] = {};
    uint64_t barriers = 0, pso_sets = 0;  // resource barriers and pipeline changes recorded
    // texture packs' pictures made into textures (an extension's replacements, the pack's own images) and their
    // milliseconds; ray tracing's scene built on the CPU (its buffers, the draws' table, the build recorded)
    double repl = 0, rt = 0;
    uint64_t repls = 0;
    // copies of a target made for a draw that reads what it draws over (Feedback), and their size
    uint64_t feedbacks = 0, feedback_bytes = 0;
};
Perf g_perf;
// Draw's time to the next step, into g_perf.lap[i]
struct DrawLap {
    bool on = diag::Perf();
    PerfClock::time_point t = on ? PerfClock::now() : PerfClock::time_point{};
    void operator()(int i) { Into(g_perf.lap, i); }
    void Into(double* acc, int i) {
        if (!on) return;
        auto n = PerfClock::now();
        acc[i] += std::chrono::duration<double, std::milli>(n - t).count();
        t = n;
    }
};
// XBE_PERF's GPU timestamps: the start of each part of the frame. The parts: its 3D (and anything before the
// anti-aliasing), anti-aliasing's motion and depth, ambient occlusion, the vendor's pass, then the UI and output
// the frame's GPU parts (XBE_PERF): its 3D; motion and depth; ray tracing's scene and guides, ambient occlusion, bounce
// lighting, reflections and water, light shafts; the anti-aliasing; what is after it
enum GpuMarkIndex : uint32_t {
    kGpuStart, kGpuAa, kGpuAo, kGpuRtScene, kGpuRtAo, kGpuRtGi, kGpuRtRefl, kGpuVendor, kGpuAfter, kGpuEnd, kGpuMarks
};
constexpr uint32_t kGpuParts = kGpuMarks - 1;
struct PerfScope {
    double& acc;
    bool on = diag::Perf();
    PerfClock::time_point t = on ? PerfClock::now() : PerfClock::time_point{};
    ~PerfScope() {
        if (on) acc += PerfMs(t);
    }
};

// F3 captures only with diag::CaptureDir set
bool CaptureOn() {
    if (diag::CaptureDir().empty()) LOG_FIRST(1, "capture: set XBE_CAPTURE_DIR");
    return !diag::CaptureDir().empty();
}

const char* kBlitHlsl = R"(
cbuffer B : register(b1) { float4 uvrect; float4 opts; };
Texture2D img : register(t0);
Texture2D lut : register(t1);
SamplerState ls : register(s0);
SamplerState ps : register(s1);
struct O { float4 p : SV_Position; float2 uv : TEXCOORD0; };
O vs(uint id : SV_VertexID) {
    O o;
    float2 t = float2((id << 1) & 2, id & 2);
    o.p = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = uvrect.xy + t * uvrect.zw;
    return o;
}
float4 psmain(O i) : SV_Target {
    float4 c = 0;
    int n = max((int)opts.z, 1);
    float2 px = fwidth(i.uv);
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) c += img.SampleLevel(ls, i.uv + px * ((float2(x, y) + 0.5) / n - 0.5), 0);
    c /= n * n;
    c.rgb *= opts.w;
    if (opts.x > 0) c.rgb = float3(lut.Sample(ps, float2(c.r, 0.5)).r, lut.Sample(ps, float2(c.g, 0.5)).g, lut.Sample(ps, float2(c.b, 0.5)).b);
    return float4(c.rgb, opts.y > 0 ? c.a : 1);
}
)";

// the frame and the UI as HDR: hdr.x scene white in scRGB units, hdr.y the peak over scene white, hdr.z kHdrKnee;
// outp.x set, written as HDR10 rather than scRGB
const char* kHdrHlsl = R"(
cbuffer B : register(b1) { float4 uvrect; float4 opts; float4 hdr; float4 outp; };
Texture2D img : register(t0);
Texture2D lut : register(t1);
Texture2D before : register(t2);  // the frame as it was before its UI, when hdr.w > 0
Texture2D level : register(t3);   // the frame's average, one texel
SamplerState ls : register(s0);
SamplerState ps : register(s1);
struct O { float4 p : SV_Position; float2 uv : TEXCOORD0; };
O vs(uint id : SV_VertexID) {
    O o;
    float2 t = float2((id << 1) & 2, id & 2);
    o.p = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = uvrect.xy + t * uvrect.zw;
    return o;
}
// the picture in linear light, as a TV of the game's day showed it: gamma 2.2
float3 Linear(float3 c) { return pow(saturate(c), 2.2); }
// scRGB light as the swap chain takes it: as it is, or for HDR10 in BT.2020's primaries and PQ's curve of nits
float3 Out(float3 s) {
    if (outp.x <= 0) return s;
    const float3x3 to2020 = {0.627404, 0.329283, 0.043313, 0.069097, 0.919540, 0.011362, 0.016391, 0.088013, 0.895595};
    float3 n = saturate(mul(to2020, s) * (80.0 / 10000.0));
    float3 p = pow(n, 0.1593017578125);
    return pow((0.8359375 + 18.8515625 * p) / (1 + 18.6875 * p), 78.84375);
}
// the game's frame: scaled by its brightest channel, so a colour keeps its hue as it brightens. Where the UI changed
// the frame it stays at scene white; a bright frame overall (a flash, a white screen) keeps its highlights nearer it
float4 scene(O i) : SV_Target {
    float3 c = img.SampleLevel(ls, i.uv, 0).rgb * opts.w;
    float peak = hdr.y;
    if (hdr.w > 0) {
        float3 d = abs(c - before.SampleLevel(ls, i.uv, 0).rgb * opts.w);
        peak = lerp(peak, 1, saturate(max(max(d.r, d.g), d.b) * 16 - 0.5));
    }
    float a = dot(level.Load(int3(0, 0, 0)).rgb, float3(0.2126, 0.7152, 0.0722));
    peak = lerp(peak, 1, saturate((a - 0.35) / 0.35));
    if (opts.x > 0) c = float3(lut.Sample(ps, float2(c.r, 0.5)).r, lut.Sample(ps, float2(c.g, 0.5)).g, lut.Sample(ps, float2(c.b, 0.5)).b);
    float3 l = Linear(c);
    float m = max(max(l.r, l.g), l.b);
    if (m > hdr.z && peak > 1) {
        float t = (m - hdr.z) / (1 - hdr.z);
        l *= (m + (peak - 1) * t * t * t) / m;
    }
    return float4(Out(l * hdr.x), 1);
}
// the UI layer, premultiplied, over the frame at scene white (in HDR10 blended in PQ's terms: only the menus' edges
// and their see-through backing differ from blending light)
float4 ui(O i) : SV_Target {
    float4 c = img.SampleLevel(ps, i.uv, 0);
    if (c.a <= 0) return 0;
    return float4(Out(Linear(c.rgb / c.a) * hdr.x) * c.a, c.a);
}
)";

// Depth is reversed in a 32 bit float buffer: 1 at the near clip, 0 at the far one, where a float keeps its finest
// steps; a 24 bit buffer storing z / w lost the steps that told distant surfaces apart
constexpr DXGI_FORMAT kDepthTypeless = DXGI_FORMAT_R32G8X24_TYPELESS;
constexpr DXGI_FORMAT kDepthFormat = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
constexpr DXGI_FORMAT kDepthRead = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
constexpr float kDepthFar = 0;

const char* kDepthHlsl = R"(
cbuffer B : register(b1) { float4 part; };  // xy: the source's part read, from the top left
Texture2D<float> img : register(t0);
SamplerState ps : register(s1);
struct O { float4 p : SV_Position; float2 uv : TEXCOORD0; };
O vs(uint id : SV_VertexID) {
    O o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.p = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
float psmain(O i) : SV_Depth { return img.SampleLevel(ps, i.uv * part.xy, 0); }
)";

const char* kClearHlsl = R"(
cbuffer B : register(b1) { float4 colour; };
float4 vs(uint id : SV_VertexID) : SV_Position {
    float2 t = float2((id << 1) & 2, id & 2);
    return float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 psmain() : SV_Target { return colour; }
)";

float Bits(uint32_t v) {
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

void Color(uint32_t argb, float* o) {
    o[0] = ((argb >> 16) & 255) / 255.0f;
    o[1] = ((argb >> 8) & 255) / 255.0f;
    o[2] = (argb & 255) / 255.0f;
    o[3] = (argb >> 24) / 255.0f;
}

// D3DBLEND, the console uses GL values
constexpr uint32_t kBlendDestColor = 0x306;  // D3DBLEND_DESTCOLOR

D3D12_BLEND Blend(uint32_t v, bool alpha) {
    switch (v) {
        case 0: return D3D12_BLEND_ZERO;
        case 1: return D3D12_BLEND_ONE;
        case 0x300: return alpha ? D3D12_BLEND_SRC_ALPHA : D3D12_BLEND_SRC_COLOR;
        case 0x301: return alpha ? D3D12_BLEND_INV_SRC_ALPHA : D3D12_BLEND_INV_SRC_COLOR;
        case 0x302: return D3D12_BLEND_SRC_ALPHA;
        case 0x303: return D3D12_BLEND_INV_SRC_ALPHA;
        case 0x304: return D3D12_BLEND_DEST_ALPHA;
        case 0x305: return D3D12_BLEND_INV_DEST_ALPHA;
        case kBlendDestColor: return alpha ? D3D12_BLEND_DEST_ALPHA : D3D12_BLEND_DEST_COLOR;
        case 0x307: return alpha ? D3D12_BLEND_INV_DEST_ALPHA : D3D12_BLEND_INV_DEST_COLOR;
        case 0x308: return D3D12_BLEND_SRC_ALPHA_SAT;
        case 0x8001: case 0x8003: return D3D12_BLEND_BLEND_FACTOR;
        case 0x8002: case 0x8004: return D3D12_BLEND_INV_BLEND_FACTOR;
        default: return D3D12_BLEND_ONE;
    }
}

// D3DBLENDOP, GL values
D3D12_BLEND_OP BlendOp(uint32_t v) {
    switch (v) {
        case 0x800A: return D3D12_BLEND_OP_SUBTRACT;
        case 0x800B: return D3D12_BLEND_OP_REV_SUBTRACT;
        case 0x8007: return D3D12_BLEND_OP_MIN;
        case 0x8008: return D3D12_BLEND_OP_MAX;
        default: return D3D12_BLEND_OP_ADD;
    }
}

// Where a fixed function draw's alpha comes from, for ray tracing's cut-outs: 1 stage 0's picture, 2 the picture times
// the vertices' colour (D3DTOP_SELECTARG1 or 2 of D3DTA_TEXTURE, D3DTOP_MODULATE of it and D3DTA_DIFFUSE, or of
// D3DTA_CURRENT, which at stage 0 is the diffuse), with stage 1 off or passing it on (with no texture of its own, a
// stage that reads one ends the stages, shaders.cpp: this title draws its leaves, ivy and walls with a MODULATE of
// TEXTURE and DIFFUSE left at stage 1 over none); 0 anything else, a pixel shader program among them
uint32_t AlphaFromPicture(const State& s) {
    if (s.ps) return 0;
    const auto& t0 = s.tss[0];
    uint32_t op = t0[kTssAlphaOp], a1 = t0[kTssAlphaArg1], a2 = t0[kTssAlphaArg2];
    auto diffuse = [](uint32_t a) { return a == 0 || a == 1; };  // D3DTA_DIFFUSE, D3DTA_CURRENT
    uint32_t source = (op == 2 && a1 == 2) || (op == 3 && a2 == 2)                       ? 1
                      : op == 4 && ((a1 == 2 && diffuse(a2)) || (diffuse(a1) && a2 == 2)) ? 2
                                                                                        : 0;
    const auto& t1 = s.tss[1];
    auto texture = [](uint32_t a) { return (a & 0xF) == 2; };  // D3DTA_TEXTURE, modifiers in the high bits
    bool ends = !s.textures[1] && t1[kTssColorOp] > 1 &&
                (texture(t1[kTssColorArg1]) || texture(t1[kTssColorArg2]) || texture(t1[kTssColorArg0]) ||
                 texture(t1[kTssAlphaArg1]) || texture(t1[kTssAlphaArg2]) || texture(t1[kTssAlphaArg0]));
    bool passed = ends || t1[kTssColorOp] == 1 || t1[kTssAlphaOp] == 1 ||
                  (t1[kTssAlphaOp] == 2 && t1[kTssAlphaArg1] == 1) || (t1[kTssAlphaOp] == 3 && t1[kTssAlphaArg2] == 1);
    return passed ? source : 0;
}

// A picture's alpha for ray tracing's cut-outs, from every seventh texel of a BGRA8 level (w x h, pitch bytes a row):
// 1 with clear parts (a texel in 200 under a quarter of its most alpha: ivy, leaves on the floor, a vine's fringe), 2
// faint all over (its most alpha under 0.6, or none: glass, a haze, a sheet the game fades in and out), 0 solid
uint8_t AlphaKind(const uint8_t* px, uint32_t w, uint32_t h, uint32_t pitch) {
    size_t n = 0, clear = 0;
    uint8_t most = 0;
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = (y * 3) % 7; x < w; x += 7) {
            most = std::max(most, px[size_t(y) * pitch + size_t(x) * 4 + 3]);
            n++;
        }
    if (!n) return 0;
    if (most < 153) return 2;
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = (y * 3) % 7; x < w; x += 7) clear += px[size_t(y) * pitch + size_t(x) * 4 + 3] * 4 < most;
    return clear * 200 > n ? 1 : 0;
}

// D3DCMP_NEVER 0x200 to D3DCMP_ALWAYS 0x207, D3D12's order from 1
D3D12_COMPARISON_FUNC Cmp(uint32_t v) {
    uint32_t n = v >= 0x200 && v <= 0x207 ? v - 0x200 : 7;
    return D3D12_COMPARISON_FUNC(n + 1);
}

// D3DSTENCILOP, GL values
D3D12_STENCIL_OP StencilOp(uint32_t v) {
    switch (v) {
        case 0: return D3D12_STENCIL_OP_ZERO;
        case 0x1E01: return D3D12_STENCIL_OP_REPLACE;
        case 0x1E02: return D3D12_STENCIL_OP_INCR_SAT;
        case 0x1E03: return D3D12_STENCIL_OP_DECR_SAT;
        case 0x150A: return D3D12_STENCIL_OP_INVERT;
        case 0x8507: return D3D12_STENCIL_OP_INCR;
        case 0x8508: return D3D12_STENCIL_OP_DECR;
        default: return D3D12_STENCIL_OP_KEEP;
    }
}

// D3DTADDRESS: 1 wrap, 2 mirror, 3 clamp, 4 border, 5 clamp to edge
D3D12_TEXTURE_ADDRESS_MODE Address(uint32_t v) {
    switch (v) {
        case 2: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
        case 3: case 5: return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        case 4: return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        default: return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    }
}

DXGI_FORMAT HostFormat(Host h) {
    switch (h) {
        case Host::BC1: return DXGI_FORMAT_BC1_UNORM;
        case Host::BC2: return DXGI_FORMAT_BC2_UNORM;
        case Host::BC3: return DXGI_FORMAT_BC3_UNORM;
        case Host::R32F: return DXGI_FORMAT_R32_FLOAT;
        default: return DXGI_FORMAT_B8G8R8A8_UNORM;
    }
}

constexpr uint32_t kFmtD16 = 0x2C;     // D3DFMT_D16
constexpr uint32_t kFmtLinD16 = 0x30;  // D3DFMT_LIN_D16

// TexInfo::shadow of a depth texture
uint8_t ShadowDepthKind(uint32_t format) { return format == kFmtD16 || format == kFmtLinD16 ? 2 : 1; }

// bytes per row of a 32 bit surface in guest memory, rounded to 64
uint32_t GuestPitch(uint32_t width) { return (width * 4 + 63) & ~63u; }

// rows of a texture upload start D3D12_TEXTURE_DATA_PITCH_ALIGNMENT apart
uint32_t UploadPitch(uint32_t row_bytes) { return (row_bytes + 255) & ~255u; }

D3D12_RESOURCE_DESC BufferDesc(uint64_t bytes) {
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = bytes;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return bd;
}

// one level, one sample: callers set Flags where they differ
D3D12_RESOURCE_DESC Texture2DDesc(DXGI_FORMAT fmt, uint64_t w, uint32_t h) {
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = w;
    td.Height = h;
    td.DepthOrArraySize = 1;
    td.MipLevels = 1;
    td.Format = fmt;
    td.SampleDesc.Count = 1;
    return td;
}

// MemSig hashes a surface up to kSigWholeMax bytes whole, a bigger one as evenly spaced samples
constexpr uint32_t kSigWholeMax = 65536;
constexpr uint32_t kSigSamples = 64;
constexpr uint32_t kSigSampleBytes = 64;

uint64_t MemSig(uint32_t data, uint32_t total) {
    const uint8_t* p = static_cast<const uint8_t*>(kernel::HostPtr(data));
    uint64_t sig = Hash(&total, sizeof(total));
    g_perf.sigs++;
    g_perf.sig_bytes += total <= kSigWholeMax ? total : kSigSamples * kSigSampleBytes;
    if (total <= kSigWholeMax) return Hash(p, total, sig);
    for (uint32_t i = 0; i < kSigSamples; i++) {
        const uint8_t* sample = p + uint64_t(total - kSigSampleBytes) * i / (kSigSamples - 1);
        sig = Hash(sample, kSigSampleBytes, sig);
    }
    return sig;
}

// MemSig samples a big surface and misses small writes (XFONT text): after a Sync every byte is compared
bool Changed(const Target& t) {
    if (t.drawn || t.read.empty()) return false;
    uint32_t row = t.gw * 4;
    uint32_t pitch = GuestPitch(t.gw);
    const uint8_t* p = static_cast<const uint8_t*>(kernel::HostPtr(t.data));
    for (uint32_t y = 0; y < t.gh; y++) {
        if (std::memcmp(p + size_t(y) * pitch, &t.read[size_t(y) * row], row)) return true;
    }
    return false;
}

// a lock alone proves nothing, reads lock too: only changed bytes mean the CPU wrote it
bool LatchCpuWrite(Target& t) {
    uint64_t e = DirtyEpoch(t.data);
    if (e <= t.epoch) return false;
    t.epoch = e;
    uint64_t sig = MemSig(t.data, t.bytes);
    if (sig == t.sig && !Changed(t)) return false;
    t.sig = sig;
    return true;
}

struct Tex {
    ComPtr<ID3D12Resource> res;
    TextureDesc d;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    uint32_t levels = 1;
    uint64_t sig = 0;
    uint64_t frame = 0;
    uint64_t used = 0;     // the present it was last drawn from (TrimReplacements)
    uint64_t palette = 0;  // hash of the colours a P8 texture was decoded with
    uint8_t flags = 0;  // Extension::Decoded's
    uint8_t alpha = 1;  // its own picture's alpha for ray tracing's cut-outs (AlphaKind), as it was last uploaded
    const Tex* swap = nullptr;  // the texture pack's image drawn in its place
    uint64_t pack_key = 0;      // its pack image is still loading
    uint32_t pack_gen = 0;      // texturedumper::Generation the key was taken at, 0 after new bytes
    // Extension::Replace's: the picture drawn instead, made once the extension has it
    uint64_t repl_id = 0;
    ComPtr<ID3D12Resource> repl;
    uint32_t repl_levels = 0;
    uint8_t repl_alpha = 1;  // its picture's alpha for ray tracing's cut-outs (AlphaKind)
    // the replacement before, kept for the next one of its size to be uploaded into rather than made anew: a movie's
    // frames, one after another in the same texture
    ComPtr<ID3D12Resource> spare;
    // the replacement's material: its normal and ORM maps, their levels, its parameters
    ComPtr<ID3D12Resource> mat[2];
    uint32_t mat_levels[2] = {};
    float mat_params[3][4] = {};
    float mat_gloss[2] = {};
};

struct PsoKey {
    uint64_t vs;
    uint64_t ps;
    uint32_t regs;
    uint32_t topo;
    uint32_t has_rt;
    uint32_t has_ds;
    uint32_t elements[16];  // per register: DXGI format, buffer << 8, offset << 16
    uint32_t blend;
    uint32_t src;
    uint32_t dst;
    uint32_t op;
    uint32_t wmask;
    uint32_t zen;
    uint32_t zwrite;
    uint32_t zfunc;
    uint32_t sten;
    uint32_t sfunc;
    uint32_t smask;
    uint32_t swmask;
    uint32_t sfail;
    uint32_t szfail;
    uint32_t spass;
    uint32_t cull;
    uint32_t ccw;
    uint32_t fill;
    int32_t zbias;
    float zslope;
    uint32_t rtfmt;
    uint32_t sprite;  // DrawShaders::sprite
    uint32_t mv;      // frame generation's motion target bound as SV_Target1: 1 kept, 2 written (PixelKey::motion)
    uint32_t rx;      // anti-aliasing's reactive mask bound as SV_Target2 (with mv): 1 kept, 2 written as its maximum
};

// Blit's settings past the source and its rectangle
struct BlitOptions {
    bool gamma = false;       // through the gamma lookup
    bool alpha = false;       // the source's alpha, else 1
    uint32_t taps = 1;        // n x n box filter per pixel
    float mul = 1;            // colour scale
    DXGI_FORMAT rtv = kBgra;  // the target's format
    bool over = false;        // premultiplied over the target, which keeps its alpha
    uint32_t hdr = 0;         // kHdrHlsl into an HDR target: 1 the game's frame, 2 the UI layer over it
};

// Shrink's settings past the source and the size
struct ShrinkOptions {
    uint32_t taps = 1;
    // into: a copy from an earlier call to draw over again, in state into_state
    ID3D12Resource* into = nullptr;
    D3D12_RESOURCE_STATES into_state = D3D12_RESOURCE_STATE_COPY_SOURCE;
    float mul = 1;
};

// a draw's four texture stages: BindStageTexturesAndTargets fills them, the constants, views and samplers read them
struct DrawTextures {
    TexInfo info[4];
    // the four stages', then a material's normal and ORM maps (kMaterialViews)
    D3D12_SHADER_RESOURCE_VIEW_DESC sv[6] = {};
    ID3D12Resource* res[6] = {};
    int mat_stage = -1;          // the first stage drawn from a replacement with a material, -1 none
    float mat[3][4] = {};        // its Material::params
    float gloss[2] = {};         // and Material::gloss
    bool rt[4] = {};            // stage samples a render target
    float k[4] = {1, 1, 1, 1};  // Extension::Sample's k
    float wide[4] = {};         // host texels per console texel of a target read into a smaller one, 0 otherwise
    bool replaced[4] = {};      // drawn from an extension's replacement, filtered across all its levels
    uint8_t cutout[4] = {};     // the picture drawn's alpha for ray tracing's cut-outs (AlphaKind)
    uint32_t data[4] = {};      // the title's texture data each stage drew from (0: none, a target)
};

// what the last draw set on the command list, so the next one sets only what differs; anything else that sets them
// clears it
struct DrawBinds {
    ID3D12PipelineState* pso = nullptr;
    D3D_PRIMITIVE_TOPOLOGY topology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
    bool rasterizer = false;  // viewport and scissor hold
    D3D12_VIEWPORT viewport{};
    D3D12_RECT scissor{};
    bool output = false;  // blend and stencil hold
    float blend[4] = {};
    uint32_t stencil = 0;
    D3D12_GPU_VIRTUAL_ADDRESS vsc = 0;  // root constant buffers
    D3D12_GPU_VIRTUAL_ADDRESS dc = 0;
    uint32_t views = UINT32_MAX;  // root descriptor table slots
    uint32_t samplers = UINT32_MAX;
};

// the last targets EnsureTarget and FindTarget marked used, so target_used_ is written once a frame each
struct UsedTarget {
    uint32_t data = 0;
    uint64_t frame = UINT64_MAX;
};

// a draw's shaders: what they are made from, their cache keys, the hashes Compile returned and the compiled blobs
struct DrawShaders {
    bool prog = false;  // a vertex program, else the fixed function pipeline
    bool quad = false;
    bool fixed_transform = false;  // fixed function on untransformed vertices: the shader reads matrices and lights
    bool sprite = false;           // a program's points drawn as point sprites (SpriteHlsl)
    FixedKey fk;
    uint64_t vkey = 0;
    uint64_t vh = 0;
    uint64_t pkey = 0;
    uint64_t ph = 0;
    ID3DBlob* vs_code = nullptr;
    ID3DBlob* ps_code = nullptr;
};

// the render target's console size in pixels, or the depth buffer's without one
struct GuestExtent {
    uint32_t w;
    uint32_t h;
};

class D3D12Backend final : public Backend, public Context {
public:
    bool Init(uint32_t width, uint32_t height) override;
    void Clear(const State& s, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color, float z,
               uint32_t stencil) override;
    void Draw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) override;
    void Copy(const Surface& src, const Surface& dst, uint32_t count, uint32_t src_rects, uint32_t dst_points) override;
    void Present(const Surface& front) override;
    void DeviceLost();
    bool lost_said_ = false;
    void WaitGpuFrame(uint64_t n) override;
    void Sync(uint32_t data, uint32_t size) override;
    void SyncLate(uint32_t data) override;
    bool DeliverLate(uint32_t data) override;
    void RecordLate(uint32_t data) override;
    void Flush() override;
    void SetGamma(const uint8_t* ramp) override;
    void ClearReport() override { counted_.clear(); }
    // done once the list being recorded, the next fence value, has run
    void GetReport(uint32_t report) override { reports_.push_back({report, counted_, fence_value_ + 1}); }

    ID3D12Device* Device() override { return dev_.Get(); }
    ID3D12GraphicsCommandList* List() override {
        ext_list_ = true;  // it may set anything on the list: the next draw binds all again
        return cl_.Get();
    }
    void Release(ComPtr<ID3D12Resource> r) override { CurrentFrame().garbage.push_back(std::move(r)); }
    D3D12_CPU_DESCRIPTOR_HANDLE View(const Target& t) override { return Cpu(rtv_heap_.Get(), t.view, rtv_inc_); }
    void Unbind() override { bound_rt_ = bound_ds_ = nullptr; }
    void Capture(Target& t, const char* tag, float mul) override;
    float Scale() const override { return scale_; }
    void GuestSize(uint32_t& w, uint32_t& h) const override {
        w = guest_w_;
        h = guest_h_;
    }
    uint64_t Presents() const override { return presents_; }

private:
    struct Frame {
        ComPtr<ID3D12CommandAllocator> alloc;
        ComPtr<ID3D12Resource> upload;
        uint8_t* map = nullptr;
        uint64_t used = 0;
        uint64_t fence = 0;
        std::vector<ComPtr<ID3D12Resource>> garbage;
        uint32_t srv = 0;
    };
    struct Alloc {
        ID3D12Resource* res;
        uint64_t off;
        uint8_t* cpu;
        D3D12_GPU_VIRTUAL_ADDRESS gpu;
    };

    Alloc Upload(uint64_t size, uint64_t align = 256);
    Frame& CurrentFrame() { return frames_[frame_ % kFrames]; }
    uint32_t AllocSrvSlots(uint32_t n);
    D3D12_CPU_DESCRIPTOR_HANDLE Cpu(ID3D12DescriptorHeap* h, uint32_t i, uint32_t inc) {
        auto c = h->GetCPUDescriptorHandleForHeapStart();
        c.ptr += size_t(i) * inc;
        return c;
    }
    D3D12_GPU_DESCRIPTOR_HANDLE Gpu(ID3D12DescriptorHeap* h, uint32_t i, uint32_t inc) {
        auto g = h->GetGPUDescriptorHandleForHeapStart();
        g.ptr += uint64_t(i) * inc;
        return g;
    }
    void Transition(ID3D12Resource* r, D3D12_RESOURCE_STATES& cur, D3D12_RESOURCE_STATES want) override;
    void Begin();
    void Submit(bool wait);
    // the frame so far submitted, not waited for, and the frame recorded on after it in the same allocator and upload
    // space (Linux's FSR and DLSS: their commands go on the queue between)
    void SubmitSoFar();
    void WaitFence(uint64_t value);
    void SetFullViewport(uint32_t w, uint32_t h);
    void SetBlitTarget(D3D12_CPU_DESCRIPTOR_HANDLE rtv, uint32_t w, uint32_t h);
    void Blit(ID3D12Resource* src, DXGI_FORMAT fmt, const float uvrect[4], bool gamma, bool alpha, uint32_t taps = 1,
              float mul = 1, DXGI_FORMAT rtv = kBgra) override {
        ext_list_ = true;
        Blit(src, fmt, uvrect, BlitOptions{.gamma = gamma, .alpha = alpha, .taps = taps, .mul = mul, .rtv = rtv});
    }
    void Blit(ID3D12Resource* src, DXGI_FORMAT fmt, const float uvrect[4], const BlitOptions& o);
    uint32_t EnsureBlitSamplers();
    void DrawFullScreen(ID3D12PipelineState* pso, D3D12_GPU_VIRTUAL_ADDRESS cb, uint32_t srv_slot,
                        std::optional<uint32_t> sampler_slot = {});
    ID3D12Resource* Feedback(Target& t) override;
    void Resolve(Target& t) {
        if (t.range != 1 && ext_) ext_->Resolve(*this, t);
    }
    // Pass, then its step
    void Pass(const char* hlsl, const char* entry, DXGI_FORMAT rtv, D3D12_CPU_DESCRIPTOR_HANDLE rh, uint32_t w,
              uint32_t h, ID3D12Resource* const src[3], const DXGI_FORMAT fmt[3], const float p[8],
              float blend) override;
    ID3D12PipelineState* EnsurePassPipeline(const char* hlsl, const char* entry, DXGI_FORMAT rtv, float blend);
    // Draw, in call order, each callee right after its caller
    bool DepthCopy(const State& s);
    ID3D12PipelineState* EnsureDepthCopyPipeline();
    void DrawDepthCopy(Target& src, Target& dst, ID3D12PipelineState* pso);
    bool AssembleDraw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices,
                      Assembled& a);
    bool BindTargets(const State& s);
    void EnsureVertexShader(const State& s, const Assembled& a, DrawShaders& sh);
    bool BindStageTexturesAndTargets(const State& s, DrawTextures& stages);
    ID3D12Resource* BindTexture(const State& s, int stage, DrawTextures& stages);
    ID3D12Resource* BindTargetTexture(const State& s, int stage, const TextureDesc& d, Target& t, bool box,
                                      DrawTextures& stages);
    void MakeReplacement(Tex& tx);
    uint32_t repl_budget_ = 0;  // replacements made this frame, a few at most so none stalls a frame
    ID3D12Resource* BindGuestTexture(const State& s, int stage, const TextureDesc& d, const FormatInfo& fi,
                                     DrawTextures& stages);
    bool CreateTexture(Tex& tx, const TextureDesc& d, const FormatInfo& fi, uint64_t key);
    void UploadTexture(Tex& tx, const TextureDesc& d, const FormatInfo& fi, const uint8_t* p, const uint32_t* colours,
                       bool same);
    void UploadTextureLevel(Tex& tx, uint32_t f, uint32_t l, const std::vector<uint8_t>& px, uint32_t pitch);
    void LatchPackKey(Tex& tx, const TextureDesc& d, const FormatInfo& fi, const uint32_t* colours);
    void LatchPackImage(Tex& tx);
    void CreatePackTexture(Tex& r, const texturedumper::Image& img);
    void ReleasePackTextures();
    void EnsurePixelShader(const PixelKey& pk, DrawShaders& sh);
    void BuildPipelineKey(const State& s, const Assembled& a, uint64_t vh, uint64_t ph, PsoKey& k);
    void KeyDepthStencil(const State& s, const Assembled& a, PsoKey& k);
    ID3D12PipelineState* EnsurePipeline(const PsoKey& k, ID3DBlob* vs, ID3DBlob* ps);
    void FillPipelineDesc(const PsoKey& k, ID3DBlob* vs, ID3DBlob* ps, D3D12_GRAPHICS_PIPELINE_STATE_DESC& d) const;
    void ExpandPoints(D3D12_GRAPHICS_PIPELINE_STATE_DESC& d, bool sprite);
    float TargetScale() const;  // host pixels per console pixel of the bound target, down (across: times aspect_)
    float RenderTargetShift(const State& s, const DrawShaders& sh) const;
    void FillDepthConstants(const State& s, const DrawShaders& sh, const PsoKey& k, DrawConstants& c) const;
    void UploadVertexConstants(const State& s, const void* edited, const float* prev);
    void UploadConstants(const State& s, const DrawShaders& sh, const PsoKey& k, const DrawTextures& stages,
                         GuestExtent size, DrawConstants& c);
    void FillTextureConstants(const State& s, const DrawShaders& sh, const DrawTextures& stages, DrawConstants& c);
    void FillTexelConstants(int i, const TextureDesc& d, const DrawTextures& stages, DrawConstants& c);
    void CaptureDraw(const State& s, uint32_t prim, uint32_t count, const PsoKey& k, const DrawConstants& c,
                     const DrawTextures& stages);
    void CaptureTextures(const State& s, const DrawTextures& stages);
    void EndDrawCapture(const State& s, const Assembled& a, uint32_t count, const DrawConstants& c);
    void CaptureInputs(const State& s, const Assembled& a, const DrawConstants& c);
    void CaptureShaders(const State& s, const Assembled& a, const DrawShaders& sh, const PixelKey& pk);
    void CaptureShader(const char* kind, uint64_t key, const std::string& src);
    void SetTextureViewsAndSamplers(const State& s, const Assembled& a, DrawTextures& stages);
    uint32_t EnsureViewSet(const DrawTextures& stages);
    uint32_t EnsureStageSamplers(const State& s, const DrawTextures& stages, bool clamp);
    void SetViewportScissor(const State& s, GuestExtent size);
    void SetBlendFactorStencilRef(const State& s);
    void UpdateTargetUsed(uint32_t data);
    // rebased: an indexed draw's indices with its base vertex added (32 bit), drawn with none, so SV_VertexID is the
    // vertex's place in its buffers whichever way a driver counts it (ray tracing's capture)
    void DrawGeometry(const Assembled& a, const uint32_t* rebased = nullptr);
    void UploadVertexBuffers(const Assembled& a);
    // visibility tests
    bool EnsureQueries();
    uint32_t BeginCountedDraw(const State& s);
    void EndCountedDraw(uint32_t query);
    float SamplesPerPixel() const;
    void DeliverReports();
    // shaders by source hash: Compile returns the bytecode and sets hash
    ID3DBlob* Compile(const std::string& src, const char* entry, const char* profile, uint64_t& hash);
    // Blit's pipeline
    ID3D12PipelineState* EnsureBlitPipeline(DXGI_FORMAT rtv, bool over = false);
    ID3D12PipelineState* EnsureHdrPipeline(uint32_t mode);  // BlitOptions::hdr
    D3D12_GRAPHICS_PIPELINE_STATE_DESC FullScreenPipelineDesc(ID3DBlob* vs, ID3DBlob* ps, DXGI_FORMAT rtv) const;
    // Init, in order, each callee right after its caller
    bool CreateDevice();
#ifdef _WIN32
    bool CreateSwapChain(HWND hwnd);
    bool FrameGenVsync();
#else
    bool CreateSwapChain();
#endif
    void CreateHeaps();
    uint32_t CreateHeap(D3D12_DESCRIPTOR_HEAP_TYPE t, uint32_t n, bool visible, ComPtr<ID3D12DescriptorHeap>& out);
    void CreateBackBufferViews();
    bool CreateRootSignature();
    void CreateFrames();
    void CreateGammaLut();
    void LoadSettings();
    // Present, in call order, each callee right after its caller; DrawFrame runs the two after it
    void FinishCapture(const Surface& front);
    void StartCapture();
    void ResizeSwap();
    void CheckDisplay();
    bool SetColourSpace(bool hdr);
    void UploadGammaRamp();
    void DrawFrame(Target* t, uint32_t bi);
    void HdrConstants();
    void BlitFrame(Target& t);
    void RenderUi();
    void RenderUiHdr(D3D12_CPU_DESCRIPTOR_HANDLE rh);
    void EnsureUiLayer();
    void SnapshotUi(Target& t);
    // frame generation: the frame's place on the back buffer, its inputs made there
    D3D12_VIEWPORT FrameViewport() const;
    void EnsureFrameGenTargets(uint32_t w, uint32_t h, DXGI_FORMAT bb);
    bool EnsureObjectMotion(const Target& rt);
    bool EnsureReactive(const Target& rt);
    // anti-aliasing (framegen.h, AntiAlias): the frame's 3D before its UI, once a frame; true when it recorded
    // anything, after which the draw at hand's state must be set again
    bool AntiAliasFrame(Target& t);
    bool EnsureAaTargets(uint32_t w, uint32_t h, DXGI_FORMAT fmt);
    bool EnsureDenoiseTargets(uint32_t w, uint32_t h);
    // ambient occlusion (g_ambient_occlusion): the frame's 3D darkened where surfaces meet, from its depth, before the
    // anti-aliasing's pass; rw x rh the part drawn
    void AmbientOcclusion(Target& t, uint32_t rw, uint32_t rh, const FrameView& view, bool rx, bool still = false);
    // ambient occlusion with no anti-aliasing running, at the frame's UI (OcclusionAlone): true when passes were recorded
    bool OcclusionAlone(Target& t);
    // the own TAA's pass (framegen::MakeOwnTaa) over the frame, in AntiAliasFrame; false when it can't
    bool TaaFrame(Target& t, const framegen::AntiAlias::Frame& f);
    const char* AaName() const {
        return AaName(aa_);
    }
    static const char* AaName(const framegen::AntiAlias* a) {
        if (a && a->VulkanImages()) return a->Kind() == framegen::kAaDlss ? "NVIDIA DLSS (Vulkan)" : "AMD FSR (Vulkan)";
        return a && a->Own() ? "TAA" : framegen::AntiAliasName(a ? a->Kind() : 0);
    }
    bool BuildRtScene(uint32_t rw, uint32_t rh);
    void Reflections(Target& t, uint32_t rw, uint32_t rh);
    void Water(Target& t, uint32_t rw, uint32_t rh);
    bool water_said_ = false, lights_said_ = false, late_said_ = false, shadow_said_ = false, sky_said_ = false;
    bool rt_view_now_ = false;  // this frame's camera, and its move from the frame before, known (the water's)
    // the sky's measure this frame (SkyLight's SRV index, -1 none) and the frame it was made for
    float sky_slot_ = -1;
    uint64_t sky_frame_ = UINT64_MAX, sky_hold_frame_ = UINT64_MAX;
    uint32_t sky_cur_ = 0;
    // the light shafts' targets (Shafts): the march, the gathered twice over (shaft_cur_ this frame's)
    ComPtr<ID3D12Resource> shafts_[kShafts];
    D3D12_RESOURCE_STATES shafts_state_[kShafts] = {};
    uint32_t shafts_w_ = 0, shafts_h_ = 0, shaft_cur_ = 0;
    uint64_t shafts_frame_ = UINT64_MAX;
    bool shafts_said_ = false;
    void Shafts(Target& t, uint32_t rw, uint32_t rh);
    // each frame slot's late water draws' pictures (Scene::AddLateDraw), made again in the next frame's descriptors
    struct LatePicture {
        ComPtr<ID3D12Resource> res;
        D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    };
    std::vector<LatePicture> late_pictures_[kFrames];
    void Lighting(Target& t, uint32_t rw, uint32_t rh);
    uint32_t Denoise(uint32_t slot, uint32_t tw, uint32_t th, uint32_t gw, uint32_t gh, uint32_t rw, uint32_t rh);
    bool Guides(const Target& t, uint32_t rw, uint32_t rh);
    void EndRtPass();
    ID3D12PipelineState* EnsureAoApplyPipeline(DXGI_FORMAT rtv);
    // XBE_PERF: a GPU timestamp at the start of a part of the frame (GpuMark); GpuFrameEnd resolves the frame's,
    // GpuFrameStart adds up the slot's last ones (its fence passed) and marks the new frame's start
    void GpuMark(uint32_t mark);
    // the GPU's timestamps kept (XBE_PERF's parts; and while ray traced lighting is on, for RtBudgetRays)
    bool GpuTiming() const;
    // ray tracing's time a frame, eased over the frames (ms; 0 none yet), and the bounce lighting's part of it
    double rt_ms_ = 0, gi_ms_ = 0;
    // the bounce lighting's rays a pixel within the ray tracing's budget: halved (to half the quality's at most) while
    // the frame's ray tracing takes longer than XBE_RT_BUDGET ms, back once there is room for them again
    float RtBudgetRays(float full);
    uint32_t gi_halved_ = 0;
    uint64_t gi_budget_frame_ = 0;
    void GpuFrameEnd();
    void GpuFrameStart();
    // upscaling (g_upscale): the frame's 3D drawn into the top left part of its target (the scene), from the frame's
    // first draw into the back buffer until its UI starts, then made up to the whole target
    void OpenScene(Target& rt);
    bool InScene() const;          // the bound targets are the scene's
    float SceneMipBias() const;    // the draw's texture level of detail bias in the scene, 0 outside it
    bool SceneTarget(const Target* t) const;  // the scene's colour or depth
    Target* SceneDepth() const;
    // the depth a picture drawn apart with the frame's depth takes while the scene is open (BindTargets), as large as
    // like, cleared the first time each frame; null when none can be made
    Target* AfterDepth(Target& like);
    bool AfterDepthFill(Target& like);
    Target after_ds_;
    uint64_t after_ds_frame_ = UINT64_MAX;
    void CloseScene(const char* why);  // the scene made whole (why: when early, for the log), the renderer's state reset
    bool FinishFrame3d(Target& t);      // the UI starts: anti-aliased or the scene closed; true when it recorded
    void StretchScene(Target& t);       // the scene's colour drawn over the whole target, when no pass made it up
    void SceneDepthUp(Target& t, bool copied);  // its depth and motion made whole, from aa_depth_ and aa_motion_
    uint64_t MotionKey(const State& s, const Assembled& a, uint64_t vkey);
    const float* PrevMotionConstants(uint64_t key, const float* now) const;
    void KeepMotionConstants(uint64_t key, const uint8_t* vsc);
    void FrameGenInputs(Target* t, uint32_t bi, D3D12_CPU_DESCRIPTOR_HANDLE rh);
    void MeasureFrame(Target& t);
    void HdrReadBack(uint32_t bi, D3D12_RESOURCE_STATES& bs);
    void ScreenShot(uint32_t bi, D3D12_RESOURCE_STATES& bs);
    void ReleaseOldTextures();
    void ResetFrame();
    void CountFps();
    void ApplySettings();
    ComPtr<ID3D12Resource> Shrink(ID3D12Resource* src, D3D12_RESOURCE_STATES& st, DXGI_FORMAT fmt, uint32_t w,
                                  uint32_t h, const ShrinkOptions& o = {});
    bool ReadBack(ID3D12Resource* res, uint32_t w, uint32_t h, std::vector<uint8_t>& out);
    void CapStep(const Surface& rt);
    // a resource CapSave shrinks to w x h and writes, in state *state, colour times mul
    struct CapImage {
        ID3D12Resource* res;
        D3D12_RESOURCE_STATES* state;
        DXGI_FORMAT fmt;
        uint32_t w;
        uint32_t h;
        float mul = 1;
    };
    void CapSave(const CapImage& img, const char* name);
    void CapSave(Target& t, const char* name);
    // a surface's place in targets_: its key, console size and entry, end when it has none
    struct TargetSlot {
        uint32_t data;
        uint64_t key;
        uint32_t gw;
        uint32_t gh;
        std::unordered_map<uint64_t, Target>::iterator it;
        bool sized;  // the entry is there at the surface's size
    };
    Target* FindTarget(const Surface& s, bool depth);
    Target* EnsureTarget(const Surface& s, bool depth);
    // EnsureTarget's steps, in call order
    TargetSlot FindTargetSlot(const Surface& s, bool depth, uint64_t alt);
    uint64_t SizedTargetKey(uint32_t data, bool depth, uint32_t gw, uint32_t gh) const;
    bool TargetStale(uint32_t data) const;
    void ReleaseTarget(std::unordered_map<uint64_t, Target>::iterator it);
    bool CreateTargetResource(Target& t, const Surface& s);
    bool CreateTargetView(Target& t);
    // UploadTarget, then its step
    void UploadTarget(Target& t, const Surface& s);
    ComPtr<ID3D12Resource> CreatePixelTexture(const std::vector<uint8_t>& px, uint32_t pitch, uint32_t w, uint32_t h);
    // Clear's arguments past the state
    struct ClearArgs {
        uint32_t count;
        const uint8_t* rects;
        uint32_t flags;
        uint32_t color;
        float z;
        uint32_t stencil;
        bool scene = false;  // into the scene's part (upscaling): a rect short of the whole target is scaled to it
    };
    void ClearTarget(Target& t, const State& s, const ClearArgs& c, std::vector<D3D12_RECT>& rs);
    void ClearChannels(Target& t, const float col[4], uint8_t mask, const std::vector<D3D12_RECT>& rs);
    ID3D12PipelineState* EnsureClearPipeline(DXGI_FORMAT rtv, uint8_t mask);
    // Copy's guest lists: count src_rects, dst_points 0 or one per rect
    struct CopyList {
        uint32_t count;
        uint32_t src_rects;
        uint32_t dst_points;
    };
    // src_part, dst_part: the share of each target its pixels are in, across and down (the scene's, upscaling)
    void CopyByDraw(const Target& src_t, ID3D12Resource* src_res, D3D12_RESOURCE_STATES& src_state, Target& dst_t,
                    const CopyList& l, const float src_part[2] = nullptr, const float dst_part[2] = nullptr);
    void CopyByRegion(const Target& src_t, ID3D12Resource* src_res, D3D12_RESOURCE_STATES& src_state, Target& dst_t,
                      const CopyList& l);
    // the target as a guest sized BGRA copy source, and the shrunk copy that holds it when it is not one already
    struct CopySource {
        ID3D12Resource* res;
        ComPtr<ID3D12Resource> down;
    };
    CopySource PrepareCopySource(Target& t, const ShrinkOptions& o);
    // RecordLate's steps, under late_lock_
    struct Late;
    void ResetLate(Late& l, const Target& t);
    ID3D12Resource* PrepareLateSource(Target& t, Late& l);
    void RecordLateCopy(Late& l, uint32_t slot, ID3D12Resource* src);

#ifdef _WIN32
    ComPtr<IDXGIFactory4> factory_;
#endif
    ComPtr<ID3D12Device> dev_;
    ComPtr<ID3D12CommandQueue> queue_;
#ifdef _WIN32
    ComPtr<IDXGISwapChain3> swap_;
#else
    std::unique_ptr<linux_present::Swap> swap_;
#endif
    ComPtr<ID3D12Resource> bb_[kFrames];
    ComPtr<ID3D12Fence> fence_;
    HANDLE event_ = nullptr;
    uint64_t fence_value_ = 0;
    Frame frames_[kFrames];
    uint64_t frame_ = 0;
    // texture view sets made since the descriptor ring last started over, by a hash of their resources and views, to
    // their first slot; resources freed in that time go to the frame's garbage, so no other one takes their address
    std::unordered_map<uint64_t, uint32_t> view_sets_;
    uint64_t view_sets_frame_ = UINT64_MAX;
    // the constants last uploaded in this command list, reused while a draw's are the same; 0 after a submit
    decltype(State::vsc) last_vsc_{};
    D3D12_GPU_VIRTUAL_ADDRESS last_vsc_gpu_ = 0;
    // the draw's vertex constants as the extension edits them, in ordinary memory: the upload heap is write-combined,
    // and reading it back (the extension's edits, the motion's copy for the next frame) takes microseconds a draw
    decltype(State::vsc) vsc_edit_{};
    DrawConstants last_dc_{};
    D3D12_GPU_VIRTUAL_ADDRESS last_dc_gpu_ = 0;
    ComPtr<ID3D12GraphicsCommandList> cl_;
    ComPtr<ID3D12DescriptorHeap> srv_heap_;
    ComPtr<ID3D12DescriptorHeap> smp_heap_;
    ComPtr<ID3D12DescriptorHeap> rtv_heap_;
    ComPtr<ID3D12DescriptorHeap> dsv_heap_;
    ComPtr<ID3D12DescriptorHeap> ui_heap_;
    uint32_t srv_inc_ = 0;
    uint32_t smp_inc_ = 0;
    uint32_t rtv_inc_ = 0;
    uint32_t dsv_inc_ = 0;
    std::vector<uint32_t> free_rtv_;
    std::vector<uint32_t> free_dsv_;
    ComPtr<ID3D12RootSignature> root_;
    std::unordered_map<uint64_t, ComPtr<ID3DBlob>> blobs_;
    std::unordered_map<uint64_t, uint64_t> shader_src_;  // vertex and pixel program keys to source hash
    std::unordered_map<uint64_t, ComPtr<ID3D12PipelineState>> psos_;
    std::unordered_map<uint64_t, uint32_t> samplers_;
    uint32_t smp_used_ = 0;
    std::unordered_map<uint64_t, Target> targets_;
    std::unordered_map<uint32_t, uint64_t> target_used_;  // target address to the frame it was last used
    std::array<UsedTarget, 4> recent_used_{};
    uint32_t recent_used_next_ = 0;
    DrawBinds binds_;
    // the extension recorded on the list (List, Blit, Pass, Capture): the draw's bindings are all set again after it
    bool ext_list_ = false;
    std::unordered_map<uint64_t, Tex> textures_;  // by data address, a P8 texture's with its palette's above
    std::unordered_map<uint64_t, Tex> pack_textures_;  // texture pack images by key, kept for the run
    std::unordered_map<uint64_t, ComPtr<ID3D12PipelineState>> blit_psos_;
    Extension* ext_ = nullptr;
    // one frame capture: every draw in a text file, each target as the frame leaves it, the textures it read
    FILE* cap_ = nullptr;
    std::filesystem::path cap_dir_;
    std::filesystem::path shot_dir_;  // a capture just finished: the frame as shown goes there (ScreenShot)
    uint32_t cap_i_ = 0;
    uint32_t cap_rt_ = 0;
    std::unordered_set<uint32_t> cap_seen_;
    std::unordered_set<uint64_t> cap_sh_;
    struct CapTex {
        ComPtr<ID3D12Resource> res;
        DXGI_FORMAT fmt;
        uint32_t data;
        uint32_t w;
        uint32_t h;
    };
    std::vector<CapTex> cap_tex_;
    ComPtr<ID3D12Resource> lut_;
    D3D12_RESOURCE_STATES lut_state_ = D3D12_RESOURCE_STATE_COPY_DEST;
    uint8_t ramp_[768] = {};
    bool ramp_dirty_ = false;
    bool gamma_ = false;
    Target* bound_rt_ = nullptr;
    Target* bound_ds_ = nullptr;
    Target* draw_rt_ = nullptr;  // the draw's targets, the bound ones go when a texture's copy is drawn
    Target* draw_ds_ = nullptr;
    uint32_t guest_w_ = 640;
    uint32_t guest_h_ = 480;
    uint32_t bb_w_ = 0;
    uint32_t bb_h_ = 0;
    float scale_ = 1.0f;
    // a target's host pixels across per host pixel down, per console pixel: widescreen squeezes the frame into the
    // console's 4:3 and the present stretches it back, so its targets are that much wider (TargetAspect), and 100%
    // draws as many pixels across as the screen shows
    float aspect_ = 1.0f;
    // surfaces locked without a wait: copies in flight, the newest finished one goes to guest memory
    struct Late {
        ComPtr<ID3D12Resource> rb[kFrames];
        ComPtr<ID3D12Resource> down;
        D3D12_RESOURCE_STATES down_state = D3D12_RESOURCE_STATE_COPY_SOURCE;
        uint64_t fence[kFrames] = {};
        uint64_t frame[kFrames] = {};
        uint64_t delivered = 0;
        uint32_t gw = 0;
        uint32_t gh = 0;
        uint32_t row_pitch = 0;
        uint32_t next = 0;
    };
    std::unordered_map<uint32_t, Late> late_;
    std::mutex late_lock_;                      // DeliverLate runs on the title's thread
    std::atomic<uint64_t> presents_seen_{0};    // presents_ for DeliverLate
    std::atomic<uint64_t> present_fence_[kFrames + 1] = {};  // by presents_, for WaitGpuFrame
    Assembled asm_;  // kept between draws for its buffers
    // visibility tests: an occlusion query per counted draw, summed into report memory once the GPU is done
    struct Query {
        uint32_t index;
        float samples_per_pixel;  // host samples one console pixel covers
    };
    struct Report {
        uint32_t va;
        std::vector<Query> queries;
        uint64_t fence;
    };
    ComPtr<ID3D12QueryHeap> query_heap_;
    ComPtr<ID3D12Resource> query_rb_;
    const uint64_t* query_counts_ = nullptr;
    uint32_t query_next_ = 0;
    std::vector<Query> counted_;  // since the last ClearReport
    std::deque<Report> reports_;
    bool tearing_ = false;
    uint32_t refresh_hz_ = 0, refresh_said_ = UINT32_MAX;  // the screen's, as frame generation's vsync last found it
    uint64_t sped_frames_ = 0;  // frames drawn while the game is sped up (app::Speed): one of each so many shown
    uint64_t refresh_at_ = 0;                                // the present it is looked at again
    // HDR output: the window's display in Windows HDR mode (asked of a factory made again when DXGI says the displays
    // changed), the swap chain in scRGB, the UI drawn apart and laid over the frame at scene brightness
#ifdef _WIN32
    ComPtr<IDXGIFactory1> display_factory_;
    HMONITOR display_monitor_ = nullptr;
#endif
    bool display_hdr_ = false;
    float display_peak_ = 0;
    bool display_logged_ = false;
    bool hdr_ = false;
    bool hdr_failed_ = false;  // the swap chain refused scRGB: SDR for the run
    bool hdr10_ = false;       // HDR as HDR10, for frame generation that takes no scRGB (framegen::FrameGen::Hdr10)
    float hdr_cb_[4] = {};
    ComPtr<ID3D12PipelineState> hdr_psos_[4];  // the frame's and the UI's, into scRGB and into HDR10
    DXGI_FORMAT HdrFormat() const { return hdr10_ ? kHdr10 : kHdr; }
    DXGI_FORMAT BackFormat() const { return hdr_ ? HdrFormat() : kBgra; }
    uint64_t fps_shown_from_ = 0;  // frame generation's Shown() at the start of the FPS count's half second
    ComPtr<ID3D12Resource> ui_;
    D3D12_RESOURCE_STATES ui_state_ = D3D12_RESOURCE_STATE_RENDER_TARGET;
    uint32_t ui_w_ = 0;
    uint32_t ui_h_ = 0;
    // the frame before its UI (Extension::UiStart): the target it was copied from, the present it is of, how many
    // copies that frame took; hdr_before_ is it while the frame is drawn to the back buffer
    ComPtr<ID3D12Resource> ui_snap_;
    D3D12_RESOURCE_STATES ui_snap_state_ = D3D12_RESOURCE_STATE_COPY_DEST;
    uint32_t ui_snap_data_ = 0;
    uint64_t ui_snap_frame_ = UINT64_MAX;
    uint32_t ui_snaps_ = 0;
    uint32_t ui_snaps_shown_ = 0;
    ID3D12Resource* hdr_before_ = nullptr;
    // the frame's average brightness: shrunk to 16 x 16, then to one texel
    ComPtr<ID3D12Resource> level16_;
    ComPtr<ID3D12Resource> level1_;
    D3D12_RESOURCE_STATES level16_state_ = D3D12_RESOURCE_STATE_RENDER_TARGET;
    D3D12_RESOURCE_STATES level1_state_ = D3D12_RESOURCE_STATE_RENDER_TARGET;
    // frame generation (framegen.h): the vendor's, the depth buffer drawn with the back buffer this frame (and the
    // present it was bound for), the inputs at the frame's size on the back buffer, the HUD-less
    // frame one per frame in flight (read as late as its present)
    std::unique_ptr<framegen::FrameGen> fg_;
    Target* fg_ds_ = nullptr;  // targets_' element, dropped with it (ReleaseTarget)
    uint64_t fg_ds_frame_ = UINT64_MAX;
    ComPtr<ID3D12Resource> fg_depth_, fg_motion_, fg_hudless_[kFrames];
    D3D12_RESOURCE_STATES fg_depth_state_ = D3D12_RESOURCE_STATE_RENDER_TARGET;
    D3D12_RESOURCE_STATES fg_motion_state_ = D3D12_RESOURCE_STATE_RENDER_TARGET;
    D3D12_RESOURCE_STATES fg_hudless_state_[kFrames] = {};
    uint32_t fg_w_ = 0, fg_h_ = 0, fg_bb_w_ = 0, fg_bb_h_ = 0;
    DXGI_FORMAT fg_bb_fmt_ = DXGI_FORMAT_UNKNOWN;
    bool fg_had_view_ = false;
    // frame generation's motion per 3D draw: the frame target's own motion, which 3D draws that write depth draw as
    // their second target (PrevPositionHlsl, MotionOut) from their vertex constants and the same draw's in the frame
    // before; pixels none drew keep kNoMotion and get the camera's motion from depth
    ComPtr<ID3D12Resource> fg_objmv_;
    D3D12_RESOURCE_STATES fg_objmv_state_ = D3D12_RESOURCE_STATE_RENDER_TARGET;
    uint32_t fg_objmv_w_ = 0, fg_objmv_h_ = 0;
    uint64_t fg_objmv_frame_ = UINT64_MAX;  // the frame it was cleared for, and drawn into
    bool bound_mv_ = false;                 // bound as the second target with bound_rt_
    bool bound_rx_ = false;                 // anti-aliasing's reactive mask bound as the third
    uint64_t draw_mv_key_ = 0;              // the draw at hand's MotionKey
    bool draw_texbox_ = false;              // the draw at hand's PixelKey::texbox, its DrawConstants::uvbox
    float draw_uvbox_[4] = {};
    // each 3D draw's vertex constants as uploaded, this frame's and the frame before's, and where they are by
    // MotionKey (several for a mesh drawn more than once: two of the same enemy)
    struct MotionConstants {
        float c[192][4];
    };
    std::unordered_map<uint64_t, std::vector<uint32_t>> mv_now_at_, mv_prev_at_;
    std::vector<MotionConstants> mv_now_, mv_prev_;
    uint64_t mv_frame_ = UINT64_MAX;
    bool mv_said_ = false;
    std::chrono::steady_clock::time_point fg_t_ = std::chrono::steady_clock::now();
    // anti-aliasing (g_antialias): AMD FSR or NVIDIA DLSS, its 3D moved by a sub-pixel offset each frame; each kind
    // made the first time it is chosen
    std::unique_ptr<framegen::AntiAlias> aa_fsr_, aa_dlss_;
    framegen::AntiAlias* aa_ = nullptr;  // the one running
    bool aa_on_ = false, aa_fsr_tried_ = false, aa_dlss_tried_ = false, aa_had_view_ = false, aa_said_ = false;
    bool aa_dlss_start_ = false;        // DLSS chosen as the device was made: Streamline loaded for it
    bool draw_jitter_ = false;          // the draw at hand takes the offset (Extension::Jitter)
    float aa_jitter_[2] = {};           // this frame's, in pixels of the size drawn
    uint32_t aa_jitter_w_ = 0, aa_jitter_h_ = 0;  // that size
    uint64_t aa_jitter_frame_ = UINT64_MAX;  // the frame a draw last took it
    uint64_t aa_done_frame_ = UINT64_MAX;    // the frame last anti-aliased
    uint64_t aa_ran_frame_ = UINT64_MAX;     // the frame the vendor's pass last ran on
    // upscaling (g_upscale): the scene (OpenScene) and its part of the target, w x h at the top left
    uint32_t up_quality_ = 0;           // framegen::Upscale, as applied
    Target* scene_ = nullptr;           // the target drawn at the part's size, null when none is
    uint64_t scene_frame_ = UINT64_MAX; // the frame a scene was last opened in, or found not to be
    uint64_t scene_closed_frame_ = UINT64_MAX;  // the frame one last closed in
    bool scene_next_ = false;           // the next frame's 3D in a scene: this frame's was anti-aliased
    uint32_t scene_w_ = 0, scene_h_ = 0;
    float scene_fx_ = 1, scene_fy_ = 1;  // its share of the target, across and down
    uint32_t scene_draws_ = 0, scene_late_ = 0, scene_logged_ = 0;  // a frame's draws in the scene, its 3D after it
    uint32_t char_in_ = 0, char_late_ = 0, char_nodepth_ = 0, char_other_ = 0, char_prev_ = 0;  // XBE_RT_CHAR_LOG
    uint32_t char_faded_ = 0, char_copies_ = 0, char_said_ = 0;                                   // XBE_RT_CHAR_LOG
    // the characters' meshes the frame's scene took (CharacterMesh), and whether by a draw writing no depth
    std::unordered_map<uint64_t, bool> char_meshes_;
    // the graphics card's memory against its budget (MemoryCheck), every few seconds
    void MemoryCheck();
    // texture packs' pictures not drawn from in a while let go, the longer unused first, so they don't fill the
    // graphics card's memory room after room; used gets the bytes in use and the budget; returns the bytes let go
    uint64_t TrimReplacements(uint64_t used, uint64_t budget);
    uint64_t trim_freed_ = 0, trim_count_ = 0;  // all let go this run, and how many
#ifdef _WIN32
    ComPtr<IDXGIAdapter3> mem_adapter_;
#endif
    bool mem_tried_ = false, mem_over_ = false;
    uint64_t mem_frame_ = 0, mem_most_ = 0, mem_report_ = 0;
    uint32_t scene_said_w_ = 0, scene_said_h_ = 0;  // the size last logged
    ComPtr<ID3D12Resource> scene_tmp_;  // StretchScene's copy of the part
    D3D12_RESOURCE_STATES scene_tmp_state_{};
    ComPtr<ID3D12Resource> aa_depth_, aa_motion_, aa_out_;
    // its reactive mask (EnsureReactive): R8 at the frame target's size, the frame's effects' share of each pixel
    ComPtr<ID3D12Resource> aa_rx_;
    // Linux's AMD FSR on Vulkan: the frame's colour and effects copied into images of its own (VulkanImages); the
    // targets above made as its own images too when aa_vk_
    ComPtr<ID3D12Resource> aa_vk_color_, aa_vk_rx_;
    D3D12_RESOURCE_STATES aa_vk_color_state_{}, aa_vk_rx_state_{};
    bool aa_vk_ = false;
    D3D12_RESOURCE_STATES aa_rx_state_{};
    uint32_t aa_rx_w_ = 0, aa_rx_h_ = 0;
    uint64_t aa_rx_frame_ = UINT64_MAX;  // the frame it was cleared for
    uint32_t rx_draws_ = 0;  // this frame's draws into it
    // ambient occlusion's target (R8, the frame target's size, the part drawn at its quality's), the view it last had
    ComPtr<ID3D12Resource> ao_;
    D3D12_RESOURCE_STATES ao_state_{};
    uint32_t ao_w_ = 0, ao_h_ = 0;
    FrameView ao_view_;
    bool ao_view_ok_ = false;
    uint32_t ao_said_ = 0;  // how ambient occlusion was last logged: 1 under anti-aliasing, 2 without
    bool ao_alone_ = false;                 // ambient occlusion on, no anti-aliasing running (ApplySettings)
    // the own TAA's histories at the output's size (RGBA16F), drawn into by turns: the one taa_cur_ says is the last
    ComPtr<ID3D12Resource> taa_[2];
    D3D12_RESOURCE_STATES taa_state_[2]{};
    uint32_t taa_w_ = 0, taa_h_ = 0, taa_cur_ = 0;
    bool taa_kept_ = false;  // the last frame's history is there to gather from
    uint64_t ao_done_frame_ = UINT64_MAX;   // the frame OcclusionAlone last ran for
    // ray traced bounce lighting's target (RGBA16F, a part of the frame target's size: Lighting)
    ComPtr<ID3D12Resource> gi_;
    D3D12_RESOURCE_STATES gi_state_{};
    uint32_t gi_w_ = 0, gi_h_ = 0;
    bool gi_said_ = false;
    // the lights' points kept for each bounce texel (rt_gi.hlsl's second target, RGBA32F at the bounce target's size),
    // this frame's and the frame before's by the frame's number, and the last frame that wrote one
    ComPtr<ID3D12Resource> rs_[kRs];
    D3D12_RESOURCE_STATES rs_state_[kRs] = {};
    uint32_t rs_w_ = 0, rs_h_ = 0;
    uint64_t rs_frame_ = UINT64_MAX;
    uint32_t rs_gw_ = 0, rs_gh_ = 0;  // the part of them drawn (the bounce target's), for ambient occlusion's lamps
    bool rs_said_ = false;
    bool EnsureReservoirs(uint32_t tw, uint32_t th);
    // the eye's hits (kGuideHit) are this frame's, at the target's size: the passes read them rather than tracing
    bool EyeHits(const Target& t) const {
        return guide_frame_ == presents_ && guide_[kGuideHit] && guide_w_ == t.w && guide_h_ == t.h;
    }
    // the glows remembered off screen (rt_glowmem.hlsl, RGBA32F): the listed lights' summary (128 x 3), the slots this
    // frame and the frame before (kGlowSlots x 5, by the frame's number), their first pass and the summaries' pair,
    // the last frame that wrote them
    ComPtr<ID3D12Resource> glow_[kGlow];
    D3D12_RESOURCE_STATES glow_state_[kGlow] = {};
    uint64_t glow_frame_ = UINT64_MAX;
    bool glow_said_ = false;
    bool EnsureGlowTargets();
    // the glows known frame to frame (GlowMemory, rt_glowmem.hlsl), this frame's: the SRV indices of the summary and
    // of the slots (-1 for none), the frame they were made for
    float glow_summary_srv_ = -1.0f, glow_slots_srv_ = -1.0f;
    uint64_t glow_done_frame_ = UINT64_MAX;
    void GlowMemory(uint32_t rw, uint32_t rh);
    // ray tracing's guides (Guides, rt_guide.hlsl) at the frame target's size, drawn once a frame: the surfaces' albedo
    // (RGBA8), world normals and roughness (RGBA16F), specular albedo (RGBA8) and the reflections' hit distance (R16F),
    // and the view space their normals were turned into the world from (the view's, as its projection has it: x, y, z
    // and the camera's place, rows)
    // after the four, each pixel's surface (world normal, view depth: RGBA16F) this frame and the frame before, by the
    // frame's number (SurfaceNow, SurfaceBefore)
    ComPtr<ID3D12Resource> guide_[kGuides + kGuideExtra];
    D3D12_RESOURCE_STATES guide_state_[kGuides + kGuideExtra] = {};
    uint64_t surface_frame_[2] = {UINT64_MAX, UINT64_MAX};
    uint32_t SurfaceNow() const { return kGuides + uint32_t(presents_ & 1); }
    uint32_t SurfaceBefore() const { return kGuides + uint32_t((presents_ & 1) ^ 1); }
    bool HaveSurfaceBefore() const { return surface_frame_[(presents_ & 1) ^ 1] + 1 == presents_; }
    // the reflections' and ambient occlusion's own denoiser (Reflections, AmbientOcclusion: OwnDenoiser), at the frame
    // target's size: rd_ 0 the reflections alone, 1 and 2 their history, 3 and 4 the frames it holds, 5 their
    // smoothing's, 6 and 7 the ambient occlusion's history; each one's last frame gathered and pair written last
    ComPtr<ID3D12Resource> rd_[kRd];
    D3D12_RESOURCE_STATES rd_state_[kRd] = {};
    uint32_t rd_w_ = 0, rd_h_ = 0, refl_cur_ = 0, aodn_cur_ = 0;
    uint64_t refl_frame_ = UINT64_MAX, aodn_frame_ = UINT64_MAX;
    bool refl_dn_said_ = false, aodn_said_ = false;
    bool EnsureOwnDenoiseTargets(uint32_t w, uint32_t h);
    // the bounce lighting's room average (rt_gimean.hlsl): its levels for a bounce target of w x h, the eased value
    // written last, the frame it was; RoomBounce: this frame's eased average, its SRV slot, -1 when there is none
    ComPtr<ID3D12Resource> mean_[kMean];
    D3D12_RESOURCE_STATES mean_state_[kMean] = {};
    uint32_t mean_w_[4] = {}, mean_h_[4] = {}, mean_cur_ = 0, mean_for_w_ = 0, mean_for_h_ = 0;
    uint64_t mean_frame_ = UINT64_MAX;
    float RoomBounce(uint32_t bounce_slot, uint32_t gw, uint32_t gh, uint32_t tw, uint32_t th);
    uint32_t LightList(float lt[4], float sk[4], uint32_t list[][4]);
    // XBE_RT_LIGHTS_FLAME: the least area a flame drawn flat (a torch's, a candle's sprite) lights as, up to 16 times
    // its own (rt_lights.hlsli), as a float's bits for the lights' list (8000: 5.8's torch warmed its wall by a third,
    // the room some 15% against RT off's; 30000 washed the room out; 0: as its sprite alone)
    // how far a light's light reaches (XBE_RT_LIGHTS_REACH, 2500 view units, the bounce rays' own reach; 0: no end),
    // a float's bits for the list's fourth column: eased to nothing toward it (rt_lights.hlsli's Reach). Without it a
    // lamp in view lit the mountains across the valley behind it, its light only falling with the distance squared
    static uint32_t LightReach() {
        static const float kReach = std::getenv("XBE_RT_LIGHTS_REACH") ? float(std::atof(std::getenv("XBE_RT_LIGHTS_REACH")))
                                                                       : 2500.0f;
        uint32_t bits;
        std::memcpy(&bits, &kReach, sizeof(bits));
        return bits;
    }
    // XBE_RT_LIGHTS_STEADY: the frames in a row a glow is there (by where it is, GlowMemory; by its picture with
    // XBE_RT_LIGHTS_STEADY_CPU) before it is a light (20, a third of a second); XBE_RT_LIGHTS_RAMP: the frames its
    // light then grows in over (20: a lamp lights up as the room comes in, a sword's trail that lasts lights the floor
    // by Hector a little, not at once)
    static uint32_t SteadyFrames() {
        static const uint32_t k = std::getenv("XBE_RT_LIGHTS_STEADY") ? uint32_t(std::atoi(std::getenv("XBE_RT_LIGHTS_STEADY")))
                                                                     : 20u;
        return k;
    }
    static uint32_t RampFrames() {
        static const uint32_t k = std::getenv("XBE_RT_LIGHTS_RAMP") ? uint32_t(std::atoi(std::getenv("XBE_RT_LIGHTS_RAMP")))
                                                                   : 20u;
        return k;
    }
    static uint32_t FlameArea() {
        static const float kFlame = std::getenv("XBE_RT_LIGHTS_FLAME") ? float(std::atof(std::getenv("XBE_RT_LIGHTS_FLAME")))
                                                                      : 8000.0f;
        uint32_t bits;
        std::memcpy(&bits, &kFlame, sizeof(bits));
        return bits;
    }
    bool EnsureMeanTargets(uint32_t tw, uint32_t th);
    float SkyLight(Target& t, uint32_t rw, uint32_t rh, uint32_t tw, uint32_t th);
    // a 2D texture's view at an SRV heap slot
    void TextureSrv(ID3D12Resource* r, DXGI_FORMAT fmt, uint32_t slot) {
        D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = fmt;
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.Texture2D.MipLevels = 1;
        dev_->CreateShaderResourceView(r, &sv, Cpu(srv_heap_.Get(), slot, srv_inc_));
    }
    // ray traced ambient occlusion's own denoiser (rt_aotemporal.hlsl): this frame's (ao_, its aw x ah part) gathered
    // into its history; the history's resource then, null when it did not run
    ID3D12Resource* GatherAmbientOcclusion(uint32_t rw, uint32_t rh, uint32_t aw, uint32_t ah);
    // ray traced reflections denoised by the port's own (rt_refltemporal.hlsl, rt_reflatrous.hlsl) and drawn over the
    // frame; false when they could not be (drawn straight over it then)
    bool DenoisedReflections(Target& t, uint32_t rw, uint32_t rh, D3D12_GPU_VIRTUAL_ADDRESS rcb, uint32_t depth_slot,
                             float rough_limit);
    // the port's own denoiser runs: no vendor's denoiser takes the frame (XBE_RT_DENOISE 0 never, 2 always)
    bool OwnDenoiser() const {
        static const int kDenoise = std::getenv("XBE_RT_DENOISE") ? std::atoi(std::getenv("XBE_RT_DENOISE")) : 1;
        return kDenoise == 2 || (kDenoise == 1 && !(aa_ && aa_->Denoises()));
    }
    uint32_t guide_w_ = 0, guide_h_ = 0;
    uint64_t guide_frame_ = UINT64_MAX;
    float guide_view_[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
    // the bounce lighting's own denoiser (Denoise, when the anti-aliasing does not denoise): history 0 and 1, moments 2
    // and 3, surfaces 4 and 5 (dn_cur_ the pair written last), its smoothing's 6 and 7, all RGBA16F at the bounce
    // target's size; the frame it last ran, whether this frame has anything to gather from (AntiAliasFrame) and the
    // camera's move
    ComPtr<ID3D12Resource> dn_[kDn];
    D3D12_RESOURCE_STATES dn_state_[kDn] = {};
    uint32_t dn_w_ = 0, dn_h_ = 0, dn_cur_ = 0;
    uint64_t dn_frame_ = UINT64_MAX;
    bool dn_said_ = false, rt_reset_ = true;
    // each picture drawn as a glow: the frame it was last, and how many frames in a row (a light only when steady)
    struct GlowSeen {
        uint64_t frame = UINT64_MAX - 1;
        uint32_t streak = 0, said = 0;
        uint64_t count_frame = UINT64_MAX;  // its draws as a glow this frame, the frame they were counted in
        uint32_t count = 0;
    };
    std::unordered_map<const void*, GlowSeen> glow_seen_;     // by the renderer's copy: the draws a frame
    std::unordered_map<uint64_t, GlowSeen> glow_steady_;        // by the title's texture data: how steady
    std::unordered_set<const void*> glow_logged_;  // XBE_RT_LIGHTS_LOG's blended pictures
    std::unordered_set<uint32_t> char_kinds_;  // XBE_RT_CHAR_LOG's kinds of character draws writing no depth
    std::unordered_set<const void*> cutout_said_;  // XBE_RT_CUTOUT_LOG's pictures
    float rt_clip_to_prev_[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    bool rr_said_ = false, guide_said_ = false;
    // ray tracing (raytrace.h): the frame's scene, whether this frame gathers one, the capture constants bound (rtc:
    // first slot, on), a buffer bound in the capture's place when there is no ray tracing
    rt::Scene rt_;
    bool rt_frame_ = false, rt_said_ = false, refl_said_ = false;
    uint32_t rt_bound_[2] = {UINT32_MAX, UINT32_MAX};
    ComPtr<ID3D12Resource> rt_dummy_;
    std::vector<uint32_t> rt_ix_;  // a captured draw's indices, its base vertex added
    void BindRtCapture();

    bool rx_said_ = false;
    // XBE_PERF: the GPU's time by part of the frame, timestamps (GpuMark) at each part's start, read back when the
    // frame's slot is used again
    ComPtr<ID3D12QueryHeap> ts_heap_;
    ComPtr<ID3D12Resource> ts_rb_;
    uint64_t* ts_map_ = nullptr;
    double ts_ms_ = 0;                  // milliseconds a tick
    bool ts_failed_ = false;
    uint32_t ts_marked_[kFrames] = {};  // the marks written for each slot's frame
    bool ts_resolved_[kFrames] = {};
    D3D12_RESOURCE_STATES aa_depth_state_{}, aa_motion_state_{}, aa_out_state_{};
    uint32_t aa_w_ = 0, aa_h_ = 0;
    DXGI_FORMAT aa_fmt_ = DXGI_FORMAT_UNKNOWN, aa_out_fmt_ = DXGI_FORMAT_UNKNOWN;
    std::chrono::steady_clock::time_point aa_t_ = std::chrono::steady_clock::now();
    uint64_t presents_ = 0;
    double fps_ = 0;
    std::chrono::steady_clock::time_point fps_t_ = std::chrono::steady_clock::now();
    uint64_t fps_n_ = 0;
};

bool D3D12Backend::Init(uint32_t width, uint32_t height) {
    g_reversed_depth = true;
    guest_w_ = width;
    guest_h_ = height;
    if (!CreateDevice()) return false;
#ifdef _WIN32
    if (!CreateSwapChain(static_cast<HWND>(app::Window()))) return false;
#else
    if (!CreateSwapChain()) return false;
#endif
    CheckDisplay();
    CreateHeaps();
    CreateBackBufferViews();
    if (!CreateRootSignature()) return false;
    CreateFrames();
    CreateGammaLut();
    if (!rt_.Init(dev_.Get(), cl_.Get(), kFrames)) {
        // the capture's root parameter bound to a buffer that is never written
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC bd = BufferDesc(256);
        bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                      IID_PPV_ARGS(&rt_dummy_));
    }
    rt_.Begin(uint32_t(frame_ % kFrames));
    // the pipelines listed made ahead, now the root signatures they are made with exist
    StartPipelinePrebuild(dev_.Get());
    ImGui_ImplDX12_Init(dev_.Get(), kFrames, DXGI_FORMAT_B8G8R8A8_UNORM, ui_heap_.Get(),
                        ui_heap_->GetCPUDescriptorHandleForHeapStart(), ui_heap_->GetGPUDescriptorHandleForHeapStart());
    LoadSettings();
    Begin();
    LOGI("d3d12: %ux%u guest, %ux%u window, render scale %.3f", width, height, bb_w_, bb_h_, scale_);
    return true;
}

#ifndef _WIN32
// Linux: vkd3d's device on the Vulkan device the settings name, else the fastest (linux/present.cpp)
bool D3D12Backend::CreateDevice() {
    // NVIDIA DLSS's files, when the player has them: the Vulkan instance and device made with what it asks for, so it can be
    // chosen while playing
    linux_dlss::Load();
    std::string name;
    if (!linux_present::CreateDevice(app::Get().gpu, dev_.ReleaseAndGetAddressOf(), name)) {
        LOGE("d3d12: no device");
        return false;
    }
    LOGR("D3D12 adapter: %s (vkd3d on Vulkan)", name.c_str());
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    dev_->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue_));
    tearing_ = true;  // a swap chain that is not waited on: IMMEDIATE or MAILBOX where the window system has one
    hdr10_ = true;    // HDR as HDR10, the HDR format Linux's compositors take (scRGB only some)
    return true;
}

bool D3D12Backend::CreateSwapChain() {
    bb_w_ = std::max<uint32_t>(linuxapp::g_pixel_w, 1);
    bb_h_ = std::max<uint32_t>(linuxapp::g_pixel_h, 1);
    swap_ = std::make_unique<linux_present::Swap>();
    if (!swap_->Create(dev_.Get(), queue_.Get(), static_cast<SDL_Window*>(app::Window()), kFrames, bb_w_, bb_h_)) {
        LOGE("d3d12: no swap chain");
        return false;
    }
    return true;
}
#else
// XBE_D3D12_DEBUG: the debug layer's errors into the log (the first 64)
void CALLBACK DebugMessage(D3D12_MESSAGE_CATEGORY, D3D12_MESSAGE_SEVERITY severity, D3D12_MESSAGE_ID id,
                           LPCSTR text, void*) {
    static std::atomic<int> said{0};
    if (severity > D3D12_MESSAGE_SEVERITY_ERROR || said.fetch_add(1) >= 64) return;
    LOGW("d3d12 debug layer (%d): %s", int(id), text ? text : "");
}

bool D3D12Backend::CreateDevice() {
    if (GetEnvironmentVariableW(L"XBE_D3D12_DEBUG", nullptr, 0)) {
        ComPtr<ID3D12Debug> dbg;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg)))) dbg->EnableDebugLayer();
    }
    // XBE_D3D12_DRED: what the GPU was doing when the device was lost, and the address it faulted on (DeviceLost)
    if (GetEnvironmentVariableW(L"XBE_D3D12_DRED", nullptr, 0)) {
        ComPtr<ID3D12DeviceRemovedExtendedDataSettings> dred;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dred)))) {
            dred->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            dred->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        }
    }
    // frame generation's libraries first, and Streamline for DLSS's: it must see the factory and device made
    uint32_t fg = app::Get().frame_generation;
    aa_dlss_start_ = g_antialias.load(std::memory_order_relaxed) == framegen::kAaDlss;
    framegen::LoadStreamline(fg == framegen::kDlss, aa_dlss_start_);
    if (fg) {
        fg_ = framegen::Make(fg);
        if (!fg_) LOGR("framegen: %s is off for this run", framegen::KindName(fg));
        hdr10_ = fg_ && fg_->Hdr10();
    }
    // Streamline's factory for its frame generation or DLSS alone (not under AMD's frame generation's swap chain)
    bool sl_factory = (fg == framegen::kDlss || !fg_) && framegen::StreamlineFactory(factory_);
    if (!sl_factory && !(fg_ && fg_->CreateFactory(factory_)) && FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory_))))
        return false;
    // the graphics card the settings name, else the fastest: the dedicated GPU first on hybrid laptops
    ComPtr<IDXGIFactory6> f6;
    ComPtr<IDXGIAdapter1> adapter;
    if (const std::string& want = app::Get().gpu; !want.empty()) {
        ComPtr<IDXGIAdapter1> a;
        for (UINT i = 0; !adapter && factory_->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; i++) {
            DXGI_ADAPTER_DESC1 d{};
            a->GetDesc1(&d);
            char name[sizeof(d.Description)];
            WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, name, sizeof(name), nullptr, nullptr);
            if (want == name && SUCCEEDED(D3D12CreateDevice(a.Get(), D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), nullptr)))
                adapter = a;
        }
        if (!adapter) LOGR("D3D12: no graphics card %s with Direct3D 12 here, the fastest one instead", want.c_str());
    }
    if (!adapter && SUCCEEDED(factory_.As(&f6))) {
        f6->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter));
    }
    if (!framegen::StreamlineDevice(adapter.Get(), dev_) && !(fg_ && adapter && fg_->CreateDevice(adapter.Get(), dev_)) &&
        FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev_))) &&
        FAILED(D3D12CreateDevice((adapter = nullptr).Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev_)))) {
        LOGE("d3d12: no device");
        return false;
    }
    DXGI_ADAPTER_DESC1 ad{};
    if (adapter) adapter->GetDesc1(&ad);
    LOGR("D3D12 adapter: %ls", adapter ? ad.Description : L"default");
    if (GetEnvironmentVariableW(L"XBE_D3D12_DEBUG", nullptr, 0)) {
        ComPtr<ID3D12InfoQueue1> iq;
        DWORD cookie = 0;
        if (SUCCEEDED(dev_.As(&iq)))
            iq->RegisterMessageCallback(DebugMessage, D3D12_MESSAGE_CALLBACK_FLAG_NONE, nullptr, &cookie);
    }
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    dev_->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue_));
    framegen::StreamlineDeviceReady(dev_.Get());
    if (fg_) fg_->DeviceReady(dev_.Get(), queue_.Get());
    ComPtr<IDXGIFactory5> f5;
    BOOL tear = FALSE;
    if (SUCCEEDED(factory_.As(&f5))) f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tear, sizeof(tear));
    tearing_ = tear;
    return true;
}

// frame generation with vsync: the game draws 60 frames a second and one is made between each two, so presents that
// wait for the screen fit one whose refresh is a multiple of 120. At another (60, 144, 165 Hz) AMD's swap chain holds
// the game under its 60 to keep the screen's pace (42 at 165 Hz), and its play slows with it, its logic a step a frame;
// NVIDIA's wants the game's own vsync off. Such a screen's frames go without waiting then, which a variable refresh
// screen (G-SYNC, FreeSync) shows without tearing. The screen looked at again every 2 seconds: the window may move
bool D3D12Backend::FrameGenVsync() {
    if (presents_ >= refresh_at_) {
        refresh_at_ = presents_ + 120;
        refresh_hz_ = 0;
        HWND hwnd = static_cast<HWND>(app::Window());
        MONITORINFOEXW mi{};
        mi.cbSize = sizeof(mi);
        DEVMODEW dm{};
        dm.dmSize = sizeof(dm);
        if (hwnd && GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi) &&
            EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1)
            refresh_hz_ = dm.dmDisplayFrequency;
    }
    // 119 to 121 Hz, 239 to 241 and so on (119.88 Hz reads 119)
    bool fits = fg_->Kind() == framegen::kFsr && refresh_hz_ >= 119 && (refresh_hz_ + 1) % 120 <= 2;
    if (refresh_hz_ != refresh_said_) {
        refresh_said_ = refresh_hz_;
        if (fits)
            LOGR("framegen: the screen at %u Hz, presented with vsync", refresh_hz_);
        else if (fg_->Kind() == framegen::kFsr)
            LOGR("framegen: the screen at %u Hz, not a multiple of 120: presented without vsync, so the game keeps its "
                 "60 (a variable refresh screen shows it without tearing)", refresh_hz_);
        else
            LOGR("framegen: %s presented without the game's vsync, as NVIDIA asks (the screen at %u Hz)",
                 framegen::KindName(fg_->Kind()), refresh_hz_);
    }
    return fits;
}

bool D3D12Backend::CreateSwapChain(HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    bb_w_ = std::max<LONG>(rc.right - rc.left, 1);
    bb_h_ = std::max<LONG>(rc.bottom - rc.top, 1);
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = bb_w_;
    sd.Height = bb_h_;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = kFrames;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.Flags = tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    // FSR 3's frame interpolation swap chain in its place
    if (fg_ && fg_->CreateSwapChain(factory_.Get(), queue_.Get(), hwnd, sd, swap_)) {
        factory_->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
        return true;
    }
    ComPtr<IDXGISwapChain1> sc1;
    if (FAILED(factory_->CreateSwapChainForHwnd(queue_.Get(), hwnd, &sd, nullptr, nullptr, &sc1))) {
        LOGE("d3d12: no swap chain");
        return false;
    }
    factory_->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    sc1.As(&swap_);
    return true;
}
#endif

uint32_t D3D12Backend::CreateHeap(D3D12_DESCRIPTOR_HEAP_TYPE t, uint32_t n, bool visible,
                                  ComPtr<ID3D12DescriptorHeap>& out) {
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = t;
    hd.NumDescriptors = n;
    hd.Flags = visible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    HRESULT hr = dev_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&out));
    if (FAILED(hr))
        LOGE("d3d12: descriptor heap %u x %u (visible %d) failed %08X", uint32_t(t), n, visible ? 1 : 0, unsigned(hr));
    return dev_->GetDescriptorHandleIncrementSize(t);
}

void D3D12Backend::CreateBackBufferViews() {
    for (int i = 0; i < kFrames; i++) {
        swap_->GetBuffer(i, IID_PPV_ARGS(&bb_[i]));
        dev_->CreateRenderTargetView(bb_[i].Get(), nullptr, Cpu(rtv_heap_.Get(), kRtvCount + i, rtv_inc_));
    }
}

void D3D12Backend::CreateHeaps() {
    // past the ring a few spare, so a view table near its end reaches no further than the heap
    srv_inc_ = CreateHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kSrvCount + kMaterialViews, true, srv_heap_);
    smp_inc_ = CreateHeap(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, kSamplerCount, true, smp_heap_);
    rtv_inc_ = CreateHeap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kTaaRtv + 2, false, rtv_heap_);
    dsv_inc_ = CreateHeap(D3D12_DESCRIPTOR_HEAP_TYPE_DSV, kDsvCount, false, dsv_heap_);
    CreateHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, true, ui_heap_);
    for (uint32_t i = kRtvCount; i-- > 0;) free_rtv_.push_back(i);
    for (uint32_t i = kDsvCount; i-- > 0;) free_dsv_.push_back(i);
}

bool D3D12Backend::CreateRootSignature() {
    D3D12_DESCRIPTOR_RANGE ranges[2] = {};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = kMaterialViews;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
    ranges[1].NumDescriptors = 4;
    D3D12_ROOT_PARAMETER params[6] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[1].Descriptor.ShaderRegister = 1;
    for (int i = 0; i < 2; i++) {
        params[2 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[2 + i].DescriptorTable.NumDescriptorRanges = 1;
        params[2 + i].DescriptorTable.pDescriptorRanges = &ranges[i];
        params[2 + i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }
    // ray tracing's capture (raytrace.h, kRtCapture): the frame's buffer (u0) and the draw's first slot and switch (b2)
    params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    params[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[5].Constants.ShaderRegister = 2;
    params[5].Constants.Num32BitValues = 4;
    params[5].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.NumParameters = 6;
    rsd.pParameters = params;
    rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ComPtr<ID3DBlob> rs;
    ComPtr<ID3DBlob> err;
    if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rs, &err))) return false;
    dev_->CreateRootSignature(0, rs->GetBufferPointer(), rs->GetBufferSize(), IID_PPV_ARGS(&root_));
    if (root_) RegisterRootSignature(root_.Get(), rs->GetBufferPointer(), rs->GetBufferSize());
    return true;
}

void D3D12Backend::CreateFrames() {
    for (auto& f : frames_) {
        dev_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&f.alloc));
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_UPLOAD};
        D3D12_RESOURCE_DESC bd = BufferDesc(kUploadSize);
        dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                      IID_PPV_ARGS(&f.upload));
        f.upload->Map(0, nullptr, reinterpret_cast<void**>(&f.map));
    }
    dev_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_));
#ifdef _WIN32
    event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
#else
    event_ = linux_present::CreateEvent();
#endif
    dev_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, frames_[0].alloc.Get(), nullptr, IID_PPV_ARGS(&cl_));
}

// gamma ramp as a 256 x 1 lookup
void D3D12Backend::CreateGammaLut() {
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC td = Texture2DDesc(DXGI_FORMAT_B8G8R8A8_UNORM, 256, 1);
    dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                  IID_PPV_ARGS(&lut_));
}

// widescreen's targets are wider by the screen's shape over the console's 4:3 (aspect_)
float TargetAspect() { return app::Widescreen() ? app::WideAspect() / (4.0f / 3.0f) : 1.0f; }

void D3D12Backend::LoadSettings() {
    scale_ = app::RenderScale(guest_h_, aa_dlss_start_);
    aspect_ = TargetAspect();
    texturedumper::Update();
    ext_ = g_extension.get();
    if (ext_) {
        ext_->Settings();
        ext_->Reset(*this);
    }
}

void D3D12Backend::Begin() {
    ID3D12DescriptorHeap* heaps[] = {srv_heap_.Get(), smp_heap_.Get()};
    cl_->SetDescriptorHeaps(2, heaps);
    cl_->SetGraphicsRootSignature(root_.Get());
    BindRtCapture();
    bound_rt_ = bound_ds_ = nullptr;
    binds_ = {};
}

// the root signature's ray tracing capture parameters: the frame's buffer, and the capture off
void D3D12Backend::BindRtCapture() {
    D3D12_GPU_VIRTUAL_ADDRESS cap = rt_.Ready() ? rt_.Capture() : rt_dummy_ ? rt_dummy_->GetGPUVirtualAddress() : 0;
    if (cap) cl_->SetGraphicsRootUnorderedAccessView(4, cap);
    const uint32_t off[4] = {0, 0, 0, 0};
    cl_->SetGraphicsRoot32BitConstants(5, 4, off, 0);
    rt_bound_[0] = 0;
    rt_bound_[1] = 0;
}

void D3D12Backend::Submit(bool wait) {
    last_vsc_gpu_ = 0;
    last_dc_gpu_ = 0;
    cl_->Close();
    ID3D12CommandList* lists[] = {cl_.Get()};
    queue_->ExecuteCommandLists(1, lists);
    queue_->Signal(fence_.Get(), ++fence_value_);
    Frame& f = CurrentFrame();
    f.fence = fence_value_;
    if (wait) {
        WaitFence(fence_value_);
        f.alloc->Reset();
        f.garbage.clear();
        f.used = 0;
        f.srv = 0;
        view_sets_.clear();
        cl_->Reset(f.alloc.Get(), nullptr);
        Begin();
    }
}

void D3D12Backend::Flush() { Submit(true); }

void D3D12Backend::SubmitSoFar() {
    Submit(false);
    // a list can be reset while what it recorded runs: its allocator is not, nor is the frame's upload space, which
    // the frame's end waits for as before
    cl_->Reset(CurrentFrame().alloc.Get(), nullptr);
    Begin();
}

void D3D12Backend::WaitFence(uint64_t value) {
    fence_->SetEventOnCompletion(value, event_);
#ifdef _WIN32
    WaitForSingleObject(event_, INFINITE);
#else
    linux_present::WaitEvent(event_);
#endif
}

void D3D12Backend::SetFullViewport(uint32_t w, uint32_t h) {
    D3D12_VIEWPORT vp{0, 0, float(w), float(h), 0, 1};
    D3D12_RECT scissor{0, 0, LONG(w), LONG(h)};
    cl_->RSSetViewports(1, &vp);
    cl_->RSSetScissorRects(1, &scissor);
    binds_ = {};
}

// one w x h colour target for a Blit, the draw's bound targets forgotten
void D3D12Backend::SetBlitTarget(D3D12_CPU_DESCRIPTOR_HANDLE rtv, uint32_t w, uint32_t h) {
    cl_->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    bound_rt_ = bound_ds_ = nullptr;
    SetFullViewport(w, h);
}

D3D12Backend::Alloc D3D12Backend::Upload(uint64_t size, uint64_t align) {
    Frame& f = CurrentFrame();
    g_perf.upload += size;
    uint64_t o = (f.used + align - 1) & ~(align - 1);
    if (o + size <= kUploadSize) {
        f.used = o + size;
        return {f.upload.Get(), o, f.map + o, f.upload->GetGPUVirtualAddress() + o};
    }
    // too big for the ring: a buffer of its own, released with the frame
    g_perf.overflow++;
    ComPtr<ID3D12Resource> r;
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_UPLOAD};
    constexpr uint64_t kOwnAlign = 0x10000;  // a committed buffer's size granularity
    D3D12_RESOURCE_DESC bd = BufferDesc((size + kOwnAlign - 1) & ~(kOwnAlign - 1));
    dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                  IID_PPV_ARGS(&r));
    uint8_t* p = nullptr;
    r->Map(0, nullptr, reinterpret_cast<void**>(&p));
    f.garbage.push_back(r);
    return {r.Get(), 0, p, r->GetGPUVirtualAddress()};
}

uint32_t D3D12Backend::AllocSrvSlots(uint32_t n) {
    Frame& f = CurrentFrame();
    const uint32_t per = kSrvCount / kFrames;
    if (f.srv + n > per) {
        LOG_FIRST(4, "d3d12: descriptor ring wrapped within a frame");
        f.srv = 0;
        view_sets_.clear();
    }
    uint32_t i = uint32_t(frame_ % kFrames) * per + f.srv;
    f.srv += n;
    return i;
}

void D3D12Backend::Transition(ID3D12Resource* r, D3D12_RESOURCE_STATES& cur, D3D12_RESOURCE_STATES want) {
    if (cur == want) return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = cur;
    b.Transition.StateAfter = want;
    cl_->ResourceBarrier(1, &b);
    g_perf.barriers++;
    cur = want;
}

// a resized target gives back its view and its resources
void D3D12Backend::ReleaseTarget(std::unordered_map<uint64_t, Target>::iterator it) {
    if (bound_rt_ == &it->second || bound_ds_ == &it->second) bound_rt_ = bound_ds_ = nullptr;
    if (fg_ds_ == &it->second) fg_ds_ = nullptr;
    if (scene_ == &it->second) scene_ = nullptr;
    (it->second.depth ? free_dsv_ : free_rtv_).push_back(it->second.view);
    CurrentFrame().garbage.push_back(it->second.res);
    if (it->second.fb) CurrentFrame().garbage.push_back(it->second.fb);
    targets_.erase(it);
}

// the host texture of a new target, at render scale or the extension's
bool D3D12Backend::CreateTargetResource(Target& t, const Surface& s) {
    float sc = scale_;
    if (ext_) ext_->MakeTarget(s, t.depth, t.gw, t.gh, sc, t.fmt);
    t.w = std::max(1u, uint32_t(std::lround(t.gw * sc * aspect_)));
    t.h = std::max(1u, uint32_t(std::lround(t.gh * sc)));
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC td = Texture2DDesc(DXGI_FORMAT_UNKNOWN, t.w, t.h);
    D3D12_CLEAR_VALUE cv{};
    if (t.depth) {
        td.Format = kDepthTypeless;
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        cv.Format = kDepthFormat;
        cv.DepthStencil.Depth = kDepthFar;
        t.state = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    } else {
        td.Format = t.fmt;
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        cv.Format = td.Format;
        t.state = D3D12_RESOURCE_STATE_RENDER_TARGET;
    }
    return SUCCEEDED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, t.state, &cv,
                                                   IID_PPV_ARGS(&t.res)));
}

// a view from the free pool, cleared; false when the pool is empty
bool D3D12Backend::CreateTargetView(Target& t) {
    auto& pool = t.depth ? free_dsv_ : free_rtv_;
    if (pool.empty()) return false;
    t.view = pool.back();
    pool.pop_back();
    if (t.depth) {
        D3D12_DEPTH_STENCIL_VIEW_DESC dv{};
        dv.Format = kDepthFormat;
        dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        dev_->CreateDepthStencilView(t.res.Get(), &dv, Cpu(dsv_heap_.Get(), t.view, dsv_inc_));
        cl_->ClearDepthStencilView(Cpu(dsv_heap_.Get(), t.view, dsv_inc_),
                                   D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, kDepthFar, 0, 0, nullptr);
    } else {
        dev_->CreateRenderTargetView(t.res.Get(), nullptr, Cpu(rtv_heap_.Get(), t.view, rtv_inc_));
        float black[4] = {0, 0, 0, 0};
        cl_->ClearRenderTargetView(Cpu(rtv_heap_.Get(), t.view, rtv_inc_), black, 0, nullptr);
    }
    return true;
}

// targets_ key of a surface's colour or depth target
uint64_t TargetMapKey(uint32_t data, bool depth) { return uint64_t(data) << 1 | (depth ? 1 : 0); }

// a smaller depth surface's console size, above the address bits
constexpr int kKeyWidthShift = 34;
constexpr int kKeyHeightShift = 48;

// a smaller depth surface laid on the depth buffer's memory is a target of its own
uint64_t D3D12Backend::SizedTargetKey(uint32_t data, bool depth, uint32_t gw, uint32_t gh) const {
    uint64_t key = TargetMapKey(data, depth);
    bool small_depth = depth && (gw != guest_w_ || gh != guest_h_);
    if (!small_depth) return key;
    return key | uint64_t(gw) << kKeyWidthShift | uint64_t(gh) << kKeyHeightShift;
}

// alt: the extension's bits of the key
D3D12Backend::TargetSlot D3D12Backend::FindTargetSlot(const Surface& s, bool depth, uint64_t alt) {
    TargetSlot slot;
    slot.data = mem::VirtualFromPhysical(s.data);
    SurfaceSize(s, slot.gw, slot.gh);
    slot.key = SizedTargetKey(slot.data, depth, slot.gw, slot.gh) | alt;
    slot.it = targets_.find(slot.key);
    slot.sized = slot.it != targets_.end() && slot.it->second.gw == slot.gw && slot.it->second.gh == slot.gh;
    return slot;
}

// the surface's target at its size, null when there is none
Target* D3D12Backend::FindTarget(const Surface& s, bool depth) {
    if (!s.header) return nullptr;
    uint64_t alt = 0;
    if (ext_ && !ext_->TargetKey(s, depth, alt)) return nullptr;
    TargetSlot slot = FindTargetSlot(s, depth, alt);
    if (!slot.sized) return nullptr;
    UpdateTargetUsed(slot.data);
    return &slot.it->second;
}

// the surface's target, made again at its size when that changed
Target* D3D12Backend::EnsureTarget(const Surface& s, bool depth) {
    if (!s.header) return nullptr;
    uint64_t alt = 0;
    if (ext_ && !ext_->TargetKey(s, depth, alt)) return nullptr;
    TargetSlot slot = FindTargetSlot(s, depth, alt);
    UpdateTargetUsed(slot.data);
    if (slot.sized) return &slot.it->second;
    if (slot.it != targets_.end()) ReleaseTarget(slot.it);
    Target t;
    t.depth = depth;
    t.gw = slot.gw;
    t.gh = slot.gh;
    if (!CreateTargetResource(t, s)) return nullptr;
    g_perf.targets++;
    // an empty pool drops the new resource with t
    if (!CreateTargetView(t)) return nullptr;
    t.data = slot.data;
    t.bytes = GuestPitch(slot.gw) * slot.gh;
    if (depth) t.bytes = slot.gw * slot.gh * 4;
    t.epoch = DirtyEpoch(slot.data);
    t.sig = MemSig(slot.data, t.bytes);
    auto& ref = targets_[slot.key] = std::move(t);
    return &ref;
}

void D3D12Backend::UpdateTargetUsed(uint32_t data) {
    for (const UsedTarget& u : recent_used_) {
        if (u.data == data && u.frame == frame_) return;
    }
    target_used_[data] = frame_;
    recent_used_[recent_used_next_++ % recent_used_.size()] = {data, frame_};
}

bool D3D12Backend::BindTargets(const State& s) {
    Target* rt = EnsureTarget(s.rt, false);
    Target* ds = s.rs[kRsZEnable] || s.rs[kRsStencilEnable] ? EnsureTarget(s.zs, true) : nullptr;
    if (rt && ds && (rt->w != ds->w || rt->h != ds->h)) ds = nullptr;
    if (!rt && !ds) return false;
    // upscaling: the frame's first draw into the back buffer opens the scene; another target drawn with the scene's
    // depth closes it first, as that draw is at the depth's whole size
    if (rt && s.rt_back_buffer) {
        OpenScene(*rt);
    } else if (scene_ && ds && ds == SceneDepth()) {
        // a picture the title draws apart with the frame's depth (an enemy's after-images, drawn into a target of
        // their own and laid over the frame as it appears and as it swings) takes a depth of its own, cleared each
        // frame, so the scene stays open: closed there, every character drawn after it was drawn after the frame's
        // 3D was anti-aliased, out of ray tracing's scene (Hector and the enemies in the game's own light for a
        // second, several times a fight). Its copies hide each other's back as before, though not behind the room.
        // XBE_AFTER_DEPTH=0: the scene closed there, as before
        static const bool kOwn = !(std::getenv("XBE_AFTER_DEPTH") && std::atoi(std::getenv("XBE_AFTER_DEPTH")) == 0);
        Target* own = kOwn && rt && !rt->depth ? AfterDepth(*ds) : nullptr;
        if (own) {
            ds = own;
        } else {
            CloseScene("another target drawn with the frame's depth");
        }
    }
    if (rt) {
        if (LatchCpuWrite(*rt)) UploadTarget(*rt, s.rt);
        Transition(rt->res.Get(), rt->state, D3D12_RESOURCE_STATE_RENDER_TARGET);
        rt->drawn = true;
        rt->cpu = false;
    }
    if (ds) {
        Transition(ds->res.Get(), ds->state, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        ds->drawn = true;
        ds->cpu = false;
        // the frame's 3D depth, for frame generation's motion: the one drawn with the back buffer
        if ((fg_ || aa_on_ || ao_alone_) && s.rt_back_buffer && rt) {
            fg_ds_ = ds;
            fg_ds_frame_ = presents_;
        }
    }
    // frame generation's motion, drawn by the frame's own 3D draws beside its colour (PrevPositionHlsl, MotionOut)
    bool mv = (fg_ || aa_on_) && rt && s.rt_back_buffer && EnsureObjectMotion(*rt);
    if (mv) Transition(fg_objmv_.Get(), fg_objmv_state_, D3D12_RESOURCE_STATE_RENDER_TARGET);
    // anti-aliasing's reactive mask beside them, which the frame's effects write (Extension::Reactive)
    bool rx = mv && aa_on_ && diag::AaReactive() && EnsureReactive(*rt);
    if (rx) Transition(aa_rx_.Get(), aa_rx_state_, D3D12_RESOURCE_STATE_RENDER_TARGET);
    if (rt != bound_rt_ || ds != bound_ds_ || mv != bound_mv_ || rx != bound_rx_) {
        D3D12_CPU_DESCRIPTOR_HANDLE rh[3] = {rt ? Cpu(rtv_heap_.Get(), rt->view, rtv_inc_) : D3D12_CPU_DESCRIPTOR_HANDLE{},
                                             Cpu(rtv_heap_.Get(), kFgObjRtv, rtv_inc_),
                                             Cpu(rtv_heap_.Get(), kAaRxRtv, rtv_inc_)};
        D3D12_CPU_DESCRIPTOR_HANDLE dh = ds ? Cpu(dsv_heap_.Get(), ds->view, dsv_inc_) : D3D12_CPU_DESCRIPTOR_HANDLE{};
        cl_->OMSetRenderTargets(rt ? (mv ? (rx ? 3 : 2) : 1) : 0, rt ? rh : nullptr, FALSE, ds ? &dh : nullptr);
        bound_rt_ = rt;
        bound_ds_ = ds;
        bound_mv_ = mv;
        bound_rx_ = rx;
    }
    return true;
}

// anti-aliasing's reactive mask at the frame target's size, cleared to 0 (nothing reactive) once a frame; false when it
// can't be made
bool D3D12Backend::EnsureReactive(const Target& rt) {
    if (!aa_rx_ || aa_rx_w_ != rt.w || aa_rx_h_ != rt.h) {
        if (aa_rx_) Release(std::move(aa_rx_));
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC td = Texture2DDesc(DXGI_FORMAT_R8_UNORM, rt.w, rt.h);
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_CLEAR_VALUE cv{};
        cv.Format = DXGI_FORMAT_R8_UNORM;
        aa_rx_state_ = D3D12_RESOURCE_STATE_RENDER_TARGET;
        if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, aa_rx_state_, &cv, IID_PPV_ARGS(&aa_rx_)))) {
            aa_rx_w_ = aa_rx_h_ = 0;
            return false;
        }
        dev_->CreateRenderTargetView(aa_rx_.Get(), nullptr, Cpu(rtv_heap_.Get(), kAaRxRtv, rtv_inc_));
        aa_rx_w_ = rt.w;
        aa_rx_h_ = rt.h;
        aa_rx_frame_ = UINT64_MAX;
    }
    if (aa_rx_frame_ != presents_) {
        Transition(aa_rx_.Get(), aa_rx_state_, D3D12_RESOURCE_STATE_RENDER_TARGET);
        float none[4] = {};
        cl_->ClearRenderTargetView(Cpu(rtv_heap_.Get(), kAaRxRtv, rtv_inc_), none, 0, nullptr);
        aa_rx_frame_ = presents_;
    }
    return true;
}

// the motion target at the frame target's size, cleared to kNoMotion once a frame; false when it can't be made
bool D3D12Backend::EnsureObjectMotion(const Target& rt) {
    if (!fg_objmv_ || fg_objmv_w_ != rt.w || fg_objmv_h_ != rt.h) {
        if (fg_objmv_) Release(std::move(fg_objmv_));
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC td = Texture2DDesc(DXGI_FORMAT_R16G16_FLOAT, rt.w, rt.h);
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_CLEAR_VALUE cv{};
        cv.Format = DXGI_FORMAT_R16G16_FLOAT;
        cv.Color[0] = cv.Color[1] = kNoMotion;
        fg_objmv_state_ = D3D12_RESOURCE_STATE_RENDER_TARGET;
        if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, fg_objmv_state_, &cv,
                                                 IID_PPV_ARGS(&fg_objmv_)))) {
            fg_objmv_w_ = fg_objmv_h_ = 0;
            return false;
        }
        dev_->CreateRenderTargetView(fg_objmv_.Get(), nullptr, Cpu(rtv_heap_.Get(), kFgObjRtv, rtv_inc_));
        fg_objmv_w_ = rt.w;
        fg_objmv_h_ = rt.h;
        fg_objmv_frame_ = UINT64_MAX;
    }
    if (fg_objmv_frame_ != presents_) {
        Transition(fg_objmv_.Get(), fg_objmv_state_, D3D12_RESOURCE_STATE_RENDER_TARGET);
        float none[4] = {kNoMotion, kNoMotion, 0, 0};
        cl_->ClearRenderTargetView(Cpu(rtv_heap_.Get(), kFgObjRtv, rtv_inc_), none, 0, nullptr);
        fg_objmv_frame_ = presents_;
    }
    return true;
}

// A 3D draw found again in the next frame by what it draws, as the game streams its meshes into buffers that move from
// frame to frame: its program, its first vertices and indices, its counts and first texture. A skinned mesh's vertices
// are its pose at rest, so it is found as its bones move.
uint64_t D3D12Backend::MotionKey(const State& s, const Assembled& a, uint64_t vkey) {
    if (mv_frame_ != presents_) {
        // a frame between with no 3D draws leaves nothing to move from
        if (mv_frame_ + 1 == presents_) {
            std::swap(mv_now_at_, mv_prev_at_);
            std::swap(mv_now_, mv_prev_);
        } else {
            mv_prev_at_.clear();
            mv_prev_.clear();
        }
        mv_now_at_.clear();
        mv_now_.clear();
        mv_frame_ = presents_;
    }
    uint64_t h = Hash(&vkey, sizeof(vkey));
    const VertexElement& e = a.elements[0];
    if ((a.regs & 1) && e.buffer < a.buffer_count) {
        const VertexBuffer& b = a.buffers[e.buffer];
        uint32_t n = std::min<uint32_t>(a.vertex_count, 4);
        for (uint32_t i = 0; b.src && i < n; i++)
            if (uint64_t(i) * b.stride + e.offset + 12 <= b.bytes) h = Hash(b.src + size_t(i) * b.stride + e.offset, 12, h);
    }
    if (a.index_src && a.index_count) h = Hash(a.index_src, std::min<size_t>(a.index_count, 8) * (a.index16 ? 2 : 4), h);
    uint32_t counts[3] = {a.vertex_count, a.index_count, a.draw_count};
    h = Hash(counts, sizeof(counts), h);
    return Hash(&s.textures[0], sizeof(s.textures[0]), h);
}

// the vertex constants the draw had in the frame before, null when it was not drawn then. Of a mesh drawn more than
// once, the draw whose constants are nearest now's: the same enemy, where it was a frame ago
const float* D3D12Backend::PrevMotionConstants(uint64_t key, const float* now) const {
    auto it = mv_prev_at_.find(key);
    if (it == mv_prev_at_.end() || it->second.empty()) return nullptr;
    const std::vector<uint32_t>& at = it->second;
    if (at.size() == 1) return &mv_prev_[at[0]].c[0][0];
    const float* best = nullptr;
    double best_d = 0;
    for (uint32_t i : at) {
        const float* p = &mv_prev_[i].c[0][0];
        double d = 0;
        for (size_t k = 0; k < sizeof(MotionConstants::c) / sizeof(float); k++)
            d += std::fabs(double(p[k]) - now[k]) / (std::fabs(double(p[k])) + std::fabs(double(now[k])) + 1.0);
        if (!best || d < best_d) {
            best = p;
            best_d = d;
        }
    }
    return best;
}

// the draw's vertex constants as uploaded (an extension's edits in), for the next frame
void D3D12Backend::KeepMotionConstants(uint64_t key, const uint8_t* vsc) {
    mv_now_at_[key].push_back(uint32_t(mv_now_.size()));
    mv_now_.emplace_back();
    std::memcpy(mv_now_.back().c, vsc, sizeof(MotionConstants::c));
}

constexpr uint64_t kDepthCopyKey = ~0ull;  // DepthCopy's pipeline in blit_psos_

// DepthCopy's pipeline, kept in blit_psos_: writes depth only, always passing; null when it can't be made
ID3D12PipelineState* D3D12Backend::EnsureDepthCopyPipeline() {
    auto& pso = blit_psos_[kDepthCopyKey];
    if (pso) return pso.Get();
    uint64_t h;
    ID3DBlob* vs = Compile(kDepthHlsl, "vs", "vs_5_0", h);
    ID3DBlob* ps = Compile(kDepthHlsl, "psmain", "ps_5_0", h);
    if (!vs || !ps) return nullptr;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC p{};
    p.pRootSignature = root_.Get();
    p.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    p.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    p.SampleMask = UINT_MAX;
    p.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    p.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    p.RasterizerState.DepthClipEnable = TRUE;
    p.DepthStencilState.DepthEnable = TRUE;
    p.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    p.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    p.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    p.DSVFormat = kDepthFormat;
    p.SampleDesc.Count = 1;
    if (FAILED(CreateGraphicsPipelineCached(dev_.Get(), p, IID_PPV_ARGS(&pso)))) return nullptr;
    return pso.Get();
}

// src's depth drawn into dst's depth buffer
void D3D12Backend::DrawDepthCopy(Target& src, Target& dst, ID3D12PipelineState* pso) {
    if (cap_) fprintf(cap_, "%04u depth copy %08X to %08X\n", cap_i_++, src.data, dst.data);
    Transition(src.res.Get(), src.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Transition(dst.res.Get(), dst.state, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    D3D12_CPU_DESCRIPTOR_HANDLE dh = Cpu(dsv_heap_.Get(), dst.view, dsv_inc_);
    cl_->OMSetRenderTargets(0, nullptr, FALSE, &dh);
    SetFullViewport(dst.w, dst.h);
    uint32_t slot = AllocSrvSlots(1);
    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = kDepthRead;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    dev_->CreateShaderResourceView(src.res.Get(), &sv, Cpu(srv_heap_.Get(), slot, srv_inc_));
    Alloc cb = Upload(256, 256);
    const float whole[4] = {1, 1, 0, 0};  // kDepthHlsl's part of the source
    std::memcpy(cb.cpu, whole, sizeof(whole));
    DrawFullScreen(pso, cb.gpu, slot);
    dst.drawn = true;
    dst.cpu = false;
    bound_rt_ = bound_ds_ = nullptr;
}

// a depth buffer drawn as colour into another depth buffer's memory: copied as depth
bool D3D12Backend::DepthCopy(const State& s) {
    TextureDesc d;
    if (!s.rt.header || !s.textures[0] || !Describe(s.textures[0], d) || Format(d.format).depth || d.cube ||
        d.depth != 1) {
        return false;
    }
    uint32_t gw;
    uint32_t gh;
    SurfaceSize(s.rt, gw, gh);
    auto di = targets_.find(SizedTargetKey(mem::VirtualFromPhysical(s.rt.data), true, gw, gh));
    auto si = targets_.find(SizedTargetKey(d.data, true, d.width, d.height));
    if (di == targets_.end() || si == targets_.end() || di == si) return false;
    Target& dst = di->second;
    Target& src = si->second;
    ID3D12PipelineState* pso = EnsureDepthCopyPipeline();
    if (!pso) return false;
    if (SceneTarget(&dst) || SceneTarget(&src)) CloseScene("a depth copy of the frame");
    DrawDepthCopy(src, dst, pso);
    return true;
}

// the pixels the CPU changed since the last Sync, opaque, the rest 0; t.read takes them
std::vector<uint8_t> LatchCpuChanges(Target& t) {
    std::vector<uint8_t> px(t.read.size(), 0);
    uint32_t guest_pitch = GuestPitch(t.gw);
    const uint8_t* guest = static_cast<const uint8_t*>(kernel::HostPtr(t.data));
    for (uint32_t y = 0; y < t.gh; y++) {
        for (uint32_t x = 0; x < t.gw; x++) {
            const uint8_t* c = guest + size_t(y) * guest_pitch + x * 4;
            size_t i = (size_t(y) * t.gw + x) * 4;
            if (!std::memcmp(c, &t.read[i], 4)) continue;
            std::memcpy(&t.read[i], c, 4);
            std::memcpy(&px[i], c, 3);
            px[i + 3] = 255;
        }
    }
    return px;
}

// BGRA pixels into a new w x h texture, left as a shader resource
ComPtr<ID3D12Resource> D3D12Backend::CreatePixelTexture(const std::vector<uint8_t>& px, uint32_t pitch, uint32_t w,
                                                        uint32_t h) {
    ComPtr<ID3D12Resource> tex;
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC td = Texture2DDesc(DXGI_FORMAT_B8G8R8A8_UNORM, w, h);
    dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                  IID_PPV_ARGS(&tex));
    uint32_t row = UploadPitch(w * 4);
    Alloc a = Upload(uint64_t(row) * h, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
    for (uint32_t y = 0; y < h; y++) std::memcpy(a.cpu + size_t(y) * row, &px[size_t(y) * pitch], w * 4);
    D3D12_TEXTURE_COPY_LOCATION dst{tex.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    D3D12_TEXTURE_COPY_LOCATION src{a.res, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    src.PlacedFootprint.Offset = a.off;
    src.PlacedFootprint.Footprint = {DXGI_FORMAT_B8G8R8A8_UNORM, w, h, 1, row};
    cl_->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    D3D12_RESOURCE_STATES st = D3D12_RESOURCE_STATE_COPY_DEST;
    Transition(tex.Get(), st, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    return tex;
}

// CPU pixels of a color surface into its host target, scaled
void D3D12Backend::UploadTarget(Target& t, const Surface& s) {
    if (SceneTarget(&t)) CloseScene("the CPU wrote into the frame");
    g_perf.rtups++;
    TextureDesc d;
    if (!Describe(s.header, d) || Format(d.format).depth) return;
    std::vector<uint8_t> px;
    uint32_t pitch;
    // after a Sync only the pixels the CPU changed go over the picture, which stays at render scale
    bool over = !t.drawn && d.width == t.gw && d.height == t.gh && t.read.size() == size_t(t.gw) * t.gh * 4;
    if (over) {
        LOG_FIRST(4, "d3d12: CPU pixels drawn over target %08X", t.data);
        pitch = t.gw * 4;
        px = LatchCpuChanges(t);
    } else {
        DecodeLevel(d, 0, 0, static_cast<const uint8_t*>(kernel::HostPtr(d.data)), nullptr, px, pitch);
    }
    ComPtr<ID3D12Resource> tex = CreatePixelTexture(px, pitch, d.width, d.height);
    Transition(t.res.Get(), t.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
    SetBlitTarget(Cpu(rtv_heap_.Get(), t.view, rtv_inc_), t.w, t.h);
    float uv[4] = {0, 0, 1, 1};
    float mul = 1 / t.range;
    Blit(tex.Get(), kBgra, uv, {.alpha = true, .mul = mul, .rtv = t.fmt, .over = over});
    CurrentFrame().garbage.push_back(tex);
    t.epoch = DirtyEpoch(t.data);
    t.sig = MemSig(t.data, t.bytes);
    t.cpu = true;
}

// samplers_ key of the full screen passes' samplers, apart from the stage samplers' hashed keys
constexpr uint64_t kBlitSamplerKey = 0xB117;

// four clamp samplers: linear at slot 0, point at slots 1 to 3
uint32_t D3D12Backend::EnsureBlitSamplers() {
    uint64_t key = kBlitSamplerKey;
    auto it = samplers_.find(key);
    uint32_t si;
    if (it == samplers_.end()) {
        if (smp_used_ + 4 > kSamplerCount) {
            samplers_.clear();
            smp_used_ = 0;
        }
        si = smp_used_;
        smp_used_ += 4;
        for (int i = 0; i < 4; i++) {
            D3D12_SAMPLER_DESC sd{};
            sd.Filter = i == 0 ? D3D12_FILTER_MIN_MAG_MIP_LINEAR : D3D12_FILTER_MIN_MAG_MIP_POINT;
            sd.AddressU = sd.AddressV = sd.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            sd.MaxLOD = D3D12_FLOAT32_MAX;
            dev_->CreateSampler(&sd, Cpu(smp_heap_.Get(), si + i, smp_inc_));
        }
        samplers_[key] = si;
    } else {
        si = it->second;
    }
    return si;
}

// the full screen triangle: cb at b0 and b1, views at srv_slot; without a sampler slot EnsureBlitSamplers gives it
void D3D12Backend::DrawFullScreen(ID3D12PipelineState* pso, D3D12_GPU_VIRTUAL_ADDRESS cb, uint32_t srv_slot,
                                  std::optional<uint32_t> sampler_slot) {
    binds_ = {};
    cl_->SetPipelineState(pso);
    cl_->SetGraphicsRootConstantBufferView(0, cb);
    cl_->SetGraphicsRootConstantBufferView(1, cb);
    cl_->SetGraphicsRootDescriptorTable(2, Gpu(srv_heap_.Get(), srv_slot, srv_inc_));
    // without a slot the passes take the blit samplers, looked up at this point as they always were
    uint32_t si = sampler_slot ? *sampler_slot : EnsureBlitSamplers();
    cl_->SetGraphicsRootDescriptorTable(3, Gpu(smp_heap_.Get(), si, smp_inc_));
    cl_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cl_->DrawInstanced(3, 1, 0, 0);
}

void D3D12Backend::Blit(ID3D12Resource* src, DXGI_FORMAT fmt, const float uvrect[4], const BlitOptions& o) {
    uint32_t slot = AllocSrvSlots(o.hdr == 1 ? 4 : 2);
    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = fmt;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    dev_->CreateShaderResourceView(src, &sv, Cpu(srv_heap_.Get(), slot, srv_inc_));
    // the HDR frame's: the frame before its UI (or the frame again), its average (or the black end of the lookup)
    if (o.hdr == 1) {
        dev_->CreateShaderResourceView(hdr_before_ ? hdr_before_ : src, &sv, Cpu(srv_heap_.Get(), slot + 2, srv_inc_));
        D3D12_SHADER_RESOURCE_VIEW_DESC lv = sv;
        lv.Format = kBgra;
        dev_->CreateShaderResourceView(level1_ ? level1_.Get() : lut_.Get(), &lv, Cpu(srv_heap_.Get(), slot + 3, srv_inc_));
    }
    sv.Format = kBgra;
    dev_->CreateShaderResourceView(lut_.Get(), &sv, Cpu(srv_heap_.Get(), slot + 1, srv_inc_));
    uint32_t si = EnsureBlitSamplers();
    Alloc cb = Upload(256, 256);
    float c[16] = {uvrect[0],  uvrect[1],  uvrect[2],  uvrect[3],  o.gamma ? 1.0f : 0.0f, o.alpha ? 1.0f : 0.0f,
                   float(o.taps), o.mul, hdr_cb_[0], hdr_cb_[1], hdr_cb_[2], hdr_cb_[3], hdr10_ ? 1.0f : 0.0f, 0, 0, 0};
    std::memcpy(cb.cpu, c, sizeof(c));
    ID3D12PipelineState* pso = o.hdr ? EnsureHdrPipeline(o.hdr) : EnsureBlitPipeline(o.rtv, o.over);
    if (pso) DrawFullScreen(pso, cb.gpu, slot, si);
}

namespace {

constexpr UINT kCompileFlags = D3DCOMPILE_OPTIMIZATION_LEVEL1;

// which mesh a character's draw is (the scene's CharacterMesh check): where its positions and its indices are read
// from and how many, the same for a copy of it drawn again the same frame with other bones (an after-image)
uint64_t CharacterMesh(const Assembled& a) {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](uint64_t v) { h = (h ^ v) * 1099511628211ull; };
    const VertexElement& e = a.elements[0];
    mix(e.buffer < a.buffer_count ? uint64_t(uintptr_t(a.buffers[e.buffer].src)) + e.offset : 0);
    mix(uint64_t(uintptr_t(a.index_src)));
    mix(a.index_count);
    mix(uint64_t(uint32_t(a.base_vertex)));
    mix(a.draw_count);
    mix(a.vertex_count);
    return h;
}

// compiled shaders kept between sessions, a file each: <exe>/cache/shaders, or the folder XBE_SHADER_CACHE names. A
// shader takes some 25 ms to compile the first time it is drawn, more than a frame; its file reads in well under a
// millisecond. Empty when the folder cannot be made: nothing kept then.
const std::filesystem::path& ShaderCacheDir() {
    static const std::filesystem::path dir = [] {
        const wchar_t* v = _wgetenv(L"XBE_SHADER_CACHE");
        std::filesystem::path d = v && *v ? std::filesystem::path(v) : app::ExeDir() / "cache" / "shaders";
        std::error_code ec;
        std::filesystem::create_directories(d, ec);
        return ec ? std::filesystem::path() : d;
    }();
    return dir;
}

// the file of a shader: its key is its source's hash with the compile flags, so a change to either is a new file
std::filesystem::path ShaderFile(uint64_t key) {
    char name[24];
    std::snprintf(name, sizeof(name), "%016llx.cso", (unsigned long long)key);
    return ShaderCacheDir() / name;
}

// a file is the blob behind its size and hash: one cut short or changed is not read, and the shader compiles again
ComPtr<ID3DBlob> LoadShader(uint64_t key) {
    if (ShaderCacheDir().empty()) return nullptr;
    FILE* f = _wfopen(ShaderFile(key).c_str(), L"rb");
    if (!f) return nullptr;
    uint64_t head[2] = {};  // size, hash
    ComPtr<ID3DBlob> blob;
    bool ok = std::fread(head, sizeof(head), 1, f) == 1 && head[0] && head[0] < (16u << 20) &&
              SUCCEEDED(D3DCreateBlob(SIZE_T(head[0]), &blob)) &&
              std::fread(blob->GetBufferPointer(), 1, size_t(head[0]), f) == head[0] &&
              Hash(blob->GetBufferPointer(), size_t(head[0])) == head[1];
    std::fclose(f);
    return ok ? blob : nullptr;
}

// written beside it and renamed, so a file is whole or not there, whichever instance wrote it
void SaveShader(uint64_t key, ID3DBlob* blob) {
    if (ShaderCacheDir().empty()) return;
    std::filesystem::path file = ShaderFile(key), tmp = file;
    // the thread's too: the pipelines made ahead put shaders in the cache beside the renderer's
    tmp += "." + std::to_string(GetCurrentProcessId()) + "." + std::to_string(GetCurrentThreadId()) + ".tmp";
    FILE* f = _wfopen(tmp.c_str(), L"wb");
    if (!f) return;
    uint64_t head[2] = {blob->GetBufferSize(), Hash(blob->GetBufferPointer(), blob->GetBufferSize())};
    bool ok = std::fwrite(head, sizeof(head), 1, f) == 1 &&
              std::fwrite(blob->GetBufferPointer(), 1, blob->GetBufferSize(), f) == blob->GetBufferSize();
    ok = std::fclose(f) == 0 && ok;
    std::error_code ec;
    if (ok) std::filesystem::rename(tmp, file, ec);
    if (!ok || ec) std::filesystem::remove(tmp, ec);
}

}  // namespace

// Pipelines kept between sessions (pipeline_cache.h): a D3D12 pipeline library, read from its file the first time a
// pipeline is asked for and written back a few seconds after the last one stored in it
namespace {

struct PipelineCache {
    std::mutex mu;
    bool tried = false;
    Microsoft::WRL::ComPtr<ID3D12PipelineLibrary> lib;
    std::vector<uint8_t> blob;  // the file's library, which the library reads from for as long as it lives
    std::unordered_map<ID3D12RootSignature*, uint64_t> roots;
    uint64_t loaded = 0, made = 0;
    bool dirty = false;
    std::chrono::steady_clock::time_point last_store{}, last_save{};
    uint32_t writes = 0;
    // the pipelines this player's game made, listed (pipelines.list): their names, their shaders' by hash, the
    // compiled shaders' keys and the root signatures' hashes; list_mu over these and the file (taken inside mu)
    std::mutex list_mu;
    bool listed_read = false;
    std::unordered_set<uint64_t> listed, blobs_listed, keys_listed, roots_listed;
    // what the card and driver are (vendor, device, driver version) and the release's list's size: the stamp written
    // beside the library once the listed are all made (pipelines.stamp)
    std::string stamp;
    // the pipelines listed made ahead (StartPipelinePrebuild): how many, how many done, whether all are
    std::atomic<uint32_t> pre_total{0}, pre_done{0}, pre_made{0};
    std::atomic<bool> pre_started{false}, pre_finished{false};
};
PipelineCache& Pipes() {
    static PipelineCache c;
    return c;
}
constexpr uint64_t kPipelineMagic = 0x314c5058ull;  // "XPL1"

std::filesystem::path PipelineFile() {
    const std::filesystem::path& d = ShaderCacheDir();
    return d.empty() ? std::filesystem::path() : d / "pipelines.bin";
}

// the library, from its file when that is whole and this driver and card can read it, else empty
void OpenPipelines(PipelineCache& c, ID3D12Device* dev) {
    if (c.tried) return;
    c.tried = true;
    static const bool kOn = !(std::getenv("XBE_PIPELINE_CACHE") && std::atoi(std::getenv("XBE_PIPELINE_CACHE")) == 0);
    if (!kOn || PipelineFile().empty()) return;
    Microsoft::WRL::ComPtr<ID3D12Device1> d1;
    if (FAILED(dev->QueryInterface(IID_PPV_ARGS(&d1)))) return;
    // the file: magic, the library's size and hash, the library
    if (FILE* f = _wfopen(PipelineFile().c_str(), L"rb")) {
        uint64_t head[3] = {};
        if (std::fread(head, sizeof(head), 1, f) == 1 && head[0] == kPipelineMagic && head[1] && head[1] < (1ull << 30)) {
            c.blob.resize(size_t(head[1]));
            if (std::fread(c.blob.data(), 1, c.blob.size(), f) != c.blob.size() ||
                Hash(c.blob.data(), c.blob.size()) != head[2])
                c.blob.clear();
        }
        std::fclose(f);
    }
    HRESULT hr = E_FAIL;
    if (!c.blob.empty()) hr = d1->CreatePipelineLibrary(c.blob.data(), c.blob.size(), IID_PPV_ARGS(&c.lib));
    if (FAILED(hr)) {
        // none yet, or another driver's or card's: started anew
        if (!c.blob.empty())
            LOGR("pipelines: the kept ones were made for another driver or card (0x%08lx), made again", long(hr));
        c.blob.clear();
        c.lib.Reset();
        if (FAILED(d1->CreatePipelineLibrary(nullptr, 0, IID_PPV_ARGS(&c.lib)))) {
            c.lib.Reset();
            LOGR("pipelines: none kept (no pipeline library on this driver)");
            return;
        }
    }
    LOGR("pipelines: %s (%.1f MB), %s", c.blob.empty() ? "none kept yet" : "the kept ones read",
         double(c.blob.size()) / 1048576.0, PipelineFile().string().c_str());
}

// its name: everything its desc holds (shaders by their bytes, root signature by its serialized bytes), so a desc that
// changed is a new name and never one the library would refuse
uint64_t PipelineName(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& d, uint64_t root) {
    uint64_t h = Hash(&root, sizeof(root));
    auto mix = [&](const void* p, size_t n) { h = Hash(p, n, h); };
    auto u = [&](uint64_t v) { mix(&v, sizeof(v)); };
    for (const D3D12_SHADER_BYTECODE* b : {&d.VS, &d.PS, &d.DS, &d.HS, &d.GS}) {
        u(b->BytecodeLength);
        if (b->pShaderBytecode && b->BytecodeLength) mix(b->pShaderBytecode, b->BytecodeLength);
    }
    u(d.StreamOutput.NumEntries);
    u(d.BlendState.AlphaToCoverageEnable);
    u(d.BlendState.IndependentBlendEnable);
    for (const D3D12_RENDER_TARGET_BLEND_DESC& b : d.BlendState.RenderTarget) {
        u(b.BlendEnable), u(b.LogicOpEnable), u(b.SrcBlend), u(b.DestBlend), u(b.BlendOp), u(b.SrcBlendAlpha);
        u(b.DestBlendAlpha), u(b.BlendOpAlpha), u(b.LogicOp), u(b.RenderTargetWriteMask);
    }
    u(d.SampleMask);
    mix(&d.RasterizerState, sizeof(d.RasterizerState));  // four byte members, no padding
    const D3D12_DEPTH_STENCIL_DESC& z = d.DepthStencilState;
    u(z.DepthEnable), u(z.DepthWriteMask), u(z.DepthFunc), u(z.StencilEnable), u(z.StencilReadMask);
    u(z.StencilWriteMask);
    mix(&z.FrontFace, sizeof(z.FrontFace));
    mix(&z.BackFace, sizeof(z.BackFace));
    u(d.InputLayout.NumElements);
    for (UINT i = 0; i < d.InputLayout.NumElements; i++) {
        const D3D12_INPUT_ELEMENT_DESC& e = d.InputLayout.pInputElementDescs[i];
        if (e.SemanticName) mix(e.SemanticName, std::strlen(e.SemanticName));
        u(e.SemanticIndex), u(e.Format), u(e.InputSlot), u(e.AlignedByteOffset), u(e.InputSlotClass);
        u(e.InstanceDataStepRate);
    }
    u(d.IBStripCutValue), u(d.PrimitiveTopologyType), u(d.NumRenderTargets);
    for (DXGI_FORMAT f : d.RTVFormats) u(f);
    u(d.DSVFormat), u(d.SampleDesc.Count), u(d.SampleDesc.Quality), u(d.NodeMask), u(d.Flags);
    return h;
}

// The pipelines a game made, listed (pipelines.list beside the library; the release's own, pipelines.list beside the
// game), with everything to make them again without the game: each one's whole desc, its shaders' and root signature's
// bytes, and which compiled shader each shader source's key is. On a first run, another card or a new driver they are
// all made ahead (PipePrebuild), and the compiled shaders the release lists are put in the shader cache, so neither a
// shader nor a pipeline is made as a room is first drawn. Records, each a tag and what follows:
//  1: a shader or root signature's bytes (their hash, size, bytes), before the first record that names them
//  2: a pipeline (its size, ListedPipe, its input elements)
//  3: a compiled shader (its key in the shader cache, its bytes' hash)
//  4: a root signature (its bytes' hash: their record is a 1)
constexpr uint64_t kListMagic = 0x3154534c4c505858ull;  // "XXPLLST1"
struct ListedPipe {
    uint64_t name = 0, root = 0;
    uint64_t sh[5] = {};  // VS, PS, DS, HS, GS by their bytes' hash, 0 none
    D3D12_BLEND_DESC blend{};
    UINT sample_mask = 0;
    D3D12_RASTERIZER_DESC rast{};
    D3D12_DEPTH_STENCIL_DESC ds{};
    UINT strip = 0, topo = 0, nrt = 0;
    DXGI_FORMAT rtv[8] = {};
    DXGI_FORMAT dsv{};
    DXGI_SAMPLE_DESC sample{};
    UINT node = 0, flags = 0, nel = 0;
};
struct ListedElem {
    char sem[32] = {};
    UINT index = 0, format = 0, slot = 0, offset = 0, cls = 0, step = 0;
};
std::filesystem::path ListFile() {
    const std::filesystem::path& d = ShaderCacheDir();
    return d.empty() ? std::filesystem::path() : d / "pipelines.list";
}
std::filesystem::path ShippedListFile() { return app::ExeDir() / "pipelines.list"; }

struct Listed {
    ListedPipe p;
    std::vector<ListedElem> el;
};
struct ListContents {
    std::vector<Listed> pipes;
    std::unordered_map<uint64_t, std::vector<uint8_t>> blobs;  // by hash (with bytes; else only their hashes)
    std::unordered_set<uint64_t> blob_hashes;
    std::vector<std::pair<uint64_t, uint64_t>> keys;  // shader cache key, bytes' hash
    std::vector<uint64_t> roots;
};
// a list's records as far as it is whole (bytes: the shaders' bytes read, else skipped); where the whole part ends
uint64_t ReadList(const std::filesystem::path& file, ListContents& out, bool bytes) {
    FILE* f = _wfopen(file.c_str(), L"rb");
    if (!f) return 0;
    _fseeki64(f, 0, SEEK_END);
    int64_t size = _ftelli64(f);
    _fseeki64(f, 0, SEEK_SET);
    uint64_t magic = 0, whole = 0;
    if (std::fread(&magic, sizeof(magic), 1, f) == 1 && magic == kListMagic) {
        whole = sizeof(magic);
        for (;;) {
            uint32_t tag = 0;
            if (std::fread(&tag, sizeof(tag), 1, f) != 1) break;
            if (tag == 1) {
                uint64_t h = 0;
                uint32_t n = 0;
                if (std::fread(&h, sizeof(h), 1, f) != 1 || std::fread(&n, sizeof(n), 1, f) != 1 || n > (16u << 20))
                    break;
                if (bytes) {
                    std::vector<uint8_t> b(n);
                    if (std::fread(b.data(), 1, n, f) != n || Hash(b.data(), n) != h) break;
                    out.blobs.emplace(h, std::move(b));
                } else if (_fseeki64(f, n, SEEK_CUR) != 0) {
                    break;
                }
                out.blob_hashes.insert(h);
            } else if (tag == 2) {
                uint32_t n = 0;
                if (std::fread(&n, sizeof(n), 1, f) != 1 || n < sizeof(ListedPipe) || n > (1u << 16)) break;
                Listed l;
                if (std::fread(&l.p, sizeof(l.p), 1, f) != 1 || l.p.nel > 32 ||
                    n != sizeof(ListedPipe) + l.p.nel * sizeof(ListedElem))
                    break;
                l.el.resize(l.p.nel);
                if (l.p.nel && std::fread(l.el.data(), sizeof(ListedElem), l.p.nel, f) != l.p.nel) break;
                for (ListedElem& e : l.el) e.sem[sizeof(e.sem) - 1] = 0;
                out.pipes.push_back(std::move(l));
            } else if (tag == 3) {
                uint64_t kh[2];
                if (std::fread(kh, sizeof(kh), 1, f) != 1) break;
                out.keys.emplace_back(kh[0], kh[1]);
            } else if (tag == 4) {
                uint64_t h = 0;
                if (std::fread(&h, sizeof(h), 1, f) != 1) break;
                out.roots.push_back(h);
            } else {
                break;
            }
            // with bytes unread (sought past), a file cut short in a blob's bytes is found here
            int64_t at = _ftelli64(f);
            if (at < 0 || at > size) break;
            whole = uint64_t(at);
        }
    }
    std::fclose(f);
    return whole;
}

// this player's list read once (its names and hashes, not its bytes): what it holds is not written again, and a
// record cut short (the game closed as it was written) cut off, so the records after it are read
void ReadOwnList(PipelineCache& c) {
    if (c.listed_read) return;
    c.listed_read = true;
    ListContents l;
    uint64_t whole = ReadList(ListFile(), l, false);
    std::error_code ec;
    uint64_t size = std::filesystem::file_size(ListFile(), ec);
    if (!ec && size > whole) {
        if (whole) std::filesystem::resize_file(ListFile(), whole, ec);
        else std::filesystem::remove(ListFile(), ec);
    }
    for (const Listed& p : l.pipes) c.listed.insert(p.p.name);
    c.blobs_listed = std::move(l.blob_hashes);
    for (const auto& k : l.keys) c.keys_listed.insert(k.first);
    for (uint64_t r : l.roots) c.roots_listed.insert(r);
}

bool ListOn() {
    static const bool kOn = !(std::getenv("XBE_PIPELINE_CACHE") && std::atoi(std::getenv("XBE_PIPELINE_CACHE")) == 0);
    return kOn && !ListFile().empty();
}
// the list, open to add to (its magic first when new)
FILE* OpenListFile() {
    bool fresh = !std::filesystem::exists(ListFile());
    FILE* f = _wfopen(ListFile().c_str(), L"ab");
    if (f && fresh) std::fwrite(&kListMagic, sizeof(kListMagic), 1, f);
    return f;
}
// bytes onto the list unless they are on it; their hash
uint64_t ListBytes(PipelineCache& c, FILE* f, const void* p, size_t n) {
    uint64_t h = Hash(p, n);
    if (c.blobs_listed.insert(h).second) {
        uint32_t tag = 1, size = uint32_t(n);
        std::fwrite(&tag, sizeof(tag), 1, f);
        std::fwrite(&h, sizeof(h), 1, f);
        std::fwrite(&size, sizeof(size), 1, f);
        std::fwrite(p, 1, n, f);
    }
    return h;
}

// a root signature onto the list
void ListRoot(PipelineCache& c, const void* blob, size_t size) {
    if (!ListOn() || !blob || !size) return;
    std::lock_guard<std::mutex> lock(c.list_mu);
    ReadOwnList(c);
    uint64_t h = Hash(blob, size);
    if (c.roots_listed.count(h)) return;
    FILE* f = OpenListFile();
    if (!f) return;
    c.roots_listed.insert(h);
    ListBytes(c, f, blob, size);
    uint32_t tag = 4;
    std::fwrite(&tag, sizeof(tag), 1, f);
    std::fwrite(&h, sizeof(h), 1, f);
    std::fclose(f);
}

// a compiled shader onto the list: its key in the shader cache and its bytes
void ListShader(uint64_t key, ID3DBlob* blob) {
    PipelineCache& c = Pipes();
    if (!ListOn() || !blob) return;
    std::lock_guard<std::mutex> lock(c.list_mu);
    ReadOwnList(c);
    if (c.keys_listed.count(key)) return;
    FILE* f = OpenListFile();
    if (!f) return;
    c.keys_listed.insert(key);
    uint64_t kh[2] = {key, ListBytes(c, f, blob->GetBufferPointer(), blob->GetBufferSize())};
    uint32_t tag = 3;
    std::fwrite(&tag, sizeof(tag), 1, f);
    std::fwrite(kh, sizeof(kh), 1, f);
    std::fclose(f);
}

// a pipeline onto the list (one with stream output is left off: none is drawn so)
void ListPipeline(PipelineCache& c, const D3D12_GRAPHICS_PIPELINE_STATE_DESC& d, uint64_t name, uint64_t root) {
    if (!ListOn() || !root || d.StreamOutput.NumEntries || d.InputLayout.NumElements > 32) return;
    std::lock_guard<std::mutex> lock(c.list_mu);
    ReadOwnList(c);
    if (c.listed.count(name)) return;
    FILE* f = OpenListFile();
    if (!f) return;
    c.listed.insert(name);
    ListedPipe p;
    p.name = name;
    p.root = root;
    const D3D12_SHADER_BYTECODE* sh[5] = {&d.VS, &d.PS, &d.DS, &d.HS, &d.GS};
    for (int i = 0; i < 5; i++)
        if (sh[i]->pShaderBytecode && sh[i]->BytecodeLength)
            p.sh[i] = ListBytes(c, f, sh[i]->pShaderBytecode, sh[i]->BytecodeLength);
    p.blend = d.BlendState;
    p.sample_mask = d.SampleMask;
    p.rast = d.RasterizerState;
    p.ds = d.DepthStencilState;
    p.strip = UINT(d.IBStripCutValue);
    p.topo = UINT(d.PrimitiveTopologyType);
    p.nrt = d.NumRenderTargets;
    std::copy(std::begin(d.RTVFormats), std::end(d.RTVFormats), p.rtv);
    p.dsv = d.DSVFormat;
    p.sample = d.SampleDesc;
    p.node = d.NodeMask;
    p.flags = UINT(d.Flags);
    p.nel = d.InputLayout.NumElements;
    std::vector<ListedElem> el(p.nel);
    for (UINT i = 0; i < p.nel; i++) {
        const D3D12_INPUT_ELEMENT_DESC& e = d.InputLayout.pInputElementDescs[i];
        if (e.SemanticName) std::snprintf(el[i].sem, sizeof(el[i].sem), "%s", e.SemanticName);
        el[i].index = e.SemanticIndex;
        el[i].format = UINT(e.Format);
        el[i].slot = e.InputSlot;
        el[i].offset = e.AlignedByteOffset;
        el[i].cls = UINT(e.InputSlotClass);
        el[i].step = e.InstanceDataStepRate;
    }
    uint32_t tag = 2, n = uint32_t(sizeof(ListedPipe) + p.nel * sizeof(ListedElem));
    std::fwrite(&tag, sizeof(tag), 1, f);
    std::fwrite(&n, sizeof(n), 1, f);
    std::fwrite(&p, sizeof(p), 1, f);
    if (p.nel) std::fwrite(el.data(), sizeof(ListedElem), p.nel, f);
    std::fclose(f);
}

// the card and driver a device was made on: "vendor:device:driver" (the launcher works the same out for the card it
// starts the game on, and makes the pipelines ahead when it differs from the stamp's)
std::string DeviceIdentity(ID3D12Device* dev) {
#ifndef _WIN32
    (void)dev;
    return {};  // Linux: no DXGI to name the card and driver; the pipelines are not made ahead there
#else
    Microsoft::WRL::ComPtr<IDXGIFactory4> f;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> a;
    DXGI_ADAPTER_DESC1 d{};
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&f))) || FAILED(f->EnumAdapterByLuid(dev->GetAdapterLuid(), IID_PPV_ARGS(&a))) ||
        FAILED(a->GetDesc1(&d)))
        return {};
    LARGE_INTEGER umd{};
    a->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd);
    char s[96];
    std::snprintf(s, sizeof(s), "%04x:%04x:%u.%u.%u.%u", d.VendorId, d.DeviceId, unsigned(umd.HighPart >> 16),
                  unsigned(umd.HighPart & 0xFFFF), unsigned(umd.LowPart >> 16), unsigned(umd.LowPart & 0xFFFF));
    return s;
#endif
}
// the stamp: the card and driver, and the release's list by its size
std::string StampFor(ID3D12Device* dev) {
    std::string id = DeviceIdentity(dev);
    if (id.empty()) return {};
    std::error_code ec;
    uint64_t n = std::filesystem::file_size(ShippedListFile(), ec);
    return id + "\nlist " + std::to_string(ec ? 0 : n) + "\n";
}
void WriteStamp(const std::string& stamp) {
    if (stamp.empty() || ShaderCacheDir().empty()) return;
    std::filesystem::path file = ShaderCacheDir() / "pipelines.stamp", tmp = file;
    tmp += "." + std::to_string(GetCurrentProcessId()) + ".tmp";
    FILE* f = _wfopen(tmp.c_str(), L"wb");
    if (!f) return;
    bool ok = std::fwrite(stamp.data(), 1, stamp.size(), f) == stamp.size();
    ok = std::fclose(f) == 0 && ok;
    std::error_code ec;
    if (ok) std::filesystem::rename(tmp, file, ec);
    if (!ok || ec) std::filesystem::remove(tmp, ec);
}

// the library's bytes into its file, beside it and renamed (a file is whole or not there)
void WritePipelines(std::filesystem::path file, std::vector<uint8_t> data, uint32_t n, std::string stamp) {
    std::filesystem::path tmp = file;
    tmp += "." + std::to_string(GetCurrentProcessId()) + "." + std::to_string(n) + ".tmp";
    FILE* f = _wfopen(tmp.c_str(), L"wb");
    if (!f) return;
    uint64_t head[3] = {kPipelineMagic, data.size(), Hash(data.data(), data.size())};
    bool ok = std::fwrite(head, sizeof(head), 1, f) == 1 && std::fwrite(data.data(), 1, data.size(), f) == data.size();
    ok = std::fclose(f) == 0 && ok;
    std::error_code ec;
    if (ok) std::filesystem::rename(tmp, file, ec);
    if (!ok || ec) std::filesystem::remove(tmp, ec);
    // the stamp once the library it vouches for is whole
    if (ok && !ec) WriteStamp(stamp);
}

}  // namespace

// (gpu::RegisterRootSignature and the rest, pipeline_cache.h's, at the end of this file: this part is the file's own)
void PipeRegisterRoot(ID3D12RootSignature* rs, const void* blob, size_t size) {
    PipelineCache& c = Pipes();
    std::lock_guard<std::mutex> lock(c.mu);
    c.roots[rs] = Hash(blob, size);
    ListRoot(c, blob, size);
}

HRESULT PipeCreate(ID3D12Device* dev, const D3D12_GRAPHICS_PIPELINE_STATE_DESC& d, REFIID riid, void** out) {
    PipelineCache& c = Pipes();
    std::lock_guard<std::mutex> lock(c.mu);
    OpenPipelines(c, dev);
    if (!c.lib) return dev->CreateGraphicsPipelineState(&d, riid, out);
    auto r = c.roots.find(d.pRootSignature);
    wchar_t name[24];
    std::swprintf(name, 24, L"%016llx", (unsigned long long)PipelineName(d, r != c.roots.end() ? r->second : 0));
    uint64_t root = r != c.roots.end() ? r->second : 0;
    if (SUCCEEDED(c.lib->LoadGraphicsPipeline(name, &d, riid, out))) {
        c.loaded++;
        ListPipeline(c, d, PipelineName(d, root), root);
        return S_OK;
    }
    HRESULT hr = dev->CreateGraphicsPipelineState(&d, riid, out);
    if (FAILED(hr)) return hr;
    c.made++;
    ListPipeline(c, d, PipelineName(d, root), root);
    // stored under its name (one the library already holds is left as it is)
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pso;
    if (SUCCEEDED(static_cast<IUnknown*>(*out)->QueryInterface(IID_PPV_ARGS(&pso))) &&
        SUCCEEDED(c.lib->StorePipeline(name, pso.Get()))) {
        c.dirty = true;
        c.last_store = std::chrono::steady_clock::now();
    }
    return hr;
}

void PipeSave(bool now) {
    PipelineCache& c = Pipes();
    std::lock_guard<std::mutex> lock(c.mu);
    if (!c.lib || !c.dirty) return;
    auto t = std::chrono::steady_clock::now();
    if (!now && (t - c.last_store < std::chrono::seconds(3) || t - c.last_save < std::chrono::seconds(15))) return;
    size_t n = c.lib->GetSerializedSize();
    std::vector<uint8_t> data(n);
    if (!n || FAILED(c.lib->Serialize(data.data(), n))) return;
    c.dirty = false;
    c.last_save = t;
    LOGR("pipelines: %llu made, %llu read from the kept ones; %.1f MB kept", (unsigned long long)c.made,
         (unsigned long long)c.loaded, double(n) / 1048576.0);
    // written off this thread: a few megabytes would hitch the frame they were written in
    // the stamp with it once the listed are all made
    std::string stamp = c.pre_finished ? c.stamp : std::string();
    if (now) {
        WritePipelines(PipelineFile(), std::move(data), ++c.writes, std::move(stamp));
    } else {
        std::thread(WritePipelines, PipelineFile(), std::move(data), ++c.writes, std::move(stamp)).detach();
    }
}

// The pipelines listed (the release's and this player's own) made ahead on threads of their own, those the library
// does not hold already, with root signatures of their own made from the listed bytes (a library's pipeline is found
// by its root signature's bytes, not the object): on a first run, another card or a new driver, every pipeline the game
// has drawn with is ready before it is drawn again (a room's first frame made two to four, 10-25 ms each). The
// compiled shaders listed that the shader cache lacks are put in it first (each took some 25 ms to compile)
void PipePrebuild(ID3D12Device* dev) {
    PipelineCache& c = Pipes();
    if (c.pre_started.exchange(true)) return;
    {
        std::lock_guard<std::mutex> lock(c.mu);
        OpenPipelines(c, dev);
        c.stamp = StampFor(dev);
        if (!c.lib) {
            c.pre_finished = true;
            return;
        }
    }
    dev->AddRef();
    std::thread([dev] {
        PipelineCache& c = Pipes();
        auto start = std::chrono::steady_clock::now();
        ListContents l;
        ReadList(ShippedListFile(), l, true);
        ReadList(ListFile(), l, true);
        // the compiled shaders into the shader cache
        uint32_t seeded = 0;
        for (const auto& k : l.keys) {
            auto b = l.blobs.find(k.second);
            if (b == l.blobs.end() || std::filesystem::exists(ShaderFile(k.first))) continue;
            ComPtr<ID3DBlob> blob;
            if (FAILED(D3DCreateBlob(b->second.size(), &blob))) continue;
            std::memcpy(blob->GetBufferPointer(), b->second.data(), b->second.size());
            SaveShader(k.first, blob.Get());
            seeded++;
        }
        // the root signatures
        std::unordered_map<uint64_t, ComPtr<ID3D12RootSignature>> roots;
        for (uint64_t h : l.roots) {
            auto b = l.blobs.find(h);
            ComPtr<ID3D12RootSignature> rs;
            if (b != l.blobs.end() && !roots.count(h) &&
                SUCCEEDED(dev->CreateRootSignature(0, b->second.data(), b->second.size(), IID_PPV_ARGS(&rs))))
                roots[h] = rs;
        }
        // each name once, with its root signature and shaders whole
        std::unordered_set<uint64_t> seen;
        std::vector<const Listed*> todo;
        for (const Listed& p : l.pipes) {
            bool whole = roots.count(p.p.root) > 0;
            for (uint64_t h : p.p.sh) whole = whole && (!h || l.blobs.count(h));
            if (whole && seen.insert(p.p.name).second) todo.push_back(&p);
        }
        c.pre_total = uint32_t(todo.size());
        std::atomic<size_t> next{0};
        auto work = [&] {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
            for (size_t i; (i = next.fetch_add(1)) < todo.size();) {
                const Listed& p = *todo[i];
                D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
                d.pRootSignature = roots[p.p.root].Get();
                D3D12_SHADER_BYTECODE* sh[5] = {&d.VS, &d.PS, &d.DS, &d.HS, &d.GS};
                for (int k = 0; k < 5; k++) {
                    if (!p.p.sh[k]) continue;
                    const std::vector<uint8_t>& b = l.blobs.find(p.p.sh[k])->second;
                    *sh[k] = {b.data(), b.size()};
                }
                std::vector<D3D12_INPUT_ELEMENT_DESC> el(p.el.size());
                for (size_t k = 0; k < el.size(); k++)
                    el[k] = {p.el[k].sem, p.el[k].index, DXGI_FORMAT(p.el[k].format), p.el[k].slot, p.el[k].offset,
                             D3D12_INPUT_CLASSIFICATION(p.el[k].cls), p.el[k].step};
                d.InputLayout = {el.empty() ? nullptr : el.data(), UINT(el.size())};
                d.BlendState = p.p.blend;
                d.SampleMask = p.p.sample_mask;
                d.RasterizerState = p.p.rast;
                d.DepthStencilState = p.p.ds;
                d.IBStripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE(p.p.strip);
                d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE(p.p.topo);
                d.NumRenderTargets = p.p.nrt;
                std::copy(std::begin(p.p.rtv), std::end(p.p.rtv), d.RTVFormats);
                d.DSVFormat = p.p.dsv;
                d.SampleDesc = p.p.sample;
                d.NodeMask = p.p.node;
                d.Flags = D3D12_PIPELINE_STATE_FLAGS(p.p.flags);
                wchar_t name[24];
                std::swprintf(name, 24, L"%016llx", (unsigned long long)p.p.name);
                ComPtr<ID3D12PipelineState> pso;
                bool held;
                {
                    std::lock_guard<std::mutex> lock(c.mu);
                    held = SUCCEEDED(c.lib->LoadGraphicsPipeline(name, &d, IID_PPV_ARGS(&pso)));
                }
                if (!held && SUCCEEDED(dev->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso)))) {
                    std::lock_guard<std::mutex> lock(c.mu);
                    if (SUCCEEDED(c.lib->StorePipeline(name, pso.Get()))) {
                        c.dirty = true;
                        c.last_store = std::chrono::steady_clock::now();
                        c.pre_made++;
                    }
                }
                c.pre_done++;
            }
        };
        unsigned n = std::max(1u, std::min(8u, std::thread::hardware_concurrency() / 2));
        std::vector<std::thread> threads;
        for (unsigned k = 0; k < n; k++) threads.emplace_back(work);
        for (std::thread& t : threads) t.join();
        double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        LOGR("pipelines: %u listed made ahead in %.1f s on %u threads (%u made, the rest kept already); %u compiled "
             "shaders put in the cache",
             c.pre_total.load(), secs, n, c.pre_made.load(), seeded);
        {
            std::lock_guard<std::mutex> lock(c.mu);
            c.pre_finished = true;
            // none made: the library's file holds them all already, and the stamp says so now
            if (!c.dirty) WriteStamp(c.stamp);
        }
        dev->Release();
    }).detach();
}

void PipePrebuildProgress(uint32_t& done, uint32_t& total, bool& finished) {
    PipelineCache& c = Pipes();
    done = c.pre_done;
    total = c.pre_total;
    finished = c.pre_finished;
}

// --prepare-shaders (the launcher's, before the game starts on a card or driver the stamp is not for): a device on the
// card the game will use (the settings' card, else the fastest, as the renderer picks), the listed made, the library
// written. Its progress in cache/shaders/prepare.txt ("done total", "finished" at the end) for the launcher's bar
bool PipePrepare() {
    PipelineCache& c = Pipes();
    if (ShaderCacheDir().empty()) return false;
    std::filesystem::path progress = ShaderCacheDir() / "prepare.txt";
    auto say = [&](const std::string& text) {
        std::filesystem::path tmp = progress;
        tmp += ".tmp";
        if (FILE* f = _wfopen(tmp.c_str(), L"wb")) {
            std::fwrite(text.data(), 1, text.size(), f);
            std::fclose(f);
            std::error_code ec;
            std::filesystem::rename(tmp, progress, ec);
        }
    };
    say("0 0\n");
#ifndef _WIN32
    return false;  // Linux: the launcher does not make them ahead (no DXGI to find the card by)
#else
    ComPtr<IDXGIFactory6> f;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&f)))) return false;
    ComPtr<IDXGIAdapter1> adapter;
    if (const std::string& want = app::Get().gpu; !want.empty()) {
        ComPtr<IDXGIAdapter1> a;
        for (UINT i = 0; !adapter && f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; i++) {
            DXGI_ADAPTER_DESC1 d{};
            a->GetDesc1(&d);
            char name[sizeof(d.Description)];
            WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, name, sizeof(name), nullptr, nullptr);
            if (want == name && SUCCEEDED(D3D12CreateDevice(a.Get(), D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), nullptr)))
                adapter = a;
        }
    }
    if (!adapter) f->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter));
    ComPtr<ID3D12Device> dev;
    if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev)))) return false;
    DXGI_ADAPTER_DESC1 ad{};
    if (adapter) adapter->GetDesc1(&ad);
    LOGR("pipelines: made ahead for %ls", adapter ? ad.Description : L"the default card");
    PipePrebuild(dev.Get());
    for (;;) {
        uint32_t done = c.pre_done, total = c.pre_total;
        bool finished = c.pre_finished;
        say(std::to_string(done) + " " + std::to_string(total) + "\n");
        if (finished) break;
        Sleep(100);
    }
    PipeSave(true);
    say("finished\n");
    return true;
#endif
}

ID3DBlob* D3D12Backend::Compile(const std::string& src, const char* entry, const char* profile, uint64_t& hash) {
    // one source can hold several entry points
    uint64_t profile_seed = Hash(profile, std::strlen(profile));
    uint64_t entry_seed = Hash(entry, std::strlen(entry), profile_seed);
    hash = Hash(src.data(), src.size(), entry_seed);
    auto it = blobs_.find(hash);
    if (it != blobs_.end()) return it->second.Get();
    PerfScope perf_compile{g_perf.compile};
    uint64_t key = Hash(&kCompileFlags, sizeof(kCompileFlags), hash);
    if (ComPtr<ID3DBlob> kept = LoadShader(key)) {
        g_perf.shaders_kept++;
        blobs_[hash] = kept;
        ListShader(key, kept.Get());
        return kept.Get();
    }
    ComPtr<ID3DBlob> code;
    ComPtr<ID3DBlob> err;
    g_perf.shaders++;
    HRESULT hr = D3DCompile(src.data(), src.size(), nullptr, nullptr, nullptr, entry, profile, kCompileFlags, 0,
                            &code, &err);
    if (FAILED(hr)) {
        LOGE("d3d12: %s compile failed: %s", profile, err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
        LOGE("%s", src.c_str());
        blobs_[hash] = nullptr;
        return nullptr;
    }
    blobs_[hash] = code;
    SaveShader(key, code.Get());
    ListShader(key, code.Get());
    return code.Get();
}

// a full screen pass's pipeline: solid, no culling, one target, writing all channels; callers set the blend
D3D12_GRAPHICS_PIPELINE_STATE_DESC D3D12Backend::FullScreenPipelineDesc(ID3DBlob* vs, ID3DBlob* ps,
                                                                        DXGI_FORMAT rtv) const {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
    d.pRootSignature = root_.Get();
    d.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    d.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    d.SampleMask = UINT_MAX;
    d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    d.RasterizerState.DepthClipEnable = TRUE;
    d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    d.NumRenderTargets = 1;
    d.RTVFormats[0] = rtv;
    d.SampleDesc.Count = 1;
    return d;
}

ID3D12PipelineState* D3D12Backend::EnsureBlitPipeline(DXGI_FORMAT rtv, bool over) {
    auto& pso = blit_psos_[uint64_t(rtv) | uint64_t(over) << 32];
    if (pso) return pso.Get();
    uint64_t h;
    ID3DBlob* vs = Compile(kBlitHlsl, "vs", "vs_5_0", h);
    ID3DBlob* ps = Compile(kBlitHlsl, "psmain", "ps_5_0", h);
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d = FullScreenPipelineDesc(vs, ps, rtv);
    if (over) {
        auto& blend = d.BlendState.RenderTarget[0];
        // premultiplied colour, the target keeps its alpha
        blend.BlendEnable = TRUE;
        blend.SrcBlend = D3D12_BLEND_ONE;
        blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        blend.SrcBlendAlpha = D3D12_BLEND_ZERO;
        blend.DestBlendAlpha = D3D12_BLEND_ONE;
        blend.BlendOp = blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    }
    CreateGraphicsPipelineCached(dev_.Get(), d, IID_PPV_ARGS(&pso));
    return pso.Get();
}

// kHdrHlsl's pipeline into an HDR target: the frame written, the UI layer premultiplied over it; null when it can't be
// made
ID3D12PipelineState* D3D12Backend::EnsureHdrPipeline(uint32_t mode) {
    auto& pso = hdr_psos_[(mode == 1 ? 0 : 1) + (hdr10_ ? 2 : 0)];
    if (pso) return pso.Get();
    uint64_t h;
    ID3DBlob* vs = Compile(kHdrHlsl, "vs", "vs_5_0", h);
    ID3DBlob* ps = Compile(kHdrHlsl, mode == 1 ? "scene" : "ui", "ps_5_0", h);
    if (!vs || !ps) return nullptr;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d = FullScreenPipelineDesc(vs, ps, HdrFormat());
    if (mode != 1) {
        auto& blend = d.BlendState.RenderTarget[0];
        blend.BlendEnable = TRUE;
        blend.SrcBlend = D3D12_BLEND_ONE;
        blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        blend.SrcBlendAlpha = D3D12_BLEND_ZERO;
        blend.DestBlendAlpha = D3D12_BLEND_ONE;
        blend.BlendOp = blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    }
    if (FAILED(CreateGraphicsPipelineCached(dev_.Get(), d, IID_PPV_ARGS(&pso)))) return nullptr;
    return pso.Get();
}

// Pass's pipeline for the shader, kept in blit_psos_; null when it can't be made
ID3D12PipelineState* D3D12Backend::EnsurePassPipeline(const char* hlsl, const char* entry, DXGI_FORMAT rtv,
                                                      float blend) {
    uint64_t seed = uint64_t(rtv) << 1 | (blend >= 0 ? 1 : 0);
    uint64_t source_seed = Hash(hlsl, std::strlen(hlsl), seed);
    auto& pso = blit_psos_[Hash(entry, std::strlen(entry), source_seed)];
    if (pso) return pso.Get();
    uint64_t h;
    ID3DBlob* vs = Compile(hlsl, "vs", "vs_5_0", h);
    ID3DBlob* ps = Compile(hlsl, entry, "ps_5_0", h);
    if (!vs || !ps) return nullptr;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d = FullScreenPipelineDesc(vs, ps, rtv);
    if (blend >= 0) {
        auto& b = d.BlendState.RenderTarget[0];
        b.BlendEnable = TRUE;
        b.SrcBlend = b.SrcBlendAlpha = D3D12_BLEND_BLEND_FACTOR;
        b.DestBlend = b.DestBlendAlpha = D3D12_BLEND_INV_BLEND_FACTOR;
        b.BlendOp = b.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    }
    if (FAILED(CreateGraphicsPipelineCached(dev_.Get(), d, IID_PPV_ARGS(&pso)))) return nullptr;
    return pso.Get();
}

void D3D12Backend::Pass(const char* hlsl, const char* entry, DXGI_FORMAT rtv, D3D12_CPU_DESCRIPTOR_HANDLE rh,
                        uint32_t w, uint32_t h, ID3D12Resource* const src[3], const DXGI_FORMAT fmt[3],
                        const float p[8], float blend) {
    ext_list_ = true;
    ID3D12PipelineState* pso = EnsurePassPipeline(hlsl, entry, rtv, blend);
    if (!pso) return;
    uint32_t slot = AllocSrvSlots(4);
    for (int i = 0; i < 4; i++) {
        D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = i < 3 && src[i] ? fmt[i] : kBgra;
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.Texture2D.MipLevels = 1;
        dev_->CreateShaderResourceView(i < 3 ? src[i] : nullptr, &sv, Cpu(srv_heap_.Get(), slot + i, srv_inc_));
    }
    Alloc cb = Upload(256, 256);
    std::memcpy(cb.cpu, p, 8 * sizeof(float));
    cl_->OMSetRenderTargets(1, &rh, FALSE, nullptr);
    SetFullViewport(w, h);
    if (blend >= 0) {
        float f[4] = {blend, blend, blend, blend};
        cl_->OMSetBlendFactor(f);
        binds_ = {};
    }
    DrawFullScreen(pso, cb.gpu, slot);
    bound_rt_ = bound_ds_ = nullptr;
}

// a copy of the target to read while drawing to it
ID3D12Resource* D3D12Backend::Feedback(Target& t) {
    if (!t.fb) {
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC td = t.res->GetDesc();
        td.Flags = D3D12_RESOURCE_FLAG_NONE;
        t.fb_state = D3D12_RESOURCE_STATE_COPY_DEST;
        if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, t.fb_state, nullptr,
                                                 IID_PPV_ARGS(&t.fb)))) {
            return nullptr;
        }
    }
    Transition(t.res.Get(), t.state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Transition(t.fb.Get(), t.fb_state, D3D12_RESOURCE_STATE_COPY_DEST);
    cl_->CopyResource(t.fb.Get(), t.res.Get());
    if (diag::Perf()) {
        g_perf.feedbacks++;
        g_perf.feedback_bytes += uint64_t(t.w) * t.h * (t.fmt == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8u : 4u);
    }
    Transition(t.fb.Get(), t.fb_state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    return t.fb.Get();
}

// the key's vertex attributes, ATTR0 to ATTR15 in register order; returns how many
uint32_t FillInputLayout(const PsoKey& k, D3D12_INPUT_ELEMENT_DESC el[16]) {
    uint32_t n = 0;
    for (uint32_t r = 0; r < 16; r++) {
        if (!((k.regs >> r) & 1)) continue;
        uint32_t e = k.elements[r];
        el[n++] = {"ATTR", r, DXGI_FORMAT(e & 0xFF), (e >> 8) & 0xFF, e >> 16,
                   D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0};
    }
    return n;
}

D3D12_PRIMITIVE_TOPOLOGY_TYPE TopologyType(uint32_t topo) {
    switch (topo) {
        case 0: return D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
        case 1: return D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
        default: return D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    }
}

constexpr uint32_t kFillWireframe = 0x1B01;  // D3DFILL_WIREFRAME
constexpr uint32_t kCullCw = 0x900;          // D3DCULL_CW

// a D3DCMP test on reversed depth: less and greater trade places
D3D12_COMPARISON_FUNC ReversedCmp(uint32_t v) {
    static const uint8_t mirror[8] = {0, 4, 2, 6, 1, 5, 3, 7};
    return D3D12_COMPARISON_FUNC(mirror[Cmp(v) - 1] + 1);
}

void FillDepthStencilDesc(const PsoKey& k, D3D12_DEPTH_STENCIL_DESC& ds) {
    ds.DepthEnable = k.zen;
    ds.DepthWriteMask = k.zwrite ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    ds.DepthFunc = ReversedCmp(k.zfunc);
    ds.StencilEnable = k.sten;
    ds.StencilReadMask = UINT8(k.smask);
    ds.StencilWriteMask = UINT8(k.swmask);
    D3D12_DEPTH_STENCILOP_DESC so{StencilOp(k.sfail), StencilOp(k.szfail), StencilOp(k.spass), Cmp(k.sfunc)};
    ds.FrontFace = ds.BackFace = so;
}

// everything of the pipeline but its input layout
void D3D12Backend::FillPipelineDesc(const PsoKey& k, ID3DBlob* vs, ID3DBlob* ps,
                                    D3D12_GRAPHICS_PIPELINE_STATE_DESC& d) const {
    d.pRootSignature = root_.Get();
    d.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    d.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    auto& b = d.BlendState.RenderTarget[0];
    b.BlendEnable = k.blend;
    b.SrcBlend = Blend(k.src, false);
    b.DestBlend = Blend(k.dst, false);
    b.BlendOp = BlendOp(k.op);
    b.SrcBlendAlpha = Blend(k.src, true);
    b.DestBlendAlpha = Blend(k.dst, true);
    b.BlendOpAlpha = BlendOp(k.op);
    b.RenderTargetWriteMask = UINT8(k.wmask);
    d.SampleMask = UINT_MAX;
    d.RasterizerState.FillMode = k.fill == kFillWireframe ? D3D12_FILL_MODE_WIREFRAME : D3D12_FILL_MODE_SOLID;
    d.RasterizerState.CullMode = k.cull ? D3D12_CULL_MODE_BACK : D3D12_CULL_MODE_NONE;
    d.RasterizerState.FrontCounterClockwise = k.ccw;
    d.RasterizerState.DepthBias = k.zbias;
    d.RasterizerState.SlopeScaledDepthBias = k.zslope;
    d.RasterizerState.DepthClipEnable = TRUE;
    FillDepthStencilDesc(k, d.DepthStencilState);
    d.PrimitiveTopologyType = TopologyType(k.topo);
    d.NumRenderTargets = k.has_rt ? (k.mv ? (k.rx ? 3 : 2) : 1) : 0;
    d.RTVFormats[0] = k.has_rt ? DXGI_FORMAT(k.rtfmt) : DXGI_FORMAT_UNKNOWN;
    if (k.has_rt && k.mv) {
        // frame generation's motion: written as is by a draw that writes depth, kept by the others
        d.BlendState.IndependentBlendEnable = TRUE;
        d.RTVFormats[1] = DXGI_FORMAT_R16G16_FLOAT;
        auto& m = d.BlendState.RenderTarget[1];
        m.BlendEnable = FALSE;
        m.LogicOpEnable = FALSE;
        m.SrcBlend = m.SrcBlendAlpha = D3D12_BLEND_ONE;
        m.DestBlend = m.DestBlendAlpha = D3D12_BLEND_ZERO;
        m.BlendOp = m.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        m.LogicOp = D3D12_LOGIC_OP_NOOP;
        m.RenderTargetWriteMask = k.mv == 2 ? UINT8(D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_GREEN) : UINT8(0);
    }
    if (k.has_rt && k.mv && k.rx) {
        // anti-aliasing's reactive mask: the most of the effects drawn over each pixel, kept by the other draws
        d.RTVFormats[2] = DXGI_FORMAT_R8_UNORM;
        auto& r = d.BlendState.RenderTarget[2];
        r.BlendEnable = k.rx == 2;
        r.LogicOpEnable = FALSE;
        r.SrcBlend = r.SrcBlendAlpha = D3D12_BLEND_ONE;
        r.DestBlend = r.DestBlendAlpha = D3D12_BLEND_ONE;
        r.BlendOp = r.BlendOpAlpha = D3D12_BLEND_OP_MAX;
        r.LogicOp = D3D12_LOGIC_OP_NOOP;
        r.RenderTargetWriteMask = k.rx == 2 ? UINT8(D3D12_COLOR_WRITE_ENABLE_RED) : UINT8(0);
    }
    d.DSVFormat = k.has_ds ? kDepthFormat : DXGI_FORMAT_UNKNOWN;
    d.SampleDesc.Count = 1;
}

// A point sprite is a square of the program's point size in console pixels (oPts, which SpriteSizeHlsl passes in
// tc3.x) with the sprite's coordinates across it, 0 to 1 from the top left, as the console makes them for stage 3.
std::string SpriteHlsl() {
    std::string s = PointHlsl();
    s.erase(s.find("[maxvertexcount"));
    return s + R"([maxvertexcount(4)]
void main(point VOut v[1], inout TriangleStream<VOut> strip) {
    float2 half_size = v[0].tc3.x * v[0].pos.w / surf.xy;  // in clip units, the target spans 2 across surf.xy pixels
    for (int i = 0; i < 4; i++) {
        VOut o = v[0];
        float2 corner = float2(i & 1, i >> 1);
        o.pos.x += (corner.x * 2 - 1) * half_size.x;
        o.pos.y -= (corner.y * 2 - 1) * half_size.y;
        o.tc0 = float4(corner, 0, 1);  // stage 3 drawn as stage 0 (SpriteStageZero)
        o.tc3 = o.tc0;
        strip.Append(o);
    }
}
)";
}

// a point is drawn as its whole console pixel, or a sprite as its square, by a geometry shader, solid, front facing
// and never culled
void D3D12Backend::ExpandPoints(D3D12_GRAPHICS_PIPELINE_STATE_DESC& d, bool sprite) {
    uint64_t hash;
    ID3DBlob* gs = Compile(sprite ? SpriteHlsl() : PointHlsl(), "main", "gs_5_0", hash);
    if (!gs) return;
    d.GS = {gs->GetBufferPointer(), gs->GetBufferSize()};
    d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    d.RasterizerState.FrontCounterClockwise = FALSE;
}

float D3D12Backend::TargetScale() const {
    const Target* t = bound_rt_ ? bound_rt_ : bound_ds_;
    if (!t || !t->gh) return 1;
    float k = float(t->h) / float(t->gh);
    return InScene() ? k * scene_fy_ : k;
}

// the XDK's screen space offset puts the console's positions half a pixel right and down of the host's, unless a
// program writes them without c-37; render target reads move back that far (TexUv)
float D3D12Backend::RenderTargetShift(const State& s, const DrawShaders& sh) const {
    bool offset = !sh.prog || ReadsViewportOffset(s.vs->program);
    return offset ? -0.5f * TargetScale() : 0.0f;
}

ID3D12PipelineState* D3D12Backend::EnsurePipeline(const PsoKey& k, ID3DBlob* vs, ID3DBlob* ps) {
    uint64_t h = Hash(&k, sizeof(k));
    auto it = psos_.find(h);
    if (it != psos_.end()) return it->second.Get();
    D3D12_INPUT_ELEMENT_DESC el[16];
    uint32_t n = FillInputLayout(k, el);
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
    d.InputLayout = {el, n};
    FillPipelineDesc(k, vs, ps, d);
    if (k.topo == uint32_t(Topology::Points)) ExpandPoints(d, k.sprite);
    ComPtr<ID3D12PipelineState> pso;
    g_perf.psos++;
    PerfScope perf_pso{g_perf.pso};
    if (FAILED(CreateGraphicsPipelineCached(dev_.Get(), d, IID_PPV_ARGS(&pso)))) LOGE("d3d12: pipeline creation failed");
    psos_[h] = pso;
    return pso.Get();
}

TexKind TextureKind(const TextureDesc& d) {
    if (d.cube) return kTexCube;
    if (d.depth > 1) return kTex3D;
    return kTex2D;
}

constexpr float kUpscaledMin = 1.01f;  // host texels per console texel past rounding: the target is upscaled

ID3D12Resource* D3D12Backend::BindTargetTexture(const State& s, int stage, const TextureDesc& d, Target& t, bool box,
                                                DrawTextures& stages) {
    TexInfo& info = stages.info[stage];
    D3D12_SHADER_RESOURCE_VIEW_DESC& sv = stages.sv[stage];
    if (&t == draw_ds_) return nullptr;
    // upscaling: a draw reading the scene's target or depth reads it made whole (its targets are bound again after
    // its textures, BindStageTexturesAndTargets)
    if (SceneTarget(&t)) CloseScene("a draw reads the frame");
    bool feedback = &t == draw_rt_;
    stages.rt[stage] = true;
    info.flags |= kTexTarget;
    if (!t.depth && LatchCpuWrite(t)) {
        Surface su;
        su.header = s.textures[size_t(stage)];
        UploadTarget(t, su);
    }
    info.kind = kTex2D;
    // a pass shrinking it averages each tap over a console texel, else upscaled taps skip texels and ghost
    Target* rt = FindTarget(s.rt, false);
    float k = float(t.h) / (t.gh * stages.k[stage]);
    // the shift back to the console's pixel centre (rtshift) is half a pixel of the target drawn into, by axis
    if (bound_rt_ && bound_rt_->gw && bound_rt_->gh) {
        float a = (float(bound_rt_->w) / bound_rt_->gw) / (float(bound_rt_->h) / bound_rt_->gh);
        if (InScene()) a *= scene_fx_ / scene_fy_;
        info.aspect = std::fabs(a - 1) < 1e-3f ? 0 : uint16_t(std::lround(a * 1000));
    }
    if (!t.depth && box && k > kUpscaledMin && rt && rt != &t && (t.gw > rt->gw || t.gh > rt->gh)) {
        stages.wide[stage] = k;
        info.flags |= kTexWide;
    }
    if (ext_) ext_->Sampled(d, &t, 0, info);
    sv.Format = t.depth ? kDepthRead : t.fmt;
    // the console reads the pixels it is about to overwrite: give the draw a copy
    if (feedback) return Feedback(t);
    Transition(t.res.Get(), t.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    if (t.depth) info.shadow = ShadowDepthKind(d.format);
    info.kind = kTex2D;
    return t.res.Get();
}

bool SameTexture(const Tex& tx, const TextureDesc& d) {
    return tx.res && tx.d.format == d.format && tx.d.width == d.width && tx.d.height == d.height &&
           tx.d.depth == d.depth && tx.d.levels == d.levels && tx.d.cube == d.cube && tx.d.pitch == d.pitch;
}

// false when the texture could not be made, its entry dropped
bool D3D12Backend::CreateTexture(Tex& tx, const TextureDesc& d, const FormatInfo& fi, uint64_t key) {
    g_perf.newtex++;
    if (tx.res) CurrentFrame().garbage.push_back(tx.res);
    if (tx.repl) CurrentFrame().garbage.push_back(tx.repl);
    if (tx.spare) CurrentFrame().garbage.push_back(tx.spare);
    for (auto& m : tx.mat)
        if (m) CurrentFrame().garbage.push_back(m);
    tx = Tex{};
    tx.d = d;
    tx.fmt = HostFormat(fi.host);
    bool bc = fi.compressed;
    uint32_t w = d.width;
    uint32_t h = d.height;
    tx.levels = d.levels;
    if (bc && ((w & 3) || (h & 3))) {
        w = (w + 3) & ~3u;
        h = (h + 3) & ~3u;
        tx.levels = 1;
    }
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC td{};
    td.Dimension = d.depth > 1 ? D3D12_RESOURCE_DIMENSION_TEXTURE3D : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = w;
    td.Height = h;
    td.DepthOrArraySize = UINT16(d.cube ? 6 : d.depth);
    td.MipLevels = UINT16(tx.levels);
    td.Format = tx.fmt;
    td.SampleDesc.Count = 1;
    if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                             IID_PPV_ARGS(&tx.res)))) {
        textures_.erase(key);
        return false;
    }
    return true;
}

void CopyLevelRows(uint8_t* out, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& fp, UINT rows, UINT64 row_size,
                   const std::vector<uint8_t>& px, uint32_t pitch) {
    uint32_t slices = fp.Footprint.Depth;
    size_t src_rows = pitch ? px.size() / pitch / std::max(slices, 1u) : 0;
    for (uint32_t z = 0; z < slices; z++) {
        for (uint32_t y = 0; y < rows; y++) {
            uint8_t* dst = out + (size_t(z) * rows + y) * fp.Footprint.RowPitch;
            if (y < src_rows) {
                std::memcpy(dst, &px[(size_t(z) * src_rows + y) * pitch], std::min<size_t>(pitch, row_size));
            } else {
                std::memset(dst, 0, size_t(row_size));
            }
        }
    }
}

void D3D12Backend::UploadTextureLevel(Tex& tx, uint32_t f, uint32_t l, const std::vector<uint8_t>& px,
                                      uint32_t pitch) {
    D3D12_RESOURCE_DESC rd = tx.res->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
    UINT rows;
    UINT64 row_size;
    UINT64 bytes;
    uint32_t sub = f * tx.levels + l;
    dev_->GetCopyableFootprints(&rd, sub, 1, 0, &fp, &rows, &row_size, &bytes);
    Alloc a = Upload(bytes, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
    CopyLevelRows(a.cpu, fp, rows, row_size, px, pitch);
    fp.Offset = a.off;
    D3D12_TEXTURE_COPY_LOCATION dl{tx.res.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    dl.SubresourceIndex = sub;
    D3D12_TEXTURE_COPY_LOCATION sl{a.res, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    sl.PlacedFootprint = fp;
    cl_->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
}

void D3D12Backend::UploadTexture(Tex& tx, const TextureDesc& d, const FormatInfo& fi, const uint8_t* p,
                                 const uint32_t* colours, bool same) {
    D3D12_RESOURCE_STATES st = same ? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_COPY_DEST;
    Transition(tx.res.Get(), st, D3D12_RESOURCE_STATE_COPY_DEST);
    uint32_t faces = d.cube ? 6 : 1;
    std::vector<uint8_t> px;
    for (uint32_t f = 0; f < faces; f++) {
        for (uint32_t l = 0; l < tx.levels; l++) {
            uint32_t pitch;
            DecodeLevel(d, l, f, p, colours, px, pitch);
            if (ext_) ext_->Decoded(d, fi, f, l, px, pitch, tx.flags);
            // (another format's, a compressed one's: taken as having clear parts, its test as the draw's)
            if (f == 0 && l == 0)
                tx.alpha = fi.host == Host::BGRA8 && !d.cube && d.depth <= 1 && px.size() >= size_t(pitch) * d.height
                               ? AlphaKind(px.data(), d.width, d.height, pitch)
                               : 1;
            UploadTextureLevel(tx, f, l, px, pitch);
        }
    }
    Transition(tx.res.Get(), st, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

void FillTextureView(const Tex& tx, const TextureDesc& d, D3D12_SHADER_RESOURCE_VIEW_DESC& sv) {
    sv.Format = tx.fmt;
    if (d.cube) {
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        sv.TextureCube.MipLevels = tx.levels;
    } else if (d.depth > 1) {
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
        sv.Texture3D.MipLevels = tx.levels;
    } else {
        sv.Texture2D.MipLevels = tx.levels;
    }
}

// a P8 texture's palette: its colours, their address (part of the texture's key) and their hash, all 0 without one
struct StagePalette {
    uint32_t colours[256] = {};
    uint32_t va = 0;
    uint64_t hash = 0;
};

// a P8 texture is decoded with the stage's palette: 256 colours, fewer by the size code in the top two bits
void ReadStagePalette(const State& s, int stage, const TextureDesc& d, StagePalette& palette) {
    constexpr uint32_t kFmtP8 = 0x0B;          // D3DFMT_P8
    constexpr uint32_t kPaletteCommon = 0;     // D3DPalette: Common, its top two bits the size code
    constexpr uint32_t kPaletteData = 4;       // D3DPalette: Data, physical address of the colours
    constexpr uint32_t kPaletteSizeShift = 30;
    if (d.format != kFmtP8 || !s.palettes[size_t(stage)]) return;
    uint32_t header = s.palettes[size_t(stage)];
    palette.va = mem::VirtualFromPhysical(kernel::R32(header + kPaletteData));
    uint32_t size_code = kernel::R32(header + kPaletteCommon) >> kPaletteSizeShift;
    size_t bytes = (std::size(palette.colours) >> size_code) * sizeof(uint32_t);
    std::memcpy(palette.colours, kernel::HostPtr(palette.va), bytes);
    palette.hash = Hash(palette.colours, sizeof(palette.colours));
}

// the extension's replacement for a texture, once it has the pixels: a texture of its own with every level
void D3D12Backend::MakeReplacement(Tex& tx) {
    std::shared_ptr<const Replacement> r = ext_->Replaced(tx.repl_id);
    if (!r) return;  // still loading
    if (r->levels.empty()) {
        tx.repl_id = 0;  // none after all: drawn as the title made it
        return;
    }
    repl_budget_--;
    // its picture's alpha, ray tracing's cut-outs (AlphaKind)
    if (r->width && r->height && r->levels[0].size() >= size_t(r->width) * r->height * 4)
        tx.repl_alpha = AlphaKind(r->levels[0].data(), r->width, r->height, r->width * 4);
    // a picture as a BGRA8 texture with every level, null when it can't be made
    auto make = [&](const Picture& pic, uint32_t& levels) -> ComPtr<ID3D12Resource> {
        if (pic.levels.empty() || !pic.width || !pic.height) return nullptr;
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC td = Texture2DDesc(DXGI_FORMAT_B8G8R8A8_UNORM, pic.width, pic.height);
        td.MipLevels = UINT16(pic.levels.size());
        Tex up;
        if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                 IID_PPV_ARGS(&up.res)))) {
            return nullptr;
        }
        up.levels = uint32_t(pic.levels.size());
        for (uint32_t l = 0; l < up.levels; l++)
            UploadTextureLevel(up, 0, l, pic.levels[l], std::max(pic.width >> l, 1u) * 4);
        D3D12_RESOURCE_STATES st = D3D12_RESOURCE_STATE_COPY_DEST;
        Transition(up.res.Get(), st, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        levels = up.levels;
        return up.res;
    };
    // into the replacement before when it is of this one's size: a movie's next frame
    if (tx.spare && !r->material) {
        D3D12_RESOURCE_DESC sd = tx.spare->GetDesc();
        if (sd.Width == r->width && sd.Height == r->height && sd.MipLevels == r->levels.size()) {
            Tex up;
            up.res = std::move(tx.spare);
            up.levels = uint32_t(r->levels.size());
            D3D12_RESOURCE_STATES st = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            Transition(up.res.Get(), st, D3D12_RESOURCE_STATE_COPY_DEST);
            for (uint32_t l = 0; l < up.levels; l++)
                UploadTextureLevel(up, 0, l, r->levels[l], std::max(r->width >> l, 1u) * 4);
            Transition(up.res.Get(), st, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            tx.repl = std::move(up.res);
            tx.repl_levels = up.levels;
            return;
        }
        CurrentFrame().garbage.push_back(tx.spare);
        tx.spare.Reset();
    }
    tx.repl = make(*r, tx.repl_levels);
    if (!tx.repl) {
        tx.repl_id = 0;  // drawn as the title made it
        return;
    }
    // its material's maps too, both or neither; they take the rest of the frame's budget, three pictures' uploads
    if (const Material* m = r->material.get()) {
        tx.mat[0] = make(m->normal, tx.mat_levels[0]);
        tx.mat[1] = make(m->orm, tx.mat_levels[1]);
        if (!tx.mat[0] || !tx.mat[1]) tx.mat[0] = tx.mat[1] = nullptr;
        std::memcpy(tx.mat_params, m->params, sizeof(tx.mat_params));
        std::memcpy(tx.mat_gloss, m->gloss, sizeof(tx.mat_gloss));
        repl_budget_ = 0;
    }
}

// a texture in guest memory: made again when its layout changed, uploaded again when its bytes did
ID3D12Resource* D3D12Backend::BindGuestTexture(const State& s, int stage, const TextureDesc& d, const FormatInfo& fi,
                                               DrawTextures& stages) {
    TexInfo& info = stages.info[stage];
    StagePalette palette;
    ReadStagePalette(s, stage, d, palette);
    const uint64_t key = uint64_t(palette.va) << 32 | d.data;
    const uint32_t* colours = palette.va ? palette.colours : nullptr;
    Tex& tx = textures_[key];
    tx.used = presents_;
    uint32_t total = TotalSize(d);
    bool same = SameTexture(tx, d);
    if (!same && !CreateTexture(tx, d, fi, key)) return nullptr;
    // cheap change check once per frame: lock epochs plus a sample of the bytes
    if (tx.frame != presents_ + 1 || !same || tx.palette != palette.hash) {
        tx.frame = presents_ + 1;
        tx.palette = palette.hash;
        const uint8_t* p = static_cast<const uint8_t*>(kernel::HostPtr(d.data));
        auto perf_t = PerfClock::now();
        uint64_t sig = MemSig(d.data, total) ^ DirtyEpoch(d.data) ^ palette.hash ^
                       uint64_t(ext_ ? ext_->ReplaceEpoch() : 0) * 0x9E3779B97F4A7C15ull;
        g_perf.sig += PerfMs(perf_t);
        if (sig != tx.sig || !same) {
            PerfScope perf_up{g_perf.texup};
            g_perf.texups++;
            g_perf.texup_bytes += total;
            tx.sig = sig;
            UploadTexture(tx, d, fi, p, colours, same);
            tx.pack_gen = 0;
            uint64_t id = ext_ ? ext_->Replace(d, p, colours) : 0;
            if (id != tx.repl_id) {
                if (tx.repl && !tx.mat[0] && !tx.mat[1]) {
                    if (tx.spare) CurrentFrame().garbage.push_back(tx.spare);
                    tx.spare = tx.repl;
                } else if (tx.repl) {
                    CurrentFrame().garbage.push_back(tx.repl);
                }
                tx.repl.Reset();
                for (auto& m : tx.mat) {
                    if (m) CurrentFrame().garbage.push_back(m);
                    m.Reset();
                }
                tx.repl_id = id;
            }
        }
    }
    if (tx.repl_id && !tx.repl && repl_budget_) {
        PerfScope perf_repl{g_perf.repl};
        MakeReplacement(tx);
        if (tx.repl) g_perf.repls++;
    }
    // an extension's replacement is drawn before the texture pack's image
    if (tx.repl) {
        FillTextureView(tx, d, stages.sv[stage]);
        stages.sv[stage].Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        stages.sv[stage].Texture2D.MipLevels = tx.repl_levels;
        stages.replaced[stage] = true;
        stages.cutout[stage] = tx.repl_alpha;
        if (tx.mat[0] && stages.mat_stage < 0 && !d.cube && d.depth <= 1) {
            stages.mat_stage = stage;
            for (int j = 0; j < 2; j++) {
                D3D12_SHADER_RESOURCE_VIEW_DESC& mv = stages.sv[4 + j];
                mv = {};
                mv.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                mv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                mv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                mv.Texture2D.MipLevels = tx.mat_levels[j];
                stages.res[4 + j] = tx.mat[j].Get();
            }
            std::memcpy(stages.mat, tx.mat_params, sizeof(stages.mat));
            std::memcpy(stages.gloss, tx.mat_gloss, sizeof(stages.gloss));
        }
        stages.data[stage] = d.data;
        if (ext_) ext_->Sampled(d, nullptr, tx.flags, info);
        return tx.repl.Get();
    }
    if (tx.pack_gen != texturedumper::Generation()) LatchPackKey(tx, d, fi, colours);
    if (tx.pack_key) LatchPackImage(tx);
    const Tex& drawn = tx.swap ? *tx.swap : tx;
    FillTextureView(drawn, d, stages.sv[stage]);
    stages.cutout[stage] = tx.swap ? 1 : tx.alpha;
    if (fi.depth) info.shadow = ShadowDepthKind(d.format);
    stages.data[stage] = d.data;
    if (ext_) ext_->Sampled(d, nullptr, tx.flags, info);
    return drawn.res.Get();
}

// after new bytes or a texture setting change: the name the pack and the dumps know the bytes by
void D3D12Backend::LatchPackKey(Tex& tx, const TextureDesc& d, const FormatInfo& fi, const uint32_t* colours) {
    const uint8_t* p = static_cast<const uint8_t*>(kernel::HostPtr(d.data));
    tx.pack_gen = texturedumper::Generation();
    tx.swap = nullptr;
    uint64_t key = texturedumper::Key(d, fi, p, colours);
    texturedumper::Dump(key, d, p, colours);
    // dumping alone keys the bytes too: the pack image is not drawn then
    tx.pack_key = texturedumper::Replacing() ? key : 0;
}

// the pack image for tx's bytes once it has loaded; textures with the same bytes share its GPU copy
void D3D12Backend::LatchPackImage(Tex& tx) {
    auto it = pack_textures_.find(tx.pack_key);
    if (it == pack_textures_.end()) {
        texturedumper::Image img;
        texturedumper::Lookup got = texturedumper::Take(tx.pack_key, img);
        if (got == texturedumper::Lookup::Loading) return;
        if (got == texturedumper::Lookup::None) {
            tx.pack_key = 0;
            return;
        }
        it = pack_textures_.emplace(tx.pack_key, Tex{}).first;
        PerfScope perf_repl{g_perf.repl};
        g_perf.repls++;
        CreatePackTexture(it->second, img);
    }
    if (it->second.res) tx.swap = &it->second;
    tx.pack_key = 0;
}

void D3D12Backend::CreatePackTexture(Tex& r, const texturedumper::Image& img) {
    r.fmt = img.format;
    r.levels = uint32_t(img.levels.size());
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC td = Texture2DDesc(r.fmt, img.width, img.height);
    td.MipLevels = UINT16(r.levels);
    if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                             IID_PPV_ARGS(&r.res)))) {
        LOGE("texture pack: a %ux%u image could not be made", img.width, img.height);
        return;
    }
    for (uint32_t l = 0; l < r.levels; l++) UploadTextureLevel(r, 0, l, img.levels[l].px, img.levels[l].pitch);
    D3D12_RESOURCE_STATES st = D3D12_RESOURCE_STATE_COPY_DEST;
    Transition(r.res.Get(), st, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

// the pack's files changed: each image is made again when next drawn; Generation moved on, so every texture lets
// go of its old copy before it draws
void D3D12Backend::ReleasePackTextures() {
    for (auto& entry : pack_textures_) CurrentFrame().garbage.push_back(entry.second.res);
    pack_textures_.clear();
}

ID3D12Resource* D3D12Backend::BindTexture(const State& s, int stage, DrawTextures& stages) {
    TexInfo& info = stages.info[stage];
    D3D12_SHADER_RESOURCE_VIEW_DESC& sv = stages.sv[stage];
    info = {};
    sv = {};
    stages.rt[stage] = false;
    stages.k[stage] = 1;
    stages.wide[stage] = 0;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sv.Texture2D.MipLevels = 1;
    TextureDesc d;
    if (!s.textures[size_t(stage)] || !Describe(s.textures[size_t(stage)], d)) return nullptr;
    const FormatInfo& fi = Format(d.format);
    info.kind = TextureKind(d);
    info.rect = d.linear;

    // rendered by the GPU: sample the target itself; DXT data loaded over a target's memory is not it
    uint64_t alt = 0;
    bool box = true;
    if (ext_) ext_->Sample(d.data, fi.depth, stage, alt, stages.k[stage], box);
    auto tit = targets_.find(SizedTargetKey(d.data, fi.depth, d.width, d.height) | alt);
    if (tit != targets_.end() && !d.cube && d.depth == 1 && !fi.compressed) {
        return BindTargetTexture(s, stage, d, tit->second, box, stages);
    }
    return BindGuestTexture(s, stage, d, fi, stages);
}

// D3DTEXF: 1 point, 2 linear, 3 anisotropic
constexpr uint32_t kTexfLinear = 2;
constexpr uint32_t kTexfAnisotropic = 3;
// D3DTADDRESS values the samplers test
constexpr uint32_t kTaddressWrap = 1;
constexpr uint32_t kTaddressClamp = 3;

// bias: added to the level of detail of a picture's reads (not a target's, not a shadow's): the upscaling scene's,
// so textures drawn at the part's size are read as sharp as at the whole
D3D12_SAMPLER_DESC StageSampler(const State& s, int i, const DrawTextures& stages, bool clamp, uint32_t aniso,
                                float bias) {
    D3D12_SAMPLER_DESC sd = {};
    const auto& t = s.tss[size_t(i)];
    uint32_t mag = t[kTssMagFilter];
    uint32_t min = t[kTssMinFilter];
    uint32_t mip = t[kTssMipFilter];
    auto lin = [](uint32_t f) { return f >= kTexfLinear; };
    D3D12_FILTER_TYPE fmin = lin(min) ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT;
    D3D12_FILTER_TYPE fmag = lin(mag) ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT;
    D3D12_FILTER_TYPE fmip = mip == kTexfLinear ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT;
    sd.Filter = D3D12_FILTER(fmin << 4 | fmag << 2 | fmip);
    bool anisotropic = min == kTexfAnisotropic || mag == kTexfAnisotropic;
    uint32_t an = anisotropic ? std::max<uint32_t>(t[kTssMaxAnisotropy], 1) : 1;
    // a replacement is far larger than the console's texture: filtered across its levels whatever the title asked
    if (stages.replaced[i]) {
        mip = kTexfLinear;
        sd.Filter = D3D12_FILTER(fmin << 4 | fmag << 2 | D3D12_FILTER_TYPE_LINEAR);
    }
    // forced wherever the title filters a texture linearly as it shrinks, whether or not it has mip levels: a title
    // whose textures have none (Castlevania's) gets its samples along a slope from level 0, where surfaces seen at an
    // angle shimmer without them; a quad drawn flat to the screen samples the same as before
    if (aniso && lin(min) && !stages.info[i].shadow) an = aniso;
    if (an > 1) {
        sd.Filter = D3D12_FILTER_ANISOTROPIC;
        sd.MaxAnisotropy = std::min<uint32_t>(an, 16);
    }
    // clamp: the draw's coordinates all lie in 0..1 (UnitTexcoords), so wrap is read as clamp
    sd.AddressU = Address(clamp && t[kTssAddressU] == kTaddressWrap ? kTaddressClamp : t[kTssAddressU]);
    sd.AddressV = Address(clamp && t[kTssAddressV] == kTaddressWrap ? kTaddressClamp : t[kTssAddressV]);
    sd.AddressW = Address(t[kTssAddressW]);
    sd.MipLODBias = Bits(t[kTssMipLodBias]);
    if (bias != 0 && !stages.rt[i] && !stages.info[i].shadow)
        sd.MipLODBias = std::clamp(sd.MipLODBias + bias, -16.0f, 15.99f);
    sd.MinLOD = stages.replaced[i] ? 0.0f : float(t[kTssMaxMipLevel]);
    sd.MaxLOD = mip ? D3D12_FLOAT32_MAX : sd.MinLOD;
    Color(t[kTssBorderColor], sd.BorderColor);
    if (stages.info[i].shadow) {
        // the console tests texel OP r, D3D12 tests r OP texel; with both reversed (g_reversed_depth) it is the same OP
        sd.Filter = lin(min) || lin(mag) ? D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT
                                         : D3D12_FILTER_COMPARISON_MIN_MAG_MIP_POINT;
        sd.ComparisonFunc = Cmp(s.rs[kRsShadowFunc]);
        sd.MaxAnisotropy = 0;
    }
    return sd;
}

uint32_t D3D12Backend::EnsureStageSamplers(const State& s, const DrawTextures& stages, bool clamp) {
    D3D12_SAMPLER_DESC sd[4];
    uint32_t aniso = app::Get().aniso;
    float bias = SceneMipBias();
    for (int i = 0; i < 4; i++) sd[i] = StageSampler(s, i, stages, clamp, aniso, bias);
    uint64_t key = Hash(sd, sizeof(sd));
    auto it = samplers_.find(key);
    if (it != samplers_.end()) return it->second;
    if (smp_used_ + 4 > kSamplerCount) {
        samplers_.clear();
        smp_used_ = 0;
    }
    uint32_t base = smp_used_;
    smp_used_ += 4;
    for (int i = 0; i < 4; i++) dev_->CreateSampler(&sd[i], Cpu(smp_heap_.Get(), base + i, smp_inc_));
    samplers_[key] = base;
    return base;
}

uint32_t SimpleState(const State& s, uint32_t method) {
    return s.simple[method >> 2];
}

constexpr uint32_t kQuadVertices = 6;  // a quad drawn as two triangles

DrawShaders ClassifyShaders(const State& s, const Assembled& a, uint32_t count) {
    DrawShaders sh;
    // a shader made from a declaration alone (no program) is the fixed function pipeline on those inputs
    sh.prog = (s.vs_handle & 1) && s.vs && !s.vs->program.empty();
    // up to a quad with no depth test: it may be a pass over the whole target, see QuadHlsl; points never are
    bool points = a.topology == Topology::Points;
    sh.quad = count <= kQuadVertices && !s.rs[kRsZEnable] && !points;
    sh.fixed_transform = !sh.prog && !a.pretransformed;
    sh.sprite = points && sh.prog && s.rs[kRsPointSpriteEnable];
    if (!sh.prog) sh.fk = MakeFixedKey(s);
    return sh;
}

// the program's hash is cached in the guest shader object
void EnsureProgramHash(const State& s) {
    if (!s.vs->hash) s.vs->hash = Hash(s.vs->program.data(), s.vs->program.size() * 4) | 1;
}

// vertex shader cache key salts
constexpr uint64_t kDeclarationSalt = 0x10000;    // fixed function seed, above the 16 bit register mask
constexpr uint64_t kPretransformedSalt = 0x5A5A;  // screen space fixed function vertices, apart from untransformed
constexpr uint64_t kQuadSalt = 0xC3C30000;        // the QuadHlsl variant, apart from the plain shader
constexpr uint64_t kSpriteSalt = 0x3C3C0000;      // the SpriteSizeHlsl variant

uint64_t VertexKey(const State& s, const Assembled& a, const DrawShaders& sh) {
    uint64_t seed = sh.prog ? s.vs->hash ^ a.regs : Hash(&s.vs_handle, 4, a.regs | kDeclarationSalt);
    uint64_t vkey = Hash(&a.decode, 8, seed);
    if (!sh.prog && a.pretransformed) vkey ^= kPretransformedSalt;
    if (sh.quad) vkey ^= kQuadSalt;
    if (sh.sprite) vkey ^= kSpriteSalt;
    if (!sh.prog) vkey = Hash(&sh.fk, sizeof(sh.fk), vkey);
    return vkey;
}

// a point sprite's program hands its point size to SpriteHlsl in tc3, whose coordinates the sprite makes
std::string SpriteSizeHlsl(std::string s) {
    const char* tc3 = "    o.tc3 = oT3;\n";
    size_t at = s.find(tc3);
    if (at == std::string::npos) {
        LOGE("d3d12: a vertex shader no longer fits SpriteSizeHlsl");
        return s;
    }
    s.replace(at, std::strlen(tc3), "    o.tc3 = oPts;\n");
    return s;
}

std::string VertexHlsl(const State& s, const Assembled& a, const DrawShaders& sh) {
    if (sh.prog) {
        std::string src = VertexProgramHlsl(s.vs->program, a.regs, a.decode);
        return sh.sprite ? SpriteSizeHlsl(std::move(src)) : src;
    }
    return FixedVertexHlsl(a.regs, a.pretransformed, a.decode, sh.fk);
}

// sets sh.vh and sh.vs_code, compiling the shader on a cache miss
void D3D12Backend::EnsureVertexShader(const State& s, const Assembled& a, DrawShaders& sh) {
    auto vit = shader_src_.find(sh.vkey);
    if (vit != shader_src_.end()) {
        sh.vs_code = blobs_[vit->second].Get();
        sh.vh = vit->second;
        return;
    }
    std::string src = ReversedDepthHlsl(VertexHlsl(s, a, sh));
    sh.vs_code = Compile(sh.quad ? QuadHlsl(std::move(src)) : src, "main", "vs_5_0", sh.vh);
    shader_src_[sh.vkey] = sh.vh;
}

bool D3D12Backend::BindStageTexturesAndTargets(const State& s, DrawTextures& stages) {
    draw_rt_ = bound_rt_;
    draw_ds_ = bound_ds_;
    {
        PerfScope perf_bind{g_perf.bind};
        for (int i = 0; i < 4; i++) stages.res[i] = BindTexture(s, i, stages);
    }
    // textures may have recorded copies that unbound the targets or moved them out of their target states
    if (bound_rt_ != draw_rt_ || bound_ds_ != draw_ds_) return BindTargets(s);
    if (bound_rt_) Transition(bound_rt_->res.Get(), bound_rt_->state, D3D12_RESOURCE_STATE_RENDER_TARGET);
    if (bound_ds_) Transition(bound_ds_->res.Get(), bound_ds_->state, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    return true;
}

constexpr uint64_t kPixelKeySalt = 0x9E3779B97F4A7C15ull;

// sets sh.ph and sh.ps_code, compiling the shader on a cache miss
void D3D12Backend::EnsurePixelShader(const PixelKey& pk, DrawShaders& sh) {
    // pixel shaders share the map with vertex shaders, the salt keeps their keys apart
    auto pit = shader_src_.find(sh.pkey ^ kPixelKeySalt);
    if (pit != shader_src_.end()) {
        sh.ps_code = blobs_[pit->second].Get();
        sh.ph = pit->second;
        return;
    }
    sh.ps_code = Compile(PixelHlsl(pk), "main", "ps_5_0", sh.ph);
    shader_src_[sh.pkey ^ kPixelKeySalt] = sh.ph;
}

void KeyBlend(const State& s, PsoKey& k) {
    k.blend = SimpleState(s, kMBlendEnable) != 0;
    if (k.blend) {
        k.src = SimpleState(s, kMSrcBlend);
        k.dst = SimpleState(s, kMDestBlend);
        k.op = SimpleState(s, kMBlendOp);
    }
    uint32_t cw = SimpleState(s, kMColorWriteEnable);
    // console ARGB bytes to D3D12 RGBA bits
    k.wmask = ((cw >> 16) & 1) | ((cw >> 8) & 1) << 1 | (cw & 1) << 2 | ((cw >> 24) & 1) << 3;
}

constexpr uint32_t kZUseW = 2;  // D3DZB_USEW

bool OffsetsPolygons(const State& s, Topology topology) {
    return topology == Topology::Triangles && SimpleState(s, kMSolidOffsetEnable);
}

// polygon offset slopes are per console pixel, an upscaled target's pixels are smaller (f: the scene's share of it)
float HostSlopeScale(const State& s, const Target* ds, float f) {
    return Bits(SimpleState(s, kMSlopeScale)) * (ds ? f * float(ds->h) / ds->gh : 1.0f);
}

void D3D12Backend::KeyDepthStencil(const State& s, const Assembled& a, PsoKey& k) {
    k.zen = s.rs[kRsZEnable] != 0;
    if (k.zen) {
        k.zwrite = SimpleState(s, kMZWriteEnable) != 0;
        k.zfunc = SimpleState(s, kMZFunc);
    }
    k.sten = s.rs[kRsStencilEnable] != 0;
    if (k.sten) {
        k.sfunc = SimpleState(s, kMStencilFunc);
        k.smask = SimpleState(s, kMStencilMask) & 0xFF;
        k.swmask = SimpleState(s, kMStencilWriteMask) & 0xFF;
        k.sfail = s.rs[kRsStencilFail];
        k.szfail = SimpleState(s, kMStencilZFail);
        k.spass = SimpleState(s, kMStencilPass);
    }
    // polygon offset: the slope here, toward the camera is up on reversed depth; the units are in ReversedDepthHlsl
    if (OffsetsPolygons(s, a.topology)) k.zslope = -HostSlopeScale(s, bound_ds_, InScene() ? scene_fy_ : 1.0f);
}

void KeyRasterizer(const State& s, const Assembled& a, PsoKey& k) {
    uint32_t cull = s.rs[kRsCullMode];
    k.cull = cull != 0 && a.topology == Topology::Triangles;
    k.ccw = cull == kCullCw;  // culling clockwise leaves counter clockwise as the front
    k.fill = s.rs[kRsFillMode];
}

void D3D12Backend::BuildPipelineKey(const State& s, const Assembled& a, uint64_t vh, uint64_t ph, PsoKey& k) {
    std::memset(&k, 0, sizeof(k));
    k.vs = vh;
    k.ps = ph;
    k.regs = a.regs;
    for (int r = 0; r < 16; r++) {
        if ((a.regs >> r) & 1) {
            const VertexElement& e = a.elements[size_t(r)];
            k.elements[r] = e.format | uint32_t(e.buffer) << 8 | uint32_t(e.offset) << 16;
        }
    }
    k.topo = uint32_t(a.topology);
    k.has_rt = bound_rt_ != nullptr;
    k.has_ds = bound_ds_ != nullptr;
    k.rtfmt = bound_rt_ ? bound_rt_->fmt : 0;
    KeyBlend(s, k);
    if (k.has_ds) KeyDepthStencil(s, a, k);
    KeyRasterizer(s, a, k);
}

// alpha test off: compare always, in Cmp() minus 1 terms like the enabled test
constexpr float kAlphaTestAlways = float(D3D12_COMPARISON_FUNC_ALWAYS - 1);

// D3DRS_FOGTABLEMODE values
constexpr uint32_t kFogTableNone = 0;
constexpr float kFogTableLinear = 3.0f;

// table mode none under a vertex program: oFog is the blend factor, passed on as linear fog from 1 to 0
void FillFogConstants(const State& s, bool vertex_program, DrawConstants& c) {
    if (!s.rs[kRsFogEnable]) return;
    if (vertex_program && s.rs[kRsFogTableMode] == kFogTableNone) {
        c.fog[0] = 1.0f;
        c.fog[1] = 0.0f;
        c.fog[2] = 0.0f;
        c.fog[3] = kFogTableLinear;
        return;
    }
    c.fog[0] = Bits(s.rs[kRsFogStart]);
    c.fog[1] = Bits(s.rs[kRsFogEnd]);
    c.fog[2] = Bits(s.rs[kRsFogDensity]);
    c.fog[3] = float(s.rs[kRsFogTableMode]);
}

// D3DRS_MULTISAMPLEMODE on the back buffer, D3DRS_MULTISAMPLERENDERTARGETMODE on any other target: on a linear surface
// 2X and 4X draw on a grid of half its columns, 4X of half its rows too, which the hardware spreads over the whole
// surface (a title draws a 4X shadow volume on 320x240 over a 640x480 back buffer)
GuestExtent MultisampleGrid(const State& s, const Surface& target, GuestExtent size) {
    constexpr uint32_t kMode2x = 1;  // D3DMULTISAMPLEMODE_2X
    constexpr uint32_t kMode4x = 2;  // D3DMULTISAMPLEMODE_4X
    uint32_t mode = s.rt_back_buffer ? s.rs[kRsMultiSampleMode] : s.rs[kRsMultiSampleRenderTargetMode];
    TextureDesc d;
    if ((mode != kMode2x && mode != kMode4x) || !Describe(target.header, d) || !d.linear) return size;
    LOG_FIRST(2, "d3d12: multisample %uX on %ux%u at %08X, drawn on %ux%u", mode * 2, size.w, size.h, target.data,
              (size.w + 1) / 2, mode == kMode4x ? (size.h + 1) / 2 : size.h);
    size.w = (size.w + 1) / 2;
    if (mode == kMode4x) size.h = (size.h + 1) / 2;
    return size;
}

void FillSurfaceConstants(const State& s, GuestExtent size, DrawConstants& c) {
    c.surf[0] = float(size.w);
    c.surf[1] = float(size.h);
    c.surf[2] = 1.0f / s.zscale;
    Color(s.rs[kRsFogColor], c.fog_color);
    c.misc[0] = float(SimpleState(s, kMAlphaRef) & 0xFF);
    c.misc[1] = SimpleState(s, kMAlphaTestEnable) ? float(Cmp(SimpleState(s, kMAlphaFunc)) - 1) : kAlphaTestAlways;
    c.misc[2] = float(Cmp(s.rs[kRsShadowFunc]) - 1);
    c.misc[3] = s.rs[kRsSpecularEnable] ? 1.0f : 0.0f;
}

void FillCombinerConstants(const State& s, DrawConstants& c) {
    for (int i = 0; i < 8; i++) {
        Color(SimpleState(s, kMPsConstant0 + i * 4), c.pc[i * 2]);
        Color(SimpleState(s, kMPsConstant1 + i * 4), c.pc[i * 2 + 1]);
    }
    Color(SimpleState(s, kMPsFinalConstant0), c.pc[16]);
    Color(SimpleState(s, kMPsFinalConstant1), c.pc[17]);
    Color(s.rs[kRsTextureFactor], c.tfactor);
}

constexpr float kGlowBoxTexels = 3.0f;  // console texels across the box each wide tap averages
// parallax (g_surface_depth): low and high, the depth of a material's deepest point as a part of its picture, and the
// most layers the view ray steps through (more edge-on)
constexpr float kParallaxLow = 0.002f, kParallaxHigh = 0.004f;
constexpr float kParallaxLayersLow = 12.0f, kParallaxLayersHigh = 24.0f;

void D3D12Backend::FillTexelConstants(int i, const TextureDesc& d, const DrawTextures& stages, DrawConstants& c) {
    c.texsize[i][0] = float(d.width);
    c.texsize[i][1] = float(d.height);
    if (!stages.rt[i]) return;
    float k = stages.k[i];
    // a 3 console texel box per tap keeps the console chain's glow width and brightness
    if (stages.wide[i] > 0) {
        float n = std::ceil(kGlowBoxTexels * stages.wide[i] * 0.5f);
        c.spread[i][0] = kGlowBoxTexels / (n * float(d.width) * k);
        c.spread[i][1] = kGlowBoxTexels / (n * float(d.height) * k);
        c.spread[i][2] = n;
    }
}

void D3D12Backend::FillTextureConstants(const State& s, const DrawShaders& sh, const DrawTextures& stages,
                                        DrawConstants& c) {
    for (int i = 0; i < 4; i++) {
        TextureDesc d;
        if (s.textures[size_t(i)] && Describe(s.textures[size_t(i)], d)) FillTexelConstants(i, d, stages, c);
        if (stages.rt[i]) c.rtshift[i] = RenderTargetShift(s, sh);
        const auto& t = s.tss[size_t(i)];
        c.bump[i][0] = Bits(t[kTssBumpEnvMat00]);
        c.bump[i][1] = Bits(t[kTssBumpEnvMat01]);
        c.bump[i][2] = Bits(t[kTssBumpEnvMat10]);
        c.bump[i][3] = Bits(t[kTssBumpEnvMat11]);
        c.bumpl[i][0] = Bits(t[kTssBumpEnvLScale]);
        c.bumpl[i][1] = Bits(t[kTssBumpEnvLOffset]);
    }
}

void MatMul(const float* x, const float* y, float* o) {
    for (int r = 0; r < 4; r++) {
        for (int col = 0; col < 4; col++) {
            o[r * 4 + col] = x[r * 4] * y[col] + x[r * 4 + 1] * y[4 + col] + x[r * 4 + 2] * y[8 + col] +
                             x[r * 4 + 3] * y[12 + col];
        }
    }
}

// HLSL mul(row vector, matrix) with column major packing wants the transpose
void Transpose(const float* m, float* o) {
    for (int r = 0; r < 4; r++) {
        for (int col = 0; col < 4; col++) o[col * 4 + r] = m[r * 4 + col];
    }
}

void InverseTranspose3x3(const float* wv, float* n) {
    float det = wv[0] * (wv[5] * wv[10] - wv[6] * wv[9]) - wv[1] * (wv[4] * wv[10] - wv[6] * wv[8]) +
                wv[2] * (wv[4] * wv[9] - wv[5] * wv[8]);
    float id = det != 0 ? 1.0f / det : 0.0f;
    for (int r = 0; r < 3; r++) {
        for (int col = 0; col < 3; col++) {
            int r1 = (r + 1) % 3;
            int r2 = (r + 2) % 3;
            int c1 = (col + 1) % 3;
            int c2 = (col + 2) % 3;
            n[r * 4 + col] = (wv[r1 * 4 + c1] * wv[r2 * 4 + c2] - wv[r1 * 4 + c2] * wv[r2 * 4 + c1]) * id;
        }
    }
}

// D3DTRANSFORMSTATETYPE indices into s.transforms
constexpr size_t kTsView = 2;
constexpr size_t kTsProjection = 3;
constexpr size_t kTsTexture0 = 16;
constexpr size_t kTsWorld = 256;

void FillTransformConstants(const State& s, GuestExtent size, DrawConstants& c) {
    uint32_t gw = size.w;
    uint32_t gh = size.h;
    // world, view, projection, then the viewport inside the target
    float wv[16];
    float wvp[16];
    MatMul(s.transforms[kTsWorld].data(), s.transforms[kTsView].data(), wv);
    MatMul(wv, s.transforms[kTsProjection].data(), wvp);
    const auto& v = s.viewport;
    float sx = float(v.w) / gw;
    float sy = float(v.h) / gh;
    float ox = (2.0f * v.x + v.w) / gw - 1;
    float oy = 1 - (2.0f * v.y + v.h) / gh;
    float vpm[16] = {sx, 0, 0, 0, 0, sy, 0, 0, 0, 0, v.max_z - v.min_z, 0, ox, oy, v.min_z, 1};
    float full[16];
    MatMul(wvp, vpm, full);
    Transpose(full, c.wvp);
    Transpose(wv, c.wv);
    // normals take the inverse transpose of the upper 3x3
    float n[16] = {};
    InverseTranspose3x3(wv, n);
    Transpose(n, c.wvn);
    for (int t = 0; t < 4; t++) Transpose(s.transforms[kTsTexture0 + size_t(t)].data(), c.tm[t]);
}

// D3DLIGHT8 as floats: Type, Diffuse[4], Specular[4], Ambient[4], Position[3], Direction[3], Range, Falloff,
// Attenuation0 to 2, Theta, Phi
constexpr int kLightFloats = 26;
constexpr int kLightDiffuse = 1;
constexpr int kLightSpecular = 5;
constexpr int kLightAmbient = 9;
constexpr int kLightPosition = 13;
constexpr int kLightDirection = 16;
constexpr int kLightRange = 19;
constexpr int kLightFalloff = 20;
constexpr int kLightAttenuation0 = 21;
constexpr int kLightAttenuation1 = 22;
constexpr int kLightAttenuation2 = 23;
constexpr int kLightTheta = 24;
constexpr int kLightPhi = 25;

void LightToView(const std::array<uint32_t, kLightFloats>& l, const float* vm, float (&o)[7][4]) {
    float f[kLightFloats];
    std::memcpy(f, l.data(), sizeof(f));
    for (int j = 0; j < 4; j++) {
        o[0][j] = f[kLightDiffuse + j];
        o[1][j] = f[kLightSpecular + j];
        o[2][j] = f[kLightAmbient + j];
    }
    const float* pos = f + kLightPosition;
    const float* dir = f + kLightDirection;
    for (int j = 0; j < 3; j++) {
        o[3][j] = pos[0] * vm[j] + pos[1] * vm[4 + j] + pos[2] * vm[8 + j] + vm[12 + j];
        o[4][j] = dir[0] * vm[j] + dir[1] * vm[4 + j] + dir[2] * vm[8 + j];
    }
    o[3][3] = f[kLightRange];
    o[4][3] = f[kLightFalloff];
    o[5][0] = f[kLightAttenuation0];
    o[5][1] = f[kLightAttenuation1];
    o[5][2] = f[kLightAttenuation2];
    o[6][0] = std::cos(f[kLightTheta] * 0.5f);
    o[6][1] = std::cos(f[kLightPhi] * 0.5f);
}

void FillLightConstants(const State& s, const FixedKey& fk, DrawConstants& c) {
    std::memcpy(c.mat, s.material.data(), sizeof(float) * 16);
    c.mat[4][0] = s.material[16];
    Color(s.rs[kRsAmbient], c.amb);
    // lights to view space
    const float* vm = s.transforms[kTsView].data();
    for (int i = 0; i < 8; i++) {
        if (!((fk.types >> (i * 2)) & 3)) continue;
        LightToView(s.lights[size_t(i)], vm, c.lt[i]);
    }
}

// a w buffer's clip range in the units of a draw's clip w
struct WRange {
    float near_w;
    float far_w;
};

// the projection's near and far clip w as D3DDevice_SetTransform finds them
WRange ProjectionWRange(const float* m) {
    float m33 = m[10];
    float m34 = m[11];
    float m43 = m[14];
    float m44 = m[15];
    if (m33 == m34 || m33 == 0) return {0, 1};
    return {m44 - m43 / m33 * m34, (m44 - m43) / (m33 - m34) * m34 + m44};
}

// NV097_SET_CLIP_MIN and MAX in buffer units, sent by D3DDevice_SetDepthClipPlanes
constexpr uint32_t kMClipMin = 0x394;
constexpr uint32_t kMClipMax = 0x398;
constexpr float kFallbackNear = 1.0f / 65536;  // of the far w, when no range is known

// Fixed function clip w is the host's view depth over the projection's range; a program's is in buffer units up to
// zscale, its near end the clip minimum the title sent
WRange DrawWRange(const State& s, bool program) {
    WRange r{0, s.zscale};
    if (!program) {
        r = ProjectionWRange(s.transforms[kTsProjection].data());
    } else if (SimpleState(s, kMClipMax)) {
        r.near_w = Bits(SimpleState(s, kMClipMin));
    }
    if (r.near_w <= 0 || r.near_w >= r.far_w) r.near_w = r.far_w * kFallbackNear;
    return r;
}

// ReversedDepthHlsl's zparams: x the w buffer's scale (0 for a z buffer), y its far w, z the polygon offset in units of
// the clip w for a w buffer, of depth for a z buffer
void D3D12Backend::FillDepthConstants(const State& s, const DrawShaders& sh, const PsoKey& k, DrawConstants& c) const {
    bool offset = OffsetsPolygons(s, Topology(k.topo));
    float units = offset ? Bits(SimpleState(s, kMDepthOffset)) : 0;
    if (s.rs[kRsZEnable] != kZUseW) {
        c.zparams[2] = units / s.zscale;
        return;
    }
    WRange r = DrawWRange(s, sh.prog);
    c.zparams[0] = r.near_w / (r.far_w - r.near_w);
    c.zparams[1] = r.far_w;
    c.zparams[2] = sh.prog ? units : units * r.far_w / s.zscale;
}

// a fresh copy of the draw's vertex constants as edited, kept as the last upload (by the title's, s.vsc)
void D3D12Backend::UploadVertexConstants(const State& s, const void* edited, const float* prev) {
    // with the frame before's after them (VC's cp) for a draw's motion, else only the draw's
    Alloc vc = Upload(prev ? 2 * sizeof(s.vsc) : sizeof(s.vsc), 256);
    std::memcpy(vc.cpu, edited, sizeof(s.vsc));
    if (prev) std::memcpy(vc.cpu + sizeof(s.vsc), prev, sizeof(s.vsc));
    std::memcpy(last_vsc_.data(), s.vsc.data(), sizeof(s.vsc));
    last_vsc_gpu_ = vc.gpu;
}

void D3D12Backend::UploadConstants(const State& s, const DrawShaders& sh, const PsoKey& k, const DrawTextures& stages,
                                   GuestExtent size, DrawConstants& c) {
    // the extension edits a copy, so its draws always get their own upload
    DrawLap lap;
    bool motion = k.mv == 2;
    const float* prev = motion ? PrevMotionConstants(draw_mv_key_, &s.vsc[0][0]) : nullptr;
    lap.Into(g_perf.clap, 0);
    bool same = !ext_ && !prev && last_vsc_gpu_ && !std::memcmp(last_vsc_.data(), s.vsc.data(), sizeof(s.vsc));
    float(*vsc)[4] = nullptr;
    if (!same) {
        std::memcpy(vsc_edit_.data(), s.vsc.data(), sizeof(s.vsc));
        vsc = reinterpret_cast<float(*)[4]>(vsc_edit_.data());
    }
    c.motion[0] = prev ? 1.0f : 0.0f;
    if (draw_jitter_ && bound_rt_ && aa_jitter_w_ && aa_jitter_h_) {
        c.motion[2] = 2 * aa_jitter_[0] / float(aa_jitter_w_);
        c.motion[3] = -2 * aa_jitter_[1] / float(aa_jitter_h_);
    }
    FillSurfaceConstants(s, size, c);
    // points: host pixels per console pixel, also keeps them off the far edge pull out of screen passes
    if (k.topo == uint32_t(Topology::Points)) c.surf[3] = TargetScale();
    FillFogConstants(s, sh.prog, c);
    FillCombinerConstants(s, c);
    lap.Into(g_perf.clap, 4);
    if (ext_) ext_->Constants(vsc, c.ext, bound_rt_, k.blend ? k.src : 1);
    lap.Into(g_perf.clap, 2);
    if (motion) KeepMotionConstants(draw_mv_key_, reinterpret_cast<const uint8_t*>(vsc ? vsc[0] : s.vsc[0].data()));
    lap.Into(g_perf.clap, 3);
    if (vsc) UploadVertexConstants(s, vsc, prev);
    lap.Into(g_perf.clap, 1);
    FillDepthConstants(s, sh, k, c);
    FillTextureConstants(s, sh, stages, c);
    if (draw_texbox_) std::memcpy(c.uvbox, draw_uvbox_, sizeof(c.uvbox));
    if (stages.mat_stage >= 0) {
        std::memcpy(c.mtl, stages.mat, sizeof(stages.mat));
        c.motion[1] = SceneMipBias();  // which the parallax's reads take back out
        // parallax (g_surface_depth): the deepest point's depth, a part of the picture's width, and the most layers
        // the view ray steps through
        uint32_t depth = g_surface_depth.load(std::memory_order_relaxed);
        c.mtl[1][3] = depth == 0 ? 0.0f : depth == 1 ? kParallaxLow : kParallaxHigh;
        c.mtl[0][0] = depth >= 2 ? kParallaxLayersHigh : kParallaxLayersLow;
        // the view's half angles, a camera of about 50 degrees up and down, across by the target's shape
        float w = float(bound_rt_ ? bound_rt_->w : 1), h = float(bound_rt_ ? bound_rt_->h : 1);
        if (InScene()) {
            w = float(scene_w_);
            h = float(scene_h_);
        }
        constexpr float kHalfTan = 0.466f;
        c.mtl[3][0] = kHalfTan * w / h;
        c.mtl[3][1] = kHalfTan;
        c.mtl[3][2] = w;
        c.mtl[3][3] = h;
    }
    if (sh.fixed_transform) {
        FillTransformConstants(s, size, c);
        FillLightConstants(s, sh.fk, c);
    }
    if (!last_dc_gpu_ || std::memcmp(&last_dc_, &c, sizeof(c))) {
        Alloc dc = Upload(sizeof(DrawConstants), 256);
        std::memcpy(dc.cpu, &c, sizeof(c));
        last_dc_ = c;
        last_dc_gpu_ = dc.gpu;
    }
    if (last_vsc_gpu_ != binds_.vsc) cl_->SetGraphicsRootConstantBufferView(0, last_vsc_gpu_);
    if (last_dc_gpu_ != binds_.dc) cl_->SetGraphicsRootConstantBufferView(1, last_dc_gpu_);
    binds_.vsc = last_vsc_gpu_;
    binds_.dc = last_dc_gpu_;
    lap.Into(g_perf.clap, 4);
}

void D3D12Backend::CaptureTextures(const State& s, const DrawTextures& stages) {
    for (int i = 0; i < 4; i++) {
        ID3D12Resource* res = stages.res[i];
        TextureDesc d;
        if (!s.textures[size_t(i)] || !Describe(s.textures[size_t(i)], d)) continue;
        bool tg = targets_.count(SizedTargetKey(d.data, Format(d.format).depth, d.width, d.height));
        fprintf(cap_, " | t%d %08X fmt %X %ux%u%s%s", i, d.data, d.format, d.width, d.height, tg ? " target" : "",
                res ? "" : " unbound");
        if (res && !tg && stages.info[i].kind == kTex2D && cap_seen_.insert(d.data).second) {
            cap_tex_.push_back({res, stages.sv[i].Format, d.data, d.width, d.height});
        }
    }
}

// s.vsc holds the 192 vertex shader constants in hardware order: D3D register + 96
constexpr int kVsConstants = 192;
constexpr int kVsConstantBias = 96;

void D3D12Backend::CaptureInputs(const State& s, const Assembled& a, const DrawConstants& c) {
    fprintf(cap_, "\n     verts");
    uint32_t n = 0;
    for (int r = 0; r < 16; r++) n += (a.regs >> r) & 1;
    for (size_t v = 0; v < std::min<size_t>(4, n ? a.vertices.size() / (n * 4) : 0); v++) {
        fprintf(cap_, " [");
        for (uint32_t k = 0; k < n * 4; k++) fprintf(cap_, " %g", a.vertices[v * n * 4 + k]);
        fprintf(cap_, " ]");
    }
    for (int r = 0; r < kVsConstants; r++) {
        const auto& v = s.vsc[r];
        if (v[0] || v[1] || v[2] || v[3]) {
            fprintf(cap_, "\n     c%d %g %g %g %g", r - kVsConstantBias, v[0], v[1], v[2], v[3]);
        }
    }
    fprintf(cap_, "\n     pc");
    for (size_t r = 0; r < std::size(c.pc); r++) {
        fprintf(cap_, " (%.3f %.3f %.3f %.3f)", c.pc[r][0], c.pc[r][1], c.pc[r][2], c.pc[r][3]);
    }
}

// the draw's line: its state, then its textures
void D3D12Backend::CaptureDraw(const State& s, uint32_t prim, uint32_t count, const PsoKey& k, const DrawConstants& c,
                               const DrawTextures& stages) {
    fprintf(cap_,
            "%04u draw rt %08X ds %08X prim %u count %u vs %08X ps %08X blend %u %X %X zen %u %X zwrite %u "
            "zbias %d %.2f cull %u cw %X c0 %.3f %.3f %.3f %.3f",
            cap_i_++, s.rt.data, bound_ds_ ? s.zs.data : 0, prim, count, s.vs_handle, s.ps ? s.ps->handle : 0, k.blend,
            k.src, k.dst, k.zen, k.zfunc, k.zwrite, k.zbias, k.zslope, k.cull, k.wmask, c.pc[0][0], c.pc[0][1],
            c.pc[0][2], c.pc[0][3]);
    CaptureTextures(s, stages);
}

// small draws: their inputs and constants (every draw's with XBE_CAPTURE_ALL); then ends the draw's line
void D3D12Backend::EndDrawCapture(const State& s, const Assembled& a, uint32_t count, const DrawConstants& c) {
    static const bool all = std::getenv("XBE_CAPTURE_ALL") != nullptr;
    if (count <= kQuadVertices || all) CaptureInputs(s, a, c);
    fputc('\n', cap_);
}

// each shader once, as translated
void D3D12Backend::CaptureShader(const char* kind, uint64_t key, const std::string& src) {
    if (!cap_sh_.insert(key).second) return;
    char name[48];
    snprintf(name, sizeof(name), "%s_%016llX.hlsl", kind, (unsigned long long)key);
    if (FILE* f = _wfopen((cap_dir_ / name).c_str(), L"w")) {
        fwrite(src.data(), 1, src.size(), f);
        fclose(f);
    }
    fprintf(cap_, "     %s\n", name);
}

void D3D12Backend::CaptureShaders(const State& s, const Assembled& a, const DrawShaders& sh, const PixelKey& pk) {
    std::string src = ReversedDepthHlsl(VertexHlsl(s, a, sh));
    CaptureShader("vs", sh.vkey, sh.quad ? QuadHlsl(std::move(src)) : src);
    CaptureShader("ps", sh.pkey, PixelHlsl(pk));
}

D3D12_SRV_DIMENSION NullViewDimension(uint8_t kind) {
    if (kind == kTexCube) return D3D12_SRV_DIMENSION_TEXTURECUBE;
    if (kind == kTex3D) return D3D12_SRV_DIMENSION_TEXTURE3D;
    return D3D12_SRV_DIMENSION_TEXTURE2D;
}

// A small draw whose every float2 input lies in 0..1: sprites, HUD pieces, text, full screen passes. The console's
// pixel centres sample such a quad from its first texel to half a texel short of its far edge; upscaled, the far
// edge's last half texel is drawn too, and wrapped it blends in the opposite edge (thin dark lines on HUD sprites).
// Clamped, it takes the edge texel instead, which a wrapped texture used once over 0..1 has there anyway.
bool UnitTexcoords(const Assembled& a) {
    constexpr float kLo = -1e-4f;
    constexpr float kHi = 1.0001f;
    constexpr uint32_t kSmallDrawVertices = 4096;  // most vertices a small draw has
    if (!a.vertices.empty() || !a.vertex_count || a.vertex_count > kSmallDrawVertices) return false;
    bool any = false;
    for (int r = 0; r < 16; r++) {
        const VertexElement& e = a.elements[size_t(r)];
        if (!(a.regs >> r & 1) || e.format != DXGI_FORMAT_R32G32_FLOAT) continue;
        const VertexBuffer& vb = a.buffers[e.buffer];
        if (!vb.src || !vb.stride || uint64_t(a.vertex_count) * vb.stride > vb.bytes) return false;
        for (uint32_t v = 0; v < a.vertex_count; v++) {
            float uv[2];
            std::memcpy(uv, vb.src + size_t(v) * vb.stride + e.offset, sizeof(uv));
            if (!(uv[0] >= kLo && uv[0] <= kHi && uv[1] >= kLo && uv[1] <= kHi)) return false;
        }
        any = true;
    }
    return any;
}

// UnitTexcoords tells of the coordinates the pixel shader gets only when the vertices' reach it as they are: not
// when a program scales or scrolls them (a sky's ring of painted strips, its clouds: clamped, their texels past 1
// smeared across the sky where the console wraps), nor when fixed function generates or transforms them
bool TexcoordsAsGiven(const State& s, const Assembled& a) {
    if ((s.vs_handle & 1) && s.vs && !s.vs->program.empty()) {
        static std::unordered_map<uint64_t, bool> copies;  // by program hash, on the render thread
        EnsureProgramHash(s);
        auto it = copies.find(s.vs->hash);
        if (it == copies.end()) it = copies.emplace(s.vs->hash, CopiesTexcoords(s.vs->program)).first;
        return it->second;
    }
    if (a.pretransformed) return true;
    for (const auto& t : s.tss)
        if (((t[kTssTexCoordIndex] >> 16) & 7) || (t[kTssTextureTransformFlags] & 7)) return false;
    return true;
}

// UnitTexcoords only changes stages that wrap
bool AnyStageWraps(const State& s) {
    for (const auto& t : s.tss) {
        if (t[kTssAddressU] == kTaddressWrap || t[kTssAddressV] == kTaddressWrap) return true;
    }
    return false;
}

// the first slot of the stages' four views, made only when this frame has not made the same set yet
uint32_t D3D12Backend::EnsureViewSet(const DrawTextures& stages) {
    if (view_sets_frame_ != frame_) {
        view_sets_.clear();
        view_sets_frame_ = frame_;
    }
    uint64_t key = Hash(stages.sv, sizeof(stages.sv), Hash(stages.res, sizeof(stages.res)));
    auto it = view_sets_.find(key);
    if (it != view_sets_.end()) return it->second;
    uint32_t slot = AllocSrvSlots(kMaterialViews);
    for (uint32_t i = 0; i < kMaterialViews; i++) {
        dev_->CreateShaderResourceView(stages.res[i], &stages.sv[i], Cpu(srv_heap_.Get(), slot + i, srv_inc_));
    }
    view_sets_[key] = slot;
    return slot;
}

// a stage without a texture gets a null view of its kind; the samplers read the views after
void D3D12Backend::SetTextureViewsAndSamplers(const State& s, const Assembled& a, DrawTextures& stages) {
    for (int i = 0; i < 4; i++) {
        if (stages.res[i]) continue;
        stages.sv[i].ViewDimension = NullViewDimension(stages.info[i].kind);
        stages.sv[i].Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    }
    // no material: null maps
    for (int i = 4; i < 6; i++) {
        if (stages.res[i]) continue;
        stages.sv[i] = {};
        stages.sv[i].ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        stages.sv[i].Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        stages.sv[i].Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        stages.sv[i].Texture2D.MipLevels = 1;
    }
    uint32_t views = EnsureViewSet(stages);
    if (views != binds_.views) cl_->SetGraphicsRootDescriptorTable(2, Gpu(srv_heap_.Get(), views, srv_inc_));
    binds_.views = views;
    bool clamp = AnyStageWraps(s) && ((ext_ && ext_->Clamp()) || (UnitTexcoords(a) && TexcoordsAsGiven(s, a)));
    uint32_t samplers = EnsureStageSamplers(s, stages, clamp);
    if (samplers != binds_.samplers) cl_->SetGraphicsRootDescriptorTable(3, Gpu(smp_heap_.Get(), samplers, smp_inc_));
    binds_.samplers = samplers;
}

// viewport covers the target, the guest viewport is in the shader; scissor clips to it
void D3D12Backend::SetViewportScissor(const State& s, GuestExtent size) {
    float W = float(bound_rt_ ? bound_rt_->w : bound_ds_->w);
    float H = float(bound_rt_ ? bound_rt_->h : bound_ds_->h);
    // upscaling: the frame's 3D in the scene's part, at the top left
    if (InScene()) {
        W = float(scene_w_);
        H = float(scene_h_);
    }
    D3D12_VIEWPORT vp{0, 0, W, H, 0, 1};
    // by axis: a multisampled grid may be narrower than the target and not shorter
    float sx = W / size.w;
    float sy = H / size.h;
    LONG x0 = LONG(s.viewport.x * sx);
    LONG y0 = LONG(s.viewport.y * sy);
    LONG x1 = LONG((s.viewport.x + s.viewport.w) * sx);
    LONG y1 = LONG((s.viewport.y + s.viewport.h) * sy);
    if (s.scissor_count) {
        x0 = std::max(x0, LONG(s.scissors[0] * sx));
        y0 = std::max(y0, LONG(s.scissors[1] * sy));
        x1 = std::min(x1, LONG(s.scissors[2] * sx));
        y1 = std::min(y1, LONG(s.scissors[3] * sy));
    }
    D3D12_RECT rect{std::clamp<LONG>(x0, 0, LONG(W)), std::clamp<LONG>(y0, 0, LONG(H)),
                    std::clamp<LONG>(x1, 0, LONG(W)), std::clamp<LONG>(y1, 0, LONG(H))};
    bool same = binds_.rasterizer && !std::memcmp(&vp, &binds_.viewport, sizeof(vp)) &&
                !std::memcmp(&rect, &binds_.scissor, sizeof(rect));
    if (same) return;
    cl_->RSSetViewports(1, &vp);
    cl_->RSSetScissorRects(1, &rect);
    binds_.rasterizer = true;
    binds_.viewport = vp;
    binds_.scissor = rect;
}

void D3D12Backend::SetBlendFactorStencilRef(const State& s) {
    float bf[4];
    Color(SimpleState(s, kMBlendColor), bf);
    uint32_t ref = SimpleState(s, kMStencilRef) & 0xFF;
    if (binds_.output && !std::memcmp(bf, binds_.blend, sizeof(bf)) && ref == binds_.stencil) return;
    cl_->OMSetBlendFactor(bf);
    cl_->OMSetStencilRef(ref);
    binds_.output = true;
    std::memcpy(binds_.blend, bf, sizeof(bf));
    binds_.stencil = ref;
}

D3D_PRIMITIVE_TOPOLOGY PrimitiveTopology(const Assembled& a) {
    if (a.topology == Topology::Points) return D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
    if (a.topology == Topology::Lines) {
        return a.strip ? D3D_PRIMITIVE_TOPOLOGY_LINESTRIP : D3D_PRIMITIVE_TOPOLOGY_LINELIST;
    }
    return a.strip ? D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP : D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
}

void D3D12Backend::UploadVertexBuffers(const Assembled& a) {
    D3D12_VERTEX_BUFFER_VIEW vbv[16];
    for (uint32_t i = 0; i < a.buffer_count; i++) {
        const VertexBuffer& b = a.buffers[i];
        Alloc vb = Upload(b.bytes, 16);
        std::memcpy(vb.cpu, b.src, b.bytes);
        vbv[i] = {vb.gpu, b.bytes, b.stride};
    }
    cl_->IASetVertexBuffers(0, a.buffer_count, vbv);
}

void D3D12Backend::DrawGeometry(const Assembled& a, const uint32_t* rebased) {
    UploadVertexBuffers(a);
    D3D_PRIMITIVE_TOPOLOGY topology = PrimitiveTopology(a);
    if (topology != binds_.topology) cl_->IASetPrimitiveTopology(topology);
    binds_.topology = topology;
    if (a.index_count) {
        bool i16 = a.index16 && !rebased;
        uint32_t ibytes = a.index_count * (i16 ? 2 : 4);
        Alloc ib = Upload(ibytes, 4);
        std::memcpy(ib.cpu, rebased ? static_cast<const void*>(rebased) : a.index_src, ibytes);
        D3D12_INDEX_BUFFER_VIEW ibv{ib.gpu, ibytes, i16 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT};
        cl_->IASetIndexBuffer(&ibv);
        cl_->DrawIndexedInstanced(a.index_count, 1, 0, rebased ? 0 : a.base_vertex, 0);
        return;
    }
    cl_->DrawInstanced(a.draw_count, 1, 0, 0);
}

// calls the extension's EndDraw on every return from Draw once made
struct DrawEnd {
    Extension* e;
    ~DrawEnd() {
        if (e) e->EndDraw();
    }
};

// Assemble, timed and counted
bool D3D12Backend::AssembleDraw(const State& s, uint32_t prim, uint32_t start, uint32_t count,
                                const uint8_t* indices, Assembled& a) {
    bool assembled;
    {
        PerfScope perf_assemble{g_perf.assemble};
        assembled = Assemble(s, prim, start, count, indices, a);
    }
    g_perf.verts += a.vertex_count;
    g_perf.raw += a.vertices.empty();
    return assembled;
}

bool D3D12Backend::EnsureQueries() {
    if (query_counts_) return true;
    D3D12_QUERY_HEAP_DESC qd{D3D12_QUERY_HEAP_TYPE_OCCLUSION, kQueries, 0};
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC bd = BufferDesc(uint64_t(kQueries) * sizeof(uint64_t));
    void* p = nullptr;
    bool made = SUCCEEDED(dev_->CreateQueryHeap(&qd, IID_PPV_ARGS(&query_heap_))) &&
                SUCCEEDED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST,
                                                        nullptr, IID_PPV_ARGS(&query_rb_))) &&
                SUCCEEDED(query_rb_->Map(0, nullptr, &p));
    if (!made) {
        LOG_FIRST(1, "d3d12: no occlusion queries, visibility tests stay pending");
        return false;
    }
    query_counts_ = static_cast<const uint64_t*>(p);
    return true;
}

// a draw while the title counts visible pixels gets an occlusion query of its own, kNoQuery otherwise
uint32_t D3D12Backend::BeginCountedDraw(const State& s) {
    if (!s.simple[kMCountEnable >> 2] || !EnsureQueries()) return kNoQuery;
    uint32_t query = query_next_++ % kQueries;
    cl_->BeginQuery(query_heap_.Get(), D3D12_QUERY_TYPE_OCCLUSION, query);
    return query;
}

void D3D12Backend::EndCountedDraw(uint32_t query) {
    if (query == kNoQuery) return;
    cl_->EndQuery(query_heap_.Get(), D3D12_QUERY_TYPE_OCCLUSION, query);
    uint64_t at = uint64_t(query) * sizeof(uint64_t);
    cl_->ResolveQueryData(query_heap_.Get(), D3D12_QUERY_TYPE_OCCLUSION, query, 1, query_rb_.Get(), at);
    counted_.push_back({query, SamplesPerPixel()});
}

// the console counts its own pixels: the bound target's scale and multisampling come out
float D3D12Backend::SamplesPerPixel() const {
    const Target* t = bound_rt_ ? bound_rt_ : bound_ds_;
    if (!t || !t->gw || !t->gh) return 1;
    float area = float(t->w) * float(t->h) / (float(t->gw) * float(t->gh));
    return area * float(t->res->GetDesc().SampleDesc.Count);
}

// reports whose draws the GPU has finished, written as the console's GPU writes them: time, count, then 0 for done
void D3D12Backend::DeliverReports() {
    constexpr uint32_t kTime = 0;
    constexpr uint32_t kTimeHigh = 4;
    constexpr uint32_t kCount = 8;
    constexpr uint32_t kStatus = 12;
    uint64_t done = fence_->GetCompletedValue();
    while (!reports_.empty() && reports_.front().fence <= done) {
        const Report& r = reports_.front();
        double pixels = 0;
        for (const Query& q : r.queries) pixels += double(query_counts_[q.index]) / q.samples_per_pixel;
        kernel::W32(r.va + kTime, uint32_t(presents_));
        kernel::W32(r.va + kTimeHigh, 0);
        kernel::W32(r.va + kCount, uint32_t(std::lround(pixels)));
        std::atomic_thread_fence(std::memory_order_release);
        kernel::W32(r.va + kStatus, 0);
        reports_.pop_front();
    }
}

constexpr uint32_t kPointList = 1;   // D3DPT_POINTLIST
constexpr uint32_t kTopDisable = 1;  // the XDK's D3DTOP_DISABLE
constexpr int kSpriteStage = 3;

// a program's point sprites under the fixed function stages: the XDK builds their combiners from stage 3 alone,
// the stage with the sprite's coordinates; a pixel shader reads t3 itself. SpriteStageZero disables stage 3.
bool SpriteOnStage3(const State& s, uint32_t prim) {
    bool prog = (s.vs_handle & 1) && s.vs && !s.vs->program.empty();
    bool stage3 = s.tss[kSpriteStage][kTssColorOp] != kTopDisable;
    return prim == kPointList && prog && !s.ps && s.rs[kRsPointSpriteEnable] && stage3;
}

// stage 3 drawn as stage 0, the only stage
State SpriteStageZero(const State& s) {
    State moved = s;
    moved.tss[0] = s.tss[kSpriteStage];
    moved.textures[0] = s.textures[kSpriteStage];
    moved.palettes[0] = s.palettes[kSpriteStage];
    for (int i = 1; i < 4; i++) {
        moved.tss[i][kTssColorOp] = kTopDisable;
        moved.tss[i][kTssAlphaOp] = kTopDisable;
        moved.textures[i] = 0;
    }
    return moved;
}

void D3D12Backend::Draw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) {
    if (SpriteOnStage3(s, prim)) {
        Draw(SpriteStageZero(s), prim, start, count, indices);
        return;
    }
    PerfScope perf_draw{g_perf.draw};
    DrawLap lap;
    g_perf.draws++;
    if (cap_) CapStep(s.rt);
    if (DepthCopy(s)) return;
    DrawEnd draw_end{ext_};
    if (ext_) ext_->BeginDraw(s);
    Assembled& a = asm_;
    if (!AssembleDraw(s, prim, start, count, indices, a)) return;
    if (ext_) ext_->Vertices(s, a);
    // past the ring every upload is a buffer of its own: seconds lost on a frame that draws a whole level once
    if (CurrentFrame().used > kUploadSize - kUploadReserve) Submit(true);
    if (!BindTargets(s)) return;
    if (ext_ && ext_->Bound(*this, s, bound_rt_, bound_ds_, a.pretransformed) && !BindTargets(s)) return;
    lap(0);

    // vertex shader
    DrawShaders sh = ClassifyShaders(s, a, count);
    if (sh.prog) EnsureProgramHash(s);
    sh.vkey = VertexKey(s, a, sh);
    EnsureVertexShader(s, a, sh);

    // stage textures, then the pixel shader made for them
    DrawTextures stages;
    if (!BindStageTexturesAndTargets(s, stages)) return;
    PixelKey pk = MakePixelKey(s, stages.info);
    pk.quad = sh.quad;
    // frame generation's motion of its own: a program's 3D draw into the frame that writes depth
    bool motion = bound_mv_ && bound_ds_ && sh.prog && !a.pretransformed && a.topology == Topology::Triangles &&
                  s.rs[kRsZEnable] && SimpleState(s, kMZWriteEnable);
    pk.motion = motion;
    if (motion) draw_mv_key_ = MotionKey(s, a, sh.vkey);
    // anti-aliasing's reactive mask: a blended effect drawn into the frame before it is anti-aliased that writes no depth
    // (a spark, fire, a glow, as the extension says), its share of the pixel by how it blends
    constexpr uint32_t kBlendOne = 1, kBlendSrcAlpha = 0x302;  // D3DBLEND_ONE, D3DBLEND_SRCALPHA (the XDK's values)
    if (bound_rx_ && aa_done_frame_ != presents_ && SimpleState(s, kMBlendEnable) &&
        !(s.rs[kRsZEnable] && SimpleState(s, kMZWriteEnable)) && ext_ && ext_->Reactive()) {
        uint32_t src = SimpleState(s, kMSrcBlend), dst = SimpleState(s, kMDestBlend);
        pk.reactive = dst == kBlendOne ? (src == kBlendSrcAlpha ? 3 : 2) : 1;
        rx_draws_++;
    }
    // anti-aliasing's offset: the frame's 3D, until it is anti-aliased (Extension::Jitter); in pixels of the size
    // drawn, the scene's part when upscaling
    bool world = bound_rt_ && s.rt_back_buffer && ext_ && ext_->Jitter();
    draw_jitter_ = aa_on_ && aa_done_frame_ != presents_ && world;
    if (draw_jitter_ && aa_jitter_frame_ != presents_) {
        bool in = InScene();
        aa_jitter_w_ = in ? scene_w_ : bound_rt_->w;
        aa_jitter_h_ = in ? scene_h_ : bound_rt_->h;
        aa_->Jitter(presents_, float(bound_rt_->w) / float(aa_jitter_w_), aa_jitter_[0], aa_jitter_[1]);
        aa_jitter_frame_ = presents_;
    }
    if (InScene()) {
        scene_draws_++;
    } else if (world && scene_closed_frame_ == presents_) {
        scene_late_++;  // the frame's 3D after its scene closed, drawn at the whole size
    }
    // a 2D quad's stage 0 read only inside its coordinates (Extension::TexelBox)
    draw_texbox_ = ext_ && TexcoordsAsGiven(s, a) && ext_->TexelBox(draw_uvbox_);
    pk.texbox = draw_texbox_ ? 1 : 0;
    // a material where the stage's fetch can take one, else its maps stay unbound
    if (stages.mat_stage >= 0 && MaterialFits(pk, stages.mat_stage)) {
        pk.material = uint32_t(stages.mat_stage) + 1;
    } else if (stages.mat_stage >= 0) {
        stages.mat_stage = -1;
        stages.res[4] = stages.res[5] = nullptr;
    }
    sh.pkey = Hash(&pk, sizeof(pk));
    EnsurePixelShader(pk, sh);
    if (!sh.vs_code || !sh.ps_code) return;
    lap(1);

    // the frame's UI starts: its 3D anti-aliased first (and upscaled, its scene closed), so the HUD-less copy is the
    // finished frame. The passes change the heaps, pipeline and targets: the draw's targets are bound again, and the
    // rest is set after
    PsoKey k;
    BuildPipelineKey(s, a, sh.vh, sh.ph, k);
    if ((hdr_ || fg_ || aa_on_ || ao_alone_) && ext_ && bound_rt_ && ext_->UiStart(k.blend && k.dst == 1)) {
        if (aa_on_ && FinishFrame3d(*bound_rt_)) {
            if (!BindTargets(s)) return;
            BuildPipelineKey(s, a, sh.vh, sh.ph, k);  // its polygon offset at the whole size
        } else if (ao_alone_ && OcclusionAlone(*bound_rt_)) {
            Begin();
            if (!BindTargets(s)) return;
        }
        if ((hdr_ || fg_) && bound_rt_) SnapshotUi(*bound_rt_);
        // the passes set their own pipelines, heaps and bindings (AMD's and NVIDIA's on Windows a compute pipeline
        // of theirs, on this list): all set again for the draw
        binds_ = {};
    }

    // pipeline
    k.sprite = sh.sprite;
    k.mv = bound_mv_ ? (motion ? 2 : 1) : 0;
    k.rx = bound_mv_ && bound_rx_ ? (pk.reactive ? 2 : 1) : 0;
    ID3D12PipelineState* pso = EnsurePipeline(k, sh.vs_code, sh.ps_code);
    if (!pso) return;
    // what the extension recorded on the list set what it liked: all set again after it, and only then. Each draw
    // binding all again cost vkd3d (Linux) a pipeline bind and the rest each time, some 1,480 a frame in a save room
    if (ext_list_) {
        binds_ = {};
        ext_list_ = false;
    }
    if (pso != binds_.pso) {
        cl_->SetPipelineState(pso);
        g_perf.pso_sets++;
    }
    binds_.pso = pso;
    lap(2);

    // constants
    GuestExtent size;
    SurfaceSize(bound_rt_ ? s.rt : s.zs, size.w, size.h);
    size = MultisampleGrid(s, bound_rt_ ? s.rt : s.zs, size);
    DrawConstants c{};
    UploadConstants(s, sh, k, stages, size, c);
    if (ext_list_) {
        binds_ = {};
        ext_list_ = false;
    }
    lap(3);

    if (cap_) {
        CaptureDraw(s, prim, count, k, c, stages);
        EndDrawCapture(s, a, count, c);
        CaptureShaders(s, a, sh, pk);
    }

    // texture views and samplers, viewport and scissor, blend factor and stencil reference
    SetTextureViewsAndSamplers(s, a, stages);
    SetViewportScissor(s, size);
    SetBlendFactorStencilRef(s);
    lap(4);

    // ray tracing's scene: the frame's 3D as drawn that writes depth (what the depth buffer holds: blended or not, as
    // this title draws its solid world blended), not what adds light (a glow), until it is anti-aliased
    // An indexed draw's indices are taken with its base vertex added (the title's indices count from the start of
    // its vertex buffer, the copy drawn starts at its first vertex), and drawn so, with none: SV_VertexID is then where
    // the vertex is in the buffers drawn, the same on every driver, and the slot it is captured to.
    uint32_t rt_first = UINT32_MAX, rt_slots = 0;
    const uint32_t* rt_rebased = nullptr;
    // water (Extension::Water: a pool, a fountain, which this title draws after its frame is anti-aliased, refracting
    // it): captured late, into a buffer of its own (Scene::LateCapture), for the next frame's scene to reflect the room
    // in (rt_water.hlsl), never into this frame's (its rays would meet the water's face)
    // Water drawn with the frame's 3D (before it is anti-aliased) is a surface of the scene, its reflections the
    // reflections pass's (rt_refl.hlsl: smooth, rippled, water's Fresnel)
    bool is_water = rt_frame_ && world && ext_ && ext_->Water();
    bool rt_water = is_water && aa_done_frame_ == presents_ && s.rs[kRsZEnable] && !a.pretransformed &&
                    a.topology == Topology::Triangles && (g_ray_tracing.load(std::memory_order_relaxed) & kRtReflections);
    // a glow, a flame, a spell adding its light (XBE_RT_LIGHTS=0: left out): gathered as a light the bounce lighting
    // samples (rt_gi.hlsl), which rays pass through
    static const bool kRtLights = !(std::getenv("XBE_RT_LIGHTS") && std::atoi(std::getenv("XBE_RT_LIGHTS")) == 0);
    bool rt_glow = SimpleState(s, kMBlendEnable) && SimpleState(s, kMDestBlend) == 1;
    // XBE_RT_LIGHTS_LOG: each blended picture drawn into the frame, the first time, and what keeps it from being a
    // light (a flame drawn after the frame's 3D was anti-aliased is not in the scene)
    static const bool kGlowLog = std::getenv("XBE_RT_LIGHTS_LOG") != nullptr;
    if (kGlowLog && SimpleState(s, kMBlendEnable) && bound_rt_ && s.rt_back_buffer && stages.res[0] &&
        glow_logged_.size() < 2000 && glow_logged_.insert(stages.res[0]).second)
        LOGR("ray tracing: blended %p frame %llu: %s%s, blend %03x %03x, %s, %s, z %s%s, %u vertices%s",
             static_cast<const void*>(stages.res[0]), (unsigned long long)presents_, world ? "3D" : "flat",
             a.pretransformed ? " pretransformed" : "", SimpleState(s, kMSrcBlend), SimpleState(s, kMDestBlend),
             s.ps ? "a pixel program" : "fixed function",
             aa_done_frame_ == presents_ ? "after the 3D was anti-aliased" : "with the 3D",
             s.rs[kRsZEnable] ? "tested" : "not tested", SimpleState(s, kMZWriteEnable) ? ", written" : "",
             a.index_count ? a.index_count : a.draw_count, rt_frame_ ? "" : ", no scene this frame");
    // a glow's steadiness by its picture (how many frames in a row it is drawn as a glow: XBE_RT_LIGHTS_LOG's, and the
    // lights' with XBE_RT_LIGHTS_STEADY_CPU=1, as before: a sword's swing, a hit's sparks, a spell's burst are there
    // for a moment, and the light they threw was grain on the walls that no frame had time to settle). As a rule the
    // glows known frame to frame tell a steady light by where it is (GlowMemory, rt_glowmem.hlsl): a flame whose
    // picture changes frame to frame is one flame
    bool steady_glow = true;
    // listed as a light (sampled for the light it throws, rt_gi.hlsl) only the first XBE_RT_LIGHTS_PER_PICTURE draws of
    // a picture a frame (16, 0: all): the save room's water glows in some 500 draws of one picture, which filled the
    // list of 128 before the candles got in, and which of them did changed as the camera turned. The rest stay in the
    // scene, their light gathered by the bounce lighting's rays that pass through them
    bool glow_listed = true;
    // A glow steady by the title's own texture (the data it draws from), not the renderer's copy of it: a torch's or a
    // candle's flame is animated by its palette, each of whose colours is a copy of its own, and by its copy no flame
    // was ever drawn two frames in a row (none was steady, none a light). Counted by its copy, as it was
    const uint64_t glow_key = stages.data[0] ? uint64_t(stages.data[0])
                                             : (uint64_t(uintptr_t(stages.res[0])) | (uint64_t(1) << 63));
    if (rt_glow && rt_frame_ && kRtLights) {
        static const uint32_t kPerPicture = std::getenv("XBE_RT_LIGHTS_PER_PICTURE")
                                                ? uint32_t(std::atoi(std::getenv("XBE_RT_LIGHTS_PER_PICTURE")))
                                                : 16u;
        GlowSeen& c = glow_seen_[stages.res[0] ? static_cast<const void*>(stages.res[0]) : nullptr];
        c.frame = presents_;
        if (c.count_frame != presents_) {
            c.count_frame = presents_;
            c.count = 0;
        }
        glow_listed = kPerPicture == 0 || ++c.count <= kPerPicture;
        const uint32_t kSteady = SteadyFrames();
        if (kSteady > 0) {
            GlowSeen& g = glow_steady_[glow_key];
            // (a frame skipped keeps the streak: an animation's sprite may be drawn every other frame)
            if (g.frame != presents_) {
                g.streak = g.frame + 2 >= presents_ ? g.streak + 1 : 1;
                g.frame = presents_;
            }
            steady_glow = g.streak >= kSteady;
            // XBE_RT_LIGHTS_LOG: each glow's picture as it is first drawn and as it turns steady (a light)
            static const bool kLightsLog = std::getenv("XBE_RT_LIGHTS_LOG") != nullptr;
            if (kLightsLog && ((g.streak == 1 && g.said < 3) || (g.streak == kSteady && g.said < 6))) {
                LOGR("ray tracing: a glow, texture %08llx (picture %p), %s, %u vertices, %s", (unsigned long long)glow_key,
                     static_cast<const void*>(stages.res[0]),
                     world ? "3D" : ext_ && ext_->ScreenLight() ? "flat, a light" : "flat, not a light",
                     a.index_count ? a.index_count : a.draw_count, g.streak == kSteady ? "steady" : "first drawn");
                g.said++;
            }
            if (glow_steady_.size() > 4096)  // textures long gone
                for (auto it = glow_steady_.begin(); it != glow_steady_.end();)
                    it = it->second.frame + 600 < presents_ ? glow_steady_.erase(it) : std::next(it);
        }
        if (glow_seen_.size() > 4096) {  // pictures long gone
            for (auto it = glow_seen_.begin(); it != glow_seen_.end();)
                it = it->second.frame + 600 < presents_ ? glow_seen_.erase(it) : std::next(it);
        }
    }
    // a flame, a glow drawn flat over the world (Extension::ScreenLight), depth tested: a light at its depth
    // a glow drawn by a pixel shader program is no light: its light is not its picture's (the save room marker's
    // swirl, masked to a ring by a second picture, lit a square of floor bright blue the size of its quad)
    // (any glow, for a moment or for good: which is which, rt_glowmem.hlsl tells by where it is, frame to frame, and a
    // light's light grows in as it proves steady; XBE_RT_LIGHTS_STEADY_CPU=1: as before, only a picture drawn as a
    // glow SteadyFrames frames in a row)
    static const bool kSteadyCpu = std::getenv("XBE_RT_LIGHTS_STEADY_CPU") && std::atoi(std::getenv("XBE_RT_LIGHTS_STEADY_CPU")) != 0;
    // a character's own glow (Extension::Character: a pass over Hector's or an enemy's skinned mesh adding its light,
    // the embroidery on Hector's coat, a golem's cracks) is its look, not a lamp: as a light it lit the floor round
    // Hector in its colour and moved with every swing of his sword (XBE_RT_LIGHTS_CHAR=1: lights, as before)
    static const bool kCharLights = std::getenv("XBE_RT_LIGHTS_CHAR") && std::atoi(std::getenv("XBE_RT_LIGHTS_CHAR")) != 0;
    const bool glow_char = rt_glow && !kCharLights && ext_ && ext_->Character();
    const bool glow_light = kRtLights && (steady_glow || !kSteadyCpu) && !s.ps && !glow_char;
    bool rt_flat = rt_frame_ && !world && rt_glow && glow_light && aa_done_frame_ != presents_ &&
                   !a.pretransformed && a.topology == Topology::Triangles && s.rs[kRsZEnable] && ext_ &&
                   ext_->ScreenLight();
    // a character drawn writing no depth (blended over what is there: the game fades it, it blinks as it is hurt in a
    // fight), by a mesh the frame's scene has not taken yet: taken all the same, its shadow and shade kept. Its shadow
    // and its occlusion cut out for those frames, and came back. One whose mesh the scene holds already this frame is
    // a copy of it (a dash's after-image, a hit's flash drawn over it): left out, or it cast a second shadow where the
    // character was a moment before. XBE_RT_CHAR_FADED=0: none taken, as before
    static const bool kCharFaded =
        !(std::getenv("XBE_RT_CHAR_FADED") && std::atoi(std::getenv("XBE_RT_CHAR_FADED")) == 0);
    const bool is_char = rt_frame_ && world && ext_ && ext_->Character() && !rt_glow;
    const uint64_t char_mesh = is_char ? CharacterMesh(a) : 0;
    bool char_faded = false;
    if (is_char && kCharFaded && !SimpleState(s, kMZWriteEnable) && s.rs[kRsZEnable]) {
        char_faded = char_meshes_.find(char_mesh) == char_meshes_.end();
        if (!char_faded) char_copies_++;
    }
    if (rt_water || rt_flat ||
        (rt_frame_ && world && aa_done_frame_ != presents_ && !a.pretransformed && a.topology == Topology::Triangles &&
         s.rs[kRsZEnable] && (rt_glow ? glow_light : SimpleState(s, kMZWriteEnable) != 0 || is_water || char_faded))) {
        bool fits = true;
        if (a.index_count) {
            rt_ix_.resize(a.index_count);
            int64_t top = -1, low = INT64_MAX, base = a.base_vertex;
            for (uint32_t k = 0; k < a.index_count; k++) {
                int64_t v = (a.index16 ? int64_t(static_cast<const uint16_t*>(a.index_src)[k])
                                       : int64_t(static_cast<const uint32_t*>(a.index_src)[k])) + base;
                rt_ix_[k] = uint32_t(v);
                top = std::max(top, v);
                low = std::min(low, v);
            }
            fits = low >= 0;
            rt_slots = uint32_t(top + 1);
            rt_rebased = rt_ix_.data();
        } else {
            rt_slots = a.draw_count;
        }
        rt_first = !fits || !rt_slots ? UINT32_MAX : rt_water ? rt_.ReserveLate(rt_slots) : rt_.Reserve(rt_slots);
        if (rt_water) {
            if (rt_first != UINT32_MAX) cl_->SetGraphicsRootUnorderedAccessView(4, rt_.LateCapture());
            else if (!late_said_) {
                LOGW("ray tracing: water drawn (%u vertices) found no room in its capture: not reflected", rt_slots);
                late_said_ = true;
            }
        } else if (rt_first == UINT32_MAX && !rt_said_) {
            LOGW("ray tracing: a frame's capture buffer is full; its next frames get a larger one");
            rt_said_ = true;
        }
    }
    // XBE_RT_CHAR_LOG: the characters' draws the frame's scene took and those it missed, and why (rays found no
    // character to shade or to cast a shadow that frame)
    if (is_char && rt_first != UINT32_MAX) {
        auto [it, fresh] = char_meshes_.try_emplace(char_mesh, char_faded);
        // XBE_RT_CHAR_LOG: a mesh taken fading and then drawn again whole, the copy (an after-image) before it
        if (!fresh && it->second && SimpleState(s, kMZWriteEnable) && char_said_ < 40 && std::getenv("XBE_RT_CHAR_LOG")) {
            char_said_++;
            LOGR("rt chars: frame %llu: a mesh taken writing no depth, then drawn writing it (%u indices)",
                 (unsigned long long)presents_, a.index_count ? a.index_count : a.draw_count);
        }
        if (char_faded) char_faded_++;
    }
    static const bool kCharLog = std::getenv("XBE_RT_CHAR_LOG") != nullptr;
    if (kCharLog && rt_frame_ && world && ext_ && ext_->Character()) {
        if (rt_first != UINT32_MAX) char_in_++;
        else if (aa_done_frame_ == presents_) char_late_++;
        else if (!s.rs[kRsZEnable] || !SimpleState(s, kMZWriteEnable)) char_nodepth_++;
        else char_other_++;
        // each kind of draw writing no depth, the first times: how it blends, whether it was taken
        if (!SimpleState(s, kMZWriteEnable) && char_said_ < 40) {
            uint32_t kind = (SimpleState(s, kMBlendEnable) ? SimpleState(s, kMSrcBlend) << 12 | SimpleState(s, kMDestBlend)
                                                          : 0) ^ (s.rs[kRsZEnable] ? 1u << 30 : 0) ^ (rt_glow ? 1u << 29 : 0) ^
                            (rt_first != UINT32_MAX ? 1u << 28 : 0) ^ (char_faded ? 1u << 27 : 0);
            if (char_kinds_.insert(kind).second) {
                char_said_++;
                LOGR("rt chars: frame %llu: a character drawn writing no depth, blend %s %03x %03x, z %s, alpha test %u "
                     "ref %u, %u indices: %s",
                     (unsigned long long)presents_, SimpleState(s, kMBlendEnable) ? "on" : "off",
                     SimpleState(s, kMSrcBlend), SimpleState(s, kMDestBlend), s.rs[kRsZEnable] ? "tested" : "not tested",
                     SimpleState(s, kMAlphaTestEnable), SimpleState(s, kMAlphaRef) & 0xFF,
                     a.index_count ? a.index_count : a.draw_count,
                     rt_glow ? "a glow, left out"
                     : rt_first != UINT32_MAX ? "taken (its mesh not yet in the scene)"
                     : is_char && kCharFaded && s.rs[kRsZEnable] ? "a copy of a mesh in the scene, left out"
                                                                  : "left out");
            }
        }
    }
    uint32_t rtc[2] = {rt_first == UINT32_MAX ? 0u : rt_first, rt_first == UINT32_MAX ? 0u : 1u};
    if (rtc[0] != rt_bound_[0] || rtc[1] != rt_bound_[1]) {
        cl_->SetGraphicsRoot32BitConstants(5, 2, rtc, 0);
        rt_bound_[0] = rtc[0];
        rt_bound_[1] = rtc[1];
    }

    // geometry, counted while the title runs a visibility test
    uint32_t query = BeginCountedDraw(s);
    DrawGeometry(a, rt_first != UINT32_MAX ? rt_rebased : nullptr);
    EndCountedDraw(query);
    if (rt_water && rt_first != UINT32_MAX) {
        // its picture (stage 0's, whose alpha says where the water is), made again in the next frame's descriptors
        LatePicture lp;
        if (stages.res[0] && !stages.rt[0] && stages.sv[0].ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2D) {
            lp.res = stages.res[0];
            lp.sv = stages.sv[0];
        }
        if (rt_.AddLateDraw(rt_rebased, a.index_count, false, a.strip, a.draw_count, rt_first, rt_slots))
            late_pictures_[frame_ % kFrames].push_back(std::move(lp));
        BindRtCapture();  // the frame's own capture back, writing none
        if (!late_said_) {
            LOGR("ray tracing: water drawn (%u vertices), captured for the next frame's reflections", rt_slots);
            late_said_ = true;
        }
    } else if (rt_first != UINT32_MAX) {
        // How it looks (rt::Look), by views in this draw's set, which the frame's descriptor ring keeps until the
        // passes: its stage 0 picture (not a render target's: the passes may draw into that target, the frame's own
        // 3D an effect reads), a texture pack material over it (its normal and ORM maps, views 4 and 5) and, for a
        // cut-out (alpha tested, its alpha its picture's), its test, so rays see through it where it is see-through.
        // Alpha from elsewhere (a pixel shader program's, the vertices' alone) is taken as solid
        rt::Look look;
        bool pic = stages.res[0] && !stages.rt[0] && stages.sv[0].ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2D;
        if (pic) look.picture = binds_.views;
        // its picture's alpha (AlphaKind): solid (a wall: opaque to rays, tested or not; this title alpha tests and
        // blends its every draw), with clear parts (a cut-out: alpha tested, as the test has it; blended with no test,
        // there where it is more than nearly clear, XBE_RT_CUTOUT_BLEND, the alpha it starts at in 255ths: 16, 0 for
        // none), or faint all over (glass, a haze, a sheet the game fades: blended, none at all to rays). Rays met the
        // whole quad of ivy and fallen leaves, and the stone behind and the floor under them shaded black in boxes;
        // the floor under a faint sheet, black
        static const uint32_t kCutoutBlend =
            std::getenv("XBE_RT_CUTOUT_BLEND") ? uint32_t(std::atoi(std::getenv("XBE_RT_CUTOUT_BLEND"))) : 16u;
        const bool blended = SimpleState(s, kMBlendEnable) && SimpleState(s, kMSrcBlend) == 0x302 &&
                             SimpleState(s, kMDestBlend) == 0x303;
        const uint32_t from = pic ? AlphaFromPicture(s) : 0;
        const uint8_t kind = pic ? stages.cutout[0] : 0;
        const uint32_t times = from == 2 ? 0x20000u : 0;
        if (from && kind == 2 && blended && kCutoutBlend && !rt_glow) {
            look.alpha = 0x10000u | times;  // never solid
        } else if (from && kind != 0) {
            if (SimpleState(s, kMAlphaTestEnable)) {
                uint32_t f = uint32_t(Cmp(SimpleState(s, kMAlphaFunc))) - 1;
                if (f != 7) look.alpha = 0x10000u | times | f << 8 | (SimpleState(s, kMAlphaRef) & 0xFF);
            } else if (blended && kCutoutBlend && !rt_glow) {
                look.alpha = 0x10000u | times | 4u << 8 | (std::min(kCutoutBlend, 255u) & 0xFF);
            }
        }
        // a blended cut-out covers its pixel as much as its alpha (0x40000: rt_ao.hlsl's cover)
        if (look.alpha && blended) look.alpha |= 0x40000u;
        // XBE_RT_CUTOUT_LOG: each picture drawn blended into the scene, once: how rays take it
        static const bool kCutoutLog = std::getenv("XBE_RT_CUTOUT_LOG") != nullptr;
        if (kCutoutLog && pic && !rt_glow && SimpleState(s, kMBlendEnable) && stages.res[0] &&
            cutout_said_.insert(stages.res[0]).second && cutout_said_.size() < 600)
            LOGR("ray tracing: blended picture %p: blend %x %x, alpha test %u func %x ref %u, alpha from the picture %u "
                 "(program %d; stage 0 alpha op %u args %x %x, stage 1 colour op %u alpha op %u args %x %x), its alpha "
                 "%u (replaced %d), rays %s, %u vertices",
                 static_cast<const void*>(stages.res[0]), SimpleState(s, kMSrcBlend), SimpleState(s, kMDestBlend),
                 SimpleState(s, kMAlphaTestEnable), SimpleState(s, kMAlphaFunc), SimpleState(s, kMAlphaRef) & 0xFF,
                 from, s.ps ? 1 : 0, s.tss[0][kTssAlphaOp], s.tss[0][kTssAlphaArg1], s.tss[0][kTssAlphaArg2],
                 s.tss[1][kTssColorOp], s.tss[1][kTssAlphaOp], s.tss[1][kTssAlphaArg1], s.tss[1][kTssAlphaArg2],
                 stages.cutout[0], stages.replaced[0] ? 1 : 0,
                 !look.alpha ? "solid" : ((look.alpha >> 8) & 7) == 0 ? "through" : "cut-out",
                 a.index_count ? a.index_count : a.draw_count);
        if (stages.mat_stage == 0 && stages.res[4] && stages.res[5]) {
            look.normal = binds_.views + 4;
            look.orm = binds_.views + 5;
            look.rough_bias = stages.mat[2][1];
            look.metal_scale = stages.mat[2][2];
            look.gloss_roughness = stages.gloss[0];
            look.gloss_metalness = stages.gloss[1];
        }
        look.character = !rt_glow && ext_ && ext_->Character();
        look.screen = rt_flat;
        look.listed = glow_listed;
        look.water = is_water && !rt_glow;
        if (rt_glow) {
            constexpr uint32_t kSrcAlpha = 0x302;  // D3DBLEND_SRCALPHA (the XDK's)
            look.emissive = SimpleState(s, kMSrcBlend) == kSrcAlpha ? 1 : 2;
            look.alpha = 0;  // its picture's alpha weighs its light, it is no cut-out
        }
        rt_.AddDraw(rt_rebased, a.index_count, false, a.strip, a.draw_count, rt_first, rt_slots, look);
    }
    lap(5);
}

// The graphics card's memory as the system counts it against the game's budget (DXGI's local segment), every 300
// frames: said when it goes over (and back), with the most it reached, and every 30 s with XBE_PERF. Over its budget,
// Windows moves the game's memory out to the system's and frames stutter; DLSS warns of it (Streamline's
// eWarnOutOfVRAM), which once sent the 3D to the output's whole size. The texture pack at full size and 4K output
// with ray tracing went past a 16 GB card's 13.7 GB
void D3D12Backend::MemoryCheck() {
    // XBE_MEM_TRACE=<frames>: checked that often, each check logged in a line
    static const uint64_t trace = std::getenv("XBE_MEM_TRACE") ? std::max(1, std::atoi(std::getenv("XBE_MEM_TRACE"))) : 0;
    if (presents_ < mem_frame_ + (trace ? trace : 300)) return;
    mem_frame_ = presents_;
#ifndef _WIN32
    // Linux: no DXGI to give the card's budget; the texture pack's pictures unused a while are still let go
    TrimReplacements(0, 0);
    return;
#else
    if (!mem_tried_) {
        mem_tried_ = true;
        ComPtr<IDXGIFactory4> f;
        ComPtr<IDXGIAdapter1> a;
        if (SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&f))) &&
            SUCCEEDED(f->EnumAdapterByLuid(dev_->GetAdapterLuid(), IID_PPV_ARGS(&a))))
            a.As(&mem_adapter_);
    }
    if (!mem_adapter_) return;
    DXGI_QUERY_VIDEO_MEMORY_INFO m{};
    if (FAILED(mem_adapter_->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &m)) || !m.Budget) return;
    const double gb = 1.0 / (1024.0 * 1024.0 * 1024.0);
    if (!mem_most_)
        LOGR("memory: the graphics card's budget %.2f GB, %.2f used", double(m.Budget) * gb, double(m.CurrentUsage) * gb);
    mem_most_ = std::max(mem_most_, m.CurrentUsage);
    if (trace) LOGR("memory trace: frame %llu, %.3f of %.2f GB", (unsigned long long)presents_,
                    double(m.CurrentUsage) / (1024.0 * 1024.0 * 1024.0), double(m.Budget) / (1024.0 * 1024.0 * 1024.0));
    uint64_t freed = TrimReplacements(m.CurrentUsage, m.Budget);
    if (freed) m.CurrentUsage = m.CurrentUsage > freed ? m.CurrentUsage - freed : 0;
    bool over = m.CurrentUsage > m.Budget;
    bool went_over = over && !mem_over_;
    if (over != mem_over_) {
        mem_over_ = over;
        LOGW("memory: the graphics card's %s its budget, %.2f of %.2f GB (the most yet %.2f)%s",
             over ? "is over" : "is back under", double(m.CurrentUsage) * gb, double(m.Budget) * gb,
             double(mem_most_) * gb,
             over ? "; frames may stutter: a smaller texture pack size, or a lower output resolution, leaves room" : "");
    }
    // every two minutes (every 30 s with XBE_PERF), and as it goes over: what the renderer holds, by kind
    if (went_over || presents_ >= mem_report_ + (diag::Perf() ? 600 : 7200)) {
        mem_report_ = presents_;
        auto bytes = [](ID3D12Resource* r) -> uint64_t {
            if (!r) return 0;
            D3D12_RESOURCE_DESC d = r->GetDesc();
            uint64_t b = d.Width * d.Height * std::max<uint32_t>(d.DepthOrArraySize, 1u);
            switch (d.Format) {
                case DXGI_FORMAT_R16G16B16A16_FLOAT: b *= 8; break;
                case DXGI_FORMAT_BC1_UNORM: case DXGI_FORMAT_BC1_UNORM_SRGB: case DXGI_FORMAT_BC4_UNORM: b /= 2; break;
                case DXGI_FORMAT_BC2_UNORM: case DXGI_FORMAT_BC3_UNORM: case DXGI_FORMAT_BC5_UNORM:
                case DXGI_FORMAT_BC7_UNORM: case DXGI_FORMAT_BC7_UNORM_SRGB: case DXGI_FORMAT_R8_UNORM: break;
                default: b *= 4; break;
            }
            return d.MipLevels > 1 ? b * 4 / 3 : b;
        };
        uint64_t own = 0, repl = 0, mats = 0, tgt = 0, fb = 0, pack = 0;
        size_t repls = 0;
        for (auto& [k, t] : textures_) {
            own += bytes(t.res.Get());
            if (t.repl) repls++;
            repl += bytes(t.repl.Get()) + bytes(t.spare.Get());
            mats += bytes(t.mat[0].Get()) + bytes(t.mat[1].Get());
        }
        for (auto& [k, t] : pack_textures_) pack += bytes(t.res.Get());
        size_t ntgt = 0;
        for (auto& [k, t] : targets_) {
            tgt += bytes(t.res.Get());
            fb += bytes(t.fb.Get());
            ntgt += t.res ? 1 : 0;
        }
        // the frame's own passes: ambient occlusion, bounce lighting and its denoiser, reflections, shafts, glows,
        // the guides, the anti-aliasing's and frame generation's, the UI's, the swap chain
        uint64_t fx = 0;
        for (ID3D12Resource* r : {ao_.Get(), gi_.Get(), aa_depth_.Get(), aa_motion_.Get(), aa_out_.Get(), aa_rx_.Get(),
                                  fg_depth_.Get(), fg_motion_.Get(), fg_objmv_.Get(), ui_.Get(), ui_snap_.Get(),
                                  scene_tmp_.Get(), level16_.Get(), level1_.Get()})
            fx += bytes(r);
        for (auto& r : shafts_) fx += bytes(r.Get());
        for (auto& r : rs_) fx += bytes(r.Get());
        for (auto& r : glow_) fx += bytes(r.Get());
        for (auto& r : guide_) fx += bytes(r.Get());
        for (auto& r : rd_) fx += bytes(r.Get());
        for (auto& r : mean_) fx += bytes(r.Get());
        for (auto& r : dn_) fx += bytes(r.Get());
        for (auto& r : fg_hudless_) fx += bytes(r.Get());
        for (auto& r : bb_) fx += bytes(r.Get());
        uint64_t scene = rt_.Bytes();
        uint64_t ours = own + repl + pack + mats + tgt + fb + fx + scene;
        LOGR("memory: the graphics card's %.2f of %.2f GB (the most yet %.2f); the game's textures %.2f GB, texture "
             "pack pictures %.2f GB (%zu) and their material maps %.2f GB, targets %.2f GB (%zu) and their copies for "
             "draws that read them %.2f GB, the frame's passes %.2f GB, ray tracing's scene %.2f GB; the rest "
             "(upscaler, frame generation, the driver's) %.2f GB; %llu pictures let go so far (%.2f GB)",
             double(m.CurrentUsage) * gb, double(m.Budget) * gb, double(mem_most_) * gb, double(own) * gb,
             double(repl + pack) * gb, repls, double(mats) * gb, double(tgt) * gb, ntgt, double(fb) * gb,
             double(fx) * gb, double(scene) * gb,
             double(m.CurrentUsage > ours ? m.CurrentUsage - ours : 0) * gb, (unsigned long long)trim_count_,
             double(trim_freed_) * gb);
    }
#endif
}

// A texture pack's picture is made into a texture of its own as its game texture is first drawn (MakeReplacement), and
// kept while that texture is: the game puts each room's textures in memory of their own, so the pictures of every room
// played stayed on the graphics card, a gigabyte and more a room at 4K with material maps, until it was full and
// Windows moved memory in and out of it each frame (half an hour of play: every room slow, the save room too). A
// picture not drawn from for XBE_TEX_IDLE seconds (60) is let go; while the card's memory in use is past
// XBE_TEX_PRESSURE of its budget (0.8), those unused for 5 s too, the longest unused first, down to 0.7 of it. One
// drawn again is loaded again (its game texture's bytes checked anew, the extension asked for it), drawn as the
// game's own until it is ready.
uint64_t D3D12Backend::TrimReplacements(uint64_t used, uint64_t budget) {
    auto env = [](const char* n, double d) { const char* e = std::getenv(n); return e ? std::atof(e) : d; };
    static const double idle_s = std::max(5.0, env("XBE_TEX_IDLE", 60.0));
    static const double pressure = std::clamp(env("XBE_TEX_PRESSURE", 0.8), 0.3, 1.0);
    const uint64_t idle = uint64_t(idle_s * 60), soon = 300;
    auto bytes = [](ID3D12Resource* r) -> uint64_t {
        if (!r) return 0;
        D3D12_RESOURCE_DESC d = r->GetDesc();
        uint64_t b = d.Width * d.Height;
        bool bc = d.Format >= DXGI_FORMAT_BC1_TYPELESS && d.Format <= DXGI_FORMAT_BC5_SNORM ||
                  d.Format >= DXGI_FORMAT_BC6H_TYPELESS && d.Format <= DXGI_FORMAT_BC7_UNORM_SRGB;
        b *= bc ? 1 : 4;
        return d.MipLevels > 1 ? b * 4 / 3 : b;
    };
    struct Old {
        uint64_t used;
        Tex* tx;
        uint64_t bytes;
    };
    std::vector<Old> old;
    for (auto& [k, tx] : textures_) {
        if (!tx.repl && !tx.spare && !tx.mat[0]) continue;
        uint64_t age = presents_ > tx.used ? presents_ - tx.used : 0;
        if (age < soon) continue;
        old.push_back({tx.used, &tx, bytes(tx.repl.Get()) + bytes(tx.spare.Get()) + bytes(tx.mat[0].Get()) +
                                         bytes(tx.mat[1].Get())});
    }
    std::sort(old.begin(), old.end(), [](const Old& a, const Old& b) { return a.used < b.used; });
    bool pressed = used > uint64_t(double(budget) * pressure);
    uint64_t want = pressed ? used - std::min(used, uint64_t(double(budget) * (pressure - 0.1))) : 0;
    uint64_t freed = 0, n = 0;
    for (const Old& o : old) {
        bool long_idle = presents_ - o.used >= idle;
        if (!long_idle && freed >= want) break;
        Tex& tx = *o.tx;
        auto drop = [&](ComPtr<ID3D12Resource>& r) {
            if (!r) return;
            CurrentFrame().garbage.push_back(r);
            r.Reset();
        };
        drop(tx.repl);
        drop(tx.spare);
        drop(tx.mat[0]);
        drop(tx.mat[1]);
        tx.repl_id = 0;
        tx.sig = 0;  // checked and its replacement asked for again when next drawn
        freed += o.bytes;
        n++;
    }
    if (n) {
        trim_freed_ += freed;
        trim_count_ += n;
        const double gb = 1.0 / (1024.0 * 1024.0 * 1024.0);
        LOGR_FIRST(20, "memory: %llu texture pack pictures unused a while let go, %.2f GB (%.2f of %.2f GB in use%s)",
                  (unsigned long long)n, double(freed) * gb, double(used) * gb, double(budget) * gb,
                  pressed ? ", near the budget" : "");
    }
    return freed;
}

// a guest rect scaled to the target and clamped to it
D3D12_RECT ScaledRect(const Target& t, int x0, int y0, int x1, int y1) {
    float sx = float(t.w) / t.gw, sy = float(t.h) / t.gh;
    return {std::clamp<LONG>(LONG(x0 * sx), 0, t.w), std::clamp<LONG>(LONG(y0 * sy), 0, t.h),
            std::clamp<LONG>(LONG(x1 * sx), 0, t.w), std::clamp<LONG>(LONG(y1 * sy), 0, t.h)};
}

// rs: the rects buffer, reused across targets
void D3D12Backend::ClearTarget(Target& t, const State& s, const ClearArgs& c, std::vector<D3D12_RECT>& rs) {
    rs.clear();
    if (c.count && c.rects) {
        for (uint32_t i = 0; i < c.count; i++) {
            int32_t r[4];
            std::memcpy(r, c.rects + size_t(i) * sizeof(r), sizeof(r));
            rs.push_back(ScaledRect(t, r[0], r[1], r[2], r[3]));
        }
    } else {
        const auto& v = s.viewport;
        rs.push_back(ScaledRect(t, int(v.x), int(v.y), int(v.x + v.w), int(v.y + v.h)));
    }
    // upscaling: a clear of part of the frame clears that part of the scene's; a whole one stays whole, so what is
    // outside the scene's part is cleared too for what is drawn whole after it
    if (c.scene) {
        for (D3D12_RECT& r : rs) {
            if (r.left <= 0 && r.top <= 0 && r.right >= LONG(t.w) && r.bottom >= LONG(t.h)) continue;
            r = {LONG(float(r.left) * scene_fx_), LONG(float(r.top) * scene_fy_), LONG(std::ceil(float(r.right) * scene_fx_)),
                 LONG(std::ceil(float(r.bottom) * scene_fy_))};
        }
    }
    if (t.depth) {
        Transition(t.res.Get(), t.state, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        D3D12_CLEAR_FLAGS cf = D3D12_CLEAR_FLAGS(((c.flags & kClearZ) ? D3D12_CLEAR_FLAG_DEPTH : 0) |
                                                 ((c.flags & kClearStencil) ? D3D12_CLEAR_FLAG_STENCIL : 0));
        float reversed = 1 - std::clamp(c.z, 0.0f, 1.0f);
        cl_->ClearDepthStencilView(Cpu(dsv_heap_.Get(), t.view, dsv_inc_), cf, reversed, UINT8(c.stencil),
                                   UINT(rs.size()), rs.data());
    } else {
        // pixels the CPU wrote before this clear go in first, a later upload would paint over the clear
        if (LatchCpuWrite(t)) UploadTarget(t, s.rt);
        Transition(t.res.Get(), t.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
        float col[4];
        Color(c.color, col);
        for (int i = 0; t.range != 1 && i < 3; i++) col[i] /= t.range;
        // D3DCLEAR_TARGET_R/G/B/A are bits 4 to 7, D3D12's write mask has the same order
        uint8_t mask = uint8_t((c.flags & kClearTarget) >> 4);
        if (mask == D3D12_COLOR_WRITE_ENABLE_ALL) {
            cl_->ClearRenderTargetView(Cpu(rtv_heap_.Get(), t.view, rtv_inc_), col, UINT(rs.size()), rs.data());
        } else {
            ClearChannels(t, col, mask, rs);
        }
    }
    t.drawn = true;
    t.cpu = false;
}

// ClearChannels' pipeline for the target format and channel mask, kept in blit_psos_; null when it can't be made
ID3D12PipelineState* D3D12Backend::EnsureClearPipeline(DXGI_FORMAT rtv, uint8_t mask) {
    uint64_t seed = uint64_t(rtv) << 8 | mask;
    auto& pso = blit_psos_[Hash(kClearHlsl, std::strlen(kClearHlsl), seed)];
    if (pso) return pso.Get();
    uint64_t h;
    ID3DBlob* vs = Compile(kClearHlsl, "vs", "vs_5_0", h);
    ID3DBlob* ps = Compile(kClearHlsl, "psmain", "ps_5_0", h);
    if (!vs || !ps) return nullptr;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d = FullScreenPipelineDesc(vs, ps, rtv);
    d.BlendState.RenderTarget[0].RenderTargetWriteMask = mask;
    if (FAILED(CreateGraphicsPipelineCached(dev_.Get(), d, IID_PPV_ARGS(&pso)))) return nullptr;
    return pso.Get();
}

// a clear of some colour channels: drawn over each rect writing only those
void D3D12Backend::ClearChannels(Target& t, const float col[4], uint8_t mask, const std::vector<D3D12_RECT>& rs) {
    ID3D12PipelineState* pso = EnsureClearPipeline(t.fmt, mask);
    if (!pso) {
        LOG_FIRST(4, "d3d12: channel clear pipeline failed, mask %X", mask);
        return;
    }
    SetBlitTarget(Cpu(rtv_heap_.Get(), t.view, rtv_inc_), t.w, t.h);
    Alloc cb = Upload(256, 256);
    std::memcpy(cb.cpu, col, 4 * sizeof(float));
    uint32_t slot = AllocSrvSlots(1);
    for (const D3D12_RECT& r : rs) {
        cl_->RSSetScissorRects(1, &r);
        binds_ = {};
        DrawFullScreen(pso, cb.gpu, slot);
    }
}

void D3D12Backend::Clear(const State& s, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color, float z,
                         uint32_t stencil) {
    PerfScope perf_clear{g_perf.clear};
    if (cap_) {
        CapStep(s.rt);
        fprintf(cap_, "%04u clear rt %08X ds %08X flags %X color %08X z %.3f\n", cap_i_++, s.rt.data, s.zs.data, flags,
                color, z);
    }
    Target* rt = (flags & kClearTarget) ? EnsureTarget(s.rt, false) : nullptr;
    Target* ds = (flags & (kClearZ | kClearStencil)) ? EnsureTarget(s.zs, true) : nullptr;
    uint32_t gw;
    uint32_t gh;
    SurfaceSize(rt ? s.rt : s.zs, gw, gh);
    std::vector<D3D12_RECT> rs;
    if (rt && s.rt_back_buffer) OpenScene(*rt);
    bool scene = scene_ && (rt ? rt == scene_ : ds && (ds == SceneDepth() || s.rt_back_buffer));
    ClearArgs c{.count = count, .rects = rects, .flags = flags, .color = color, .z = z, .stencil = stencil, .scene = scene};
    for (Target* t : {rt, ds}) {
        if (t) ClearTarget(*t, s, c, rs);
    }
}

// CopyRects' guest lists: src_rects RECTs, dst_points POINTs, all int32
constexpr uint32_t kGuestRectBytes = 16;
constexpr uint32_t kRectLeft = 0;
constexpr uint32_t kRectTop = 4;
constexpr uint32_t kRectRight = 8;
constexpr uint32_t kRectBottom = 12;
constexpr uint32_t kGuestPointBytes = 8;
constexpr uint32_t kPointX = 0;
constexpr uint32_t kPointY = 4;

struct GuestRect {
    int32_t left;
    int32_t top;
    int32_t right;
    int32_t bottom;
};

struct GuestPoint {
    int32_t x;
    int32_t y;
};

GuestRect ReadGuestRect(uint32_t rects, uint32_t i) {
    uint32_t r = rects + i * kGuestRectBytes;
    GuestRect g;
    g.left = int32_t(kernel::R32(r + kRectLeft));
    g.top = int32_t(kernel::R32(r + kRectTop));
    g.right = int32_t(kernel::R32(r + kRectRight));
    g.bottom = int32_t(kernel::R32(r + kRectBottom));
    return g;
}

GuestPoint ReadGuestPoint(uint32_t points, uint32_t i) {
    uint32_t p = points + i * kGuestPointBytes;
    GuestPoint g;
    g.x = int32_t(kernel::R32(p + kPointX));
    g.y = int32_t(kernel::R32(p + kPointY));
    return g;
}

// a drawn copy's rect in guest pixels: the source corners and where it lands
struct DrawnRect {
    float x0 = 0;
    float y0 = 0;
    float x1 = 0;
    float y1 = 0;
    float dx = 0;
    float dy = 0;
};

// without dst_points the rect lands where it is
DrawnRect ReadDrawnRect(uint32_t src_rects, uint32_t dst_points, uint32_t i) {
    GuestRect g = ReadGuestRect(src_rects, i);
    DrawnRect r;
    r.x0 = float(g.left);
    r.y0 = float(g.top);
    r.x1 = float(g.right);
    r.y1 = float(g.bottom);
    r.dx = r.x0;
    r.dy = r.y0;
    if (dst_points) {
        GuestPoint p = ReadGuestPoint(dst_points, i);
        r.dx = float(p.x);
        r.dy = float(p.y);
    }
    return r;
}

// a guest coordinate read as unsigned, times the host scale
UINT ScaleUnsigned(int32_t v, float sc) { return UINT(uint32_t(v) * sc); }

// a drawn copy's taps round down a ratio a hair over a whole number
constexpr float kTapsSlack = 1e-3f;

// one Blit per rect: for formats that differ
void D3D12Backend::CopyByDraw(const Target& src_t, ID3D12Resource* src_res, D3D12_RESOURCE_STATES& src_state,
                              Target& dst_t, const CopyList& l, const float src_part[2], const float dst_part[2]) {
    Transition(src_res, src_state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Transition(dst_t.res.Get(), dst_t.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_CPU_DESCRIPTOR_HANDLE rh = Cpu(rtv_heap_.Get(), dst_t.view, rtv_inc_);
    cl_->OMSetRenderTargets(1, &rh, FALSE, nullptr);
    const float whole[2] = {1, 1};
    if (!src_part) src_part = whole;
    if (!dst_part) dst_part = whole;
    float src_scale = src_part[0] * float(src_t.w) / src_t.gw;
    float dst_scale = dst_part[0] * float(dst_t.w) / dst_t.gw, dst_down = dst_part[1] * float(dst_t.h) / dst_t.gh;
    bool listed = l.count && l.src_rects;
    uint32_t n = listed ? l.count : 1;
    for (uint32_t i = 0; i < n; i++) {
        float whole_w = float(std::min(src_t.gw, dst_t.gw));
        float whole_h = float(std::min(src_t.gh, dst_t.gh));
        DrawnRect r{.x0 = 0, .y0 = 0, .x1 = whole_w, .y1 = whole_h};
        if (listed) r = ReadDrawnRect(l.src_rects, l.dst_points, i);
        if (r.x1 <= r.x0 || r.y1 <= r.y0) continue;
        D3D12_VIEWPORT vp{r.dx * dst_scale, r.dy * dst_down, (r.x1 - r.x0) * dst_scale, (r.y1 - r.y0) * dst_down,
                          0, 1};
        LONG right = std::min(LONG(vp.TopLeftX + vp.Width), LONG(dst_t.w));
        LONG bottom = std::min(LONG(vp.TopLeftY + vp.Height), LONG(dst_t.h));
        D3D12_RECT scissor{LONG(vp.TopLeftX), LONG(vp.TopLeftY), right, bottom};
        if (scissor.right <= scissor.left || scissor.bottom <= scissor.top) continue;
        cl_->RSSetViewports(1, &vp);
        cl_->RSSetScissorRects(1, &scissor);
        binds_ = {};
        float uv[4] = {src_part[0] * r.x0 / src_t.gw, src_part[1] * r.y0 / src_t.gh,
                       src_part[0] * (r.x1 - r.x0) / src_t.gw, src_part[1] * (r.y1 - r.y0) / src_t.gh};
        uint32_t taps = uint32_t(std::ceil(std::max(src_scale / dst_scale, 1.0f) - kTapsSlack));
        float mul = 1 / dst_t.range;
        BlitOptions o{.alpha = true, .taps = taps, .mul = mul, .rtv = dst_t.fmt};
        Blit(src_res, src_t.fmt, uv, o);
    }
    dst_t.drawn = true;
    dst_t.cpu = false;
    bound_rt_ = bound_ds_ = nullptr;
}

// one CopyTextureRegion per rect, clamped to both targets
void D3D12Backend::CopyByRegion(const Target& src_t, ID3D12Resource* src_res, D3D12_RESOURCE_STATES& src_state,
                                Target& dst_t, const CopyList& l) {
    Transition(src_res, src_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Transition(dst_t.res.Get(), dst_t.state, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION dl{dst_t.res.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    D3D12_TEXTURE_COPY_LOCATION sl{src_res, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    float dst_scale = float(dst_t.w) / dst_t.gw, dst_down = float(dst_t.h) / dst_t.gh;
    bool listed = l.count && l.src_rects;
    uint32_t n = listed ? l.count : 1;
    for (uint32_t i = 0; i < n; i++) {
        D3D12_BOX b{0, 0, 0, std::min(src_t.w, dst_t.w), std::min(src_t.h, dst_t.h), 1};
        uint32_t dx = 0;
        uint32_t dy = 0;
        if (listed) {
            GuestRect r = ReadGuestRect(l.src_rects, i);
            b.left = ScaleUnsigned(r.left, dst_scale);
            b.top = ScaleUnsigned(r.top, dst_down);
            b.right = std::min(ScaleUnsigned(r.right, dst_scale), src_t.w);
            b.bottom = std::min(ScaleUnsigned(r.bottom, dst_down), src_t.h);
            dx = b.left;
            dy = b.top;
            if (l.dst_points) {
                GuestPoint p = ReadGuestPoint(l.dst_points, i);
                dx = ScaleUnsigned(p.x, dst_scale);
                dy = ScaleUnsigned(p.y, dst_down);
            }
        }
        if (b.right <= b.left || b.bottom <= b.top || dx >= dst_t.w || dy >= dst_t.h) continue;
        b.right = std::min<UINT>(b.right, b.left + dst_t.w - dx);
        b.bottom = std::min<UINT>(b.bottom, b.top + dst_t.h - dy);
        cl_->CopyTextureRegion(&dl, dx, dy, 0, &sl, &b);
    }
    dst_t.drawn = true;
    dst_t.cpu = false;
    bound_rt_ = bound_ds_ = nullptr;
}

void D3D12Backend::Copy(const Surface& src, const Surface& dst, uint32_t count, uint32_t src_rects,
                        uint32_t dst_points) {
    PerfScope perf_copy{g_perf.copy};
    Target* src_t = FindTarget(src, false);
    if (cap_) {
        const char* from = "memory";
        if (src_t) from = src_t->drawn ? "target" : "idle target";
        fprintf(cap_, "%04u copy %08X %s to %08X\n", cap_i_++, src.data, from, dst.data);
    }
    // pixels the CPU made: the copy happens in memory, textures and targets pick it up from there
    if (!src_t || !src_t->drawn) {
        if (!CopySurfaceMemory(src, dst, count, src_rects, dst_points)) {
            LOG_FIRST(8, "d3d12: CopyRects %08X to %08X skipped", src.data, dst.data);
        } else if (Target* dst_t = FindTarget(dst, false)) {
            UploadTarget(*dst_t, dst);
        }
        return;
    }
    Resolve(*src_t);
    Target* dst_t = EnsureTarget(dst, false);
    if (!dst_t || src_t == dst_t) return;
    CopyList l{.count = count, .src_rects = src_rects, .dst_points = dst_points};
    // upscaling: from or into the scene, its part of its target
    if (scene_ && (src_t == scene_ || dst_t == scene_)) {
        const float part[2] = {scene_fx_, scene_fy_};
        LOG_FIRST(2, "upscaling: a copy %s the frame's 3D (%08X to %08X)", src_t == scene_ ? "from" : "into", src.data,
                  dst.data);
        CopyByDraw(*src_t, src_t->res.Get(), src_t->state, *dst_t, l, src_t == scene_ ? part : nullptr,
                   dst_t == scene_ ? part : nullptr);
        return;
    }
    if (src_t->fmt != dst_t->fmt) {
        // formats differ: drawn across
        CopyByDraw(*src_t, src_t->res.Get(), src_t->state, *dst_t, l);
        return;
    }
    CopyByRegion(*src_t, src_t->res.Get(), src_t->state, *dst_t, l);
    if (cap_) {
        char name[32];
        snprintf(name, sizeof(name), "%04u_copy_%08X", cap_i_, dst_t->data);
        CapSave(*dst_t, name);
    }
}

void D3D12Backend::ResizeSwap() {
#ifdef _WIN32
    HWND hwnd = static_cast<HWND>(app::Window());
    RECT rc;
    GetClientRect(hwnd, &rc);
    uint32_t w = std::max<LONG>(rc.right - rc.left, 1);
    uint32_t h = std::max<LONG>(rc.bottom - rc.top, 1);
    if (presents_ % 30 == 0) CheckDisplay();
    // minimized, the window has no client area: the swap chain keeps its size until it comes back, rather than going
    // to 1x1 under frame generation (the game aborted, an exception through FSR's frame generation, as its window was
    // put out of the way)
    if (IsIconic(hwnd) || rc.right <= rc.left || rc.bottom <= rc.top) return;
#else
    uint32_t w = std::max<uint32_t>(linuxapp::g_pixel_w, 1);
    uint32_t h = std::max<uint32_t>(linuxapp::g_pixel_h, 1);
    if (presents_ % 30 == 0) CheckDisplay();
#endif
    bool hdr = app::Get().hdr && display_hdr_ && !hdr_failed_;
    if (w == bb_w_ && h == bb_h_ && hdr == hdr_) return;
    if (fg_) fg_->BeforeResize();
    Flush();
    for (auto& f : frames_) {
        if (f.fence) WaitFence(f.fence);
    }
    for (auto& b : bb_) b.Reset();
    bool was = hdr_;
#ifdef _WIN32
    UINT flags = tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    hdr_ = hdr && SUCCEEDED(swap_->ResizeBuffers(kFrames, w, h, HdrFormat(), flags)) && SetColourSpace(true);
    if (hdr && !hdr_) {
        hdr_failed_ = true;
        LOGR("hdr: the swap chain takes no %s here, SDR output", hdr10_ ? "HDR10" : "scRGB");
    }
    if (!hdr_) {
        swap_->ResizeBuffers(kFrames, w, h, kBgra, flags);
        SetColourSpace(false);
    }
#else
    hdr_ = hdr && swap_->ResizeBuffers(w, h, true);
    if (hdr && !hdr_) {
        hdr_failed_ = true;
        LOGR("hdr: no HDR10 back buffers here, SDR output");
    }
    if (!hdr_) swap_->ResizeBuffers(w, h, false);
#endif
    CreateBackBufferViews();
    bb_w_ = w;
    bb_h_ = h;
    if (fg_) fg_->Resized(w, h, BackFormat());
    if (hdr_ != was) {
        if (hdr_) {
            LOGR("hdr: output HDR (%s), scene %u nits, peak %u nits", hdr10_ ? "HDR10, for frame generation" : "scRGB",
                 app::Get().hdr_scene_nits, app::HdrPeak());
        } else {
            LOGR("hdr: output SDR");
        }
    }
    app::SetHdr({display_hdr_, hdr_, uint32_t(display_peak_)});
}

// the game window's display: in Windows HDR mode or not, and its peak; asked again when DXGI reports the displays
// changed (HDR switched in Windows, a display plugged in) or the window went to another monitor
void D3D12Backend::CheckDisplay() {
#ifndef _WIN32
    // Linux: the window's compositor in HDR, and its surface taking an HDR10 swap chain (linux/present.cpp)
    float peak = 0;
    bool hdr = swap_ && swap_->HdrDisplay(peak);
    if (!display_logged_ || hdr != display_hdr_ || peak != display_peak_) {
        LOGR("hdr: the display is %s, peak %.0f nits%s", hdr ? "HDR" : "SDR", peak,
             app::Get().hdr ? "" : ", HDR output off in the settings");
        display_logged_ = true;
    }
    display_hdr_ = hdr;
    display_peak_ = peak;
    app::SetHdr({display_hdr_, hdr_, uint32_t(display_peak_)});
#else
    HMONITOR mon = MonitorFromWindow(static_cast<HWND>(app::Window()), MONITOR_DEFAULTTONEAREST);
    if (display_factory_ && display_factory_->IsCurrent() && mon == display_monitor_) return;
    display_monitor_ = mon;
    display_factory_.Reset();
    bool hdr = false;
    float peak = 0;
    if (SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&display_factory_)))) {
        ComPtr<IDXGIAdapter1> a;
        for (UINT i = 0; display_factory_->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; i++) {
            ComPtr<IDXGIOutput> o;
            for (UINT j = 0; a->EnumOutputs(j, &o) != DXGI_ERROR_NOT_FOUND; j++) {
                ComPtr<IDXGIOutput6> o6;
                DXGI_OUTPUT_DESC1 d{};
                if (FAILED(o.As(&o6)) || FAILED(o6->GetDesc1(&d)) || d.Monitor != mon) continue;
                hdr = d.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
                peak = d.MaxLuminance;
            }
        }
    }
    if (!display_logged_ || hdr != display_hdr_ || peak != display_peak_) {
        LOGR("hdr: the display is %s in Windows, peak %.0f nits%s", hdr ? "HDR" : "SDR", peak,
             app::Get().hdr ? "" : ", HDR output off in the settings");
        display_logged_ = true;
    }
    display_hdr_ = hdr;
    display_peak_ = peak;
    app::SetHdr({display_hdr_, hdr_, uint32_t(display_peak_)});
#endif
}

// the swap chain's colour space: scRGB (or HDR10) for HDR, else sRGB; false when the display path refuses it
bool D3D12Backend::SetColourSpace(bool hdr) {
#ifndef _WIN32
    (void)hdr;
    return true;  // Linux: the swap chain's own (linux/present.cpp)
#else
    DXGI_COLOR_SPACE_TYPE cs = !hdr     ? DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709
                               : hdr10_ ? DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020
                                        : DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;
    UINT support = 0;
    if (FAILED(swap_->CheckColorSpaceSupport(cs, &support)) ||
        !(support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT)) {
        return false;
    }
    return SUCCEEDED(swap_->SetColorSpace1(cs));
#endif
}

void D3D12Backend::FinishCapture(const Surface& front) {
    CapStep(Surface{});
    if (Target* f = FindTarget(front, false)) CapSave(*f, "final");
    for (auto& t : cap_tex_) {
        char name[32];
        snprintf(name, sizeof(name), "tex_%08X", t.data);
        D3D12_RESOURCE_STATES st = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        CapSave({.res = t.res.Get(), .state = &st, .fmt = t.fmt, .w = t.w, .h = t.h}, name);
    }
    fclose(cap_);
    cap_ = nullptr;
    cap_tex_.clear();
    LOGR("d3d12: frame captured to %s", cap_dir_.string().c_str());
    g_captures_saved++;
}

void D3D12Backend::StartCapture() {
    char stamp[32];
#ifdef _WIN32
    SYSTEMTIME tm;
    GetLocalTime(&tm);
    snprintf(stamp, sizeof(stamp), "%02u%02u%02u", tm.wHour, tm.wMinute, tm.wSecond);
#else
    std::time_t now = std::time(nullptr);
    std::strftime(stamp, sizeof(stamp), "%H%M%S", std::localtime(&now));
#endif
    std::error_code ec;
    std::filesystem::path dir = diag::CaptureDir().empty() ? std::filesystem::path(L"captures") : diag::CaptureDir();
    cap_dir_ = std::filesystem::weakly_canonical(app::ExeDir() / dir / stamp, ec);
    std::filesystem::create_directories(cap_dir_, ec);
    cap_ = _wfopen((cap_dir_ / "frame.txt").c_str(), L"w");
    cap_i_ = cap_rt_ = 0;
    cap_seen_.clear();
    cap_sh_.clear();
}

// ramp_ holds the red, green and blue ramps, 256 entries each
constexpr int kRampRed = 0;
constexpr int kRampGreen = 256;
constexpr int kRampBlue = 512;

void D3D12Backend::UploadGammaRamp() {
    constexpr uint32_t kRampBytes = 256 * 4;  // one BGRA texel per ramp entry
    ramp_dirty_ = false;
    Alloc a = Upload(kRampBytes + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
    bool identity = true;
    for (int i = 0; i < 256; i++) {
        uint8_t* p = a.cpu + i * 4;
        p[0] = ramp_[kRampBlue + i];
        p[1] = ramp_[kRampGreen + i];
        p[2] = ramp_[kRampRed + i];
        p[3] = 255;
        identity &= ramp_[kRampRed + i] == i && ramp_[kRampGreen + i] == i && ramp_[kRampBlue + i] == i;
    }
    gamma_ = !identity;
    Transition(lut_.Get(), lut_state_, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION dl{lut_.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    D3D12_TEXTURE_COPY_LOCATION sl{a.res, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    sl.PlacedFootprint.Offset = a.off;
    sl.PlacedFootprint.Footprint = {DXGI_FORMAT_B8G8R8A8_UNORM, 256, 1, 1, kRampBytes};
    cl_->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
}

// the frame's place on the back buffer: 16:9 or 16:10 when the console reports a widescreen display, else 4:3, centred
D3D12_VIEWPORT D3D12Backend::FrameViewport() const {
    bool wide = app::Widescreen();
    float aspect = wide ? app::WideAspect() : 4.0f / 3.0f;
    float w = float(bb_w_);
    float h = float(bb_h_);
    float vw = std::min(w, h * aspect);
    float vh = vw / aspect;
    // whole pixels, so frame generation's inputs cover it exactly
    float x = std::floor((w - vw) / 2), y = std::floor((h - vh) / 2);
    return {x, y, std::round(vw), std::round(vh), 0, 1};
}

void D3D12Backend::BlitFrame(Target& t) {
    float uv[4] = {0, 0, 1, 1};
    D3D12_VIEWPORT vp = FrameViewport();
    D3D12_RECT scissor{0, 0, LONG(bb_w_), LONG(bb_h_)};
    cl_->RSSetViewports(1, &vp);
    cl_->RSSetScissorRects(1, &scissor);
    binds_ = {};
    Transition(t.res.Get(), t.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    hdr_before_ = nullptr;
    if (hdr_ && ui_snap_ && ui_snap_frame_ == presents_ && ui_snap_data_ == t.data) {
        Transition(ui_snap_.Get(), ui_snap_state_, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        hdr_before_ = ui_snap_.Get();
    }
    hdr_cb_[3] = hdr_before_ ? 1.0f : 0.0f;
    ui_snaps_shown_ = ui_snap_frame_ == presents_ ? ui_snaps_ : 0;
    Blit(t.res.Get(), t.fmt, uv, {.gamma = gamma_, .rtv = BackFormat(), .hdr = hdr_ ? 1u : 0u});
    hdr_before_ = nullptr;
}

// HDR: the frame as it is before its UI, for the frame's pass to tell what the UI changed; a few copies a frame at
// most, the last one kept
void D3D12Backend::SnapshotUi(Target& t) {
    constexpr uint32_t kMaxSnaps = 4;
    if (ui_snap_frame_ != presents_) ui_snaps_ = 0;
    if (ui_snaps_ >= kMaxSnaps) return;
    D3D12_RESOURCE_DESC rd = t.res->GetDesc();
    D3D12_RESOURCE_DESC have = ui_snap_ ? ui_snap_->GetDesc() : D3D12_RESOURCE_DESC{};
    if (!ui_snap_ || have.Width != rd.Width || have.Height != rd.Height || have.Format != rd.Format) {
        if (ui_snap_) Release(std::move(ui_snap_));
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC td = Texture2DDesc(rd.Format, rd.Width, rd.Height);
        ui_snap_state_ = D3D12_RESOURCE_STATE_COPY_DEST;
        if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, ui_snap_state_, nullptr,
                                                 IID_PPV_ARGS(&ui_snap_)))) {
            return;
        }
    }
    Transition(t.res.Get(), t.state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Transition(ui_snap_.Get(), ui_snap_state_, D3D12_RESOURCE_STATE_COPY_DEST);
    cl_->CopyResource(ui_snap_.Get(), t.res.Get());
    Transition(t.res.Get(), t.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
    ui_snap_data_ = t.data;
    ui_snap_frame_ = presents_;
    ui_snaps_++;
}

// frame generation's inputs from the frame's depth buffer and the extension's view (Extension::View), drawn over the
// frame's place on the back buffer at its size: the depth buffer lines up with the frame, so uv is both's, times
// opt.zw: the part of the depth and motion the frame is in (upscaling's scene, at the top left), else 1
const char* const kFrameGenHlsl = R"(
cbuffer P : register(b1) { float4 m0; float4 m1; float4 m2; float4 m3; float4 opt; };
Texture2D<float> depth : register(t0);
// motion: the 3D draws' own (MotionOut, opt.x 1 when drawn this frame), 1024 where none drew; showmotion: the result
Texture2D<float2> mv : register(t1);
SamplerState ls : register(s0);
SamplerState ps : register(s1);
struct O { float4 p : SV_Position; float2 uv : TEXCOORD0; };
O vs(uint id : SV_VertexID) {
    O o;
    float2 t = float2((id << 1) & 2, id & 2);
    o.p = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = t;
    return o;
}
// to where the pixel was a frame before, in UV units: as its draw moved (a character, a door, the world with the
// camera), else for the world as it held still
float4 motion(O i) : SV_Target {
    float2 at = i.uv * opt.zw;
    if (opt.x != 0) {
        float2 own = mv.SampleLevel(ps, at, 0);
        if (all(abs(own) < 4)) return float4(own, 0, 0);
    }
    float d = depth.SampleLevel(ps, at, 0);
    float4 c = float4(i.uv.x * 2 - 1, 1 - i.uv.y * 2, d, 1);
    float4 p = float4(dot(m0, c), dot(m1, c), dot(m2, c), dot(m3, c));
    if (!(abs(p.w) > 1e-20)) return 0;
    float2 prev = float2(p.x / p.w * 0.5 + 0.5, 0.5 - p.y / p.w * 0.5);
    float2 mv = prev - i.uv;
    return float4(all(abs(mv) < 4) ? mv : 0, 0, 0);
}
float4 depthcopy(O i) : SV_Target { return depth.SampleLevel(ps, i.uv * opt.zw, 0).xxxx; }
// upscaling: the scene's motion (motion's, every pixel's) over the whole of the 3D draws' own motion target
float4 mvwhole(O i) : SV_Target { return float4(mv.SampleLevel(ps, i.uv * opt.zw, 0), 0, 0); }
// XBE_FG_SHOW_MOTION: the motion drawn over the frame, x red, y green, grey still
float4 showmotion(O i) : SV_Target {
    float2 m = mv.SampleLevel(ps, i.uv, 0) * 20;
    return float4(saturate(0.5 + m.x), saturate(0.5 + m.y), 0.5, 1);
}
)";

// anti-aliasing's depth and motion at the frame's size (drawn as frame generation's are), and AMD's output in the
// frame's format when the card can write it so, then copied back; else RGBA and drawn back
bool D3D12Backend::EnsureAaTargets(uint32_t w, uint32_t h, DXGI_FORMAT fmt) {
    bool vk = aa_ && aa_->VulkanImages();
    if (aa_depth_ && aa_motion_ && aa_out_ && aa_w_ == w && aa_h_ == h && aa_fmt_ == fmt && aa_vk_ == vk) return true;
#ifndef _WIN32
    if (aa_vk_) {
        // the anti-aliasing's own images: released once the GPU is done with them, then their Vulkan images freed
        Flush();
        for (ComPtr<ID3D12Resource>* r : {std::addressof(aa_depth_), std::addressof(aa_motion_), std::addressof(aa_out_),
                                          std::addressof(aa_vk_color_), std::addressof(aa_vk_rx_)}) {
            ID3D12Resource* raw = r->Get();
            *r = nullptr;
            if (raw) linux_vk::DropImage(raw);
        }
        aa_vk_ = false;
    }
#endif
    if (aa_depth_) Release(std::move(aa_depth_));
    if (aa_motion_) Release(std::move(aa_motion_));
    if (aa_out_) Release(std::move(aa_out_));
    if (aa_vk_color_) Release(std::move(aa_vk_color_));
    if (aa_vk_rx_) Release(std::move(aa_vk_rx_));
#ifndef _WIN32
    if (vk) {
        // Linux's AMD FSR and NVIDIA DLSS on Vulkan: every input and the output an image of its own, in COMMON (Vulkan's
        // GENERAL)
        auto own = [&](ComPtr<ID3D12Resource>& r, D3D12_RESOURCE_STATES& st, DXGI_FORMAT f, bool rt) {
            r.Attach(linux_vk::MakeImage(dev_.Get(), w, h, f, rt));
            st = D3D12_RESOURCE_STATE_COMMON;
            return r != nullptr;
        };
        bool made = own(aa_depth_, aa_depth_state_, DXGI_FORMAT_R32_FLOAT, true) &&
                    own(aa_motion_, aa_motion_state_, DXGI_FORMAT_R16G16_FLOAT, true) &&
                    own(aa_out_, aa_out_state_, DXGI_FORMAT_R8G8B8A8_UNORM, false) &&
                    own(aa_vk_color_, aa_vk_color_state_, fmt, false) &&
                    own(aa_vk_rx_, aa_vk_rx_state_, DXGI_FORMAT_R8_UNORM, false);
        if (made) {
            dev_->CreateRenderTargetView(aa_depth_.Get(), nullptr, Cpu(rtv_heap_.Get(), kAaRtv, rtv_inc_));
            dev_->CreateRenderTargetView(aa_motion_.Get(), nullptr, Cpu(rtv_heap_.Get(), kAaRtv + 1, rtv_inc_));
            aa_out_fmt_ = DXGI_FORMAT_R8G8B8A8_UNORM;
            aa_w_ = w;
            aa_h_ = h;
            aa_fmt_ = fmt;
            aa_vk_ = true;
            return true;
        }
        LOGR("antialias: %s's images were not made at %ux%u", AaName(), w, h);
        aa_depth_ = aa_motion_ = aa_out_ = aa_vk_color_ = aa_vk_rx_ = nullptr;
        return false;
    }
#endif
    aa_vk_ = false;
    auto rt = [&](ComPtr<ID3D12Resource>& r, D3D12_RESOURCE_STATES& st, DXGI_FORMAT f, uint32_t rtv) {
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC td = Texture2DDesc(f, w, h);
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_CLEAR_VALUE cv{};
        cv.Format = f;
        st = D3D12_RESOURCE_STATE_RENDER_TARGET;
        if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, st, &cv, IID_PPV_ARGS(&r)))) return false;
        dev_->CreateRenderTargetView(r.Get(), nullptr, Cpu(rtv_heap_.Get(), rtv, rtv_inc_));
        return true;
    };
    D3D12_FEATURE_DATA_FORMAT_SUPPORT fs{fmt};
    bool store = SUCCEEDED(dev_->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &fs, sizeof(fs))) &&
                 (fs.Support1 & D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW) &&
                 (fs.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE);
    aa_out_fmt_ = store ? fmt : DXGI_FORMAT_R8G8B8A8_UNORM;
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC od = Texture2DDesc(aa_out_fmt_, w, h);
    od.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    aa_out_state_ = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    bool ok = rt(aa_depth_, aa_depth_state_, DXGI_FORMAT_R32_FLOAT, kAaRtv) &&
              rt(aa_motion_, aa_motion_state_, DXGI_FORMAT_R16G16_FLOAT, kAaRtv + 1) &&
              SUCCEEDED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &od, aa_out_state_, nullptr,
                                                      IID_PPV_ARGS(&aa_out_)));
    if (!ok) {
        aa_depth_ = aa_motion_ = aa_out_ = nullptr;
        return false;
    }
    aa_w_ = w;
    aa_h_ = h;
    aa_fmt_ = fmt;
    return true;
}

// Ambient occlusion (AmbientOcclusion): how much of the sky above each point of the frame's 3D its surroundings hide,
// from the depth alone. Each pixel's place in the view and its surface's normal come from the depth buffer (the view's
// near_w at its 0 and far_w at its 1, FrameView: this title's has the near plane at 0); samples on a spiral within a radius in the view, turned per pixel and per frame (the
// anti-aliasing gathers the frames), count where they stand above the surface. aoapply then blurs it among neighbours
// at a like depth and multiplies the frame by it, less under the frame's effects (the reactive mask)
const char* const kAoHlsl = R"(
cbuffer P : register(b1) {
    float4 cam;   // the view's depth at the buffer's 0 and its 1, tan of half the view across, down
    float4 part;  // the depth's part the frame is in (uv scale), and one of its texels in the frame's uv
    float4 ao;    // radius in view units, strength, distance where it has faded out, samples
    float4 fr;    // the frame's number, reactive mask (1) or none, one ambient occlusion texel in the frame's uv
    float4 aop;   // the ambient occlusion's part of its texture (uv scale), XBE_AO_VIEW, 1 with no anti-aliasing
};
Texture2D<float> depth : register(t0);
Texture2D<float> aot : register(t1);
Texture2D<float> rx : register(t2);
SamplerState ls : register(s0);
SamplerState ps : register(s1);
struct O { float4 p : SV_Position; float2 uv : TEXCOORD0; };
O vs(uint id : SV_VertexID) {
    O o;
    float2 t = float2((id << 1) & 2, id & 2);
    o.p = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = t;
    return o;
}
float Depth(float2 uv) { return depth.SampleLevel(ps, uv * part.xy, 0); }
// the buffer holds a + b / z: 1 / z runs straight from 1 / cam.x at 0 to 1 / cam.y at 1
float ViewZ(float d) { return cam.x * cam.y / (d * cam.x + (1 - d) * cam.y); }
float3 ViewPos(float2 uv, float d) {
    float z = ViewZ(d);
    return float3((uv.x * 2 - 1) * cam.z * z, (1 - uv.y * 2) * cam.w * z, z);
}
// interleaved gradient noise: a turn per pixel that differs from its neighbours
float Noise(float2 px) { return frac(52.9829189 * frac(dot(px, float2(0.06711056, 0.00583715)))); }
float4 aomain(O i) : SV_Target {
    float d = Depth(i.uv);
    // XBE_AO_VIEW 3: the view depth in bands of 1000 units; 4: why it gave none (0 no depth, 0.2 too far, 0.4 a radius
    // under a pixel), else 0.6 and up with its shade; 5: 3 and 4 by turns, about 8 seconds each
    float mode = aop.z == 5 ? (fr.x < 512 ? 3 : 4) : aop.z;
    bool why = mode == 4;
    float3 p = ViewPos(i.uv, d);
    if (p.z >= 0.999 * max(cam.x, cam.y)) return why ? 0 : 1;  // nothing drawn there: the far plane
    if (mode == 3) return frac(p.z / 1000);
    if (p.z >= ao.z) return why ? 0.2 : 1;
    // the surface's slope from the nearer neighbour each way, so an edge doesn't bend it
    float2 du = float2(part.z, 0), dv = float2(0, part.w);
    float3 pr = ViewPos(i.uv + du, Depth(i.uv + du)) - p, pl = p - ViewPos(i.uv - du, Depth(i.uv - du));
    float3 pd = ViewPos(i.uv + dv, Depth(i.uv + dv)) - p, pu = p - ViewPos(i.uv - dv, Depth(i.uv - dv));
    float3 dx = abs(pr.z) < abs(pl.z) ? pr : pl, dy = abs(pd.z) < abs(pu.z) ? pd : pu;
    float3 n = normalize(cross(dx, dy));
    if (n.z > 0) n = -n;  // facing the camera, which looks along +z
    // the radius on the screen at the point's distance, in the frame's uv
    float2 r = ao.x / (2 * p.z * cam.zw);
    if (max(r.x, r.y) < 0.75 * part.z) return why ? 0.4 : 1;  // under a pixel: too far for any
    int count = int(ao.w);
    float turn = (Noise(i.p.xy) + fr.x * 0.618034) * 6.2831853, hidden = 0;
    [loop] for (int k = 0; k < count; k++) {
        float t = (k + 0.5) / count;
        float a = turn + k * 2.3999632;  // the golden angle: a spiral that fills the disc evenly
        float2 su = i.uv + float2(cos(a), sin(a)) * r * sqrt(t);
        if (any(su < 0) || any(su > 1)) continue;
        float3 v = ViewPos(su, Depth(su)) - p;
        float len = length(v);
        // above the surface, by its angle to it, less the farther within the radius
        float near = saturate(1 - len * len / (ao.x * ao.x));
        hidden += max(dot(v, n) / max(len, 1e-3) - 0.1, 0) * near;
    }
    float vis = saturate(1 - ao.y * hidden / count);
    if (why) return 0.6 + 0.4 * vis;
    // faded out with distance: the far half of its reach
    return lerp(vis, 1, saturate((p.z - 0.5 * ao.z) / (0.5 * ao.z)));
}
float4 aoapply(O i) : SV_Target {
    float zc = ViewZ(Depth(i.uv)), sum = 0, wsum = 0;
    // among the 3 by 3 neighbours, 5 by 5 when nothing gathers the frames after (no anti-aliasing): its noise held
    // still, the wider blur smooths it instead
    int reach = aop.w > 0 ? 2 : 1;
    [loop] for (int y = -reach; y <= reach; y++) {
        [loop] for (int x = -reach; x <= reach; x++) {
            float2 su = i.uv + float2(x, y) * fr.zw;
            float a = aot.SampleLevel(ls, saturate(su) * aop.xy, 0);
            float w = exp(-abs(ViewZ(Depth(saturate(su))) - zc) / (0.05 * zc));
            sum += a * w;
            wsum += w;
        }
    }
    float v = sum / max(wsum, 1e-4);
    // an effect over the pixel (a spark, a glow) keeps its own light
    if (fr.y > 0) v = lerp(v, 1, saturate(rx.SampleLevel(ps, i.uv * part.xy, 0) / 0.9));
    if (aop.z == 2) v = frac(zc / 1000) * 0.8 + 0.2 * saturate(zc / ao.z);  // XBE_AO_VIEW=2: bands of 1000 units
    if (aop.z >= 3) v = aot.SampleLevel(ps, i.uv * aop.xy, 0);  // 3, 4: aomain's own, unblurred
    return float4(v, v, v, 1);
}
)";

// the frame multiplied by ambient occlusion's: colour times the pass's, alpha kept
ID3D12PipelineState* D3D12Backend::EnsureAoApplyPipeline(DXGI_FORMAT rtv) {
    auto& pso = blit_psos_[Hash("aoapply", 7, uint64_t(rtv))];
    if (pso) return pso.Get();
    uint64_t h;
    ID3DBlob* vs = Compile(kAoHlsl, "vs", "vs_5_0", h);
    ID3DBlob* ps = Compile(kAoHlsl, "aoapply", "ps_5_0", h);
    if (!vs || !ps) return nullptr;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d = FullScreenPipelineDesc(vs, ps, rtv);
    auto& b = d.BlendState.RenderTarget[0];
    b.BlendEnable = TRUE;
    b.SrcBlend = b.SrcBlendAlpha = D3D12_BLEND_ZERO;
    b.DestBlend = D3D12_BLEND_SRC_COLOR;
    b.DestBlendAlpha = D3D12_BLEND_ONE;
    b.BlendOp = b.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    b.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_GREEN | D3D12_COLOR_WRITE_ENABLE_BLUE;
    if (diag::AoView()) b.BlendEnable = FALSE;  // XBE_AO_VIEW: its shade alone
    if (FAILED(CreateGraphicsPipelineCached(dev_.Get(), d, IID_PPV_ARGS(&pso)))) return nullptr;
    return pso.Get();
}

// ambient occlusion's quality (g_ambient_occlusion): its radius in view units, its strength, the distance where it has
// faded out, its samples, and the part of the frame's size it is worked out at. This title's units are a few
// millimetres (Hector stands about 400 tall, some 2000 from the camera in the throne room), so a radius of 200 to 250
// is about a metre: High's 250 looked right there, contact shadow at the stairs, pillars and alcoves without darkening
// whole walls (1000 did)
struct AoQuality {
    float radius, strength, reach, samples, scale;
};
// (strength 2.4: a right-angled corner at about 0.72 after the blur, a flat floor or wall at 1, checked on a made-up
// depth buffer of one)
constexpr AoQuality kAoLow{200, 2.4f, 15000, 8, 0.5f};
constexpr AoQuality kAoHigh{250, 2.4f, 20000, 16, 1.0f};

// the world's up in the view's space (ray tracing's: x right, y up, z ahead): the world's axis nearest the camera's
// up, as this title's camera stays about level. tilt: degrees toward the world's next axis (a light above, a little
// to one side, the same way in every view)
static void WorldUpInView(const FrameView& v, float out[3], float tilt = 0) {
    int axis = 0;
    float best = 0;
    for (int i = 0; i < 3; i++)
        if (std::abs(v.up[i]) > std::abs(best)) {
            best = v.up[i];
            axis = i;
        }
    float w[3] = {0, 0, 0};
    float a = tilt * 3.14159265f / 180.0f;
    w[axis] = (best < 0 ? -1.0f : 1.0f) * std::cos(a);
    w[(axis + 1) % 3] = std::sin(a);
    for (int k = 0; k < 3; k++) {
        const float* e = k == 0 ? v.right : k == 1 ? v.up : v.forward;
        out[k] = w[0] * e[0] + w[1] * e[1] + w[2] * e[2];
    }
}

// characters' metal shines (rt_scene.hlsli's CharacterGloss) unless XBE_RT_CHARACTER_GLOSS=0
static float CharacterGlossOn() {
    static const float on = std::getenv("XBE_RT_CHARACTER_GLOSS") ? float(std::atof(std::getenv("XBE_RT_CHARACTER_GLOSS"))) : 1.0f;
    return on;
}

// a 4x4 matrix's inverse (row major), false when it has none
static bool Invert4(const double m[16], double out[16]) {
    double a[4][8];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 8; j++) a[i][j] = j < 4 ? m[i * 4 + j] : (j - 4 == i ? 1.0 : 0.0);
    for (int c = 0; c < 4; c++) {
        int best = c;
        for (int r = c + 1; r < 4; r++)
            if (std::abs(a[r][c]) > std::abs(a[best][c])) best = r;
        if (std::abs(a[best][c]) < 1e-12) return false;
        if (best != c)
            for (int j = 0; j < 8; j++) std::swap(a[c][j], a[best][j]);
        double k = 1.0 / a[c][c];
        for (int j = 0; j < 8; j++) a[c][j] *= k;
        for (int r = 0; r < 4; r++) {
            if (r == c) continue;
            double f = a[r][c];
            if (f != 0)
                for (int j = 0; j < 8; j++) a[r][j] -= f * a[c][j];
        }
    }
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) out[i * 4 + j] = a[i][j + 4];
    return true;
}

// the frame's ray tracing scene built, once (raytrace.h): put back into view space by the view's scale across and down
bool D3D12Backend::BuildRtScene(uint32_t rw, uint32_t rh) {
    if (!rt_frame_ || !ao_view_ok_) return false;
    if (rt_.Built()) return true;
    PerfScope perf_rt{g_perf.rt};
    if (!rt_.Draws()) {
        if (!rt_said_) LOGR("ray tracing: frame %llu gathered no draws for its scene", (unsigned long long)presents_);
        rt_said_ = true;
        return false;
    }
    float tan_y = std::tan(0.5f * ao_view_.fov_y);
    const float to_view[12] = {tan_y * float(rw) / float(rh), 0, 0, 0, 0, tan_y, 0, 0, 0, 0, 1, 0};
    // the frame before's water (Scene::LateCapture), its clip x, y and w, put into this frame's: the camera's move
    // (clip_to_prev, this frame's clip to the one before's) taken back. Its depth is the view's own depth, so the move
    // is affine in (x, y, w, 1): z = k0 w + k1
    float water[12];
    bool wet = false;
    if (rt_view_now_ && (g_ray_tracing.load(std::memory_order_relaxed) & kRtReflections)) {
        const float* c = ao_view_.clip_to_prev;
        double n = ao_view_.near_w, f = ao_view_.far_w;
        if (std::abs(f - n) > 1e-6) {
            double k0 = f / (f - n), k1 = n * f / (n - f);
            double m[16];
            const int row[3] = {0, 1, 3};
            for (int i = 0; i < 3; i++) {
                const float* r = c + row[i] * 4;
                m[i * 4 + 0] = r[0];
                m[i * 4 + 1] = r[1];
                m[i * 4 + 2] = r[2] * k0 + r[3];
                m[i * 4 + 3] = r[2] * k1;
            }
            m[12] = m[13] = m[14] = 0;
            m[15] = 1;
            double inv[16];
            if (Invert4(m, inv)) {
                const double sx = tan_y * double(rw) / double(rh), sy = tan_y;
                for (int j = 0; j < 4; j++) {
                    water[j] = float(sx * inv[j]);
                    water[4 + j] = float(sy * inv[4 + j]);
                    water[8 + j] = float(inv[8 + j]);
                }
                wet = true;
            }
        }
    }
    // the water's pictures, made again in this frame's descriptors (the frame before's are written again two frames on)
    std::vector<uint32_t> pics;
    if (wet) {
        uint32_t n = rt_.LateBefore();
        const std::vector<LatePicture>& lp = late_pictures_[(frame_ + kFrames - 1) % kFrames];
        pics.assign(n, UINT32_MAX);
        if (lp.size() == n)
            for (uint32_t i = 0; i < n; i++) {
                if (!lp[i].res) continue;
                uint32_t slot = AllocSrvSlots(1);
                dev_->CreateShaderResourceView(lp[i].res.Get(), &lp[i].sv, Cpu(srv_heap_.Get(), slot, srv_inc_));
                pics[i] = slot;
            }
    }
    return rt_.Build(to_view, wet ? water : nullptr, pics.empty() ? nullptr : pics.data());
}

// after a ray tracing pass, which has its own root signature: the renderer's back, with what it binds once
void D3D12Backend::EndRtPass() {
    cl_->SetGraphicsRootSignature(root_.Get());
    BindRtCapture();
    binds_ = {};
    bound_rt_ = bound_ds_ = nullptr;
}

// the guides' formats (Guides)
static const DXGI_FORMAT kGuideFmt[kGuides + kGuideExtra] = {
    DXGI_FORMAT_R8G8B8A8_UNORM,       DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R8G8B8A8_UNORM,
    DXGI_FORMAT_R16_FLOAT,            DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT,
    // the eye's hits: the draw, its triangle, the point on it and the distance, each exact in a float
    DXGI_FORMAT_R32G32B32A32_FLOAT};

// Ray tracing's guides at the frame's full size (rt_guide.hlsl), once a frame: each pixel's surface albedo, normal,
// roughness and specular albedo, which the bounce lighting's smoothing takes and DLSS Ray Reconstruction denoises with;
// its surface for the own denoisers; and what the eye's ray met there (kGuideHit), for the passes after it
bool D3D12Backend::Guides(const Target& t, uint32_t rw, uint32_t rh) {
    if (guide_frame_ == presents_) return true;
    const DXGI_FORMAT* kFmt = kGuideFmt;
    if (!guide_[0] || guide_w_ != t.w || guide_h_ != t.h) {
        LOGR_FIRST(16, "ray tracing: guides made at %ux%u (frame %llu)", t.w, t.h, (unsigned long long)presents_);
        surface_frame_[0] = surface_frame_[1] = UINT64_MAX;  // new: no surfaces from before
        for (uint32_t i = 0; i < kGuides + kGuideExtra; i++) {
            if (guide_[i]) Release(std::move(guide_[i]));
            D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
            D3D12_RESOURCE_DESC td = Texture2DDesc(kFmt[i], t.w, t.h);
            td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            D3D12_CLEAR_VALUE cv{};
            cv.Format = kFmt[i];
            guide_state_[i] = D3D12_RESOURCE_STATE_RENDER_TARGET;
            if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, guide_state_[i], &cv,
                                                     IID_PPV_ARGS(&guide_[i])))) {
                for (auto& g : guide_) g = nullptr;
                guide_w_ = guide_h_ = 0;
                return false;
            }
            dev_->CreateRenderTargetView(guide_[i].Get(), nullptr, Cpu(rtv_heap_.Get(), kGuideRtv + i, rtv_inc_));
        }
        guide_w_ = t.w;
        guide_h_ = t.h;
    }
    uint32_t slot = AllocSrvSlots(1);
    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = DXGI_FORMAT_R32_FLOAT;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    dev_->CreateShaderResourceView(aa_depth_.Get(), &sv, Cpu(srv_heap_.Get(), slot, srv_inc_));
    float tan_y = std::tan(0.5f * ao_view_.fov_y);
    // the view in the world: the ray tracing's view space (x right, y up, z ahead on the screen) for the normals, and
    // the view's own as its projection has it (each axis turned round where the projection turns it) for the denoiser
    const FrameView& v = ao_view_;
    const float* axes[3] = {v.right, v.up, v.forward};
    float flip[3] = {v.view_to_clip[0] < 0 ? -1.0f : 1.0f, v.view_to_clip[5] < 0 ? -1.0f : 1.0f,
                     v.view_to_clip[14] < 0 ? -1.0f : 1.0f};
    // characters' metal and water (rt_guide.hlsl): water's roughness, with reflections on
    static const float kWaterRough = std::getenv("XBE_RT_WATER_ROUGHNESS") ? float(std::atof(std::getenv("XBE_RT_WATER_ROUGHNESS"))) : 0.04f;
    float water = (g_ray_tracing.load(std::memory_order_relaxed) & kRtReflections) ? kWaterRough : 0.0f;
    float p[20] = {v.near_w, v.far_w, tan_y * float(rw) / float(rh), tan_y, float(rw), float(rh), CharacterGlossOn(), water};
    for (int a = 0; a < 3; a++)
        for (int k = 0; k < 3; k++) {
            p[8 + a * 4 + k] = axes[a][k];
            guide_view_[a * 3 + k] = flip[a] * axes[a][k];
        }
    std::copy(v.position, v.position + 3, guide_view_ + 9);
    if (!guide_said_ && v.valid) {
        // the view's handedness against the world's: the screen's x against up across ahead
        const float* r = v.right;
        const float* u = v.up;
        const float* f = v.forward;
        float hand = r[0] * (u[1] * f[2] - u[2] * f[1]) + r[1] * (u[2] * f[0] - u[0] * f[2]) + r[2] * (u[0] * f[1] - u[1] * f[0]);
        LOGR("ray tracing: guides' normals in the world (right %.2f %.2f %.2f, %s-handed), the view's projection's axes "
             "%+.0f %+.0f %+.0f", double(r[0]), double(r[1]), double(r[2]), hand < 0 ? "right" : "left",
             double(flip[0]), double(flip[1]), double(flip[2]));
        guide_said_ = true;
    }
    Alloc cb = Upload(256, 256);
    std::memcpy(cb.cpu, p, sizeof(p));
    // the four, this frame's surfaces (the frame before's kept) and the eye's hits
    const uint32_t sn = SurfaceNow();
    for (uint32_t i = 0; i < kGuides; i++) Transition(guide_[i].Get(), guide_state_[i], D3D12_RESOURCE_STATE_RENDER_TARGET);
    Transition(guide_[sn].Get(), guide_state_[sn], D3D12_RESOURCE_STATE_RENDER_TARGET);
    Transition(guide_[kGuideHit].Get(), guide_state_[kGuideHit], D3D12_RESOURCE_STATE_RENDER_TARGET);
    rt::Scene::Out more[kGuides + 1];
    for (uint32_t i = 1; i < kGuides; i++) more[i - 1] = {Cpu(rtv_heap_.Get(), kGuideRtv + i, rtv_inc_), kFmt[i]};
    more[kGuides - 1] = {Cpu(rtv_heap_.Get(), kGuideRtv + sn, rtv_inc_), kFmt[sn]};
    more[kGuides] = {Cpu(rtv_heap_.Get(), kGuideRtv + kGuideHit, rtv_inc_), kFmt[kGuideHit]};
    bool drawn = rt_.Trace(rt::Scene::Pass::Guides, cb.gpu, Gpu(srv_heap_.Get(), slot, srv_inc_),
                           Gpu(srv_heap_.Get(), 0, srv_inc_), Cpu(rtv_heap_.Get(), kGuideRtv, rtv_inc_), kFmt[0], rw,
                           rh, more, kGuides + 1);
    const D3D12_RESOURCE_STATES kRead =
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    // the hit distance stays a target for the reflections (Reflections), the rest are read
    for (uint32_t i = 0; i + 1 < kGuides; i++) Transition(guide_[i].Get(), guide_state_[i], kRead);
    Transition(guide_[sn].Get(), guide_state_[sn], kRead);
    Transition(guide_[kGuideHit].Get(), guide_state_[kGuideHit], kRead);
    if (drawn) surface_frame_[presents_ & 1] = presents_;
    EndRtPass();
    if (drawn) guide_frame_ = presents_;
    return drawn;
}

// the bounce lighting's own denoiser's targets (Denoise), at the bounce target's size; new ones hold nothing to gather
bool D3D12Backend::EnsureDenoiseTargets(uint32_t w, uint32_t h) {
    if (dn_[0] && dn_w_ == w && dn_h_ == h) return true;
    for (uint32_t i = 0; i < kDn; i++) {
        if (dn_[i]) Release(std::move(dn_[i]));
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC td = Texture2DDesc(DXGI_FORMAT_R16G16B16A16_FLOAT, w, h);
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_CLEAR_VALUE cv{};
        cv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        dn_state_[i] = D3D12_RESOURCE_STATE_RENDER_TARGET;
        if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, dn_state_[i], &cv,
                                                 IID_PPV_ARGS(&dn_[i])))) {
            for (auto& d : dn_) d = nullptr;
            dn_w_ = dn_h_ = 0;
            return false;
        }
        dev_->CreateRenderTargetView(dn_[i].Get(), nullptr, Cpu(rtv_heap_.Get(), kDnRtv + i, rtv_inc_));
    }
    dn_w_ = w;
    dn_h_ = h;
    dn_frame_ = UINT64_MAX;  // nothing gathered yet
    return true;
}

// the reflections' and ambient occlusion's own denoiser's targets (rd_), at the frame target's size; new ones hold
// nothing to gather
bool D3D12Backend::EnsureOwnDenoiseTargets(uint32_t w, uint32_t h) {
    if (rd_[0] && rd_w_ == w && rd_h_ == h) return true;
    static const DXGI_FORMAT kFmt[kRd] = {DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT,
                                          DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16_FLOAT,
                                          DXGI_FORMAT_R16_FLOAT,          DXGI_FORMAT_R16G16B16A16_FLOAT,
                                          DXGI_FORMAT_R16G16_FLOAT,       DXGI_FORMAT_R16G16_FLOAT};
    for (uint32_t i = 0; i < kRd; i++) {
        if (rd_[i]) Release(std::move(rd_[i]));
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC td = Texture2DDesc(kFmt[i], w, h);
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_CLEAR_VALUE cv{};
        cv.Format = kFmt[i];
        rd_state_[i] = D3D12_RESOURCE_STATE_RENDER_TARGET;
        if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, rd_state_[i], &cv,
                                                 IID_PPV_ARGS(&rd_[i])))) {
            for (auto& d : rd_) d = nullptr;
            rd_w_ = rd_h_ = 0;
            return false;
        }
        dev_->CreateRenderTargetView(rd_[i].Get(), nullptr, Cpu(rtv_heap_.Get(), kRdRtv + i, rtv_inc_));
    }
    rd_w_ = w;
    rd_h_ = h;
    refl_frame_ = aodn_frame_ = UINT64_MAX;
    return true;
}

// The bounce lighting's own denoiser, for cards whose anti-aliasing does not denoise (AMD FSR; DLSS without Ray
// Reconstruction): this frame's bounce target (rt_gi.hlsl) gathered into what each surface held over the frames before
// (rt_gitemporal.hlsl), then smoothed across its neighbours on the same surface by how much it still varies
// (rt_giatrous.hlsl, XBE_RT_DN_PASSES passes, 5: taps 1, 2, 4, 8 and 16 texels apart). Lighting's SRVs from slot (the
// depth, the bounce target, the guides, the motion, then dn_'s); the SRV heap's index of the light to throw back
uint32_t D3D12Backend::Denoise(uint32_t slot, uint32_t tw, uint32_t th, uint32_t gw, uint32_t gh, uint32_t rw,
                                uint32_t rh) {
    static const int kView = std::getenv("XBE_RT_VIEW") ? std::atoi(std::getenv("XBE_RT_VIEW")) : 0;
    auto env = [](const char* n, float d) { const char* e = std::getenv(n); return e ? float(std::atof(e)) : d; };
    // (32 frames, 5 passes: 1 to 16 texels apart; at 24 and 4 the bounce lighting's low noise stayed as blotches the
    // size of a floor tile, which the stronger lighting showed)
    static const float kFrames = std::clamp(env("XBE_RT_DN_FRAMES", 32), 1.0f, 255.0f),
                       kPasses = std::clamp(env("XBE_RT_DN_PASSES", 5), 0.0f, 6.0f),
                       kSigma = env("XBE_RT_DN_SIGMA", 4), kDepth = env("XBE_RT_DN_DEPTH", 0.05f),
                       kNormal = env("XBE_RT_DN_NORMAL", 0.8f), kSmoothDepth = env("XBE_RT_DN_SMOOTH_DEPTH", 0.02f),
                       kSmoothNormal = env("XBE_RT_DN_SMOOTH_NORMAL", 64);
    const D3D12_RESOURCE_STATES kRead =
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    const uint32_t dn = slot + 5;  // dn_[0]'s SRV
    const DXGI_FORMAT f16 = DXGI_FORMAT_R16G16B16A16_FLOAT;
    // gathered: this frame's into the pair not written last, from the one that was (nothing to gather from after a
    // cut, a frame it did not run, or new targets)
    uint32_t cur = dn_cur_ ^ 1, prev = dn_cur_;
    bool reset = rt_reset_ || dn_frame_ == UINT64_MAX || dn_frame_ + 1 != presents_;
    float tan_y = std::tan(0.5f * ao_view_.fov_y);
    float tp[36] = {ao_view_.near_w, ao_view_.far_w, tan_y * float(rw) / float(rh), tan_y,
                    float(rw), float(rh), float(gw), float(gh),
                    float(slot + 1), float(dn + prev), float(dn + 2 + prev), float(dn + 4 + prev),
                    float(slot + 4), float(slot + 3), float(slot), kFrames};
    std::memcpy(tp + 16, rt_clip_to_prev_, sizeof(rt_clip_to_prev_));
    tp[32] = reset ? 1.0f : 0.0f;
    tp[33] = kDepth;
    tp[34] = kNormal;
    tp[35] = float(kView);
    Alloc tcb = Upload(256, 256);
    std::memcpy(tcb.cpu, tp, sizeof(tp));
    for (uint32_t i : {cur, 2 + cur, 4 + cur}) Transition(dn_[i].Get(), dn_state_[i], D3D12_RESOURCE_STATE_RENDER_TARGET);
    for (uint32_t i : {prev, 2 + prev, 4 + prev}) Transition(dn_[i].Get(), dn_state_[i], kRead);
    rt::Scene::Out more[2] = {{Cpu(rtv_heap_.Get(), kDnRtv + 2 + cur, rtv_inc_), f16},
                              {Cpu(rtv_heap_.Get(), kDnRtv + 4 + cur, rtv_inc_), f16}};
    if (!rt_.Trace(rt::Scene::Pass::BounceGather, tcb.gpu, Gpu(srv_heap_.Get(), slot, srv_inc_),
                   Gpu(srv_heap_.Get(), 0, srv_inc_), Cpu(rtv_heap_.Get(), kDnRtv + cur, rtv_inc_), f16, gw, gh, more, 2))
        return slot + 1;
    for (uint32_t i : {cur, 2 + cur, 4 + cur}) Transition(dn_[i].Get(), dn_state_[i], kRead);
    dn_cur_ = cur;
    dn_frame_ = presents_;
    // smoothed: the history and its moments first, then each pass's light and variance into the other of 6 and 7
    uint32_t light = dn + cur;
    int passes = int(kPasses);
    for (int p = 0; p < passes; p++) {
        uint32_t out = 6 + (p & 1);
        float sp[16] = {ao_view_.near_w, ao_view_.far_w, tan_y * float(rw) / float(rh), tan_y,
                        float(rw), float(rh), float(gw), float(gh),
                        float(light), float(dn + 2 + cur), float(dn + 4 + cur), float(1 << p),
                        p == 0 ? 1.0f : 0.0f, kSigma, kSmoothDepth, kSmoothNormal};
        Alloc scb = Upload(256, 256);
        std::memcpy(scb.cpu, sp, sizeof(sp));
        Transition(dn_[out].Get(), dn_state_[out], D3D12_RESOURCE_STATE_RENDER_TARGET);
        if (!rt_.Trace(rt::Scene::Pass::BounceSmooth, scb.gpu, Gpu(srv_heap_.Get(), slot, srv_inc_),
                       Gpu(srv_heap_.Get(), 0, srv_inc_), Cpu(rtv_heap_.Get(), kDnRtv + out, rtv_inc_), f16, gw, gh))
            break;
        Transition(dn_[out].Get(), dn_state_[out], kRead);
        light = dn + out;
    }
    if (!dn_said_) {
        LOGR("ray tracing: bounce lighting denoised here (no denoiser in the anti-aliasing): gathered over up to %.0f "
             "frames at %ux%u, smoothed %d times", double(kFrames), gw, gh, passes);
        dn_said_ = true;
    }
    (void)tw;
    (void)th;
    return light;
}

// the lights' points kept for each bounce texel, this frame's and the frame before's (rt_gi.hlsl's second target), for a
// bounce target of tw x th: false when they could not be made
bool D3D12Backend::EnsureReservoirs(uint32_t tw, uint32_t th) {
    if (rs_[0] && rs_[1] && rs_w_ == tw && rs_h_ == th) return true;
    const DXGI_FORMAT f = DXGI_FORMAT_R32G32B32A32_FLOAT;
    for (uint32_t i = 0; i < kRs; i++) {
        if (rs_[i]) Release(std::move(rs_[i]));
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC td = Texture2DDesc(f, tw, th);
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_CLEAR_VALUE cv{};
        cv.Format = f;
        rs_state_[i] = D3D12_RESOURCE_STATE_RENDER_TARGET;
        if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, rs_state_[i], &cv,
                                                 IID_PPV_ARGS(&rs_[i])))) {
            for (auto& r : rs_) r = nullptr;
            rs_w_ = rs_h_ = 0;
            return false;
        }
        dev_->CreateRenderTargetView(rs_[i].Get(), nullptr, Cpu(rtv_heap_.Get(), kRsRtv + i, rtv_inc_));
    }
    rs_w_ = tw;
    rs_h_ = th;
    rs_frame_ = UINT64_MAX;
    return true;
}

// the glows remembered off screen's targets (rt_glowmem.hlsl's): false when they could not be made
bool D3D12Backend::EnsureGlowTargets() {
    if (glow_[0] && glow_[1] && glow_[2] && glow_[3] && glow_[4]) return true;
    const DXGI_FORMAT f = DXGI_FORMAT_R32G32B32A32_FLOAT;
    for (uint32_t i = 0; i < kGlow; i++) {
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC td = i == 0 || i == 4 ? Texture2DDesc(f, kMaxLights, 3) : Texture2DDesc(f, kGlowSlots, 5);
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_CLEAR_VALUE cv{};
        cv.Format = f;
        glow_state_[i] = D3D12_RESOURCE_STATE_RENDER_TARGET;
        if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, glow_state_[i], &cv,
                                                 IID_PPV_ARGS(&glow_[i])))) {
            for (auto& g : glow_) g = nullptr;
            return false;
        }
        dev_->CreateRenderTargetView(glow_[i].Get(), nullptr, Cpu(rtv_heap_.Get(), kGlowRtv + i, rtv_inc_));
    }
    glow_frame_ = UINT64_MAX;
    return true;
}

// The glows known from frame to frame (rt_glowmem.hlsl), for the passes that sample the scene's lights (Lighting,
// AmbientOcclusion's lamp shadows): each listed light told by where it is in slots moved with the camera, for its
// streak (the frames in a row it has been there: a light's light grows in from XBE_RT_LIGHTS_STEADY frames over
// XBE_RT_LIGHTS_RAMP more, so a sword's swing, a hit's sparks, a spell's burst throw none and a torch all of it), for
// its place in the frame before's list (the bounce lighting's reservoirs follow a light along the list as others come
// and go), and for a memory of it off screen for XBE_RT_GLOW_MEMORY seconds (20; 0: none at all): a torch just out
// of view goes on lighting the wall in view beside it. Once a frame, before the passes: glow_summary_srv_ and
// glow_slots_srv_ this frame's (-1 for none)
void D3D12Backend::GlowMemory(uint32_t rw, uint32_t rh) {
    if (glow_done_frame_ == presents_) return;
    glow_done_frame_ = presents_;
    glow_summary_srv_ = glow_slots_srv_ = -1.0f;
    auto env = [](const char* n, float d) { const char* e = std::getenv(n); return e ? float(std::atof(e)) : d; };
    // XBE_RT_GLOW_MATCH: how near a light drawn is to a slot's glow to be it, a part of its size's (the square root
    // of its area: 1.5) and at least 150; XBE_RT_GLOW_SCREEN: the part of the screen's middle where a glow not drawn is
    // gone rather than out of view (0.9: one at the edge may be culled before it leaves)
    static const float kSeconds = env("XBE_RT_GLOW_MEMORY", 20.0f), kMatch = env("XBE_RT_GLOW_MATCH", 1.5f),
                       kScreen = env("XBE_RT_GLOW_SCREEN", 0.9f);
    if (kSeconds <= 0 || !EnsureGlowTargets()) return;
    const uint32_t cur = 1 + uint32_t(presents_ & 1), prev = 1 + uint32_t((presents_ & 1) ^ 1), pass1 = 3;
    const uint32_t sum_cur = (presents_ & 1) ? 4 : 0, sum_prev = (presents_ & 1) ? 0 : 4;
    const bool before = !rt_reset_ && glow_frame_ != UINT64_MAX && glow_frame_ + 1 == presents_;
    // the camera's move the other way: the frame before's clip space to this frame's
    double m[16], inv[16];
    for (int i = 0; i < 16; i++) m[i] = rt_clip_to_prev_[i];
    float pc[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    const bool moved = Invert4(m, inv);
    if (moved)
        for (int i = 0; i < 16; i++) pc[i] = float(inv[i]);
    // the depth (the pass's table), then the summary, the slots the frame before, this frame's, the first pass's,
    // and the frame before's summary
    const uint32_t slot = AllocSrvSlots(6), first = slot + 1;
    TextureSrv(aa_depth_.Get(), DXGI_FORMAT_R32_FLOAT, slot);
    TextureSrv(glow_[sum_cur].Get(), DXGI_FORMAT_R32G32B32A32_FLOAT, first);
    TextureSrv(glow_[prev].Get(), DXGI_FORMAT_R32G32B32A32_FLOAT, first + 1);
    TextureSrv(glow_[cur].Get(), DXGI_FORMAT_R32G32B32A32_FLOAT, first + 2);
    TextureSrv(glow_[pass1].Get(), DXGI_FORMAT_R32G32B32A32_FLOAT, first + 3);
    TextureSrv(glow_[sum_prev].Get(), DXGI_FORMAT_R32G32B32A32_FLOAT, first + 4);
    struct GlowConstants {
        float p[8];
        float lt[4];
        float sk[4];
        float gm[4];
        float gm2[4];
        float pc[16];
        uint32_t list[kMaxLights][4];
    } gc{};
    float tan_y = std::tan(0.5f * ao_view_.fov_y);
    float p[8] = {ao_view_.near_w, ao_view_.far_w, tan_y * float(rw) / float(rh), tan_y,
                  float(rw), float(rh), float(SteadyFrames()), float(RampFrames())};
    std::memcpy(gc.p, p, sizeof(p));
    LightList(gc.lt, gc.sk, gc.list);
    gc.sk[0] = before && moved ? float(first + 4) : -1.0f;
    gc.gm[1] = float(first);
    gc.gm[2] = before && moved ? float(first + 1) : -1.0f;
    gc.gm[3] = std::max(1.0f, kSeconds * 60.0f);
    gc.gm2[0] = kMatch;
    gc.gm2[1] = 150.0f;
    gc.gm2[2] = kScreen;
    gc.gm2[3] = float(kGlowSlots);
    std::memcpy(gc.pc, pc, sizeof(pc));
    const D3D12_RESOURCE_STATES kRead =
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    const DXGI_FORMAT f = DXGI_FORMAT_R32G32B32A32_FLOAT;
    // the summary, then the slots' first pass from it and the frame before's, then the slots from that
    Alloc sum = Upload(sizeof(GlowConstants), 256);
    std::memcpy(sum.cpu, &gc, sizeof(gc));
    gc.gm[0] = 1;
    Alloc one = Upload(sizeof(GlowConstants), 256);
    std::memcpy(one.cpu, &gc, sizeof(gc));
    gc.gm[0] = 2;
    gc.gm[2] = float(first + 3);
    Alloc two = Upload(sizeof(GlowConstants), 256);
    std::memcpy(two.cpu, &gc, sizeof(gc));
    Transition(glow_[sum_cur].Get(), glow_state_[sum_cur], D3D12_RESOURCE_STATE_RENDER_TARGET);
    Transition(glow_[sum_prev].Get(), glow_state_[sum_prev], kRead);
    Transition(glow_[prev].Get(), glow_state_[prev], kRead);
    bool drawn = rt_.Trace(rt::Scene::Pass::GlowMemory, sum.gpu, Gpu(srv_heap_.Get(), slot, srv_inc_),
                           Gpu(srv_heap_.Get(), 0, srv_inc_), Cpu(rtv_heap_.Get(), kGlowRtv + sum_cur, rtv_inc_), f,
                           kMaxLights, 3);
    Transition(glow_[sum_cur].Get(), glow_state_[sum_cur], kRead);
    Transition(glow_[pass1].Get(), glow_state_[pass1], D3D12_RESOURCE_STATE_RENDER_TARGET);
    drawn = drawn && rt_.Trace(rt::Scene::Pass::GlowMemory, one.gpu, Gpu(srv_heap_.Get(), slot, srv_inc_),
                               Gpu(srv_heap_.Get(), 0, srv_inc_), Cpu(rtv_heap_.Get(), kGlowRtv + pass1, rtv_inc_), f,
                               kGlowSlots, 5);
    Transition(glow_[pass1].Get(), glow_state_[pass1], kRead);
    Transition(glow_[cur].Get(), glow_state_[cur], D3D12_RESOURCE_STATE_RENDER_TARGET);
    drawn = drawn && rt_.Trace(rt::Scene::Pass::GlowMemory, two.gpu, Gpu(srv_heap_.Get(), slot, srv_inc_),
                               Gpu(srv_heap_.Get(), 0, srv_inc_), Cpu(rtv_heap_.Get(), kGlowRtv + cur, rtv_inc_), f,
                               kGlowSlots, 5);
    Transition(glow_[cur].Get(), glow_state_[cur], kRead);
    glow_frame_ = drawn ? presents_ : UINT64_MAX;
    if (drawn && !glow_said_) {
        LOGR("ray tracing: the glows known frame to frame, %u slots: a light from %u frames in a row, grown in over %u "
             "more, remembered off screen for up to %.0f s",
             kGlowSlots, SteadyFrames(), RampFrames(), double(kSeconds));
        glow_said_ = true;
    }
    if (drawn) {
        glow_summary_srv_ = float(first);
        glow_slots_srv_ = float(first + 2);
    }
}

// the room's average's targets (RoomBounce's levels, each 1/8 of the one before, and its eased pair) and the sky's
// (SkyLight's), for a bounce target of tw x th: false when they could not be made
bool D3D12Backend::EnsureMeanTargets(uint32_t tw, uint32_t th) {
    auto down = [](uint32_t v) { return std::max(1u, (v + 7) / 8); };
    if (!mean_[0] || mean_for_w_ != tw || mean_for_h_ != th) {
        uint32_t w = tw, h = th;
        for (uint32_t i = 0; i < kMean; i++) {
            if (i < 4) {
                w = down(w);
                h = down(h);
            }
            uint32_t mw = i < 4 ? w : 1, mh = i < 4 ? h : 1;
            if (mean_[i]) Release(std::move(mean_[i]));
            D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
            // the eased pair at full precision: a frame's share of it is as small as a part in a thousand or so
            const DXGI_FORMAT mf = i == 4 || i == 5 ? DXGI_FORMAT_R32G32B32A32_FLOAT : DXGI_FORMAT_R16G16B16A16_FLOAT;
            D3D12_RESOURCE_DESC td = Texture2DDesc(mf, mw, mh);
            td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            D3D12_CLEAR_VALUE cv{};
            cv.Format = mf;
            mean_state_[i] = D3D12_RESOURCE_STATE_RENDER_TARGET;
            if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, mean_state_[i], &cv,
                                                     IID_PPV_ARGS(&mean_[i])))) {
                for (auto& m : mean_) m = nullptr;
                mean_for_w_ = mean_for_h_ = 0;
                return false;
            }
            dev_->CreateRenderTargetView(mean_[i].Get(), nullptr, Cpu(rtv_heap_.Get(), kMeanRtv + i, rtv_inc_));
        }
        mean_for_w_ = tw;
        mean_for_h_ = th;
        mean_frame_ = UINT64_MAX;
        sky_hold_frame_ = UINT64_MAX;
    }
    return true;
}

// The sky's colour as the frame shows it (rt_gimean.hlsl, where the frame drew nothing) and the share of the frame it
// covers, for the bounce lighting's rays that leave the scene upward (rt_gi.hlsl): its SRV index, -1 for none.
// XBE_RT_SKY its strength (0.4, 0: none)
float D3D12Backend::SkyLight(Target& t, uint32_t rw, uint32_t rh, uint32_t tw, uint32_t th) {
    auto env = [](const char* n, float d) { const char* e = std::getenv(n); return e ? float(std::atof(e)) : d; };
    static const float kSky = env("XBE_RT_SKY", 0.4f);
    if (sky_frame_ == presents_) return sky_slot_;
    sky_frame_ = presents_;
    sky_slot_ = -1;
    if (kSky <= 0 || !EnsureMeanTargets(tw, th)) return -1;
    const D3D12_RESOURCE_STATES kRead =
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    constexpr uint32_t kSkyMean = 6, kSkyHeld = 7;  // this frame's, then the held pair
    uint32_t slot = AllocSrvSlots(5);
    TextureSrv(t.res.Get(), t.fmt, slot);
    TextureSrv(aa_depth_.Get(), DXGI_FORMAT_R32_FLOAT, slot + 1);
    TextureSrv(mean_[kSkyMean].Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, slot + 2);
    TextureSrv(mean_[kSkyHeld].Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, slot + 3);
    TextureSrv(mean_[kSkyHeld + 1].Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, slot + 4);
    float p[8] = {float(slot), float(rw), float(rh), 3, float(slot + 1), ao_view_.near_w, ao_view_.far_w, 0};
    Alloc cb = Upload(256, 256);
    std::memcpy(cb.cpu, p, sizeof(p));
    Transition(t.res.Get(), t.state, kRead);
    Transition(mean_[kSkyMean].Get(), mean_state_[kSkyMean], D3D12_RESOURCE_STATE_RENDER_TARGET);
    bool drawn = rt_.Trace(rt::Scene::Pass::BounceMean, cb.gpu, Gpu(srv_heap_.Get(), slot, srv_inc_),
                           Gpu(srv_heap_.Get(), 0, srv_inc_), Cpu(rtv_heap_.Get(), kMeanRtv + kSkyMean, rtv_inc_),
                           DXGI_FORMAT_R16G16B16A16_FLOAT, 1, 1);
    Transition(mean_[kSkyMean].Get(), mean_state_[kSkyMean], kRead);
    if (!sky_said_) {
        LOGR("ray tracing: the sky's light %s, strength %.2f", drawn ? "measured" : "not measured", double(kSky));
        sky_said_ = true;
    }
    // held over the frames since the cut (rt_gimean.hlsl, 4): the sky as the frame showed it rose and fell as the
    // camera tilted (more of it in view looking up, none looking down at Hector), and the whole outdoors' light with
    // it. Its share of the frame rises toward the most seen (over a third of a second) and does not fall, and a
    // twentieth of the frame (XBE_RT_SKY_SHARE) is the whole sky's light: on the bridge before the courtyard the light
    // still rose the first time the camera tilted down to the sky. Its colour eases toward the frame's over about two
    // seconds while any is in view
    if (drawn) {
        static const float kRise = 1.0f / std::max(1.0f, env("XBE_RT_SKY_RISE", 20.0f)),
                           kHue = 1.0f / std::max(1.0f, env("XBE_RT_SKY_EASE", 120.0f));
        bool reset = rt_reset_ || sky_hold_frame_ == UINT64_MAX || sky_hold_frame_ + 30 < presents_;
        uint32_t cur = kSkyHeld + (sky_cur_ ^ 1), prev = kSkyHeld + sky_cur_;
        float h[8] = {float(slot + 2), 1, 1, 4, float(slot + 3 + (prev - kSkyHeld)), kHue, reset ? 1.0f : 0.0f, kRise};
        Alloc hcb = Upload(256, 256);
        std::memcpy(hcb.cpu, h, sizeof(h));
        Transition(mean_[prev].Get(), mean_state_[prev], kRead);
        Transition(mean_[cur].Get(), mean_state_[cur], D3D12_RESOURCE_STATE_RENDER_TARGET);
        drawn = rt_.Trace(rt::Scene::Pass::BounceMean, hcb.gpu, Gpu(srv_heap_.Get(), slot, srv_inc_),
                          Gpu(srv_heap_.Get(), 0, srv_inc_), Cpu(rtv_heap_.Get(), kMeanRtv + cur, rtv_inc_),
                          DXGI_FORMAT_R16G16B16A16_FLOAT, 1, 1);
        Transition(mean_[cur].Get(), mean_state_[cur], kRead);
        if (drawn) {
            sky_cur_ ^= 1;
            sky_hold_frame_ = presents_;
            sky_slot_ = float(slot + 3 + (cur - kSkyHeld));
            return sky_slot_;
        }
    }
    sky_slot_ = -1.0f;
    return sky_slot_;
}

// The bounce lighting's room average (rt_gimean.hlsl), which it takes the place of the game's flat fill by: the bounce
// target's (bounce_slot, gw x gh drawn of tw x th) averaged down four levels to one texel, then the mean of every
// frame's since the cut (the room's, whichever way the camera looked: eased toward this frame's alone, it rose and
// fell as the camera turned, and the room's light with it), the last XBE_RT_GI_HOLD frames' once there are more
// (1200, about 20 seconds). The held value's SRV slot, -1 when it could not be made
float D3D12Backend::RoomBounce(uint32_t bounce_slot, uint32_t gw, uint32_t gh, uint32_t tw, uint32_t th) {
    auto env = [](const char* n, float d) { const char* e = std::getenv(n); return e ? float(std::atof(e)) : d; };
    static const float kEase = 1.0f / std::clamp(env("XBE_RT_GI_HOLD", 1200.0f), 1.0f, 100000.0f);
    auto down = [](uint32_t v) { return std::max(1u, (v + 7) / 8); };
    if (!EnsureMeanTargets(tw, th)) return -1;
    const D3D12_RESOURCE_STATES kRead =
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    const DXGI_FORMAT f16 = DXGI_FORMAT_R16G16B16A16_FLOAT;
    uint32_t slot = AllocSrvSlots(kMean);
    const DXGI_FORMAT f32 = DXGI_FORMAT_R32G32B32A32_FLOAT;
    for (uint32_t i = 0; i < kMean; i++) TextureSrv(mean_[i].Get(), i == 4 || i == 5 ? f32 : f16, slot + i);
    uint32_t sw = gw, sh = gh;
    bool drawn = true;
    for (uint32_t i = 0; i < 4 && drawn; i++) {
        uint32_t dw = down(sw), dh = down(sh);
        float p[8] = {float(i == 0 ? bounce_slot : slot + i - 1), float(sw), float(sh), i == 0 ? 0.0f : 1.0f};
        Alloc cb = Upload(256, 256);
        std::memcpy(cb.cpu, p, sizeof(p));
        Transition(mean_[i].Get(), mean_state_[i], D3D12_RESOURCE_STATE_RENDER_TARGET);
        drawn = rt_.Trace(rt::Scene::Pass::BounceMean, cb.gpu, Gpu(srv_heap_.Get(), slot, srv_inc_),
                          Gpu(srv_heap_.Get(), 0, srv_inc_), Cpu(rtv_heap_.Get(), kMeanRtv + i, rtv_inc_), f16, dw, dh);
        Transition(mean_[i].Get(), mean_state_[i], kRead);
        sw = dw;
        sh = dh;
    }
    uint32_t cur = 4 + (mean_cur_ ^ 1), prev = 4 + mean_cur_;
    // afresh after a cut or a time without a view (a loading, a room's change); not after a frame without bounce
    // lighting alone (a menu over the room, the bounce off a while): the room's held as it was
    bool reset = rt_reset_ || mean_frame_ == UINT64_MAX;
    float e[8] = {float(slot + 3), 1, 1, 2, float(slot + prev), kEase, reset ? 1.0f : 0.0f, 0};
    Alloc ecb = Upload(256, 256);
    std::memcpy(ecb.cpu, e, sizeof(e));
    Transition(mean_[prev].Get(), mean_state_[prev], kRead);
    Transition(mean_[cur].Get(), mean_state_[cur], D3D12_RESOURCE_STATE_RENDER_TARGET);
    drawn = drawn && rt_.Trace(rt::Scene::Pass::BounceMean, ecb.gpu, Gpu(srv_heap_.Get(), slot, srv_inc_),
                               Gpu(srv_heap_.Get(), 0, srv_inc_), Cpu(rtv_heap_.Get(), kMeanRtv + cur, rtv_inc_), f32, 1, 1);
    Transition(mean_[cur].Get(), mean_state_[cur], kRead);
    if (!drawn) return -1;
    mean_cur_ ^= 1;
    mean_frame_ = presents_;
    return float(slot + cur);
}

// The scene's lights as a pass that samples them takes them (rt_lights.hlsli): lt (the lights listed, their brightness,
// the candidates a sample is picked from, the most light they throw on a surface), sk's z and w (the lights drawn
// flat's brightness, the most area a light's light is counted from) and the list, as Lighting gives them (its
// XBE_RT_LIGHTS_* settings). The lights listed
uint32_t D3D12Backend::LightList(float lt[4], float sk[4], uint32_t list[][4]) {
    auto env = [](const char* n, float d) { const char* e = std::getenv(n); return e ? float(std::atof(e)) : d; };
    static const float kLightGain = env("XBE_RT_LIGHTS_GAIN", 40.0f), kLightTriesSet = env("XBE_RT_LIGHTS_TRIES", 0),
                       kLightMost = env("XBE_RT_LIGHTS_MAX", 0.75f), kFlat = env("XBE_RT_LIGHTS_FLAT", 0.8f),
                       kLightArea = env("XBE_RT_LIGHTS_AREA", 30000.0f);
    const std::vector<uint32_t>& em = rt_.Emissive();
    const uint32_t lights = uint32_t(std::min<size_t>(em.size() / 2, kMaxLights));
    lt[0] = float(lights);
    lt[1] = kLightGain * g_rt_lighting_strength.load(std::memory_order_relaxed);
    lt[2] = std::max(1.0f, kLightTriesSet > 0 ? kLightTriesSet : float(4 + 2 * RtQuality()));
    lt[3] = kLightMost;
    sk[2] = kFlat;
    sk[3] = kLightArea;
    const uint32_t flame = FlameArea(), reach = LightReach();
    for (uint32_t i = 0; i < kMaxLights; i++) {
        list[i][0] = i < lights ? em[i * 2] : 0;
        list[i][1] = i < lights ? em[i * 2 + 1] : 0;
        list[i][2] = flame;
        list[i][3] = reach;
    }
    return lights;
}

// Ray traced bounce lighting over the frame's 3D (rt_gi.hlsl, rt_giapply.hlsl): the light its surfaces throw onto each
// other, gathered by a few rays a pixel at a part of its size (XBE_RT_GI_SCALE, half), smoothed, thrown back in each
// surface's colour at the full size (Guides) and added in before the anti-aliasing gathers it over the frames
void D3D12Backend::Lighting(Target& t, uint32_t rw, uint32_t rh) {
    if (!(g_ray_tracing.load(std::memory_order_relaxed) & kRtLighting) || !BuildRtScene(rw, rh) || !Guides(t, rw, rh))
        return;
    static const int kView = std::getenv("XBE_RT_VIEW") ? std::atoi(std::getenv("XBE_RT_VIEW")) : 0;
    auto env = [](const char* n, float d) { const char* e = std::getenv(n); return e ? float(std::atof(e)) : d; };
    // the quality's (RtQuality), unless the environment says: low one ray a pixel at a third of the frame's size,
    // reaching 1500; medium one at half, 2000; high four at half, 2500 (two left blotches crawling on 5.9's floor,
    // twice the flicker of ambient occlusion alone; four, as little as it)
    const uint32_t q = RtQuality();
    static const float kScaleSet = env("XBE_RT_GI_SCALE", 0), kRaysSet = env("XBE_RT_GI_RAYS", 0),
                       kReachSet = env("XBE_RT_GI_REACH", 0);
    const float kScale = kScaleSet > 0 ? std::clamp(kScaleSet, 0.25f, 1.0f) : q == 0 ? 0.34f : 0.5f;
    const float kRaysFull = kRaysSet > 0 ? kRaysSet : q == 2 ? 4.0f : 1.0f;
    const float kRays = RtBudgetRays(kRaysFull);
    const float kReach = kReachSet > 0 ? kReachSet : q == 0 ? 1500.0f : q == 1 ? 2000.0f : 2500.0f;
    // XBE_RT_GI_FADE: the distance the bounce lighting has faded out by, from half of it (10000: a room's far wall
    // keeps it whole; at 20000 the sky cards past 5.9's rampart, some 15000 off, took the light the rays brought back
    // from the rampart and its lamp, a green and orange smear across the sky)
    static const float kFade = env("XBE_RT_GI_FADE", 10000.0f), kStrength = env("XBE_RT_GI_STRENGTH", 0),
                       kStep = env("XBE_RT_GI_STEP", 1), kBias = env("XBE_RT_AO_BIAS", 1.0f);
    uint32_t tw = std::max(1u, uint32_t(std::ceil(float(t.w) * kScale)));
    uint32_t th = std::max(1u, uint32_t(std::ceil(float(t.h) * kScale)));
    if (!gi_ || gi_w_ != tw || gi_h_ != th) {
        if (gi_) Release(std::move(gi_));
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC td = Texture2DDesc(DXGI_FORMAT_R16G16B16A16_FLOAT, tw, th);
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_CLEAR_VALUE cv{};
        cv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        gi_state_ = D3D12_RESOURCE_STATE_RENDER_TARGET;
        if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, gi_state_, &cv, IID_PPV_ARGS(&gi_)))) {
            gi_w_ = gi_h_ = 0;
            return;
        }
        dev_->CreateRenderTargetView(gi_.Get(), nullptr, Cpu(rtv_heap_.Get(), kGiRtv, rtv_inc_));
        gi_w_ = tw;
        gi_h_ = th;
    }
    uint32_t gw = std::max(1u, uint32_t(float(rw) * kScale)), gh = std::max(1u, uint32_t(float(rh) * kScale));
    // the depth (the bounce pass's table, and every pass's by its index), the bounce target, the guides (the
    // smoothing's), the motion and the denoiser's targets (Denoise's)
    // (and after them, the lights' points kept the frame before, rt_gi.hlsl's, three once the glows remembered's, the
    // anti-aliasing's reactive mask, rt_giapply.hlsl's, and the eye's hits, the guides')
    uint32_t slot = AllocSrvSlots(11 + kDn);
    // the normals the smoothing and the denoiser tell surfaces apart by are the faces' (this frame's surfaces,
    // rt_guide.hlsl's fifth target), not the guides' bent by a texture pack's normal map: across a flat wall the bent
    // ones swing 10 to 30 degrees texel to texel, and at the smoothing's sharpness (64) that left the wall's noise alone
    ID3D12Resource* src[5 + kDn] = {aa_depth_.Get(), gi_.Get(), guide_[0].Get(), guide_[SurfaceNow()].Get(),
                                    aa_motion_.Get()};
    DXGI_FORMAT fmt[5 + kDn] = {DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT, kGuideFmt[0],
                                kGuideFmt[SurfaceNow()], DXGI_FORMAT_R16G16_FLOAT};
    // the bounce lighting denoised here, with DLSS Ray Reconstruction too unless XBE_RT_GI_DENOISE=0 (the lights'
    // samples left to it alone flickered on the walls they lit, a frame's grain too coarse for it to settle)
    static const bool kGiDenoise = env("XBE_RT_GI_DENOISE", 1) != 0;
    bool denoise = OwnDenoiser() || kGiDenoise;
    if (denoise && !EnsureDenoiseTargets(tw, th)) denoise = false;
    for (uint32_t i = 0; i < kDn; i++) {
        src[5 + i] = denoise ? dn_[i].Get() : nullptr;
        fmt[5 + i] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    }
    for (uint32_t i = 0; i < 5 + kDn; i++) {
        D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = fmt[i];
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.Texture2D.MipLevels = 1;
        dev_->CreateShaderResourceView(src[i], &sv, Cpu(srv_heap_.Get(), slot + i, srv_inc_));
    }
    float tan_y = std::tan(0.5f * ao_view_.fov_y);
    // 100% is 4 times the light the surfaces throw as their pictures and vertex colours give it: this title's colours
    // are dark (its lighting is in a narrow range the console brightened), and at 4 the courtyard's water lights the
    // carved wall above it as it would
    float strength = kStrength > 0 ? kStrength : 4.0f * g_rt_lighting_strength.load(std::memory_order_relaxed);
    // the scene's lights (its glows, flames, spells: Scene::Emissive), up to 128 of them in the order drawn, each
    // known from frame to frame by where it is (GlowMemory); XBE_RT_LIGHTS_GAIN their brightness (40: a flame's
    // picture is as bright as a picture gets, the fire it stands for many times that; at 24 a torch lit little past
    // its own bracket), XBE_RT_LIGHTS_TRIES the candidates a sample is picked from (the quality's: 4, 6, 8)
    static const float kLightTriesSet = env("XBE_RT_LIGHTS_TRIES", 0);
    const std::vector<uint32_t>& em = rt_.Emissive();
    struct GiConstants {
        float p[16];
        float lt[4];
        float sky[4];
        float sk[4];
        float gl[4];
        float rs[4];
        float pv[16];
        float gm[4];
        uint32_t list[kMaxLights][4];
    } gc{};
    // the sky's light (SkyLight), outdoors
    WorldUpInView(ao_view_, gc.sky);
    gc.sky[3] = SkyLight(t, rw, rh, tw, th);
    static const float kSkyStrength = env("XBE_RT_SKY", 0.4f), kSkyShare = env("XBE_RT_SKY_SHARE", 0.05f);
    gc.sk[0] = kSkyStrength;
    gc.sk[1] = kSkyShare;
    // the lights drawn flat (a torch's flame, a glow's sprite, motes in the air): 0.8 as bright as the scene's own
    // (XBE_RT_LIGHTS_FLAT; a steady one only, none bigger than a flame, so a cloud of motes tints rather than floods)
    // XBE_RT_LIGHTS_AREA: the most area of a light whose light counts (30000, a torch's flame, two triangles of some
    // 15000 each; 4000 left a torch a sixth of its light; 0: any), so a glowing sheet, the save room marker under
    // Hector, mist, a beam, is lit as a torch at most
    // (lt, sk's z and w and the list: LightList's)
    const uint32_t lights = LightList(gc.lt, gc.sk, gc.list);
    // XBE_RT_GLOW_BOUNCE: the glows' light the bounce rays gather as they pass through them (1: as the eye sees them;
    // the save room's water's glow on the stone around it, all of it, not the few draws of it that are listed)
    // XBE_RT_GLOW_SKIP: from how far off a surface they count (40 units: a glow laid over the floor, the marker that
    // points to a save room, is the floor's look, not light thrown on it)
    static const float kGlowBounce = env("XBE_RT_GLOW_BOUNCE", 1.0f), kGlowSkip = env("XBE_RT_GLOW_SKIP", 40.0f);
    gc.gl[0] = kGlowBounce;
    gc.gl[1] = kGlowSkip;
    // XBE_RT_GI_BOUNCE2: the lights' light bounced a second time, off what the bounce rays meet (1, 0: none), each
    // ray's sample picked of XBE_RT_GI_BOUNCE2_TRIES candidates (2)
    static const float kBounce2 = env("XBE_RT_GI_BOUNCE2", 1.0f), kBounce2Tries = env("XBE_RT_GI_BOUNCE2_TRIES", 2.0f);
    gc.gl[2] = kBounce2;
    gc.gl[3] = std::max(1.0f, kBounce2Tries);
    // XBE_RT_LIGHTS_REUSE: each bounce texel's lights' point kept from frame to frame and resampled with the next
    // frame's candidates (ReSTIR's reuse over time), counting for at most this many frames' candidates (6, the most the
    // kept point's 6 bits hold with High's 8 candidates; 0: each frame's afresh). A torch found once is kept while it
    // is strong there: the grain of its light on a wall settles rather than crawls, as if it were picked of several
    // times the candidates
    // XBE_RT_LIGHTS_REUSE_DEPTH: kept only where the surface was as deep as this one was, within this part (0.05)
    static const float kReuse = env("XBE_RT_LIGHTS_REUSE", 6.0f), kReuseDepth = env("XBE_RT_LIGHTS_REUSE_DEPTH", 0.05f);
    const uint32_t rs_cur = uint32_t(presents_ & 1), rs_prev = rs_cur ^ 1;
    const bool kept = EnsureReservoirs(tw, th);
    const bool reuse = kept && kReuse > 0 && !rt_reset_ && rs_frame_ != UINT64_MAX && rs_frame_ + 1 == presents_;
    const float tries = std::max(1.0f, kLightTriesSet > 0 ? kLightTriesSet : float(4 + 2 * q));
    gc.rs[0] = float(slot + 5 + kDn);
    gc.rs[1] = reuse ? 1.0f : 0.0f;
    gc.rs[2] = kReuse > 0 ? std::min(kReuse * tries, 63.0f - tries) : 0.0f;
    gc.rs[3] = kReuseDepth;
    std::memcpy(gc.pv, rt_clip_to_prev_, sizeof(rt_clip_to_prev_));
    if (kept) TextureSrv(rs_[rs_prev].Get(), DXGI_FORMAT_R32G32B32A32_FLOAT, slot + 5 + kDn);
    if (kept && !rs_said_) {
        if (kReuse > 0)
            LOGR("ray tracing: the lights' points kept from frame to frame, counting for up to %.0f candidates",
                 gc.rs[2]);
        rs_said_ = true;
    }
    float gp[16] = {ao_view_.near_w, ao_view_.far_w, tan_y * float(rw) / float(rh), tan_y,
                    float(rw), float(rh), float(gw), float(gh),
                    kReach, std::max(1.0f, kRays), strength, kFade,
                    float(presents_ % 65536), float(kView), 4.0f * kBias, 0.004f * kBias};
    std::memcpy(gc.p, gp, sizeof(gp));
    if (lights && !lights_said_) {
        LOGR("ray tracing: %zu lights in the scene (its glows, flames and spells), %u slots of them sampled a frame",
             em.size() / 2, lights);
        lights_said_ = true;
    }
    // the glows known frame to frame (GlowMemory, made before the passes): their slots (every one remembered off
    // screen a candidate for the lights' sample) and, two SRV slots before them, the summary (how much of each
    // listed light is there)
    GlowMemory(rw, rh);
    gc.gm[0] = glow_slots_srv_;
    gc.gm[1] = float(kGlowSlots);
    {
        const uint32_t reach = LightReach();  // the glows remembered's reach: the full one (rt_lights.hlsli's Remembered)
        std::memcpy(&gc.gm[3], &reach, sizeof(gc.gm[3]));
    }
    // the eye's hits (Guides, kGuideHit): the bounce pass takes each texel's surface from them, not from a ray of its own
    gc.gm[2] = -1.0f;
    if (EyeHits(t)) {
        TextureSrv(guide_[kGuideHit].Get(), kGuideFmt[kGuideHit], slot + 10 + kDn);
        gc.gm[2] = float(slot + 10 + kDn);
    }
    rs_gw_ = gw;
    rs_gh_ = gh;
    Alloc gcb = Upload(sizeof(GiConstants), 256);
    std::memcpy(gcb.cpu, &gc, sizeof(gc));
    const D3D12_RESOURCE_STATES kRead =
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    Transition(gi_.Get(), gi_state_, D3D12_RESOURCE_STATE_RENDER_TARGET);
    // its second target the lights' points kept here, for the frame after (the frame before's read)
    rt::Scene::Out keep{Cpu(rtv_heap_.Get(), kRsRtv + rs_cur, rtv_inc_), DXGI_FORMAT_R32G32B32A32_FLOAT};
    if (kept) {
        Transition(rs_[rs_prev].Get(), rs_state_[rs_prev], kRead);
        Transition(rs_[rs_cur].Get(), rs_state_[rs_cur], D3D12_RESOURCE_STATE_RENDER_TARGET);
    }
    bool drawn = rt_.Trace(rt::Scene::Pass::Bounce, gcb.gpu, Gpu(srv_heap_.Get(), slot, srv_inc_),
                           Gpu(srv_heap_.Get(), 0, srv_inc_), Cpu(rtv_heap_.Get(), kGiRtv, rtv_inc_),
                           DXGI_FORMAT_R16G16B16A16_FLOAT, gw, gh, kept ? &keep : nullptr, kept ? 1 : 0);
    if (kept) {
        Transition(rs_[rs_cur].Get(), rs_state_[rs_cur], kRead);
        rs_frame_ = drawn ? presents_ : UINT64_MAX;
    }
    Transition(gi_.Get(), gi_state_, kRead);
    // gathered over the frames and smoothed (Denoise), else as the rays found it
    uint32_t light = drawn && denoise && kView != 21 && kView != 22 ? Denoise(slot, tw, th, gw, gh, rw, rh) : slot + 1;
    // in the place of the game's flat fill (its room average, RoomBounce), unless XBE_RT_GI_NEUTRAL=0 (all of it added,
    // brightening the room): what falls short of the average takes out up to XBE_RT_GI_FILL of the frame's own (0.7),
    // as the strength (the Lighting strength setting) has it. At 0.3 the game's own flat fill stayed most of each
    // room's light, and ray traced lighting looked little different from ambient occlusion alone: a torch barely
    // warmed its wall, a corner far from any light kept the fill's brightness. At 0.7 the light falls off from the
    // lamps into the dark; the Lighting strength setting at 50% gives about the old look
    // XBE_RT_GI_BOOST: how much of the light above the room's average is added (1.5), the lit side of the contrast as
    // the fill is the dark side
    // XBE_RT_GI_CHAR_FILL: a character's share (0.6), as the strength has it; XBE_RT_GI_CHAR_BOOST: how much of what
    // it gets above the room's average is added to a character (0: none; the game lights Hector and the enemies with
    // lights of their own, and what the rays added stood them out brighter than the floor they stood on)
    static const float kNeutral = env("XBE_RT_GI_NEUTRAL", 1), kFill = env("XBE_RT_GI_FILL", 0.7f),
                       kBoost = env("XBE_RT_GI_BOOST", 1.5f), kCharFill = env("XBE_RT_GI_CHAR_FILL", 0.6f),
                       kCharBoost = env("XBE_RT_GI_CHAR_BOOST", 0.0f);
    float mean = drawn && kNeutral != 0 ? RoomBounce(slot + 1, gw, gh, tw, th) : -1.0f;
    float fill = std::clamp(kFill * g_rt_lighting_strength.load(std::memory_order_relaxed), 0.0f, 0.9f);
    float char_fill = std::clamp(kCharFill * g_rt_lighting_strength.load(std::memory_order_relaxed), 0.0f, 0.9f);
    // the frame's effects (the anti-aliasing's reactive mask: glows, beams, fire drawn into the frame before it), left
    // as the game drew them (rt_giapply.hlsl), unless XBE_RT_GI_EFFECTS=0
    static const bool kEffects = env("XBE_RT_GI_EFFECTS", 1) != 0;
    const bool effects = kEffects && aa_rx_ && aa_rx_frame_ == presents_ && aa_rx_w_ == t.w && aa_rx_h_ == t.h;
    if (effects) {
        Transition(aa_rx_.Get(), aa_rx_state_, kRead);
        TextureSrv(aa_rx_.Get(), DXGI_FORMAT_R8_UNORM, slot + 9 + kDn);
    }
    // XBE_RT_GI_LIFT_MOST: the most a surface's light is raised (2: its own and twice more, a torch's wall in a dark
    // room), eased toward from XBE_RT_GI_LIFT_KNEE (0.8) on; cut at 1 before, the walls by the purple lamps doubled to
    // a flat purple (the easing keeps their carving)
    static const float kLiftMost = env("XBE_RT_GI_LIFT_MOST", 2.0f), kLiftKnee = env("XBE_RT_GI_LIFT_KNEE", 0.8f);
    // XBE_RT_GI_HUE: how much of the light's own hue a surface it raises takes (1: all; 0: white, the surface's own
    // colour raised), the raise set by the light's brightness alone
    static const float kLiftHue = env("XBE_RT_GI_HUE", 1.0f);
    // XBE_RT_GI_MEAN_FLOOR: the least the room's average bounce is taken as for what raises a surface (0.15,
    // rt_giapply.hlsl), so a dark room's small glows and noise are not raised by how dark it is (at 0 an item's sparkle
    // on 5.9's night rampart lit a white pool across the floor; at 0.2 2.17's torches lit little)
    static const float kMeanFloor = env("XBE_RT_GI_MEAN_FLOOR", 0.15f);
    // XBE_RT_GI_HUE_MEAN: a lift's hue is the light's against the room's average's (1), not against white (0): the
    // room's own colour is in its fill already, and a surface that gets more of the room's light is raised in white
    // (rt_giapply.hlsl)
    static const float kHueMean = env("XBE_RT_GI_HUE_MEAN", 1.0f);
    float ap[28] = {ao_view_.near_w, ao_view_.far_w, tan_y * float(rw) / float(rh), tan_y,
                    float(rw), float(rh), float(gw), float(gh),
                    float(slot), float(light), std::max(1.0f, kStep), float(kView),
                    float(slot + 2), float(slot + 3), mean, fill,
                    kBoost, char_fill, kCharBoost, effects ? float(slot + 9 + kDn + 1) : 0.0f,
                    kLiftMost, kLiftKnee, kFade, kLiftHue,
                    kMeanFloor, kHueMean, gc.sky[3], 0};
    Alloc acb = Upload(256, 256);
    std::memcpy(acb.cpu, ap, sizeof(ap));
    Transition(t.res.Get(), t.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
    drawn = drawn && rt_.Trace(kView ? rt::Scene::Pass::BounceApplyView : rt::Scene::Pass::BounceApply, acb.gpu,
                               Gpu(srv_heap_.Get(), slot, srv_inc_),
                               Gpu(srv_heap_.Get(), 0, srv_inc_), Cpu(rtv_heap_.Get(), t.view, rtv_inc_), t.fmt, rw, rh);
    EndRtPass();
    if (!gi_said_) {
        LOGR("ray tracing: bounce lighting %s, %.0f rays a pixel at %ux%u for the 3D at %ux%u, reaching %.0f, strength "
             "%.2f", drawn ? "drawn" : "not drawn", double(std::max(1.0f, kRays)), gw, gh, rw, rh, double(kReach),
             double(strength));
        gi_said_ = true;
    }
}

// Ray traced reflections over the frame's 3D (rt_refl.hlsl): where a texture pack's material is smooth, what its
// mirror direction meets, before the anti-aliasing gathers them
void D3D12Backend::Reflections(Target& t, uint32_t rw, uint32_t rh) {
    if (!(g_ray_tracing.load(std::memory_order_relaxed) & kRtReflections) || !BuildRtScene(rw, rh)) return;
    static const int kView = std::getenv("XBE_RT_VIEW") ? std::atoi(std::getenv("XBE_RT_VIEW")) : 0;
    auto env = [](const char* n, float d) { const char* e = std::getenv(n); return e ? float(std::atof(e)) : d; };
    static const float kRough = env("XBE_RT_REFL_ROUGHNESS", 0.6f), kReach = env("XBE_RT_REFL_REACH", 3000.0f),
                       kBias = env("XBE_RT_AO_BIAS", 1.0f), kForce = env("XBE_RT_REFL_FORCE", 0);
    float tan_y = std::tan(0.5f * ao_view_.fov_y);
    // the quality's (RtQuality): low reflects the smoothest surfaces only, and not as far
    const uint32_t q = RtQuality();
    const float rough = q == 0 ? std::min(kRough, 0.35f) : q == 1 ? std::min(kRough, 0.5f) : kRough;
    const float reach = kReach * (q == 0 ? 0.6f : q == 1 ? 0.8f : 1.0f);
    // water drawn with the frame's 3D (Look::water): XBE_RT_WATER_ROUGHNESS (0.04, 0: none reflects),
    // XBE_RT_WATER_RIPPLE its waves' slope (0.15), XBE_RT_WATER_WAVE their length (40)
    static const float kWaterRough = env("XBE_RT_WATER_ROUGHNESS", 0.04f), kRipple = env("XBE_RT_WATER_RIPPLE", 0.15f),
                       kWave = env("XBE_RT_WATER_WAVE", 40.0f);
    const FrameView& v = ao_view_;
    float rp[40] = {ao_view_.near_w, ao_view_.far_w, tan_y * float(rw) / float(rh), tan_y,
                    float(rw), float(rh), float(rw), float(rh),
                    g_rt_reflection_strength.load(std::memory_order_relaxed), rough, reach, kForce,
                    float(presents_ % 65536), float(kView), 2.0f * kBias, 0.002f * kBias,
                    CharacterGlossOn(), 0, 0, 0,
                    kWaterRough, kRipple, kWave, float(presents_ % 36000) / 60.0f,
                    v.right[0], v.right[1], v.right[2], 0,
                    v.up[0], v.up[1], v.up[2], 0,
                    v.forward[0], v.forward[1], v.forward[2], 0,
                    v.position[0], v.position[1], v.position[2], 0};
    Alloc rcb = Upload(256, 256);
    std::memcpy(rcb.cpu, rp, sizeof(rp));
    uint32_t slot = AllocSrvSlots(1);
    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = DXGI_FORMAT_R32_FLOAT;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    dev_->CreateShaderResourceView(aa_depth_.Get(), &sv, Cpu(srv_heap_.Get(), slot, srv_inc_));
    // denoised here when no vendor's denoiser takes the frame, else drawn straight over it
    if (OwnDenoiser() && Guides(t, rw, rh) && EnsureOwnDenoiseTargets(t.w, t.h) &&
        DenoisedReflections(t, rw, rh, rcb.gpu, slot, rough)) {
        if (!refl_said_) {
            LOGR("ray tracing: reflections drawn and denoised over the 3D at %ux%u, strength %.2f, smooth below "
                 "roughness %.2f", rw, rh, double(rp[8]), double(rough));
            refl_said_ = true;
        }
        return;
    }
    Transition(t.res.Get(), t.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
    // each ray's length into the guides' hit distance, when they were drawn this frame (for the denoiser)
    bool hits = guide_frame_ == presents_ && guide_[kGuides - 1] && guide_w_ == t.w && guide_h_ == t.h;
    rt::Scene::Out hit{Cpu(rtv_heap_.Get(), kGuideRtv + kGuides - 1, rtv_inc_), kGuideFmt[kGuides - 1]};
    if (hits) Transition(guide_[kGuides - 1].Get(), guide_state_[kGuides - 1], D3D12_RESOURCE_STATE_RENDER_TARGET);
    bool drawn = rt_.Trace(rt::Scene::Pass::Reflections, rcb.gpu, Gpu(srv_heap_.Get(), slot, srv_inc_),
                           Gpu(srv_heap_.Get(), 0, srv_inc_), Cpu(rtv_heap_.Get(), t.view, rtv_inc_), t.fmt, rw, rh,
                           hits ? &hit : nullptr, hits ? 1 : 0);
    EndRtPass();
    if (!refl_said_) {
        LOGR("ray tracing: reflections %s over the 3D at %ux%u (its target %ux%u, format %d), strength %.2f, smooth "
             "below roughness %.2f", drawn ? "drawn" : "not drawn", rw, rh, t.w, t.h, int(t.fmt), double(rp[8]),
             double(rough));
        refl_said_ = true;
    }
}

// Ray traced reflections through the port's own denoiser, when no vendor's denoiser takes the frame: drawn into a target
// of their own (rd_ 0), gathered over the frames (rt_refltemporal.hlsl: XBE_RT_DN_REFL_FRAMES at most for the
// roughest, 12, held within this frame's neighbours' span, XBE_RT_DN_REFL_SPAN deviations, 1), then smoothed twice
// (rt_reflatrous.hlsl, taps 1 and 2 apart, as wide as XBE_RT_DN_REFL_SPREAD steps for the roughest, 1.5), the second
// pass drawn over the frame
bool D3D12Backend::DenoisedReflections(Target& t, uint32_t rw, uint32_t rh, D3D12_GPU_VIRTUAL_ADDRESS rcb,
                                       uint32_t depth_slot, float rough_limit) {
    static const int kView = std::getenv("XBE_RT_VIEW") ? std::atoi(std::getenv("XBE_RT_VIEW")) : 0;
    auto env = [](const char* n, float d) { const char* e = std::getenv(n); return e ? float(std::atof(e)) : d; };
    static const float kFrames = std::clamp(env("XBE_RT_DN_REFL_FRAMES", 12), 1.0f, 255.0f),
                       kSpan = env("XBE_RT_DN_REFL_SPAN", 1.0f), kSpread = env("XBE_RT_DN_REFL_SPREAD", 1.5f),
                       kDepth = env("XBE_RT_DN_DEPTH", 0.05f), kNormal = env("XBE_RT_DN_NORMAL", 0.8f),
                       kSmoothDepth = env("XBE_RT_DN_SMOOTH_DEPTH", 0.02f),
                       kSmoothNormal = env("XBE_RT_DN_SMOOTH_NORMAL", 64);
    const D3D12_RESOURCE_STATES kRead =
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    const DXGI_FORMAT f16 = DXGI_FORMAT_R16G16B16A16_FLOAT, r16 = DXGI_FORMAT_R16_FLOAT;
    // the reflections alone: what they add, premultiplied, and the share of the surface's own they take out
    Transition(rd_[0].Get(), rd_state_[0], D3D12_RESOURCE_STATE_RENDER_TARGET);
    if (!rt_.Trace(rt::Scene::Pass::ReflectionsAlone, rcb, Gpu(srv_heap_.Get(), depth_slot, srv_inc_),
                   Gpu(srv_heap_.Get(), 0, srv_inc_), Cpu(rtv_heap_.Get(), kRdRtv, rtv_inc_), f16, rw, rh)) {
        EndRtPass();
        return false;
    }
    Transition(rd_[0].Get(), rd_state_[0], kRead);
    // gathered: into the history and frames not written last, from those that were
    uint32_t cur = refl_cur_ ^ 1, prev = refl_cur_;
    bool reset = rt_reset_ || refl_frame_ == UINT64_MAX || refl_frame_ + 1 != presents_ || !HaveSurfaceBefore();
    // the reflections alone, the histories, the frames, the smoothing's, the surfaces now and before, the motion, the
    // depth, the guides' normals (roughness)
    uint32_t slot = AllocSrvSlots(11);
    TextureSrv(rd_[0].Get(), f16, slot);
    TextureSrv(rd_[1].Get(), f16, slot + 1);
    TextureSrv(rd_[2].Get(), f16, slot + 2);
    TextureSrv(rd_[3].Get(), r16, slot + 3);
    TextureSrv(rd_[4].Get(), r16, slot + 4);
    TextureSrv(rd_[5].Get(), f16, slot + 5);
    TextureSrv(guide_[SurfaceNow()].Get(), f16, slot + 6);
    TextureSrv(guide_[SurfaceBefore()].Get(), f16, slot + 7);
    TextureSrv(aa_motion_.Get(), DXGI_FORMAT_R16G16_FLOAT, slot + 8);
    TextureSrv(aa_depth_.Get(), DXGI_FORMAT_R32_FLOAT, slot + 9);
    TextureSrv(guide_[1].Get(), kGuideFmt[1], slot + 10);
    float tan_y = std::tan(0.5f * ao_view_.fov_y);
    float gp[44] = {ao_view_.near_w, ao_view_.far_w, tan_y * float(rw) / float(rh), tan_y,
                    float(rw), float(rh), float(rw), float(rh),
                    float(slot), float(slot + 1 + prev), float(slot + 6), float(slot + 7),
                    float(slot + 8), float(slot + 9), float(slot + 10), kFrames};
    std::memcpy(gp + 16, rt_clip_to_prev_, sizeof(rt_clip_to_prev_));
    float tail[8] = {reset ? 1.0f : 0.0f, kDepth, kNormal, kSpan, rough_limit, float(kView), float(slot + 3 + prev), 0};
    std::memcpy(gp + 32, tail, sizeof(tail));
    Alloc gcb = Upload(256, 256);
    std::memcpy(gcb.cpu, gp, sizeof(gp));
    for (uint32_t i : {1 + prev, 3 + prev}) Transition(rd_[i].Get(), rd_state_[i], kRead);
    for (uint32_t i : {1 + cur, 3 + cur}) Transition(rd_[i].Get(), rd_state_[i], D3D12_RESOURCE_STATE_RENDER_TARGET);
    rt::Scene::Out frames{Cpu(rtv_heap_.Get(), kRdRtv + 3 + cur, rtv_inc_), r16};
    bool drawn = rt_.Trace(rt::Scene::Pass::ReflectionGather, gcb.gpu, Gpu(srv_heap_.Get(), slot, srv_inc_),
                           Gpu(srv_heap_.Get(), 0, srv_inc_), Cpu(rtv_heap_.Get(), kRdRtv + 1 + cur, rtv_inc_), f16, rw,
                           rh, &frames, 1);
    for (uint32_t i : {1 + cur, 3 + cur}) Transition(rd_[i].Get(), rd_state_[i], kRead);
    if (!drawn) {
        EndRtPass();
        return false;
    }
    refl_cur_ = cur;
    refl_frame_ = presents_;
    // smoothed twice: into rd_ 5, then from it over the frame
    for (int p = 0; p < 2; p++) {
        float sp[20] = {ao_view_.near_w, ao_view_.far_w, tan_y * float(rw) / float(rh), tan_y,
                        float(rw), float(rh), float(rw), float(rh),
                        float(p == 0 ? slot + 1 + cur : slot + 5), float(slot + 3 + cur), float(slot + 6), float(1 << p),
                        float(slot + 10), rough_limit, kSmoothDepth, kSmoothNormal,
                        kFrames, float(kView), kSpread, 0};
        Alloc scb = Upload(256, 256);
        std::memcpy(scb.cpu, sp, sizeof(sp));
        if (p == 0) {
            Transition(rd_[5].Get(), rd_state_[5], D3D12_RESOURCE_STATE_RENDER_TARGET);
            drawn = rt_.Trace(rt::Scene::Pass::ReflectionSmooth, scb.gpu, Gpu(srv_heap_.Get(), slot, srv_inc_),
                              Gpu(srv_heap_.Get(), 0, srv_inc_), Cpu(rtv_heap_.Get(), kRdRtv + 5, rtv_inc_), f16, rw, rh);
            Transition(rd_[5].Get(), rd_state_[5], kRead);
        } else {
            Transition(t.res.Get(), t.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
            drawn = drawn && rt_.Trace(rt::Scene::Pass::ReflectionSmoothOver, scb.gpu, Gpu(srv_heap_.Get(), slot, srv_inc_),
                                       Gpu(srv_heap_.Get(), 0, srv_inc_), Cpu(rtv_heap_.Get(), t.view, rtv_inc_), t.fmt,
                                       rw, rh);
        }
    }
    EndRtPass();
    if (!refl_dn_said_) {
        LOGR("ray tracing: reflections denoised here (no denoiser in the anti-aliasing): gathered over up to %.0f frames "
             "at %ux%u, smoothed twice%s", double(kFrames), rw, rh, drawn ? "" : " (not drawn)");
        refl_dn_said_ = true;
    }
    return true;
}

// Ray traced light shafts over the frame's 3D (rt_shafts.hlsl), with the Lighting setting: the light the air catches
// from the sky through the room's openings and from the scene's lights (a torch's glow in the air), marched at a
// quarter of the frame's size, gathered over the frames and added over the frame. XBE_RT_SHAFTS their strength (1, 0:
// none), XBE_RT_SHAFTS_DENSITY the air's thickness (0.00015 of the light a unit), XBE_RT_SHAFTS_REACH how far a march
// goes (12000), XBE_RT_SHAFTS_GLOW the lights' brightness in the air against on surfaces (4), XBE_RT_SHAFTS_SKY the
// sky's (0: none; outdoors it laid a haze over every open place, which the game's own fog already is). The quality's (RtQuality) steps: none at low, 6 at medium, 10 at high
void D3D12Backend::Shafts(Target& t, uint32_t rw, uint32_t rh) {
    if (!(g_ray_tracing.load(std::memory_order_relaxed) & kRtLighting) || !BuildRtScene(rw, rh)) return;
    static const int kView = std::getenv("XBE_RT_VIEW") ? std::atoi(std::getenv("XBE_RT_VIEW")) : 0;
    auto env = [](const char* n, float d) { const char* e = std::getenv(n); return e ? float(std::atof(e)) : d; };
    static const float kStrength = env("XBE_RT_SHAFTS", 1.0f), kDensity = env("XBE_RT_SHAFTS_DENSITY", 0.00015f),
                       kReach = env("XBE_RT_SHAFTS_REACH", 12000.0f), kGlow = env("XBE_RT_SHAFTS_GLOW", 4.0f),
                       kSkyAir = env("XBE_RT_SHAFTS_SKY", 0.0f), kStepsSet = env("XBE_RT_SHAFTS_STEPS", 0),
                       kLightGain = env("XBE_RT_LIGHTS_GAIN", 24.0f), kSkyShare = env("XBE_RT_SKY_SHARE", 0.05f),
                       kFlat = env("XBE_RT_LIGHTS_FLAT", 0.8f), kLightArea = env("XBE_RT_LIGHTS_AREA", 30000.0f);
    const uint32_t q = RtQuality();
    const float steps = kStepsSet > 0 ? kStepsSet : q == 0 ? 0.0f : q == 1 ? 6.0f : 10.0f;
    if (kStrength <= 0 || steps <= 0) return;
    const DXGI_FORMAT f16 = DXGI_FORMAT_R16G16B16A16_FLOAT;
    uint32_t tw = std::max(1u, (t.w + 3) / 4), th = std::max(1u, (t.h + 3) / 4);
    if (!shafts_[0] || shafts_w_ != tw || shafts_h_ != th) {
        for (uint32_t i = 0; i < kShafts; i++) {
            if (shafts_[i]) Release(std::move(shafts_[i]));
            D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
            D3D12_RESOURCE_DESC td = Texture2DDesc(f16, tw, th);
            td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            D3D12_CLEAR_VALUE cv{};
            cv.Format = f16;
            shafts_state_[i] = D3D12_RESOURCE_STATE_RENDER_TARGET;
            if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, shafts_state_[i], &cv,
                                                     IID_PPV_ARGS(&shafts_[i])))) {
                for (auto& r : shafts_) r = nullptr;
                shafts_w_ = shafts_h_ = 0;
                return;
            }
            dev_->CreateRenderTargetView(shafts_[i].Get(), nullptr, Cpu(rtv_heap_.Get(), kShaftRtv + i, rtv_inc_));
        }
        shafts_w_ = tw;
        shafts_h_ = th;
        shafts_frame_ = UINT64_MAX;
    }
    const D3D12_RESOURCE_STATES kRead =
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    uint32_t sw = std::max(1u, (rw + 3) / 4), sh = std::max(1u, (rh + 3) / 4);
    uint32_t cur = 1 + (shaft_cur_ ^ 1), prev = 1 + shaft_cur_;
    bool reset = rt_reset_ || shafts_frame_ == UINT64_MAX || shafts_frame_ + 1 != presents_;
    // the depth, the march, the gathered before, the motion, the gathered now
    uint32_t slot = AllocSrvSlots(5);
    TextureSrv(aa_depth_.Get(), DXGI_FORMAT_R32_FLOAT, slot);
    TextureSrv(shafts_[0].Get(), f16, slot + 1);
    TextureSrv(shafts_[prev].Get(), f16, slot + 2);
    TextureSrv(aa_motion_.Get(), DXGI_FORMAT_R16G16_FLOAT, slot + 3);
    TextureSrv(shafts_[cur].Get(), f16, slot + 4);
    struct ShaftConstants {
        float p[32];
        uint32_t list[kMaxLights][4];
    } c{};
    float tan_y = std::tan(0.5f * ao_view_.fov_y);
    float up[3];
    WorldUpInView(ao_view_, up);
    // the scene's own lights only (a torch, a fire, a spell drawn in 3D, first in the list): those drawn flat (a
    // fountain's spray, motes, a glow's sprite) filled whole rooms with a grainy haze that sparkled frame to frame
    const std::vector<uint32_t>& em = rt_.Emissive();
    uint32_t lights = std::min<uint32_t>(rt_.EmissiveSolid(), kMaxLights);
    for (uint32_t i = 0; i < lights; i++) {
        c.list[i][0] = em[i * 2];
        c.list[i][1] = em[i * 2 + 1];
    }
    float strength = kStrength * g_rt_lighting_strength.load(std::memory_order_relaxed);
    float sky = sky_frame_ == presents_ ? sky_slot_ : -1.0f;
    const float p[32] = {ao_view_.near_w, ao_view_.far_w, tan_y * float(rw) / float(rh), tan_y,
                         float(rw), float(rh), float(sw), float(sh),
                         0, float(kView), float(presents_ % 65536), reset ? 1.0f : 0.0f,
                         kDensity, kReach, steps, strength,
                         up[0], up[1], up[2], sky,
                         float(slot), float(slot + 1), float(slot + 2), float(slot + 3),
                         float(lights), kLightGain * kGlow, kSkyAir, kSkyShare,
                         float(tw), float(th), kFlat, kLightArea};
    std::memcpy(c.p, p, sizeof(p));
    Alloc mcb = Upload(sizeof(ShaftConstants), 256);
    std::memcpy(mcb.cpu, &c, sizeof(c));
    // the march
    Transition(shafts_[0].Get(), shafts_state_[0], D3D12_RESOURCE_STATE_RENDER_TARGET);
    bool drawn = rt_.Trace(rt::Scene::Pass::Shafts, mcb.gpu, Gpu(srv_heap_.Get(), slot, srv_inc_),
                           Gpu(srv_heap_.Get(), 0, srv_inc_), Cpu(rtv_heap_.Get(), kShaftRtv, rtv_inc_), f16, sw, sh);
    Transition(shafts_[0].Get(), shafts_state_[0], kRead);
    // gathered with the frames before
    c.p[8] = 1;
    Alloc gcb = Upload(sizeof(ShaftConstants), 256);
    std::memcpy(gcb.cpu, &c, sizeof(c));
    Transition(shafts_[prev].Get(), shafts_state_[prev], kRead);
    Transition(shafts_[cur].Get(), shafts_state_[cur], D3D12_RESOURCE_STATE_RENDER_TARGET);
    drawn = drawn && rt_.Trace(rt::Scene::Pass::Shafts, gcb.gpu, Gpu(srv_heap_.Get(), slot, srv_inc_),
                               Gpu(srv_heap_.Get(), 0, srv_inc_), Cpu(rtv_heap_.Get(), kShaftRtv + cur, rtv_inc_), f16,
                               sw, sh);
    Transition(shafts_[cur].Get(), shafts_state_[cur], kRead);
    // over the frame
    c.p[8] = 2;
    c.p[21] = float(slot + 4);
    Alloc acb = Upload(sizeof(ShaftConstants), 256);
    std::memcpy(acb.cpu, &c, sizeof(c));
    Transition(t.res.Get(), t.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
    drawn = drawn && rt_.Trace(rt::Scene::Pass::ShaftsOver, acb.gpu, Gpu(srv_heap_.Get(), slot, srv_inc_),
                               Gpu(srv_heap_.Get(), 0, srv_inc_), Cpu(rtv_heap_.Get(), t.view, rtv_inc_), t.fmt, rw, rh);
    EndRtPass();
    if (drawn) {
        shaft_cur_ ^= 1;
        shafts_frame_ = presents_;
    }
    if (!shafts_said_) {
        LOGR("ray tracing: light shafts %s at %ux%u, %.0f steps, %u lights, the sky's %s, strength %.2f",
             drawn ? "drawn" : "not drawn", sw, sh, double(steps), lights, sky >= 0 ? "measured" : "none",
             double(strength));
        shafts_said_ = true;
    }
}

// Ray traced reflections on water over the frame's 3D (rt_water.hlsl), with the reflections setting: where the scene's
// water (the frame before's, Scene::LateCapture) lies before what the frame drew, the room in its mirror direction, rippled by slow
// waves laid in the world, in water's Fresnel share. XBE_RT_WATER (its strength, 1), XBE_RT_WATER_RIPPLE (the waves'
// slope, 0.15), XBE_RT_WATER_WAVE (their length, 40), XBE_RT_WATER_REACH (6000), XBE_RT_WATER_FADE (30000)
void D3D12Backend::Water(Target& t, uint32_t rw, uint32_t rh) {
    if (!(g_ray_tracing.load(std::memory_order_relaxed) & kRtReflections) || !BuildRtScene(rw, rh) || !rt_.HasWater())
        return;
    static const int kView = std::getenv("XBE_RT_VIEW") ? std::atoi(std::getenv("XBE_RT_VIEW")) : 0;
    auto env = [](const char* n, float d) { const char* e = std::getenv(n); return e ? float(std::atof(e)) : d; };
    static const float kStrength = env("XBE_RT_WATER", 1.0f), kRipple = env("XBE_RT_WATER_RIPPLE", 0.15f),
                       kWave = env("XBE_RT_WATER_WAVE", 40.0f), kReach = env("XBE_RT_WATER_REACH", 6000.0f),
                       kFade = env("XBE_RT_WATER_FADE", 30000.0f), kBias = env("XBE_RT_AO_BIAS", 1.0f);
    float tan_y = std::tan(0.5f * ao_view_.fov_y);
    const FrameView& v = ao_view_;
    float p[32] = {v.near_w, v.far_w, tan_y * float(rw) / float(rh), tan_y,
                   float(rw), float(rh), float(rw), float(rh),
                   kStrength * g_rt_reflection_strength.load(std::memory_order_relaxed), kReach, kRipple, kFade,
                   float(presents_ % 36000) / 60.0f, float(kView), 2.0f * kBias, 0.002f * kBias,
                   v.right[0], v.right[1], v.right[2], kWave,
                   v.up[0], v.up[1], v.up[2], 0,
                   v.forward[0], v.forward[1], v.forward[2], 0,
                   v.position[0], v.position[1], v.position[2], 0};
    Alloc cb = Upload(256, 256);
    std::memcpy(cb.cpu, p, sizeof(p));
    uint32_t slot = AllocSrvSlots(1);
    TextureSrv(aa_depth_.Get(), DXGI_FORMAT_R32_FLOAT, slot);
    Transition(t.res.Get(), t.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
    // each ray's length into the guides' hit distance, for DLSS Ray Reconstruction
    bool hits = guide_frame_ == presents_ && guide_[kGuides - 1] && guide_w_ == t.w && guide_h_ == t.h;
    rt::Scene::Out hit{Cpu(rtv_heap_.Get(), kGuideRtv + kGuides - 1, rtv_inc_), kGuideFmt[kGuides - 1]};
    if (hits) Transition(guide_[kGuides - 1].Get(), guide_state_[kGuides - 1], D3D12_RESOURCE_STATE_RENDER_TARGET);
    bool drawn = rt_.Trace(rt::Scene::Pass::Water, cb.gpu, Gpu(srv_heap_.Get(), slot, srv_inc_),
                           Gpu(srv_heap_.Get(), 0, srv_inc_), Cpu(rtv_heap_.Get(), t.view, rtv_inc_), t.fmt, rw, rh,
                           hits ? &hit : nullptr, hits ? 1 : 0);
    EndRtPass();
    if (!water_said_) {
        LOGR("ray tracing: water's reflections %s over the 3D at %ux%u, strength %.2f, ripples %.2f",
             drawn ? "drawn" : "not drawn", rw, rh, double(p[8]), double(kRipple));
        water_said_ = true;
    }
}

// Ray traced ambient occlusion gathered over the frames (rt_aotemporal.hlsl), for the blur and multiply after it, when
// no vendor's denoiser takes the frame: XBE_RT_DN_AO_FRAMES at most (16), held within this frame's neighbours' span
// (XBE_RT_DN_AO_SPAN deviations, 1.5)
ID3D12Resource* D3D12Backend::GatherAmbientOcclusion(uint32_t rw, uint32_t rh, uint32_t aw, uint32_t ah) {
    const uint32_t w = guide_w_, h = guide_h_;
    if (!ao_ || ao_w_ != w || ao_h_ != h || !EnsureOwnDenoiseTargets(w, h)) return nullptr;
    static const int kView = std::getenv("XBE_RT_VIEW") ? std::atoi(std::getenv("XBE_RT_VIEW")) : 0;
    auto env = [](const char* n, float d) { const char* e = std::getenv(n); return e ? float(std::atof(e)) : d; };
    static const float kFrames = std::clamp(env("XBE_RT_DN_AO_FRAMES", 16), 1.0f, 255.0f),
                       kSpan = env("XBE_RT_DN_AO_SPAN", 1.5f), kDepth = env("XBE_RT_DN_DEPTH", 0.05f),
                       kNormal = env("XBE_RT_DN_NORMAL", 0.8f);
    const D3D12_RESOURCE_STATES kRead =
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    uint32_t cur = 6 + (aodn_cur_ ^ 1), prev = 6 + aodn_cur_;
    bool reset = rt_reset_ || aodn_frame_ == UINT64_MAX || aodn_frame_ + 1 != presents_ || !HaveSurfaceBefore();
    // this frame's, the history before, the surfaces now and before, the motion, the depth
    uint32_t slot = AllocSrvSlots(6);
    TextureSrv(ao_.Get(), DXGI_FORMAT_R8_UNORM, slot);
    TextureSrv(rd_[prev].Get(), DXGI_FORMAT_R16G16_FLOAT, slot + 1);
    TextureSrv(guide_[SurfaceNow()].Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, slot + 2);
    TextureSrv(guide_[SurfaceBefore()].Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, slot + 3);
    TextureSrv(aa_motion_.Get(), DXGI_FORMAT_R16G16_FLOAT, slot + 4);
    TextureSrv(aa_depth_.Get(), DXGI_FORMAT_R32_FLOAT, slot + 5);
    float tan_y = std::tan(0.5f * ao_view_.fov_y);
    float p[36] = {ao_view_.near_w, ao_view_.far_w, tan_y * float(rw) / float(rh), tan_y,
                   float(rw), float(rh), float(aw), float(ah),
                   float(slot), float(slot + 1), float(slot + 2), float(slot + 3),
                   float(slot + 4), float(slot + 5), kFrames, kSpan};
    std::memcpy(p + 16, rt_clip_to_prev_, sizeof(rt_clip_to_prev_));
    p[32] = reset ? 1.0f : 0.0f;
    p[33] = kDepth;
    p[34] = kNormal;
    p[35] = float(kView);
    Alloc cb = Upload(256, 256);
    std::memcpy(cb.cpu, p, sizeof(p));
    Transition(ao_.Get(), ao_state_, kRead);
    Transition(rd_[prev].Get(), rd_state_[prev], kRead);
    Transition(rd_[cur].Get(), rd_state_[cur], D3D12_RESOURCE_STATE_RENDER_TARGET);
    bool drawn = rt_.Trace(rt::Scene::Pass::AmbientOcclusionGather, cb.gpu, Gpu(srv_heap_.Get(), slot, srv_inc_),
                           Gpu(srv_heap_.Get(), 0, srv_inc_), Cpu(rtv_heap_.Get(), kRdRtv + cur, rtv_inc_),
                           DXGI_FORMAT_R16G16_FLOAT, aw, ah);
    EndRtPass();
    Transition(rd_[cur].Get(), rd_state_[cur], kRead);
    if (!drawn) return nullptr;
    aodn_cur_ ^= 1;
    aodn_frame_ = presents_;
    if (!aodn_said_) {
        LOGR("ray tracing: ambient occlusion denoised here (no denoiser in the anti-aliasing): gathered over up to %.0f "
             "frames at %ux%u", double(kFrames), aw, ah);
        aodn_said_ = true;
    }
    return rd_[cur].Get();
}

// Ambient occlusion over the frame's 3D (rw x rh of t from the top left), from anti-aliasing's copy of its depth: worked
// out into ao_ at its quality's part of that size, then multiplied into the frame, before the anti-aliasing's pass
// gathers it with the frames before. A frame without a view found takes the last one's camera. still: no anti-aliasing
// after it, so its samples' turn stays the same from frame to frame (a turning one would flicker) and it is blurred wider
void D3D12Backend::AmbientOcclusion(Target& t, uint32_t rw, uint32_t rh, const FrameView& view, bool rx, bool still) {
    uint32_t q = g_ambient_occlusion.load(std::memory_order_relaxed);
    if (!q) return;
    // the view given (ambient occlusion alone, with no anti-aliasing, takes it here: anti-aliasing's pass took it
    // before), else the last one found
    if (view.valid) {
        ao_view_ = view;
        ao_view_ok_ = true;
    }
    if (!ao_view_ok_ || ao_view_.near_w <= 0 || ao_view_.far_w <= ao_view_.near_w || ao_view_.fov_y <= 0) return;
    const AoQuality& aq = q >= 2 ? kAoHigh : kAoLow;
    if (!ao_ || ao_w_ != t.w || ao_h_ != t.h) {
        if (ao_) Release(std::move(ao_));
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC td = Texture2DDesc(DXGI_FORMAT_R8_UNORM, t.w, t.h);
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_CLEAR_VALUE cv{};
        cv.Format = DXGI_FORMAT_R8_UNORM;
        ao_state_ = D3D12_RESOURCE_STATE_RENDER_TARGET;
        if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, ao_state_, &cv, IID_PPV_ARGS(&ao_)))) {
            ao_w_ = ao_h_ = 0;
            return;
        }
        dev_->CreateRenderTargetView(ao_.Get(), nullptr, Cpu(rtv_heap_.Get(), kAoRtv, rtv_inc_));
        ao_w_ = t.w;
        ao_h_ = t.h;
    }
    ID3D12PipelineState* opso = EnsurePassPipeline(kAoHlsl, "aomain", DXGI_FORMAT_R8_UNORM, -1);
    ID3D12PipelineState* apso = EnsureAoApplyPipeline(t.fmt);
    if (!opso || !apso) return;
    // ray traced (raytrace.h): through the frame's scene, worked out at a part of the 3D's size: the quality's up to
    // 1080p's pixels, half above (XBE_RT_AO_SCALE: as set). At 4K the rays from every pixel took most of the ray
    // tracing's time (some 2.6 ms of 4.3 to 5.7 on the 5080, and the bounce lighting's rays were halved to make room);
    // aoapply reads the target smoothly by depth, as it does Low's half
    bool traced = (g_ray_tracing.load(std::memory_order_relaxed) & kRtAmbientOcclusion) && BuildRtScene(rw, rh);
    static const float kRtAoScale = std::getenv("XBE_RT_AO_SCALE") ? float(std::atof(std::getenv("XBE_RT_AO_SCALE"))) : 0.0f;
    const float scale = !traced ? aq.scale
                        : kRtAoScale > 0 ? std::clamp(kRtAoScale, 0.25f, 1.0f)
                        : uint64_t(rw) * uint64_t(rh) > 1920ull * 1080ull ? std::min(aq.scale, 0.5f) : aq.scale;
    uint32_t aw = std::max(1u, uint32_t(float(rw) * scale)), ah = std::max(1u, uint32_t(float(rh) * scale));
    const D3D12_RESOURCE_STATES kRead =
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    Transition(aa_depth_.Get(), aa_depth_state_, kRead);
    if (rx) Transition(aa_rx_.Get(), aa_rx_state_, kRead);
    // (and after the four, for the ray traced pass: the eye's hits and the bounce lighting's kept lights)
    uint32_t slot = AllocSrvSlots(6);
    ID3D12Resource* src[4] = {aa_depth_.Get(), ao_.Get(), rx ? aa_rx_.Get() : nullptr, nullptr};
    DXGI_FORMAT fmt[4] = {DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8_UNORM, kBgra};
    for (int i = 0; i < 4; i++) {
        D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = fmt[i];
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.Texture2D.MipLevels = 1;
        dev_->CreateShaderResourceView(src[i], &sv, Cpu(srv_heap_.Get(), slot + i, srv_inc_));
    }
    float tan_y = std::tan(0.5f * ao_view_.fov_y);
    float radius = diag::AoRadius(aq.radius);  // its reach goes with it
    float p[20] = {ao_view_.near_w, ao_view_.far_w, tan_y * float(rw) / float(rh), tan_y,
                   float(rw) / float(t.w), float(rh) / float(t.h), 1.0f / float(rw), 1.0f / float(rh),
                   radius, diag::AoStrength(aq.strength), aq.reach * radius / aq.radius, aq.samples,
                   still ? 0.0f : float(presents_ % 1024), rx ? 1.0f : 0.0f, 1.0f / float(aw), 1.0f / float(ah),
                   float(aw) / float(t.w), float(ah) / float(t.h), float(diag::AoView()), still ? 1.0f : 0.0f};
    int rt_view = 0;
    if (traced) {
        static const int kView = std::getenv("XBE_RT_VIEW") ? std::atoi(std::getenv("XBE_RT_VIEW")) : 0;
        auto env = [](const char* n, float d) { const char* e = std::getenv(n); return e ? float(std::atof(e)) : d; };
        static const float kRadius = env("XBE_RT_AO_RADIUS", 0), kStrength = env("XBE_RT_AO_STRENGTH", 1.5f),
                           kRays = env("XBE_RT_AO_RAYS", 0), kBias = env("XBE_RT_AO_BIAS", 1.0f);
        rt_view = kView;
        float rr = kRadius > 0 ? kRadius : radius;
        // the quality's (RtQuality): low 2 rays a pixel, medium 4 (3 at ambient occlusion's Low), high 6 (3); 8 where
        // High is worked out at half the size (a third of the rays a frame still)
        const uint32_t rq = RtQuality();
        float rays = kRays > 0 ? kRays : rq == 0 ? 2.0f : q >= 2 ? (rq == 1 ? 4.0f : scale < aq.scale ? 8.0f : 6.0f) : 3.0f;
        // characters' shadows (rt_ao.hlsl): from a soft light above, which the characters alone block; XBE_RT_SHADOWS
        // their darkest (0.7, 0: none), XBE_RT_SHADOW_REACH how far toward the light a character still shades (1000:
        // about two and a half characters' height), XBE_RT_SHADOW_SPREAD the light's width (0.2: about 11 degrees),
        // XBE_RT_SHADOW_TILT its angle from straight above (40 degrees, the same way in every room and view: a shadow
        // falls to one side, seen past the character's feet)
        static const float kShadow = env("XBE_RT_SHADOWS", 0.7f), kShadowReach = env("XBE_RT_SHADOW_REACH", 1000.0f),
                           kShadowSpread = env("XBE_RT_SHADOW_SPREAD", 0.2f), kTilt = env("XBE_RT_SHADOW_TILT", 40.0f);
        // XBE_RT_SHADOWS_TAKE: one shadow by a lamp, the lamp's in the light above's place wholly once the lamp
        // gives 1 / this of the most a lamp can (4: a quarter; 0: both, each as much as the lamp's part, as before,
        // when a character by a torch had two)
        static const float kShadowTake = env("XBE_RT_SHADOWS_TAKE", 4.0f);
        float up[3];
        WorldUpInView(ao_view_, up, kTilt);
        bool shade = kShadow > 0 && rt_.HasCharacters();
        float rp[24] = {ao_view_.near_w, ao_view_.far_w, tan_y * float(rw) / float(rh), tan_y,
                        float(rw), float(rh), float(aw), float(ah),
                        rr, kStrength, aq.reach * rr / aq.radius, rays,
                        float(presents_ % 65536), float(kView), 4.0f * kBias, 0.004f * kBias,
                        up[0], up[1], up[2], shade ? kShadow : 0.0f,
                        kShadowReach, kShadowSpread, kShadowTake, 0};
        if (shade && !shadow_said_) {
            LOGR("ray tracing: characters' shadows from a light above, darkest %.2f, reaching %.0f", double(kShadow),
                 double(kShadowReach));
            shadow_said_ = true;
        }
        // and from the scene's lights (rt_lights.hlsli, the bounce lighting's list): XBE_RT_SHADOWS_LAMPS their
        // darkest (0.7, 0: none) where a lamp lights a surface as much as a lamp can
        static const float kLampShadow = env("XBE_RT_SHADOWS_LAMPS", 0.7f);
        struct AoConstants {
            float p[24];
            float lt[4];
            float sk[4];
            float hv[4];
            float rs[4];
            float pv[16];
            uint32_t list[kMaxLights][4];
        } ac{};
        std::memcpy(ac.p, rp, sizeof(rp));
        ac.hv[0] = ac.hv[1] = ac.hv[2] = ac.rs[0] = -1.0f;
        const bool lamps = shade && kLampShadow > 0 && (g_ray_tracing.load(std::memory_order_relaxed) & kRtLighting);
        if (lamps) {
            uint32_t n = LightList(ac.lt, ac.sk, ac.list);
            ac.sk[0] = n ? kLampShadow : 0.0f;
            // the glows known frame to frame (GlowMemory): how much of each light is there, and the lights' places
            // along the list since the frame before (the kept lights')
            GlowMemory(rw, rh);
            ac.hv[1] = glow_summary_srv_;
            ac.hv[2] = glow_slots_srv_;
            // the lamp each surface's shadow falls from: the bounce lighting's kept light there the frame before
            // (rs_[], rt_gi.hlsl's reservoirs, read as its KeptBefore does with the camera's move), else picked afresh
            const uint32_t rs_prev = uint32_t((presents_ & 1) ^ 1);
            if (n && rs_[rs_prev] && !rt_reset_ && rs_frame_ != UINT64_MAX && rs_frame_ + 1 == presents_ && rs_gw_ &&
                rs_gh_) {
                Transition(rs_[rs_prev].Get(), rs_state_[rs_prev], kRead);
                TextureSrv(rs_[rs_prev].Get(), DXGI_FORMAT_R32G32B32A32_FLOAT, slot + 5);
                ac.rs[0] = float(slot + 5);
                ac.rs[1] = float(rs_gw_);
                ac.rs[2] = float(rs_gh_);
                ac.rs[3] = env("XBE_RT_LIGHTS_REUSE_DEPTH", 0.05f);
            }
        }
        std::memcpy(ac.pv, rt_clip_to_prev_, sizeof(ac.pv));
        // the eye's hits (Guides, kGuideHit), where the guides ran before this: the surface each pixel's rays leave from
        if (EyeHits(t)) {
            TextureSrv(guide_[kGuideHit].Get(), kGuideFmt[kGuideHit], slot + 4);
            ac.hv[0] = float(slot + 4);
        }
        // XBE_RT_AO_SHEET: how far over a surface a sheet level with it still lies on it, its rays going on past it
        // (rt_ao.hlsl TraceAo; 32 units, about 15 cm; 0: they stop at it)
        static const float kSheet = env("XBE_RT_AO_SHEET", 32.0f);
        ac.sk[1] = kSheet;
        Alloc rcb = Upload(sizeof(AoConstants), 256);
        std::memcpy(rcb.cpu, &ac, sizeof(ac));
        // drawn into the ambient occlusion's target as the screen space pass is, with the depth and the frame's
        // pictures (the cut-outs')
        Transition(ao_.Get(), ao_state_, D3D12_RESOURCE_STATE_RENDER_TARGET);
        rt_.Trace(rt::Scene::Pass::AmbientOcclusion, rcb.gpu, Gpu(srv_heap_.Get(), slot, srv_inc_),
                  Gpu(srv_heap_.Get(), 0, srv_inc_), Cpu(rtv_heap_.Get(), kAoRtv, rtv_inc_), DXGI_FORMAT_R8_UNORM, aw,
                  ah);
        EndRtPass();
        // gathered over the frames by the port's own denoiser when no vendor's takes the frame: the blur and multiply
        // after it then take the history (its view in the ambient occlusion's place: nothing before reads that)
        if (OwnDenoiser() && Guides(t, rw, rh)) {
            if (ID3D12Resource* h = GatherAmbientOcclusion(rw, rh, aw, ah)) TextureSrv(h, DXGI_FORMAT_R16G16_FLOAT, slot + 1);
        }
        // its debug views shown as they are, unblurred (aoapply's XBE_AO_VIEW 3)
        if (kView) p[18] = 3;
    }
    Alloc cb = Upload(256, 256);
    std::memcpy(cb.cpu, p, sizeof(p));
    if (!traced) {
        // worked out at its part of the frame's size
        Transition(ao_.Get(), ao_state_, D3D12_RESOURCE_STATE_RENDER_TARGET);
        D3D12_CPU_DESCRIPTOR_HANDLE oh = Cpu(rtv_heap_.Get(), kAoRtv, rtv_inc_);
        cl_->OMSetRenderTargets(1, &oh, FALSE, nullptr);
        SetFullViewport(aw, ah);
        DrawFullScreen(opso, cb.gpu, slot);
    }
    // then multiplied into the frame's part
    Transition(ao_.Get(), ao_state_, kRead);
    Transition(t.res.Get(), t.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_CPU_DESCRIPTOR_HANDLE th = Cpu(rtv_heap_.Get(), t.view, rtv_inc_);
    cl_->OMSetRenderTargets(1, &th, FALSE, nullptr);
    SetFullViewport(rw, rh);
    DrawFullScreen(apso, cb.gpu, slot);
    bound_rt_ = bound_ds_ = nullptr;
    if (ao_said_ != (still ? 2u : 1u)) {
        LOGR("ambient occlusion: %s%s, worked out at %ux%u for the 3D at %ux%u%s; the view %.1f to %.1f deep, %.1f "
             "degrees high", q >= 2 ? "high" : "low", traced ? ", ray traced" : "", aw, ah, rw, rh,
             still ? ", with no anti-aliasing" : "", ao_view_.near_w, ao_view_.far_w, ao_view_.fov_y * 57.29578f);
        if (traced && rt_view) LOGR("ambient occlusion: XBE_RT_VIEW %d", rt_view);
        ao_said_ = still ? 2 : 1;
    }
}

// The frame's 3D anti-aliased in place, before its UI: its motion (each 3D draw's own, else the camera's from depth)
// and its depth drawn at its size as frame generation's are, then AMD's pass over it, written back. The world's
// draws took this frame's offset (Extension::Jitter), which it takes back out
bool D3D12Backend::AntiAliasFrame(Target& t) {
    if (aa_done_frame_ == presents_) return false;
    aa_done_frame_ = presents_;
    // XBE_AA_TRACE=N: where every Nth frame's 3D is closed, among the game's draws (with CV_WIDE_TRACE=N's)
    static const uint64_t kAaTrace = std::getenv("XBE_AA_TRACE") ? std::max(1, std::atoi(std::getenv("XBE_AA_TRACE"))) : 0;
    if (kAaTrace && presents_ % kAaTrace == 0)
        LOGR("antialias %llu: the frame's 3D closed and anti-aliased here", (unsigned long long)presents_);
    if (!aa_ || aa_jitter_frame_ != presents_) return false;
    Target* ds = fg_ds_frame_ == presents_ ? fg_ds_ : nullptr;
    // the frame as the game draws it, 8 bit gamma-encoded colour (AMD's pass is told so)
    if (!ds || ds->w != t.w || ds->h != t.h || t.fmt != kBgra || !EnsureAaTargets(t.w, t.h, t.fmt)) {
        LOG_FIRST(4, "antialias: frame %llu not anti-aliased: depth %d, sizes %ux%u %ux%u, format %d",
                  (unsigned long long)presents_, int(ds != nullptr), t.w, t.h, ds ? ds->w : 0, ds ? ds->h : 0,
                  int(t.fmt));
        return false;
    }
    ID3D12PipelineState* mpso = EnsurePassPipeline(kFrameGenHlsl, "motion", DXGI_FORMAT_R16G16_FLOAT, -1);
    ID3D12PipelineState* dpso = EnsurePassPipeline(kFrameGenHlsl, "depthcopy", DXGI_FORMAT_R32_FLOAT, -1);
    if (!mpso || !dpso) return false;
    GpuMark(kGpuAa);
    // upscaling: the scene's part drawn, its depth and motion at that size from the top left, as the colour's
    bool scene = scene_ == &t;
    uint32_t rw = scene ? scene_w_ : t.w, rh = scene ? scene_h_ : t.h;
    FrameView view;
    bool have = ext_ && ext_->View(view) && view.valid;
    if (!have) {
        // no camera move found: each draw's own motion only, the rest still
        view = {};
        for (int i = 0; i < 16; i++) view.clip_to_prev[i] = view.view_to_clip[i] = i % 5 == 0 ? 1.0f : 0.0f;
    }
    Transition(ds->res.Get(), ds->state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    bool own = fg_objmv_ && fg_objmv_frame_ == presents_ && fg_objmv_w_ == t.w && fg_objmv_h_ == t.h;
    if (own) Transition(fg_objmv_.Get(), fg_objmv_state_, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    uint32_t slot = AllocSrvSlots(4);
    for (int i = 0; i < 4; i++) {
        D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = i == 0 ? kDepthRead : i == 1 ? DXGI_FORMAT_R16G16_FLOAT : kBgra;
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.Texture2D.MipLevels = 1;
        ID3D12Resource* r = i == 0 ? ds->res.Get() : i == 1 && own ? fg_objmv_.Get() : nullptr;
        dev_->CreateShaderResourceView(r, &sv, Cpu(srv_heap_.Get(), slot + i, srv_inc_));
    }
    Alloc cb = Upload(256, 256);
    std::memcpy(cb.cpu, view.clip_to_prev, sizeof(view.clip_to_prev));
    float opt[4] = {own ? 1.0f : 0.0f, 0, scene ? scene_fx_ : 1.0f, scene ? scene_fy_ : 1.0f};
    std::memcpy(cb.cpu + sizeof(view.clip_to_prev), opt, sizeof(opt));
    Transition(aa_motion_.Get(), aa_motion_state_, D3D12_RESOURCE_STATE_RENDER_TARGET);
    Transition(aa_depth_.Get(), aa_depth_state_, D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_CPU_DESCRIPTOR_HANDLE mh = Cpu(rtv_heap_.Get(), kAaRtv + 1, rtv_inc_);
    cl_->OMSetRenderTargets(1, &mh, FALSE, nullptr);
    SetFullViewport(rw, rh);
    binds_ = {};
    DrawFullScreen(mpso, cb.gpu, slot);
    D3D12_CPU_DESCRIPTOR_HANDLE dh = Cpu(rtv_heap_.Get(), kAaRtv, rtv_inc_);
    cl_->OMSetRenderTargets(1, &dh, FALSE, nullptr);
    DrawFullScreen(dpso, cb.gpu, slot);
    bound_rt_ = bound_ds_ = nullptr;
    const D3D12_RESOURCE_STATES kRead =
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    Transition(aa_motion_.Get(), aa_motion_state_, kRead);
    Transition(aa_depth_.Get(), aa_depth_state_, kRead);
    // the frame's effects, when they were drawn into this frame at its size
    bool rx = aa_rx_ && aa_rx_frame_ == presents_ && aa_rx_w_ == t.w && aa_rx_h_ == t.h;
    // ambient occlusion into the frame, which the vendor's pass then gathers with the frames before
    GpuMark(kGpuAo);
    if (view.valid) {  // the camera's, which ambient occlusion and ray tracing take from the frame view
        ao_view_ = view;
        ao_view_ok_ = true;
    }
    // the ray tracing scene built first, before any pass draws: built after the screen space passes, a later build
    // over it hung the GPU when a room changed
    // and the guides when a denoiser takes them: DLSS Ray Reconstruction's, else the port's own (OwnDenoiser)
    rt_view_now_ = have && view.valid && !view.cut;
    if (BuildRtScene(rw, rh) && (aa_->Denoises() || OwnDenoiser())) Guides(t, rw, rh);
    GpuMark(kGpuRtScene);
    // the bounce lighting's own denoiser: whether there is anything to gather from, and the camera's move
    rt_reset_ = !aa_had_view_ || !have || view.cut;
    std::copy(view.clip_to_prev, view.clip_to_prev + 16, rt_clip_to_prev_);
    AmbientOcclusion(t, rw, rh, view.valid ? view : FrameView{}, rx);
    GpuMark(kGpuRtAo);
    Lighting(t, rw, rh);
    GpuMark(kGpuRtGi);
    Reflections(t, rw, rh);
    Water(t, rw, rh);
    GpuMark(kGpuRtRefl);
    Shafts(t, rw, rh);
    GpuMark(kGpuVendor);
    Transition(t.res.Get(), t.state, kRead);
    bool guided = guide_frame_ == presents_ && guide_w_ == t.w && guide_h_ == t.h && guide_[0];
    if (guided) Transition(guide_[kGuides - 1].Get(), guide_state_[kGuides - 1], kRead);
    if (rx) Transition(aa_rx_.Get(), aa_rx_state_, kRead);
    Transition(aa_out_.Get(), aa_out_state_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    auto now = std::chrono::steady_clock::now();
    framegen::AntiAlias::Frame f;
    f.color = t.res.Get();
    f.depth = aa_depth_.Get();
    f.motion = aa_motion_.Get();
    f.reactive = rx ? aa_rx_.Get() : nullptr;
    f.output = aa_out_.Get();
    f.w = t.w;
    f.h = t.h;
    f.rw = rw;
    f.rh = rh;
    // the quality set even on a frame drawn whole (the first 3D frame after a 2D screen): DLSS keeps its mode, made
    // again only when the setting changes
    f.quality = up_quality_;
    f.jitter[0] = aa_jitter_[0];
    f.jitter[1] = aa_jitter_[1];
    f.reset = !aa_had_view_ || !have || view.cut;
    f.view = view;
    f.frame_ms = float(std::chrono::duration<double, std::milli>(now - aa_t_).count());
    f.sharpness = float(std::min<uint32_t>(g_aa_sharpness.load(std::memory_order_relaxed), 100)) / 100.0f;
    if (guided && aa_->Denoises()) {
        f.albedo = guide_[0].Get();
        f.normals = guide_[1].Get();
        f.specular = guide_[2].Get();
        f.hit_distance = guide_[3].Get();
        std::copy(guide_view_, guide_view_ + 12, f.view_to_world);
        if (!rr_said_) {
            LOGR("ray tracing: its guides given to %s's denoiser (Ray Reconstruction) at %ux%u",
                 framegen::AntiAliasName(aa_->Kind()), rw, rh);
            rr_said_ = true;
        }
    }
    aa_t_ = now;
    aa_had_view_ = have;
    bool taa = aa_->Own();
    bool ran = false;
    if (aa_->VulkanImages() && aa_vk_) {
        // Linux's AMD FSR (or NVIDIA DLSS) on Vulkan: the frame's colour and effects copied into its own images, every
        // input and the output in COMMON (Vulkan's GENERAL), the frame so far submitted; FSR's commands then go on
        // the queue after it, and the frame's after those, none waited for (vkd3d submits a list to the queue as it
        // is executed: nothing here makes the queue wait)
        Transition(t.res.Get(), t.state, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Transition(aa_vk_color_.Get(), aa_vk_color_state_, D3D12_RESOURCE_STATE_COPY_DEST);
        cl_->CopyResource(aa_vk_color_.Get(), t.res.Get());
        if (rx) {
            Transition(aa_rx_.Get(), aa_rx_state_, D3D12_RESOURCE_STATE_COPY_SOURCE);
            Transition(aa_vk_rx_.Get(), aa_vk_rx_state_, D3D12_RESOURCE_STATE_COPY_DEST);
            cl_->CopyResource(aa_vk_rx_.Get(), aa_rx_.Get());
        }
        for (auto [r, st] : {std::pair{aa_vk_color_.Get(), &aa_vk_color_state_}, std::pair{aa_vk_rx_.Get(), &aa_vk_rx_state_},
                             std::pair{aa_depth_.Get(), &aa_depth_state_}, std::pair{aa_motion_.Get(), &aa_motion_state_},
                             std::pair{aa_out_.Get(), &aa_out_state_}})
            Transition(r, *st, D3D12_RESOURCE_STATE_COMMON);
        SubmitSoFar();
        f.color = aa_vk_color_.Get();
        f.reactive = rx ? aa_vk_rx_.Get() : nullptr;
        ran = aa_->RunQueue(f);
    } else {
        ran = taa ? TaaFrame(t, f) : aa_->Run(cl_.Get(), f);
    }
    if (ran) {
        if (!aa_said_) {
            LOGR("antialias: %s on the frame's 3D at %ux%u%s", AaName(), t.w, t.h,
                 aa_out_fmt_ == t.fmt ? "" : ", drawn back from RGBA");
            aa_said_ = true;
        }
        aa_ran_frame_ = presents_;
        if (rx && rx_draws_ && !rx_said_) {
            LOGR("antialias: %u effects drawn into the reactive mask (sparks, fire, glows: taken from this frame)", rx_draws_);
            rx_said_ = true;
        }
        g_antialias_drawn.store(rw << 16 | rh, std::memory_order_relaxed);
        g_antialias_output.store(t.w << 16 | t.h, std::memory_order_relaxed);
        if (taa) {
            // drawn back into the frame by TaaFrame
        } else if (aa_out_fmt_ == t.fmt) {
            Transition(aa_out_.Get(), aa_out_state_, D3D12_RESOURCE_STATE_COPY_SOURCE);
            Transition(t.res.Get(), t.state, D3D12_RESOURCE_STATE_COPY_DEST);
            cl_->CopyResource(t.res.Get(), aa_out_.Get());
        } else {
            Transition(aa_out_.Get(), aa_out_state_, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            Transition(t.res.Get(), t.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
            ID3D12DescriptorHeap* heaps[] = {srv_heap_.Get(), smp_heap_.Get()};
            cl_->SetDescriptorHeaps(2, heaps);
            SetBlitTarget(Cpu(rtv_heap_.Get(), t.view, rtv_inc_), t.w, t.h);
            float uv[4] = {0, 0, 1, 1};
            Blit(aa_out_.Get(), aa_out_fmt_, uv, BlitOptions{.alpha = true, .rtv = t.fmt});
        }
    }
    GpuMark(kGpuAfter);
    Transition(t.res.Get(), t.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
    // upscaling: the scene's depth and motion made whole too, or when the pass did not run, its colour stretched
    if (scene) {
        if (!ran) StretchScene(t);
        SceneDepthUp(t, true);
    }
    return true;
}

// The own TAA (framegen::MakeOwnTaa): each output pixel's point, the frame's offset taken back out, read from the
// frame drawn (bilinear between its pixels, at its part's size when upscaling) and gathered with where that point was in
// the frames before (the history, read again through the motion of the nearest surface around it, Catmull-Rom so it
// stays sharp), a tenth of it new each frame. What the history holds is first clipped into the colours around the point
// now (YCoCg, their mean and 1.25 spreads either way), so what moved or changed leaves no trail; an effect over the
// pixel (the reactive mask), a cut or a point not seen before takes the frame as drawn. Then drawn back into the frame,
// sharpened within its neighbours' range
const char* const kTaaHlsl = R"(
cbuffer P : register(b1) {
    float4 outp;  // the output's size, one of its pixels in uv
    float4 part;  // the part drawn's size in pixels, its share of the textures (uv scale)
    float4 jit;   // the frame's offset in pixels drawn, the history kept (1) or not, the frame's own share
    float4 shp;   // sharpening 0 to 1, a reactive mask (1) or none, 1 when nearer is a smaller depth (else -1)
};
Texture2D<float4> col : register(t0);
Texture2D<float2> mv : register(t1);
Texture2D<float4> hist : register(t2);
Texture2D<float> rx : register(t3);
Texture2D<float> depth : register(t4);
SamplerState ls : register(s0);
SamplerState ps : register(s1);
struct O { float4 p : SV_Position; float2 uv : TEXCOORD0; };
O vs(uint id : SV_VertexID) {
    O o;
    float2 t = float2((id << 1) & 2, id & 2);
    o.p = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = t;
    return o;
}
float3 YCoCg(float3 c) { return float3(dot(c, float3(0.25, 0.5, 0.25)), dot(c, float3(0.5, 0, -0.5)), dot(c, float3(-0.25, 0.5, -0.25))); }
float3 Rgb(float3 y) { return float3(y.x + y.y - y.z, y.x + y.z, y.x - y.y - y.z); }
// the history at uv, Catmull-Rom from nine bilinear reads
float3 History(float2 uv) {
    float2 pos = uv * outp.xy;
    float2 c1 = floor(pos - 0.5) + 0.5;
    float2 f = pos - c1;
    float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    float2 w3 = f * f * (-0.5 + 0.5 * f);
    float2 w12 = w1 + w2;
    float2 t0 = (c1 - 1) * outp.zw, t3 = (c1 + 2) * outp.zw, t12 = (c1 + w2 / w12) * outp.zw;
    float3 r = hist.SampleLevel(ls, float2(t0.x, t0.y), 0).rgb * (w0.x * w0.y);
    r += hist.SampleLevel(ls, float2(t12.x, t0.y), 0).rgb * (w12.x * w0.y);
    r += hist.SampleLevel(ls, float2(t3.x, t0.y), 0).rgb * (w3.x * w0.y);
    r += hist.SampleLevel(ls, float2(t0.x, t12.y), 0).rgb * (w0.x * w12.y);
    r += hist.SampleLevel(ls, float2(t12.x, t12.y), 0).rgb * (w12.x * w12.y);
    r += hist.SampleLevel(ls, float2(t3.x, t12.y), 0).rgb * (w3.x * w12.y);
    r += hist.SampleLevel(ls, float2(t0.x, t3.y), 0).rgb * (w0.x * w3.y);
    r += hist.SampleLevel(ls, float2(t12.x, t3.y), 0).rgb * (w12.x * w3.y);
    r += hist.SampleLevel(ls, float2(t3.x, t3.y), 0).rgb * (w3.x * w3.y);
    return max(r, 0);
}
// h moved toward the box's middle until it is inside it
float3 Clip(float3 h, float3 lo, float3 hi) {
    float3 c = 0.5 * (hi + lo), e = 0.5 * (hi - lo) + 1e-4;
    float3 v = h - c;
    float3 a = abs(v / e);
    float m = max(a.x, max(a.y, a.z));
    return m > 1 ? c + v / m : h;
}
float4 taa(O i) : SV_Target {
    float2 tex = part.zw / part.xy;       // a pixel drawn, in the textures' uv
    float2 lim = part.zw - 0.5 * tex;     // the last pixel drawn's centre: nothing past the part is read
    float2 at = i.uv * part.xy + jit.xy;  // the point, the frame's offset taken out, in pixels drawn
    float2 c0 = min((floor(at) + 0.5) * tex, lim);
    float3 m1 = 0, m2 = 0;
    [unroll] for (int y = -1; y <= 1; y++) {
        [unroll] for (int x = -1; x <= 1; x++) {
            float3 c = YCoCg(col.SampleLevel(ps, clamp(c0 + float2(x, y) * tex, 0, lim), 0).rgb);
            m1 += c;
            m2 += c * c;
        }
    }
    m1 /= 9;
    float3 sd = sqrt(max(m2 / 9 - m1 * m1, 0));
    float4 here = col.SampleLevel(ps, c0, 0);
    float3 cur = YCoCg(col.SampleLevel(ls, min(at * tex, lim), 0).rgb);
    // the motion of the nearest surface among the pixel and its corners: an edge moves with what is in front
    float2 best = c0;
    float bd = depth.SampleLevel(ps, c0, 0) * shp.z;
    [unroll] for (int k = 0; k < 4; k++) {
        float2 o = clamp(c0 + float2((k & 1) ? 1 : -1, (k & 2) ? 1 : -1) * tex, 0, lim);
        float d = depth.SampleLevel(ps, o, 0) * shp.z;
        if (d < bd) {
            bd = d;
            best = o;
        }
    }
    float2 m = mv.SampleLevel(ps, best, 0);
    float2 prev = i.uv + m;
    float w = jit.w;
    // faster motion leans on the frame more: less of a trail
    w = max(w, saturate(length(m * outp.xy) / 64));
    if (shp.y > 0) w = max(w, saturate(rx.SampleLevel(ps, c0, 0)));
    if (jit.z == 0 || any(prev < 0) || any(prev > 1)) w = 1;
    float3 h = Clip(YCoCg(History(prev)), m1 - 1.25 * sd, m1 + 1.25 * sd);
    return float4(saturate(Rgb(lerp(h, cur, w))), here.a);
}
float4 taaout(O i) : SV_Target {
    float4 c = hist.SampleLevel(ps, i.uv, 0);
    float3 n = hist.SampleLevel(ps, i.uv - float2(0, outp.w), 0).rgb, s = hist.SampleLevel(ps, i.uv + float2(0, outp.w), 0).rgb;
    float3 e = hist.SampleLevel(ps, i.uv + float2(outp.z, 0), 0).rgb, w = hist.SampleLevel(ps, i.uv - float2(outp.z, 0), 0).rgb;
    float3 lo = min(c.rgb, min(min(n, s), min(e, w))), hi = max(c.rgb, max(max(n, s), max(e, w)));
    float3 sharp = c.rgb + (4 * c.rgb - n - s - e - w) * (0.5 * shp.x);
    return float4(clamp(sharp, lo, hi), c.a);
}
)";

// the own TAA in AMD FSR's place: always on Linux, where AMD's library (a Windows one) can't load; on Windows with
// XBE_TAA=1, to see it beside FSR
bool OwnTaaChosen() {
#ifdef _WIN32
    static const bool on = [] {
        const char* v = std::getenv("XBE_TAA");
        return v && *v == '1';
    }();
    return on;
#else
    return true;
#endif
}

// The own TAA over the frame (kTaaHlsl): its histories made at the output's size, the frame gathered into the one not
// drawn last, which is then drawn back into the frame. The frame and the reactive mask come readable
bool D3D12Backend::TaaFrame(Target& t, const framegen::AntiAlias::Frame& f) {
    if (!f.color || !f.depth || !f.motion || !f.w || !f.h || !f.rw || !f.rh) return false;
    if (!taa_[0] || !taa_[1] || taa_w_ != f.w || taa_h_ != f.h) {
        for (auto& r : taa_)
            if (r) Release(std::move(r));
        for (int i = 0; i < 2; i++) {
            D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
            D3D12_RESOURCE_DESC td = Texture2DDesc(kHdr, f.w, f.h);
            td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            D3D12_CLEAR_VALUE cv{};
            cv.Format = kHdr;
            taa_state_[i] = D3D12_RESOURCE_STATE_RENDER_TARGET;
            if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, taa_state_[i], &cv,
                                                     IID_PPV_ARGS(&taa_[i])))) {
                taa_[0] = taa_[1] = nullptr;
                taa_w_ = taa_h_ = 0;
                return false;
            }
            dev_->CreateRenderTargetView(taa_[i].Get(), nullptr, Cpu(rtv_heap_.Get(), kTaaRtv + i, rtv_inc_));
        }
        taa_w_ = f.w;
        taa_h_ = f.h;
        taa_kept_ = false;
    }
    ID3D12PipelineState* gpso = EnsurePassPipeline(kTaaHlsl, "taa", kHdr, -1);
    ID3D12PipelineState* opso = EnsurePassPipeline(kTaaHlsl, "taaout", t.fmt, -1);
    if (!gpso || !opso) return false;
    const D3D12_RESOURCE_STATES kRead =
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    uint32_t prev = taa_cur_, cur = taa_cur_ ^ 1;
    ID3D12DescriptorHeap* heaps[] = {srv_heap_.Get(), smp_heap_.Get()};
    cl_->SetDescriptorHeaps(2, heaps);
    cl_->SetGraphicsRootSignature(root_.Get());
    Transition(taa_[prev].Get(), taa_state_[prev], kRead);
    Transition(taa_[cur].Get(), taa_state_[cur], D3D12_RESOURCE_STATE_RENDER_TARGET);
    auto views = [&](ID3D12Resource* history) {
        ID3D12Resource* src[5] = {f.color, f.motion, history, f.reactive, f.depth};
        const DXGI_FORMAT fmt[5] = {t.fmt, DXGI_FORMAT_R16G16_FLOAT, kHdr, DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R32_FLOAT};
        uint32_t slot = AllocSrvSlots(kMaterialViews);
        for (uint32_t i = 0; i < kMaterialViews; i++) {
            D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
            sv.Format = i < 5 ? fmt[i] : kBgra;
            sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sv.Texture2D.MipLevels = 1;
            dev_->CreateShaderResourceView(i < 5 ? src[i] : nullptr, &sv, Cpu(srv_heap_.Get(), slot + i, srv_inc_));
        }
        return slot;
    };
    bool keep = taa_kept_ && !f.reset;
    float near_first = f.view.near_w > f.view.far_w ? -1.0f : 1.0f;
    float p[16] = {float(f.w), float(f.h), 1.0f / float(f.w), 1.0f / float(f.h),
                   float(f.rw), float(f.rh), float(f.rw) / float(t.w), float(f.rh) / float(t.h),
                   f.jitter[0], f.jitter[1], keep ? 1.0f : 0.0f, 0.1f,
                   f.sharpness, f.reactive ? 1.0f : 0.0f, near_first, 0};
    Alloc cb = Upload(256, 256);
    std::memcpy(cb.cpu, p, sizeof(p));
    // gathered into the history not drawn last
    uint32_t slot = views(taa_[prev].Get());
    D3D12_CPU_DESCRIPTOR_HANDLE hh = Cpu(rtv_heap_.Get(), kTaaRtv + cur, rtv_inc_);
    cl_->OMSetRenderTargets(1, &hh, FALSE, nullptr);
    SetFullViewport(f.w, f.h);
    DrawFullScreen(gpso, cb.gpu, slot);
    // then drawn back into the frame
    Transition(taa_[cur].Get(), taa_state_[cur], kRead);
    Transition(t.res.Get(), t.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
    slot = views(taa_[cur].Get());
    D3D12_CPU_DESCRIPTOR_HANDLE th = Cpu(rtv_heap_.Get(), t.view, rtv_inc_);
    cl_->OMSetRenderTargets(1, &th, FALSE, nullptr);
    SetFullViewport(t.w, t.h);
    DrawFullScreen(opso, cb.gpu, slot);
    bound_rt_ = bound_ds_ = nullptr;
    taa_cur_ = cur;
    taa_kept_ = true;
    return true;
}

// Ambient occlusion with no anti-aliasing running: the frame's 3D depth copied as anti-aliasing's is (depthcopy, into
// aa_depth_), then AmbientOcclusion into the frame as it is, held still. Once a frame, at its UI's start or else before
// it is shown; a frame without the 3D's depth (a 2D screen's, a movie's) has none
bool D3D12Backend::OcclusionAlone(Target& t) {
    if (ao_done_frame_ == presents_) return false;
    ao_done_frame_ = presents_;
    Target* ds = fg_ds_frame_ == presents_ ? fg_ds_ : nullptr;
    if (!ds || ds->w != t.w || ds->h != t.h || t.fmt != kBgra || !EnsureAaTargets(t.w, t.h, t.fmt)) return false;
    ID3D12PipelineState* dpso = EnsurePassPipeline(kFrameGenHlsl, "depthcopy", DXGI_FORMAT_R32_FLOAT, -1);
    if (!dpso) return false;
    FrameView view;
    if (!(ext_ && ext_->View(view) && view.valid)) view = {};
    GpuMark(kGpuAo);
    ID3D12DescriptorHeap* heaps[] = {srv_heap_.Get(), smp_heap_.Get()};
    cl_->SetDescriptorHeaps(2, heaps);
    cl_->SetGraphicsRootSignature(root_.Get());
    Transition(ds->res.Get(), ds->state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    uint32_t slot = AllocSrvSlots(2);
    for (int i = 0; i < 2; i++) {
        D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = i == 0 ? kDepthRead : DXGI_FORMAT_R16G16_FLOAT;
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.Texture2D.MipLevels = 1;
        dev_->CreateShaderResourceView(i == 0 ? ds->res.Get() : nullptr, &sv, Cpu(srv_heap_.Get(), slot + i, srv_inc_));
    }
    Alloc cb = Upload(256, 256);
    float p[20] = {};
    p[0] = p[5] = p[10] = p[15] = 1;  // kFrameGenHlsl's m0 to m3, unused here
    p[18] = p[19] = 1;                // its opt.zw: the whole of it read
    std::memcpy(cb.cpu, p, sizeof(p));
    Transition(aa_depth_.Get(), aa_depth_state_, D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_CPU_DESCRIPTOR_HANDLE dh = Cpu(rtv_heap_.Get(), kAaRtv, rtv_inc_);
    cl_->OMSetRenderTargets(1, &dh, FALSE, nullptr);
    SetFullViewport(t.w, t.h);
    DrawFullScreen(dpso, cb.gpu, slot);
    bound_rt_ = bound_ds_ = nullptr;
    Transition(aa_depth_.Get(), aa_depth_state_,
               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    AmbientOcclusion(t, t.w, t.h, view.valid ? view : FrameView{}, false, true);
    Transition(t.res.Get(), t.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
    GpuMark(kGpuAfter);
    return true;
}

// Upscaling (g_upscale): the scene opens at the frame's first draw into the back buffer when the frame before was
// anti-aliased, a 3D frame: from then on what is drawn into the target, and its depth, goes into the top left part of
// it at the anti-aliasing's size for the quality (RenderSize), until the UI starts and the anti-aliasing's pass makes
// the part up to the whole target. A 2D screen's frames, and the first 3D frame after one, draw whole
void D3D12Backend::OpenScene(Target& rt) {
    if (scene_ || scene_frame_ == presents_) return;
    scene_frame_ = presents_;
    if (!scene_next_ || !aa_on_ || up_quality_ == framegen::kUpNative || rt.fmt != kBgra || !rt.w || !rt.h) return;
    uint32_t w = 0, h = 0;
    aa_->RenderSize(rt.w, rt.h, up_quality_, w, h);
    w = std::min(w, rt.w);
    h = std::min(h, rt.h);
    if (!w || !h || (w == rt.w && h == rt.h)) return;
    scene_ = &rt;
    scene_w_ = w;
    scene_h_ = h;
    scene_fx_ = float(w) / float(rt.w);
    scene_fy_ = float(h) / float(rt.h);
    scene_draws_ = scene_late_ = 0;
    binds_.rasterizer = false;
    if (w != scene_said_w_ || h != scene_said_h_) {
        LOGR("upscaling: the 3D drawn at %ux%u, made up to %ux%u by %s", w, h, rt.w, rt.h,
             AaName());
        scene_said_w_ = w;
        scene_said_h_ = h;
    }
}

Target* D3D12Backend::SceneDepth() const { return scene_ && fg_ds_frame_ == presents_ ? fg_ds_ : nullptr; }

Target* D3D12Backend::AfterDepth(Target& like) {
    if (!after_ds_.res || after_ds_.w != like.w || after_ds_.h != like.h) {
        if (after_ds_.res) {
            free_dsv_.push_back(after_ds_.view);
            Release(std::move(after_ds_.res));
        }
        after_ds_ = Target{};
        after_ds_.depth = true;
        after_ds_.gw = like.gw;
        after_ds_.gh = like.gh;
        after_ds_.w = like.w;
        after_ds_.h = like.h;
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC td = Texture2DDesc(kDepthTypeless, like.w, like.h);
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        D3D12_CLEAR_VALUE cv{};
        cv.Format = kDepthFormat;
        cv.DepthStencil.Depth = kDepthFar;
        after_ds_.state = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, after_ds_.state, &cv,
                                                 IID_PPV_ARGS(&after_ds_.res))) ||
            !CreateTargetView(after_ds_)) {  // cleared as it is made
            after_ds_ = Target{};
            return nullptr;
        }
        after_ds_frame_ = presents_;
        LOGR("upscaling: a depth of its own for pictures drawn apart with the frame's depth, %ux%u", like.w, like.h);
    }
    if (after_ds_frame_ != presents_) {
        // the frame's depth so far, drawn whole into it (as the scene's own is when it closes), so the copies are
        // hidden behind the room as they were; its stencil cleared
        if (!AfterDepthFill(like)) return nullptr;
        after_ds_frame_ = presents_;
    }
    return &after_ds_;
}

// the scene's depth (its part at the scene's size) drawn whole into after_ds_: copied out at the part's size, then
// stretched over it, as SceneDepthUp does into the scene's own; the renderer's state reset after (Begin)
bool D3D12Backend::AfterDepthFill(Target& like) {
    if (!scene_ || !EnsureAaTargets(scene_->w, scene_->h, scene_->fmt)) return false;
    ID3D12PipelineState* dpso = EnsurePassPipeline(kFrameGenHlsl, "depthcopy", DXGI_FORMAT_R32_FLOAT, -1);
    ID3D12PipelineState* zpso = EnsureDepthCopyPipeline();
    if (!dpso || !zpso) return false;
    Alloc cb = Upload(256, 256);
    float p[20] = {};
    p[0] = p[5] = p[10] = p[15] = 1;
    p[18] = scene_fx_;
    p[19] = scene_fy_;
    std::memcpy(cb.cpu, p, sizeof(p));
    Alloc zcb = Upload(256, 256);
    const float part[4] = {scene_fx_, scene_fy_, 0, 0};
    std::memcpy(zcb.cpu, part, sizeof(part));
    ID3D12DescriptorHeap* heaps[] = {srv_heap_.Get(), smp_heap_.Get()};
    cl_->SetDescriptorHeaps(2, heaps);
    cl_->SetGraphicsRootSignature(root_.Get());
    BindRtCapture();
    // the part's depth into the anti-aliasing's depth copy (it makes its own again when it runs)
    Transition(like.res.Get(), like.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Transition(aa_depth_.Get(), aa_depth_state_, D3D12_RESOURCE_STATE_RENDER_TARGET);
    uint32_t slot = AllocSrvSlots(4);
    for (int i = 0; i < 4; i++) {
        D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = i == 0 ? kDepthRead : i == 1 ? DXGI_FORMAT_R16G16_FLOAT : kBgra;
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.Texture2D.MipLevels = 1;
        dev_->CreateShaderResourceView(i == 0 ? like.res.Get() : nullptr, &sv, Cpu(srv_heap_.Get(), slot + i, srv_inc_));
    }
    D3D12_CPU_DESCRIPTOR_HANDLE dh = Cpu(rtv_heap_.Get(), kAaRtv, rtv_inc_);
    cl_->OMSetRenderTargets(1, &dh, FALSE, nullptr);
    SetFullViewport(scene_w_, scene_h_);
    DrawFullScreen(dpso, cb.gpu, slot);
    // stretched over the after-images' depth, cleared first for its stencil
    const D3D12_RESOURCE_STATES kRead =
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    Transition(aa_depth_.Get(), aa_depth_state_, kRead);
    Transition(after_ds_.res.Get(), after_ds_.state, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    D3D12_CPU_DESCRIPTOR_HANDLE zh = Cpu(dsv_heap_.Get(), after_ds_.view, dsv_inc_);
    cl_->ClearDepthStencilView(zh, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, kDepthFar, 0, 0, nullptr);
    uint32_t zslot = AllocSrvSlots(1);
    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = DXGI_FORMAT_R32_FLOAT;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    dev_->CreateShaderResourceView(aa_depth_.Get(), &sv, Cpu(srv_heap_.Get(), zslot, srv_inc_));
    cl_->OMSetRenderTargets(0, nullptr, FALSE, &zh);
    SetFullViewport(after_ds_.w, after_ds_.h);
    DrawFullScreen(zpso, zcb.gpu, zslot);
    Transition(like.res.Get(), like.state, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    Begin();
    return true;
}

bool D3D12Backend::SceneTarget(const Target* t) const { return scene_ && t && (t == scene_ || t == SceneDepth()); }

// upscaling's texture bias, in levels: at Quality the drawn size over the output's (log2), so the textures are as sharp
// as the whole size draws them; at Balanced half that, at Performance none. AMD's formula takes a level more
// (log2(render / display) - 1), but the reads are then most of the 3D's cost on a GPU that shares its memory: on the
// Radeon 890M the HD pack's textures and their parallax took 10.4 ms of a frame with half a level more, 9.6 as sharp
// as the whole size, 6.5 at the drawn size's own, at 1280x800 and 960x600 alike. Its guide warns too that fine, high
// contrast pictures shimmer with more, which the pack's stone and brick are
float D3D12Backend::SceneMipBias() const {
    if (!InScene() || !bound_rt_ || !bound_rt_->w || !scene_w_) return 0;
    float k = up_quality_ == framegen::kUpQuality ? 1.0f : up_quality_ == framegen::kUpBalanced ? 0.5f : 0.0f;
    return std::log2(float(scene_w_) / float(bound_rt_->w)) * k + diag::AaMipBias(0.0f);
}

bool D3D12Backend::InScene() const {
    if (!scene_) return false;
    return bound_rt_ ? bound_rt_ == scene_ : bound_ds_ && bound_ds_ == SceneDepth();
}

// the scene made whole: anti-aliased up to its target's size when its 3D took the offset (AntiAliasFrame), else its
// part stretched over it; its depth and motion too, so what is drawn after it lines up. why: closed before its UI
// started, for the log. The renderer's heaps, root signature and targets are set again after
void D3D12Backend::CloseScene(const char* why) {
    if (!scene_) return;
    Target& t = *scene_;
    if (why)
        LOGR_FIRST(8, "upscaling: frame %llu's scene made whole early, %s (after %u draws in it)",
                  (unsigned long long)presents_, why, scene_draws_);
    bool aa = aa_on_ && aa_jitter_frame_ == presents_ && aa_done_frame_ != presents_;
    if (!(aa && AntiAliasFrame(t))) {
        StretchScene(t);
        SceneDepthUp(t, false);
    }
    scene_ = nullptr;
    scene_closed_frame_ = presents_;
    Begin();
}

// the frame's UI starts: its scene closed, else its 3D anti-aliased at its size; true when passes were recorded,
// after which the draw's targets are bound again
bool D3D12Backend::FinishFrame3d(Target& t) {
    if (scene_) {
        CloseScene(nullptr);
        return true;
    }
    if (AntiAliasFrame(t)) {
        Begin();
        return true;
    }
    return false;
}

// the scene's part drawn over the whole target, filtered, from a copy of it: a frame whose scene had no 3D to
// anti-alias (a 2D screen's first frame after the 3D), or the pass did not run
void D3D12Backend::StretchScene(Target& t) {
    D3D12_RESOURCE_DESC rd = t.res->GetDesc();
    D3D12_RESOURCE_DESC have = scene_tmp_ ? scene_tmp_->GetDesc() : D3D12_RESOURCE_DESC{};
    if (!scene_tmp_ || have.Width != rd.Width || have.Height != rd.Height || have.Format != rd.Format) {
        if (scene_tmp_) Release(std::move(scene_tmp_));
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC td = Texture2DDesc(rd.Format, uint32_t(rd.Width), rd.Height);
        scene_tmp_state_ = D3D12_RESOURCE_STATE_COPY_DEST;
        if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, scene_tmp_state_, nullptr,
                                                 IID_PPV_ARGS(&scene_tmp_)))) {
            scene_tmp_ = nullptr;
            return;
        }
    }
    Transition(t.res.Get(), t.state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Transition(scene_tmp_.Get(), scene_tmp_state_, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION dl{scene_tmp_.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    D3D12_TEXTURE_COPY_LOCATION sl{t.res.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    D3D12_BOX b{0, 0, 0, scene_w_, scene_h_, 1};
    cl_->CopyTextureRegion(&dl, 0, 0, 0, &sl, &b);
    Transition(scene_tmp_.Get(), scene_tmp_state_, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Transition(t.res.Get(), t.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
    Begin();
    SetBlitTarget(Cpu(rtv_heap_.Get(), t.view, rtv_inc_), t.w, t.h);
    float uv[4] = {0, 0, scene_fx_, scene_fy_};
    Blit(scene_tmp_.Get(), rd.Format, uv, BlitOptions{.alpha = true, .rtv = t.fmt});
    bound_rt_ = bound_ds_ = nullptr;
}

// the scene's depth drawn over the whole of its depth buffer, and its motion (every pixel's) over the whole of the 3D
// draws' own, from the anti-aliasing's copies of them at the part's size (copied), else the depth copied for it here:
// what the frame draws after its scene, and frame generation's inputs, read them whole. The stencil is kept as drawn
void D3D12Backend::SceneDepthUp(Target& t, bool copied) {
    Target* ds = SceneDepth();
    if (!ds || ds->w != t.w || ds->h != t.h || !EnsureAaTargets(t.w, t.h, t.fmt)) return;
    const D3D12_RESOURCE_STATES kRead =
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    Alloc cb = Upload(256, 256);
    float p[20] = {};
    p[0] = p[5] = p[10] = p[15] = 1;  // kFrameGenHlsl's m0 to m3, unused here
    p[18] = scene_fx_;                // its opt.zw: the part read
    p[19] = scene_fy_;
    std::memcpy(cb.cpu, p, sizeof(p));
    Alloc zcb = Upload(256, 256);
    const float part[4] = {scene_fx_, scene_fy_, 0, 0};  // kDepthHlsl's
    std::memcpy(zcb.cpu, part, sizeof(part));
    ID3D12DescriptorHeap* heaps[] = {srv_heap_.Get(), smp_heap_.Get()};
    cl_->SetDescriptorHeaps(2, heaps);
    cl_->SetGraphicsRootSignature(root_.Get());
    BindRtCapture();
    auto views = [&](ID3D12Resource* depth, ID3D12Resource* mv) {
        uint32_t slot = AllocSrvSlots(4);
        for (int i = 0; i < 4; i++) {
            D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
            sv.Format = i == 0 ? (depth == aa_depth_.Get() ? DXGI_FORMAT_R32_FLOAT : kDepthRead)
                               : i == 1 ? DXGI_FORMAT_R16G16_FLOAT : kBgra;
            sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sv.Texture2D.MipLevels = 1;
            dev_->CreateShaderResourceView(i == 0 ? depth : i == 1 ? mv : nullptr, &sv, Cpu(srv_heap_.Get(), slot + i, srv_inc_));
        }
        return slot;
    };
    if (!copied) {
        ID3D12PipelineState* dpso = EnsurePassPipeline(kFrameGenHlsl, "depthcopy", DXGI_FORMAT_R32_FLOAT, -1);
        if (!dpso) return;
        Transition(ds->res.Get(), ds->state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Transition(aa_depth_.Get(), aa_depth_state_, D3D12_RESOURCE_STATE_RENDER_TARGET);
        uint32_t slot = views(ds->res.Get(), nullptr);
        D3D12_CPU_DESCRIPTOR_HANDLE dh = Cpu(rtv_heap_.Get(), kAaRtv, rtv_inc_);
        cl_->OMSetRenderTargets(1, &dh, FALSE, nullptr);
        SetFullViewport(scene_w_, scene_h_);
        DrawFullScreen(dpso, cb.gpu, slot);
    }
    ID3D12PipelineState* zpso = EnsureDepthCopyPipeline();
    if (zpso) {
        Transition(aa_depth_.Get(), aa_depth_state_, kRead);
        Transition(ds->res.Get(), ds->state, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        uint32_t slot = AllocSrvSlots(1);
        D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = DXGI_FORMAT_R32_FLOAT;
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.Texture2D.MipLevels = 1;
        dev_->CreateShaderResourceView(aa_depth_.Get(), &sv, Cpu(srv_heap_.Get(), slot, srv_inc_));
        D3D12_CPU_DESCRIPTOR_HANDLE dh = Cpu(dsv_heap_.Get(), ds->view, dsv_inc_);
        cl_->OMSetRenderTargets(0, nullptr, FALSE, &dh);
        SetFullViewport(ds->w, ds->h);
        DrawFullScreen(zpso, zcb.gpu, slot);
    }
    ID3D12PipelineState* mpso =
        copied && fg_objmv_ && fg_objmv_w_ == t.w && fg_objmv_h_ == t.h
            ? EnsurePassPipeline(kFrameGenHlsl, "mvwhole", DXGI_FORMAT_R16G16_FLOAT, -1)
            : nullptr;
    if (mpso) {
        Transition(aa_motion_.Get(), aa_motion_state_, kRead);
        Transition(fg_objmv_.Get(), fg_objmv_state_, D3D12_RESOURCE_STATE_RENDER_TARGET);
        uint32_t slot = views(nullptr, aa_motion_.Get());
        D3D12_CPU_DESCRIPTOR_HANDLE mh = Cpu(rtv_heap_.Get(), kFgObjRtv, rtv_inc_);
        cl_->OMSetRenderTargets(1, &mh, FALSE, nullptr);
        SetFullViewport(t.w, t.h);
        DrawFullScreen(mpso, cb.gpu, slot);
        fg_objmv_frame_ = presents_;
    }
    bound_rt_ = bound_ds_ = nullptr;
    binds_ = {};
}

// the inputs at the frame's size, the HUD-less frames at the back buffer's
void D3D12Backend::EnsureFrameGenTargets(uint32_t w, uint32_t h, DXGI_FORMAT bb) {
    if (fg_depth_ && fg_w_ == w && fg_h_ == h && fg_bb_w_ == bb_w_ && fg_bb_h_ == bb_h_ && fg_bb_fmt_ == bb) return;
    auto make = [&](ComPtr<ID3D12Resource>& r, D3D12_RESOURCE_STATES& st, DXGI_FORMAT f, uint32_t rw, uint32_t rh,
                    uint32_t rtv) {
        if (r) Release(std::move(r));
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC td = Texture2DDesc(f, rw, rh);
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_CLEAR_VALUE cv{};
        cv.Format = f;
        st = D3D12_RESOURCE_STATE_RENDER_TARGET;
        if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, st, &cv, IID_PPV_ARGS(&r)))) return;
        dev_->CreateRenderTargetView(r.Get(), nullptr, Cpu(rtv_heap_.Get(), rtv, rtv_inc_));
    };
    make(fg_depth_, fg_depth_state_, DXGI_FORMAT_R32_FLOAT, w, h, kFgRtv);
    make(fg_motion_, fg_motion_state_, DXGI_FORMAT_R16G16_FLOAT, w, h, kFgRtv + 1);
    for (uint32_t i = 0; i < kFrames; i++) make(fg_hudless_[i], fg_hudless_state_[i], bb, bb_w_, bb_h_, kFgRtv + 2 + i);
    fg_w_ = w;
    fg_h_ = h;
    fg_bb_w_ = bb_w_;
    fg_bb_h_ = bb_h_;
    fg_bb_fmt_ = bb;
}

void D3D12Backend::FrameGenInputs(Target* t, uint32_t bi, D3D12_CPU_DESCRIPTOR_HANDLE rh) {
    D3D12_VIEWPORT vp = FrameViewport();
    uint32_t vw = uint32_t(vp.Width), vh = uint32_t(vp.Height);
    DXGI_FORMAT bbf = BackFormat();
    EnsureFrameGenTargets(vw, vh, bbf);
    framegen::Inputs in;
    in.back = bb_[bi].Get();
    in.rect = {LONG(vp.TopLeftX), LONG(vp.TopLeftY), LONG(vp.TopLeftX) + LONG(vw), LONG(vp.TopLeftY) + LONG(vh)};
    in.hdr = hdr_;
    in.peak_nits = float(app::HdrPeak());
    auto now = std::chrono::steady_clock::now();
    in.frame_ms = float(std::chrono::duration<double, std::milli>(now - fg_t_).count());
    fg_t_ = now;
    FrameView view;
    bool have = ext_ && ext_->View(view) && view.valid;
    Target* ds = fg_ds_frame_ == presents_ ? fg_ds_ : nullptr;
    bool ok = have && t && ds && fg_depth_ && fg_motion_ && ds->w == t->w && ds->h == t->h;
    if (ok) {
        Transition(ds->res.Get(), ds->state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        ID3D12PipelineState* mpso = EnsurePassPipeline(kFrameGenHlsl, "motion", DXGI_FORMAT_R16G16_FLOAT, -1);
        ID3D12PipelineState* dpso = EnsurePassPipeline(kFrameGenHlsl, "depthcopy", DXGI_FORMAT_R32_FLOAT, -1);
        ok = mpso && dpso;
        if (ok) {
            // the 3D draws' own motion, when they drew it into this frame at its size
            bool own = fg_objmv_ && fg_objmv_frame_ == presents_ && fg_objmv_w_ == ds->w && fg_objmv_h_ == ds->h;
            if (own) Transition(fg_objmv_.Get(), fg_objmv_state_, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            if (own && !mv_said_) {
                LOGR("framegen: motion of each 3D draw (characters, the moving and the still world)");
                mv_said_ = true;
            }
            uint32_t slot = AllocSrvSlots(4);
            for (int i = 0; i < 4; i++) {
                D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
                sv.Format = i == 0 ? kDepthRead : i == 1 ? DXGI_FORMAT_R16G16_FLOAT : kBgra;
                sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                sv.Texture2D.MipLevels = 1;
                ID3D12Resource* r = i == 0 ? ds->res.Get() : i == 1 && own ? fg_objmv_.Get() : nullptr;
                dev_->CreateShaderResourceView(r, &sv, Cpu(srv_heap_.Get(), slot + i, srv_inc_));
            }
            Alloc cb = Upload(256, 256);
            std::memcpy(cb.cpu, view.clip_to_prev, sizeof(view.clip_to_prev));
            float opt[4] = {own ? 1.0f : 0.0f, 0, 1, 1};
            std::memcpy(cb.cpu + sizeof(view.clip_to_prev), opt, sizeof(opt));
            Transition(fg_motion_.Get(), fg_motion_state_, D3D12_RESOURCE_STATE_RENDER_TARGET);
            Transition(fg_depth_.Get(), fg_depth_state_, D3D12_RESOURCE_STATE_RENDER_TARGET);
            D3D12_CPU_DESCRIPTOR_HANDLE mh = Cpu(rtv_heap_.Get(), kFgRtv + 1, rtv_inc_);
            cl_->OMSetRenderTargets(1, &mh, FALSE, nullptr);
            SetFullViewport(vw, vh);
            DrawFullScreen(mpso, cb.gpu, slot);
            D3D12_CPU_DESCRIPTOR_HANDLE dh = Cpu(rtv_heap_.Get(), kFgRtv, rtv_inc_);
            cl_->OMSetRenderTargets(1, &dh, FALSE, nullptr);
            DrawFullScreen(dpso, cb.gpu, slot);
            Transition(fg_motion_.Get(), fg_motion_state_, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            Transition(fg_depth_.Get(), fg_depth_state_, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            in.depth = fg_depth_.Get();
            in.motion = fg_motion_.Get();
            in.view = view;
            static const bool show = GetEnvironmentVariableW(L"XBE_FG_SHOW_MOTION", nullptr, 0) > 0;
            ID3D12PipelineState* spso = show ? EnsurePassPipeline(kFrameGenHlsl, "showmotion", bbf, 0.6f) : nullptr;
            if (spso) {
                uint32_t ms = AllocSrvSlots(4);
                for (int i = 0; i < 4; i++) {
                    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
                    sv.Format = i == 1 ? DXGI_FORMAT_R16G16_FLOAT : kBgra;
                    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                    sv.Texture2D.MipLevels = 1;
                    dev_->CreateShaderResourceView(i == 1 ? fg_motion_.Get() : nullptr, &sv, Cpu(srv_heap_.Get(), ms + i, srv_inc_));
                }
                cl_->OMSetRenderTargets(1, &rh, FALSE, nullptr);
                D3D12_RECT scissor{0, 0, LONG(bb_w_), LONG(bb_h_)};
                cl_->RSSetViewports(1, &vp);
                cl_->RSSetScissorRects(1, &scissor);
                float f[4] = {0.6f, 0.6f, 0.6f, 0.6f};
                cl_->OMSetBlendFactor(f);
                binds_ = {};
                DrawFullScreen(spso, cb.gpu, ms);
            }
        }
    }
    // the offset in pixels of the size the 3D was drawn at, made the inputs' (the scene's depth made whole)
    if (aa_on_ && aa_jitter_frame_ == presents_ && aa_jitter_w_ && aa_jitter_h_) {
        in.jitter[0] = aa_jitter_[0] * float(vw) / float(aa_jitter_w_);
        in.jitter[1] = aa_jitter_[1] * float(vh) / float(aa_jitter_h_);
    }
    in.reset = !fg_had_view_ || view.cut || !ok;
    fg_had_view_ = ok;
    // XBE_FG_TRACE: why a frame had no inputs
    static const bool trace = GetEnvironmentVariableW(L"XBE_FG_TRACE", nullptr, 0) > 0;
    if (trace && !ok)
        LOGR("framegen: frame %llu no inputs: view %d, frame %d, depth %d (bound %llu), sizes %ux%u %ux%u",
             (unsigned long long)presents_, int(have), int(t != nullptr), int(fg_ds_ != nullptr),
             (unsigned long long)fg_ds_frame_, t ? t->w : 0, t ? t->h : 0, fg_ds_ ? fg_ds_->w : 0, fg_ds_ ? fg_ds_->h : 0);
    // the frame as it was before its UI, drawn as the frame was: what differs from it is the HUD and the menus
    uint32_t k = uint32_t(presents_ % kFrames);
    if (ok && ui_snap_ && ui_snap_frame_ == presents_ && ui_snap_data_ == t->data && fg_hudless_[k]) {
        Transition(fg_hudless_[k].Get(), fg_hudless_state_[k], D3D12_RESOURCE_STATE_RENDER_TARGET);
        D3D12_CPU_DESCRIPTOR_HANDLE hh = Cpu(rtv_heap_.Get(), kFgRtv + 2 + k, rtv_inc_);
        float black[4] = {0, 0, 0, 1};
        cl_->ClearRenderTargetView(hh, black, 0, nullptr);
        cl_->OMSetRenderTargets(1, &hh, FALSE, nullptr);
        D3D12_RECT scissor{0, 0, LONG(bb_w_), LONG(bb_h_)};
        cl_->RSSetViewports(1, &vp);
        cl_->RSSetScissorRects(1, &scissor);
        binds_ = {};
        Transition(ui_snap_.Get(), ui_snap_state_, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        float uv[4] = {0, 0, 1, 1};
        hdr_before_ = ui_snap_.Get();
        Blit(ui_snap_.Get(), t->fmt, uv, {.gamma = gamma_, .rtv = bbf, .hdr = hdr_ ? 1u : 0u});
        hdr_before_ = nullptr;
        Transition(fg_hudless_[k].Get(), fg_hudless_state_[k], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        in.hudless = fg_hudless_[k].Get();
    }
    fg_->Frame(cl_.Get(), in);
    // the renderer's heaps, root signature and the back buffer again, for the UI
    Begin();
    cl_->OMSetRenderTargets(1, &rh, FALSE, nullptr);
    SetFullViewport(bb_w_, bb_h_);
}

// HDR: the frame's average, as one texel the frame's pass reads
void D3D12Backend::MeasureFrame(Target& t) {
    auto make = [&](ComPtr<ID3D12Resource>& r, D3D12_RESOURCE_STATES& st, uint32_t n) {
        if (r) return;
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC td = Texture2DDesc(kBgra, n, n);
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_CLEAR_VALUE cv{};
        cv.Format = kBgra;
        st = D3D12_RESOURCE_STATE_RENDER_TARGET;
        dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, st, &cv, IID_PPV_ARGS(&r));
    };
    make(level16_, level16_state_, 16);
    make(level1_, level1_state_, 1);
    if (!level16_ || !level1_) return;
    if (Shrink(t.res.Get(), t.state, t.fmt, 16, 16, {.taps = 16, .into = level16_.Get(), .into_state = level16_state_}))
        level16_state_ = D3D12_RESOURCE_STATE_COPY_SOURCE;
    if (Shrink(level16_.Get(), level16_state_, kBgra, 1, 1, {.taps = 16, .into = level1_.Get(), .into_state = level1_state_}))
        level1_state_ = D3D12_RESOURCE_STATE_COPY_SOURCE;
    Transition(level1_.Get(), level1_state_, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

// the HDR shader's constants from the settings: scene white, the peak over it, the knee
void D3D12Backend::HdrConstants() {
    float scene = float(std::clamp(app::Get().hdr_scene_nits, 80u, 500u));
    float peak = std::max(float(app::HdrPeak()), scene);
    hdr_cb_[0] = scene / kScrgbNits;
    hdr_cb_[1] = peak / scene;
    hdr_cb_[2] = kHdrKnee;
}

// the first controller for ImGui's navigation (a game's settings menu), before its frame, on both platforms: SDL
// reads any controller, where the Win32 backend knows XInput's alone (a PlayStation or Switch pad did nothing in the
// menu). The D-pad and left stick move, A takes, B backs out; the other buttons are the game's to use (input::Raw), X
// and the shoulders left out so as not to start ImGui's window switching or tweak speeds, the right stick the game's
// camera under the menu
void UiPad() {
    input::Pad p = input::Raw(0);
    if (!p.connected) return;
    ImGuiIO& io = ImGui::GetIO();
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    io.AddKeyEvent(ImGuiKey_GamepadDpadUp, (p.buttons & 0x01) != 0);
    io.AddKeyEvent(ImGuiKey_GamepadDpadDown, (p.buttons & 0x02) != 0);
    io.AddKeyEvent(ImGuiKey_GamepadDpadLeft, (p.buttons & 0x04) != 0);
    io.AddKeyEvent(ImGuiKey_GamepadDpadRight, (p.buttons & 0x08) != 0);
    io.AddKeyEvent(ImGuiKey_GamepadFaceDown, p.analog[0] > 0x40);
    io.AddKeyEvent(ImGuiKey_GamepadFaceRight, p.analog[1] > 0x40);
    // a stick past a quarter of its throw, as far as it is beyond that
    auto stick = [&](ImGuiKey k, int v) {
        constexpr float kDead = 8192;
        io.AddKeyAnalogEvent(k, v > kDead, std::clamp((float(v) - kDead) / (32767 - kDead), 0.0f, 1.0f));
    };
    stick(ImGuiKey_GamepadLStickLeft, -int(p.lx));
    stick(ImGuiKey_GamepadLStickRight, p.lx);
    stick(ImGuiKey_GamepadLStickUp, p.ly);
    stick(ImGuiKey_GamepadLStickDown, -int(p.ly));
}

void D3D12Backend::RenderUi() {
    std::lock_guard lock(app::UiLock());
    ImGui_ImplDX12_NewFrame();
#ifdef _WIN32
    ImGui_ImplWin32_NewFrame();
#else
    linuxapp::ImGuiNewFrame();
#endif
    UiPad();
    ImGui::NewFrame();
    if (app::kGame.ui) app::kGame.ui(fps_);
    ImGui::Render();
    ID3D12DescriptorHeap* uh[] = {ui_heap_.Get()};
    cl_->SetDescriptorHeaps(1, uh);
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), cl_.Get());
}

// HDR: ImGui draws in SDR into a layer of its own, which goes over the frame at scene brightness, so the menus never
// take the highlights' brightness; nothing at all when ImGui draws nothing
void D3D12Backend::RenderUiHdr(D3D12_CPU_DESCRIPTOR_HANDLE rh) {
    std::lock_guard lock(app::UiLock());
    ImGui_ImplDX12_NewFrame();
#ifdef _WIN32
    ImGui_ImplWin32_NewFrame();
#else
    linuxapp::ImGuiNewFrame();
#endif
    UiPad();
    ImGui::NewFrame();
    if (app::kGame.ui) app::kGame.ui(fps_);
    ImGui::Render();
    ImDrawData* dd = ImGui::GetDrawData();
    if (!dd || dd->TotalVtxCount == 0) return;
    EnsureUiLayer();
    Transition(ui_.Get(), ui_state_, D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_CPU_DESCRIPTOR_HANDLE uh = Cpu(rtv_heap_.Get(), kUiRtv, rtv_inc_);
    float clear[4] = {0, 0, 0, 0};
    cl_->ClearRenderTargetView(uh, clear, 0, nullptr);
    cl_->OMSetRenderTargets(1, &uh, FALSE, nullptr);
    ID3D12DescriptorHeap* heaps[] = {ui_heap_.Get()};
    cl_->SetDescriptorHeaps(1, heaps);
    ImGui_ImplDX12_RenderDrawData(dd, cl_.Get());
    Begin();  // the renderer's heaps and root signature back for the pass
    Transition(ui_.Get(), ui_state_, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cl_->OMSetRenderTargets(1, &rh, FALSE, nullptr);
    SetFullViewport(bb_w_, bb_h_);
    float uv[4] = {0, 0, 1, 1};
    Blit(ui_.Get(), kBgra, uv, {.alpha = true, .rtv = HdrFormat(), .over = true, .hdr = 2});
}

// the UI layer at the back buffer's size, cleared to nothing
void D3D12Backend::EnsureUiLayer() {
    if (ui_ && ui_w_ == bb_w_ && ui_h_ == bb_h_) return;
    if (ui_) Release(std::move(ui_));
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC td = Texture2DDesc(kBgra, bb_w_, bb_h_);
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE cv{};
    cv.Format = kBgra;
    ui_state_ = D3D12_RESOURCE_STATE_RENDER_TARGET;
    dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, ui_state_, &cv, IID_PPV_ARGS(&ui_));
    dev_->CreateRenderTargetView(ui_.Get(), nullptr, Cpu(rtv_heap_.Get(), kUiRtv, rtv_inc_));
    ui_w_ = bb_w_;
    ui_h_ = bb_h_;
}

void D3D12Backend::DrawFrame(Target* t, uint32_t bi) {
    // a frame whose 3D took anti-aliasing's offset with no UI drawn after it: anti-aliased here, its scene closed
    if (scene_) {
        CloseScene(nullptr);
    } else if (t && aa_on_ && aa_jitter_frame_ == presents_ && aa_done_frame_ != presents_ && AntiAliasFrame(*t)) {
        Begin();
    } else if (t && ao_alone_ && OcclusionAlone(*t)) {
        Begin();
    }
    D3D12_RESOURCE_STATES bs = D3D12_RESOURCE_STATE_PRESENT;
    Transition(bb_[bi].Get(), bs, D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_CPU_DESCRIPTOR_HANDLE rh = Cpu(rtv_heap_.Get(), kRtvCount + bi, rtv_inc_);
    if (hdr_ && t) MeasureFrame(*t);
    float black[4] = {0, 0, 0, 1};
    cl_->ClearRenderTargetView(rh, black, 0, nullptr);
    cl_->OMSetRenderTargets(1, &rh, FALSE, nullptr);
    bound_rt_ = bound_ds_ = nullptr;
    if (hdr_) HdrConstants();
    if (t) BlitFrame(*t);
    if (fg_) FrameGenInputs(t, bi, rh);
    // a capture's frame as the screen shows it, before the port's own menus go over it
    if (!shot_dir_.empty()) {
        ScreenShot(bi, bs);
        shot_dir_.clear();
        Transition(bb_[bi].Get(), bs, D3D12_RESOURCE_STATE_RENDER_TARGET);
        cl_->OMSetRenderTargets(1, &rh, FALSE, nullptr);
        bound_rt_ = bound_ds_ = nullptr;
    }
    if (hdr_) {
        RenderUiHdr(rh);
        HdrReadBack(bi, bs);
    } else {
        RenderUi();
    }
    Transition(bb_[bi].Get(), bs, D3D12_RESOURCE_STATE_PRESENT);
}

namespace {

// a half float's value, from a table of all of them
float Half(uint16_t h) {
    static const std::vector<float> table = [] {
        std::vector<float> t(65536);
        for (uint32_t i = 0; i < 65536; i++) {
            uint32_t e = (i >> 10) & 31, m = i & 1023;
            float v = e == 0 ? std::ldexp(float(m), -24)
                    : e == 31 ? (m ? NAN : INFINITY)
                              : std::ldexp(float(m | 1024), int(e) - 25);
            t[i] = (i & 0x8000) ? -v : v;
        }
        return t;
    }();
    return table[h];
}

}  // namespace

// A capture's (StartCapture, the F10 menu's Capture this frame) picture as the screen shows it: the back buffer at the
// output's size, widescreen, ray tracing, upscaling and the HUD, as screen.bmp beside the console frame (final.bmp,
// 640x480 before all of that). HDR is brought to SDR: the scene's white (hdr_scene_nits) to white, the highlights
// above it eased in rather than cut
void D3D12Backend::ScreenShot(uint32_t bi, D3D12_RESOURCE_STATES& bs) {
    D3D12_RESOURCE_DESC rd = bb_[bi]->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
    UINT rows;
    UINT64 row_size, bytes;
    dev_->GetCopyableFootprints(&rd, 0, 1, 0, &fp, &rows, &row_size, &bytes);
    ComPtr<ID3D12Resource> rb;
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC bd = BufferDesc(bytes);
    if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                             IID_PPV_ARGS(&rb))))
        return;
    Transition(bb_[bi].Get(), bs, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION dl{rb.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    dl.PlacedFootprint = fp;
    D3D12_TEXTURE_COPY_LOCATION sl{bb_[bi].Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    cl_->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
    Flush();
    uint8_t* p = nullptr;
    if (FAILED(rb->Map(0, nullptr, reinterpret_cast<void**>(&p)))) return;
    const uint32_t w = uint32_t(rd.Width), h = rd.Height;
    const DXGI_FORMAT f = rd.Format;
    const float scene = std::max(hdr_cb_[0] * kScrgbNits, 80.0f);
    // HDR10's PQ code values to nits
    static const std::vector<float> pq = [] {
        std::vector<float> t(1024);
        for (int i = 0; i < 1024; i++) {
            double e = std::pow(i / 1023.0, 1 / 78.84375);
            t[i] = float(10000 * std::pow(std::max(e - 0.8359375, 0.0) / (18.8515625 - 18.6875 * e), 1 / 0.1593017578125));
        }
        return t;
    }();
    auto srgb = [](float v) {
        v = std::isfinite(v) ? std::max(v, 0.0f) : 0.0f;
        v = v < 0.8f ? v : 0.8f + 0.2f * (1 - std::exp(-(v - 0.8f) / 0.2f));  // the highlights eased toward white
        v = v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1 / 2.4f) - 0.055f;
        return uint8_t(std::clamp(v * 255 + 0.5f, 0.0f, 255.0f));
    };
    const uint32_t pitch = (w * 3 + 3) & ~3u;
    std::vector<uint8_t> out(size_t(pitch) * h);
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t* row = p + size_t(y) * fp.Footprint.RowPitch;
        uint8_t* o = out.data() + size_t(h - 1 - y) * pitch;  // a BMP's rows bottom up
        for (uint32_t x = 0; x < w; x++, o += 3) {
            if (f == DXGI_FORMAT_R16G16B16A16_FLOAT) {
                const uint16_t* v = reinterpret_cast<const uint16_t*>(row) + x * 4;
                float k = kScrgbNits / scene;
                o[0] = srgb(Half(v[2]) * k);
                o[1] = srgb(Half(v[1]) * k);
                o[2] = srgb(Half(v[0]) * k);
            } else if (f == DXGI_FORMAT_R10G10B10A2_UNORM) {
                uint32_t v = reinterpret_cast<const uint32_t*>(row)[x];
                float r = pq[v & 1023], g = pq[(v >> 10) & 1023], b = pq[(v >> 20) & 1023];
                // BT.2020 to BT.709
                float r7 = 1.6605f * r - 0.5876f * g - 0.0728f * b, g7 = -0.1246f * r + 1.1329f * g - 0.0083f * b,
                      b7 = -0.0182f * r - 0.1006f * g + 1.1187f * b;
                o[0] = srgb(b7 / scene);
                o[1] = srgb(g7 / scene);
                o[2] = srgb(r7 / scene);
            } else {  // B8G8R8A8, already as the screen shows it
                const uint8_t* v = row + x * 4;
                o[0] = v[0];
                o[1] = v[1];
                o[2] = v[2];
            }
        }
    }
    rb->Unmap(0, nullptr);
    if (FILE* fo = _wfopen((shot_dir_ / "screen.bmp").c_str(), L"wb")) {
        uint32_t size = 54 + uint32_t(out.size());
        uint8_t head[54] = {'B', 'M'};
        auto put = [&](int at, uint32_t v) { std::memcpy(head + at, &v, 4); };
        put(2, size);
        put(10, 54);
        put(14, 40);
        put(18, w);
        put(22, h);
        head[26] = 1;
        head[28] = 24;
        put(34, uint32_t(out.size()));
        std::fwrite(head, 1, sizeof(head), fo);
        std::fwrite(out.data(), 1, out.size(), fo);
        std::fclose(fo);
        LOGR("d3d12: the screen as shown, %ux%u, captured to %s", w, h, (shot_dir_ / "screen.bmp").string().c_str());
    }
}

// tests of the HDR output: XBE_HDR_STATS logs the frame's brightness every two seconds; with XBE_HDR_DUMP=<folder>,
// a file named dump there has the next frame written to that folder as hdr-<n>.raw (HDRF, width, height as 32 bit
// numbers, then each pixel's RGBA as half floats) and is deleted
void D3D12Backend::HdrReadBack(uint32_t bi, D3D12_RESOURCE_STATES& bs) {
    static const bool stats = GetEnvironmentVariableW(L"XBE_HDR_STATS", nullptr, 0) > 0;
    static const std::filesystem::path dir = [] {
        wchar_t b[MAX_PATH];
        DWORD n = GetEnvironmentVariableW(L"XBE_HDR_DUMP", b, MAX_PATH);
        return n && n < MAX_PATH ? std::filesystem::path(b) : std::filesystem::path();
    }();
    std::error_code ec;
    bool dump = !dir.empty() && presents_ % 15 == 0 && std::filesystem::exists(dir / "dump", ec);
    if (!dump && !(stats && presents_ % 120 == 60)) return;
    D3D12_RESOURCE_DESC rd = bb_[bi]->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
    UINT rows;
    UINT64 row_size;
    UINT64 bytes;
    dev_->GetCopyableFootprints(&rd, 0, 1, 0, &fp, &rows, &row_size, &bytes);
    ComPtr<ID3D12Resource> rb;
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC bd = BufferDesc(bytes);
    if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                             IID_PPV_ARGS(&rb)))) {
        return;
    }
    Transition(bb_[bi].Get(), bs, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION dl{rb.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    dl.PlacedFootprint = fp;
    D3D12_TEXTURE_COPY_LOCATION sl{bb_[bi].Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    cl_->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
    Flush();
    uint8_t* p = nullptr;
    if (FAILED(rb->Map(0, nullptr, reinterpret_cast<void**>(&p)))) return;
    uint32_t w = uint32_t(rd.Width), h = rd.Height;
    if (dump && hdr10_) {
        LOGR("hdr: frames are written as scRGB only; this one is HDR10");
        std::filesystem::remove(dir / "dump", ec);
        dump = false;
    }
    if (dump) {
        char name[48];
        snprintf(name, sizeof(name), "hdr-%llu.raw", (unsigned long long)presents_);
        if (FILE* f = _wfopen((dir / name).c_str(), L"wb")) {
            uint32_t head[3] = {0x46524448u /*HDRF*/, w, h};
            fwrite(head, 4, 3, f);
            for (uint32_t y = 0; y < h; y++) fwrite(p + size_t(y) * fp.Footprint.RowPitch, 8, w, f);
            fclose(f);
            LOGR("hdr: frame written to %s", (dir / name).string().c_str());
        }
        std::filesystem::remove(dir / "dump", ec);
    }
    // luminance in nits, a histogram by the nit
    std::vector<uint32_t> hist(10001);
    double sum = 0;
    uint64_t n = 0, bad = 0, below = 0, lifted = 0, bright = 0;
    float scene = hdr_cb_[0] * kScrgbNits, top = 0;
    // HDR10: PQ's code values back to nits, a table of the 1024
    static const std::vector<float> pq = [] {
        std::vector<float> t(1024);
        for (int i = 0; i < 1024; i++) {
            double e = std::pow(i / 1023.0, 1 / 78.84375);
            t[i] = float(10000 * std::pow(std::max(e - 0.8359375, 0.0) / (18.8515625 - 18.6875 * e), 1 / 0.1593017578125));
        }
        return t;
    }();
    for (uint32_t y = 0; y < h; y++) {
        const uint16_t* row = reinterpret_cast<const uint16_t*>(p + size_t(y) * fp.Footprint.RowPitch);
        const uint32_t* row10 = reinterpret_cast<const uint32_t*>(row);
        for (uint32_t x = 0; x < w; x++) {
            float nits;
            if (hdr10_) {
                uint32_t v = row10[x];
                nits = 0.2627f * pq[v & 1023] + 0.6780f * pq[(v >> 10) & 1023] + 0.0593f * pq[(v >> 20) & 1023];
            } else {
                float r = Half(row[x * 4]), g = Half(row[x * 4 + 1]), b = Half(row[x * 4 + 2]);
                if (!std::isfinite(r) || !std::isfinite(g) || !std::isfinite(b)) {
                    bad++;
                    continue;
                }
                if (r < 0 || g < 0 || b < 0) below++;
                nits = (0.2126f * r + 0.7152f * g + 0.0722f * b) * kScrgbNits;
            }
            top = std::max(top, nits);
            sum += nits;
            n++;
            lifted += nits > scene * 1.01f;
            bright += nits > 1000;
            hist[std::clamp(int(nits), 0, 10000)]++;
        }
    }
    rb->Unmap(0, nullptr);
    auto at = [&](double q) {
        uint64_t want = uint64_t(q * double(n)), seen = 0;
        for (int i = 0; i <= 10000; i++)
            if ((seen += hist[i]) > want) return i;
        return 10000;
    };
    LOGR("hdr: frame %ux%u, scene %.0f nits, peak %.0f | luminance mean %.0f, median %d, 99%% %d, 99.9%% %d, max %.0f "
         "nits | %.2f%% lifted past scene white, %.3f%% past 1000 nits | %llu not numbers, %llu below zero | UI copies %u",
         w, h, scene, scene * hdr_cb_[1], n ? sum / double(n) : 0.0, at(0.5), at(0.99), at(0.999), top,
         n ? 100.0 * double(lifted) / double(n) : 0.0, n ? 100.0 * double(bright) / double(n) : 0.0,
         (unsigned long long)bad, (unsigned long long)below, ui_snaps_shown_);
}

void D3D12Backend::ReleaseOldTextures() {
    constexpr uint64_t kSweepEvery = 60;    // presents between sweeps
    constexpr size_t kTexturesKept = 2048;  // no sweep below this many
    constexpr uint64_t kIdleFrames = 600;
    // textures unused for ten seconds go, levels stream theirs in and out
    if (presents_ % kSweepEvery != 0 || textures_.size() <= kTexturesKept) return;
    for (auto it = textures_.begin(); it != textures_.end();) {
        if (it->second.frame + kIdleFrames < presents_) {
            CurrentFrame().garbage.push_back(it->second.res);
            if (it->second.repl) CurrentFrame().garbage.push_back(it->second.repl);
            if (it->second.spare) CurrentFrame().garbage.push_back(it->second.spare);
            for (auto& m : it->second.mat)
                if (m) CurrentFrame().garbage.push_back(m);
            it = textures_.erase(it);
        } else {
            ++it;
        }
    }
}

void D3D12Backend::ResetFrame() {
    Frame& f = CurrentFrame();
    if (f.fence && fence_->GetCompletedValue() < f.fence) {
        auto perf_wait = PerfClock::now();
        WaitFence(f.fence);
        g_perf.wait += PerfMs(perf_wait);
    }
    f.alloc->Reset();
    f.garbage.clear();
    repl_budget_ = 2;
    f.used = 0;
    f.srv = 0;
    cl_->Reset(f.alloc.Get(), nullptr);
    rt_.Begin(uint32_t(frame_ % kFrames));
    late_pictures_[frame_ % kFrames].clear();
    // a scene this frame: ray traced ambient occlusion or reflections, which go with anti-aliasing's depth copy
    {
        uint32_t rt = g_ray_tracing.load(std::memory_order_relaxed);
        bool ao = (rt & kRtAmbientOcclusion) && g_ambient_occlusion.load(std::memory_order_relaxed);
        rt_frame_ = rt_.Ready() && aa_on_ && (ao || (rt & (kRtReflections | kRtLighting)));
    }
    Begin();
    GpuFrameStart();
}

void D3D12Backend::CountFps() {
    fps_n_++;
    auto now = std::chrono::steady_clock::now();
    double el = std::chrono::duration<double>(now - fps_t_).count();
    if (el >= 0.5) {
        fps_ = fps_n_ / el;
        fps_n_ = 0;
        fps_t_ = now;
        // with frame generation, the frames that reached the screen too: the game's and the generated ones
        if (fg_) {
            uint64_t shown = fg_->Shown();
            app::FrameGenState s;
            s.kind = fg_->Kind();
            s.active = fg_->Active();
            s.hdr10 = hdr10_ && hdr_;
            s.shown_fps = shown && fps_shown_from_ ? double(shown - fps_shown_from_) / el : 0;
            s.game_fps = fps_;
            fps_shown_from_ = shown;
            app::SetFrameGen(s);
            static uint32_t counts = 0;
            if (diag::Perf() && ++counts % 4 == 0)
                LOGR("framegen: %.1f frames a second shown, the game's %.1f%s", s.shown_fps, fps_,
                     s.active ? "" : " (not generating)");
            // more than one frame made for each of the game's, and the game held below its rate by it: the driver's
            // multi frame generation (the NVIDIA app's override), which DLSS-G's options do not hold back; once a run
            static uint32_t slow = 0;
            bool multi = s.active && fps_ > 1 && s.shown_fps > fps_ * 2.3 && fps_ < app::Get().fps_cap * 0.95;
            slow = multi ? slow + 1 : 0;
            if (slow == 6)
                LOGR("framegen: %.1f frames shown for each of the game's, which is at %.0f a second, not %u: the "
                     "NVIDIA app's DLSS override is making more than one frame (multi frame generation); set its "
                     "frame generation to 2x for this game",
                     s.shown_fps / fps_, fps_, app::Get().fps_cap);
        }
    }
}

void LogPerf() {
    static PerfClock::time_point perf_start = PerfClock::now();
    static uint64_t perf_frames = 0;
    perf_frames++;
    // XBE_HITCH_MS (34, two frames at 60): a frame that took longer, logged with what the renderer did in it (shaders
    // compiled or read from the disk cache, pipelines made, textures made and uploaded) and the rest of its time, the
    // game's own (a room loading)
    static const double kHitch = std::getenv("XBE_HITCH_MS") ? std::atof(std::getenv("XBE_HITCH_MS")) : 34.0;
    static PerfClock::time_point frame_start = PerfClock::now();
    static Perf before{};
    static uint64_t frame_no = 0;
    {
        double dt = PerfMs(frame_start);
        frame_start = PerfClock::now();
        const Perf& p = g_perf;
        double host = (p.draw + p.clear + p.copy + p.present + p.sync) -
                      (before.draw + before.clear + before.copy + before.present + before.sync);
        if (++frame_no > 2 && kHitch > 0 && dt > kHitch)
            LOGR("hitch: frame %llu took %.1f ms | renderer %.1f (compile %.1f: %llu shaders made, %llu from the disk "
                 "cache; pipelines %.1f: %llu made; textures %.1f: %llu made, %llu uploads %.2f MB; pack pictures "
                 "%.1f: %llu; ray tracing's scene %.1f; draws %llu) | the rest %.1f",
                 (unsigned long long)frame_no, dt, host, p.compile - before.compile,
                 (unsigned long long)(p.shaders - before.shaders),
                 (unsigned long long)(p.shaders_kept - before.shaders_kept), p.pso - before.pso,
                 (unsigned long long)(p.psos - before.psos), p.texup - before.texup,
                 (unsigned long long)(p.newtex - before.newtex), (unsigned long long)(p.texups - before.texups),
                 double(p.texup_bytes - before.texup_bytes) / 1048576.0, p.repl - before.repl,
                 (unsigned long long)(p.repls - before.repls), p.rt - before.rt,
                 (unsigned long long)(p.draws - before.draws), dt - host);
    }
    double el = PerfMs(perf_start);
    if (el >= 1000.0) {
        Perf& p = g_perf;
        double n = double(perf_frames);
        double host = p.draw + p.clear + p.copy + p.present + p.sync;
        // asked for with XBE_PERF, so written as a report: a release build keeps it, and it is in the file as
        // the second ends
        LOGR("perf: %.1f fps %.2f ms/frame | host %.2f (draw %.2f: assemble %.2f bind %.2f [check %.2f, upload %.2f] "
             "compile %.2f pso %.2f; clear %.2f copy %.2f sync %.2f present %.2f [swap %.2f wait %.2f]) other %.2f",
             n * 1000.0 / el, el / n, host / n, p.draw / n, p.assemble / n, p.bind / n, p.sig / n, p.texup / n,
             p.compile / n, p.pso / n, p.clear / n, p.copy / n, p.sync / n, p.present / n, p.swap / n, p.wait / n,
             (el - host) / n);
        LOGR("perf: title thread per frame: recording %.2f ms, waiting %.2f ms (%.1f waits: read-back %.2f "
             "resource %.2f fence %.2f flush %.2f copy %.2f, next frame %.2f) | read-backs waited for: %.1f lock, "
             "%.1f NOOVERWRITE",
             g_perf_capture_ns.exchange(0) / n / 1e6, g_perf_wait_ns.exchange(0) / n / 1e6,
             g_perf_waits.exchange(0) / n, g_perf_wait_kind_ns[0].exchange(0) / n / 1e6,
             g_perf_wait_kind_ns[1].exchange(0) / n / 1e6, g_perf_wait_kind_ns[2].exchange(0) / n / 1e6,
             g_perf_wait_kind_ns[3].exchange(0) / n / 1e6, g_perf_wait_kind_ns[4].exchange(0) / n / 1e6,
             g_perf_wait_kind_ns[5].exchange(0) / n / 1e6, g_perf_sync_block.exchange(0) / n,
             g_perf_sync_late.exchange(0) / n);
        LOGR("perf: per frame %.0f draws (%.0f raw) %.0fk verts %.1f MB upload %.1f overflow | %.0f checks %.2f MB "
             "hashed | %.1f tex uploads %.2f MB, %.1f new tex, %.1f new targets, %.1f target uploads, %.1f shrinks, "
             "%.1f read-backs %.1f late | %llu shaders compiled %llu kept %llu psos this second | %.1f copies of a target "
             "drawn into (%.0f MB)",
             p.draws / n, p.raw / n, p.verts / n / 1000.0, p.upload / n / 1048576.0, p.overflow / n, p.sigs / n,
             p.sig_bytes / n / 1048576.0, p.texups / n, p.texup_bytes / n / 1048576.0, p.newtex / n,
             p.targets / n, p.rtups / n, p.shrinks / n, p.syncs / n, p.lates / n,
             (unsigned long long)p.shaders, (unsigned long long)p.shaders_kept, (unsigned long long)p.psos,
             p.feedbacks / n, p.feedback_bytes / n / 1048576.0);
        LOGR("perf: draw steps per frame: targets %.2f, shaders %.2f, pipeline %.2f, constants %.2f, views %.2f, "
             "geometry %.2f ms | %.0f barriers, %.0f pipeline changes",
             p.lap[0] / n, p.lap[1] / n, p.lap[2] / n, p.lap[3] / n, p.lap[4] / n, p.lap[5] / n, p.barriers / n,
             p.pso_sets / n);
        LOGR("perf: constants per frame: the frame before's found %.2f, uploaded %.2f, extension %.2f, kept %.2f, "
             "the rest %.2f ms",
             p.clap[0] / n, p.clap[1] / n, p.clap[2] / n, p.clap[3] / n, p.clap[4] / n);
        if (p.gpu_frames) {
            double g = double(p.gpu_frames);
            LOGR("perf: GPU per frame %.2f ms: 3D %.2f, motion and depth %.2f, ambient occlusion %.2f, "
                 "anti-aliasing %.2f, after it (UI, output) %.2f | the ray tracing's parts: scene and guides %.2f, "
                 "ambient occlusion %.2f, bounce lighting %.2f, reflections and water %.2f, light shafts %.2f",
                 p.gpu[kGpuParts] / g, p.gpu[0] / g, p.gpu[1] / g,
                 (p.gpu[2] + p.gpu[3] + p.gpu[4] + p.gpu[5] + p.gpu[6]) / g, p.gpu[7] / g, p.gpu[8] / g, p.gpu[2] / g,
                 p.gpu[3] / g, p.gpu[4] / g, p.gpu[5] / g, p.gpu[6] / g);
        }
        g_perf = Perf{};
        perf_start = PerfClock::now();
        perf_frames = 0;
    }
    before = g_perf;
}

bool D3D12Backend::GpuTiming() const {
    return diag::Perf() || (g_ray_tracing.load(std::memory_order_relaxed) & kRtLighting);
}

float D3D12Backend::RtBudgetRays(float full) {
    // XBE_RT_BUDGET: the ray tracing's share of a frame, in ms (0: the rays as the quality has them); by default 36%
    // of a frame at the frame rate cap (60 when uncapped or at the monitor's): 6 ms at 60. The save room (5.27), its
    // water's glow over the floor gathered by every bounce ray, took 10 ms a frame of it at 4K where the rooms around
    // took 3 to 4, and fell under 50 frames a second
    static const float kSet = std::getenv("XBE_RT_BUDGET") ? float(std::atof(std::getenv("XBE_RT_BUDGET"))) : -1;
    uint32_t cap = app::Get().fps_cap;
    float budget = kSet >= 0 ? kSet : 0.36f * 1000.0f / float(cap > app::kFpsCapRefresh ? std::max(cap, 30u) : 60u);
    if (budget <= 0 || full < 2 || rt_ms_ <= 0) return full;
    const uint32_t most = full >= 4 ? 1 : 0;  // halved once at most: four rays to two (one ray's light crawls)
    if (presents_ >= gi_budget_frame_ + 90) {
        // the bounce lighting's rays doubled would take about its time again
        if (rt_ms_ > budget && gi_halved_ < most) {
            gi_halved_++;
            gi_budget_frame_ = presents_;
            LOGR_FIRST(16, "ray tracing: %.1f ms a frame, over its %.1f ms: the bounce lighting at %.0f rays a pixel",
                      rt_ms_, double(budget), double(full) / double(1u << gi_halved_));
        } else if (gi_halved_ > 0 && rt_ms_ + gi_ms_ < 0.85 * budget) {
            gi_halved_--;
            gi_budget_frame_ = presents_;
            LOGR_FIRST(16, "ray tracing: %.1f ms a frame, room in its %.1f ms: the bounce lighting at %.0f rays a pixel",
                      rt_ms_, double(budget), double(full) / double(1u << gi_halved_));
        }
    }
    return std::max(1.0f, full / float(1u << gi_halved_));
}

void D3D12Backend::GpuMark(uint32_t mark) {
    if (!GpuTiming() || ts_failed_) return;
    if (!ts_heap_) {
        D3D12_QUERY_HEAP_DESC qd{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, kFrames * kGpuMarks, 0};
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_READBACK};
        D3D12_RESOURCE_DESC bd = BufferDesc(uint64_t(kFrames) * kGpuMarks * sizeof(uint64_t));
        uint64_t freq = 0;
        void* p = nullptr;
        bool made = SUCCEEDED(dev_->CreateQueryHeap(&qd, IID_PPV_ARGS(&ts_heap_))) &&
                    SUCCEEDED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
                                                            D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                            IID_PPV_ARGS(&ts_rb_))) &&
                    SUCCEEDED(queue_->GetTimestampFrequency(&freq)) && freq && SUCCEEDED(ts_rb_->Map(0, nullptr, &p));
        if (!made) {
            LOGR("perf: no GPU timestamps");
            ts_failed_ = true;
            ts_heap_.Reset();
            return;
        }
        ts_map_ = static_cast<uint64_t*>(p);
        ts_ms_ = 1000.0 / double(freq);
    }
    uint32_t slot = uint32_t(frame_ % kFrames);
    // a part the frame skipped (no anti-aliasing: its motion and depth, its vendor's pass) takes no time: marked here,
    // where the next part starts, so the marks stay in order (marked at the frame's end, after a later part's, they
    // were not, and the frame went uncounted)
    for (uint32_t m = mark > kGpuStart ? kGpuStart + 1 : mark; m < mark; m++)
        if (!(ts_marked_[slot] & 1u << m)) {
            cl_->EndQuery(ts_heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * kGpuMarks + m);
            ts_marked_[slot] |= 1u << m;
        }
    cl_->EndQuery(ts_heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * kGpuMarks + mark);
    ts_marked_[slot] |= 1u << mark;
}

void D3D12Backend::GpuFrameEnd() {
    if (!GpuTiming() || !ts_heap_) return;
    uint32_t slot = uint32_t(frame_ % kFrames);
    // a frame begun before the timestamps were: not counted
    bool whole = ts_marked_[slot] & 1u;
    if (!whole) GpuMark(kGpuStart);
    // the parts the frame did not have (no anti-aliasing) take no time: marked at its end
    for (uint32_t m = kGpuStart + 1; m < kGpuMarks; m++)
        if (!(ts_marked_[slot] & 1u << m)) GpuMark(m);
    cl_->ResolveQueryData(ts_heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * kGpuMarks, kGpuMarks, ts_rb_.Get(),
                          uint64_t(slot) * kGpuMarks * sizeof(uint64_t));
    ts_resolved_[slot] = whole;
}

void D3D12Backend::GpuFrameStart() {
    if (!GpuTiming()) return;
    uint32_t slot = uint32_t(frame_ % kFrames);
    if (ts_resolved_[slot] && ts_map_) {
        const uint64_t* t = ts_map_ + slot * kGpuMarks;
        bool ordered = true;
        for (uint32_t m = 1; m < kGpuMarks; m++) ordered = ordered && t[m] >= t[m - 1];
        if (ordered && diag::Perf()) {
            for (uint32_t m = 0; m < kGpuParts; m++) g_perf.gpu[m] += double(t[m + 1] - t[m]) * ts_ms_;
            g_perf.gpu[kGpuParts] += double(t[kGpuMarks - 1] - t[0]) * ts_ms_;
            g_perf.gpu_frames++;
        }
        // the ray tracing's time, eased over some 20 frames (a frame without it, a 2D screen's, not counted)
        double rt = ordered ? double(t[kGpuVendor] - t[kGpuAo]) * ts_ms_ : 0;
        double gi = ordered ? double(t[kGpuRtGi] - t[kGpuRtAo]) * ts_ms_ : 0;
        if (rt > 0.05) {
            rt_ms_ = rt_ms_ > 0 ? rt_ms_ * 0.95 + rt * 0.05 : rt;
            gi_ms_ = gi_ms_ > 0 ? gi_ms_ * 0.95 + gi * 0.05 : gi;
        }
    }
    ts_resolved_[slot] = false;
    ts_marked_[slot] = 0;
    GpuMark(kGpuStart);
}

// the device lost (said once): Windows' reason, and with XBE_D3D12_DRED the GPU's last steps and the fault's address
void D3D12Backend::DeviceLost() {
    if (lost_said_) return;
    lost_said_ = true;
    LOGE("d3d12: the device was lost (0x%08lx)", (unsigned long)dev_->GetDeviceRemovedReason());
#ifdef _WIN32
    ComPtr<ID3D12DeviceRemovedExtendedData> dred;
    if (FAILED(dev_.As(&dred))) return;
    D3D12_DRED_PAGE_FAULT_OUTPUT pf{};
    if (SUCCEEDED(dred->GetPageFaultAllocationOutput(&pf)))
        LOGE("d3d12: it faulted at GPU address 0x%llx", (unsigned long long)pf.PageFaultVA);
    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT bc{};
    if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput(&bc))) {
        for (const D3D12_AUTO_BREADCRUMB_NODE* n = bc.pHeadAutoBreadcrumbNode; n; n = n->pNext) {
            uint32_t done = n->pLastBreadcrumbValue ? *n->pLastBreadcrumbValue : 0;
            if (done >= n->BreadcrumbCount) continue;  // a list that finished
            LOGE("d3d12: a command list stopped after %u of its %u steps; the next: %d, the ones before: %d %d %d", done,
                 n->BreadcrumbCount, int(n->pCommandHistory[done]), done > 0 ? int(n->pCommandHistory[done - 1]) : -1,
                 done > 1 ? int(n->pCommandHistory[done - 2]) : -1, done > 2 ? int(n->pCommandHistory[done - 3]) : -1);
        }
    }
#endif
}

void D3D12Backend::Present(const Surface& front) {
    PerfScope perf_present{g_perf.present};
#ifdef __ANDROID__
    // The activity can replace the native window while backgrounded: make the Vulkan surface and
    // swap chain again before drawing. If the new surface is not ready yet, skip this frame and
    // try again; the game keeps running behind the splash/transition.
    bool force_surface = g_surface_dirty.exchange(false);
    if (force_surface || swap_->SurfaceChanged()) {
        if (!swap_->RecreateSurface(force_surface)) {
            g_surface_dirty = true;
            return;
        }
        for (ComPtr<ID3D12Resource>& b : bb_) b.Reset();
        CreateBackBufferViews();
    }
#endif
    if (cap_) {
        FinishCapture(front);
        shot_dir_ = cap_dir_;
    } else if (g_capture.exchange(false)) {
        // F3 with XBE_CAPTURE_DIR, or a menu's button anywhere (into captures\)
        if (g_capture_asked.exchange(false) || CaptureOn()) StartCapture();
    }
    ResizeSwap();
    Target* t = EnsureTarget(front, false);
    if (t && LatchCpuWrite(*t)) UploadTarget(*t, front);
    if (t) Resolve(*t);
    if (ramp_dirty_) UploadGammaRamp();
    Transition(lut_.Get(), lut_state_, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    uint32_t bi = swap_->GetCurrentBackBufferIndex();
    DrawFrame(t, bi);
    GpuFrameEnd();
    Submit(false);
    present_fence_[presents_ % (kFrames + 1)].store(fence_value_);
    DeliverReports();
    bool vsync = app::Get().vsync;
    auto perf_swap = PerfClock::now();
    // the game sped up (app::Speed): one frame of each so many reaches the screen, the others drawn and let go, so
    // the screen's refresh (vsync) does not hold the game to its own speed
    uint32_t speed = app::Speed();
    bool shown = speed <= 1 || ++sped_frames_ % speed == 0;
    if (shown) {
        if (fg_) fg_->PresentStart();
#ifdef _WIN32
        if (vsync && fg_ && !FrameGenVsync()) vsync = false;
        HRESULT presented = swap_->Present(vsync ? 1 : 0, !vsync && tearing_ ? DXGI_PRESENT_ALLOW_TEARING : 0);
        if (presented == DXGI_ERROR_DEVICE_REMOVED || presented == DXGI_ERROR_DEVICE_RESET) DeviceLost();
#else
        swap_->Present(vsync ? 1 : 0);
#endif
        if (fg_) fg_->PresentEnd();
    }
#ifdef __ANDROID__
    // The Java splash stays up until the game has actually put a frame on the screen.
    if (presents_ == 0) noble_android::SetBooted();
#endif
    if (aa_dlss_) aa_dlss_->FrameDone();
    g_perf.swap += PerfMs(perf_swap);

    // upscaling: the next frame's 3D drawn in a scene when this frame's was anti-aliased (a 3D frame, not a 2D
    // screen's); the first few frames' scenes in the log, to see the UI start where it should
    scene_next_ = aa_on_ && aa_ran_frame_ == presents_ && up_quality_ != framegen::kUpNative;
    if (scene_closed_frame_ == presents_ && scene_logged_ < 3) {
        scene_logged_++;
        LOGR("upscaling: frame %llu, %u draws in the scene, %u 3D draws after it at the whole size",
             (unsigned long long)presents_, scene_draws_, scene_late_);
    }
    {
        static const bool kCharLog = std::getenv("XBE_RT_CHAR_LOG") != nullptr;
        if (kCharLog && (char_late_ || char_nodepth_ || char_other_ || char_faded_ || char_copies_ ||
                         (char_prev_ && !char_in_) || (!char_prev_ && char_in_)))
            LOGR("rt chars: frame %llu: %u in the scene (frame before %u; %u of them writing no depth), %u after the 3D "
                 "was anti-aliased, %u writing no depth left out (%u copies), %u else",
                 (unsigned long long)presents_, char_in_, char_prev_, char_faded_, char_late_, char_nodepth_,
                 char_copies_, char_other_);
        char_prev_ = char_in_;
        char_in_ = char_late_ = char_nodepth_ = char_other_ = char_faded_ = char_copies_ = 0;
        char_meshes_.clear();
        MemoryCheck();
    }
    scene_draws_ = scene_late_ = 0;
    rx_draws_ = 0;
    presents_++;
    presents_seen_.store(presents_);
    frame_++;
    ReleaseOldTextures();
    ResetFrame();
    CountFps();
    if (diag::Perf()) LogPerf();
    SavePipelineCache(false);
    ApplySettings();
}

// the slot is not reused before the caller's wait: the title runs at most one present ahead
void D3D12Backend::WaitGpuFrame(uint64_t n) {
    uint64_t fence = present_fence_[n % (kFrames + 1)].load();
    if (fence > fence_->GetCompletedValue()) fence_->SetEventOnCompletion(fence, nullptr);
}

void D3D12Backend::ApplySettings() {
    if (texturedumper::Update()) ReleasePackTextures();
    // anti-aliasing: the kind chosen, its files loaded the first time; NVIDIA DLSS only with Streamline loaded for it
    // as the game started, AMD FSR in its place until the next start
    uint32_t kind = g_antialias.load(std::memory_order_relaxed);
    framegen::AntiAlias* pick = nullptr;
    if (kind == framegen::kAaDlss) {
        if (!aa_dlss_ && !aa_dlss_tried_) {
            aa_dlss_tried_ = true;
#ifdef _WIN32
            aa_dlss_ = framegen::MakeAntiAlias(dev_.Get(), framegen::kAaDlss);
#else
            aa_dlss_ = linux_dlss::MakeDlss(dev_.Get(), queue_.Get());
#endif
        }
        pick = aa_dlss_.get();
    }
    if (kind == framegen::kAaFsr || (kind == framegen::kAaDlss && !pick)) {
        if (!aa_fsr_ && !aa_fsr_tried_) {
            aa_fsr_tried_ = true;
#ifdef _WIN32
            aa_fsr_ = OwnTaaChosen() ? framegen::MakeOwnTaa() : framegen::MakeAntiAlias(dev_.Get(), framegen::kAaFsr);
#else
            const char* own = std::getenv("XBE_TAA");
            if (!(own && *own == '1')) aa_fsr_ = linux_fsr::MakeFsr(dev_.Get(), queue_.Get());
            if (!aa_fsr_) aa_fsr_ = framegen::MakeOwnTaa();
#endif
        }
        pick = aa_fsr_.get();
    }
    if (pick != aa_) {
        if (pick)
            LOGR("antialias: %s%s", AaName(pick),
                 kind == framegen::kAaDlss && pick->Kind() != framegen::kAaDlss ? ", NVIDIA DLSS not running" : "");
        aa_ = pick;
        aa_had_view_ = aa_said_ = scene_next_ = false;
    }
    aa_on_ = aa_ != nullptr;
    ao_alone_ = !aa_on_ && g_ambient_occlusion.load(std::memory_order_relaxed) > 0;
    up_quality_ = std::min<uint32_t>(g_upscale.load(std::memory_order_relaxed), framegen::kUpPerformance);
    g_antialias_running.store(aa_ ? aa_->Kind() : 0, std::memory_order_relaxed);
    if (!aa_on_) g_antialias_drawn.store(0, std::memory_order_relaxed);
    float want = app::RenderScale(guest_h_, kind == framegen::kAaDlss), aspect = TargetAspect();
    bool remake = ext_ && ext_->Settings();
    if (std::fabs(want - scale_) > 1e-4f || std::fabs(aspect - aspect_) > 1e-4f || remake) {
        Flush();
        for (auto& [key, tg] : targets_) (tg.depth ? free_dsv_ : free_rtv_).push_back(tg.view);
        targets_.clear();
        scale_ = want;
        aspect_ = aspect;
        if (ext_) ext_->Reset(*this);
        bound_rt_ = bound_ds_ = nullptr;
    }
}

// the target resolved, then shrunk with o when it is not guest sized BGRA; res null on failure
D3D12Backend::CopySource D3D12Backend::PrepareCopySource(Target& t, const ShrinkOptions& o) {
    if (SceneTarget(&t)) CloseScene("the CPU reads the frame");
    Resolve(t);
    CopySource out{t.res.Get(), nullptr};
    if (t.w != t.gw || t.h != t.gh || t.fmt != kBgra) {
        out.down = Shrink(t.res.Get(), t.state, t.fmt, t.gw, t.gh, o);
        out.res = out.down.Get();
    } else {
        Transition(t.res.Get(), t.state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    }
    return out;
}

// long unused: the title freed it and other data is at this address now
bool D3D12Backend::TargetStale(uint32_t data) const {
    auto it = target_used_.find(data);
    uint64_t used = it == target_used_.end() ? 0 : it->second;
    return frame_ - used > kStaleFrames;
}

// the CPU reads a surface the GPU drew: bring the pixels back
void D3D12Backend::Sync(uint32_t data, uint32_t) {
    auto it = targets_.find(TargetMapKey(data, false));
    if (it == targets_.end() || !it->second.drawn || TargetStale(data)) return;
    PerfScope perf_sync{g_perf.sync};
    g_perf.syncs++;
    if (diag::Perf()) {
        LOG_FIRST(16, "perf: read-back of %08X (%ux%u guest, %ux%u host)", data, it->second.gw, it->second.gh,
                  it->second.w, it->second.h);
    }
    Target& t = it->second;
    // src.down, when there is one, stays alive until ReadBack is done
    CopySource src = PrepareCopySource(t, {});
    if (!src.res) return;
    std::vector<uint8_t> px;
    if (!ReadBack(src.res, t.gw, t.gh, px)) return;
    // color targets are 32 bit linear in guest memory, the pitch rounds to 64 bytes
    uint32_t pitch = GuestPitch(t.gw);
    uint8_t* guest = static_cast<uint8_t*>(kernel::HostPtr(data));
    for (uint32_t y = 0; y < t.gh; y++) std::memcpy(guest + size_t(y) * pitch, &px[size_t(y) * t.gw * 4], t.gw * 4);
    t.read = std::move(px);
    t.drawn = false;
    t.sig = MemSig(t.data, t.bytes);
}

// A NOOVERWRITE lock: the console reads whatever the GPU has finished, so a surface locked every frame gets the newest
// finished copy (a frame or two old) and the CPU never waits on the GPU; one locked now and then still waits in Sync
void D3D12Backend::SyncLate(uint32_t data) {
    if (!DeliverLate(data)) Sync(data, 0);
    RecordLate(data);
}

bool D3D12Backend::DeliverLate(uint32_t data) {
    std::lock_guard lock(late_lock_);
    auto it = late_.find(data);
    if (it == late_.end()) return false;
    Late& l = it->second;
    uint64_t done = fence_->GetCompletedValue();
    uint64_t now = presents_seen_.load();
    int best = -1;
    for (int i = 0; i < kFrames; i++) {
        bool finished = l.fence[i] && l.fence[i] <= done;
        bool recent = now - l.frame[i] <= uint64_t(kFrames) + 1;
        bool newest = best < 0 || l.fence[i] > l.fence[best];
        if (finished && recent && newest) best = i;
    }
    if (best < 0) {
        if (diag::Perf()) {
            LOG_FIRST(16, "perf: late %08X nothing finished: done %llu now %llu, slots %llu/%llu %llu/%llu %llu/%llu",
                      data, (unsigned long long)done, (unsigned long long)now, (unsigned long long)l.fence[0],
                      (unsigned long long)l.frame[0], (unsigned long long)l.fence[1], (unsigned long long)l.frame[1],
                      (unsigned long long)l.fence[2], (unsigned long long)l.frame[2]);
        }
        return false;
    }
    if (l.fence[best] > l.delivered) {
        uint8_t* p = nullptr;
        // the last row is not padded to the pitch: a range to the end of the pitch fails to map
        D3D12_RANGE rr{0, size_t(l.row_pitch) * (l.gh - 1) + l.gw * 4};
        if (FAILED(l.rb[best]->Map(0, &rr, reinterpret_cast<void**>(&p)))) return false;
        uint32_t pitch = GuestPitch(l.gw);
        uint8_t* dst = static_cast<uint8_t*>(kernel::HostPtr(data));
        for (uint32_t y = 0; y < l.gh; y++) std::memcpy(dst + size_t(y) * pitch, p + size_t(y) * l.row_pitch, l.gw * 4);
        D3D12_RANGE none{0, 0};
        l.rb[best]->Unmap(0, &none);
        l.delivered = l.fence[best];
    }
    return true;
}

// a slot set for another size: its buffers go with the frame
void D3D12Backend::ResetLate(Late& l, const Target& t) {
    auto& garbage = CurrentFrame().garbage;
    for (auto& r : l.rb) {
        if (r) garbage.push_back(r);
    }
    if (l.down) garbage.push_back(l.down);
    l = Late{};
    l.gw = t.gw;
    l.gh = t.gh;
}

// the target as a guest sized BGRA copy source, shrunk into l.down when it is not one already
ID3D12Resource* D3D12Backend::PrepareLateSource(Target& t, Late& l) {
    ShrinkOptions reuse{.into = l.down.Get(), .into_state = l.down_state};
    CopySource src = PrepareCopySource(t, reuse);
    if (src.down) {
        l.down = src.down;
        l.down_state = D3D12_RESOURCE_STATE_COPY_SOURCE;
    }
    return src.res;
}

// src into the readback buffer of the slot, made on first use
void D3D12Backend::RecordLateCopy(Late& l, uint32_t slot, ID3D12Resource* src) {
    D3D12_RESOURCE_DESC rd = src->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
    UINT rows;
    UINT64 row_size;
    UINT64 bytes;
    dev_->GetCopyableFootprints(&rd, 0, 1, 0, &fp, &rows, &row_size, &bytes);
    if (!l.rb[slot]) {
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_READBACK};
        D3D12_RESOURCE_DESC bd = BufferDesc(bytes);
        if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST,
                                                 nullptr, IID_PPV_ARGS(&l.rb[slot])))) {
            return;
        }
    }
    D3D12_TEXTURE_COPY_LOCATION dl{l.rb[slot].Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    dl.PlacedFootprint = fp;
    D3D12_TEXTURE_COPY_LOCATION sl{src, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    cl_->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
    l.row_pitch = fp.Footprint.RowPitch;
    l.fence[slot] = fence_value_ + 1;  // the next Submit carries it
    l.frame[slot] = presents_;
    l.next = (slot + 1) % kFrames;
}

void D3D12Backend::RecordLate(uint32_t data) {
    auto it = targets_.find(TargetMapKey(data, false));
    if (it == targets_.end() || TargetStale(data)) return;
    Target& t = it->second;
    PerfScope perf_sync{g_perf.sync};
    g_perf.lates++;
    // what DeliverLate or Sync left in guest memory is no CPU write to upload back
    t.sig = MemSig(t.data, t.bytes);
    t.read.clear();
    std::lock_guard lock(late_lock_);
    Late& l = late_[data];
    if (l.gw != t.gw || l.gh != t.gh) {
        if (diag::Perf()) LOG_FIRST(16, "perf: late %08X ring reset, %ux%u -> %ux%u", data, l.gw, l.gh, t.gw, t.gh);
        ResetLate(l, t);
    }
    // a copy for a later lock, unless the GPU is a whole ring behind
    uint32_t slot = l.next;
    if (l.fence[slot] > fence_->GetCompletedValue()) return;
    ID3D12Resource* src = PrepareLateSource(t, l);
    if (!src) return;
    RecordLateCopy(l, slot, src);
}

// filters src into a new w x h target, left as a copy source
ComPtr<ID3D12Resource> D3D12Backend::Shrink(ID3D12Resource* src, D3D12_RESOURCE_STATES& st, DXGI_FORMAT fmt,
                                            uint32_t w, uint32_t h, const ShrinkOptions& o) {
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC td = Texture2DDesc(DXGI_FORMAT_B8G8R8A8_UNORM, w, h);
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE cv{};
    cv.Format = td.Format;
    ComPtr<ID3D12Resource> down = o.into;
    D3D12_RESOURCE_STATES down_state = o.into ? o.into_state : D3D12_RESOURCE_STATE_RENDER_TARGET;
    g_perf.shrinks++;
    if (free_rtv_.empty()) return nullptr;
    if (down) {
        Transition(down.Get(), down_state, D3D12_RESOURCE_STATE_RENDER_TARGET);
    } else if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td,
                                                    D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, IID_PPV_ARGS(&down)))) {
        return nullptr;
    }
    // borrows the next free view without taking it, only for this recording
    D3D12_CPU_DESCRIPTOR_HANDLE rh = Cpu(rtv_heap_.Get(), free_rtv_.back(), rtv_inc_);
    dev_->CreateRenderTargetView(down.Get(), nullptr, rh);
    Transition(src, st, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    SetBlitTarget(rh, w, h);
    float uv[4] = {0, 0, 1, 1};
    Blit(src, fmt, uv, {.alpha = true, .taps = o.taps, .mul = o.mul});
    Transition(down.Get(), down_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    return down;
}

// a BGRA copy source to the CPU, rows packed; waits for the GPU
bool D3D12Backend::ReadBack(ID3D12Resource* res, uint32_t w, uint32_t h, std::vector<uint8_t>& out) {
    D3D12_RESOURCE_DESC rd = res->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
    UINT rows;
    UINT64 row_size;
    UINT64 bytes;
    dev_->GetCopyableFootprints(&rd, 0, 1, 0, &fp, &rows, &row_size, &bytes);
    ComPtr<ID3D12Resource> rb;
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC bd = BufferDesc(bytes);
    if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                             IID_PPV_ARGS(&rb)))) {
        return false;
    }
    D3D12_TEXTURE_COPY_LOCATION dl{rb.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    dl.PlacedFootprint = fp;
    D3D12_TEXTURE_COPY_LOCATION sl{res, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    cl_->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
    Flush();
    uint8_t* p = nullptr;
    rb->Map(0, nullptr, reinterpret_cast<void**>(&p));
    out.resize(size_t(w) * h * 4);
    for (uint32_t y = 0; y < h; y++) std::memcpy(&out[size_t(y) * w * 4], p + size_t(y) * fp.Footprint.RowPitch, w * 4);
    rb->Unmap(0, nullptr);
    return true;
}

// BMP file and info header: byte offsets of the fields CapSave writes
constexpr int kBmpHeaderBytes = 54;
constexpr int kBmpFileSize = 2;
constexpr int kBmpPixelOffset = 10;
constexpr int kBmpInfoSize = 14;
constexpr int kBmpWidth = 18;
constexpr int kBmpHeight = 22;  // negative: rows top down
constexpr int kBmpPlanes = 26;
constexpr int kBmpBitCount = 28;
constexpr int kBmpImageSize = 34;
constexpr uint32_t kBmpInfoBytes = 40;

void D3D12Backend::CapSave(const CapImage& img, const char* name) {
    ComPtr<ID3D12Resource> down = Shrink(img.res, *img.state, img.fmt, img.w, img.h, {.mul = img.mul});
    std::vector<uint8_t> px;
    if (!down || !ReadBack(down.Get(), img.w, img.h, px)) return;
    FILE* f = _wfopen((cap_dir_ / (std::string(name) + ".bmp")).c_str(), L"wb");
    if (!f) return;
    uint8_t hd[kBmpHeaderBytes] = {'B', 'M'};  // BITMAPFILEHEADER and BITMAPINFOHEADER, top down 32 bit
    auto put = [&](int o, uint32_t v) { std::memcpy(hd + o, &v, 4); };
    put(kBmpFileSize, kBmpHeaderBytes + uint32_t(px.size()));
    put(kBmpPixelOffset, kBmpHeaderBytes);
    put(kBmpInfoSize, kBmpInfoBytes);
    put(kBmpWidth, img.w);
    put(kBmpHeight, uint32_t(-int32_t(img.h)));
    hd[kBmpPlanes] = 1;
    hd[kBmpBitCount] = 32;
    put(kBmpImageSize, uint32_t(px.size()));
    fwrite(hd, 1, kBmpHeaderBytes, f);
    fwrite(px.data(), 1, px.size(), f);
    fclose(f);
}

void D3D12Backend::CapSave(Target& t, const char* name) {
    DXGI_FORMAT fmt = t.depth ? kDepthRead : t.fmt;
    float mul = t.depth ? 1 : t.range;
    CapSave({.res = t.res.Get(), .state = &t.state, .fmt = fmt, .w = t.gw, .h = t.gh, .mul = mul}, name);
}

// the frame moves to another target: save the one it leaves
void D3D12Backend::CapStep(const Surface& rt) {
    uint32_t d = rt.header ? mem::VirtualFromPhysical(rt.data) : 0;
    if (d == cap_rt_) return;
    auto it = targets_.find(TargetMapKey(cap_rt_, false));
    if (cap_rt_ && it != targets_.end()) {
        char name[32];
        snprintf(name, sizeof(name), "%04u_%08X", cap_i_, cap_rt_);
        CapSave(it->second, name);
    }
    auto z = targets_.find(TargetMapKey(cap_rt_, true));
    if (cap_rt_ && z != targets_.end()) {
        char name[32];
        snprintf(name, sizeof(name), "%04u_%08X_z", cap_i_, cap_rt_);
        CapSave(z->second, name);
    }
    cap_rt_ = d;
}

void D3D12Backend::Capture(Target& t, const char* tag, float mul) {
    ext_list_ = true;
    if (!cap_) return;
    char name[32];
    snprintf(name, sizeof(name), "%04u_%s_%08X", cap_i_, tag, t.data);
    CapSave({.res = t.res.Get(), .state = &t.state, .fmt = t.fmt, .w = t.gw, .h = t.gh, .mul = mul}, name);
}

void D3D12Backend::SetGamma(const uint8_t* ramp) {
    std::memcpy(ramp_, ramp, sizeof(ramp_));
    ramp_dirty_ = true;
}

}  // namespace

// pipeline_cache.h's, by the file's own (PipeRegisterRoot, PipeCreate, PipeSave)
void RegisterRootSignature(ID3D12RootSignature* rs, const void* blob, size_t size) { PipeRegisterRoot(rs, blob, size); }
HRESULT CreateGraphicsPipelineCached(ID3D12Device* dev, const D3D12_GRAPHICS_PIPELINE_STATE_DESC& d, REFIID riid,
                                     void** out) {
    return PipeCreate(dev, d, riid, out);
}
void SavePipelineCache(bool now) { PipeSave(now); }
void StartPipelinePrebuild(ID3D12Device* dev) { PipePrebuild(dev); }
void PipelinePrebuildProgress(uint32_t& done, uint32_t& total, bool& finished) {
    PipePrebuildProgress(done, total, finished);
}
bool PrepareShaders() { return PipePrepare(); }

std::unique_ptr<Backend> CreateBackend() { return std::make_unique<D3D12Backend>(); }

}  // namespace gpu
