import native, { RuntimeStatus } from 'libentry.so';

@Entry
@Component
struct Index {
  @State private running: boolean = false;
  @State private connected: boolean = false;
  @State private frames: number = 0;
  @State private fps: number = 0;
  @State private statusText: string = 'Ready — click BootAnimation Demo or Full Android 11';
  @State private svboxPath: string = '/storage/Users/currentUser/Workspace/svbox/build/svbox';
  @State private androidDir: string = '/storage/Users/currentUser/Workspace/svbox-Android';
  private pollTimer: number = -1;
  private readonly displayW: number = 480;
  private readonly displayH: number = 854;

  aboutToAppear(): void {
    this.pollTimer = setInterval(() => {
      try {
        const st: RuntimeStatus = native.getStatus();
        this.running = st.running;
        this.connected = st.connected;
        this.frames = st.frames;
        this.fps = st.fps;
        if (st.statusText.length > 0) {
          this.statusText = st.statusText;
        }
      } catch (_e) {
      }
    }, 300);
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

  private launchMode(mode: string): void {
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

  build() {
    Column() {
      // Top control bar
      Row({ space: 10 }) {
        Text('svbox-Android')
          .fontSize(16)
          .fontWeight(FontWeight.Bold)
          .fontColor('#F3F4F6')

        Text(this.connected ? `HWC Live · ${this.fps} fps · #${this.frames}` : (this.running ? 'Booting...' : 'Idle'))
          .fontSize(12)
          .fontColor(this.connected ? '#34D399' : '#9CA3AF')
          .padding({ left: 8, right: 8, top: 4, bottom: 4 })
          .backgroundColor('#1F2937')
          .borderRadius(6)

        Blank()

        Button('BootAnim Demo')
          .height(30)
          .fontSize(12)
          .backgroundColor('#2563EB')
          .enabled(!this.running)
          .onClick(() => {
            this.launchMode('bootanim');
          })

        Button('Full Android 11')
          .height(30)
          .fontSize(12)
          .backgroundColor('#059669')
          .enabled(!this.running)
          .onClick(() => {
            this.launchMode('full');
          })

        Button('Stop')
          .height(30)
          .fontSize(12)
          .backgroundColor('#DC2626')
          .enabled(this.running)
          .onClick(() => {
            this.statusText = native.stopContainer();
          })
      }
      .width('100%')
      .height(48)
      .padding({ left: 16, right: 16 })
      .backgroundColor('#181B22')
      .alignItems(VerticalAlign.Center)

      // Center Android display surface (XComponent backed by OHNativeWindow)
      Stack({ alignContent: Alignment.Center }) {
        XComponent({
          id: 'svbox_android_surface',
          type: XComponentType.SURFACE,
          libraryname: 'entry'
        })
          .height('94%')
          .aspectRatio(this.displayW / this.displayH)
          .backgroundColor('#000000')
      }
      .width('100%')
      .layoutWeight(1)
      .backgroundColor('#0D0F14')

      // Bottom status bar
      Row() {
        Text(this.statusText)
          .fontSize(11)
          .fontColor('#9CA3AF')
          .maxLines(1)
          .textOverflow({ overflow: TextOverflow.Ellipsis })
      }
      .width('100%')
      .height(28)
      .padding({ left: 16, right: 16 })
      .backgroundColor('#181B22')
      .alignItems(VerticalAlign.Center)
    }
    .width('100%')
    .height('100%')
    .backgroundColor('#0D0F14')
  }
}
