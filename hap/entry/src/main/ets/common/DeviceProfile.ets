import { deviceInfo } from '@kit.BasicServicesKit';

export type DeviceFormFactor = 'pc' | 'tablet' | 'phone';

export function getDeviceFormFactor(): DeviceFormFactor {
  const override: string | undefined = AppStorage.get<string>('deviceFormOverride');
  if (override === 'pc' || override === 'tablet' || override === 'phone') {
    return override;
  }
  if (override === 'mobile') {
    return 'phone';
  }
  const dt: string = (deviceInfo.deviceType ?? '2in1').toLowerCase();
  if (dt === '2in1' || dt === 'pc') {
    return 'pc';
  }
  if (dt === 'tablet') {
    return 'tablet';
  }
  return 'phone';
}

export function isPcDevice(): boolean {
  return getDeviceFormFactor() === 'pc';
}

export function isMobileDevice(): boolean {
  return getDeviceFormFactor() !== 'pc';
}

/**
 * Mobile devices (phone / tablet) run strictly in single-container mode:
 * multi-instance creation, cloning, and batch management are PC-only.
 */
export function supportsMultiInstance(): boolean {
  return isPcDevice();
}

/**
 * PC keyboard/mouse mapping and automation overlay is enabled on PC and tablet
 * (for external keyboard/mouse users), hidden on phone screens.
 */
export function supportsPcKeymap(): boolean {
  return getDeviceFormFactor() !== 'phone';
}

export function getDeviceFormLabel(): string {
  const f: DeviceFormFactor = getDeviceFormFactor();
  if (f === 'pc') return 'HarmonyOS PC';
  if (f === 'tablet') return 'HarmonyOS 平板（单容器模式）';
  return 'HarmonyOS 手机（单容器模式）';
}
