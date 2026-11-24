import { fileIo } from '@kit.CoreFileKit';
import { buffer } from '@kit.ArkTS';

export interface ResolutionPresetItem {
  label: string;
  key: string;
  w: number;
  h: number;
}

export const RESOLUTION_PRESETS: ResolutionPresetItem[] = [
  { label: '720×1280 竖', key: '720x1280', w: 720, h: 1280 },
  { label: '1080×1920 竖', key: '1080x1920', w: 1080, h: 1920 },
  { label: '1280×720 横', key: '1280x720', w: 1280, h: 720 },
  { label: '1600×900 横', key: '1600x900', w: 1600, h: 900 },
  { label: '1920×1080 横', key: '1920x1080', w: 1920, h: 1080 },
];

export const REFRESH_RATES: number[] = [60, 90, 120];

export interface AndroidAppItem {
  packageName: string;
  component: string;
  label: string;
  iconFile: string;
  resizeable: boolean;
  isGame: boolean;
  isSystem: boolean;
  orientation: number;
  defaultMode: string; // 'adaptive' | 'fixed_portrait' | 'fixed_landscape'
}

export interface AppWindowPref {
  mode: string;         // 'adaptive' | 'fixed_portrait' | 'fixed_landscape'
  presetKey: string;    // e.g. '720x1280' or '1280x720'
  refreshRate: number;  // 60 | 90 | 120
  winW: number;         // last logical window width (px)
  winH: number;         // last logical window height (px)
}

export interface WmEventItem {
  event: string;
  taskId: number;
  fromSlot: number;
  fromPkg: string;
  packageName: string;
  component: string;
  label: string;
  slot: number;
}

export function computeDpiForSize(w: number, h: number): number {
  if (w === 720 && h === 1280) return 320;
  if (w === 1080 && h === 1920) return 420;
  if (w === 1280 && h === 720) return 240;
  if (w === 1600 && h === 900) return 280;
  if (w === 1920 && h === 1080) return 320;
  const shortSide: number = Math.min(w, h);
  let dpi: number = Math.floor((shortSide * 280) / 720);
  if (dpi < 160) dpi = 160;
  if (dpi > 480) dpi = 480;
  return dpi;
}

export function computeShortcutId(pkg: string): string {
  // Deterministic <= 63 bytes shortcutId for AppGalleryKit
  const sanitized: string = pkg.replace(/[^a-zA-Z0-9._-]/g, '_');
  if (sanitized.length <= 58) {
    return `sv_${sanitized}`;
  }
  let h1: number = 0x811c9dc5;
  let h2: number = 0x9e3779b9;
  for (let i: number = 0; i < pkg.length; i++) {
    const ch: number = pkg.charCodeAt(i);
    h1 = Math.imul(h1 ^ ch, 0x01000193) >>> 0;
    h2 = Math.imul(h2 ^ (ch + i), 0x85ebca6b) >>> 0;
  }
  const hex: string = h1.toString(16).padStart(8, '0') + h2.toString(16).padStart(8, '0');
  return `sv_${sanitized.substring(0, 42)}_${hex}`;
}

export function findPresetByKey(key: string): ResolutionPresetItem {
  for (let i: number = 0; i < RESOLUTION_PRESETS.length; i++) {
    if (RESOLUTION_PRESETS[i].key === key) {
      return RESOLUTION_PRESETS[i];
    }
  }
  return RESOLUTION_PRESETS[0];
}

export function defaultPrefForApp(app: AndroidAppItem | undefined, globalHz: number): AppWindowPref {
  const mode: string = app ? app.defaultMode : 'adaptive';
  const hz: number = (globalHz === 60 || globalHz === 90 || globalHz === 120) ? globalHz : 60;
  if (mode === 'fixed_landscape') {
    return { mode: 'fixed_landscape', presetKey: '1280x720', refreshRate: hz, winW: 1280, winH: 760 };
  }
  if (mode === 'fixed_portrait') {
    return { mode: 'fixed_portrait', presetKey: '720x1280', refreshRate: hz, winW: 540, winH: 960 };
  }
  return { mode: 'adaptive', presetKey: '720x1280', refreshRate: hz, winW: 640, winH: 920 };
}

interface PrefRecordEntry {
  pkg: string;
  pref: AppWindowPref;
}

class AppWindowPrefsStore {
  private loaded: boolean = false;
  private filePath: string = '';
  private map: Map<string, AppWindowPref> = new Map<string, AppWindowPref>();

  private ensureLoaded(filesDir: string): void {
    if (this.loaded && this.filePath.length > 0) return;
    if (filesDir.length === 0) return;
    this.filePath = `${filesDir}/app_window_prefs.json`;
    this.loaded = true;
    try {
      if (!fileIo.accessSync(this.filePath)) return;
      const stat: fileIo.Stat = fileIo.statSync(this.filePath);
      if (stat.size <= 0 || stat.size > 1024 * 1024) return;
      const f: fileIo.File = fileIo.openSync(this.filePath, fileIo.OpenMode.READ_ONLY);
      const ab: ArrayBuffer = new ArrayBuffer(stat.size);
      fileIo.readSync(f.fd, ab);
      fileIo.closeSync(f);
      const txt: string = buffer.from(ab).toString('utf-8');
      const parsed: PrefRecordEntry[] = JSON.parse(txt) as PrefRecordEntry[];
      if (Array.isArray(parsed)) {
        for (let i: number = 0; i < parsed.length; i++) {
          const item: PrefRecordEntry = parsed[i];
          if (item && item.pkg && item.pref) {
            this.map.set(item.pkg, item.pref);
          }
        }
      }
    } catch (_e) {
    }
  }

  private persist(): void {
    if (this.filePath.length === 0) return;
    try {
      const list: PrefRecordEntry[] = [];
      this.map.forEach((pref: AppWindowPref, pkg: string) => {
        list.push({ pkg: pkg, pref: pref });
      });
      const txt: string = JSON.stringify(list);
      const f: fileIo.File = fileIo.openSync(
        this.filePath,
        fileIo.OpenMode.CREATE | fileIo.OpenMode.TRUNC | fileIo.OpenMode.WRITE_ONLY
      );
      fileIo.writeSync(f.fd, txt);
      fileIo.closeSync(f);
    } catch (_e) {
    }
  }

  public getPref(filesDir: string, pkg: string, app: AndroidAppItem | undefined, globalHz: number): AppWindowPref {
    this.ensureLoaded(filesDir);
    const existing: AppWindowPref | undefined = this.map.get(pkg);
    if (existing) {
      return {
        mode: existing.mode,
        presetKey: existing.presetKey,
        refreshRate: existing.refreshRate,
        winW: existing.winW,
        winH: existing.winH,
      };
    }
    return defaultPrefForApp(app, globalHz);
  }

  public savePref(filesDir: string, pkg: string, pref: AppWindowPref): void {
    this.ensureLoaded(filesDir);
    this.map.set(pkg, {
      mode: pref.mode,
      presetKey: pref.presetKey,
      refreshRate: pref.refreshRate,
      winW: pref.winW,
      winH: pref.winH,
    });
    this.persist();
  }
}

export const appWindowPrefs: AppWindowPrefsStore = new AppWindowPrefsStore();
