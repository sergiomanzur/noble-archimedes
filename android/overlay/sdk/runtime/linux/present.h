#pragma once
// Linux: the D3D12 device on Vulkan (vkd3d), and the swap chain DXGI gives on Windows. The renderer draws into back
// buffers that are Vulkan images of this one's, wrapped as D3D12 resources; Present copies the one drawn into the
// window's swap chain image, which can have another count, format or size.
#include <SDL3/SDL.h>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>
#include <vector>

#include "d3d12.h"

namespace gpu::linux_present {

// the device on the graphics card the settings name, else the fastest; its name for the log, the fence events
// signalled through the event functions below
bool CreateDevice(const std::string& want, ID3D12Device** device, std::string& name);

// a fence's event (CreateEventW's on Windows): auto reset
HANDLE CreateEvent();
void WaitEvent(HANDLE event);

class Swap {
public:
    ~Swap();
    bool Create(ID3D12Device* device, ID3D12CommandQueue* queue, SDL_Window* window, uint32_t count, uint32_t w,
                uint32_t h);
    uint32_t GetCurrentBackBufferIndex() const { return next_; }
    HRESULT GetBuffer(uint32_t i, REFIID iid, void** out);
    // sync 1: at the display's refresh (FIFO), 0: as soon as drawn
    HRESULT Present(uint32_t sync);
    // the back buffers made again at this size, HDR10 (R10G10B10A2, PQ over BT.2020) or BGRA; the caller has let go of
    // them and the GPU is done with them. The swap chain follows at the next present
    bool ResizeBuffers(uint32_t w, uint32_t h, bool hdr);
    // Android: the SDL window's native surface was destroyed and made again (background/resume).
    // force asks for a new surface even when the native window pointer did not change; if the
    // existing surface still owns that window, the old one is kept.
    bool RecreateSurface(bool force = false);
    // Android: the window's native surface differs from the one the swap chain was made on
    bool SurfaceChanged() const;
    // the window's display shows HDR: its compositor says so (SDL's window properties, from Wayland's colour
    // management) and the window's surface takes an HDR10 swap chain; peak its brightest, in nits, 0 when unknown.
    // XBE_HDR_FORCE=1 takes it as HDR without either, for tests: the HDR picture is made and shown as it is (SDR where the surface has no HDR10)
    bool HdrDisplay(float& peak);

private:
    struct Buffer {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        ID3D12Resource* resource = nullptr;
#ifdef __ANDROID__
        VkImageView view = VK_NULL_HANDLE;
        VkDescriptorSet descriptor = VK_NULL_HANDLE;
#endif
    };
    struct Slot {
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkFence done = VK_NULL_HANDLE;
        VkSemaphore acquired = VK_NULL_HANDLE;
    };
    bool CreateBuffers();
    void DestroyBuffers();
    bool CreateSwapchain();
    bool SurfaceHdr10(VkSurfaceFormatKHR* out);
    void SendHdrMetadata();
    void DestroySwapchain(bool keep_handle);
    VkSurfaceTransformFlagBitsKHR pretransform_ = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
#ifdef __ANDROID__
    // Android panels are often portrait while the window is landscape: the compositor rotates the
    // swap chain image, so pre-rotate the frame into it with this small full-screen shader.
    bool CreateRotateResources();
    void DestroyRotateResources();
    void RotateImage(VkCommandBuffer cmd, Buffer& src, VkImage dst, uint32_t index);
    VkRenderPass rotate_pass_ = VK_NULL_HANDLE;
    std::vector<VkImageView> rotate_views_;
    std::vector<VkFramebuffer> rotate_framebuffers_;
    VkDescriptorSetLayout rotate_set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout rotate_layout_ = VK_NULL_HANDLE;
    VkPipeline rotate_pipeline_ = VK_NULL_HANDLE;
    VkDescriptorPool rotate_pool_ = VK_NULL_HANDLE;
    VkSampler rotate_sampler_ = VK_NULL_HANDLE;
#endif

    ID3D12Device* dev_ = nullptr;
    ID3D12CommandQueue* queue_ = nullptr;
    ID3D12Fence* fence_ = nullptr;
    HANDLE event_ = nullptr;
    uint64_t fence_value_ = 0;
    SDL_Window* window_ = nullptr;
    void* surface_window_ = nullptr;  // Android: the ANativeWindow surface_ was made on
    VkInstance vki_ = VK_NULL_HANDLE;
    VkPhysicalDevice vkpd_ = VK_NULL_HANDLE;
    VkDevice vkd_ = VK_NULL_HANDLE;
    uint32_t family_ = 0;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkSwapchainKHR swap_ = VK_NULL_HANDLE;
    VkFormat format_ = VK_FORMAT_UNDEFINED;
    VkExtent2D extent_{};
    std::vector<VkImage> images_;
    std::vector<VkSemaphore> rendered_;  // one an image: its present waits for its copy
    bool fifo_ = true;
    bool hdr_ = false;                                         // the back buffers HDR10
    bool hdr_shown_ = false;                                   // and the swap chain too
    PFN_vkSetHdrMetadataEXT set_hdr_metadata_ = nullptr;       // VK_EXT_hdr_metadata's, when the device has it
    bool stale_ = false;  // out of date or suboptimal: made again at the next present
    VkCommandPool pool_ = VK_NULL_HANDLE;
    std::vector<Slot> slots_;
    uint32_t slot_ = 0;
    std::vector<Buffer> buffers_;
    uint32_t count_ = 0, w_ = 0, h_ = 0, next_ = 0;
};

}  // namespace gpu::linux_present
