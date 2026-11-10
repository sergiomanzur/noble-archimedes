// Android: NVIDIA DLSS is a Windows/NVIDIA-desktop feature. Keep the renderer's
// API surface linked but permanently unavailable; AMD FSR's Vulkan path and the
// renderer's own TAA remain.
#include <memory>

#include "linux/dlss_vk.h"

namespace gpu::linux_dlss {

bool Load() { return false; }
bool Loaded() { return false; }
void InstanceExtensions(std::vector<const char*>&) {}
void DeviceExtensions(VkInstance, VkPhysicalDevice, std::vector<const char*>&) {}
std::unique_ptr<framegen::AntiAlias> MakeDlss(ID3D12Device*, ID3D12CommandQueue*) { return nullptr; }

}  // namespace gpu::linux_dlss
