import { AbilityConstant, UIAbility, Want } from '@kit.AbilityKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { window } from '@kit.ArkUI';
import { image } from '@kit.ImageKit';
import native from 'libentry.so';

const TAG = 'SvboxAndroid';

export default class EntryAbility extends UIAbility {
  private mainWindow: window.Window | undefined = undefined;
  private launchSeq: number = 0;

  private applyWant(want: Want, isNewWant: boolean): void {
    const rawAuto: Object | undefined = want.parameters ? want.parameters['autoStart'] : undefined;
    const rawUiMode: Object | undefined = want.parameters ? want.parameters['uiMode'] : undefined;
    const rawGpu: Object | undefined = want.parameters ? want.parameters['gpuMode'] : undefined;
    const rawRes: Object | undefined = want.parameters ? want.parameters['resolution'] : undefined;
    const rawHz: Object | undefined = want.parameters ? want.parameters['refreshRate'] : undefined;
    const rawSetHz: Object | undefined = want.parameters ? want.parameters['setRefreshRate'] : undefined;
    const rawSnap: Object | undefined = want.parameters ? want.parameters['snapPath'] : undefined;
    const rawCmd: Object | undefined = want.parameters ? want.parameters['guestCmd'] : undefined;
    const rawCmdOut: Object | undefined = want.parameters ? want.parameters['guestCmdOut'] : undefined;
    const rawDump: Object | undefined = want.parameters ? want.parameters['dumpProcs'] : undefined;
    const rawTap: Object | undefined = want.parameters ? want.parameters['injectTap'] : undefined;
    const autoStr: string = typeof rawAuto === 'string' ? rawAuto : '';
    const uiModeStr: string = typeof rawUiMode === 'string' ? rawUiMode : '';
    const gpuStr: string = typeof rawGpu === 'string' ? rawGpu : '';
    const resStr: string = typeof rawRes === 'string' ? rawRes : '';
    const hzStr: string = typeof rawHz === 'string' ? rawHz : (typeof rawHz === 'number' ? String(rawHz) : '');
    const setHzStr: string = typeof rawSetHz === 'string' ? rawSetHz : (typeof rawSetHz === 'number' ? String(rawSetHz) : '');
    const snapStr: string = typeof rawSnap === 'string' ? rawSnap : '';
    const cmdStr: string = typeof rawCmd === 'string' ? rawCmd : '';
    const cmdOutStr: string = typeof rawCmdOut === 'string' ? rawCmdOut : '';
    const dumpStr: string = typeof rawDump === 'string' ? rawDump : '';
    const tapStr: string = typeof rawTap === 'string' ? rawTap : '';

    if (setHzStr.length > 0 && autoStr.length === 0) {
      const hz: number = Number(setHzStr);
      if (hz === 60 || hz === 90 || hz === 120) {
        AppStorage.setOrCreate<number>('refreshRate', hz);
        native.setRefreshRate(hz);
      }
      return;
    }

    if (cmdStr.length > 0 && autoStr.length === 0) {
      native.runGuestCmd(cmdStr, cmdOutStr);
      return;
    }
    if (dumpStr.length > 0 && autoStr.length === 0) {
      native.dumpProcs(dumpStr);
      return;
    }
    if (tapStr.length > 0 && autoStr.length === 0) {
      const parts: string[] = tapStr.split(',');
      if (parts.length === 2) {
        const x: number = Number(parts[0]);
        const y: number = Number(parts[1]);
        if (!Number.isNaN(x) && !Number.isNaN(y)) {
          native.injectTap(x, y);
        }
      }
      return;
    }

    if (snapStr.length > 0 && autoStr.length === 0) {
      this.captureWindowSnapshot(snapStr);
      return;
    }

    if (uiModeStr === 'multiwindow' || uiModeStr === 'emulator') {
      AppStorage.setOrCreate<string>('requestedUiMode', uiModeStr);
    } else if (autoStr === 'bootanim' || autoStr === 'full') {
      AppStorage.setOrCreate<string>('requestedUiMode', 'emulator');
    }
    if (gpuStr.length > 0) {
      AppStorage.setOrCreate<string>('gpuMode', gpuStr);
    }
    if (resStr.length > 0) {
      AppStorage.setOrCreate<string>('resolution', resStr);
    }
    if (hzStr.length > 0) {
      const hz: number = Number(hzStr);
      if (hz === 60 || hz === 90 || hz === 120) {
        AppStorage.setOrCreate<number>('refreshRate', hz);
      }
    }
    if (autoStr.length > 0 || !isNewWant) {
      AppStorage.setOrCreate<string>('autoStart', autoStr);
    }
    if (isNewWant && autoStr.length > 0) {
      this.launchSeq += 1;
      AppStorage.setOrCreate<number>('launchSeq', this.launchSeq);
    }
  }

  private captureWindowSnapshot(outPath: string): void {
    if (!this.mainWindow) {
      hilog.error(0x0000, TAG, 'captureWindowSnapshot: mainWindow unavailable');
      return;
    }
    this.mainWindow.snapshot().then((pm: image.PixelMap) => {
      pm.getImageInfo().then((info: image.ImageInfo) => {
        const w: number = info.size.width;
        const h: number = info.size.height;
        const buf: ArrayBuffer = new ArrayBuffer(w * h * 4);
        pm.readPixelsToBufferSync(buf);
        const ok: boolean = native.saveRawSnapshot(outPath, w, h, buf);
        hilog.info(0x0000, TAG, 'Snapshot saved %{public}s (%{public}dx%{public}d fmt=%{public}d ok=%{public}d)',
          outPath, w, h, info.pixelFormat, ok ? 1 : 0);
        pm.release();
      }).catch((err: Error) => {
        hilog.error(0x0000, TAG, 'snapshot getImageInfo failed: %{public}s', String(err));
        pm.release();
      });
    }).catch((err: Error) => {
      hilog.error(0x0000, TAG, 'window.snapshot failed: %{public}s', String(err));
    });
  }

  onCreate(want: Want, _launchParam: AbilityConstant.LaunchParam): void {
    hilog.info(0x0000, TAG, 'EntryAbility onCreate');
    AppStorage.setOrCreate<string>('sandboxTempDir', this.context.tempDir);
    AppStorage.setOrCreate<string>('sandboxFilesDir', this.context.filesDir);
    AppStorage.setOrCreate<number>('launchSeq', 0);
    this.applyWant(want, false);
  }

  onNewWant(want: Want, _launchParam: AbilityConstant.LaunchParam): void {
    hilog.info(0x0000, TAG, 'EntryAbility onNewWant');
    this.applyWant(want, true);
  }

  onForeground(): void {
    try {
      const curMode: string = AppStorage.get<string>('activeUiMode') ?? 'multiwindow';
      native.setDisplayVisible(0, curMode === 'emulator');
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
      this.mainWindow = windowStage.getMainWindowSync();
    } catch (e) {
      hilog.error(0x0000, TAG, 'getMainWindowSync failed: %{public}s', String(e));
    }
    windowStage.loadContent('pages/Index', (err) => {
      if (err.code) {
        hilog.error(0x0000, TAG, 'Failed to load Index: %{public}s', JSON.stringify(err));
      }
    });
  }
}
