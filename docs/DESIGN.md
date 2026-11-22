# svbox-Android Architecture & Graphics Roadmap

## 1. Design Principle: Zero Android Specialization in `svbox`

`svbox` (on branch `feat/vdev-binder`) remains a completely generic Linux userspace container runtime. It exposes only generic POSIX/OCI container primitives:

- `--vdev GUEST_PATH:HOST_SOCK[:chr|binder]` — userspace character device proxy with `AF_UNIX` + `SCM_RIGHTS` + shared `memfd`.
- `--cap-add ALL|<hex>` — OCI-style capability bounding set extension.
- `--emulate-mounts` — private mount table including `unshare(CLONE_NEWNS)` and propagation flags.
- Generic HarmonyOS platform compatibility fixes (`PR_HM_JIT`, high-memory `mmap` hint for 8 GiB arenas, `st_dev == 0 -> 1` normalization for `hmdfs`, and `-EINVAL` for `PR_SET/GET_TAGGED_ADDR_CTRL` due to `TBID0`).

All Android-specific runtime services (`svbox-binderd`, `svbox-propd`, Android rootfs management, display/input bridges, and the HarmonyOS PC HAP UI) live exclusively in **`svbox-Android`**.

---

## 2. Verified HarmonyOS PC Platform Findings

| Finding | Root Cause on HarmonyOS PC (2in1, API 26) | Solution |
| --- | --- | --- |
| Kernel `/dev/binder` cannot run `servicemanager` | SELinux `neverallow` blocks `BINDER_SET_CONTEXT_MGR` for all domains except `samgr` | Userspace `svbox-binderd` via `svbox --vdev /dev/binder:...:binder` |
| PC HAP JIT requires `PR_HM_JIT` | Raw `mmap(RWX)` / `mprotect(+X)` returns `EINVAL` in HAP sandbox | `svbox` wraps `mprotect(+X)` with `prctl(0x6a6974, 0, 0/1)` + HAP requests `ohos.permission.kernel.EXEMPT_ANONYMOUS_EXECUTABLE_MEMORY` |
| `hmdfs` reports `st_dev == 0` | Bionic `linker64` skips `(st_dev, st_ino)` deduplication when `st_dev == 0`, loading duplicate copies of `.so` files (splitting `libEGL_swiftshader` / `libGLESv2_swiftshader` TLS) | Normalize `st_dev == 0` to `1` in `svbox` stat post-processing |
| Scudo `0xb4` pointer tagging traps JIT execution | Kirin ARM64 enables `TBID0=1` (data-only TBI); executing `blr x8` on a `0xb4`-tagged heap pointer traps infinitely in kernel page-fault retry | Return `-EINVAL` on `PR_SET/GET_TAGGED_ADDR_CTRL` in `svbox` on HarmonyOS |
| `fstat(memfd)` returns `EACCES` | HarmonyOS SELinux denies `getattr` on `memfd` inodes, breaking Android `libcutils` ashmem-on-memfd | Set `sys.use_memfd=false` so Android uses native `/dev/ashmem` (verified working in PC HAP) |
| UNIX socket bind location | Binding `AF_UNIX` sockets in `/tmp` (`EACCES`) or `~` (`EPERM`) is denied | Always place sockets under the HAP sandbox `temp/` or `files/` directory |

---

## 3. Graphics & XComponent Integration Roadmap (Zero AOSP Rebuild)

Because Android's graphics stack is modular (Treble HAL + dynamic EGL/Vulkan ICD plugins), we **never need to recompile AOSP `system.img`** to connect Android framebuffers to HarmonyOS PC's `XComponent` (`OHNativeWindow`).

### Path 1 (Active in v0.1 — Verified Live at 30fps): Prebuilt `/ipc/hwcomposer.sock` Zero-Copy Ashmem Stream

Uses the stock Redroid 11 `gralloc.redroid.so` + `hwcomposer.redroid.so` + `libEGL_swiftshader.so` binaries completely unmodified:

```text
Android App / BootAnimation
    │  (OpenGL ES 3.0 SwiftShader / Skia CPU)
    ▼
SurfaceFlinger ──► gralloc.redroid.so (/dev/ashmem buffer)
    │
    ▼
hwcomposer.redroid.so  (ro.boot.use_redroid_stream=1)
    │  AF_UNIX SOCK_STREAM: /ipc/hwcomposer.sock
    │  Per frame:
    │    1. recv(64 bytes)          -> private_handle_t header (size, magic, etc.)
    │    2. recvmsg(1B + SCM_RIGHTS)-> prime_fd (zero-copy /dev/ashmem FD)
    │    3. mmap(prime_fd)          -> RGBA_8888 pixels
    │    4. send(2 bytes "ok")      -> frame ACK
    ▼
HarmonyOS PC HAP (hwc_renderer.cpp)
    │  OH_NativeWindow_NativeWindowRequestBuffer()
    │  memcpy RGBA rows into BufferHandle
    │  OH_NativeWindow_NativeWindowFlushBuffer()
    ▼
ArkUI XComponent Surface
```

- **Pros**: Zero Android-side custom binaries; verified working end-to-end on HarmonyOS PC (`first_frame.png` rendered cleanly at 30fps).
- **Limitations**: Software SwiftShader CPU rendering; fixed display geometry set at boot via `ro.boot.redroid_width/height`.

### Path 2 (Planned for v0.2 — Recorded): Custom `hwcomposer.svbox.so` HAL Plugin

Replace **only** the single vendor HAL `.so` via `svbox -v host/hwcomposer.svbox.so:/vendor/lib64/hw/hwcomposer.redroid.so` (still zero AOSP rebuild):

- Pure MIT C/C++ (~400 lines) implementing the standard AOSP `hwcomposer2.h` C ABI.
- **Capabilities unlocked**:
  - **Dynamic PC window resize**: Forward HarmonyOS `XComponent` `OnSurfaceChanged(w, h)` events to trigger HWC2 `hotplug` / `setActiveConfig` so Android dynamically adapts resolution & DPI when the user resizes the PC window.
  - **Native VSync pacing**: Drive `SurfaceFlinger` VSync directly from HarmonyOS `OH_NativeVSync` callbacks (`libnative_vsync.so`), eliminating frame judder.
  - **Dirty-rect / partial present** and **multi-display / freeform per-app windowing** (one `XComponent` per Android virtual display).
  - Direct **zero-copy buffer sharing** when paired with `OH_NativeBuffer` / DMA-buf handles.

### Path 3 (Planned for v0.3 — Recorded): Hardware GPU Acceleration via EGL/GLES/Vulkan Forwarding

Replace **only** `/vendor/lib64/egl/libEGL_svbox.so` + `libGLESv2_svbox.so` (and optionally `vulkan.svbox.so`) selected by `ro.hardware.egl=svbox` (still zero AOSP rebuild):

- Implements an API-forwarding GLES 3.x / Vulkan ICD inside the Android container (architecturally similar to `gfxstream` / `virgl` / `ANGLE`).
- Streams GLES/Vulkan command buffers over shared `ashmem` ring buffers to a host render thread inside the HarmonyOS PC HAP.
- The host render thread executes directly on HarmonyOS PC's native GPU driver (`/system/lib64/libEGL.so`, `libGLESv3.so`, `libvulkan.so`, `libnative_window.so` — already verified 7/7 `dlopen OK` in E1), rendering directly onto the `XComponent` `OHNativeWindow` at full native GPU speed with minimal CPU overhead.

---

## 4. Input Roadmap (Touch, Mouse, Keyboard)

1. **Primary (Zero AOSP Modification)**: Expose a virtual `evdev` character device via `svbox --vdev /dev/input/event0:<sock>:chr` (or Redroid's `/ipc/uinputd.sock`). The HAP's `XComponent` `DispatchTouchEvent` / `OnMouseEvent` / `OnKeyEvent` callbacks encode standard Linux `struct input_event` packets (`EV_ABS` multitouch + `EV_KEY` + `EV_SYN`) directly into Android's `InputFlinger`.
2. **Fallback / Automation**: Inject key/tap/text events via `cmd input` over Binder.
