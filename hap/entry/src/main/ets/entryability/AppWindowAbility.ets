import { AbilityConstant, UIAbility, Want } from '@kit.AbilityKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { window } from '@kit.ArkUI';
import native from 'libentry.so';
import { AndroidAppItem, AppWindowPref, appWindowPrefs, loadCachedAppList } from '../common/AppWindowPrefs';
import { syncHostClipboardToAndroid } from '../common/InputBridgeHelper';
import { DeviceInstance, instanceManager } from '../model/InstanceModel';
import {
  applyImmersiveDecor,
  applyMissionIconAndQuickBar,
  cleanupQuickBarGroup,
  registerSlotWindow,
  unregisterSlotWindow
} from '../common/WindowHelper';

const TAG: string = 'SvboxAppWin';

export default class AppWindowAbility extends UIAbility {
  private localStorage: LocalStorage = new LocalStorage();
  private displaySlot: number = -1;
  private androidPkg: string = '';
  private mainWindow: window.Window | undefined = undefined;
  private quickBarGroupKey: string = '';

  private parseWant(want: Want, isRelaunch: boolean): void {
    const params = want.parameters;
    const pkg: string = params && typeof params['androidPkg'] === 'string'
      ? (params['androidPkg'] as string) : '';
    const comp: string = params && typeof params['androidComp'] === 'string'
      ? (params['androidComp'] as string) : '';
    const label: string = params && typeof params['androidLabel'] === 'string'
      ? (params['androidLabel'] as string) : pkg;
    const moveTaskRaw: string = params && typeof params['moveTaskId'] === 'string'
      ? (params['moveTaskId'] as string) : '';
    const moveTaskId: number = moveTaskRaw.length > 0 ? parseInt(moveTaskRaw, 10) : -1;

    if (pkg.length > 0) {
      this.androidPkg = pkg;
    }
    if (this.displaySlot < 1 && this.androidPkg.length > 0) {
      this.displaySlot = native.allocDisplaySlot(this.androidPkg);
    }

    this.localStorage.setOrCreate<string>('androidPkg', this.androidPkg);
    this.localStorage.setOrCreate<string>('androidComp', comp);
    this.localStorage.setOrCreate<string>('androidLabel', label.length > 0 ? label : this.androidPkg);
    this.localStorage.setOrCreate<number>('displaySlot', this.displaySlot);
    this.localStorage.setOrCreate<number>('moveTaskId', isNaN(moveTaskId) ? -1 : moveTaskId);
    this.localStorage.setOrCreate<string>('sandboxTempDir', this.context.tempDir);
    this.localStorage.setOrCreate<string>('sandboxFilesDir', this.context.filesDir);

    if (isRelaunch) {
      const seq: number = (this.localStorage.get<number>('relaunchSeq') ?? 0) + 1;
      this.localStorage.setOrCreate<number>('relaunchSeq', seq);
    }
  }

  onCreate(want: Want, _launchParam: AbilityConstant.LaunchParam): void {
    hilog.info(0x0000, TAG, 'AppWindowAbility onCreate');
    this.parseWant(want, false);
  }

  onNewWant(want: Want, _launchParam: AbilityConstant.LaunchParam): void {
    hilog.info(0x0000, TAG, 'AppWindowAbility onNewWant pkg=%{public}s', this.androidPkg);
    this.parseWant(want, true);
  }

  onForeground(): void {
    if (this.displaySlot >= 1) {
      try {
        native.setDisplayVisible(this.displaySlot, true);
        if (native.isWmAgentReady()) {
          native.wmRpcAsync(`FOCUS_DISPLAY ${this.displaySlot}`);
          syncHostClipboardToAndroid(false);
        }
      } catch (_e) {
      }
    }
  }

  onBackground(): void {
    if (this.displaySlot >= 1) {
      try {
        native.setDisplayVisible(this.displaySlot, false);
      } catch (_e) {
      }
    }
  }

  onWindowStageCreate(windowStage: window.WindowStage): void {
    try {
      const win: window.Window = windowStage.getMainWindowSync();
      this.mainWindow = win;
      registerSlotWindow(this.displaySlot, win);

      const globalHz: number = AppStorage.get<number>('refreshRate') ?? 60;
      let appMeta: AndroidAppItem | undefined = undefined;
      let cachedApps: AndroidAppItem[] = AppStorage.get<AndroidAppItem[]>('androidApps') ?? [];
      if (cachedApps.length === 0) {
        const primary: DeviceInstance | undefined =
          instanceManager.getById(this.context.filesDir, 'inst_0') ?? instanceManager.getAll(this.context.filesDir)[0];
        cachedApps = loadCachedAppList(this.context.filesDir, primary ? primary.dataSubdir : 'userdata14');
      }
      for (let i: number = 0; i < cachedApps.length; i++) {
        if (cachedApps[i].packageName === this.androidPkg) {
          appMeta = cachedApps[i];
          break;
        }
      }
      const pref: AppWindowPref = appWindowPrefs.getPref(this.context.filesDir, this.androidPkg, appMeta, globalHz);

      try {
        win.resize(pref.winW, pref.winH);
      } catch (_e) {
      }
      if (pref.mode === 'fixed_portrait' || pref.mode === 'fixed_landscape') {
        const ratio: number = pref.mode === 'fixed_landscape' ? (1280.0 / 784.0) : (720.0 / 1344.0);
        try {
          win.setContentAspectRatio(ratio, false, false).catch(() => {});
        } catch (_e) {
        }
      } else {
        try {
          win.resetAspectRatio().catch(() => {});
        } catch (_e) {
        }
      }

      const props: window.WindowProperties = win.getWindowProperties();
      this.localStorage.setOrCreate<number>('winPxW', props.windowRect.width);
      this.localStorage.setOrCreate<number>('winPxH', props.windowRect.height);
      this.localStorage.setOrCreate<number>('decorRightAvoidVp', 136);

      win.on('windowSizeChange', (sz: window.Size) => {
        this.localStorage.setOrCreate<number>('winPxW', sz.width);
        this.localStorage.setOrCreate<number>('winPxH', sz.height);
      });

      win.on('windowEvent', (ev: window.WindowEventType) => {
        if (ev === window.WindowEventType.WINDOW_ACTIVE && this.displaySlot >= 1) {
          try {
            native.setDisplayVisible(this.displaySlot, true);
            if (native.isWmAgentReady()) {
              native.wmRpcAsync(`FOCUS_DISPLAY ${this.displaySlot}`);
              syncHostClipboardToAndroid(false);
            }
          } catch (_e) {
          }
        }
      });
    } catch (err) {
      hilog.error(0x0000, TAG, 'windowStage setup error: %{public}s', String(err));
    }

    windowStage.loadContent('pages/AppWindowPage', this.localStorage, (err) => {
      if (err.code) {
        hilog.error(0x0000, TAG, 'Failed to load AppWindowPage: %{public}s', JSON.stringify(err));
        return;
      }
      if (!this.mainWindow) return;
      const rightAvoid: number = applyImmersiveDecor(this.mainWindow, 38, true);
      this.localStorage.setOrCreate<number>('decorRightAvoidVp', rightAvoid);

      const title: string = this.localStorage.get<string>('androidLabel') ?? this.androidPkg;
      const iconsDir: string = native.getIconsDir();
      const iconPath: string = iconsDir.length > 0 ? `${iconsDir}/${this.androidPkg}.png` : '';
      this.localStorage.setOrCreate<boolean>('iconSetupStarted', iconPath.length > 0);
      applyMissionIconAndQuickBar(this.context, this.mainWindow, this.androidPkg, title, iconPath)
        .then((grp: string) => {
          if (grp.length > 0) {
            this.quickBarGroupKey = grp;
            this.localStorage.setOrCreate<string>('quickBarGroupKey', grp);
          }
        });
    });
  }

  onDestroy(): void {
    hilog.info(0x0000, TAG, 'AppWindowAbility onDestroy slot=%{public}d pkg=%{public}s',
      this.displaySlot, this.androidPkg);
    const grp: string = this.quickBarGroupKey.length > 0
      ? this.quickBarGroupKey
      : (this.localStorage.get<string>('quickBarGroupKey') ?? '');
    if (grp.length > 0) {
      cleanupQuickBarGroup(this.context, grp);
    }
    const pulledBack: boolean = this.localStorage.get<boolean>('pulledBack') ?? false;
    if (this.displaySlot >= 1) {
      const slot: number = this.displaySlot;
      const pkg: string = this.androidPkg;
      unregisterSlotWindow(slot);
      try {
        if (native.isWmAgentReady()) {
          if (!pulledBack) {
            native.wmRpc(`CLOSE_APP ${slot} ${pkg}`);
          }
          native.wmRpc(`DESTROY_DISPLAY ${slot}`);
        }
        native.releaseDisplaySlot(slot);
      } catch (_e) {
      }
      this.displaySlot = -1;
    }
  }
}
