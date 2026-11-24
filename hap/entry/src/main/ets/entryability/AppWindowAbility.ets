import { AbilityConstant, UIAbility, Want } from '@kit.AbilityKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { window } from '@kit.ArkUI';
import native from 'libentry.so';
import { AppWindowPref, appWindowPrefs } from '../common/AppWindowPrefs';

const TAG: string = 'SvboxAppWin';

export default class AppWindowAbility extends UIAbility {
  private mainWindow: window.Window | undefined = undefined;
  private localStorage: LocalStorage = new LocalStorage();
  private displaySlot: number = -1;
  private androidPkg: string = '';

  private parseWant(want: Want, isNewWant: boolean): void {
    const rawPkg: Object | undefined = want.parameters ? want.parameters['androidPkg'] : undefined;
    const rawComp: Object | undefined = want.parameters ? want.parameters['androidComp'] : undefined;
    const rawLabel: Object | undefined = want.parameters ? want.parameters['androidLabel'] : undefined;
    const rawTask: Object | undefined = want.parameters ? want.parameters['moveTaskId'] : undefined;
    const rawSlot: Object | undefined = want.parameters ? want.parameters['preAllocSlot'] : undefined;

    const pkg: string = typeof rawPkg === 'string' ? rawPkg : '';
    const comp: string = typeof rawComp === 'string' ? rawComp : '';
    const label: string = typeof rawLabel === 'string' && rawLabel.length > 0 ? rawLabel : pkg;
    const taskId: number = typeof rawTask === 'string' ? Number(rawTask) : (typeof rawTask === 'number' ? rawTask : -1);
    const preSlot: number = typeof rawSlot === 'string' ? Number(rawSlot) : (typeof rawSlot === 'number' ? rawSlot : -1);

    if (pkg.length > 0) {
      this.androidPkg = pkg;
    }
    if (this.displaySlot < 1) {
      if (preSlot >= 1 && preSlot <= 8) {
        this.displaySlot = preSlot;
      } else {
        this.displaySlot = native.allocDisplaySlot(this.androidPkg);
      }
    }

    this.localStorage.setOrCreate<string>('androidPkg', this.androidPkg);
    this.localStorage.setOrCreate<string>('androidComp', comp);
    this.localStorage.setOrCreate<string>('androidLabel', label);
    this.localStorage.setOrCreate<number>('displaySlot', this.displaySlot);
    this.localStorage.setOrCreate<number>('moveTaskId', Number.isNaN(taskId) ? -1 : taskId);
    this.localStorage.setOrCreate<string>('sandboxTempDir', this.context.tempDir);
    this.localStorage.setOrCreate<string>('sandboxFilesDir', this.context.filesDir);

    if (isNewWant) {
      const seq: number = (this.localStorage.get<number>('relaunchSeq') ?? 0) + 1;
      this.localStorage.setOrCreate<number>('relaunchSeq', seq);
    } else {
      this.localStorage.setOrCreate<number>('relaunchSeq', 0);
    }

    AppStorage.setOrCreate<string>('sandboxTempDir', this.context.tempDir);
    AppStorage.setOrCreate<string>('sandboxFilesDir', this.context.filesDir);
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
      const globalHz: number = AppStorage.get<number>('refreshRate') ?? 60;
      const pref: AppWindowPref = appWindowPrefs.getPref(this.context.filesDir, this.androidPkg, undefined, globalHz);

      try {
        win.resize(pref.winW, pref.winH);
      } catch (_e) {
      }

      const props: window.WindowProperties = win.getWindowProperties();
      this.localStorage.setOrCreate<number>('winPxW', props.windowRect.width);
      this.localStorage.setOrCreate<number>('winPxH', props.windowRect.height);

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
      try {
        const title: string = this.localStorage.get<string>('androidLabel') ?? this.androidPkg;
        this.mainWindow?.setWindowTitle(title);
      } catch (_e) {
      }
    });
  }

  onDestroy(): void {
    hilog.info(0x0000, TAG, 'AppWindowAbility onDestroy slot=%{public}d pkg=%{public}s',
      this.displaySlot, this.androidPkg);
    const pulledBack: boolean = this.localStorage.get<boolean>('pulledBack') ?? false;
    if (this.displaySlot >= 1) {
      const slot: number = this.displaySlot;
      const pkg: string = this.androidPkg;
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
