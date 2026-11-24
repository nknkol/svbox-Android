import { common, Want } from '@kit.AbilityKit';
import { productViewManager } from '@kit.AppGalleryKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { AndroidAppItem, computeShortcutId } from './AppWindowPrefs';

const TAG: string = 'SvboxShortcut';

export interface ShortcutOpResult {
  ok: boolean;
  code: number;
  message: string;
  limit: number;
}

export async function queryPinnedShortcutIds(): Promise<string[]> {
  try {
    const infos: productViewManager.PinShortcutInfo[] = await productViewManager.getPinShortcutInfos();
    const ids: string[] = [];
    for (let i: number = 0; i < infos.length; i++) {
      if (infos[i].shortcutId) {
        ids.push(infos[i].shortcutId);
      }
    }
    return ids;
  } catch (err) {
    const bErr: BusinessError = err as BusinessError;
    hilog.warn(0x0000, TAG, 'getPinShortcutInfos unavailable: code=%{public}d msg=%{public}s',
      bErr.code ?? -1, bErr.message ?? String(err));
    return [];
  }
}

export async function pinAndroidAppToDesktop(
  ctx: common.UIAbilityContext,
  app: AndroidAppItem,
  iconSandboxPath: string
): Promise<ShortcutOpResult> {
  const shortcutId: string = computeShortcutId(app.packageName);
  const label: string = (app.label && app.label.length > 0) ? app.label.substring(0, 250) : app.packageName;
  const want: Want = {
    bundleName: 'org.svbox.android',
    moduleName: 'entry',
    abilityName: 'AppWindowAbility',
    parameters: {
      'androidPkg': app.packageName,
      'androidComp': app.component,
      'androidLabel': label,
      'fromDesktopShortcut': '1',
    }
  };

  try {
    const checkRes: productViewManager.CheckShortcutResult =
      await productViewManager.checkPinShortcutPermitted(
        ctx,
        shortcutId,
        want,
        label,
        iconSandboxPath,
        ''
      );
    const tid: string = checkRes.tid ?? '';
    const limit: number = checkRes.limit ?? 10;
    if (checkRes.code !== 0 || tid.length === 0) {
      return {
        ok: false,
        code: checkRes.code,
        message: `加桌校验失败 (code=${checkRes.code})`,
        limit: limit,
      };
    }
    await productViewManager.requestNewPinShortcut(ctx, tid);
    hilog.info(0x0000, TAG, 'Pinned desktop shortcut %{public}s -> %{public}s', shortcutId, app.packageName);
    return {
      ok: true,
      code: 0,
      message: `已将「${label}」添加到桌面`,
      limit: limit,
    };
  } catch (err) {
    const bErr: BusinessError = err as BusinessError;
    const code: number = bErr.code ?? -1;
    let hint: string = bErr.message ?? String(err);
    if (code === 1006620003) {
      hint = `「${label}」桌面图标已存在`;
    } else if (code === 1006620004) {
      hint = 'PC 桌面快捷方式已达 10 个上限，请先移除旧图标';
    } else if (code === 1006620007) {
      hint = '已取消添加到桌面';
    } else if (code === 1006620005) {
      hint = '系统风控/签名校验未放行 (1006620005)';
    }
    hilog.warn(0x0000, TAG, 'pinAndroidAppToDesktop failed code=%{public}d msg=%{public}s', code, hint);
    return { ok: false, code: code, message: hint, limit: 10 };
  }
}

export async function removeAndroidAppShortcut(
  ctx: common.UIAbilityContext,
  pkg: string
): Promise<ShortcutOpResult> {
  const shortcutId: string = computeShortcutId(pkg);
  try {
    await productViewManager.removePinShortcut(ctx, shortcutId);
    hilog.info(0x0000, TAG, 'Removed desktop shortcut %{public}s', shortcutId);
    return { ok: true, code: 0, message: '已移除桌面图标', limit: 10 };
  } catch (err) {
    const bErr: BusinessError = err as BusinessError;
    const code: number = bErr.code ?? -1;
    hilog.warn(0x0000, TAG, 'removePinShortcut failed code=%{public}d msg=%{public}s',
      code, bErr.message ?? String(err));
    return { ok: false, code: code, message: `移除失败 (code=${code})`, limit: 10 };
  }
}
