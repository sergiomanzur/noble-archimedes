// Android: the game's own XMV movies play through the Xbox decoder; HD movie
// packs use FFmpeg on the Linux preview, which is not built into this APK yet.
// Keep the hook surface linked and report no replacement frames.
#include <cstdint>
#include <memory>
#include <string>

#include "gpu/ext.h"

bool g_hd_movies = false;
uint32_t g_movie_view = 0;

void MoviesInit() {}
void MoviesFrame(uint64_t) {}
uint64_t MoviesReplace(const gpu::TextureDesc&, const uint8_t*) { return 0; }
bool MoviesOwn(uint64_t) { return false; }
std::string MoviesJson() { return "{}"; }
std::shared_ptr<const gpu::Replacement> MoviesReplaced(uint64_t) { return nullptr; }
bool MoviesScale(const gpu::TextureDesc&, float, float, float, float&, float&) { return false; }
