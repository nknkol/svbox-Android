import { common, Want } from '@kit.AbilityKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import native, { RuntimeStatus } from 'libentry.so';
import {
  AndroidAppItem,
  REFRESH_RATES,
  RESOLUTION_PRESETS,
  ResolutionPresetItem,
  WmEventItem
} from '../common/AppWindowPrefs';

const TAG: string = 'SvboxMain';

interface ListAppsReply {
  ok?: boolean;
  apps?: AndroidAppItem[];
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
  @StorageLink('mainDecorRightAvoidVp') decorRightAvoidVp: number = 136;

  @State private uiMode: string = 'manager'; // 'manager' (MuMu instance list) | 'emulator' (Display 0)
  @State private showInstanceSettings: boolean = false;
  @State private running: boolean = false;
  @State private paused: boolean = false;
  @State private connected: boolean = false;
  @State private wmAgentReady: boolean = false;
  @State private frames: number = 0;
  @State private fps: number = 0;
  @State private dispW: number = 720;
  @State private dispH: number = 1280;
  @State private resIdx: number = 0;
  @State private refreshRate: number = 60;
  @State private cpuPercent: number = 0;
  @State private memMb: number = 0;
  @State private memLimitMb: number = 4096;
  @State private activeProcs: number = 0;
  @State private profile: string = 'Balanced';
  @State private gpuMode: string = 'vulkan';
  @State private gpuRenderer: string = '';
  @State private cpuset: string = '';
  @State private topology: string = '';
  @State private statusText: string = '就绪';
  @State private stageW: number = 720;
  @State private stageH: number = 820;
  @State private surfaceToggle: boolean = false;
  @State private appCount: number = 0;

  private pollTimer: number = -1;
  private lastLaunchSeq: number = 0;
  private appsFetchedOnce: boolean = false;
  private autoBootTriggered: boolean = false;
  private svboxPath: string = '/storage/Users/currentUser/Workspace/svbox/build/svbox';
  private androidDir: string = '/storage/Users/currentUser/Workspace/svbox-Android';

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
    if (nextMode !== 'manager' && nextMode !== 'emulator') return;
    this.uiMode = nextMode;
    AppStorage.setOrCreate<string>('activeUiMode', nextMode === 'emulator' ? 'emulator' : 'multiwindow');
    AppStorage.setOrCreate<string>('requestedUiMode', '');
    try {
      native.setDisplayVisible(0, nextMode === 'emulator');
      if (nextMode === 'emulator' && this.wmAgentReady) {
        native.wmRpcAsync('FOCUS_DISPLAY 0');
      }
    } catch (_e) {
    }
  }

  private openAppsCenterPage(): void {
    try {
      this.getUIContext().getRouter().pushUrl({ url: 'pages/AppsPage' });
    } catch (err) {
      hilog.error(0x0000, TAG, 'pushUrl AppsPage failed: %{public}s', String(err));
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
    if (reqUi === 'emulator') {
      this.uiMode = 'emulator';
    }
    AppStorage.setOrCreate<string>('activeUiMode', this.uiMode === 'emulator' ? 'emulator' : 'multiwindow');
    this.lastLaunchSeq = AppStorage.get<number>('launchSeq') ?? 0;

    try {
      native.setResourceConfig(this.profile, '', this.memLimitMb);
      native.setGpuMode(this.gpuMode);
      native.setRefreshRate(this.refreshRate);
    } catch (_e) {
    }

    const autoStart: string | undefined = AppStorage.get<string>('autoStart');
    if (autoStart === 'bootanim' || autoStart === 'full') {
      this.autoBootTriggered = true;
      setTimeout(() => {
        this.launchMode(autoStart);
      }, 300);
    } else if (!this.autoBootTriggered) {
      this.autoBootTriggered = true;
      setTimeout(() => {
        const cur: RuntimeStatus = native.getStatus();
        if (!cur.running) {
          this.launchMode('full');
          native.setDisplayVisible(0, this.uiMode === 'emulator');
        }
      }, 350);
    }

    this.pollTimer = setInterval(() => {
      this.pollRuntime();
    }, 500);
  }

  aboutToDisappear(): void {
    if (this.pollTimer !== -1) {
      clearInterval(this.pollTimer);
      this.pollTimer = -1;
    }
  }

  private pollRuntime(): void {
    try {
      const reqUi: string | undefined = AppStorage.get<string>('requestedUiMode');
      if (reqUi === 'emulator' && this.uiMode !== 'emulator') {
        this.setUiMode('emulator');
      } else if (reqUi === 'multiwindow' && this.uiMode !== 'manager') {
        this.setUiMode('manager');
      }

      const seq: number = AppStorage.get<number>('launchSeq') ?? 0;
      if (seq > this.lastLaunchSeq) {
        this.lastLaunchSeq = seq;
        const reqGpu: string | undefined = AppStorage.get<string>('gpuMode');
        if (reqGpu === 'swiftshader' || reqGpu === 'gles' || reqGpu === 'vulkan') {
          this.gpuMode = reqGpu;
          native.setGpuMode(this.gpuMode);
        }
        const reqRes: string | undefined = AppStorage.get<string>('resolution');
        let resChanged: boolean = false;
        if (reqRes && reqRes.length > 0) {
          resChanged = this.applyResolutionKey(reqRes);
        }
        const reqHz: number | undefined = AppStorage.get<number>('refreshRate');
        if (reqHz === 60 || reqHz === 90 || reqHz === 120) {
          this.refreshRate = reqHz;
          native.setRefreshRate(this.refreshRate);
        }
        const mode: string | undefined = AppStorage.get<string>('autoStart');
        if (mode === 'bootanim' || mode === 'full') {
          if (resChanged) {
            this.surfaceToggle = !this.surfaceToggle;
          }
          setTimeout(() => {
            this.launchMode(mode);
          }, resChanged ? 150 : 20);
        }
      }

      const st: RuntimeStatus = native.getStatus();
      this.running = st.running;
      this.paused = st.paused;
      this.connected = st.connected;
      this.wmAgentReady = st.wmAgentReady;
      this.frames = st.frames;
      this.fps = st.fps;
      if (st.width > 0 && st.height > 0) {
        this.dispW = st.width;
        this.dispH = st.height;
      }
      if (st.refreshRate === 60 || st.refreshRate === 90 || st.refreshRate === 120) {
        this.refreshRate = st.refreshRate;
        AppStorage.setOrCreate<number>('refreshRate', this.refreshRate);
      }
      this.cpuPercent = st.cpuPercent;
      this.memMb = st.memMb;
      this.memLimitMb = st.memLimitMb;
      this.activeProcs = st.activeProcs;
      this.profile = st.profile;
      this.gpuMode = st.gpuMode;
      this.gpuRenderer = st.gpuRenderer;
      this.cpuset = st.cpuset;
      this.topology = st.topology;
      this.statusText = st.statusText;

      if (this.wmAgentReady && !this.appsFetchedOnce) {
        this.appsFetchedOnce = true;
        this.prefetchApps();
      }
      if (!this.running) {
        this.appsFetchedOnce = false;
        this.appCount = 0;
      }

      if (this.wmAgentReady) {
        this.drainWmEvents();
      }
    } catch (_e) {
    }
  }

  private async prefetchApps(): Promise<void> {
    try {
      const raw: string = await native.wmRpcAsync('LIST_APPS 1');
      const rep: ListAppsReply = JSON.parse(raw) as ListAppsReply;
      if (rep.ok && rep.apps) {
        this.appCount = rep.apps.length;
        AppStorage.setOrCreate<AndroidAppItem[]>('androidApps', rep.apps);
      }
    } catch (_e) {
    }
  }

  private drainWmEvents(): void {
    try {
      const raw: string = native.pollWmEvents();
      if (!raw || raw === '[]') return;
      const items: WmEventItem[] = JSON.parse(raw) as WmEventItem[];
      for (let i: number = 0; i < items.length; i++) {
        const ev: WmEventItem = items[i];
        if (ev.event === 'CROSS_APP_LAUNCH' && ev.packageName && ev.packageName.length > 0) {
          this.openAndroidAppWindow(ev.packageName, ev.component ?? '', ev.label ?? ev.packageName, ev.taskId ?? -1);
        }
      }
    } catch (_e) {
    }
  }

  private async openAndroidAppWindow(pkg: string, comp: string, label: string, moveTaskId: number): Promise<void> {
    try {
      const ctx: common.UIAbilityContext = this.getUIContext().getHostContext() as common.UIAbilityContext;
      const want: Want = {
        bundleName: 'org.svbox.android',
        abilityName: 'AppWindowAbility',
        parameters: {
          'androidPkg': pkg,
          'androidComp': comp,
          'androidLabel': label.length > 0 ? label : pkg,
          'moveTaskId': String(moveTaskId),
        },
      };
      await ctx.startAbility(want);
    } catch (err) {
      hilog.error(0x0000, TAG, 'openAndroidAppWindow failed: %{public}s', String(err));
      this.showToast(`打开应用窗口失败: ${String(err)}`);
    }
  }

  private async handlePopOutCurrentEmulatorApp(): Promise<void> {
    if (!this.wmAgentReady) {
      this.showToast('等待 Android 14 窗口服务就绪');
      return;
    }
    const probeSlot: number = native.allocDisplaySlot('__popout_probe__');
    if (probeSlot < 1) {
      this.showToast('已达到最大 8 个独立窗口上限');
      return;
    }
    try {
      const raw: string = await native.wmRpcAsync('GET_TASKS 0');
      const parsed: Record<string, Object> = JSON.parse(raw) as Record<string, Object>;
      const tasks: Object[] = (parsed['tasks'] as Object[]) ?? [];
      let targetPkg: string = '';
      let targetComp: string = '';
      let targetTid: number = -1;
      for (let i: number = 0; i < tasks.length; i++) {
        const t: Record<string, Object> = tasks[i] as Record<string, Object>;
        const p: string = String(t['packageName'] ?? '');
        if (p.length === 0 || p === 'com.android.launcher3' || p === 'com.android.systemui') continue;
        targetPkg = p;
        targetComp = String(t['component'] ?? '');
        targetTid = Number(t['taskId'] ?? -1);
        break;
      }
      native.releaseDisplaySlot(probeSlot);
      if (targetPkg.length === 0 || targetTid <= 0) {
        const popRaw: string = await native.wmRpcAsync('POP_OUT_EMULATOR 1');
        const pr: PopOutReply = JSON.parse(popRaw) as PopOutReply;
        if (!pr.ok) {
          this.showToast('当前模拟器前台没有可拆出的应用');
          return;
        }
        targetPkg = pr.packageName ?? '';
        targetComp = pr.component ?? '';
        targetTid = pr.taskId ?? -1;
      }
      if (targetPkg.length > 0) {
        const cached: AndroidAppItem[] = AppStorage.get<AndroidAppItem[]>('androidApps') ?? [];
        let label: string = targetPkg;
        for (let i: number = 0; i < cached.length; i++) {
          if (cached[i].packageName === targetPkg) {
            label = cached[i].label;
            break;
          }
        }
        await this.openAndroidAppWindow(targetPkg, targetComp, label, targetTid);
      }
    } catch (err) {
      native.releaseDisplaySlot(probeSlot);
      this.showToast(`拆出失败: ${String(err)}`);
    }
  }

  private launchMode(mode: string): void {
    const preset = RESOLUTION_PRESETS[this.resIdx];
    this.dispW = preset.w;
    this.dispH = preset.h;
    native.setGpuMode(this.gpuMode);
    native.setRefreshRate(this.refreshRate);
    this.statusText = native.startContainer(
      this.getSandboxTempDir(),
      this.svboxPath,
      this.androidDir,
      mode,
      preset.w,
      preset.h
    );
  }

  private selectRefreshRate(hz: number): void {
    this.refreshRate = hz;
    AppStorage.setOrCreate<number>('refreshRate', hz);
    native.setRefreshRate(hz);
  }

  private selectResolution(idx: number): void {
    if (this.running) return;
    this.resIdx = idx;
    this.dispW = RESOLUTION_PRESETS[idx].w;
    this.dispH = RESOLUTION_PRESETS[idx].h;
    this.surfaceToggle = !this.surfaceToggle;
  }

  private selectGpuMode(mode: string): void {
    if (this.running) return;
    this.gpuMode = mode;
    native.setGpuMode(mode);
  }

  private selectProfile(prof: string): void {
    if (this.running) return;
    this.profile = prof;
    native.setResourceConfig(prof, '', this.memLimitMb);
  }

  private gpuLabel(): string {
    if (this.gpuMode === 'vulkan') return 'Vulkan';
    if (this.gpuMode === 'gles') return 'GLES';
    return 'CPU';
  }

  private statusDotColor(): string {
    if (!this.running) return '#6B7280';
    if (this.paused) return '#F59E0B';
    if (this.wmAgentReady) return '#10B981';
    return '#38BDF8';
  }

  private statusBadgeLabel(): string {
    if (!this.running) return '已停止';
    if (this.paused) return '已暂停';
    if (this.wmAgentReady) return '运行中';
    return '启动中';
  }

  @Builder
  private buildManagerView() {
    Row() {
      // Left Navigation Sidebar (MuMu Player Style)
      Column({ space: 6 }) {
        Button({ type: ButtonType.Normal }) {
          Row({ space: 10 }) {
            SymbolGlyph($r('sys.symbol.display'))
              .fontSize(16)
              .fontColor(['#38BDF8'])
            Text('实例管理')
              .fontSize(13)
              .fontWeight(FontWeight.Medium)
              .fontColor('#F9FAFB')
          }
          .width('100%')
          .alignItems(VerticalAlign.Center)
        }
        .width('100%')
        .height(38)
        .padding({ left: 12, right: 12 })
        .borderRadius(9)
        .backgroundColor('#1C2638')

        Button({ type: ButtonType.Normal }) {
          Row({ space: 10 }) {
            SymbolGlyph($r('sys.symbol.square_grid_2x2'))
              .fontSize(16)
              .fontColor(['#9CA3AF'])
            Text('应用中心')
              .fontSize(13)
              .fontColor('#D1D5DB')
            Blank()
            if (this.appCount > 0) {
              Text(`${this.appCount}`)
                .fontSize(11)
                .fontColor('#93C5FD')
            }
          }
          .width('100%')
          .alignItems(VerticalAlign.Center)
        }
        .width('100%')
        .height(38)
        .padding({ left: 12, right: 12 })
        .borderRadius(9)
        .backgroundColor(Color.Transparent)
        .onClick(() => {
          this.openAppsCenterPage();
        })

        Button({ type: ButtonType.Normal }) {
          Row({ space: 10 }) {
            SymbolGlyph($r('sys.symbol.phone'))
              .fontSize(16)
              .fontColor(['#9CA3AF'])
            Text('传统模拟器')
              .fontSize(13)
              .fontColor('#D1D5DB')
          }
          .width('100%')
          .alignItems(VerticalAlign.Center)
        }
        .width('100%')
        .height(38)
        .padding({ left: 12, right: 12 })
        .borderRadius(9)
        .backgroundColor(Color.Transparent)
        .onClick(() => {
          this.setUiMode('emulator');
        })

        Blank()

        // Host GPU & Runtime telemetry card at bottom of sidebar
        Column({ space: 4 }) {
          Text('宿主引擎')
            .fontSize(10)
            .fontColor('#6B7280')
          Text(this.gpuRenderer.length > 0 ? 'Maleoon 916 · Vulkan' : `GPU: ${this.gpuLabel()}`)
            .fontSize(11)
            .fontWeight(FontWeight.Medium)
            .fontColor('#D1D5DB')
            .maxLines(1)
            .textOverflow({ overflow: TextOverflow.Ellipsis })
          Text(`CPU ${Math.round(this.cpuPercent)}% · ${this.memMb}MB`)
            .fontSize(10)
            .fontColor('#9CA3AF')
        }
        .width('100%')
        .padding(10)
        .backgroundColor('#141821')
        .borderRadius(9)
        .border({ width: 1, color: '#202634' })
        .alignItems(HorizontalAlign.Start)
      }
      .width(176)
      .height('100%')
      .padding({ left: 12, right: 12, top: 14, bottom: 14 })
      .backgroundColor('#10131A')
      .border({ width: { right: 1 }, color: '#1E2430' })

      // Right Main Area: MuMu-style Instance List
      Column({ space: 14 }) {
        // Section Header
        Row() {
          Column({ space: 2 }) {
            Text('Android 实例列表')
              .fontSize(16)
              .fontWeight(600)
              .fontColor('#F9FAFB')
            Text('单实例共享内核 · 独立应用窗口与传统模拟器无缝共用')
              .fontSize(12)
              .fontColor('#6B7280')
          }
          .alignItems(HorizontalAlign.Start)

          Blank()

          // Quick global refresh rate toggle pill
          Row({ space: 3 }) {
            ForEach(REFRESH_RATES, (hz: number) => {
              Button(`${hz}Hz`)
                .height(26)
                .fontSize(11)
                .fontColor(this.refreshRate === hz ? '#FFFFFF' : '#9CA3AF')
                .backgroundColor(this.refreshRate === hz ? '#0284C7' : Color.Transparent)
                .borderRadius(6)
                .padding({ left: 10, right: 10 })
                .onClick(() => { this.selectRefreshRate(hz); })
            })
          }
          .padding(3)
          .backgroundColor('#161B24')
          .borderRadius(8)
          .border({ width: 1, color: '#242B38' })
        }
        .width('100%')
        .alignItems(VerticalAlign.Center)

        // Instance Card (MuMu Row Style)
        Column({ space: 0 }) {
          Row({ space: 14 }) {
            // Left: Instance OS Icon Badge
            Stack({ alignContent: Alignment.Center }) {
              Column()
                .width(44)
                .height(44)
                .borderRadius(11)
                .backgroundColor(this.running ? '#172554' : '#1E232E')
              SymbolGlyph($r('sys.symbol.pad_fill'))
                .fontSize(22)
                .fontColor([this.running ? '#38BDF8' : '#6B7280'])
            }

            // Middle: Instance Name, Status & Spec Metadata
            Column({ space: 5 }) {
              Row({ space: 8 }) {
                Text('Android 14 · 默认实例')
                  .fontSize(15)
                  .fontWeight(600)
                  .fontColor('#F9FAFB')

                Row({ space: 5 }) {
                  Circle({ width: 7, height: 7 })
                    .fill(this.statusDotColor())
                  Text(this.statusBadgeLabel())
                    .fontSize(11)
                    .fontColor(this.statusDotColor())
                }
                .padding({ left: 8, right: 8, top: 2, bottom: 2 })
                .backgroundColor('#1C222E')
                .borderRadius(10)

                Text('API 34')
                  .fontSize(10)
                  .fontColor('#9CA3AF')
                  .padding({ left: 6, right: 6, top: 2, bottom: 2 })
                  .backgroundColor('#1C222E')
                  .borderRadius(5)
              }
              .alignItems(VerticalAlign.Center)

              Text(
                `${this.gpuLabel()} · ${this.refreshRate}Hz · 模拟器 ${RESOLUTION_PRESETS[this.resIdx].w}×${RESOLUTION_PRESETS[this.resIdx].h} · 调度 ${this.profile}` +
                (this.running ? ` · ${this.activeProcs} 进程` : '')
              )
                .fontSize(12)
                .fontColor('#9CA3AF')
                .maxLines(1)
                .textOverflow({ overflow: TextOverflow.Ellipsis })
            }
            .layoutWeight(1)
            .alignItems(HorizontalAlign.Start)

            // Right: Action Buttons (100% native SymbolGlyph)
            Row({ space: 8 }) {
              if (!this.running) {
                Button({ type: ButtonType.Normal }) {
                  Row({ space: 6 }) {
                    SymbolGlyph($r('sys.symbol.play_fill'))
                      .fontSize(14)
                      .fontColor(['#FFFFFF'])
                    Text('启动')
                      .fontSize(13)
                      .fontWeight(FontWeight.Medium)
                      .fontColor('#FFFFFF')
                  }
                }
                .height(34)
                .padding({ left: 16, right: 16 })
                .borderRadius(8)
                .backgroundColor('#059669')
                .onClick(() => {
                  this.launchMode('full');
                  native.setDisplayVisible(0, false);
                })
              } else {
                Button({ type: ButtonType.Normal }) {
                  Row({ space: 6 }) {
                    SymbolGlyph($r('sys.symbol.square_grid_2x2'))
                      .fontSize(14)
                      .fontColor(['#FFFFFF'])
                    Text('应用列表')
                      .fontSize(13)
                      .fontWeight(FontWeight.Medium)
                      .fontColor('#FFFFFF')
                  }
                }
                .height(34)
                .padding({ left: 14, right: 14 })
                .borderRadius(8)
                .backgroundColor('#2563EB')
                .onClick(() => {
                  this.openAppsCenterPage();
                })

                Button({ type: ButtonType.Normal }) {
                  Row({ space: 5 }) {
                    SymbolGlyph($r('sys.symbol.phone'))
                      .fontSize(14)
                      .fontColor(['#E5E7EB'])
                    Text('桌面')
                      .fontSize(12)
                      .fontColor('#E5E7EB')
                  }
                }
                .height(34)
                .padding({ left: 12, right: 12 })
                .borderRadius(8)
                .backgroundColor('#232A37')
                .onClick(() => {
                  this.setUiMode('emulator');
                })

                Button({ type: ButtonType.Normal }) {
                  SymbolGlyph(this.paused ? $r('sys.symbol.play_fill') : $r('sys.symbol.pause_fill'))
                    .fontSize(15)
                    .fontColor([this.paused ? '#34D399' : '#FBBF24'])
                }
                .width(34)
                .height(34)
                .borderRadius(8)
                .backgroundColor('#232A37')
                .onClick(() => {
                  this.statusText = native.setPaused(!this.paused);
                })

                Button({ type: ButtonType.Normal }) {
                  SymbolGlyph($r('sys.symbol.stop_circle_fill'))
                    .fontSize(16)
                    .fontColor(['#F87171'])
                }
                .width(34)
                .height(34)
                .borderRadius(8)
                .backgroundColor('#2A1D22')
                .onClick(() => {
                  this.statusText = native.stopContainer();
                  this.appCount = 0;
                  this.appsFetchedOnce = false;
                  AppStorage.setOrCreate<AndroidAppItem[]>('androidApps', []);
                })
              }

              Button({ type: ButtonType.Normal }) {
                SymbolGlyph($r('sys.symbol.gearshape'))
                  .fontSize(16)
                  .fontColor([this.showInstanceSettings ? '#38BDF8' : '#9CA3AF'])
              }
              .width(34)
              .height(34)
              .borderRadius(8)
              .backgroundColor(this.showInstanceSettings ? '#1C2A3E' : '#232A37')
              .onClick(() => {
                this.showInstanceSettings = !this.showInstanceSettings;
              })
            }
          }
          .width('100%')
          .padding({ left: 16, right: 16, top: 16, bottom: 16 })
          .alignItems(VerticalAlign.Center)

          // Expandable Inline Instance Settings Panel
          if (this.showInstanceSettings) {
            Column({ space: 12 }) {
              Divider().color('#222836')

              // Resolution Presets (for Classic Emulator Display 0)
              Row({ space: 10 }) {
                Text('模拟器分辨率')
                  .fontSize(12)
                  .fontColor('#9CA3AF')
                  .width(84)
                Row({ space: 6 }) {
                  ForEach(RESOLUTION_PRESETS, (item: ResolutionPresetItem, idx: number) => {
                    Button(item.label)
                      .height(26)
                      .fontSize(11)
                      .fontColor(this.resIdx === idx ? '#FFFFFF' : '#9CA3AF')
                      .backgroundColor(this.resIdx === idx ? '#2563EB' : '#1E2430')
                      .borderRadius(6)
                      .padding({ left: 9, right: 9 })
                      .enabled(!this.running)
                      .onClick(() => { this.selectResolution(idx); })
                  })
                }
              }
              .width('100%')
              .alignItems(VerticalAlign.Center)

              // GPU & CPU Profile Row
              Row({ space: 20 }) {
                Row({ space: 10 }) {
                  Text('图形引擎')
                    .fontSize(12)
                    .fontColor('#9CA3AF')
                    .width(84)
                  Row({ space: 6 }) {
                    ForEach(['vulkan', 'gles', 'swiftshader'], (m: string) => {
                      Button(m === 'vulkan' ? 'Vulkan' : (m === 'gles' ? 'GLES' : 'CPU'))
                        .height(26)
                        .fontSize(11)
                        .fontColor(this.gpuMode === m ? '#FFFFFF' : '#9CA3AF')
                        .backgroundColor(this.gpuMode === m ? '#7C3AED' : '#1E2430')
                        .borderRadius(6)
                        .padding({ left: 10, right: 10 })
                        .enabled(!this.running)
                        .onClick(() => { this.selectGpuMode(m); })
                    })
                  }
                }

                Row({ space: 10 }) {
                  Text('性能模式')
                    .fontSize(12)
                    .fontColor('#9CA3AF')
                  Row({ space: 6 }) {
                    ForEach(['Balanced', 'Performance', 'Eco'], (p: string) => {
                      Button(p === 'Balanced' ? '均衡' : (p === 'Performance' ? '性能' : '节能'))
                        .height(26)
                        .fontSize(11)
                        .fontColor(this.profile === p ? '#FFFFFF' : '#9CA3AF')
                        .backgroundColor(this.profile === p ? '#0284C7' : '#1E2430')
                        .borderRadius(6)
                        .padding({ left: 10, right: 10 })
                        .enabled(!this.running)
                        .onClick(() => { this.selectProfile(p); })
                    })
                  }
                }
              }
              .width('100%')
              .alignItems(VerticalAlign.Center)

              if (this.running) {
                Text('提示：刷新率可随时热切换；分辨率、图形引擎与性能模式需在停止实例后修改。')
                  .fontSize(11)
                  .fontColor('#6B7280')
              }
            }
            .width('100%')
            .padding({ left: 16, right: 16, bottom: 16 })
            .alignItems(HorizontalAlign.Start)
          }
        }
        .width('100%')
        .backgroundColor('#151922')
        .borderRadius(12)
        .border({ width: 1, color: '#242B3A' })

        Blank()

        // Compact Footer Runtime Status Bar
        Row({ space: 12 }) {
          Text(this.statusText)
            .fontSize(11)
            .fontColor('#9CA3AF')
            .maxLines(1)
            .textOverflow({ overflow: TextOverflow.Ellipsis })
            .layoutWeight(1)

          if (this.cpuset.length > 0 || this.topology.length > 0) {
            Text(`CPU[${this.cpuset.length > 0 ? this.cpuset : 'all'}] · ${this.topology}`)
              .fontSize(11)
              .fontColor('#6B7280')
              .maxLines(1)
          }
        }
        .width('100%')
        .padding({ left: 12, right: 12, top: 8, bottom: 8 })
        .backgroundColor('#131720')
        .borderRadius(8)
        .border({ width: 1, color: '#1F2532' })
      }
      .layoutWeight(1)
      .height('100%')
      .padding({ left: 18, right: 18, top: 16, bottom: 14 })
    }
    .width('100%')
    .layoutWeight(1)
  }

  @Builder
  private buildEmulatorView() {
    Column() {
      // Emulator Viewport (Display 0)
      Stack({ alignContent: Alignment.Center }) {
        if (!this.surfaceToggle) {
          XComponent({
            id: 'svbox_android_surface',
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
      }
      .width('100%')
      .layoutWeight(1)
      .backgroundColor('#0B0D12')
      .onAreaChange((_oldVal: Area, newVal: Area) => {
        const w: number = Number(newVal.width);
        const h: number = Number(newVal.height);
        if (w > 100 && h > 100) {
          this.stageW = w;
          this.stageH = h;
        }
      })

      // Clean Android Nav Dock (100% HarmonyOS SymbolGlyph)
      Row({ space: 20 }) {
        Button({ type: ButtonType.Normal }) {
          SymbolGlyph($r('sys.symbol.chevron_left'))
            .fontSize(17)
            .fontColor(['#E5E7EB'])
        }
        .width(42)
        .height(30)
        .borderRadius(7)
        .backgroundColor('#1F2530')
        .onClick(() => { native.sendNavKey('back'); })

        Button({ type: ButtonType.Normal }) {
          SymbolGlyph($r('sys.symbol.house'))
            .fontSize(16)
            .fontColor(['#E5E7EB'])
        }
        .width(42)
        .height(30)
        .borderRadius(7)
        .backgroundColor('#1F2530')
        .onClick(() => { native.sendNavKey('home'); })

        Button({ type: ButtonType.Normal }) {
          SymbolGlyph($r('sys.symbol.rectangle_on_rectangle'))
            .fontSize(15)
            .fontColor(['#E5E7EB'])
        }
        .width(42)
        .height(30)
        .borderRadius(7)
        .backgroundColor('#1F2530')
        .onClick(() => { native.sendNavKey('recents'); })
      }
      .width('100%')
      .height(42)
      .backgroundColor('#13161D')
      .border({ width: { top: 1 }, color: '#222733' })
      .justifyContent(FlexAlign.Center)
      .alignItems(VerticalAlign.Center)
    }
    .width('100%')
    .layoutWeight(1)
  }

  build() {
    Column() {
      // Single Integrated HarmonyOS PC Title Bar (44vp, native 3-button avoidance)
      Row({ space: 10 }) {
        if (this.uiMode === 'emulator') {
          Button({ type: ButtonType.Normal }) {
            Row({ space: 4 }) {
              SymbolGlyph($r('sys.symbol.chevron_left'))
                .fontSize(15)
                .fontColor(['#E5E7EB'])
              Text('实例管理')
                .fontSize(12)
                .fontColor('#E5E7EB')
            }
          }
          .height(28)
          .padding({ left: 8, right: 10 })
          .borderRadius(7)
          .backgroundColor('#1E2430')
          .onClick(() => {
            this.setUiMode('manager');
          })

          Text(`传统模拟器 · ${this.dispW}×${this.dispH} · ${this.fps}/${this.refreshRate}Hz`)
            .fontSize(12)
            .fontColor('#9CA3AF')
            .maxLines(1)

          Blank()

          Button({ type: ButtonType.Normal }) {
            Row({ space: 5 }) {
              SymbolGlyph($r('sys.symbol.arrow_up_right'))
                .fontSize(13)
                .fontColor(['#FFFFFF'])
              Text('拆出独立窗')
                .fontSize(12)
                .fontColor('#FFFFFF')
            }
          }
          .height(28)
          .padding({ left: 10, right: 10 })
          .borderRadius(7)
          .backgroundColor('#0284C7')
          .enabled(this.wmAgentReady)
          .onClick(() => {
            this.handlePopOutCurrentEmulatorApp();
          })
        } else {
          Row({ space: 8 }) {
            SymbolGlyph($r('sys.symbol.pad_fill'))
              .fontSize(17)
              .fontColor(['#38BDF8'])
            Text('svbox Android')
              .fontSize(14)
              .fontWeight(600)
              .fontColor('#F9FAFB')
            Text('Android 14')
              .fontSize(10)
              .fontColor('#93C5FD')
              .padding({ left: 6, right: 6, top: 2, bottom: 2 })
              .backgroundColor('#172554')
              .borderRadius(5)
          }
          .alignItems(VerticalAlign.Center)

          Blank()

          if (this.running) {
            Row({ space: 5 }) {
              SymbolGlyph($r('sys.symbol.bolt_fill'))
                .fontSize(12)
                .fontColor(['#34D399'])
              Text(`${this.refreshRate}Hz`)
                .fontSize(11)
                .fontColor('#D1D5DB')
            }
            .padding({ left: 8, right: 8, top: 4, bottom: 4 })
            .backgroundColor('#1A202C')
            .borderRadius(6)
          }
        }
      }
      .width('100%')
      .height(44)
      .padding({ left: 14, right: this.decorRightAvoidVp })
      .backgroundColor('#13161D')
      .border({ width: { bottom: 1 }, color: '#222733' })
      .alignItems(VerticalAlign.Center)

      if (this.uiMode === 'emulator') {
        this.buildEmulatorView()
      } else {
        this.buildManagerView()
      }
    }
    .width('100%')
    .height('100%')
    .backgroundColor('#0E1117')
  }
}
