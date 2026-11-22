# svbox-Android 鸿蒙 PC 安卓容器实战手记（Experience Log）

> **用途**：持续记录从单二进制内存加载器（`elf-patch-loader`）演进到通用无特权用户态容器（`svbox`），最终在普通签名 **HarmonyOS PC (2in1, API 26, ARM64 / Maleoon 935)** HAP 内跑起原版未改动 **Android 11 桌面 + 硬件加速** 的完整工程历程、踩坑根因、跨项目借鉴经验与真机数据对比。
> **记录规则**：每完成一个工程步骤，均在此追加实测数据、踩坑根因与架构取舍，作为长期外部记忆与后续技术博客原始素材。

---

## 第 0 章：技术演进脉络与跨项目经验沉淀

### 0.1 三代演进主线
1. **第一代 — [`harmonybrew-cask/patches/elf-patch-loader`](file:///storage/Users/currentUser/Workspace/harmonybrew-cask/patches/elf-patch-loader/README.md)**
   * **定位**：29 KB `-nostdlib` 静态 PIE 内存态 ELF 加载器。
   * **核心突破**：在不修改上游 Linux ARM64 二进制磁盘字节、不对大体积主程序重签名的前提下，通过 `pread64` 将上游 ELF 读入匿名内存，借助鸿蒙内核 `prctl(PR_HM_JIT = 0x6a6974)` 切换可执行权限并刷新指令缓存（`dc cvau` + `ic ivau`），内存注入 `DT_RUNPATH` / `PT_INTERP`，并用 `TRAP_SIGSYS`（零栈 AArch64 汇编信号处理 + 防目标程序覆盖 `sigaction/sigprocmask` 的守卫）消除 Seccomp `SIGSYS` 崩溃。
2. **第二代 — [`svbox`](file:///storage/Users/currentUser/Workspace/svbox/README.md)（分支 `feat/vdev-binder`）**
   * **定位**：100% 通用的无特权用户态 Linux 容器运行时（**严守零 Android 硬编码原则**）。
   * **核心突破**：在内存态 ELF 加载基础上，通过 `svc #0` 指令静态改写跳转至常驻 payload，透明接管系统调用：路径视图与虚拟挂载表（含 `unshare(CLONE_NEWNS)` 私有克隆）、跨进程 UID/GID 与 `owner.db` 文件属主模拟、`/proc` 视图伪造、用户态字符设备代理 `--vdev GUEST:SOCK[:chr|binder]`。
3. **第三代 — [`svbox-Android`](file:///storage/Users/currentUser/Workspace/svbox-Android/README.md)**
   * **定位**：运行在普通签名鸿蒙 PC HAP（`org.svbox.android`）里的原版 Android 11（API 30, ARM64）容器。
   * **里程碑基线（v0.1，2026-11-22 已达成）**：
     * 未重编任何一行 AOSP，直接启动原版 Redroid 11 镜像至完整 **720×1280 @ 320dpi 桌面**（`sys.boot_completed=1`、`Starting phase 1000`、`QuickstepLauncher` + `SystemUI` + `ImageWallpaper` + 142 个系统包在线）；
     * 通过 `svbox-binderd`（用户态多路 Binder 总线，支持 `BINDER_SET_MAX_THREADS` / `BR_SPAWN_LOOPER`、按节点 FIFO 序列化 `TF_ONE_WAY`、进程退出自动广播 `BR_RELEASE` / `BR_DEAD_BINDER`）绕过鸿蒙内核 SELinux 对 `BINDER_SET_CONTEXT_MGR` 的 `neverallow` 封锁；
     * 通过 `hwcomposer.redroid.so` (`/ipc/hwcomposer.sock` + `SCM_RIGHTS` ashmem FD) → HAP `XComponent` (`OHNativeWindow`) 实现首版实时画面输出（~11 fps SwiftShader 软渲染）。

---

### 0.2 已踩平的鸿蒙 PC 沙箱与内核硬核坑（E1 → E4 基线汇总）

| 编号 | 现象 | 根因（HarmonyOS PC API 26） | 通用解法 |
| --- | --- | --- | --- |
| **P1** | 内核 `/dev/binder` 无法注册 `servicemanager` | 鸿蒙内核 SELinux `neverallow` 仅允许 `samgr` 调用 `BINDER_SET_CONTEXT_MGR` | `svbox --vdev /dev/binder:...:binder` + 用户态 `svbox-binderd` |
| **P2** | HAP 内并发拉起守护进程偶发 `mprotect(+X): EINVAL` | 同一 HAP UID 下多进程并发开关 `PR_HM_JIT` (`prctl(0x6a6974, 0, 0/1)`) 存在内核窗口竞争 | `svbox` 在 `elf_load.c`/`platform.c` 对 `PR_HM_JIT` 加 32 次 `sched_yield` 重试，HAP 侧配合启动错峰 |
| **P3** | `linker64` 重复加载同一个 `.so` 导致 TLS 分裂（SwiftShader 黑屏/崩溃） | 鸿蒙 `hmdfs` 对部分文件返回 `st_dev == 0`，Bionic `linker64` 将其视为不可去重 | `svbox` 对 `stat` 结果将 `st_dev == 0` 规范化为 `1` |
| **P4** | Scudo `0xb4` 指针标签在 JIT 跳转时无限页故障卡死 | 麒麟 ARM64 启用了 `TBID0=1`（仅数据 TBI，不支持指令指针标签） | `svbox` 在鸿蒙平台对 `PR_SET/GET_TAGGED_ADDR_CTRL` 返回 `-EINVAL` |
| **P5** | `fstat(memfd)` 返回 `EACCES` 破坏 ashmem-on-memfd | 鸿蒙 SELinux 禁止对 `memfd` inode 调用 `getattr` | 设置 `sys.use_memfd=false`，回退使用原生可用的 `/dev/ashmem` |
| **P6** | `AlarmManagerService` 触发 `SIGSYS`（syscall 170 `settimeofday`）崩溃 | HAP Seccomp 对特权时间/主机名/重启等 syscall 下发 `SECCOMP_RET_TRAP` | `svbox` 显式拦截返回 `-EPERM` + 后续引入通用 `SIGSYS -> -ENOSYS` 守卫 |
| **P7** | `UEventObserver` 启动崩溃（`AF_NETLINK` 返回 `EACCES`） | HAP 沙箱禁用 `NETLINK_KOBJECT_UEVENT` | `svbox` 通用降级为空闲 `AF_UNIX SOCK_DGRAM` 套接字 |
| **P8** | `Launcher3` 启动时 `/data/user/0/...` 凭空消失 | 某子进程调用 `unshare(CLONE_NEWNS)` + `mount(tmpfs, /data/user)` 污染了容器共享挂载表 | `svbox` 实现 `mounts_unshare()`，将共享 `mount_table_fd` 克隆为进程私有 `memfd` |
| **P9** | `zygote64` 报错 `no namespace called com_android_art` | 把 `rootfs/system/bin` bind-mount 到 `/bin` 导致反向路径映射覆盖 `/system/bin/*`，破坏 `/linkerconfig/ld.config.txt` 的 `dir.system` 匹配 | 移除多余 `/bin` 绑定，让 `/system/bin/app_process64` 保持原始虚拟路径 |
| **P10** | 开机动画退场后最后一帧残留在桌面上层 | `svbox-binderd` 进程断开时未发送 `BR_RELEASE` / `BR_DEAD_BINDER`，`SurfaceFlinger` 未回收图层 | 仅在 `drop_conn` 时发送 `BR_RELEASE`（仅当句柄数归零）与 `BR_DEAD_BINDER`（避免活连接并发 UAF） |
| **P11** | `PermissionController` 在 `bindApplication` 前执行 `scheduleCreateService` 报 NPE | `TF_ONE_WAY` 异步事务并发派发到不同 Binder 线程破坏了顺序 | `svbox-binderd` 对每个 `bnode` 的 `TF_ONE_WAY` 事务做 FIFO 串行化，直到收到 `BC_FREE_BUFFER` |
| **P12** | `StartAudioService` 单项耗时 **10,224ms**（占 15.5s 总启动时间 66%） | 仅注册了空音频桩，未运行合规 Audio HAL / AudioFlinger | Step 2 接入真实 `audioserver` + `OHAudio` 桥彻底消灭 |
| **P13** | `webview_zygote` 报错 `Failed to create symlink /data/user/0 Permission denied` | HAP 沙箱 `files/` 目录禁止宿主 `symlink()` 系统调用 | Step 1 在 `svbox` VFS 层做通用符号链接/挂载兜底 |

---

### 0.3 本机工作区跨项目经验复用清单

在启动 Step 1–4 前，系统梳理了同设备（Maleoon 935, API 26）上已验证通过的 6 个项目经验：

1. **[`GPUIWorK/gpui-ohos`](file:///storage/Users/currentUser/Workspace/GPUIWorK/gpui-ohos/src/ohos)**（核心参考）
   * [`sched_qos.rs`](file:///storage/Users/currentUser/Workspace/GPUIWorK/gpui-ohos/src/ohos/sched_qos.rs)：**极重要沙箱实坑**——shell 下可读的 `/sys/devices/system/cpu/*/cpu_capacity` 在 HAP 沙箱内被拦截！改用 ~2ms/核单核 ALU 微测探针自动识别大/中/小核集群；同时验证 `dlopen("libqos.so")` + `OH_QoS_SetThreadQoS` 与 `HMS_FAST_SchedulingOptimization_PerfHint` 不可对同一 `tid` 混用。
   * [`seccomp_guard.rs`](file:///storage/Users/currentUser/Workspace/GPUIWorK/gpui-ohos/src/ohos/seccomp_guard.rs)：20 行零栈 AArch64 `SIGSYS` handler，直接将 `si_code == SYS_SECCOMP` 的陷阱写成 `uc_mcontext.regs[0] = -ENOSYS`。
   * [`frame_delivery.rs`](file:///storage/Users/currentUser/Workspace/GPUIWorK/gpui-ohos/src/ohos/frame_delivery.rs) & [`ohos-performance.md`](file:///storage/Users/currentUser/Workspace/GPUIWorK/gpui-ohos/docs/ohos-performance.md)：`OH_NativeVSync` 按需帧调度与切后台自动注销停帧。
   * [`wgpu_context.rs`](file:///storage/Users/currentUser/Workspace/GPUIWorK/gpui-ohos/src/ohos/wgpu_context.rs)、[`audio.rs`](file:///storage/Users/currentUser/Workspace/GPUIWorK/gpui-ohos/src/ohos/audio.rs)、[`keyboard.rs`](file:///storage/Users/currentUser/Workspace/GPUIWorK/gpui-ohos/src/ohos/keyboard.rs)、[`clipboard.rs`](file:///storage/Users/currentUser/Workspace/GPUIWorK/gpui-ohos/src/ohos/clipboard.rs)：Maleoon 935 上的 GLES/Vulkan 双后端、`OHAudio`、API 26 硬件键与剪贴板实机范本。
2. **[`blender-app`](file:///storage/Users/currentUser/blender-app/entry/src/main/cpp/xcomponent_input.h)**
   * 同一 GPU 上验证通过的 `XComponent` 鼠标/触摸/滚轮/硬键盘/IME 与窗口 resize 事件映射逻辑。
3. **[`harmonybrew-cask/patches/elf-patch-loader`](file:///storage/Users/currentUser/Workspace/harmonybrew-cask/patches/elf-patch-loader/README.md)**
   * `TRAP_SIGSYS` 不仅注册 `SIGSYS` handler，还配套拦截 `rt_sigaction` / `rt_sigprocmask` 防止 guest 覆盖或屏蔽 `SIGSYS`——这一守卫思想直接吸收进 `svbox`。
4. **[`GPUIWorK/INAndroid/twoyi_harmonyos_feasibility.md`](file:///storage/Users/currentUser/Workspace/GPUIWorK/INAndroid/twoyi_harmonyos_feasibility.md)**
   * 鸿蒙 PC 应用沙箱实测能力矩阵（`/dev/ashmem`、`/dev/hvgr0`、`libEGL`/`libGLESv3`/`libvulkan`/`libnative_window`/`libnative_vsync` 全部 `dlopen OK`；`/dev/input/*` `EACCES` 需用户态 `evdev`）。
5. **[`openwarp-ohos`](file:///storage/Users/currentUser/openwarp-ohos/docs/harmonyos-port-plan.md)** & **[`tdesktop`](file:///storage/Users/currentUser/tdesktop/docs/harmonyos-port-plan.md)**
   * `openwarp-ohos` 的 `crates/ohos_core` / ELF TLS 补丁经验；`tdesktop` 的交叉编译链、拖拽/剪贴板、窗口恢复、权限与通知文档（主要用于后续 Step 4 交叉编译与第 4 阶段桌面化打磨）。

---

### 0.4 官方 API 26 NDK 与 Linux 兼容资源控制实测核验（2026-11-22）

* **官方 NDK API**（核验自本机 API 26 SDK 头文件与离线文档镜像）：
  * `<qos/qos.h>` (`libqos.so`, KernelEnhanceKit, API 12+)：`OH_QoS_SetThreadQoS` 6 级调度（`QOS_BACKGROUND` ~ `QOS_USER_INTERACTIVE`）。
  * `<FASTKit/scheduling_optimization.h>` (API 26+)：`HMS_FAST_SchedulingOptimization_PerfHint` 前台场景提频。
  * `<hidebug/hidebug.h>` (`libohhidebug.so`, API 12+)：`OH_HiDebug_GetAppCpuUsage` / `GetSystemCpuUsage` / `GetAppNativeMemInfo` / `GetGraphicsMemory` / `GetAppMemoryLimit`。
* **HAP Seccomp 允许列表**（核验自 [`seccomp-symbol`](https://developer.huawei.com/consumer/cn/doc/harmonyos-references/seccomp-symbol)）：
  * 全平台开放：`sched_setaffinity` / `sched_getaffinity`、`sched_setscheduler` / `sched_setattr`、`setpriority` / `getpriority`、`setrlimit` / `prlimit64`、`ioprio_set`（arm64）、`kill` / `tgkill`、`process_madvise`。
* **本机 20 核鸿蒙 PC 实测结论**：
  * 物理拓扑：4 小核 `cpu0~3` (`capacity=688, 2.05GHz`) + 8 中核 `cpu4~11` (`capacity=888, 2.01GHz`) + 8 大核 `cpu12~19` (`capacity=1024, 2.32GHz`)。
  * `sched_setaffinity` **生效且 `fork/exec` 自动继承**；`SCHED_BATCH` / `SCHED_IDLE`、`nice 10/19`、`setrlimit(RLIMIT_CPU/RLIMIT_AS)` 均**生效**。

---

## 推进路线与里程碑日志

* **Step 0**：建立本经验手记并提交基线。
* **Step 1**：`svbox` 通用容器底座升级（`SIGSYS` 守卫、`--cpuset`/`--mem-mb`/`--qos`、虚拟 `symlink`、通用 `evdev` vdev、进程树清理）。
* **Step 2**：`svbox-Android` 交互闭环 + 真声音 (`OHAudio`) + 事件就绪快速启动 + 网络/DNS 连通。
* **Step 3**：异步显示管线 + `OH_NativeVSync` 按需帧调度 + HAP 大小核 ALU 探针 + `HiDebug` 实时监控 + Step 4 GPU 引擎选型实测报告。
* **Step 4**（下一阶段，切 `xhigh`）：开源 GPU 硬件加速引擎（`gfxstream` / `Mesa Venus + ANGLE`）裁剪移植与 GL/VK 双驱动切换。

---
*(后续每个 Step 完成后，将在下方按章节持续追加实测与踩坑记录。)*
