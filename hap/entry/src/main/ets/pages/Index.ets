import native, { RuntimeStatus } from 'libentry.so';

interface ResolutionPreset {
  label: string;
  key: string;
  w: number;
  h: number;
}

const RESOLUTION_PRESETS: ResolutionPreset[] = [
  { label: '720×1280 竖', key: '720x1280', w: 720, h: 1280 },
  { label: '1080×1920 竖', key: '1080x1920', w: 1080, h: 1920 },
  { label: '1280×720 横', key: '1280x720', w: 1280, h: 720 },
  { label: '1600×900 横', key: '1600x900', w: 1600, h: 900 },
  { label: '1920×1080 横', key: '1920x1080', w: 1920, h: 1080 },
];

const REFRESH_RATES: number[] = [60, 90, 120];

@Entry
@Component
struct Index {
  @State private running: boolean = false;
  @State private paused: boolean = false;
  @State private connected: boolean = false;
  @State private vsyncActive: boolean = false;
  @State private inputReady: boolean = false;
  @State private audioActive: boolean = false;
  @State private frames: number = 0;
  @State private fps: number = 0;
  @State private refreshRate: number = 60;
  @State private resIdx: number = 0;
  @State private displayW: number = 720;
  @State private displayH: number = 1280;
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
  @State private statusText: string = 'Ready — Android 14 (60/90/120Hz · Multi-Resolution)';
  @State private svboxPath: string = '/storage/Users/currentUser/Workspace/svbox/build/svbox';
  @State private androidDir: string = '/storage/Users/currentUser/Workspace/svbox-Android';
  private pollTimer: number = -1;
  private lastLaunchSeq: number = 0;

  private applyResolutionKey(key: string): boolean {
    for (let i: number = 0; i < RESOLUTION_PRESETS.length; i++) {
      if (RESOLUTION_PRESETS[i].key === key) {
        const changed: boolean = (this.resIdx !== i);
        this.resIdx = i;
        this.displayW = RESOLUTION_PRESETS[i].w;
        this.displayH = RESOLUTION_PRESETS[i].h;
        return changed;
      }
    }
    return false;
  }

  private computeFitWidth(): number {
    const padW: number = Math.max(120, this.stageW * 0.96);
    const padH: number = Math.max(120, this.stageH * 0.96);
    const aspect: number = this.displayW / this.displayH;
    if (padW / padH > aspect) {
      return Math.floor(padH * aspect);
    }
    return Math.floor(padW);
  }

  private computeFitHeight(): number {
    const padW: number = Math.max(120, this.stageW * 0.96);
    const padH: number = Math.max(120, this.stageH * 0.96);
    const aspect: number = this.displayW / this.displayH;
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
    this.lastLaunchSeq = AppStorage.get<number>('launchSeq') ?? 0;
    try {
      native.setResourceConfig(this.profile, '', this.memLimitMb);
      native.setGpuMode(this.gpuMode);
      native.setRefreshRate(this.refreshRate);
    } catch (_e) {
    }
    const autoMode: string | undefined = AppStorage.get<string>('autoStart');
    if (autoMode && (autoMode === 'bootanim' || autoMode === 'full')) {
      setTimeout(() => {
        if (!this.running) {
          this.launchMode(autoMode);
        }
      }, 300);
    }
    this.pollTimer = setInterval(() => {
      try {
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
      } catch (_e) {
      }
    }, 500);
  }

  aboutToDisappear(): void {
    if (this.pollTimer !== -1) {
      clearInterval(this.pollTimer);
      this.pollTimer = -1;
    }
    try {
      native.stopContainer();
    } catch (_e) {
    }
  }

  private getSandboxTempDir(): string {
    const dir: string | undefined = AppStorage.get<string>('sandboxTempDir');
    return dir && dir.length > 0 ? dir : '/data/storage/el2/base/haps/entry/temp';
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
    try {
      native.setRefreshRate(this.refreshRate);
    } catch (_e) {
    }
  }

  private cycleResolution(): void {
    this.resIdx = (this.resIdx + 1) % RESOLUTION_PRESETS.length;
    this.displayW = RESOLUTION_PRESETS[this.resIdx].w;
    this.displayH = RESOLUTION_PRESETS[this.resIdx].h;
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
      this.displayW,
      this.displayH
    );
  }

  private buildBadgeText(): string {
    if (this.paused) {
      return `Paused (0% CPU) · #${this.frames}`;
    }
    if (this.connected) {
      const gpuTag = this.gpuLabel();
      const inTag = this.inputReady ? 'Input' : 'NoIn';
      const audTag = this.audioActive ? 'Aud' : 'Idle';
      return `${gpuTag} · ${this.displayW}×${this.displayH} · ${this.fps}/${this.refreshRate}Hz · CPU ${this.cpuPercent}% · ${this.memMb}MB · ${inTag}/${audTag} · #${this.frames}`;
    }
    return this.running ? `Booting A14 (${this.gpuLabel()} · ${this.displayW}×${this.displayH}@${this.refreshRate}Hz)...` : 'Idle · Android 14';
  }

  build() {
    Column() {
      // Top control bar
      Row({ space: 6 }) {
        Text('svbox-A14')
          .fontSize(14)
          .fontWeight(FontWeight.Bold)
          .fontColor('#F3F4F6')

        Text(this.buildBadgeText())
          .fontSize(11)
          .fontColor(this.paused ? '#FBBF24' : (this.connected ? '#34D399' : '#9CA3AF'))
          .padding({ left: 8, right: 8, top: 4, bottom: 4 })
          .backgroundColor('#1F2937')
          .borderRadius(6)
          .maxLines(1)

        Blank()

        Button(`${this.refreshRate}Hz`)
          .height(28)
          .fontSize(11)
          .backgroundColor(this.refreshRate === 120 ? '#059669' : (this.refreshRate === 90 ? '#0284C7' : '#374151'))
          .onClick(() => {
            this.cycleRefreshRate();
          })

        Button(RESOLUTION_PRESETS[this.resIdx].label)
          .height(28)
          .fontSize(11)
          .backgroundColor('#374151')
          .enabled(!this.running)
          .onClick(() => {
            this.cycleResolution();
          })

        Button(`GPU: ${this.gpuLabel()}`)
          .height(28)
          .fontSize(11)
          .backgroundColor(this.gpuMode === 'swiftshader' ? '#374151' : (this.gpuMode === 'vulkan' ? '#7C3AED' : '#0284C7'))
          .enabled(!this.running)
          .onClick(() => {
            this.cycleGpuMode();
          })

        Button(`CPU: ${this.profile}`)
          .height(28)
          .fontSize(11)
          .backgroundColor('#374151')
          .enabled(!this.running)
          .onClick(() => {
            this.cycleProfile();
          })

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

          Button(this.paused ? 'Resume' : 'Pause')
            .height(28)
            .fontSize(11)
            .backgroundColor(this.paused ? '#10B981' : '#D97706')
            .onClick(() => {
              this.statusText = native.setPaused(!this.paused);
            })
        } else {
          Button('BootAnim')
            .height(28)
            .fontSize(11)
            .backgroundColor('#2563EB')
            .onClick(() => {
              this.launchMode('bootanim');
            })

          Button('Full Android 14')
            .height(28)
            .fontSize(11)
            .backgroundColor('#059669')
            .onClick(() => {
              this.launchMode('full');
            })
        }

        Button('Stop')
          .height(28)
          .fontSize(11)
          .backgroundColor('#DC2626')
          .enabled(this.running)
          .onClick(() => {
            this.statusText = native.stopContainer();
          })
      }
      .width('100%')
      .height(48)
      .padding({ left: 12, right: 12 })
      .backgroundColor('#181B22')
      .alignItems(VerticalAlign.Center)

      // Center Android display surface (auto-fitted to both portrait & landscape presets).
      Stack({ alignContent: Alignment.Center }) {
        if (this.surfaceEpoch % 2 === 0) {
          XComponent({
            id: 'svbox_android_surface_a',
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
      .backgroundColor('#0D0F14')
      .onAreaChange((_oldVal: Area, newVal: Area) => {
        const w: number = Number(newVal.width);
        const h: number = Number(newVal.height);
        if (w > 100 && h > 100) {
          this.stageW = w;
          this.stageH = h;
        }
      })

      // Bottom status bar
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
