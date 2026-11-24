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
  ResolutionPresetItem
} from '../common/AppWindowPrefs';
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

  private bootPollTimer: number = -1;
  private resizeDebounceTimer: number = -1;
  private displayCreated: boolean = false;
  private iconApplied: boolean = false;
  private lastSampledArgb: number = 0;
  private curAppMeta: AndroidAppItem | undefined = undefined;
  private svboxPath: string = '/storage/Users/currentUser/Workspace/svbox/build/svbox';
  private androidDir: string = '/storage/Users/currentUser/Workspace/svbox-Android';

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
    const iconsDir: string = native.getIconsDir();
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

    // Direct cold-launch from HarmonyOS desktop shortcut even if EntryAbility isn't running yet
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
      if (ds.width > 0 && ds.height > 0 && this.winMode === 'adaptive') {
        this.dispW = ds.width;
        this.dispH = ds.height;
      }
      if (ds.topBarColor && ds.topBarColor !== 0) {
        this.applySampledTopColor(ds.topBarColor);
      }
    } catch (_e) {
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
    const dpi: number = computeAdaptiveDpi(targetW, targetH);
    this.dispW = targetW;
    this.dispH = targetH;
    try {
      await native.wmRpcAsync(`RESIZE_DISPLAY ${this.displaySlot} ${targetW} ${targetH} ${dpi}`);
    } catch (_e) {
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

    // Fixed mode: switch between fixed_portrait (720x1280) and fixed_landscape (1280x720)
    const nextKey: string = isLandscapeNow ? '720x1280' : '1280x720';
    this.winMode = isLandscapeNow ? 'fixed_portrait' : 'fixed_landscape';
    const preset: ResolutionPresetItem = findPresetByKey(nextKey);
    this.presetKey = preset.key;
    this.dispW = preset.w;
    this.dispH = preset.h;
    if (win) {
      try {
        await win.resize(isLandscapeNow ? 860 : 1480, isLandscapeNow ? 1380 : 940);
      } catch (_e) {
      }
    }
    this.persistPref();
    if (this.ready && this.displaySlot >= 1) {
      const dpi: number = computeDpiForSize(preset.w, preset.h);
      try {
        await native.wmRpcAsync(`RESIZE_DISPLAY ${this.displaySlot} ${preset.w} ${preset.h} ${dpi}`);
      } catch (_e) {
      }
    }
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
          .width(this.computeFitWidth())
          .height(this.computeFitHeight())
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
    }
    .width('100%')
    .height('100%')
    .backgroundColor(this.topBarHex)
  }
}
