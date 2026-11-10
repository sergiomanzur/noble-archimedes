# Android virtual descriptor heaps

File: `libs/vkd3d/device.c`, in `vkd3d_init_device_caps()` replacing the
assignment of `device->use_vk_heaps`.

```diff
-    device->use_vk_heaps = vulkan_info->EXT_descriptor_indexing
-            && !(device->vkd3d_instance->config_flags & VKD3D_CONFIG_FLAG_VIRTUAL_HEAPS)
-            && descriptor_indexing->descriptorBindingUniformBufferUpdateAfterBind
-            && descriptor_indexing->descriptorBindingSampledImageUpdateAfterBind
-            && descriptor_indexing->descriptorBindingStorageImageUpdateAfterBind
-            && descriptor_indexing->descriptorBindingUniformTexelBufferUpdateAfterBind
-            && descriptor_indexing->descriptorBindingStorageTexelBufferUpdateAfterBind;
+#ifdef __ANDROID__
+    /* Mobile Vulkan drivers report descriptor indexing but their update-after-bind
+     * descriptor pools are too small for a D3D12-sized heap. */
+    device->use_vk_heaps = false;
+#else
+    device->use_vk_heaps = vulkan_info->EXT_descriptor_indexing
+            && !(device->vkd3d_instance->config_flags & VKD3D_CONFIG_FLAG_VIRTUAL_HEAPS)
+            && descriptor_indexing->descriptorBindingUniformBufferUpdateAfterBind
+            && descriptor_indexing->descriptorBindingSampledImageUpdateAfterBind
+            && descriptor_indexing->descriptorBindingStorageImageUpdateAfterBind
+            && descriptor_indexing->descriptorBindingUniformTexelBufferUpdateAfterBind
+            && descriptor_indexing->descriptorBindingStorageTexelBufferUpdateAfterBind;
+#endif
```

Everything else is upstream vkd3d 2.1. The Adreno 740 path also uses a smaller
renderer SRV ring (`3 * 8192` instead of `3 * 32768`); that is in the game
runtime (`android/overlay/sdk/runtime/gpu/d3d12.cpp`), not in vkd3d.
