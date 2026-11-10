# vkd3d for Android (prebuilt)

`libvkd3d.so`, `libvkd3d-shader.so` and `libvkd3d-utils.so` are WineHQ vkd3d
2.1 built for `arm64-v8a` Android API 26, plus the public headers. They are the
Direct3D 12 implementation the advanced renderer uses on Vulkan.

- Source: https://dl.winehq.org/vkd3d/source/vkd3d-2.1.tar.xz
- License: LGPL-2.1-or-later, copyright the vkd3d project authors. See
  `LICENSE` and `COPYING`.
- Local change: on Android, vkd3d is built with its **virtual descriptor
  heaps** path (`device->use_vk_heaps = false`). Mobile Vulkan drivers report
  descriptor indexing but their update-after-bind descriptor pools are too
  small for D3D12-sized heaps. The one-file change is in
  `patches/android-virtual-heaps.md`.

The unmodified upstream source is the preferred way to replace these libraries.
The binaries here are offered under the LGPL and can be rebuilt from that source
with the patch applied.

## Rebuild

`tools/build_vkd3d_android.sh` performs the build in MSYS2 with the Android NDK
26.2. It needs `bison`, `flex`, `make`, `pkgconf`, `perl-JSON` and
`coreutils`:

```bash
pacman -S --needed bison flex make pkgconf perl-JSON coreutils
./tools/build_vkd3d_android.sh
```

The script downloads vkd3d 2.1, applies the Android virtual-heap change,
cross-compiles with `aarch64-linux-android26-clang`, and copies the three
libraries here. Set `NDK` to a different NDK if needed. The build is also
repeatable by hand from the upstream tarball with the same flags; no vkd3d
source is hidden or modified beyond the documented patch.
