import { LengthUnit } from '@kit.ArkUI';
import native, { RuntimeStatus } from 'libentry.so';
import {
  computeDpiForSize,
  REFRESH_RATES,
  RESOLUTION_PRESETS,
  ResolutionPresetItem
} from '../common/AppWindowPrefs';
import {
  cloneInstanceRecord,
  cpuCoresToProfile,
  DEVICE_MODEL_PRESETS,
  DeviceInstance,
  DeviceModelPreset,
  formatDiskSize,
  formatGpuModeLabel,
  profileToCpuCores
} from '../model/InstanceModel';

interface SettingsNavTab {
  key: string;
  label: string;
  icon: Resource;
}

interface ShortcutRow {
  action: string;
  keys: string;
}

const SHORTCUT_ITEMS: ShortcutRow[] = [
  { action: '返回上一级（Android Back）', keys: 'Esc / 窗口左上角返回' },
  { action: '回到桌面（Android Home）', keys: '标题栏主页按钮' },
  { action: '切换多任务（Recents）', keys: '标题栏多任务按钮' },
  { action: '横竖屏切换', keys: '标题栏旋转按钮' },
  { action: '鼠标滚轮滚动页面', keys: '滚轮上下滚动' },
  { action: '将当前应用拆出为独立窗口', keys: '模拟器顶栏「拆出」按钮' },
];

@CustomDialog
export struct DeviceSettingsDialog {
  controller: CustomDialogController;
  instance: DeviceInstance | undefined = undefined;
  filesDir: string = '';
  isInstanceRunning: boolean = false;
  onSave: (updated: DeviceInstance) => void = (): void => {};
  onToast: (msg: string) => void = (): void => {};

  @State private activeTab: string = 'perf';
  @State private draftProfile: string = 'Balanced';
  @State private draftCpuCores: number = 4;
  @State private draftMemMb: number = 4096;
  @State private draftGpuMode: string = 'vulkan';
  @State private draftRefreshRate: number = 120;
  @State private draftRunMode: string = 'emulator';
  @State private draftDeviceType: string = 'phone';
  @State private draftResKey: string = '720x1280';
  @State private draftWidth: number = 720;
  @State private draftHeight: number = 1280;
  @State private draftDpi: number = 320;
  @State private draftEnableAudio: boolean = true;
  @State private draftEnableRoot: boolean = true;
  @State private draftKeepAlive: boolean = true;
  @State private draftPreventSleep: boolean = false;
  @State private draftAutoGamepad: boolean = true;
  @State private draftCompactDisk: boolean = false;
  @State private draftBrand: string = '';
  @State private draftModel: string = '';
  @State private draftAdbPort: number = 5555;

  @State private diskUsageMb: number = 0;
  @State private shareDir: string = '/storage/Users/currentUser/Download/svbox-share';
  @State private cleaningDisk: boolean = false;
  @State private diagStatus: RuntimeStatus | undefined = undefined;

  private readonly mainTabs: SettingsNavTab[] = [
    { key: 'perf', label: '性能', icon: $r('sys.symbol.bolt_fill') },
    { key: 'display', label: '显示', icon: $r('sys.symbol.display') },
    { key: 'audio', label: '声音', icon: $r('sys.symbol.speaker_wave_2') },
    { key: 'disk', label: '磁盘', icon: $r('sys.symbol.externaldrive') },
    { key: 'network', label: '网络', icon: $r('sys.symbol.wifi') },
    { key: 'model', label: '机型', icon: $r('sys.symbol.phone_1') },
    { key: 'shortcuts', label: '快捷键', icon: $r('sys.symbol.keyboard') },
    { key: 'other', label: '其他', icon: $r('sys.symbol.slider_horizontal_2') },
  ];

  aboutToAppear(): void {
    if (this.instance) {
      this.draftProfile = this.instance.profile;
      this.draftCpuCores = this.instance.cpuCores;
      this.draftMemMb = this.instance.memLimitMb;
      this.draftGpuMode = this.instance.gpuMode;
      this.draftRefreshRate = this.instance.refreshRate;
      this.draftRunMode = this.instance.runMode;
      this.draftDeviceType = this.instance.deviceType;
      this.draftResKey = this.instance.resolutionKey;
      this.draftWidth = this.instance.width;
      this.draftHeight = this.instance.height;
      this.draftDpi = this.instance.dpi;
      this.draftEnableAudio = this.instance.enableAudio;
      this.draftEnableRoot = this.instance.enableRoot;
      this.draftKeepAlive = this.instance.keepAlive;
      this.draftPreventSleep = this.instance.preventSleep;
      this.draftAutoGamepad = this.instance.autoGamepad;
      this.draftCompactDisk = this.instance.compactDisk;
      this.draftBrand = this.instance.deviceBrand;
      this.draftModel = this.instance.deviceModel;
      this.draftAdbPort = this.instance.adbPort;
    }
    this.refreshDiagnosticsAndDisk();
  }

  private refreshDiagnosticsAndDisk(): void {
    try {
      const st: RuntimeStatus = native.getStatus();
      this.diagStatus = st;
      if (st.shareDir && st.shareDir.length > 0) {
        this.shareDir = st.shareDir;
      }
      if (this.instance && this.filesDir.length > 0) {
        this.diskUsageMb = native.getInstanceDiskMb(this.filesDir, this.instance.dataSubdir);
      }
    } catch (_e) {
    }
  }

  private resetToDefaults(): void {
    this.draftProfile = 'Balanced';
    this.draftCpuCores = 4;
    this.draftMemMb = 4096;
    this.draftGpuMode = 'vulkan';
    this.draftRefreshRate = 120;
    this.draftRunMode = 'emulator';
    this.draftDeviceType = 'phone';
    this.draftResKey = '720x1280';
    this.draftWidth = 720;
    this.draftHeight = 1280;
    this.draftDpi = 320;
    this.draftEnableAudio = true;
    this.draftEnableRoot = true;
    this.draftKeepAlive = true;
    this.draftPreventSleep = false;
    this.draftAutoGamepad = true;
    this.draftBrand = '';
    this.draftModel = '';
    this.draftAdbPort = 5555;
    this.onToast('已恢复默认配置，点击保存后生效');
  }

  private handleSave(): void {
    if (!this.instance) {
      this.controller.close();
      return;
    }
    const next: DeviceInstance = cloneInstanceRecord(this.instance);
    next.profile = this.draftProfile;
    next.cpuCores = this.draftCpuCores;
    next.memLimitMb = this.draftMemMb;
    next.gpuMode = this.draftGpuMode;
    next.refreshRate = this.draftRefreshRate;
    next.runMode = this.draftRunMode;
    next.deviceType = this.draftDeviceType;
    next.resolutionKey = this.draftResKey;
    next.width = this.draftWidth;
    next.height = this.draftHeight;
    next.dpi = this.draftDpi;
    next.enableAudio = this.draftEnableAudio;
    next.enableRoot = this.draftEnableRoot;
    next.keepAlive = this.draftKeepAlive;
    next.preventSleep = this.draftPreventSleep;
    next.autoGamepad = this.draftAutoGamepad;
    next.compactDisk = this.draftCompactDisk;
    next.deviceBrand = this.draftBrand.trim();
    next.deviceModel = this.draftModel.trim();
    next.adbPort = this.draftAdbPort;
    this.onSave(next);
    this.controller.close();
  }

  @Builder
  private optionPill(label: string, active: boolean, onTap: () => void) {
    Button({ type: ButtonType.Normal }) {
      Text(label)
        .fontSize(12)
        .fontWeight(active ? FontWeight.Medium : FontWeight.Normal)
        .fontColor(active ? $r('sys.color.font_on_primary') : $r('sys.color.font_primary'))
    }
    .height(30)
    .padding({ left: 14, right: 14 })
    .borderRadius(15)
    .backgroundColor(active ? $r('sys.color.brand') : $r('sys.color.comp_background_tertiary'))
    .onClick(onTap)
  }

  @Builder
  private toggleRow(title: string, desc: string, checked: boolean, onToggle: (v: boolean) => void) {
    Row({ space: 12 }) {
      Column({ space: 2 }) {
        Text(title)
          .fontSize(14)
          .fontWeight(FontWeight.Medium)
          .fontColor($r('sys.color.font_primary'))
        Text(desc)
          .fontSize(11)
          .fontColor($r('sys.color.font_tertiary'))
      }
      .alignItems(HorizontalAlign.Start)
      .layoutWeight(1)

      Toggle({ type: ToggleType.Switch, isOn: checked })
        .selectedColor($r('sys.color.brand'))
        .onChange(onToggle)
    }
    .width('100%')
    .padding({ top: 4, bottom: 4 })
  }

  @Builder
  private perfTabBody() {
    Column({ space: 14 }) {
      if (this.isInstanceRunning) {
        Row({ space: 8 }) {
          SymbolGlyph($r('sys.symbol.info_circle'))
            .fontSize(15)
            .fontColor([$r('sys.color.icon_emphasize')])
          Text('设备运行中，部分 CPU/内存/渲染设置将在重启设备后生效')
            .fontSize(12)
            .fontColor($r('sys.color.font_emphasize'))
            .layoutWeight(1)
        }
        .width('100%')
        .padding({ left: 12, right: 12, top: 10, bottom: 10 })
        .borderRadius(12)
        .backgroundColor($r('sys.color.comp_emphasize_tertiary'))
      }

      // Performance Preset
      Column({ space: 10 }) {
        Text('性能方案')
          .fontSize(13)
          .fontWeight(FontWeight.Medium)
          .fontColor($r('sys.color.font_secondary'))
        Row({ space: 8 }) {
          this.optionPill('性能模式', this.draftProfile === 'Performance', () => {
            this.draftProfile = 'Performance';
            this.draftCpuCores = profileToCpuCores('Performance');
          })
          this.optionPill('均衡模式（推荐）', this.draftProfile === 'Balanced', () => {
            this.draftProfile = 'Balanced';
            this.draftCpuCores = profileToCpuCores('Balanced');
          })
          this.optionPill('节能模式', this.draftProfile === 'Eco', () => {
            this.draftProfile = 'Eco';
            this.draftCpuCores = profileToCpuCores('Eco');
          })
        }
      }
      .width('100%')
      .padding(14)
      .borderRadius(14)
      .backgroundColor($r('sys.color.background_secondary'))
      .alignItems(HorizontalAlign.Start)

      // CPU & RAM
      Column({ space: 12 }) {
        Text('计算资源分配')
          .fontSize(13)
          .fontWeight(FontWeight.Medium)
          .fontColor($r('sys.color.font_secondary'))

        Row({ space: 10 }) {
          Text('CPU 核数')
            .fontSize(13)
            .fontColor($r('sys.color.font_primary'))
            .width(72)
          Row({ space: 8 }) {
            this.optionPill('2 核', this.draftCpuCores === 2, () => {
              this.draftCpuCores = 2;
              this.draftProfile = cpuCoresToProfile(2);
            })
            this.optionPill('4 核', this.draftCpuCores === 4, () => {
              this.draftCpuCores = 4;
              this.draftProfile = cpuCoresToProfile(4);
            })
            this.optionPill('6 核', this.draftCpuCores === 6, () => {
              this.draftCpuCores = 6;
              this.draftProfile = cpuCoresToProfile(6);
            })
            this.optionPill('8 核', this.draftCpuCores === 8, () => {
              this.draftCpuCores = 8;
              this.draftProfile = cpuCoresToProfile(8);
            })
          }
        }
        .width('100%')
        .alignItems(VerticalAlign.Center)

        Row({ space: 10 }) {
          Text('内存上限')
            .fontSize(13)
            .fontColor($r('sys.color.font_primary'))
            .width(72)
          Row({ space: 8 }) {
            this.optionPill('2 GB', this.draftMemMb === 2048, () => { this.draftMemMb = 2048; })
            this.optionPill('4 GB', this.draftMemMb === 4096, () => { this.draftMemMb = 4096; })
            this.optionPill('6 GB', this.draftMemMb === 6144, () => { this.draftMemMb = 6144; })
            this.optionPill('8 GB', this.draftMemMb === 8192, () => { this.draftMemMb = 8192; })
          }
        }
        .width('100%')
        .alignItems(VerticalAlign.Center)
      }
      .width('100%')
      .padding(14)
      .borderRadius(14)
      .backgroundColor($r('sys.color.background_secondary'))
      .alignItems(HorizontalAlign.Start)

      // GPU & Refresh Rate
      Column({ space: 12 }) {
        Text('图形渲染与高刷引擎')
          .fontSize(13)
          .fontWeight(FontWeight.Medium)
          .fontColor($r('sys.color.font_secondary'))

        Row({ space: 10 }) {
          Text('显卡渲染')
            .fontSize(13)
            .fontColor($r('sys.color.font_primary'))
            .width(72)
          Row({ space: 8 }) {
            this.optionPill('Vulkan（推荐）', this.draftGpuMode === 'vulkan', () => { this.draftGpuMode = 'vulkan'; })
            this.optionPill('OpenGL ES', this.draftGpuMode === 'gles', () => { this.draftGpuMode = 'gles'; })
            this.optionPill('CPU 软渲', this.draftGpuMode === 'swiftshader', () => { this.draftGpuMode = 'swiftshader'; })
          }
        }
        .width('100%')
        .alignItems(VerticalAlign.Center)

        Row({ space: 10 }) {
          Text('屏幕帧率')
            .fontSize(13)
            .fontColor($r('sys.color.font_primary'))
            .width(72)
          Row({ space: 8 }) {
            ForEach(
              REFRESH_RATES,
              (hz: number) => {
                this.optionPill(`${hz} Hz`, this.draftRefreshRate === hz, () => { this.draftRefreshRate = hz; })
              },
              (hz: number): string => String(hz)
            )
          }
        }
        .width('100%')
        .alignItems(VerticalAlign.Center)

        Text('Vulkan 模式采用宿主 Maleoon 硬件直通与异步提交管线，可流畅运行 120Hz 高帧率应用')
          .fontSize(11)
          .fontColor($r('sys.color.font_tertiary'))
      }
      .width('100%')
      .padding(14)
      .borderRadius(14)
      .backgroundColor($r('sys.color.background_secondary'))
      .alignItems(HorizontalAlign.Start)
    }
    .width('100%')
  }

  @Builder
  private displayTabBody() {
    Column({ space: 14 }) {
      Column({ space: 10 }) {
        Text('窗口运行模式')
          .fontSize(13)
          .fontWeight(FontWeight.Medium)
          .fontColor($r('sys.color.font_secondary'))
        Row({ space: 8 }) {
          this.optionPill('传统模拟器窗口', this.draftRunMode === 'emulator', () => { this.draftRunMode = 'emulator'; })
          this.optionPill('自由多窗口模式', this.draftRunMode === 'multiwindow', () => { this.draftRunMode = 'multiwindow'; })
        }
        Text(
          this.draftRunMode === 'emulator'
            ? '启动后直接打开独立完整 Android 桌面模拟器窗口，并支持随时将前台应用拆出'
            : '无桌面容器背景运行，通过「应用中心」以独立鸿蒙原生窗口启动各个 Android 应用'
        )
          .fontSize(11)
          .fontColor($r('sys.color.font_tertiary'))
      }
      .width('100%')
      .padding(14)
      .borderRadius(14)
      .backgroundColor($r('sys.color.background_secondary'))
      .alignItems(HorizontalAlign.Start)

      Column({ space: 10 }) {
        Text('分辨率预设')
          .fontSize(13)
          .fontWeight(FontWeight.Medium)
          .fontColor($r('sys.color.font_secondary'))

        Flex({ wrap: FlexWrap.Wrap, space: { main: { value: 8, unit: LengthUnit.VP }, cross: { value: 8, unit: LengthUnit.VP } } }) {
          ForEach(
            RESOLUTION_PRESETS,
            (item: ResolutionPresetItem) => {
              this.optionPill(item.label, this.draftResKey === item.key, () => {
                this.draftResKey = item.key;
                this.draftWidth = item.w;
                this.draftHeight = item.h;
                this.draftDpi = computeDpiForSize(item.w, item.h);
                this.draftDeviceType = item.w > item.h ? 'tablet' : 'phone';
              })
            },
            (item: ResolutionPresetItem): string => item.key
          )
        }
        .width('100%')
      }
      .width('100%')
      .padding(14)
      .borderRadius(14)
      .backgroundColor($r('sys.color.background_secondary'))
      .alignItems(HorizontalAlign.Start)

      Column({ space: 10 }) {
        Text('像素密度（DPI）')
          .fontSize(13)
          .fontWeight(FontWeight.Medium)
          .fontColor($r('sys.color.font_secondary'))
        Row({ space: 8 }) {
          this.optionPill('240 DPI', this.draftDpi === 240, () => { this.draftDpi = 240; })
          this.optionPill('280 DPI', this.draftDpi === 280, () => { this.draftDpi = 280; })
          this.optionPill('320 DPI', this.draftDpi === 320, () => { this.draftDpi = 320; })
          this.optionPill('420 DPI', this.draftDpi === 420, () => { this.draftDpi = 420; })
        }
        Text(`当前画布：${this.draftWidth} × ${this.draftHeight} · ${this.draftDpi} DPI`)
          .fontSize(11)
          .fontColor($r('sys.color.font_tertiary'))
      }
      .width('100%')
      .padding(14)
      .borderRadius(14)
      .backgroundColor($r('sys.color.background_secondary'))
      .alignItems(HorizontalAlign.Start)
    }
    .width('100%')
  }

  @Builder
  private audioTabBody() {
    Column({ space: 14 }) {
      Column({ space: 10 }) {
        this.toggleRow(
          '启用 OHAudio 宿主音频桥接',
          '将 Android 14 音频流低延迟直通至鸿蒙 PC 默认输出设备',
          this.draftEnableAudio,
          (v: boolean) => { this.draftEnableAudio = v; }
        )
      }
      .width('100%')
      .padding(14)
      .borderRadius(14)
      .backgroundColor($r('sys.color.background_secondary'))

      Column({ space: 6 }) {
        Text('音频架构信息')
          .fontSize(13)
          .fontWeight(FontWeight.Medium)
          .fontColor($r('sys.color.font_secondary'))
        Text('输出管线：audio.primary.svbox.so → IPC Socket → OH_AudioRenderer (48kHz Stereo)')
          .fontSize(12)
          .fontColor($r('sys.color.font_primary'))
        Text(this.diagStatus && this.diagStatus.audioActive ? '当前音频状态：活跃播放中' : '当前音频状态：待机就绪')
          .fontSize(11)
          .fontColor($r('sys.color.font_tertiary'))
      }
      .width('100%')
      .padding(14)
      .borderRadius(14)
      .backgroundColor($r('sys.color.background_secondary'))
      .alignItems(HorizontalAlign.Start)
    }
    .width('100%')
  }

  @Builder
  private diskTabBody() {
    Column({ space: 14 }) {
      // Disk Usage & Cleanup
      Column({ space: 12 }) {
        Row() {
          Column({ space: 3 }) {
            Text('磁盘空间占用')
              .fontSize(14)
              .fontWeight(FontWeight.Medium)
              .fontColor($r('sys.color.font_primary'))
            Text(`隔离目录：${this.instance ? this.instance.dataSubdir : 'userdata14'}`)
              .fontSize(11)
              .fontColor($r('sys.color.font_tertiary'))
          }
          .alignItems(HorizontalAlign.Start)
          .layoutWeight(1)

          Text(formatDiskSize(this.diskUsageMb))
            .fontSize(18)
            .fontWeight(FontWeight.Bold)
            .fontColor($r('sys.color.font_emphasize'))
        }
        .width('100%')

        Divider().color($r('sys.color.comp_divider')).opacity(0.35)

        Row({ space: 10 }) {
          Column({ space: 2 }) {
            Text('磁盘缓存清理')
              .fontSize(13)
              .fontWeight(FontWeight.Medium)
              .fontColor($r('sys.color.font_primary'))
            Text('安全清理应用缓存、崩溃日志与 ANR 临时文件，保留已安装应用与账号数据')
              .fontSize(11)
              .fontColor($r('sys.color.font_tertiary'))
          }
          .alignItems(HorizontalAlign.Start)
          .layoutWeight(1)

          Button(this.cleaningDisk ? '清理中...' : '立即清理', { type: ButtonType.Capsule })
            .height(32)
            .padding({ left: 16, right: 16 })
            .fontSize(12)
            .fontColor($r('sys.color.font_on_primary'))
            .backgroundColor($r('sys.color.brand'))
            .enabled(!this.cleaningDisk)
            .onClick(() => {
              if (!this.instance) return;
              this.cleaningDisk = true;
              try {
                const freed: number = native.cleanInstanceCache(this.filesDir, this.instance.dataSubdir);
                this.diskUsageMb = native.getInstanceDiskMb(this.filesDir, this.instance.dataSubdir);
                this.onToast(`磁盘清理完成，释放 ${freed.toFixed(1)} MB`);
              } catch (e) {
                this.onToast(`清理失败：${String(e)}`);
              } finally {
                this.cleaningDisk = false;
              }
            })
        }
        .width('100%')
      }
      .width('100%')
      .padding(14)
      .borderRadius(14)
      .backgroundColor($r('sys.color.background_secondary'))

      // Shared Folder
      Column({ space: 8 }) {
        Text('宿主机共享文件夹')
          .fontSize(13)
          .fontWeight(FontWeight.Medium)
          .fontColor($r('sys.color.font_secondary'))
        Row({ space: 10 }) {
          Text(this.shareDir)
            .fontSize(12)
            .fontColor($r('sys.color.font_primary'))
            .layoutWeight(1)
            .maxLines(1)
            .textOverflow({ overflow: TextOverflow.Ellipsis })
          Button('检查目录', { type: ButtonType.Normal })
            .height(30)
            .padding({ left: 12, right: 12 })
            .borderRadius(8)
            .fontSize(12)
            .fontColor($r('sys.color.font_primary'))
            .backgroundColor($r('sys.color.comp_background_tertiary'))
            .onClick(() => {
              const ok: boolean = native.ensureDir(this.shareDir);
              this.onToast(ok ? `共享目录已就绪：${this.shareDir}` : '创建共享目录失败');
            })
        }
        .width('100%')
        Text('对应 Android 内部路径：/sdcard/Download/svbox-share')
          .fontSize(11)
          .fontColor($r('sys.color.font_tertiary'))
      }
      .width('100%')
      .padding(14)
      .borderRadius(14)
      .backgroundColor($r('sys.color.background_secondary'))
      .alignItems(HorizontalAlign.Start)

      // Compact mode
      Column() {
        this.toggleRow(
          '精简磁盘缓存模式',
          '限制系统临时缓存增长，适合多实例长期保存',
          this.draftCompactDisk,
          (v: boolean) => { this.draftCompactDisk = v; }
        )
      }
      .width('100%')
      .padding(14)
      .borderRadius(14)
      .backgroundColor($r('sys.color.background_secondary'))
    }
    .width('100%')
  }

  @Builder
  private networkTabBody() {
    Column({ space: 14 }) {
      Column({ space: 10 }) {
        Text('网络桥接模式')
          .fontSize(13)
          .fontWeight(FontWeight.Medium)
          .fontColor($r('sys.color.font_secondary'))
        Row({ space: 8 }) {
          SymbolGlyph($r('sys.symbol.wifi'))
            .fontSize(16)
            .fontColor([$r('sys.color.confirm')])
          Text('宿主机共享网络（NAT + 内置 DNS 代理 223.5.5.5 / 8.8.8.8）')
            .fontSize(13)
            .fontColor($r('sys.color.font_primary'))
        }
        Text('自动复用鸿蒙 PC 的 WLAN / 有线网络与代理配置，无需额外虚拟网卡驱动')
          .fontSize(11)
          .fontColor($r('sys.color.font_tertiary'))
      }
      .width('100%')
      .padding(14)
      .borderRadius(14)
      .backgroundColor($r('sys.color.background_secondary'))
      .alignItems(HorizontalAlign.Start)

      Column({ space: 10 }) {
        Text('ADB 调试端口')
          .fontSize(13)
          .fontWeight(FontWeight.Medium)
          .fontColor($r('sys.color.font_secondary'))
        Row({ space: 8 }) {
          this.optionPill('5555（默认）', this.draftAdbPort === 5555, () => { this.draftAdbPort = 5555; })
          this.optionPill('5557', this.draftAdbPort === 5557, () => { this.draftAdbPort = 5557; })
          this.optionPill('5559', this.draftAdbPort === 5559, () => { this.draftAdbPort = 5559; })
          this.optionPill('16384', this.draftAdbPort === 16384, () => { this.draftAdbPort = 16384; })
        }
        Text(`连接命令：adb connect 127.0.0.1:${this.draftAdbPort}`)
          .fontSize(12)
          .fontColor($r('sys.color.font_emphasize'))
      }
      .width('100%')
      .padding(14)
      .borderRadius(14)
      .backgroundColor($r('sys.color.background_secondary'))
      .alignItems(HorizontalAlign.Start)
    }
    .width('100%')
  }

  @Builder
  private modelTabBody() {
    Column({ space: 14 }) {
      Column({ space: 10 }) {
        Text('常用机型预设')
          .fontSize(13)
          .fontWeight(FontWeight.Medium)
          .fontColor($r('sys.color.font_secondary'))

        Flex({ wrap: FlexWrap.Wrap, space: { main: { value: 8, unit: LengthUnit.VP }, cross: { value: 8, unit: LengthUnit.VP } } }) {
          ForEach(
            DEVICE_MODEL_PRESETS,
            (preset: DeviceModelPreset) => {
              this.optionPill(
                preset.label,
                this.draftBrand === preset.brand && this.draftModel === preset.model,
                () => {
                  this.draftBrand = preset.brand;
                  this.draftModel = preset.model;
                }
              )
            },
            (preset: DeviceModelPreset): string => preset.label
          )
        }
        .width('100%')
      }
      .width('100%')
      .padding(14)
      .borderRadius(14)
      .backgroundColor($r('sys.color.background_secondary'))
      .alignItems(HorizontalAlign.Start)

      Column({ space: 12 }) {
        Text('自定义机型属性（ro.product.*）')
          .fontSize(13)
          .fontWeight(FontWeight.Medium)
          .fontColor($r('sys.color.font_secondary'))

        Row({ space: 10 }) {
          Text('品牌')
            .fontSize(13)
            .fontColor($r('sys.color.font_primary'))
            .width(56)
          TextInput({ placeholder: '留空使用默认，例如 Xiaomi / Google', text: this.draftBrand })
            .layoutWeight(1)
            .height(36)
            .fontSize(13)
            .fontColor($r('sys.color.font_primary'))
            .backgroundColor($r('sys.color.comp_background_primary'))
            .borderRadius(10)
            .onChange((v: string) => { this.draftBrand = v; })
        }
        .width('100%')

        Row({ space: 10 }) {
          Text('型号')
            .fontSize(13)
            .fontColor($r('sys.color.font_primary'))
            .width(56)
          TextInput({ placeholder: '留空使用默认，例如 Pixel 8 Pro', text: this.draftModel })
            .layoutWeight(1)
            .height(36)
            .fontSize(13)
            .fontColor($r('sys.color.font_primary'))
            .backgroundColor($r('sys.color.comp_background_primary'))
            .borderRadius(10)
            .onChange((v: string) => { this.draftModel = v; })
        }
        .width('100%')
      }
      .width('100%')
      .padding(14)
      .borderRadius(14)
      .backgroundColor($r('sys.color.background_secondary'))
      .alignItems(HorizontalAlign.Start)
    }
    .width('100%')
  }

  @Builder
  private shortcutsTabBody() {
    Column({ space: 8 }) {
      Text('常用键鼠操作与快捷键')
        .fontSize(13)
        .fontWeight(FontWeight.Medium)
        .fontColor($r('sys.color.font_secondary'))
        .margin({ bottom: 4 })

      ForEach(
        SHORTCUT_ITEMS,
        (row: ShortcutRow) => {
          Row() {
            Text(row.action)
              .fontSize(13)
              .fontColor($r('sys.color.font_primary'))
              .layoutWeight(1)
            Text(row.keys)
              .fontSize(12)
              .fontColor($r('sys.color.font_emphasize'))
              .padding({ left: 10, right: 10, top: 4, bottom: 4 })
              .borderRadius(8)
              .backgroundColor($r('sys.color.comp_emphasize_tertiary'))
          }
          .width('100%')
          .padding({ left: 14, right: 14, top: 10, bottom: 10 })
          .borderRadius(12)
          .backgroundColor($r('sys.color.background_secondary'))
        },
        (row: ShortcutRow): string => row.action
      )
    }
    .width('100%')
    .alignItems(HorizontalAlign.Start)
  }

  @Builder
  private otherTabBody() {
    Column({ space: 12 }) {
      Column({ space: 8 }) {
        this.toggleRow(
          '开启 ADB Root 权限',
          '允许通过 adb shell 以 root 身份调试容器环境',
          this.draftEnableRoot,
          (v: boolean) => { this.draftEnableRoot = v; }
        )
        Divider().color($r('sys.color.comp_divider')).opacity(0.35)
        this.toggleRow(
          '后台保活运行',
          '最小化窗口时保持 Android 实例与推送连接持续在线',
          this.draftKeepAlive,
          (v: boolean) => { this.draftKeepAlive = v; }
        )
        Divider().color($r('sys.color.comp_divider')).opacity(0.35)
        this.toggleRow(
          '运行时自动连接游戏手柄',
          '检测到鸿蒙 GameControllerKit / USB 手柄时自动透传至设备',
          this.draftAutoGamepad,
          (v: boolean) => { this.draftAutoGamepad = v; }
        )
        Divider().color($r('sys.color.comp_divider')).opacity(0.35)
        this.toggleRow(
          '设备运行时防止电脑休眠',
          '适合挂机下载或长时间调试任务',
          this.draftPreventSleep,
          (v: boolean) => { this.draftPreventSleep = v; }
        )
      }
      .width('100%')
      .padding(14)
      .borderRadius(14)
      .backgroundColor($r('sys.color.background_secondary'))
    }
    .width('100%')
  }

  @Builder
  private diagRow(label: string, textVal: string) {
    Row() {
      Text(label)
        .fontSize(12)
        .fontColor($r('sys.color.font_secondary'))
        .width(120)
      Text(textVal)
        .fontSize(12)
        .fontWeight(FontWeight.Medium)
        .fontColor($r('sys.color.font_primary'))
        .layoutWeight(1)
        .maxLines(2)
        .textOverflow({ overflow: TextOverflow.Ellipsis })
    }
    .width('100%')
    .padding({ top: 5, bottom: 5 })
  }

  @Builder
  private diagTabBody() {
    Column({ space: 12 }) {
      Row() {
        Text('实时容器与宿主诊断')
          .fontSize(14)
          .fontWeight(FontWeight.Medium)
          .fontColor($r('sys.color.font_primary'))
        Blank()
        Button('刷新诊断', { type: ButtonType.Normal })
          .height(28)
          .padding({ left: 12, right: 12 })
          .borderRadius(8)
          .fontSize(12)
          .fontColor($r('sys.color.font_emphasize'))
          .backgroundColor($r('sys.color.comp_emphasize_tertiary'))
          .onClick(() => {
            this.refreshDiagnosticsAndDisk();
          })
      }
      .width('100%')

      Column({ space: 4 }) {
        this.diagRow('容器状态', this.diagStatus && this.diagStatus.running ? `运行中 (${this.diagStatus.statusText})` : '未开机')
        Divider().color($r('sys.color.comp_divider')).opacity(0.3)
        this.diagRow('渲染后端', this.diagStatus ? `${formatGpuModeLabel(this.diagStatus.gpuMode)} · ${this.diagStatus.gpuRenderer}` : 'Maleoon Vulkan')
        Divider().color($r('sys.color.comp_divider')).opacity(0.3)
        this.diagRow('显示画布', this.diagStatus ? `${this.diagStatus.width}×${this.diagStatus.height} @ ${this.diagStatus.fps}/${this.diagStatus.refreshRate}Hz` : '-')
        Divider().color($r('sys.color.comp_divider')).opacity(0.3)
        this.diagRow('宿主 CPU 拓扑', this.diagStatus && this.diagStatus.topology.length > 0 ? this.diagStatus.topology : 'ARM64 多核')
        Divider().color($r('sys.color.comp_divider')).opacity(0.3)
        this.diagRow('CPU / 内存占用', this.diagStatus ? `CPU ${Math.round(this.diagStatus.cpuPercent)}% · ${this.diagStatus.memMb} / ${this.diagStatus.memLimitMb} MB` : '-')
        Divider().color($r('sys.color.comp_divider')).opacity(0.3)
        this.diagRow('活跃进程数', this.diagStatus ? `${this.diagStatus.activeProcs} 个容器进程` : '0')
        Divider().color($r('sys.color.comp_divider')).opacity(0.3)
        this.diagRow('窗口代理 WM Agent', this.diagStatus && this.diagStatus.wmAgentReady ? '已连接（支持自由多窗）' : '未就绪')
        Divider().color($r('sys.color.comp_divider')).opacity(0.3)
        this.diagRow('当前实例磁盘', formatDiskSize(this.diskUsageMb))
      }
      .width('100%')
      .padding(14)
      .borderRadius(14)
      .backgroundColor($r('sys.color.background_secondary'))
    }
    .width('100%')
  }

  build() {
    Column({ space: 0 }) {
      // Split-pane body (MuMu Settings layout + HarmonyOS Master-Detail)
      Row({ space: 0 }) {
        // Left Navigation Sidebar
        Column({ space: 4 }) {
          Column({ space: 2 }) {
            Text('设备设置')
              .fontSize(17)
              .fontWeight(FontWeight.Bold)
              .fontColor($r('sys.color.font_primary'))
            Text(this.instance ? this.instance.name : '')
              .fontSize(11)
              .fontColor($r('sys.color.font_tertiary'))
              .maxLines(1)
              .textOverflow({ overflow: TextOverflow.Ellipsis })
          }
          .width('100%')
          .alignItems(HorizontalAlign.Start)
          .padding({ left: 10, right: 8, top: 6, bottom: 12 })

          ForEach(
            this.mainTabs,
            (tab: SettingsNavTab) => {
              Row({ space: 10 }) {
                SymbolGlyph(tab.icon)
                  .fontSize(16)
                  .fontColor([
                    this.activeTab === tab.key
                      ? $r('sys.color.icon_emphasize')
                      : $r('sys.color.icon_secondary')
                  ])
                Text(tab.label)
                  .fontSize(13)
                  .fontWeight(this.activeTab === tab.key ? FontWeight.Medium : FontWeight.Normal)
                  .fontColor(
                    this.activeTab === tab.key
                      ? $r('sys.color.font_emphasize')
                      : $r('sys.color.font_primary')
                  )
              }
              .width('100%')
              .height(38)
              .padding({ left: 12, right: 12 })
              .borderRadius(10)
              .backgroundColor(
                this.activeTab === tab.key
                  ? $r('sys.color.comp_emphasize_tertiary')
                  : Color.Transparent
              )
              .onClick(() => {
                this.activeTab = tab.key;
                if (tab.key === 'disk') {
                  this.refreshDiagnosticsAndDisk();
                }
              })
            },
            (tab: SettingsNavTab): string => tab.key
          )

          Blank()

          Divider().color($r('sys.color.comp_divider')).opacity(0.4).margin({ top: 6, bottom: 6 })

          Row({ space: 10 }) {
            SymbolGlyph($r('sys.symbol.wrench_and_screwdriver'))
              .fontSize(15)
              .fontColor([
                this.activeTab === 'diag'
                  ? $r('sys.color.icon_emphasize')
                  : $r('sys.color.icon_secondary')
              ])
            Text('设备诊断')
              .fontSize(13)
              .fontWeight(this.activeTab === 'diag' ? FontWeight.Medium : FontWeight.Normal)
              .fontColor(
                this.activeTab === 'diag'
                  ? $r('sys.color.font_emphasize')
                  : $r('sys.color.font_primary')
              )
          }
          .width('100%')
          .height(38)
          .padding({ left: 12, right: 12 })
          .borderRadius(10)
          .backgroundColor(
            this.activeTab === 'diag'
              ? $r('sys.color.comp_emphasize_tertiary')
              : Color.Transparent
          )
          .onClick(() => {
            this.activeTab = 'diag';
            this.refreshDiagnosticsAndDisk();
          })
        }
        .width(172)
        .height('100%')
        .padding({ left: 12, right: 12, top: 14, bottom: 14 })
        .backgroundColor($r('sys.color.background_secondary'))
        .border({ width: { right: 1 }, color: $r('sys.color.comp_divider') })

        // Right Content Area
        Column({ space: 0 }) {
          // Right Top Header with Close Button
          Row() {
            Text(
              this.activeTab === 'perf' ? '性能设置'
                : (this.activeTab === 'display' ? '显示设置'
                : (this.activeTab === 'audio' ? '声音设置'
                : (this.activeTab === 'disk' ? '磁盘与共享目录'
                : (this.activeTab === 'network' ? '网络与 ADB'
                : (this.activeTab === 'model' ? '机型模拟'
                : (this.activeTab === 'shortcuts' ? '快捷键说明'
                : (this.activeTab === 'other' ? '其他设置' : '设备诊断')))))))
            )
              .fontSize(16)
              .fontWeight(FontWeight.Bold)
              .fontColor($r('sys.color.font_primary'))
            Blank()
            Button({ type: ButtonType.Circle }) {
              SymbolGlyph($r('sys.symbol.xmark'))
                .fontSize(14)
                .fontColor([$r('sys.color.icon_secondary')])
            }
            .width(30)
            .height(30)
            .backgroundColor($r('sys.color.comp_background_tertiary'))
            .onClick(() => {
              this.controller.close();
            })
          }
          .width('100%')
          .padding({ left: 22, right: 18, top: 16, bottom: 12 })

          Scroll() {
            Column() {
              if (this.activeTab === 'perf') {
                this.perfTabBody()
              } else if (this.activeTab === 'display') {
                this.displayTabBody()
              } else if (this.activeTab === 'audio') {
                this.audioTabBody()
              } else if (this.activeTab === 'disk') {
                this.diskTabBody()
              } else if (this.activeTab === 'network') {
                this.networkTabBody()
              } else if (this.activeTab === 'model') {
                this.modelTabBody()
              } else if (this.activeTab === 'shortcuts') {
                this.shortcutsTabBody()
              } else if (this.activeTab === 'other') {
                this.otherTabBody()
              } else {
                this.diagTabBody()
              }
            }
            .width('100%')
            .padding({ left: 22, right: 22, top: 4, bottom: 18 })
          }
          .layoutWeight(1)
          .scrollBar(BarState.Auto)

          Divider().color($r('sys.color.comp_divider')).opacity(0.35)

          // Footer Bar
          Row({ space: 10 }) {
            Button('恢复默认', { type: ButtonType.Normal })
              .height(34)
              .padding({ left: 14, right: 14 })
              .borderRadius(17)
              .fontSize(13)
              .fontColor($r('sys.color.font_secondary'))
              .backgroundColor(Color.Transparent)
              .onClick(() => {
                this.resetToDefaults();
              })

            Blank()

            Button('取消', { type: ButtonType.Capsule })
              .height(36)
              .padding({ left: 20, right: 20 })
              .fontSize(13)
              .fontColor($r('sys.color.font_primary'))
              .backgroundColor($r('sys.color.comp_background_tertiary'))
              .onClick(() => {
                this.controller.close();
              })

            Button('保存设置', { type: ButtonType.Capsule })
              .height(36)
              .padding({ left: 22, right: 22 })
              .fontSize(13)
              .fontWeight(FontWeight.Medium)
              .fontColor($r('sys.color.font_on_primary'))
              .backgroundColor($r('sys.color.brand'))
              .onClick(() => {
                this.handleSave();
              })
          }
          .width('100%')
          .padding({ left: 18, right: 22, top: 12, bottom: 14 })
        }
        .layoutWeight(1)
        .height('100%')
        .backgroundColor($r('sys.color.comp_background_primary'))
      }
      .width('100%')
      .height('100%')
    }
    .width(740)
    .height(540)
    .clip(true)
    .backgroundColor($r('sys.color.comp_background_primary'))
    .borderRadius(24)
    .shadow({ radius: 36, color: '#2B000000', offsetY: 10 })
  }
}
