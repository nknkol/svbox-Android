import { common } from '@kit.AbilityKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { window } from '@kit.ArkUI';
import native, { DisplayStats, RuntimeStatus } from 'libentry.so';
import {
  AndroidAppItem,
  AppWindowPref,
  appWindowPrefs,
  computeAdaptiveDpi,
  computeDpiForSize,
  findPresetByKey,
  getCachedIconsDir,
  loadCachedAppList,
  ResolutionPresetItem
} from '../common/AppWindowPrefs';
import {
  pasteHostClipboardToAndroid,
  sendTextToAndroid,
  syncHostClipboardToAndroid
} from '../common/InputBridgeHelper';
import { DeviceInstance, instanceManager } from '../model/InstanceModel';
import {
  applyMissionIconAndQuickBar,
  argbToHex,
  getSlotWindow,
  isColorDark,
  updateDecorButtonTheme
} from '../common/WindowHelper';

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
  @LocalStorageProp('winPxW') @Watch('onWindowSizeChanged') winPxW: number = 860;
  @LocalStorageProp('winPxH') @Watch('onWindowSizeChanged') winPxH: number = 1380;
  @LocalStorageProp('decorRightAvoidVp') decorRightAvoidVp: number = 136;
  @LocalStorageProp('iconSetupStarted') iconSetupStarted: boolean = false;

  @State private winMode: string = 'adaptive';
  @State private presetKey: string = '720x1280';
  @State private curHz: number = 60;
  @State private dispW: number = 720;
  @State private dispH: number = 1080;
  @State private stageVpW: number = 0;
  @State private stageVpH: number = 0;
  @State private stageReady: boolean = false;
  @State private ready: boolean = false;
  @State private bannerText: string = '正在初始化 Android 14 应用窗口...';
  @State private iconUri: string = '';
  @State private topBarHex: string = '#161920';
  @State private isDarkBar: boolean = true;
  @State private showImeBar: boolean = false;
  @State private imeDraft: string = '';

  private bootPollTimer: number = -1;
  private resizeDebounceTimer: number = -1;
  private resizeInFlight: boolean = false;
  private pendingResizeW: number = 0;
  private pendingResizeH: number = 0;
  private lastAppliedW: number = 0;
  private lastAppliedH: number = 0;
  private displayCreated: boolean = false;
  private iconApplied: boolean = false;
  private lastSampledArgb: number = 0;
  private curAppMeta: AndroidAppItem | undefined = undefined;
  private svboxPath: string = '/storage/Users/currentUser/Workspace/svbox/build/svbox';
  private androidDir: string = '/storage/Users/currentUser/Workspace/svbox-Android';

  private lookupAppMeta(): AndroidAppItem | undefined {
    let cached: AndroidAppItem[] | undefined = AppStorage.get<AndroidAppItem[]>('androidApps');
    if (!cached || cached.length === 0) {
      const filesDir: string = this.sandboxFilesDir.length > 0
        ? this.sandboxFilesDir : '/data/storage/el2/base/haps/entry/files';
      const primary: DeviceInstance | undefined =
        instanceManager.getById(filesDir, 'inst_0') ?? instanceManager.getAll(filesDir)[0];
      cached = loadCachedAppList(filesDir, primary ? primary.dataSubdir : 'userdata14');
    }
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
    let physW: number = 860;
    let physH: number = 1300;
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
      physH = Math.max(320, this.winPxH - 80);
    }

    // Uniform scale down only if longer side exceeds 2400px, keeping 1:1 aspect ratio
    const maxSide: number = Math.max(physW, physH);
    const scale: number = maxSide > 2400 ? (2400 / maxSide) : 1.0;
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
    const filesDir: string = this.sandboxFilesDir.length > 0
      ? this.sandboxFilesDir : '/data/storage/el2/base/haps/entry/files';
    const primary: DeviceInstance | undefined = instanceManager.getById(filesDir, 'inst_0') ?? instanceManager.getAll(filesDir)[0];
    const fallbackIconsDir: string = getCachedIconsDir(filesDir, primary ? primary.dataSubdir : 'userdata14');
    const runtimeIconsDir: string = native.getIconsDir();
    const iconsDir: string = runtimeIconsDir.length > 0 ? runtimeIconsDir : fallbackIconsDir;
    if (iconsDir.length > 0 && this.androidPkg.length > 0) {
      this.iconUri = `file://${iconsDir}/${this.androidPkg}.png`;
      if (!this.iconApplied && !this.iconSetupStarted) {
        const win: window.Window | undefined = getSlotWindow(this.displaySlot);
        const ctx: common.UIAbilityContext | undefined =
          this.getUIContext().getHostContext() as common.UIAbilityContext;
        if (win && ctx) {
          this.iconApplied = true;
          const label: string = this.androidLabel.length > 0 ? this.androidLabel : this.androidPkg;
          applyMissionIconAndQuickBar(ctx, win, this.androidPkg, label, `${iconsDir}/${this.androidPkg}.png`);
        }
      }
    }
  }

  private persistPref(): void {
    appWindowPrefs.savePref(this.sandboxFilesDir, this.androidPkg, {
      mode: this.winMode,
      presetKey: this.presetKey,
      refreshRate: this.curHz,
      winW: this.winPxW > 200 ? this.winPxW : 860,
      winH: this.winPxH > 200 ? this.winPxH : 1380,
    });
  }

  aboutToAppear(): void {
    this.loadPrefs();
    try {
      native.setDisplayRefreshRate(this.displaySlot, this.curHz);
    } catch (_e) {
    }

    // Direct cold-launch from HarmonyOS desktop shortcut or offline app list click
    const st: RuntimeStatus = native.getStatus();
    if (!st.running) {
      this.bannerText = `正在启动 Android 14 并打开「${this.androidLabel}」...`;
      const tempDir: string = this.sandboxTempDir.length > 0
        ? this.sandboxTempDir
        : '/data/storage/el2/base/haps/entry/temp';
      const filesDir: string = this.sandboxFilesDir.length > 0
        ? this.sandboxFilesDir
        : '/data/storage/el2/base/haps/entry/files';
      const primary: DeviceInstance | undefined =
        instanceManager.getById(filesDir, 'inst_0') ?? instanceManager.getAll(filesDir)[0];
      try {
        if (primary) {
          native.setInstanceConfig(
            primary.id, primary.dataSubdir, primary.dpi, primary.enableAudio,
            primary.enableRoot, primary.deviceBrand, primary.deviceModel, primary.adbPort, primary.enableGms
          );
          native.setResourceConfig(
            primary.profile, '', primary.memLimitMb, primary.diskLimitGb, primary.cpuCores
          );
          native.setGpuMode(primary.gpuMode);
          native.setSyscallMode(primary.syscallMode || 'adaptive');
          native.setRefreshRate(primary.refreshRate);
          native.startContainer(tempDir, this.svboxPath, this.androidDir, 'full', primary.width, primary.height);
        } else {
          native.setGpuMode('vulkan');
          native.setRefreshRate(this.curHz);
          native.startContainer(tempDir, this.svboxPath, this.androidDir, 'full', 720, 1280);
        }
        native.setDisplayVisible(0, false);
      } catch (_e) {
      }
    }

    this.bootPollTimer = setInterval(() => {
      this.tickStatus();
    }, 350);
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

  private applySampledTopColor(argb: number): void {
    if (argb === 0 || argb === this.lastSampledArgb) return;
    this.lastSampledArgb = argb;
    const hex: string = argbToHex(argb);
    const dark: boolean = isColorDark(argb);
    this.topBarHex = hex;
    hilog.info(0x0000, TAG, 'immersive topBar slot=%{public}d color=%{public}s dark=%{public}d',
      this.displaySlot, hex, dark ? 1 : 0);
    if (dark !== this.isDarkBar) {
      this.isDarkBar = dark;
      updateDecorButtonTheme(getSlotWindow(this.displaySlot), dark);
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
      if (ds.topBarColor && ds.topBarColor !== 0) {
        this.applySampledTopColor(ds.topBarColor);
      }
    } catch (_e) {
    }
  }

  private applyWindowAspectPolicy(needUpdateRect: boolean): void {
    const win: window.Window | undefined = getSlotWindow(this.displaySlot);
    if (!win) return;
    if (this.winMode === 'adaptive') {
      try {
        win.resetAspectRatio().catch(() => {});
      } catch (_e) {
      }
    } else {
      const isLand: boolean = this.dispW > this.dispH;
      // Include 38vp title bar (~64px at typical desktop scale) so XComponent fits edge-to-edge without black bars.
      const ratio: number = isLand ? (this.dispW / (this.dispH + 64.0)) : (this.dispW / (this.dispH + 64.0));
      try {
        win.setContentAspectRatio(ratio, false, needUpdateRect).catch(() => {});
      } catch (_e) {
      }
    }
  }

  private modePillLabel(): string {
    if (this.winMode === 'adaptive') return '支持·自由';
    if (this.winMode === 'fixed_landscape') return '兼容·横屏';
    return '兼容·竖屏';
  }

  private async switchWindowMode(nextMode: string): Promise<void> {
    if (this.winMode === nextMode) return;
    const win: window.Window | undefined = getSlotWindow(this.displaySlot);
    this.winMode = nextMode;
    if (nextMode === 'adaptive') {
      this.applyWindowAspectPolicy(false);
      const sz: number[] = this.computeAdaptiveTargetSize();
      this.dispW = sz[0];
      this.dispH = sz[1];
      this.persistPref();
      if (this.ready && this.displaySlot >= 1) {
        this.queueAdaptiveResize(sz[0], sz[1]);
      }
      return;
    }

    const isLand: boolean = nextMode === 'fixed_landscape';
    const preset: ResolutionPresetItem = findPresetByKey(isLand ? '1280x720' : '720x1280');
    this.presetKey = preset.key;
    this.dispW = preset.w;
    this.dispH = preset.h;
    this.lastAppliedW = preset.w;
    this.lastAppliedH = preset.h;
    this.pendingResizeW = 0;
    this.pendingResizeH = 0;
    if (win) {
      try {
        await win.resize(isLand ? 1480 : 860, isLand ? 940 : 1380);
      } catch (_e) {
      }
    }
    this.applyWindowAspectPolicy(true);
    this.persistPref();
    if (this.ready && this.displaySlot >= 1) {
      const dpi: number = computeDpiForSize(preset.w, preset.h);
      try {
        await native.wmRpcAsync(`RESIZE_DISPLAY ${this.displaySlot} ${preset.w} ${preset.h} ${dpi}`);
      } catch (_e) {
      }
    }
  }

  private async handleSubmitImeText(): Promise<void> {
    const txt: string = this.imeDraft;
    if (txt.length === 0 || !this.ready) return;
    this.imeDraft = '';
    await sendTextToAndroid(this.displaySlot, txt);
  }

  private async handlePasteClipboard(): Promise<void> {
    if (!this.ready) return;
    await pasteHostClipboardToAndroid(this.displaySlot);
  }

  @Builder
  private modeMenuBuilder() {
    Menu() {
      MenuItem({ content: this.winMode === 'adaptive' ? '✓ 支持模式（自由调整尺寸）' : '支持模式（自由调整尺寸）' })
        .onClick(() => { this.switchWindowMode('adaptive'); })
      MenuItem({ content: this.winMode === 'fixed_portrait' ? '✓ 兼容模式 · 竖屏（等比缩放）' : '兼容模式 · 竖屏（等比缩放）' })
        .onClick(() => { this.switchWindowMode('fixed_portrait'); })
      MenuItem({ content: this.winMode === 'fixed_landscape' ? '✓ 兼容模式 · 横屏（等比缩放）' : '兼容模式 · 横屏（等比缩放）' })
        .onClick(() => { this.switchWindowMode('fixed_landscape'); })
      MenuItem({ content: '粘贴鸿蒙剪贴板到此窗口' })
        .enabled(this.ready)
        .onClick(() => { this.handlePasteClipboard(); })
    }
  }

  private async createAndLaunchDisplay(): Promise<void> {
    if (this.displayCreated || this.displaySlot < 1) return;
    this.displayCreated = true;
    const dpi: number = this.winMode === 'adaptive'
      ? computeAdaptiveDpi(this.dispW, this.dispH)
      : computeDpiForSize(this.dispW, this.dispH);
    this.bannerText = `正在打开「${this.androidLabel}」...`;
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
      this.lastAppliedW = this.dispW;
      this.lastAppliedH = this.dispH;
      this.applyWindowAspectPolicy(false);
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
      const sz: number[] = this.computeAdaptiveTargetSize();
      this.queueAdaptiveResize(sz[0], sz[1]);
    }, 80);
  }

  private queueAdaptiveResize(targetW: number, targetH: number): void {
    if (!this.ready || this.winMode !== 'adaptive' || this.displaySlot < 1) return;
    if (Math.abs(targetW - this.lastAppliedW) < 4 && Math.abs(targetH - this.lastAppliedH) < 4 && !this.resizeInFlight) {
      return;
    }
    this.pendingResizeW = targetW;
    this.pendingResizeH = targetH;
    if (this.resizeInFlight) {
      return;
    }
    this.drainAdaptiveResizeQueue();
  }

  private async drainAdaptiveResizeQueue(): Promise<void> {
    if (this.resizeInFlight) return;
    this.resizeInFlight = true;
    try {
      while (this.pendingResizeW > 0 && this.pendingResizeH > 0 && this.winMode === 'adaptive' && this.ready) {
        const w: number = this.pendingResizeW;
        const h: number = this.pendingResizeH;
        this.pendingResizeW = 0;
        this.pendingResizeH = 0;
        if (Math.abs(w - this.lastAppliedW) < 4 && Math.abs(h - this.lastAppliedH) < 4) {
          continue;
        }
        const dpi: number = computeAdaptiveDpi(w, h);
        this.dispW = w;
        this.dispH = h;
        try {
          await native.wmRpcAsync(`RESIZE_DISPLAY ${this.displaySlot} ${w} ${h} ${dpi}`);
          this.lastAppliedW = w;
          this.lastAppliedH = h;
        } catch (_e) {
          break;
        }
      }
    } finally {
      this.resizeInFlight = false;
    }
  }

  private async toggleOrientation(): Promise<void> {
    const win: window.Window | undefined = getSlotWindow(this.displaySlot);
    const isLandscapeNow: boolean = this.dispW > this.dispH;
    if (this.winMode === 'adaptive') {
      const targetWinW: number = isLandscapeNow ? 860 : 1480;
      const targetWinH: number = isLandscapeNow ? 1380 : 940;
      if (win) {
        try {
          await win.resize(targetWinW, targetWinH);
        } catch (_e) {
        }
      }
      return;
    }

    // Compat mode: switch between fixed_portrait (720x1280) and fixed_landscape (1280x720)
    await this.switchWindowMode(isLandscapeNow ? 'fixed_portrait' : 'fixed_landscape');
  }

  private computeFitWidth(): number {
    if (!this.stageReady || this.stageVpW <= 0 || this.stageVpH <= 0) {
      return 120;
    }
    const scaleFactor: number = this.winMode === 'adaptive' ? 1.0 : 0.99;
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
    const scaleFactor: number = this.winMode === 'adaptive' ? 1.0 : 0.99;
    const padW: number = Math.max(120, this.stageVpW * scaleFactor);
    const padH: number = Math.max(120, this.stageVpH * scaleFactor);
    const aspect: number = this.dispW / Math.max(1, this.dispH);
    if (padW / padH > aspect) {
      return Math.max(120, Math.round(padH));
    }
    return Math.max(120, Math.round(padW / aspect));
  }

  private titleInkColor(): string {
    return this.isDarkBar ? '#F3F4F6' : '#181C24';
  }

  private buttonHoverBg(): string {
    return this.isDarkBar ? '#1FFFFFFF' : '#14000000';
  }

  build() {
    Column() {
      // Single Immersive System-Integrated Title Bar (38vp, dynamically tinted to Android app top bar)
      Row({ space: 6 }) {
        // Back button (native HarmonyOS SymbolGlyph)
        Button({ type: ButtonType.Normal }) {
          SymbolGlyph($r('sys.symbol.chevron_left'))
            .fontSize(16)
            .fontColor([this.titleInkColor()])
        }
        .width(28)
        .height(28)
        .borderRadius(6)
        .backgroundColor(Color.Transparent)
        .stateStyles({
          pressed: { .backgroundColor(this.buttonHoverBg()) }
        })
        .onClick(() => {
          if (this.ready) {
            native.wmRpcAsync(`SEND_KEY ${this.displaySlot} 4`);
          }
        })

        if (this.iconUri.length > 0) {
          Image(this.iconUri)
            .width(18)
            .height(18)
            .borderRadius(4)
            .objectFit(ImageFit.Contain)
        }

        Text(this.androidLabel.length > 0 ? this.androidLabel : this.androidPkg)
          .fontSize(13)
          .fontWeight(FontWeight.Medium)
          .fontColor(this.titleInkColor())
          .maxLines(1)
          .textOverflow({ overflow: TextOverflow.Ellipsis })
          .layoutWeight(1)

        // Support Mode vs Compat Mode switcher pill + IME Bridge + Orientation toggle
        Button({ type: ButtonType.Normal }) {
          Text(this.modePillLabel())
            .fontSize(11)
            .fontWeight(FontWeight.Medium)
            .fontColor(this.titleInkColor())
            .maxLines(1)
        }
        .height(24)
        .padding({ left: 8, right: 8 })
        .borderRadius(12)
        .backgroundColor(this.buttonHoverBg())
        .bindMenu(this.modeMenuBuilder())

        Button({ type: ButtonType.Normal }) {
          SymbolGlyph($r('sys.symbol.keyboard'))
            .fontSize(15)
            .fontColor([this.showImeBar ? '#38BDF8' : this.titleInkColor()])
        }
        .width(28)
        .height(28)
        .borderRadius(6)
        .backgroundColor(Color.Transparent)
        .stateStyles({
          pressed: { .backgroundColor(this.buttonHoverBg()) }
        })
        .onClick(() => {
          this.showImeBar = !this.showImeBar;
          if (this.showImeBar) {
            syncHostClipboardToAndroid(false);
          }
        })

        // Single Portrait/Landscape toggle button placed directly left of system 3-button area
        Button({ type: ButtonType.Normal }) {
          SymbolGlyph($r('sys.symbol.screen_rotation'))
            .fontSize(16)
            .fontColor([this.titleInkColor()])
        }
        .width(28)
        .height(28)
        .borderRadius(6)
        .backgroundColor(Color.Transparent)
        .stateStyles({
          pressed: { .backgroundColor(this.buttonHoverBg()) }
        })
        .onClick(() => {
          this.toggleOrientation();
        })
      }
      .width('100%')
      .height(38)
      .padding({ left: 8, right: Math.max(152, this.decorRightAvoidVp) })
      .backgroundColor(this.topBarHex)
      .animation({ duration: 220, curve: Curve.EaseOut })
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
          .width(this.winMode === 'adaptive' ? '100%' : this.computeFitWidth())
          .height(this.winMode === 'adaptive' ? '100%' : this.computeFitHeight())
          .backgroundColor('#000000')

        if (!this.ready || this.bannerText.length > 0) {
          Column({ space: 10 }) {
            LoadingProgress()
              .width(34)
              .height(34)
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
      .backgroundColor(this.topBarHex)
      .animation({ duration: 220, curve: Curve.EaseOut })
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

      if (this.showImeBar) {
        Row({ space: 8 }) {
          TextInput({ placeholder: '鸿蒙中文输入法桥接：输入后回车发送至当前窗口', text: this.imeDraft })
            .layoutWeight(1)
            .height(32)
            .fontSize(13)
            .fontColor('#F3F4F6')
            .placeholderColor('#9CA3AF')
            .backgroundColor('#262B36')
            .borderRadius(8)
            .enterKeyType(EnterKeyType.Send)
            .onChange((val: string) => { this.imeDraft = val; })
            .onSubmit(() => { this.handleSubmitImeText(); })

          Button('发送', { type: ButtonType.Capsule })
            .height(30)
            .padding({ left: 12, right: 12 })
            .fontSize(12)
            .fontColor('#FFFFFF')
            .backgroundColor('#0A59F7')
            .enabled(this.imeDraft.length > 0 && this.ready)
            .onClick(() => { this.handleSubmitImeText(); })

          PasteButton({ text: PasteDescription.PASTE })
            .height(30)
            .fontSize(12)
            .fontColor('#F3F4F6')
            .backgroundColor('#374151')
            .onClick((_ev: ClickEvent, res: PasteButtonOnClickResult) => {
              if (res === PasteButtonOnClickResult.SUCCESS) {
                this.handlePasteClipboard();
              }
            })
        }
        .width('100%')
        .padding({ left: 10, right: 10, top: 6, bottom: 6 })
        .backgroundColor('#161920')
        .alignItems(VerticalAlign.Center)
      }
    }
    .width('100%')
    .height('100%')
    .backgroundColor(this.topBarHex)
  }
}
