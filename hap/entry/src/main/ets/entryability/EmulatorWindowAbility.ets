import { AbilityConstant, Configuration, ConfigurationConstant, UIAbility, Want } from '@kit.AbilityKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { KeyboardAvoidMode, window } from '@kit.ArkUI';
import native from 'libentry.so';
import { findPresetByKey, InstanceConfig, instanceConfigStore, ResolutionPresetItem } from '../common/AppWindowPrefs';
import { getDeviceFormFactor } from '../common/DeviceProfile';
import { applyImmersiveDecor, registerSlotWindow, unregisterSlotWindow, updateDecorButtonTheme } from '../common/WindowHelper';
import { syncHostClipboardToAndroid } from '../common/InputBridgeHelper';
import { hostImeManager } from '../common/HostImeController';

const TAG: string = 'SvboxEmuWin';
const EMU_TOP_BAR_VP: number = 40;

export default class EmulatorWindowAbility extends UIAbility {
  private localStorage: LocalStorage = new LocalStorage();
  private mainWindow: window.Window | undefined = undefined;
  private initialPresetW: number = 720;
  private initialPresetH: number = 1280;

  private applyWant(want: Want): void {
    const params = want.parameters;
    const resKey: string = params && typeof params['resolutionKey'] === 'string'
      ? (params['resolutionKey'] as string) : '';
    const instName: string = params && typeof params['instanceName'] === 'string'
      ? (params['instanceName'] as string) : '';
    const instId: string = params && typeof params['instanceId'] === 'string'
      ? (params['instanceId'] as string) : '';
    if (resKey.length > 0) {
      this.localStorage.setOrCreate<string>('emuResKey', resKey);
    }
    if (instName.length > 0) {
      this.localStorage.setOrCreate<string>('emuInstanceName', instName);
    }
    if (instId.length > 0) {
      this.localStorage.setOrCreate<string>('emuInstanceId', instId);
    }
    this.localStorage.setOrCreate<string>('sandboxTempDir', this.context.tempDir);
    this.localStorage.setOrCreate<string>('sandboxFilesDir', this.context.filesDir);
  }

  private async lockEmulatorAspect(win: window.Window, dispW: number, dispH: number, resizeNow: boolean): Promise<void> {
    if (getDeviceFormFactor() === 'phone') return;
    let barPx: number = 80;
    try {
      barPx = Math.round(win.getUIContext().vp2px(EMU_TOP_BAR_VP));
    } catch (_e) {
    }
    const isLand: boolean = dispW > dispH;
    const targetW: number = isLand ? 1480 : 760;
    const stageH: number = Math.round(targetW * (dispH / Math.max(1, dispW)));
    const targetH: number = stageH + barPx;
    const ratio: number = targetW / Math.max(1, targetH);
    try {
      // Lock traditional emulator window strictly to proportional scaling (needUpdateRect=false so resize sets exact size)
      await win.setContentAspectRatio(ratio, false, false);
    } catch (_e) {
    }
    if (resizeNow) {
      try {
        await win.resize(targetW, targetH);
      } catch (_e) {
      }
    }
  }

  onConfigurationUpdate(newConfig: Configuration): void {
    const isDark: boolean = newConfig.colorMode === ConfigurationConstant.ColorMode.COLOR_MODE_DARK;
    updateDecorButtonTheme(this.mainWindow, isDark);
  }

  onCreate(want: Want, _launchParam: AbilityConstant.LaunchParam): void {
    hilog.info(0x0000, TAG, 'EmulatorWindowAbility onCreate');
    this.applyWant(want);
  }

  onNewWant(want: Want, _launchParam: AbilityConstant.LaunchParam): void {
    hilog.info(0x0000, TAG, 'EmulatorWindowAbility onNewWant');
    this.applyWant(want);
    try {
      native.setDisplayVisible(0, true);
      hostImeManager.setWindowActive(0, true);
      if (native.isWmAgentReady()) {
        native.wmRpcAsync('FOCUS_DISPLAY 0');
      }
    } catch (_e) {
    }
  }

  onForeground(): void {
    try {
      native.setDisplayVisible(0, true);
      hostImeManager.setWindowActive(0, true);
      if (native.isWmAgentReady()) {
        native.wmRpcAsync('FOCUS_DISPLAY 0');
        syncHostClipboardToAndroid(false);
      }
    } catch (_e) {
    }
  }

  onBackground(): void {
    try {
      hostImeManager.setWindowActive(0, false);
      native.setDisplayVisible(0, false);
    } catch (_e) {
    }
  }

  onWindowStageCreate(windowStage: window.WindowStage): void {
    try {
      const win: window.Window = windowStage.getMainWindowSync();
      this.mainWindow = win;
      registerSlotWindow(0, win);
      hostImeManager.registerWindow(0, win, EMU_TOP_BAR_VP);

      const cfg: InstanceConfig = instanceConfigStore.get(this.context.filesDir);
      const reqKey: string = this.localStorage.get<string>('emuResKey') ?? cfg.resolutionKey;
      const preset: ResolutionPresetItem = findPresetByKey(reqKey);
      this.initialPresetW = preset.w;
      this.initialPresetH = preset.h;
      const isDarkInit: boolean = this.context.config.colorMode === ConfigurationConstant.ColorMode.COLOR_MODE_DARK;
      applyImmersiveDecor(win, EMU_TOP_BAR_VP, isDarkInit);

      this.localStorage.setOrCreate<number>('decorRightAvoidVp', 136);
      win.on('windowEvent', (ev: window.WindowEventType) => {
        if (ev === window.WindowEventType.WINDOW_ACTIVE) {
          hostImeManager.setWindowActive(0, true);
          syncHostClipboardToAndroid(false);
        } else if (ev === window.WindowEventType.WINDOW_INACTIVE) {
          hostImeManager.setWindowActive(0, false);
        }
      });
    } catch (err) {
      hilog.error(0x0000, TAG, 'windowStage setup error: %{public}s', String(err));
    }

    windowStage.loadContent('pages/EmulatorWindowPage', this.localStorage, (err) => {
      if (err.code) {
        hilog.error(0x0000, TAG, 'Failed to load EmulatorWindowPage: %{public}s', JSON.stringify(err));
        return;
      }
      if (!this.mainWindow) return;
      try {
        this.mainWindow.getUIContext().setKeyboardAvoidMode(KeyboardAvoidMode.RESIZE);
      } catch (_e) {
      }
      const isDark: boolean = this.context.config.colorMode === ConfigurationConstant.ColorMode.COLOR_MODE_DARK;
      const rightAvoid: number = applyImmersiveDecor(this.mainWindow, EMU_TOP_BAR_VP, isDark);
      this.localStorage.setOrCreate<number>('decorRightAvoidVp', rightAvoid);
      // Re-apply aspect lock with exact UIContext vp2px(40) once content is loaded
      this.lockEmulatorAspect(this.mainWindow, this.initialPresetW, this.initialPresetH, true);
      const title: string = this.localStorage.get<string>('emuInstanceName') ?? 'Android 14 · 默认设备';
      try {
        this.mainWindow.setWindowTitle(title);
        this.context.setMissionLabel(title);
      } catch (_e) {
      }
    });
  }

  onDestroy(): void {
    hilog.info(0x0000, TAG, 'EmulatorWindowAbility onDestroy');
    hostImeManager.unregisterWindow(0);
    unregisterSlotWindow(0);
    try {
      native.setDisplayVisible(0, false);
    } catch (_e) {
    }
  }
}
