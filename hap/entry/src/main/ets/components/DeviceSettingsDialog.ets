import { LengthUnit } from '@kit.ArkUI';
import native, { RuntimeStatus } from 'libentry.so';
import {
  computeDpiForSize,
  REFRESH_RATES,
  RESOLUTION_PRESETS,
  ResolutionPresetItem
} from '../common/AppWindowPrefs';
import {
  ControlSize,
  FontSize,
  IconSize,
  Motion,
  Radius,
  Spacing
} from '../common/DesignTokens';
import {
  cloneInstanceRecord,
  cpuCoresToProfile,
  DEVICE_MODEL_PRESETS,
  DeviceInstance,
  DeviceModelPreset,
  DISK_SIZE_OPTIONS_GB,
  formatDiskSize,
  formatGpuModeLabel,
  formatSyscallModeLabel,
  normalizeDiskLimitGb,
  normalizeSyscallMode,
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

const NAV_ITEM_H: number = 38;
const NAV_ITEM_GAP: number = 4;

@Component
struct SettingsOptionPill {
  @Prop label: string = '';
  @Prop active: boolean = false;
  onTap: () => void = (): void => {};

  build() {
    Button({ type: ButtonType.Normal }) {
      Text(this.label)
        .fontSize(FontSize.CAPTION)
        .fontWeight(this.active ? FontWeight.Medium : FontWeight.Normal)
        .fontColor(this.active ? $r('sys.color.font_on_primary') : $r('sys.color.font_primary'))
    }
    .height(ControlSize.BTN_SM)
    .padding({ left: Spacing.MD, right: Spacing.MD })
    .borderRadius(Radius.CAPSULE)
    .backgroundColor(this.active ? $r('sys.color.brand') : $r('sys.color.comp_background_tertiary'))
    .animation({ duration: Motion.DUR_MICRO, curve: Motion.STD_CURVE })
    .hoverEffect(HoverEffect.Highlight)
    .clickEffect({ level: ClickEffectLevel.LIGHT, scale: 0.95 })
    .onClick(() => {
      this.onTap();
    })
  }
}

@Component
struct SettingsToggleRow {
  @Prop title: string = '';
  @Prop desc: string = '';
  @Prop checked: boolean = false;
  onToggle: (v: boolean) => void = (_v: boolean): void => {};

  build() {
    Row({ space: Spacing.MD }) {
      Column({ space: Spacing.XS }) {
        Text(this.title)
          .fontSize(FontSize.BODY_LG)
          .fontWeight(FontWeight.Medium)
          .fontColor($r('sys.color.font_primary'))
        Text(this.desc)
          .fontSize(FontSize.MICRO)
          .fontColor($r('sys.color.font_tertiary'))
      }
      .alignItems(HorizontalAlign.Start)
      .layoutWeight(1)

      Toggle({ type: ToggleType.Switch, isOn: this.checked })
        .selectedColor($r('sys.color.brand'))
        .onChange((v: boolean) => {
          this.onToggle(v);
        })
    }
    .width('100%')
    .padding({ top: Spacing.XS, bottom: Spacing.XS })
  }
}

@Component
struct SettingsDiagRow {
  @Prop label: string = '';
  @Prop textVal: string = '';

  build() {
    Row({ space: Spacing.MD }) {
      Text(this.label)
        .fontSize(FontSize.CAPTION)
        .fontColor($r('sys.color.font_secondary'))
        .width(116)
      Text(this.textVal)
        .fontSize(FontSize.CAPTION)
        .fontWeight(FontWeight.Medium)
        .fontColor($r('sys.color.font_primary'))
        .layoutWeight(1)
        .maxLines(2)
        .textOverflow({ overflow: TextOverflow.Ellipsis })
    }
    .width('100%')
    .padding({ top: Spacing.XS, bottom: Spacing.XS })
  }
}

@Component
export struct DeviceSettingsSheet {
  instance: DeviceInstance | undefined = undefined;
  filesDir: string = '';
  isInstanceRunning: boolean = false;
  compactLayout: boolean = false;
  onSave: (updated: DeviceInstance) => void = (): void => {};
  onCancel: () => void = (): void => {};
  onToast: (msg: string) => void = (): void => {};

  @State private activeTab: string = 'perf';
  @State private activeMainTabIndex: number = 0;
  @State private draftProfile: string = 'Balanced';
  @State private draftCpuCores: number = 4;
  @State private draftMemMb: number = 4096;
  @State private draftDiskLimitGb: number = 64;
  @State private draftGpuMode: string = 'vulkan';
  @State private draftSyscallMode: string = 'adaptive';
  @State private draftRefreshRate: number = 120;
  @State private draftRunMode: string = 'emulator';
  @State private draftDeviceType: string = 'phone';
  @State private draftResKey: string = '720x1280';
  @State private draftWidth: number = 720;
  @State private draftHeight: number = 1280;
  @State private draftDpi: number = 320;
  @State private draftEnableAudio: boolean = true;
  @State private draftEnableRoot: boolean = true;
  @State private draftEnableGms: boolean = false;
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
      this.draftDiskLimitGb = normalizeDiskLimitGb(this.instance.diskLimitGb);
      this.draftGpuMode = this.instance.gpuMode;
      this.draftSyscallMode = normalizeSyscallMode(this.instance.syscallMode);
      this.draftRefreshRate = this.instance.refreshRate;
      this.draftRunMode = this.instance.runMode;
      this.draftDeviceType = this.instance.deviceType;
      this.draftResKey = this.instance.resolutionKey;
      this.draftWidth = this.instance.width;
      this.draftHeight = this.instance.height;
      this.draftDpi = this.instance.dpi;
      this.draftEnableAudio = this.instance.enableAudio;
      this.draftEnableRoot = this.instance.enableRoot;
      this.draftEnableGms = this.instance.enableGms === true;
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

  private switchTab(tabKey: string, idx: number): void {
    if (this.activeTab === tabKey) return;
    try {
      this.getUIContext().animateTo({
        duration: Motion.DUR_SHORT,
        curve: Motion.SPRING_SNAPPY
      }, () => {
        this.activeTab = tabKey;
        this.activeMainTabIndex = idx;
      });
    } catch (_e) {
      this.activeTab = tabKey;
      this.activeMainTabIndex = idx;
    }
    if (tabKey === 'disk' || tabKey === 'diag') {
      this.refreshDiagnosticsAndDisk();
    }
  }

  private animateOption(fn: () => void): void {
    try {
      this.getUIContext().animateTo({
        duration: Motion.DUR_MICRO,
        curve: Motion.SPRING_SNAPPY
      }, fn);
    } catch (_e) {
      fn();
    }
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
    this.animateOption(() => {
      this.draftProfile = 'Balanced';
      this.draftCpuCores = 4;
      this.draftMemMb = 4096;
      this.draftDiskLimitGb = 64;
      this.draftGpuMode = 'vulkan';
      this.draftSyscallMode = 'adaptive';
      this.draftRefreshRate = 120;
      this.draftRunMode = 'emulator';
      this.draftDeviceType = 'phone';
      this.draftResKey = '720x1280';
      this.draftWidth = 720;
      this.draftHeight = 1280;
      this.draftDpi = 320;
      this.draftEnableAudio = true;
      this.draftEnableRoot = true;
      this.draftEnableGms = false;
      this.draftKeepAlive = true;
      this.draftPreventSleep = false;
      this.draftAutoGamepad = true;
      this.draftBrand = '';
      this.draftModel = '';
      this.draftAdbPort = 5555;
    });
    this.onToast('已恢复默认配置，点击保存后生效');
  }

  private handleSave(): void {
    if (!this.instance) {
      this.onCancel();
      return;
    }
    const next: DeviceInstance = cloneInstanceRecord(this.instance);
    next.profile = this.draftProfile;
    next.cpuCores = this.draftCpuCores;
    next.memLimitMb = this.draftMemMb;
    next.diskLimitGb = normalizeDiskLimitGb(this.draftDiskLimitGb);
    next.gpuMode = this.draftGpuMode;
    next.syscallMode = normalizeSyscallMode(this.draftSyscallMode);
    next.refreshRate = this.draftRefreshRate;
    next.runMode = this.draftRunMode;
    next.deviceType = this.draftDeviceType;
    next.resolutionKey = this.draftResKey;
    next.width = this.draftWidth;
    next.height = this.draftHeight;
    next.dpi = this.draftDpi;
    next.enableAudio = this.draftEnableAudio;
    next.enableRoot = this.draftEnableRoot;
    next.enableGms = this.draftEnableGms;
    next.keepAlive = this.draftKeepAlive;
    next.preventSleep = this.draftPreventSleep;
    next.autoGamepad = this.draftAutoGamepad;
    next.compactDisk = this.draftCompactDisk;
    next.deviceBrand = this.draftBrand.trim();
    next.deviceModel = this.draftModel.trim();
    next.adbPort = this.draftAdbPort;
    this.onSave(next);
  }

  @Builder
  private perfTabBody() {
    Column({ space: Spacing.MD }) {
      if (this.isInstanceRunning) {
        Row({ space: Spacing.SM }) {
          SymbolGlyph($r('sys.symbol.info_circle'))
            .fontSize(IconSize.SM)
            .fontColor([$r('sys.color.icon_emphasize')])
          Text('设备运行中，部分 CPU/内存/渲染设置将在重启设备后生效')
            .fontSize(FontSize.CAPTION)
            .fontColor($r('sys.color.font_emphasize'))
            .layoutWeight(1)
        }
        .width('100%')
        .padding({ left: Spacing.MD, right: Spacing.MD, top: Spacing.SM, bottom: Spacing.SM })
        .borderRadius(Radius.MD)
        .backgroundColor($r('sys.color.comp_emphasize_tertiary'))
      }

      Column({ space: Spacing.MD }) {
        Text('性能方案')
          .fontSize(FontSize.BODY_MD)
          .fontWeight(FontWeight.Medium)
          .fontColor($r('sys.color.font_secondary'))
        Flex({
          wrap: FlexWrap.Wrap,
          space: { main: { value: Spacing.SM, unit: LengthUnit.VP }, cross: { value: Spacing.SM, unit: LengthUnit.VP } }
        }) {
          SettingsOptionPill({
            label: '性能模式',
            active: this.draftProfile === 'Performance',
            onTap: () => {
              this.animateOption(() => {
                this.draftProfile = 'Performance';
                this.draftCpuCores = profileToCpuCores('Performance');
              });
            }
          })
          SettingsOptionPill({
            label: '均衡模式（推荐）',
            active: this.draftProfile === 'Balanced',
            onTap: () => {
              this.animateOption(() => {
                this.draftProfile = 'Balanced';
                this.draftCpuCores = profileToCpuCores('Balanced');
              });
            }
          })
          SettingsOptionPill({
            label: '节能模式',
            active: this.draftProfile === 'Eco',
            onTap: () => {
              this.animateOption(() => {
                this.draftProfile = 'Eco';
                this.draftCpuCores = profileToCpuCores('Eco');
              });
            }
          })
        }
        .width('100%')
      }
      .width('100%')
      .padding(Spacing.LG)
      .borderRadius(Radius.CARD)
      .backgroundColor($r('sys.color.comp_background_primary'))
      .border({ width: 0.6, color: $r('sys.color.comp_divider') })
      .alignItems(HorizontalAlign.Start)

      Column({ space: Spacing.MD }) {
        Text('计算资源分配')
          .fontSize(FontSize.BODY_MD)
          .fontWeight(FontWeight.Medium)
          .fontColor($r('sys.color.font_secondary'))

        Row({ space: Spacing.MD }) {
          Text('CPU 核数')
            .fontSize(FontSize.BODY_MD)
            .fontColor($r('sys.color.font_primary'))
            .width(64)
          Flex({
            wrap: FlexWrap.Wrap,
            space: { main: { value: Spacing.SM, unit: LengthUnit.VP }, cross: { value: Spacing.SM, unit: LengthUnit.VP } }
          }) {
            SettingsOptionPill({
              label: '2 核',
              active: this.draftCpuCores === 2,
              onTap: () => { this.animateOption(() => { this.draftCpuCores = 2; }); }
            })
            SettingsOptionPill({
              label: '4 核',
              active: this.draftCpuCores === 4,
              onTap: () => { this.animateOption(() => { this.draftCpuCores = 4; }); }
            })
            SettingsOptionPill({
              label: '6 核',
              active: this.draftCpuCores === 6,
              onTap: () => { this.animateOption(() => { this.draftCpuCores = 6; }); }
            })
            SettingsOptionPill({
              label: '8 核',
              active: this.draftCpuCores === 8,
              onTap: () => { this.animateOption(() => { this.draftCpuCores = 8; }); }
            })
          }
          .layoutWeight(1)
        }
        .width('100%')
        .alignItems(VerticalAlign.Center)

        Text(
          this.diagStatus && this.diagStatus.topology.length > 0
            ? `宿主实测拓扑：${this.diagStatus.topology}（按性能模式自动优先绑定大/中/小核，并为宿主保留核心余量）`
            : '按宿主机真实 CPU 大小核拓扑动态绑定核心，自动适配手机、平板与 PC'
        )
          .fontSize(FontSize.MICRO)
          .fontColor($r('sys.color.font_tertiary'))

        Row({ space: Spacing.MD }) {
          Text('内存上限')
            .fontSize(FontSize.BODY_MD)
            .fontColor($r('sys.color.font_primary'))
            .width(64)
          Flex({
            wrap: FlexWrap.Wrap,
            space: { main: { value: Spacing.SM, unit: LengthUnit.VP }, cross: { value: Spacing.SM, unit: LengthUnit.VP } }
          }) {
            SettingsOptionPill({
              label: '2 GB',
              active: this.draftMemMb === 2048,
              onTap: () => { this.animateOption(() => { this.draftMemMb = 2048; }); }
            })
            SettingsOptionPill({
              label: '4 GB',
              active: this.draftMemMb === 4096,
              onTap: () => { this.animateOption(() => { this.draftMemMb = 4096; }); }
            })
            SettingsOptionPill({
              label: '6 GB',
              active: this.draftMemMb === 6144,
              onTap: () => { this.animateOption(() => { this.draftMemMb = 6144; }); }
            })
            SettingsOptionPill({
              label: '8 GB',
              active: this.draftMemMb === 8192,
              onTap: () => { this.animateOption(() => { this.draftMemMb = 8192; }); }
            })
          }
          .layoutWeight(1)
        }
        .width('100%')
        .alignItems(VerticalAlign.Center)
      }
      .width('100%')
      .padding(Spacing.LG)
      .borderRadius(Radius.CARD)
      .backgroundColor($r('sys.color.comp_background_primary'))
      .border({ width: 0.6, color: $r('sys.color.comp_divider') })
      .alignItems(HorizontalAlign.Start)

      Column({ space: Spacing.MD }) {
        Text('图形渲染与高刷引擎')
          .fontSize(FontSize.BODY_MD)
          .fontWeight(FontWeight.Medium)
          .fontColor($r('sys.color.font_secondary'))

        Row({ space: Spacing.MD }) {
          Text('显卡渲染')
            .fontSize(FontSize.BODY_MD)
            .fontColor($r('sys.color.font_primary'))
            .width(64)
          Flex({
            wrap: FlexWrap.Wrap,
            space: { main: { value: Spacing.SM, unit: LengthUnit.VP }, cross: { value: Spacing.SM, unit: LengthUnit.VP } }
          }) {
            SettingsOptionPill({
              label: 'Vulkan（推荐）',
              active: this.draftGpuMode === 'vulkan',
              onTap: () => { this.animateOption(() => { this.draftGpuMode = 'vulkan'; }); }
            })
            SettingsOptionPill({
              label: 'OpenGL ES',
              active: this.draftGpuMode === 'gles',
              onTap: () => { this.animateOption(() => { this.draftGpuMode = 'gles'; }); }
            })
            SettingsOptionPill({
              label: 'CPU 软渲',
              active: this.draftGpuMode === 'swiftshader',
              onTap: () => { this.animateOption(() => { this.draftGpuMode = 'swiftshader'; }); }
            })
          }
          .layoutWeight(1)
        }
        .width('100%')
        .alignItems(VerticalAlign.Center)

        Row({ space: Spacing.MD }) {
          Text('屏幕帧率')
            .fontSize(FontSize.BODY_MD)
            .fontColor($r('sys.color.font_primary'))
            .width(64)
          Flex({
            wrap: FlexWrap.Wrap,
            space: { main: { value: Spacing.SM, unit: LengthUnit.VP }, cross: { value: Spacing.SM, unit: LengthUnit.VP } }
          }) {
            ForEach(
              REFRESH_RATES,
              (hz: number) => {
                SettingsOptionPill({
                  label: `${hz} Hz`,
                  active: this.draftRefreshRate === hz,
                  onTap: () => { this.animateOption(() => { this.draftRefreshRate = hz; }); }
                })
              },
              (hz: number): string => String(hz)
            )
          }
          .layoutWeight(1)
        }
        .width('100%')
        .alignItems(VerticalAlign.Center)

        Text('Vulkan 模式采用宿主 Maleoon 硬件直通与异步提交管线，可流畅运行 120Hz 高帧率应用')
          .fontSize(FontSize.MICRO)
          .fontColor($r('sys.color.font_tertiary'))
      }
      .width('100%')
      .padding(Spacing.LG)
      .borderRadius(Radius.CARD)
      .backgroundColor($r('sys.color.comp_background_primary'))
      .border({ width: 0.6, color: $r('sys.color.comp_divider') })
      .alignItems(HorizontalAlign.Start)

      Column({ space: Spacing.MD }) {
        Text('系统调用拦截模式')
          .fontSize(FontSize.BODY_MD)
          .fontWeight(FontWeight.Medium)
          .fontColor($r('sys.color.font_secondary'))

        Flex({
          wrap: FlexWrap.Wrap,
          space: { main: { value: Spacing.SM, unit: LengthUnit.VP }, cross: { value: Spacing.SM, unit: LengthUnit.VP } }
        }) {
          SettingsOptionPill({
            label: '自适应（推荐）',
            active: this.draftSyscallMode === 'adaptive',
            onTap: () => { this.animateOption(() => { this.draftSyscallMode = 'adaptive'; }); }
          })
          SettingsOptionPill({
            label: '纯净模式',
            active: this.draftSyscallMode === 'pure_seccomp',
            onTap: () => { this.animateOption(() => { this.draftSyscallMode = 'pure_seccomp'; }); }
          })
          SettingsOptionPill({
            label: '混合模式',
            active: this.draftSyscallMode === 'hybrid',
            onTap: () => { this.animateOption(() => { this.draftSyscallMode = 'hybrid'; }); }
          })
          SettingsOptionPill({
            label: '原版内联',
            active: this.draftSyscallMode === 'inline',
            onTap: () => { this.animateOption(() => { this.draftSyscallMode = 'inline'; }); }
          })
        }
        .width('100%')

        Text(
          this.draftSyscallMode === 'pure_seccomp'
            ? '纯净模式：所有容器进程均采用纯 seccomp 拦截 + BPF 热路径直通，完全不修改任何可执行代码段（重启后生效）'
            : (this.draftSyscallMode === 'hybrid'
              ? '混合模式：系统库使用内联补丁加速，仅第三方应用库（/data）走纯 seccomp（重启后生效）'
              : (this.draftSyscallMode === 'inline'
                ? '原版内联：系统库与第三方应用库均使用 svbox 原版内联指令替换（性能最高，仅部分反作弊/加壳校验可能敏感，重启后生效）'
                : '自适应：系统守护进程保留内联补丁，Zygote 与全部 Android 应用采用纯 seccomp + BPF 直通，兼顾代码段纯净度与帧率（重启后生效）'))
        )
          .fontSize(FontSize.MICRO)
          .fontColor($r('sys.color.font_tertiary'))
      }
      .width('100%')
      .padding(Spacing.LG)
      .borderRadius(Radius.CARD)
      .backgroundColor($r('sys.color.comp_background_primary'))
      .border({ width: 0.6, color: $r('sys.color.comp_divider') })
      .alignItems(HorizontalAlign.Start)
    }
    .width('100%')
  }

  @Builder
  private displayTabBody() {
    Column({ space: Spacing.MD }) {
      Column({ space: Spacing.MD }) {
        Text('窗口运行模式')
          .fontSize(FontSize.BODY_MD)
          .fontWeight(FontWeight.Medium)
          .fontColor($r('sys.color.font_secondary'))
        Row({ space: Spacing.SM }) {
          SettingsOptionPill({
            label: '传统模拟器窗口',
            active: this.draftRunMode === 'emulator',
            onTap: () => { this.animateOption(() => { this.draftRunMode = 'emulator'; }); }
          })
          SettingsOptionPill({
            label: '自由多窗口模式',
            active: this.draftRunMode === 'multiwindow',
            onTap: () => { this.animateOption(() => { this.draftRunMode = 'multiwindow'; }); }
          })
        }
        Text(
          this.draftRunMode === 'emulator'
            ? '启动后直接打开独立完整 Android 桌面模拟器窗口，并支持随时将前台应用拆出'
            : '容器后台运行（单容器模式），通过「应用」中心以独立鸿蒙原生窗口启动各个 Android 应用'
        )
          .fontSize(FontSize.MICRO)
          .fontColor($r('sys.color.font_tertiary'))
      }
      .width('100%')
      .padding(Spacing.LG)
      .borderRadius(Radius.CARD)
      .backgroundColor($r('sys.color.comp_background_primary'))
      .border({ width: 0.6, color: $r('sys.color.comp_divider') })
      .alignItems(HorizontalAlign.Start)

      Column({ space: Spacing.MD }) {
        Text('分辨率预设')
          .fontSize(FontSize.BODY_MD)
          .fontWeight(FontWeight.Medium)
          .fontColor($r('sys.color.font_secondary'))

        Flex({
          wrap: FlexWrap.Wrap,
          space: { main: { value: Spacing.SM, unit: LengthUnit.VP }, cross: { value: Spacing.SM, unit: LengthUnit.VP } }
        }) {
          ForEach(
            RESOLUTION_PRESETS,
            (item: ResolutionPresetItem) => {
              SettingsOptionPill({
                label: item.label,
                active: this.draftResKey === item.key,
                onTap: () => {
                  this.animateOption(() => {
                    this.draftResKey = item.key;
                    this.draftWidth = item.w;
                    this.draftHeight = item.h;
                    this.draftDpi = computeDpiForSize(item.w, item.h);
                    this.draftDeviceType = item.w > item.h ? 'tablet' : 'phone';
                  });
                }
              })
            },
            (item: ResolutionPresetItem): string => item.key
          )
        }
        .width('100%')
      }
      .width('100%')
      .padding(Spacing.LG)
      .borderRadius(Radius.CARD)
      .backgroundColor($r('sys.color.comp_background_primary'))
      .border({ width: 0.6, color: $r('sys.color.comp_divider') })
      .alignItems(HorizontalAlign.Start)

      Column({ space: Spacing.MD }) {
        Text('像素密度（DPI）')
          .fontSize(FontSize.BODY_MD)
          .fontWeight(FontWeight.Medium)
          .fontColor($r('sys.color.font_secondary'))
        Row({ space: Spacing.SM }) {
          SettingsOptionPill({
            label: '240 DPI',
            active: this.draftDpi === 240,
            onTap: () => { this.animateOption(() => { this.draftDpi = 240; }); }
          })
          SettingsOptionPill({
            label: '280 DPI',
            active: this.draftDpi === 280,
            onTap: () => { this.animateOption(() => { this.draftDpi = 280; }); }
          })
          SettingsOptionPill({
            label: '320 DPI',
            active: this.draftDpi === 320,
            onTap: () => { this.animateOption(() => { this.draftDpi = 320; }); }
          })
          SettingsOptionPill({
            label: '420 DPI',
            active: this.draftDpi === 420,
            onTap: () => { this.animateOption(() => { this.draftDpi = 420; }); }
          })
        }
        Text(`当前画布：${this.draftWidth} × ${this.draftHeight} · ${this.draftDpi} DPI`)
          .fontSize(FontSize.MICRO)
          .fontColor($r('sys.color.font_tertiary'))
      }
      .width('100%')
      .padding(Spacing.LG)
      .borderRadius(Radius.CARD)
      .backgroundColor($r('sys.color.comp_background_primary'))
      .border({ width: 0.6, color: $r('sys.color.comp_divider') })
      .alignItems(HorizontalAlign.Start)
    }
    .width('100%')
  }

  @Builder
  private audioTabBody() {
    Column({ space: Spacing.MD }) {
      Column() {
        SettingsToggleRow({
          title: '启用 OHAudio 宿主音频桥接',
          desc: '将 Android 14 音频流低延迟直通至鸿蒙 PC 默认输出设备',
          checked: this.draftEnableAudio,
          onToggle: (v: boolean) => { this.draftEnableAudio = v; }
        })
      }
      .width('100%')
      .padding(Spacing.LG)
      .borderRadius(Radius.CARD)
      .backgroundColor($r('sys.color.comp_background_primary'))
      .border({ width: 0.6, color: $r('sys.color.comp_divider') })

      Column({ space: Spacing.SM }) {
        Text('音频架构信息')
          .fontSize(FontSize.BODY_MD)
          .fontWeight(FontWeight.Medium)
          .fontColor($r('sys.color.font_secondary'))
        Text('输出管线：audio.primary.svbox.so → IPC Socket → OH_AudioRenderer (48kHz Stereo)')
          .fontSize(FontSize.CAPTION)
          .fontColor($r('sys.color.font_primary'))
        Text(this.diagStatus && this.diagStatus.audioActive ? '当前音频状态：活跃播放中' : '当前音频状态：待机就绪')
          .fontSize(FontSize.MICRO)
          .fontColor($r('sys.color.font_tertiary'))
      }
      .width('100%')
      .padding(Spacing.LG)
      .borderRadius(Radius.CARD)
      .backgroundColor($r('sys.color.comp_background_primary'))
      .border({ width: 0.6, color: $r('sys.color.comp_divider') })
      .alignItems(HorizontalAlign.Start)
    }
    .width('100%')
  }

  private isDiskTierAllowed(gb: number): boolean {
    const usedGb: number = this.diskUsageMb / 1024.0;
    // System occupies ~2 GB; keep at least 0.5 GB free headroom above actual userdata usage.
    const usableDataGb: number = gb - 2.0;
    return usedGb <= 0.1 || usableDataGb >= usedGb + 0.5;
  }

  @Builder
  private diskTabBody() {
    Column({ space: Spacing.MD }) {
      Column({ space: Spacing.MD }) {
        Row({ space: Spacing.MD }) {
          Column({ space: Spacing.XS }) {
            Text('磁盘空间占用')
              .fontSize(FontSize.BODY_LG)
              .fontWeight(FontWeight.Medium)
              .fontColor($r('sys.color.font_primary'))
            Text(`隔离目录：${this.instance ? this.instance.dataSubdir : 'userdata14'}`)
              .fontSize(FontSize.MICRO)
              .fontColor($r('sys.color.font_tertiary'))
          }
          .alignItems(HorizontalAlign.Start)
          .layoutWeight(1)

          Column({ space: 2 }) {
            Text(`${formatDiskSize(this.diskUsageMb)} / ${this.draftDiskLimitGb} GB`)
              .fontSize(FontSize.TITLE_MD)
              .fontWeight(FontWeight.Bold)
              .fontColor($r('sys.color.font_emphasize'))
            Text(`虚拟可用上限约 ${Math.max(1, this.draftDiskLimitGb - 2)} GB`)
              .fontSize(FontSize.MICRO)
              .fontColor($r('sys.color.font_tertiary'))
          }
          .alignItems(HorizontalAlign.End)
        }
        .width('100%')

        Divider().color($r('sys.color.comp_divider')).opacity(0.35)

        Column({ space: Spacing.SM }) {
          Row({ space: Spacing.SM }) {
            Text('存储空间容量')
              .fontSize(FontSize.BODY_MD)
              .fontWeight(FontWeight.Medium)
              .fontColor($r('sys.color.font_primary'))
            Text('支持无损扩容')
              .fontSize(FontSize.MICRO)
              .fontColor($r('sys.color.font_emphasize'))
              .padding({ left: Spacing.SM, right: Spacing.SM, top: 2, bottom: 2 })
              .borderRadius(Radius.BADGE)
              .backgroundColor($r('sys.color.comp_emphasize_tertiary'))
          }
          .width('100%')
          .alignItems(VerticalAlign.Center)

          Flex({
            wrap: FlexWrap.Wrap,
            space: { main: { value: Spacing.SM, unit: LengthUnit.VP }, cross: { value: Spacing.SM, unit: LengthUnit.VP } }
          }) {
            ForEach(DISK_SIZE_OPTIONS_GB, (gb: number) => {
              Button({ type: ButtonType.Normal }) {
                Text(`${gb} GB`)
                  .fontSize(FontSize.CAPTION)
                  .fontWeight(this.draftDiskLimitGb === gb ? FontWeight.Medium : FontWeight.Normal)
                  .fontColor(this.draftDiskLimitGb === gb ? $r('sys.color.font_on_primary') : $r('sys.color.font_primary'))
              }
              .height(ControlSize.BTN_SM)
              .padding({ left: Spacing.MD, right: Spacing.MD })
              .borderRadius(Radius.CAPSULE)
              .backgroundColor(
                this.draftDiskLimitGb === gb
                  ? $r('sys.color.brand')
                  : $r('sys.color.comp_background_tertiary')
              )
              .opacity(this.isDiskTierAllowed(gb) ? 1.0 : 0.45)
              .hoverEffect(HoverEffect.Highlight)
              .clickEffect({ level: ClickEffectLevel.LIGHT, scale: 0.95 })
              .onClick(() => {
                if (!this.isDiskTierAllowed(gb)) {
                  const needGb: number = Math.ceil(this.diskUsageMb / 1024.0 + 2.5);
                  this.onToast(`当前已用 ${formatDiskSize(this.diskUsageMb)}，请选择 ${needGb} GB 以上容量`);
                  return;
                }
                this.animateOption(() => {
                  this.draftDiskLimitGb = gb;
                });
              })
            }, (gb: number) => `disk_tier_${gb}`)
          }
          .width('100%')

          Text('无损调整目录式虚拟卷上限，保留全部应用与数据，不格式化存储（重启设备生效）。8 GB 中系统约占 2 GB，可用约 6 GB；日常推荐 32 GB / 64 GB。')
            .fontSize(FontSize.MICRO)
            .fontColor($r('sys.color.font_tertiary'))
        }
        .width('100%')
        .alignItems(HorizontalAlign.Start)

        Divider().color($r('sys.color.comp_divider')).opacity(0.35)

        Row({ space: Spacing.MD }) {
          Column({ space: Spacing.XS }) {
            Text('磁盘缓存清理')
              .fontSize(FontSize.BODY_MD)
              .fontWeight(FontWeight.Medium)
              .fontColor($r('sys.color.font_primary'))
            Text(this.isInstanceRunning
              ? '设备运行中，请先停止该设备后再执行磁盘清理'
              : '安全清理应用缓存、崩溃日志与 ANR 临时文件，保留已安装应用与账号数据')
              .fontSize(FontSize.MICRO)
              .fontColor(this.isInstanceRunning ? $r('sys.color.warning') : $r('sys.color.font_tertiary'))
          }
          .alignItems(HorizontalAlign.Start)
          .layoutWeight(1)

          Button(this.cleaningDisk ? '清理中...' : '立即清理', { type: ButtonType.Capsule })
            .height(ControlSize.BTN_SM)
            .padding({ left: Spacing.LG, right: Spacing.LG })
            .fontSize(FontSize.CAPTION)
            .fontColor($r('sys.color.font_on_primary'))
            .backgroundColor($r('sys.color.brand'))
            .enabled(!this.cleaningDisk && !this.isInstanceRunning)
            .opacity(this.isInstanceRunning ? 0.45 : 1.0)
            .hoverEffect(HoverEffect.Highlight)
            .clickEffect({ level: ClickEffectLevel.LIGHT, scale: 0.95 })
            .onClick(() => {
              if (!this.instance) return;
              if (this.isInstanceRunning) {
                this.onToast('请先停止该设备后再执行磁盘清理');
                return;
              }
              this.cleaningDisk = true;
              try {
                const freed: number = native.cleanInstanceCache(this.filesDir, this.instance.dataSubdir);
                if (freed < 0) {
                  this.onToast('请先停止该设备后再执行磁盘清理');
                  return;
                }
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
      .padding(Spacing.LG)
      .borderRadius(Radius.CARD)
      .backgroundColor($r('sys.color.comp_background_primary'))
      .border({ width: 0.6, color: $r('sys.color.comp_divider') })

      Column({ space: Spacing.SM }) {
        Text('宿主机共享文件夹')
          .fontSize(FontSize.BODY_MD)
          .fontWeight(FontWeight.Medium)
          .fontColor($r('sys.color.font_secondary'))
        Row({ space: Spacing.SM }) {
          Text(this.shareDir)
            .fontSize(FontSize.CAPTION)
            .fontColor($r('sys.color.font_primary'))
            .layoutWeight(1)
            .maxLines(1)
            .textOverflow({ overflow: TextOverflow.Ellipsis })
          Button('检查目录', { type: ButtonType.Capsule })
            .height(ControlSize.BTN_SM)
            .padding({ left: Spacing.MD, right: Spacing.MD })
            .fontSize(FontSize.CAPTION)
            .fontColor($r('sys.color.font_primary'))
            .backgroundColor($r('sys.color.comp_background_tertiary'))
            .hoverEffect(HoverEffect.Highlight)
            .clickEffect({ level: ClickEffectLevel.LIGHT, scale: 0.95 })
            .onClick(() => {
              const ok: boolean = native.ensureDir(this.shareDir);
              this.onToast(ok ? `共享目录已就绪：${this.shareDir}` : '创建共享目录失败');
            })
        }
        .width('100%')
        Text('对应 Android 内部路径：/sdcard/Download/svbox-share')
          .fontSize(FontSize.MICRO)
          .fontColor($r('sys.color.font_tertiary'))
      }
      .width('100%')
      .padding(Spacing.LG)
      .borderRadius(Radius.CARD)
      .backgroundColor($r('sys.color.comp_background_primary'))
      .border({ width: 0.6, color: $r('sys.color.comp_divider') })
      .alignItems(HorizontalAlign.Start)

      Column() {
        SettingsToggleRow({
          title: '精简磁盘缓存模式',
          desc: '限制系统临时缓存增长，适合多实例长期保存',
          checked: this.draftCompactDisk,
          onToggle: (v: boolean) => { this.draftCompactDisk = v; }
        })
      }
      .width('100%')
      .padding(Spacing.LG)
      .borderRadius(Radius.CARD)
      .backgroundColor($r('sys.color.comp_background_primary'))
      .border({ width: 0.6, color: $r('sys.color.comp_divider') })
    }
    .width('100%')
  }

  @Builder
  private networkTabBody() {
    Column({ space: Spacing.MD }) {
      Column({ space: Spacing.SM }) {
        Text('网络桥接模式')
          .fontSize(FontSize.BODY_MD)
          .fontWeight(FontWeight.Medium)
          .fontColor($r('sys.color.font_secondary'))
        Row({ space: Spacing.SM }) {
          SymbolGlyph($r('sys.symbol.wifi'))
            .fontSize(IconSize.SM)
            .fontColor([$r('sys.color.confirm')])
          Text('宿主机共享网络（NAT + 内置 DNS 代理 223.5.5.5 / 8.8.8.8）')
            .fontSize(FontSize.BODY_MD)
            .fontColor($r('sys.color.font_primary'))
        }
        Text('自动复用鸿蒙 PC 的 WLAN / 有线网络与代理配置，无需额外虚拟网卡驱动')
          .fontSize(FontSize.MICRO)
          .fontColor($r('sys.color.font_tertiary'))
      }
      .width('100%')
      .padding(Spacing.LG)
      .borderRadius(Radius.CARD)
      .backgroundColor($r('sys.color.comp_background_primary'))
      .border({ width: 0.6, color: $r('sys.color.comp_divider') })
      .alignItems(HorizontalAlign.Start)

      Column({ space: Spacing.MD }) {
        Text('ADB 调试端口')
          .fontSize(FontSize.BODY_MD)
          .fontWeight(FontWeight.Medium)
          .fontColor($r('sys.color.font_secondary'))
        Row({ space: Spacing.SM }) {
          SettingsOptionPill({
            label: '5555（默认）',
            active: this.draftAdbPort === 5555,
            onTap: () => { this.animateOption(() => { this.draftAdbPort = 5555; }); }
          })
          SettingsOptionPill({
            label: '5557',
            active: this.draftAdbPort === 5557,
            onTap: () => { this.animateOption(() => { this.draftAdbPort = 5557; }); }
          })
          SettingsOptionPill({
            label: '5559',
            active: this.draftAdbPort === 5559,
            onTap: () => { this.animateOption(() => { this.draftAdbPort = 5559; }); }
          })
          SettingsOptionPill({
            label: '16384',
            active: this.draftAdbPort === 16384,
            onTap: () => { this.animateOption(() => { this.draftAdbPort = 16384; }); }
          })
        }
        Text(`连接命令：adb connect 127.0.0.1:${this.draftAdbPort}`)
          .fontSize(FontSize.CAPTION)
          .fontColor($r('sys.color.font_emphasize'))
      }
      .width('100%')
      .padding(Spacing.LG)
      .borderRadius(Radius.CARD)
      .backgroundColor($r('sys.color.comp_background_primary'))
      .border({ width: 0.6, color: $r('sys.color.comp_divider') })
      .alignItems(HorizontalAlign.Start)
    }
    .width('100%')
  }

  @Builder
  private modelTabBody() {
    Column({ space: Spacing.MD }) {
      Column({ space: Spacing.MD }) {
        Text('常用机型预设')
          .fontSize(FontSize.BODY_MD)
          .fontWeight(FontWeight.Medium)
          .fontColor($r('sys.color.font_secondary'))

        Flex({
          wrap: FlexWrap.Wrap,
          space: { main: { value: Spacing.SM, unit: LengthUnit.VP }, cross: { value: Spacing.SM, unit: LengthUnit.VP } }
        }) {
          ForEach(
            DEVICE_MODEL_PRESETS,
            (preset: DeviceModelPreset) => {
              SettingsOptionPill({
                label: preset.label,
                active: this.draftBrand === preset.brand && this.draftModel === preset.model,
                onTap: () => {
                  this.animateOption(() => {
                    this.draftBrand = preset.brand;
                    this.draftModel = preset.model;
                  });
                }
              })
            },
            (preset: DeviceModelPreset): string => preset.label
          )
        }
        .width('100%')
      }
      .width('100%')
      .padding(Spacing.LG)
      .borderRadius(Radius.CARD)
      .backgroundColor($r('sys.color.comp_background_primary'))
      .border({ width: 0.6, color: $r('sys.color.comp_divider') })
      .alignItems(HorizontalAlign.Start)

      Column({ space: Spacing.MD }) {
        Text('自定义机型属性（ro.product.*）')
          .fontSize(FontSize.BODY_MD)
          .fontWeight(FontWeight.Medium)
          .fontColor($r('sys.color.font_secondary'))

        Row({ space: Spacing.MD }) {
          Text('品牌')
            .fontSize(FontSize.BODY_MD)
            .fontColor($r('sys.color.font_primary'))
            .width(48)
          TextInput({ placeholder: '留空使用默认，例如 Xiaomi / Google', text: this.draftBrand })
            .layoutWeight(1)
            .height(ControlSize.INPUT_H)
            .fontSize(FontSize.BODY_MD)
            .fontColor($r('sys.color.font_primary'))
            .backgroundColor($r('sys.color.background_secondary'))
            .borderRadius(Radius.MD)
            .onChange((v: string) => { this.draftBrand = v; })
        }
        .width('100%')

        Row({ space: Spacing.MD }) {
          Text('型号')
            .fontSize(FontSize.BODY_MD)
            .fontColor($r('sys.color.font_primary'))
            .width(48)
          TextInput({ placeholder: '留空使用默认，例如 Pixel 8 Pro', text: this.draftModel })
            .layoutWeight(1)
            .height(ControlSize.INPUT_H)
            .fontSize(FontSize.BODY_MD)
            .fontColor($r('sys.color.font_primary'))
            .backgroundColor($r('sys.color.background_secondary'))
            .borderRadius(Radius.MD)
            .onChange((v: string) => { this.draftModel = v; })
        }
        .width('100%')
      }
      .width('100%')
      .padding(Spacing.LG)
      .borderRadius(Radius.CARD)
      .backgroundColor($r('sys.color.comp_background_primary'))
      .border({ width: 0.6, color: $r('sys.color.comp_divider') })
      .alignItems(HorizontalAlign.Start)
    }
    .width('100%')
  }

  @Builder
  private shortcutsTabBody() {
    Column({ space: Spacing.SM }) {
      Text('常用键鼠操作与快捷键')
        .fontSize(FontSize.BODY_MD)
        .fontWeight(FontWeight.Medium)
        .fontColor($r('sys.color.font_secondary'))
        .margin({ bottom: Spacing.XS })

      ForEach(
        SHORTCUT_ITEMS,
        (row: ShortcutRow) => {
          Row({ space: Spacing.MD }) {
            Text(row.action)
              .fontSize(FontSize.BODY_MD)
              .fontColor($r('sys.color.font_primary'))
              .layoutWeight(1)
            Text(row.keys)
              .fontSize(FontSize.CAPTION)
              .fontColor($r('sys.color.font_emphasize'))
              .padding({ left: Spacing.MD, right: Spacing.MD, top: Spacing.XS, bottom: Spacing.XS })
              .borderRadius(Radius.SM)
              .backgroundColor($r('sys.color.comp_emphasize_tertiary'))
          }
          .width('100%')
          .padding({ left: Spacing.LG, right: Spacing.LG, top: Spacing.MD, bottom: Spacing.MD })
          .borderRadius(Radius.MD)
          .backgroundColor($r('sys.color.comp_background_primary'))
          .border({ width: 0.6, color: $r('sys.color.comp_divider') })
        },
        (row: ShortcutRow): string => row.action
      )
    }
    .width('100%')
    .alignItems(HorizontalAlign.Start)
  }

  @Builder
  private otherTabBody() {
    Column({ space: Spacing.MD }) {
      Column({ space: Spacing.SM }) {
        SettingsToggleRow({
          title: '启用 Google 服务框架（GMS / Google Play）',
          desc: this.diagStatus && this.diagStatus.gmsAvailable
            ? '以只读 Overlay 层叠加 GMS Core 与 Play 商店（不修改 rootfs14，重启设备生效）'
            : '未检测到 gms_layer14 扩展层，请先运行 scripts/prepare_gms_layer.py 准备镜像层',
          checked: this.draftEnableGms,
          onToggle: (v: boolean) => { this.draftEnableGms = v; }
        })
        Divider().color($r('sys.color.comp_divider')).opacity(0.35)
        SettingsToggleRow({
          title: '开启 ADB Root 权限',
          desc: '允许通过 adb shell 以 root 身份调试容器环境',
          checked: this.draftEnableRoot,
          onToggle: (v: boolean) => { this.draftEnableRoot = v; }
        })
        Divider().color($r('sys.color.comp_divider')).opacity(0.35)
        SettingsToggleRow({
          title: '后台保活运行',
          desc: '最小化窗口时保持 Android 实例与推送连接持续在线',
          checked: this.draftKeepAlive,
          onToggle: (v: boolean) => { this.draftKeepAlive = v; }
        })
        Divider().color($r('sys.color.comp_divider')).opacity(0.35)
        SettingsToggleRow({
          title: '运行时自动连接游戏手柄',
          desc: '检测到鸿蒙 GameControllerKit / USB 手柄时自动透传至设备',
          checked: this.draftAutoGamepad,
          onToggle: (v: boolean) => { this.draftAutoGamepad = v; }
        })
        Divider().color($r('sys.color.comp_divider')).opacity(0.35)
        SettingsToggleRow({
          title: '设备运行时防止电脑休眠',
          desc: '适合挂机下载或长时间调试任务',
          checked: this.draftPreventSleep,
          onToggle: (v: boolean) => { this.draftPreventSleep = v; }
        })
      }
      .width('100%')
      .padding(Spacing.LG)
      .borderRadius(Radius.CARD)
      .backgroundColor($r('sys.color.comp_background_primary'))
      .border({ width: 0.6, color: $r('sys.color.comp_divider') })
    }
    .width('100%')
  }

  @Builder
  private diagTabBody() {
    Column({ space: Spacing.MD }) {
      Row() {
        Text('实时容器与宿主诊断')
          .fontSize(FontSize.BODY_LG)
          .fontWeight(FontWeight.Medium)
          .fontColor($r('sys.color.font_primary'))
        Blank()
        Button('刷新诊断', { type: ButtonType.Capsule })
          .height(ControlSize.BTN_SM)
          .padding({ left: Spacing.MD, right: Spacing.MD })
          .fontSize(FontSize.CAPTION)
          .fontColor($r('sys.color.font_emphasize'))
          .backgroundColor($r('sys.color.comp_emphasize_tertiary'))
          .hoverEffect(HoverEffect.Highlight)
          .clickEffect({ level: ClickEffectLevel.LIGHT, scale: 0.95 })
          .onClick(() => {
            this.refreshDiagnosticsAndDisk();
          })
      }
      .width('100%')

      Column({ space: Spacing.XS }) {
        SettingsDiagRow({
          label: '容器状态',
          textVal: this.diagStatus && this.diagStatus.running ? `运行中 (${this.diagStatus.statusText})` : '未开机'
        })
        Divider().color($r('sys.color.comp_divider')).opacity(0.3)
        SettingsDiagRow({
          label: '渲染后端',
          textVal: this.diagStatus ? `${formatGpuModeLabel(this.diagStatus.gpuMode)} · ${this.diagStatus.gpuRenderer}` : 'Maleoon Vulkan'
        })
        SettingsDiagRow({
          label: '系统调用模式',
          textVal: this.diagStatus ? `${formatSyscallModeLabel(this.diagStatus.syscallMode)} (${this.diagStatus.syscallMode})` : '自适应'
        })
        Divider().color($r('sys.color.comp_divider')).opacity(0.3)
        SettingsDiagRow({
          label: '显示画布',
          textVal: this.diagStatus ? `${this.diagStatus.width}×${this.diagStatus.height} @ ${this.diagStatus.fps}/${this.diagStatus.refreshRate}Hz` : '-'
        })
        Divider().color($r('sys.color.comp_divider')).opacity(0.3)
        SettingsDiagRow({
          label: '宿主 CPU 拓扑',
          textVal: this.diagStatus && this.diagStatus.topology.length > 0 ? this.diagStatus.topology : 'ARM64 多核'
        })
        SettingsDiagRow({
          label: '容器绑定核心',
          textVal: this.diagStatus && this.diagStatus.cpuset.length > 0 ? this.diagStatus.cpuset : '-'
        })
        Divider().color($r('sys.color.comp_divider')).opacity(0.3)
        SettingsDiagRow({
          label: '真实内存 (PSS)',
          textVal: this.diagStatus
            ? `容器 ${this.diagStatus.memMb} MB (上限 ${this.diagStatus.memLimitMb} MB) · 宿主 ${this.diagStatus.hostMemMb ?? 0} MB / 显存 ${this.diagStatus.gfxMemMb ?? 0} MB`
            : '-'
        })
        Divider().color($r('sys.color.comp_divider')).opacity(0.3)
        SettingsDiagRow({
          label: '活跃进程数',
          textVal: this.diagStatus ? `${this.diagStatus.activeProcs} 个容器进程 · CPU ${Math.round(this.diagStatus.cpuPercent)}%` : '0'
        })
        Divider().color($r('sys.color.comp_divider')).opacity(0.3)
        SettingsDiagRow({
          label: '窗口代理 WM Agent',
          textVal: this.diagStatus && this.diagStatus.wmAgentReady ? '已连接（支持自由多窗）' : '未就绪'
        })
        Divider().color($r('sys.color.comp_divider')).opacity(0.3)
        SettingsDiagRow({
          label: '当前实例磁盘',
          textVal: `${formatDiskSize(this.diskUsageMb)} / ${this.draftDiskLimitGb} GB`
        })
      }
      .width('100%')
      .padding(Spacing.LG)
      .borderRadius(Radius.CARD)
      .backgroundColor($r('sys.color.comp_background_primary'))
      .border({ width: 0.6, color: $r('sys.color.comp_divider') })
    }
    .width('100%')
  }

  @Builder
  private tabBodyRouter() {
    Column() {
      if (this.activeTab === 'perf') {
        Column() { this.perfTabBody() }.width('100%').transition(Motion.tabContentTransition())
      } else if (this.activeTab === 'display') {
        Column() { this.displayTabBody() }.width('100%').transition(Motion.tabContentTransition())
      } else if (this.activeTab === 'audio') {
        Column() { this.audioTabBody() }.width('100%').transition(Motion.tabContentTransition())
      } else if (this.activeTab === 'disk') {
        Column() { this.diskTabBody() }.width('100%').transition(Motion.tabContentTransition())
      } else if (this.activeTab === 'network') {
        Column() { this.networkTabBody() }.width('100%').transition(Motion.tabContentTransition())
      } else if (this.activeTab === 'model') {
        Column() { this.modelTabBody() }.width('100%').transition(Motion.tabContentTransition())
      } else if (this.activeTab === 'shortcuts') {
        Column() { this.shortcutsTabBody() }.width('100%').transition(Motion.tabContentTransition())
      } else if (this.activeTab === 'other') {
        Column() { this.otherTabBody() }.width('100%').transition(Motion.tabContentTransition())
      } else {
        Column() { this.diagTabBody() }.width('100%').transition(Motion.tabContentTransition())
      }
    }
    .width('100%')
    .alignItems(HorizontalAlign.Start)
  }

  build() {
    Column({ space: 0 }) {
      Divider().color($r('sys.color.comp_divider')).opacity(0.35)

      if (this.compactLayout) {
        // Mobile / Narrow Foldable Layout: Top Horizontally Scrollable Pill Bar + Top-Aligned Content
        Scroll() {
          Row({ space: Spacing.SM }) {
            ForEach(
              this.mainTabs,
              (tab: SettingsNavTab, idx: number) => {
                Button({ type: ButtonType.Capsule }) {
                  Row({ space: Spacing.XS + 2 }) {
                    SymbolGlyph(tab.icon)
                      .fontSize(IconSize.SM)
                      .fontColor([
                        this.activeTab === tab.key
                          ? $r('sys.color.icon_on_primary')
                          : $r('sys.color.icon_secondary')
                      ])
                    Text(tab.label)
                      .fontSize(FontSize.CAPTION)
                      .fontWeight(this.activeTab === tab.key ? FontWeight.Medium : FontWeight.Normal)
                      .fontColor(
                        this.activeTab === tab.key
                          ? $r('sys.color.font_on_primary')
                          : $r('sys.color.font_primary')
                      )
                  }
                }
                .height(ControlSize.BTN_SM)
                .padding({ left: Spacing.MD, right: Spacing.MD })
                .backgroundColor(
                  this.activeTab === tab.key
                    ? $r('sys.color.brand')
                    : $r('sys.color.comp_background_tertiary')
                )
                .animation({ duration: Motion.DUR_MICRO, curve: Motion.STD_CURVE })
                .onClick(() => { this.switchTab(tab.key, idx); })
              },
              (tab: SettingsNavTab): string => tab.key
            )

            Button({ type: ButtonType.Capsule }) {
              Row({ space: Spacing.XS + 2 }) {
                SymbolGlyph($r('sys.symbol.wrench_and_screwdriver'))
                  .fontSize(IconSize.SM)
                  .fontColor([
                    this.activeTab === 'diag'
                      ? $r('sys.color.icon_on_primary')
                      : $r('sys.color.icon_secondary')
                  ])
                Text('诊断')
                  .fontSize(FontSize.CAPTION)
                  .fontWeight(this.activeTab === 'diag' ? FontWeight.Medium : FontWeight.Normal)
                  .fontColor(
                    this.activeTab === 'diag'
                      ? $r('sys.color.font_on_primary')
                      : $r('sys.color.font_primary')
                  )
              }
            }
            .height(ControlSize.BTN_SM)
            .padding({ left: Spacing.MD, right: Spacing.MD })
            .backgroundColor(
              this.activeTab === 'diag'
                ? $r('sys.color.brand')
                : $r('sys.color.comp_background_tertiary')
            )
            .animation({ duration: Motion.DUR_MICRO, curve: Motion.STD_CURVE })
            .onClick(() => { this.switchTab('diag', -1); })
          }
          .padding({ left: Spacing.XXL, right: Spacing.XXL, top: Spacing.MD, bottom: Spacing.MD })
        }
        .width('100%')
        .scrollable(ScrollDirection.Horizontal)
        .scrollBar(BarState.Off)

        Divider().color($r('sys.color.comp_divider')).opacity(0.3)

        Scroll() {
          Column() {
            this.tabBodyRouter()
          }
          .width('100%')
          .padding({ left: Spacing.XXL, right: Spacing.XXL, top: Spacing.LG, bottom: Spacing.LG })
        }
        .width('100%')
        .layoutWeight(1)
        .align(Alignment.TopStart)
        .scrollBar(BarState.Auto)
        .scrollBarWidth(3)
        .scrollBarColor($r('sys.color.comp_divider'))
      } else {
        // Tablet / PC Split-Pane Layout (24vp left/right baseline alignment)
        Row({ space: 0 }) {
          Column({ space: 0 }) {
            Stack({ alignContent: Alignment.TopStart }) {
              if (this.activeMainTabIndex >= 0) {
                Column()
                  .width('100%')
                  .height(NAV_ITEM_H)
                  .borderRadius(Radius.MD)
                  .backgroundColor($r('sys.color.comp_emphasize_tertiary'))
                  .translate({ y: this.activeMainTabIndex * (NAV_ITEM_H + NAV_ITEM_GAP) })
                  .animation({ duration: Motion.DUR_SHORT, curve: Motion.SPRING_SNAPPY })
              }

              Column({ space: NAV_ITEM_GAP }) {
                ForEach(
                  this.mainTabs,
                  (tab: SettingsNavTab, idx: number) => {
                    Row({ space: Spacing.MD }) {
                      SymbolGlyph(tab.icon)
                        .fontSize(IconSize.SM)
                        .fontColor([
                          this.activeTab === tab.key
                            ? $r('sys.color.icon_emphasize')
                            : $r('sys.color.icon_secondary')
                        ])
                      Text(tab.label)
                        .fontSize(FontSize.BODY_MD)
                        .fontWeight(this.activeTab === tab.key ? FontWeight.Medium : FontWeight.Normal)
                        .fontColor(
                          this.activeTab === tab.key
                            ? $r('sys.color.font_emphasize')
                            : $r('sys.color.font_primary')
                        )
                    }
                    .width('100%')
                    .height(NAV_ITEM_H)
                    .padding({ left: Spacing.SM, right: Spacing.MD })
                    .borderRadius(Radius.MD)
                    .hoverEffect(HoverEffect.Highlight)
                    .clickEffect({ level: ClickEffectLevel.LIGHT, scale: 0.97 })
                    .onClick(() => {
                      this.switchTab(tab.key, idx);
                    })
                  },
                  (tab: SettingsNavTab): string => tab.key
                )
              }
              .width('100%')
            }
            .width('100%')

            Blank()

            Divider().color($r('sys.color.comp_divider')).opacity(0.35).margin({ top: Spacing.SM, bottom: Spacing.SM })

            Row({ space: Spacing.MD }) {
              SymbolGlyph($r('sys.symbol.wrench_and_screwdriver'))
                .fontSize(IconSize.SM)
                .fontColor([
                  this.activeTab === 'diag'
                    ? $r('sys.color.icon_emphasize')
                    : $r('sys.color.icon_secondary')
                ])
              Text('设备诊断')
                .fontSize(FontSize.BODY_MD)
                .fontWeight(this.activeTab === 'diag' ? FontWeight.Medium : FontWeight.Normal)
                .fontColor(
                  this.activeTab === 'diag'
                    ? $r('sys.color.font_emphasize')
                    : $r('sys.color.font_primary')
                )
            }
            .width('100%')
            .height(NAV_ITEM_H)
            .padding({ left: Spacing.SM, right: Spacing.MD })
            .borderRadius(Radius.MD)
            .backgroundColor(
              this.activeTab === 'diag'
                ? $r('sys.color.comp_emphasize_tertiary')
                : Color.Transparent
            )
            .animation({ duration: Motion.DUR_MICRO, curve: Motion.STD_CURVE })
            .hoverEffect(HoverEffect.Highlight)
            .clickEffect({ level: ClickEffectLevel.LIGHT, scale: 0.97 })
            .onClick(() => {
              this.switchTab('diag', -1);
            })
          }
          .width(172)
          .height('100%')
          .padding({ left: Spacing.LG, right: Spacing.MD, top: Spacing.LG, bottom: Spacing.LG })
          .border({ width: { right: 0.6 }, color: $r('sys.color.comp_divider') })

          // Right Content Area: Strictly Top-Aligned + Subtle 3vp Scrollbar + 24vp Right Baseline
          Scroll() {
            Column() {
              this.tabBodyRouter()
            }
            .width('100%')
            .padding({ left: Spacing.XL, right: Spacing.XXL, top: Spacing.LG, bottom: Spacing.LG })
          }
          .layoutWeight(1)
          .height('100%')
          .align(Alignment.TopStart)
          .scrollBar(BarState.Auto)
          .scrollBarWidth(3)
          .scrollBarColor($r('sys.color.comp_divider'))
        }
        .width('100%')
        .layoutWeight(1)
      }

      Divider().color($r('sys.color.comp_divider')).opacity(0.35)

      // Sheet Footer Bar (strictly 24vp left/right aligned with header and content cards)
      Row({ space: Spacing.MD }) {
        Button('恢复默认', { type: ButtonType.Capsule })
          .height(ControlSize.BTN_MD)
          .padding({ left: Spacing.MD, right: Spacing.MD })
          .fontSize(FontSize.BODY_MD)
          .fontColor($r('sys.color.font_secondary'))
          .backgroundColor(Color.Transparent)
          .hoverEffect(HoverEffect.Highlight)
          .clickEffect({ level: ClickEffectLevel.LIGHT, scale: 0.96 })
          .onClick(() => {
            this.resetToDefaults();
          })

        Blank()

        Button('取消', { type: ButtonType.Capsule })
          .height(ControlSize.BTN_MD)
          .padding({ left: Spacing.XL, right: Spacing.XL })
          .fontSize(FontSize.BODY_MD)
          .fontColor($r('sys.color.font_primary'))
          .backgroundColor($r('sys.color.comp_background_tertiary'))
          .hoverEffect(HoverEffect.Highlight)
          .clickEffect({ level: ClickEffectLevel.LIGHT, scale: 0.96 })
          .onClick(() => {
            this.onCancel();
          })

        Button('保存设置', { type: ButtonType.Capsule })
          .height(ControlSize.BTN_MD)
          .padding({ left: Spacing.XXL, right: Spacing.XXL })
          .fontSize(FontSize.BODY_MD)
          .fontWeight(FontWeight.Medium)
          .fontColor($r('sys.color.font_on_primary'))
          .backgroundColor($r('sys.color.brand'))
          .hoverEffect(HoverEffect.Highlight)
          .clickEffect({ level: ClickEffectLevel.LIGHT, scale: 0.96 })
          .onClick(() => {
            this.handleSave();
          })
      }
      .width('100%')
      .padding({ left: Spacing.XXL, right: Spacing.XXL, top: Spacing.MD, bottom: Spacing.LG })
    }
    .width('100%')
    .height('100%')
  }
}
