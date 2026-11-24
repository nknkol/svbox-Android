import { common, Want } from '@kit.AbilityKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import native, { RuntimeStatus } from 'libentry.so';
import {
  AndroidAppItem,
  findPresetByKey,
  InstanceConfig,
  instanceConfigStore,
  ResolutionPresetItem
} from '../common/AppWindowPrefs';

const TAG: string = 'SvboxEmuPage';

interface PopOutReply {
  ok?: boolean;
  taskId?: number;
  packageName?: string;
  component?: string;
  label?: string;
  error?: string;
}

@Entry({ useSharedStorage: true })
@Component
struct EmulatorWindowPage {
  @LocalStorageProp('sandboxTempDir') sandboxTempDir: string = '';
  @LocalStorageProp('sandboxFilesDir') sandboxFilesDir: string = '';
  @LocalStorageProp('decorRightAvoidVp') decorRightAvoidVp: number = 152;

  @State private running: boolean = false;
  @State private wmAgentReady: boolean = false;
  @State private dispW: number = 720;
  @State private dispH: number = 1280;
  @State private fps: number = 0;
  @State private refreshRate: number = 60;
  @State private statusText: string = '正在连接 Android 14 模拟器屏幕...';
  @State private stageW: number = 420;
  @State private stageH: number = 720;

  private pollTimer: number = -1;
  private svboxPath: string = '/storage/Users/currentUser/Workspace/svbox/build/svbox';
  private androidDir: string = '/storage/Users/currentUser/Workspace/svbox-Android';

  private showToast(msg: string): void {
    try {
      this.getUIContext().getPromptAction().showToast({ message: msg, duration: 2000 });
    } catch (_e) {
    }
  }

  private computeFitWidth(): number {
    const padW: number = Math.max(120, this.stageW * 0.99);
    const padH: number = Math.max(120, this.stageH * 0.99);
    const aspect: number = this.dispW / Math.max(1, this.dispH);
    if (padW / padH > aspect) {
      return Math.floor(padH * aspect);
    }
    return Math.floor(padW);
  }

  private computeFitHeight(): number {
    const padW: number = Math.max(120, this.stageW * 0.99);
    const padH: number = Math.max(120, this.stageH * 0.99);
    const aspect: number = this.dispW / Math.max(1, this.dispH);
    if (padW / padH > aspect) {
      return Math.floor(padH);
    }
    return Math.floor(padW / aspect);
  }

  aboutToAppear(): void {
    const filesDir: string = this.sandboxFilesDir.length > 0
      ? this.sandboxFilesDir : '/data/storage/el2/base/haps/entry/files';
    const cfg: InstanceConfig = instanceConfigStore.get(filesDir);
    const preset: ResolutionPresetItem = findPresetByKey(cfg.resolutionKey);
    this.dispW = preset.w;
    this.dispH = preset.h;
    this.refreshRate = cfg.refreshRate;

    const st: RuntimeStatus = native.getStatus();
    if (!st.running) {
      const tempDir: string = this.sandboxTempDir.length > 0
        ? this.sandboxTempDir : '/data/storage/el2/base/haps/entry/temp';
      try {
        native.setResourceConfig(cfg.profile, '', 4096);
        native.setGpuMode(cfg.gpuMode);
        native.setRefreshRate(cfg.refreshRate);
        this.statusText = native.startContainer(tempDir, this.svboxPath, this.androidDir, 'full', preset.w, preset.h);
      } catch (_e) {
      }
    }

    try {
      native.setDisplayVisible(0, true);
      if (native.isWmAgentReady()) {
        native.wmRpcAsync('FOCUS_DISPLAY 0');
      }
    } catch (_e) {
    }

    this.pollTimer = setInterval(() => {
      try {
        const cur: RuntimeStatus = native.getStatus();
        this.running = cur.running;
        this.wmAgentReady = cur.wmAgentReady;
        this.fps = cur.fps;
        if (cur.width > 0 && cur.height > 0) {
          this.dispW = cur.width;
          this.dispH = cur.height;
        }
        if (cur.refreshRate === 60 || cur.refreshRate === 90 || cur.refreshRate === 120) {
          this.refreshRate = cur.refreshRate;
        }
        this.statusText = cur.statusText;
      } catch (_e) {
      }
    }, 450);
  }

  aboutToDisappear(): void {
    if (this.pollTimer !== -1) {
      clearInterval(this.pollTimer);
      this.pollTimer = -1;
    }
    try {
      native.setDisplayVisible(0, false);
    } catch (_e) {
    }
  }

  private async handlePopOutCurrentApp(): Promise<void> {
    if (!this.wmAgentReady) {
      this.showToast('等待 Android 14 窗口服务就绪');
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
        const ctx: common.UIAbilityContext = this.getUIContext().getHostContext() as common.UIAbilityContext;
        const want: Want = {
          bundleName: 'org.svbox.android',
          abilityName: 'AppWindowAbility',
          parameters: {
            'androidPkg': targetPkg,
            'androidComp': targetComp,
            'androidLabel': label,
            'moveTaskId': String(targetTid),
          },
        };
        await ctx.startAbility(want);
      }
    } catch (err) {
      hilog.warn(0x0000, TAG, 'popOut failed: %{public}s', String(err));
      this.showToast(`拆出失败: ${String(err)}`);
    }
  }

  build() {
    Column() {
      // Single Immersive Dark Title Bar (38vp, strictly avoiding native 3-button area)
      Row({ space: 6 }) {
        Row({ space: 6 }) {
          SymbolGlyph($r('sys.symbol.display'))
            .fontSize(15)
            .fontColor(['#38BDF8'])

          Text('传统模拟器')
            .fontSize(13)
            .fontWeight(FontWeight.Medium)
            .fontColor('#F3F4F6')
            .maxLines(1)
            .textOverflow({ overflow: TextOverflow.Ellipsis })
            .layoutWeight(1)

          Text(`${this.fps}/${this.refreshRate}Hz`)
            .fontSize(10)
            .fontColor('#9CA3AF')
            .padding({ left: 6, right: 6, top: 2, bottom: 2 })
            .backgroundColor('#1E2430')
            .borderRadius(5)
            .maxLines(1)
        }
        .layoutWeight(1)
        .alignItems(VerticalAlign.Center)

        Button({ type: ButtonType.Normal }) {
          Row({ space: 4 }) {
            SymbolGlyph($r('sys.symbol.arrow_up_right'))
              .fontSize(12)
              .fontColor(['#E5E7EB'])
            Text('拆出')
              .fontSize(11)
              .fontColor('#E5E7EB')
          }
        }
        .height(26)
        .padding({ left: 8, right: 8 })
        .borderRadius(6)
        .backgroundColor('#1E2430')
        .enabled(this.wmAgentReady)
        .onClick(() => { this.handlePopOutCurrentApp(); })
      }
      .width('100%')
      .height(38)
      .padding({ left: 10, right: Math.max(152, this.decorRightAvoidVp) })
      .backgroundColor('#13161D')
      .border({ width: { bottom: 1 }, color: '#222733' })
      .alignItems(VerticalAlign.Center)

      // Display 0 Zero-Copy Vulkan Viewport
      Stack({ alignContent: Alignment.Center }) {
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

        if (!this.running) {
          Column({ space: 10 }) {
            LoadingProgress()
              .width(34)
              .height(34)
              .color('#38BDF8')
            Text(this.statusText)
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
          this.stageW = w;
          this.stageH = h;
        }
      })

      // Compact Android Navigation Dock (100% HarmonyOS SymbolGlyph)
      Row({ space: 22 }) {
        Button({ type: ButtonType.Normal }) {
          SymbolGlyph($r('sys.symbol.chevron_left'))
            .fontSize(16)
            .fontColor(['#E5E7EB'])
        }
        .width(42)
        .height(28)
        .borderRadius(7)
        .backgroundColor('#1E2430')
        .onClick(() => { native.sendNavKey('back'); })

        Button({ type: ButtonType.Normal }) {
          SymbolGlyph($r('sys.symbol.house'))
            .fontSize(15)
            .fontColor(['#E5E7EB'])
        }
        .width(42)
        .height(28)
        .borderRadius(7)
        .backgroundColor('#1E2430')
        .onClick(() => { native.sendNavKey('home'); })

        Button({ type: ButtonType.Normal }) {
          SymbolGlyph($r('sys.symbol.rectangle_on_rectangle'))
            .fontSize(14)
            .fontColor(['#E5E7EB'])
        }
        .width(42)
        .height(28)
        .borderRadius(7)
        .backgroundColor('#1E2430')
        .onClick(() => { native.sendNavKey('recents'); })
      }
      .width('100%')
      .height(40)
      .backgroundColor('#13161D')
      .border({ width: { top: 1 }, color: '#222733' })
      .justifyContent(FlexAlign.Center)
      .alignItems(VerticalAlign.Center)
    }
    .width('100%')
    .height('100%')
    .backgroundColor('#0B0D12')
  }
}
