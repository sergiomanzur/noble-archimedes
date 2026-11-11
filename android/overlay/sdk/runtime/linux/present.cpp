// Linux: the D3D12 device on Vulkan through vkd3d, and the swap chain (linux/present.h)
#ifndef _WIN32
#include "linux/present.h"

#include <SDL3/SDL_vulkan.h>
#include <vkd3d.h>

#include <algorithm>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "app/app.h"
#include "core/log.h"
#include "linux/dlss_vk.h"
#include "linux/sdl_app.h"
#ifdef __ANDROID__
#include <vulkan/vulkan_android.h>

#include "android_present_shaders.h"
#endif

namespace gpu::linux_present {

// a fence's event: vkd3d signals it from its fence thread
struct Event {
    std::mutex m;
    std::condition_variable cv;
    bool set = false;
};

namespace {

HRESULT SignalEvent(HANDLE handle) {
    auto* e = static_cast<Event*>(handle);
    {
        std::lock_guard lock(e->m);
        e->set = true;
    }
    e->cv.notify_all();
    return S_OK;
}

uint32_t MemoryType(VkPhysicalDevice pd, uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

}  // namespace

HANDLE CreateEvent() { return new Event; }

void WaitEvent(HANDLE handle) {
    auto* e = static_cast<Event*>(handle);
    std::unique_lock lock(e->m);
    e->cv.wait(lock, [&] { return e->set; });
    e->set = false;
}

#ifdef __ANDROID__
namespace {

// The Adreno driver's vkCmdPushDescriptorSetKHR frees one of its own objects and then uses it again, writing into memory
// that has gone to someone else: vkd3d's objects (a texture's, a render target's) turn up with their first word zeroed,
// and a frame draws nothing or the title screen comes out black, on some starts and not others. vkd3d uses push
// descriptors for the root constant buffers only when the device offers the extension, and works without it, so the
// device is not shown it: vkd3d is handed a vkGetInstanceProcAddr whose device extension list lacks it.
PFN_vkGetInstanceProcAddr g_real_get_instance_proc = nullptr;
VkInstance g_filter_instance = VK_NULL_HANDLE;

VKAPI_ATTR VkResult VKAPI_CALL EnumerateDeviceExtensionsWithoutPush(VkPhysicalDevice pd, const char* layer, uint32_t* count,
                                                                    VkExtensionProperties* props) {
    auto real = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(
            g_real_get_instance_proc(g_filter_instance, "vkEnumerateDeviceExtensionProperties"));
    if (!real) return VK_ERROR_INITIALIZATION_FAILED;
    uint32_t n = 0;
    VkResult r = real(pd, layer, &n, nullptr);
    if (r != VK_SUCCESS) return r;
    std::vector<VkExtensionProperties> all(n);
    r = real(pd, layer, &n, all.data());
    if (r != VK_SUCCESS && r != VK_INCOMPLETE) return r;
    all.resize(n);
    std::vector<VkExtensionProperties> kept;
    for (const VkExtensionProperties& e : all)
        if (std::strcmp(e.extensionName, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME) != 0) kept.push_back(e);
    if (!props) {
        *count = uint32_t(kept.size());
        return VK_SUCCESS;
    }
    uint32_t copied = std::min<uint32_t>(*count, uint32_t(kept.size()));
    for (uint32_t i = 0; i < copied; i++) props[i] = kept[i];
    *count = copied;
    return copied < kept.size() ? VK_INCOMPLETE : VK_SUCCESS;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL InstanceProcWithoutPush(VkInstance instance, const char* name) {
    if (instance != VK_NULL_HANDLE) g_filter_instance = instance;
    if (name && !std::strcmp(name, "vkEnumerateDeviceExtensionProperties") && instance != VK_NULL_HANDLE)
        return reinterpret_cast<PFN_vkVoidFunction>(&EnumerateDeviceExtensionsWithoutPush);
    return g_real_get_instance_proc(instance, name);
}

}  // namespace
#endif

bool CreateDevice(const std::string& want, ID3D12Device** device, std::string& name) {
    // the instance with the window system's surface extensions, so a swap chain can be made on its device
    Uint32 n_ext = 0;
    const char* const* exts = SDL_Vulkan_GetInstanceExtensions(&n_ext);
    if (!exts) {
        LOGE("d3d12: no Vulkan here (%s)", SDL_GetError());
        return false;
    }
    vkd3d_instance_create_info ici{};
    ici.type = VKD3D_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pfn_signal_event = SignalEvent;
    ici.wchar_size = sizeof(WCHAR);
    ici.instance_extensions = exts;
    ici.instance_extension_count = n_ext;
#ifdef __ANDROID__
    if ((g_real_get_instance_proc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_Vulkan_GetVkGetInstanceProcAddr())))
        ici.pfn_vkGetInstanceProcAddr = InstanceProcWithoutPush;
#endif

    // HDR colour spaces for the window's swap chain, and what NVIDIA DLSS asks for when its files are there
    // (linux/dlss_vk.h), where the driver has them
    std::vector<const char*> optional_instance{VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME};
    linux_dlss::InstanceExtensions(optional_instance);
    vkd3d_optional_instance_extensions_info oi{};
    oi.type = VKD3D_STRUCTURE_TYPE_OPTIONAL_INSTANCE_EXTENSIONS_INFO;
    oi.extensions = optional_instance.data();
    oi.extension_count = uint32_t(optional_instance.size());
    ici.next = &oi;
    vkd3d_instance* instance = nullptr;
    if (FAILED(vkd3d_create_instance(&ici, &instance))) {
        LOGE("d3d12: vkd3d could not start Vulkan");
        return false;
    }
    // the card the settings name, else a discrete one over an integrated one over the rest (a laptop's dedicated GPU)
    VkInstance vki = vkd3d_instance_get_vk_instance(instance);
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(vki, &n, nullptr);
    std::vector<VkPhysicalDevice> cards(n);
    vkEnumeratePhysicalDevices(vki, &n, cards.data());
    VkPhysicalDevice pick = VK_NULL_HANDLE;
    int best = -1;
    for (VkPhysicalDevice pd : cards) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(pd, &p);
        LOGI("d3d12: Vulkan device %s", p.deviceName);
        int rank = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU     ? 3
                   : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2
                   : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU            ? 0
                                                                            : 1;
        if (!want.empty() && want == p.deviceName) rank = 100;
        if (rank > best) {
            best = rank;
            pick = pd;
        }
    }
    if (!want.empty() && best < 100) LOGR("D3D12: no graphics card %s with Vulkan here, the fastest one instead", want.c_str());
    const char* dev_exts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    vkd3d_device_create_info dci{};
    dci.type = VKD3D_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.minimum_feature_level = D3D_FEATURE_LEVEL_11_0;
    dci.instance = instance;
    dci.vk_physical_device = pick;
    dci.device_extensions = dev_exts;
    dci.device_extension_count = 1;
    // and the HDR metadata sent with an HDR10 swap chain, and NVIDIA DLSS's, on NVIDIA's own driver
    std::vector<const char*> optional_device{VK_EXT_HDR_METADATA_EXTENSION_NAME};
    linux_dlss::DeviceExtensions(vki, pick, optional_device);
    vkd3d_optional_device_extensions_info od{};
    od.type = VKD3D_STRUCTURE_TYPE_OPTIONAL_DEVICE_EXTENSIONS_INFO;
    od.extensions = optional_device.data();
    od.extension_count = uint32_t(optional_device.size());
    dci.next = &od;
    HRESULT hr = vkd3d_create_device(&dci, __uuidof(ID3D12Device), reinterpret_cast<void**>(device));
    vkd3d_instance_decref(instance);  // the device holds it
    if (FAILED(hr)) {
        LOGE("d3d12: vkd3d could not make a device (%08x)", unsigned(hr));
        return false;
    }
    VkPhysicalDeviceProperties p;
    vkGetPhysicalDeviceProperties(vkd3d_get_vk_physical_device(*device), &p);
    name = p.deviceName;
    return true;
}

Swap::~Swap() {
    if (vkd_) vkDeviceWaitIdle(vkd_);
    DestroyBuffers();
    DestroySwapchain(false);
    for (Slot& s : slots_) {
        if (s.done) vkDestroyFence(vkd_, s.done, nullptr);
        if (s.acquired) vkDestroySemaphore(vkd_, s.acquired, nullptr);
    }
    if (pool_) vkDestroyCommandPool(vkd_, pool_, nullptr);
    if (surface_) SDL_Vulkan_DestroySurface(vki_, surface_, nullptr);
    if (fence_) fence_->Release();
}

bool Swap::Create(ID3D12Device* device, ID3D12CommandQueue* queue, SDL_Window* window, uint32_t count, uint32_t w,
                  uint32_t h) {
    dev_ = device;
    queue_ = queue;
    window_ = window;
    count_ = count;
    w_ = w;
    h_ = h;
    vki_ = vkd3d_instance_get_vk_instance(vkd3d_instance_from_device(device));
    vkpd_ = vkd3d_get_vk_physical_device(device);
    vkd_ = vkd3d_get_vk_device(device);
    family_ = vkd3d_get_vk_queue_family_index(queue);
    uint32_t n_ext = 0;
    vkEnumerateDeviceExtensionProperties(vkpd_, nullptr, &n_ext, nullptr);
    std::vector<VkExtensionProperties> device_exts(n_ext);
    vkEnumerateDeviceExtensionProperties(vkpd_, nullptr, &n_ext, device_exts.data());
    for (const VkExtensionProperties& e : device_exts)
        if (std::string(e.extensionName) == VK_EXT_HDR_METADATA_EXTENSION_NAME)
            set_hdr_metadata_ = reinterpret_cast<PFN_vkSetHdrMetadataEXT>(vkGetDeviceProcAddr(vkd_, "vkSetHdrMetadataEXT"));
    if (!SDL_Vulkan_CreateSurface(window, vki_, nullptr, &surface_)) {
        LOGE("d3d12: no Vulkan surface for the window (%s)", SDL_GetError());
        return false;
    }
#ifdef __ANDROID__
    surface_window_ = SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER,
                                             nullptr);
#endif
    VkBool32 presents = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(vkpd_, family_, surface_, &presents);
    if (!presents) {
        LOGE("d3d12: the device's queue cannot show pictures in this window");
        return false;
    }
    dev_->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), reinterpret_cast<void**>(&fence_));
    event_ = CreateEvent();
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = family_;
    vkCreateCommandPool(vkd_, &pci, nullptr, &pool_);
    slots_.resize(count + 1);
    for (Slot& s : slots_) {
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = pool_;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        vkAllocateCommandBuffers(vkd_, &ai, &s.cmd);
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        vkCreateFence(vkd_, &fci, nullptr, &s.done);
        VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        vkCreateSemaphore(vkd_, &sci, nullptr, &s.acquired);
    }
    return CreateBuffers() && CreateSwapchain();
}

// the back buffers: BGRA images (HDR10's R10G10B10A2 with HDR) the renderer draws into as render targets, copied from
// at present. Their D3D12 present state is the copy source layout, which is how this copy reads them.
bool Swap::CreateBuffers() {
    buffers_.resize(count_);
    for (Buffer& b : buffers_) {
        VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ii.imageType = VK_IMAGE_TYPE_2D;
        ii.format = hdr_ ? VK_FORMAT_A2B10G10R10_UNORM_PACK32 : VK_FORMAT_B8G8R8A8_UNORM;
        ii.extent = {w_, h_, 1};
        ii.mipLevels = 1;
        ii.arrayLayers = 1;
        ii.samples = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                   VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(vkd_, &ii, nullptr, &b.image) != VK_SUCCESS) return false;
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(vkd_, b.image, &req);
        VkMemoryAllocateInfo mi{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mi.allocationSize = req.size;
        mi.memoryTypeIndex = MemoryType(vkpd_, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (mi.memoryTypeIndex == UINT32_MAX) mi.memoryTypeIndex = MemoryType(vkpd_, req.memoryTypeBits, 0);
        if (vkAllocateMemory(vkd_, &mi, nullptr, &b.memory) != VK_SUCCESS) return false;
        vkBindImageMemory(vkd_, b.image, b.memory, 0);
#ifdef __ANDROID__
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = b.image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = ii.format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (vkCreateImageView(vkd_, &vi, nullptr, &b.view) != VK_SUCCESS) return false;
#endif
        vkd3d_image_resource_create_info ri{};
        ri.type = VKD3D_STRUCTURE_TYPE_IMAGE_RESOURCE_CREATE_INFO;
        ri.vk_image = b.image;
        ri.desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        ri.desc.Width = w_;
        ri.desc.Height = h_;
        ri.desc.DepthOrArraySize = 1;
        ri.desc.MipLevels = 1;
        ri.desc.Format = hdr_ ? DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_B8G8R8A8_UNORM;
        ri.desc.SampleDesc.Count = 1;
        ri.desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        ri.flags = VKD3D_RESOURCE_INITIAL_STATE_TRANSITION | VKD3D_RESOURCE_PRESENT_STATE_TRANSITION;
        ri.present_state = D3D12_RESOURCE_STATE_COPY_SOURCE;
        if (FAILED(vkd3d_create_image_resource(dev_, &ri, &b.resource))) return false;
    }
    next_ = 0;
    return true;
}

void Swap::DestroyBuffers() {
    for (Buffer& b : buffers_) {
        if (b.resource) b.resource->Release();
#ifdef __ANDROID__
        if (b.view) vkDestroyImageView(vkd_, b.view, nullptr);
#endif
        if (b.image) vkDestroyImage(vkd_, b.image, nullptr);
        if (b.memory) vkFreeMemory(vkd_, b.memory, nullptr);
    }
    buffers_.clear();
}

bool Swap::CreateSwapchain() {
    VkSurfaceCapabilitiesKHR caps;
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vkpd_, surface_, &caps) != VK_SUCCESS) return false;
    uint32_t nf = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(vkpd_, surface_, &nf, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(nf);
    vkGetPhysicalDeviceSurfaceFormatsKHR(vkpd_, surface_, &nf, formats.data());
    // BGRA or RGBA as stored, not one the display would make linear: the picture is already as it should look. With
    // HDR, a 10 bit one in HDR10's colour space (PQ over BT.2020), as the back buffers hold it
    VkSurfaceFormatKHR chosen = formats.empty() ? VkSurfaceFormatKHR{} : formats[0];
    for (const VkSurfaceFormatKHR& f : formats)
        if ((f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            chosen = f;
            break;
        }
    hdr_shown_ = hdr_ && SurfaceHdr10(&chosen);
    format_ = chosen.format;
    uint32_t nm = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(vkpd_, surface_, &nm, nullptr);
    std::vector<VkPresentModeKHR> modes(nm);
    vkGetPhysicalDeviceSurfacePresentModesKHR(vkpd_, surface_, &nm, modes.data());
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    if (!fifo_)
        for (VkPresentModeKHR want : {VK_PRESENT_MODE_IMMEDIATE_KHR, VK_PRESENT_MODE_MAILBOX_KHR})
            if (std::find(modes.begin(), modes.end(), want) != modes.end()) {
                mode = want;
                break;
            }
    // the window's size as the main loop last saw it (Wayland leaves currentExtent to the swap chain)
    VkExtent2D window{linuxapp::g_pixel_w.load(), linuxapp::g_pixel_h.load()};
    extent_ = window.width && window.height ? window
             : caps.currentExtent.width != UINT32_MAX ? caps.currentExtent : VkExtent2D{1280, 720};
    pretransform_ = caps.currentTransform;
    // A portrait Android panel in a landscape window reports ROTATE_90/270. The swap chain image is
    // then stored sideways, so its extent is the window's swapped dimensions.
    if (pretransform_ == VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR ||
        pretransform_ == VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR)
        std::swap(extent_.width, extent_.height);
    extent_.width = std::clamp(std::max(extent_.width, 1u), caps.minImageExtent.width, std::max(caps.maxImageExtent.width, 1u));
    extent_.height = std::clamp(std::max(extent_.height, 1u), caps.minImageExtent.height, std::max(caps.maxImageExtent.height, 1u));
    VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    sci.surface = surface_;
    sci.minImageCount = std::max(3u, caps.minImageCount);
    if (caps.maxImageCount) sci.minImageCount = std::min(sci.minImageCount, caps.maxImageCount);
    sci.imageFormat = format_;
    sci.imageColorSpace = chosen.colorSpace;
    sci.imageExtent = extent_;
    sci.imageArrayLayers = 1;
    sci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sci.preTransform = pretransform_;
    sci.compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
                             ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                             : VkCompositeAlphaFlagBitsKHR(caps.supportedCompositeAlpha & -caps.supportedCompositeAlpha);
    sci.presentMode = mode;
    sci.clipped = VK_TRUE;
    sci.oldSwapchain = swap_;
    VkSwapchainKHR made = VK_NULL_HANDLE;
    VkResult r = vkCreateSwapchainKHR(vkd_, &sci, nullptr, &made);
    DestroySwapchain(false);
    if (r != VK_SUCCESS) {
        LOGE("d3d12: no Vulkan swap chain (%d)", int(r));
        return false;
    }
    swap_ = made;
    uint32_t ni = 0;
    vkGetSwapchainImagesKHR(vkd_, swap_, &ni, nullptr);
    images_.resize(ni);
    vkGetSwapchainImagesKHR(vkd_, swap_, &ni, images_.data());
    rendered_.resize(ni);
    for (VkSemaphore& s : rendered_) {
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        vkCreateSemaphore(vkd_, &si, nullptr, &s);
    }
#ifdef __ANDROID__
    if (pretransform_ != VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR && !CreateRotateResources()) {
        LOGE("d3d12: the display's rotation could not be set up");
        return false;
    }
#endif
    stale_ = false;
    LOGI("d3d12: swap chain %ux%u, %u images, format %d, %s, %s", extent_.width, extent_.height, ni, int(format_),
         hdr_shown_ ? "HDR10" : "SDR",
         mode == VK_PRESENT_MODE_FIFO_KHR ? "vsync" : mode == VK_PRESENT_MODE_MAILBOX_KHR ? "mailbox" : "immediate");
    if (hdr_shown_) SendHdrMetadata();
    return true;
}

#ifdef __ANDROID__
// The window's compositor rotates the swap chain image to the panel's orientation. Render the
// back buffer through a full-screen triangle into the (already sideways) swap chain image so the
// picture is upright after that rotation. The shader is generated from android/present_rotate.*.
void Swap::DestroyRotateResources() {
    if (!vkd_) return;
    for (VkFramebuffer f : rotate_framebuffers_) vkDestroyFramebuffer(vkd_, f, nullptr);
    rotate_framebuffers_.clear();
    for (VkImageView v : rotate_views_) vkDestroyImageView(vkd_, v, nullptr);
    rotate_views_.clear();
    if (rotate_pipeline_) vkDestroyPipeline(vkd_, rotate_pipeline_, nullptr);
    if (rotate_pass_) vkDestroyRenderPass(vkd_, rotate_pass_, nullptr);
    if (rotate_pool_) vkDestroyDescriptorPool(vkd_, rotate_pool_, nullptr);
    if (rotate_sampler_) vkDestroySampler(vkd_, rotate_sampler_, nullptr);
    if (rotate_layout_) vkDestroyPipelineLayout(vkd_, rotate_layout_, nullptr);
    if (rotate_set_layout_) vkDestroyDescriptorSetLayout(vkd_, rotate_set_layout_, nullptr);
    rotate_pipeline_ = VK_NULL_HANDLE;
    rotate_pass_ = VK_NULL_HANDLE;
    rotate_pool_ = VK_NULL_HANDLE;
    rotate_sampler_ = VK_NULL_HANDLE;
    rotate_layout_ = VK_NULL_HANDLE;
    rotate_set_layout_ = VK_NULL_HANDLE;
}

bool Swap::CreateRotateResources() {
    DestroyRotateResources();
    if (pretransform_ == VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR || buffers_.empty() || images_.empty()) return true;

    VkShaderModule vert = VK_NULL_HANDLE, frag = VK_NULL_HANDLE;
    VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    sm.codeSize = sizeof(kPresentRotateVert);
    sm.pCode = kPresentRotateVert;
    if (vkCreateShaderModule(vkd_, &sm, nullptr, &vert) != VK_SUCCESS) return false;
    sm.codeSize = sizeof(kPresentRotateFrag);
    sm.pCode = kPresentRotateFrag;
    if (vkCreateShaderModule(vkd_, &sm, nullptr, &frag) != VK_SUCCESS) {
        vkDestroyShaderModule(vkd_, vert, nullptr);
        return false;
    }

    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo dsl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dsl.bindingCount = 1;
    dsl.pBindings = &binding;
    vkCreateDescriptorSetLayout(vkd_, &dsl, nullptr, &rotate_set_layout_);

    VkPushConstantRange pcr{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32};
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &rotate_set_layout_;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &pcr;
    vkCreatePipelineLayout(vkd_, &pl, nullptr, &rotate_layout_);

    VkAttachmentDescription color{};
    color.format = format_;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;
    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rp.attachmentCount = 1;
    rp.pAttachments = &color;
    rp.subpassCount = 1;
    rp.pSubpasses = &sub;
    rp.dependencyCount = 1;
    rp.pDependencies = &dep;
    if (vkCreateRenderPass(vkd_, &rp, nullptr, &rotate_pass_) != VK_SUCCESS) return false;

    VkPipelineShaderStageCreateInfo stages[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vert, "main",
         nullptr},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, frag, "main",
         nullptr}};
    VkPipelineVertexInputStateCreateInfo vin{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                         VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    ds.dynamicStateCount = 2;
    ds.pDynamicStates = dyn;
    VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gp.stageCount = 2;
    gp.pStages = stages;
    gp.pVertexInputState = &vin;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vp;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms;
    gp.pColorBlendState = &cb;
    gp.pDynamicState = &ds;
    gp.layout = rotate_layout_;
    gp.renderPass = rotate_pass_;
    gp.subpass = 0;
    VkResult pr = vkCreateGraphicsPipelines(vkd_, VK_NULL_HANDLE, 1, &gp, nullptr, &rotate_pipeline_);

    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = VK_FILTER_LINEAR;
    si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod = 0.0f;
    vkCreateSampler(vkd_, &si, nullptr, &rotate_sampler_);

    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, uint32_t(buffers_.size())};
    VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.maxSets = uint32_t(buffers_.size());
    pci.poolSizeCount = 1;
    pci.pPoolSizes = &ps;
    vkCreateDescriptorPool(vkd_, &pci, nullptr, &rotate_pool_);
    std::vector<VkDescriptorSetLayout> layouts(buffers_.size(), rotate_set_layout_);
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = rotate_pool_;
    ai.descriptorSetCount = uint32_t(layouts.size());
    ai.pSetLayouts = layouts.data();
    std::vector<VkDescriptorSet> sets(buffers_.size());
    if (vkAllocateDescriptorSets(vkd_, &ai, sets.data()) != VK_SUCCESS) return false;
    for (size_t i = 0; i < buffers_.size(); i++) {
        buffers_[i].descriptor = sets[i];
        VkDescriptorImageInfo image{rotate_sampler_, buffers_[i].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = sets[i];
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &image;
        vkUpdateDescriptorSets(vkd_, 1, &write, 0, nullptr);
    }

    rotate_views_.resize(images_.size());
    rotate_framebuffers_.resize(images_.size());
    for (size_t i = 0; i < images_.size(); i++) {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = images_[i];
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = format_;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (vkCreateImageView(vkd_, &vi, nullptr, &rotate_views_[i]) != VK_SUCCESS) return false;
        VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fi.renderPass = rotate_pass_;
        fi.attachmentCount = 1;
        fi.pAttachments = &rotate_views_[i];
        fi.width = extent_.width;
        fi.height = extent_.height;
        fi.layers = 1;
        if (vkCreateFramebuffer(vkd_, &fi, nullptr, &rotate_framebuffers_[i]) != VK_SUCCESS) return false;
    }

    vkDestroyShaderModule(vkd_, vert, nullptr);
    vkDestroyShaderModule(vkd_, frag, nullptr);
    if (pr != VK_SUCCESS) {
        LOGE("d3d12: the display-rotation pipeline could not be made (%d)", int(pr));
        return false;
    }
    return true;
}

void Swap::RotateImage(VkCommandBuffer cmd, Buffer& src, VkImage dst, uint32_t index) {
    (void)dst;
    // Source: the renderer left the back buffer as a copy source; sample it and put it back the same way.
    VkImageMemoryBarrier bars[2] = {{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}, {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}};
    bars[0].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    bars[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    bars[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    bars[0].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    bars[0].image = src.image;
    bars[1].srcAccessMask = 0;
    bars[1].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    bars[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    bars[1].newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    bars[1].image = images_[index];
    for (VkImageMemoryBarrier& bar : bars)
        bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &bars[0]);
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &bars[1]);

    VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rp.renderPass = rotate_pass_;
    rp.framebuffer = rotate_framebuffers_[index];
    rp.renderArea = {{0, 0}, extent_};
    VkClearValue clear{};
    rp.clearValueCount = 1;
    rp.pClearValues = &clear;
    vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, rotate_pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, rotate_layout_, 0, 1, &src.descriptor, 0, nullptr);
    VkViewport viewport{0, 0, float(extent_.width), float(extent_.height), 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, extent_};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    struct Push {
        float rot[4];
        float off[4];
    } push{};
    switch (pretransform_) {
        case VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR:
            push = {{0, 1, -1, 0}, {0, 1, 0, 0}};
            break;
        case VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR:
            push = {{0, -1, 1, 0}, {1, 0, 0, 0}};
            break;
        case VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR:
            push = {{-1, 0, 0, -1}, {1, 1, 0, 0}};
            break;
        default:
            push = {{1, 0, 0, 1}, {0, 0, 0, 0}};
            break;
    }
    vkCmdPushConstants(cmd, rotate_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd);

    bars[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    bars[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    bars[0].oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    bars[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &bars[0]);
}
#endif

// a 10 bit format of the window's surface in HDR10's colour space, into out
bool Swap::SurfaceHdr10(VkSurfaceFormatKHR* out) {
    uint32_t nf = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(vkpd_, surface_, &nf, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(nf);
    vkGetPhysicalDeviceSurfaceFormatsKHR(vkpd_, surface_, &nf, formats.data());
    for (const VkSurfaceFormatKHR& f : formats)
        if ((f.format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 || f.format == VK_FORMAT_A2R10G10B10_UNORM_PACK32) &&
            f.colorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT) {
            if (out) *out = f;
            return true;
        }
    return false;
}

// what the picture holds, for the compositor's tone mapping: BT.2020's primaries and D65 white, as HDR10 has them,
// and its brightest at the peak the game tone maps to (app::HdrPeak), its scene at the scene brightness set
void Swap::SendHdrMetadata() {
    if (!set_hdr_metadata_) return;
    VkHdrMetadataEXT md{VK_STRUCTURE_TYPE_HDR_METADATA_EXT};
    md.displayPrimaryRed = {0.708f, 0.292f};
    md.displayPrimaryGreen = {0.170f, 0.797f};
    md.displayPrimaryBlue = {0.131f, 0.046f};
    md.whitePoint = {0.3127f, 0.3290f};
    md.maxLuminance = float(app::HdrPeak());
    md.minLuminance = 0.0f;
    md.maxContentLightLevel = float(app::HdrPeak());
    md.maxFrameAverageLightLevel = float(app::Get().hdr_scene_nits);
    set_hdr_metadata_(vkd_, 1, &swap_, &md);
}

bool Swap::HdrDisplay(float& peak) {
    peak = 0;
    static const bool force = [] {
        const char* v = std::getenv("XBE_HDR_FORCE");
        return v && *v == '1';
    }();
    SDL_PropertiesID p = SDL_GetWindowProperties(window_);
    bool on = SDL_GetBooleanProperty(p, SDL_PROP_WINDOW_HDR_ENABLED_BOOLEAN, false);
    if (on) {
        // SDR white in scRGB (1 is 80 nits), and how many times brighter the display goes
        float white = SDL_GetFloatProperty(p, SDL_PROP_WINDOW_SDR_WHITE_LEVEL_FLOAT, 1.0f);
        float headroom = SDL_GetFloatProperty(p, SDL_PROP_WINDOW_HDR_HEADROOM_FLOAT, 1.0f);
        peak = white * headroom * 80.0f;
    }
    return force || (on && SurfaceHdr10(nullptr));
}

void Swap::DestroySwapchain(bool) {
    if (!vkd_) return;
    if (swap_ || !rendered_.empty()) vkDeviceWaitIdle(vkd_);
#ifdef __ANDROID__
    DestroyRotateResources();
#endif
    for (VkSemaphore s : rendered_) vkDestroySemaphore(vkd_, s, nullptr);
    rendered_.clear();
    if (swap_) vkDestroySwapchainKHR(vkd_, swap_, nullptr);
    swap_ = VK_NULL_HANDLE;
    images_.clear();
}

HRESULT Swap::GetBuffer(uint32_t i, REFIID iid, void** out) {
    if (i >= buffers_.size() || !buffers_[i].resource) return E_INVALIDARG;
    return buffers_[i].resource->QueryInterface(iid, out);
}

// Android: the activity's native window can be replaced while the app is backgrounded. The new
// Vulkan surface is made first, against the new native window, so a failure leaves the old swap
// chain in place for the next attempt. A forced attempt with the same window (a quick
// background/foreground) finds the old surface still owning it and keeps the old swap chain.
bool Swap::SurfaceChanged() const {
#ifdef __ANDROID__
    if (!vkd_ || !window_) return false;
    void* native = SDL_GetPointerProperty(SDL_GetWindowProperties(window_), SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER,
                                          nullptr);
    return !swap_ || native != surface_window_;
#else
    return false;
#endif
}

bool Swap::RecreateSurface(bool force) {
#ifdef __ANDROID__
    if (!vkd_ || !window_) return false;
    void* native = SDL_GetPointerProperty(SDL_GetWindowProperties(window_), SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER,
                                          nullptr);
    if (!native) {
        LOG_FIRST(4, "d3d12: the Android surface is not ready; holding the last picture");
        return false;
    }
    if (surface_ && native == surface_window_) {
        // Same native window. On a normal resume the old surface is still valid; only rebuild
        // the swap chain when an earlier attempt made a surface but no swap chain.
        if (!force && swap_) return true;
        if (!swap_) return CreateSwapchain();
    }
    PFN_vkCreateAndroidSurfaceKHR create = reinterpret_cast<PFN_vkCreateAndroidSurfaceKHR>(
            vkGetInstanceProcAddr(vki_, "vkCreateAndroidSurfaceKHR"));
    if (!create) {
        LOGE("d3d12: the Vulkan instance has no Android surface function");
        return false;
    }
    VkAndroidSurfaceCreateInfoKHR ci{VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};
    ci.window = static_cast<ANativeWindow*>(native);
    VkSurfaceKHR fresh = VK_NULL_HANDLE;
    VkResult r = create(vki_, &ci, nullptr, &fresh);
    if (r == VK_ERROR_NATIVE_WINDOW_IN_USE_KHR) {
        // The old surface still owns this window: the activity kept it across the resume.
        return swap_ != VK_NULL_HANDLE;
    }
    if (r != VK_SUCCESS) {
        LOG_FIRST(4, "d3d12: no new Android Vulkan surface (%d)", int(r));
        return false;
    }
    VkBool32 presents = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(vkpd_, family_, fresh, &presents);
    if (!presents) {
        SDL_Vulkan_DestroySurface(vki_, fresh, nullptr);
        LOGE("d3d12: the new Android surface cannot show pictures");
        return false;
    }
    vkDeviceWaitIdle(vkd_);
    DestroySwapchain(false);
    if (surface_) SDL_Vulkan_DestroySurface(vki_, surface_, nullptr);
    surface_ = fresh;
    surface_window_ = native;
    LOGI("d3d12: the Android surface was made again");
    return CreateSwapchain();
#else
    (void)force;
    return false;
#endif
}

bool Swap::ResizeBuffers(uint32_t w, uint32_t h, bool hdr) {
    vkDeviceWaitIdle(vkd_);
    DestroyBuffers();
    w_ = w;
    h_ = h;
    hdr_ = hdr;
    stale_ = true;
    return CreateBuffers();
}

HRESULT Swap::Present(uint32_t sync) {
    bool fifo = sync != 0;
    if (fifo != fifo_) {
        fifo_ = fifo;
        stale_ = true;
    }
    Buffer& b = buffers_[next_];
    // the frame's commands handed to Vulkan (not yet run): the copy goes after them on the same queue
    vkd3d_queue_signal_on_cpu(queue_, fence_, ++fence_value_);
    fence_->SetEventOnCompletion(fence_value_, event_);
    WaitEvent(event_);

    for (int attempt = 0; attempt < 2; attempt++) {
        if (stale_ || !swap_) {
            vkDeviceWaitIdle(vkd_);
            if (!CreateSwapchain()) return E_FAIL;
        }
        Slot& s = slots_[slot_];
        vkWaitForFences(vkd_, 1, &s.done, VK_TRUE, UINT64_MAX);
        uint32_t index = 0;
        VkResult r = vkAcquireNextImageKHR(vkd_, swap_, UINT64_MAX, s.acquired, VK_NULL_HANDLE, &index);
        if (r == VK_ERROR_OUT_OF_DATE_KHR) {
            stale_ = true;
            continue;
        }
        // An Android surface can stay suboptimal forever (the panel's natural rotation differs from the
        // window's); recreating every frame would never show a picture. Only OUT_OF_DATE recreates.
        if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) return E_FAIL;
        vkResetFences(vkd_, 1, &s.done);

        // the back buffer, written by the frame before it on this queue, into the swap chain image at its size
        VkCommandBuffer cmd = s.cmd;
        vkResetCommandBuffer(cmd, 0);
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &bi);
#ifdef __ANDROID__
        bool rotated = pretransform_ != VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR && rotate_pipeline_ &&
                       index < rotate_framebuffers_.size();
#else
        bool rotated = false;
#endif
        if (rotated) {
            RotateImage(cmd, b, images_[index], index);
        } else {
            VkImageMemoryBarrier bars[2] = {{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER},
                                            {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}};
            bars[0].srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
            bars[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            bars[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            bars[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            bars[0].image = b.image;
            bars[1].srcAccessMask = 0;
            bars[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            bars[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            bars[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            bars[1].image = images_[index];
            for (VkImageMemoryBarrier& bar : bars) {
                bar.srcQueueFamilyIndex = bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            }
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                                 0, nullptr, 2, bars);
            VkImageBlit blit{};
            blit.srcSubresource = blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            blit.srcOffsets[1] = {int32_t(w_), int32_t(h_), 1};
            blit.dstOffsets[1] = {int32_t(extent_.width), int32_t(extent_.height), 1};
            bool same = w_ == extent_.width && h_ == extent_.height;
            vkCmdBlitImage(cmd, b.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, images_[index],
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, same ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
            bars[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            bars[1].dstAccessMask = 0;
            bars[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            bars[1].newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0,
                                 nullptr, 0, nullptr, 1, &bars[1]);
        }
        vkEndCommandBuffer(cmd);

        VkQueue vq = vkd3d_acquire_vk_queue(queue_);
        VkPipelineStageFlags wait_stage =
                rotated ? VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT : VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &s.acquired;
        si.pWaitDstStageMask = &wait_stage;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &rendered_[index];
        vkQueueSubmit(vq, 1, &si, s.done);
        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &rendered_[index];
        pi.swapchainCount = 1;
        pi.pSwapchains = &swap_;
        pi.pImageIndices = &index;
        r = vkQueuePresentKHR(vq, &pi);
        vkd3d_release_vk_queue(queue_);
        slot_ = (slot_ + 1) % uint32_t(slots_.size());
        if (r == VK_ERROR_OUT_OF_DATE_KHR) stale_ = true;
        // SUBOPTIMAL alone is ignored: the Android compositor may keep it for the surface's whole life.
        // the window's size changed: a swap chain of the new size from the next present. A rotated
        // (portrait panel) swap chain stores the window's swapped dimensions.
        uint32_t want_w = extent_.width, want_h = extent_.height;
        if (pretransform_ == VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR ||
            pretransform_ == VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR)
            std::swap(want_w, want_h);
        if (linuxapp::g_pixel_w != want_w || linuxapp::g_pixel_h != want_h) stale_ = true;
        break;
    }
    next_ = (next_ + 1) % count_;
    return S_OK;
}

}  // namespace gpu::linux_present

// the Win32 event functions the shared sources call for fence waits (linux/windows.h)
HANDLE CreateEventW(void*, BOOL, BOOL, const wchar_t*) { return gpu::linux_present::CreateEvent(); }
uint32_t WaitForSingleObject(HANDLE handle, uint32_t) {
    gpu::linux_present::WaitEvent(handle);
    return WAIT_OBJECT_0;
}
BOOL CloseHandle(HANDLE handle) {
    delete static_cast<gpu::linux_present::Event*>(handle);
    return TRUE;
}
#endif
