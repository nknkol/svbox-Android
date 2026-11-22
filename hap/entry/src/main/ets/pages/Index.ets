import native, { RuntimeStatus } from 'libentry.so';

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
  @State private cpuPercent: number = 0;
  @State private memMb: number = 0;
  @State private memLimitMb: number = 4096;
  @State private activeProcs: number = 0;
  @State private profile: string = 'perf';
  @State private cpuset: string = '';
  @State private topology: string = '';
  @State private statusText: string = 'Ready — click Full Android 11 or BootAnim Demo';
  @State private svboxPath: string = '/storage/Users/currentUser/Workspace/svbox/build/svbox';
  @State private androidDir: string = '/storage/Users/currentUser/Workspace/svbox-Android';
  private pollTimer: number = -1;
  private readonly displayW: number = 720;
  private readonly displayH: number = 1280;

  aboutToAppear(): void {
    try {
      native.setResourceConfig(this.profile, '', this.memLimitMb);
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
        const st: RuntimeStatus = native.getStatus();
        this.running = st.running;
        this.paused = st.paused;
        this.connected = st.connected;
        this.vsyncActive = st.vsyncActive;
        this.inputReady = st.inputReady;
        this.audioActive = st.audioActive;
        this.frames = st.frames;
        this.fps = st.fps;
        this.cpuPercent = Math.round(st.cpuPercent);
        this.memMb = st.memMb;
        this.memLimitMb = st.memLimitMb;
        this.activeProcs = st.activeProcs;
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

  private launchMode(mode: string): void {
    try {
      native.setResourceConfig(this.profile, '', this.memLimitMb);
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
      const syncTag = this.vsyncActive ? 'VSync' : 'Async';
      const inTag = this.inputReady ? 'Input' : 'NoIn';
      const audTag = this.audioActive ? 'Aud' : 'Idle';
      return `${syncTag} · ${this.fps} fps · CPU ${this.cpuPercent}% · ${this.memMb}/${this.memLimitMb}MB · ${inTag}/${audTag} · #${this.frames}`;
    }
    return this.running ? 'Booting...' : 'Idle';
  }

  build() {
    Column() {
      // Top control bar
      Row({ space: 8 }) {
        Text('svbox-Android')
          .fontSize(15)
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

        Button(`Profile: ${this.profile}`)
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

          Button('Full Android 11')
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
      .padding({ left: 14, right: 14 })
      .backgroundColor('#181B22')
      .alignItems(VerticalAlign.Center)

      // Center Android display surface (XComponent backed by OHNativeWindow)
      Stack({ alignContent: Alignment.Center }) {
        XComponent({
          id: 'svbox_android_surface',
          type: XComponentType.SURFACE,
          libraryname: 'entry'
        })
          .focusable(true)
          .defaultFocus(true)
          .height('95%')
          .aspectRatio(this.displayW / this.displayH)
          .backgroundColor('#000000')
      }
      .width('100%')
      .layoutWeight(1)
      .backgroundColor('#0D0F14')

      // Bottom status bar
      Row({ space: 12 }) {
        Text(this.statusText)
          .fontSize(11)
          .fontColor('#9CA3AF')
          .maxLines(1)
          .textOverflow({ overflow: TextOverflow.Ellipsis })
          .layoutWeight(1)

        if (this.cpuset.length > 0 || this.topology.length > 0) {
          Text(`CPU[${this.cpuset.length > 0 ? this.cpuset : 'all'}] · ${this.topology} · ${this.activeProcs} procs`)
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
