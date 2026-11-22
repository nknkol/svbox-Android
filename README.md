# svbox-Android

Run stock, unmodified **Android 11 (API 30, ARM64)** containers on **HarmonyOS PC (2in1, HongMeng Kernel, API 26)** on top of the generic MIT [`svbox`](https://github.com/nknkol/svbox) userspace syscall-rewriting container runtime.

> **Architectural Boundary**: `svbox` core stays 100% generic (zero Android-specific code). All Android-specific userspace daemons, rootfs bootstrap scripts, and the HarmonyOS PC HAP shell live in this standalone repository.

## Components

- **`binderd/`** (`svbox-binderd`): Pure MIT C userspace Binder router implementing 64-bit Android Binder ABI v8 (`/dev/binder`, `/dev/hwbinder`, `/dev/vndbinder`), shared `memfd` transaction buffers, `SCM_RIGHTS` FD passing (`BINDER_TYPE_FD` + `BINDER_TYPE_FDA`), HIDL scatter-gather (`BINDER_TYPE_PTR`), node reference counting (`BR_INCREFS`/`BR_ACQUIRE`), and Linux Binder nested synchronous transaction routing.
- **`propd/`** (`svbox-propd`): Pure MIT C Android 11 Bionic binary property trie generator (`/dev/__properties__`) and live `/dev/socket/property_service` (`PROP_MSG_SETPROP2` v2 & legacy v1) server.
- **`scripts/boot_e4.py`**: CLI bring-up and verification harness for booting stock Redroid Android 11 (`servicemanager`, `hwservicemanager`, `installd`, `vold`, `idmap2d`, `keystore`, graphics/power/sensors HALs, `SurfaceFlinger`, `zygote64` -> `system_server`).
- **`hap/`**: Initial HarmonyOS PC (2in1, API 26) ArkTS + NDK C++ HAP with a native `XComponent` (`OHNativeWindow`) display receiving zero-copy RGBA frames over `/ipc/hwcomposer.sock` (Graphics Path 1).
- **`docs/DESIGN.md`**: Full architecture, HarmonyOS PC platform findings, and graphics/input roadmap (Paths 1, 2, and 3).

## Quick Build (Host Daemons)

```bash
make SVBOX_DIR=/storage/Users/currentUser/Workspace/svbox
```

## Quick Build (HarmonyOS PC HAP)

```bash
cd hap
export CLT=/data/service/hnp/hmos-clt.org/hmos-clt_1.0.0
export DEVECO_SDK_HOME=$CLT/sdk
export NODE_HOME=$CLT/node/bin
export PATH=$CLT/hvigor/bin:$CLT/ohpm/bin:$CLT/node/bin:$PATH
ohpm install --all
hvigorw assembleHap --no-daemon
# Output: hap/entry/build/default/outputs/default/entry-default-unsigned.hap
```

## Verified Milestones on HarmonyOS PC (HAD-W32, API 26)

- [x] Stock Android 11 Bionic + APEX + ART JIT + `dex2oat64` + `app_process64`
- [x] Userspace Binder (`servicemanager` + `hwservicemanager` + `lshal list`)
- [x] `zygote64` preloading 12,107 classes in 591ms & forking `system_server` (`uid=1000`)
- [x] 77+ live Android 11 system services (`SurfaceFlinger`, `activity`, `package`, `window`, `display`, `input`, `power`, `user`, `settings`, ...) + `cmd package list packages` returning 142 scanned APKs (`rc=0`)
- [x] Live 30fps rendered RGBA frame streaming from `BootAnimation` + `OpenGL ES 3.0 SwiftShader` + `SurfaceFlinger` + `gralloc.redroid` + `hwcomposer.redroid` via `/ipc/hwcomposer.sock` (`SCM_RIGHTS` ashmem FD)
- [x] Initial HarmonyOS PC `XComponent` HAP (`hap/`)
