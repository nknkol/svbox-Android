import { common, Want } from '@kit.AbilityKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { window } from '@kit.ArkUI';
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
  syncHostClipboardToAndroid
} from '../common/InputBridgeHelper';
import { hostImeManager } from '../common/HostImeController';
import { getDeviceFormFactor, supportsPcKeymap } from '../common/DeviceProfile';
import {
  applyKeymapToNative,
  KeymapProfile,
  keymapStore
} from '../common/KeymapStore';
import {
  KeymapEditorOverlay,
  KeymapHintLayer,
  MacroAutomationOverlay
} from '../components/KeymapOverlay';
import { getSlotWindow } from '../common/WindowHelper';
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

interface RotationReply {
  ok?: boolean;
  rotation?: number;
}

@Entry({ useSharedStorage: true })
@Component
struct EmulatorWindowPage {
  @LocalStorageProp('sandboxTempDir') sandboxTempDir: string = '';
  @LocalStorageProp('sandboxFilesDir') sandboxFilesDir: string = '';
  @LocalStorageProp('decorRightAvoidVp') decorRightAvoidVp: number = 152;
  @LocalStorageProp('emuInstanceName') emuInstanceName: string = '';
  @LocalStorageProp('emuInstanceId') emuInstanceId: string = '';
  @StorageProp('hostImeAttachedSlot') hostImeAttachedSlot: number = -1;

  @State private running: boolean = false;
  @State private wmAgentReady: boolean = false;
  @State private dispW: number = 720;
  @State private dispH: number = 1280;
  @State private displayRotation: number = 0; // 0 = natural, 1 = rotated 90 deg CCW
  @State private fps: number = 0;
  @State private refreshRate: number = 120;
  @State private statusText: string = '正在连接 Android 14 模拟器屏幕...';
  @State private stageW: number = 380;
  @State private stageH: number = 675;
  @State private barVpW: number = 380;
  @State private keymapProfile: KeymapProfile = { pkg: '__emulator__', enabled: false, showHints: true, items: [] };
  @State private showKeymapEditor: boolean = false;
  @State private showMacroPanel: boolean = false;

  private pollTimer: number = -1;
  private rotSynced: boolean = false;
  private manualRotating: boolean = false;
  private svboxPath: string = '/storage/Users/currentUser/Workspace/svbox/build/svbox';
  private androidDir: string = '/storage/Users/currentUser/Workspace/svbox-Android';

  private showToast(msg: string): void {
    try {
      this.getUIContext().getPromptAction().showToast({ message: msg, duration: 2000 });
    } catch (_e) {
    }
  }

  private isRotated90(): boolean {
    return (this.displayRotation & 1) === 1;
  }

  private surfaceRotateAngle(): number {
    const r: number = this.displayRotation & 3;
    if (r === 1) return -90;
    if (r === 2) return 180;
    if (r === 3) return 90;
    return 0;
  }

  private visualW(): number {
    return this.isRotated90() ? this.dispH : this.dispW;
  }

  private visualH(): number {
    return this.isRotated90() ? this.dispW : this.dispH;
  }

  private computeFitWidth(): number {
    const padW: number = Math.max(120, this.stageW);
    const padH: number = Math.max(120, this.stageH);
    const aspect: number = this.visualW() / Math.max(1, this.visualH());
    if (padW / padH > aspect) {
      return Math.floor(padH * aspect);
    }
    return Math.floor(padW);
  }

  private computeFitHeight(): number {
    const padW: number = Math.max(120, this.stageW);
    const padH: number = Math.max(120, this.stageH);
    const aspect: number = this.visualW() / Math.max(1, this.visualH());
    if (padW / padH > aspect) {
      return Math.floor(padH);
    }
    return Math.floor(padW / aspect);
  }

  // Unrotated XComponent dimensions (when rotated -90 deg, width/height swap visually to fitW x fitH)
  private surfaceBoxW(): number {
    return this.isRotated90() ? this.computeFitHeight() : this.computeFitWidth();
  }

  private surfaceBoxH(): number {
    return this.isRotated90() ? this.computeFitWidth() : this.computeFitHeight();
  }

  private availBarVp(): number {
    return Math.max(0, this.barVpW - Math.max(152, this.decorRightAvoidVp) - 24);
  }

  private showRecentsInBar(): boolean {
    return this.availBarVp() >= 256;
  }

  private showImeInBar(): boolean {
    return this.availBarVp() >= 292;
  }

  private showPopOutInBar(): boolean {
    return this.availBarVp() >= 328;
  }

  private showKeymapInBar(): boolean {
    return supportsPcKeymap() && this.availBarVp() >= 364;
  }

  private showMacroInBar(): boolean {
    return supportsPcKeymap() && this.availBarVp() >= 400;
  }

  private showFpsInBar(): boolean {
    return this.availBarVp() >= 468;
  }

  private async applyEmulatorAspectLock(resizeWindow: boolean): Promise<void> {
    if (getDeviceFormFactor() === 'phone') return;
    const win: window.Window | undefined = getSlotWindow(0);
    if (!win) return;
    let barPx: number = 80;
    try {
      barPx = Math.round(this.getUIContext().vp2px(ControlSize.EMU_BAR_H));
    } catch (_e) {
    }
    const vw: number = this.visualW();
    const vh: number = this.visualH();
    const isLand: boolean = vw > vh;
    const targetW: number = isLand ? 1480 : 760;
    const stageH: number = Math.round(targetW * (vh / Math.max(1, vw)));
    const targetH: number = stageH + barPx;
    const ratio: number = targetW / Math.max(1, targetH);
    try {
      await win.setContentAspectRatio(ratio, false, false);
      if (resizeWindow) {
        await win.resize(targetW, targetH);
      }
    } catch (err) {
      hilog.warn(0x0000, TAG, 'applyEmulatorAspectLock warn: %{public}s', String(err));
    }
  }

  private applyGuestRotation(rot: number, resizeWindow: boolean): void {
    const r: number = rot & 3;
    const prevLandscape: boolean = this.isRotated90();
    const nextLandscape: boolean = (r & 1) === 1;
    this.displayRotation = r;
    native.setDisplay0Rotation(r);
    if (resizeWindow || prevLandscape !== nextLandscape) {
      this.applyEmulatorAspectLock(true);
    }
  }

  private async syncDisplayRotationFromGuest(): Promise<void> {
    if (this.rotSynced || !native.isWmAgentReady()) return;
    this.rotSynced = true;
    try {
      const raw: string = await native.wmRpcAsync('GET_DISPLAY_ROTATION');
      const rep: RotationReply = JSON.parse(raw) as RotationReply;
      if (rep.ok && rep.rotation !== undefined) {
        const rot: number = rep.rotation & 3;
        if (rot !== this.displayRotation) {
          this.applyGuestRotation(rot, true);
        }
      }
    } catch (_e) {
    }
  }

  private async handleToggleRotation(): Promise<void> {
    if (!this.wmAgentReady) {
      this.showToast('等待 Android 14 窗口服务就绪后再旋转');
      return;
    }
    const nextRot: number = this.isRotated90() ? 0 : 1;
    this.manualRotating = true;
    try {
      const raw: string = await native.wmRpcAsync(`SET_DISPLAY_ROTATION ${nextRot}`);
      const rep: RotationReply = JSON.parse(raw) as RotationReply;
      if (!rep.ok) {
        this.showToast('屏幕旋转失败');
        return;
      }
      const applied: number = rep.rotation !== undefined ? (rep.rotation & 3) : nextRot;
      this.applyGuestRotation(applied, true);
    } catch (err) {
      hilog.warn(0x0000, TAG, 'handleToggleRotation failed: %{public}s', String(err));
      this.showToast(`旋转异常：${String(err)}`);
    } finally {
      setTimeout(() => {
        this.manualRotating = false;
      }, 250);
    }
  }

  private async handleToggleHostIme(): Promise<void> {
    if (!this.wmAgentReady) {
      this.showToast('等待 Android 14 窗口服务就绪');
      return;
    }
    const attachedNow: boolean = await hostImeManager.toggleManualAttach(0);
    if (attachedNow) {
      syncHostClipboardToAndroid(false);
      this.showToast('已直接接管鸿蒙系统输入法，可直接打字');
    } else {
      this.showToast('已释放鸿蒙输入法，恢复键鼠直通');
    }
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

    const st: RuntimeStatus = native.getStatus(false);
    if (!st.running && inst) {
      const tempDir: string = this.sandboxTempDir.length > 0
        ? this.sandboxTempDir : '/data/storage/el2/base/haps/entry/temp';
      try {
        native.setInstanceConfig(
          inst.id, inst.dataSubdir, inst.dpi, inst.enableAudio,
          inst.enableRoot, inst.deviceBrand, inst.deviceModel, inst.adbPort, inst.enableGms
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
        this.syncDisplayRotationFromGuest();
      }
      if (supportsPcKeymap()) {
        this.keymapProfile = keymapStore.getProfile(filesDir, '__emulator__');
        applyKeymapToNative(0, this.keymapProfile);
      }
    } catch (_e) {
    }

    this.pollTimer = setInterval(() => {
      try {
        const cur: RuntimeStatus = native.getStatus(false);
        if (this.running !== cur.running) this.running = cur.running;
        if (this.wmAgentReady !== cur.wmAgentReady) {
          this.wmAgentReady = cur.wmAgentReady;
          if (cur.wmAgentReady) {
            this.syncDisplayRotationFromGuest();
          }
        }
        if (this.fps !== cur.fps) this.fps = cur.fps;
        if (cur.width > 0 && cur.height > 0 && (this.dispW !== cur.width || this.dispH !== cur.height)) {
          this.dispW = cur.width;
          this.dispH = cur.height;
          this.applyEmulatorAspectLock(false);
        }
        if (!this.manualRotating && cur.displayRotation !== undefined) {
          const guestRot: number = cur.displayRotation & 3;
          if (guestRot !== this.displayRotation) {
            hilog.info(0x0000, TAG, 'Auto-following Android Display 0 rotation: %{public}d -> %{public}d',
              this.displayRotation, guestRot);
            this.applyGuestRotation(guestRot, true);
          }
        }
        if ((cur.refreshRate === 60 || cur.refreshRate === 90 || cur.refreshRate === 120) &&
            this.refreshRate !== cur.refreshRate) {
          this.refreshRate = cur.refreshRate;
        }
        if (this.statusText !== cur.statusText) this.statusText = cur.statusText;
      } catch (_e) {
      }
    }, 250);
  }

  aboutToDisappear(): void {
    if (this.pollTimer !== -1) {
      clearInterval(this.pollTimer);
      this.pollTimer = -1;
    }
    try {
      hostImeManager.setWindowActive(0, false);
      native.setKeymapConfig(0, false, '');
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
      if (!this.showFpsInBar()) {
        MenuItem({ content: `实时帧率：${this.fps}/${this.refreshRate}Hz` })
          .enabled(false)
      }
      if (!this.showRecentsInBar()) {
        MenuItem({ content: '多任务视图' })
          .onClick(() => { native.sendNavKey('recents'); })
      }
      if (!this.showImeInBar()) {
        MenuItem({
          content: this.hostImeAttachedSlot === 0
            ? '关闭鸿蒙共享输入法'
            : '手动唤起鸿蒙共享输入法'
        })
          .enabled(this.wmAgentReady)
          .onClick(() => { this.handleToggleHostIme(); })
      }
      MenuItem({ content: this.isRotated90() ? '旋转回竖屏' : '旋转为横屏' })
        .enabled(this.wmAgentReady)
        .onClick(() => { this.handleToggleRotation(); })
      if (!this.showPopOutInBar()) {
        MenuItem({ content: '拆出前台应用到独立窗口' })
          .enabled(this.wmAgentReady)
          .onClick(() => { this.handlePopOutCurrentApp(); })
      }
      MenuItem({ content: '粘贴鸿蒙剪贴板到 Android' })
        .enabled(this.wmAgentReady)
        .onClick(() => { this.handlePasteClipboard(); })
      if (supportsPcKeymap()) {
        MenuItem({ content: this.keymapProfile.enabled ? '关闭键鼠映射' : '开启键鼠映射' })
          .onClick(() => { this.toggleKeymapEnabled(); })
        MenuItem({ content: '编辑键鼠映射布局...' })
          .onClick(() => {
            this.showMacroPanel = false;
            this.showKeymapEditor = true;
          })
        MenuItem({ content: '自动化脚本 / 宏录制...' })
          .onClick(() => {
            this.showKeymapEditor = false;
            this.showMacroPanel = !this.showMacroPanel;
          })
      }
      MenuItem({ content: '音量加' })
        .onClick(() => { native.sendNavKey('vol_up'); })
      MenuItem({ content: '音量减' })
        .onClick(() => { native.sendNavKey('vol_down'); })
      MenuItem({ content: '电源键 / 锁屏唤醒' })
        .onClick(() => { native.sendNavKey('power'); })
    }
  }

  private getFilesDir(): string {
    return this.sandboxFilesDir.length > 0
      ? this.sandboxFilesDir : '/data/storage/el2/base/haps/entry/files';
  }

  private toggleKeymapEnabled(): void {
    if (this.keymapProfile.items.length === 0) {
      this.showKeymapEditor = true;
      this.showToast('尚未配置键位，请选择模板或添加键位');
      return;
    }
    const next: KeymapProfile = {
      pkg: this.keymapProfile.pkg,
      enabled: !this.keymapProfile.enabled,
      showHints: this.keymapProfile.showHints,
      items: this.keymapProfile.items,
    };
    this.keymapProfile = next;
    keymapStore.saveProfile(this.getFilesDir(), next);
    applyKeymapToNative(0, next);
    this.showToast(next.enabled ? '已开启键鼠映射' : '已关闭键鼠映射');
  }

  private handleSaveKeymap(profile: KeymapProfile): void {
    this.keymapProfile = profile;
    this.showKeymapEditor = false;
    keymapStore.saveProfile(this.getFilesDir(), profile);
    applyKeymapToNative(0, profile);
    this.showToast(profile.enabled ? `键鼠映射已生效 (${profile.items.length} 项)` : '键鼠映射已清空');
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
  private topIconBtn(iconRes: Resource, active: boolean, onTap: () => void) {
    Button({ type: ButtonType.Normal }) {
      SymbolGlyph(iconRes)
        .fontSize(IconSize.SM)
        .fontColor([active ? $r('sys.color.brand') : $r('sys.color.icon_primary')])
    }
    .width(ControlSize.BTN_SM)
    .height(ControlSize.BTN_SM)
    .flexShrink(0)
    .borderRadius(Radius.SM)
    .backgroundColor(active ? $r('sys.color.comp_emphasize_tertiary') : Color.Transparent)
    .hoverEffect(HoverEffect.Highlight)
    .clickEffect({ level: ClickEffectLevel.LIGHT, scale: 0.9 })
    .onClick(onTap)
  }

  build() {
    Column({ space: 0 }) {
      // Responsive HarmonyOS PC Top Toolbar (40vp): collapses secondary items into More (⋮) to prevent clipping
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
          .width(24)
          .height(24)
          .flexShrink(0)

          Text(this.emuInstanceName.length > 0 ? this.emuInstanceName : 'Android 14 · 默认设备')
            .fontSize(FontSize.BODY_MD)
            .fontWeight(FontWeight.Medium)
            .fontColor($r('sys.color.font_primary'))
            .maxLines(1)
            .textOverflow({ overflow: TextOverflow.Ellipsis })
            .layoutWeight(1)

          if (this.showFpsInBar()) {
            Text(`${this.fps}/${this.refreshRate}Hz`)
              .fontSize(FontSize.BADGE)
              .fontWeight(FontWeight.Medium)
              .fontColor($r('sys.color.font_emphasize'))
              .padding({ left: Spacing.SM, right: Spacing.SM, top: 2, bottom: 2 })
              .borderRadius(Radius.BADGE)
              .backgroundColor($r('sys.color.comp_emphasize_tertiary'))
              .maxLines(1)
              .flexShrink(0)
          }
        }
        .layoutWeight(1)
        .clip(true)
        .alignItems(VerticalAlign.Center)

        Row({ space: Spacing.XS }) {
          if (this.showKeymapInBar()) {
            this.topIconBtn($r('sys.symbol.gamecontroller'), this.keymapProfile.enabled || this.showKeymapEditor, () => {
              this.showMacroPanel = false;
              this.showKeymapEditor = !this.showKeymapEditor;
            })
          }
          if (this.showMacroInBar()) {
            this.topIconBtn($r('sys.symbol.bolt_fill'), this.showMacroPanel, () => {
              this.showKeymapEditor = false;
              this.showMacroPanel = !this.showMacroPanel;
            })
          }
          if (this.showImeInBar()) {
            this.topIconBtn($r('sys.symbol.keyboard'), this.hostImeAttachedSlot === 0, () => {
              this.handleToggleHostIme();
            })
          }
          this.topIconBtn($r('sys.symbol.chevron_left'), false, () => { native.sendNavKey('back'); })
          this.topIconBtn($r('sys.symbol.house'), false, () => { native.sendNavKey('home'); })
          if (this.showRecentsInBar()) {
            this.topIconBtn($r('sys.symbol.rectangle_on_rectangle'), false, () => { native.sendNavKey('recents'); })
          }
          this.topIconBtn($r('sys.symbol.screen_rotation'), this.isRotated90(), () => {
            this.handleToggleRotation();
          })
          if (this.showPopOutInBar()) {
            this.topIconBtn($r('sys.symbol.arrow_up_right'), false, () => { this.handlePopOutCurrentApp(); })
          }

          Button({ type: ButtonType.Normal }) {
            SymbolGlyph($r('sys.symbol.dot_grid_1x2'))
              .fontSize(IconSize.SM)
              .fontColor([$r('sys.color.icon_primary')])
          }
          .width(ControlSize.BTN_SM)
          .height(ControlSize.BTN_SM)
          .flexShrink(0)
          .borderRadius(Radius.SM)
          .backgroundColor(Color.Transparent)
          .hoverEffect(HoverEffect.Highlight)
          .clickEffect({ level: ClickEffectLevel.LIGHT, scale: 0.9 })
          .bindMenu(this.emuMoreMenuBuilder())
        }
        .flexShrink(0)
        .alignItems(VerticalAlign.Center)
      }
      .width('100%')
      .height(ControlSize.EMU_BAR_H)
      .padding({ left: Spacing.MD, right: Math.max(152, this.decorRightAvoidVp) })
      .backgroundColor($r('sys.color.comp_background_primary'))
      .border({ width: { bottom: 0.6 }, color: $r('sys.color.comp_divider') })
      .alignItems(VerticalAlign.Center)
      .onAreaChange((_oldVal: Area, newVal: Area) => {
        const w: number = Number(newVal.width);
        if (w > 100 && Math.abs(w - this.barVpW) >= 1) {
          this.barVpW = w;
        }
      })

      // Full-Height Zero-Copy Vulkan Viewport (with native RS hardware rotation support)
      Stack({ alignContent: Alignment.Center }) {
        XComponent({
          id: 'svbox_display_0',
          type: XComponentType.SURFACE,
          libraryname: 'entry'
        })
          .focusable(true)
          .defaultFocus(true)
          .width(this.surfaceBoxW())
          .height(this.surfaceBoxH())
          .rotate({ angle: this.surfaceRotateAngle() })
          .backgroundColor('#000000')

        if (this.keymapProfile.enabled && this.keymapProfile.showHints &&
            !this.showKeymapEditor && this.keymapProfile.items.length > 0) {
          KeymapHintLayer({
            items: this.keymapProfile.items,
            viewW: this.computeFitWidth(),
            viewH: this.computeFitHeight(),
          })
        }

        if (this.showKeymapEditor) {
          KeymapEditorOverlay({
            pkg: '__emulator__',
            viewW: this.computeFitWidth(),
            viewH: this.computeFitHeight(),
            initialProfile: this.keymapProfile,
            onSave: (p: KeymapProfile) => { this.handleSaveKeymap(p); },
            onClose: () => { this.showKeymapEditor = false; },
          })
        }

        if (this.showMacroPanel) {
          MacroAutomationOverlay({
            filesDir: this.getFilesDir(),
            pkg: '__emulator__',
            displaySlot: 0,
            viewW: this.computeFitWidth(),
            viewH: this.computeFitHeight(),
            onClose: () => { this.showMacroPanel = false; },
          })
        }

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
      .clip(true)
      .backgroundColor('#0D0F14')
      .onAreaChange((_oldVal: Area, newVal: Area) => {
        const w: number = Number(newVal.width);
        const h: number = Number(newVal.height);
        if (w > 100 && h > 100) {
          this.stageW = w;
          this.stageH = h;
        }
      })
    }
    .width('100%')
    .height('100%')
    .backgroundColor('#0D0F14')
  }
}
