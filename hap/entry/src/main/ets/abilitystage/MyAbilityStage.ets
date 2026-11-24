import { AbilityStage, Want } from '@kit.AbilityKit';
import { hilog } from '@kit.PerformanceAnalysisKit';

const TAG: string = 'SvboxStage';

export default class MyAbilityStage extends AbilityStage {
  onCreate(): void {
    hilog.info(0x0000, TAG, 'MyAbilityStage onCreate');
  }

  onAcceptWant(want: Want): string {
    if (want.abilityName === 'AppWindowAbility') {
      const rawPkg: Object | undefined = want.parameters ? want.parameters['androidPkg'] : undefined;
      const pkg: string = typeof rawPkg === 'string' && rawPkg.length > 0 ? rawPkg : 'default';
      const key: string = `svbox_app_${pkg}`;
      hilog.info(0x0000, TAG, 'onAcceptWant AppWindowAbility -> %{public}s', key);
      return key;
    }
    return 'EntryAbility';
  }
}
