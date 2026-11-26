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
  return computeAdaptiveDpi(w, h);
}

/**
 * Fixed PC-style desktop density for freeform adaptive windows.
 * Keeping DPI constant (~240 dpi) means resizing a window larger expands Android's
 * logical dp workspace (triggering sw600dp+ Tablet/PC dual-pane layouts) rather than
 * magnifying phone UI.
 */
export function computeAdaptiveDpi(_w: number, _h: number): number {
  return 240;
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
    return { mode: 'fixed_landscape', presetKey: '1280x720', refreshRate: hz, winW: 1480, winH: 940 };
  }
  if (mode === 'fixed_portrait') {
    return { mode: 'fixed_portrait', presetKey: '720x1280', refreshRate: hz, winW: 860, winH: 1380 };
  }
  return { mode: 'adaptive', presetKey: '720x1280', refreshRate: hz, winW: 860, winH: 1380 };
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

export interface InstanceConfig {
  runMode: string;        // 'multiwindow' | 'emulator'
  resolutionKey: string;  // e.g. '720x1280'
  refreshRate: number;    // 60 | 90 | 120
  gpuMode: string;        // 'vulkan' | 'gles' | 'swiftshader'
  profile: string;        // 'Balanced' | 'Performance' | 'Eco'
}

export function defaultInstanceConfig(): InstanceConfig {
  return {
    runMode: 'multiwindow',
    resolutionKey: '720x1280',
    refreshRate: 60,
    gpuMode: 'vulkan',
    profile: 'Balanced',
  };
}

class InstanceConfigStore {
  private loaded: boolean = false;
  private filePath: string = '';
  private cfg: InstanceConfig = defaultInstanceConfig();

  private ensureLoaded(filesDir: string): void {
    if (this.loaded && this.filePath.length > 0) return;
    if (filesDir.length === 0) return;
    this.filePath = `${filesDir}/instance_config.json`;
    this.loaded = true;
    try {
      if (!fileIo.accessSync(this.filePath)) return;
      const stat: fileIo.Stat = fileIo.statSync(this.filePath);
      if (stat.size <= 0 || stat.size > 65536) return;
      const f: fileIo.File = fileIo.openSync(this.filePath, fileIo.OpenMode.READ_ONLY);
      const ab: ArrayBuffer = new ArrayBuffer(stat.size);
      fileIo.readSync(f.fd, ab);
      fileIo.closeSync(f);
      const parsed: InstanceConfig = JSON.parse(buffer.from(ab).toString('utf-8')) as InstanceConfig;
      if (parsed) {
        if (parsed.runMode === 'multiwindow' || parsed.runMode === 'emulator') {
          this.cfg.runMode = parsed.runMode;
        }
        if (parsed.resolutionKey && parsed.resolutionKey.length > 0) {
          this.cfg.resolutionKey = parsed.resolutionKey;
        }
        if (parsed.refreshRate === 60 || parsed.refreshRate === 90 || parsed.refreshRate === 120) {
          this.cfg.refreshRate = parsed.refreshRate;
        }
        if (parsed.gpuMode === 'vulkan' || parsed.gpuMode === 'gles' || parsed.gpuMode === 'swiftshader') {
          this.cfg.gpuMode = parsed.gpuMode;
        }
        if (parsed.profile === 'Balanced' || parsed.profile === 'Performance' || parsed.profile === 'Eco') {
          this.cfg.profile = parsed.profile;
        }
      }
    } catch (_e) {
    }
  }

  public get(filesDir: string): InstanceConfig {
    this.ensureLoaded(filesDir);
    return {
      runMode: this.cfg.runMode,
      resolutionKey: this.cfg.resolutionKey,
      refreshRate: this.cfg.refreshRate,
      gpuMode: this.cfg.gpuMode,
      profile: this.cfg.profile,
    };
  }

  public save(filesDir: string, next: InstanceConfig): void {
    this.ensureLoaded(filesDir);
    this.cfg = {
      runMode: next.runMode,
      resolutionKey: next.resolutionKey,
      refreshRate: next.refreshRate,
      gpuMode: next.gpuMode,
      profile: next.profile,
    };
    if (this.filePath.length === 0) return;
    try {
      const f: fileIo.File = fileIo.openSync(
        this.filePath,
        fileIo.OpenMode.CREATE | fileIo.OpenMode.TRUNC | fileIo.OpenMode.WRITE_ONLY
      );
      fileIo.writeSync(f.fd, JSON.stringify(this.cfg));
      fileIo.closeSync(f);
    } catch (_e) {
    }
  }
}

export const instanceConfigStore: InstanceConfigStore = new InstanceConfigStore();

export function getCachedIconsDir(filesDir: string, dataSubdir: string): string {
  const base: string = filesDir.length > 0 ? filesDir : '/data/storage/el2/base/haps/entry/files';
  const sub: string = dataSubdir.length > 0 ? dataSubdir : 'userdata14';
  return `${base}/${sub}/icons`;
}

export function loadCachedAppList(filesDir: string, dataSubdir: string): AndroidAppItem[] {
  try {
    const base: string = filesDir.length > 0 ? filesDir : '/data/storage/el2/base/haps/entry/files';
    const sub: string = dataSubdir.length > 0 ? dataSubdir : 'userdata14';
    const path: string = `${base}/${sub}/apps_cache.json`;
    if (!fileIo.accessSync(path)) return [];
    const stat: fileIo.Stat = fileIo.statSync(path);
    if (stat.size <= 0 || stat.size > 4 * 1024 * 1024) return [];
    const f: fileIo.File = fileIo.openSync(path, fileIo.OpenMode.READ_ONLY);
    const ab: ArrayBuffer = new ArrayBuffer(stat.size);
    fileIo.readSync(f.fd, ab);
    fileIo.closeSync(f);
    const parsed: AndroidAppItem[] = JSON.parse(buffer.from(ab).toString('utf-8')) as AndroidAppItem[];
    return Array.isArray(parsed) ? parsed : [];
  } catch (_e) {
    return [];
  }
}

export function saveCachedAppList(filesDir: string, dataSubdir: string, apps: AndroidAppItem[]): void {
  try {
    const base: string = filesDir.length > 0 ? filesDir : '/data/storage/el2/base/haps/entry/files';
    const sub: string = dataSubdir.length > 0 ? dataSubdir : 'userdata14';
    const dir: string = `${base}/${sub}`;
    try {
      fileIo.mkdirSync(dir, true);
    } catch (_mk) {
    }
    const path: string = `${dir}/apps_cache.json`;
    const f: fileIo.File = fileIo.openSync(
      path,
      fileIo.OpenMode.CREATE | fileIo.OpenMode.TRUNC | fileIo.OpenMode.WRITE_ONLY
    );
    fileIo.writeSync(f.fd, JSON.stringify(apps));
    fileIo.closeSync(f);
  } catch (_e) {
  }
}
