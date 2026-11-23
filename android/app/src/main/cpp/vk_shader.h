#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// HLSL to SPIR-V through glslang, with an on-disk cache.
namespace vkshader {

// Single descriptor set layout matching gpu/shaders.cpp:
//   b0..b1 -> bindings 0..1  (dynamic uniform buffers: vertex constants, DrawConstants)
//   t0..t5 -> bindings 2..7  (4 stage textures + 2 material maps mtn/mto)
//   s0..s3 -> bindings 8..11 (4 stage samplers)
constexpr uint32_t kBindingCb = 0;
constexpr uint32_t kBindingTex = 2;
constexpr uint32_t kTexBindings = 6;
constexpr uint32_t kBindingSampler = kBindingTex + kTexBindings;
constexpr uint32_t kSamplerBindings = 4;
constexpr uint32_t kBindings = kBindingSampler + kSamplerBindings;

enum class Stage { Vertex, Fragment };

// clip_distance false strips SV_ClipDistance0 (Mali-G57 compiler crash workaround);
// max_point_size > 0 emits [[vk::builtin("PointSize")]] on game vertex shaders so point
// primitives render cleanly on mobile GPUs without geometry shaders.
void Init(const std::filesystem::path& cache_dir, bool clip_distance, float max_point_size);

// Compile HLSL to Vulkan 1.1 / SPIR-V 1.3 (always entry point "main" for Mali compatibility).
// hash names the cached SPIR-V module on disk.
bool Compile(const std::string& hlsl, const char* entry, Stage stage, uint64_t hash, std::vector<uint32_t>& spirv);

}  // namespace vkshader
