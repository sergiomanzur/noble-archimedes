#pragma once
#include <atomic>
#include <cstdint>
#include <memory>

#include "gpu/state.h"

namespace gpu {

// D3DCLEAR flags as the XDK defines them
enum : uint32_t {
    kClearTarget = 0xF0,  // ARGB channels
    kClearZ = 0x01,
    kClearStencil = 0x02,
};

class Backend {
public:
    virtual ~Backend() = default;
    // the title's back buffer size, the output window may differ
    virtual bool Init(uint32_t width, uint32_t height) = 0;
    // rects: count D3DRECTs (4 x int32), null for the whole viewport
    virtual void Clear(const State& s, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color, float z,
                       uint32_t stencil) = 0;
    // indices: the title's 16 bit indices, null for non indexed draws, then start is the first vertex
    virtual void Draw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) = 0;
    virtual void Copy(const Surface& src, const Surface& dst, uint32_t count, uint32_t src_rects_va,
                      uint32_t dst_points_va) = 0;
    virtual void Present(const Surface& front) = 0;
    // the CPU is about to touch this resource's memory: write GPU results back first
    virtual void Sync(uint32_t data_va, uint32_t size) = 0;
    // a lock that does not wait for the GPU (NOOVERWRITE): a result a frame or two old is what the console gives too
    virtual void SyncLate(uint32_t data_va) = 0;
    // SyncLate in two for the render thread: DeliverLate (any thread) writes the newest finished copy to guest memory,
    // false when there is none recent; RecordLate (in order with the draws) starts a copy for a later lock
    virtual bool DeliverLate(uint32_t) { return false; }
    virtual void RecordLate(uint32_t) {}
    virtual void Flush() = 0;
    virtual void SetGamma(const uint8_t* ramp /*3 x 256*/) = 0;
    // visibility tests: draws count passing pixels while simple[kMCountEnable / 4] is set, ClearReport zeroes the
    // count, GetReport writes it to a 16 byte report once the GPU is past those draws; without them it stays pending
    virtual void ClearReport() {}
    virtual void GetReport(uint32_t /*report*/) {}

    // the XDK's fences and busy checks against the render thread; a backend that draws at once is never behind
    virtual uint32_t Fence() { return ++fence_; }
    virtual bool FencePending(uint32_t) { return false; }
    virtual void WaitFence(uint32_t) {}
    virtual bool Busy(uint32_t /*resource*/) { return false; }
    virtual void WaitResource(uint32_t /*resource*/) {}
    // until the GPU has drawn present number n, counted from 0; any thread
    virtual void WaitGpuFrame(uint64_t /*n*/) {}

private:
    uint32_t fence_ = 0;
};

std::unique_ptr<Backend> CreateBackend();
// the same backend driven from a thread of its own: calls return once recorded
std::unique_ptr<Backend> CreateThreadedBackend(std::unique_ptr<Backend> inner);

// perf diagnostic: the title's thread recording for the render thread, and waiting on it
inline std::atomic<uint64_t> g_perf_capture_ns{0}, g_perf_wait_ns{0}, g_perf_waits{0};
inline std::atomic<uint64_t> g_perf_wait_kind_ns[6];  // read-back, resource, fence, flush, copy, next frame
inline std::atomic<uint64_t> g_perf_sync_late{0}, g_perf_sync_block{0};  // read-backs the title waited for, by cause

// F3: save the next frame, every draw and target, under diag::CaptureDir
inline std::atomic<bool> g_capture{false};
// a capture asked for from a menu: saved to captures\ beside the game when XBE_CAPTURE_DIR does not say where
inline std::atomic<bool> g_capture_asked{false};
// Android: the native window's surface was replaced (backgrounded/resumed); the renderer recreates it
inline std::atomic<bool> g_surface_dirty{false};
inline std::atomic<uint32_t> g_captures_saved{0};  // frame captures finished this run
// surfaces with material maps seen into (shaders.cpp, parallax): 0 off, 1 low, 2 high
inline std::atomic<uint32_t> g_surface_depth{0};
// anti-aliasing of the frame's 3D (framegen.h, AntiAlias): 0 off, 1 AMD FSR, 2 NVIDIA DLSS. Set before the renderer
// starts too: DLSS needs Streamline loaded before the device is made
inline std::atomic<uint32_t> g_antialias{0};
// its upscaling (framegen.h, Upscale): 0 the frame's own size, 1 quality, 2 balanced, 3 performance
inline std::atomic<uint32_t> g_upscale{0};
// AMD FSR's sharpening after it, percent (0 none); DLSS has none of its own
inline std::atomic<uint32_t> g_aa_sharpness{30};
// ambient occlusion of the frame's 3D, before its anti-aliasing (which it needs): 0 off, 1 low, 2 high
inline std::atomic<uint32_t> g_ambient_occlusion{0};
// what runs, for a menu: the kind (AntiAliasKind, 0 none), and the last frame's 3D size drawn and output (w << 16 | h)
inline std::atomic<uint32_t> g_antialias_running{0};
inline std::atomic<uint32_t> g_antialias_drawn{0}, g_antialias_output{0};

}  // namespace gpu
