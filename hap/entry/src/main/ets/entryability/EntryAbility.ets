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
    const rawGpu: Object | undefined = want.parameters ? want.parameters['gpuMode'] : undefined;
    const rawSnap: Object | undefined = want.parameters ? want.parameters['snapPath'] : undefined;
    const autoStr: string = typeof rawAuto === 'string' ? rawAuto : '';
    const gpuStr: string = typeof rawGpu === 'string' ? rawGpu : '';
    const snapStr: string = typeof rawSnap === 'string' ? rawSnap : '';

    if (snapStr.length > 0 && autoStr.length === 0) {
      this.captureWindowSnapshot(snapStr);
      return;
    }

    if (gpuStr.length > 0) {
      AppStorage.setOrCreate<string>('gpuMode', gpuStr);
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
      native.setVisible(true);
    } catch (_e) {
    }
  }

  onBackground(): void {
    try {
      native.setVisible(false);
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
