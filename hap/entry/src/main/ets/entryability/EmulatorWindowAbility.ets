import { AbilityConstant, Configuration, ConfigurationConstant, UIAbility, Want } from '@kit.AbilityKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { window } from '@kit.ArkUI';
import native from 'libentry.so';
import { findPresetByKey, InstanceConfig, instanceConfigStore, ResolutionPresetItem } from '../common/AppWindowPrefs';
import { applyImmersiveDecor, registerSlotWindow, unregisterSlotWindow, updateDecorButtonTheme } from '../common/WindowHelper';

const TAG: string = 'SvboxEmuWin';

export default class EmulatorWindowAbility extends UIAbility {
  private localStorage: LocalStorage = new LocalStorage();
  private mainWindow: window.Window | undefined = undefined;

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
      if (native.isWmAgentReady()) {
        native.wmRpcAsync('FOCUS_DISPLAY 0');
      }
    } catch (_e) {
    }
  }

  onForeground(): void {
    try {
      native.setDisplayVisible(0, true);
      if (native.isWmAgentReady()) {
        native.wmRpcAsync('FOCUS_DISPLAY 0');
      }
    } catch (_e) {
    }
  }

  onBackground(): void {
    try {
      native.setDisplayVisible(0, false);
    } catch (_e) {
    }
  }

  onWindowStageCreate(windowStage: window.WindowStage): void {
    try {
      const win: window.Window = windowStage.getMainWindowSync();
      this.mainWindow = win;
      registerSlotWindow(0, win);

      const cfg: InstanceConfig = instanceConfigStore.get(this.context.filesDir);
      const reqKey: string = this.localStorage.get<string>('emuResKey') ?? cfg.resolutionKey;
      const preset: ResolutionPresetItem = findPresetByKey(reqKey);
      const isLand: boolean = preset.w > preset.h;
      const winW: number = isLand ? 1560 : 820;
      const winH: number = isLand ? 960 : 1460;
      try {
        win.resize(winW, winH);
      } catch (_e) {
      }
      this.localStorage.setOrCreate<number>('decorRightAvoidVp', 136);
    } catch (err) {
      hilog.error(0x0000, TAG, 'windowStage setup error: %{public}s', String(err));
    }

    windowStage.loadContent('pages/EmulatorWindowPage', this.localStorage, (err) => {
      if (err.code) {
        hilog.error(0x0000, TAG, 'Failed to load EmulatorWindowPage: %{public}s', JSON.stringify(err));
        return;
      }
      if (!this.mainWindow) return;
      const isDark: boolean = this.context.config.colorMode === ConfigurationConstant.ColorMode.COLOR_MODE_DARK;
      const rightAvoid: number = applyImmersiveDecor(this.mainWindow, 40, isDark);
      this.localStorage.setOrCreate<number>('decorRightAvoidVp', rightAvoid);
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
    unregisterSlotWindow(0);
    try {
      native.setDisplayVisible(0, false);
    } catch (_e) {
    }
  }
}
