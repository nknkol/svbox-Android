import { AbilityConstant, UIAbility, Want } from '@kit.AbilityKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { window } from '@kit.ArkUI';
import native from 'libentry.so';

const TAG = 'SvboxAndroid';

export default class EntryAbility extends UIAbility {
  onCreate(want: Want, launchParam: AbilityConstant.LaunchParam): void {
    hilog.info(0x0000, TAG, 'EntryAbility onCreate');
    AppStorage.setOrCreate<string>('sandboxTempDir', this.context.tempDir);
    AppStorage.setOrCreate<string>('sandboxFilesDir', this.context.filesDir);
    const rawAuto: Object | undefined = want.parameters ? want.parameters['autoStart'] : undefined;
    AppStorage.setOrCreate<string>('autoStart', typeof rawAuto === 'string' ? rawAuto : '');
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
    windowStage.loadContent('pages/Index', (err) => {
      if (err.code) {
        hilog.error(0x0000, TAG, 'Failed to load Index: %{public}s', JSON.stringify(err));
      }
    });
  }
}
