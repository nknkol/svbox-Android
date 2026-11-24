import { common, Want } from '@kit.AbilityKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import native, { DisplayStats, RuntimeStatus } from 'libentry.so';
import {
  AndroidAppItem,
  AppWindowPref,
  appWindowPrefs,
  computeDpiForSize,
  findPresetByKey,
  REFRESH_RATES,
  RESOLUTION_PRESETS,
  ResolutionPresetItem
} from '../common/AppWindowPrefs';
import { pinAndroidAppToDesktop, ShortcutOpResult } from '../common/DesktopShortcut';

const TAG: string = 'SvboxAppPage';

interface WmBasicReply {
  ok?: boolean;
  displayId?: number;
  error?: string;
}

@Entry({ useSharedStorage: true })
@Component
struct AppWindowPage {
  @LocalStorageProp('androidPkg') androidPkg: string = '';
  @LocalStorageProp('androidComp') androidComp: string = '';
  @LocalStorageProp('androidLabel') androidLabel: string = '';
  @LocalStorageProp('displaySlot') displaySlot: number = 1;
  @LocalStorageProp('moveTaskId') moveTaskId: number = -1;
  @LocalStorageProp('relaunchSeq') @Watch('onRelaunchSeq') relaunchSeq: number = 0;
  @LocalStorageProp('sandboxTempDir') sandboxTempDir: string = '';
  @LocalStorageProp('sandboxFilesDir') sandboxFilesDir: string = '';
  @LocalStorageProp('winPxW') @Watch('onWindowSizeChanged') winPxW: number = 640;
  @LocalStorageProp('winPxH') @Watch('onWindowSizeChanged') winPxH: number = 920;

  @State private winMode: string = 'adaptive';
  @State private presetKey: string = '720x1280';
  @State private curHz: number = 60;
  @State private dispW: number = 720;
  @State private dispH: number = 1080;
  @State private stageVpW: number = 0;
  @State private stageVpH: number = 0;
  @State private stageReady: boolean = false;
  @State private ready: boolean = false;
  @State private fps: number = 0;
  @State private frames: number = 0;
  @State private bannerText: string = '正在初始化 Android 14 独立窗口...';
  @State private iconUri: string = '';

  private bootPollTimer: number = -1;
  private resizeDebounceTimer: number = -1;
  private displayCreated: boolean = false;
  private curAppMeta: AndroidAppItem | undefined = undefined;
  private svboxPath: string = '/storage/Users/currentUser/Workspace/svbox/build/svbox';
  private androidDir: string = '/storage/Users/currentUser/Workspace/svbox-Android';

  private showToast(msg: string): void {
    try {
      this.getUIContext().getPromptAction().showToast({ message: msg, duration: 2000 });
    } catch (_e) {
    }
  }

  private lookupAppMeta(): AndroidAppItem | undefined {
    const cached: AndroidAppItem[] | undefined = AppStorage.get<AndroidAppItem[]>('androidApps');
    if (cached) {
      for (let i: number = 0; i < cached.length; i++) {
        if (cached[i].packageName === this.androidPkg) {
          return cached[i];
        }
      }
    }
    return undefined;
  }

  private computeAdaptiveTargetSize(): number[] {
    let physW: number = 720;
    let physH: number = 1080;
    if (this.stageVpW > 100 && this.stageVpH > 100) {
      try {
        const uiCtx = this.getUIContext();
        physW = Math.round(uiCtx.vp2px(this.stageVpW));
        physH = Math.round(uiCtx.vp2px(this.stageVpH));
      } catch (_e) {
        physW = Math.round(this.stageVpW * 2);
        physH = Math.round(this.stageVpH * 2);
      }
    } else if (this.winPxW > 200 && this.winPxH > 260) {
      physW = this.winPxW;
      physH = Math.max(320, this.winPxH - 160);
    }

    // Uniform scale down only if longer side exceeds 1600px, preserving exact 1:1 aspect ratio
    const maxSide: number = Math.max(physW, physH);
    const scale: number = maxSide > 1600 ? (1600 / maxSide) : 1.0;
    let targetW: number = Math.max(320, Math.round(physW * scale));
    let targetH: number = Math.max(320, Math.round(physH * scale));
    targetW = targetW - (targetW % 2);
    targetH = targetH - (targetH % 2);
    return [targetW, targetH];
  }

  private loadPrefs(): void {
    this.curAppMeta = this.lookupAppMeta();
    const globalHz: number = AppStorage.get<number>('refreshRate') ?? 60;
    const pref: AppWindowPref = appWindowPrefs.getPref(
      this.sandboxFilesDir, this.androidPkg, this.curAppMeta, globalHz
    );
    this.winMode = pref.mode;
    this.presetKey = pref.presetKey;
    this.curHz = pref.refreshRate;
    if (this.winMode === 'adaptive') {
      const sz: number[] = this.computeAdaptiveTargetSize();
      this.dispW = sz[0];
      this.dispH = sz[1];
    } else {
      const p: ResolutionPresetItem = findPresetByKey(this.presetKey);
      this.dispW = p.w;
      this.dispH = p.h;
    }
    this.refreshIconUri();
  }

  private refreshIconUri(): void {
    const iconsDir: string = native.getIconsDir();
    if (iconsDir.length > 0 && this.androidPkg.length > 0) {
      this.iconUri = `file://${iconsDir}/${this.androidPkg}.png`;
    }
  }

  private persistPref(): void {
    appWindowPrefs.savePref(this.sandboxFilesDir, this.androidPkg, {
      mode: this.winMode,
      presetKey: this.presetKey,
      refreshRate: this.curHz,
      winW: this.winPxW > 200 ? this.winPxW : 640,
      winH: this.winPxH > 200 ? this.winPxH : 920,
    });
  }

  aboutToAppear(): void {
    this.loadPrefs();
    try {
      native.setDisplayRefreshRate(this.displaySlot, this.curHz);
    } catch (_e) {
    }

    // Support direct cold-launch from HarmonyOS desktop shortcut even if EntryAbility isn't running yet
    const st: RuntimeStatus = native.getStatus();
    if (!st.running) {
      this.bannerText = `正在启动 Android 14 并打开「${this.androidLabel}」...`;
      const tempDir: string = this.sandboxTempDir.length > 0
        ? this.sandboxTempDir
        : '/data/storage/el2/base/haps/entry/temp';
      try {
        native.setGpuMode('vulkan');
        native.setRefreshRate(this.curHz);
        native.startContainer(tempDir, this.svboxPath, this.androidDir, 'full', 720, 1280);
        native.setDisplayVisible(0, false);
      } catch (_e) {
      }
    }

    this.bootPollTimer = setInterval(() => {
      this.tickStatus();
    }, 400);
    this.tickStatus();
  }

  aboutToDisappear(): void {
    if (this.bootPollTimer !== -1) {
      clearInterval(this.bootPollTimer);
      this.bootPollTimer = -1;
    }
    if (this.resizeDebounceTimer !== -1) {
      clearTimeout(this.resizeDebounceTimer);
      this.resizeDebounceTimer = -1;
    }
  }

  private tickStatus(): void {
    try {
      if (!this.displayCreated) {
        if (native.isWmAgentReady() && (this.stageReady || this.winMode !== 'adaptive')) {
          if (this.winMode === 'adaptive') {
            const sz: number[] = this.computeAdaptiveTargetSize();
            this.dispW = sz[0];
            this.dispH = sz[1];
          }
          this.refreshIconUri();
          this.createAndLaunchDisplay();
        } else {
          const st: RuntimeStatus = native.getStatus();
          this.bannerText = st.running
            ? `等待 Android 14 窗口服务就绪 (${st.statusText})...`
            : '正在启动 Android 14...';
        }
        return;
      }
      const ds: DisplayStats = native.getDisplayStats(this.displaySlot);
      this.frames = ds.framesPosted;
      this.fps = ds.fps;
      if (ds.width > 0 && ds.height > 0 && this.winMode === 'adaptive') {
        this.dispW = ds.width;
        this.dispH = ds.height;
      }
    } catch (_e) {
    }
  }

  private async createAndLaunchDisplay(): Promise<void> {
    if (this.displayCreated || this.displaySlot < 1) return;
    this.displayCreated = true;
    const dpi: number = computeDpiForSize(this.dispW, this.dispH);
    this.bannerText = `正在创建独立屏幕 #${this.displaySlot} (${this.dispW}×${this.dispH}@${this.curHz}Hz)...`;
    try {
      const createJson: string = await native.wmRpcAsync(
        `CREATE_DISPLAY ${this.displaySlot} ${this.dispW} ${this.dispH} ${dpi} ${this.curHz} ${this.androidPkg}`
      );
      const rep: WmBasicReply = JSON.parse(createJson) as WmBasicReply;
      if (!rep.ok) {
        this.displayCreated = false;
        this.bannerText = `建屏失败: ${rep.error ?? 'unknown'}`;
        return;
      }

      if (this.moveTaskId > 0) {
        await native.wmRpcAsync(`MOVE_TASK ${this.moveTaskId} ${this.displaySlot} ${this.androidPkg}`);
      } else {
        const compArg: string = this.androidComp.length > 0 ? ` ${this.androidComp}` : '';
        await native.wmRpcAsync(`LAUNCH_APP ${this.displaySlot} ${this.androidPkg}${compArg}`);
      }
      this.ready = true;
      this.bannerText = '';
      native.setDisplayRefreshRate(this.displaySlot, this.curHz);
      native.setDisplayVisible(this.displaySlot, true);
    } catch (err) {
      this.displayCreated = false;
      this.bannerText = `启动异常: ${String(err)}`;
      hilog.error(0x0000, TAG, 'createAndLaunchDisplay failed: %{public}s', String(err));
    }
  }

  private onRelaunchSeq(): void {
    if (!this.ready || this.displaySlot < 1) return;
    if (this.moveTaskId > 0) {
      native.wmRpcAsync(`MOVE_TASK ${this.moveTaskId} ${this.displaySlot} ${this.androidPkg}`);
    } else {
      native.wmRpcAsync(`FOCUS_DISPLAY ${this.displaySlot}`);
    }
  }

  private onWindowSizeChanged(): void {
    this.persistPref();
    this.scheduleAdaptiveResize();
  }

  private scheduleAdaptiveResize(): void {
    if (!this.ready || this.winMode !== 'adaptive' || !this.stageReady) return;
    if (this.resizeDebounceTimer !== -1) {
      clearTimeout(this.resizeDebounceTimer);
    }
    this.resizeDebounceTimer = setTimeout(() => {
      this.resizeDebounceTimer = -1;
      this.applyAdaptiveResize();
    }, 180);
  }

  private async applyAdaptiveResize(): Promise<void> {
    if (!this.ready || this.winMode !== 'adaptive' || this.displaySlot < 1 || !this.stageReady) return;
    const sz: number[] = this.computeAdaptiveTargetSize();
    const targetW: number = sz[0];
    const targetH: number = sz[1];
    if (Math.abs(targetW - this.dispW) < 4 && Math.abs(targetH - this.dispH) < 4) return;
    const dpi: number = computeDpiForSize(targetW, targetH);
    this.dispW = targetW;
    this.dispH = targetH;
    try {
      await native.wmRpcAsync(`RESIZE_DISPLAY ${this.displaySlot} ${targetW} ${targetH} ${dpi}`);
    } catch (_e) {
    }
  }

  private async applyFixedPreset(key: string): Promise<void> {
    const preset: ResolutionPresetItem = findPresetByKey(key);
    this.presetKey = preset.key;
    this.dispW = preset.w;
    this.dispH = preset.h;
    this.persistPref();
    if (this.ready && this.displaySlot >= 1) {
      const dpi: number = computeDpiForSize(preset.w, preset.h);
      try {
        await native.wmRpcAsync(`RESIZE_DISPLAY ${this.displaySlot} ${preset.w} ${preset.h} ${dpi}`);
      } catch (_e) {
      }
    }
  }

  private cycleWindowMode(): void {
    if (this.winMode === 'adaptive') {
      this.winMode = 'fixed_portrait';
      this.applyFixedPreset('720x1280');
      this.showToast('已切换为固定竖屏 (720×1280)');
    } else if (this.winMode === 'fixed_portrait') {
      this.winMode = 'fixed_landscape';
      this.applyFixedPreset('1280x720');
      this.showToast('已切换为固定横屏 (1280×720)');
    } else {
      this.winMode = 'adaptive';
      this.persistPref();
      this.applyAdaptiveResize();
      this.showToast('已切换为自适应窗口实时重排');
    }
  }

  private cycleFixedPreset(): void {
    let curIdx: number = 0;
    for (let i: number = 0; i < RESOLUTION_PRESETS.length; i++) {
      if (RESOLUTION_PRESETS[i].key === this.presetKey) {
        curIdx = i;
        break;
      }
    }
    const next: ResolutionPresetItem = RESOLUTION_PRESETS[(curIdx + 1) % RESOLUTION_PRESETS.length];
    this.winMode = next.w > next.h ? 'fixed_landscape' : 'fixed_portrait';
    this.applyFixedPreset(next.key);
    this.showToast(`分辨率已切至 ${next.label}`);
  }

  private cycleRefreshRate(): void {
    const idx: number = REFRESH_RATES.indexOf(this.curHz);
    this.curHz = REFRESH_RATES[(idx + 1) % REFRESH_RATES.length];
    this.persistPref();
    try {
      native.setDisplayRefreshRate(this.displaySlot, this.curHz);
    } catch (_e) {
    }
    this.showToast(`窗口刷新率: ${this.curHz}Hz`);
  }

  private async handlePinToDesktop(): Promise<void> {
    const iconsDir: string = native.getIconsDir();
    const iconPath: string = `${iconsDir}/${this.androidPkg}.png`;
    const ctx: common.UIAbilityContext = this.getUIContext().getHostContext() as common.UIAbilityContext;
    const item: AndroidAppItem = this.curAppMeta ?? {
      packageName: this.androidPkg,
      component: this.androidComp,
      label: this.androidLabel,
      iconFile: `${this.androidPkg}.png`,
      resizeable: this.winMode === 'adaptive',
      isGame: false,
      isSystem: false,
      orientation: -1,
      defaultMode: this.winMode,
    };
    const res: ShortcutOpResult = await pinAndroidAppToDesktop(ctx, item, iconPath);
    this.showToast(res.message);
  }

  private async handlePullBackToEmulator(): Promise<void> {
    try {
      await native.wmRpcAsync(`PULL_BACK_TO_EMULATOR ${this.displaySlot}`);
      const storage: LocalStorage | undefined = this.getUIContext().getSharedLocalStorage();
      if (storage) {
        storage.setOrCreate<boolean>('pulledBack', true);
      }
      AppStorage.setOrCreate<string>('requestedUiMode', 'emulator');
      const ctx: common.UIAbilityContext = this.getUIContext().getHostContext() as common.UIAbilityContext;
      const want: Want = {
        bundleName: 'org.svbox.android',
        moduleName: 'entry',
        abilityName: 'EntryAbility',
        parameters: { 'uiMode': 'emulator' }
      };
      ctx.startAbility(want).catch((_err: Error) => {});
      ctx.terminateSelf().catch((_err: Error) => {});
    } catch (_e) {
    }
  }

  private modeBadgeLabel(): string {
    if (this.winMode === 'adaptive') return '自适应';
    if (this.winMode === 'fixed_landscape') return '固定横';
    return '固定竖';
  }

  private computeFitWidth(): number {
    if (!this.stageReady || this.stageVpW <= 0 || this.stageVpH <= 0) {
      return 120;
    }
    const scaleFactor: number = this.winMode === 'adaptive' ? 1.0 : 0.98;
    const padW: number = Math.max(120, this.stageVpW * scaleFactor);
    const padH: number = Math.max(120, this.stageVpH * scaleFactor);
    const aspect: number = this.dispW / Math.max(1, this.dispH);
    if (padW / padH > aspect) {
      return Math.max(120, Math.round(padH * aspect));
    }
    return Math.max(120, Math.round(padW));
  }

  private computeFitHeight(): number {
    if (!this.stageReady || this.stageVpW <= 0 || this.stageVpH <= 0) {
      return 120;
    }
    const scaleFactor: number = this.winMode === 'adaptive' ? 1.0 : 0.98;
    const padW: number = Math.max(120, this.stageVpW * scaleFactor);
    const padH: number = Math.max(120, this.stageVpH * scaleFactor);
    const aspect: number = this.dispW / Math.max(1, this.dispH);
    if (padW / padH > aspect) {
      return Math.max(120, Math.round(padH));
    }
    return Math.max(120, Math.round(padW / aspect));
  }

  build() {
    Column() {
      // Compact HarmonyOS PC native chrome bar for single-app window
      Row({ space: 6 }) {
        Button('◀')
          .height(26)
          .fontSize(11)
          .backgroundColor('#272C36')
          .onClick(() => {
            if (this.ready) {
              native.wmRpcAsync(`SEND_KEY ${this.displaySlot} 4`);
            }
          })

        if (this.iconUri.length > 0) {
          Image(this.iconUri)
            .width(20)
            .height(20)
            .borderRadius(4)
            .objectFit(ImageFit.Contain)
        }

        Text(this.androidLabel.length > 0 ? this.androidLabel : this.androidPkg)
          .fontSize(13)
          .fontWeight(FontWeight.Medium)
          .fontColor('#F3F4F6')
          .maxLines(1)
          .textOverflow({ overflow: TextOverflow.Ellipsis })
          .constraintSize({ maxWidth: 160 })

        Text(`${this.dispW}×${this.dispH} · ${this.fps}/${this.curHz}Hz`)
          .fontSize(10)
          .fontColor('#9CA3AF')
          .padding({ left: 6, right: 6, top: 3, bottom: 3 })
          .backgroundColor('#1F242D')
          .borderRadius(4)
          .maxLines(1)

        Blank()

        Button(this.modeBadgeLabel())
          .height(26)
          .fontSize(11)
          .backgroundColor(this.winMode === 'adaptive' ? '#0284C7' : '#4B5563')
          .onClick(() => {
            this.cycleWindowMode();
          })

        if (this.winMode !== 'adaptive') {
          Button(findPresetByKey(this.presetKey).label)
            .height(26)
            .fontSize(10)
            .backgroundColor('#374151')
            .onClick(() => {
              this.cycleFixedPreset();
            })
        }

        Button(`${this.curHz}Hz`)
          .height(26)
          .fontSize(11)
          .backgroundColor(this.curHz === 120 ? '#059669' : (this.curHz === 90 ? '#0284C7' : '#374151'))
          .onClick(() => {
            this.cycleRefreshRate();
          })

        Button('加桌')
          .height(26)
          .fontSize(11)
          .backgroundColor('#374151')
          .onClick(() => {
            this.handlePinToDesktop();
          })

        Button('收回')
          .height(26)
          .fontSize(11)
          .backgroundColor('#374151')
          .onClick(() => {
            this.handlePullBackToEmulator();
          })
      }
      .width('100%')
      .height(40)
      .padding({ left: 10, right: 10 })
      .backgroundColor('#161920')
      .alignItems(VerticalAlign.Center)

      // Zero-copy Vulkan surface viewport for this Android VirtualDisplay
      Stack({ alignContent: Alignment.Center }) {
        XComponent({
          id: `svbox_display_${this.displaySlot}`,
          type: XComponentType.SURFACE,
          libraryname: 'entry'
        })
          .focusable(true)
          .defaultFocus(true)
          .width(this.computeFitWidth())
          .height(this.computeFitHeight())
          .backgroundColor('#000000')

        if (!this.ready || this.bannerText.length > 0) {
          Column({ space: 10 }) {
            LoadingProgress()
              .width(36)
              .height(36)
              .color('#38BDF8')
            Text(this.bannerText.length > 0 ? this.bannerText : '正在渲染...')
              .fontSize(13)
              .fontColor('#D1D5DB')
          }
          .padding(20)
          .backgroundColor('#CC11141B')
          .borderRadius(12)
        }
      }
      .width('100%')
      .layoutWeight(1)
      .backgroundColor('#0B0D12')
      .onAreaChange((_oldVal: Area, newVal: Area) => {
        const w: number = Number(newVal.width);
        const h: number = Number(newVal.height);
        if (w > 100 && h > 100) {
          const changed: boolean = Math.abs(w - this.stageVpW) >= 1 || Math.abs(h - this.stageVpH) >= 1;
          this.stageVpW = w;
          this.stageVpH = h;
          if (!this.stageReady) {
            this.stageReady = true;
            if (this.winMode === 'adaptive') {
              const sz: number[] = this.computeAdaptiveTargetSize();
              this.dispW = sz[0];
              this.dispH = sz[1];
            }
            this.tickStatus();
          } else if (changed) {
            this.scheduleAdaptiveResize();
          }
        }
      })
    }
    .width('100%')
    .height('100%')
    .backgroundColor('#0B0D12')
  }
}
