// Android native Vulkan 1.1 renderer for Noble Archimedes (Xbox Direct3D 8 -> Vulkan 1.1).
// Replaces vkd3d + d3d12.cpp on Android: direct swapchain rendering, pooled device memory,
// CPU DXT1/3/5 decode for GPUs without BC textures (Mali / PowerVR), render-pass and fence
// fallbacks for Vulkan 1.1 drivers, glslang HLSL->SPIR-V with disk + VkPipelineCache, and
// full support for Castlevania's gpu::Extension (widescreen, HUD edges, pause menu spread,
// overscan fix, texel box clamping, and HD texture packs with material maps).
#include <vulkan/vulkan.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <sys/system_properties.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <dxgiformat.h>
#include <imgui.h>
#include <backends/imgui_impl_vulkan.h>

#include "app/app.h"
#include "core/log.h"
#include "gpu/backend.h"
#include "gpu/ext.h"
#include "gpu/shaders.h"
#include "gpu/texture.h"
#include "gpu/texturedumper.h"
#include "gpu/vertex.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "linux/sdl_app.h"
#include "noble_android.h"
#include "vk_shader.h"

namespace gpu {

namespace {

constexpr int kFrames = 3;
// 16 MB per in-flight frame (48 MB total) keeps RAM low on 4 GB phones while comfortably holding
// a frame's vertices, indices, constants, and staging uploads; overflows flush cleanly.
constexpr VkDeviceSize kUploadSize = 16ull << 20;
constexpr VkDeviceSize kUploadReserve = 2ull << 20;
// b0: float4 c[192] = 3072 bytes; b1: DrawConstants (~2224 bytes) <= 3072 bytes.
constexpr VkDeviceSize kUboRange = 3072;
constexpr VkDeviceSize kUboAlign = 256;
constexpr VkFormat kBgra = VK_FORMAT_B8G8R8A8_UNORM;
constexpr uint32_t kSetsPerPool = 1024;
constexpr uint32_t kVendorArm = 0x13B5;
constexpr uint32_t kQueries = 512;
constexpr uint32_t kNoQuery = ~0u;
static_assert(sizeof(DrawConstants) <= kUboRange);
static_assert(sizeof(State::vsc) <= kUboRange);

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
    if (opts.x > 0)
        c.rgb = float3(lut.Sample(ps, float2(c.r, 0.5)).r,
                       lut.Sample(ps, float2(c.g, 0.5)).g,
                       lut.Sample(ps, float2(c.b, 0.5)).b);
    return float4(c.rgb, opts.y > 0 ? c.a : 1);
}
)";

const char* kDepthHlsl = R"(
Texture2D<float> img : register(t0);
SamplerState ps : register(s0);
struct O { float4 p : SV_Position; float2 uv : TEXCOORD0; };
O vs(uint id : SV_VertexID) {
    O o;
    float2 t = float2((id << 1) & 2, id & 2);
    o.p = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = t;
    return o;
}
float psmain(O i) : SV_Depth { return img.SampleLevel(ps, i.uv, 0); }
)";

// ---- Console state -> Vulkan conversions

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

VkBlendFactor Blend(uint32_t v, bool alpha) {
    switch (v) {
        case 0: return VK_BLEND_FACTOR_ZERO;
        case 1: return VK_BLEND_FACTOR_ONE;
        case 0x300: return alpha ? VK_BLEND_FACTOR_SRC_ALPHA : VK_BLEND_FACTOR_SRC_COLOR;
        case 0x301: return alpha ? VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        case 0x302: return VK_BLEND_FACTOR_SRC_ALPHA;
        case 0x303: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        case 0x304: return VK_BLEND_FACTOR_DST_ALPHA;
        case 0x305: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        case 0x306: return alpha ? VK_BLEND_FACTOR_DST_ALPHA : VK_BLEND_FACTOR_DST_COLOR;
        case 0x307: return alpha ? VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
        case 0x308: return alpha ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
        case 0x8001: return VK_BLEND_FACTOR_CONSTANT_COLOR;
        case 0x8002: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
        case 0x8003: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
        case 0x8004: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
        default: return VK_BLEND_FACTOR_ONE;
    }
}

VkBlendOp BlendOp(uint32_t v) {
    switch (v) {
        case 0x800A: return VK_BLEND_OP_SUBTRACT;
        case 0x800B: return VK_BLEND_OP_REVERSE_SUBTRACT;
        case 0x8007: return VK_BLEND_OP_MIN;
        case 0x8008: return VK_BLEND_OP_MAX;
        default: return VK_BLEND_OP_ADD;
    }
}

// D3DCMP_NEVER 0x200..ALWAYS 0x207 matches VkCompareOp 0..7.
uint32_t CmpIndex(uint32_t v) { return v >= 0x200 && v <= 0x207 ? v - 0x200 : 7; }
VkCompareOp Cmp(uint32_t v) { return VkCompareOp(CmpIndex(v)); }

// Reversed-Z flips LESS <-> GREATER and LESS_OR_EQUAL <-> GREATER_OR_EQUAL.
VkCompareOp ReversedCmp(uint32_t v) {
    static constexpr VkCompareOp kRev[8] = {
        VK_COMPARE_OP_NEVER,            // NEVER
        VK_COMPARE_OP_GREATER,          // LESS -> GREATER
        VK_COMPARE_OP_EQUAL,            // EQUAL
        VK_COMPARE_OP_GREATER_OR_EQUAL, // LESSEQUAL -> GREATEREQUAL
        VK_COMPARE_OP_LESS,             // GREATER -> LESS
        VK_COMPARE_OP_NOT_EQUAL,        // NOTEQUAL
        VK_COMPARE_OP_LESS_OR_EQUAL,    // GREATEREQUAL -> LESSEQUAL
        VK_COMPARE_OP_ALWAYS,           // ALWAYS
    };
    return kRev[CmpIndex(v)];
}

VkStencilOp StencilOp(uint32_t v) {
    switch (v) {
        case 0: return VK_STENCIL_OP_ZERO;
        case 0x1E01: return VK_STENCIL_OP_REPLACE;
        case 0x1E02: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
        case 0x1E03: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
        case 0x150A: return VK_STENCIL_OP_INVERT;
        case 0x8507: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
        case 0x8508: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
        default: return VK_STENCIL_OP_KEEP;
    }
}

VkSamplerAddressMode Address(uint32_t v) {
    switch (v) {
        case 2: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        case 3:
        case 5: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        case 4: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        default: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    }
}

VkFormat VertexFormat(uint32_t dxgi) {
    switch (dxgi) {
        case DXGI_FORMAT_R32G32B32A32_FLOAT: return VK_FORMAT_R32G32B32A32_SFLOAT;
        case DXGI_FORMAT_R32G32B32_FLOAT: return VK_FORMAT_R32G32B32_SFLOAT;
        case DXGI_FORMAT_R32G32_FLOAT: return VK_FORMAT_R32G32_SFLOAT;
        case DXGI_FORMAT_R32_FLOAT: return VK_FORMAT_R32_SFLOAT;
        case DXGI_FORMAT_R32_UINT: return VK_FORMAT_R32_UINT;
        case DXGI_FORMAT_R16G16B16A16_SNORM: return VK_FORMAT_R16G16B16A16_SNORM;
        case DXGI_FORMAT_R16G16B16A16_SINT: return VK_FORMAT_R16G16B16A16_SINT;
        case DXGI_FORMAT_R16G16_SNORM: return VK_FORMAT_R16G16_SNORM;
        case DXGI_FORMAT_R16G16_SINT: return VK_FORMAT_R16G16_SINT;
        case DXGI_FORMAT_R16_SNORM: return VK_FORMAT_R16_SNORM;
        case DXGI_FORMAT_R16_SINT: return VK_FORMAT_R16_SINT;
        case DXGI_FORMAT_R8G8B8A8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_R8G8_UNORM: return VK_FORMAT_R8G8_UNORM;
        case DXGI_FORMAT_R8_UNORM: return VK_FORMAT_R8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_UNORM: return VK_FORMAT_B8G8R8A8_UNORM;
        default: return VK_FORMAT_R32G32B32A32_SFLOAT;
    }
}

bool HasStencil(VkFormat f) {
    return f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT_S8_UINT || f == VK_FORMAT_D16_UNORM_S8_UINT;
}

bool MaliProfileForced() {
    char prop[PROP_VALUE_MAX] = {};
    return __system_property_get("debug.noble.mali_profile", prop) > 0 && prop[0] == '1';
}

// ---- CPU DXT1 / DXT3 / DXT5 -> BGRA8 decoder (for Mali / PowerVR without BC textures)

void Rgb565(uint16_t c, uint8_t* bgr) {
    uint32_t r = (c >> 11) & 31;
    uint32_t g = (c >> 5) & 63;
    uint32_t b = c & 31;
    bgr[2] = uint8_t((r << 3) | (r >> 2));
    bgr[1] = uint8_t((g << 2) | (g >> 4));
    bgr[0] = uint8_t((b << 3) | (b >> 2));
}

void ColourBlock(const uint8_t* b, bool dxt1, uint8_t* out) {
    uint16_t c0 = uint16_t(b[0] | (b[1] << 8));
    uint16_t c1 = uint16_t(b[2] | (b[3] << 8));
    uint8_t pal[4][4] = {};
    Rgb565(c0, pal[0]);
    Rgb565(c1, pal[1]);
    for (int i = 0; i < 4; i++) pal[i][3] = 255;
    for (int k = 0; k < 3; k++) {
        if (c0 > c1 || !dxt1) {
            pal[2][k] = uint8_t((2 * pal[0][k] + pal[1][k] + 1) / 3);
            pal[3][k] = uint8_t((pal[0][k] + 2 * pal[1][k] + 1) / 3);
        } else {
            pal[2][k] = uint8_t((pal[0][k] + pal[1][k]) / 2);
            pal[3][k] = 0;
        }
    }
    if (dxt1 && c0 <= c1) pal[3][3] = 0;
    uint32_t bits = uint32_t(b[4]) | (uint32_t(b[5]) << 8) | (uint32_t(b[6]) << 16) | (uint32_t(b[7]) << 24);
    for (int i = 0; i < 16; i++) std::memcpy(out + i * 4, pal[(bits >> (i * 2)) & 3], 4);
}

void DecodeDxt(Host host, const std::vector<uint8_t>& in, uint32_t row_pitch, uint32_t w, uint32_t h, uint32_t z,
               std::vector<uint8_t>& out) {
    uint32_t block = host == Host::BC1 ? 8 : 16;
    uint32_t bw = std::max(1u, (w + 3) / 4);
    uint32_t bh = std::max(1u, (h + 3) / 4);
    out.assign(size_t(w) * h * z * 4, 0);
    uint8_t px[64];
    for (uint32_t s = 0; s < z; s++) {
        for (uint32_t by = 0; by < bh; by++) {
            for (uint32_t bx = 0; bx < bw; bx++) {
                size_t at = (size_t(s) * bh + by) * row_pitch + size_t(bx) * block;
                if (at + block > in.size()) continue;
                const uint8_t* b = &in[at];
                ColourBlock(host == Host::BC1 ? b : b + 8, host == Host::BC1, px);
                if (host == Host::BC2) {
                    for (int i = 0; i < 16; i++) px[i * 4 + 3] = uint8_t(((b[i / 2] >> ((i & 1) * 4)) & 15) * 17);
                } else if (host == Host::BC3) {
                    uint8_t a[8] = {b[0], b[1]};
                    if (a[0] > a[1]) {
                        for (int i = 1; i < 7; i++) a[i + 1] = uint8_t(((7 - i) * a[0] + i * a[1] + 3) / 7);
                    } else {
                        for (int i = 1; i < 5; i++) a[i + 1] = uint8_t(((5 - i) * a[0] + i * a[1] + 2) / 5);
                        a[6] = 0;
                        a[7] = 255;
                    }
                    uint64_t bits = 0;
                    for (int i = 0; i < 6; i++) bits |= uint64_t(b[2 + i]) << (8 * i);
                    for (int i = 0; i < 16; i++) px[i * 4 + 3] = a[(bits >> (3 * i)) & 7];
                }
                for (uint32_t y = 0; y < 4 && by * 4 + y < h; y++)
                    for (uint32_t x = 0; x < 4 && bx * 4 + x < w; x++)
                        std::memcpy(&out[((size_t(s) * h + by * 4 + y) * w + bx * 4 + x) * 4], px + (y * 4 + x) * 4, 4);
            }
        }
    }
}

// ---- Pooled Vulkan device memory allocator (32 MB blocks for 4 GB mobile RAM hygiene)

class Allocator {
public:
    struct Alloc {
        VkDeviceMemory mem = VK_NULL_HANDLE;
        VkDeviceSize off = 0, size = 0;
        uint8_t* map = nullptr;
        int block = -1;  // -1: dedicated allocation
    };

    void Init(VkPhysicalDevice pd, VkDevice dev) {
        dev_ = dev;
        vkGetPhysicalDeviceMemoryProperties(pd, &props_);
    }

    bool Get(const VkMemoryRequirements& r, VkMemoryPropertyFlags want, VkMemoryPropertyFlags nice, bool linear,
             Alloc& out) {
        int type = Type(r.memoryTypeBits, want | nice);
        if (type < 0) type = Type(r.memoryTypeBits, want);
        if (type < 0) return false;
        bool host = (props_.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;
        std::lock_guard lock(m_);
        if (r.size > kBlock / 4) {
            out = {};
            if (!Allocate(uint32_t(type), r.size, out.mem)) return false;
            out.size = r.size;
            if (host) vkMapMemory(dev_, out.mem, 0, VK_WHOLE_SIZE, 0, reinterpret_cast<void**>(&out.map));
            return true;
        }
        for (size_t i = 0; i < blocks_.size(); i++) {
            Block& b = blocks_[i];
            if (b.type != uint32_t(type) || b.linear != linear) continue;
            if (Carve(b, r, out)) {
                out.block = int(i);
                return true;
            }
        }
        Block b;
        b.type = uint32_t(type);
        b.linear = linear;
        if (!Allocate(b.type, kBlock, b.mem)) return false;
        if (host) vkMapMemory(dev_, b.mem, 0, VK_WHOLE_SIZE, 0, reinterpret_cast<void**>(&b.map));
        b.free[0] = kBlock;
        blocks_.push_back(std::move(b));
        Carve(blocks_.back(), r, out);
        out.block = int(blocks_.size() - 1);
        return true;
    }

    void Free(Alloc& a) {
        if (!a.mem) return;
        std::lock_guard lock(m_);
        if (a.block < 0) {
            vkFreeMemory(dev_, a.mem, nullptr);
        } else {
            auto& free = blocks_[size_t(a.block)].free;
            auto it = free.emplace(a.off, a.size).first;
            if (auto next = std::next(it); next != free.end() && it->first + it->second == next->first) {
                it->second += next->second;
                free.erase(next);
            }
            if (it != free.begin()) {
                auto prev = std::prev(it);
                if (prev->first + prev->second == it->first) {
                    prev->second += it->second;
                    free.erase(it);
                }
            }
        }
        a = {};
    }

private:
    static constexpr VkDeviceSize kBlock = 32ull << 20;
    struct Block {
        VkDeviceMemory mem = VK_NULL_HANDLE;
        uint32_t type = 0;
        bool linear = false;
        uint8_t* map = nullptr;
        std::map<VkDeviceSize, VkDeviceSize> free;
    };

    int Type(uint32_t bits, VkMemoryPropertyFlags want) const {
        for (uint32_t i = 0; i < props_.memoryTypeCount; i++)
            if (((bits >> i) & 1) && (props_.memoryTypes[i].propertyFlags & want) == want) return int(i);
        return -1;
    }

    bool Allocate(uint32_t type, VkDeviceSize size, VkDeviceMemory& mem) {
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = size;
        ai.memoryTypeIndex = type;
        if (vkAllocateMemory(dev_, &ai, nullptr, &mem) == VK_SUCCESS) return true;
        LOGE("vulkan: out of device memory (%llu bytes)", (unsigned long long)size);
        return false;
    }

    bool Carve(Block& b, const VkMemoryRequirements& r, Alloc& out) {
        VkDeviceSize align = std::max<VkDeviceSize>(r.alignment, 1);
        for (auto it = b.free.begin(); it != b.free.end(); ++it) {
            VkDeviceSize o = it->first, s = it->second;
            VkDeviceSize at = (o + align - 1) / align * align;
            if (at + r.size > o + s) continue;
            b.free.erase(it);
            if (at > o) b.free[o] = at - o;
            if (at + r.size < o + s) b.free[at + r.size] = o + s - at - r.size;
            out.mem = b.mem;
            out.off = at;
            out.size = r.size;
            out.map = b.map ? b.map + at : nullptr;
            return true;
        }
        return false;
    }

    VkDevice dev_ = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties props_{};
    std::vector<Block> blocks_;
    std::mutex m_;
};

struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;    // sampled: all levels (depth aspect only for depth)
    VkImageView attach = VK_NULL_HANDLE;  // attachment: level 0 (depth+stencil for depth)
    Allocator::Alloc mem;
    VkFormat fmt = VK_FORMAT_UNDEFINED;
    uint32_t w = 1, h = 1, d = 1, layers = 1, levels = 1;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    bool depth = false, stencil = false;
    explicit operator bool() const { return image != VK_NULL_HANDLE; }
};

struct Buffer {
    VkBuffer buf = VK_NULL_HANDLE;
    Allocator::Alloc mem;
};

struct VkTarget {
    Image img;
    Image fb;  // feedback copy when a draw samples the target it renders into
    bool depth = false;
    uint32_t gw = 0, gh = 0, w = 0, h = 0;
    uint32_t data = 0, bytes = 0;
    uint64_t epoch = 0, sig = 0;
    bool drawn = false;
    bool cpu = false;
    std::vector<uint8_t> read;
    // Mirror passed to gpu::Extension hooks (Widescreen reads gw, gh, data, cpu, depth).
    gpu::Target ext_target;

    void SyncExt() {
        ext_target.depth = depth;
        ext_target.gw = gw;
        ext_target.gh = gh;
        ext_target.w = w;
        ext_target.h = h;
        ext_target.data = data;
        ext_target.bytes = bytes;
        ext_target.drawn = drawn;
        ext_target.cpu = cpu;
    }
};

struct Tex {
    Image img;
    Image mat_normal;
    Image mat_orm;
    float mat_params[3][4] = {};
    bool has_material = false;
    bool replaced = false;
    uint64_t repl_id = 0;
    uint8_t flags = 0;
    TextureDesc d;
    VkFormat fmt = VK_FORMAT_UNDEFINED;
    bool decode = false;
    uint32_t levels = 1;
    uint64_t sig = 0;
    uint64_t frame = 0;
    uint64_t palette = 0;

    VkDeviceSize Bytes() const { return img.mem.size + mat_normal.mem.size + mat_orm.mem.size; }
};

struct PsoKey {
    uint64_t vs;
    uint64_t ps;
    uint32_t regs;
    uint32_t topo;  // Topology | (strip ? 0x10 : 0)
    uint32_t has_rt;
    uint32_t has_ds;
    uint32_t elements[16];  // DXGI format | buffer << 8 | offset << 16
    uint32_t strides[16];
    uint32_t buffers;
    uint32_t blend, src, dst, op, wmask;
    uint32_t zen, zwrite, zfunc;
    uint32_t sten, sfunc, smask, swmask, sfail, szfail, spass;
    uint32_t cull, ccw, fill;
    float zslope;
    uint32_t rtfmt;
};

struct BlitOptions {
    bool gamma = false;
    bool alpha = false;
    uint32_t taps = 1;
    float mul = 1.0f;
    bool over = false;
};

struct DrawTextures {
    TexInfo info[4];
    VkImageView view[vkshader::kTexBindings] = {};  // t0..t3 + t4 (mtn) + t5 (mto)
    VkFormat fmt[4] = {};
    uint32_t levels[4] = {1, 1, 1, 1};
    bool rt[4] = {};
    bool replaced[4] = {};
    float k[4] = {1, 1, 1, 1};
    float wide[4] = {};
    int mat_stage = -1;
    float mat[3][4] = {};
};

struct DrawShaders {
    bool prog = false;
    bool quad = false;
    bool fixed_transform = false;
    bool sprite = false;
    FixedKey fk;
    uint64_t vkey = 0, vh = 0, pkey = 0, ph = 0;
    VkShaderModule vs = VK_NULL_HANDLE, ps = VK_NULL_HANDLE;
};

struct GuestExtent {
    uint32_t w, h;
};

// ---- Helpers matching sdk/runtime/gpu/d3d12.cpp

uint32_t SimpleState(const State& s, uint32_t method) { return s.simple[method >> 2]; }

float TargetAspect() { return app::Widescreen() ? app::WideAspect() / (4.0f / 3.0f) : 1.0f; }

constexpr uint32_t kTaddressWrap = 1;
constexpr uint32_t kTaddressClamp = 3;
constexpr uint32_t kTexfLinear = 2;
constexpr uint32_t kTexfAnisotropic = 3;
constexpr uint32_t kFmtD16 = 0x2C;
constexpr uint32_t kFmtLinD16 = 0x30;
constexpr uint32_t kZUseW = 2;
constexpr uint32_t kQuadVertices = 6;
constexpr uint32_t kPointList = 1;
constexpr uint32_t kTopDisable = 1;
constexpr int kSpriteStage = 3;
constexpr float kAlphaTestAlways = 7.0f;
constexpr float kGlowBoxTexels = 3.0f;
constexpr float kUpscaledMin = 1.01f;
constexpr float kParallaxLow = 0.002f, kParallaxHigh = 0.004f;
constexpr float kParallaxLayersLow = 12.0f, kParallaxLayersHigh = 24.0f;

uint8_t ShadowDepthKind(uint32_t format) { return format == kFmtD16 || format == kFmtLinD16 ? 2 : 1; }
uint32_t GuestPitch(uint32_t width) { return (width * 4 + 63) & ~63u; }

constexpr uint32_t kSigWholeMax = 65536;
constexpr uint32_t kSigSamples = 64;
constexpr uint32_t kSigSampleBytes = 64;

uint64_t MemSig(uint32_t data, uint32_t total) {
    const uint8_t* p = static_cast<const uint8_t*>(kernel::HostPtr(data));
    uint64_t sig = Hash(&total, sizeof(total));
    if (total <= kSigWholeMax) return Hash(p, total, sig);
    for (uint32_t i = 0; i < kSigSamples; i++) {
        const uint8_t* sample = p + uint64_t(total - kSigSampleBytes) * i / (kSigSamples - 1);
        sig = Hash(sample, kSigSampleBytes, sig);
    }
    return sig;
}

bool Changed(const VkTarget& t) {
    if (t.drawn || t.read.empty()) return false;
    uint32_t row = t.gw * 4, pitch = GuestPitch(t.gw);
    const uint8_t* p = static_cast<const uint8_t*>(kernel::HostPtr(t.data));
    for (uint32_t y = 0; y < t.gh; y++)
        if (std::memcmp(p + size_t(y) * pitch, &t.read[size_t(y) * row], row)) return true;
    return false;
}

bool LatchCpuWrite(VkTarget& t) {
    uint64_t e = DirtyEpoch(t.data);
    if (e <= t.epoch) return false;
    t.epoch = e;
    uint64_t sig = MemSig(t.data, t.bytes);
    if (sig == t.sig && !Changed(t)) return false;
    t.sig = sig;
    return true;
}

std::vector<uint8_t> LatchCpuChanges(VkTarget& t) {
    std::vector<uint8_t> px(t.read.size(), 0);
    uint32_t guest_pitch = GuestPitch(t.gw);
    const uint8_t* guest = static_cast<const uint8_t*>(kernel::HostPtr(t.data));
    for (uint32_t y = 0; y < t.gh; y++)
        for (uint32_t x = 0; x < t.gw; x++) {
            const uint8_t* c = guest + size_t(y) * guest_pitch + x * 4;
            size_t i = (size_t(y) * t.gw + x) * 4;
            if (!std::memcmp(c, &t.read[i], 4)) continue;
            std::memcpy(&t.read[i], c, 4);
            std::memcpy(&px[i], c, 3);
            px[i + 3] = 255;
        }
    return px;
}

TexKind TextureKind(const TextureDesc& d) {
    if (d.cube) return kTexCube;
    if (d.depth > 1) return kTex3D;
    return kTex2D;
}

bool SameTexture(const Tex& tx, const TextureDesc& d) {
    return tx.img && tx.d.format == d.format && tx.d.width == d.width && tx.d.height == d.height &&
           tx.d.depth == d.depth && tx.d.levels == d.levels && tx.d.cube == d.cube && tx.d.pitch == d.pitch;
}

struct StagePalette {
    uint32_t colours[256] = {};
    uint32_t va = 0;
    uint64_t hash = 0;
};

void ReadStagePalette(const State& s, int stage, const TextureDesc& d, StagePalette& palette) {
    constexpr uint32_t kFmtP8 = 0x0B, kPaletteCommon = 0, kPaletteData = 4, kPaletteSizeShift = 30;
    if (d.format != kFmtP8 || !s.palettes[size_t(stage)]) return;
    uint32_t header = s.palettes[size_t(stage)];
    palette.va = mem::VirtualFromPhysical(kernel::R32(header + kPaletteData));
    uint32_t size_code = kernel::R32(header + kPaletteCommon) >> kPaletteSizeShift;
    size_t bytes = (std::size(palette.colours) >> size_code) * sizeof(uint32_t);
    std::memcpy(palette.colours, kernel::HostPtr(palette.va), bytes);
    palette.hash = Hash(palette.colours, sizeof(palette.colours));
}

DrawShaders ClassifyShaders(const State& s, const Assembled& a, uint32_t count) {
    DrawShaders sh;
    sh.prog = (s.vs_handle & 1) && s.vs && !s.vs->program.empty();
    bool points = a.topology == Topology::Points;
    sh.quad = count <= kQuadVertices && !s.rs[kRsZEnable] && !points;
    sh.fixed_transform = !sh.prog && !a.pretransformed;
    sh.sprite = points && sh.prog && s.rs[kRsPointSpriteEnable];
    if (!sh.prog) sh.fk = MakeFixedKey(s);
    return sh;
}

void EnsureProgramHash(const State& s) {
    if (!s.vs->hash) s.vs->hash = Hash(s.vs->program.data(), s.vs->program.size() * 4) | 1;
}

constexpr uint64_t kDeclarationSalt = 0x10000;
constexpr uint64_t kPretransformedSalt = 0x5A5A;
constexpr uint64_t kQuadSalt = 0xC3C30000;
constexpr uint64_t kSpriteSalt = 0x3C3C0000;
constexpr uint64_t kPixelKeySalt = 0x9E3779B97F4A7C15ull;

uint64_t VertexKey(const State& s, const Assembled& a, const DrawShaders& sh) {
    uint64_t seed = sh.prog ? s.vs->hash ^ a.regs : Hash(&s.vs_handle, 4, a.regs | kDeclarationSalt);
    uint64_t vkey = Hash(&a.decode, 8, seed);
    if (!sh.prog && a.pretransformed) vkey ^= kPretransformedSalt;
    if (sh.quad) vkey ^= kQuadSalt;
    if (sh.sprite) vkey ^= kSpriteSalt;
    if (!sh.prog) vkey = Hash(&sh.fk, sizeof(sh.fk), vkey);
    return vkey;
}

std::string VertexHlsl(const State& s, const Assembled& a, const DrawShaders& sh) {
    if (sh.prog) return VertexProgramHlsl(s.vs->program, a.regs, a.decode);
    return FixedVertexHlsl(a.regs, a.pretransformed, a.decode, sh.fk);
}

void KeyBlend(const State& s, PsoKey& k) {
    k.blend = SimpleState(s, kMBlendEnable) != 0;
    if (k.blend) {
        k.src = SimpleState(s, kMSrcBlend);
        k.dst = SimpleState(s, kMDestBlend);
        k.op = SimpleState(s, kMBlendOp);
    }
    uint32_t cw = SimpleState(s, kMColorWriteEnable);
    k.wmask = ((cw >> 16) & 1) | (((cw >> 8) & 1) << 1) | ((cw & 1) << 2) | (((cw >> 24) & 1) << 3);
}

void KeyRasterizer(const State& s, const Assembled& a, PsoKey& k) {
    constexpr uint32_t kCullCw = 0x900;
    uint32_t cull = s.rs[kRsCullMode];
    k.cull = cull != 0 && a.topology == Topology::Triangles;
    k.ccw = cull == kCullCw;
    k.fill = s.rs[kRsFillMode];
}

bool OffsetsPolygons(const State& s, Topology topology) {
    return topology == Topology::Triangles && SimpleState(s, kMSolidOffsetEnable);
}

float HostSlopeScale(const State& s, const VkTarget* ds) {
    return Bits(SimpleState(s, kMSlopeScale)) * (ds && ds->gh ? float(ds->h) / ds->gh : 1.0f);
}

void FillFogConstants(const State& s, bool vertex_program, DrawConstants& c) {
    constexpr uint32_t kFogTableNone = 0;
    constexpr float kFogTableLinear = 3.0f;
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

GuestExtent MultisampleGrid(const State& s, const Surface& target, GuestExtent size) {
    constexpr uint32_t kMode2x = 1;
    constexpr uint32_t kMode4x = 2;
    uint32_t mode = s.rt_back_buffer ? s.rs[kRsMultiSampleMode] : s.rs[kRsMultiSampleRenderTargetMode];
    TextureDesc d;
    if ((mode != kMode2x && mode != kMode4x) || !Describe(target.header, d) || !d.linear) return size;
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
    c.misc[1] = SimpleState(s, kMAlphaTestEnable) ? float(CmpIndex(SimpleState(s, kMAlphaFunc))) : kAlphaTestAlways;
    c.misc[2] = float(CmpIndex(s.rs[kRsShadowFunc]));
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

void FillTexelConstants(int i, const TextureDesc& d, const DrawTextures& stages, DrawConstants& c) {
    c.texsize[i][0] = float(d.width);
    c.texsize[i][1] = float(d.height);
    if (!stages.rt[i]) return;
    float k = stages.k[i];
    if (stages.wide[i] > 0) {
        float n = std::ceil(kGlowBoxTexels * stages.wide[i] * 0.5f);
        c.spread[i][0] = kGlowBoxTexels / (n * float(d.width) * k);
        c.spread[i][1] = kGlowBoxTexels / (n * float(d.height) * k);
        c.spread[i][2] = n;
    }
}

void MatMul(const float* x, const float* y, float* o) {
    for (int r = 0; r < 4; r++)
        for (int col = 0; col < 4; col++)
            o[r * 4 + col] = x[r * 4] * y[col] + x[r * 4 + 1] * y[4 + col] + x[r * 4 + 2] * y[8 + col] +
                             x[r * 4 + 3] * y[12 + col];
}

void Transpose(const float* m, float* o) {
    for (int r = 0; r < 4; r++)
        for (int col = 0; col < 4; col++) o[col * 4 + r] = m[r * 4 + col];
}

void InverseTranspose3x3(const float* wv, float* n) {
    float det = wv[0] * (wv[5] * wv[10] - wv[6] * wv[9]) - wv[1] * (wv[4] * wv[10] - wv[6] * wv[8]) +
                wv[2] * (wv[4] * wv[9] - wv[5] * wv[8]);
    float id = det != 0 ? 1.0f / det : 0.0f;
    for (int r = 0; r < 3; r++)
        for (int col = 0; col < 3; col++) {
            int r1 = (r + 1) % 3, r2 = (r + 2) % 3, c1 = (col + 1) % 3, c2 = (col + 2) % 3;
            n[r * 4 + col] = (wv[r1 * 4 + c1] * wv[r2 * 4 + c2] - wv[r1 * 4 + c2] * wv[r2 * 4 + c1]) * id;
        }
}

constexpr size_t kTsView = 2;
constexpr size_t kTsProjection = 3;
constexpr size_t kTsTexture0 = 16;
constexpr size_t kTsWorld = 256;

void FillTransformConstants(const State& s, GuestExtent size, DrawConstants& c) {
    float wv[16], wvp[16];
    MatMul(s.transforms[kTsWorld].data(), s.transforms[kTsView].data(), wv);
    MatMul(wv, s.transforms[kTsProjection].data(), wvp);
    const auto& v = s.viewport;
    float sx = float(v.w) / size.w, sy = float(v.h) / size.h;
    float ox = (2.0f * v.x + v.w) / size.w - 1.0f, oy = 1.0f - (2.0f * v.y + v.h) / size.h;
    float vpm[16] = {sx, 0, 0, 0, 0, sy, 0, 0, 0, 0, v.max_z - v.min_z, 0, ox, oy, v.min_z, 1};
    float full[16];
    MatMul(wvp, vpm, full);
    Transpose(full, c.wvp);
    Transpose(wv, c.wv);
    float n[16] = {};
    InverseTranspose3x3(wv, n);
    Transpose(n, c.wvn);
    for (int t = 0; t < 4; t++) Transpose(s.transforms[kTsTexture0 + size_t(t)].data(), c.tm[t]);
}

void LightToView(const std::array<uint32_t, 26>& l, const float* vm, float (&o)[7][4]) {
    float f[26];
    std::memcpy(f, l.data(), sizeof(f));
    for (int j = 0; j < 4; j++) {
        o[0][j] = f[1 + j];
        o[1][j] = f[5 + j];
        o[2][j] = f[9 + j];
    }
    const float* pos = f + 13;
    const float* dir = f + 16;
    for (int j = 0; j < 3; j++) {
        o[3][j] = pos[0] * vm[j] + pos[1] * vm[4 + j] + pos[2] * vm[8 + j] + vm[12 + j];
        o[4][j] = dir[0] * vm[j] + dir[1] * vm[4 + j] + dir[2] * vm[8 + j];
    }
    o[3][3] = f[19];
    o[4][3] = f[20];
    o[5][0] = f[21];
    o[5][1] = f[22];
    o[5][2] = f[23];
    o[6][0] = std::cos(f[24] * 0.5f);
    o[6][1] = std::cos(f[25] * 0.5f);
}

void FillLightConstants(const State& s, const FixedKey& fk, DrawConstants& c) {
    std::memcpy(c.mat, s.material.data(), sizeof(float) * 16);
    c.mat[4][0] = s.material[16];
    Color(s.rs[kRsAmbient], c.amb);
    const float* vm = s.transforms[kTsView].data();
    for (int i = 0; i < 8; i++)
        if ((fk.types >> (i * 2)) & 3) LightToView(s.lights[size_t(i)], vm, c.lt[i]);
}

struct WRange {
    float near_w;
    float far_w;
};

WRange ProjectionWRange(const float* m) {
    float m33 = m[10], m34 = m[11], m43 = m[14], m44 = m[15];
    if (m33 == m34 || m33 == 0) return {0, 1};
    return {m44 - m43 / m33 * m34, (m44 - m43) / (m33 - m34) * m34 + m44};
}

constexpr uint32_t kMClipMin = 0x394;
constexpr uint32_t kMClipMax = 0x398;
constexpr float kFallbackNear = 1.0f / 65536.0f;

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

void FillDepthConstants(const State& s, const DrawShaders& sh, Topology topo, DrawConstants& c) {
    bool offset = OffsetsPolygons(s, topo);
    float units = offset ? Bits(SimpleState(s, kMDepthOffset)) : 0.0f;
    if (s.rs[kRsZEnable] != kZUseW) {
        c.zparams[2] = units / s.zscale;
        return;
    }
    WRange r = DrawWRange(s, sh.prog);
    c.zparams[0] = r.near_w / (r.far_w - r.near_w);
    c.zparams[1] = r.far_w;
    c.zparams[2] = sh.prog ? units : units * r.far_w / s.zscale;
}

bool UnitTexcoords(const Assembled& a) {
    constexpr float kLo = -1e-4f, kHi = 1.0001f;
    constexpr uint32_t kSmallDrawVertices = 4096;
    if (!a.vertices.empty() || !a.vertex_count || a.vertex_count > kSmallDrawVertices) return false;
    bool any = false;
    for (int r = 0; r < 16; r++) {
        const VertexElement& e = a.elements[size_t(r)];
        if (!((a.regs >> r) & 1) || e.format != DXGI_FORMAT_R32G32_FLOAT) continue;
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

bool TexcoordsAsGiven(const State& s, const Assembled& a) {
    if ((s.vs_handle & 1) && s.vs && !s.vs->program.empty()) {
        static std::unordered_map<uint64_t, bool> copies;
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

bool AnyStageWraps(const State& s) {
    for (const auto& t : s.tss)
        if (t[kTssAddressU] == kTaddressWrap || t[kTssAddressV] == kTaddressWrap) return true;
    return false;
}

bool SpriteOnStage3(const State& s, uint32_t prim) {
    bool prog = (s.vs_handle & 1) && s.vs && !s.vs->program.empty();
    bool stage3 = s.tss[kSpriteStage][kTssColorOp] != kTopDisable;
    return prim == kPointList && prog && !s.ps && s.rs[kRsPointSpriteEnable] && stage3;
}

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

struct DrawEnd {
    Extension* e;
    ~DrawEnd() {
        if (e) e->EndDraw();
    }
};

struct GuestRect {
    int32_t left, top, right, bottom;
};

GuestRect ReadGuestRect(uint32_t rects, uint32_t i) {
    uint32_t r = rects + i * 16;
    return {int32_t(kernel::R32(r)), int32_t(kernel::R32(r + 4)), int32_t(kernel::R32(r + 8)),
            int32_t(kernel::R32(r + 12))};
}

void ReadGuestPoint(uint32_t points, uint32_t i, int32_t& x, int32_t& y) {
    x = int32_t(kernel::R32(points + i * 8));
    y = int32_t(kernel::R32(points + i * 8 + 4));
}

uint64_t TargetMapKey(uint32_t data, bool depth, uint64_t alt = 0) {
    return alt ? alt : (uint64_t(data) << 1) | (depth ? 1 : 0);
}

void LayoutMasks(VkImageLayout l, VkPipelineStageFlags& st, VkAccessFlags& ac) {
    switch (l) {
        case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
            st = VK_PIPELINE_STAGE_TRANSFER_BIT;
            ac = VK_ACCESS_TRANSFER_WRITE_BIT;
            break;
        case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
            st = VK_PIPELINE_STAGE_TRANSFER_BIT;
            ac = VK_ACCESS_TRANSFER_READ_BIT;
            break;
        case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
            st = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT;
            ac = VK_ACCESS_SHADER_READ_BIT;
            break;
        case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
            st = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            ac = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            break;
        case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
            st = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
            ac = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            break;
        case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:
            st = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
            ac = 0;
            break;
        default:
            st = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
            ac = 0;
            break;
    }
}

bool HasExtension(const std::vector<VkExtensionProperties>& list, const char* name) {
    return std::any_of(list.begin(), list.end(),
                       [&](const VkExtensionProperties& e) { return !std::strcmp(e.extensionName, name); });
}

#define VK_CHECK(x)                                      \
    do {                                                 \
        VkResult _r = (x);                               \
        if (_r != VK_SUCCESS) {                          \
            LOGE("vulkan: %s failed (%d)", #x, int(_r)); \
            return false;                                \
        }                                                \
    } while (0)

class VulkanBackend final : public Backend, public Context {
public:
    ~VulkanBackend() override;
    bool Init(uint32_t width, uint32_t height) override;
    void Clear(const State& s, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color, float z,
               uint32_t stencil) override;
    void Draw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) override;
    void Copy(const Surface& src, const Surface& dst, uint32_t count, uint32_t src_rects, uint32_t dst_points) override;
    void Present(const Surface& front) override;
    void Sync(uint32_t data, uint32_t size) override;
    void SyncLate(uint32_t data) override;
    bool DeliverLate(uint32_t data) override;
    void RecordLate(uint32_t data) override;
    void Flush() override { Submit(true, false); }
    void SetGamma(const uint8_t* ramp) override {
        std::memcpy(ramp_, ramp, sizeof(ramp_));
        ramp_dirty_ = true;
    }
    void ClearReport() override { counted_.clear(); }
    void GetReport(uint32_t report) override { reports_.push_back({report, counted_, value_ + 1}); }
    void WaitGpuFrame(uint64_t n) override { WaitValue(present_value_[n % (kFrames + 1)].load()); }

    // gpu::Context (Castlevania's Widescreen extension only calls GuestSize and Presents)
    ID3D12Device* Device() override { return nullptr; }
    ID3D12GraphicsCommandList* List() override { return nullptr; }
    void Transition(ID3D12Resource*, D3D12_RESOURCE_STATES&, D3D12_RESOURCE_STATES) override {}
    void Release(Microsoft::WRL::ComPtr<ID3D12Resource>) override {}
    D3D12_CPU_DESCRIPTOR_HANDLE View(const gpu::Target&) override { return {}; }
    void Unbind() override {}
    ID3D12Resource* Feedback(gpu::Target&) override { return nullptr; }
    void Blit(ID3D12Resource*, DXGI_FORMAT, const float[4], bool, bool, uint32_t, float, DXGI_FORMAT) override {}
    void Pass(const char*, const char*, DXGI_FORMAT, D3D12_CPU_DESCRIPTOR_HANDLE, uint32_t, uint32_t,
              ID3D12Resource* const[3], const DXGI_FORMAT[3], const float[8], float) override {}
    void Capture(gpu::Target&, const char*, float) override {}
    float Scale() const override { return scale_; }
    void GuestSize(uint32_t& w, uint32_t& h) const override {
        w = guest_w_;
        h = guest_h_;
    }
    uint64_t Presents() const override { return presents_; }

private:
    struct Frame {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer cb = VK_NULL_HANDLE;
        Buffer upload;
        VkDeviceSize used = 0;
        uint64_t value = 0;
        std::vector<Image> images;
        std::vector<Buffer> buffers;
        std::vector<VkDescriptorPool> pools;
        size_t pool_at = 0;
        std::unordered_map<uint64_t, VkDescriptorSet> sets;
        VkSemaphore acquire = VK_NULL_HANDLE;
    };
    struct Alloc {
        VkBuffer buf = VK_NULL_HANDLE;
        VkDeviceSize off = 0;
        uint8_t* cpu = nullptr;
    };
    struct Late {
        Buffer rb[kFrames];
        Image down;
        uint64_t fence[kFrames] = {};
        uint64_t frame[kFrames] = {};
        uint64_t delivered = 0;
        uint32_t gw = 0, gh = 0, next = 0;
    };
    struct Query {
        uint32_t index = 0;
        float samples_per_pixel = 1.0f;
    };
    struct Report {
        uint32_t va = 0;
        std::vector<Query> queries;
        uint64_t fence = 0;
    };

    Frame& CurrentFrame() { return frames_[frame_ % kFrames]; }
    VkCommandBuffer Cb() { return CurrentFrame().cb; }

    bool CreateInstance();
    bool CreateSurface();
    bool PickDevice();
    bool CreateDevice();
    bool CreateSwapchain();
    void DestroySwapchain();
    bool CreateLayouts();
    bool CreateFrames();
    bool CreateFixedImages();
    bool InitUi();

    bool CreateImage(Image& im, VkFormat fmt, uint32_t w, uint32_t h, uint32_t d, uint32_t layers, uint32_t levels,
                     VkImageUsageFlags usage, VkImageViewType vt, bool cube = false);
    bool CreateBuffer(Buffer& b, VkDeviceSize size, VkBufferUsageFlags usage, bool host);
    void Destroy(Image& im);
    void Destroy(Buffer& b);
    void Retire(Image& im) {
        if (rend_color_ == &im || rend_depth_ == &im) EndRender();
        if (im) CurrentFrame().images.push_back(im);
        im = {};
    }
    void Retire(Buffer& b) {
        if (b.buf) CurrentFrame().buffers.push_back(b);
        b = {};
    }
    Alloc Upload(VkDeviceSize size, VkDeviceSize align = 16);
    Alloc Constants(const void* data, size_t bytes);

    void Begin();
    void Submit(bool wait, bool present);
    void WaitValue(uint64_t v);
    uint64_t Completed();
    VkFence SubmitFence(uint64_t value);
    void PollFences();
    void ResetFrame(Frame& f);
    void Transition(Image& im, VkImageLayout to);
    void BeginRender(Image* color, Image* depth);
    void EndRender();
    VkRenderPass Pass(VkFormat color, VkFormat depth, bool clear = false);
    VkFramebuffer Framebuffer(VkFormat color_fmt, VkFormat depth_fmt, VkImageView color, VkImageView depth,
                              uint32_t w, uint32_t h);
    void DropFramebuffers(VkImageView v);
    void SetViewport(float x, float y, float w, float h);
    void SetScissor(int32_t x0, int32_t y0, int32_t x1, int32_t y1);
    VkDescriptorSet Set(const VkImageView views[vkshader::kTexBindings], const VkSampler samplers[4]);
    void BindSet(VkDescriptorSet set, VkDeviceSize vc, VkDeviceSize dc);

    VkShaderModule Module(const std::string& src, const char* entry, vkshader::Stage stage, uint64_t& hash);
    VkPipeline FullScreenPipeline(const char* hlsl, const char* ps_entry, VkFormat color, VkFormat depth, bool over);
    VkPipeline EnsurePipeline(const PsoKey& k, VkShaderModule vs, VkShaderModule ps);
    VkSampler Sampler(const VkSamplerCreateInfo& ci);
    VkSampler StageSampler(const State& s, int i, const DrawTextures& stages, bool clamp);

    VkTarget* FindTarget(const Surface& s, bool depth);
    VkTarget* EnsureTarget(const Surface& s, bool depth);
    void ReleaseTarget(std::unordered_map<uint64_t, VkTarget>::iterator it);
    void UploadTarget(VkTarget& t, const Surface& s);
    bool PixelImage(Image& im, const std::vector<uint8_t>& px, uint32_t pitch, uint32_t w, uint32_t h);
    bool UploadPicture(Image& im, const Picture& pic);
    void Blit(Image& dst, const VkRect2D& to, Image& src, const float uvrect[4], const BlitOptions& o);
    Image* Feedback(VkTarget& t);
    bool DepthCopy(const State& s);
    bool Shrink(Image& src, uint32_t w, uint32_t h, Image& down, uint32_t taps = 1);
    Image* CopySource(VkTarget& t, Image& down);
    bool ReadBack(Image& im, uint32_t w, uint32_t h, std::vector<uint8_t>& out);

    float TargetScale() const;
    float RenderTargetShift(const State& s, const DrawShaders& sh) const;
    void EnsureVertexShader(const State& s, const Assembled& a, DrawShaders& sh);
    void EnsurePixelShader(const PixelKey& pk, DrawShaders& sh);
    bool BindStageTextures(const State& s, DrawTextures& stages);
    VkImageView BindTexture(const State& s, int stage, DrawTextures& stages);
    VkImageView BindTargetTexture(const State& s, int stage, const TextureDesc& d, VkTarget& t, bool box,
                                  DrawTextures& stages);
    VkImageView BindGuestTexture(const State& s, int stage, const TextureDesc& d, const FormatInfo& fi,
                                 DrawTextures& stages);
    bool CreateTexture(Tex& tx, const TextureDesc& d, const FormatInfo& fi, uint64_t key);
    void UploadTexture(Tex& tx, const TextureDesc& d, const FormatInfo& fi, const uint8_t* p, const uint32_t* colours);
    void MakeReplacement(Tex& tx);
    void BuildPipelineKey(const State& s, const Assembled& a, const DrawShaders& sh, PsoKey& k);
    void KeyDepthStencil(const State& s, const Assembled& a, PsoKey& k);
    void ClearTarget(VkTarget& t, const State& s, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color,
                     float z, uint32_t stencil);
    bool EnsureQueries();
    uint32_t BeginCountedDraw(const State& s);
    void EndCountedDraw(uint32_t query);
    float SamplesPerPixel() const;
    void DeliverReports();

    bool AcquireImage(uint32_t& index);
    void DrawFrame(VkTarget* t, uint32_t index);
    void RenderUi();
    void UploadGammaRamp();
    void ReleaseOldTextures();
    void ApplySettings();
    void SavePipelineCache();

    VkInstance instance_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice pd_ = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties props_{};
    VkPhysicalDeviceFeatures feats_{};
    uint32_t api_ = VK_API_VERSION_1_1;
    uint32_t qfam_ = 0;
    VkDevice dev_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    std::vector<const char*> dev_exts_;
    bool mali_profile_ = false;
    bool bc_ = false;
    VkFormat depth_fmt_ = VK_FORMAT_D24_UNORM_S8_UINT;
    bool depth_linear_ = false;
    bool r32f_linear_ = false;
    Allocator alloc_;
    PFN_vkCmdBeginRenderingKHR begin_rendering_ = nullptr;
    PFN_vkCmdEndRenderingKHR end_rendering_ = nullptr;
    PFN_vkWaitSemaphoresKHR wait_semaphores_ = nullptr;
    PFN_vkGetSemaphoreCounterValueKHR counter_value_ = nullptr;
    bool dynamic_ = true;
    bool timeline_ok_ = true;
    struct Pending {
        uint64_t value;
        VkFence fence;
    };
    std::mutex fence_lock_;
    std::vector<Pending> pending_;
    std::vector<VkFence> spare_fences_;
    uint64_t fenced_ = 0;
    std::unordered_map<uint64_t, VkRenderPass> passes_;
    struct Fb {
        VkImageView color, depth;
        VkFramebuffer fb;
    };
    std::unordered_map<uint64_t, Fb> framebuffers_;

    VkSwapchainKHR swap_ = VK_NULL_HANDLE;
    VkFormat swap_fmt_ = VK_FORMAT_B8G8R8A8_UNORM;
    VkExtent2D swap_extent_{};
    std::vector<VkImage> swap_images_;
    std::vector<VkImageView> swap_views_;
    std::vector<VkSemaphore> swap_done_;
    bool vsync_ = true;
    bool swap_stale_ = false;
    bool surface_lost_ = false;
    uint32_t surface_failures_ = 0;
    uint32_t present_index_ = 0;

    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipelineCache cache_ = VK_NULL_HANDLE;
    size_t cache_saved_ = 0;
    VkSemaphore timeline_ = VK_NULL_HANDLE;
    std::atomic<uint64_t> value_{0};
    Frame frames_[kFrames];
    uint64_t frame_ = 0;

    bool rendering_ = false;
    Image* rend_color_ = nullptr;
    Image* rend_depth_ = nullptr;

    std::unordered_map<uint64_t, VkShaderModule> modules_;
    std::unordered_map<uint64_t, uint64_t> shader_src_;
    std::unordered_map<uint64_t, VkPipeline> pipelines_;
    std::unordered_map<uint64_t, VkSampler> samplers_;
    std::unordered_map<uint64_t, VkTarget> targets_;
    std::unordered_map<uint64_t, Tex> textures_;
    Image lut_;
    Image dummy_[3];  // 2D, 3D, cube
    VkSampler linear_ = VK_NULL_HANDLE, point_ = VK_NULL_HANDLE;
    VkDescriptorPool ui_pool_ = VK_NULL_HANDLE;
    bool ui_ = false;
    uint8_t ramp_[768] = {};
    bool ramp_dirty_ = false;
    bool gamma_ = false;

    Extension* ext_ = nullptr;
    VkTarget* draw_rt_ = nullptr;
    VkTarget* draw_ds_ = nullptr;
    bool draw_texbox_ = false;
    float draw_uvbox_[4] = {};
    std::array<std::array<float, 4>, 192> vsc_edit_{};
    uint32_t guest_w_ = 640, guest_h_ = 480;
    float scale_ = 1.0f;
    float aspect_ = 1.0f;
    Assembled asm_;

    VkQueryPool query_pool_ = VK_NULL_HANDLE;
    bool query_failed_ = false;
    uint32_t query_next_ = 0;
    std::vector<Query> counted_;
    std::deque<Report> reports_;

    std::unordered_map<uint32_t, Late> late_;
    std::mutex late_lock_;
    std::atomic<uint64_t> presents_seen_{0};
    std::atomic<uint64_t> present_value_[kFrames + 1] = {};
    uint64_t presents_ = 0;
    double fps_ = 0.0;
    std::chrono::steady_clock::time_point fps_t_ = std::chrono::steady_clock::now();
    uint64_t fps_n_ = 0;
};

// ---- Init and teardown

VulkanBackend::~VulkanBackend() {
    if (!dev_) return;
    vkDeviceWaitIdle(dev_);
    SavePipelineCache();
    if (ui_) {
        std::lock_guard lock(app::UiLock());
        ImGui_ImplVulkan_Shutdown();
    }
    if (ui_pool_) vkDestroyDescriptorPool(dev_, ui_pool_, nullptr);
    DestroySwapchain();
    for (auto& [k, t] : targets_) {
        Destroy(t.img);
        Destroy(t.fb);
    }
    targets_.clear();
    for (auto& [k, tx] : textures_) {
        Destroy(tx.img);
        Destroy(tx.mat_normal);
        Destroy(tx.mat_orm);
    }
    textures_.clear();
    for (auto& [k, l] : late_) {
        for (Buffer& b : l.rb) Destroy(b);
        Destroy(l.down);
    }
    late_.clear();
    Destroy(lut_);
    for (Image& d : dummy_) Destroy(d);
    for (Frame& f : frames_) {
        for (Image& im : f.images) Destroy(im);
        for (Buffer& b : f.buffers) Destroy(b);
        Destroy(f.upload);
        for (VkDescriptorPool p : f.pools) vkDestroyDescriptorPool(dev_, p, nullptr);
        if (f.acquire) vkDestroySemaphore(dev_, f.acquire, nullptr);
        if (f.pool) vkDestroyCommandPool(dev_, f.pool, nullptr);
    }
    if (query_pool_) vkDestroyQueryPool(dev_, query_pool_, nullptr);
    if (timeline_) vkDestroySemaphore(dev_, timeline_, nullptr);
    for (Pending& p : pending_) vkDestroyFence(dev_, p.fence, nullptr);
    for (VkFence f : spare_fences_) vkDestroyFence(dev_, f, nullptr);
    for (auto& [k, p] : pipelines_)
        if (p) vkDestroyPipeline(dev_, p, nullptr);
    for (auto& [k, m] : modules_)
        if (m) vkDestroyShaderModule(dev_, m, nullptr);
    for (auto& [k, s] : samplers_)
        if (s) vkDestroySampler(dev_, s, nullptr);
    for (auto& [k, fb] : framebuffers_)
        if (fb.fb) vkDestroyFramebuffer(dev_, fb.fb, nullptr);
    for (auto& [k, rp] : passes_)
        if (rp) vkDestroyRenderPass(dev_, rp, nullptr);
    if (cache_) vkDestroyPipelineCache(dev_, cache_, nullptr);
    if (layout_) vkDestroyPipelineLayout(dev_, layout_, nullptr);
    if (set_layout_) vkDestroyDescriptorSetLayout(dev_, set_layout_, nullptr);
    vkDestroyDevice(dev_, nullptr);
    if (surface_) vkDestroySurfaceKHR(instance_, surface_, nullptr);
    if (instance_) vkDestroyInstance(instance_, nullptr);
}

bool VulkanBackend::Init(uint32_t width, uint32_t height) {
    guest_w_ = width;
    guest_h_ = height;
    g_reversed_depth = true;
    mali_profile_ = MaliProfileForced();
    if (mali_profile_) LOGR("vulkan: strict Mali profile enabled (debug.noble.mali_profile=1)");
    if (!CreateInstance() || !CreateSurface() || !PickDevice() || !CreateDevice()) return false;
    float max_pt = std::max(1.0f, std::min(props_.limits.pointSizeRange[1], 64.0f));
    vkshader::Init(app::ExeDir() / "shadercache", feats_.shaderClipDistance != VK_FALSE, max_pt);
    alloc_.Init(pd_, dev_);
    if (!CreateLayouts() || !CreateFrames() || !CreateSwapchain() || !CreateFixedImages()) return false;
    Begin();
    ui_ = InitUi();
    scale_ = app::RenderScale(guest_h_);
    aspect_ = TargetAspect();
    texturedumper::Update();
    ext_ = g_extension.get();
    if (ext_) {
        ext_->Settings();
        ext_->Reset(*this);
    }
    LOGR("vulkan: ready (%ux%u guest, %ux%u target, %ux%u swap, scale %.3f, aspect %.3f, wide %d, %s, %s, BC %s)",
         width, height,
         uint32_t(std::lround(width * scale_ * aspect_)), uint32_t(std::lround(height * scale_)),
         swap_extent_.width, swap_extent_.height, scale_, aspect_, int(app::Widescreen()),
         dynamic_ ? "dynamic rendering" : "render passes",
         timeline_ok_ ? "timeline semaphores" : "fences",
         bc_ ? "GPU" : "CPU DXT");
    return true;
}

bool VulkanBackend::CreateInstance() {
    uint32_t n = 0;
    const char* const* sdl = SDL_Vulkan_GetInstanceExtensions(&n);
    if (!sdl || !n) {
        LOGE("vulkan: SDL_Vulkan_GetInstanceExtensions failed: %s", SDL_GetError());
        return false;
    }
    std::vector<const char*> exts(sdl, sdl + n);
    uint32_t loader = VK_API_VERSION_1_0;
    if (auto enum_ver = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
            vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceVersion"))) {
        enum_ver(&loader);
    }
    VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    ai.pApplicationName = app::kGame.log;
    ai.pEngineName = "NobleArchimedes";
    ai.apiVersion = mali_profile_ ? VK_API_VERSION_1_1
                                  : std::min<uint32_t>(std::max<uint32_t>(loader, VK_API_VERSION_1_1), VK_API_VERSION_1_3);
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &ai;
    ci.enabledExtensionCount = uint32_t(exts.size());
    ci.ppEnabledExtensionNames = exts.data();
    VK_CHECK(vkCreateInstance(&ci, nullptr, &instance_));
    return true;
}

bool VulkanBackend::CreateSurface() {
    SDL_Window* win = linuxapp::g_window.load();
    if (!win) {
        if (surface_failures_++ == 0) LOGI("vulkan: waiting for SDL window");
        return false;
    }
    SDL_PropertiesID props = SDL_GetWindowProperties(win);
    if (!SDL_GetPointerProperty(props, SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER, nullptr)) {
        if (surface_failures_++ == 0) LOGI("vulkan: waiting for Android native window");
        return false;
    }
    if (!SDL_Vulkan_CreateSurface(win, instance_, nullptr, &surface_)) {
        surface_ = VK_NULL_HANDLE;
        if (surface_failures_++ == 0) LOGE("vulkan: no surface: %s", SDL_GetError());
        return false;
    }
    if (surface_failures_) LOGI("vulkan: surface created after %u tries", surface_failures_ + 1);
    surface_failures_ = 0;
    surface_lost_ = false;
    return true;
}

bool VulkanBackend::PickDevice() {
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(instance_, &n, nullptr);
    std::vector<VkPhysicalDevice> pds(n);
    if (n) vkEnumeratePhysicalDevices(instance_, &n, pds.data());
    if (pds.empty()) LOGE("vulkan: driver lists no physical devices");
    for (VkPhysicalDevice pd : pds) {
        VkPhysicalDeviceProperties p{};
        vkGetPhysicalDeviceProperties(pd, &p);
        LOGR("GPU: %s (vendor %04X, Vulkan %u.%u.%u, driver %08X)", p.deviceName, p.vendorID,
             VK_API_VERSION_MAJOR(p.apiVersion), VK_API_VERSION_MINOR(p.apiVersion),
             VK_API_VERSION_PATCH(p.apiVersion), p.driverVersion);
        if (p.apiVersion < VK_API_VERSION_1_1) {
            LOGE("vulkan: %s is below Vulkan 1.1", p.deviceName);
            continue;
        }
        uint32_t en = 0;
        vkEnumerateDeviceExtensionProperties(pd, nullptr, &en, nullptr);
        std::vector<VkExtensionProperties> ext(en);
        if (en) vkEnumerateDeviceExtensionProperties(pd, nullptr, &en, ext.data());
        if (!HasExtension(ext, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
            LOGE("vulkan: %s lacks VK_KHR_swapchain", p.deviceName);
            continue;
        }
        uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, nullptr);
        std::vector<VkQueueFamilyProperties> qs(qn);
        if (qn) vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, qs.data());
        for (uint32_t q = 0; q < qn; q++) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(pd, q, surface_, &present);
            if (!(qs[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) || !present) continue;
            pd_ = pd;
            props_ = p;
            api_ = mali_profile_ ? VK_API_VERSION_1_1 : std::min<uint32_t>(p.apiVersion, VK_API_VERSION_1_3);
            qfam_ = q;
            dev_exts_ = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
            auto want = [&](const char* name) {
                if (HasExtension(ext, name)) dev_exts_.push_back(name);
            };
            bool has_khr_dr = HasExtension(ext, VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME);
            bool dynamic = !mali_profile_ && (api_ >= VK_API_VERSION_1_3 || has_khr_dr);
            // ImGui_ImplVulkan looks up vkCmdBeginRenderingKHR via vkGetInstanceProcAddr even on 1.3.
            if (dynamic && !vkGetInstanceProcAddr(instance_, "vkCmdBeginRenderingKHR")) {
                LOGR("vulkan: vkCmdBeginRenderingKHR unavailable, using render passes");
                dynamic = false;
            }
            if (dynamic && has_khr_dr) {
                want(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME);
                if (api_ < VK_API_VERSION_1_3) {
                    want(VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME);
                    want(VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME);
                }
            }
            dynamic_ = dynamic;
            // Vulkan 1.1 timeline semaphore extension lost device on Mali-G76; use fences below 1.2.
            timeline_ok_ = !mali_profile_ && api_ >= VK_API_VERSION_1_2;
            if (!dynamic_) LOGR("vulkan: using render passes");
            if (!timeline_ok_) LOGR("vulkan: using VkFence synchronization");
            return true;
        }
        LOGE("vulkan: %s has no graphics+present queue", p.deviceName);
    }
    LOGE("vulkan: no suitable Vulkan 1.1 GPU found");
    return false;
}

bool VulkanBackend::CreateDevice() {
    VkPhysicalDeviceTimelineSemaphoreFeaturesKHR ts{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES_KHR};
    VkPhysicalDeviceDynamicRenderingFeaturesKHR dr{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR};
    auto chain = [&]() -> void* {
        ts.pNext = nullptr;
        dr.pNext = timeline_ok_ ? &ts : nullptr;
        return dynamic_ ? static_cast<void*>(&dr) : timeline_ok_ ? static_cast<void*>(&ts) : nullptr;
    };
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, chain()};
    auto get_features2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
        vkGetInstanceProcAddr(instance_, "vkGetPhysicalDeviceFeatures2"));
    if (!get_features2) {
        get_features2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
            vkGetInstanceProcAddr(instance_, "vkGetPhysicalDeviceFeatures2KHR"));
    }
    if (get_features2) {
        get_features2(pd_, &f2);
    } else {
        vkGetPhysicalDeviceFeatures(pd_, &f2.features);
        timeline_ok_ = false;
        dynamic_ = false;
    }
    if (timeline_ok_ && !ts.timelineSemaphore) {
        LOGR("vulkan: timeline semaphores unsupported, using fences");
        timeline_ok_ = false;
    }
    if (dynamic_ && !dr.dynamicRendering) {
        LOGR("vulkan: dynamic rendering unsupported, using render passes");
        dynamic_ = false;
    }

    feats_ = {};
    if (!mali_profile_) {
        feats_.samplerAnisotropy = f2.features.samplerAnisotropy;
        feats_.fillModeNonSolid = f2.features.fillModeNonSolid;
        feats_.textureCompressionBC = f2.features.textureCompressionBC;
        // Mali-G57 driver crashes on SV_ClipDistance0 pipelines; avoid enabling clip distance on ARM.
        feats_.shaderClipDistance = (props_.vendorID != kVendorArm) ? f2.features.shaderClipDistance : VK_FALSE;
    }
    bc_ = feats_.textureCompressionBC != VK_FALSE;

    VkPhysicalDeviceFeatures2 on{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, chain()};
    on.features = feats_;
    dr.dynamicRendering = VK_TRUE;
    ts.timelineSemaphore = VK_TRUE;

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qi.queueFamilyIndex = qfam_;
    qi.queueCount = 1;
    qi.pQueuePriorities = &prio;

    VkDeviceCreateInfo ci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &on};
    ci.queueCreateInfoCount = 1;
    ci.pQueueCreateInfos = &qi;
    ci.enabledExtensionCount = uint32_t(dev_exts_.size());
    ci.ppEnabledExtensionNames = dev_exts_.data();
    VK_CHECK(vkCreateDevice(pd_, &ci, nullptr, &dev_));
    vkGetDeviceQueue(dev_, qfam_, 0, &queue_);

    bool core13 = api_ >= VK_API_VERSION_1_3;
    bool core12 = api_ >= VK_API_VERSION_1_2;
    if (dynamic_) {
        begin_rendering_ = reinterpret_cast<PFN_vkCmdBeginRenderingKHR>(
            vkGetDeviceProcAddr(dev_, core13 ? "vkCmdBeginRendering" : "vkCmdBeginRenderingKHR"));
        end_rendering_ = reinterpret_cast<PFN_vkCmdEndRenderingKHR>(
            vkGetDeviceProcAddr(dev_, core13 ? "vkCmdEndRendering" : "vkCmdEndRenderingKHR"));
        if (!begin_rendering_ || !end_rendering_) {
            LOGR("vulkan: dynamic rendering proc missing, falling back to render passes");
            dynamic_ = false;
        }
    }
    if (timeline_ok_) {
        wait_semaphores_ = reinterpret_cast<PFN_vkWaitSemaphoresKHR>(
            vkGetDeviceProcAddr(dev_, core12 ? "vkWaitSemaphores" : "vkWaitSemaphoresKHR"));
        counter_value_ = reinterpret_cast<PFN_vkGetSemaphoreCounterValueKHR>(
            vkGetDeviceProcAddr(dev_, core12 ? "vkGetSemaphoreCounterValue" : "vkGetSemaphoreCounterValueKHR"));
        if (!wait_semaphores_ || !counter_value_) {
            LOGR("vulkan: timeline semaphore proc missing, falling back to fences");
            timeline_ok_ = false;
        }
    }

    auto usable_depth = [&](VkFormat f) {
        VkFormatProperties fp{};
        vkGetPhysicalDeviceFormatProperties(pd_, f, &fp);
        VkFormatFeatureFlags need = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
        return (fp.optimalTilingFeatures & need) == need;
    };
    // Prefer D32S8 with reversed depth when available (higher precision and widely supported on Mali/Adreno),
    // falling back to D24S8.
    depth_fmt_ = usable_depth(VK_FORMAT_D32_SFLOAT_S8_UINT) ? VK_FORMAT_D32_SFLOAT_S8_UINT : VK_FORMAT_D24_UNORM_S8_UINT;
    if (!usable_depth(depth_fmt_)) depth_fmt_ = VK_FORMAT_D24_UNORM_S8_UINT;

    VkFormatProperties fp{};
    vkGetPhysicalDeviceFormatProperties(pd_, depth_fmt_, &fp);
    depth_linear_ = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0;
    vkGetPhysicalDeviceFormatProperties(pd_, VK_FORMAT_R32_SFLOAT, &fp);
    r32f_linear_ = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0;
    LOGR("vulkan: depth %s (linear %d), R32F linear %d, aniso %d (max %.1f)",
         depth_fmt_ == VK_FORMAT_D32_SFLOAT_S8_UINT ? "D32S8" : "D24S8",
         int(depth_linear_), int(r32f_linear_), int(feats_.samplerAnisotropy),
         feats_.samplerAnisotropy ? props_.limits.maxSamplerAnisotropy : 1.0f);
    return true;
}

bool VulkanBackend::CreateSwapchain() {
    if (!surface_) return false;
    VkSurfaceCapabilitiesKHR caps{};
    VkResult cr = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd_, surface_, &caps);
    if (cr != VK_SUCCESS) {
        if (cr == VK_ERROR_SURFACE_LOST_KHR) surface_lost_ = true;
        LOGE("vulkan: surface capabilities failed (%d)", int(cr));
        return false;
    }
    uint32_t n = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(pd_, surface_, &n, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(n);
    if (n) vkGetPhysicalDeviceSurfaceFormatsKHR(pd_, surface_, &n, formats.data());
    if (formats.empty()) return false;
    VkSurfaceFormatKHR chosen = formats[0];
    for (const auto& f : formats) {
        if ((f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            chosen = f;
            break;
        }
    }

    app::Mode wp = app::WindowPixels();
    uint32_t w = wp.width, h = wp.height;
    if (!w || !h) {
        w = caps.currentExtent.width;
        h = caps.currentExtent.height;
    }
    if (!w || !h || w == 0xFFFFFFFFu || h == 0xFFFFFFFFu) return false;
    if (caps.minImageExtent.width && caps.maxImageExtent.width)
        w = std::clamp(w, caps.minImageExtent.width, caps.maxImageExtent.width);
    if (caps.minImageExtent.height && caps.maxImageExtent.height)
        h = std::clamp(h, caps.minImageExtent.height, caps.maxImageExtent.height);

    vsync_ = app::Get().vsync;
    uint32_t pn = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(pd_, surface_, &pn, nullptr);
    std::vector<VkPresentModeKHR> modes(pn);
    if (pn) vkGetPhysicalDeviceSurfacePresentModesKHR(pd_, surface_, &pn, modes.data());
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    if (!vsync_) {
        for (VkPresentModeKHR m : {VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR}) {
            if (std::find(modes.begin(), modes.end(), m) != modes.end()) {
                mode = m;
                break;
            }
        }
    }

    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = surface_;
    ci.minImageCount = std::max(caps.minImageCount, 3u);
    if (caps.maxImageCount) ci.minImageCount = std::min(ci.minImageCount, caps.maxImageCount);
    ci.imageFormat = chosen.format;
    ci.imageColorSpace = chosen.colorSpace;
    ci.imageExtent = {w, h};
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)
                          ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR
                          : caps.currentTransform;
    ci.compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
                            ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                            : VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    ci.presentMode = mode;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = swap_;
    if (swap_) vkQueueWaitIdle(queue_);
    VkSwapchainKHR fresh = VK_NULL_HANDLE;
    VkResult sr = vkCreateSwapchainKHR(dev_, &ci, nullptr, &fresh);
    if (sr != VK_SUCCESS) {
        if (sr == VK_ERROR_SURFACE_LOST_KHR || sr == VK_ERROR_NATIVE_WINDOW_IN_USE_KHR) surface_lost_ = true;
        LOGE("vulkan: swap chain %ux%u failed (%d)", w, h, int(sr));
        return false;
    }
    DestroySwapchain();
    swap_ = fresh;
    swap_fmt_ = chosen.format;
    swap_extent_ = ci.imageExtent;

    uint32_t count = 0;
    vkGetSwapchainImagesKHR(dev_, swap_, &count, nullptr);
    swap_images_.resize(count);
    vkGetSwapchainImagesKHR(dev_, swap_, &count, swap_images_.data());
    for (VkImage im : swap_images_) {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = im;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = swap_fmt_;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView v = VK_NULL_HANDLE;
        vkCreateImageView(dev_, &vi, nullptr, &v);
        swap_views_.push_back(v);
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkSemaphore s = VK_NULL_HANDLE;
        vkCreateSemaphore(dev_, &si, nullptr, &s);
        swap_done_.push_back(s);
    }
    swap_stale_ = false;
    LOGR("vulkan: swap chain %ux%u, %u images, format %d, %s", w, h, count, int(swap_fmt_),
         vsync_ ? "vsync" : "no vsync");
    return true;
}

void VulkanBackend::DestroySwapchain() {
    if (!swap_) return;
    vkQueueWaitIdle(queue_);
    for (VkImageView v : swap_views_) {
        DropFramebuffers(v);
        vkDestroyImageView(dev_, v, nullptr);
    }
    for (VkSemaphore s : swap_done_) vkDestroySemaphore(dev_, s, nullptr);
    swap_views_.clear();
    swap_done_.clear();
    swap_images_.clear();
    vkDestroySwapchainKHR(dev_, swap_, nullptr);
    swap_ = VK_NULL_HANDLE;
}

bool VulkanBackend::CreateLayouts() {
    VkDescriptorSetLayoutBinding b[vkshader::kBindings] = {};
    for (uint32_t i = 0; i < vkshader::kBindings; i++) {
        b[i].binding = i;
        b[i].descriptorCount = 1;
        b[i].descriptorType = i < vkshader::kBindingTex       ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC
                              : i < vkshader::kBindingSampler ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE
                                                              : VK_DESCRIPTOR_TYPE_SAMPLER;
        b[i].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = vkshader::kBindings;
    li.pBindings = b;
    VK_CHECK(vkCreateDescriptorSetLayout(dev_, &li, nullptr, &set_layout_));

    VkPipelineLayoutCreateInfo pi{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pi.setLayoutCount = 1;
    pi.pSetLayouts = &set_layout_;
    VK_CHECK(vkCreatePipelineLayout(dev_, &pi, nullptr, &layout_));

    std::vector<char> blob;
    std::ifstream f(app::ExeDir() / "shadercache" / "pipelines.bin", std::ios::binary);
    if (f) blob.assign(std::istreambuf_iterator<char>(f), {});
    VkPipelineCacheCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    ci.initialDataSize = blob.size();
    ci.pInitialData = blob.data();
    if (vkCreatePipelineCache(dev_, &ci, nullptr, &cache_) != VK_SUCCESS) {
        ci.initialDataSize = 0;
        ci.pInitialData = nullptr;
        VK_CHECK(vkCreatePipelineCache(dev_, &ci, nullptr, &cache_));
    }
    cache_saved_ = blob.size();
    return true;
}

bool VulkanBackend::CreateFrames() {
    if (timeline_ok_) {
        VkSemaphoreTypeCreateInfoKHR ti{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO_KHR};
        ti.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE_KHR;
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &ti};
        VK_CHECK(vkCreateSemaphore(dev_, &si, nullptr, &timeline_));
    }
    for (Frame& f : frames_) {
        VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pi.queueFamilyIndex = qfam_;
        VK_CHECK(vkCreateCommandPool(dev_, &pi, nullptr, &f.pool));
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = f.pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VK_CHECK(vkAllocateCommandBuffers(dev_, &ai, &f.cb));
        if (!CreateBuffer(f.upload, kUploadSize,
                          VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                              VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                          true))
            return false;
        VkSemaphoreCreateInfo bi{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VK_CHECK(vkCreateSemaphore(dev_, &bi, nullptr, &f.acquire));
    }
    return true;
}

bool VulkanBackend::CreateFixedImages() {
    constexpr VkImageUsageFlags kSampled = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (!CreateImage(lut_, kBgra, 256, 1, 1, 1, 1, kSampled, VK_IMAGE_VIEW_TYPE_2D) ||
        !CreateImage(dummy_[0], kBgra, 1, 1, 1, 1, 1, kSampled, VK_IMAGE_VIEW_TYPE_2D) ||
        !CreateImage(dummy_[1], kBgra, 1, 1, 1, 1, 1, kSampled, VK_IMAGE_VIEW_TYPE_3D) ||
        !CreateImage(dummy_[2], kBgra, 1, 1, 1, 6, 1, kSampled, VK_IMAGE_VIEW_TYPE_CUBE, true))
        return false;

    VkSamplerCreateInfo sc{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sc.magFilter = sc.minFilter = VK_FILTER_LINEAR;
    sc.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sc.addressModeU = sc.addressModeV = sc.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sc.maxLod = VK_LOD_CLAMP_NONE;
    linear_ = Sampler(sc);
    sc.magFilter = sc.minFilter = VK_FILTER_NEAREST;
    sc.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    point_ = Sampler(sc);

    Begin();
    VkClearColorValue black{};
    for (Image* im : {&lut_, &dummy_[0], &dummy_[1], &dummy_[2]}) {
        Transition(*im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageSubresourceRange r{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, im->layers};
        vkCmdClearColorImage(Cb(), im->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &r);
        Transition(*im, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    for (int i = 0; i < 768; i++) ramp_[i] = uint8_t(i & 255);
    UploadGammaRamp();
    Submit(true, false);
    vkEndCommandBuffer(Cb());
    vkResetCommandPool(dev_, CurrentFrame().pool, 0);
    return true;
}

bool VulkanBackend::InitUi() {
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pi.maxSets = 16;
    pi.poolSizeCount = 1;
    pi.pPoolSizes = &ps;
    if (vkCreateDescriptorPool(dev_, &pi, nullptr, &ui_pool_) != VK_SUCCESS) return false;
    std::lock_guard lock(app::UiLock());
    if (!ImGui::GetCurrentContext()) return false;
    ImGui_ImplVulkan_InitInfo ii{};
    ii.Instance = instance_;
    ii.PhysicalDevice = pd_;
    ii.Device = dev_;
    ii.QueueFamily = qfam_;
    ii.Queue = queue_;
    ii.DescriptorPool = ui_pool_;
    ii.PipelineCache = cache_;
    ii.MinImageCount = 2;
    ii.ImageCount = uint32_t(std::max<size_t>(swap_images_.size(), 2));
    ii.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    ii.UseDynamicRendering = dynamic_;
    ii.PipelineRenderingCreateInfo = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR};
    ii.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    ii.PipelineRenderingCreateInfo.pColorAttachmentFormats = &swap_fmt_;
    if (!dynamic_) ii.RenderPass = Pass(swap_fmt_, VK_FORMAT_UNDEFINED);
    ImGui::GetIO().BackendRendererName = "vulkan";
    return ImGui_ImplVulkan_Init(&ii);
}

// ---- Memory and object helpers

bool VulkanBackend::CreateImage(Image& im, VkFormat fmt, uint32_t w, uint32_t h, uint32_t d, uint32_t layers,
                                uint32_t levels, VkImageUsageFlags usage, VkImageViewType vt, bool cube) {
    im = {};
    im.fmt = fmt;
    im.w = w;
    im.h = h;
    im.d = d;
    im.layers = layers;
    im.levels = levels;
    im.depth = (fmt == depth_fmt_);
    im.stencil = im.depth && HasStencil(fmt);

    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
    ci.imageType = d > 1 ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    ci.format = fmt;
    ci.extent = {w, h, d};
    ci.mipLevels = levels;
    ci.arrayLayers = layers;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(dev_, &ci, nullptr, &im.image) != VK_SUCCESS) return false;

    VkMemoryRequirements r{};
    vkGetImageMemoryRequirements(dev_, im.image, &r);
    if (!alloc_.Get(r, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, false, im.mem) ||
        vkBindImageMemory(dev_, im.image, im.mem.mem, im.mem.off) != VK_SUCCESS) {
        Destroy(im);
        return false;
    }

    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = im.image;
    vi.viewType = vt;
    vi.format = fmt;
    vi.subresourceRange = {VkImageAspectFlags(im.depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT), 0,
                           levels, 0, layers};
    if (usage & VK_IMAGE_USAGE_SAMPLED_BIT) vkCreateImageView(dev_, &vi, nullptr, &im.view);
    if (usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.subresourceRange = {
            VkImageAspectFlags(im.depth ? (VK_IMAGE_ASPECT_DEPTH_BIT | (im.stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0))
                                        : VK_IMAGE_ASPECT_COLOR_BIT),
            0, 1, 0, 1};
        vkCreateImageView(dev_, &vi, nullptr, &im.attach);
    }
    return true;
}

bool VulkanBackend::CreateBuffer(Buffer& b, VkDeviceSize size, VkBufferUsageFlags usage, bool host) {
    b = {};
    VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    ci.size = std::max<VkDeviceSize>(size, 16);
    ci.usage = usage;
    if (vkCreateBuffer(dev_, &ci, nullptr, &b.buf) != VK_SUCCESS) return false;
    VkMemoryRequirements r{};
    vkGetBufferMemoryRequirements(dev_, b.buf, &r);
    VkMemoryPropertyFlags want =
        host ? (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
             : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    VkMemoryPropertyFlags nice = host && (usage & VK_BUFFER_USAGE_TRANSFER_DST_BIT) ? VK_MEMORY_PROPERTY_HOST_CACHED_BIT : 0;
    if (!alloc_.Get(r, want, nice, true, b.mem) || vkBindBufferMemory(dev_, b.buf, b.mem.mem, b.mem.off) != VK_SUCCESS) {
        Destroy(b);
        return false;
    }
    return true;
}

void VulkanBackend::Destroy(Image& im) {
    if (im.view) vkDestroyImageView(dev_, im.view, nullptr);
    DropFramebuffers(im.attach);
    if (im.attach) vkDestroyImageView(dev_, im.attach, nullptr);
    if (im.image) vkDestroyImage(dev_, im.image, nullptr);
    alloc_.Free(im.mem);
    im = {};
}

void VulkanBackend::Destroy(Buffer& b) {
    if (b.buf) vkDestroyBuffer(dev_, b.buf, nullptr);
    alloc_.Free(b.mem);
    b = {};
}

VulkanBackend::Alloc VulkanBackend::Upload(VkDeviceSize size, VkDeviceSize align) {
    align = std::max<VkDeviceSize>(align, 16);
    if (size > kUploadSize / 4) {
        Buffer b;
        if (!CreateBuffer(b, size,
                          VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                              VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                          true))
            return {};
        Alloc a{b.buf, 0, b.mem.map};
        CurrentFrame().buffers.push_back(b);
        return a;
    }
    Frame* f = &CurrentFrame();
    VkDeviceSize o = (f->used + align - 1) / align * align;
    if (o + size > kUploadSize) {
        Submit(true, false);
        f = &CurrentFrame();
        o = 0;
    }
    f->used = o + size;
    return {f->upload.buf, o, f->upload.mem.map + o};
}

VulkanBackend::Alloc VulkanBackend::Constants(const void* data, size_t bytes) {
    VkDeviceSize align = std::max<VkDeviceSize>(kUboAlign, props_.limits.minUniformBufferOffsetAlignment);
    Alloc a = Upload(kUboRange, align);
    if (!a.cpu) return {};
    if (data && bytes) std::memcpy(a.cpu, data, std::min<size_t>(bytes, kUboRange));
    else std::memset(a.cpu, 0, kUboRange);
    return a;
}

// ---- Command recording and synchronization

void VulkanBackend::Begin() {
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(Cb(), &bi);
    rendering_ = false;
    rend_color_ = rend_depth_ = nullptr;
}

uint64_t VulkanBackend::Completed() {
    if (!timeline_ok_) {
        std::lock_guard lock(fence_lock_);
        PollFences();
        return fenced_;
    }
    uint64_t v = 0;
    counter_value_(dev_, timeline_, &v);
    return v;
}

void VulkanBackend::PollFences() {
    size_t done = 0;
    while (done < pending_.size() && vkGetFenceStatus(dev_, pending_[done].fence) == VK_SUCCESS) {
        fenced_ = pending_[done].value;
        vkResetFences(dev_, 1, &pending_[done].fence);
        spare_fences_.push_back(pending_[done].fence);
        done++;
    }
    pending_.erase(pending_.begin(), pending_.begin() + ptrdiff_t(done));
}

VkFence VulkanBackend::SubmitFence(uint64_t value) {
    VkFence f = VK_NULL_HANDLE;
    if (!spare_fences_.empty()) {
        f = spare_fences_.back();
        spare_fences_.pop_back();
    } else {
        VkFenceCreateInfo ci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (vkCreateFence(dev_, &ci, nullptr, &f) != VK_SUCCESS) return VK_NULL_HANDLE;
    }
    pending_.push_back({value, f});
    return f;
}

void VulkanBackend::WaitValue(uint64_t v) {
    if (!v || Completed() >= v) return;
    if (!timeline_ok_) {
        std::lock_guard lock(fence_lock_);
        for (const Pending& p : pending_) {
            if (p.value >= v) {
                vkWaitForFences(dev_, 1, &p.fence, VK_TRUE, UINT64_MAX);
                break;
            }
        }
        PollFences();
        return;
    }
    VkSemaphoreWaitInfoKHR wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO_KHR};
    wi.semaphoreCount = 1;
    wi.pSemaphores = &timeline_;
    wi.pValues = &v;
    wait_semaphores_(dev_, &wi, UINT64_MAX);
}

void VulkanBackend::ResetFrame(Frame& f) {
    WaitValue(f.value);
    for (Image& im : f.images) Destroy(im);
    for (Buffer& b : f.buffers) Destroy(b);
    f.images.clear();
    f.buffers.clear();
    for (VkDescriptorPool p : f.pools) vkResetDescriptorPool(dev_, p, 0);
    f.pool_at = 0;
    f.sets.clear();
    f.used = 0;
}

void VulkanBackend::Submit(bool wait, bool present) {
    EndRender();
    Frame& f = CurrentFrame();
    vkEndCommandBuffer(f.cb);
    uint64_t signal = ++value_;
    VkSemaphore signals[2] = {timeline_, VK_NULL_HANDLE};
    uint64_t values[2] = {signal, 0};
    uint32_t first = timeline_ok_ ? 0 : 1;
    VkTimelineSemaphoreSubmitInfoKHR ti{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO_KHR};
    ti.signalSemaphoreValueCount = present ? 2 : 1;
    ti.pSignalSemaphoreValues = values;
    uint64_t wait_val = 0;
    ti.waitSemaphoreValueCount = present ? 1 : 0;
    ti.pWaitSemaphoreValues = &wait_val;

    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO, timeline_ok_ ? &ti : nullptr};
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    if (present) {
        signals[1] = swap_done_[present_index_];
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &f.acquire;
        si.pWaitDstStageMask = &stage;
    }
    si.commandBufferCount = 1;
    si.pCommandBuffers = &f.cb;
    si.signalSemaphoreCount = (present ? 2 : 1) - first;
    si.pSignalSemaphores = signals + first;

    VkResult r;
    if (timeline_ok_) {
        r = vkQueueSubmit(queue_, 1, &si, VK_NULL_HANDLE);
    } else {
        std::lock_guard lock(fence_lock_);
        r = vkQueueSubmit(queue_, 1, &si, SubmitFence(signal));
    }
    if (r != VK_SUCCESS)
        LOGE("vulkan: submit %llu%s failed (%d)", (unsigned long long)signal, present ? " (present)" : "", int(r));
    f.value = signal;
    if (wait) {
        ResetFrame(f);
        vkResetCommandPool(dev_, f.pool, 0);
        Begin();
    }
    DeliverReports();
}

void VulkanBackend::Transition(Image& im, VkImageLayout to) {
    if (im.layout == to) return;
    EndRender();
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    VkPipelineStageFlags src, dst;
    LayoutMasks(im.layout, src, b.srcAccessMask);
    LayoutMasks(to, dst, b.dstAccessMask);
    if (im.layout == VK_IMAGE_LAYOUT_UNDEFINED) src = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    b.oldLayout = im.layout;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = im.image;
    VkImageAspectFlags aspect =
        im.depth ? (VK_IMAGE_ASPECT_DEPTH_BIT | (im.stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0)) : VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange = {aspect, 0, im.levels, 0, im.layers};
    vkCmdPipelineBarrier(Cb(), src, dst, 0, 0, nullptr, 0, nullptr, 1, &b);
    im.layout = to;
}

void VulkanBackend::BeginRender(Image* color, Image* depth) {
    if (rendering_ && color == rend_color_ && depth == rend_depth_) return;
    EndRender();
    if (color) Transition(*color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    if (depth) Transition(*depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);

    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                       VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    VkPipelineStageFlags st = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                              VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    vkCmdPipelineBarrier(Cb(), st, st, 0, 1, &mb, 0, nullptr, 0, nullptr);

    rendering_ = true;
    rend_color_ = color;
    rend_depth_ = depth;
    Image* any = color ? color : depth;
    if (!dynamic_) {
        VkFormat cf = color ? color->fmt : VK_FORMAT_UNDEFINED;
        VkFormat df = depth ? depth->fmt : VK_FORMAT_UNDEFINED;
        VkRenderPassBeginInfo bi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        bi.renderPass = Pass(cf, df);
        bi.framebuffer = Framebuffer(cf, df, color ? color->attach : VK_NULL_HANDLE,
                                     depth ? depth->attach : VK_NULL_HANDLE, any->w, any->h);
        bi.renderArea = {{0, 0}, {any->w, any->h}};
        vkCmdBeginRenderPass(Cb(), &bi, VK_SUBPASS_CONTENTS_INLINE);
        return;
    }

    VkRenderingAttachmentInfoKHR ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR};
    VkRenderingAttachmentInfoKHR da{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR};
    VkRenderingInfoKHR ri{VK_STRUCTURE_TYPE_RENDERING_INFO_KHR};
    ri.renderArea = {{0, 0}, {any->w, any->h}};
    ri.layerCount = 1;
    if (color) {
        ca.imageView = color->attach;
        ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &ca;
    }
    if (depth) {
        da.imageView = depth->attach;
        da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        da.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        ri.pDepthAttachment = &da;
        if (depth->stencil) ri.pStencilAttachment = &da;
    }
    begin_rendering_(Cb(), &ri);
}

void VulkanBackend::EndRender() {
    if (!rendering_) return;
    if (dynamic_) end_rendering_(Cb());
    else vkCmdEndRenderPass(Cb());
    rendering_ = false;
    rend_color_ = rend_depth_ = nullptr;
}

VkRenderPass VulkanBackend::Pass(VkFormat color, VkFormat depth, bool clear) {
    const uint64_t k[3] = {uint64_t(color), uint64_t(depth), uint64_t(clear)};
    uint64_t key = Hash(k, sizeof(k));
    if (auto it = passes_.find(key); it != passes_.end()) return it->second;
    VkAttachmentDescription at[2] = {};
    VkAttachmentReference cr{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference dr{0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sp{};
    sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    uint32_t n = 0;
    if (color != VK_FORMAT_UNDEFINED) {
        at[n] = {0, color, VK_SAMPLE_COUNT_1_BIT,
                 clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD,
                 VK_ATTACHMENT_STORE_OP_STORE,
                 VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE,
                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        cr.attachment = n++;
        sp.colorAttachmentCount = 1;
        sp.pColorAttachments = &cr;
    }
    if (depth != VK_FORMAT_UNDEFINED) {
        at[n] = {0, depth, VK_SAMPLE_COUNT_1_BIT,
                 VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE,
                 VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE,
                 VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        dr.attachment = n++;
        sp.pDepthStencilAttachment = &dr;
    }
    VkRenderPassCreateInfo ci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    ci.attachmentCount = n;
    ci.pAttachments = at;
    ci.subpassCount = 1;
    ci.pSubpasses = &sp;
    VkRenderPass rp = VK_NULL_HANDLE;
    if (vkCreateRenderPass(dev_, &ci, nullptr, &rp) != VK_SUCCESS) LOGE("vulkan: vkCreateRenderPass failed");
    passes_[key] = rp;
    return rp;
}

VkFramebuffer VulkanBackend::Framebuffer(VkFormat color_fmt, VkFormat depth_fmt, VkImageView color, VkImageView depth,
                                         uint32_t w, uint32_t h) {
    const uint64_t k[4] = {uint64_t(color), uint64_t(depth), w, h};
    uint64_t key = Hash(k, sizeof(k));
    if (auto it = framebuffers_.find(key); it != framebuffers_.end()) return it->second.fb;
    VkImageView views[2];
    uint32_t n = 0;
    if (color) views[n++] = color;
    if (depth) views[n++] = depth;
    VkFramebufferCreateInfo ci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    ci.renderPass = Pass(color_fmt, depth_fmt);
    ci.attachmentCount = n;
    ci.pAttachments = views;
    ci.width = w;
    ci.height = h;
    ci.layers = 1;
    VkFramebuffer fb = VK_NULL_HANDLE;
    if (vkCreateFramebuffer(dev_, &ci, nullptr, &fb) != VK_SUCCESS) LOGE("vulkan: framebuffer %ux%u failed", w, h);
    framebuffers_[key] = {color, depth, fb};
    return fb;
}

void VulkanBackend::DropFramebuffers(VkImageView v) {
    if (!v || framebuffers_.empty()) return;
    for (auto it = framebuffers_.begin(); it != framebuffers_.end();) {
        if (it->second.color == v || it->second.depth == v) {
            vkDestroyFramebuffer(dev_, it->second.fb, nullptr);
            it = framebuffers_.erase(it);
        } else {
            ++it;
        }
    }
}

// Negative viewport height (Vulkan 1.1 core) matches D3D Y-up clip space with Y-down screen space.
void VulkanBackend::SetViewport(float x, float y, float w, float h) {
    VkViewport vp{x, y + h, w, -h, 0.0f, 1.0f};
    vkCmdSetViewport(Cb(), 0, 1, &vp);
}

void VulkanBackend::SetScissor(int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
    int32_t lx = std::max(x0, 0), ly = std::max(y0, 0);
    VkRect2D r{{lx, ly}, {uint32_t(std::max(x1 - lx, 0)), uint32_t(std::max(y1 - ly, 0))}};
    vkCmdSetScissor(Cb(), 0, 1, &r);
}

VkDescriptorSet VulkanBackend::Set(const VkImageView views[vkshader::kTexBindings], const VkSampler samplers[4]) {
    Frame& f = CurrentFrame();
    uint64_t key = Hash(views, sizeof(VkImageView) * vkshader::kTexBindings, Hash(samplers, sizeof(VkSampler) * 4));
    if (auto it = f.sets.find(key); it != f.sets.end()) return it->second;
    VkDescriptorSet set = VK_NULL_HANDLE;
    for (;;) {
        if (f.pool_at == f.pools.size()) {
            VkDescriptorPoolSize sizes[3] = {
                {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, kSetsPerPool * 2},
                {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kSetsPerPool * vkshader::kTexBindings},
                {VK_DESCRIPTOR_TYPE_SAMPLER, kSetsPerPool * vkshader::kSamplerBindings},
            };
            VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
            pi.maxSets = kSetsPerPool;
            pi.poolSizeCount = 3;
            pi.pPoolSizes = sizes;
            VkDescriptorPool p = VK_NULL_HANDLE;
            if (vkCreateDescriptorPool(dev_, &pi, nullptr, &p) != VK_SUCCESS) return VK_NULL_HANDLE;
            f.pools.push_back(p);
        }
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = f.pools[f.pool_at];
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &set_layout_;
        if (vkAllocateDescriptorSets(dev_, &ai, &set) == VK_SUCCESS) break;
        f.pool_at++;
    }

    VkDescriptorBufferInfo bi{f.upload.buf, 0, kUboRange};
    VkDescriptorImageInfo ii[vkshader::kTexBindings];
    VkDescriptorImageInfo si[vkshader::kSamplerBindings];
    VkWriteDescriptorSet w[vkshader::kBindings];
    for (uint32_t i = 0; i < vkshader::kBindings; i++) {
        w[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[i].dstSet = set;
        w[i].dstBinding = i;
        w[i].descriptorCount = 1;
        if (i < vkshader::kBindingTex) {
            w[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
            w[i].pBufferInfo = &bi;
        } else if (i < vkshader::kBindingSampler) {
            uint32_t s = i - vkshader::kBindingTex;
            ii[s] = {VK_NULL_HANDLE, views[s] ? views[s] : dummy_[0].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            w[i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            w[i].pImageInfo = &ii[s];
        } else {
            uint32_t s = i - vkshader::kBindingSampler;
            si[s] = {samplers[s] ? samplers[s] : point_, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
            w[i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
            w[i].pImageInfo = &si[s];
        }
    }
    vkUpdateDescriptorSets(dev_, vkshader::kBindings, w, 0, nullptr);
    f.sets[key] = set;
    return set;
}

void VulkanBackend::BindSet(VkDescriptorSet set, VkDeviceSize vc, VkDeviceSize dc) {
    uint32_t offs[2] = {uint32_t(vc), uint32_t(dc)};
    vkCmdBindDescriptorSets(Cb(), VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &set, 2, offs);
}

// ---- Shaders, pipelines, samplers

VkShaderModule VulkanBackend::Module(const std::string& src, const char* entry, vkshader::Stage stage, uint64_t& hash) {
    uint64_t seed = Hash(entry, std::strlen(entry), stage == vkshader::Stage::Vertex ? 1 : 2);
    hash = Hash(src.data(), src.size(), seed);
    if (auto it = modules_.find(hash); it != modules_.end()) return it->second;
    std::vector<uint32_t> spirv;
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkshader::Compile(src, entry, stage, hash, spirv)) {
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = spirv.size() * 4;
        ci.pCode = spirv.data();
        if (vkCreateShaderModule(dev_, &ci, nullptr, &m) != VK_SUCCESS) m = VK_NULL_HANDLE;
    }
    modules_[hash] = m;
    return m;
}

VkPipeline VulkanBackend::FullScreenPipeline(const char* hlsl, const char* ps_entry, VkFormat color, VkFormat depth,
                                             bool over) {
    uint64_t key = Hash(hlsl, std::strlen(hlsl), (uint64_t(color) << 32) | (uint64_t(depth) << 1) | (over ? 1 : 0));
    key = Hash(ps_entry, std::strlen(ps_entry), key);
    if (auto it = pipelines_.find(key); it != pipelines_.end()) return it->second;
    uint64_t hv = 0, hp = 0;
    VkShaderModule vs = Module(hlsl, "vs", vkshader::Stage::Vertex, hv);
    VkShaderModule ps = Module(hlsl, ps_entry, vkshader::Stage::Fragment, hp);
    VkPipeline pipe = VK_NULL_HANDLE;
    if (vs && ps) {
        VkPipelineShaderStageCreateInfo st[2] = {
            {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
            {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
        };
        st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        st[0].module = vs;
        st[0].pName = "main";
        st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        st[1].module = ps;
        st[1].pName = "main";

        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vp.viewportCount = vp.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = VK_CULL_MODE_NONE;
        rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        ds.depthTestEnable = ds.depthWriteEnable = (depth != VK_FORMAT_UNDEFINED);
        ds.depthCompareOp = VK_COMPARE_OP_ALWAYS;
        VkPipelineColorBlendAttachmentState ba{};
        ba.colorWriteMask = 0xF;
        if (over) {
            ba.blendEnable = VK_TRUE;
            ba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
            ba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
            ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        }
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cb.attachmentCount = (color != VK_FORMAT_UNDEFINED) ? 1 : 0;
        cb.pAttachments = &ba;
        VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dy.dynamicStateCount = 2;
        dy.pDynamicStates = dyn;

        VkPipelineRenderingCreateInfoKHR rinfo{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR};
        rinfo.colorAttachmentCount = cb.attachmentCount;
        rinfo.pColorAttachmentFormats = &color;
        rinfo.depthAttachmentFormat = depth;
        rinfo.stencilAttachmentFormat = HasStencil(depth) ? depth : VK_FORMAT_UNDEFINED;

        VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, dynamic_ ? &rinfo : nullptr};
        if (!dynamic_) pi.renderPass = Pass(color, depth);
        pi.stageCount = 2;
        pi.pStages = st;
        pi.pVertexInputState = &vi;
        pi.pInputAssemblyState = &ia;
        pi.pViewportState = &vp;
        pi.pRasterizationState = &rs;
        pi.pMultisampleState = &ms;
        pi.pDepthStencilState = &ds;
        pi.pColorBlendState = &cb;
        pi.pDynamicState = &dy;
        pi.layout = layout_;
        if (vkCreateGraphicsPipelines(dev_, cache_, 1, &pi, nullptr, &pipe) != VK_SUCCESS) {
            LOGE("vulkan: fullscreen pipeline failed on %s (vendor %04X, driver %08X)",
                 props_.deviceName, props_.vendorID, props_.driverVersion);
            pipe = VK_NULL_HANDLE;
        }
    }
    pipelines_[key] = pipe;
    return pipe;
}

VkPipeline VulkanBackend::EnsurePipeline(const PsoKey& k, VkShaderModule vs, VkShaderModule ps) {
    uint64_t h = Hash(&k, sizeof(k));
    if (auto it = pipelines_.find(h); it != pipelines_.end()) return it->second;

    VkPipelineShaderStageCreateInfo st[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
    };
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    st[0].module = vs;
    st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    st[1].module = ps;
    st[1].pName = "main";

    VkVertexInputBindingDescription vb[16];
    VkVertexInputAttributeDescription va[16];
    uint32_t na = 0;
    for (uint32_t i = 0; i < k.buffers; i++) vb[i] = {i, k.strides[i], VK_VERTEX_INPUT_RATE_VERTEX};
    for (uint32_t r = 0; r < 16; r++) {
        if (!((k.regs >> r) & 1)) continue;
        uint32_t e = k.elements[r];
        va[na++] = {r, (e >> 8) & 0xFF, VertexFormat(e & 0xFF), e >> 16};
    }

    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = k.buffers;
    vi.pVertexBindingDescriptions = vb;
    vi.vertexAttributeDescriptionCount = na;
    vi.pVertexAttributeDescriptions = va;

    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    Topology topo = Topology(k.topo & 0xF);
    bool strip = (k.topo >> 4) != 0;
    ia.topology = topo == Topology::Points  ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST
                  : topo == Topology::Lines ? (strip ? VK_PRIMITIVE_TOPOLOGY_LINE_STRIP : VK_PRIMITIVE_TOPOLOGY_LINE_LIST)
                                            : (strip ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);

    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    constexpr uint32_t kFillWireframe = 0x1B01;
    rs.polygonMode = (k.fill == kFillWireframe && feats_.fillModeNonSolid) ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
    rs.cullMode = k.cull ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
    rs.frontFace = k.ccw ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
    rs.depthBiasEnable = k.zslope != 0.0f;
    rs.depthBiasConstantFactor = 0.0f;
    rs.depthBiasSlopeFactor = k.zslope;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = k.zen != 0;
    ds.depthWriteEnable = k.zwrite != 0;
    ds.depthCompareOp = ReversedCmp(k.zfunc);
    ds.stencilTestEnable = k.sten != 0;
    VkStencilOpState so{StencilOp(k.sfail), StencilOp(k.spass), StencilOp(k.szfail), Cmp(k.sfunc), k.smask, k.swmask, 0};
    ds.front = ds.back = so;

    VkPipelineColorBlendAttachmentState ba{};
    ba.blendEnable = k.blend != 0;
    ba.srcColorBlendFactor = Blend(k.src, false);
    ba.dstColorBlendFactor = Blend(k.dst, false);
    ba.colorBlendOp = BlendOp(k.op);
    ba.srcAlphaBlendFactor = Blend(k.src, true);
    ba.dstAlphaBlendFactor = Blend(k.dst, true);
    ba.alphaBlendOp = BlendOp(k.op);
    ba.colorWriteMask = k.wmask;

    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = k.has_rt ? 1 : 0;
    cb.pAttachments = &ba;

    VkDynamicState dyn[4] = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR,
        VK_DYNAMIC_STATE_BLEND_CONSTANTS,
        VK_DYNAMIC_STATE_STENCIL_REFERENCE,
    };
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = 4;
    dy.pDynamicStates = dyn;

    VkFormat color = VkFormat(k.rtfmt);
    VkPipelineRenderingCreateInfoKHR rinfo{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR};
    rinfo.colorAttachmentCount = cb.attachmentCount;
    rinfo.pColorAttachmentFormats = &color;
    rinfo.depthAttachmentFormat = k.has_ds ? depth_fmt_ : VK_FORMAT_UNDEFINED;
    rinfo.stencilAttachmentFormat = (k.has_ds && HasStencil(depth_fmt_)) ? depth_fmt_ : VK_FORMAT_UNDEFINED;

    VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, dynamic_ ? &rinfo : nullptr};
    if (!dynamic_) pi.renderPass = Pass(k.has_rt ? color : VK_FORMAT_UNDEFINED, k.has_ds ? depth_fmt_ : VK_FORMAT_UNDEFINED);
    pi.stageCount = 2;
    pi.pStages = st;
    pi.pVertexInputState = &vi;
    pi.pInputAssemblyState = &ia;
    pi.pViewportState = &vp;
    pi.pRasterizationState = &rs;
    pi.pMultisampleState = &ms;
    pi.pDepthStencilState = &ds;
    pi.pColorBlendState = &cb;
    pi.pDynamicState = &dy;
    pi.layout = layout_;

    VkPipeline pipe = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(dev_, cache_, 1, &pi, nullptr, &pipe) != VK_SUCCESS) {
        LOGE("vulkan: graphics pipeline failed on %s (vendor %04X, driver %08X); skipping draw",
             props_.deviceName, props_.vendorID, props_.driverVersion);
        pipe = VK_NULL_HANDLE;
    }
    pipelines_[h] = pipe;
    return pipe;
}

VkSampler VulkanBackend::Sampler(const VkSamplerCreateInfo& ci) {
    uint64_t key = Hash(&ci.magFilter, sizeof(VkSamplerCreateInfo) - offsetof(VkSamplerCreateInfo, magFilter));
    if (auto it = samplers_.find(key); it != samplers_.end()) return it->second;
    VkSampler s = VK_NULL_HANDLE;
    if (vkCreateSampler(dev_, &ci, nullptr, &s) != VK_SUCCESS) s = point_;
    samplers_[key] = s;
    return s;
}

VkBorderColor Border(uint32_t argb) {
    if ((argb >> 24) < 128) return VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    uint32_t sum = ((argb >> 16) & 255) + ((argb >> 8) & 255) + (argb & 255);
    return sum >= 384 ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE : VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
}

VkSampler VulkanBackend::StageSampler(const State& s, int i, const DrawTextures& stages, bool clamp) {
    VkSamplerCreateInfo sd{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    const auto& t = s.tss[size_t(i)];
    uint32_t mag = t[kTssMagFilter], min = t[kTssMinFilter], mip = t[kTssMipFilter];
    auto lin = [](uint32_t f) { return f >= kTexfLinear; };
    bool filterable = stages.fmt[i] != VK_FORMAT_R32_SFLOAT || r32f_linear_;
    sd.minFilter = (lin(min) && filterable) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    sd.magFilter = (lin(mag) && filterable) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    if (stages.replaced[i]) mip = kTexfLinear;
    sd.mipmapMode = (mip == kTexfLinear) ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    bool anisotropic = (min == kTexfAnisotropic || mag == kTexfAnisotropic);
    uint32_t an = anisotropic ? std::max<uint32_t>(t[kTssMaxAnisotropy], 1) : 1;
    uint32_t aniso = app::Get().aniso;
    if (aniso && lin(min) && !stages.info[i].shadow) an = aniso;
    if (an > 1 && feats_.samplerAnisotropy) {
        sd.anisotropyEnable = VK_TRUE;
        sd.maxAnisotropy = std::min(float(an), props_.limits.maxSamplerAnisotropy);
    }
    uint32_t u = t[kTssAddressU], v = t[kTssAddressV];
    sd.addressModeU = Address(clamp && u == kTaddressWrap ? kTaddressClamp : u);
    sd.addressModeV = Address(clamp && v == kTaddressWrap ? kTaddressClamp : v);
    sd.addressModeW = Address(t[kTssAddressW]);
    float limit = props_.limits.maxSamplerLodBias;
    sd.mipLodBias = std::clamp(Bits(t[kTssMipLodBias]), -limit, limit);
    sd.minLod = stages.replaced[i] ? 0.0f : std::min(float(t[kTssMaxMipLevel]), float(std::max(stages.levels[i], 1u) - 1));
    sd.maxLod = mip ? VK_LOD_CLAMP_NONE : sd.minLod;
    sd.borderColor = Border(t[kTssBorderColor]);
    if (stages.info[i].shadow) {
        // Reversed depth: both buffer and reference are inverted, so comparison op stays the console's own.
        bool linear = (lin(min) || lin(mag)) && depth_linear_;
        sd.minFilter = sd.magFilter = linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        sd.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sd.compareEnable = VK_TRUE;
        sd.compareOp = Cmp(s.rs[kRsShadowFunc]);
        sd.anisotropyEnable = VK_FALSE;
        sd.maxAnisotropy = 1.0f;
    }
    return Sampler(sd);
}

// ---- Targets

VkTarget* VulkanBackend::FindTarget(const Surface& s, bool depth) {
    if (!s.header) return nullptr;
    uint64_t alt = 0;
    if (ext_ && !ext_->TargetKey(s, depth, alt)) return nullptr;
    uint32_t data = mem::VirtualFromPhysical(s.data), gw = 0, gh = 0;
    SurfaceSize(s, gw, gh);
    auto it = targets_.find(TargetMapKey(data, depth, alt));
    if (it == targets_.end() || it->second.gw != gw || it->second.gh != gh) return nullptr;
    return &it->second;
}

void VulkanBackend::ReleaseTarget(std::unordered_map<uint64_t, VkTarget>::iterator it) {
    if (draw_rt_ == &it->second) draw_rt_ = nullptr;
    if (draw_ds_ == &it->second) draw_ds_ = nullptr;
    Retire(it->second.img);
    Retire(it->second.fb);
    targets_.erase(it);
}

VkTarget* VulkanBackend::EnsureTarget(const Surface& s, bool depth) {
    if (!s.header) return nullptr;
    uint64_t alt = 0;
    if (ext_ && !ext_->TargetKey(s, depth, alt)) return nullptr;
    uint32_t data = mem::VirtualFromPhysical(s.data), gw = 0, gh = 0;
    SurfaceSize(s, gw, gh);
    uint64_t key = TargetMapKey(data, depth, alt);
    auto it = targets_.find(key);
    if (it != targets_.end() && it->second.gw == gw && it->second.gh == gh) return &it->second;
    if (it != targets_.end()) ReleaseTarget(it);

    float sc = scale_;
    DXGI_FORMAT ext_fmt = DXGI_FORMAT_B8G8R8A8_UNORM;
    if (ext_) ext_->MakeTarget(s, depth, gw, gh, sc, ext_fmt);
    VkTarget t;
    t.depth = depth;
    t.gw = gw;
    t.gh = gh;
    t.w = std::max(1u, uint32_t(std::lround(gw * sc * aspect_)));
    t.h = std::max(1u, uint32_t(std::lround(gh * sc)));

    VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                              VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                              (depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    if (!CreateImage(t.img, depth ? depth_fmt_ : kBgra, t.w, t.h, 1, 1, 1, usage, VK_IMAGE_VIEW_TYPE_2D)) {
        LOGE("vulkan: failed to create %ux%u target", t.w, t.h);
        return nullptr;
    }

    Transition(t.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    if (depth) {
        // Reversed-Z far plane is 0.0.
        VkClearDepthStencilValue far_val{0.0f, 0};
        VkImageSubresourceRange r{
            VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT | (t.img.stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0)),
            0, 1, 0, 1};
        vkCmdClearDepthStencilImage(Cb(), t.img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &far_val, 1, &r);
    } else {
        VkClearColorValue black{};
        VkImageSubresourceRange r{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(Cb(), t.img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &r);
    }

    t.data = data;
    t.bytes = depth ? gw * gh * 4 : GuestPitch(gw) * gh;
    t.epoch = DirtyEpoch(data);
    t.sig = MemSig(data, t.bytes);
    t.SyncExt();
    VkTarget& ref = targets_[key];
    ref = std::move(t);
    return &ref;
}

bool VulkanBackend::PixelImage(Image& im, const std::vector<uint8_t>& px, uint32_t pitch, uint32_t w, uint32_t h) {
    if (!CreateImage(im, kBgra, w, h, 1, 1, 1, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                     VK_IMAGE_VIEW_TYPE_2D))
        return false;
    Alloc a = Upload(VkDeviceSize(w) * h * 4, 16);
    if (!a.cpu) {
        Destroy(im);
        return false;
    }
    for (uint32_t y = 0; y < h; y++) std::memcpy(a.cpu + size_t(y) * w * 4, &px[size_t(y) * pitch], w * 4);
    Transition(im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy c{a.off, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {w, h, 1}};
    vkCmdCopyBufferToImage(Cb(), a.buf, im.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    Transition(im, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return true;
}

bool VulkanBackend::UploadPicture(Image& im, const Picture& pic) {
    if (!pic.width || !pic.height || pic.levels.empty()) return false;
    uint32_t levels = uint32_t(pic.levels.size());
    if (!CreateImage(im, kBgra, pic.width, pic.height, 1, 1, levels,
                     VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_VIEW_TYPE_2D))
        return false;
    Transition(im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    for (uint32_t l = 0; l < levels; l++) {
        uint32_t mw = std::max(pic.width >> l, 1u), mh = std::max(pic.height >> l, 1u);
        size_t bytes = size_t(mw) * mh * 4;
        if (pic.levels[l].size() < bytes) continue;
        Alloc a = Upload(bytes, 16);
        if (!a.cpu) continue;
        std::memcpy(a.cpu, pic.levels[l].data(), bytes);
        VkBufferImageCopy c{a.off, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, l, 0, 1}, {0, 0, 0}, {mw, mh, 1}};
        vkCmdCopyBufferToImage(Cb(), a.buf, im.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    }
    Transition(im, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return true;
}

void VulkanBackend::UploadTarget(VkTarget& t, const Surface& s) {
    TextureDesc d;
    if (!DescribeSurface(s, d) && !Describe(s.header, d)) return;
    if (Format(d.format).depth) return;
    std::vector<uint8_t> px;
    uint32_t pitch = 0;
    bool over = !t.drawn && d.width == t.gw && d.height == t.gh && t.read.size() == size_t(t.gw) * t.gh * 4;
    if (over) {
        pitch = t.gw * 4;
        px = LatchCpuChanges(t);
    } else {
        DecodeLevel(d, 0, 0, static_cast<const uint8_t*>(kernel::HostPtr(d.data)), nullptr, px, pitch);
    }
    Image tex;
    if (!PixelImage(tex, px, pitch, d.width, d.height)) return;
    float uv[4] = {0, 0, 1, 1};
    Blit(t.img, {{0, 0}, {t.w, t.h}}, tex, uv, {.alpha = true, .over = over});
    Retire(tex);
    t.epoch = DirtyEpoch(t.data);
    t.sig = MemSig(t.data, t.bytes);
    t.cpu = true;
    t.SyncExt();
}

void VulkanBackend::Blit(Image& dst, const VkRect2D& to, Image& src, const float uvrect[4], const BlitOptions& o) {
    float c[8] = {uvrect[0], uvrect[1], uvrect[2], uvrect[3],
                  o.gamma ? 1.0f : 0.0f, o.alpha ? 1.0f : 0.0f, float(o.taps), o.mul};
    Alloc cb = Constants(c, sizeof(c));
    if (!cb.cpu) return;
    VkPipeline pipe = FullScreenPipeline(kBlitHlsl, "psmain", dst.fmt, VK_FORMAT_UNDEFINED, o.over);
    if (!pipe) return;
    Transition(src, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    Transition(lut_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    const VkImageView views[vkshader::kTexBindings] = {src.view, lut_.view, VK_NULL_HANDLE, VK_NULL_HANDLE,
                                                       VK_NULL_HANDLE, VK_NULL_HANDLE};
    const VkSampler samplers[4] = {linear_, point_, point_, point_};
    VkDescriptorSet set = Set(views, samplers);
    if (!set) return;
    BeginRender(&dst, nullptr);
    vkCmdBindPipeline(Cb(), VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    BindSet(set, cb.off, cb.off);
    SetViewport(float(to.offset.x), float(to.offset.y), float(to.extent.width), float(to.extent.height));
    SetScissor(0, 0, int32_t(dst.w), int32_t(dst.h));
    vkCmdDraw(Cb(), 3, 1, 0, 0);
}

Image* VulkanBackend::Feedback(VkTarget& t) {
    if (!t.fb && !CreateImage(t.fb, t.img.fmt, t.w, t.h, 1, 1, 1,
                              VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_VIEW_TYPE_2D))
        return nullptr;
    Transition(t.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    Transition(t.fb, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageAspectFlags aspect = t.depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    VkImageCopy c{{aspect, 0, 0, 1}, {0, 0, 0}, {aspect, 0, 0, 1}, {0, 0, 0}, {t.w, t.h, 1}};
    vkCmdCopyImage(Cb(), t.img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t.fb.image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    Transition(t.fb, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return &t.fb;
}

bool VulkanBackend::DepthCopy(const State& s) {
    TextureDesc d;
    if (!s.rt.header || !s.textures[0] || !Describe(s.textures[0], d) || Format(d.format).depth || d.cube || d.depth != 1)
        return false;
    auto di = targets_.find(TargetMapKey(mem::VirtualFromPhysical(s.rt.data), true));
    auto si = targets_.find(TargetMapKey(d.data, true));
    if (di == targets_.end() || si == targets_.end() || di == si) return false;
    VkTarget& dst = di->second;
    VkTarget& src = si->second;
    Alloc cb = Constants(nullptr, 0);
    if (!cb.cpu) return false;
    VkPipeline pipe = FullScreenPipeline(kDepthHlsl, "psmain", VK_FORMAT_UNDEFINED, depth_fmt_, false);
    if (!pipe) return false;
    Transition(src.img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    const VkImageView views[vkshader::kTexBindings] = {src.img.view, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE,
                                                       VK_NULL_HANDLE, VK_NULL_HANDLE};
    const VkSampler samplers[4] = {point_, point_, point_, point_};
    VkDescriptorSet set = Set(views, samplers);
    if (!set) return false;
    BeginRender(nullptr, &dst.img);
    vkCmdBindPipeline(Cb(), VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    BindSet(set, cb.off, cb.off);
    SetViewport(0, 0, float(dst.w), float(dst.h));
    SetScissor(0, 0, int32_t(dst.w), int32_t(dst.h));
    vkCmdDraw(Cb(), 3, 1, 0, 0);
    dst.drawn = true;
    dst.cpu = false;
    dst.SyncExt();
    return true;
}

bool VulkanBackend::Shrink(Image& src, uint32_t w, uint32_t h, Image& down, uint32_t taps) {
    if (!down || down.w != w || down.h != h) {
        Retire(down);
        if (!CreateImage(down, kBgra, w, h, 1, 1, 1,
                         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_VIEW_TYPE_2D))
            return false;
    }
    float uv[4] = {0, 0, 1, 1};
    Blit(down, {{0, 0}, {w, h}}, src, uv, {.alpha = true, .taps = taps});
    Transition(down, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    return true;
}

Image* VulkanBackend::CopySource(VkTarget& t, Image& down) {
    if (t.depth) return nullptr;
    if (t.w != t.gw || t.h != t.gh) {
        uint32_t taps = uint32_t(std::ceil(std::max(float(t.w) / t.gw, float(t.h) / t.gh) - 1e-3f));
        return Shrink(t.img, t.gw, t.gh, down, std::clamp(taps, 1u, 4u)) ? &down : nullptr;
    }
    Transition(t.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    return &t.img;
}

bool VulkanBackend::ReadBack(Image& im, uint32_t w, uint32_t h, std::vector<uint8_t>& out) {
    Buffer rb;
    if (!CreateBuffer(rb, VkDeviceSize(w) * h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true)) return false;
    EndRender();
    VkBufferImageCopy c{0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {w, h, 1}};
    vkCmdCopyImageToBuffer(Cb(), im.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb.buf, 1, &c);
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(Cb(), VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, nullptr, 0,
                         nullptr);
    Submit(true, false);
    out.assign(rb.mem.map, rb.mem.map + size_t(w) * h * 4);
    Destroy(rb);
    return true;
}

// ---- Draws, textures, occlusion queries, clears, copy

float VulkanBackend::TargetScale() const {
    const VkTarget* t = draw_rt_ ? draw_rt_ : draw_ds_;
    return (t && t->gh) ? float(t->h) / float(t->gh) : 1.0f;
}

float VulkanBackend::RenderTargetShift(const State& s, const DrawShaders& sh) const {
    bool offset = !sh.prog || ReadsViewportOffset(s.vs->program);
    return offset ? -0.5f * TargetScale() : 0.0f;
}

void VulkanBackend::EnsureVertexShader(const State& s, const Assembled& a, DrawShaders& sh) {
    if (auto it = shader_src_.find(sh.vkey); it != shader_src_.end()) {
        sh.vh = it->second;
        sh.vs = modules_[sh.vh];
        return;
    }
    std::string src = ReversedDepthHlsl(VertexHlsl(s, a, sh));
    if (sh.quad) src = QuadHlsl(std::move(src));
    sh.vs = Module(src, "main", vkshader::Stage::Vertex, sh.vh);
    shader_src_[sh.vkey] = sh.vh;
}

void VulkanBackend::EnsurePixelShader(const PixelKey& pk, DrawShaders& sh) {
    if (auto it = shader_src_.find(sh.pkey ^ kPixelKeySalt); it != shader_src_.end()) {
        sh.ph = it->second;
        sh.ps = modules_[sh.ph];
        return;
    }
    sh.ps = Module(PixelHlsl(pk), "main", vkshader::Stage::Fragment, sh.ph);
    shader_src_[sh.pkey ^ kPixelKeySalt] = sh.ph;
}

VkImageView VulkanBackend::BindTargetTexture(const State& s, int stage, const TextureDesc& d, VkTarget& t, bool box,
                                             DrawTextures& stages) {
    TexInfo& info = stages.info[stage];
    if (&t == draw_ds_) return VK_NULL_HANDLE;
    bool feedback = (&t == draw_rt_);
    stages.rt[stage] = true;
    if (!t.depth && LatchCpuWrite(t)) {
        Surface su{};
        su.header = s.textures[size_t(stage)];
        UploadTarget(t, su);
    }
    info.kind = kTex2D;
    info.flags |= kTexTarget;
    if (draw_rt_ && draw_rt_->gw && draw_rt_->gh) {
        float rt_aspect = (float(draw_rt_->w) / draw_rt_->gw) / (float(draw_rt_->h) / draw_rt_->gh);
        info.aspect = uint16_t(std::lround(rt_aspect * 1000.0f));
    }
    VkTarget* rt = draw_rt_;
    float k = float(t.h) / (t.gh * stages.k[stage]);
    if (!t.depth && box && k > kUpscaledMin && rt && rt != &t && (t.gw > rt->gw || t.gh > rt->gh)) {
        stages.wide[stage] = k;
        info.flags |= kTexWide;
    }
    stages.fmt[stage] = t.img.fmt;
    t.SyncExt();
    if (ext_) ext_->Sampled(d, &t.ext_target, 0, info);
    if (feedback) {
        Image* fb = Feedback(t);
        return fb ? fb->view : VK_NULL_HANDLE;
    }
    if (t.depth) info.shadow = ShadowDepthKind(d.format);
    Transition(t.img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return t.img.view;
}

bool VulkanBackend::CreateTexture(Tex& tx, const TextureDesc& d, const FormatInfo& fi, uint64_t key) {
    Retire(tx.img);
    Retire(tx.mat_normal);
    Retire(tx.mat_orm);
    tx = Tex{};
    tx.d = d;
    bool bc = fi.compressed;
    tx.decode = bc && !bc_;
    switch (fi.host) {
        case Host::BC1: tx.fmt = VK_FORMAT_BC1_RGBA_UNORM_BLOCK; break;
        case Host::BC2: tx.fmt = VK_FORMAT_BC2_UNORM_BLOCK; break;
        case Host::BC3: tx.fmt = VK_FORMAT_BC3_UNORM_BLOCK; break;
        case Host::R32F: tx.fmt = VK_FORMAT_R32_SFLOAT; break;
        default: tx.fmt = kBgra; break;
    }
    if (tx.decode) tx.fmt = kBgra;
    uint32_t w = d.width, h = d.height;
    tx.levels = d.levels;
    if (bc && ((w & 3) || (h & 3))) {
        w = (w + 3) & ~3u;
        h = (h + 3) & ~3u;
        tx.levels = 1;
    }
    VkImageViewType vt = d.cube ? VK_IMAGE_VIEW_TYPE_CUBE : d.depth > 1 ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D;
    if (!CreateImage(tx.img, tx.fmt, w, h, d.cube ? 1 : d.depth, d.cube ? 6 : 1, tx.levels,
                     VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, vt, d.cube)) {
        textures_.erase(key);
        return false;
    }
    return true;
}

void VulkanBackend::UploadTexture(Tex& tx, const TextureDesc& d, const FormatInfo& fi, const uint8_t* p,
                                  const uint32_t* colours) {
    Transition(tx.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    uint32_t faces = d.cube ? 6 : 1;
    std::vector<uint8_t> px, decoded;
    bool bc = fi.compressed && !tx.decode;
    uint32_t block = fi.host == Host::BC1 ? 8 : 16;
    for (uint32_t f = 0; f < faces; f++) {
        for (uint32_t l = 0; l < tx.levels; l++) {
            uint32_t pitch = 0;
            DecodeLevel(d, l, f, p, colours, px, pitch);
            if (ext_) ext_->Decoded(d, fi, f, l, px, pitch, tx.flags);
            uint32_t mw = std::max(tx.img.w >> l, 1u);
            uint32_t mh = std::max(tx.img.h >> l, 1u);
            uint32_t md = std::max(tx.img.d >> l, 1u);
            const std::vector<uint8_t>* src = &px;
            if (tx.decode) {
                DecodeDxt(fi.host, px, pitch, mw, mh, md, decoded);
                pitch = mw * 4;
                src = &decoded;
            }
            uint32_t row = bc ? std::max(1u, (mw + 3) / 4) * block : mw * 4;
            uint32_t rows = bc ? std::max(1u, (mh + 3) / 4) : mh;
            size_t src_rows = (pitch && md) ? src->size() / pitch / md : 0;
            Alloc a = Upload(VkDeviceSize(row) * rows * md, 16);
            if (!a.cpu) continue;
            for (uint32_t z = 0; z < md; z++) {
                for (uint32_t y = 0; y < rows; y++) {
                    uint8_t* dst = a.cpu + (size_t(z) * rows + y) * row;
                    if (y < src_rows)
                        std::memcpy(dst, &(*src)[(size_t(z) * src_rows + y) * pitch], std::min(pitch, row));
                    else
                        std::memset(dst, 0, row);
                }
            }
            VkBufferImageCopy c{a.off, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, l, f, 1}, {0, 0, 0}, {mw, mh, md}};
            vkCmdCopyBufferToImage(Cb(), a.buf, tx.img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        }
    }
    Transition(tx.img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

void VulkanBackend::MakeReplacement(Tex& tx) {
    if (!ext_ || !tx.repl_id) return;
    std::shared_ptr<const Replacement> r = ext_->Replaced(tx.repl_id);
    if (!r) return;
    tx.repl_id = 0;
    Image rep;
    if (!UploadPicture(rep, *r)) return;
    Retire(tx.img);
    tx.img = std::move(rep);
    tx.fmt = kBgra;
    tx.levels = tx.img.levels;
    tx.replaced = true;
    Retire(tx.mat_normal);
    Retire(tx.mat_orm);
    tx.has_material = false;
    if (r->material) {
        Image norm, orm;
        if (UploadPicture(norm, r->material->normal) && UploadPicture(orm, r->material->orm)) {
            tx.mat_normal = std::move(norm);
            tx.mat_orm = std::move(orm);
            std::memcpy(tx.mat_params, r->material->params, sizeof(tx.mat_params));
            tx.has_material = true;
        } else {
            Retire(norm);
            Retire(orm);
        }
    }
}

VkImageView VulkanBackend::BindGuestTexture(const State& s, int stage, const TextureDesc& d, const FormatInfo& fi,
                                            DrawTextures& stages) {
    TexInfo& info = stages.info[stage];
    StagePalette palette;
    ReadStagePalette(s, stage, d, palette);
    const uint64_t key = (uint64_t(palette.va) << 32) | d.data;
    Tex& tx = textures_[key];
    bool same = SameTexture(tx, d);
    if (!same && !CreateTexture(tx, d, fi, key)) return VK_NULL_HANDLE;
    Tex& t = textures_[key];
    const uint64_t repl_epoch = uint64_t(ext_ ? ext_->ReplaceEpoch() : 0) * 0x9E3779B97F4A7C15ull;
    if (t.frame != presents_ + 1 || !same || t.palette != palette.hash) {
        t.frame = presents_ + 1;
        t.palette = palette.hash;
        uint64_t sig = MemSig(d.data, TotalSize(d)) ^ DirtyEpoch(d.data) ^ palette.hash ^ repl_epoch;
        if (sig != t.sig || !same) {
            t.sig = sig;
            t.replaced = false;
            t.has_material = false;
            Retire(t.mat_normal);
            Retire(t.mat_orm);
            const uint8_t* p = static_cast<const uint8_t*>(kernel::HostPtr(d.data));
            const uint32_t* colours = palette.va ? palette.colours : nullptr;
            t.repl_id = ext_ ? ext_->Replace(d, p, colours) : 0;
            if (t.repl_id) MakeReplacement(t);
            if (!t.replaced) UploadTexture(t, d, fi, p, colours);
        }
    }
    if (t.repl_id) MakeReplacement(t);
    Transition(t.img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (fi.depth) info.shadow = ShadowDepthKind(d.format);
    stages.fmt[stage] = t.fmt;
    stages.levels[stage] = t.levels;
    stages.replaced[stage] = t.replaced;
    if (t.has_material && stages.mat_stage < 0) {
        Transition(t.mat_normal, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        Transition(t.mat_orm, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        stages.mat_stage = stage;
        stages.view[4] = t.mat_normal.view;
        stages.view[5] = t.mat_orm.view;
        std::memcpy(stages.mat, t.mat_params, sizeof(stages.mat));
    }
    if (ext_) ext_->Sampled(d, nullptr, t.flags, info);
    return t.img.view;
}

VkImageView VulkanBackend::BindTexture(const State& s, int stage, DrawTextures& stages) {
    TexInfo& info = stages.info[stage];
    info = {};
    stages.rt[stage] = false;
    stages.replaced[stage] = false;
    stages.k[stage] = 1.0f;
    stages.wide[stage] = 0.0f;
    stages.fmt[stage] = kBgra;
    stages.levels[stage] = 1;
    TextureDesc d;
    if (!s.textures[size_t(stage)] || !Describe(s.textures[size_t(stage)], d)) return VK_NULL_HANDLE;
    const FormatInfo& fi = Format(d.format);
    info.kind = TextureKind(d);
    info.rect = d.linear;
    uint64_t alt = 0;
    bool box = true;
    if (ext_) ext_->Sample(d.data, fi.depth, stage, alt, stages.k[stage], box);
    auto tit = targets_.find(TargetMapKey(d.data, fi.depth, alt));
    if (tit != targets_.end() && !d.cube && d.depth == 1 && !fi.compressed)
        return BindTargetTexture(s, stage, d, tit->second, box, stages);
    return BindGuestTexture(s, stage, d, fi, stages);
}

bool VulkanBackend::BindStageTextures(const State& s, DrawTextures& stages) {
    for (int i = 0; i < 4; i++) stages.view[i] = BindTexture(s, i, stages);
    for (int i = 0; i < 4; i++) {
        if (stages.view[i]) continue;
        uint8_t kind = stages.info[i].kind;
        stages.view[i] = dummy_[kind == kTex3D ? 1 : kind == kTexCube ? 2 : 0].view;
        if (stages.info[i].shadow) stages.info[i].shadow = 0;
    }
    if (!stages.view[4]) stages.view[4] = dummy_[0].view;
    if (!stages.view[5]) stages.view[5] = dummy_[0].view;
    return true;
}

void VulkanBackend::KeyDepthStencil(const State& s, const Assembled& a, PsoKey& k) {
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
    // Reversed depth negates polygon slope bias; constant depth offset is applied in ReversedDepthHlsl (zparams.z).
    if (OffsetsPolygons(s, a.topology)) k.zslope = -HostSlopeScale(s, draw_ds_);
}

void VulkanBackend::BuildPipelineKey(const State& s, const Assembled& a, const DrawShaders& sh, PsoKey& k) {
    std::memset(&k, 0, sizeof(k));
    k.vs = sh.vh;
    k.ps = sh.ph;
    k.regs = a.regs;
    for (int r = 0; r < 16; r++) {
        if ((a.regs >> r) & 1) {
            const VertexElement& e = a.elements[size_t(r)];
            k.elements[r] = e.format | (uint32_t(e.buffer) << 8) | (uint32_t(e.offset) << 16);
        }
    }
    k.buffers = a.buffer_count;
    for (uint32_t i = 0; i < a.buffer_count; i++) {
        const VertexBuffer& b = a.buffers[i];
        k.strides[i] = (b.tail && b.stride && (b.bytes / b.stride) > 0) ? (b.stride + b.tail) : b.stride;
    }
    k.topo = uint32_t(a.topology) | (a.strip ? 0x10 : 0);
    k.has_rt = draw_rt_ != nullptr;
    k.has_ds = draw_ds_ != nullptr;
    k.rtfmt = draw_rt_ ? uint32_t(draw_rt_->img.fmt) : 0;
    KeyBlend(s, k);
    if (k.has_ds) KeyDepthStencil(s, a, k);
    KeyRasterizer(s, a, k);
}

bool VulkanBackend::EnsureQueries() {
    if (query_pool_) return true;
    if (query_failed_) return false;
    VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    qi.queryType = VK_QUERY_TYPE_OCCLUSION;
    qi.queryCount = kQueries;
    if (vkCreateQueryPool(dev_, &qi, nullptr, &query_pool_) != VK_SUCCESS) {
        query_failed_ = true;
        return false;
    }
    return true;
}

uint32_t VulkanBackend::BeginCountedDraw(const State& s) {
    if (!s.simple[kMCountEnable >> 2] || !EnsureQueries()) return kNoQuery;
    uint32_t query = query_next_++ % kQueries;
    EndRender();
    vkCmdResetQueryPool(Cb(), query_pool_, query, 1);
    return query;
}

void VulkanBackend::EndCountedDraw(uint32_t query) {
    if (query == kNoQuery) return;
    vkCmdEndQuery(Cb(), query_pool_, query);
    counted_.push_back({query, SamplesPerPixel()});
}

float VulkanBackend::SamplesPerPixel() const {
    const VkTarget* t = draw_rt_ ? draw_rt_ : draw_ds_;
    if (!t || !t->gw || !t->gh) return 1.0f;
    return float(t->w) * float(t->h) / (float(t->gw) * float(t->gh));
}

void VulkanBackend::DeliverReports() {
    if (reports_.empty() || !query_pool_) return;
    constexpr uint32_t kTime = 0, kTimeHigh = 4, kCount = 8, kStatus = 12;
    uint64_t done = Completed();
    while (!reports_.empty() && reports_.front().fence <= done) {
        const Report& r = reports_.front();
        double pixels = 0.0;
        for (const Query& q : r.queries) {
            uint64_t count = 0;
            if (vkGetQueryPoolResults(dev_, query_pool_, q.index, 1, sizeof(count), &count, sizeof(count),
                                      VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
                pixels += double(count) / double(std::max(q.samples_per_pixel, 1e-4f));
            }
        }
        kernel::W32(r.va + kTime, uint32_t(presents_));
        kernel::W32(r.va + kTimeHigh, 0);
        kernel::W32(r.va + kCount, uint32_t(std::lround(pixels)));
        std::atomic_thread_fence(std::memory_order_release);
        kernel::W32(r.va + kStatus, 0);
        reports_.pop_front();
    }
}

void VulkanBackend::Draw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) {
    if (SpriteOnStage3(s, prim)) {
        Draw(SpriteStageZero(s), prim, start, count, indices);
        return;
    }
    if (DepthCopy(s)) return;
    DrawEnd draw_end{ext_};
    if (ext_) ext_->BeginDraw(s);
    Assembled& a = asm_;
    if (!Assemble(s, prim, start, count, indices, a)) return;
    if (ext_) ext_->Vertices(s, a);

    if (CurrentFrame().used > kUploadSize - kUploadReserve) Submit(true, false);

    VkTarget* rt = EnsureTarget(s.rt, false);
    VkTarget* ds = (s.rs[kRsZEnable] || s.rs[kRsStencilEnable]) ? EnsureTarget(s.zs, true) : nullptr;
    if (rt && ds && (rt->w != ds->w || rt->h != ds->h)) ds = nullptr;
    if (!rt && !ds) return;
    if (rt && LatchCpuWrite(*rt)) UploadTarget(*rt, s.rt);
    if (rt) rt->SyncExt();
    if (ds) ds->SyncExt();
    draw_rt_ = rt;
    draw_ds_ = ds;
    if (ext_ && ext_->Bound(*this, s, rt ? &rt->ext_target : nullptr, ds ? &ds->ext_target : nullptr, a.pretransformed)) {
        rt = EnsureTarget(s.rt, false);
        ds = (s.rs[kRsZEnable] || s.rs[kRsStencilEnable]) ? EnsureTarget(s.zs, true) : nullptr;
        if (rt && ds && (rt->w != ds->w || rt->h != ds->h)) ds = nullptr;
        if (!rt && !ds) return;
        draw_rt_ = rt;
        draw_ds_ = ds;
    }

    DrawShaders sh = ClassifyShaders(s, a, count);
    if (sh.prog) EnsureProgramHash(s);
    sh.vkey = VertexKey(s, a, sh);
    EnsureVertexShader(s, a, sh);

    DrawTextures stages;
    if (!BindStageTextures(s, stages) || (!draw_rt_ && !draw_ds_)) return;
    PixelKey pk = MakePixelKey(s, stages.info);
    pk.quad = sh.quad ? 1 : 0;
    draw_texbox_ = ext_ && TexcoordsAsGiven(s, a) && ext_->TexelBox(draw_uvbox_);
    pk.texbox = draw_texbox_ ? 1 : 0;
    if (stages.mat_stage >= 0 && MaterialFits(pk, stages.mat_stage)) {
        pk.material = uint32_t(stages.mat_stage) + 1;
    } else if (stages.mat_stage >= 0) {
        stages.mat_stage = -1;
        stages.view[4] = stages.view[5] = dummy_[0].view;
    }
    sh.pkey = Hash(&pk, sizeof(pk));
    EnsurePixelShader(pk, sh);
    if (!sh.vs || !sh.ps) return;

    PsoKey k;
    BuildPipelineKey(s, a, sh, k);
    VkPipeline pipe = EnsurePipeline(k, sh.vs, sh.ps);
    if (!pipe) return;

    // Pre-flight ring space so all allocations for this draw live in the same Frame.
    VkDeviceSize need = 2 * kUboRange + 1024;
    for (uint32_t i = 0; i < a.buffer_count; i++) {
        const VertexBuffer& b = a.buffers[i];
        uint32_t n = b.stride ? b.bytes / b.stride : 0;
        VkDeviceSize vbytes = (b.tail && n) ? VkDeviceSize(n) * (b.stride + b.tail) : (b.bytes + b.tail);
        need += std::max<VkDeviceSize>(vbytes, 16) + 16;
    }
    need += VkDeviceSize(a.index_count) * 4 + 16;
    if (need < kUploadSize / 4 && CurrentFrame().used + need > kUploadSize) Submit(true, false);

    GuestExtent size;
    SurfaceSize(draw_rt_ ? s.rt : s.zs, size.w, size.h);
    size = MultisampleGrid(s, draw_rt_ ? s.rt : s.zs, size);

    DrawConstants c{};
    FillSurfaceConstants(s, size, c);
    if (a.topology == Topology::Points) c.surf[3] = TargetScale();
    FillFogConstants(s, sh.prog, c);
    FillCombinerConstants(s, c);

    std::memcpy(vsc_edit_.data(), s.vsc.data(), sizeof(s.vsc));
    float(*vsc)[4] = reinterpret_cast<float(*)[4]>(vsc_edit_.data());
    if (ext_) ext_->Constants(vsc, c.ext, draw_rt_ ? &draw_rt_->ext_target : nullptr, k.blend ? k.src : 1);

    FillDepthConstants(s, sh, a.topology, c);
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
    if (draw_texbox_) std::memcpy(c.uvbox, draw_uvbox_, sizeof(c.uvbox));
    if (stages.mat_stage >= 0) {
        std::memcpy(c.mtl, stages.mat, sizeof(stages.mat));
        uint32_t depth = g_surface_depth.load(std::memory_order_relaxed);
        c.mtl[1][3] = depth == 0 ? 0.0f : depth == 1 ? kParallaxLow : kParallaxHigh;
        c.mtl[0][0] = depth >= 2 ? kParallaxLayersHigh : kParallaxLayersLow;
        float w = float(draw_rt_ ? draw_rt_->w : 1), h = float(draw_rt_ ? draw_rt_->h : 1);
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

    Alloc vc = Constants(vsc, sizeof(s.vsc));
    Alloc dc = Constants(&c, sizeof(c));
    if (!vc.cpu || !dc.cpu) return;

    VkBuffer vbufs[16];
    VkDeviceSize voffs[16];
    for (uint32_t i = 0; i < a.buffer_count; i++) {
        const VertexBuffer& b = a.buffers[i];
        uint32_t n = b.stride ? b.bytes / b.stride : 0;
        if (b.tail && n) {
            uint32_t wide = b.stride + b.tail;
            Alloc vb = Upload(VkDeviceSize(n) * wide, 16);
            if (!vb.cpu) return;
            for (uint32_t v = 0; v < n; v++) {
                uint8_t* dst = vb.cpu + size_t(v) * wide;
                size_t at = size_t(v) * b.stride;
                size_t have = std::min<size_t>(wide, b.bytes - at);
                if (b.src && have) std::memcpy(dst, b.src + at, have);
                if (have < wide) std::memset(dst + have, 0, wide - have);
            }
            vbufs[i] = vb.buf;
            voffs[i] = vb.off;
        } else {
            VkDeviceSize total = std::max<VkDeviceSize>(VkDeviceSize(b.bytes) + b.tail, 16);
            Alloc vb = Upload(total, 16);
            if (!vb.cpu) return;
            if (b.src && b.bytes) std::memcpy(vb.cpu, b.src, b.bytes);
            if (total > b.bytes) std::memset(vb.cpu + b.bytes, 0, size_t(total - b.bytes));
            vbufs[i] = vb.buf;
            voffs[i] = vb.off;
        }
    }

    Alloc ib{};
    if (a.index_count) {
        uint32_t ibytes = a.index_count * (a.index16 ? 2 : 4);
        ib = Upload(ibytes, 4);
        if (!ib.cpu) return;
        std::memcpy(ib.cpu, a.index_src, ibytes);
    }

    bool clamp = AnyStageWraps(s) && ((ext_ && ext_->Clamp()) || (UnitTexcoords(a) && TexcoordsAsGiven(s, a)));
    VkSampler samplers[4];
    for (int i = 0; i < 4; i++) samplers[i] = StageSampler(s, i, stages, clamp);
    VkDescriptorSet set = Set(stages.view, samplers);
    if (!set) return;

    uint32_t query = BeginCountedDraw(s);
    BeginRender(draw_rt_ ? &draw_rt_->img : nullptr, draw_ds_ ? &draw_ds_->img : nullptr);
    if (draw_rt_) {
        draw_rt_->drawn = true;
        draw_rt_->cpu = false;
        draw_rt_->SyncExt();
    }
    if (draw_ds_) {
        draw_ds_->drawn = true;
        draw_ds_->cpu = false;
        draw_ds_->SyncExt();
    }

    vkCmdBindPipeline(Cb(), VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    BindSet(set, vc.off, dc.off);
    VkTarget& bt = draw_rt_ ? *draw_rt_ : *draw_ds_;
    float W = float(bt.w), H = float(bt.h);
    SetViewport(0, 0, W, H);
    float sx = W / size.w, sy = H / size.h;
    int32_t x0 = int32_t(s.viewport.x * sx), y0 = int32_t(s.viewport.y * sy);
    int32_t x1 = int32_t((s.viewport.x + s.viewport.w) * sx), y1 = int32_t((s.viewport.y + s.viewport.h) * sy);
    if (s.scissor_count) {
        x0 = std::max(x0, int32_t(s.scissors[0] * sx));
        y0 = std::max(y0, int32_t(s.scissors[1] * sy));
        x1 = std::min(x1, int32_t(s.scissors[2] * sx));
        y1 = std::min(y1, int32_t(s.scissors[3] * sy));
    }
    SetScissor(std::clamp(x0, 0, int32_t(W)), std::clamp(y0, 0, int32_t(H)),
               std::clamp(x1, 0, int32_t(W)), std::clamp(y1, 0, int32_t(H)));

    float bf[4];
    Color(SimpleState(s, kMBlendColor), bf);
    vkCmdSetBlendConstants(Cb(), bf);
    vkCmdSetStencilReference(Cb(), VK_STENCIL_FACE_FRONT_AND_BACK, SimpleState(s, kMStencilRef) & 0xFF);
    if (a.buffer_count) vkCmdBindVertexBuffers(Cb(), 0, a.buffer_count, vbufs, voffs);
    if (query != kNoQuery) vkCmdBeginQuery(Cb(), query_pool_, query, 0);
    if (a.index_count) {
        vkCmdBindIndexBuffer(Cb(), ib.buf, ib.off, a.index16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(Cb(), a.index_count, 1, 0, a.base_vertex, 0);
    } else {
        vkCmdDraw(Cb(), a.draw_count, 1, 0, 0);
    }
    EndCountedDraw(query);
}

VkRect2D ScaledRect(const VkTarget& t, int x0, int y0, int x1, int y1) {
    float sx = float(t.w) / t.gw, sy = float(t.h) / t.gh;
    int32_t l = std::clamp(int32_t(std::lround(x0 * sx)), 0, int32_t(t.w));
    int32_t tp = std::clamp(int32_t(std::lround(y0 * sy)), 0, int32_t(t.h));
    int32_t r = std::clamp(int32_t(std::lround(x1 * sx)), 0, int32_t(t.w));
    int32_t b = std::clamp(int32_t(std::lround(y1 * sy)), 0, int32_t(t.h));
    return {{l, tp}, {uint32_t(std::max(r - l, 0)), uint32_t(std::max(b - tp, 0))}};
}

void VulkanBackend::ClearTarget(VkTarget& t, const State& s, uint32_t count, const uint8_t* rects, uint32_t flags,
                                uint32_t color, float z, uint32_t stencil) {
    std::vector<VkClearRect> rs;
    auto add = [&](const VkRect2D& r) {
        if (r.extent.width && r.extent.height) rs.push_back({r, 0, 1});
    };
    if (count && rects) {
        for (uint32_t i = 0; i < count; i++) {
            int32_t r[4];
            std::memcpy(r, rects + size_t(i) * sizeof(r), sizeof(r));
            add(ScaledRect(t, r[0], r[1], r[2], r[3]));
        }
    } else {
        const auto& v = s.viewport;
        add(ScaledRect(t, int(v.x), int(v.y), int(v.x + v.w), int(v.y + v.h)));
    }
    t.drawn = true;
    t.cpu = false;
    t.SyncExt();
    if (rs.empty()) return;

    VkClearAttachment ca{};
    if (t.depth) {
        ca.aspectMask = ((flags & kClearZ) ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) |
                        (((flags & kClearStencil) && t.img.stencil) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
        if (!ca.aspectMask) return;
        // Reversed-Z: clear depth is 1 - z.
        ca.clearValue.depthStencil = {1.0f - std::clamp(z, 0.0f, 1.0f), stencil & 0xFF};
        BeginRender(nullptr, &t.img);
    } else {
        if (LatchCpuWrite(t)) UploadTarget(t, s.rt);
        ca.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        Color(color, ca.clearValue.color.float32);
        BeginRender(&t.img, nullptr);
    }
    vkCmdClearAttachments(Cb(), 1, &ca, uint32_t(rs.size()), rs.data());
}

void VulkanBackend::Clear(const State& s, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color,
                          float z, uint32_t stencil) {
    VkTarget* rt = (flags & kClearTarget) ? EnsureTarget(s.rt, false) : nullptr;
    VkTarget* ds = (flags & (kClearZ | kClearStencil)) ? EnsureTarget(s.zs, true) : nullptr;
    for (VkTarget* t : {rt, ds})
        if (t) ClearTarget(*t, s, count, rects, flags, color, z, stencil);
}

void VulkanBackend::Copy(const Surface& src, const Surface& dst, uint32_t count, uint32_t src_rects,
                         uint32_t dst_points) {
    VkTarget* src_t = FindTarget(src, false);
    if (!src_t || !src_t->drawn) {
        if (!CopySurfaceMemory(src, dst, count, src_rects, dst_points)) {
            LOG_FIRST(8, "vulkan: CopyRects %08X to %08X skipped", src.data, dst.data);
        } else if (VkTarget* dst_t = FindTarget(dst, false)) {
            UploadTarget(*dst_t, dst);
        }
        return;
    }
    VkTarget* dst_t = EnsureTarget(dst, false);
    src_t = FindTarget(src, false);
    if (!dst_t || !src_t || src_t == dst_t) return;

    float src_sx = float(src_t->w) / src_t->gw, src_sy = float(src_t->h) / src_t->gh;
    float dst_sx = float(dst_t->w) / dst_t->gw, dst_sy = float(dst_t->h) / dst_t->gh;
    bool same_scale = std::fabs(src_sx - dst_sx) <= 1e-3f && std::fabs(src_sy - dst_sy) <= 1e-3f;
    bool listed = count && src_rects;
    uint32_t n = listed ? count : 1;

    if (!same_scale) {
        for (uint32_t i = 0; i < n; i++) {
            GuestRect g{0, 0, int32_t(src_t->gw), int32_t(src_t->gh)};
            int32_t dx = 0, dy = 0;
            if (listed) {
                g = ReadGuestRect(src_rects, i);
                dx = g.left;
                dy = g.top;
                if (dst_points) ReadGuestPoint(dst_points, i, dx, dy);
            }
            int32_t rw = g.right - g.left, rh = g.bottom - g.top;
            if (rw <= 0 || rh <= 0) continue;
            float uv[4] = {float(g.left) / src_t->gw, float(g.top) / src_t->gh,
                           float(rw) / src_t->gw, float(rh) / src_t->gh};
            VkRect2D to = ScaledRect(*dst_t, dx, dy, dx + rw, dy + rh);
            if (to.extent.width && to.extent.height)
                Blit(dst_t->img, to, src_t->img, uv, {.alpha = true});
        }
        dst_t->drawn = true;
        dst_t->cpu = false;
        dst_t->SyncExt();
        return;
    }

    Transition(src_t->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    Transition(dst_t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    std::vector<VkImageCopy> regions;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t l = 0, tp = 0, r = std::min(src_t->w, dst_t->w), b = std::min(src_t->h, dst_t->h);
        uint32_t dx = 0, dy = 0;
        if (listed) {
            GuestRect g = ReadGuestRect(src_rects, i);
            l = uint32_t(std::clamp(int32_t(std::lround(g.left * dst_sx)), 0, int32_t(src_t->w)));
            tp = uint32_t(std::clamp(int32_t(std::lround(g.top * dst_sy)), 0, int32_t(src_t->h)));
            r = uint32_t(std::clamp(int32_t(std::lround(g.right * dst_sx)), 0, int32_t(src_t->w)));
            b = uint32_t(std::clamp(int32_t(std::lround(g.bottom * dst_sy)), 0, int32_t(src_t->h)));
            dx = l;
            dy = tp;
            if (dst_points) {
                int32_t px = 0, py = 0;
                ReadGuestPoint(dst_points, i, px, py);
                dx = uint32_t(std::clamp(int32_t(std::lround(px * dst_sx)), 0, int32_t(dst_t->w)));
                dy = uint32_t(std::clamp(int32_t(std::lround(py * dst_sy)), 0, int32_t(dst_t->h)));
            }
        }
        if (r <= l || b <= tp || dx >= dst_t->w || dy >= dst_t->h) continue;
        r = std::min(r, l + dst_t->w - dx);
        b = std::min(b, tp + dst_t->h - dy);
        regions.push_back({{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                           {int32_t(l), int32_t(tp), 0},
                           {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                           {int32_t(dx), int32_t(dy), 0},
                           {r - l, b - tp, 1}});
    }
    if (!regions.empty())
        vkCmdCopyImage(Cb(), src_t->img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst_t->img.image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, uint32_t(regions.size()), regions.data());
    dst_t->drawn = true;
    dst_t->cpu = false;
    dst_t->SyncExt();
}

// ---- Read-backs

void VulkanBackend::Sync(uint32_t data, uint32_t) {
    auto it = targets_.find(TargetMapKey(data, false));
    if (it == targets_.end() || !it->second.drawn) return;
    VkTarget& t = it->second;
    Image down;
    Image* src = CopySource(t, down);
    std::vector<uint8_t> px;
    bool ok = src && ReadBack(*src, t.gw, t.gh, px);
    Destroy(down);
    if (!ok) return;
    uint32_t pitch = GuestPitch(t.gw);
    uint8_t* guest = static_cast<uint8_t*>(kernel::HostPtr(data));
    for (uint32_t y = 0; y < t.gh; y++)
        std::memcpy(guest + size_t(y) * pitch, &px[size_t(y) * t.gw * 4], t.gw * 4);
    t.read = std::move(px);
    t.drawn = false;
    t.sig = MemSig(t.data, t.bytes);
    t.SyncExt();
}

void VulkanBackend::SyncLate(uint32_t data) {
    if (!DeliverLate(data)) Sync(data, 0);
    RecordLate(data);
}

bool VulkanBackend::DeliverLate(uint32_t data) {
    std::lock_guard lock(late_lock_);
    auto it = late_.find(data);
    if (it == late_.end()) return false;
    Late& l = it->second;
    uint64_t done = Completed(), now = presents_seen_.load();
    int best = -1;
    for (int i = 0; i < kFrames; i++) {
        bool finished = l.fence[i] && l.fence[i] <= done;
        bool recent = now - l.frame[i] <= uint64_t(kFrames) + 1;
        if (finished && recent && (best < 0 || l.fence[i] > l.fence[best])) best = i;
    }
    if (best < 0) return false;
    if (l.fence[best] > l.delivered) {
        const uint8_t* p = l.rb[best].mem.map;
        uint32_t pitch = GuestPitch(l.gw);
        uint8_t* dst = static_cast<uint8_t*>(kernel::HostPtr(data));
        for (uint32_t y = 0; y < l.gh; y++)
            std::memcpy(dst + size_t(y) * pitch, p + size_t(y) * l.gw * 4, l.gw * 4);
        l.delivered = l.fence[best];
    }
    return true;
}

void VulkanBackend::RecordLate(uint32_t data) {
    auto it = targets_.find(TargetMapKey(data, false));
    if (it == targets_.end()) return;
    VkTarget& t = it->second;
    t.sig = MemSig(t.data, t.bytes);
    t.read.clear();
    std::lock_guard lock(late_lock_);
    Late& l = late_[data];
    if (l.gw != t.gw || l.gh != t.gh) {
        for (Buffer& b : l.rb) Retire(b);
        Retire(l.down);
        l = Late{};
        l.gw = t.gw;
        l.gh = t.gh;
    }
    uint32_t slot = l.next;
    if (l.fence[slot] > Completed()) return;
    Image* src = CopySource(t, l.down);
    if (!src) return;
    if (!l.rb[slot].buf &&
        !CreateBuffer(l.rb[slot], VkDeviceSize(t.gw) * t.gh * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true))
        return;
    EndRender();
    VkBufferImageCopy c{0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {t.gw, t.gh, 1}};
    vkCmdCopyImageToBuffer(Cb(), src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, l.rb[slot].buf, 1, &c);
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(Cb(), VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, nullptr, 0,
                         nullptr);
    l.fence[slot] = value_ + 1;
    l.frame[slot] = presents_;
    l.next = (slot + 1) % kFrames;
}

// ---- Present, ImGui overlay, LRU texture eviction, pipeline cache

bool VulkanBackend::AcquireImage(uint32_t& index) {
    bool dirty = gpu::g_surface_dirty.exchange(false, std::memory_order_acq_rel);
    if (!surface_ || surface_lost_ || dirty) {
        if (surface_) LOGI("vulkan: recreating surface (%s)", surface_lost_ ? "lost" : "resume");
        DestroySwapchain();
        if (surface_) vkDestroySurfaceKHR(instance_, surface_, nullptr);
        surface_ = VK_NULL_HANDLE;
        if (!CreateSurface()) return false;
        swap_stale_ = true;
    }
    app::Mode wp = app::WindowPixels();
    if (!swap_ || swap_stale_ || (wp.width && wp.width != swap_extent_.width) ||
        (wp.height && wp.height != swap_extent_.height) || app::Get().vsync != vsync_) {
        if (!CreateSwapchain()) return false;
    }
    constexpr uint64_t kAcquireTimeout = 500'000'000;
    for (int tries = 0; tries < 2; tries++) {
        VkResult r = vkAcquireNextImageKHR(dev_, swap_, kAcquireTimeout, CurrentFrame().acquire, VK_NULL_HANDLE, &index);
        // Leave VK_SUBOPTIMAL_KHR alone: Android reports it whenever preTransform differs from display rotation,
        // and recreating the swapchain every frame destroys in-flight images on Mali drivers.
        if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) return true;
        if (r == VK_TIMEOUT || r == VK_NOT_READY) return false;
        if (r == VK_ERROR_SURFACE_LOST_KHR) {
            surface_lost_ = true;
            return false;
        }
        if (r != VK_ERROR_OUT_OF_DATE_KHR || !CreateSwapchain()) return false;
    }
    return false;
}

void VulkanBackend::DrawFrame(VkTarget* t, uint32_t index) {
    EndRender();
    VkCommandBuffer cb = Cb();
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = swap_images_[index];
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &b);

    float aspect = app::Widescreen() ? app::WideAspect() : (4.0f / 3.0f);
    float w = float(swap_extent_.width), h = float(swap_extent_.height);
    float vw = std::min(w, h * aspect), vh = vw / aspect;
    float c[8] = {0.0f, 0.0f, 1.0f, 1.0f, gamma_ ? 1.0f : 0.0f, 0.0f, 1.0f, 1.0f};
    Alloc cbuf = Constants(c, sizeof(c));
    VkPipeline pipe = (t && cbuf.cpu) ? FullScreenPipeline(kBlitHlsl, "psmain", swap_fmt_, VK_FORMAT_UNDEFINED, false)
                                      : VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (pipe) {
        Transition(t->img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        Transition(lut_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        const VkImageView views[vkshader::kTexBindings] = {t->img.view, lut_.view, VK_NULL_HANDLE, VK_NULL_HANDLE,
                                                           VK_NULL_HANDLE, VK_NULL_HANDLE};
        const VkSampler samplers[4] = {linear_, point_, point_, point_};
        set = Set(views, samplers);
    }

    VkRenderingAttachmentInfoKHR ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR};
    ca.imageView = swap_views_[index];
    ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    ca.clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    VkRenderingInfoKHR ri{VK_STRUCTURE_TYPE_RENDERING_INFO_KHR};
    ri.renderArea = {{0, 0}, swap_extent_};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &ca;

    if (dynamic_) {
        begin_rendering_(cb, &ri);
    } else {
        VkRenderPassBeginInfo bi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        bi.renderPass = Pass(swap_fmt_, VK_FORMAT_UNDEFINED, true);
        bi.framebuffer = Framebuffer(swap_fmt_, VK_FORMAT_UNDEFINED, swap_views_[index], VK_NULL_HANDLE,
                                     swap_extent_.width, swap_extent_.height);
        bi.renderArea = ri.renderArea;
        bi.clearValueCount = 1;
        bi.pClearValues = &ca.clearValue;
        vkCmdBeginRenderPass(cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
    }
    if (pipe && set) {
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        BindSet(set, cbuf.off, cbuf.off);
        SetViewport((w - vw) * 0.5f, (h - vh) * 0.5f, vw, vh);
        SetScissor(0, 0, int32_t(w), int32_t(h));
        vkCmdDraw(cb, 3, 1, 0, 0);
    }
    RenderUi();
    if (dynamic_) end_rendering_(cb);
    else vkCmdEndRenderPass(cb);

    b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    b.dstAccessMask = 0;
    b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &b);
}

void VulkanBackend::RenderUi() {
    if (!ui_) return;
    std::lock_guard lock(app::UiLock());
    ImGui_ImplVulkan_NewFrame();
    linuxapp::ImGuiNewFrame();
    ImGui::GetIO().DisplaySize = ImVec2(float(swap_extent_.width), float(swap_extent_.height));
    ImGui::NewFrame();
    if (app::kGame.ui) app::kGame.ui(fps_);
    ImGui::Render();
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), Cb());
}

void VulkanBackend::UploadGammaRamp() {
    ramp_dirty_ = false;
    Alloc a = Upload(256 * 4, 16);
    if (!a.cpu) return;
    bool identity = true;
    for (int i = 0; i < 256; i++) {
        uint8_t* p = a.cpu + i * 4;
        p[0] = ramp_[512 + i];
        p[1] = ramp_[256 + i];
        p[2] = ramp_[i];
        p[3] = 255;
        identity &= ramp_[i] == i && ramp_[256 + i] == i && ramp_[512 + i] == i;
    }
    gamma_ = !identity;
    Transition(lut_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy c{a.off, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {256, 1, 1}};
    vkCmdCopyBufferToImage(Cb(), a.buf, lut_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    Transition(lut_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

// Keep decoded textures bounded for 4 GB RAM devices (Mali/PowerVR expand DXT 4x-8x to BGRA8).
void VulkanBackend::ReleaseOldTextures() {
    constexpr uint64_t kSweepEvery = 60;
    constexpr size_t kTexturesKept = 1024;
    constexpr uint64_t kIdleFrames = 600;
    constexpr VkDeviceSize kTextureBytes = 256ull << 20;
    if (presents_ % kSweepEvery != 0) return;
    VkDeviceSize bytes = 0;
    for (const auto& [key, t] : textures_) bytes += t.Bytes();
    if (textures_.size() <= kTexturesKept && bytes <= kTextureBytes) return;
    std::vector<std::pair<uint64_t, uint64_t>> lru;
    for (auto it = textures_.begin(); it != textures_.end();) {
        if (it->second.frame + kIdleFrames < presents_) {
            bytes -= it->second.Bytes();
            Retire(it->second.img);
            Retire(it->second.mat_normal);
            Retire(it->second.mat_orm);
            it = textures_.erase(it);
        } else {
            lru.push_back({it->second.frame, it->first});
            ++it;
        }
    }
    if (bytes <= kTextureBytes && textures_.size() <= kTexturesKept) return;
    std::sort(lru.begin(), lru.end());
    for (const auto& [frame, key] : lru) {
        if ((bytes <= kTextureBytes && textures_.size() <= kTexturesKept) || frame + kSweepEvery >= presents_) break;
        auto it = textures_.find(key);
        if (it == textures_.end()) continue;
        bytes -= it->second.Bytes();
        Retire(it->second.img);
        Retire(it->second.mat_normal);
        Retire(it->second.mat_orm);
        textures_.erase(it);
    }
}

void VulkanBackend::ApplySettings() {
    texturedumper::Update();
    float want = app::RenderScale(guest_h_);
    float aspect = TargetAspect();
    bool remake = ext_ && ext_->Settings();
    if (!remake && std::fabs(want - scale_) <= 1e-4f && std::fabs(aspect - aspect_) <= 1e-4f) return;
    Submit(true, false);
    for (auto& [key, t] : targets_) {
        Retire(t.img);
        Retire(t.fb);
    }
    targets_.clear();
    draw_rt_ = draw_ds_ = nullptr;
    scale_ = want;
    aspect_ = aspect;
    if (ext_) ext_->Reset(*this);
    LOGR("vulkan: render scale %.3f, aspect %.3f (%ux%u target, wide %d)", scale_, aspect_,
         uint32_t(std::lround(guest_w_ * scale_ * aspect_)), uint32_t(std::lround(guest_h_ * scale_)),
         int(app::Widescreen()));
}

void VulkanBackend::SavePipelineCache() {
    if (!cache_) return;
    size_t size = 0;
    if (vkGetPipelineCacheData(dev_, cache_, &size, nullptr) != VK_SUCCESS || size == 0 || size == cache_saved_) return;
    std::vector<char> blob(size);
    if (vkGetPipelineCacheData(dev_, cache_, &size, blob.data()) != VK_SUCCESS) return;
    std::filesystem::path dir = app::ExeDir() / "shadercache";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::filesystem::path p = dir / "pipelines.bin";
    std::filesystem::path tmp = p;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary);
        if (!f) return;
        f.write(blob.data(), std::streamsize(size));
    }
    std::filesystem::rename(tmp, p, ec);
    cache_saved_ = size;
}

void VulkanBackend::Present(const Surface& front) {
    VkTarget* t = EnsureTarget(front, false);
    if (t && LatchCpuWrite(*t)) UploadTarget(*t, front);
    if (ramp_dirty_) UploadGammaRamp();

    uint32_t index = 0;
    bool shown = AcquireImage(index);
    if (shown) {
        present_index_ = index;
        DrawFrame(t, index);
    }
    Submit(false, shown);
    present_value_[presents_ % (kFrames + 1)].store(value_);
    if (shown) {
        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &swap_done_[index];
        pi.swapchainCount = 1;
        pi.pSwapchains = &swap_;
        pi.pImageIndices = &index;
        VkResult r = vkQueuePresentKHR(queue_, &pi);
        if (r == VK_ERROR_OUT_OF_DATE_KHR) swap_stale_ = true;
        if (r == VK_ERROR_SURFACE_LOST_KHR) surface_lost_ = true;
    }
    if (presents_ == 0) noble_android::SetBooted();
    presents_++;
    presents_seen_.store(presents_);
    frame_++;

    Frame& next = CurrentFrame();
    ResetFrame(next);
    vkResetCommandPool(dev_, next.pool, 0);
    Begin();
    ReleaseOldTextures();

    fps_n_++;
    auto now = std::chrono::steady_clock::now();
    double el = std::chrono::duration<double>(now - fps_t_).count();
    if (el >= 0.5) {
        fps_ = fps_n_ / el;
        fps_n_ = 0;
        fps_t_ = now;
    }
    if (presents_ == 120 || presents_ % 600 == 0) SavePipelineCache();
    ApplySettings();
}

}  // namespace

std::unique_ptr<Backend> CreateBackend() { return std::make_unique<VulkanBackend>(); }

}  // namespace gpu
