#pragma once
// Android: Direct3D 12 is vkd3d's (runtime/linux/windows.h). IID_PPV_ARGS takes both raw interface pointers and the
// small ComPtr of runtime/wrl/client.h; vkd3d's __uuidof takes the interface type, not an expression.
#include "windows.h"
#include <vkd3d_d3d12.h>
#include <vkd3d_d3d12sdklayers.h>

namespace Microsoft {
namespace WRL {
template <typename T>
class ComPtr;
}  // namespace WRL
}  // namespace Microsoft

template <typename T>
const GUID& UuidOfPpv(Microsoft::WRL::ComPtr<T>*) {
    return __uuidof(T);
}
template <typename T>
const GUID& UuidOfPpv(T**) {
    return __uuidof(T);
}
template <typename T>
void** IID_PPV_ARGS_Helper(Microsoft::WRL::ComPtr<T>* p) {
    return reinterpret_cast<void**>(p->ReleaseAndGetAddressOf());
}
template <typename T>
void** IID_PPV_ARGS_Helper(T** pp) {
    return reinterpret_cast<void**>(pp);
}
#define IID_PPV_ARGS(pp) UuidOfPpv(pp), IID_PPV_ARGS_Helper(pp)
