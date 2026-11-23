#include "gpu/shaders.h"
#include "gpu/vertex.h"

#include <algorithm>
#include <bit>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace gpu {

namespace {

std::string F(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return buf;
}

const char* kCommonHead = R"(
struct VOut {
    float4 pos : SV_Position;
    float4 d0 : COLOR0;
    float4 d1 : COLOR1;
    float4 b0 : COLOR2;
    float4 b1 : COLOR3;
    float4 tc0 : TEXCOORD0;
    float4 tc1 : TEXCOORD1;
    float4 tc2 : TEXCOORD2;
    float4 tc3 : TEXCOORD3;
    float fogamt : TEXCOORD4;
)"
#ifndef __ANDROID__
R"(    // frame generation's motion (PrevPositionHlsl, MotionOut): the clip position now and, from the draw's
    // constants of the frame before, then; mvp.z 1 when there is a then
    float4 mvc : TEXCOORD6;
    float4 mvp : TEXCOORD7;
)"
#endif
R"(};
)"
#ifdef __ANDROID__
R"(cbuffer VC : register(b0) { float4 c[192]; };
)"
#else
R"(// c: the draw's vertex constants; cp: the same draw's in the frame before, uploaded only for DrawConstants::motion
cbuffer VC : register(b0) { float4 c[192]; float4 cp[192]; };
)"
#endif
R"(cbuffer DC : register(b1) {
    float4 surf;
    float4 fogp;
    float4 fogcolor;
    float4 misc;
    float4 pc[18];
    float4 tfactor;
    float4 texsize[4];
    float4 bumpmat[4];
    float4 bumplum[4];
    float4x4 wvp;
)";
const char* kCommonTail = R"(    float4 spread[4];
    float4x4 wv;
    float4x4 wvn;
    float4x4 tm[4];
    float4 mat[5];
    float4 amb;
    float4 lt[56];
    float4 rtshift;
    float4 zparams;
    float4 motion;
};
)";

const ShaderHooks& Hooks() {
    static const ShaderHooks plain;
    return g_shader_hooks ? *g_shader_hooks : plain;
}

std::string Common() { return kCommonHead + Hooks().Constants() + kCommonTail; }

// vertex programs

const char* kVsHelpers = R"(
)"
#ifndef __ANDROID__
R"(// ray tracing's scene (raytrace.h): with rtc.y set, each vertex as drawn goes to the frame's capture buffer from slot
// rtc.x (kRtCapture)
RWByteAddressBuffer rtcap : register(u0);
cbuffer RtCapture : register(b2) { uint4 rtc; };
)"
#endif
R"(
float4 mac_mul(float4 a, float4 b) { return (a == 0 || b == 0) ? (float4)0 : a * b; }
int mac_arl(float v) { return (int)floor(v + 1e-3); }
// infinity as asfloat(0x7F800000), which every HLSL compiler takes (FXC alone reads 1.#INF)
// magnitude clamped between 1 / 2^64 and 2^64, sign kept
float magclamp(float v) { float m = clamp(abs(v), 5.42101e-20, 1.84467e+19); return v < 0 ? -m : m; }
float4 ilu_rcp(float v) { return (1.0 / v).xxxx; }
float4 ilu_rcc(float v) { return magclamp(1.0 / v).xxxx; }
float4 ilu_rsq(float v) { return v == 0 ? asfloat(0x7F800000u).xxxx : (rsqrt(abs(v))).xxxx; }
// 2^floor(v), fraction, 2^v
float4 ilu_exp(float v) {
    float whole = floor(v);
    return float4(exp2(whole), v - whole, exp2(v), 1);
}
// exponent, mantissa, log2 of |v|; zero logs to negative infinity
float4 ilu_log(float v) {
    float mag = abs(v), l2 = log2(mag), ex = floor(l2);
    return mag == 0 ? float4(-asfloat(0x7F800000u), 1, -asfloat(0x7F800000u), 1) : float4(ex, mag / exp2(ex), l2, 1);
}
// power clamped to 128 less 1/256
float4 ilu_lit(float4 s) {
    const float cap = 127.99609375;
    float diffuse = max(s.x, 0);
    float spec = exp2(clamp(s.w, -cap, cap) * log2(max(s.y, 0)));
    return float4(1, diffuse, diffuse > 0 ? spec : 0, 1);
}
float fogfactor(float d) {
    if (fogp.w == 3) return (fogp.y - d) / (fogp.y - fogp.x);
    if (fogp.w == 1) return exp(-d * fogp.z);
    if (fogp.w == 2) return exp(-(d * fogp.z) * (d * fogp.z));
    return 1;
}
)";

struct Tok {
    const uint32_t* t;
    uint32_t f(int sub, int bit, int len) const { return (t[sub] >> bit) & ((1u << len) - 1); }
};

const char* kMask[16] = {"", "w", "z", "zw", "y", "yw", "yz", "yzw", "x", "xw", "xz", "xzw", "xy", "xyw", "xyz", "xyzw"};
const char* kLanes[5] = {"", "x", "xy", "xyz", "xyzw"};  // oFog takes one lane per mask bit, from x
const char* kOut[16] = {"oPos", "", "", "oD0", "oD1", "oFog", "oPts", "oB0", "oB1", "oT0", "oT1", "oT2", "oT3", "", "", ""};

// operand A 0, B 1, C 2
std::string Src(const Tok& k, int which, uint16_t regs, const char* cname = "c") {
    uint32_t neg, sw[4], reg, mux;
    if (which == 0) {
        neg = k.f(1, 8, 1);
        sw[0] = k.f(1, 6, 2), sw[1] = k.f(1, 4, 2), sw[2] = k.f(1, 2, 2), sw[3] = k.f(1, 0, 2);
        reg = k.f(2, 28, 4), mux = k.f(2, 26, 2);
    } else if (which == 1) {
        neg = k.f(2, 25, 1);
        sw[0] = k.f(2, 23, 2), sw[1] = k.f(2, 21, 2), sw[2] = k.f(2, 19, 2), sw[3] = k.f(2, 17, 2);
        reg = k.f(2, 13, 4), mux = k.f(2, 11, 2);
    } else {
        neg = k.f(2, 10, 1);
        sw[0] = k.f(2, 8, 2), sw[1] = k.f(2, 6, 2), sw[2] = k.f(2, 4, 2), sw[3] = k.f(2, 2, 2);
        reg = k.f(2, 0, 2) << 2 | k.f(3, 30, 2), mux = k.f(3, 28, 2);
        uint32_t ilu = k.f(1, 25, 3);
        if (ilu >= 2 && ilu <= 6) sw[1] = sw[2] = sw[3] = sw[0];  // scalar ops read x
    }
    // mux: 1 temporary (R12 reads oPos), 2 input, 3 constant
    std::string base;
    if (mux == 1) {
        base = reg == 12 ? "oPos" : F("R%u", reg);
    } else if (mux == 2) {
        uint32_t v = k.f(1, 9, 4);
        base = (regs >> v) & 1 ? F("i.v%u", v) : (v == 3 ? "float4(1,1,1,1)" : "float4(0,0,0,1)");
    } else {
        uint32_t cr = k.f(1, 13, 8);
        base = k.f(3, 1, 1) ? F("%s[clamp(a0 + %u, 0, 191)]", cname, cr) : F("%s[%u]", cname, cr);
    }
    const char* xyzw = "xyzw";
    std::string s = F("(%s(%s).%c%c%c%c)", neg ? "-" : "", base.c_str(), xyzw[sw[0]], xyzw[sw[1]], xyzw[sw[2]], xyzw[sw[3]]);
    return s;
}

// MAC opcodes from 1: mov mul add mad dp3 dph dp4 dst min max slt sge arl
std::string MacExpr(uint32_t op, const std::string& a, const std::string& b, const std::string& c) {
    switch (op) {
        case 1: return a;
        case 2: return "mac_mul(" + a + ", " + b + ")";
        case 3: return "(" + a + " + " + c + ")";
        case 4: return "(mac_mul(" + a + ", " + b + ") + " + c + ")";
        case 5: return "dot(" + a + ".xyz, " + b + ".xyz).xxxx";
        case 6: return "dot(float4(" + a + ".xyz, 1), " + b + ").xxxx";
        case 7: return "dot(" + a + ", " + b + ").xxxx";
        case 8: return "float4(1, " + a + ".y * " + b + ".y, " + a + ".z, " + b + ".w)";
        case 9: return "min(" + a + ", " + b + ")";
        case 10: return "max(" + a + ", " + b + ")";
        case 11: return "(float4)(" + a + " < " + b + ")";
        case 12: return "(float4)(" + a + " >= " + b + ")";
        case 13: return a;
        default: return "(float4)0";
    }
}

// ILU opcodes from 1: mov rcp rcc rsq exp log lit
std::string IluExpr(uint32_t op, const std::string& c) {
    switch (op) {
        case 1: return c;
        case 2: return "ilu_rcp(" + c + ".x)";
        case 3: return "ilu_rcc(" + c + ".x)";
        case 4: return "ilu_rsq(" + c + ".x)";
        case 5: return "ilu_exp(" + c + ".x)";
        case 6: return "ilu_log(" + c + ".x)";
        case 7: return "ilu_lit(" + c + ")";
        default: return "(float4)0";
    }
}

std::string RegDest(uint32_t r) { return r == 12 ? "oPos" : F("R%u", r); }

std::string VsInputStruct(uint16_t regs) {
    std::string s = "struct VIn {\n";
    for (int r = 0; r < 16; r++)
        if ((regs >> r) & 1) s += F("    float4 v%d : ATTR%d;\n", r, r);
#ifdef __ANDROID__
    if (!regs) s += "    uint id : SV_VertexID;\n";
#else
    s += "    uint id : SV_VertexID;\n";  // ray tracing's capture (kRtCapture), and a draw without inputs
#endif
    return s + "};\n";
}

// the input struct and the head of main: elements read raw are finished into float4 i.vN first
std::string VsInputs(uint16_t regs, uint64_t decode) {
    if (!decode) return VsInputStruct(regs) + "VOut main(VIn i) {\n";
    std::string raw = "struct VRaw {\n", in = "struct VIn {\n", body;
    bool packed = false;
    for (int r = 0; r < 16; r++) {
        if (!((regs >> r) & 1)) continue;
        uint32_t d = uint32_t(decode >> (r * 4)) & 15;
        const char* type = d == kDecodeInt || d == kDecodeIntW1 ? "int4" : d == kDecodePacked ? "uint" : "float4";
        raw += F("    %s v%d : ATTR%d;\n", type, r, r);
        in += F("    float4 v%d;\n", r);
        packed |= d == kDecodePacked;
        switch (d) {
            case kDecodeW1: body += F("    i.v%d = float4(ri.v%d.xyz, 1);\n", r, r); break;
            case kDecodeInt: body += F("    i.v%d = float4(ri.v%d);\n", r, r); break;
            case kDecodeIntW1: body += F("    i.v%d = float4(float3(ri.v%d.xyz), 1);\n", r, r); break;
            case kDecodePacked: body += F("    i.v%d = UnpackNp3(ri.v%d);\n", r, r); break;
            case kDecodeFloat2H: body += F("    i.v%d = float4(ri.v%d.xy, 0, ri.v%d.z);\n", r, r, r); break;
            default: body += F("    i.v%d = ri.v%d;\n", r, r); break;
        }
    }
#ifndef __ANDROID__
    raw += "    uint id : SV_VertexID;\n";
    in += "    uint id;\n";
    body += "    i.id = ri.id;\n";
#endif
    std::string s = raw + "};\n" + in + "};\n";
    if (packed)
        s += "float4 UnpackNp3(uint u) {\n"
             "    int3 v = int3(int(u << 21) >> 21, int(u << 10) >> 21, int(u) >> 22);\n"
             "    return float4(max(float3(v) / float3(1023, 1023, 511), -1), 1);\n"
             "}\n";
    return s + "VOut main(VRaw ri) {\n    VIn i;\n" + body;
}

const char* kVsEpilogue = R"(
    VOut o;
    precise float4 p = oPos;
    p.w = magclamp(p.w);
    precise float4 q;
    // full screen passes drawn half a pixel up-left for the console's integer pixel centers stop half a pixel short
    // of the far edges on D3D12's, which upscaled leaves the last host columns and rows undrawn
    // surf.w set: a point, which stays put since pulled out it would leave the target
    if (surf.w == 0 && abs(p.x - (surf.x - 0.5)) < 1e-3) p.x = surf.x;
    if (surf.w == 0 && abs(p.y - (surf.y - 0.5)) < 1e-3) p.y = surf.y;
    q.x = p.x * 2 / surf.x - 1;
    q.y = 1 - p.y * 2 / surf.y;
    q.z = p.z * surf.z;
    q.xyz *= p.w;
    q.w = p.w;
    o.pos = q;
@POSITION@    o.d0 = saturate(oD0);
    o.d1 = saturate(oD1);
    o.b0 = saturate(oB0);
    o.b1 = saturate(oB1);
    o.tc0 = oT0;
    o.tc1 = oT1;
    o.tc2 = oT2;
    o.tc3 = oT3;
    o.fogamt = fogfactor(oFog.x);
@MOTION@    return o;
}
)";

// the epilogue's @POSITION@ mark, before the line it starts: ShaderHooks::Position
std::string WithPosition(std::string s) {
    constexpr std::string_view kMark = "@POSITION@";
    size_t at = s.find(kMark);
    if (at != std::string::npos) s.replace(at, kMark.size(), Hooks().Position());
    return s;
}

// ray tracing's capture (raytrace.h): the vertex as drawn, anti-aliasing's offset and all (as the depth the passes
// find a pixel's place from was drawn: the two agree at every edge), at its slot: clip x, y, w (the scene's
// positions), z; its first texture coordinates; its colour; its index. A uniform branch, off but for the frame's 3D
// while there is ray tracing
#ifdef __ANDROID__
const char* kRtCapture = "";
#else
const char* kRtCapture = R"(    [branch] if (rtc.y != 0) {
        uint ra = (rtc.x + i.id) * 32;
        rtcap.Store4(ra, asuint(float4(o.pos.x, o.pos.y, o.pos.w, o.pos.z)));
        uint4 col = uint4(saturate(o.d0) * 255 + 0.5);
        rtcap.Store4(ra + 16, uint4(asuint(o.tc0.x), asuint(o.tc0.y), col.r | col.g << 8 | col.b << 16 | col.a << 24, i.id));
    }
)";
#endif

// the epilogue's @MOTION@ mark: a program's clip positions now and in the frame before (PrevPositionHlsl,
// MotionOut), else none. Then anti-aliasing's sub-pixel offset (motion.zw, clip units; 0 but for the frame's 3D),
// after the motion is taken, which goes without it
std::string WithMotion(std::string s, bool program) {
    constexpr std::string_view kMark = "@MOTION@";
    size_t at = s.find(kMark);
    if (at == std::string::npos) return s;
#ifdef __ANDROID__
    (void)program;
    s.replace(at, kMark.size(), "    o.pos.xy += motion.zw * o.pos.w;\n");
    return s;
#else
    std::string none = std::string("    o.mvc = o.pos;\n    o.mvp = 0;\n    o.pos.xy += motion.zw * o.pos.w;\n") + kRtCapture;
    // the frame before's position only when the renderer uploaded its constants: a uniform branch, skipped otherwise
    std::string prev = std::string("    o.mvc = o.pos;\n    o.mvp = 0;\n"
                                   "    [branch] if (motion.x != 0) {\n"
                                   "        o.mvp = HookPos(PrevClip(i));\n"
                                   "        o.mvp.z = 1;\n"
                                   "    }\n"
                                   "    o.pos.xy += motion.zw * o.pos.w;\n") +
                       kRtCapture;
    s.replace(at, kMark.size(), program ? prev : none);
    return s;
#endif
}

std::string VsEpilogue(bool program = false) { return WithMotion(WithPosition(kVsEpilogue), program); }

const char* kVsPrologue = R"(
    int a0 = 0;
    // precise: EQUAL depth passes need bit exact positions
    precise float4 R0 = 0, R1 = 0, R2 = 0, R3 = 0, R4 = 0, R5 = 0, R6 = 0, R7 = 0, R8 = 0, R9 = 0, R10 = 0, R11 = 0;
    precise float4 oPos = 0;
    float4 oD0 = float4(0, 0, 0, 1), oD1 = 0, oB0 = float4(0, 0, 0, 1), oB1 = 0, oFog = 0, oPts = 1;
    float4 oT0 = float4(0, 0, 0, 1), oT1 = float4(0, 0, 0, 1), oT2 = float4(0, 0, 0, 1), oT3 = float4(0, 0, 0, 1);
)";

// combiners

struct In {
    uint32_t reg, chan, mod;
};
In ParseIn(uint32_t v) { return {v & 0xF, v & 0x10, v & 0xE0}; }

struct Ps {
    const PixelKey& k;
    int stage = 0;
    std::string e, f;  // final combiner E F for EF_PROD
    bool clamp_sum = false, inv_v1 = false, inv_r0 = false;
    std::string code;

    std::string Var(uint32_t reg) {
        uint32_t flags = k.count >> 8;  // PS_COMBINERCOUNT flags: 0x10 unique C0 per stage, 0x100 unique C1
        switch (reg) {
            case 0: return "float4(0,0,0,0)";
            case 1: return F("pc[%d]", stage == 8 ? 16 : ((flags & 0x10) ? stage * 2 : 0));
            case 2: return F("pc[%d]", stage == 8 ? 17 : ((flags & 0x100) ? stage * 2 + 1 : 1));
            case 3: return "fogin";
            case 4: return "v0";
            case 5: return "v1";
            case 8: return "t0";
            case 9: return "t1";
            case 10: return "t2";
            case 11: return "t3";
            case 12: return "r0";
            case 13: return "r1";
            case 14:
                return F("%s(float4(%s.rgb + %s.rgb, 0))", clamp_sum ? "saturate" : "", inv_v1 ? "(1 - v1)" : "v1",
                         inv_r0 ? "(1 - r0)" : "r0");
            case 15: return "float4(" + e + " * " + f + ", 0)";
            default: return "float4(0,0,0,0)";
        }
    }

    std::string Input(In in, bool alpha) {
        std::string r = Var(in.reg);
        if (!alpha) r += in.chan ? ".aaa" : ".rgb";
        else r += in.chan ? ".a" : ".b";
        switch (in.mod) {
            // PS_INPUTMAPPING
            case 0x00: return "max(" + r + ", 0)";
            case 0x20: return "(1 - saturate(" + r + "))";
            case 0x40: return "(2 * max(" + r + ", 0) - 1)";
            case 0x60: return "(-2 * max(" + r + ", 0) + 1)";
            case 0x80: return "(max(" + r + ", 0) - 0.5)";
            case 0xA0: return "(-max(" + r + ", 0) + 0.5)";
            case 0xC0: return r;
            default: return "-" + r;
        }
    }

    static std::string Map(const std::string& v, uint32_t m) {
        switch (m) {
            // PS_COMBINEROUTPUT bias and shift
            case 0x08: return "(" + v + " - 0.5)";
            case 0x10: return "(" + v + " * 2)";
            case 0x18: return "((" + v + " - 0.5) * 2)";
            case 0x20: return "(" + v + " * 4)";
            case 0x30: return "(" + v + " / 2)";
            default: return v;
        }
    }

    static const char* Dest(uint32_t reg) {
        switch (reg) {
            case 4: return "v0";
            case 5: return "v1";
            case 8: return "t0";
            case 9: return "t1";
            case 10: return "t2";
            case 11: return "t3";
            case 12: return "r0";
            case 13: return "r1";
            default: return nullptr;
        }
    }

    // rgb and alpha halves read the registers before either writes, so the writes come back to the caller
    std::string Stage(uint32_t inputs, uint32_t output, bool alpha) {
        In d = ParseIn(inputs & 0xFF), c = ParseIn((inputs >> 8) & 0xFF), b = ParseIn((inputs >> 16) & 0xFF),
           a = ParseIn(inputs >> 24);
        // fl: 1 cd dot, 2 ab dot, 4 mux, 0x38 mapping, 0x40 and 0x80 blue to alpha
        uint32_t cd_dst = output & 0xF, ab_dst = (output >> 4) & 0xF, sum_dst = (output >> 8) & 0xF, fl = output >> 12;
        const char* m = alpha ? "a" : "rgb";
        const char* T = alpha ? "float" : "float3";
        const char* n = alpha ? "a" : "c";
        std::string A = Input(a, alpha), B = Input(b, alpha), C = Input(c, alpha), D = Input(d, alpha);
        std::string ab = (fl & 2) ? "dot(" + A + ", " + B + ")" : "(" + A + " * " + B + ")";
        std::string cd = (fl & 1) ? "dot(" + C + ", " + D + ")" : "(" + C + " * " + D + ")";
        uint32_t map = fl & 0x38;
        code += F("%s ab%s = clamp((%s)%s, -1, 1);\n", T, n, T, Map(ab, map).c_str());
        code += F("%s cd%s = clamp((%s)%s, -1, 1);\n", T, n, T, Map(cd, map).c_str());
        std::string sum = (fl & 4) ? F("((%s) ? (%s)%s : (%s)%s)",
                                       ((k.count >> 8) & 1) ? "r0.a >= 0.5" : "(uint(r0.a * 255) & 1) == 1", T,
                                       cd.c_str(), T, ab.c_str())
                                   : "(" + ab + " + " + cd + ")";
        code += F("%s ms%s = clamp((%s)%s, -1, 1);\n", T, n, T, Map(sum, map).c_str());
        std::string post;
        if (const char* r = Dest(ab_dst)) {
            post += F("%s.%s = ab%s;\n", r, m, n);
            if (!alpha && (fl & 0x80)) post += F("%s.a = abc.b;\n", r);
        }
        if (const char* r = Dest(cd_dst)) {
            post += F("%s.%s = cd%s;\n", r, m, n);
            if (!alpha && (fl & 0x40)) post += F("%s.a = cdc.b;\n", r);
        }
        if (const char* r = Dest(sum_dst)) post += F("%s.%s = ms%s;\n", r, m, n);
        return post;
    }
};

// a dot stage's operand: the source stage's rgb, raw or read back as signed bytes
std::string DotInput(uint32_t map, uint32_t src) {
    switch (map) {
        case 1: return F("sbyte(t%u.rgb, 128, 128, 127)", src);        // offset by 128
        case 2: return F("sbyte(t%u.rgb, -0.5, 255.5, 127.5)", src);  // two's complement, (2b + 1) / 255
        case 3: return F("sbyte(t%u.rgb, 0, 256, 127)", src);         // two's complement
        default: return F("t%u.rgb", src);
    }
}

// stage i's coordinates as the title wrote them, over 0..1. The console draws half a pixel right and down of the
// host unless a program writes positions raw: a render target is read where the console's offset puts it
// box_w: kept inside DrawConstants::uvbox (PixelKey::texbox), the coordinates divided by it as uv is
std::string TexUv(const TexInfo& t, int i, const std::string& uv, const char* box_w = nullptr) {
    std::string n = "(" + uv + ")";
    if (t.rect) n += F(" / texsize[%d].xy", i);
    if (box_w && t.kind == kTex2D && !t.shadow && !(t.flags & kTexTarget))
        n = F("texbox(%s, %s, texsize[%d].xy, %s)", n.c_str(), box_w, i, t.rect ? "true" : "false");
    n += F(" + texsize[%d].zw", i);
    if (!(t.flags & kTexTarget)) return n;
    std::string step = t.aspect ? F("ddx(%s) * %.3f + ddy(%s)", n.c_str(), t.aspect / 1000.0, n.c_str())
                                : "ddx(" + n + ") + ddy(" + n + ")";
    return n + F(" + rtshift[%d] * (", i) + step + ")";
}

std::string MaterialUv(const PixelKey& k, int i);

// the material stage's fetch: its coordinates (muv) and their steps across a pixel, then moved by parallax (puv), which
// the picture and its maps (material) are read at
std::string MaterialFetch(int i, const std::string& uv) {
    return F("float2 muv = %s;\nfloat2 mdx = ddx(muv), mdy = ddy(muv);\nfloat2 puv = parallax(muv, mdx, mdy, i.pos, smp%d);\n"
             "float4 t%d = tex%d.SampleGrad(smp%d, puv, mdx, mdy);\n",
             uv.c_str(), i, i, i, i);
}

// texture shader stage i, per its mode
std::string TexFetch(const PixelKey& k, int i) {
    uint32_t mode = (k.tex_modes >> (i * 5)) & 0x1F;
    const TexInfo& t = k.tex[i];
    uint32_t in = i == 1 ? 0 : i == 2 ? (k.input_texture >> 16) & 0xF : (k.input_texture >> 20) & 0xF;
    uint32_t dm = i == 0 ? 0 : (k.dot_mapping >> ((i - 1) * 4)) & 0xF;
    const std::string tc = F("tc%d", i);
    const char* q = tc.c_str();
    auto put = [&](const std::string& v) { return F("float4 t%d = %s;\n", i, v.c_str()); };
    // dpN, or dpNn for stage N + 1's product taken early
    auto dp = [&](int n, int coord, uint32_t map, uint32_t src) {
        return F("float dp%d%s = dot(tc%d.xyz, %s);\n", n, coord == n ? "" : "n", coord, DotInput(map, src).c_str());
    };
    auto tex2d = [&](const std::string& uv) {
        if (t.flags & kTexWide) return F("spreadtap(tex%d, smp%d, %s, spread[%d], %s)", i, i, uv.c_str(), i, Hooks().ReadScale(t).c_str());
        return F("tex%d.Sample(smp%d, %s)", i, i, uv.c_str());
    };
    auto cube = [&](const std::string& dir) {
        std::string c = Hooks().Cube(t, i, dir);
        return c.empty() ? F("tex%d.Sample(smp%d, %s)", i, i, dir.c_str()) : c;
    };
    const char* box_w = k.texbox && i == 0 && mode == 1 ? "tc0.w" : nullptr;
    auto sample2 = [&](const std::string& uv) {
        if (t.kind == kTex2D) return tex2d(TexUv(t, i, uv, box_w));
        if (t.kind == kTexCube) return cube("float3(" + uv + ", 1)");
        if (t.kind == kTex3D) return F("tex%d.Sample(smp%d, float3(%s, 0))", i, i, uv.c_str());
        return std::string("float4(0,0,0,1)");
    };
    auto sample3 = [&](const std::string& uvw) {
        if (t.kind == kTex2D) return tex2d(TexUv(t, i, "(" + uvw + ").xy"));
        if (t.kind == kTexCube) return cube(uvw);
        if (t.kind == kTex3D) return F("tex%d.Sample(smp%d, %s)", i, i, uvw.c_str());
        return std::string("float4(0,0,0,1)");
    };
    auto depth = [&](const std::string& ref) {
        std::string uv = TexUv(t, i, F("%s.xy / %s.w", q, q));
        std::string rest = F(", %s, %d, i.pos.xy, texsize[%d].xy)", ref.c_str(), t.shadow, i);
        return put(F("shadow(tex%d, smp%d, ", i, i) + uv + rest);
    };
    if (i == int(k.material) - 1 && (mode == 1 || mode == 2) && !t.shadow) return MaterialFetch(i, MaterialUv(k, i));
    std::string s;
    switch (mode) {
        // PS_TEXTUREMODES: 1 project2d, 2 project3d, 3 cubemap, 4 passthru, 5 clipplane, 6 and 7 bumpenvmap, 9 dot_st, 0xA dot_zw, 0xD dot_str_3d, 0xE dot_str_cube, 0xF dpndnt_ar, 0x10 dpndnt_gb, 0x11 dotproduct
        case 0: return put("float4(0,0,0,1)");
        case 1: return t.shadow ? depth("0") : put(sample2(F("%s.xy / %s.w", q, q)));
        case 2: return t.shadow ? depth(F("%s.z / %s.w", q, q)) : put(sample3(F("%s.xyz / %s.w", q, q)));
        case 3: return put(sample3(tc + ".xyz"));
        case 4: return put(tc);
        case 5:  // clip plane: a set compare_mode bit kills at >= 0, a clear one below 0
            s = put("0");
            for (int j = 0; j < 4; j++) s += F("if (%s.%c %s 0) discard;\n", q, "xyzw"[j], ((k.compare_mode >> (i * 4 + j)) & 1) ? ">=" : "<");
            return s;
        case 6:
        case 7:  // bump env map: signed du dv through the 2x2 matrix, 7 scales by luminance
            s = F("float2 duv%d = sbyte(t%u.bgr, 0, 256, 127).xy;\n", i, in);
            s += put(sample2(F("%s.xy + float2(dot(bumpmat[%d].xz, duv%d), dot(bumpmat[%d].yw, duv%d))", q, i, i, i, i)));
            if (mode == 7) s += F("t%d *= bumplum[%d].x * t%u.r + bumplum[%d].y;\n", i, i, in, i);
            return s;
        case 9: return dp(i, i, dm, in) + put(sample2(F("float2(dp%d, dp%d)", i - 1, i)));
        case 0xA:
        case 0x11: return dp(i, i, dm, in) + put("0");
        case 0xB:  // reflect diffuse at stage 2: stage 3's product early for the normal
            s = dp(i, i, dm, in) + dp(i, i + 1, (k.dot_mapping >> 8) & 0xF, (k.input_texture >> 20) & 0xF);
            return s + put(sample3(F("float3(dp%d, dp%d, dp%dn)", i - 1, i, i)));
        case 0xC:  // reflect specular at stage 3: eye from the three w
            s = dp(i, i, dm, in);
            s += F("float3 nrm%d = float3(dp%d, dp%d, dp%d), eye%d = float3(tc%d.w, tc%d.w, tc%d.w);\n", i, i - 2, i - 1, i, i, i - 2,
                   i - 1, i);
            s += F("float3 refl%d = 2 * nrm%d * dot(nrm%d, eye%d) / dot(nrm%d, nrm%d) - eye%d;\n", i, i, i, i, i, i, i);
            return s + put(sample3(F("refl%d", i)));
        case 0xD:
        case 0xE: return dp(i, i, dm, in) + put(sample3(F("float3(dp%d, dp%d, dp%d)", i - 2, i - 1, i)));
        case 0xF: return put(sample2(F("t%u.ar", in)));
        case 0x10: return put(sample2(F("t%u.gb", in)));
        default: return put("0");
    }
}

const char* kPsSbyte = R"(
// unorm to byte b, less lo under 128 or hi from 128, over d
float3 sbyte(float3 u, float lo, float hi, float d) { float3 b = u * 255; return (b - (b >= 128 ? hi : lo)) / d; }
)";
const char* kPsTail = R"(float4 spreadtap(Texture2D t, SamplerState s, float2 uv, float4 w, float m) {
    float4 c = 0;
    int n = (int)w.z;
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) {
            float4 v = t.SampleLevel(s, uv + w.xy * (float2(x, y) - (n - 1) * 0.5), 0);
            c += float4(saturate(v.rgb * m), v.a);
        }
    return c / (n * n);
}
void alphatest(float a) {
    int f = (int)misc.y;
    int v = (int)round(a * 255), r = (int)misc.x;
    bool ok = f == 0 ? false : f == 1 ? v < r : f == 2 ? v == r : f == 3 ? v <= r : f == 4 ? v > r : f == 5 ? v != r : f == 6 ? v >= r : true;
    if (!ok) discard;
}
)";

std::string PsHelpers() { return kPsSbyte + Hooks().Shadow() + kPsTail; }

// fixed function texture stage arguments, D3DTA
std::string FfArg(uint32_t a, int stage, bool alpha) {
    std::string v;
    switch (a & 0xF) {
        case 0: v = "v0"; break;
        case 1: v = "cur"; break;
        case 2: v = F("t%d", stage); break;
        case 3: v = "tfactor"; break;
        case 4: v = "v1"; break;
        case 5: v = "tmp"; break;
        default: v = "cur"; break;
    }
    if (a & 0x20) v = "(" + v + ").aaaa";
    if (a & 0x10) v = "(1 - " + v + ")";
    return "(" + v + ")" + (alpha ? ".a" : ".rgb");
}

// D3DTOP
std::string FfOp(uint32_t op, const std::string& a1, const std::string& a2, const std::string& a0, int stage, bool alpha) {
    switch (op) {
        case 2: return a1;
        case 3: return a2;
        case 4: return a1 + " * " + a2;
        case 5: return a1 + " * " + a2 + " * 2";
        case 6: return a1 + " * " + a2 + " * 4";
        case 7: return a1 + " + " + a2;
        case 8: return a1 + " + " + a2 + " - 0.5";
        case 9: return "(" + a1 + " + " + a2 + " - 0.5) * 2";
        case 10: return a1 + " - " + a2;
        case 11: return a1 + " + " + a2 + " * (1 - " + a1 + ")";
        case 12: return "lerp(" + a2 + ", " + a1 + ", v0.a)";
        case 13: return "lerp(" + a2 + ", " + a1 + ", cur.a)";
        case 14: return F("lerp(%s, %s, t%d.a)", a2.c_str(), a1.c_str(), stage);
        case 15: return "lerp(" + a2 + ", " + a1 + ", tfactor.a)";
        case 16: return F("%s + %s * (1 - t%d.a)", a1.c_str(), a2.c_str(), stage);
        case 18: return alpha ? a1 : a1 + " + " + a2 + " * " + a1.substr(0, a1.size() - 4) + ".a";
        case 19: return alpha ? a1 : a1 + " * " + a2 + " + " + a1.substr(0, a1.size() - 4) + ".a";
        case 20: return alpha ? a1 : a1 + " + " + a2 + " * (1 - " + a1.substr(0, a1.size() - 4) + ".a)";
        case 21: return alpha ? a1 : "(1 - " + a1 + ") * " + a2 + " + " + a1.substr(0, a1.size() - 4) + ".a";
        case 22: return F("saturate(dot((%s - 0.5) * 2, (%s - 0.5) * 2))", alpha ? ("float3(" + a1 + ",0,0)").c_str() : a1.c_str(),
                          alpha ? ("float3(" + a2 + ",0,0)").c_str() : a2.c_str());
        case 23: return a0 + " + " + a1 + " * " + a2;
        case 24: return "lerp(" + a2 + ", " + a1 + ", " + a0 + ")";
        default: return alpha ? "cur.a" : "cur.rgb";
    }
}

}  // namespace

bool PixelKey::operator==(const PixelKey& o) const { return std::memcmp(this, &o, sizeof(*this)) == 0; }

std::string ShaderHooks::Constants() const { return "    float4 ext;\n    float4 ext2;\n"; }

std::string ShaderHooks::Shadow() const {
    std::string r = g_reversed_depth ? "1 - clamp(z, 0, mx) / mx" : "clamp(z, 0, mx) / mx";
    return R"(// the sampler compares, a linear filter blends the four results like the console does
float4 shadow(Texture2D<float> t, SamplerComparisonState s, float2 uv, float z, int fmt, float2 px, float2 size) {
    float mx = fmt == 2 ? 65535.0 : 16777215.0;
    return t.SampleCmpLevelZero(s, uv, )" + r + R"().xxxx;
}
)";
}

std::string ShaderHooks::Output() const { return "outc.rgb = saturate(outc.rgb);\n"; }

uint64_t Hash(const void* p, size_t n, uint64_t h) {
    // 16 bytes a step through a 64 x 64 -> 128 bit multiply: it runs over textures every frame and keys every draw
    constexpr uint64_t k0 = 0x9E3779B97F4A7C15ull, k1 = 0xE7037ED1A0B428DBull;
    auto mix = [](uint64_t a, uint64_t b) {
        unsigned __int128 r = static_cast<unsigned __int128>(a) * b;
        return uint64_t(r) ^ uint64_t(r >> 64);
    };
    const uint8_t* b = static_cast<const uint8_t*>(p);
    uint64_t seed = h ^ k0;
    size_t i = 0;
    for (; i + 16 <= n; i += 16) {
        uint64_t x, y;
        std::memcpy(&x, b + i, 8);
        std::memcpy(&y, b + i + 8, 8);
        seed = mix(x ^ k1, y ^ seed);
    }
    if (size_t r = n - i) {
        uint64_t x = 0, y = 0;
        if (r >= 8) {
            std::memcpy(&x, b + i, 8);
            std::memcpy(&y, b + n - 8, 8);
        } else {
            std::memcpy(&x, b + i, r);
        }
        seed = mix(x ^ k1, y ^ seed ^ r);
    }
    return mix(seed ^ k0, uint64_t(n) ^ k1);
}

namespace {

// D3DTOP and D3DTA values PixelKey::ff holds
constexpr uint32_t kTopSelectArg1 = 2;
constexpr uint32_t kTopSelectArg2 = 3;
constexpr uint32_t kTopBlendTextureAlpha = 14;
constexpr uint32_t kTopBlendTextureAlphaPm = 16;
constexpr uint32_t kTopModulateAlphaAddColor = 18;
constexpr uint32_t kTopModulateInvColorAddAlpha = 21;
constexpr uint32_t kTopMultiplyAdd = 23;
constexpr uint32_t kTopLerp = 24;
constexpr uint32_t kTaCurrent = 1;
constexpr uint32_t kTaTexture = 2;

// args in PixelKey::ff order: arg1, arg2, arg0
bool ReadsTexture(uint32_t op, const uint32_t* args, bool alpha) {
    if (op == kTopBlendTextureAlpha || op == kTopBlendTextureAlphaPm) return true;
    bool alpha_arg1_only = alpha && op >= kTopModulateAlphaAddColor && op <= kTopModulateInvColorAddAlpha;
    bool uses_arg1 = op != kTopSelectArg2;
    bool uses_arg2 = op != kTopSelectArg1 && !alpha_arg1_only;
    bool uses_arg0 = op == kTopMultiplyAdd || op == kTopLerp;
    auto texture = [](uint32_t a) { return (a & 0xF) == kTaTexture; };
    return (uses_arg1 && texture(args[0])) || (uses_arg2 && texture(args[1])) || (uses_arg0 && texture(args[2]));
}

// the XDK's combiner builder turns a stage op that reads a missing texture into a pass of the current colour
void PassMissingTexture(uint32_t* f) {
    if (f[0] > 1 && ReadsTexture(f[0], f + 1, false)) {
        f[0] = kTopSelectArg1;
        f[1] = kTaCurrent;
    }
    if (f[4] > 1 && ReadsTexture(f[4], f + 5, true)) {
        f[4] = kTopSelectArg1;
        f[5] = kTaCurrent;
    }
}

}  // namespace

PixelKey MakePixelKey(const State& s, const TexInfo tex[4]) {
    PixelKey k;
    std::memset(&k, 0, sizeof(k));
    for (int i = 0; i < 4; i++) {
        k.tex[i] = tex[i];
        if (s.tss[size_t(i)][kTssAlphaKill] == 4) k.alphakill |= 1u << i;  // D3DTALPHAKILL_ENABLE
    }
    k.flat = s.simple[kMShadeMode >> 2] == 0x1D00;  // D3DSHADE_FLAT
    k.two_sided = s.rs[kRsTwoSidedLighting] != 0;
    k.alphatest = s.simple[kMAlphaTestEnable >> 2] != 0;
    if (s.ps) {
        const uint32_t* d = s.ps->def;
        k.programmable = 1;
        // D3DPIXELSHADERDEF dwords: alpha inputs 0, final 8 and 9, alpha outputs 26, rgb inputs 34, compare mode 42, rgb outputs 45, count 53, dot mapping 55, input texture 56
        k.count = d[53];
        for (int i = 0; i < 8; i++) {
            k.a_in[i] = d[i];
            k.a_out[i] = d[26 + i];
            k.rgb_in[i] = d[34 + i];
            k.rgb_out[i] = d[45 + i];
        }
        k.final_abcd = d[8];
        k.final_efg = d[9];
        // the stage modes follow the bound textures, as D3D's lazy shader stage setup does
        uint32_t modes = s.rs[kRsPsTextureModes];
        for (int i = 0; i < 4; i++) {
            uint32_t m = (modes >> (i * 5)) & 0x1F;
            const TexInfo& t = tex[i];
            if (!t.kind && m != 4 && m != 5 && m != 0xA && m != 0x11) m = 0;
            else if (m >= 1 && m <= 3) m = t.kind == kTexCube ? 3 : (t.kind == kTex3D || t.shadow) ? 2 : 1;
            else if (m == 0xD || m == 0xE) m = t.kind == kTexCube ? 0xE : 0xD;
            k.tex_modes |= m << (i * 5);
        }
        k.dot_mapping = d[55];
        k.input_texture = d[56];
        k.compare_mode = d[42];
        return k;
    }
    // the console's stages divide by q; a vertex program's q is its own
    bool program = (s.vs_handle & 1) && s.vs && !s.vs->program.empty();
    for (int i = 0; i < 4; i++) {
        const auto& t = s.tss[size_t(i)];
        uint32_t v[9] = {t[kTssColorOp], t[kTssColorArg1], t[kTssColorArg2], t[kTssColorArg0], t[kTssAlphaOp],
                         t[kTssAlphaArg1],  t[kTssAlphaArg2], t[kTssAlphaArg0], t[kTssResultArg]};
        // A later stage with no texture that still reads D3DTA_TEXTURE ends the cascade: XDK D3D builds that
        // stage's combiner as current * 1, a pass through. Taking the missing texture as white instead let a
        // MODULATE(TEXTURE, DIFFUSE) left over from a multitexture draw replace the colour with the diffuse.
        // (Castlevania: Curse of Darkness draws its single texture quads so.) ff[i] stays 0, which ends it.
        auto reads_texture = [&](int first, int n) {
            for (int a = first; a < first + n; a++)
                if ((v[a] & 0xF) == 2) return true;  // D3DTA_TEXTURE, modifiers in the high bits
            return false;
        };
        if (i > 0 && !tex[i].kind && t[kTssColorOp] > 1 && (reads_texture(1, 3) || reads_texture(5, 3))) break;
        std::memcpy(k.ff[i], v, sizeof(v));
        if (!tex[i].kind) PassMissingTexture(k.ff[i]);
        if (program || (t[kTssTextureTransformFlags] & 0x100)) k.proj |= 1u << i;  // D3DTTFF_PROJECTED
        if (t[kTssColorOp] <= 1) break;
    }
    return k;
}

constexpr uint32_t kMuxConstant = 3;
constexpr uint32_t kViewportOffsetConstant = 59;  // c-37: constants start at c-96

bool ReadsConstant(const std::vector<uint32_t>& prog, uint32_t reg) {
    for (size_t n = 0; n + 4 <= prog.size(); n += 4) {
        Tok k{&prog[n]};
        // an instruction reads at most one constant, through any of its operands
        bool constant = k.f(2, 26, 2) == kMuxConstant || k.f(2, 11, 2) == kMuxConstant || k.f(3, 28, 2) == kMuxConstant;
        bool relative = k.f(3, 1, 1);
        if (constant && !relative && k.f(1, 13, 8) == reg) return true;
    }
    return false;
}

bool ReadsViewportOffset(const std::vector<uint32_t>& prog) { return ReadsConstant(prog, kViewportOffsetConstant); }

bool CopiesTexcoords(const std::vector<uint32_t>& prog) {
    for (size_t n = 0; n + 4 <= prog.size(); n += 4) {
        Tok k{&prog[n]};
        uint32_t mac = k.f(1, 21, 4), ilu = k.f(1, 25, 3), omask = k.f(3, 12, 4);
        bool use_ilu = k.f(3, 2, 1) == 1;
        uint32_t o = k.f(3, 3, 8) & 0xF;
        bool writes = omask && k.f(3, 11, 1) && ((use_ilu && ilu) || (!use_ilu && mac && mac != 13));
        // an output write to oT0..oT3 (kOut 9 to 12) that reaches x or y (mask bits 3 and 2)
        if (writes && o >= 9 && o <= 12 && (omask & 0xC)) {
            // mov from an input, x from x and y from y, not negated: MAC's operand A, or ILU's C
            uint32_t op = use_ilu ? ilu : mac;
            uint32_t mux = use_ilu ? k.f(3, 28, 2) : k.f(2, 26, 2);
            uint32_t neg = use_ilu ? k.f(2, 10, 1) : k.f(1, 8, 1);
            uint32_t sx = use_ilu ? k.f(2, 8, 2) : k.f(1, 6, 2), sy = use_ilu ? k.f(2, 6, 2) : k.f(1, 4, 2);
            if (op != 1 || mux != 2 || neg || sx != 0 || sy != 1) return false;
        }
        if (k.f(3, 0, 1)) break;  // last instruction
    }
    return true;
}

// the program's instructions, reading its constants from cname
std::string ProgramBody(const std::vector<uint32_t>& prog, uint16_t regs, const char* cname) {
    std::string s;
    for (size_t n = 0; n + 4 <= prog.size(); n += 4) {
        Tok k{&prog[n]};
        uint32_t mac = k.f(1, 21, 4), ilu = k.f(1, 25, 3);
        if (mac || ilu) {
            std::string a = Src(k, 0, regs, cname), b = Src(k, 1, regs, cname), c = Src(k, 2, regs, cname);
            s += "    {\n";
            if (mac) s += "        precise float4 mr = " + MacExpr(mac, a, b, c) + ";\n";
            if (ilu) s += "        precise float4 ir = " + IluExpr(ilu, c) + ";\n";
            uint32_t r = k.f(3, 20, 4), omask = k.f(3, 12, 4);
            if (mac == 13) {
                s += "        a0 = mac_arl(mr.x);\n";
            } else if (mac) {
                uint32_t m = k.f(3, 24, 4);
                if (ilu && r == 1) m = 0;
                if (m && r <= 12) s += F("        %s.%s = mr.%s;\n", RegDest(r).c_str(), kMask[m], kMask[m]);
            }
            if (ilu) {
                uint32_t m = k.f(3, 16, 4), ir = mac ? 1 : r;
                if (m && ir <= 12) s += F("        %s.%s = ir.%s;\n", RegDest(ir).c_str(), kMask[m], kMask[m]);
            }
            bool use_ilu = k.f(3, 2, 1) == 1;
            if (omask && k.f(3, 11, 1) && ((use_ilu && ilu) || (!use_ilu && mac && mac != 13))) {
                uint32_t o = k.f(3, 3, 8) & 0xF;
                const char* src = use_ilu ? "ir" : "mr";
                if (kOut[o][0]) {
                    const char* msk = o == 5 ? kLanes[std::popcount(omask)] : kMask[omask];
                    s += F("        %s.%s = %s.%s;\n", kOut[o], msk, src, msk);
                }
            }
            s += "    }\n";
        }
        if (k.f(3, 0, 1)) break;  // last instruction
    }
    return s;
}

// Frame generation's motion: the program run again on the frame before's constants of the same draw (cp), its position
// taken to clip space as the epilogue takes this frame's (without the far edge pull out of screen passes, which a 3D
// draw never has), and through ShaderHooks::Position as o.pos is. The compiler keeps only what oPos needs.
std::string PrevPositionHlsl(const std::vector<uint32_t>& prog, uint16_t regs) {
    std::string s = "float4 PrevPos(VIn i) {\n";
    s += kVsPrologue;
    s += ProgramBody(prog, regs, "cp");
    s += "    return oPos;\n}\n";
    s += R"(float4 PrevClip(VIn i) {
    float4 p = PrevPos(i);
    p.w = magclamp(p.w);
    float4 q;
    q.x = p.x * 2 / surf.x - 1;
    q.y = 1 - p.y * 2 / surf.y;
    q.z = p.z * surf.z;
    q.xyz *= p.w;
    q.w = p.w;
    return q;
}
float4 HookPos(float4 pin) {
    VOut o = (VOut)0;
    float4 oT0 = 0;
    o.pos = pin;
)";
    s += Hooks().Position();
    s += "    return o.pos;\n}\n";
    return s;
}

std::string VertexProgramHlsl(const std::vector<uint32_t>& prog, uint16_t regs, uint64_t decode) {
    std::string s = Common();
    s += kVsHelpers;
    s += VsInputs(regs, decode);
#ifndef __ANDROID__
    // the motion's functions before main, after the input structs
    size_t entry = s.find("VOut main(");
    if (entry != std::string::npos) s.insert(entry, PrevPositionHlsl(prog, regs));
#endif
    s += kVsPrologue;
    s += ProgramBody(prog, regs, "c");
    s += VsEpilogue(true);
    return s;
}

FixedKey MakeFixedKey(const State& s) {
    FixedKey f;
    if (s.rs[kRsLighting]) {
        f.light = 1 | (s.rs[kRsSpecularEnable] ? 2 : 0) | (s.rs[kRsLocalViewer] ? 4 : 0) | (s.rs[kRsNormalizeNormals] ? 8 : 0);
        if (s.rs[kRsColorVertex])
            f.light |= (s.rs[kRsDiffuseMaterialSource] & 3) << 8 | (s.rs[kRsSpecularMaterialSource] & 3) << 10 |
                       (s.rs[kRsAmbientMaterialSource] & 3) << 12 | (s.rs[kRsEmissiveMaterialSource] & 3) << 14;
        for (int i = 0; i < 8; i++)
            if ((s.light_enable >> i) & 1) f.types |= (s.lights[size_t(i)][0] & 3) << (i * 2);
    }
    for (int t = 0; t < 4; t++) {
        uint32_t tci = s.tss[size_t(t)][kTssTexCoordIndex], ttf = s.tss[size_t(t)][kTssTextureTransformFlags];
        uint32_t set = tci & 3, size = 2;
        if (s.vs && (s.vs_handle & 1)) {
            uint32_t ty = s.vs->inputs[9 + set].type;
            size = ty == 0x16 ? 3 : std::min<uint32_t>(ty >> 4, 4);  // 0x16 NORMPACKED3
            if (ty >> 4 > 4) size = 3;
            if (!size) size = 2;
        } else {
            uint32_t code = (s.vs_handle >> (16 + set * 2)) & 3;
            size = code == 0 ? 2 : code == 1 ? 3 : code == 2 ? 4 : 1;
        }
        f.tex[t] = set | ((tci >> 16) & 7) << 4 | (ttf & 7) << 8 | (ttf & 0x100 ? 0x800 : 0) | size << 12;
    }
    return f;
}

// D3D's fixed function vertex pipeline: transform, lighting in view space, texture coordinate generation and transforms
std::string FixedVertexHlsl(uint16_t regs, bool pretransformed, uint64_t decode, const FixedKey& f) {
    std::string s = Common();
    s += kVsHelpers;
    s += VsInputs(regs, decode);
    s += kVsPrologue;
    auto v = [&](int r, const char* def) { return (regs >> r) & 1 ? F("i.v%d", r) : std::string(def); };
    bool lit = !pretransformed && (f.light & 1);
    if (pretransformed) {
        s += "    float4 rhw = " + v(0, "float4(0,0,0,1)") + ";\n";
        s += "    float w = rhw.w == 0 ? 1 : 1 / rhw.w;\n";
        s += "    oPos = float4(rhw.xy, rhw.z / surf.z, w);\n";
    } else {
        s += "    float4 ip = float4(" + v(0, "float4(0,0,0,1)") + ".xyz, 1);\n";
        s += "    float4 cp = mul(ip, wvp);\n";
        s += "    float3 vp = mul(ip, wv).xyz;\n";
        s += "    float3 vn = mul(float4(" + v(2, "float4(0,0,0,0)") + ".xyz, 0), wvn).xyz;\n";
        if (f.light & 8) s += "    vn = dot(vn, vn) > 0 ? normalize(vn) : vn;\n";
    }
    if (lit) {
        // D3DMCS: 1 the vertex diffuse, 2 its specular, else the material
        auto src = [&](int field, const char* mat) {
            uint32_t m = (f.light >> (8 + field * 2)) & 3;
            if (m == 1 && ((regs >> 3) & 1)) return std::string("i.v3");
            if (m == 2 && ((regs >> 4) & 1)) return std::string("i.v4");
            return std::string(mat);
        };
        s += "    float4 md = " + src(0, "mat[0]") + ", ms = " + src(1, "mat[2]") + ", ma = " + src(2, "mat[1]") +
             ", me = " + src(3, "mat[3]") + ";\n";
        s += "    float3 la = 0, ld = 0, ls = 0;\n";
        s += (f.light & 4) ? "    float3 eye = -normalize(vp);\n" : "    float3 eye = float3(0, 0, 1);\n";
        for (int n = 0; n < 8; n++) {
            uint32_t type = (f.types >> (n * 2)) & 3;  // D3DLIGHTTYPE: 1 point, 2 spot, 3 directional
            if (!type) continue;
            int b = n * 7;
            s += "    {\n        float3 l;\n        float att = 1;\n";
            if (type == 3) {
                s += F("        l = -normalize(lt[%d].xyz);\n", b + 4);
            } else {
                s += F("        float3 d = lt[%d].xyz - vp;\n        float dist = length(d);\n        l = d / max(dist, 1e-6);\n", b + 3);
                s += F("        att = dist > lt[%d].w ? 0 : 1 / max(lt[%d].x + lt[%d].y * dist + lt[%d].z * dist * dist, 1e-6);\n", b + 3, b + 5,
                       b + 5, b + 5);
                if (type == 2) {
                    s += F("        float rho = dot(-l, normalize(lt[%d].xyz));\n", b + 4);
                    s += F("        att *= rho > lt[%d].x ? 1 : rho <= lt[%d].y ? 0 : pow(saturate((rho - lt[%d].y) / (lt[%d].x - lt[%d].y)), lt[%d].w);\n",
                           b + 6, b + 6, b + 6, b + 6, b + 6, b + 4);
                }
            }
            s += F("        la += att * lt[%d].rgb;\n", b + 2);
            s += "        float nl = dot(vn, l);\n";
            s += F("        ld += att * lt[%d].rgb * max(nl, 0);\n", b);
            if (f.light & 2) s += F("        if (nl > 0) ls += att * lt[%d].rgb * pow(max(dot(vn, normalize(l + eye)), 0), mat[4].x);\n", b + 1);
            s += "    }\n";
        }
        s += "    oD0 = float4(me.rgb + ma.rgb * (amb.rgb + la) + md.rgb * ld, md.a);\n";
        s += (f.light & 2) ? "    oD1 = float4(ms.rgb * ls, ms.a);\n" : "    oD1 = float4(0,0,0,0);\n";
    } else {
        s += "    oD0 = " + v(3, "float4(1,1,1,1)") + ";\n";
        s += "    oD1 = " + v(4, "float4(0,0,0,0)") + ";\n";
    }
    s += "    oB0 = oD0;\n    oB1 = oD1;\n";
    for (int t = 0; t < 4; t++) {
        uint32_t u = f.tex[t], set = u & 3, gen = (u >> 4) & 7, count = (u >> 8) & 7, size = (u >> 12) & 7;
        std::string in = v(9 + int(set), "float4(0,0,0,1)");
        if (pretransformed || (!gen && !count)) {
            s += F("    oT%d = %s;\n", t, in.c_str());
            continue;
        }
        // generated coordinates are view space; a transform takes the set with 1 after its last element
        std::string c = gen == 1 ? "float4(vn, 1)" : gen == 2 ? "float4(vp, 1)" : gen == 3 ? "float4(reflect(normalize(vp), vn), 1)"
                        : size == 1 ? F("float4(%s.x, 1, 0, 0)", in.c_str()) : size == 2 ? F("float4(%s.xy, 1, 0)", in.c_str())
                        : size == 3 ? F("float4(%s.xyz, 1)", in.c_str()) : in;
        if (!count) {
            s += F("    oT%d = %s;\n", t, c.c_str());
            continue;
        }
        s += F("    {\n        float4 tc = mul(%s, tm[%d]);\n", c.c_str(), t);
        if (u & 0x800) s += F("        oT%d = float4(tc.xy, tc.z, tc[%u]);\n", t, std::min<uint32_t>(count, 4) - 1);
        else s += F("        oT%d = float4(tc.xy, %s, 1);\n", t, count >= 3 ? "tc.z" : "0");
        s += "    }\n";
    }
    s += "    oFog = " + v(5, "float4(0,0,0,0)") + ";\n";
    if (pretransformed) return s + VsEpilogue();
    s += R"(
    VOut o;
    // like the pretransformed ones, target sized passes end half a console pixel short of the far edges
    float2 sp = (float2(cp.x, -cp.y) / cp.w + 1) * 0.5 * surf.xy;
    if (surf.w == 0 && abs(sp.x - (surf.x - 0.5)) < 1e-3) cp.x = cp.w;
    if (surf.w == 0 && abs(sp.y - (surf.y - 0.5)) < 1e-3) cp.y = -cp.w;
    o.pos = cp;
@POSITION@    o.d0 = saturate(oD0);
    o.d1 = saturate(oD1);
    o.b0 = o.d0;
    o.b1 = o.d1;
    o.tc0 = oT0;
    o.tc1 = oT1;
    o.tc2 = oT2;
    o.tc3 = oT3;
    o.fogamt = fogfactor(abs(cp.z));
)"
#ifndef __ANDROID__
R"(    o.mvc = o.pos;
    o.mvp = 0;
)"
#endif
R"(    o.pos.xy += motion.zw * o.pos.w;
@RTCAPTURE@    return o;
}
)";
    s = WithPosition(s);
    size_t rt = s.find("@RTCAPTURE@");
    if (rt != std::string::npos) s.replace(rt, 11, kRtCapture);
    return s;
}

namespace {

// a texture pack's material maps over a surface's colour, as the PCSX2 build of the game this pack was made for shades
// them: the normal map's relief against the surface's own facing (from the view space position: across and down the
// screen, toward the viewer) under a fixed light, darkened by its occlusion, with the specular its roughness and
// metalness give; flat relief leaves the colour as it was
const char* kMaterialHlsl = R"(Texture2D mtn : register(t4);
Texture2D mto : register(t5);
// how deep a point of the surface lies, 0 the top to 1 the deepest: its height map's (the ORM map's alpha, 1 the top)
// where the material has one, else its occlusion read as depth (crevices, a wall's mortar: deepest at the maps' floor
// of 0.56)
float sinking(float4 orm, bool height) { return height ? 1 - orm.a : saturate((1 - orm.r) * 2.27); }
// parallax: the view ray marched into the surface, how deep each point lies (sinking), and the coordinates moved to
// where the ray meets it. mtl[1].w the deepest point's depth as a part of the picture (0: off), times the material's
// own, |mtl[2].w|, negative where it has a height map; mtl[0].x the most layers, taken edge-on. dx, dy: the
// coordinates' steps across a pixel, as they are without it
float2 parallax(float2 uv, float2 dx, float2 dy, float4 pos, SamplerState sm) {
    float depth = mtl[1].w * abs(mtl[2].w);
    bool height = mtl[2].w < 0;
    float2 ndc = pos.xy / mtl[3].zw * 2 - 1;
    float3 p = float3(ndc * mtl[3].xy * pos.w, -pos.w);
    float3 dpdx = ddx(p), dpdy = ddy(p);
    float det = dx.x * dy.y - dx.y * dy.x;
    if (depth <= 0 || abs(det) < 1e-24) return uv;
    float3 t = (dpdx * dy.y - dpdy * dx.y) / det;  // the surface along u, a unit of it
    float3 b = (dpdy * dx.x - dpdx * dy.x) / det;  // and along v
    float3 n = cross(t, b);
    float nn = dot(n, n);
    if (nn < 1e-30) return uv;
    n *= rsqrt(nn);
    float3 v = normalize(-p);
    float vn = dot(v, n);
    if (vn < 0) { n = -n; vn = -vn; }
    // the view along the surface as moves of u and v: t x + b y = v - vn n
    float3 vp = v - vn * n;
    float tt = dot(t, t), tb = dot(t, b), bb = dot(b, b), g = tt * bb - tb * tb;
    if (g < 1e-30) return uv;
    float2 along = float2(bb * dot(vp, t) - tb * dot(vp, b), tt * dot(vp, b) - tb * dot(vp, t)) / g;
    // from the top to the deepest point the ray moves this far across the picture
    float2 reach = -along / max(vn, 0.25) * depth * sqrt(sqrt(tt * bb));
    int layers = (int)clamp(lerp(mtl[0].x * 0.5, mtl[0].x, 1 - vn), 4, 48);
    float layer = 1.0 / layers;
    float2 duv = reach * layer, at = uv;
    // the march's reads at one level of detail, the pixel's footprint by its area: trilinear, without the sampler's
    // anisotropic taps (up to 16 a read, each step) or upscaling's texture bias (motion.y, which SampleLevel takes too)
    float mw, mh;
    mto.GetDimensions(mw, mh);
    float2 fx = dx * float2(mw, mh), fy = dy * float2(mw, mh);
    float lod = 0.25 * log2(max(dot(fx, fx) * dot(fy, fy), 1e-12)) - motion.y;
    float ray = 0, here = sinking(mto.SampleLevel(sm, at, lod), height), before = here;
    [loop] for (int k = 0; k < layers && ray < here; k++) {
        before = here;
        at += duv;
        ray += layer;
        here = sinking(mto.SampleLevel(sm, at, lod), height);
    }
    if (ray <= 0) return uv;
    // between the last two layers, where the ray went under the surface
    float above = before - (ray - layer), below = ray - here;
    return lerp(at - duv, at, saturate(above / max(above + below, 1e-5)));
}
// uv as parallax moved it; duvdx, duvdy as they are without it
float3 material(float3 base, float2 uv, float2 duvdx, float2 duvdy, float4 pos, SamplerState sm) {
    float2 ndc = pos.xy / mtl[3].zw * 2 - 1;
    float3 p = float3(ndc * mtl[3].xy * pos.w, -pos.w);
    float3 dpdx = ddx(p), dpdy = ddy(p);
    float det = duvdx.x * duvdy.y - duvdx.y * duvdy.x;
    float3 gn = cross(dpdx, dpdy);
    gn = dot(gn, gn) < 1e-30 ? float3(0, 0, 1) : normalize(gn);
    if (gn.z < 0) gn = -gn;
    float3 tg = float3(1, 0, 0);
    if (abs(det) > 1e-24) {
        tg = (dpdx * duvdy.y - dpdy * duvdx.y) / det;
        tg -= gn * dot(gn, tg);
        tg = dot(tg, tg) < 1e-30 ? float3(1, 0, 0) : normalize(tg);
    }
    float3 bt = normalize(cross(gn, tg));
    if (det < 0) bt = -bt;
    float3 tn = mtn.SampleGrad(sm, uv, duvdx, duvdy).rgb * 2 - 1;
    float3 orm = mto.SampleGrad(sm, uv, duvdx, duvdy).rgb;
    float variance = saturate(1 - length(tn));  // a filtered normal shorter than one: detail folded into roughness
    tn.xy *= mtl[0].y;
    tn.z = max(tn.z, 0.05);
    tn = normalize(tn);
    float3 n = normalize(tg * tn.x + bt * tn.y + gn * tn.z);
    float ao = lerp(1, orm.r, mtl[2].x);
    float rough0 = clamp(orm.g + mtl[2].y, 0.04, 1);
    float rough = sqrt(clamp(rough0 * rough0 + variance * 0.5, 0.0016, 1));
    float metal = saturate(orm.b * mtl[2].z);
    float3 l = normalize(mtl[1].xyz), v = float3(0, 0, 1), h = normalize(l + v);
    float flat_ndotl = max(dot(gn, l), 0), ndotl = max(dot(n, l), 0);
    float flat_ndoth = max(dot(gn, h), 0), ndoth = max(dot(n, h), 0);
    float vdoth = max(dot(v, h), 0);
    float delta = ndotl - flat_ndotl;
    float relief = delta < 0 ? delta * 2.25 : delta * 1.10;
    float diffuse = clamp(1 + relief * mtl[0].z, 0.42, 1.28);
    float3 c = saturate(base);
    float3 shaded = c * diffuse * ao * (1 - metal * 0.22);
    float3 f0 = lerp(float3(0.04, 0.04, 0.04), c, metal);
    float3 fresnel = f0 + (1 - f0) * pow(1 - vdoth, 5);
    float power = lerp(96, 7, rough);
    float lobe = pow(ndoth, power), flat_lobe = pow(flat_ndoth, power);
    float spec = (max(lobe - flat_lobe, 0) + max(ndoth - flat_ndoth, 0) * (1 - rough) * 0.35) * (1 - rough * 0.55);
    return saturate(shaded + fresnel * spec * mtl[0].w * mtl[0].z);
}
)";

// the material stage's texture coordinates, as its fetch takes them
std::string MaterialUv(const PixelKey& k, int i) {
    const TexInfo& t = k.tex[i];
    if (k.programmable) {
        uint32_t mode = (k.tex_modes >> (i * 5)) & 0x1F;
        return TexUv(t, i, mode == 1 ? F("tc%d.xy / tc%d.w", i, i) : F("(tc%d.xyz / tc%d.w).xy", i, i));
    }
    return TexUv(t, i, (k.proj >> i) & 1 ? F("tc%d.xy / tc%d.w", i, i) : F("tc%d.xy", i));
}

}  // namespace

bool MaterialFits(const PixelKey& k, int i) {
    const TexInfo& t = k.tex[i];
    if (t.kind != kTex2D || t.rect || t.shadow || (t.flags & (kTexWide | kTexTarget))) return false;
    if (k.programmable) {
        uint32_t mode = (k.tex_modes >> (i * 5)) & 0x1F;
        return mode == 1 || mode == 2;
    }
    for (int j = 0; j <= i; j++)
        if (k.ff[j][0] <= 1) return false;
    return true;
}

std::string PixelHlsl(const PixelKey& k) {
    std::string s = Common();
    int mat = int(k.material) - 1;
    if (mat >= 0 || k.texbox) {
        // the material's constants past the others, its maps after the four stages; then a quad's texel box
        const char* end = "    float4 motion;\n};\n";
        size_t at = s.find(end);
        if (at != std::string::npos)
            s.replace(at, std::strlen(end),
                      k.texbox ? "    float4 motion;\n    float4 mtl[4];\n    float4 uvbox;\n};\n"
                               : "    float4 motion;\n    float4 mtl[4];\n};\n");
    }
    s += PsHelpers();
    if (k.texbox)
        s += R"(// stage 0 read only inside its quad's coordinates (uvbox, divided by w as they are), half a texel in: upscaled, the
// quad's edge pixels sample past the console's last pixel centre and blend in the sheet's next picture
float2 texbox(float2 uv, float w, float2 size, bool rect) {
    float2 a = uvbox.xy / w, b = uvbox.zw / w;
    if (rect) { a /= size; b /= size; }
    float2 lo = min(a, b), hi = max(a, b), h = 0.5 / size, mid = (lo + hi) * 0.5;
    return clamp(uv, min(lo + h, mid), max(hi - h, mid));
}
)";
    for (int i = 0; i < 4; i++) {
        const TexInfo& t = k.tex[i];
        const char* type = t.shadow ? "Texture2D<float>" : t.kind == kTex3D ? "Texture3D" : t.kind == kTexCube ? "TextureCube" : "Texture2D";
        s += F("%s tex%d : register(t%d);\n%s smp%d : register(s%d);\n", type, i, i, t.shadow ? "SamplerComparisonState" : "SamplerState", i, i);
    }
    if (mat >= 0) s += kMaterialHlsl;
    if (k.motion) {
        // frame generation's motion of the pixel, to where it was in the frame before, in the frame's UV units;
        // kNoMotion where the draw has no frame before: the renderer's camera motion from depth instead
        s += R"(float4 MotionOut(VOut i) {
    if (i.mvp.z < 0.5 || abs(i.mvp.w) < 1e-6 || abs(i.mvc.w) < 1e-6) return float4(1024, 1024, 0, 0);
    float2 here = i.mvc.xy / i.mvc.w, before = i.mvp.xy / i.mvp.w;
    return float4((before - here) * float2(0.5, -0.5), 0, 0);
}
)";
    }
    // the outputs past the colour: frame generation's motion (SV_Target1), anti-aliasing's reactive mask (SV_Target2)
    std::string outs;
    if (k.motion) outs += ", out float4 mvout : SV_Target1";
    if (k.reactive) outs += ", out float rxout : SV_Target2";
    s += "float4 main(VOut i, bool front : SV_IsFrontFace" + outs + (outs.empty() ? ") : SV_Target {\n" : ") : SV_Target0 {\n");
    if (k.motion) s += "    mvout = MotionOut(i);\n";
    s += "    float4 tc0 = i.tc0, tc1 = i.tc1, tc2 = i.tc2, tc3 = i.tc3;\n";
    s += k.two_sided ? "    float4 v0 = front ? i.d0 : i.b0, v1 = front ? i.d1 : i.b1;\n" : "    float4 v0 = i.d0, v1 = i.d1;\n";
    s += "    if (misc.w == 0) v1 = float4(0, 0, 0, 1);\n";
    s += "    float4 fogin = float4(fogcolor.rgb, saturate(i.fogamt));\n";
    std::string body;
    if (k.programmable) {
        for (int i = 0; i < 4; i++) {
            body += TexFetch(k, i);
            body += Hooks().Fetched(k.tex[i], i);
        }
        for (int i = 0; i < 4; i++) {
            uint32_t m = (k.tex_modes >> (i * 5)) & 0x1F;
            bool fetch = m && m != 4 && m != 5 && m != 0xA && m != 0x11;
            if (fetch && ((k.alphakill >> i) & 1)) body += F("if (t%d.a == 0) discard;\n", i);
        }
        uint32_t mode0 = k.tex_modes & 0x1F;
        body += F("float4 r0 = float4(0, 0, 0, %s), r1 = 0;\n", mode0 ? "t0.a" : "1");
        Ps ps{k};
        uint32_t n = std::min<uint32_t>(k.count & 0xFF, 8);
        for (uint32_t st = 0; st < n; st++) {
            ps.stage = int(st);
            ps.code += "{\n";
            std::string wc = ps.Stage(k.rgb_in[st], k.rgb_out[st], false);
            std::string wa = ps.Stage(k.a_in[st], k.a_out[st], true);
            ps.code += wc + wa + "}\n";
        }
        body += ps.code;
        if (mat >= 0) body += F("r0.rgb = material(r0.rgb, puv, mdx, mdy, i.pos, smp%d);\n", mat);
        if (k.final_abcd || k.final_efg) {
            ps.stage = 8;
            uint32_t fl = k.final_efg & 0xFF;
            ps.clamp_sum = fl & 0x80;
            ps.inv_v1 = fl & 0x40;
            ps.inv_r0 = fl & 0x20;
            ps.e = ps.Input(ParseIn(k.final_efg >> 24), false);
            ps.f = ps.Input(ParseIn((k.final_efg >> 16) & 0xFF), false);
            std::string A = ps.Input(ParseIn(k.final_abcd >> 24), false), B = ps.Input(ParseIn((k.final_abcd >> 16) & 0xFF), false),
                        C = ps.Input(ParseIn((k.final_abcd >> 8) & 0xFF), false), D = ps.Input(ParseIn(k.final_abcd & 0xFF), false),
                        G = ps.Input(ParseIn((k.final_efg >> 8) & 0xFF), true);
            body += "float4 outc;\n";
            body += "outc.rgb = " + D + " + lerp(" + C + ", " + B + ", " + A + ");\n";
            body += "outc.a = " + G + ";\n";
        } else {
            body += "float4 outc = float4(lerp(fogin.rgb, r0.rgb, fogin.a), r0.a);\n";
        }
    } else {
        body += "float4 cur = v0, tmp = 0;\n";
        for (int i = 0; i < 4; i++) {
            const uint32_t* f = k.ff[i];
            if (f[0] <= 1) break;
            const TexInfo& t = k.tex[i];
            bool proj = (k.proj >> i) & 1;
            const char* box_w = k.texbox && i == 0 ? (proj ? "tc0.w" : "1") : nullptr;
            std::string uv = TexUv(t, i, proj ? F("tc%d.xy / tc%d.w", i, i) : F("tc%d.xy", i), box_w);
            std::string cube;
            if (t.shadow)
                body += F("float4 t%d = shadow(tex%d, smp%d, ", i, i, i) + TexUv(t, i, F("tc%d.xy / tc%d.w", i, i)) +
                        F(", tc%d.z / tc%d.w, %d, i.pos.xy, texsize[%d].xy);\n", i, i, t.shadow, i);
            else if (t.kind == kTex2D && (t.flags & kTexWide))
                body += F("float4 t%d = spreadtap(tex%d, smp%d, %s, spread[%d], %s);\n", i, i, i, uv.c_str(), i, Hooks().ReadScale(t).c_str());
            else if (i == int(k.material) - 1) body += MaterialFetch(i, uv);
            else if (t.kind == kTex2D) body += F("float4 t%d = tex%d.Sample(smp%d, %s);\n", i, i, i, uv.c_str());
            else if (t.kind == kTexCube && !(cube = Hooks().Cube(t, i, F("tc%d.xyz", i))).empty()) body += F("float4 t%d = %s;\n", i, cube.c_str());
            else if (t.kind) body += F("float4 t%d = tex%d.Sample(smp%d, tc%d.xyz);\n", i, i, i, i);
            else body += F("float4 t%d = float4(1,1,1,1);\n", i);
            body += Hooks().Fetched(t, i);
            if ((k.alphakill >> i) & 1) body += F("if (t%d.a == 0) discard;\n", i);
            const char* dst = f[8] == 5 ? "tmp" : "cur";
            std::string rgb = FfOp(f[0], FfArg(f[1], i, false), FfArg(f[2], i, false), FfArg(f[3], i, false), i, false);
            std::string a = f[4] <= 1 ? std::string("cur.a") : FfOp(f[4], FfArg(f[5], i, true), FfArg(f[6], i, true), FfArg(f[7], i, true), i, true);
            body += F("{ float3 c3 = saturate(%s); float c1 = saturate(%s); %s = float4(c3, c1); }\n", rgb.c_str(), a.c_str(), dst);
        }
        if (mat >= 0) body += F("cur.rgb = material(cur.rgb, puv, mdx, mdy, i.pos, smp%d);\n", mat);
        body += "cur.rgb = saturate(cur.rgb + v1.rgb);\n";
        body += "float4 outc = float4(lerp(fogin.rgb, cur.rgb, fogin.a), cur.a);\n";
    }
    if (k.alphatest) body += "alphatest(outc.a);\n";
    body += Hooks().Output();
    // the effect's share of what the pixel shows, at most 0.9 (AMD: the upscaler keeps a little history even there)
    if (k.reactive == 1) body += "rxout = min(outc.a, 0.9);\n";
    if (k.reactive == 2) body += "rxout = min(max(outc.r, max(outc.g, outc.b)), 0.9);\n";
    if (k.reactive == 3) body += "rxout = min(max(outc.r, max(outc.g, outc.b)) * outc.a, 0.9);\n";
    body += "return outc;\n}\n";
    s += body;
    return k.quad ? QuadHlsl(std::move(s)) : s;
}

// the vertex shader hands on the pixel position as drawn and as pulled (e), the pixel shader takes the stretch back out
std::string QuadHlsl(std::string s) {
    auto sub = [&](const char* at, const char* before, const char* after) {
        size_t p = s.find(at);
        if (p != std::string::npos) s.replace(p, std::strlen(at), std::string(before) + at + after);
    };
    sub("    float fogamt : TEXCOORD4;\n", "", "    noperspective float4 e : TEXCOORD5;\n");
    sub("    if (surf.w == 0 && abs(p.x - (surf.x - 0.5)) < 1e-3) p.x = surf.x;\n", "    o.e.xy = p.xy;\n", "");
    sub("    if (surf.w == 0 && abs(p.y - (surf.y - 0.5)) < 1e-3) p.y = surf.y;\n", "", "    o.e.zw = p.xy;\n");
    sub("    o.pos = cp;\n", "    o.e = float4(sp, (float2(cp.x, -cp.y) / cp.w + 1) * 0.5 * surf.xy);\n", "");
    sub("float4 main(VOut i, bool front", R"(float4 unstretch(float4 t, float4 e) {
    float2 d = e.zw - e.xy, g = float2(ddx(e.x), ddy(e.y));
    return t + (d.x ? d.x / g.x * ddx(t) : 0) + (d.y ? d.y / g.y * ddy(t) : 0);
}
)", "");
    for (int i = 0; i < 4; i++) sub(F("i.tc%d", i).c_str(), "unstretch(", ", i.e)");
    return s;
}

namespace {

// A w buffer's reversed depth comes from the clip w alone, zparams.x * (zparams.y / w - 1), exact where z / w had
// already rounded away; a z buffer's is 1 - z. zparams.z is the polygon offset in the units of each
const char* kReversedClipHlsl = R"(float ReversedClip(float z, float w) {
    if (zparams.x != 0) return zparams.x * (zparams.y - (w + zparams.z));
    return w - z - zparams.z * w;
}
)";

bool InsertAfter(std::string& s, const char* at, const char* text) {
    size_t p = s.find(at);
    if (p == std::string::npos) return false;
    s.insert(p + std::strlen(at), text);
    return true;
}

}  // namespace

std::string ReversedDepthHlsl(std::string s) {
    size_t entry = s.find("VOut main(");
    if (entry != std::string::npos) s.insert(entry, kReversedClipHlsl);
    InsertAfter(s, "    o.pos = q;\n", "    o.pos.z = ReversedClip(q.z, q.w);\n");
    InsertAfter(s, "    o.pos = cp;\n", "    o.pos.z = ReversedClip(cp.z, cp.w);\n");
    return s;
}

// a point covers its console pixel, several host pixels on an upscaled target: drawn as a quad of that size
std::string PointHlsl() {
    return Common() + R"(
[maxvertexcount(4)]
void main(point VOut v[1], inout TriangleStream<VOut> strip) {
    float2 half_pixel = v[0].pos.w / surf.xy;  // in clip units, the target spans 2 across surf.xy pixels
    // a target is read half a host pixel back (surf.w host pixels per console pixel): left and top reach that far
    float2 back = (0.5 - 1e-3) * 2 * half_pixel / surf.w;
    for (int i = 0; i < 4; i++) {
        VOut o = v[0];
        o.pos.x += (i & 1) ? half_pixel.x : -half_pixel.x - back.x;
        o.pos.y += (i & 2) ? -half_pixel.y : half_pixel.y + back.y;
        strip.Append(o);
    }
}
)";
}

}  // namespace gpu
