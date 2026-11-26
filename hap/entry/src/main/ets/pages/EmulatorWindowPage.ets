import { common, Want } from '@kit.AbilityKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import native, { RuntimeStatus } from 'libentry.so';
import {
  AndroidAppItem,
  findPresetByKey,
  ResolutionPresetItem
} from '../common/AppWindowPrefs';
import {
  ControlSize,
  FontSize,
  IconSize,
  Motion,
  Radius,
  Spacing
} from '../common/DesignTokens';
import {
  pasteHostClipboardToAndroid,
  sendTextToAndroid,
  syncHostClipboardToAndroid
} from '../common/InputBridgeHelper';
import { DeviceInstance, instanceManager } from '../model/InstanceModel';

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
  @LocalStorageProp('emuInstanceName') emuInstanceName: string = '';
  @LocalStorageProp('emuInstanceId') emuInstanceId: string = '';

  @State private running: boolean = false;
  @State private wmAgentReady: boolean = false;
  @State private dispW: number = 720;
  @State private dispH: number = 1280;
  @State private fps: number = 0;
  @State private refreshRate: number = 120;
  @State private statusText: string = '正在连接 Android 14 模拟器屏幕...';
  @State private stageW: number = 420;
  @State private stageH: number = 720;
  @State private showImeBar: boolean = false;
  @State private imeDraft: string = '';

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
    const padW: number = Math.max(120, this.stageW);
    const padH: number = Math.max(120, this.stageH);
    const aspect: number = this.dispW / Math.max(1, this.dispH);
    if (padW / padH > aspect) {
      return Math.floor(padH * aspect);
    }
    return Math.floor(padW);
  }

  private computeFitHeight(): number {
    const padW: number = Math.max(120, this.stageW);
    const padH: number = Math.max(120, this.stageH);
    const aspect: number = this.dispW / Math.max(1, this.dispH);
    if (padW / padH > aspect) {
      return Math.floor(padH);
    }
    return Math.floor(padW / aspect);
  }

  aboutToAppear(): void {
    const filesDir: string = this.sandboxFilesDir.length > 0
      ? this.sandboxFilesDir : '/data/storage/el2/base/haps/entry/files';
    const inst: DeviceInstance | undefined = this.emuInstanceId.length > 0
      ? instanceManager.getById(filesDir, this.emuInstanceId)
      : instanceManager.getAll(filesDir)[0];
    if (inst) {
      this.dispW = inst.width;
      this.dispH = inst.height;
      this.refreshRate = inst.refreshRate;
    } else {
      const preset: ResolutionPresetItem = findPresetByKey('720x1280');
      this.dispW = preset.w;
      this.dispH = preset.h;
    }

    const st: RuntimeStatus = native.getStatus();
    if (!st.running && inst) {
      const tempDir: string = this.sandboxTempDir.length > 0
        ? this.sandboxTempDir : '/data/storage/el2/base/haps/entry/temp';
      try {
        native.setInstanceConfig(
          inst.id, inst.dataSubdir, inst.dpi, inst.enableAudio,
          inst.enableRoot, inst.deviceBrand, inst.deviceModel, inst.adbPort
        );
        native.setResourceConfig(inst.profile, '', inst.memLimitMb, inst.diskLimitGb, inst.cpuCores);
        native.setGpuMode(inst.gpuMode);
        native.setSyscallMode(inst.syscallMode || 'adaptive');
        native.setRefreshRate(inst.refreshRate);
        this.statusText = native.startContainer(tempDir, this.svboxPath, this.androidDir, 'full', inst.width, inst.height);
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
      this.showToast(`拆出失败：${String(err)}`);
    }
  }

  @Builder
  private emuMoreMenuBuilder() {
    Menu() {
      MenuItem({ content: '音量加' })
        .onClick(() => { native.sendNavKey('vol_up'); })
      MenuItem({ content: '音量减' })
        .onClick(() => { native.sendNavKey('vol_down'); })
      MenuItem({ content: '电源键 / 锁屏唤醒' })
        .onClick(() => { native.sendNavKey('power'); })
      MenuItem({ content: '粘贴鸿蒙剪贴板到 Android' })
        .enabled(this.wmAgentReady)
        .onClick(() => { this.handlePasteClipboard(); })
      MenuItem({ content: '拆出前台应用到独立窗口' })
        .enabled(this.wmAgentReady)
        .onClick(() => { this.handlePopOutCurrentApp(); })
    }
  }

  private async handleSubmitImeText(): Promise<void> {
    const txt: string = this.imeDraft;
    if (txt.length === 0) return;
    if (!this.wmAgentReady) {
      this.showToast('等待窗口代理就绪后再发送文本');
      return;
    }
    this.imeDraft = '';
    const ok: boolean = await sendTextToAndroid(0, txt);
    if (!ok) {
      this.showToast('文本提交失败，请确认 Android 输入框已获焦');
    }
  }

  private async handlePasteClipboard(): Promise<void> {
    if (!this.wmAgentReady) {
      this.showToast('等待窗口代理就绪');
      return;
    }
    const ok: boolean = await pasteHostClipboardToAndroid(0);
    if (!ok) {
      this.showToast('鸿蒙剪贴板为空或未读取到文本');
    }
  }

  @Builder
  private topIconBtn(iconRes: Resource, onTap: () => void) {
    Button({ type: ButtonType.Normal }) {
      SymbolGlyph(iconRes)
        .fontSize(IconSize.SM)
        .fontColor([$r('sys.color.icon_primary')])
    }
    .width(ControlSize.BTN_SM)
    .height(ControlSize.BTN_SM)
    .borderRadius(Radius.SM)
    .backgroundColor(Color.Transparent)
    .hoverEffect(HoverEffect.Highlight)
    .clickEffect({ level: ClickEffectLevel.LIGHT, scale: 0.9 })
    .onClick(onTap)
  }

  build() {
    Column({ space: 0 }) {
      // MuMu-Style Ultra-Clean HarmonyOS PC Top Toolbar (40vp, ZERO bottom chrome!)
      Row({ space: Spacing.XS }) {
        Row({ space: Spacing.SM }) {
          Stack({ alignContent: Alignment.Center }) {
            Column()
              .width(24)
              .height(24)
              .borderRadius(Radius.BADGE)
              .backgroundColor($r('sys.color.brand'))
            SymbolGlyph($r('sys.symbol.pad_fill'))
              .fontSize(IconSize.XS)
              .fontColor([$r('sys.color.icon_on_primary')])
          }

          Text(this.emuInstanceName.length > 0 ? this.emuInstanceName : 'Android 14 · 默认设备')
            .fontSize(FontSize.BODY_MD)
            .fontWeight(FontWeight.Medium)
            .fontColor($r('sys.color.font_primary'))
            .maxLines(1)
            .textOverflow({ overflow: TextOverflow.Ellipsis })
            .layoutWeight(1)

          Text(`${this.fps}/${this.refreshRate}Hz`)
            .fontSize(FontSize.BADGE)
            .fontWeight(FontWeight.Medium)
            .fontColor($r('sys.color.font_emphasize'))
            .padding({ left: Spacing.SM, right: Spacing.SM, top: 2, bottom: 2 })
            .borderRadius(Radius.BADGE)
            .backgroundColor($r('sys.color.comp_emphasize_tertiary'))
            .maxLines(1)
        }
        .layoutWeight(1)
        .alignItems(VerticalAlign.Center)

        Row({ space: Spacing.XS }) {
          this.topIconBtn($r('sys.symbol.keyboard'), () => {
            this.showImeBar = !this.showImeBar;
            if (this.showImeBar) {
              syncHostClipboardToAndroid(false);
            }
          })
          this.topIconBtn($r('sys.symbol.chevron_left'), () => { native.sendNavKey('back'); })
          this.topIconBtn($r('sys.symbol.house'), () => { native.sendNavKey('home'); })
          this.topIconBtn($r('sys.symbol.rectangle_on_rectangle'), () => { native.sendNavKey('recents'); })
          this.topIconBtn($r('sys.symbol.arrow_up_right'), () => { this.handlePopOutCurrentApp(); })

          Button({ type: ButtonType.Normal }) {
            SymbolGlyph($r('sys.symbol.dot_grid_1x2'))
              .fontSize(IconSize.SM)
              .fontColor([$r('sys.color.icon_primary')])
          }
          .width(ControlSize.BTN_SM)
          .height(ControlSize.BTN_SM)
          .borderRadius(Radius.SM)
          .backgroundColor(Color.Transparent)
          .hoverEffect(HoverEffect.Highlight)
          .clickEffect({ level: ClickEffectLevel.LIGHT, scale: 0.9 })
          .bindMenu(this.emuMoreMenuBuilder())
        }
        .alignItems(VerticalAlign.Center)
      }
      .width('100%')
      .height(ControlSize.EMU_BAR_H)
      .padding({ left: Spacing.MD, right: Math.max(152, this.decorRightAvoidVp) })
      .backgroundColor($r('sys.color.comp_background_primary'))
      .border({ width: { bottom: 0.6 }, color: $r('sys.color.comp_divider') })
      .alignItems(VerticalAlign.Center)

      // Full-Height Zero-Copy Vulkan Viewport
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
          Column({ space: Spacing.MD }) {
            LoadingProgress()
              .width(ControlSize.BTN_MD)
              .height(ControlSize.BTN_MD)
              .color($r('sys.color.brand'))
            Text(this.statusText)
              .fontSize(FontSize.BODY_MD)
              .fontColor($r('sys.color.font_primary'))
          }
          .padding({ left: Spacing.XXL, right: Spacing.XXL, top: Spacing.XL, bottom: Spacing.XL })
          .backgroundColor($r('sys.color.comp_background_primary'))
          .borderRadius(Radius.CARD)
          .shadow(ShadowStyle.OUTER_DEFAULT_LG)
          .transition(Motion.pillPopTransition())
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

      if (this.showImeBar) {
        Row({ space: Spacing.SM }) {
          TextInput({ placeholder: '鸿蒙中文输入法桥接：输入后按回车直接上屏到 Android', text: this.imeDraft })
            .layoutWeight(1)
            .height(34)
            .fontSize(FontSize.BODY_MD)
            .fontColor($r('sys.color.font_primary'))
            .placeholderColor($r('sys.color.font_tertiary'))
            .backgroundColor($r('sys.color.background_secondary'))
            .borderRadius(Radius.MD)
            .enterKeyType(EnterKeyType.Send)
            .onChange((val: string) => { this.imeDraft = val; })
            .onSubmit(() => { this.handleSubmitImeText(); })

          Button('发送', { type: ButtonType.Capsule })
            .height(32)
            .padding({ left: Spacing.MD, right: Spacing.MD })
            .fontSize(FontSize.CAPTION)
            .fontColor($r('sys.color.font_on_primary'))
            .backgroundColor($r('sys.color.brand'))
            .enabled(this.imeDraft.length > 0 && this.wmAgentReady)
            .onClick(() => { this.handleSubmitImeText(); })

          PasteButton({ text: PasteDescription.PASTE })
            .height(32)
            .fontSize(FontSize.CAPTION)
            .fontColor($r('sys.color.font_primary'))
            .backgroundColor($r('sys.color.comp_background_tertiary'))
            .onClick((_ev: ClickEvent, res: PasteButtonOnClickResult) => {
              if (res === PasteButtonOnClickResult.SUCCESS) {
                this.handlePasteClipboard();
              }
            })
        }
        .width('100%')
        .padding({ left: Spacing.MD, right: Spacing.MD, top: Spacing.XS + 2, bottom: Spacing.XS + 2 })
        .backgroundColor($r('sys.color.comp_background_primary'))
        .border({ width: { top: 0.6 }, color: $r('sys.color.comp_divider') })
        .alignItems(VerticalAlign.Center)
      }
    }
    .width('100%')
    .height('100%')
    .backgroundColor('#0D0F14')
  }
}
