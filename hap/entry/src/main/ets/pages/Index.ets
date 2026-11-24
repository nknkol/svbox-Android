import { common, Want } from '@kit.AbilityKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import native, { RuntimeStatus } from 'libentry.so';
import {
  AndroidAppItem,
  AppWindowPref,
  appWindowPrefs,
  computeShortcutId,
  REFRESH_RATES,
  RESOLUTION_PRESETS,
  WmEventItem
} from '../common/AppWindowPrefs';
import {
  pinAndroidAppToDesktop,
  queryPinnedShortcutIds,
  removeAndroidAppShortcut,
  ShortcutOpResult
} from '../common/DesktopShortcut';

const TAG: string = 'SvboxIndex';

interface ListAppsReply {
  ok?: boolean;
  apps?: AndroidAppItem[];
  error?: string;
}

interface PopOutReply {
  ok?: boolean;
  taskId?: number;
  packageName?: string;
  component?: string;
  label?: string;
  error?: string;
}

@Entry
@Component
struct Index {
  // Default to Free Multi-Window Mode ('multiwindow'), switchable live to 'emulator'
  @State private uiMode: string = 'multiwindow';
  @State private running: boolean = false;
  @State private paused: boolean = false;
  @State private connected: boolean = false;
  @State private wmAgentReady: boolean = false;
  @State private vsyncActive: boolean = false;
  @State private inputReady: boolean = false;
  @State private audioActive: boolean = false;
  @State private frames: number = 0;
  @State private fps: number = 0;
  @State private refreshRate: number = 60;
  @State private resIdx: number = 0;
  @State private dispW: number = 720;
  @State private dispH: number = 1280;
  @State private stageW: number = 960;
  @State private stageH: number = 720;
  @State private cpuPercent: number = 0;
  @State private memMb: number = 0;
  @State private memLimitMb: number = 4096;
  @State private activeProcs: number = 0;
  @State private profile: string = 'perf';
  @State private gpuMode: string = 'vulkan';
  @State private surfaceEpoch: number = 0;
  @State private gpuRenderer: string = '';
  @State private cpuset: string = '';
  @State private topology: string = '';
  @State private statusText: string = '正在初始化 Android 14 自由多窗环境...';
  @State private svboxPath: string = '/storage/Users/currentUser/Workspace/svbox/build/svbox';
  @State private androidDir: string = '/storage/Users/currentUser/Workspace/svbox-Android';

  // Multi-window App Launcher state
  @State private appList: AndroidAppItem[] = [];
  @State private iconsDir: string = '';
  @State private searchQuery: string = '';
  @State private appFilter: string = 'all'; // 'all' | 'user' | 'system'
  @State private loadingApps: boolean = false;
  @State private pinnedIds: string[] = [];
  @State private prefEpoch: number = 0;

  private pollTimer: number = -1;
  private lastLaunchSeq: number = 0;
  private appsFetchedOnce: boolean = false;
  private autoBootTriggered: boolean = false;

  private showToast(msg: string): void {
    try {
      this.getUIContext().getPromptAction().showToast({ message: msg, duration: 2000 });
    } catch (_e) {
    }
  }

  private getSandboxTempDir(): string {
    const dir: string | undefined = AppStorage.get<string>('sandboxTempDir');
    return dir && dir.length > 0 ? dir : '/data/storage/el2/base/haps/entry/temp';
  }

  private getSandboxFilesDir(): string {
    const dir: string | undefined = AppStorage.get<string>('sandboxFilesDir');
    return dir && dir.length > 0 ? dir : '/data/storage/el2/base/haps/entry/files';
  }

  private applyResolutionKey(key: string): boolean {
    for (let i: number = 0; i < RESOLUTION_PRESETS.length; i++) {
      if (RESOLUTION_PRESETS[i].key === key) {
        const changed: boolean = (this.resIdx !== i);
        this.resIdx = i;
        this.dispW = RESOLUTION_PRESETS[i].w;
        this.dispH = RESOLUTION_PRESETS[i].h;
        return changed;
      }
    }
    return false;
  }

  private setUiMode(nextMode: string): void {
    if (nextMode !== 'multiwindow' && nextMode !== 'emulator') return;
    this.uiMode = nextMode;
    AppStorage.setOrCreate<string>('activeUiMode', nextMode);
    AppStorage.setOrCreate<string>('requestedUiMode', '');
    try {
      native.setDisplayVisible(0, nextMode === 'emulator');
      if (nextMode === 'emulator' && this.wmAgentReady) {
        native.wmRpcAsync('FOCUS_DISPLAY 0');
      }
    } catch (_e) {
    }
    if (nextMode === 'multiwindow' && this.wmAgentReady && this.appList.length === 0) {
      this.refreshApps(false);
    }
  }

  private computeFitWidth(): number {
    const padW: number = Math.max(120, this.stageW * 0.96);
    const padH: number = Math.max(120, this.stageH * 0.96);
    const aspect: number = this.dispW / Math.max(1, this.dispH);
    if (padW / padH > aspect) {
      return Math.floor(padH * aspect);
    }
    return Math.floor(padW);
  }

  private computeFitHeight(): number {
    const padW: number = Math.max(120, this.stageW * 0.96);
    const padH: number = Math.max(120, this.stageH * 0.96);
    const aspect: number = this.dispW / Math.max(1, this.dispH);
    if (padW / padH > aspect) {
      return Math.floor(padH);
    }
    return Math.floor(padW / aspect);
  }

  aboutToAppear(): void {
    const rawGpu: string | undefined = AppStorage.get<string>('gpuMode');
    if (rawGpu === 'swiftshader' || rawGpu === 'gles' || rawGpu === 'vulkan') {
      this.gpuMode = rawGpu;
    }
    const rawRes: string | undefined = AppStorage.get<string>('resolution');
    if (rawRes && rawRes.length > 0) {
      this.applyResolutionKey(rawRes);
    }
    const rawHz: number | undefined = AppStorage.get<number>('refreshRate');
    if (rawHz === 60 || rawHz === 90 || rawHz === 120) {
      this.refreshRate = rawHz;
    }
    const reqUi: string | undefined = AppStorage.get<string>('requestedUiMode');
    if (reqUi === 'emulator' || reqUi === 'multiwindow') {
      this.uiMode = reqUi;
    }
    AppStorage.setOrCreate<string>('activeUiMode', this.uiMode);
    this.lastLaunchSeq = AppStorage.get<number>('launchSeq') ?? 0;

    try {
      native.setResourceConfig(this.profile, '', this.memLimitMb);
      native.setGpuMode(this.gpuMode);
      native.setRefreshRate(this.refreshRate);
    } catch (_e) {
    }

    this.refreshPinnedShortcuts();

    const autoMode: string | undefined = AppStorage.get<string>('autoStart');
    if (autoMode && (autoMode === 'bootanim' || autoMode === 'full')) {
      this.autoBootTriggered = true;
      setTimeout(() => {
        if (!this.running) {
          this.launchMode(autoMode);
        }
      }, 250);
    } else if (!this.autoBootTriggered) {
      // Default Free Multi-Window Mode: silently boot single shared Android 14 instance
      this.autoBootTriggered = true;
      setTimeout(() => {
        const cur: RuntimeStatus = native.getStatus();
        if (!cur.running) {
          this.launchMode('full');
          native.setDisplayVisible(0, this.uiMode === 'emulator');
        }
      }, 300);
    }

    this.pollTimer = setInterval(() => {
      this.onPollTick();
    }, 500);
  }

  aboutToDisappear(): void {
    if (this.pollTimer !== -1) {
      clearInterval(this.pollTimer);
      this.pollTimer = -1;
    }
  }

  private onPollTick(): void {
    try {
      const reqUi: string | undefined = AppStorage.get<string>('requestedUiMode');
      if ((reqUi === 'emulator' || reqUi === 'multiwindow') && reqUi !== this.uiMode) {
        this.setUiMode(reqUi);
      }

      const curSeq: number = AppStorage.get<number>('launchSeq') ?? 0;
      if (curSeq !== this.lastLaunchSeq) {
        this.lastLaunchSeq = curSeq;
        const nextGpu: string | undefined = AppStorage.get<string>('gpuMode');
        const gpuChanged: boolean =
          (nextGpu === 'swiftshader' || nextGpu === 'gles' || nextGpu === 'vulkan') &&
          nextGpu !== this.gpuMode;
        if (nextGpu === 'swiftshader' || nextGpu === 'gles' || nextGpu === 'vulkan') {
          this.gpuMode = nextGpu;
        }
        const nextRes: string | undefined = AppStorage.get<string>('resolution');
        const resChanged: boolean = (nextRes && nextRes.length > 0) ? this.applyResolutionKey(nextRes) : false;
        const nextHz: number | undefined = AppStorage.get<number>('refreshRate');
        if (nextHz === 60 || nextHz === 90 || nextHz === 120) {
          this.refreshRate = nextHz;
          native.setRefreshRate(this.refreshRate);
        }
        const nextMode: string | undefined = AppStorage.get<string>('autoStart');
        if (this.running) {
          native.stopContainer();
          this.appsFetchedOnce = false;
        }
        if (gpuChanged || resChanged) {
          this.surfaceEpoch++;
        }
        if (nextMode === 'bootanim' || nextMode === 'full') {
          setTimeout(() => {
            this.launchMode(nextMode);
          }, (gpuChanged || resChanged) ? 650 : 400);
        }
      }

      const st: RuntimeStatus = native.getStatus();
      this.running = st.running;
      this.paused = st.paused;
      this.connected = st.connected;
      this.wmAgentReady = st.wmAgentReady;
      this.vsyncActive = st.vsyncActive;
      this.inputReady = st.inputReady;
      this.audioActive = st.audioActive;
      this.frames = st.frames;
      this.fps = st.fps;
      if (st.refreshRate === 60 || st.refreshRate === 90 || st.refreshRate === 120) {
        this.refreshRate = st.refreshRate;
      }
      this.cpuPercent = Math.round(st.cpuPercent);
      this.memMb = st.memMb;
      this.memLimitMb = st.memLimitMb;
      this.activeProcs = st.activeProcs;
      this.gpuRenderer = st.gpuRenderer;
      this.cpuset = st.cpuset;
      this.topology = st.topology;
      if (st.statusText.length > 0) {
        this.statusText = st.statusText;
      }

      if (this.wmAgentReady) {
        if (!this.appsFetchedOnce && !this.loadingApps) {
          this.appsFetchedOnce = true;
          this.refreshApps(false);
        }
        this.drainWmEvents();
      } else {
        this.appsFetchedOnce = false;
      }
    } catch (_e) {
    }
  }

  private drainWmEvents(): void {
    try {
      const raw: string = native.pollWmEvents();
      if (!raw || raw.length <= 2) return;
      const evs: WmEventItem[] = JSON.parse(raw) as WmEventItem[];
      if (!Array.isArray(evs)) return;
      for (let i: number = 0; i < evs.length; i++) {
        const ev: WmEventItem = evs[i];
        if (ev.event === 'CROSS_APP_LAUNCH' && ev.packageName && ev.packageName.length > 0) {
          hilog.info(0x0000, TAG, 'Cross-app launch -> new OHOS window: pkg=%{public}s taskId=%{public}d',
            ev.packageName, ev.taskId);
          this.openAndroidAppWindow(ev.packageName, ev.component ?? '', ev.label ?? ev.packageName, ev.taskId);
        }
      }
    } catch (_e) {
    }
  }

  private async refreshApps(forceIcons: boolean): Promise<void> {
    if (!native.isWmAgentReady() || this.loadingApps) return;
    this.loadingApps = true;
    try {
      this.iconsDir = native.getIconsDir();
      const raw: string = await native.wmRpcAsync(forceIcons ? 'LIST_APPS 1' : 'LIST_APPS 0');
      const rep: ListAppsReply = JSON.parse(raw) as ListAppsReply;
      if (rep.ok && Array.isArray(rep.apps)) {
        const sorted: AndroidAppItem[] = rep.apps.slice();
        sorted.sort((a: AndroidAppItem, b: AndroidAppItem): number => {
          if (a.isSystem !== b.isSystem) return a.isSystem ? 1 : -1;
          return a.label.localeCompare(b.label);
        });
        this.appList = sorted;
        AppStorage.setOrCreate<AndroidAppItem[]>('androidApps', sorted);
      }
    } catch (err) {
      hilog.warn(0x0000, TAG, 'refreshApps error: %{public}s', String(err));
    } finally {
      this.loadingApps = false;
    }
  }

  private async refreshPinnedShortcuts(): Promise<void> {
    this.pinnedIds = await queryPinnedShortcutIds();
  }

  private isAppPinned(pkg: string): boolean {
    const sid: string = computeShortcutId(pkg);
    return this.pinnedIds.indexOf(sid) >= 0;
  }

  private getFilteredApps(): AndroidAppItem[] {
    const q: string = this.searchQuery.trim().toLowerCase();
    const out: AndroidAppItem[] = [];
    for (let i: number = 0; i < this.appList.length; i++) {
      const a: AndroidAppItem = this.appList[i];
      if (this.appFilter === 'user' && a.isSystem) continue;
      if (this.appFilter === 'system' && !a.isSystem) continue;
      if (q.length > 0) {
        const hay: string = `${a.label} ${a.packageName}`.toLowerCase();
        if (hay.indexOf(q) < 0) continue;
      }
      out.push(a);
    }
    return out;
  }

  private openAndroidAppWindow(pkg: string, comp: string, label: string, moveTaskId: number = -1): void {
    try {
      const ctx: common.UIAbilityContext = this.getUIContext().getHostContext() as common.UIAbilityContext;
      const want: Want = {
        bundleName: 'org.svbox.android',
        moduleName: 'entry',
        abilityName: 'AppWindowAbility',
        parameters: {
          'androidPkg': pkg,
          'androidComp': comp,
          'androidLabel': label,
          'moveTaskId': String(moveTaskId),
        }
      };
      ctx.startAbility(want).catch((err: Error) => {
        hilog.error(0x0000, TAG, 'startAbility AppWindowAbility failed: %{public}s', String(err));
        this.showToast(`打开窗口失败: ${String(err)}`);
      });
    } catch (err) {
      this.showToast(`打开窗口异常: ${String(err)}`);
    }
  }

  private async handlePopOutCurrentEmulatorApp(): Promise<void> {
    if (!this.wmAgentReady) {
      this.showToast('窗口服务尚未就绪');
      return;
    }
    const slot: number = native.allocDisplaySlot('');
    if (slot < 1) {
      this.showToast('独立窗口已达 8 个上限');
      return;
    }
    try {
      await native.wmRpcAsync(`CREATE_DISPLAY ${slot} 720 1280 320 ${this.refreshRate}`);
      const raw: string = await native.wmRpcAsync(`POP_OUT_EMULATOR ${slot}`);
      const rep: PopOutReply = JSON.parse(raw) as PopOutReply;
      if (!rep.ok || !rep.packageName) {
        await native.wmRpcAsync(`DESTROY_DISPLAY ${slot}`);
        native.releaseDisplaySlot(slot);
        this.showToast('当前模拟器前台没有可拆出的应用');
        return;
      }
      const ctx: common.UIAbilityContext = this.getUIContext().getHostContext() as common.UIAbilityContext;
      const want: Want = {
        bundleName: 'org.svbox.android',
        moduleName: 'entry',
        abilityName: 'AppWindowAbility',
        parameters: {
          'androidPkg': rep.packageName,
          'androidComp': rep.component ?? '',
          'androidLabel': rep.label ?? rep.packageName,
          'moveTaskId': String(rep.taskId ?? -1),
          'preAllocSlot': String(slot),
        }
      };
      ctx.startAbility(want).catch((_e: Error) => {});
      this.showToast(`已将「${rep.label ?? rep.packageName}」拆出为独立窗口`);
    } catch (_e) {
      native.releaseDisplaySlot(slot);
    }
  }

  private getEffectiveModeLabel(app: AndroidAppItem): string {
    const _tick: number = this.prefEpoch;
    const pref: AppWindowPref = appWindowPrefs.getPref(
      this.getSandboxFilesDir(), app.packageName, app, this.refreshRate
    );
    if (_tick < 0) return '';
    if (pref.mode === 'adaptive') return '自适应';
    if (pref.mode === 'fixed_landscape') return '固定横';
    return '固定竖';
  }

  private cycleAppWindowPref(app: AndroidAppItem): void {
    const filesDir: string = this.getSandboxFilesDir();
    const cur: AppWindowPref = appWindowPrefs.getPref(filesDir, app.packageName, app, this.refreshRate);
    if (cur.mode === 'adaptive') {
      cur.mode = 'fixed_portrait';
      cur.presetKey = '720x1280';
      cur.winW = 540;
      cur.winH = 960;
    } else if (cur.mode === 'fixed_portrait') {
      cur.mode = 'fixed_landscape';
      cur.presetKey = '1280x720';
      cur.winW = 1280;
      cur.winH = 760;
    } else {
      cur.mode = 'adaptive';
      cur.winW = 640;
      cur.winH = 920;
    }
    appWindowPrefs.savePref(filesDir, app.packageName, cur);
    this.prefEpoch++;
    this.showToast(`${app.label} 窗口模式: ${this.getEffectiveModeLabel(app)}`);
  }

  private async toggleDesktopShortcut(app: AndroidAppItem): Promise<void> {
    const ctx: common.UIAbilityContext = this.getUIContext().getHostContext() as common.UIAbilityContext;
    if (this.isAppPinned(app.packageName)) {
      const rm: ShortcutOpResult = await removeAndroidAppShortcut(ctx, app.packageName);
      this.showToast(rm.message);
      await this.refreshPinnedShortcuts();
      return;
    }
    const dir: string = this.iconsDir.length > 0 ? this.iconsDir : native.getIconsDir();
    const iconPath: string = `${dir}/${app.iconFile.length > 0 ? app.iconFile : (app.packageName + '.png')}`;
    const res: ShortcutOpResult = await pinAndroidAppToDesktop(ctx, app, iconPath);
    this.showToast(res.message);
    await this.refreshPinnedShortcuts();
  }

  private cycleProfile(): void {
    const order: string[] = ['eco', 'balanced', 'perf', 'all'];
    const idx: number = order.indexOf(this.profile);
    this.profile = order[(idx + 1) % order.length];
    try {
      native.setResourceConfig(this.profile, '', this.memLimitMb);
    } catch (_e) {
    }
  }

  private cycleGpuMode(): void {
    const order: string[] = ['vulkan', 'gles', 'swiftshader'];
    const idx: number = order.indexOf(this.gpuMode);
    this.gpuMode = order[(idx + 1) % order.length];
    this.surfaceEpoch++;
    try {
      native.setGpuMode(this.gpuMode);
    } catch (_e) {
    }
  }

  private cycleRefreshRate(): void {
    const idx: number = REFRESH_RATES.indexOf(this.refreshRate);
    this.refreshRate = REFRESH_RATES[(idx + 1) % REFRESH_RATES.length];
    AppStorage.setOrCreate<number>('refreshRate', this.refreshRate);
    try {
      native.setRefreshRate(this.refreshRate);
    } catch (_e) {
    }
  }

  private cycleResolution(): void {
    this.resIdx = (this.resIdx + 1) % RESOLUTION_PRESETS.length;
    this.dispW = RESOLUTION_PRESETS[this.resIdx].w;
    this.dispH = RESOLUTION_PRESETS[this.resIdx].h;
    this.surfaceEpoch++;
  }

  private gpuLabel(): string {
    if (this.gpuMode === 'vulkan') return 'Vulkan';
    if (this.gpuMode === 'gles') return 'GLES';
    return 'ANGLE-SW';
  }

  private launchMode(mode: string): void {
    try {
      native.setResourceConfig(this.profile, '', this.memLimitMb);
      native.setGpuMode(this.gpuMode);
      native.setRefreshRate(this.refreshRate);
    } catch (_e) {
    }
    const tempDir: string = this.getSandboxTempDir();
    this.statusText = native.startContainer(
      tempDir,
      this.svboxPath,
      this.androidDir,
      mode,
      this.dispW,
      this.dispH
    );
  }

  private buildBadgeText(): string {
    if (this.paused) {
      return `Paused (0% CPU) · #${this.frames}`;
    }
    if (this.wmAgentReady) {
      return `Android 14 · ${this.gpuLabel()} · ${this.refreshRate}Hz · CPU ${this.cpuPercent}% · ${this.memMb}MB · ${this.appList.length} Apps`;
    }
    if (this.connected) {
      return `${this.gpuLabel()} · ${this.dispW}×${this.dispH} · ${this.fps}/${this.refreshRate}Hz · CPU ${this.cpuPercent}% · ${this.memMb}MB`;
    }
    return this.running ? `正在启动 Android 14 (${this.gpuLabel()} · ${this.refreshRate}Hz)...` : 'Idle · Android 14';
  }

  build() {
    Column() {
      // Top Control Bar with Dual-Mode Switcher
      Row({ space: 6 }) {
        Text('svbox-A14')
          .fontSize(14)
          .fontWeight(FontWeight.Bold)
          .fontColor('#F3F4F6')

        // Segmented Dual-Mode Switcher
        Row({ space: 2 }) {
          Button('自由多窗')
            .height(26)
            .fontSize(11)
            .backgroundColor(this.uiMode === 'multiwindow' ? '#0284C7' : '#262B35')
            .onClick(() => { this.setUiMode('multiwindow'); })
          Button('传统模拟器')
            .height(26)
            .fontSize(11)
            .backgroundColor(this.uiMode === 'emulator' ? '#059669' : '#262B35')
            .onClick(() => { this.setUiMode('emulator'); })
        }
        .padding(2)
        .backgroundColor('#11141B')
        .borderRadius(8)

        Text(this.buildBadgeText())
          .fontSize(11)
          .fontColor(this.paused ? '#FBBF24' : (this.wmAgentReady ? '#34D399' : '#9CA3AF'))
          .padding({ left: 8, right: 8, top: 4, bottom: 4 })
          .backgroundColor('#1F2937')
          .borderRadius(6)
          .maxLines(1)

        Blank()

        Button(`${this.refreshRate}Hz`)
          .height(28)
          .fontSize(11)
          .backgroundColor(this.refreshRate === 120 ? '#059669' : (this.refreshRate === 90 ? '#0284C7' : '#374151'))
          .onClick(() => { this.cycleRefreshRate(); })

        if (this.uiMode === 'emulator') {
          Button(RESOLUTION_PRESETS[this.resIdx].label)
            .height(28)
            .fontSize(11)
            .backgroundColor('#374151')
            .enabled(!this.running)
            .onClick(() => { this.cycleResolution(); })

          Button(`GPU: ${this.gpuLabel()}`)
            .height(28)
            .fontSize(11)
            .backgroundColor(this.gpuMode === 'vulkan' ? '#7C3AED' : '#374151')
            .enabled(!this.running)
            .onClick(() => { this.cycleGpuMode(); })

          Button(`CPU: ${this.profile}`)
            .height(28)
            .fontSize(11)
            .backgroundColor('#374151')
            .enabled(!this.running)
            .onClick(() => { this.cycleProfile(); })

          if (this.running) {
            Button('◀')
              .height(28)
              .fontSize(12)
              .backgroundColor('#374151')
              .onClick(() => { native.sendNavKey('back'); })
            Button('●')
              .height(28)
              .fontSize(12)
              .backgroundColor('#374151')
              .onClick(() => { native.sendNavKey('home'); })
            Button('■')
              .height(28)
              .fontSize(12)
              .backgroundColor('#374151')
              .onClick(() => { native.sendNavKey('recents'); })
            Button('拆出应用')
              .height(28)
              .fontSize(11)
              .backgroundColor('#0284C7')
              .enabled(this.wmAgentReady)
              .onClick(() => { this.handlePopOutCurrentEmulatorApp(); })
          }
        } else {
          Button('刷新应用')
            .height(28)
            .fontSize(11)
            .backgroundColor('#374151')
            .enabled(this.wmAgentReady && !this.loadingApps)
            .onClick(() => { this.refreshApps(true); })
        }

        if (this.running) {
          Button(this.paused ? 'Resume' : 'Pause')
            .height(28)
            .fontSize(11)
            .backgroundColor(this.paused ? '#10B981' : '#D97706')
            .onClick(() => {
              this.statusText = native.setPaused(!this.paused);
            })
        } else {
          Button('启动 A14')
            .height(28)
            .fontSize(11)
            .backgroundColor('#059669')
            .onClick(() => {
              this.launchMode('full');
              native.setDisplayVisible(0, this.uiMode === 'emulator');
            })
        }

        Button('停止')
          .height(28)
          .fontSize(11)
          .backgroundColor('#DC2626')
          .enabled(this.running)
          .onClick(() => {
            this.statusText = native.stopContainer();
            this.appList = [];
            this.appsFetchedOnce = false;
          })
      }
      .width('100%')
      .height(48)
      .padding({ left: 12, right: 12 })
      .backgroundColor('#181B22')
      .alignItems(VerticalAlign.Center)

      // Main Content Area: Free Multi-Window Launcher OR Classic Emulator Display 0
      Stack({ alignContent: Alignment.Center }) {
        // Keep Display 0 XComponent mounted when in emulator mode (hidden in multiwindow to save GPU)
        if (this.uiMode === 'emulator') {
          if (this.surfaceEpoch % 2 === 0) {
            XComponent({
              id: 'svbox_display_0',
              type: XComponentType.SURFACE,
              libraryname: 'entry'
            })
              .focusable(true)
              .defaultFocus(true)
              .width(this.computeFitWidth())
              .height(this.computeFitHeight())
              .backgroundColor('#000000')
          } else {
            XComponent({
              id: 'svbox_android_surface_b',
              type: XComponentType.SURFACE,
              libraryname: 'entry'
            })
              .focusable(true)
              .defaultFocus(true)
              .width(this.computeFitWidth())
              .height(this.computeFitHeight())
              .backgroundColor('#000000')
          }
        } else {
          // Free Multi-Window Mode: Native HarmonyOS PC App Launcher
          Column({ space: 12 }) {
            // Search & Filter Bar
            Row({ space: 10 }) {
              TextInput({ placeholder: '搜索 Android 应用名称或包名...', text: this.searchQuery })
                .height(34)
                .layoutWeight(1)
                .fontSize(13)
                .fontColor('#F3F4F6')
                .placeholderColor('#6B7280')
                .backgroundColor('#1A1E26')
                .borderRadius(8)
                .onChange((val: string) => {
                  this.searchQuery = val;
                })

              Row({ space: 4 }) {
                Button('全部')
                  .height(30)
                  .fontSize(11)
                  .backgroundColor(this.appFilter === 'all' ? '#0284C7' : '#262B35')
                  .onClick(() => { this.appFilter = 'all'; })
                Button('第三方')
                  .height(30)
                  .fontSize(11)
                  .backgroundColor(this.appFilter === 'user' ? '#0284C7' : '#262B35')
                  .onClick(() => { this.appFilter = 'user'; })
                Button('系统')
                  .height(30)
                  .fontSize(11)
                  .backgroundColor(this.appFilter === 'system' ? '#0284C7' : '#262B35')
                  .onClick(() => { this.appFilter = 'system'; })
              }

              Text(`桌面图标 ${this.pinnedIds.length}/10`)
                .fontSize(11)
                .fontColor('#93C5FD')
                .padding({ left: 10, right: 10, top: 6, bottom: 6 })
                .backgroundColor('#172554')
                .borderRadius(8)
            }
            .width('100%')
            .alignItems(VerticalAlign.Center)

            if (!this.wmAgentReady) {
              Column({ space: 12 }) {
                LoadingProgress()
                  .width(42)
                  .height(42)
                  .color('#38BDF8')
                Text(this.running ? 'Android 14 正在后台启动，应用列表即将就绪...' : '点击右上角「启动 A14」启动容器')
                  .fontSize(14)
                  .fontColor('#D1D5DB')
                Text('单实例共享内核 · 点击任意应用即开独立鸿蒙窗口 · 支持跨应用自动弹新窗与桌面快捷方式')
                  .fontSize(12)
                  .fontColor('#6B7280')
              }
              .width('100%')
              .layoutWeight(1)
              .justifyContent(FlexAlign.Center)
            } else if (this.getFilteredApps().length === 0) {
              Column({ space: 8 }) {
                Text(this.loadingApps ? '正在同步应用列表与图标...' : '暂无匹配应用')
                  .fontSize(14)
                  .fontColor('#9CA3AF')
              }
              .width('100%')
              .layoutWeight(1)
              .justifyContent(FlexAlign.Center)
            } else {
              Grid() {
                ForEach(
                  this.getFilteredApps(),
                  (app: AndroidAppItem) => {
                    GridItem() {
                      Column({ space: 6 }) {
                        // App Icon + Tap to open independent HarmonyOS window
                        Column({ space: 6 }) {
                          if (this.iconsDir.length > 0 && app.iconFile.length > 0) {
                            Image(`file://${this.iconsDir}/${app.iconFile}`)
                              .width(52)
                              .height(52)
                              .borderRadius(12)
                              .objectFit(ImageFit.Contain)
                          } else {
                            Row() {
                              Text(app.label.length > 0 ? app.label.substring(0, 1).toUpperCase() : 'A')
                                .fontSize(22)
                                .fontWeight(FontWeight.Bold)
                                .fontColor('#F9FAFB')
                            }
                            .width(52)
                            .height(52)
                            .borderRadius(12)
                            .backgroundColor('#1E3A8A')
                            .justifyContent(FlexAlign.Center)
                            .alignItems(VerticalAlign.Center)
                          }

                          Text(app.label)
                            .fontSize(12)
                            .fontWeight(FontWeight.Medium)
                            .fontColor('#F3F4F6')
                            .maxLines(1)
                            .textOverflow({ overflow: TextOverflow.Ellipsis })
                            .textAlign(TextAlign.Center)
                            .width('100%')
                        }
                        .width('100%')
                        .alignItems(HorizontalAlign.Center)
                        .onClick(() => {
                          this.openAndroidAppWindow(app.packageName, app.component, app.label, -1);
                        })

                        // Quick Action Row: Per-App Window Mode & Desktop Shortcut
                        Row({ space: 4 }) {
                          Button(this.getEffectiveModeLabel(app))
                            .height(22)
                            .fontSize(10)
                            .padding({ left: 6, right: 6 })
                            .backgroundColor('#272C36')
                            .onClick(() => { this.cycleAppWindowPref(app); })

                          Button(this.isAppPinned(app.packageName) ? '移桌' : '加桌')
                            .height(22)
                            .fontSize(10)
                            .padding({ left: 6, right: 6 })
                            .backgroundColor(this.isAppPinned(app.packageName) ? '#065F46' : '#1E3A8A')
                            .onClick(() => { this.toggleDesktopShortcut(app); })
                        }
                        .justifyContent(FlexAlign.Center)
                      }
                      .width('100%')
                      .padding(10)
                      .backgroundColor('#161A22')
                      .borderRadius(12)
                      .border({ width: 1, color: '#242A36' })
                    }
                  },
                  (app: AndroidAppItem): string => `${app.packageName}_${this.prefEpoch}_${this.pinnedIds.length}`
                )
              }
              .columnsTemplate('1fr 1fr 1fr 1fr 1fr 1fr')
              .columnsGap(12)
              .rowsGap(12)
              .width('100%')
              .layoutWeight(1)
            }
          }
          .width('100%')
          .height('100%')
          .padding({ left: 16, right: 16, top: 12, bottom: 12 })
        }
      }
      .width('100%')
      .layoutWeight(1)
      .backgroundColor('#0D0F14')
      .onAreaChange((_oldVal: Area, newVal: Area) => {
        const w: number = Number(newVal.width);
        const h: number = Number(newVal.height);
        if (w > 100 && h > 100) {
          this.stageW = w;
          this.stageH = h;
        }
      })

      // Bottom Status Bar
      Row({ space: 12 }) {
        Text(this.statusText)
          .fontSize(11)
          .fontColor('#9CA3AF')
          .maxLines(1)
          .textOverflow({ overflow: TextOverflow.Ellipsis })
          .layoutWeight(1)

        if (this.cpuset.length > 0 || this.topology.length > 0 || this.gpuRenderer.length > 0) {
          Text(`${this.gpuRenderer.length > 0 ? this.gpuRenderer + ' · ' : ''}CPU[${this.cpuset.length > 0 ? this.cpuset : 'all'}] · ${this.topology} · ${this.activeProcs} procs`)
            .fontSize(11)
            .fontColor('#6B7280')
            .maxLines(1)
        }
      }
      .width('100%')
      .height(28)
      .padding({ left: 14, right: 14 })
      .backgroundColor('#181B22')
      .alignItems(VerticalAlign.Center)
    }
    .width('100%')
    .height('100%')
    .backgroundColor('#0D0F14')
  }
}
