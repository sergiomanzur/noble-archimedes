// Android/Linux: the small part of WRL's ComPtr the renderer uses. The Windows SDK's wrl/client.h needs COM and
// WinRT headers the Android host does not have; vkd3d's interfaces are plain COM, so this is enough.
#pragma once

#include <windows.h>

#include <cstddef>
#include <utility>


namespace Microsoft {
namespace WRL {

template <typename T>
class ComPtr {
public:
    ComPtr() = default;
    ComPtr(std::nullptr_t) {}
    ComPtr(T* p) : p_(p) {
        if (p_) p_->AddRef();
    }
    ComPtr(const ComPtr& other) : p_(other.p_) {
        if (p_) p_->AddRef();
    }
    ComPtr(ComPtr&& other) noexcept : p_(other.p_) { other.p_ = nullptr; }
    ~ComPtr() { Reset(); }

    ComPtr& operator=(const ComPtr& other) {
        if (this != __builtin_addressof(other)) {
            if (other.p_) other.p_->AddRef();
            Reset();
            p_ = other.p_;
        }
        return *this;
    }
    ComPtr& operator=(ComPtr&& other) noexcept {
        if (this != __builtin_addressof(other)) {
            Reset();
            p_ = other.p_;
            other.p_ = nullptr;
        }
        return *this;
    }
    ComPtr& operator=(T* p) {
        if (p) p->AddRef();
        Reset();
        p_ = p;
        return *this;
    }
    ComPtr& operator=(std::nullptr_t) {
        Reset();
        return *this;
    }

    T* Get() const { return p_; }
    // as the Windows ComPtr: the address releases what it held, for a creating call to fill in
    T** operator&() {
        Reset();
        return &p_;
    }
    T** GetAddressOf() { return &p_; }
    T* const* GetAddressOf() const { return &p_; }
    T** ReleaseAndGetAddressOf() {
        Reset();
        return &p_;
    }
    T* operator->() const { return p_; }
    T& operator*() const { return *p_; }
    explicit operator bool() const { return p_ != nullptr; }
    bool operator!() const { return p_ == nullptr; }
    bool operator==(const ComPtr& other) const { return p_ == other.p_; }
    bool operator!=(const ComPtr& other) const { return p_ != other.p_; }
    bool operator==(std::nullptr_t) const { return p_ == nullptr; }
    bool operator!=(std::nullptr_t) const { return p_ != nullptr; }

    void Reset() {
        if (p_) {
            p_->Release();
            p_ = nullptr;
        }
    }
    void Attach(T* p) {
        Reset();
        p_ = p;
    }
    T* Detach() {
        T* p = p_;
        p_ = nullptr;
        return p;
    }
    void Swap(ComPtr& other) { std::swap(p_, other.p_); }

    template <typename U>
    HRESULT As(ComPtr<U>* out) const {
        if (!p_) return E_NOINTERFACE;
        return p_->QueryInterface(__uuidof(U), reinterpret_cast<void**>(out->ReleaseAndGetAddressOf()));
    }

private:
    T* p_ = nullptr;
};

}  // namespace WRL
}  // namespace Microsoft

// vkd3d's __uuidof takes the interface type, not a reference as the Windows macro does: take the type from the
// ComPtr instead.
#ifndef IID_PPV_ARGS
template <typename T>
const GUID& UuidOfComPtr(Microsoft::WRL::ComPtr<T>*) {
    return __uuidof(T);
}
template <typename T>
void** IID_PPV_ARGS_Helper(Microsoft::WRL::ComPtr<T>* p) {
    return reinterpret_cast<void**>(p->GetAddressOf());
}
template <typename T>
const GUID& UuidOfPtr(T**) {
    return __uuidof(T);
}
template <typename T>
void** IID_PPV_ARGS_Helper(T** p) {
    return reinterpret_cast<void**>(p);
}
#define IID_PPV_ARGS(ppType) UuidOfComPtr(ppType), IID_PPV_ARGS_Helper(ppType)
#endif
