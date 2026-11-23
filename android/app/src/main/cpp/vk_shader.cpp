// HLSL to SPIR-V for the Android native Vulkan 1.1 renderer via glslang, with on-disk caching.
#include "vk_shader.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <regex>

#include <SPIRV/GlslangToSpv.h>
#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>

#include "core/log.h"

namespace vkshader {

namespace {

std::filesystem::path g_dir;
std::mutex g_lock;
bool g_clip_distance = true;
float g_max_point_size = 1.0f;
// Bumped whenever HLSL->SPIR-V adaptation changes so stale cache entries are ignored.
constexpr uint32_t kCacheVersion = 1;

struct Param {
    std::string type;
    std::string name;
    bool opaque = false;
};

bool IsIdentChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

std::string Trim(std::string_view sv) {
    size_t a = 0, b = sv.size();
    while (a < b && std::isspace(static_cast<unsigned char>(sv[a]))) a++;
    while (b > a && std::isspace(static_cast<unsigned char>(sv[b - 1]))) b--;
    return std::string(sv.substr(a, b - a));
}

bool IsOpaqueType(const std::string& type) {
    return type.find("Texture") != std::string::npos || type.find("Sampler") != std::string::npos;
}

std::vector<Param> ParseParams(const std::string& list) {
    std::vector<Param> out;
    size_t start = 0;
    int angle = 0;
    for (size_t i = 0; i <= list.size(); i++) {
        char c = i < list.size() ? list[i] : ',';
        if (c == '<') angle++;
        else if (c == '>' && angle > 0) angle--;
        else if (c == ',' && angle == 0) {
            std::string item = Trim(std::string_view(list).substr(start, i - start));
            start = i + 1;
            if (item.empty()) continue;
            size_t end = item.size();
            size_t id_start = end;
            while (id_start > 0 && IsIdentChar(item[id_start - 1])) id_start--;
            if (id_start == end) continue;
            Param p;
            p.name = item.substr(id_start, end - id_start);
            p.type = Trim(std::string_view(item).substr(0, id_start));
            p.opaque = IsOpaqueType(p.type);
            out.push_back(std::move(p));
        }
    }
    return out;
}

size_t FindMatchingBrace(const std::string& s, size_t open) {
    int depth = 0;
    for (size_t i = open; i < s.size(); i++) {
        if (s[i] == '{') depth++;
        else if (s[i] == '}' && --depth == 0) return i + 1;
    }
    return std::string::npos;
}

// Split balanced comma-separated call arguments starting right after '('.
// Sets close_pos to the index of the closing ')'.
bool SplitCallArgs(const std::string& s, size_t args_start, std::vector<std::string>& args, size_t& close_pos) {
    args.clear();
    int paren = 0, bracket = 0;
    size_t start = args_start;
    for (size_t i = args_start; i < s.size(); i++) {
        char c = s[i];
        if (c == '(') paren++;
        else if (c == ')') {
            if (paren == 0 && bracket == 0) {
                std::string arg = Trim(std::string_view(s).substr(start, i - start));
                if (!arg.empty() || !args.empty()) args.push_back(std::move(arg));
                close_pos = i;
                return true;
            }
            if (paren > 0) paren--;
        } else if (c == '[') bracket++;
        else if (c == ']' && bracket > 0) bracket--;
        else if (c == ',' && paren == 0 && bracket == 0) {
            args.push_back(Trim(std::string_view(s).substr(start, i - start)));
            start = i + 1;
        }
    }
    return false;
}

std::string ReplaceIdent(const std::string& src, const std::string& from, const std::string& to) {
    std::string out;
    out.reserve(src.size());
    size_t pos = 0;
    while (pos < src.size()) {
        size_t hit = src.find(from, pos);
        if (hit == std::string::npos) {
            out.append(src, pos, std::string::npos);
            break;
        }
        bool left_ok = hit == 0 || !IsIdentChar(src[hit - 1]);
        bool right_ok = hit + from.size() == src.size() || !IsIdentChar(src[hit + from.size()]);
        if (left_ok && right_ok) {
            out.append(src, pos, hit - pos);
            out.append(to);
            pos = hit + from.size();
        } else {
            out.append(src, pos, hit + 1 - pos);
            pos = hit + 1;
        }
    }
    return out;
}

// Without an optimizer pass, glslang passes Texture*/Sampler* function parameters through
// function-scope variables (OpStore to opaque type, VUID 06924). Adreno tolerates it; ARM Mali
// fails compilation or samples garbage. Inline every helper that takes Texture*/Sampler*
// parameters (at any argument position: spreadtap, shadow, parallax, material) per call site
// so the specialized copy references the global texture/sampler directly.
std::string Specialize(const std::string& hlsl) {
    static const std::regex def(R"(\n(\w[\w<>]*)[ \t]+(\w+)\(([^()]*)\)\s*\{)");
    std::string s = hlsl;
    std::string made;
    size_t from = 0;
    for (std::smatch m; std::regex_search(s.cbegin() + long(from), s.cend(), m, def);) {
        size_t start = from + size_t(m.position(0));
        size_t open = start + size_t(m.length(0)) - 1;
        size_t end = FindMatchingBrace(s, open);
        std::vector<Param> params = ParseParams(m[3].str());
        bool has_opaque = false;
        for (const Param& p : params) has_opaque |= p.opaque;
        if (!has_opaque || end == std::string::npos) {
            from = open;
            continue;
        }

        std::string ret_type = m[1].str();
        std::string name = m[2].str();
        std::string body = s.substr(open, end - open);
        std::string kept_params;
        for (const Param& p : params) {
            if (p.opaque) continue;
            if (!kept_params.empty()) kept_params += ", ";
            kept_params += p.type + " " + p.name;
        }
        s.erase(start, end - start);

        // Rewrite every call site `name(...)` across both the shader body and previously specialized helpers.
        auto rewrite_calls = [&](std::string& target) {
            std::string out;
            out.reserve(target.size());
            size_t pos = 0;
            while (pos < target.size()) {
                size_t hit = target.find(name, pos);
                if (hit == std::string::npos) {
                    out.append(target, pos, std::string::npos);
                    break;
                }
                size_t after = hit + name.size();
                bool left_ok = hit == 0 || !IsIdentChar(target[hit - 1]);
                size_t lparen = after;
                while (lparen < target.size() && std::isspace(static_cast<unsigned char>(target[lparen]))) lparen++;
                if (!left_ok || lparen >= target.size() || target[lparen] != '(') {
                    out.append(target, pos, after - pos);
                    pos = after;
                    continue;
                }
                std::vector<std::string> args;
                size_t close = 0;
                if (!SplitCallArgs(target, lparen + 1, args, close) || args.size() != params.size()) {
                    out.append(target, pos, lparen + 1 - pos);
                    pos = lparen + 1;
                    continue;
                }
                std::string spec = name;
                std::string copy = body;
                std::string kept_args;
                bool valid_opaque = true;
                for (size_t i = 0; i < params.size(); i++) {
                    if (params[i].opaque) {
                        if (args[i].empty()) {
                            valid_opaque = false;
                            break;
                        }
                        spec += "_" + args[i];
                        copy = ReplaceIdent(copy, params[i].name, args[i]);
                    } else {
                        if (!kept_args.empty()) kept_args += ", ";
                        kept_args += args[i];
                    }
                }
                if (!valid_opaque) {
                    out.append(target, pos, lparen + 1 - pos);
                    pos = lparen + 1;
                    continue;
                }
                std::string decl = "\n" + ret_type + " " + spec + "(" + kept_params + ") " + copy + "\n";
                if (made.find(decl) == std::string::npos) made += decl;
                out.append(target, pos, hit - pos);
                out += spec + "(" + kept_args + ")";
                pos = close + 1;
            }
            target = std::move(out);
        };

        rewrite_calls(s);
        // Also handle opaque helpers calling another opaque helper if ever introduced.
        if (!made.empty()) rewrite_calls(made);
    }

    if (made.empty()) return hlsl;
    // Place specialized helpers after all Texture*/Sampler* declarations and right before main().
    size_t main_pos = s.rfind("\nfloat4 main(");
    if (main_pos == std::string::npos) return hlsl;
    return s.insert(main_pos, made);
}

std::string Adapt(const std::string& hlsl, Stage stage) {
    std::string s = Specialize(hlsl);
    for (size_t at; (at = s.find("1.#INF")) != std::string::npos;) s.replace(at, 6, "asfloat(0x7F800000u)");

    // Mali-G57 crashes at pipeline creation on unused SV_ClipDistance0 inputs or when unsupported.
    if (stage == Stage::Fragment || !g_clip_distance) {
        static const std::regex clip(R"(\n[ \t]*float behind : SV_ClipDistance0;)");
        s = std::regex_replace(s, clip, "");
    }
    if (stage == Stage::Vertex && !g_clip_distance) {
        static const std::regex write(R"(\n[ \t]*o\.behind = [^;]*;)");
        s = std::regex_replace(s, write, "");
    }

    // Emit PointSize on game vertex shaders so point primitives scale cleanly without geometry shaders
    // and never leave gl_PointSize undefined on Mali drivers lacking VK_KHR_maintenance5.
    if (stage == Stage::Vertex && g_max_point_size > 0.0f) {
        if (s.find("float fogamt : TEXCOORD4;") != std::string::npos) {
            static const std::regex member(R"((\n[ \t]*)(float fogamt : TEXCOORD4;))");
            static const std::regex write(R"((\n[ \t]*)(o\.fogamt = [^;]*;))");
            char psize_stmt[128];
            std::snprintf(psize_stmt, sizeof(psize_stmt),
                          "$1$2$1o.psize = clamp(max(1.0, surf.w), 1.0, %.1f);", g_max_point_size);
            s = std::regex_replace(s, member, "$1$2$1[[vk::builtin(\"PointSize\")]] float psize : PSIZE;");
            s = std::regex_replace(s, write, psize_stmt);
        }
    }

    // Bind vertex inputs to their Xbox register index (ATTRn -> [[vk::location(n)]]).
    static const std::regex attr(R"((\n[ \t]*)(\w+ v\d+ : ATTR(\d+);))");
    return std::regex_replace(s, attr, "$1[[vk::location($3)]] $2");
}

std::filesystem::path CachePath(uint64_t hash) {
    char name[64];
    std::snprintf(name, sizeof(name), "%016llx_v%u%s_p%u.spv", (unsigned long long)hash, kCacheVersion,
                  g_clip_distance ? "" : "_nc", unsigned(g_max_point_size + 0.5f));
    return g_dir / name;
}

bool Load(uint64_t hash, std::vector<uint32_t>& spirv) {
    if (g_dir.empty()) return false;
    std::ifstream f(CachePath(hash), std::ios::binary | std::ios::ate);
    if (!f) return false;
    std::streamsize n = f.tellg();
    if (n <= 0 || (n % 4) != 0) return false;
    spirv.resize(size_t(n) / 4);
    f.seekg(0);
    return bool(f.read(reinterpret_cast<char*>(spirv.data()), n));
}

void Store(uint64_t hash, const std::vector<uint32_t>& spirv) {
    if (g_dir.empty()) return;
    std::filesystem::path p = CachePath(hash);
    std::filesystem::path tmp = p;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary);
        if (!f) return;
        f.write(reinterpret_cast<const char*>(spirv.data()), std::streamsize(spirv.size() * 4));
    }
    std::error_code ec;
    std::filesystem::rename(tmp, p, ec);
}

}  // namespace

void Init(const std::filesystem::path& cache_dir, bool clip_distance, float max_point_size) {
    std::lock_guard lock(g_lock);
    static bool initialized = false;
    if (!initialized) {
        glslang::InitializeProcess();
        initialized = true;
    }
    g_clip_distance = clip_distance;
    g_max_point_size = std::clamp(max_point_size, 0.0f, 64.0f);
    std::error_code ec;
    std::filesystem::create_directories(cache_dir, ec);
    g_dir = ec ? std::filesystem::path() : cache_dir;
}

bool Compile(const std::string& hlsl, const char* entry, Stage stage, uint64_t hash, std::vector<uint32_t>& spirv) {
    if (Load(hash, spirv)) return true;
    std::lock_guard lock(g_lock);
    if (Load(hash, spirv)) return true;

    EShLanguage lang = stage == Stage::Vertex ? EShLangVertex : EShLangFragment;
    std::string text = Adapt(hlsl, stage);
    const char* src = text.c_str();

    glslang::TShader sh(lang);
    sh.setStrings(&src, 1);
    sh.setEntryPoint("main");
    sh.setSourceEntryPoint(entry);
    sh.setEnvInput(glslang::EShSourceHlsl, lang, glslang::EShClientVulkan, 100);
    sh.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_1);
    sh.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_3);
    sh.setHlslIoMapping(true);
    sh.setAutoMapBindings(true);
    sh.setAutoMapLocations(true);
    sh.setShiftBinding(glslang::EResUbo, kBindingCb);
    sh.setShiftBinding(glslang::EResTexture, kBindingTex);
    sh.setShiftBinding(glslang::EResSampler, kBindingSampler);

    auto messages = EShMessages(EShMsgSpvRules | EShMsgVulkanRules | EShMsgReadHlsl | EShMsgHlslOffsets);
    if (!sh.parse(GetDefaultResources(), 100, false, messages)) {
        LOGE("vulkan: %s %s compile failed: %s", stage == Stage::Vertex ? "vertex" : "pixel", entry, sh.getInfoLog());
        return false;
    }

    glslang::TProgram program;
    program.addShader(&sh);
    if (!program.link(messages) || !program.mapIO()) {
        LOGE("vulkan: %s %s link failed: %s", stage == Stage::Vertex ? "vertex" : "pixel", entry, program.getInfoLog());
        return false;
    }

    glslang::SpvOptions options;
    options.disableOptimizer = true;
    options.validate = false;
    spirv.clear();
    glslang::GlslangToSpv(*program.getIntermediate(lang), spirv, &options);
    if (spirv.empty()) return false;
    Store(hash, spirv);
    return true;
}

}  // namespace vkshader
