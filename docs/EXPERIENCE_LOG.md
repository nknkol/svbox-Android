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

---

### 十一、Android 14 (`API 34`) 迁移、60 / 90 / 120Hz 高刷切换与常用固定分辨率预设（2026-11-24）

#### A. 架构边界与分层改动

1. **Layer 1 `svbox`（`feat/vdev-binder`，零 Android/OHOS 专用代码，26/26 全量回归通过）**
   * **标准 Linux `O_PATH` `fchmod`/`fchown` 语义（`src/payload/logic/vfs_ops.c`）**：对 `O_PATH` 文件描述符执行 `fchmod`/`fchown` 返回 `-EBADF`，修复 `fchmodat(..., AT_SYMLINK_NOFOLLOW)` 回退路径，使 Android 14 `linkerconfig` 正常退出 0。
   * **通用用户态 `bpf(2)` 仿真（`--emulate-bpf`，`bpf_emu.{h,c}` / `dispatch.c` / `options.c` / `test/vfs/test_bpf_emu.py`）**：支持 `BPF_MAP_CREATE`、`BPF_OBJ_PIN`、`BPF_OBJ_GET`、`BPF_OBJ_GET_INFO_BY_FD`、`BPF_MAP_{LOOKUP,UPDATE,DELETE}_ELEM`、`BPF_MAP_GET_NEXT_KEY`、`BPF_PROG_{ATTACH,DETACH}`（容量 `128` maps / `256` fds，栈路径缓冲收敛至 2 KiB）。
   * **虚拟 POSIX DAC 校验（`owners_check_access()`，`container_state.{h,c}` + `vfs_ops.c` `faccessat`/`faccessat2`）**：对 `W_OK` 及 `owner.db` 记录项严格按虚拟 POSIX 权限判定（满足 Android 14 只读 DEX 安全校验），对未记录项在宿主机剥离 `other` 权限位的挂载（如 `hmdfs` `0660`/`02771`）上保留属主可读/可执行可见性，修复 `SystemConfig` 读取 `/vendor/etc/permissions/*.xml` 失败。
   * **标准 Linux `AF_NETLINK` 补齐（`net_stack.c` + `tools/net/svbox_net.c`）**：在虚拟 `AF_NETLINK` socket 上实现 `__NR_connect` 与 `__NR_getpeername`；在 `svbox-net` 地址增删时向 `RTMGRP_IPV4_IFADDR`/`RTMGRP_IPV6_IFADDR` 组播 `RTM_NEWADDR`/`RTM_DELADDR`，并补齐标准 Linux `IFA_CACHEINFO (6)` 与 `IFA_FLAGS (8)` 属性，彻底修复 Android 14 `NetlinkMonitor` / `IpClient` `connect failed: EINVAL` 与 `unparsable netlink msg`。

2. **Layer 2 `ohos_bridge/` 与 `libgfxstream_host.so`**
   * `gpu/ohos_host/svbox_gfxstream_capi.{h,cpp}`：`setAvdInfo(true, 34)`，新增 `svbox_gfx_set_vsync_hz(int hz)` 动态同步宿主渲染器 VSync 周期。
   * `ohos_bridge/{include/ohos_bridge/gpu_bridge.h,src/gpu_bridge.cpp}`：暴露 `GfxstreamBridge::SetRefreshRate(int hz)`，重编并更新 `hap/entry/libs/arm64-v8a/libgfxstream_host.so`。

3. **Layer 3 `svbox-Android`（Android 14 兼容层 + 高刷与分辨率控制）**
   * **只读 Android 14 根文件系统**：`rootfs14/` 从 `redroid14-arm64.tar.gz` 解包并严格只读（0 字节改动）；HAP 内置导入镜像 `rootfs14.tar` 直接由 `gzip -dc` 解压重打，确保全量 POSIX 权限为纯净 `0755`/`0644`（导入标记 `.imported_a14_v2`）。
   * **`propd/svbox_propd.c`**：单属性区扩容至 `1024 KiB`，满足 Android 14 更大属性空间。
   * **`netd/svbox_netd.c`**：升级至 Android 14 `INetd=12` / `IDnsResolver=9` 接口版本、哈希与事务码；新增 `android.system.suspend.ISystemSuspend/default`（显式写入 `@VintfStability` 标记 `0x3f`、`version=1`）+ `IWakeLock` 及 `suspend_control{,_internal}` AIDL 桩，彻底消除原版 `android.system.suspend-service` 的 Scudo double-free 崩溃与 `PowerManagerService` 阻塞。
   * **`gpu/android_guest/hwcomposer_ranchu.cpp`**：HWC2 暴露 3 档刷新率配置（`config 0 = 60Hz`、`1 = 90Hz`、`2 = 120Hz`），支持运行时 `setActiveConfig()` 原子切换 `vsyncPeriodNs_` 并唤醒 VSync 线程，同时动态读取 `ro.boot.redroid_dpi`/`ro.sf.lcd_density` 上报 DPI。
   * **`hap/entry/src/main/cpp/android_runtime.{h,cpp}`**：
     * 完整迁至 Android 14（`rootfs14`/`userdata14`、`5.15.0-android14`、`--emulate-bpf`、A14 ClassPath、`apex-info-list.xml`、`linkerconfig`、静态 `eth0`、64 位 `audio.service`/`cameraserver`/`keystore2`/`credstore`/`artd`）。
     * 在 `kAndroid14BpfSpecs` 中补全 `map_gpuMem_gpu_mem_total_map`、`prog_gpuMem_*`、`map_gpuWork_*`、`prog_gpuWork_*` 与 `netd_shared` 描述符，根治 `system_server` 在 `libmeminfo.so` (`BpfMapRO::abortOnMismatch`) 中的静默 `SIGABRT`。
     * 新增 `ComputeDpiForResolution()`、热切换 `SetRefreshRate(hz)`（联动 `hwcomposer` + `GfxstreamBridge` + Android `DisplayModeDirector` `peak_refresh_rate`/`min_refresh_rate`）及非 60Hz 启动后的刷新率自动同步。
   * **HAP 交互层（`napi_init.cpp` / `Index.d.ts` / `EntryAbility.ets` / `Index.ets`）**：接入 `OH_NativeXComponent_SetExpectedFrameRateRange`，新增工具栏 `60Hz / 90Hz / 120Hz` 实时热切换按钮、5 组常用固定分辨率预设菜单（竖屏 `720×1280@320dpi`、`1080×1920@420dpi`；横屏 `1280×720@240dpi`、`1600×900@280dpi`、`1920×1080@320dpi`）与等比居中视口自适应，以及 `Want` 参数（`resolution`、`refreshRate`、`setRefreshRate`）。

#### B. 关键排障根因速记

| 现象 | 根因 | 修复 |
| :--- | :--- | :--- |
| `system_server` 启动后在 `libmeminfo.so` 无日志 `SIGABRT` | `ReadProcessGpuUsageKb()` 构造 `BpfMapRO("/sys/fs/bpf/map_gpuMem_gpu_mem_total_map")`，缺失时 `abortOnMismatch()` 直接调用 `abort()` | 在 `kAndroid14BpfSpecs` 预置 GPU BPF map/prog，验证 `dumpsys meminfo -s` 退出 0 且 `system_server` 0 崩溃 |
| `PowerManagerService` 阻塞或 `system_server` `SIGSEGV (0x4)` | 原版 `android.system.suspend-service` 触发 Scudo double-free；初版桩对象未带 `VINTF (0x3f)` 稳定性标记 | 在 `svbox-netd` 中以 `p_binder_stab(..., 0x3f)` 托管 `ISystemSuspend/default` 与 `IWakeLock` |
| `SystemConfig` 无法读 `/vendor/etc/permissions`，`EthernetService` 未启动 | 宿主机 `hmdfs` 剥离 `other` 权限位（`02771`/`0660`）且 `owners_check_access()` 仅判 `fmode & 7` | Layer 1 `owners_check_access()` 保留未记录文件属主读/执行位 + `rootfs14.tar` 直接由 `gzip -dc` 重打为 `0755`/`0644` |
| `IpClient/eth0` 报 `connect failed: EINVAL` 及 `unparsable netlink msg` | 虚拟 `AF_NETLINK` 缺 `connect()` 处理；`RTM_NEWADDR` 未广播且缺 `IFA_CACHEINFO (6)`/`IFA_FLAGS (8)` | Layer 1 `net_stack.c` 补 `AF_NETLINK` `connect`/`getpeername`；`svbox_net.c` 组播 `RTM_NEWADDR` 并附标准 `IFA_CACHEINFO`/`IFA_FLAGS` |

#### C. `HAD-W32` 真机端到端验收结果

| 验收项 | 真机实测结果 |
| :--- | :--- |
| **Android 14 启动与稳定性** | `ro.build.version.release=14`、`ro.build.version.sdk=34`，`sys.boot_completed=1`（~15–20s），**`svcs=229`**，`system_server` 0 次 Fatal、`dumpsys meminfo -s` 退出 0 |
| **Maleoon 916 Vulkan GPU 渲染** | `GLES: Google (HUAWEI), Android Emulator OpenGL ES Translator (Maleoon 916), OpenGL ES 3.1 (OpenGL ES 3.2 B312)`，`vk=1`；竖屏/横屏抓图目视 QuickstepLauncher 清晰无花屏 |
| **60 / 90 / 120Hz 热切换** | 运行时依次切换 `60Hz → 90Hz → 120Hz`：`hwcomposer_ranchu` 实时打印 `setActiveConfig config=0 (16666666 ns)` → `config=1 (11111111 ns)` → `config=2 (8333333 ns)`，`dumpsys display` `activeMode` 精准命中 `60.00 Hz` / `90.00 Hz` / `120.00 Hz` |
| **5 组常用固定分辨率预设** | 全部实测启动并校验 `wm size` / `wm density`：`720×1280@320dpi`、`1080×1920@420dpi`、`1280×720@240dpi`、`1600×900@280dpi`、`1920×1080@320dpi` 均 100% 匹配并支持高刷 |
| **网络 / 音频 / APK 安装** | `Active default network: 100`（`Ethernet CONNECTED`, `eth0 10.0.2.15/24`, `DNS 223.5.5.5, 8.8.8.8`），`ping -c 1 223.5.5.5` `0% packet loss`；`media.audio_flinger` 在线；`pm install` / `am start` / `pm uninstall` 全通 |
| **Layer 1 回归 & HAP 打包安装** | `svbox` 全量回归 **26/26 PASS**，Layer 1 零 Android/OHOS 词条纯净度通过；`hvigorw assembleHap` + `pack_hnp.py` + `hda install` 全部成功安装至 `HAD-W32`（`org.svbox.android`）；`rootfs/` 与 `rootfs14/` 均 0 字节改动 |

---

### 15. 双模式（默认自由多窗 + 传统模拟器）与鸿蒙 PC 桌面快捷方式（2026-11-24）

在单一共享 Android 14 容器实例上，零修改 `rootfs14/` 实现 **默认自由多窗模式（单应用独立鸿蒙 PC 窗口）+ 一键无重启切换传统模拟器模式**，以及基于 `@kit.AppGalleryKit` `productViewManager` 的 **「发送到桌面」快捷方式**。

#### A. 严格三层架构实现总览

1. **Layer 1 (`svbox` `feat/vdev-binder`)**：
   * `src/payload/include/config.h`：仅将通用常量 `CONFIG_MAX_VDEVS` 从 `16` 提升至 `32`（容纳 8 路次级虚拟触控设备 `/dev/input/event10..17`），零 Android/OHOS 词条，**26/26 回归全过**。
2. **Layer 2 (`ohos_bridge/` + `gpu/ohos_host/`)**：
   * **多屏零拷贝 Vulkan 呈现**：`VkEmulation::createSecondaryDisplay()` 共享宿主机 `VkDevice`/`VkQueue` 创建多路独立 `DisplayVk` + `DisplaySurface`（`SVBOX_GFX_MAX_DISPLAYS = 9`，slot 0 为主模拟器屏，slot 1..8 为独立应用窗口）；新增 `<gfx.sock>.present` AF_UNIX 零拷贝呈帧通道，直接凭 guest `ColorBuffer` `hostHandle` 调用 `FrameBuffer::borrowColorBufferForDisplay(cbHandle)` 上屏，**全程零 CPU readback**。
   * **多屏独立触控路由**：`InputBridge` 新增 `StartSecondaryTouch(1..8)`（`/dev/input/event10..17`，`EVIOCGPHYS = "svbox-disp-<slot>"`，归一化 `0..32767` ABS_MT 坐标）及 `InjectTouchForDisplay` / `InjectMouseForDisplay` / `InjectScrollForDisplay`。
3. **Layer 3 (`wm_agent/` + `hap/entry/`)**：
   * **Android 14 常驻多屏窗口代理 `wm_agent/`**（不改 `rootfs14/`）：
     * `libsvbox_wm_jni.cpp`（345 KB stripped）：封装 `AImageReader`（`GPU_SAMPLED_IMAGE | GPU_COLOR_OUTPUT`）并直接从 `AHardwareBuffer_getNativeHandle()` 提取 `gralloc.ranchu.so` `cb_handle_t::hostHandle` 投递至 `<gfx.sock>.present`。
     * `SvboxWmAgent.java`（17.3 KB DEX，经 `app_process64 -Xusejit:false` 运行，镜像放 `/data/local/tmp/svbox_wm/` 以满足 Android 14 `clns-1` 命名空间约束）：支持 `LIST_APPS`（提取 `<= 100 KB` PNG 图标 + 自动探测 `resizeable`/`isGame`/锁定方向给出默认窗口策略）、`CREATE_DISPLAY`（原生 `60/90/120Hz` `VirtualDisplay` + `addUniqueIdAssociation("svbox-disp-N", uniqueId)` + 隐藏系统导航栏）、`RESIZE_DISPLAY`（原地 `resizeVirtualDisplay` + `setVirtualDisplaySurface` 不杀 Activity 实时重排）、`LAUNCH_APP`、`MOVE_TASK`、`POP_OUT_EMULATOR`、`PULL_BACK_TO_EMULATOR`、`SEND_KEY`、`CLOSE_APP`、`DESTROY_DISPLAY`，并通过 `ITaskStackListener` 推送异步 `CROSS_APP_LAUNCH`（自动豁免 `PermissionController`/`IntentResolver`/`SystemUI` 系统弹窗）与 `DISPLAY_EMPTY` 事件。
   * **HarmonyOS PC 双模式 UI & 桌面快捷方式（`hap/entry/`）**：
     * `MyAbilityStage.ets` + `AppWindowAbility.ets`（`launchType: "specified"`）+ `AppWindowPage.ets`：每个 Android 应用映射到独立鸿蒙窗口 `svbox_app_<pkg>`；支持拖拽期 GPU 平滑缩放 + 250ms 防抖实排 `RESIZE_DISPLAY`、手动切换 `自适应 / 固定竖 / 固定横`、5 档预设、独立 `60/90/120Hz`、「加桌」与「收回模拟器」，并支持从桌面快捷方式冷启动直接拉起 Android 14 并进入目标应用。
     * `EntryAbility.ets` + `Index.ets`：默认进入 **自由多窗 App Launcher**（搜索、应用过滤、`桌面图标 N/10`、真实图标网格、模式切换与加桌/删桌、跨应用自动弹窗），一键无重启切换至 **传统模拟器模式**（`Display 0` + 5 档预设 + 60/90/120Hz + **「拆出应用」`POP_OUT_EMULATOR`**）。
     * `AppWindowPrefs.ets` + `DesktopShortcut.ets`：按包名持久化窗口偏好（`app_window_prefs.json`），封装 `@kit.AppGalleryKit` `productViewManager`（`checkPinShortcutPermitted` / `requestNewPinShortcut` / `getPinShortcutInfos` / `removePinShortcut`，`shortcutId <= 63B`、全 string `want.parameters`、`<= 100 KB` PNG 图标，PC 上限 10 个）。

#### B. 关键排障根因速记

| 现象 | 根因 | 修复 |
| :--- | :--- | :--- |
| `app_process64` 启动 `SvboxWmAgent` `SIGSEGV (0x278)` 或拒启 | 混入 `SF_LD_PATH` 破坏 Android 14 `linkerconfig` 命名空间；`-Xint` 与 `dalvik.vm.usejit=true` 冲突 | 去掉 `SF_LD_PATH` 并使用 `-Xusejit:false` |
| `System.load("/ipc/libsvbox_wm_jni.so")` 报 `clns-1` `UnsatisfiedLinkError` | Android 14 `libnativeloader` `clns-1` 仅允许 `/data:/mnt/expand:/system/lib64` | 宿主机将 `svbox-wm-agent.jar` 与 `libsvbox_wm_jni.so` 释放到 `/data/local/tmp/svbox_wm/` |
| `IVirtualDisplayCallback` 回调 `AbstractMethodError` | Android 14 `VirtualDisplayAdapter` 新增 `onPaused()`/`onResumed()`/`onStopped()` | 在 `SvboxWmAgent` 的 `IVirtualDisplayCallback.Stub` 子类补全全部回调 |
| 次级虚拟触控设备落回 Display 0 | 缺少对应 `svbox-touchscreen-<slot>.idc`（`device.internal = 0`） | 在 `inject_layer/system/usr/idc/` 生成 `svbox-touchscreen-1..8.idc` |

#### C. `HAD-W32` 真机端到端验收结果（`M1_M3_DUALMODE_E2E_ALL_PASSED`）

| 验收项 | 真机实测结果 |
| :--- | :--- |
| **Android 14 冷启动 + `SvboxWmAgent`** | Android 14 **17.4s** 启动完成，`SvboxWmAgent` 就绪，`ITaskStackListener` 注册成功 |
| **应用列表与图标提取 (`LIST_APPS`)** | 扫描 10 个应用，全部 PNG 图标 **14.8 KB – 44.9 KB**（均 `<= 100 KB`）；自动识别 `Camera -> fixed_portrait (resize=False)`、`Settings/Calendar/Contacts/Clock/WebView -> adaptive (resize=True)` |
| **三屏并行零拷贝渲染 + 实时重排** | 同时运行 `Display 0` (`720×1280 60/90/120Hz`) + `Display 2` (`Settings 600×900@280dpi 120Hz`, 39 帧) + `Display 3` (`Calendar 960×600@240dpi 90Hz`, 17 帧)；执行 `RESIZE_DISPLAY 1 900 600 280` 零杀进程实时横屏重排（57 帧），抓图目视清晰无花屏 |
| **多屏独立触控路由 (`dumpsys input`)** | `svbox-touchscreen-1` (`svbox-disp-1`, `svbox-touchscreen-1.idc`) 绑定 `virtual:android:svbox-vdisp-1` (`displayId=2`, `logicalFrame=[0, 0, 900, 600]`, `isActive=[1]`)；`svbox-touchscreen-2` (`svbox-disp-2`) 绑定 `virtual:android:svbox-vdisp-2` (`displayId=3`, `logicalFrame=[0, 0, 960, 600]`) |
| **跨应用弹窗 & 双模式实时互转** | `CROSS_APP_LAUNCH` 精准捕获 Settings 内拉起 Contacts (`taskId=10`)，且豁免 `PermissionController` 权限弹窗；`MOVE_TASK`、`SEND_KEY`、`PULL_BACK_TO_EMULATOR`（`moved=2`）、`POP_OUT_EMULATOR`（`Calendar -> displayId=3`）、`CLOSE_APP`、`DESTROY_DISPLAY` 全通；`system_server Fatal signals = 0` |
| **构建打包与 Layer 1 回归** | `svbox` 回归 **26/26 PASS**、Layer 1 纯净度通过；`hvigorw assembleHap` 0 错误 + `pack_hnp.py` 注入 `svbox.hnp`（5.96 MB，含 `svbox`、`libsvbox_wm_jni.so`、`svbox-wm-agent.jar`），`module.json` 含 `AppWindowAbility (specified)` 与 `MyAbilityStage`；`rootfs/` 与 `rootfs14/` 0 字节改动 |

---

### 16. 无 FUSE `IVold` 存储挂载、Binder 引用竞态与 `mediaswcodec` 自启修复（2026-11-24）

彻底根治 Android 14 在无 `/dev/fuse` 的鸿蒙 PC 内核上 `VolumeInfo{emulated;0}` 无法挂载、`MediaProvider` 卡死/报 `Volume external_primary not found` 以及 `Files`（`com.android.documentsui`）/ `Gallery`（`com.android.gallery3d`）白屏问题，全程保持 `rootfs14/` 与 Layer 1 `svbox` 0 字节改动。

#### A. 核心改动（严格限定在 Layer 3 `svbox-Android`）

1. **`netd/svbox_netd.c`（新增无 FUSE `android.os.IVold` 服务 `NODE_VOLD`）**：
   * 注册 `"vold"`（`android.os.IVold`），按 `rootfs14/system/framework/framework.jar`（`classes3.dex`）校验的精确事务码实现：`setListener (1)`、`reset (4)`、`onUserAdded (6)`、`onUserRemoved (7)`、`onUserStarted (8)`、`mount (15)`、`unmount (16)`、`setupAppDir (23)`、`fixupAppDir (24)`、`ensureAppDirsCreated (25)`、`getStorageLifeTime (31)`、`getWriteAmount (34)`、`getUnlockedUsers (45)`、`unlockUserKey (46)`、`lockUserKey (47)`、`prepareUserStorage (48)`、checkpoint/incfs 查询（`53/54/55/62/63/64/70`）及 `getStorageSize (76)`。
   * 在 `mount("emulated;0", ...)` 中跳过 `IVoldMountCallback.onVolumeChecking(fuseFd, ...)`（避免 `StorageSessionController` 拉起 FUSE 会话），直接通过 `IVoldListener` 单向回调依次上报 `STATE_CHECKING (1)` → `path="/storage/emulated"` → `internalPath="/data/media"` → `STATE_MOUNTED (2)`，无缝对接 `svbox` 已 bind-mount 的 `/storage/emulated/0 <-> /data/media/0`。
   * 新增 `--data-dir <userdata14>` 参数，为 `setupAppDir` / `ensureAppDirsCreated` / `prepareUserStorage` 实时在宿主机创建对应应用外部存储目录。
2. **`binderd/svbox_binderd.c`（修复 Binder 节点引用计数竞态）**：
   * 根因：`node_inc_refs()` 向发送线程 `direct` 队列投递 `BR_INCREFS`/`BR_ACQUIRE` 后，若快速原生服务端在发送线程消费前就 `BC_FREE_BUFFER` 释放句柄，`node_check_release()` 会立刻向 `owner->inbox` 投递 `BR_RELEASE`/`BR_DECREFS` 并唤醒其他线程抢先析构对象，导致 `system_server` 在 `IVold.mount()` 处 `SIGSEGV (fault addr 0x4)`。
   * 修复：按 Linux 内核 `binder.c` 语义在 `struct bnode` 增加 `pending_weak_ref` / `pending_strong_ref`，实现 `SV_BC_INCREFS_DONE` 与 `SV_BC_ACQUIRE_DONE` 清标并触发 `node_check_release()`，彻底消除竞态。
3. **`hap/entry/src/main/cpp/android_runtime.cpp`**：
   * 移除原版 `system/bin/vold` 启动，向 `svbox-netd` 传入 `--data-dir <dataBase_>`，并配置 `persist.sys.fuse.transcode_enabled=false` / `sys.fuse.transcode_enabled=false`。
   * 开机自启 `apex/com.android.media.swcodec/bin/mediaswcodec`（不带 `sfLdPath` 以保留 APEX linker namespace 对 `gralloc-mapper` 的解析），彻底解除 `StorageManagerService` Handler 线程在 `MediaCodecList` → `android.hardware.media.c2@1.0::IComponentStore/software` 上的永久阻塞，确保开机自动跑完 `H_BOOT_COMPLETED` / `H_COMPLETE_UNLOCK_USER` / `emulated;0` 自动挂载与 `ACTION_MEDIA_MOUNTED` 广播。

#### B. `HAD-W32` 真机冷启动验收结果

| 验收项 | 真机实测结果 |
| :--- | :--- |
| **冷启动自动挂载** | ~9s 完成启动，`svbox-netd` 自动完成 `reset -> onUserAdded(0) -> onUserStarted(0) -> mount(emulated;0) -> STATE_MOUNTED` |
| **`dumpsys mount`** | `VolumeInfo{emulated;0}: state=MOUNTED`, `path=/storage/emulated`, `internalPath=/data/media`；`Local unlocked users: [0]`、`System unlocked users: [0]` |
| **`ExternalStorageProvider`** | `content://com.android.externalstorage.documents/root` 返回 `root_id=primary` (`document_id=primary:`) |
| **`MediaProvider`** | `content://media/external/images/media` 毫秒级返回零报错；`content://media/external/file` 索引 `volume_name=external_primary` 下 14 个标准目录（`Music/Movies/Download/DCIM/Documents/...`） |
| **`Files` & `Gallery` 独立窗口** | `Files` (`com.android.documentsui`) 加载全部 7 个根目录（最近/primary/下载/图片/视频/音频/文档），`immersive topBar=#FFFFFF`；`Gallery` (`com.android.gallery3d`) `+358ms` 完成首帧显示，`immersive topBar=#080808 dark=1`；双窗口零白屏、零崩溃、零 ANR |

---

### 17. 全栈启动与运行期深度性能优化（2026-11-24）

围绕 Layer 1 `svbox` 的 `svc #0` 扫描/加载路径、跨进程共享缓存、ART 单映射 JIT 支持，以及 Layer 3 `svbox-binderd` / `svbox-netd` / `android_runtime.cpp` 编排层完成全套优化，并在 `HAD-W32` 上保留同口径 **BEFORE vs AFTER** 量化对比（原始 JSON 留档于 scratch 目录 `before_opt_v2_*.json` / `after_opt_v2_*.json`）。

#### A. 分层改动清单

1. **Layer 1 `svbox`（`feat/vdev-binder`，通用 Linux 能力，27/27 回归全过，零 Android 专用代码）**：
   * **ARM64 NEON 4 路单趟扫描 + 批量 ELF 节表读取 + 非代码段预检（`src/payload/include/stub_island.h`）**：
     * `island_code_ranges()` 改为每批 `64` 项（4 KiB）一次 `pread64`（将每库 ~35 次系统调用压缩为 1 次），并识别有效 ELF 中零 `SHF_EXECINSTR` 重叠段返回 `-1` 直接跳过扫描。
     * `island_scan_word_range()` 使用 4×128-bit 向量（每次 64B = 16 条指令）单趟直扫 `SHF_EXECINSTR` 区间，彻底取消第二趟扫描与逐字区间判定；对调用方已持纯 `RW` 映射的场景（`map_exec_copy`）省略两次冗余 `mprotect`。
   * **跨进程无锁共享 `SVC-Site Cache`（新增 `src/payload/include/svc_cache.h`，接入 `loader/hook.c` 与 `payload/logic/hook_runtime.c`）**：
     * 位于 `<mount_scratch>/.svbox_svc_cache`（1 MiB `MAP_SHARED`，2048 槽 4 路探测 + 229K 位点池，per-slot seqlock 无锁并发），以 `(dev, ino, file_size, mtime_sec, mtime_nsec, file_off, map_len)` 精确校验；文件变更自动失效。零 `svc` 库 O(1) 跳过扫描，含 `svc` 库直接按缓存偏移数组打补丁。
   * **大块加载与匿名可执行页优化（`src/payload/logic/dispatch.c`）**：
     * `read_region()` 由 `4 KiB` 提升至 `1 MiB` 分块，并在 EOF 立即退出（利用新匿名页零页特性）。
     * `map_exec()` 对匿名 `MAP_FIXED|PROT_EXEC`（如 ART 32 MiB JIT 代码缓存）先 `RW` 映射再 `platform_mprotect_exec()`，且跳过全零匿名页扫描避免提前缺页占用物理内存。
   * **通用容器策略补齐（`config.h` / `options.c` / `slow_syscalls.inc` / `dispatch.c`）**：
     * 虚拟 `CAP_SYS_NICE` 兼容：`sched_setscheduler` / `sched_setparam` / `sched_setattr` 在宿主机返回 `-EPERM` 且容器持有虚拟 `CAP_SYS_NICE` 时返回 `0`。
     * 新增通用 `--deny-memfd-prefix PREFIX`，按前缀对 `memfd_create` 返回 `-ENOSYS`（供宿主不支持 `MAP_SHARED|PROT_EXEC` 的运行时优雅回退单映射匿名 JIT）。
     * 新增回归测试 `test/vfs/test_svc_cache_and_sched.py`（冷/热缓存命中、文件覆盖自动失效、`CAP_SYS_NICE`、`--deny-memfd-prefix` 全覆盖）。

2. **Layer 3 `svbox-Android`（`main`）**：
   * **`binderd/svbox_binderd.c`**：在 `struct bnode` 上维护 `handle_count` 与 `strong_ref_total`，将 `node_check_release()` 由 `O(MAX_PROCS × handles_cap)` 全表双层扫描降为 **O(1)**；在 `struct bproc` 上维护 `async_node_count`，同步 `BC_FREE_BUFFER` O(1) 跳过异步节点遍历。
   * **`netd/svbox_netd.c`**：新增轻量 `"stats"`（`NODE_STATSD`）桩消除 `Service stats didn't start` 5 秒超时；将 `IVold` 的 `onVolumeCreated` 严格对齐至 `onUserStarted`（消除超快冷启动下 `onUserAdded` 抢跑竞态）。
   * **`hap/entry/src/main/cpp/android_runtime.cpp` + `Index.ets`**：
     * 传入 `--deny-memfd-prefix jit-`，恢复 Android 14 `system_server` 及全部子应用真 AArch64 ART JIT。
     * 持久化跨重启 `svbox_svc_cache`（软链至 `userdata14/cache/svbox_svc_cache`）与整目录 `/linkerconfig` 缓存（`userdata14/cache/linkerconfig_a14/`，含 `ld.config.txt` 与 `apex.libraries.config.txt`）。
     * 拉起原版 `mediaextractor` 与 `mediaserver`（消除 7×5s `waitForService` 超时）；预建 `/data/misc/bluetooth/logs` 与 `/data/misc/bluedroid`；精简串行 `SleepCancelableMs` 空等；新增 `--ps autoStart restart` 便于自动化基准复测。

#### B. `HAD-W32` 真机 BEFORE vs AFTER 量化性能对比

##### 1. Layer 1 `svbox` 实测对比（5 轮中位数，大核 `12-19`）

| 指标 | 优化前 (BEFORE) | 优化后 (AFTER) | 变化 / 提速 |
| :--- | :---: | :---: | :---: |
| **8 MiB `.so` `dlopen`（冷扫 → 跨进程缓存命中）** | `47.27 ms`（冷） | **`8.08 ms`**（热命中） | **5.85× 提速** |
| **Alpine Overlay `fork+exec /bin/true`** | `8.64 ms` | **`7.97 ms`** | **-7.8%** |
| **Alpine Overlay `jit`（RW→RX 桩岛周期）** | `33.80 µs` | **`29.40 µs`** | **-13.0%** |
| **Alpine Overlay `small300`** | `56.05 ms` | **`54.79 ms`** | **-2.2%** |
| **Bare `stat_rw` / `stat_ro`** | `3.713 / 3.653 µs` | `3.737 / 3.694 µs` | 持平（零回退） |
| **Alpine Overlay `stat_rw` / `stat_ro`** | `11.605 / 21.852 µs` | `12.026 / 22.015 µs` | 持平（零回退） |

##### 2. Layer 3 Android 14 真机实测对比（`HAD-W32`）

| 指标 | 优化前 (BEFORE) | 优化后 (AFTER) | 变化 / 提速 |
| :--- | :---: | :---: | :---: |
| **冷启动至 `sys.boot_completed=1`** | `10,260 ms` | **`7,064 ms`**（3 轮：`7270 / 7004 / 7064 ms`，首启 `adbd 1256 ms`） | **-3.20 s（-31.2% / 1.45×）** |
| **`Settings` 冷启动 (`am start -W TotalTime`)** | `779 ms` | **`451 ms`** | **-42.1%（1.73×）** |
| **`Files` 冷启动 (`TotalTime`)** | `523 ms` | **`437 ms`** | **-16.4%（1.20×）** |
| **`Clock` 冷启动 (`TotalTime`)** | `483 ms` | **`445 ms`** | **-7.9%** |
| **`Gallery` 冷启动 (`TotalTime`)** | `435 ms` | **`421 ms`** | **-3.2%** |
| **`service check activity` × 50** | `1,808 ms` | **`1,737 ms`** | **-3.9%** |
| **5 秒 `Service .* didn't start` 超时数** | `11` 次 | **`0` 次** | **-100% 清零** |
| **子进程 ART JIT 创建失败（`system_server` + Apps）** | `3` 处（全量回退解释器） | **`0` 处（仅 `zygote64` 1 条无害告警，全部 App 启用真 JIT）** | **真 JIT 恢复生效** |
| **`bt_stack_manage` `SIGABRT` 次数** | `5` 次（含 `real time scheduling` 崩溃循环） | `2` 次（`real time scheduling` 与 `snoop_logger` 目录报错均清零） | **-60%** |
| **存储与多窗验收** | `emulated;0` `MOUNTED` | `emulated;0` 3 轮快速冷启均 `MOUNTED`、`System unlocked [0]`、`MediaProvider` 14 目录、`Files` + `Gallery` + `Clock` 三屏并行零崩溃 | **全部 PASS** |

---

### 18. `owner.db` 宿主临时文件 Inode 复用碰撞导致 `/dev/__properties__` 全员拒载 & CPU 93% 死循环（已根治）

* **现象**：应用在多轮启停或关闭重开后，启动卡死在 `Booting Full Android 14 ... Zygote...`，传统模拟器黑屏 `0/120Hz`，整机 CPU 飙至 `93%`（仅占 `416MB` 内存）。
* **根因链条**：
  1. `userdata14/owner.db` 持久化按 `(st_dev, st_ino)` 存储虚拟 `uid/gid/mode`，而宿主 `temp/svbox-android14-hap/` 与 `files/userdata14/` 位于**同一 `el2` 文件系统（同一 `st_dev`）**。
  2. 此前容器运行中 `system_server` 等非 root 进程创建并删除过 `/data` 文件，或宿主 `RemoveDirRecursive` 在容器外直接删除临时文件时未触发 `owners_forget()`，导致 `owner.db` 中残留 `uid=1000 (system:system)` 等旧 `(st_dev, st_ino)` 记录。
  3. 后续启动时，`svbox-propd` 在宿主创建 `property_info` 和 `u:object_r:default_prop:s0` 恰好复用了被内核回收的旧 inode，导致容器内 `ls -la /dev/__properties__` 显示两者归属 `system:system` 而非 `root:root`。
  4. Bionic `libc` 严格校验 `st.st_uid == 0 && st.st_gid == 0`，直接拒载整个属性区 → `ro.property_service.version` / `ro.product.cpu.abilist64` / `ro.hardware.hwcomposer` / `ro.debuggable` 全空 → `zygote64`、`adbd`、`hwcomposer` 崩溃退出，`surfaceflinger` / `audioserver` / `artd` 等 12+ Binder 客户端在 `WaitForProperty("servicemanager.ready")` 中因 `__system_property_area__ == nullptr` 立即返回 `false` 形成 100% CPU 紧循环。
* **根治修复**：
  1. **Layer 3 (`android_runtime.cpp`)**：新增 `ForgetOwnerDbTree()`（按 `ELFOWN1` 协议加 seqlock 原子标记过期 `(st_dev, st_ino)` 为 `OWNER_DEAD`）并在 `EnsureOwnerDb()` 中加入 >50% 负载自动 tombstone 压缩；在 `RemoveDirRecursive` 前、启动 `linkerconfig`/`servicemanager` 前（全量清理 `runBase_` 与 `linkerconfig_a14`）、以及 `adbd`/`zygote64`/`wm_agent` 启动前清理宿主新建文件/Socket 的碰撞记录。
  2. **Layer 3 (`EntryAbility.ets`)**：补充 `onDestroy()` 主动调用 `native.stopContainer()`，避免关闭主窗时子进程被 `ClearSession` 强杀。
  3. **Layer 1 (`svbox/src/payload/logic/container_state.c`)**：为 `read_begin()` 补齐与 `spin_lock()` 对称的死写者 seqlock 恢复（>200,000 次 yield 后推进奇数 `version` 并释放 `lock`），杜绝运行期进程被 `SIGKILL` 留下奇数 `version` 导致其他读者永久死循环。
* **真机实测验证**：不清空已有被污染的 `owner.db`，直接启动 `Vulkan · 120Hz · 720×1280 · 传统模拟器`，首启即自动清理 **44 条**残留 inode 记录，`/dev/__properties__/*` 全部恢复 `root:root`，`sys.boot_completed=1`、`modeId 3 @ 120Hz`、`emulated;0 mounted`、`Launcher3` 正常出画；连续 Stop → Start 重启（自动清理 **41 条**）同样 2/2 稳定通过。

---

### 19. Layer 1 `/proc/{maps,smaps,status,cmdline}` 虚拟化（防劫持暴露）与酷安 16.7.3 真机跑通

* **背景**：此前安装酷安 `CoolApk-16.7.3`（109 MB，内置 `libNetHTProtect.so` / `libverbanr.so` / `libsecsdk.so` / `libpanglearmor.so` 等壳与反篡改 SDK）冷启触发 `SIGABRT`（`SI_QUEUE`），且 `F/DEBUG` 回溯落入 `<anonymous:3f80015000>`。根因在于容器内进程可直接通过 `/proc/self/maps`（匿名可执行拷贝段、payload `0x3f80000000`、stub island、`owner.db`、`.svbox_svc_cache`）、`/proc/self/status`（`Seccomp: 2`）以及 `/proc/<pid>/cmdline`（`svbox --root ...` 参数）识别出用户态 syscall 劫持实现。
* **Layer 1 通用修复（`svbox`，`feat/vdev-binder`，零 Android/OHOS 耦合）**：
  1. **初始与运行期可执行 VMA 追踪**（[`config.h`](file:///storage/Users/currentUser/Workspace/svbox/src/payload/include/config.h)、[`loader/elf_load.c`](file:///storage/Users/currentUser/Workspace/svbox/src/loader/elf_load.c)、[`loader/main.c`](file:///storage/Users/currentUser/Workspace/svbox/src/loader/main.c)、[`hook_runtime.c`](file:///storage/Users/currentUser/Workspace/svbox/src/payload/logic/hook_runtime.c)、[`dispatch.c`](file:///storage/Users/currentUser/Workspace/svbox/src/payload/logic/dispatch.c)）：记录 loader 阶段拷贝的 `prog`/`interp` `PT_LOAD` 与运行期 `map_exec_copy()` 映射的原文件 `(dev, ino, file_off, host_path)`，并在 `munmap` / 非执行 `MAP_FIXED` `mmap` 时同步剔除或拆分。
  2. **流式 `/proc/<pid>/{maps,smaps}` 虚拟化**（[`vfs_procfs.c`](file:///storage/Users/currentUser/Workspace/svbox/src/payload/logic/vfs_procfs.c)、[`vfs_ops.c`](file:///storage/Users/currentUser/Workspace/svbox/src/payload/logic/vfs_ops.c)）：隐藏全部 `hook_protected_overlap`（payload、stub islands、`owner.db`、`.svbox_svc_cache`、`mount_table`）与内部路径，恢复匿名拷贝执行段的真实容器内路径/偏移/`dev:ino`，并将剩余宿主路径翻译为容器路径。
  3. **`/proc/<pid>/status` & `/proc/<pid>/cmdline` 净化**：当启用 `seccomp_backstop || trap_sigsys` 时将 `Seccomp:` 与 `Seccomp_filters:` 改写为 `0`；将 `Bridge.proc_data` 从 `8192` 扩至 `65536` 以完整解析超过 20 KB 的容器命令行，同时在 Layer 3 [`android_runtime.cpp`](file:///storage/Users/currentUser/Workspace/svbox-Android/hap/entry/src/main/cpp/android_runtime.cpp) 的 `runSvbox()` 与 `wm_agent` 启动参数中统一插入 `"--"` 分隔符。
  4. **伴生坑修复（`mounts_unshare()` 过期保护区导致 `Zygote` `munmap` `-EPERM`）**：首次真机验证发现 `Zygote` 调用 `unshare(CLONE_NEWNS)` 触发 `mounts_unshare()` 释放旧 `mount_table` 后，其 VA 仍残留在 `protected_lo/hi` 中，随后内核将该地址复用于 `ZygoteCommandBuffer` 导致 `freeNativeBuffer` 报 `JNI FatalError: Failed to unmap argument buffer`。新增 `hook_unnote_internal()` 在 `mounts_unshare()` `munmap(prev)` 前原子注销旧映射并注册 `new_t`，彻底根治。
* **真机实测结果**：
  * `/proc/self/status` 输出 `Seccomp: 0`；`/proc/self/maps` 零泄露且全部 `r-xp` 恢复真实 `/system/...` 与 `.so` 路径；`ps -A` 全系统进程恢复原生名称（`surfaceflinger` / `zygote64` / `system_server` / `com.android.launcher3`）。
  * **酷安 16.7.3（`com.coolapk.market`）无需 Step 2 即直接跑通**：冷启 `2,631 ms` 正常进入隐私条款页，点击“同意协议并继续”后完整加载首页头条、图片与底部导航栏，常驻 `660 MB RSS` 零崩溃。证明酷安此前 `SIGABRT` 的直接根因就是 `/proc/self/maps`（匿名 `r-xp` 与 `svbox` 痕迹）/ `status` (`Seccomp`) / `cmdline` 检测。

---

### 20. 停止模拟器主线程卡死（`THREAD_BLOCK_6S` AppFreeze）根因定位与修复

* **现象**：在传统模拟器或关闭模拟器窗口后点击停止实例，应用卡死触发鸿蒙 `AppFreeze`（`THREAD_BLOCK_6S`）。
* **FaultLog 根因铁证**（`/data/log/faultlog/faultlogger/appfreeze-org.svbox.android-20020420-20261125011443053.log`）：
  * 主线程（ArkUI）：`StopContainerNapi -> AndroidRuntime::Stop -> GfxstreamBridge::Stop -> svbox_gfx_stop -> RendererImpl::stop(true) -> RenderThread::waitForFinished()`。
  * 子线程（`RenderThread`）：永久阻塞在 `FrameBuffer::waitUntilInitialized()`。
  * **触发链**：关闭 `EmulatorWindowAbility` 或 `GfxstreamBridge::Stop()` 提前调 `fnClearWindow_()` 会触发 `FrameBuffer::removeSubWindow()`，而原版 `gfxstream` 在 `removeSubWindow()` 中执行了 `sInitialized.store(false)`；随后任何新进或竞态的 `RenderThread` 均在 `waitUntilInitialized()` 上永久等待，且 `FrameBuffer::setShuttingDown()` 未将 `sInitialized` 置 `true` 也未广播 `sGlobals()->condVar`，形成永久死锁。此外 `AndroidRuntime::Stop()` 此前先 join 各桥接线程后才杀容器子进程，导致桥接线程在等待活着的 Guest Socket 时产生数秒额外卡顿。
* **根治修复**：
  1. **Layer 2 (`apply_host_patches.py`)**：`FrameBuffer::removeSubWindow()` 不再清除 `sInitialized`；将 `FrameBuffer::setShuttingDown()` 移至 `FrameBuffer.cpp` 实现，关停时置 `m_shuttingDown = true`、`sInitialized.store(true, release)` 并调用 `sGlobals()->condVar.broadcast()` 唤醒所有等待者。
  2. **Layer 2 (`ohos_bridge/src/gpu_bridge.cpp`)**：`GfxstreamBridge::Stop()` 移除 `fnStop_()` 之前冗余的 `fnClearWindow_()`。
  3. **Layer 3 (`android_runtime.cpp`)**：`AndroidRuntime::Stop()` 改为**优先向全部容器进程组发送 `SIGCONT -> SIGTERM -> 60ms -> SIGKILL`** 让全部宿主机桥接 Socket 瞬间收到 EOF 退出，随后再 join 线程并关闭 `GfxstreamBridge`（并在 `bootThread_.join()` 后再补收一次竞态残留 PID）。
* **真机实测验证**：执行 `autoStart restart`（完整 `Stop -> Start` 闭环），`hilog` 在 `< 1s` 内干净输出 `SvboxGfxBridge: gfxstream stopped`，全流程 **0 次 `THREAD_BLOCK` / `AppFreeze`**，且 `8s` 即完成二次重启进入 `sys.boot_completed=1`。

---

### 21. 酷安延迟 9–12s 闪退根因（字节 Metasec `.text` 哈希自检）→ Step 2 零字节改写 + Vulkan 流畅度深度优化

#### A. 延迟闪退根因定位（`libPglbizssdk_ml.so` 精确到单条指令）
* **现象**：酷安通过 Step 1 正常进入首页后，约 `9–12s` 在穿山甲线程 `csj_io` / `csj_b_6` 触发 `SIGSEGV (fault addr 0x4000)`，寄存器呈现典型反篡改自毁特征：`sp = x29 = 0x7b`、`lr = 0`、`pc = 0x4000`、`x8 = 0x77df8b85`（期望哈希）、`x9` 每次启动随 ASLR 变化。
* **反汇编铁证**：`ucrash` 日志与 `linker64` 寄存器定位到 `com.volcengine.mobsecBiz.metasec.ml.PglMSManagerUtils.init` 加载的 **`libPglbizssdk_ml.so`**（含 **197 处内联 `svc #0`**）。其 `DT_INIT_ARRAY` 构造函数 `0xa7a70` 在魔数标记 `0xff77f6f1` (`0xa7b90`) 与 `0xf077f6f1` (`0xa7c9c`) 之间放置了不可达的诱饵 `svc #0`（`0xa7bec`、`0xa7c50`），随后对区间内所有 32 位指令做滚动异或/移位哈希并与 `0x77df8b85` 比对；`svbox` 原先将其内联改写为 `b <stub_island>` 导致哈希失配，直接调用 `0xa47f0` 自毁。
* **Step 2 通用解法（Layer 1 `svbox` `feat/vdev-binder`）**：
  * 实测验证鸿蒙 HAP 沙箱内允许 `PR_SET_NO_NEW_PRIVS` + `seccomp(SECCOMP_SET_MODE_FILTER)`（`rc=0`）。
  * 打通 `--seccomp-backstop` 与 `--trap-sigsys` 组合（[backstop.c](file:///storage/Users/currentUser/Workspace/svbox/src/loader/backstop.c)、[signals.c](file:///storage/Users/currentUser/Workspace/svbox/src/loader/signals.c)、[main.c](file:///storage/Users/currentUser/Workspace/svbox/src/loader/main.c)）：`SIGSYS` 处理器携带 `SA_RESTORER` (`svbox_sigsys_restorer`)，捕获 `si_errno == BACKSTOP_TRAP_DATA` 的未改写 `svc #0` 转发 `dispatch_syscall()`，宿主机沙箱触发的其他 `SYS_SECCOMP` 则按 `--trap-sigsys` 转 `-ENOSYS`。
  * 新增通用选项 `--no-inline-patch` 与 `--no-inline-patch-prefix DIR`（[options.c](file:///storage/Users/currentUser/Workspace/svbox/src/loader/options.c)、[dispatch.c](file:///storage/Users/currentUser/Workspace/svbox/src/payload/logic/dispatch.c)）：Layer 3（[android_runtime.cpp](file:///storage/Users/currentUser/Workspace/svbox-Android/hap/entry/src/main/cpp/android_runtime.cpp)）传入 `--seccomp-backstop --trap-sigsys --no-inline-patch-prefix /data --no-inline-patch-prefix /data_mirror`。系统库（`/system`、`/apex`、`/vendor`）继续走全速内联补丁（9s 进桌面），而所有用户应用 `.so` 及运行期 `mprotect(PROT_EXEC)` 解壳代码 **0 字节改写 `.text`**，同时全部直接 `svc #0` 经 `seccomp-backstop` 透明拦截。
  * **真机验证**：酷安连续滑动 25s+（含打开 `WebViewActivity`）**同 PID 零崩溃**，彻底通过 Metasec 完整性校验。

#### B. 酷安流畅度瓶颈定位与 Vulkan 4 KiB 脏页 Diff 优化
* **`dumpsys gfxinfo framestats` 根因**：UI 线程仅 `2–5 ms`、Maleoon 916 GPU 仅 `1 ms`，但 Vulkan 模式 `RenderThread` (`IssueDrawCommandsStart -> SwapBuffers`) 高达 **`35–95 ms`**（P50 `97 ms` ≈ 10 fps）。根因在于此前 Guest 侧 `HOST_COHERENT` 模拟（[apply_guest_patches.py](file:///storage/Users/currentUser/Workspace/svbox-Android/gpu/android_guest/apply_guest_patches.py)）在每次 `on_vkQueueSubmit()` 中对所有常驻映射的 `VkDeviceMemory` 全量刷写 `VK_WHOLE_SIZE`（每帧经 `/ipc/gfxstream.sock` 拷贝数十 MB）。
* **优化落地**：在 `ResourceTracker.cpp` 引入 4 KiB 粒度脏页对比与连续合并（`flushShadowPtr` + `flushPageSynced` 位图），仅通过 socket 刷写实际改动的脏页。
* **真机同口径滑动实测对比（`720×1280 @ 120Hz`）**：
  * **Vulkan 优化前**：`232 帧` · Janky `93.97%` · P50 **`97 ms`** · P90 `250 ms` · P99 `700 ms` · `IssueDraw->Swap` `35–95 ms`
  * **Vulkan 4 KiB 脏页优化后**：`248 帧` · Janky `66.53%` · P50 **`36 ms`（快 2.7×）** · P90 `69 ms`（快 3.6×） · P99 `150 ms`（快 4.7×） · `IssueDraw->Swap` **`13–19 ms`**
  * **GLES（`skiagl`）同场景对比**：**`704 帧`**（~70–100+ fps）· Janky **`13.78%`** · P50 **`14 ms`**（大量帧 `6–11 ms`）· P90 `27 ms` · `IssueDraw->Swap` **`5.4–7.5 ms`**
  * 剩余差距原因：Vulkan 模式当前无 `GlDirectMem`（跨进程 `memfd`/`ashmem` 零拷贝）且 `vkFlushMappedMemoryRanges` 每次需同步等回复，外加无 `/dev/goldfish_sync` 时 `qsriFence`/`vkDeviceWaitIdle` 串行等待；后续通过 `SCM_RIGHTS` 共享 `memfd` 开启 `GlDirectMem` + 异步虚拟 sync FD 可将 Vulkan 也推至 90–120 fps。

---

### 22. GLES + Vulkan 双模式 120 FPS 满帧冲刺（酷安实测 P50 9–11 ms，100–120 FPS）

在 4 KiB 脏页 Diff 将 Vulkan P50 压到 `36 ms` 后，通过全链路 Host/Guest IPC 与 Vulkan 协议级热点剖析（`VK_PROF` + `VK_READ_PROF`），逐一消灭了剩余所有阻塞，最终在 `HAD-W32`（Maleoon 916 · `720×1280 @ 120Hz`）上把 **GLES 与 Vulkan 双模式均推至 100–120 FPS 满帧区间**：

#### A. Host 端优化（[svbox_gfxstream_capi.cpp](file:///storage/Users/currentUser/Workspace/svbox-Android/gpu/ohos_host/svbox_gfxstream_capi.cpp)、[apply_host_patches.py](file:///storage/Users/currentUser/Workspace/svbox-Android/gpu/ohos_host/apply_host_patches.py)）
1. **Direct `send()` 回包直发快速路径（`RenderChannel::setDirectWriteCallback`）**：Host `RenderThread` 产生回复时直接在 `writeToGuest()` 内经 `MSG_DONTWAIT` 写入 Guest `AF_UNIX` socket（仅在 `EAGAIN` 时回落 `gfx-epoll`），彻底消除了每笔同步 RPC 的 `eventfd -> epoll_wait -> tryRead -> send` 两次线程跳转。
2. **`O(1)` ready-FD 批处理 + 大缓冲区 + `QOS_USER_INTERACTIVE (5)`**：`ServerLoop` 消除 300 连接 `O(N)` 扫描、`rxBuf` 扩至 `512 KiB`、`SO_SNDBUF/SO_RCVBUF` 扩至 `2 MiB`；`gfx-epoll` / `gfx-present` / `RenderThread` / `RenderWindowThread` 全部调用 `OH_QoS_SetThreadQoS(5)` 调度至大核满频；`SwapChainStateVk` 优先选用 `VK_PRESENT_MODE_MAILBOX_KHR`。
3. **异步 Fire-and-Forget 协议扩展（`VkDecoder.cpp`）**：`OP_vkFlushMappedMemoryRanges`、`OP_vkBindBufferMemory`、`OP_vkBindImageMemory`、`OP_vkResetFences`、`OP_vkResetCommandPool` 改为异步单向执行（省去 >1,200 次/2s 同步等待）。
4. **客户端高位别名句柄表（`VulkanBoxedHandles` + `VkDecoder.cpp`）**：以 bit-63 为哨兵标记的客户端预分配句柄别名（`registerClientAlias` / `resolveClientAlias` / `popClientAlias`），支撑 Guest 异步创建 `VkBuffer` 与 `VkSemaphore`（每 2 秒消灭 **1,840 次**同步句柄往返，且在 `remove`/`removeDelayed` 中自动回收零泄漏）。

#### B. Guest 端优化（[apply_guest_patches.py](file:///storage/Users/currentUser/Workspace/svbox-Android/gpu/android_guest/apply_guest_patches.py)）
1. **GLES `eglSwapBuffers` 去同步 `glFinish`**：[egl.cpp](file:///storage/Users/currentUser/Workspace/svbox-Android/gpu/third_party/goldfish-opengl-r/system/egl/egl.cpp) 无 native sync fd 时以有序 `hostCon->flush()` 替代阻塞 `eglWaitClient()`，`SwapBuffers -> FrameCompleted` 从 `2.5–10.2 ms` 降至 **`0.33–0.63 ms`**。
2. **Vulkan Live-Bound + CommandBuffer 精确脏区间追踪**：拦截 `vkBindBufferMemory*`/`vkDestroyBuffer`/`vkBindImageMemory*`/`vkDestroyImage` 及 `vkCmdBindVertexBuffers`/`vkCmdBindIndexBuffer`/`vkCmdCopyBuffer`/`vkCmdCopyBufferToImage`，`on_vkQueueSubmit` 仅扫描本帧命令缓冲实际引用的子区间（跳过 VMA 8 MB 块内 95% 空洞与未引用缓冲），单次扫描从 **`8,192–32,358 KB` (`0.94–1.93 ms`)** 暴降至 **`53–436 KB` (`0.04–0.15 ms`)**。
3. **`on_vkGetBufferMemoryRequirements2` 缓存**：复用 `vkCreateBufferWithRequirementsGOOGLE` 已缓存的 `baseRequirements`（含 `VkMemoryDedicatedRequirements`），消灭 900 次/2s 冗余同步 IPC（`-165 ms/2s`）。
4. **Client-Alias 异步 `vkCreateBufferWithRequirementsGOOGLE` + `vkCreateSemaphore`**：按 `(device, flags, usage, sharingMode)` 缓存原始对齐/内存类型位，命中后以 bit-63 客户端句柄异步创建，消灭 **1,840 次/2s 同步 IPC（`-498 ms/2s`）**。
5. **`on_vkGetSemaphoreFdKHR` 去全管线停顿**：Android 14 `SkiaVulkanPipeline` 每帧 `swapBuffers` 均调用 `vkGetSemaphoreFdKHR`，改用有序 `enc->flush()` 替代全 GPU 排空 `vkDeviceWaitIdle`（`-167 ms/2s`，`SwapBuffers` 降至 **`0.22–0.40 ms`**）。
6. **`on_vkGetFenceStatus` 时间窗防队头阻塞 + FIFO 批量缓存**：在 `vkQueueSubmit` 后 `3.5 ms` 内及 `8 ms` 轮询窗内不打断 Host `RenderThread` 流水线（24 ms 安全上限保底不卡死），任一 fence 返回 `VK_SUCCESS` 时按 Vulkan FIFO 队列语义批量标记同队列更早 fence 为已触发。

#### C. 酷安主信息流真机同口径实测对比（`HAD-W32` · `720×1280 @ 120Hz` · 大核 `12-19` · 10 轮连续快速滑动）
| 指标 | Vulkan 优化前 | Vulkan 4KiB Diff | **Vulkan 最终版 (`skiavk`)** | **GLES 最终版 (`skiagl`)** |
| --- | --- | --- | --- | --- |
| **10s 渲染总帧数** | `232 帧` | `248 帧` | **`891–944 帧`**（**3.8×–4.1×**） | **`696–753 帧`** |
| **P50 帧耗时** | `97 ms` | `36 ms` | **`11 ms`**（**快 8.8×**） | **`9–10 ms`** |
| **P90 帧耗时** | `250 ms` | `69 ms` | **`23 ms`**（**快 10.9×**） | **`17–25 ms`** |
| **P99 帧耗时** | `700 ms` | `150 ms` | **`42–69 ms`**（**快 10×–16.7×**） | **`113 ms`** |
| **卡顿率（Janky）** | `93.97%` | `66.53%` | **`9.01%–9.09%`**（**↓10.4×**） | **`4.25%–7.33%`** |
| **滑动连续帧间隔** | `35–95 ms` | `13–19 ms` | **`6.9–8.33 ms`（锁 120 FPS）** | **`6.7–7.7 ms`（锁 120 FPS）** |
| **稳定性 & 画面** | 9–12s 闪退 | 0 崩溃 | **0 崩溃 · 100% 像素无撕裂** | **0 崩溃 · 100% 像素无撕裂** |

---

## 第 11 章：鸿蒙设计规范（HarmonyOS PC）+ MuMu 布局 ArkUI 重写与多实例管理

### 11.1 视觉规范与布局重构
对照华为官方《HarmonyOS 设计指南 — 电脑应用设计 / 色彩 / 圆角参数》及 `mumu/` 参考图，彻底移除硬编码深色 hex 色值，使用官方语义分层色（`$r('sys.color.background_secondary')`、`comp_background_primary`、`brand`、`confirm`、`warning`、`font_primary/secondary/tertiary`、`comp_divider`）及系统圆角体系（`6/12/16/20/24 vp`）重写全量 ArkUI 界面，**自动适配鸿蒙 PC 浅色/深色系统主题**及窗口三键栏主题同步（[EntryAbility.ets](file:///storage/Users/currentUser/Workspace/svbox-Android/hap/entry/src/main/ets/entryability/EntryAbility.ets)、[EmulatorWindowAbility.ets](file:///storage/Users/currentUser/Workspace/svbox-Android/hap/entry/src/main/ets/entryability/EmulatorWindowAbility.ets)）：
- **主管理器（[Index.ets](file:///storage/Users/currentUser/Workspace/svbox-Android/hap/entry/src/main/ets/pages/Index.ets)）**：采用双分栏架构（`72vp` 左侧图标导航轨「设备 / 应用 / 设置」+ 右侧工作区），顶部工具栏提供 `+ 新建设备`、`批量操作`、`排列窗口`、排序菜单（按编号/时间/名称）及卡片/紧凑列表视图切换；应用中心直接内嵌于主窗，支持搜索、分类过滤与桌面快捷方式管理。
- **传统模拟器窗口（[EmulatorWindowPage.ets](file:///storage/Users/currentUser/Workspace/svbox-Android/hap/entry/src/main/ets/pages/EmulatorWindowPage.ets)）**：参考 MuMu 设备窗，去除冗余底栏，将返回/主页/多任务/拆出独立窗/音量与电源菜单集成至 `40vp` 极简顶栏，最大化 Android 渲染视口面积。
- **完整弹窗体系**：新建设备（[CreateInstanceDialog.ets](file:///storage/Users/currentUser/Workspace/svbox-Android/hap/entry/src/main/ets/components/CreateInstanceDialog.ets)）、分栏式 9 页签设备设置（[DeviceSettingsDialog.ets](file:///storage/Users/currentUser/Workspace/svbox-Android/hap/entry/src/main/ets/components/DeviceSettingsDialog.ets)）、重命名（[RenameDialog.ets](file:///storage/Users/currentUser/Workspace/svbox-Android/hap/entry/src/main/ets/components/RenameDialog.ets)）、备份与还原（[BackupRestoreDialog.ets](file:///storage/Users/currentUser/Workspace/svbox-Android/hap/entry/src/main/ets/components/BackupRestoreDialog.ets)）及全局设置中心（[GlobalSettingsDialog.ets](file:///storage/Users/currentUser/Workspace/svbox-Android/hap/entry/src/main/ets/components/GlobalSettingsDialog.ets)）。

### 11.2 原生多实例隔离与存储运维能力
- **持久化多实例模型（[InstanceModel.ets](file:///storage/Users/currentUser/Workspace/svbox-Android/hap/entry/src/main/ets/model/InstanceModel.ets)）**：自动兼容迁移原有默认实例 `#0`（保留 `userdata14` 已有应用与数据），支持一次批量新建 1–5 台独立 Android 14 设备（`instances/<id>` 独立 `/data` 与 `owner.db`）。
- **C++ Runtime & NAPI 后端（[android_runtime.cpp](file:///storage/Users/currentUser/Workspace/svbox-Android/hap/entry/src/main/cpp/android_runtime.cpp)、[napi_init.cpp](file:///storage/Users/currentUser/Workspace/svbox-Android/hap/entry/src/main/cpp/napi_init.cpp)）**：新增 `setInstanceConfig`（隔离数据目录、自定义 DPI、音频开关、ADB Root 开关、自定义机型 `ro.product.*`、自定义 ADB 端口）、实时磁盘占用统计（`getInstanceDiskMb`）、缓存清理（`cleanInstanceCache`）、后台异步复制实例（`cloneInstanceAsync`）、安全删除（`deleteInstanceData`）及 `tar/tar.gz` 异步备份还原（`backupInstanceAsync` / `restoreInstanceAsync`）。
- **真机验收（`HAD-W32`）**：已完成待机态与 120Hz Vulkan 运行态截屏视觉核验（`2090×1394`），并确认 `Android 14` + `SvboxWmAgent` + `ADB 127.0.0.1:5555` + `Maleoon 916 Vulkan` 全链路正常开机。

### 11.3 华为设计精修：统一设计令牌、原生半模态面板（`bindSheet`）与完整动效体系
- **统一设计令牌（[DesignTokens.ets](file:///storage/Users/currentUser/Workspace/svbox-Android/hap/entry/src/main/ets/common/DesignTokens.ets)）**：严格收敛全应用间距（`4/8/12/16/20/24 vp` 8vp/4vp 栅格）、图标 5 档尺寸（`12/16/20/24/44 vp`）、字号层级、控件高度与华为官方动效曲线（标准贝塞尔 `cubic-bezier(0.4, 0, 0.2, 1)`、加减速曲线及阻尼弹簧 `springCurve(0, 1, 228, 30)` / `(0, 1, 320, 34)`）。
- **原生半模态面板重构（`bindSheet` + `SheetType.CENTER` + `BlurStyle.COMPONENT_ULTRA_THICK`）**：将设备设置（[DeviceSettingsDialog.ets](file:///storage/Users/currentUser/Workspace/svbox-Android/hap/entry/src/main/ets/components/DeviceSettingsDialog.ets)）、新建设备（[CreateInstanceDialog.ets](file:///storage/Users/currentUser/Workspace/svbox-Android/hap/entry/src/main/ets/components/CreateInstanceDialog.ets)）、备份与还原（[BackupRestoreDialog.ets](file:///storage/Users/currentUser/Workspace/svbox-Android/hap/entry/src/main/ets/components/BackupRestoreDialog.ets)）及全局设置中心（[GlobalSettingsDialog.ets](file:///storage/Users/currentUser/Workspace/svbox-Android/hap/entry/src/main/ets/components/GlobalSettingsDialog.ets)）全部重构为鸿蒙 PC 居中半模态面板，自带系统沉浸毛玻璃材质、原生双行标题栏与关闭按钮及弹簧转场动效，并分散挂载于 4 个独立结构锚点避免冲突。
- **全链路交互动效**：左侧导航轨、设置侧边栏与视图切换分段开关采用弹簧滑移选中指示器；「设备 ↔ 应用」主工作区、设置 9 子页签、常规/批量工具栏、设备卡片增删与排序、复选框滑入及状态胶囊均配置 `animateTo` + `TransitionEffect` 过渡；卡片与按钮全面覆盖 PC 悬浮微浮起（`onHover` + `ShadowStyle`）与 `ClickEffectLevel.LIGHT` 按压回弹反馈。

### 11.4 根因定位与修复：`svbox-netd` Binder Pipe `EPIPE` 导致 `system_server` 崩溃卡开机动画 + 磁盘清理安全加固
- **现象**：用户执行磁盘清理后再次启动 Android 14，画面一直停留在开机动画无法进桌面。
- **真实根因（100% 从 `hap_logs/logcat.txt` 实证）**：
  1. 并非用户数据损坏——`logcat.txt` 明确记录 `StatsCompanionService.lambda$informAllUids$0()` 在后台线程 `statsCompanionService.bg` 向 `stats` (`NODE_STATSD`) 服务传入的管道写端执行 `ProtoOutputStream.flush()` 时触发 `IOException: write failed: EPIPE (Broken pipe)` → 未捕获 `RuntimeException` → `system_server` 收到 `SIGKILL (9)` 退出 → `Zygote` 随之退出，开机动画无限循环。
  2. 定位到 [`netd/svbox_netd.c`](file:///storage/Users/currentUser/Workspace/svbox-Android/netd/svbox_netd.c) `binder_wr()` 在通过 `SCM_RIGHTS` 收到 Binder 传递的文件描述符后直接 `close(fds[i])`——把 `StatsCompanionService.informAllUids()` 传来的管道读端 `readFd` 立刻关掉了！此前缓存热态启动快时写端偶尔先写完赢下竞态，清理缓存后启动时序略微变化便稳定踩中 `EPIPE`。
- **双端修复**：
  1. **[`netd/svbox_netd.c`](file:///storage/Users/currentUser/Workspace/svbox-Android/netd/svbox_netd.c)**：新增 `dispose_received_fd()` + `drain_pipe_thread()`；对收到的 `S_ISFIFO` 管道读端启动 64KB 栈分离线程异步 `poll` + `read` 排空至 EOF 后再 `close()`（15s 空闲兜底超时），既彻底消灭 `EPIPE` 又防止超过 64KB 管道缓冲导致对端写阻塞。
  2. **[`napi_init.cpp`](file:///storage/Users/currentUser/Workspace/svbox-Android/hap/entry/src/main/cpp/napi_init.cpp) / [`DeviceSettingsDialog.ets`](file:///storage/Users/currentUser/Workspace/svbox-Android/hap/entry/src/main/ets/components/DeviceSettingsDialog.ets) / [`Index.ets`](file:///storage/Users/currentUser/Workspace/svbox-Android/hap/entry/src/main/ets/pages/Index.ets)**：`CleanInstanceCacheNapi` 运行态禁止清理同一运行中实例（返回 `-1.0`），移除对系统级 `/data/cache` 的清空，仅安全清理 `/data/tombstones`、`/data/anr`、`/data/system/dropbox` 及 `/data/data/*/cache`、`code_cache`；UI 侧在设备运行中禁用「立即清理 / 磁盘清理」并给出明确提示。
- **真机实测（`HAD-W32`）**：重编 `svbox-netd`、HAP 与 `svbox.hnp` 安装验证，Android 14 秒进桌面（`sys.boot_completed=1`、`Starting phase 1000`、`QuickstepLauncher`、`SvboxWmAgent`、`ADB 127.0.0.1:5555` 全部在线），`StatsCompanionService` 零报错，已装应用（酷安/贴吧/CPU Info）100% 保留。

### 11.5 修复 Android 内 CPU 拓扑探测（`Cores: 1 / ARM Neon: No` → `Cores: 8 / ARM Neon: Yes`）与 `--cpuset` 档位大小写匹配
- **现象**：在 Android 14 内运行 `CPU Info`（底层使用 PyTorch/Meta `libcpuinfo`）显示 `Cores: 1`、`ARM Neon: No`，且日志刷屏 `getMinMaxFreq() - cannot read file`；同时 `ps` 发现 `Performance` 档位未下发 `--cpuset`。
- **双根因**：
  1. **HAP 沙箱拦截宿主 `/sys` 且未挂载 `/sys/devices/system/cpu`**：容器虽然真实跑在 8 核 ARM64（`nproc=8`、`/proc/cpuinfo` 具备 `fp asimd ... sve`、`AT_HWCAP=0xffffffff`），但 `libcpuinfo` 的 `cpuinfo_initialize()` 第一步读取 `/sys/devices/system/cpu/{possible,present,kernel_max}` 即返回 `ENOENT` 并提前中止，未进入 `/proc/cpuinfo` 解析与 `cpuinfo_isa.neon` 置位，导致同时误报 `Cores: 1` 与 `ARM Neon: No`。
  2. **[`ohos_bridge/src/sched_qos.cpp`](file:///storage/Users/currentUser/Workspace/svbox-Android/ohos_bridge/src/sched_qos.cpp)**：`ResolveCpuset()` 仅匹配小写 `"perf"/"balanced"/"eco"`，而新 UI 传入 `"Performance"/"Balanced"/"Eco"` 导致返回空字符串、未下发 `--cpuset`。
- **修复与实机验收**：
  1. [`ohos_bridge/src/sched_qos.cpp`](file:///storage/Users/currentUser/Workspace/svbox-Android/ohos_bridge/src/sched_qos.cpp#L262-L303)：统一小写归一化并兼容 `"perf"/"performance"`、`"balanced"`、`"eco"`，默认大核兜底。
  2. [`hap/entry/src/main/cpp/android_runtime.cpp`](file:///storage/Users/currentUser/Workspace/svbox-Android/hap/entry/src/main/cpp/android_runtime.cpp#L270-L410)：每次启动按实际 `--cpuset` 核数动态生成完整 `/sys/devices/system/cpu` 目录树（`possible/present/online/kernel_max`、`cpu0..N-1/{online,topology/*,cpufreq/*,cache/index0..3/*}`）并挂载进容器，同步从宿主 `/proc/cpuinfo` 读取 `Hardware` 写入 `ro.soc.{manufacturer,model}`、`ro.hardware.chipname`、`ro.board.platform`。
  3. **真机实测（`HAD-W32`）**：`CPU Info` 实时界面恢复显示 **`Cores: 8`**、**`ARM Neon: Yes`**、8 核 L1d/L1i Cache 及 **`Core 0–7: 2.32 GHz`**，`ro.soc.model=KirinX90`，`logcat` 零 `getMinMaxFreq` 报错。

### 11.6 修复安装应用卡死（`PackageInstallerActivity` Binder `NO_MEMORY` / `OutOfMemoryError` / ANR）
- **现象**：在 Android 14 内从酷安点击安装带高分辨率图标的 APK 时，系统安装器界面卡死 5 秒后触发 ANR 崩溃退出。
- **真实根因（`hap_logs/logcat.txt` 实证）**：
  1. `IPCThreadState` 连续打印 `Driver returned error (NO_MEMORY)`，随后 `PackageInstallerActivity.startInstall()` 调用 `IActivityTaskManager.startActivity()` 抛出 `java.lang.OutOfMemoryError`，且未清空的 `mOut` 导致后续所有主线程 Binder 事务（包括崩溃上报）持续返回 `NO_MEMORY` 直至 5s 输入超时 ANR。
  2. 定位到 [`svbox/src/payload/logic/vdev.c`](file:///storage/Users/currentUser/Workspace/svbox/src/payload/logic/vdev.c#L74) 的 `VDEV_BWR_SCRATCH_CAP` 仅有 **256 KiB**（且 `vdev_proto.h` `VDEV_MAX_PAYLOAD` 为 512 KiB），低于 Android 内核 Binder 的 1 MiB 单事务上限；当系统安装器把大尺寸应用图标 `Bitmap` 随 `Intent` 一起打包跨进程传递时超过 256 KiB，触发 `vdev.c` 直接返回 `-ENOMEM`。
- **修复与真机验证**：
  1. **[`svbox/src/payload/include/vdev_proto.h`](file:///storage/Users/currentUser/Workspace/svbox/src/payload/include/vdev_proto.h#L14) & [`svbox/src/payload/logic/vdev.c`](file:///storage/Users/currentUser/Workspace/svbox/src/payload/logic/vdev.c#L730-L810)**：将通用 `VDEV_MAX_PAYLOAD` 提升至 **2 MiB**；`binder_write_read()` 改为三档缓冲策略（`<= 8 KiB` 栈零拷贝 → `<= 256 KiB` 线程缓存 → `256 KiB ~ 2 MiB` 一次性按需 `mmap` 用完即 `munmap`，零常驻内存开销）。
  2. **[`svbox-Android/binderd/svbox_binderd.c`](file:///storage/Users/currentUser/Workspace/svbox-Android/binderd/svbox_binderd.c#L41)**：将每进程按需分页的 `PROC_MEM_SIZE` 从 2 MiB 扩至 **4 MiB**，避免并发大事务碎片化。
  3. **真机实测（`HAD-W32`）**：已重编 `svbox`、`svbox-binderd` 并重新打入 `svbox.hnp` 安装验证；对 114 MB 酷安 APK 走系统 `PackageInstallerActivity` 点击安装/更新及 `pm install -r` 均无卡顿、`NO_MEMORY / OutOfMemoryError / ANR` 计数为 **0**。

### 11.7 根治 Unity/IL2CPP（《月圆之夜》）更新完成卡死（Boehm GC `SIGSEGV`）+ `seccomp-backstop` 热点直通加速 + `binderd` `guest_map_len` 边界修复
- **现象**：运行《月圆之夜》（Unity 2020.3 IL2CPP）时：① 首次启动较慢；② 走到「更新完成!」（`LoadConfigAndCache:zh-cn`）后画面锁死在 `0/120Hz`。
- **三项精确根因（从 CrashSight `tomb_*.txt`、`/proc/$pid/maps`、`libil2cpp.so` 反汇编与 `logcat.txt` 完整实证）**：
  1. **卡死根因（`svbox` `payload_stack` 期间投递异步信号导致 Boehm GC 越界 `SIGSEGV`）**：
     - 反汇编 `libil2cpp.so` 崩溃 PC（`0x12ae5fc`: `ldr x24, [x20, #16]`，`r24 = 0x3f80050090` 为 `svbox` payload `.text` 返回地址，`r23 = 0x7f46897dd0` 在 64 KiB `payload_stack` 顶部 `0x7f46898000` 下方 560 字节）实证：某 IL2CPP 线程正在 `svbox` `payload_stack` 上执行拦截系统调用时，被 Boehm GC 的 `SIGPWR` 暂停信号打断，导致 `GC_suspend_handler` 把 `stop_info.stack_ptr` 记在了 `payload_stack` 上；随后 `UnityMain` 的 `GC_mark_from` 试图从 `payload_stack` 扫到线程真实 `pthread` 栈顶，跨越 `0x7f46898000` 撞上紧邻的 `---p` 保护页触发 `SEGV_ACCERR`，其余 14 条 IL2CPP 线程永远停在 `do_rt_sigsuspend`，画面停帧 `0/120Hz`。
  2. **启动慢根因（`svbox/src/loader/backstop.c` BPF 过滤列表过窄）**：
     - `--no-inline-patch-prefix /data` 下 `backstop_filter()` 仅放行 6 个系统调用，导致 `/data` 原生库中大量本就直通的 `futex`、`clock_gettime`、`nanosleep`、`sched_yield`、`tgkill`、`rt_sigsuspend`、`epoll_pwait`、`ppoll`、`gettid` 等热点调用全部走内核 `SIGSYS` 陷阱。
  3. **顺手修复的隐藏隐患（`svbox-binderd` `proc_mem_alloc` 未按 `guest_map_len` 钳位）**：
     - Android `libbinder` 仅映射 `1 MiB - 8 KiB`（`1040384` B），但 `proc_mem_alloc` 原先按 `PROC_MEM_SIZE` 分配，高并发大事务下可能越过进程实际 `mmap` 上界引发 `Parcel::unflattenBinder` `SEGV_MAPERR`。
- **修复与真机验证（`HAD-W32`）**：
  1. **[`svbox`](file:///storage/Users/currentUser/Workspace/svbox/src/payload/arch/aarch64/entry.S)（通用 Linux 机制）**：`payload_stack_claim(guest_sp)` 记录线程真实栈指针；`integrity.c` 对未设 `SA_ONSTACK` 的异步用户信号处理器包装通用汇编跳板 `payload_sig_trampoline`——仅当信号打断 `payload_stack` 时切回真实 `guest_sp`、修补 `uc_mcontext.sp` 并临时挂起 `reentry_guard`，执行完毕无损切回 `rt_sigreturn`；同时移除 `SIGSYS` 的 `SA_ONSTACK`，并在 `backstop.c` 放行 27 个纯直通热点系统调用。
  2. **[`binderd/svbox_binderd.c`](file:///storage/Users/currentUser/Workspace/svbox-Android/binderd/svbox_binderd.c#L559-L570)**：`proc_mem_alloc()` 严格按 `min(proc->mem_size, proc->guest_map_len)` 分配。
  3. **真机实测（`HAD-W32`）**：《月圆之夜》顺利越过「更新完成!」与全量 YooAsset 配置加载进入 **`[GameEntrancePanel]` 主入口与登录界面**，`GameManager` 启动加载耗时从 `24.57s` 降至 **`14.20s`**（快 42%），**零 `SIGSEGV` / 零新 tombstone / 零 `system_server` 崩溃**，`hwcomposer` 连续稳定输出 3,900+ 帧。

### 11.8 根治 `system_server` 偶发 `SIGSEGV` 导致酷安/全系统卡死（`svbox-binderd` 在途 `BINDER` 节点引用计数 UAF）
- **现象**：正常浏览酷安一段时间（尤其在子服务退出或释放句柄后），界面卡死且日志报 `DeadSystemException`，随后整个 Android 系统无响应。
- **真实根因（`F/DEBUG` 回溯 100% 实证）**：
  - `system_server`（`binder:64357_1`）在 `IActivityManager$Stub.onTransact` → `Parcel::unflattenBinder()` → `RefBase::incStrongRequireStrong()` 处触发空指针/悬垂指针 `SIGSEGV`（`x9 = 0x73622a85 = SV_BINDER_TYPE_BINDER`），进而导致 `Zygote` 退出、所有应用陷入 `DeadSystemException`。
  - 定位到 [`binderd/svbox_binderd.c`](file:///storage/Users/currentUser/Workspace/svbox-Android/binderd/svbox_binderd.c) 两处引用计数缺口：
    1. 将客户端 `SV_BINDER_TYPE_HANDLE` 还原为 `SV_BINDER_TYPE_BINDER`（`node->ptr / node->cookie`）发回属主进程（如 `system_server`）时，未在目标事务 `mem_block` 上为该 `node` 保活在途强引用；一旦客户端紧接着执行 `BC_RELEASE` 或退出，`node->strong_ref_total` 归零立刻下发 `BR_RELEASE` 销毁 `JavaBBinder`，随后 `system_server` 读到在途事务调用 `incStrongRequireStrong()` 立即 UAF 崩溃；
    2. `BR_TRANSACTION` 的目标节点 `target_node` 同样未在 `mem_block` 中保活在途强引用，且对已发过 `BR_RELEASE` 的节点未做空指针/死节点防护。
- **修复与真机压测（`HAD-W32`）**：
  - [`binderd/svbox_binderd.c`](file:///storage/Users/currentUser/Workspace/svbox-Android/binderd/svbox_binderd.c#L535-L580)：在 `struct mem_block` 中新增在途节点追踪 `mem_block_add_node()`（在 `BC_FREE_BUFFER` 与进程 `drop_conn` 时成对释放 `strong_ref_total`）；同时新增 `bnode.released_strong_once` 标记，对 `target_node` 与 `HANDLE → BINDER` 还原均保活在途强引用并对已释放节点安全降级（`SV_BR_DEAD_REPLY` / 空 binder）。
  - 真机执行酷安两轮连续滑动 + 进贴返回 + 强杀重启压测（模拟活跃句柄突然销毁），`system_server` 与酷安全程稳定存活（`P50 = 9 ms`、`GPU P99 = 1 ms`），`SIGSEGV / unflattenBinder / DeadSystemException / ANR` 全为 **0**。

### 11.9 根治《王者荣耀》启动无响应 ANR（`madvise(MADV_DONTNEED)` 误清零 `map_exec_copy` 匿名代码段致 `SIGILL` + `IVold` 外部应用目录权限修复）
- **现象**：启动《王者荣耀》（`com.tencent.tmgp.sgame`）停留在黑屏约 5 秒后弹出「应用无响应（ANR）」。
- **精准根因（从 CrashSight tombstone、`/proc/<pid>/smaps`、运行时 `mprotect`/`SIGILL` 实测抓包 100% 实证）**：
  1. **主因（Layer 1 `svbox` 通用语义缺口：`madvise(MADV_DONTNEED)` 清零 `map_exec_copy` 匿名化文件代码段）**：
     - 鸿蒙内核不允许直接执行文件映射，因此 `svbox` 的 `map_exec_copy()` 将 `libunity.so` 的 `24 MiB` 可执行 `.text` 段（`PT_LOAD[2]`）转为 `MAP_ANONYMOUS` 匿名拷贝并记录至 `vma_track`。
     - 启动 `StartUnity` 时，腾讯运行时库 `libtprt.so` 按标准 Linux 语义对 `libunity.so` `.text` 执行 `mprotect(RWX)` → `madvise(MADV_DONTNEED)`（在真实 Linux 文件私密映射上仅释放干净页 RSS、下次访问自动从磁盘重读）→ `mprotect(RX)`；但在 `svbox` 匿名拷贝 VMA 上直接透传 `MADV_DONTNEED` 导致内核把 ~18 MiB `.text` **永久清零为 `0x00000000` (`udf #0`)**。
     - 随后 `libtprt.so + 0x739a8` (`blr x8`) 调用第一个 `.text` 构造函数 `libunity.so + 0x54bf28`，立即触发 `SIGILL (ILL_ILLOPC)` → CrashSight 捕获信号拉起 Crashpad → 主线程卡在 `procmgr_wait` 等待被沙箱禁止的 ptrace → 触发 5 秒 `FocusEvent` ANR。
  2. **次因（`svbox-netd` `IVold` 外部应用目录权限不足）**：
     - `svbox-netd` 在宿主机创建 `/storage/emulated/0/Android/data/<pkg>` 时默认 `0775`（容器内映射为 `root:root 0775`），导致非 root 应用 UID（`u0_a91`）写缓存目录报 `EACCES`，刷屏 `GCloudCore: CreatePath .../cache/GCloudSDKLog failed!`。
- **修复与真机验证（`HAD-W32`）**：
  1. **[`svbox/src/payload/logic/hook_runtime.c`](file:///storage/Users/currentUser/Workspace/svbox/src/payload/logic/hook_runtime.c#L306-L346) & [`svbox/src/payload/logic/dispatch.c`](file:///storage/Users/currentUser/Workspace/svbox/src/payload/logic/dispatch.c#L304-L352)**（严格通用 Linux 语义，零 Android/游戏代码）：新增 `vma_track_next_overlap()`（覆盖动态 `g_vmas` 与启动期 `init_vmas`，同时将 `VMA_TRACK_MAX` 扩至 `2048` 并在 `readlinkat` 失败时仍登记匿名执行拷贝 VMA）；在 `__NR_madvise` 对破坏性匿名建议（`MADV_DONTNEED` / `MADV_FREE` / `MADV_DONTNEED_LOCKED`）按重叠区间拆分，仅对真正匿名子区间透传，绝不向内核转发会清零模拟文件代码段的破坏性 `madvise`。
  2. **[`netd/svbox_netd.c`](file:///storage/Users/currentUser/Workspace/svbox-Android/netd/svbox_netd.c#L801-L876)**：`vold_mkdir_p()` 对外部存储目录（`/storage/emulated/*`、`/sdcard/*`、`/data/media/*`）显式 `chmod 0777`，确保所有应用 UID 可正常读写外部 `Android/data/<pkg>` 目录。
  3. **真机实测（`HAD-W32`）**：重编 `svbox`、`svbox-netd` 并重新打入 `svbox.hnp` 安装验证：《王者荣耀》**`+4.81s` 完成 `Fully drawn`**，顺利通过 `StartUnity`、`Sgame UnityReady1` 与 Maleoon 916 GLES 3.2 渲染初始化，进入「下载推荐 / 资源更新（3258MB）」3D 动态主界面，稳定 `30 FPS`（`avg=33.33ms`），**`SIGILL` / `ANR` / `CreatePath failed!` 全部清零**。
  4. **附带观测（独立于无响应问题）**：进入游戏更新界面持续运行约 15–25s 后，游戏自身安全模块线程（`libtersafe.so`）会主动向自身进程发送 `kill(getpid(), SIGKILL)` 退出（已通过 `svbox` 系统调用实测确认发送方为 `sgame` 自身线程，非容器崩溃或 ANR）。

### 11.10 纯 Seccomp 模式（`--no-inline-patch` + BPF `fast_io`）底层语义修复与容器拦截模式切换（2026-11-25）

- **背景与目标**：
  - 为兼顾应用代码段零改写与热路径 I/O 性能，在 Layer 1 `svbox` 完善全局 `--no-inline-patch`（跳过 `prog`/`linker64` 及所有映射 `.so` 的 `svc #0` 内联补丁）+ BPF 热路径直通，并在 Layer 2 `svbox-Android` 容器运行时开放实例级「系统调用拦截模式」（`自适应 / 纯净模式 / 混合模式`，默认 `自适应`）。
- **Layer 1 `svbox` 通用 Linux 修复（零 Android/游戏硬编码）**：
  1. **BPF 热路径扩展（[`backstop.c`](file:///storage/Users/currentUser/Workspace/svbox/src/loader/backstop.c) & [`main.c`](file:///storage/Users/currentUser/Workspace/svbox/src/loader/main.c)）**：非 `--isolate` / 非 `--lazy-layer` 模式下，在 BPF 中直接 `ALLOW` `lseek/read/write/readv/writev/pread64/pwrite64`，并对 `rseq (293)` / `clone3 (435)` 直接返回 `SECCOMP_RET_ERRNO(ENOSYS)`，消除 ~98% 渲染与资产读写热路径的 `SIGSYS` 陷入开销。
  2. **`rt_sigaction` `sa_mask` 去除 `SIGSYS`（[`integrity.c`](file:///storage/Users/currentUser/Workspace/svbox/src/payload/logic/integrity.c)）**：对所有 `rt_sigaction`（含同步故障信号 `SIGSEGV`/`SIGABRT`/`SIGILL` 等）剥离 `sa_mask` 中的 `SIGSYS` 位，避免使用 `sigfillset()` 的信号处理器在运行期屏蔽 `SIGSYS` 触发内核直接 `SIGSYS` 终止。
  3. **`rt_sigprocmask` 同步回写 `ucontext->uc_sigmask`（[`backstop.c`](file:///storage/Users/currentUser/Workspace/svbox/src/loader/backstop.c)）**：在 `SIGSYS` 处理器内执行 `rt_sigprocmask` 后，必须将新的内核信号掩码写回 `uc->uc_sigmask`（保持 `SIGSYS` 放开），否则返回时的 `rt_sigreturn` 会用旧 `uc_sigmask` 覆盖掉刚刚生效的信号屏蔽（此前导致依赖 `SIGRTMIN` 屏蔽的 Bionic POSIX timer 如 `com.android.bluetooth` 被 `signal 32` 杀掉）。
  4. **破坏性 `madvise` 避开当前活跃栈帧（[`syscall_disp.c`](file:///storage/Users/currentUser/Workspace/svbox/src/payload/arch/aarch64/syscall_disp.c)）**：部分运行时（如 ART `Thread::InstallImplicitProtection()`）会对当前栈下方执行 `madvise(MADV_DONTNEED)` 仅保留 1 页（4 KiB）裕量；在纯 seccomp 模式下，AArch64 内核 `SIGSYS` 压入的 `rt_sigframe` + `pt_regs` 约 5 KiB，会落入该区间被清零，导致返回 `entry.S` 时从全 0 帧弹出 `x0..x30=0, pc=0` 触发 `SIGSEGV`。现对破坏性 `madvise` 自动拆分区间，跳过 `[page_down(regs), page_up(regs + 12 KiB))`，其余栈页正常回收。
  5. **`/proc/self/maps` 虚拟化补全（[`hook_runtime.c`](file:///storage/Users/currentUser/Workspace/svbox/src/payload/logic/hook_runtime.c)、[`net_stack.c`](file:///storage/Users/currentUser/Workspace/svbox/src/payload/logic/net_stack.c)、[`vdev.c`](file:///storage/Users/currentUser/Workspace/svbox/src/payload/logic/vdev.c)、[`vfs_procfs.c`](file:///storage/Users/currentUser/Workspace/svbox/src/payload/logic/vfs_procfs.c)）**：内部 `sv_net_state` memfd 通过 `hook_note_internal` 隐藏；`vdev` 映射（如 `/dev/binder`）通过 `vma_track_add_guest` 在 `/proc/self/maps` 中还原为真实客体设备路径而非 `/memfd:svbox-binder (deleted)`。
- **Layer 2 `svbox-Android` 运行时设置与实测（`HAD-W32`）**：
  - 在 `InstanceModel.ets`、`DeviceSettingsDialog.ets`、`napi_init.cpp`、`android_runtime.{h,cpp}` 接入实例级 `syscallMode`（默认 `adaptive`：守护进程保内联补丁、`zygote64` 及全部应用纯 seccomp；另支持 `pure_seccomp` 全局纯 seccomp 与 `hybrid` 仅 `/data` 纯 seccomp），持久化并展示在「设备设置 -> 性能」与「诊断」页。
  - **真机实测**：`adaptive` 模式下 Android 14 冷启动至桌面约 **7s**，《王者荣耀》**`Displayed +2.71s`、`Fully drawn +4.35s`**（与混合模式 `+4.81s` 相比甚至略快），稳态渲染 **`avg=33.31–33.35ms`（稳态 30 FPS，零帧率损耗）**。
  - **追加通用 Linux `/proc` & `prctl` 一致性补齐**：进一步修复了 `/proc/self/smaps` 中 `map_exec_copy` 只读代码段的 `Anonymous`/`Private_Dirty`/`VmFlags` 虚拟化（还原为 `Private_Clean: <Rss> kB` / `Private_Dirty: 0 kB` / `Anonymous: 0 kB` / `VmFlags: pr`）、`/proc/self/fd/<N>` 对 `--vdev` 返回客体设备路径（如 `/dev/binder`）、跨 PID `/proc/<pid>/maps` 过滤固定地址 loader/payload 与 `/memfd:svbox_net_state`，以及 `/proc/self/status` `NoNewPrivs` 与 `prctl(PR_GET_SECCOMP / PR_GET_NO_NEW_PRIVS)` 的一致性。经真机实测，《王者荣耀》在零代码段改写 + `maps`/`smaps`/`status`/`fd`/`prctl` 一致条件下仍会于 15–20s 后由自身安全线程发 `SIGKILL` 退出，说明退出与用户态容器/非 VM 架构差异（如 `SIGSYS` 陷入、`/proc` memfd、以及当前 `redroid` `userdebug/test-keys` + `/system/xbin/su` + `ro.debuggable=1` 等镜像环境）相关。

### 11.11 容器 VFS 虚拟符号链接与完整隔离一致性修复：彻底解决《王者荣耀》启动闪退（2026-11-25）

- **根因定位（严格通过通用 Linux 系统调用审计定位，未触碰任何第三方二进制）**：
  - 在 `HAD-W32` 鸿蒙应用沙箱中，宿主内核禁止 `symlinkat(2)` 并返回 `-EACCES`/`-EPERM`。`svbox` 原 `vfs_ops.c` 回退逻辑将失败的 `symlinkat` 转为「创建普通文件 + 添加 `MOUNT_BIND` 挂载项」：
    1. 对新建符号链接调用 `lstat()` 返回 **`S_IFREG`** 而非 **`S_IFLNK`**；
    2. 调用 `readlinkat()` 返回 **`-EINVAL`** 而非目标路径；
    3. 调用 `unlinkat()` 删除该符号链接时被 `contains_mount()` 拦截返回 **`-EBUSY`（`-16`）**，且在 `/proc/self/mounts` 残留伪造挂载项。
  - 同时审计并修复了以下真实容器隔离与属性一致性漏洞：
    1. 宿主环境变量（`HAP_DEBUGGABLE=true`、`OHOS_SOCKET_AppSpawn`、`HNP_*`、`UBSAN_OPTIONS`、`TMPDIR=/data/storage/el2/...` 等）通过 `execv()` 泄漏至容器内所有进程的 `/proc/<pid>/environ`；
    2. `/proc/<pid>/cgroup` 泄漏宿主路径（`/100/org.svbox.android/app_<pid>`），`/proc/<pid>/attr/current` 与 `getxattr("security.selinux")` 泄漏宿主标签（`u:r:debug_hap:s0` / `u:object_r:debug_hap_data_file:s0`）；
    3. `/proc/<pid>/status` 中 `Groups:` 泄漏宿主附加组（`1006 1097 3099`）且 `SigCgt:` 未与客体 `rt_sigaction(SIGSYS)` 状态保持一致；
    4. 磁盘 5 分区 `build.prop`（`/system/build.prop` 等）与 `getprop`（`override.prop`）不一致（磁盘仍含 `redroid`/`userdebug`/`test-keys`/`ro.debuggable=1`）；
    5. 虚拟 procfs memfd 的 `ioctl(FIONREAD)` 返回 `-EACCES` 而非标准 `-ENOTTY`；`sched_setaffinity`/`sched_getaffinity` 拒绝 4 字节 `cpu_set_t` 返回 `-EINVAL`。
- **Layer 1 `svbox` 通用 Linux 修复（零 Android/游戏硬编码）**：
  1. **完整 POSIX 虚拟符号链接（[`container_state.h`](file:///storage/Users/currentUser/Workspace/svbox/src/payload/include/container_state.h)、[`container_state.c`](file:///storage/Users/currentUser/Workspace/svbox/src/payload/logic/container_state.c)、[`vfs_ops.c`](file:///storage/Users/currentUser/Workspace/svbox/src/payload/logic/vfs_ops.c)、[`vfs_walk.c`](file:///storage/Users/currentUser/Workspace/svbox/src/payload/logic/vfs_walk.c)）**：在 `owner.db` 新增 `OWNER_SYMLINK` 标志与 `symlinks` 计数；当宿主拒绝 `symlinkat` 且 `owner.db` 启用时，将链接目标写入普通备份文件并打标 `OWNER_SYMLINK`，全面支持 `lstat`（`S_IFLNK | 0777`）、`stat`/路径跟随、`readlinkat`、`openat(O_NOFOLLOW -> -ELOOP)` 与 `unlinkat -> 0`，不再污染挂载表。
  2. **容器 procfs / LSM / 环境与系统调用一致性（[`main.c`](file:///storage/Users/currentUser/Workspace/svbox/src/loader/main.c)、[`options.c`](file:///storage/Users/currentUser/Workspace/svbox/src/loader/options.c)、[`vfs_procfs.c`](file:///storage/Users/currentUser/Workspace/svbox/src/payload/logic/vfs_procfs.c)、[`vfs_ops.c`](file:///storage/Users/currentUser/Workspace/svbox/src/payload/logic/vfs_ops.c)、[`dispatch.c`](file:///storage/Users/currentUser/Workspace/svbox/src/payload/logic/dispatch.c)、[`integrity.c`](file:///storage/Users/currentUser/Workspace/svbox/src/payload/logic/integrity.c)）**：新增 `--glue-dir` 与 `/proc/self/environ` 虚拟化；实现 `/proc/<pid>/cgroup` 容器根路径剥离（cgroup namespace 语义）、`/proc/<pid>/status` 的 `Groups:` 与 `SigCgt:/SigIgn:` 一致性、`/proc/<pid>/attr/*` 返回 `-EINVAL` 与 `getxattr/lgetxattr/fgetxattr("security.selinux")` 返回 `-ENODATA`；修正虚拟 procfs `ioctl -> -ENOTTY` 及 `sched_{get,set}affinity` 4 字节位图支持。
- **Layer 2 `svbox-Android` 修复（[`android_runtime.cpp`](file:///storage/Users/currentUser/Workspace/svbox-Android/hap/entry/src/main/cpp/android_runtime.cpp)）**：
  1. `Spawn()` 启动 `svbox` 时改为 `execve()` 并仅传入显式 `-e` 干净 Android 环境变量，配合 `--glue-dir <runBase>/tmp` 彻底消除宿主环境变量进入内核 `/proc/<pid>/environ`。
  2. 在临时 `inject_layer` 中基于 `override.prop` 同步生成一致的 5 分区 `build.prop`/`default.prop`（不修改只读 `rootfs14`），并在 `EnsureOwnerDb` / `ForgetOwnerDbTree` 中同步维护 `symlinks` 计数。
- **真机实测与回归验证（`HAD-W32`）**：
  - `svbox` 全部 VFS/FS Policy/OCI 回归套件通过；容器内 `ln -s` / `ls -l`（`lrwxrwxrwx`）/ `readlink` / 跟随读取 / `rm` 全部符合 POSIX。
  - 正式构建安装后在 `enableRoot=false` 下启动《王者荣耀》（PID 51231），**连续稳定运行 98 秒以上无任何 `SIGKILL` 或闪退**，`mResumed=true`，稳态 30 FPS 流畅渲染（`avg=33.34ms`）。

### 11.12 根治 Android 14 Chromium WebView 125 三重故障（`webview_zygote` ANR、V8 JIT `CodeRange` OOM 与 `drawVk` 白屏死锁）
- **背景与现象**：在《王者荣耀》点击「与QQ好友玩」拉起 `OpenWebView` 或直接启动 `org.chromium.webview_shell` 时，依次暴露出 3 个叠加的底层问题：
  1. **`webview_zygote` 启动即 `SIGABRT` → 20.4s `ActivityManager` 锁阻塞 ANR**：`com_android_internal_os_Zygote.cpp:1187` 遍历 `/data/user` 检查 `d_type` 时报 `Unexpected type: 8 /data/user/0`，因为 `OWNER_SYMLINK` 虚拟符号链接在 `getdents64` 中仍返回宿主普通文件的 `DT_REG (8)` 而非 `DT_LNK (10)`。
  2. **`sandboxed_process0` V8 JIT `CodeRange` OOM 崩溃（`SIGTRAP`）**：在 `ro.build.type=user` 下（Chromium 忽略 `/data/local/tmp/webview-command-line`），`sandboxed_process0` 通过 `seccomp(2)`/`prctl(PR_SET_SECCOMP)` 在 `svbox` 的 `--seccomp-backstop` 之上叠加安装了宿主内核 Seccomp-BPF 过滤器，把 `svbox` 内部 `platform_mprotect_exec` 的 `prctl(PR_HM_JIT)` 及 VFS 调用拦截为 `-EPERM`，导致 V8 预留 256 MiB `CodeRange`（`mprotect(..., RWX|BTI)`）失败返回 `-EINVAL`。
  3. **`skiavk` 下 `WebViewFunctor::drawVk` 白屏死锁 ANR**：`debug.hwui.renderer=skiavk` 时，无 `/dev/goldfish_sync` 条件下 HWUI `RenderThread` 在 `drawVk` 中等待 `Chrome_InProcGpuThread`，而后者阻塞于 `gfxstream.sock`（`af_unix_recvfrom`），导致 HTTPS 多帧网页白屏并 5s 输入超时 ANR。
- **Layer 1 `svbox` 通用 Linux 修复**：
  1. **`getdents64` 虚拟符号链接 `d_type` 修正（[`container_state.h`](file:///storage/Users/currentUser/Workspace/svbox/src/payload/include/container_state.h)、[`container_state.c`](file:///storage/Users/currentUser/Workspace/svbox/src/payload/logic/container_state.c)、[`syscall_disp.c`](file:///storage/Users/currentUser/Workspace/svbox/src/payload/arch/aarch64/syscall_disp.c)）**：新增 `owners_fix_getdents()`，仅在 `owners_has_symlinks()` 为真时做一次 `fstat(dirfd)` + `owner.db` O(1) mmap 查表，将 `OWNER_SYMLINK` 条目的 `d_type` 从 `DT_REG`/`DT_UNKNOWN` 改写为 `DT_LNK (10)`。
  2. **guest Seccomp-BPF 7 指令放行前缀包装（[`slow_syscalls.inc`](file:///storage/Users/currentUser/Workspace/svbox/src/payload/arch/aarch64/slow_syscalls.inc)、[`dispatch.c`](file:///storage/Users/currentUser/Workspace/svbox/src/payload/logic/dispatch.c)）**：路由 `__NR_seccomp` 至 `dispatch_syscall`，在 guest 调用 `SECCOMP_SET_MODE_FILTER`（含 `prctl(PR_SET_SECCOMP, 2, &fprog)` 与 `seccomp(1, flags, &fprog)`）时自动前置 7 条 cBPF 指令对 `svbox` 自身 loader/payload 地址段（`LOADER_FIXED_BASE` / `PAYLOAD_FIXED_BASE`）返回 `SECCOMP_RET_ALLOW`，其余探测与规则全部原生透传内核；Chromium 渲染进程原生启用真实内核 Seccomp-BPF 且不再拦截 `svbox` 内部的 `prctl(PR_HM_JIT)` / VFS 调用，彻底解锁 **完整 V8 JIT**。
- **Layer 2 `svbox-Android` 修复**：
  1. [`android_runtime.cpp`](file:///storage/Users/currentUser/Workspace/svbox-Android/hap/entry/src/main/cpp/android_runtime.cpp)：将 `debug.hwui.renderer` 固定为 `skiagl`（与 `debug.renderengine.backend=gles` 对齐，规避 `drawVk` 互操作死锁，同时保持 `ro.hardware.vulkan=ranchu` 使 3D 游戏继续享受原生 Vulkan 硬件加速），并移除旧版 `/data/local/tmp/webview-command-line`（`--jitless`）临时绕过；新增 `inline`（原版全量内联替换）与 `--ps syscallMode` 调试切换支持。
  2. [`egl.cpp`](file:///storage/Users/currentUser/Workspace/svbox-Android/gpu/third_party/goldfish-opengl-r/system/egl/egl.cpp) / [`apply_guest_patches.py`](file:///storage/Users/currentUser/Workspace/svbox-Android/gpu/android_guest/apply_guest_patches.py)：将 `eglMakeCurrent` 中的 `rcMakeCurrentAsync` 改为同步 `rcMakeCurrent`，彻底消除多线程交接 EGLContext（如 `UnityMain` 解绑 `EGL_NO_CONTEXT` 后 `UnityGfxDeviceW` 立即绑定同一上下文）在宿主双 `RenderThread` 上的 `EGL_BAD_ACCESS` 竞态（修复前 2/3 启动出现 263 条 `bufferId==0` / `!state->getBoundTexture` 错误导致字体与弹窗边框变白块；修复后连续 3 次冷启动 `emuglGLESv2_enc` 报错降至 **0 条**、3/3 画面完整无白块）。
- **真机验证（`HAD-W32`，`enableRoot=false`，`ro.build.type=user`，无 `--jitless`）**：
  - `svbox` 全量 `make check-oci` + VFS/FS Policy 回归全通过。
  - 冷启动后 `webview_zygote`、`com.android.webview:sandboxed_process0`（真实内核 Seccomp-BPF 零警告）、`media.extractor`、`media.swcodec` 全部零报错常驻；`org.chromium.webview_shell` 加载本地 HTML/JS（V8 JIT `6*7=42`）与真实 HTTPS 网页（`https://www.qq.com/`、`https://m.baidu.com/`）完整渲染无白屏；《王者荣耀》内点击「与QQ好友玩」2 秒内正常弹出清晰「QQ 授权登录」WebView 页面，连续 3 次冷启动零白块。
  - **系统调用替换模式实测对比**：`inline`（原版全量内联）因修改 `/data` 下游戏 `.so` 代码段约 <20s 触发 `SIGBUS`；`hybrid`（系统库内联 + `/data` seccomp）与 `adaptive`（Zygote/App 全走 seccomp + BPF 直通）均可完整进入公告/登录页且零白块，但约 60s 处仍存在后续待查的周期性退出（`hybrid` `SIGSEGV` / `adaptive` `SIGBUS`）。

### 11.13 根治登录后 / 60s 周期性 `SIGBUS`/`SIGSEGV` 闪退（Layer 1 `/proc/<pid>/map_files` 能力校验与隐藏 + `[vvar]/[vdso]` 归一化）
- **现象**：用户通过 QQ WebView 登录并进入《王者荣耀》游戏大厅后约 44–60s，主进程与 `xg_vip_service` 周期性以 `signal 7 (Bus error)` / `signal 11 (Segmentation fault)` 退出（即使切换 `pure_seccomp` 零代码页改写依然复现）。
- **精准根因（通过 `svbox` 自身系统调用边界环形缓冲审计定位，未触碰任何第三方二进制）**：
  1. 退出前触发线程连续执行了 **1,014 次 `readlinkat("/proc/self/map_files/<lo>-<hi>")`**。`svbox` 此前已虚拟化 `/proc/<pid>/maps`、`smaps` 与 `/proc/<pid>/fd`，但完全遗漏了 `/proc/<pid>/map_files`。
  2. 上游 Linux 5.15 内核（`fs/proc/base.c`）要求 `CAP_SYS_ADMIN` 或 `CAP_CHECKPOINT_RESTORE` 才允许列举/解析 `/proc/<pid>/map_files`（普通非特权 UID 应返回 `-EPERM`），而鸿蒙内核未校验容器虚拟能力，导致普通应用直接读出了 `0x3f00000000` 处的 `/data/app/svbox.org/.../svbox`、`/memfd:elf-mounts (deleted)`、`owner.db`、`.svbox_svc_cache` 等宿主运行时内部映射。
  3. 同时 `/proc/<pid>/maps` 中仍暴露鸿蒙内核特有的特殊 VMA 名称 `[kshare]` / `[shmm]`（标准 Linux 为 `[vvar]` / `[vdso]`），且 `path_is_internal_runtime()` 遗漏 `/memfd:elf-mounts`。
- **Layer 1 `svbox` 通用 Linux 修复（[`vfs_bridge_internal.h`](file:///storage/Users/currentUser/Workspace/svbox/src/payload/logic/vfs_bridge_internal.h)、[`vfs_ops.c`](file:///storage/Users/currentUser/Workspace/svbox/src/payload/logic/vfs_ops.c)、[`vfs_procfs.c`](file:///storage/Users/currentUser/Workspace/svbox/src/payload/logic/vfs_procfs.c)，零 Android/游戏代码）**：
  1. 新增 `pb_proc_map_files_guard()` 并在 `proot_proc_filter_pids()` 扩展对 `/proc/<pid>/map_files` 目录 `getdents64` 的处理：对无 `CAP_SYS_ADMIN`/`CAP_CHECKPOINT_RESTORE` 的非特权容器进程严格按 Linux 5.15 语义返回 `-EPERM`（并清零目录缓冲防字节残留）；对特权进程过滤掉全部受保护/内部运行时映射并经 `vma_track` / 容器路径翻译目标。
  2. 在 `path_is_internal_runtime()` 补齐 `/memfd:elf-mounts`，并在 `pb_proc_maps()` 中于 `platform_is_harmonyos()` 下将 `[kshare]` / `[shmm]` 归一化为标准 Linux `[vvar]` / `[vdso]`。
- **真机与全量回归验证（`HAD-W32`，`enableRoot=false`，`syscallMode=adaptive`）**：
  - `svbox` 全量回归 **100% PASS**（`make check-oci`、`test/vfs/test_svc.py` 84/84、`test/fs_policy/fake_ids.py`、`backstop.py`）；容器内 root 列举 `/proc/self/map_files` 0 泄漏、UID `10091` 返回 `EPERM`，`/proc/self/maps` 显示标准 `[vvar]` / `[vdso]`。
  - 真机实测《王者荣耀》主进程（PID `5199`）成功登录并进入大厅（`_m_isLobbyLoginFinish:True`，连同 `estPlugin`、`webview_zygote`、`sandboxed_process0` 并发运行），**连续稳定存活超过 8 分钟以上零 `SIGBUS`/零 `SIGSEGV`/零 `emuglGLESv2_enc` 报错**，`xg_vip_service`（PID `6826`）同样持续稳定在线 6 分钟以上。

### 11.14 根治 Microsoft Edge（Chromium 130+）启动卡死：`mremap(MREMAP_DONTUNMAP)` 清零 `map_exec_copy` 代码段致 V8 `SIGTRAP` + `Groups:` 格式对齐
- **现象**：打开 Microsoft Edge（`com.microsoft.emmx`）永久卡在启动 Logo 页，后台 `com.microsoft.emmx:sandboxed_process0`（`CrRendererMain`）反复触发 `SIGTRAP (TRAP_BRKPT)` 崩溃，且 Crashpad 报 `format error: unrecognized Groups format`。
- **精准根因**：
  1. Chromium 126+/130+ V8 `CodeRange::RemapEmbeddedBuiltins()` 对 `libchrome.so` 内嵌 builtins 段（`0x22f000` B）调用 `mremap(src, len, len, MREMAP_MAYMOVE | MREMAP_FIXED | MREMAP_DONTUNMAP, dst)` 与 `mprotect(dst, len, PROT_READ | PROT_EXEC)` 后，执行自检 `CHECK(*dst == *src)`。
  2. 在真实 Linux 文件私有映射上，`MREMAP_DONTUNMAP` 会保留源处文件内容（读源地址自动从文件重缺页）；但因鸿蒙禁止文件页可执行，`svbox` 的 `map_exec_copy()` 将 `libchrome.so` `.text` 转为 `MAP_ANONYMOUS` 匿名拷贝，透传 `MREMAP_DONTUNMAP` 导致内核把匿名页整体迁走并将原 `src` **清零为全 `0`**（`x8=0xadbe6bf9d503245f` vs `x9=0x0`），立即触发 V8 `SIGTRAP`。
  3. 同时 `pb_proc_status()` 输出 `/proc/<pid>/status` 的 `Groups:` 行缺少上游 Linux 内核 `fs/proc/array.c` 的尾部空格，导致 Crashpad 解析报格式错误。
- **Layer 1 `svbox` 通用 Linux 修复（[`dispatch.c`](file:///storage/Users/currentUser/Workspace/svbox/src/payload/logic/dispatch.c)、[`vfs_procfs.c`](file:///storage/Users/currentUser/Workspace/svbox/src/payload/logic/vfs_procfs.c)，零 Android/浏览器硬编码）**：
  1. 在 `dispatch.c` 拦截 `__NR_mremap`：当携带 `MREMAP_DONTUNMAP` 且源区间重叠受追踪 `map_exec_copy` VMA 时，以 `MAP_PRIVATE | MAP_ANONYMOUS` 在目标处新建副本、`memcpy` 拷贝并调用 `platform_mprotect_exec(PROT_READ | PROT_EXEC)`，保持源与目标双侧字节完整一致。
  2. 在 `dispatch.c` 新增 `mprotect_private_exec_copy()`：鸿蒙下对 `MAP_PRIVATE` 非匿名页调用 `mprotect(..., PROT_EXEC)` 遇 `-EACCES` 时，自动匿名拷贝升级为可执行（严格跳过 `MAP_SHARED`）。
  3. 在 `vfs_procfs.c` 补齐 `Groups:` 每项后的尾随空格以与 Linux `fs/proc/array.c` 字节级一致。
- **真机实测与回归验证（`HAD-W32`）**：
  - `svbox` 专项测试（`mremap(MREMAP_DONTUNMAP)` on `exec_copy` 源/目标字节全一致、`MAP_PRIVATE` `mprotect(RX)` 成功、`MAP_SHARED` 保持 `-EACCES`）+ 84 项 SVC + `fake_ids` + `backstop` 全部 PASS，Layer 1 纯净度通过。
  - 真机冷启 Microsoft Edge（PID `51944`）：`+1.53s` 越过启动页进入引导/默认浏览器授权弹窗，`com.microsoft.emmx_zygote`（`52072`）与隔离渲染进程 `com.microsoft.emmx:sandboxed_process0`（`52088`）稳定常驻，**`SIGTRAP` / `aw_browser_terminator` / `unrecognized Groups format` 全部清零**。

### 11.15 Linux / Android 14 容器标准接口全量对齐（Phase 1–3）及前后兼容性/性能基准对比
- **背景与目标**：对比上游 Linux 容器（Waydroid/LXC）与真实 Android 14 设备，系统排查并分 3 阶段对齐 Layer 1（`svbox`，100% 通用 Linux/POSIX 语义，零 Android/应用硬编码）与 Layer 2（`svbox-Android`，零改动 `rootfs/` 与 `rootfs14/`）的 14 项关键接口差异（`L1-1`–`L1-7` + `L2-1`–`L2-7`），并在对齐前后执行标准化兼容性评分与性能微基准对比。
- **Phase 1 — Layer 1 `svbox` 通用 Linux 容器语义（commit `0f69d15`）**：
  - `L1-1` 跨 PID `/proc/<pid>/{maps,smaps,fd,map_files}` 通过 `vdev_match_memfd()` 按 `--vdev` 名称还原 `/dev/*` 设备路径，消除 `/memfd:svbox-*` 泄漏；
  - `L1-2` 修复 `union_view.c` 空 lower 目录 `.` 返回 `ENOENT`（消除 `ls -la /dev /sys` 的 `d????????? .`），并在 `vfs_ops.c` 修正 `statfs`/`fstatfs` `f_type`（`/dev` `0x1021994` `tmpfs`、`/sys` `0x62656572` `sysfs`、`cgroup`/`bpf`/`proc`）；
  - `L1-3` 新增 `-v HOST:GUEST[:MODE][,nomnt]` 与 `--proc-cmdline STR`；`/proc/self/mounts` 与 `mountinfo` 过滤内部单文件/子目录 bind 并合成标准 `tmpfs /dev` 与 `sysfs /sys`；
  - `L1-4`–`L1-6` 缓冲虚拟化 `/proc/<pid>/{status,smaps_rollup,cgroup}`（`FDSize: 64`、`Cpus_allowed_list` 对齐 `--cpuset`、剥离鸿蒙特有 `PurgSum/PurgPin/Pss_HHP` 及 `load/iolimit/workingset/dummy` 控制器）；`/proc/stat` 单调递增 `CLOCK_BOOTTIME` 节拍；`/proc/cmdline` 及 `/proc/net/{tcp,tcp6,udp,udp6,unix,arp}`、`/proc/{modules,swaps}` 标准头回退。
- **Phase 2 — Layer 2 `svbox-Android` 系统属性、电源/温控与启动稳定性（commit `47b0da2`）**：
  - `L2-1` / `L2-6` 合成 `/sys/class/power_supply/{ac,battery}/*`、`/sys/class/thermal/thermal_zone{0,1}/*` 及 `cpufreq/stats/time_in_state`；`dumpsys battery` 报真实状态（`present=true, level=88, voltage=4210, temp=285, Li-ion`），`KernelCpuSpeedReader` 报错由 22 清零；
  - `L2-3` 清理 `eng.frank.*`/`redroid`/`ro.adb.secure=0` 属性并前置生成脱敏分区 `build.prop` 供 `propd` 与磁盘共享单一真相源；`hwcomposer_ranchu.cpp` 优先读取 `ro.sf.display.*`；非 root 模式不启动 `adbd` 且 `adb_enabled=0, development_settings_enabled=0`；
  - `L2-4` 定位上游 Redroid14 `bluetooth@1.1-service.sim` 编译期 `std::optional<Address>` 偏移 16 vs 32 ABI 缺陷导致必现 `SIGABRT`，在 `injectLayer_` 声明无蓝牙特性并停止拉起缺陷二进制，开机 `Fatal signal 6` 由 10 清零；
  - `L2-7` 启动 `system/bin/gpuservice`，`Failed to get gpu service` 报错由 16 清零。
- **Phase 3 — 嵌套 Seccomp、虚拟 Sensors HAL 与实时网络统计（`svbox` `3b7de06` + `svbox-Android` 本次提交）**：
  - `L1-7` 将 `BACKSTOP_TRAP_DATA` 升至 `0xffff`，在 wrapped seccomp 前缀中保留内部 `raw_syscall` 放行同时让 `fast_passthrough_svc` 接受 guest BPF 过滤，并通过 `SVBOX_PR_GET_GUEST_SECCOMP_TRAPS` 区分 guest `SECCOMP_RET_TRAP`（含 `data==0` 与 `data!=0`）与宿主沙箱 `si_errno==0` 拦截，实现 10/10 嵌套 seccomp 模式全通且不误伤 Chromium/Edge；
  - `L2-5` `net_stack.c` 读写映射 `svbox_net_state` 并用无锁 FD 位图原子统计 `AF_INET/AF_INET6` 收发字节与包数，同时提供 `/sys/class/net/<iface>/statistics/*`；`android_runtime.cpp` 切换至私网 `192.168.100.15/24`（gw `192.168.100.1`）+ 持久化随机 MAC（`eth_mac.txt`）并挂载 `/sys/class/net` 目录骨架；
  - `L2-2` 新增纯 C `sensors_hal/svbox_sensors_service.c`（AIDL V2 `android.hardware.sensors.ISensors/default` + FMQ 共享内存环 + futex EventFlag）及 VINTF 清单片段，向 `SensorService` 提供 8 个硬件传感器（含 9 个 AOSP 融合传感器共 **17 个在线传感器**）与实时加速度/环境事件流。
- **真机前后对比（`HAD-W32`，`enableRoot=false`，`syscallMode=adaptive`）**：
  - **兼容性评分**：Android 14 评分卡 **`0 / 13` -> `13 / 13 PASS`**；Layer 1 `L1-7` 嵌套 seccomp **`FAIL (0/2)` -> `PASS (10/10)`**；全开机日志崩溃/报错：`Fatal signal` `10 -> 0`、`KernelCpuSpeedReader` `22 -> 0`、`Failed to get gpu service` `16 -> 0`、`FATAL EXCEPTION` `0 -> 0`。
  - **性能微基准（中位数，3 轮）**：`svbox` 核心 syscall 无回退且小幅优化（`stat` `20,795 -> 17,701 ns` -14.9%、`pread_4k` `1,793 -> 1,511 ns` -15.7%、`mmap_64k` `3,251 -> 2,785 ns` -14.3%）；procfs/statfs 显著加速（`proc_status` `76,743 -> 51,151 ns` -33.3%、`proc_maps` `73,388 -> 57,852 ns` -21.2%、`proc_cgroup` `85,573 -> 51,304 ns` -40.0%、`proc_stat` `519,082 -> 363,326 ns` -30.0%、`statfs_sys` `12,998 -> 10,173 ns` -21.7%）；Android 14 开机 ~13s、SurfaceFlinger 维持 **120.00 Hz**（Maleoon 916 Vulkan/GLES）、在线服务 **235**。
  - **应用回归**：Microsoft Edge（主进程 + `zygote` + `sandboxed_process0`）与《王者荣耀》（主进程 + `xg_vip_service`）均正常启动在线、**0 `Fatal signal` / 0 `SIGTRAP` / 0 `SIGSYS` / 0 `aw_browser_terminator`**，`/proc/net/dev` 实时递增至 `eth0 rx=462,208 B / tx=91,727 B`；`rootfs/` 与 `rootfs14/` 零改动。

### 2026-11-26 存储容量虚高与无损扩容、设置持久化及第三方浏览器（Edge / UC）修复
- **存储虚占 94 GB（系统 82 GB）与 8/16/32/64/128 GB 无损扩容**：
  - 根因：`svbox --disk-mb 32768` 对 `/` 和 `/data` 同时汇报 32 GiB（34.36 GB），Android `StorageManagerService` 求和得 `68.7 GB > 64 GB` 向上取整为 `128 GB`，再减去 `/data` 可用 `34.36 GB`，算出虚高的 `已使用 94 GB / 系统 82 GB`。
  - 修复：Layer 1 `svbox`（commit `4d6e592`，ABI 32）新增通用 `--root-disk-mb` 与 `--disk-used-mb`，在 `vfs_ops.c` 按挂载体源是否位于 `root`/`lower` 区分 rootfs（报满）与外部数据卷（扣减实际占用）；Layer 2 按十进制 GB 档位映射（`root=1900 MiB`，`8/16/32/64/128 GB` 对应 `5720/13350/28600/59100/120150 MiB`）并在启动时扫描 `userdata/data` 实占，UI（设备设置→磁盘、新建设备弹窗）开放五档选择与无损扩容。真机验证：`64 GB` 档报 `已使用 18 GB / 共 64 GB（游戏 7.1 GB，系统 6.6 GB）`，无损切换至 `128 GB` 后零数据丢失且立即报 `共 128 GB`。
- **系统设置 /「允许安装未知应用」重启失效**：
  - 根因：Android 14 `AppOpsService` 延迟 10 秒才写 `/data/system/appops.xml`，而 `AndroidRuntime::Stop()` 仅等待 60ms 即 `SIGKILL`；且 `svbox-propd` 未将运行时 `persist.*` 落盘。
  - 修复：`AndroidRuntime::Stop()` 停止前执行 `FlushGuestStateBeforeStop()`（最多 1.5s 预算，先唤醒暂停进程再同步执行 `cmd appops write-settings; sync`）并将 `SIGTERM` 宽限提升至 250ms；`svbox-propd` 新增 `--persist <dataBase_/persist.prop>` 原子落盘并在 `override.prop` 之前加载。真机验证：修改 `REQUEST_INSTALL_PACKAGES=allow`、`settings put global`、`setprop persist.sys.*` 后 2 秒内强制重启，三项全部保留。
- **UC 浏览器弹窗「安装失败」并自杀**：
  - 根因：默认机型回退为 `SVBox14`/`svbox14`，含 `vbox` 子串触发 UC U4 内核 `112:COMPATIABLE_NOT_SUPPORT_ARCH: vbox` 检查。
  - 修复：默认机型回退改为干净真实 ARM64 标识（`HUAWEI` / `ALN-AL00` / `aln`），并预创建 `/data/misc/shared_relro`（`01777`）消除 `WebViewLoader` 报错。真机验证：UC 浏览器 9 进程稳定在线、U4 内核成功初始化并正常加载页面。
- **Microsoft Edge 卡在隐私确认页无法点击网页**：
  - 根因：HarmonyOS PC 上 `OH_NativeXComponent` 对鼠标点击会同时触发 `DispatchMouseEventCB` 与 `DispatchTouchEventCB`，双指同时落点导致 Chromium/Blink `WebView` 将单击误判为双指手势并吞掉点击。
  - 修复：`DispatchTouchEventCB` 过滤 `OH_NATIVEXCOMPONENT_SOURCE_TYPE_MOUSE` / `OH_NATIVEXCOMPONENT_TOOL_TYPE_MOUSE`，同时在 `InputBridge` 增加 40ms/6px 同坐标鼠触去重兜底。真机验证：Edge 顺利通过隐私确认并流畅打开 `https://www.baidu.com`。

### 2026-11-26 阶段 1–3：核心基础修复、输入桥接与自由多窗重构、GMS 只读叠加层
- **阶段 1（commit `810326e`）**：
  - 修复 `DeviceSettingsDialog.ets` 与 `CreateInstanceDialog.ets` 因 `@Builder` 按值传参导致选项点击不切换状态的问题（重构为响应式 `@Component` + `@Prop`）。
  - 取消 `"12-19"` 20 核硬编码，改为运行时探测宿主机 `cpu_capacity` / `cpuinfo_max_freq` / ALU 拓扑，按 `性能模式 × cpuCores` 动态绑定核心（为 6/8/12 核移动设备做好准备）。
  - 内存统计由虚高的 RSS 求和改为真实物理内存 **PSS**（`/proc/<pid>/smaps_rollup` + HiDebug `nmem.pss`）。
  - 新增应用列表离线缓存（`apps_cache.json` + 持久化 `dataBase_/icons`）与点击自动启动单容器，新增本地 APK 离线选择器安装（`picker.DocumentViewPicker` + `installLocalApkAsync`）。
- **阶段 2（commit `a3c5d88`）**：
  - `SvboxWmAgent` 切换为 `SHELL_UID`（`-u 2000:2000`，`com.android.shell`，对齐 scrcpy），实现中文输入法文本桥接（`INPUT_TEXT`、`SEND_KEY_COMBO`）与双向剪贴板同步（`SET_CLIPBOARD`/`GET_CLIPBOARD`/`CLIPBOARD_CHANGED` + 鸿蒙零 ACL `PasteButton`），实机验证中文 UTF-8 双向往返与 `Denying clipboard access = 0`。
  - 自由多窗重构为**支持模式**（`adaptive`，`resetAspectRatio()` + 100% 实时跟手拉伸 + 80ms 防抖单飞 `RESIZE_DISPLAY`，修复 `tickStatus` 回写旧尺寸）与**兼容模式**（`fixed_portrait`/`fixed_landscape`，使用单窗隔离 API `win.setContentAspectRatio(ratio, false, true)` 锁定比例不污染主控台）。
- **阶段 3（可开关 GMS 测试叠加层）**：
  - 基于开源 `MindTheGapps-14.0.0-arm64` 通过 `scripts/prepare_gms_layer.py` 组装精简只读层 `gms_layer14/`（24 文件，215.3 MiB；剔除 SetupWizard/Velvet 等容器冲突组件，并将 `product`/`system_ext` 映射至 `system/` 避免被 bind-mount 遮蔽），零修改 `rootfs14/`。
  - 全链路贯通 `enableGms` 开关（C++ `--lower gms_layer14` + `override.prop` / NAPI / UI「设备设置→其他」/ `--ps enableGms`），并在关闭 GMS 时自动停用可能残留于 `/data/app` 的 Play Store 自更新包避免后台崩溃。
  - 真机验证：`com.google.android.gms`、`com.google.android.gsf`、`com.android.vending` 等 7 个包与 6 个核心进程稳定运行，零 FATAL/privapp 异常，Google Play 商店成功打开并自动更新至 `54.4.11`，点击登录直达 Google 账号登录页（`MinuteMaidActivity`）。
- **阶段 4（单容器瘦身与内存占用优化）**：
  - Layer 1 `svbox`（commits `31a60db`、`e957ef2`）：补全 `--mem-mb` 模式下 `/proc/meminfo` 的 `Cached`/`KReclaimable`/`Buffers`/`Active`/`Inactive`/`SwapTotal`/`SwapFree` 上限裁剪，修复 Android 误判 `Free RAM = 14.5 GB (Lost -12.4 GB)` 导致从不回收缓存进程的根因。
  - Layer 2 `svbox-Android`：
    1. 启动后自动回收残留 `bootanimation` 进程（`53.9 MB -> 0 MB` PSS + 释放宿主 EGL 缓冲）。
    2. `SvboxWmAgent` 新增基于 `lastUpdateTime` 的磁盘标签缓存 `/ipc/icons/labels.tsv` 并采用紧凑堆参数 `-Xms4m -Xmx32m`，避免 `PackageManager.loadLabel/loadIcon` 在 `ResourcesManager` 永久 mmap 全部已安装 APK（`.apk mmap 28.0 -> 2.5 MB`，`SvboxWmAgent` 总 PSS `146.0 -> 111.8 MB`）。
    3. 按 `memLimitMb`（2G/4G/6G/8G+）分档动态注入 `dalvik.vm.heap*`（`heaptargetutilization=0.75`）、限制 `dalvik.vm.madvise.*` 预读膨胀、配置 `activity_manager_constants max_cached_processes`，并新增 `trimMemory()` 一键清理与全窗隐藏 20 秒自动后台清理机制。
  - 真机验证（4GB 档纯净 AOSP 空闲态）：总 Guest PSS 从 `1,413.1 MB` 降至 `1,183.1 MB`（`-230 MB / -16.3%`），Cached PSS 从 `237.3 MB` 降至 `13.7 MB`（`-94.2%`），`dumpsys meminfo` 空闲估算恢复真实（`Free 644 MB`）。
- **阶段 5（移动设备支持 + PC 键鼠映射与自动化脚本）**：
  - **移动设备优化（`DeviceProfile.ets` + `Index.ets` + `EmulatorWindowAbility`/`AppWindowAbility`）**：基于 `deviceInfo.deviceType`（并支持 `--ps deviceForm mobile|tablet|pc` 调试覆写）识别 PC / 平板 / 手机形态；移动端强制单容器模式（隐藏新建设备、批量管理、克隆/删除实例），默认直达离线应用中心单点拉起自由多窗；全窗口启用 `KeyboardAvoidMode.RESIZE` 避免软键盘遮挡，优化前后台切换无缝恢复渲染且不销毁 Vulkan 桥接。
  - **PC 键鼠映射引擎（`ohos_bridge/src/input_bridge.cpp` + `KeymapStore.ets` + `KeymapOverlay.ets`）**：在原生 C++ `InputBridge` 中实现零往返延迟多指虚拟触控合成（`InjectNormalizedTouch` + `SetKeymapConfig` + `HandleKeymapKey`/`HandleKeymapMouse`），支持 `WASD` 8 向模拟摇杆（finger `500`）、多键并行点击（finger `501..518`）与右键走位（finger `519`），并在模拟器与自由多窗内提供实时可拖拽键位编辑器、半透明无遮挡提示层与 MOBA/FPS/通用一键模板，按包名持久化至 `keymaps.json`。
  - **轻量自动化脚本/宏（`MacroStore` + `MacroRunner` + `MacroAutomationOverlay`）**：支持屏幕可视化录制点击/滑动与等待间隔（录制同时实时透传至 Android）、按包名持久化至 `macros.json`、支持 `1次/5次/20次/无限` 循环回放与随时中断。
  - 全链路真机回归（启用 GMS + Stage 4 瘦身 + Stage 5 全功能）：GMS 开启态总 Guest PSS 由 `2,316.7 MB` 降至 `1,691.7 MB`（`-625.0 MB / -27.0%`），7 个 GMS 进程、剪贴板 UTF-8 双向同步、`TRIM_MEMORY` 与动态 8 核拓扑均通过，零 JS/Android 致命异常。
- **2026-11-27 界面严重卡顿与拖拽缩放窗口出现白框修复**：
  - 根因 1：`GetStatusNapi()` 每 450–500ms 在 ArkUI 主线程同步调用 `SchedQos::SampleTelemetry()` 遍历全部 60+ Android 进程的 `/proc/<pid>/smaps_rollup`（触发内核 `mmap_read_lock` 逐页表扫描），导致 `org.svbox.android` 主线程长期满载 **103% CPU**、`render_service` 每 8.3ms 丢弃 VSync（`ArkUI request vsync, but no vsync received in 3 seconds`），输入卡死且窗口拖拽时 ArkUI 无法及时提交新尺寸帧而残留系统透明白框。
  - 根因 2：`AndroidRuntime::OnSurfaceChanged()` / `OnDisplaySurfaceChanged()` 在同一 `OHNativeWindow*` 拖拽缩放的每一像素都重复调用 `GfxstreamBridge::SetWindow()` / `SetDisplayWindow()`，在主线程上加锁并重建 Vulkan Swapchain。
  - 修复：`SchedQos::SampleTelemetry()` 改为 **O(1) 非阻塞缓存读取（<0.01ms）+ 后台 `Background` QoS 线程每 3 秒异步温和采样**（每 4 进程 `usleep(1000)` 避免内核锁突发）；`OnSurfaceChanged` / `OnDisplaySurfaceChanged` 对同一 `OHNativeWindow*` 指针跳过重复重建，由鸿蒙 `RenderService` 硬件 120Hz 缩放；`EmulatorWindowPage` 轮询降频至 1000ms 并仅在值变更时更新 `@State`。真机实测：主线程 CPU 从 **103% 降至 0.0%**，VSync 丢失从每帧告警降至 **0**。
- **2026-11-27 Google Play 下发 32 位旧版 Chrome 101 闪退 & 自由多窗串窗/黑屏根因修复**：
  - **问题 1 根因（Play 商店错发 32 位 Chrome 101 `495106123` 且 Edge/Brave/Chrome Beta 全报不兼容）**：
    - `android_runtime.cpp` 此前为绕开上游 Redroid14 `bluetooth@1.1-service.sim` 的 `SIGABRT`，在 `injectLayer_/vendor/etc/permissions/redroid-removed-permissions.xml` 显式剔除了 `android.hardware.bluetooth` 与 `android.hardware.bluetooth_le`。
    - 自 2022 年 5 月 Chromium M102 起，Chrome（Stable/Beta/Dev/Canary）、Edge、Brave 等现代 Chromium 浏览器均在 `AndroidManifest.xml` 声明了 Android 12 `BLUETOOTH_CONNECT`/`BLUETOOTH_SCAN`/`BLUETOOTH_ADVERTISE` 权限且未标记 `<uses-feature android:name="android.hardware.bluetooth" android:required="false" />`，触发 Google Play 隐式硬件特性过滤——现代 64 位 Chrome（102–158）全部被过滤，Play 商店被迫回退到 2022 年 5 月最后一个不触发该过滤的旧版 `101.0.4951.61`（`495106123`，32 位浏览器主模块 + 64 位 WebView），在纯 64 位设备上启动即因 `Starting in 64-bit mode requires the 64-bit native library` 崩溃。
    - 修复：在 `injectLayer_/vendor/etc/permissions/svbox-handheld-features.xml` 恢复并补齐标准手持硬件特性（`bluetooth`/`bluetooth_le`/`touchscreen.multitouch*`/`camera.any`/`camera.front`/`camera.autofocus`/`location.gps`/`sensor.gyroscope`/`sensor.light`/`usb.host`），同时通过启动初始化命令设置 `bluetooth_on=0`、`ble_scan_always_enabled=0` 并 `pm disable-user --user 0 com.android.bluetooth`，保持蓝牙服务零进程、零崩溃。
    - 真机实测：Play 商店立刻恢复 Microsoft Edge 兼容性并下发原生纯 64 位 **Google Chrome `158.0.8127.44`（`versionCode=812704404`，尾号 `4` = `arm64` 64-bit）**，完整拉起 `com.android.chrome` + `chrome_zygote` + `privileged_process` + `sandboxed_process0` 并正常加载网页，冷启动 `Fatal signal = 0`。
  - **问题 2 根因（自由多窗下应用串画到其他应用窗口内 / 空白剪裁 / 黑屏）**：
    - 串画根因：Play 商店（`slot=1, displayId=2, 1784x1768`）内点击打开 Instagram（`Task=60`）时，Android 14 `WindowManagerShell` 正在 `displayId=2` 上播放约 660ms 的 `OPEN` Shell Transition（`Surface(Task=60)` 被挂载到过渡动画根 leash）；`SvboxWmAgent` 在 `t≈140ms` 立即调用 `moveRootTaskToDisplay(60, 6)`，待 `t=660ms` 过渡动画结束时 `mFinishTransaction` 又把 `Surface(Task=60)` 抢回 `displayId=2` 的 `DefaultTaskDisplayArea`，导致 `Task=60` 逻辑尺寸按 `displayId=6`（`860x1308`）排版，却在 SurfaceFlinger 中叠加绘制到 Play 商店窗口左上角，同时 `slot=2` 永久卡死在 `frames=1`。
    - 黑屏根因：`cmdLaunchApp` 未检测目标应用是否已有存活 Task 位于 `displayId=0` 或其他屏（`ActivityStarter` 复用旧 Task 留在原屏导致新屏 `frames=0` 黑屏），且 `AppWindowPage.ets` 未检查 `LAUNCH_APP`/`MOVE_TASK` 返回值（如卸载缓存残留 `rc=-92`），失败时仍隐藏加载遮罩裸露黑屏。
    - 修复：`SvboxWmAgent.java` + `libsvbox_wm_jni.cpp` 新增 `nativeGetReaderFrames()`、`handleTaskEvent` 校验独立 RootTask 避免误拆宿主内嵌页、`moveTaskCleanly()` 等待源屏 Shell Transition 动画落稳后再搬迁并刷新源屏+目标屏焦点（附 700ms 帧数自愈看门狗）、`cmdLaunchApp()` 优先查找并搬迁已有存活 Task + 失效组件自动重解析 + 启动后跨屏校验；`AppWindowPage.ets` 增加 `MOVE_TASK -> LAUNCH_APP` 自动降级及失败态重试/关闭提示层。
    - 真机实测：同时运行 Play 商店（`slot=1, 1782x1770`）、64 位 Chrome（`slot=2, 2116x1582`）及由 Play 商店跨应用拉起的 Instagram（`slot=3, 1462x1522`），`dumpsys SurfaceFlinger` 验证 3 个 VirtualDisplay `layerStack=2/3/4` 输出图层 100% 独立零串画，三屏均持续输出帧且 `Fatal signal = 0`。
- **2026-11-27 WSA 风格鸿蒙共享输入法 + 传统模拟器等比锁定 / 旋转 / 标题栏响应式折叠**：
  - **WSA 风格直接共用鸿蒙系统输入法（零底部独立桥接输入框）**：
    - Android 侧新增无界面系统 IME `ime_bridge/SvboxHostIme.apk`（纯 Python AXML + `resources.arsc` + APK Signature Scheme v2 签名，零修改只读 `rootfs14/`，运行期注入 `injectLayer_/system/app/SvboxHostIme/SvboxHostIme.apk`），通过抽象 Unix Socket `@svbox_ime` 与 `SvboxWmAgent` 保持长连接。
    - 实测踩坑 1：Android 14 `InputMethodManagerService.onUnlockUser(0)` 解锁时会重载 `settings_secure.xml` 覆盖早期启动时设置的默认输入法回退至 `LatinIME`；在 `SvboxWmAgent.java` 增加 `ensureHostImeActiveLoop()` 于用户解锁并 `sys.boot_completed=1` 后再次确认启用/选中 `org.svbox.ime/.SvboxHostImeService`，确保常驻生效。
    - 实测踩坑 2：HarmonyOS `@kit.IMEKit` `InputMethodController` 要求 `setPreviewText` / `finishTextPreview` 必须在 `attach()` **之前**注册，而 `insertText` / `deleteLeft` / `deleteRight` / `sendFunctionKey` / `moveCursor` / `handleExtendAction` 必须在 `attach()` **之后**注册（否则报 `12800009: input method client detached. need to be attached first`）。`HostImeController.ets` 拆分 `bindPreAttachListeners()` / `bindPostAttachListeners()` 并在 `InputBridge::ShouldSuppressKeyForHostIme()` + `DispatchKeyEventCB` 中于宿主 IME 接管时返回 `false` 交由鸿蒙 IMF 消费，彻底杜绝中英双打与游戏键位误触发；同时将 Android `CursorAnchorInfo` 换算为鸿蒙物理屏幕坐标，令小艺拼音候选浮窗精准贴合 Android 输入框正下方。
  - **传统模拟器等比例缩放锁定 + 屏幕横竖屏旋转 + 标题栏防裁剪溢出折叠**：
    - `EmulatorWindowAbility.ets` 与 `EmulatorWindowPage.ets` 先执行 `applyImmersiveDecor(win, 40)` 再调用 `win.setContentAspectRatio(ratio, false, false)` 后 `win.resize(targetW, targetH)`（规避 `needUpdateRect=true` 覆盖目标宽度的系统行为），竖屏精确锁定 `760×1427`、横屏精确锁定 `1480×909`，零黑边且禁止自由拉伸宽高比。
    - 新增 Display 0 横竖屏旋转（`SvboxWmAgent` `SET_DISPLAY_ROTATION` 通过 `IWindowManager.setFixedToUserRotation(0, 2)` + `freezeDisplayRotation(0, rot)` 驱动 Android 旋转，ArkUI `XComponent` 使用 `.rotate({ angle: -90 })` 配合 `svbox-touchscreen.idc` `orientationAware=1` 实现零闪烁硬件旋转与像素级精准触控，同时 `InputBridge::SetDisplay0Rotation()` 同步旋转归一化键鼠映射层坐标）。
    - `EmulatorWindowPage.ets` 与 `AppWindowPage.ets` 标题栏通过 `.onAreaChange` 实时测量 `barVpW` 并为图标添加 `.flexShrink(0)`，窄屏时自动将 FPS 徽章、多任务、输入法、拆出、键鼠映射、宏录制按优先级收入 `更多 ⋮` 菜单，宽屏自动展开，彻底消除左侧图标裁剪。
  - **真机实测（`HAD-W32`）**：冷启动 `Fatal signal = 0`；竖屏 `760×1427` 零裁剪零黑边 -> 点击旋转切换横屏 `1480×909`（`105/120Hz` + 完整工具栏展开 + 点击精准启动 WebView）-> 点击 `about:blank` 输入框自动触发 `Host IME attached`（零警告）并弹出原生小艺拼音候选框 -> 实测物理键盘 `nihao`+空格上屏 `你好`、`shijie`+空格+退格变为 `你好世` -> 回主页自动 `Host IME detached` -> 旋转回竖屏 `760×1427` 完整闭环。






















