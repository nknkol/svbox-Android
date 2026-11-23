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

