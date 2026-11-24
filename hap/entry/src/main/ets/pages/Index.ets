import { common, Want } from '@kit.AbilityKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import native, { RuntimeStatus } from 'libentry.so';
import {
  AndroidAppItem,
  InstanceConfig,
  instanceConfigStore,
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

@Entry
@Component
struct Index {
  @StorageLink('mainDecorRightAvoidVp') decorRightAvoidVp: number = 136;

  @State private runMode: string = 'multiwindow'; // 'multiwindow' | 'emulator'
  @State private showInstanceSettings: boolean = false;
  @State private running: boolean = false;
  @State private paused: boolean = false;
  @State private wmAgentReady: boolean = false;
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

  private getSandboxFilesDir(): string {
    const dir: string | undefined = AppStorage.get<string>('sandboxFilesDir');
    return dir && dir.length > 0 ? dir : '/data/storage/el2/base/haps/entry/files';
  }

  private persistInstanceConfig(): void {
    instanceConfigStore.save(this.getSandboxFilesDir(), {
      runMode: this.runMode,
      resolutionKey: RESOLUTION_PRESETS[this.resIdx].key,
      refreshRate: this.refreshRate,
      gpuMode: this.gpuMode,
      profile: this.profile,
    });
  }

  private applyResolutionKey(key: string): void {
    for (let i: number = 0; i < RESOLUTION_PRESETS.length; i++) {
      if (RESOLUTION_PRESETS[i].key === key) {
        this.resIdx = i;
        this.dispW = RESOLUTION_PRESETS[i].w;
        this.dispH = RESOLUTION_PRESETS[i].h;
        return;
      }
    }
  }

  private openAppsCenterPage(): void {
    try {
      this.getUIContext().getRouter().pushUrl({ url: 'pages/AppsPage' });
    } catch (err) {
      hilog.error(0x0000, TAG, 'pushUrl AppsPage failed: %{public}s', String(err));
    }
  }

  private async openEmulatorWindow(): Promise<void> {
    try {
      const ctx: common.UIAbilityContext = this.getUIContext().getHostContext() as common.UIAbilityContext;
      const want: Want = {
        bundleName: 'org.svbox.android',
        abilityName: 'EmulatorWindowAbility',
        parameters: {
          'resolutionKey': RESOLUTION_PRESETS[this.resIdx].key,
        },
      };
      await ctx.startAbility(want);
    } catch (err) {
      hilog.error(0x0000, TAG, 'openEmulatorWindow failed: %{public}s', String(err));
      this.showToast(`打开传统模拟器窗口失败: ${String(err)}`);
    }
  }

  aboutToAppear(): void {
    const saved: InstanceConfig = instanceConfigStore.get(this.getSandboxFilesDir());
    this.runMode = saved.runMode;
    this.applyResolutionKey(saved.resolutionKey);
    this.refreshRate = saved.refreshRate;
    this.gpuMode = saved.gpuMode;
    this.profile = saved.profile;

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
      this.runMode = reqUi;
    }
    AppStorage.setOrCreate<number>('refreshRate', this.refreshRate);
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
        this.startInstance(autoStart);
      }, 300);
    } else if (!this.autoBootTriggered) {
      this.autoBootTriggered = true;
      setTimeout(() => {
        const cur: RuntimeStatus = native.getStatus();
        if (!cur.running) {
          this.startInstance('full');
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
      if (reqUi === 'emulator') {
        AppStorage.setOrCreate<string>('requestedUiMode', '');
        this.openEmulatorWindow();
      } else if (reqUi === 'multiwindow') {
        AppStorage.setOrCreate<string>('requestedUiMode', '');
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
        if (reqRes && reqRes.length > 0) {
          this.applyResolutionKey(reqRes);
        }
        const reqHz: number | undefined = AppStorage.get<number>('refreshRate');
        if (reqHz === 60 || reqHz === 90 || reqHz === 120) {
          this.refreshRate = reqHz;
          native.setRefreshRate(this.refreshRate);
        }
        const mode: string | undefined = AppStorage.get<string>('autoStart');
        if (mode === 'bootanim' || mode === 'full') {
          this.startInstance(mode);
        } else if (mode === 'restart') {
          native.stopContainer();
          this.running = false;
          setTimeout((): void => {
            this.startInstance('full');
          }, 300);
        }
      }

      const st: RuntimeStatus = native.getStatus();
      this.running = st.running;
      this.paused = st.paused;
      this.wmAgentReady = st.wmAgentReady;
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
    }
  }

  private startInstance(mode: string): void {
    const preset: ResolutionPresetItem = RESOLUTION_PRESETS[this.resIdx];
    this.dispW = preset.w;
    this.dispH = preset.h;
    native.setResourceConfig(this.profile, '', this.memLimitMb);
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
    if (this.runMode === 'emulator') {
      native.setDisplayVisible(0, true);
      this.openEmulatorWindow();
    } else {
      native.setDisplayVisible(0, false);
    }
  }

  private selectRunMode(nextMode: string): void {
    if (nextMode !== 'multiwindow' && nextMode !== 'emulator') return;
    this.runMode = nextMode;
    this.persistInstanceConfig();
    if (this.running && nextMode === 'emulator') {
      this.openEmulatorWindow();
    }
  }

  private selectRefreshRate(hz: number): void {
    if (this.running) return;
    this.refreshRate = hz;
    AppStorage.setOrCreate<number>('refreshRate', hz);
    native.setRefreshRate(hz);
    this.persistInstanceConfig();
  }

  private selectResolution(idx: number): void {
    if (this.running) return;
    this.resIdx = idx;
    this.dispW = RESOLUTION_PRESETS[idx].w;
    this.dispH = RESOLUTION_PRESETS[idx].h;
    this.persistInstanceConfig();
  }

  private selectGpuMode(mode: string): void {
    if (this.running) return;
    this.gpuMode = mode;
    native.setGpuMode(mode);
    this.persistInstanceConfig();
  }

  private selectProfile(prof: string): void {
    if (this.running) return;
    this.profile = prof;
    native.setResourceConfig(prof, '', this.memLimitMb);
    this.persistInstanceConfig();
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

  build() {
    Column() {
      // Single Integrated HarmonyOS PC Title Bar (44vp, native 3-button avoidance)
      Row({ space: 10 }) {
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
      .width('100%')
      .height(44)
      .padding({ left: 14, right: Math.max(152, this.decorRightAvoidVp) })
      .backgroundColor('#13161D')
      .border({ width: { bottom: 1 }, color: '#222733' })
      .alignItems(VerticalAlign.Center)

      // MuMu-Style Sidebar + Instance List Workspace
      Row() {
        // Left Navigation Sidebar
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
              SymbolGlyph($r('sys.symbol.pad'))
                .fontSize(16)
                .fontColor(['#9CA3AF'])
              Text('传统模拟器')
                .fontSize(13)
                .fontColor('#D1D5DB')
              Blank()
              SymbolGlyph($r('sys.symbol.arrow_up_right'))
                .fontSize(12)
                .fontColor(['#6B7280'])
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
            this.openEmulatorWindow();
          })

          Blank()

          // Host GPU & Telemetry Card
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
        .width(180)
        .height('100%')
        .padding({ left: 12, right: 12, top: 14, bottom: 14 })
        .backgroundColor('#10131A')
        .border({ width: { right: 1 }, color: '#1E2430' })

        // Right Main Area: MuMu-style Instance List
        Column({ space: 14 }) {
          Row() {
            Column({ space: 2 }) {
              Text('Android 实例列表')
                .fontSize(16)
                .fontWeight(600)
                .fontColor('#F9FAFB')
              Text('单实例共享内核 · 自由多窗与独立传统模拟器窗口无缝共用')
                .fontSize(12)
                .fontColor('#6B7280')
            }
            .alignItems(HorizontalAlign.Start)

            Blank()
          }
          .width('100%')
          .alignItems(VerticalAlign.Center)

          // Instance Card
          Column({ space: 0 }) {
            Row({ space: 14 }) {
              Stack({ alignContent: Alignment.Center }) {
                Column()
                  .width(44)
                  .height(44)
                  .borderRadius(11)
                  .backgroundColor(this.running ? '#172554' : '#1E232E')
                SymbolGlyph(this.runMode === 'emulator' ? $r('sys.symbol.display') : $r('sys.symbol.pad_fill'))
                  .fontSize(22)
                  .fontColor([this.running ? '#38BDF8' : '#6B7280'])
              }

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

                  Text(this.runMode === 'emulator' ? '传统模拟器' : '自由多窗')
                    .fontSize(10)
                    .fontColor(this.runMode === 'emulator' ? '#34D399' : '#93C5FD')
                    .padding({ left: 7, right: 7, top: 2, bottom: 2 })
                    .backgroundColor(this.runMode === 'emulator' ? '#064E3B' : '#172554')
                    .borderRadius(5)

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

              // Right Action Group (100% HarmonyOS SymbolGlyph)
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
                    this.startInstance('full');
                  })
                } else {
                  if (this.runMode === 'emulator') {
                    Button({ type: ButtonType.Normal }) {
                      Row({ space: 6 }) {
                        SymbolGlyph($r('sys.symbol.pad'))
                          .fontSize(14)
                          .fontColor(['#FFFFFF'])
                        Text('打开模拟器')
                          .fontSize(13)
                          .fontWeight(FontWeight.Medium)
                          .fontColor('#FFFFFF')
                      }
                    }
                    .height(34)
                    .padding({ left: 14, right: 14 })
                    .borderRadius(8)
                    .backgroundColor('#059669')
                    .onClick(() => {
                      this.openEmulatorWindow();
                    })

                    Button({ type: ButtonType.Normal }) {
                      Row({ space: 5 }) {
                        SymbolGlyph($r('sys.symbol.square_grid_2x2'))
                          .fontSize(14)
                          .fontColor(['#E5E7EB'])
                        Text('应用列表')
                          .fontSize(12)
                          .fontColor('#E5E7EB')
                      }
                    }
                    .height(34)
                    .padding({ left: 12, right: 12 })
                    .borderRadius(8)
                    .backgroundColor('#232A37')
                    .onClick(() => {
                      this.openAppsCenterPage();
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
                        SymbolGlyph($r('sys.symbol.pad'))
                          .fontSize(14)
                          .fontColor(['#E5E7EB'])
                        Text('模拟器窗')
                          .fontSize(12)
                          .fontColor('#E5E7EB')
                      }
                    }
                    .height(34)
                    .padding({ left: 12, right: 12 })
                    .borderRadius(8)
                    .backgroundColor('#232A37')
                    .onClick(() => {
                      this.openEmulatorWindow();
                    })
                  }

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

                // Running Mode: Free Multi-Window vs Classic Emulator (Standalone Window)
                Row({ space: 10 }) {
                  Text('运行模式')
                    .fontSize(12)
                    .fontColor('#9CA3AF')
                    .width(84)
                  Row({ space: 6 }) {
                    Button({ type: ButtonType.Normal }) {
                      Row({ space: 5 }) {
                        SymbolGlyph($r('sys.symbol.square_grid_2x2'))
                          .fontSize(12)
                          .fontColor([this.runMode === 'multiwindow' ? '#FFFFFF' : '#9CA3AF'])
                        Text('自由多窗口')
                          .fontSize(11)
                          .fontColor(this.runMode === 'multiwindow' ? '#FFFFFF' : '#9CA3AF')
                      }
                    }
                    .height(28)
                    .padding({ left: 11, right: 11 })
                    .borderRadius(6)
                    .backgroundColor(this.runMode === 'multiwindow' ? '#2563EB' : '#1E2430')
                    .onClick(() => { this.selectRunMode('multiwindow'); })

                    Button({ type: ButtonType.Normal }) {
                      Row({ space: 5 }) {
                        SymbolGlyph($r('sys.symbol.pad'))
                          .fontSize(12)
                          .fontColor([this.runMode === 'emulator' ? '#FFFFFF' : '#9CA3AF'])
                        Text('传统模拟器')
                          .fontSize(11)
                          .fontColor(this.runMode === 'emulator' ? '#FFFFFF' : '#9CA3AF')
                      }
                    }
                    .height(28)
                    .padding({ left: 11, right: 11 })
                    .borderRadius(6)
                    .backgroundColor(this.runMode === 'emulator' ? '#059669' : '#1E2430')
                    .onClick(() => { this.selectRunMode('emulator'); })
                  }
                }
                .width('100%')
                .alignItems(VerticalAlign.Center)

                // Refresh Rate (only editable when instance is stopped)
                Row({ space: 10 }) {
                  Text('刷新率')
                    .fontSize(12)
                    .fontColor('#9CA3AF')
                    .width(84)
                  Row({ space: 6 }) {
                    ForEach(REFRESH_RATES, (hz: number) => {
                      Button(`${hz}Hz`)
                        .height(26)
                        .fontSize(11)
                        .fontColor(this.refreshRate === hz ? '#FFFFFF' : '#9CA3AF')
                        .backgroundColor(this.refreshRate === hz ? '#0284C7' : '#1E2430')
                        .borderRadius(6)
                        .padding({ left: 10, right: 10 })
                        .enabled(!this.running)
                        .onClick(() => { this.selectRefreshRate(hz); })
                    })
                  }
                }
                .width('100%')
                .alignItems(VerticalAlign.Center)

                // Resolution Presets (for Classic Emulator Window)
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
                  Text('提示：运行模式可随时切换；刷新率、分辨率、图形引擎与性能模式需在停止实例后修改。')
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
    .width('100%')
    .height('100%')
    .backgroundColor('#0E1117')
  }
}
