import { common, ConfigurationConstant } from '@kit.AbilityKit';
import { image } from '@kit.ImageKit';
import { fileIo } from '@kit.CoreFileKit';
import { window } from '@kit.ArkUI';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { quickBarManager } from '@kit.DeskTopExtensionKit';

const TAG: string = 'SvboxWinHelper';

const slotWindows: Map<number, window.Window> = new Map<number, window.Window>();

export function registerSlotWindow(slot: number, win: window.Window): void {
  if (slot >= 0) {
    slotWindows.set(slot, win);
  }
}

export function unregisterSlotWindow(slot: number): void {
  slotWindows.delete(slot);
}

export function getSlotWindow(slot: number): window.Window | undefined {
  return slotWindows.get(slot);
}

export function isColorDark(argb: number): boolean {
  const r: number = (argb >>> 16) & 0xff;
  const g: number = (argb >>> 8) & 0xff;
  const b: number = argb & 0xff;
  const lum: number = 0.299 * r + 0.587 * g + 0.114 * b;
  return lum < 148;
}

export function argbToHex(argb: number): string {
  const rgb: number = argb & 0xffffff;
  return '#' + rgb.toString(16).padStart(6, '0').toUpperCase();
}

export function applyImmersiveDecor(win: window.Window, heightVp: number, darkBg: boolean): number {
  let rightAvoidVp: number = 136;
  try {
    win.setWindowDecorVisible(false);
  } catch (err) {
    hilog.warn(0x0000, TAG, 'setWindowDecorVisible(false) warn: %{public}s', String(err));
  }
  try {
    win.setWindowDecorHeight(heightVp);
  } catch (err) {
    hilog.warn(0x0000, TAG, 'setWindowDecorHeight warn: %{public}s', String(err));
  }
  try {
    win.setWindowTitleMoveEnabled(true);
  } catch (_e) {
  }
  try {
    win.setDecorButtonStyle({
      colorMode: darkBg
        ? ConfigurationConstant.ColorMode.COLOR_MODE_DARK
        : ConfigurationConstant.ColorMode.COLOR_MODE_LIGHT,
    });
  } catch (_e) {
  }
  try {
    const rect: window.TitleButtonRect = win.getTitleButtonRect();
    if (rect && rect.width > 0) {
      rightAvoidVp = Math.max(116, Math.ceil(rect.width + rect.right + 10));
    }
  } catch (_e) {
  }
  return rightAvoidVp;
}

export function updateDecorButtonTheme(win: window.Window | undefined, darkBg: boolean): void {
  if (!win) return;
  try {
    win.setDecorButtonStyle({
      colorMode: darkBg
        ? ConfigurationConstant.ColorMode.COLOR_MODE_DARK
        : ConfigurationConstant.ColorMode.COLOR_MODE_LIGHT,
    });
  } catch (_e) {
  }
}

export async function decodeIconPixelMap(iconPath: string): Promise<image.PixelMap | undefined> {
  if (!iconPath || iconPath.length === 0) return undefined;
  try {
    if (!fileIo.accessSync(iconPath)) {
      return undefined;
    }
    const src: image.ImageSource = image.createImageSource(iconPath);
    const pm: image.PixelMap = await src.createPixelMap({
      desiredSize: { width: 256, height: 256 },
    });
    src.release();
    return pm;
  } catch (err) {
    hilog.warn(0x0000, TAG, 'decodeIconPixelMap(%s) failed: %{public}s', iconPath, String(err));
    return undefined;
  }
}

export async function applyMissionIconAndQuickBar(
  ctx: common.UIAbilityContext,
  win: window.Window,
  pkg: string,
  label: string,
  iconPath: string
): Promise<string> {
  const title: string = label && label.length > 0 ? label : pkg;
  try {
    await win.setWindowTitle(title);
  } catch (_e) {
  }
  try {
    await ctx.setMissionLabel(title);
  } catch (_e) {
  }

  const pm: image.PixelMap | undefined = await decodeIconPixelMap(iconPath);
  if (!pm) {
    return '';
  }

  try {
    await ctx.setMissionWindowIcon(pm);
    hilog.info(0x0000, TAG, 'setMissionWindowIcon ok for %{public}s', pkg);
  } catch (err) {
    hilog.warn(0x0000, TAG, 'setMissionWindowIcon warn for %{public}s: %{public}s', pkg, String(err));
  }

  const groupKey: string = `svbox_grp_${pkg}`;
  try {
    const supported: boolean = await quickBarManager.isQuickBarCapabilitySupported(ctx);
    if (supported) {
      try {
        await quickBarManager.addQuickBarGroup(ctx, {
          groupKey: groupKey,
          groupIcon: pm,
        });
      } catch (addErr) {
        // 1020210005 = Group already exists (safe to reuse)
        hilog.info(0x0000, TAG, 'addQuickBarGroup(%{public}s) note: %{public}s', groupKey, String(addErr));
      }
      const winId: number = win.getWindowProperties().id;
      await quickBarManager.setWindowToGroup(ctx, String(winId), groupKey);
      hilog.info(0x0000, TAG, 'setWindowToGroup(win=%{public}d, group=%{public}s) ok', winId, groupKey);
      try {
        pm.release();
      } catch (_e) {
      }
      return groupKey;
    }
  } catch (err) {
    hilog.warn(0x0000, TAG, 'quickBarManager group warn: %{public}s', String(err));
  }

  try {
    pm.release();
  } catch (_e) {
  }
  return '';
}

export async function cleanupQuickBarGroup(ctx: common.UIAbilityContext, groupKey: string): Promise<void> {
  if (!groupKey || groupKey.length === 0) return;
  try {
    await quickBarManager.deleteQuickBarGroup(ctx, groupKey);
  } catch (_e) {
  }
}
