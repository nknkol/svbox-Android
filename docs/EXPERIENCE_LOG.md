# svbox-Android 鸿蒙 PC 安卓容器实战手记（Experience Log）

> **用途**：持续记录从单二进制内存加载器（`elf-patch-loader`）演进到通用无特权用户态容器（`svbox`），最终在普通签名 **HarmonyOS PC (2in1, API 26, ARM64 / Maleoon 916)** HAP 内跑起原版未改动 **Android 11 桌面 + 硬件加速** 的完整工程历程、踩坑根因、跨项目借鉴经验与真机数据对比。
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

在启动 Step 1–4 前，系统梳理了同设备（Maleoon 916, API 26）上已验证通过的 6 个项目经验：

1. **[`GPUIWorK/gpui-ohos`](file:///storage/Users/currentUser/Workspace/GPUIWorK/gpui-ohos/src/ohos)**（核心参考）
   * [`sched_qos.rs`](file:///storage/Users/currentUser/Workspace/GPUIWorK/gpui-ohos/src/ohos/sched_qos.rs)：**极重要沙箱实坑**——shell 下可读的 `/sys/devices/system/cpu/*/cpu_capacity` 在 HAP 沙箱内被拦截！改用 ~2ms/核单核 ALU 微测探针自动识别大/中/小核集群；同时验证 `dlopen("libqos.so")` + `OH_QoS_SetThreadQoS` 与 `HMS_FAST_SchedulingOptimization_PerfHint` 不可对同一 `tid` 混用。
   * [`seccomp_guard.rs`](file:///storage/Users/currentUser/Workspace/GPUIWorK/gpui-ohos/src/ohos/seccomp_guard.rs)：20 行零栈 AArch64 `SIGSYS` handler，直接将 `si_code == SYS_SECCOMP` 的陷阱写成 `uc_mcontext.regs[0] = -ENOSYS`。
   * [`frame_delivery.rs`](file:///storage/Users/currentUser/Workspace/GPUIWorK/gpui-ohos/src/ohos/frame_delivery.rs) & [`ohos-performance.md`](file:///storage/Users/currentUser/Workspace/GPUIWorK/gpui-ohos/docs/ohos-performance.md)：`OH_NativeVSync` 按需帧调度与切后台自动注销停帧。
   * [`wgpu_context.rs`](file:///storage/Users/currentUser/Workspace/GPUIWorK/gpui-ohos/src/ohos/wgpu_context.rs)、[`audio.rs`](file:///storage/Users/currentUser/Workspace/GPUIWorK/gpui-ohos/src/ohos/audio.rs)、[`keyboard.rs`](file:///storage/Users/currentUser/Workspace/GPUIWorK/gpui-ohos/src/ohos/keyboard.rs)、[`clipboard.rs`](file:///storage/Users/currentUser/Workspace/GPUIWorK/gpui-ohos/src/ohos/clipboard.rs)：Maleoon 916 上的 GLES/Vulkan 双后端、`OHAudio`、API 26 硬件键与剪贴板实机范本。
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

## 第 1 章（Step 1）：`svbox` 通用容器底座升级（commits `a2eba39` + `3e82322` on `feat/vdev-binder`）

### 1.1 本步目标与设计约束
保持 `svbox` 100% 通用、零 Android 硬编码，一次性补齐后续交互、资源控制与沙箱容错所需的 5 项通用 Linux 容器原语：
1. **`--trap-sigsys`（通用 Seccomp `SIGSYS -> -ENOSYS` 兜底）**：
   * 吸收 [`gpui-ohos/seccomp_guard.rs`](file:///storage/Users/currentUser/Workspace/GPUIWorK/gpui-ohos/src/ohos/seccomp_guard.rs) 与 [`harmonybrew-cask/patches/elf-patch-loader`](file:///storage/Users/currentUser/Workspace/harmonybrew-cask/patches/elf-patch-loader/README.md) 的双保险思路：
   * 在 loader（含每次 child execve 重装）安装零栈 AArch64 `SIGSYS` 汇编处理函数与 `rt_sigreturn` restorer，将 `si_code == SYS_SECCOMP` 的内核陷阱直接改写 `uc_mcontext.regs[0] = -38` (`-ENOSYS`)；
   * 在 payload `integrity.c` 同时拦截 `rt_sigaction(SIGSYS)`（虚拟化记录，不覆盖内核 handler）与 `rt_sigprocmask`（从 guest 的 `SIG_BLOCK`/`SIG_SETMASK` 掩码中清除 `SIGSYS` 位），防止 ART / Bionic 意外覆盖或屏蔽陷阱守卫。
2. **`--cpuset LIST` + `--mem-mb N` + `--nice N` + `--sched other|batch|idle`**：
   * 启动时直接调用原生内核 `sched_setaffinity` / `setpriority` / `sched_setscheduler`（经 `fork/exec` 自动继承全进程树）；
   * 在 `dispatch.c` 拦截 `sched_setaffinity` 对 guest 掩码与 `cpu_mask` 取交集钳位（若交集为空则回退到 `cpu_mask`，兼容 `12-19` 这类非 0 起始的大核掩码）；
   * 在 `vfs_procfs.c` + `vfs_bridge.c` **早于宿主 `pb_resolve` 拦截** `/proc/cpuinfo`、`/sys/devices/system/cpu/{online,present,possible}` 和 `/proc/meminfo`（即使 HAP 沙箱禁止访问宿主 `/sys` 也能稳定返回容器内虚拟核数与虚拟内存上限，防止 ART/Skia 盲目开 20 线程）。
3. **通用 `symlinkat` 沙箱兜底**：
   * 针对鸿蒙 HAP `files/` / `temp/` 禁止 `symlink()`（`EACCES`）导致 `webview_zygote` 在私有 `tmpfs` 中执行 `symlink("/data/data", "/data/user/0")` 崩溃的问题（P13）；
   * 在 `vfs_bridge.c` `mutate()` 中：仅当宿主 `symlinkat` 返回 `-EACCES`/`-EPERM` 且启用了 `--emulate-mounts`、且目标解析到容器内已存在目录/常规文件时，在 `b->first_host` 创建占位节点并在本进程挂载表中调用 `mounts_add()` 建立绑定挂载。零栈分配（复用 `Bridge` 现有缓冲区），完全通用。
4. **通用字符设备代理 `--vdev ...:chr` 完善（为 `evdev` 铺路）**：
   * 修复两个实测隐藏坑：① `openat` 握手完成后，若 guest 传了 `O_NONBLOCK` 必须补调 `fcntl(F_SETFL, O_NONBLOCK)`，否则 Android `EventHub` 的非阻塞 `read()` 循环会永久卡死；② `generic_chr_ioctl` 拷贝返回长度改为 `min(rep.payload_len, req->out_len)`，兼容 Linux `EVIOCGNAME(len)` / `EVIOCGBIT(ev, len)` 等变长返回 ioctl；③ `CONFIG_MAX_VDEVS` 从 8 扩到 16。

### 1.2 真机验证数据
* **`--cpuset 0-3 --mem-mb 4096 --nice 10 --sched batch --trap-sigsys`**：
  * `sched_getaffinity` = `[0, 1, 2, 3]`，guest 主动请求扩至全 20 核后仍被钳位在 `[0, 1, 2, 3]`；
  * `/proc/cpuinfo` 精准呈现 4 核，`/sys/devices/system/cpu/online` = `0-3`，`/proc/meminfo` `MemTotal` = `4194304 kB`，`nice=10`，`sched=SCHED_BATCH(3)`；
* **`--cpuset 12-19`（非连续大核集群）**：guest 试图绑逻辑核 `{0}` 自动回落至 `[12..19]`，`/sys/devices/system/cpu/online` = `0-7`，`/proc/cpuinfo` = 8 核；
* **`svbox` 核心回归全通过**：`check_dispatch.py`（166 slow syscalls）、`check_payload_abi.py`、`test_mounts.py`、`fake_ids.py`、`guest_signals.py`、`test_modify.py` 全部 PASS。

---

## 第 2 章（Step 2）：交互闭环、64 位真声音与 4 秒级启动

### 2.1 虚拟 `evdev` 输入子系统（`InputBridge` + `svbox --vdev`）
* **设计**：在 HAP Native C++ 内部直接起两个 Unix Socket 服务端（`input_touch.sock` 对应 `/dev/input/event0` 多点触控屏 Type B MT 协议，`input_kbd.sock` 对应 `/dev/input/event1` 物理键盘与导航键），实现 `svbox` 通用 `vdev_proto.h` 协议（`VDEV_OP_OPEN` / `VDEV_OP_ATTACH_THREAD` / `VDEV_OP_IOCTL`）。
* **优势**：`XComponent` 的 Touch / Mouse / Key 回调直接在宿主进程内写 24 字节标准 Linux `struct input_event`（`CLOCK_MONOTONIC` 时间戳）到 Android `EventHub` 打开的实时 socket FD，**零中间进程转发延迟**；同时给 `svbox-touchscreen` 挂载了 `/system/usr/idc/svbox-touchscreen.idc`（`touch.deviceType = touchScreen`、`device.internal = 1`）。
* **关键实坑与根因（超适合写进博客！）**：
  1. **坑 2-1：`Touch device 'svbox-touchscreen' did not report support for X or Y axis!`**
     * **现象**：`EventHub` 成功识别了 `event0`（`classes=0x14`），但 `InputReader` 判定 X/Y 轴不支持。
     * **根因**：`svbox/src/payload/logic/vdev.c` 的 `generic_chr_ioctl` 中，`req` 指针与 `recv_msg` 接收缓冲区共用了同一个栈数组 `buf[]`！收到 24 字节的 `struct input_absinfo` 时，偏移 20 处的 `ai.resolution = 0` **正好覆写了 `req->out_len`**，导致随后的 `if (req->out_len > 0)` 判定为假、从未把 `input_absinfo` 拷回 guest 内存！
     * **修复**：调用 `recv_msg` 前先用局部变量 `uint32_t out_len = req->out_len;` 保存长度；`EVIOCGABS` 立刻生效，日志输出：`Device reconfigured: id=3, name='svbox-touchscreen', size 720x1280, orientation 0, mode 1, display id 0`。
  2. **坑 2-2：键盘 `EVIOCSREP` 警告、`.idc` 缺失导致 `ADISPLAY_ID_NONE` 与鸿蒙 NDK `KEY_UNKNOWN` 宏冲突**
     * **现象 A**：`EventHub` 报 `Unable to disable kernel key repeat for /dev/input/event1: Inappropriate ioctl` → 在 `input_bridge.cpp` 中处理 `nr == 0x03`（`EVIOCGREP/EVIOCSREP`）返回 `0`。
     * **现象 B**：未提供 `.idc` 时，`InputReader` 把 `svbox-touchscreen` 绑定到 `ADISPLAY_ID_NONE`，触控坐标无法投射到主屏 → 运行时生成并挂载 `/system/usr/idc/svbox-touchscreen.idc`（`touch.deviceType = touchScreen`、`device.internal = 1`），即刻关联 `display id 0`。
     * **现象 C（编译坑）**：同时 `#include <linux/input-event-codes.h>` 与鸿蒙 `ace/xcomponent/native_interface_xcomponent.h` 时，前者的 `#define KEY_UNKNOWN 0` 会破坏后者枚举 `OH_NativeXComponent_KeyCode::KEY_UNKNOWN` → 在 `napi_init.cpp` 中改用局部 `constexpr` 定义 Linux 键值常量避免宏污染。

### 2.2 消灭 10.2 秒启动卡顿 + 64 位真声音（`OHAudio`）
* **根因定位**：回看 `hap_logs/logcat.txt` 发现 `StartAudioService` 耗时 **10,224ms** 的真正元凶是 `ServiceManager: Waiting for service 'media.metrics'`（两次 5 秒同步超时）+ 缺少合规 Audio HAL / AudioFlinger。
* **关键实坑与根因**：
  1. **坑 2-3：Redroid 11 的 `/vendor/bin/hw/android.hardware.audio.service` 是 32 位 ARM ELF，且 `svbox` 初始可执行文件早于 bind mount 加载！**
     * **现象**：64 位 `svbox` 启动它时直接报 `error: bogus ELF header`，但反查 `/vendor/lib64/hw/android.hardware.audio@6.0-impl.so`、`/vendor/lib64/hw/android.hardware.audio.effect@6.0-impl.so` 和 `/system/bin/audioserver` **全都是 64 位 ARM64**；另外，最初尝试只用 `--bind` 覆盖路径启动自编译二进制时仍命中旧文件，因为 `svbox` loader 是在 payload 虚拟挂载表接管**之前**直接相对 `--root` 打开首个可执行文件的。
     * **解法**：自写 75 行 64 位 `audio_hal/svbox_audio_service.cpp`，直接调用 `libhidlbase.so` 导出的 `configureRpcThreadpool` + `registerPassthroughServiceImplementation` + `joinRpcThreadpool`，以 64 位宿主进程托管原版 `IDevicesFactory@6.0` 与 `IEffectsFactory@6.0`，并在启动前复制到 `rootfs_/vendor/bin/hw/svbox-audio-service` 以独立路径拉起！
  2. **坑 2-4：只链 `libdl.so` 动态 `dlopen(libhidlbase.so)` 触发 Bionic IE-TLS 报错**
     * **现象**：`dlopen failed: TLS symbol "(null)" in dlopened "/system/lib64/libc.so" referenced from "/system/lib64/libc.so" using IE access model`。
     * **根因**：Android Bionic `libc.so` 使用 Initial-Exec（IE）TLS 模型，必须在进程启动阶段作为 `DT_NEEDED` 静态声明加载，不能在运行时被 `dlopen` 延迟拉起。
     * **解法**：编译 `svbox-audio-service` 时直接带 `-Wl,--no-as-needed` 链接 Android 11 的 `libc.so` + `libdl.so` + `libhidlbase.so`，启动后 `register IDevicesFactory@6.0 -> 0`、`register IEffectsFactory@6.0 -> 0` 一次通过！
  3. **坑 2-5：零依赖 `audio.primary.svbox.so` → 鸿蒙 PC `libohaudio.so`（含 `getMicMute: -38` 细节）**
     * 反汇编验证了 Android 11 `audio_hw_device_t`（320B）与 `audio_stream_out_t`（280B）精确字段偏移，用纯 `-nostdlib -shared` + 原生 AArch64 `svc #0`（零 libc 依赖、零 ABI 冲突）编写 `audio_hal/audio_primary_svbox.c`，通过 `/ipc/audio.sock` 把 `AudioFlinger` 混音后的 `44100Hz / Stereo / S16LE` PCM 流送入 HAP 的 `AudioBridge`（256 KiB 环形缓冲 + `OH_AudioStreamBuilder_SetRendererWriteDataCallback` + 600ms 静音自动 `OH_AudioRenderer_Pause` 符合鸿蒙低功耗规范）；初版未实现 `get_mic_mute`/`set_mic_mute`/`get_master_mute` 时 `AudioFlinger` 反复打印 `getMicMute: error -38`，补齐状态字段后告警清零。

### 2.3 `webview_zygote` 彻底复活 + DNS 代理 + 事件就绪启动
* **坑 2-6：`webview_zygote` 过了 `symlink` 又卡在 `setfilecon /data/user/0 Permission denied`**
  * **现象**：Step 1 的 `symlinkat` 兜底成功让 `webview_zygote` 越过 `Zygote.cpp:1365`，紧接着在 `1253` 行因 `setxattr("security.selinux")` 返回 `EACCES` 再次中止。
  * **通用修复**：在 `svbox/src/payload/logic/vfs_bridge.c` 中，容器 fake-root 模式下若对 `security.*` 命名空间调用 `setxattr`/`lsetxattr` 遭遇 `EACCES`/`EPERM`/`EOPNOTSUPP`，返回 `0`（已通过 `test/fs_policy/xattrs.py --normal` 回归）。修复后 `webview_zygote` **零报错存活**！
* **坑 2-7：`AudioFlinger` 的 `totalMemory` 仍读到宿主机 32 GB**
  * **根因**：除了 `/proc/meminfo`，Android 还通过 `sysinfo(2)` 读取 `totalram`。
  * **通用修复**：在 `svbox/src/payload/logic/dispatch.c` 补上 `__NR_sysinfo` 内存上限钳位，修复后 `AudioFlinger` 精准报告 `totalMemory: 4294967296`（精确 4096 MiB）！
* **真实 DNS 代理**：在 HAP 中实现 `DnsProxyLoop` 响应 Android Bionic `/dev/socket/dnsproxyd`（直接调鸿蒙宿主 `getaddrinfo()`），同时配置 `ANDROID_DNS_MODE=local` + `/etc/resolv.conf` + `net.dns1/net.dns2`。
* **干净进程树回收与后台 0% CPU 挂起**：`Spawn()` 为每个容器根进程建独立进程组（`setpgid(0, 0)`），`Stop()` 对整个 `-pgid` 发送 `SIGCONT -> SIGTERM -> SIGKILL` 杜绝孤儿进程；`SetPaused(true)` 发送 `SIGSTOP` 冻结整棵容器进程树实现 0% CPU 挂起。

---

## 第 3 章（Step 3）：异步 VSync 显示、大小核探针与实时监控 + Step 4 GPU 选型实测

### 3.1 异步双缓冲 `HwcRenderer` + `OH_NativeVSync` 按需帧调度
* **架构改造**（[`hwc_renderer.cpp`](file:///storage/Users/currentUser/Workspace/svbox-Android/hap/entry/src/main/cpp/hwc_renderer.cpp)）：
  * 接收线程（`QOS_USER_INTERACTIVE`）收到 `hwcomposer.redroid.so` 的 ashmem FD 后，拷贝至双缓冲后台槽并翻转 `frontIdx_`，**立刻回发 `"ok"` ACK**，彻底解除 `SurfaceFlinger` 对 `OHNativeWindow` 交换链的锁步阻塞；
  * 接入 `libnative_vsync.so`（`OH_NativeVSync_Create` + `OH_NativeVSync_RequestFrame`，实测 `OH_NativeVSync=1`），仅在有新帧或窗口回到前台时按需请求 VSync 回调；`EntryAbility::onBackground()` 自动停帧。

### 3.2 大小核集群发现 + `OH_QoS` + `HiDebug` 实时遥测
* **实现**（[`sched_qos.cpp`](file:///storage/Users/currentUser/Workspace/svbox-Android/hap/entry/src/main/cpp/sched_qos.cpp)）：
  * 优先尝试读取 `/sys/devices/system/cpu/cpu*/cpu_capacity`，若被 HAP 沙箱拒绝则自动回退到 ~2ms/核单核 ALU 微测探针（吸收 `gpui-ohos/sched_qos.rs` 方案）；本机真机探得 **`20c (L=4 [0-3], M=8 [4-11], B=8 [12-19])`**；
  * 4 档可切换性能策略：`eco`（小核 `0-3` + `nice 10` + `SCHED_BATCH`）、`balanced`（中核 `4-9`）、`perf`（大核 `12-19`）、`all`（不限核）；
  * 宿主关键线程按级设定 QoS：`hwc-recv` / `input-touch` / `input-kbd` = `QOS_USER_INTERACTIVE(5)`，`audio-bridge` = `QOS_USER_INITIATED(3)`，`dns-proxy` = `QOS_UTILITY(1)`，`lmkd` / `logdw` = `QOS_BACKGROUND(0)`；
  * 接入 `libohhidebug.so`（`OH_HiDebug_GetSystemCpuUsage` / `GetAppNativeMemInfoWithCache` / `GetGraphicsMemory`）与容器进程树 RSS 汇总，实时展示在 HAP 顶部状态栏。

### 3.3 Step 0 → Step 3 真机前后实测对比

| 指标 | v0.1 基线（Step 0 前） | Step 1–3 完成后（真机实测） | 提升 / 变化 |
| --- | --- | --- | --- |
| **`StartAudioService` 耗时** | 10,224 ms | **97 ms** | **快 105 倍** |
| **`TotalBootTime`（进桌面）** | 15,534 ms | **4,457 ms** | **快 3.5 倍** |
| **触控 & 键盘输入** | 无 | **原生 `evdev` 多点触控 + 物理键盘 + 导航键** | Display 0 `720×1280` 完整上线 |
| **声音子系统** | 空桩（无声音） | **64 位 `IDevicesFactory@6.0` + `audio.primary.svbox.so` → `OHAudio`** | 真混音通路打通 |
| **`webview_zygote`** | 崩溃（`symlink` 权限拒） | **零报错存活** | 解锁 WebView 系 App 基础 |
| **内存上限虚拟化** | 暴露宿主机 32 GB | **`/proc/meminfo` + `sysinfo(2)` 精确 `4096 MiB`** | `AudioFlinger` 实测 `4294967296` |
| **CPU 核心控制** | 不受限（全 20 核） | **内核硬绑核 `--cpuset 12-19` + 4 档策略 + `SIGSTOP` 0% 暂停** | 完整大小核治理 |
| **显示同步** | 同步锁步阻塞 | **异步双缓冲 + `OH_NativeVSync=1` 按需停帧** | 解除 `SurfaceFlinger` 阻塞 |

---

### 3.4 Step 4 前置实测：鸿蒙 PC（Maleoon 916, API 26）GPU 能力探测与开源引擎选型

在进入 Step 4（建议切换 `xhigh` + `/compact` 后推进）前，在本机直接编译运行了原生 EGL/GLES3/Vulkan 能力探针，测得 **Maleoon 916** 的真实驱动支持矩阵：

| 维度 | 本机真机实测结果（`HAD-W32`, API 26） |
| --- | --- |
| **EGL / GLES** | **EGL 1.4 (OpenHarmony)** · **HUAWEI Maleoon 916** · **OpenGL ES 3.2 B312** (GLSL ES 3.20)<br>关键扩展：`EGL_KHR_image_base`, `EGL_KHR_fence_sync`, `EGL_ANDROID_native_fence_sync`, `EGL_KHR_surfaceless_context` |
| **Vulkan** | **Instance API 1.3.275** · **Device `Maleoon 916` API 1.2.309** (69 项设备扩展)<br>关键扩展：`VK_OHOS_surface`, `VK_OHOS_external_memory`, `VK_KHR_external_memory`, `VK_KHR_external_memory_fd`, `VK_KHR_external_semaphore_fd`, `VK_KHR_external_fence_fd`, `VK_KHR_timeline_semaphore`, `VK_KHR_synchronization2`, `VK_KHR_sampler_ycbcr_conversion`, `VK_EXT_queue_family_foreign`, `VK_KHR_swapchain` |
| **关键缺失项** | **不支持** `VK_EXT_external_memory_dma_buf`、`VK_EXT_image_drm_format_modifier`、`EGL_EXT_image_dma_buf_import`（标准 Linux DRM/GBM dma-buf 修饰符扩展） |

#### 开源 GPU 引擎对比与选型结论

| 对比项 | 方案 1：Google **`gfxstream`**（推荐 ★） | 方案 2：**Mesa `Venus` (`virtio-gpu`) + ANGLE** |
| --- | --- | --- |
| **Maleoon 916 驱动契合度** | **极高**：宿主侧直接走标准 **EGL/GLES 3.2 + Vulkan 1.2** + `VK_OHOS_surface` / `EGL_KHR_surfaceless_context`，**完全不依赖 Linux DRM/GBM dma-buf modifier** | **中低**：`virglrenderer` / `venus` 默认强依赖 Linux `dma-buf` + `VK_EXT_external_memory_dma_buf` + `VK_EXT_image_drm_format_modifier`（Maleoon 916 缺失，需大幅改造内存导入层） |
| **GL / VK 双驱动支持** | **原生一体支持**：Guest 侧自带 `libEGL_emulation` + `libGLESv2_emulation` + `vulkan.ranchu.so`，Host 侧支持 GLES 与 Vulkan 双后端切换 | 需同时维护 `venus`（VK）+ `ANGLE`（GLES-on-VK）两套大型工程 |
| **传输层改造难度** | **低**：原生抽象 `IOStream`（本就支持 TCP/Unix socket + shared memory ringbuffer），可直接嫁接到我们的 `/ipc/gfxstream.sock` + `/dev/ashmem` | 需模拟 `virtio-gpu` DRM ioctl / blob resource 协议，改动面更大 |
| **Android 11 契合度** | **100% 原生**：AOSP Android 11 Cuttlefish / Emulator 官方标配图形栈（含 `gralloc.ranchu` / `hwcomposer.ranchu` 协议参考） | Android 11 上 `venus` 仍偏早期实验，ANGLE 需额外裁剪 |

**结论（供 Step 4 拍板）**：**首选裁剪 Google `gfxstream`**（Socket + Ashmem 传输层 + 鸿蒙 `OHNativeWindow` / `EGL 1.4 + GLES 3.2` / `Vulkan 1.2 + VK_OHOS_surface` 宿主渲染器），保留现有 SwiftShader 作为兜底模式，实现 **`SwiftShader / OpenGL ES / Vulkan` 三选一渲染器切换**！

---

### 3.5 当前已知限制与待解问题清单（Open Issues，截至 Step 3）

为了便于后续会话（`/compact` 后）与博客收尾时精准衔接，集中记录当前尚未覆盖的边界与待办项：

| 编号 | 类别 | 当前状态 / 已知限制 | 计划解决阶段 |
| --- | --- | --- | --- |
| **O1** | GPU 硬件加速 | **已解决（Step 4 完成）**：裁剪移植 Google `gfxstream`，打通 Maleoon 916 OpenGL ES 3.2 与 Vulkan 1.2 + `SkiaVK` + `OH_NativeBuffer` 零拷贝，支持 `SwiftShader / GLES / Vulkan` 同进程实时切换 | **已完成（见第 4 章）** |
| **O2** | 音频录制（麦克风） | `audio_primary_svbox.c` 已打通播放输出（`open_output_stream` → `OHAudio`），尚未实现录音输入（`open_input_stream`） | Step 4 后按需补齐 |
| **O3** | 动态分辨率 & 窗口缩放 | 当前虚拟屏分辨率固定 `720×1280 @ 320dpi`，`XComponent` 拖动 resize 时仅做拉伸映射，未动态通知 Android 改变逻辑分辨率/横竖屏 | 桌面化体验阶段 |
| **O4** | 桌面输入与互通 | 物理键盘按键已通，但尚未接鸿蒙中文 IME 文本直传、宿主↔安卓双向剪贴板同步、文件拖拽/APK 一键安装与共享目录 | 桌面化体验阶段（可复用 `gpui-ohos/clipboard.rs` 与 `tdesktop` 经验） |
| **O5** | 相机 / 传感器 / 媒体硬解 | 尚未接入摄像头、传感器与鸿蒙硬件编解码（`AVCodec`）桥接 | 远期可选增强 |
| **O6** | `svbox` 分支合流 | `svbox` 的全部通用改动目前沉淀在 `feat/vdev-binder`（`3e82322`），`main` 分支保持干净未合并 | 待整体回归或需要发布时由人工决定是否合入 `main` |

---

## 第 4 章（Step 4）：基于裁剪版 Google `gfxstream` 的 Maleoon 916 硬件 GPU 加速（GLES 3.2 + Vulkan 1.2 + `OH_NativeBuffer` 零拷贝）

### 4.1 整体架构与裁剪边界

严格维持 `svbox`（`feat/vdev-binder`）**100% 通用 Linux 用户态容器**不侵入任何 Android 逻辑，全部 GPU 加速代码均沉淀在 `svbox-Android` 仓库内：

```text
┌─────────────────────────────────────────────────────────────────────────┐
│ Android 11 Guest (svbox 容器内，/vendor/lib64 按 gpuMode 动态挂载)       │
│  App / Launcher3 / SystemUI                                             │
│   ├─ GLES 模式: libhwui (skiagl) -> libEGL_emulation + libGLESv2_emu    │
│   └─ Vulkan 模式: libhwui (skiavk) -> vulkan.ranchu + libvulkan_enc     │
│  SurfaceFlinger -> gralloc.ranchu + hwcomposer.ranchu (零 CPU 回读直呈)  │
│        │                                                                │
│        ▼ svbox_pipe_guest.cpp (AF_UNIX /ipc/gfxstream.sock)             │
├────────┼────────────────────────────────────────────────────────────────┤
│ HarmonyOS PC Host (org.svbox.android HAP, ARM64 API 26, Maleoon 916)    │
│  GfxstreamBridge -> libgfxstream_host.so (6.7 MB stripped)              │
│   ├─ 单 epoll AF_UNIX 管道复用器 (opengles / GLProcessPipe / refcount)  │
│   ├─ GLES 后端: glestranslator (egl2egl -> libEGL + libGLESv3)          │
│   │             -> DisplayGl -> OHNativeWindow (XComponent)             │
│   └─ Vulkan 后端: VkDecoder + VkEmulation (Maleoon 916 Vulkan 1.2.309)  │
│             ├─ ColorBufferVk <-> ColorBufferGl:                         │
│             │   VK_OHOS_external_memory (OH_NativeBuffer)               │
│             │   <-> EGL_NATIVE_BUFFER_OHOS (0x34E1) 真零拷贝共享         │
│             └─ DisplayVk -> VK_OHOS_surface Swapchain -> OHNativeWindow │
└─────────────────────────────────────────────────────────────────────────┘
```

* **宿主机引擎**（[`gpu/ohos_host/`](file:///storage/Users/currentUser/Workspace/svbox-Android/gpu/ohos_host/CMakeLists.txt)）：基于固定版本的 `aemu` + `gfxstream`（见 [`gpu/fetch_sources.sh`](file:///storage/Users/currentUser/Workspace/svbox-Android/gpu/fetch_sources.sh)），通过幂等脚本 [`apply_host_patches.py`](file:///storage/Users/currentUser/Workspace/svbox-Android/gpu/ohos_host/apply_host_patches.py) 施加全部 `__OHOS__` 补丁，使用鸿蒙 NDK 交叉编译为单动态库 `libgfxstream_host.so`（strip 后 **6.7 MB**），通过 [`svbox_gfxstream_capi.cpp`](file:///storage/Users/currentUser/Workspace/svbox-Android/gpu/ohos_host/svbox_gfxstream_capi.cpp) 暴露极简 C API 并将 `gfxstream` 全部日志桥接到 `hilog`（Tag `SvboxGfx`）。
* **Android 11 Guest 驱动栈**（[`gpu/android_guest/`](file:///storage/Users/currentUser/Workspace/svbox-Android/gpu/android_guest/CMakeLists.txt)）：基于 `goldfish-opengl`（`android11-gsi`），施加 [`apply_guest_patches.py`](file:///storage/Users/currentUser/Workspace/svbox-Android/gpu/android_guest/apply_guest_patches.py)，以自研 [`svbox_pipe_guest.cpp`](file:///storage/Users/currentUser/Workspace/svbox-Android/gpu/android_guest/svbox_pipe_guest.cpp) 替换内核 `/dev/goldfish_pipe` 为 `/ipc/gfxstream.sock`，以零回读 [`hwcomposer_ranchu.cpp`](file:///storage/Users/currentUser/Workspace/svbox-Android/gpu/android_guest/hwcomposer_ranchu.cpp) 走 `rcFBPost(cbHandle)` 直呈，产出 14 枚 `.so` 经 `scripts/pack_hnp.py` strip 后打入 `svbox.hnp`（总计 **5.3 MB**）。
* **运行时三模式切换**：[`Index.ets`](file:///storage/Users/currentUser/Workspace/svbox-Android/hap/entry/src/main/ets/pages/Index.ets) + [`android_runtime.cpp`](file:///storage/Users/currentUser/Workspace/svbox-Android/hap/entry/src/main/cpp/android_runtime.cpp) + [`gfxstream_bridge.cpp`](file:///storage/Users/currentUser/Workspace/svbox-Android/hap/entry/src/main/cpp/gfxstream_bridge.cpp) 支持在 UI / `aa start --ps gpuMode <swiftshader|gles|vulkan>` 一键动态切换三种后端，无需重启 HAP 进程。

---

### 4.2 鸿蒙 PC（Maleoon 916, API 26）真机硬核踩坑全记录

> 这一节记录了在真实华为鸿蒙 PC（`HAD-W32`，Maleoon 916 GPU）上逐一攻克的 **12 个底层图形驱动与协议坑**，每一个都经真机日志与像素验证，可直接作为博客核心技术干货。

1. **坑 4-1：鸿蒙 `libGLESv2.so` 不导出部分 GLES 3.x 符号**
   * **现象**：`glestranslator` 的 `EglOsApi_egl.cpp` 默认 `dlopen("libGLESv2.so")`，导致 GLES 3.x 函数表解析残缺。
   * **解法**：`#elif defined(__OHOS__)` 优先加载 `libGLESv3.so`（`libGLESv2.so` 作为 Alt）。
2. **坑 4-2：`OH_NativeWindow_NativeWindowHandleOpt(GET_BUFFER_GEOMETRY)` 参数顺序是 `(height, width)`**
   * **现象**：与直觉相反，鸿蒙 NDK `GET_BUFFER_GEOMETRY` 的可变参数顺序是**高度在前、宽度在后**（`(&ohH, &ohW)`），写反会导致视口宽高互换。
   * **解法**：在 [`NativeSubWindow_ohos.cpp`](file:///storage/Users/currentUser/Workspace/svbox-Android/gpu/ohos_host/NativeSubWindow_ohos.cpp) 与 `EglOsApi_egl.cpp` 严格按 `(&ohH, &ohW)` 读取。
3. **坑 4-3：华为 `higpu` EGL 强制要求 `EGL_ALPHA_SIZE=8` 与 `SET_FORMAT = EGL_NATIVE_VISUAL_ID`**
   * **现象**：`EmulationGl.cpp` 默认只请求 RGB 888 配置，在鸿蒙 `higpu` 上直接创建 `OHNativeWindow` Surface 时格式不匹配导致 `eglCreateWindowSurface` 失败。
   * **解法**：`EmulationGl.cpp` 在 `__OHOS__` 补充 `EGL_ALPHA_SIZE, 8`，并在 `EglOsApi_egl.cpp` 调用 `eglCreateWindowSurface` 前通过 `SET_FORMAT` 同步 `EGL_NATIVE_VISUAL_ID`。
4. **坑 4-4：GLES 模式画面全黑（`cbPx=(0,0,0,255)`）—— Desktop GL `#version 330 core` 隐形杀手**
   * **现象**：`SurfaceFlinger` 和 `DisplayGl::post` 全链路 `glGetError()==0`、`eglSwapBuffers==1`，但每一个 `ColorBuffer` 读回都是纯黑 `(0,0,0,255)`。
   * **根因**：把 `gfxstream` 内部日志全部桥接到 `hilog` 后立即现形：`[E GLEScontext.cpp:2755] fail to compile: Language version '330' unknown, this compiler only supports up to version '320 es'`！原来 `GLESVersionDetector::shouldEnableCoreProfile()` 在 `SELECTED_RENDERER_HOST && dispatchMaj > 2` 时默认返回 `true`，导致 `glestranslator`（`setupImageBlitState` / `CoreProfileEngine`）误以为宿主机是桌面 OpenGL，发射了 `#version 330 core` 着色器，被 Maleoon 916 原生 GLES 3.2 编译器全部拒绝，所有 FBO/Texture blit 静默失效。
   * **解法**：`GLESVersionDetector.cpp` 中 `#ifdef __OHOS__ return false;`，修复后 **GLES 模式立刻一帧点亮**！
5. **坑 4-5：GLES 画面顶部和右侧被裁切**
   * **根因**：`svbox_gfxstream_capi.cpp` 向 `showOpenGLSubwindow` 传入了窗口物理尺寸 `surfaceW×surfaceH`（`626×1113`）去设置 `SET_BUFFER_GEOMETRY`，却在 `glViewport` 里渲染 `720×1280`，导致超出部分被裁切。
   * **解法**：统一将 Guest 逻辑分辨率 `fbW×fbH`（`720×1280`）传给 `showOpenGLSubwindow`，让鸿蒙 RenderService 做硬件等比缩放，画面瞬间 100% 完整无裁切。
6. **坑 4-6：Vulkan 交换链创建后报错 `VulkanSwapchainLayer RequestBuffer[3] failed (40601000)`**
   * **根因**：鸿蒙 `XComponent` 的 `OHNativeWindow` `BufferQueue` 默认只有 **3 个槽位**，而 `gfxstream` 的 `SwapChainStateVk` 默认按 `minImageCount + 1` 申请了 4 张 Swapchain Image。
   * **解法**：`SwapChainStateVk.cpp` 在 `__OHOS__` 上将 `imageCount` 钳位在 `[2, 3]`，并选用 `VK_FORMAT_R8G8B8A8_UNORM` 与自适应 `compositeAlpha`（`OPAQUE -> INHERIT -> PRE_MULTIPLIED`）。
7. **坑 4-7：同一 `OHNativeWindow` 无法在 EGL 与 Vulkan 之间切换（`VK_ERROR_SURFACE_LOST_KHR`）**
   * **根因**：鸿蒙 `BufferQueue` 绑定了 EGL 生产者后，即便 `eglDestroySurface`，在同一个 `OHNativeWindow` 上创建 `VK_OHOS_surface` 交换链仍会报 `VK_ERROR_SURFACE_LOST_KHR`。
   * **解法**：[`Index.ets`](file:///storage/Users/currentUser/Workspace/svbox-Android/hap/entry/src/main/ets/pages/Index.ets) 引入 `surfaceEpoch` 状态交替挂载 `svbox_android_surface_a/b` 重建全新 `OHNativeWindow`，同时在 [`android_runtime.cpp`](file:///storage/Users/currentUser/Workspace/svbox-Android/hap/entry/src/main/cpp/android_runtime.cpp) 的 `OnSurfaceDestroyed(void* window)` 校验指针防止旧窗口析构误清掉新窗口。
8. **坑 4-8：同进程第二次切回 Vulkan 触发 `FATAL: Attempted to re-initialize VkDecoderGlobalState`**
   * **根因**：`FrameBuffer` 销毁时不会主动调用 `VkDecoderGlobalState::reset()`，第二次初始化 Vulkan 模式时触发单例断言中止。
   * **解法**：在 `VkDecoderGlobalState::initialize()` 中改为 `if (sGlobalDecoderState) { reset(); }`，实测 `Vulkan -> GLES -> Vulkan -> SwiftShader` 无限次热切换零崩溃。
9. **坑 4-9：Maleoon 916 不支持 `OPAQUE_FD` 导出 `VkImage`，打通 `VK_OHOS_external_memory` (`OH_NativeBuffer`) + `EGL_NATIVE_BUFFER_OHOS` 真零拷贝互通**
   * **硬件探针实测结论**：Maleoon 916 上 `VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT` 对 `VkImage` 返回 `VK_ERROR_FORMAT_NOT_SUPPORTED`，导致 Android `SkiaVK` 报错 `MakeFromAHardwareBuffer failed`。而鸿蒙扩展 **`VK_OHOS_external_memory`**（`OH_NativeBuffer`，句柄位 `0x00008000`）对全部常规图像格式均报告 `features=0x7`（`DEDICATED_ONLY | EXPORTABLE | IMPORTABLE`），且支持经 `OH_NativeWindow_CreateNativeWindowBufferFromNativeBuffer` + `eglCreateImageKHR(EGL_NATIVE_BUFFER_OHOS=0x34E1)` + `glEGLImageTargetTexture2DOES` 零拷贝导入 OpenGL ES！
   * **关键实现细节**（均在 [`apply_host_patches.py`](file:///storage/Users/currentUser/Workspace/svbox-Android/gpu/ohos_host/apply_host_patches.py)）：
     * 因 `gfxstream` 自带旧版 `vulkan_core.h` 优先于鸿蒙 sysroot，显式定义常量：`kVkStNativeBufferPropsOhos = 1000452001`、`kVkStImportNativeBufferOhos = 1000452003`、`kVkStMemGetNativeBufferOhos = 1000452004`、`kVkExtMemHandleOhosNativeBuffer = 0x8000`；
     * 导出侧：所有 `ColorBufferVk` 图像强制走 `dedicatedImage` + `vkGetMemoryNativeBufferOHOS` 拿到 `OH_NativeBuffer*`，再调用 `vkGetNativeBufferPropertiesOHOS` 回填真实 `allocationSize`；
     * 导入侧（Guest `VkDevice`）：通过 `vkGetNativeBufferPropertiesOHOS` 获取宿主机 `allocationSize` 和 `memoryTypeBits`（Maleoon 916 上为 `0x2`，即 host type index `1`），且**必须绕过** `getHostMemoryInfoFromGuestMemoryTypeIndex` 防止宿主机索引被二次错误重映射；
     * GL 互通侧：`ColorBufferGl::importOhosNativeBuffer` 通过 `EGL_NATIVE_BUFFER_OHOS` 绑定到 `m_tex`，并调用新增的 `emulationVk->setColorBufferGlExported(handle)`，实现 `SurfaceFlinger`（GLES）与 `SkiaVK`（Vulkan）及 `DisplayVk` 三方真零拷贝共享！
10. **坑 4-10：Android 11 Guest `goldfish-opengl-r` 剥离 `VkMemoryDedicatedAllocateInfo` 导致 `dedicated=0`**
    * **现象**：打通 `OH_NativeBuffer` 后，`DisplayVk` 正常显示壁纸，但 `Launcher3` / `SystemUI` 全部 `SkiaVK` 图层仍不可见。诊断日志暴露 `VkDecoderGlobalState::on_vkAllocateMemory` 收到的全部 ColorBuffer 导入均为 `dedicated=0`！
    * **根因**：2020 年版 `goldfish-opengl-r` 的 `ResourceTracker.cpp:3965` 写了 `shouldPassThroughDedicatedAllocInfo = !exportAllocateInfoPtr && !importAhbInfoPtr && ...`，主动剥离了 `SkiaVK` 传下来的 `VkMemoryDedicatedAllocateInfo`，违背 Maleoon 916 `OH_NativeBuffer` 的 `DEDICATED_ONLY_BIT` 约束。
    * **解法**：在 [`apply_guest_patches.py`](file:///storage/Users/currentUser/Workspace/svbox-Android/gpu/android_guest/apply_guest_patches.py) 放行 `importAhbInfoPtr` / `exportAllocateInfoPtr` 的 `dedicatedAllocInfo` 透传，修复后 `dedicated=1` 且 `ColorBufferVk` 与 `ColorBufferGl` 像素计数 **100% 比特级一致**（`vkNZ == glNZ == 98616`）。
11. **坑 4-11：容器无 `/dev/goldfish_sync` 时的日志刷屏与异步栅栏竞态**
    * **现象**：`logcat` 每秒刷数十条 `ensureSyncDeviceFd: failed to create sync device`，且 `on_vkGetSemaphoreFdKHR` / `on_vkQueueSignalReleaseImageANDROID` 在 GPU 未完成时即返回 `-1`（代表已触发）。
    * **解法**：Guest 端缓存 `ensureSyncDeviceFd()` 结果消除刷屏，无 sync 设备时在 `on_vkGetSemaphoreFdKHR` 调用 `vkDeviceWaitIdle`、在 `on_vkGetFenceFdKHR` 调用 `vkWaitForFences` 后返回 `-1`；Host 端 `VkAndroidNativeBuffer.cpp` 在 `__OHOS__` 同步等待 `qsriFence` 完成再返回 `-1`。
12. **坑 4-12（压轴大坑）：Vulkan 桌面“只有无纹理几何遮罩、图标与文字全透明”—— `VK_MEMORY_PROPERTY_HOST_COHERENT_BIT` 被宿主剥离**
    * **现象**：`dedicated=1` 生效后，转储 `Launcher3` 的 `ColorBuffer` 发现底部热区圆角遮罩（纯着色器无纹理绘制）已经清晰渲染，但所有应用图标、搜索框图标、状态栏文字时钟、导航栏图标全部空白 `(0,0,0,0)`。
    * **根因定位**：在 `VkSubDecoder.cpp` 打印完整命令流，一举捕获惊人真相：`HardwareBitmapUploader` 在上传每个图标时录制的都是**空命令缓冲 `[BeginCommandBuffer, EndCommandBuffer]`**，连一次 `vkCmdCopyBufferToImage` 都没发！追查发现宿主机 `VkEmulatedPhysicalDeviceMemory.cpp:65` 在无 `GlDirectMem` / `VirtioGpuNext` 时将暴露给 Guest 的**全部内存类型的 `VK_MEMORY_PROPERTY_HOST_COHERENT_BIT` 全部剥离**（这直接违反 Vulkan 1.0 规范 10.2 节“至少存在一种 `HOST_VISIBLE | HOST_COHERENT` 内存类型”的强制保证），导致 Android 11 `SkiaVK` 的 `GrVkTransferBuffer` 和 `HardwareBitmapUploader` 找不到匹配的 Staging Buffer 内存类型而静默跳过全部纹理/字形上传！（而 Vertex/Uniform 缓冲允许非 Coherent 并主动调 `vkFlushMappedMemoryRanges`，所以无纹理几何能画出来。）
    * **解法**：我们在 Guest 端 [`apply_guest_patches.py`](file:///storage/Users/currentUser/Workspace/svbox-Android/gpu/android_guest/apply_guest_patches.py) 已通过匿名 `mmap` 影子内存 + `on_vkUnmapMemory`/`on_vkQueueSubmit` 自动调用 `vkFlushMappedMemoryRanges` 完整模拟了 `HOST_COHERENT` 语义；在宿主 `VkEmulatedPhysicalDeviceMemory.cpp` 对 `#ifndef __OHOS__` 保留 `VK_MEMORY_PROPERTY_HOST_COHERENT_BIT` 后，**Android 11 `SkiaVK` 完整桌面（壁纸、状态栏时钟、Google 搜索栏、Gallery/Contacts/Camera 图标、底部导航栏）瞬间 100% 像素级清晰点亮！**

---

### 4.3 真机三模式同进程实测对比（`HAD-W32` · Maleoon 916）

测试环境：同一台华为鸿蒙 PC（`HAD-W32`，HarmonyOS API 26，20 核大小核 `L=4, M=8, B=8`，Maleoon 916 GPU），同一份纯净构建的 `org.svbox.android` HAP，统一 `profile=perf`（`CPU[12-19]`、`4096 MB` 内存上限、`720×1280 @ 60Hz`），**在同一 HAP 进程（`PID 42473`）内连续完成 `Vulkan -> GLES -> Vulkan -> SwiftShader` 实时切换并同口径采样（启动后 `+15s`）**：

| 指标 | SwiftShader（CPU 软渲染） | GLES（Maleoon 916 GLES 3.2） | Vulkan（Maleoon 916 Vulkan 1.2 + `SkiaVK`） |
| --- | --- | --- | --- |
| **Guest 渲染管线** | `libEGL_swiftshader` + `skiagl` | `libEGL_emulation` + `skiagl` | `vulkan.ranchu` + `skiavk` + `libEGL_emulation` |
| **Host 合成/呈现** | `svbox-hwc` ashmem 拷屏 | `DisplayGl` → `eglSwapBuffers` | `DisplayVk` → `VK_OHOS_surface` + `OH_NativeBuffer` 零拷贝 |
| **`TotalBootTime`（进桌面）** | `4,518 ms` | `4,473 ms`（最快 `3,896 ms`） | **`4,313 ms`** |
| **`SurfaceFlinger Boot is finished`** | `6,426 ms` | `6,629 ms` | **`6,161 ms`** |
| **最短帧耗时 (`app_time_stats min`)** | N/A（纯 CPU 栅格化） | **`2.12 ms`**（峰值 45 fps） | **`2.67 ms`** |
| **`+15s` 全 UID 总 CPU%**（HAP + 容器） | `129.3%`（`8.4%` + `120.9%`） | **`59.6%`**（`9.3%` + `50.3%`，**↓2.17×**） | **`54.4%`**（`9.3%` + `45.1%`，**↓2.38×**） |
| **稳态桌面全 UID 总 CPU%** | `129%–297%`（动画/重绘期） | `~59.6%` | **`35.3%`**（`9.2%` + `26.1%`，**↓3.7×–8.4×**） |
| **`+15s` 全 UID 总 RSS 内存**（HAP + 容器） | `4,774.9 MB`（`175.8` + `4,599.0`） | **`4,031.2 MB`**（`190.9` + `3,840.2`，**-744 MB**） | **`4,112.6 MB`**（稳态回落至 **`3,391.6 MB`**，**-1,383 MB**） |
| **视觉截图核验** | 像素级完好（`snap_bench_swiftshader.png`） | 像素级完好（`snap_bench_gles.png`） | 像素级完好（`snap_bench_vulkan2.png`） |
| **同进程实时模式切换（`PID 42473`）** | 通过 | 通过（`Vulkan -> GLES`） | 通过（`GLES -> Vulkan` 二次初始化零崩溃） |

### 4.4 构建与打包要点备忘

* 每次使用 `hvigorw assembleHap` 重新生成未签名 HAP 后，**必须执行** `python3 scripts/pack_hnp.py /storage/Users/currentUser/Workspace/svbox/build/svbox hap/entry/build/default/outputs/default/entry-default-unsigned.hap` 将 `hnp/arm64-v8a/svbox.hnp` 注入 HAP 包内，否则 `hda install` 会因 `ExtractHnpFiles` 缺失 HNP 报 `code:9568409 errno:22`。
* 仓库边界核验：`/storage/Users/currentUser/Workspace/svbox`（`feat/vdev-binder` @ `3e82322`）在 Step 4 全程 **0 改动**，继续保持 100% 通用 Linux 用户态容器定位。

---

### 4.5 真机多应用“打开首帧后卡死”深度排查与 5 大根因修复

桌面点亮后真机实测打开 `Settings`、`DeskClock`、`Gallery`、`Files`、`WebView` 等应用时，暴露出**“Launcher3 正常，但打开应用仅显示首帧（或 SplashScreen）后卡死/ANR”**的现象。通过抓取真实 `logcat.txt` 与容器内多应用自动化压测，一次性挖出并根治了 **5 个叠加的底层 Bug**（全部属于运行时/Binder/系统服务层，与 GPU 渲染本身无关）：

1. **坑 4-13（核心杀手 #1）：`svbox-binderd` 的 `BC_REQUEST_DEATH_NOTIFICATION` ABI 错位（`0x4010630e` vs `0x400c630e`）**
   * **现象**：所有被 OOM/空闲回收（`empty #17`）、最近任务划掉（`remove task`）或崩溃退出的进程，在 `logcat` 中**从未出现过一条 `Process ... has died`**！`ActivityManager` 永久保留僵尸 `ProcessRecord`；用户再次点击该应用图标时，`system_server` 只画出 `SplashScreen` 启动窗（“首帧”）并尝试向死进程发生命周期事务，随后永久卡死并报 `Activity top resumed state loss timeout` / `Activity destroy timeout`。
   * **根因**：Linux 64 位内核 `uapi/linux/android/binder.h` 中 `struct binder_handle_cookie { __u32 handle; binder_uintptr_t cookie; } __packed;` 是 **`__packed` 12 字节（`0x0c`）** 而非自然对齐的 16 字节（`0x10`）！因此真实 Android 用户态发出的命令码是 **`0x400c630e` / `0x400c630f`**，而原 `svbox_binderd.c` 只匹配了 16 字节对齐的 `0x4010630e` / `0x4010630f`，导致**全系统 100% 的 `linkToDeath` 请求被静默丢弃**！
   * **同时捕获的伴生 Bug**：`handle_conn_event(CONN_NOTIFY)` 判断断开写了 `if (n <= 0 && errno != EAGAIN && errno != EWOULDBLOCK)`——但 `recv()` 返回 `0`（EOF）时**不会重置 `errno`**，若前序非阻塞 `send(notify_fd)` 恰好留下 `errno == EAGAIN`，进程退出 EOF 会被直接忽略！
   * **解法**：在 [`binderd/svbox_binderd.c`](file:///storage/Users/currentUser/Workspace/svbox-Android/binderd/svbox_binderd.c) 支持 `0x400c630e` / `0x400c630f` 与 `__attribute__((packed))` 12 字节结构，并在 `CONN_NOTIFY` 显式区分 `n == 0` EOF。
2. **坑 4-14（核心杀手 #2）：`svbox-binderd` 句柄表 `MAX_HANDLES=1024` 永不回收，满后静默退化为句柄 `0`（`servicemanager`）**
   * **现象**：容器仅运行几分钟、连续打开 5~6 个应用后，新打开的任何应用（包括活着的 `Settings`）在绘制首帧后直接冻结 ANR；`logcat` 出现铁证：
     ```text
     [W/Parcel] **** enforceInterface() expected 'android.os.IServiceManager' but read 'android.view.IWindow'
     [I/servicemanager] oneway function results will be dropped but finished with status BAD_TYPE
     ```
   * **根因**：`Launcher3` 和 `SystemUI` 在开机早期注册的 Binder 句柄仍有效，所以桌面始终正常；但 `svbox_binderd.c` 在 `BC_RELEASE`/`BC_DECREFS` 及远端进程死亡时**从未回收 `handles[]` 与 `g_nodes[]` 槽位**。一旦 `system_server` 用满 1024 个句柄，`get_or_create_handle()` 溢出后返回 `0`（即 `servicemanager`！），导致所有新窗口的 `IWindow` 回调全部发给了 `servicemanager`，窗口永不收到 `resized` / `focus` 而假死。
   * **解法**：在 [`binderd/svbox_binderd.c`](file:///storage/Users/currentUser/Workspace/svbox-Android/binderd/svbox_binderd.c) 将 `proc->handles` 与 `g_nodes` 改为动态倍增扩容（永不溢出，且异常时绝不回退为 `0`），按 Linux Binder `binder_transaction_buffer_release` 语义在 `mem_block` 中追踪 `in_flight` 传输计数，在 `BC_RELEASE` / `BC_DECREFS` / `BC_FREE_BUFFER` / `BC_DEAD_BINDER_DONE` / 进程死亡时完整回收死句柄与无引用节点并投递 `BR_RELEASE` / `BR_DECREFS`。
3. **坑 4-15：`Gallery3D` 与 `Camera2` 在 `DisableCameraReceiver` 永久卡死等待 `media.camera`**
   * **根因**：Redroid 11 的 `/system/bin/cameraserver` 是 32 位 ARM ELF，在纯 64 位 `svbox` 中无法启动；导致 `BOOT_COMPLETED` 时 `com.android.gallery3d` 与 `com.android.camera2` 的 `DisableCameraReceiver` 在主线程死循环等待 `ServiceManager::getService("media.camera")`，不仅令 `Gallery` 打开必超时（`10025ms`），还把全系统 `BOOT_COMPLETED` 广播队列拖慢 120 秒（两次 60s ANR）。
   * **解法**：新增轻量 64 位 ARM64 Binder 桩服务 [`camera_stub/svbox_cameraserver.c`](file:///storage/Users/currentUser/Workspace/svbox-Android/camera_stub/svbox_cameraserver.c)（基于 Android 原生 `libbinder_ndk.so` 注册 `media.camera` 并报告 0 个摄像头），打包进 `svbox.hnp` 随容器启动。`BOOT_COMPLETED` 耗时从 >120s 降至 `< 8s`，`Gallery3D` 冷/热启动降至 `359–441ms`。
4. **坑 4-16：鸿蒙 PC W^X 内存策略导致 Chromium WebView V8 `CodeRange` OOM 崩溃**
   * **现象**：打开 `WebViewBrowserActivity` 出现首帧后立刻因 `V8 process OOM (Failed to reserve virtual memory for CodeRange)` 触发 `SIGTRAP` 崩溃。
   * **根因**：鸿蒙 PC 对无 JIT ACL 的调试 HAP 禁止匿名/memfd `PROT_EXEC` 映射（ART 也因此自动退回解释执行），导致 V8 JIT 预留可执行 `CodeRange` 失败。
   * **解法**：[`android_runtime.cpp`](file:///storage/Users/currentUser/Workspace/svbox-Android/hap/entry/src/main/cpp/android_runtime.cpp) 启动时自动写入 `/data/local/tmp/webview-command-line`（`_ --js-flags=--jitless`），Chromium WebView 125 正常常驻不崩溃。
5. **坑 4-17：`zygote64` 偶发 `Not whitelisted (1402): .../boot.log` 启动中止**
   * **根因**：鸿蒙 HAP 的 `RLIMIT_NOFILE` 高达 32768，文件描述符可分配到 `> 1024`（如 `1402`），而 `AndroidRuntime::Spawn()` 关闭子进程继承 FD 的循环上限被截断在 `1024`，且 `std::ofstream bootLog` 未带 `O_CLOEXEC`。
   * **解法**：`boot.log` 改用 `open(..., O_CLOEXEC)`，且 `Spawn()` 改为遍历 `/proc/self/fd` 关闭全部多余 FD。

#### 真机修复验证结果（`HAD-W32` · Vulkan & GLES 双模式）
* **8 大内置应用全通过（Vulkan 模式）**：`Settings (380ms)`、`DeskClock (338ms)`、`Gallery3D (441ms)`、`Contacts (444ms)`、`Calendar (430ms)`、`Files (394ms)`、`QuickSearchBox (326ms)`、`WebView (845ms)` 全部 `Status: ok`。
* **进程强杀后冷重启 4/4 全通过**：`kill -9` 杀掉 `DeskClock` / `Settings` / `Gallery3D` / `Files` 后，`ActivityManager` 立即收到 `BR_DEAD_BINDER`（`Process ... has died`）并干净冷重启（`434–455ms`），彻底消除僵尸 `ProcessRecord`。
* **20 轮连续多应用切换压测**：`STRESS_FAIL=0`（20/20 通过，单次切换 `82–235ms`），全程 `0` 条 `enforceInterface` 错乱、`0` 次超时/ANR。
* **超越首帧的连续 60Hz 动画与交互验证**：在 **Vulkan**（`39 fps · CPU 32% · 3504MB RSS · #1467`）和 **GLES**（`39 fps · CPU 24% · #1810`）双模式下，打开 `Settings` 点击进入子页面 `Battery`，打开 `DeskClock` 切换到 `Stopwatch` 启动秒表连续计时并截图核验流畅运行。

---

## 第 5 章：架构定调——以 `svbox` 为核心的三层解耦与演进路线

> **核心定位明确（2026-11-23）**：项目的真正长期产品是 **`svbox` 本身**（一款可在鸿蒙 PC、无 Root Android / Termux、受限普通 Linux 上运行的通用无特权/无 Namespace 用户态容器）。Android 11 扮演的是**最严苛的兼容性压力测试负载**——原版 Android 镜像尽量 **0 改动**；开发过程中踩出的所有容器通用兼容性问题全部沉淀进 `svbox`，鸿蒙平台专属桥接做成**面向标准 Linux 协议的通用桥接层**（未来可直接接入 Ubuntu / Debian / Alpine 等任意 Linux 发行版），Android 适配仅作为最外层的薄兼容模块。

### 5.1 三层解耦边界

```text
┌──────────────────────────────────────────────────────────────────────┐
│ Layer 3: Guest 兼容薄壳（Android 镜像原则上 0 改动 / 只读）           │
│   svbox-binderd · svbox-propd · svbox-netd · svbox-cameraserver      │
│   GPU guest HALs / Audio HAL 全部通过外置进程 + bind mount 注入       │
├──────────────────────────────────────────────────────────────────────┤
│ Layer 2: OHOS 通用宿主机桥接层（面向标准 Linux 协议，任意 Linux 复用） │
│   显示 & GPU（OHNativeWindow / EGL / Vulkan / OH_NativeBuffer）       │
│   外设输入（XComponent 键鼠/触控/滚轮/手柄 → 标准 Linux evdev）       │
│   音频（OHAudio ↔ 标准 PCM Socket） · USB（目录 / vdev tty / DDK）    │
│   宿主机共享目录 · 剪贴板 · 大小核 QoS 调度                           │
├──────────────────────────────────────────────────────────────────────┤
│ Layer 1: svbox 核心引擎（跨平台无 Root / 无 Namespace Linux 容器）    │
│   VFS / Overlay / EROFS(--lower) / owner.db / fake_ids               │
│   通用资源视图（--cpuset / --mem-mb / --disk-mb）                     │
│   通用 --vdev 协议（chr / binder / tun / tty）                        │
│   用户态网络隔离与虚拟网卡 / TUN 代理（net_view → user-space netstack）│
└──────────────────────────────────────────────────────────────────────┘
```

### 5.2 四大关键子系统架构决策

1. **网络隔离与 TUN 代理软件（方案 2，归属 `svbox` 通用层）**
   * **架构决策**：不借宿主机全局 `VpnExtensionAbility`，而是在 **`svbox` 层**基于现有 [`net_view.c`](file:///storage/Users/currentUser/Workspace/svbox/src/payload/logic/net_view.c) 与 `--vdev` 构建通用的**用户态虚拟网卡 + `/dev/net/tun` + 容器级 socket 重定向**（支持按 fd 豁免直连，兼容 Android `VpnService.protect(fd)` 与 Linux 标记语义）。
   * **Guest 表现层**：参考 Android Emulator，向 Android 暴露标准 `eth0`/`wlan0` 在线网络状态与 `tun0`，让 Clash / sing-box / WireGuard 等 TUN 代理软件无需 `CAP_NET_ADMIN` 且不劫持宿主机全局流量即可原生运行。
2. **硬件外设范围收敛**
   * 聚焦桌面刚需 4 件套：**网络（模拟器语义）、GPU（已达成）、键鼠/触控/手柄外设、USB**；放弃鸿蒙沙箱不开放裸 HCI 的蓝牙协议栈与蜂窝基带（蓝牙耳机/键鼠由鸿蒙宿主机接管经音频/输入桥天然可用）。
   * 手柄无需改动 `svbox`（沿用通用 `--vdev /dev/input/eventN:...:chr`），在 OHOS 桥接层输出标准 Linux `evdev` 手柄轴/键码即可被 Android 游戏与 Linux 原生识别；USB 经鸿蒙 NDK `usb`/`usb_serial`/`hid`/`ddk` 授权后映射为目录挂载或 `--vdev` 字符设备。
3. **存储空间、分区与零改镜像（归属 `svbox` 通用层，100% Linux 兼容）**
   * `svbox` 已原生具备 `--lower DIR[:IMAGE]`（含 EROFS 只读镜像按需读取）、`--root-mode ro|rw|deny`、`-v HOST:GUEST[:ro|rw|deny]` 与 `mount -t overlay`。
   * 后续补齐 `svbox` 通用 `--disk-mb`（`statfs`/`fstatfs` 虚拟容量视图），并在 `svbox-Android` 侧切换为：**只读 `--lower` 原版镜像（0 字节改写）+ 持久化 `filesDir/userdata` + 单文件 `-v` 注入外置 HAL + 宿主机共享目录双向 bind mount**。
4. **ROOT、内核态 KernelSU 与调试**
   * **对比结论**：Windows/Linux 完整 VM（WSA / QEMU / Cuttlefish）通过替换带 KernelSU 补丁的独立 Guest Linux 内核支持 `.ko` 模块；Linux 共享内核容器（Waydroid / Redroid）与无内核特权的 `svbox-Android` 则走**纯用户态方案**——Systemless 修改走 `svbox` `--lower`+`overlay`，细粒度 `su` 授权与 Root 隐藏走 `svbox` `fake_ids`（安全沙箱内 Root、零宿主机逃逸风险），Hook 生态走用户态 Zygisk/Riru 钩子。
   * **调试**：直接拉起镜像自带的 64 位原生 `adbd`（绑定 `127.0.0.1:5555`），一次打通 `adb shell` / `adb install` / `adb push/pull` / `adb logcat` / JDWP 断点调试，并消除当前 `logcat` 中的 `jdwp control socket` 刷屏。

### 5.3 后续演进路线（Step 5 → Step 8）

| 阶段 | 核心目标 | 沉淀到 `svbox` 核心层 | 沉淀到 OHOS 桥接 / Android 薄层 | 真机验收标准 |
| :--- | :--- | :--- | :--- | :--- |
| **Step 5** | **零改只读镜像 + 存储/兼容反哺 + `adbd` 调试台** | 修 `vdev_proto.h` 12B `packed` 宏；修 `/proc/meminfo` `fstat` `EACCES`；新增通用 `--disk-mb` 虚拟配额 | 切换 `--lower` 只读 `rootfs` + `-v` 注入全部 HAL（0 写入原镜像）；`/data` 迁至 `filesDir` 持久化 + 共享目录；拉起 `adbd` (`127.0.0.1:5555`) | `adb connect` 装/卸 APK 流畅、`jdwp` 零报错、重启数据持久化、原版 `rootfs` 哈希 0 变化 |
| **Step 6** | **`svbox` 通用用户态网络隔离与虚拟 TUN（方案 2）** | 通用用户态 `netstack` + `--vdev` 虚拟 `/dev/net/tun` + 容器内 socket 拦截与直连豁免 + 虚拟网卡视图 | 模拟器风格 `eth0`/`wlan0` 在线状态上报 + `VpnService` 接入虚拟 `tun0` | 普通 Linux 容器 `tun` 单元测试通过；Android 内实装 Clash / sing-box TUN 模式接管容器流量且不影响宿主机 |
| **Step 7** | **OHOS 通用桥接层解耦 + 手柄 / USB + 跨 Linux 复用验证** | 如有通用 `evdev`/`tty`/USB `ioctl` 缺口反哺 `svbox` | 从 HAP 抽离独立 `ohos_bridge/`（显示/GPU/输入/音频/USB/共享目录），补齐 Gamepad `evdev` 与 USB 桥接 | Android 游戏识别手柄；**用同一套 `svbox` + `ohos_bridge/` 直接拉起一个标准 Linux 发行版（如 Debian/Alpine 图形/终端）**证明通用复用 |
| **Step 8** | **跨宿主机移植验证 & 用户态 Root 管理** | 验证 `svbox` 在无 Root Android（Termux）/ 标准受限 Linux 宿主机上的可移植性 | 可选用户态 `su` 按 UID 授权管理 + 多容器实例/快照 | 同一套 `svbox` 源码在鸿蒙 PC 与无 Root Linux/Android 宿主机均通过核心回归测试 |

---

### 5.4 Step 5 实机落地与验证记录（2026-11-23）

Step 5 严格遵循“**容器通用兼容性沉淀进 `svbox`，Android 镜像零修改**”原则，在 `svbox`（`feat/vdev-binder`）上连续落地 **3 个通用核心提交**（`b927a58`、`39a6e42`、`ac81b86`），并在 `svbox-Android` 上完成零改只读 `rootfs`、持久化 `/data` + 鸿蒙双向共享目录与原生 64 位 `adbd` 调试闭环。

#### A. 反哺 `svbox` 核心层（`feat/vdev-binder`，100% 通用 Linux，零 Android 专属代码）

1. **`b927a58` — Binder ABI 修复 + `memfd` `fstat/statx` SELinux 兜底 + 通用 `--disk-mb` 配额**
   * [`vdev_proto.h`](file:///storage/Users/currentUser/Workspace/svbox/src/payload/include/vdev_proto.h)：修正 `SV_BC_REQUEST_DEATH_NOTIFICATION`（`0x400c630eu`）、`SV_BC_CLEAR_DEATH_NOTIFICATION`（`0x400c630fu`）与 `__attribute__((packed))` 12 字节 `struct sv_binder_handle_cookie`。
   * [`syscall_disp.c`](file:///storage/Users/currentUser/Workspace/svbox/src/payload/arch/aarch64/syscall_disp.c)：针对鸿蒙等 SELinux 强约束宿主机对 `memfd` inode 拒绝 `fstat`（`-EACCES`）与空路径 `statx(AT_EMPTY_PATH)`（`-EFAULT`）的问题，通过 `fcntl(fd, F_GET_SEALS) >= 0` 精确识别真实 `memfd` 并以 `lseek(SEEK_END)` 合成标准 POSIX `stat`/`statx`（彻底消灭 `/proc/meminfo: fstat failed: EACCES` 与 guest `memfd` 元数据失败）。
   * [`config.h`](file:///storage/Users/currentUser/Workspace/svbox/src/payload/include/config.h) / [`options.c`](file:///storage/Users/currentUser/Workspace/svbox/src/loader/options.c) / [`syscall_disp.c`](file:///storage/Users/currentUser/Workspace/svbox/src/payload/arch/aarch64/syscall_disp.c)：新增通用 `--disk-mb N` 参数，对非伪文件系统 `statfs`/`fstatfs` 统一钳位虚拟总容量与可用空间。
2. **`39a6e42` — 修复 `--lower` 子目录绑定挂载下的 `/proc/self/fd` 反向映射与 `union_alias` 缓冲重叠 Bug（坑 5-1）**
   * **现象**：切到 `--root <root_upper> --lower <inject_layer> --lower <rootfs>` 后，`zygote64` 启动即中止：`Not whitelisted (4): /system/apex/com.android.art.debug/javalib/core-oj.jar`（而非绑定后的 `/apex/com.android.art/javalib/core-oj.jar`）。
   * **双根因**：
     1. [`union_view.c`](file:///storage/Users/currentUser/Workspace/svbox/src/payload/logic/union_view.c) `union_alias()` 在被 [`vfs_bridge.c`](file:///storage/Users/currentUser/Workspace/svbox/src/payload/logic/vfs_bridge.c) `host_map()` 以 `pb_bridge_alias(b, b->mapped, b->mapped)` 原地调用时，当 `strlen(upper) > strlen(lower)` 时 `memcpy` 覆盖了重叠的 `rel` 尾部，导致路径损坏、所有命中 `--lower` 子目录的 bind mount 反向校验返回 `-EXDEV`；
     2. `observed_guest_path` / `preferred_guest_path` 之前先强制做 lower→upper 别名再比对 `host_prefix`，且按 `strlen(upper)` 而非**宿主真实匹配前缀长度**排序。
   * **解法**：`union_alias()` 改为 `memmove` 原地安全实现、`host_map()` 直接原地别名；`observed_guest_path_len()` / `preferred_guest_path()` / `visible_guest_path()` 优先直匹配真实宿主 bind 前缀并按宿主实际消费前缀长度排序。
3. **`ac81b86` — 修复无特权容器 `getsockopt(SOL_SOCKET, SO_PEERCRED)` 返回宿主机真实 UID（坑 5-2）**
   * **现象**：`adbd` 启动并监听 `@jdwp-control` 后，全系统 Android 进程仍高频报 `untrusted uid 20020422 on other end of socket`，导致 `adbd.log` 每分钟膨胀近 1 MB（`received data of incorrect size from JDWP client: read 0, expected 4`）且 `adb jdwp` 为空。
   * **根因**：Linux/鸿蒙内核对 `AF_UNIX` `SO_PEERCRED` 始终返回宿主真实 UID/GID（`20020422`），而 `svbox` 此前未将 `__NR_getsockopt` 纳入 `slow_syscalls.inc` 做容器 ID 还原（同样影响 `dbus-daemon`/`systemd`/`polkit`/`sshd` 等所有通用 Linux 守护进程）。
   * **解法**：在 [`slow_syscalls.inc`](file:///storage/Users/currentUser/Workspace/svbox/src/payload/arch/aarch64/slow_syscalls.inc) 拦截 `__NR_getsockopt`，在 [`fake_ids.c`](file:///storage/Users/currentUser/Workspace/svbox/src/payload/logic/fake_ids.c) 新增 `fake_ids_peercred()`：优先读取 `mount_scratch/.pid_ids/<ucred.pid>` 精确还原对端容器的 fake `euid`/`egid`，找不到时对宿主自有 UID/GID 回退为容器默认 fake ID。
4. **回归测试**：新增 [`test/vfs/test_resource_limits.py`](file:///storage/Users/currentUser/Workspace/svbox/test/vfs/test_resource_limits.py)（覆盖 `--mem-mb`、`--disk-mb`、`memfd` `fstat/fstatat/statx`、只读 `--lower`、`--lower` 子目录 bind `/proc/self/fd` 反向映射、`SO_PEERCRED`），并与 `test_union.py`（8/8 含 `--isolate` 与 EROFS）、`test_paths.py`（57/57）、`test_mounts.py`（4/4）、`test_owners.py`（4/4）、`check_payload_abi.py`（5/5）全部通过。

#### B. `svbox-Android` 零改只读镜像、持久化存储与原生 `adbd` 踩坑实录

* **多层只读 Overlay 架构**（[`android_runtime.cpp`](file:///storage/Users/currentUser/Workspace/svbox-Android/hap/entry/src/main/cpp/android_runtime.cpp)）：
  * 启动命令切换为 `--root <temp/root_upper> --lower <temp/inject_layer> --lower <files/rootfs>`，每次启动重建空的 `root_upper` 与 `inject_layer`（按需放入 `svbox-touchscreen.idc`、Audio HAL/服务、Camera Stub，仅在 GLES/Vulkan 模式下放入 14 个 `gpu_guest` 库），**原版 `rootfs` 0 字节写入**；首次升级自动清理历史版本遗留在 `rootfs/` 中的 16 个旧注入文件（由 `files/.rootfs_clean_v1` 标记）。
  * `binderd/svbox_binderd.c` 清理临时兼容宏，直接复用 `svbox` 修正后的标准 `SV_BC_REQUEST_DEATH_NOTIFICATION` / `struct sv_binder_handle_cookie`。
* **持久化 `/data` + 鸿蒙共享目录 + `--disk-mb 32768`**：
  * Android `/data` 与 `owner.db` 从易失 `temp/` 迁至持久化 `files/userdata/`；`EnsureOwnerDb()` 在保留历史伪 UID/GID 记录的同时，自动清除因上次强杀（`SIGKILL`）可能残留的 `lock` 或奇数 `version`，杜绝重启死锁。
  * 预建标准用户目录并挂载 `/sdcard`、`/storage/emulated/0`、`/storage/self/primary` 指向 `files/userdata/data/media/0`，同时自动创建宿主机 `/storage/Users/currentUser/Download/svbox-share` 双向绑定至 `/sdcard/Download/HarmonyShare`。
  * **坑 5-3：`vold` 启动扫描 `/proc/mounts` 对 `/storage/*` 子挂载执行 `ForceUnmount` 拖慢开机 20 秒**：`vold` 对每个 `/storage/` 下的已有挂载重试 `umount2()` 5 次（每次 1 秒）。通过 `BaseSvboxArgs(/*includeStorageSubmounts=*/false)` 仅在启动 `vold` 自身时隐藏 `/storage/*` 子挂载，零侵入 `svbox` 即把 `VoldNativeService::start()` 恢复至 `<10ms`，`vold_stale=0`。
  * **坑 5-4：`Settings -> Storage` 显示 `93.64 GB Used of 128 GB` 的原理解析**：`df -h /data` 与 `dumpsys diskstats` 均准确报告 `32G`（`33554432K total, 100% free`），而 Android 11 `StorageStatsService.getTotalBytes()` 在无真实 `/sys/block/*` 设备时会把 `/`（32 GiB）+ `/data`（32 GiB）相加为 `68.72 GB`，经 `FileUtils.roundStorageSize()` 向上取整至商业闪存档位 `128 GB` 再减去 `/data` 可用 `34.36 GB` 得出 `93.64 GB`。属 AOSP 既有启发式行为，不影响任何应用配额与 `StatFs` 实际判断。
* **原生 64 位 `adbd`（`127.0.0.1:5555`）连踩两坑与根治**：
  * **坑 5-5：相对路径执行 `apex/com.android.adbd/bin/adbd` 错过 Bionic `linkerconfig` 命名空间**：`/linkerconfig/com.android.adbd/ld.config.txt` 匹配 `dir.com.android.adbd = /apex/com.android.adbd/bin`（必须带前导 `/`），直接传相对路径会导致 `libadb_protos.so not found`。改为 `system/bin/sh -c "exec /apex/com.android.adbd/bin/adbd --root_seclabel=u:r:su:s0"` 后原生命中命名空间，且不污染子 shell 的 `LD_LIBRARY_PATH`。
  * **坑 5-6：空监听 `/dev/socket/fwmarkd` 导致 `adbd`（及一切容器内 TCP 服务端）在 `accept4()` 永久死锁**：旧代码创建了 `/dev/socket/fwmarkd` 与 `/dev/socket/mdns` 监听却未运行处理线程；Android Bionic `libnetd_client.so` 会在 `accept4()` 后连接 `fwmarkd` 并永远阻塞在 `af_unix_recvfrom`。保留 `/dev/socket/mdns`（`SystemServer` 的 `NsdService.awaitConnection()` 需要 `connect()` 成功以免 Watchdog 超时）并移除无服务的 `/dev/socket/fwmarkd` 后，`adbd` TCP 5555 毫秒级响应。

#### C. `HAD-W32` 真机 Step 5 全项验收结果

| 验收项 | 真机实测结果 |
| :--- | :--- |
| **`adbd` 与日志洁净度** | `adbd.log` 仅 **`3.9 KB`**（修 `SO_PEERCRED` 前为 749 KB/min）；`jdwp_refused=0`、`untrusted_uid=0`、`meminfo_fstat_err=0`、`vold_stale=0` |
| **原生 `adb` 调试链** | `adb connect 127.0.0.1:5555` 成功；`adb shell id` → `uid=0(root)`；`pm list packages` → `142`；**`adb jdwp` 列出 26 个活跃 Android 进程 PID** |
| **`adb push` / `pull` 吞吐与完整性** | 256 KiB 随机二进制往返 **`96.6 MB/s` push / `64.6 MB/s` pull**，`cmp` 逐字节一致 |
| **虚拟磁盘配额 (`--disk-mb 32768`)** | `df -h / /data /sdcard` 与 `dumpsys diskstats` 均准确返回 **`32G`**；`Settings -> Storage` Vulkan 真机截图 [`snap_step5_storage.png`](file:///storage/Users/currentUser/Workspace/svbox-Android/hap_logs/snap_step5_storage.png) 渲染清晰 |
| **鸿蒙双向共享目录** | `/storage/Users/currentUser/Download/svbox-share` ↔ `/sdcard/Download/HarmonyShare` 双向读写即时同步通过 |
| **零改只读 `rootfs` 校验** | 切至 SwiftShader 后 `inject_layer` `gpuLibs=0`，容器内 `/vendor/lib64/egl` 与 `/vendor/lib64/hw` 中 **`ranchu_count=0`**（零 `emulation`/`ranchu` 残留），证明 `rootfs/` 零写入污染 |
| **跨重启持久化 + 三 GPU 模式全回归** | 依次完成 `Vulkan → GLES → SwiftShader → Vulkan` 完整停启切换：`/data/local/tmp`、`/sdcard/Documents` 与 `settings global` 三项持久化数据 100% 保留，三种 GPU 模式均正常抵达 `BOOT_COMPLETED` |

---

### 5.5 Step 6 实机落地与验证记录（2026-11-23）

Step 6 严格遵循“**通用虚拟网络与 TUN 协议栈归属 `svbox` 核心层（零 Android/OHOS 代码），模拟器 `eth0` 与 `INetd`/`fwmarkd`/`dnsproxyd` 胶水归属 `svbox-Android` 薄层，原版 `rootfs/` 零字节修改**”架构原则，在 `svbox`（`feat/vdev-binder`）落地 **2 个核心提交**（`d894c41`、`569cd1e`），并在鸿蒙 PC（`HAD-W32`）完成从 Android 11 `VALIDATED` 网络状态到容器内 Go TUN 代理 + FakeIP DNS + Chromium WebView 透明接管的全链路实机验证。

#### A. `svbox` 核心层通用用户态网络与虚拟 TUN 栈（`feat/vdev-binder`，100% 纯 Linux 通用）

1. **`d894c41` — 通用用户态虚拟网络协议栈与 TUN 代理（`--net-sock` + `--vdev GUEST:SOCK:tun` + `svbox-net`）**
   * **内核级真 FD 虚拟 TUN**（[`vdev.c`](file:///storage/Users/currentUser/Workspace/svbox/src/payload/logic/vdev.c)、[`vdev_proto.h`](file:///storage/Users/currentUser/Workspace/svbox/src/payload/include/vdev_proto.h)）：新增 `VDEV_TYPE_TUN`，`open("/dev/net/tun" | "/dev/tun")` 通过控制通道 `SCM_RIGHTS` 返回真实内核 `AF_UNIX SOCK_SEQPACKET` FD（天然保留 IP 报文边界、完整支持 `epoll`/`poll`/`O_NONBLOCK`/`dup`/`fork`/跨进程 `SCM_RIGHTS` 传递），最后一个持有者关闭时 `svbox-net` 通过 `EPOLLHUP` 自动拆毁虚拟网卡与关联路由。
   * **独立零依赖 C11 网络守护进程 `svbox-net`**（[`tools/net/svbox_net.c`](file:///storage/Users/currentUser/Workspace/svbox/tools/net/svbox_net.c)、[`tools/net/tcp_stack.c`](file:///storage/Users/currentUser/Workspace/svbox/tools/net/tcp_stack.c)、[`tools/net/net_internal.h`](file:///storage/Users/currentUser/Workspace/svbox/tools/net/net_internal.h)）：
     * 单写者只读共享内存 `struct sv_net_state`（`memfd` + `PROT_READ` 映射，0 常驻连接），维护虚拟接口表、IPv4/IPv6 路由表、UID 策略规则与受保护 socket inode 表；
     * 内置 RFC 793/7323 用户态 IPv4/IPv6 + TCP/UDP/ICMPv4 协议栈（256 KiB 环形收发缓冲、Window Scale=6、乱序重组、快速重传与指数退避 RTO、半关闭 `FIN`/`RST`）；
     * 完整实现 `AF_NETLINK NETLINK_ROUTE`（`GET/NEW/DEL LINK/ADDR/ROUTE/RULE`）、`SIOC*` ioctl、`/proc/net/{dev,route,if_inet6}` 与 `/sys/class/net/<iface>/*` 虚拟视图（同时解决鸿蒙 HAP 沙箱读宿主 `/proc/net/*` 报 SELinux `EACCES` 与宿主网卡泄露问题）；
     * 透明接管容器内 IPv4、双栈 IPv6 `::ffff:` 与纯 IPv6 的 TCP/UDP 流量：无活跃 TUN 时 0 开销直连快路径；有活跃 TUN 时按最长前缀匹配路由注入 TUN，并通过 `getpeername`/`getsockname`/`recvfrom`/`recvmsg` 透明还原原始对端与虚拟网卡地址；
     * 提供四种防回环直连豁免机制：`SO_MARK`、`SO_BINDTODEVICE`、跨进程 socket inode 豁免（`NET_OP_PROTECT_INO`）与 UID 范围策略规则（`NET_OP_UID_RULES`），并暴露通用 `NET_OP_DNS_QUERY` RPC 供外部解析器透传 TUN FakeIP DNS。
2. **`569cd1e` — 修复实机 `iproute2` 暴露的两处通用 RTNetlink 兼容性问题（坑 6-3）**
   * **现象**：容器内执行 Android 原生 `ip addr add` / `ip route add` 时地址与路由均成功生效，但 `ip` 进程随后卡死超时。
   * **双根因**：
     1. `iproute2`（及 Go `netlink`）发包时设置 `req.nlmsg_pid = 0`，但校验内核返回的 `NLMSG_ERROR` ACK 必须满足 `reply.nlmsg_pid == local.nl_pid`；此前 `nl_send_ack()` 误填了 `req->pid`（`0`）而非已绑定的 `nc->pid`。
     2. `AF_UNIX SOCK_SEQPACKET` 在内核 `recvmsg()`/`recvfrom()` 返回时会把 `msg_namelen` 覆盖为 `0`，导致在 `post_syscall` 中 `m->msg_namelen > 0` 恒假、未写回内核 `sockaddr_nl (nl_pid = 0)`。
   * **解法**：`svbox-net` 统一用解析后的 `pid` 填充 `NLMSG_ERROR` 并在 `NET_OP_NL_BIND` 同步客户端 `nl_pid`；[`net_stack.c`](file:///storage/Users/currentUser/Workspace/svbox/src/payload/logic/net_stack.c) 改为在 `pre_syscall` 直接拦截虚拟 Netlink FD 的 `recvfrom`/`recvmsg`，在内核改写前记录调用方缓冲区容量并完整回填 `sockaddr_nl`。
3. **通用 Linux 回归与纯净度校验**：
   * `grep -rniE "android|ohos|harmony"` 扫描全部新增网络源码与测试 **0 命中**；
   * [`test/vfs/test_tun_netstack.py`](file:///storage/Users/currentUser/Workspace/svbox/test/vfs/test_tun_netstack.py)（含 256 KiB 乱序 TCP + SHA-256、双栈 IPv6、FakeIP→`tun0`→`SO_MARK` 代理环路、多对端无连接 UDP、四种绕过机制、`NET_OP_DNS_QUERY`、ICMPv4、`SCM_RIGHTS` 跨进程传递与自动拆毁、`iproute2` 风格 Netlink ACK）及 Go `net.Interfaces()`、`test_net.py`（4/4）、`test_resource_limits.py`、`test_union.py`（8/8）、`test_mounts.py`（4/4）、`test_owners.py`（4/4）、`test_paths.py`（57/57）、`check_dispatch.py`（170 slow syscalls）、`check_payload_abi.py`（5/5）**全部 PASS**。

#### B. `svbox-Android` 模拟器网络接入与踩坑实录

* **HNP 打包与守护进程编排**（[`scripts/pack_hnp.py`](file:///storage/Users/currentUser/Workspace/svbox-Android/scripts/pack_hnp.py)、[`android_runtime.cpp`](file:///storage/Users/currentUser/Workspace/svbox-Android/hap/entry/src/main/cpp/android_runtime.cpp)）：
  * 将 `svbox-net` 打入 `svbox.hnp`，启动时拉起 `svbox-net --iface eth0:10.0.2.15/24:10.0.2.2:02:00:00:00:00:01 --dns 223.5.5.5`，为所有容器进程统一传递 `--net-sock` 与 `--vdev /dev/tun:...:tun` + `--vdev /dev/net/tun:...:tun`；预置 AOSP `IpConfigStore` v3 格式 `/data/misc/ethernet/ipconfig.txt`，使 `EthernetService` 自动完成静态 IP 配置。
* **完整 `svbox-netd`（`INetd` + `IDnsResolver` + `INetdUnsolicitedEventListener` + `/dev/socket/fwmarkd`）**（[`netd/svbox_netd.c`](file:///storage/Users/currentUser/Workspace/svbox-Android/netd/svbox_netd.c)）：
  * 基于 `svbox-net` 实时驱动 `interfaceGetList/GetCfg/SetCfg/AddAddress/DelAddress/ClearAddrs/SetMtu`、`networkCreatePhysical/CreateVpn/Destroy/AddInterface/RemoveInterface/AddRoute/AddRouteParcel/AddUidRanges/RemoveUidRanges/SetDefault` 与 `IDnsResolver.setResolverConfiguration`，并启动独立线程服务 `/dev/socket/fwmarkd`：收到 `PROTECT_FROM_VPN (cmd=3)` 或物理网 `SELECT_NETWORK (cmd=2)` 时提取 `SCM_RIGHTS` socket 的 `fstat().st_ino` 注册至 `svbox-net` `NET_OP_PROTECT_INO`，立即返回 `0`（既彻底杜绝 Step 5 遇到的 `adbd` `accept4` 死锁，又完整打通 `VpnService.protect(fd)`）。
* **多坑排障记录**：
  * **坑 6-1：`TUNSETIFF` 的 `_IOC_SIZE` 陷阱**：Linux 头文件定义 `TUNSETIFF = _IOW('T', 202, int) = 0x400454ca`（编码长度仅 4B），实际读写 40B `struct ifreq`。在 `vdev.c` `tun_ioctl()` 中对 `TUNSETIFF`/`TUNGETIFF`/`TUNSETQUEUE` 显式做 40B 双向序列化，并放行 `FIONBIO`/`FIONREAD`。
  * **坑 6-2：鸿蒙内核 `accept4()` 从 `listen_fd` 继承 `O_NONBLOCK`**：标准 Linux 上 `accept4(..., SOCK_CLOEXEC)` 返回的新连接始终为阻塞模式，但鸿蒙内核会继承非阻塞监听 socket 的 `O_NONBLOCK`，导致 `svbox-net` 接收短控制 RPC 时偶发 `EAGAIN`/`BrokenPipeError`。在 `svbox_net.c` 对所有接受的控制连接显式清除 `O_NONBLOCK` 并设置 2s 超时后彻底根除。
  * **坑 6-4：Android 11 `NetworkMonitor` 校验失败与 `dnsproxyd` 协议三重差异**：
    1. 旧启动参数 `-e ANDROID_DNS_MODE=local` 会令 `libnetd_client.so` 直接拒绝 `resnsend`（返回 `ENOSYS`），导致 `NetworkMonitor` 一直 `everValidated=false`；移除该环境变量并实现 `resnsend`（直接回写 `[be32 rcode][be32 len][dns_wire]`）后，`PROBE_DNS` 8–12ms 通过。
    2. 反汇编原版 `libc.so`（`android_getaddrinfo_proxy` / `android_read_hostent`）与 `libnetd_client.so`（`getNetworkForDnsInternal`）确认：`getaddrinfo`、`gethostbyname` 与 `getdnsnetid` 严格只读 **4 字节** `"222\0"` 头（此前发送 9 字节 `"222 0 ok\0"` 导致后续二进制流整体错位 5 字节）；同时 Bionic `AI_NUMERICSERV (0x08)` / `AI_ADDRCONFIG (0x400)` 与鸿蒙 musl 定义正好互换，且 `hints==NULL` 时 Bionic 发送 `-1`。全部按字节对齐修正并改为多 worker 并发处理（避免容器内 TUN 代理查询上游 DNS 时与被代理应用产生单线程死锁），同时支持活跃 TUN 时按调用方 fake UID 路由 `NET_OP_DNS_QUERY` 获取 FakeIP。
  * **坑 6-5（附带发现，留待后续专题修复）**：实测 `pm install` 时，`PackageInstallerSession.validateApkInstallLocked()` 内非 Binder 线程池线程向 `servicemanager` 查询同进程自持服务（`platform_compat`）会阻塞在 `svbox-binderd` `af_unix_recvmsg`（而独立进程执行 `service check platform_compat` 5ms 返回），系既有 `svbox-binderd` 同进程多线程 `BR_REPLY` 路由边界问题，与本次网络栈无关，后续可针对性修复以打通热装第三方 APK。

#### C. `HAD-W32` 真机 Step 6 全项验收结果

| 验收项 | 真机实测结果 |
| :--- | :--- |
| **模拟器风格 `eth0` 与 Android 联网认证** | `dumpsys connectivity`：`Active default network: 100`（`Ethernet`，`eth0 10.0.2.15/24`，gw `10.0.2.2`，DNS `223.5.5.5, 8.8.8.8`），**`Capabilities: ...&VALIDATED`**、`Score{70}`、`everValidated{true}`、`lastValidated{true}`；`PROBE_DNS` 10–12ms、`PROBE_HTTP` 140ms `204`、`PROBE_HTTPS` 830ms `204` |
| **容器网络视图隔离 & 无 SELinux 报错** | 容器内 `ip addr` / `ip route` / `/proc/net/{dev,route}` 与 Go `net.Interfaces()` 仅见 `lo` 与 `eth0`，无任何 `Permission denied` 与宿主网卡泄露；`/dev/tun`、`/dev/net/tun` 均呈 `crw-rw-rw- 10, 200` |
| **Bionic 全协议 DNS（含 `ping`）** | `getaddrinfo`、`resnsend`、`getdnsnetid`、`gethostbyname` 全通；`adb shell ping -c 1 www.baidu.com` 正常解析并 `0% packet loss (5.86 ms)` |
| **容器内 Go TUN 代理全链路（10/10 PASS）** | 静态 ARM64 Go TUN 代理打开 `/dev/tun` 创建 `tun0 (10.200.0.1/24)` + `ip route add default dev tun0`：<br>① **FakeIP DNS → TUN TCP → `fwmarkd PROTECT_FROM_VPN` → 宿主机 HTTP**：`200 OK`（`netd.log` 记录 `fwmarkd cmd=3 protected socket ino=...`）；<br>② **FakeIP DNS → TUN TCP → `SO_MARK` 绕过 → 宿主机 HTTP**：`200 OK`；<br>③ **直返 TUN TCP & UDP 回显 (`198.19.1.44:5353`)**：均通过；<br>④ **真实互联网 `connectivitycheck.gstatic.com:80` 经 `tun0` → `fwmarkd` 代理**：返回 `HTTP/1.1 204 No Content`；<br>⑤ **真机 Android Chromium WebView（`Chrome/125.0.6422.113`）打开 `http://fwmark-test.svbox.local/from-android-webview`**：真实 App 流量无缝命中 FakeIP DNS、经 `tun0` 与 `fwmarkd` 抵达宿主机服务端（连带 `/favicon.ico` 均成功，`/proc/net/dev` 实时统计 `tun0: RX 3445B/46pkts, TX 3149B/40pkts`） |
| **宿主机零污染 & 自动拆毁恢复** | TUN 代理运行期间宿主机 `/proc/net/dev` 完全无 `tun0`；退出 TUN 代理后 `svbox-net` 自动拆毁 `tun0` 与默认路由，容器恢复至 `lo` + `eth0` 且直连外网 `HTTP/1.1 204 No Content` 立即恢复 |
| **`svbox-netd` VPN 控制面 RPC** | `service call netd 5`（`networkCreateVpn(101)`）与 `service call netd 6`（`networkDestroy(101)`）均返回 `Parcel(00000000)` 零异常 |
| **零改只读 `rootfs/` 校验** | `rootfs/` 0 字节改动，`adbd` (`127.0.0.1:5555`) 响应流畅无卡死 |

---

### 5.6 Step 6.5 与 Step 7 实机落地与验证记录（2026-11-23）

#### A. Step 6.5：根因定位并修复 `pm install` 挂死（反哺 Layer 1 `svbox` `vdev.c`）

* **真因定位（通用容器运行时缺陷，非 Android 特有）**：
  * `svbox/src/payload/logic/vdev.c` 中 `struct vdev_open_slot` 静态固定 `VDEV_MAX_THREADS = 64` 且从未回收已退出线程；`system_server` 启动后常驻 ~112–136 个线程，导致第 65+ 个线程（如 `PackageInstaller`、`StorageManager`）每次 `ioctl(BINDER_WRITE_READ)` 均在 `get_thread_rpc_fd()` 中新建并泄漏独立 `rpc_fd`，第一次发送 `BC_TRANSACTION` 在通道 A，第二次 `waitForResponse` 却挂在全新通道 B 等待 `BR_REPLY`，永久阻塞在 `af_unix_recvmsg`（并令 `system_server` 与 `svbox-binderd` 泄漏 400+ FD）。
* **Layer 1 `svbox` 通用修复（`feat/vdev-binder`，零 Android/OHOS 代码）**：
  1. `vdev.c`：将每槽线程表改为按需 `mmap` 动态扩容（初始 256，上限 4096），扩容前以 `__NR_tgkill(pid, tid, 0) == -ESRCH` 自动回收已退出线程并关闭其 `rpc_fd`；收到 `SV_BINDER_THREAD_EXIT` 后立即清理当前线程通道；`VDEV_MAX_OPEN` 提升至 32；通用字符设备 `ioctl` 统一规范化 32 位命令字。
  2. 新增通用多线程回归单测 `test/vfs/test_vdev_threads.py`（128 并发线程状态会话 + 320 短生命周期线程回收验证，`total_attaches=448, peak_live=257`）。
* **`HAD-W32` 真机结果**：
  * `system_server` FD 数由 **741 → 325**，`svbox-binderd` FD 数由 **668 → 319**；
  * `adb shell pm install -r /data/local/tmp/vpntest.apk`：**`Success`（0.26–0.29s）**，`pm uninstall org.svbox.vpntest`：**`Success`（0.23–0.30s）**。

#### B. Step 7：Layer 2 `ohos_bridge/` 解耦 + 手柄/USB 桥接 + 标准 ARM64 Linux 发行版真机复用验证

* **Layer 1 `svbox` 通用增强（标准 Linux `evdev` / `termios` / `usbfs` / BusyBox 网络兼容）**：
  1. `vdev.c`：`VDEV_TYPE_CHR` 字符设备对 `FIONBIO (0x5421)` / `FIONREAD (0x541b)` 透明直通内核流 socket；支持传统 Linux `termios`/`winsize`/`TIOCM*`（`TCGETS=0x5401` 36B 等）；支持标准 Linux `usbfs`（`USBDEVFS_CONTROL` / `USBDEVFS_BULK` 内嵌 `void *data` 指针双向封送，以及 `USBDEVFS_CLAIMINTERFACE` / `GETDRIVER` / `CONNECTINFO` / `GET_CAPABILITIES`）；修正 `fill_chr_stat()` `makedev` 编码及默认 minor（`220 + vdev_idx`）消除 8-bit 溢出。
  2. `net_proto.h` / `net_stack.c` / `svbox_net.c`：补齐 BusyBox `ip addr` 所需的 `SIOCGIFTXQLEN (0x8942)` / `SIOCSIFTXQLEN (0x8943)`。
  3. 新增通用单测 `test/vfs/test_vdev_chr_ioctls.py`，全量 `svbox` 套件 + 零关键词纯净度校验全部 PASS。
* **Layer 2 `ohos_bridge/` 独立静态库（零 Android 业务代码，同时服务 HAP 与独立 CLI）**：
  * 从 `hap/entry/src/main/cpp/` 彻底抽离并删除旧副本，形成独立 `ohos_bridge/` 模块（`CMakeLists.txt` + `Makefile` + `include/ohos_bridge/*` + `src/*`）：
    * `SurfaceBridge`（`OHNativeWindow` + `OH_NativeVSync`，同时支持 HWC prime-FD 与通用 RGBA 帧缓冲服务 `/ipc/fb.sock`）
    * `GfxstreamBridge`（动态 `libgfxstream_host.so`）
    * `InputBridge`（`/dev/input/event0` 触控、`/dev/input/event1` 键盘、**新增 `/dev/input/event2` 标准 Linux Xbox 360 手柄 `svbox-gamepad`** + 动态挂载鸿蒙 `GameControllerKit` `libohgame_controller.z.so` 21 路按键/摇杆/扳机监听）
    * `AudioBridge`（动态 `libohaudio.so` 播放，HAP 与独立 CLI 均可出声）
    * `UsbBridge`（**新增 `/dev/bus/usb/001/002` 标准 Linux `usbfs` + `/dev/ttyUSB0` 虚拟串口**，支持 `libusb_ndk.z.so` 探测、宿主委托回调与内置回环/AT 响应器）
    * `SchedQos`（动态 `libqos.so` + `libohhidebug.so` + 20 核 L/M/B 拓扑探测）
    * `OhosBridge::ContainerRunner` + 独立运行器 `ohos_bridge/tools/svbox_linux.cpp`（`build/svbox-linux`）。
* **Layer 3 `svbox-Android` HAP 瘦身**：
  * `hap/entry/src/main/cpp/` 仅保留 `CMakeLists.txt`、`android_runtime.{h,cpp}` 与 `napi_init.cpp`，通过 `add_subdirectory(ohos_bridge)` 直接链接 `libohos_bridge.a`，并同步向 Android 11 暴露 `/dev/input/event2`、`/dev/bus/usb/001/002`、`/dev/ttyUSB0` 及 NAPI 手柄注入接口。

#### C. `HAD-W32` 真机 Step 7 双系统（Alpine Linux 3.21.3 ARM64 + Android 11）验收结果

| 验收项 | 真机实测结果 |
| :--- | :--- |
| **标准 ARM64 Linux 发行版真机运行** | 原版未改 **Alpine Linux 3.21.3 ARM64** 通过 `svbox` + `ohos_bridge/build/svbox-linux` 直接启动（`Linux svbox-linux 6.1.0-svbox aarch64`，只读 `--lower` + 可写 `--upper` + 宿主共享目录 `/mnt/share`），`SchedQos` 探测 `20c (sys: L=4 M=8 B=8)` |
| **Alpine 虚拟网络 & `apk` 真机装包** | `ip addr` 洁净输出 `eth0 10.0.2.15/24 qlen 1000`（零 `SIOCGIFTXQLEN` 报错）；经 `svbox-net` 完成 `apk update`（25,263 包）+ `apk add python3`（17 包，49 MiB，`Python 3.12.16`）；`/dev/net/tun` 创建与自动拆毁全通 |
| **Alpine 手柄 / 键盘 `evdev` 链路** | 鸿蒙 `GameControllerKit` 成功注册 21 路监听器（`gck=1`）；Alpine 内打开 `/dev/input/event2`（`svbox-gamepad`，`USB 045e:028e`）与 `/dev/input/event1`，实收注入的 `ABS_X/Y/RZ/HAT0X` + `BTN_SOUTH`（8 事件）与 `KEY_A`（4 事件） |
| **Alpine Linux `usbfs` & `/dev/ttyUSB0`** | 读取 `/dev/bus/usb/001/002` 50B 原始 USB 描述符（`VID:PID=1d6b:0104`），`USBDEVFS_GET_CAPABILITIES` / `CONNECTINFO` / `CLAIMINTERFACE` / `GETDRIVER("usbfs")` / `CONTROL GET_DESCRIPTOR("HarmonyOS Virtual USB Bridge")` / `BULK 24B` 往返全通；`/dev/ttyUSB0` `TCGETS`/`TIOCGWINSZ` + `AT\r\n` → `OK\r\n` 通过 |
| **Alpine 鸿蒙 `OHAudio` & 通用 RGBA 帧缓冲** | Alpine 经 `/ipc/audio.sock` 推送 35,280B 440Hz PCM，真机拉起 `libohaudio.so` 播放（`audio_backend=1, audio_rx=audio_played=35280`）；经 `/ipc/fb.sock` 推送 320×240 RGBA 帧获 `"ok"` ACK，宿主机像素校验 100% 一致 |
| **Android 11 零回归 + 手柄/USB 原生识别** | 解耦后的 `org.svbox.android` 真机启动流畅：`dumpsys input` 识别 **`svbox-touchscreen` (`event0`)、`svbox-keyboard` (`event1`)、`svbox-gamepad` (`event2`, `Classes: 0x80000141 EXTERNAL\|JOYSTICK\|GAMEPAD\|KEYBOARD`, `Sources: 0x01000511`)**；`/dev/bus/usb/001/002` 返回 50B 描述符；`dumpsys connectivity` `VALIDATED (Score 70)`；`pm install` **0.26s** / `pm uninstall` **0.30s**；`SurfaceFlinger` 正常渲染，`rootfs/` 0 字节改动 |

---

### 十、Step 8：全链路性能优化、桩岛 100% 兜底、高并发网络与 VFS 模块化重构

#### A. 核心工程落地项

1. **桩岛 100% 兜底 + 共享区域页级复用池（`stub_island.h` / `hook_runtime.c` / `dispatch.c` / `syscall_disp.c`）**
   * 修正末尾指令越界检查（`slot + 31 * 4`），避免近边界截断。
   * 升级三阶段空洞探测（`1 MiB → 64 KiB → 4 KiB` 边缘扫描）并将临时 `small_list[256]` 移至栈上（消除临时堆/映射开销）。
   * 新增 **Shared Region Stub Pool**（64 区域 × 256 KiB，64 页/区位图管理，严格 W^X `PROT_NONE → RW → RX → PROT_NONE`）及 `hook_on_munmap()` 自动回收机制，消除 JIT 代码高频映射下的页泄漏与反复 `mmap/munmap` 抖动。
   * 对超大（>256 MiB）或 ±128 MiB 虚拟地址空间被占满的极端场景，在 `--seccomp-backstop` 开启时 **100% 自动降级至 `SIGSYS` 兜底拦截**（未开启 backstop 时严格 fail-closed 返回 `-ENOMEM`）。
   * 新增通用单测 `test/vfs/test_stub_island_fallback.py`（5,000 次 JIT 池循环、64 KiB 窄缝探测、0 字节空闲空间的 `--seccomp-backstop` 100% 兜底、无 backstop fail-closed 全通）。

2. **高并发用户态虚拟网络（`net_proto.h` / `net_internal.h` / `tcp_stack.c` / `svbox_net.c` / `net_stack.c`）**
   * 并发容量整体提升 **8×**：`NET_MAX_TCP_CONNS` / `NET_MAX_UDP_PORTS` / `SV_NET_FLOW_CAP` / `SV_NET_PROTECT_CAP` 由 `256 → 2048`，`SV_NET_UDP_REP_CAP` 升至 `1024`，`NET_MAX_NLS` 升至 `256`，`NET_MAX_DNS_REQS` 升至 `128`。
   * `tcp_stack.c` 引入 **O(1) 四元组 TCP 哈希表**（`tcp_buckets[4096]` + 轮转槽位分配游标 + `active_conns == 0` 跳过定时扫描）；`svbox_net.c` 引入单槽 `net_sync_flow_slot()`（避免每次建连/断连拷贝整张 2048 槽流表）、`listen(512)`、`epoll_wait(256)` 及 TUN 单轮 64 包公平限流。
   * `net_stack.c`：`g_mark_fds` 改为按需 `mmap` 动态扩容（64 静态内联 → 256 → 4096），并加入原子活跃计数快路径（`g_nl_active_count` / `g_mark_active_count == 0` 时零加锁零扫描）与 `/proc`/`/sys` 首字节快速剪枝。
   * 新增高并发压测 `test/vfs/test_net_high_concurrency.py`：**320 路并发 TCP 经虚拟 TUN 全双工回显 + 128 路并发 `SO_MARK` 旁路 socket 在 0.28s 内全部通过**。

3. **零 `mmap` Binder 热路径 & `svbox-binderd` O(1) 索引（`vdev.c` / `svbox_binderd.c`）**
   * `vdev.c`：`binder_write_read()` 彻底消除每次调用 `mmap(256 KiB)` + `munmap(256 KiB)`——常规事务（≤ 8 KiB）直接使用栈上 8 KiB 缓冲区（零 `mmap`、零缺页、零 TLB shootdown），超大事务复用每线程懒分配缓存的 256 KiB 缓冲区并在线程退出/关槽时回收；`find_slot_by_fd()` 增加 `g_vdev_open_count` 原子快路径跳过普通 FD 的 `getpid()`。
   * `svbox-Android/binderd/svbox_binderd.c`：为 `find_thread()`、`find_node()` 与 `get_or_create_node()` 引入 8192 桶整数索引 O(1) 哈希表（天然兼容 `realloc`），`listen(512)` + `epoll_wait(256)`。

4. **VFS 模块化拆分 + 5 项热路径优化（`vfs_*.c` / `union_view.c` / `container_state.c`）**
   * 将原 2,513 行（107 KB）的 `vfs_bridge.c` 清晰拆分为 5 个职责单一的模块（均控制在 10–34 KB）：
     * `vfs_bridge.c`（656 行 / 24 KB）：`Bridge` 池化、`mounts_version()` 挂载/绑定/视图表跨调用复用、隔离模式乐观重试与公开 `proot_*` 接口
     * `vfs_ops.c`（810 行 / 35 KB）：系统调用分发、路径变更、R5 属主、socket 地址改写与恒等根（`--root /` 无绑定）零遍历直通快路径
     * `vfs_walk.c`（718 行 / 26 KB）：路径解析、单次调用 `last_map` 去重、`union_map` 复传 `struct stat` 消除二次 `lstat`、受保护目录遍历与 `union_prepare`
     * `vfs_cache.c`（514 行 / 17 KB）：只读 `--lower` 不可变 stat/空目录/readlink 缓存 + 带 8 次稳态门限及 `(dev, ino, mtime, ctime, size, nlink, mut_gen)` 强校验的 `upper` 目录 256-bit Bloom 过滤与白化摘要缓存
     * `vfs_glue.c`（242 行 / 10 KB）：绑定挂载合成父目录（glue）树管理
   * `union_view.c`：跳过底层无效 `.wh.` 检查；`walk()` 直接回填 `hit->st`；结合只读空目录裁剪 `child_mask` 与稳态 `upper` 目录摘要避免冗余 `hmdfs` / `f2fs` 系统调用。
   * 同步清理了 `svbox-Android/` 根目录 36 个历史遗留 `hdc-*.log` 与 `__pycache__/` 杂物，保持三层架构目录清爽。

#### B. 5 轮前后性能对比（`HAD-W32` 大核 `12–19`，5 轮中位数 `[min..max]`）

> 同口径基线（`HOST` 本地 f2fs 5 轮前后完全一致：`stat_rw 3.114 vs 3.115 µs`、`open_rw 5.402 vs 5.461 µs`、`small300 24.64 vs 24.97 ms`），原始 JSON 留档于 scratch 目录 `before_baseline.json` / `after_baseline.json`。

| 指标 | `SVBOX_BARE` 优化前 | `SVBOX_BARE` 优化后 | 提速 | `ALPINE_OVERLAY` 优化前 | `ALPINE_OVERLAY` 优化后 | 提速 |
| :--- | :---: | :---: | :---: | :---: | :---: | :---: |
| **`stat_rw`（可写文件）** | `19.61 µs` `[19.55..19.91]` | **`3.75 µs`** `[3.74..4.43]` | **5.23×** | `33.96 µs` `[33.81..34.10]` | **`11.59 µs`** `[11.53..11.61]` | **2.93×** |
| **`stat_ro`（只读底层）** | `19.56 µs` `[19.53..19.70]` | **`3.68 µs`** `[3.66..3.73]` | **5.32×** | `141.38 µs` `[140.75..141.84]` | **`21.72 µs`** `[21.60..21.77]` | **6.51×** |
| **`stat_miss`（不存在路径）** | `20.47 µs` `[20.45..20.55]` | **`4.00 µs`** `[3.98..4.04]` | **5.12×** | `105.09 µs` `[104.88..106.18]` | **`9.14 µs`** `[9.10..9.24]` | **11.50×** |
| **`open+close_rw`** | `21.76 µs` `[21.46..22.13]` | **`6.82 µs`** `[6.74..6.92]` | **3.19×** | `38.61 µs` `[38.44..38.94]` | **`16.34 µs`** `[16.21..17.01]` | **2.36×** |
| **`open+close_ro`** | `21.93 µs` `[21.74..22.01]` | **`6.73 µs`** `[6.66..7.39]` | **3.26×** | `219.79 µs` `[219.17..220.53]` | **`38.62 µs`** `[38.54..38.84]` | **5.69×** |
| **`small300`（300 小文件全周期）** | `44.97 ms` `[44.59..57.01]` | **`26.94 ms`** `[25.97..30.52]` | **1.67×** | `127.75 ms` `[126.28..130.19]` | **`52.82 ms`** `[52.34..53.22]` | **2.42×** |
| **`jit`（RW→RX 桩岛周期）** | `101.04 µs` `[76.25..127.81]` | **`42.77 µs`** `[40.87..46.44]` | **2.36×** | `87.61 µs` `[70.14..101.26]` | **`42.58 µs`** `[28.57..42.74]` | **2.06×** |
| **`exec`（fork+exec `/bin/true`）** | `38.44 ms` `[37.59..39.36]` | **`36.37 ms`** `[35.71..37.23]` | **1.06×** | `9.01 ms` `[8.44..9.09]` | **`8.68 ms`** `[8.35..8.86]` | **1.04×** |

#### C. 全量回归与双系统真机验收

* **`svbox` 全量回归**：25/25 测试全部 PASS（含 `test_union.py`、`test_mounts.py`、`test_owners.py`、`test_paths.py`、`test_svc.py` 84 项、`test_go_abi.py`、`test_many_blocked.py`、`test_fd_table`、`test_fs_policy`、`test_stub_island_fallback.py`、`test_net_high_concurrency.py`、`test_tun_netstack.py`、`test_vdev_threads.py`、`test_vdev_chr_ioctls.py`），Layer 1 零 Android/OHOS 新增代码纯净度检查通过。
* **Alpine 3.21.3 ARM64 真机端到端验收**：原版只读 rootfs 正常启动，`/dev/input/event2`、`/dev/bus/usb/001/002`、`/dev/ttyUSB0`、`/ipc/fb.sock`、`eth0`/`lo` 全通。
* **Android 11 真机验收（重编打包 HNP+HAP 安装实测）**：Vulkan 模式稳定进桌面，`SurfaceFlinger` 60Hz 正常，`eth0` `VALIDATED (Score 70)`，触控/键盘/手柄全在线；3 轮 `pm install` 耗时由 Step 6.5 的 `0.26–0.29s` 进一步降至 **`0.188–0.220s`（中位数 `0.200s`，↑ ~30%）**，`rootfs/` 保持 0 字节改动。







