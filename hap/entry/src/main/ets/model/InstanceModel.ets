import { fileIo } from '@kit.CoreFileKit';
import { buffer } from '@kit.ArkTS';
import {
  computeDpiForSize,
  findPresetByKey,
  InstanceConfig,
  instanceConfigStore,
  ResolutionPresetItem
} from '../common/AppWindowPrefs';

export interface DeviceModelPreset {
  label: string;
  brand: string;
  model: string;
}

export const DEVICE_MODEL_PRESETS: DeviceModelPreset[] = [
  { label: '默认（系统自动识别）', brand: '', model: '' },
  { label: 'Pixel 8 Pro', brand: 'Google', model: 'Pixel 8 Pro' },
  { label: 'Xiaomi 14 Pro', brand: 'Xiaomi', model: '23116PN5BC' },
  { label: 'OnePlus 12', brand: 'OnePlus', model: 'PJD110' },
  { label: 'Galaxy S24 Ultra', brand: 'samsung', model: 'SM-S9280' },
  { label: 'ROG Phone 8', brand: 'asus', model: 'ASUS_AI2401' },
];

export interface DeviceInstance {
  id: string;
  index: number;
  name: string;
  deviceType: string;      // 'phone' | 'tablet'
  androidVersion: string;  // 'Android 14'
  dataSubdir: string;      // 'userdata14' for #0, 'instances/inst_xxx' for others
  runMode: string;         // 'emulator' | 'multiwindow'
  resolutionKey: string;
  width: number;
  height: number;
  dpi: number;
  refreshRate: number;     // 60 | 90 | 120
  gpuMode: string;         // 'vulkan' | 'gles' | 'swiftshader'
  profile: string;         // 'Performance' | 'Balanced' | 'Eco'
  cpuCores: number;        // 2 | 4 | 6 | 8
  memLimitMb: number;      // 2048 | 4096 | 6144 | 8192
  scenario: string;        // 'normal' | 'extreme' | 'performance'
  compactDisk: boolean;
  enableAudio: boolean;
  enableRoot: boolean;
  keepAlive: boolean;
  preventSleep: boolean;
  autoGamepad: boolean;
  deviceBrand: string;
  deviceModel: string;
  adbPort: number;
  createdAt: number;
  lastRunAt: number;
}

export interface CreateInstanceOptions {
  deviceType: string;      // 'phone' | 'tablet'
  androidVersion: string;  // 'Android 14'
  count: number;           // 1..5
  scenario: string;        // 'normal' | 'extreme' | 'performance'
  compactDisk: boolean;
  namePrefix: string;
  gpuMode: string;
  refreshRate: number;
  runMode: string;
}

export interface GlobalSettings {
  autoBootDefault: boolean;
  batchStartIntervalSec: number;
  windowArrangeMode: string; // 'grid' | 'cascade'
  backupDir: string;
  sortBy: string;            // 'index' | 'time' | 'name'
  viewMode: string;          // 'card' | 'compact'
}

interface PersistedStateV2 {
  version: number;
  nextIndex: number;
  instances: DeviceInstance[];
  globalSettings: GlobalSettings;
}

export function defaultGlobalSettings(): GlobalSettings {
  return {
    autoBootDefault: false,
    batchStartIntervalSec: 3,
    windowArrangeMode: 'grid',
    backupDir: '/storage/Users/currentUser/Download/svbox-backups',
    sortBy: 'index',
    viewMode: 'card',
  };
}

export function cloneInstanceRecord(src: DeviceInstance): DeviceInstance {
  return {
    id: src.id,
    index: src.index,
    name: src.name,
    deviceType: src.deviceType,
    androidVersion: src.androidVersion,
    dataSubdir: src.dataSubdir,
    runMode: src.runMode,
    resolutionKey: src.resolutionKey,
    width: src.width,
    height: src.height,
    dpi: src.dpi,
    refreshRate: src.refreshRate,
    gpuMode: src.gpuMode,
    profile: src.profile,
    cpuCores: src.cpuCores,
    memLimitMb: src.memLimitMb,
    scenario: src.scenario,
    compactDisk: src.compactDisk,
    enableAudio: src.enableAudio,
    enableRoot: src.enableRoot,
    keepAlive: src.keepAlive,
    preventSleep: src.preventSleep,
    autoGamepad: src.autoGamepad,
    deviceBrand: src.deviceBrand,
    deviceModel: src.deviceModel,
    adbPort: src.adbPort,
    createdAt: src.createdAt,
    lastRunAt: src.lastRunAt,
  };
}

export function profileToCpuCores(profile: string): number {
  if (profile === 'Performance') return 8;
  if (profile === 'Eco') return 2;
  return 4;
}

export function cpuCoresToProfile(cores: number): string {
  if (cores >= 6) return 'Performance';
  if (cores <= 2) return 'Eco';
  return 'Balanced';
}

export function formatGpuModeLabel(gpuMode: string): string {
  if (gpuMode === 'vulkan') return 'Vulkan';
  if (gpuMode === 'gles') return 'OpenGL ES';
  return 'CPU 软渲';
}

export function formatDiskSize(mb: number): string {
  if (mb <= 0.05) return '未初始化';
  if (mb >= 1024) {
    return `${(mb / 1024).toFixed(2)} GB`;
  }
  return `${mb.toFixed(1)} MB`;
}

class InstanceManagerStore {
  private loaded: boolean = false;
  private filePath: string = '';
  private nextIndex: number = 1;
  private instances: DeviceInstance[] = [];
  private settings: GlobalSettings = defaultGlobalSettings();

  private createDefaultPrimary(filesDir: string): DeviceInstance {
    const legacy: InstanceConfig = instanceConfigStore.get(filesDir);
    const preset: ResolutionPresetItem = findPresetByKey(legacy.resolutionKey);
    const isTablet: boolean = preset.w > preset.h;
    const now: number = Date.now();
    return {
      id: 'inst_0',
      index: 0,
      name: 'Android 14 · 默认设备',
      deviceType: isTablet ? 'tablet' : 'phone',
      androidVersion: 'Android 14',
      dataSubdir: 'userdata14',
      runMode: legacy.runMode === 'emulator' ? 'emulator' : 'multiwindow',
      resolutionKey: preset.key,
      width: preset.w,
      height: preset.h,
      dpi: computeDpiForSize(preset.w, preset.h),
      refreshRate: legacy.refreshRate === 120 ? 120 : (legacy.refreshRate === 90 ? 90 : 60),
      gpuMode: legacy.gpuMode,
      profile: legacy.profile,
      cpuCores: profileToCpuCores(legacy.profile),
      memLimitMb: 4096,
      scenario: 'normal',
      compactDisk: false,
      enableAudio: true,
      enableRoot: true,
      keepAlive: true,
      preventSleep: false,
      autoGamepad: true,
      deviceBrand: '',
      deviceModel: '',
      adbPort: 5555,
      createdAt: now,
      lastRunAt: 0,
    };
  }

  private ensureLoaded(filesDir: string): void {
    if (this.loaded && this.filePath.length > 0) return;
    if (filesDir.length === 0) return;
    this.filePath = `${filesDir}/instances_v2.json`;
    this.loaded = true;
    try {
      if (fileIo.accessSync(this.filePath)) {
        const stat: fileIo.Stat = fileIo.statSync(this.filePath);
        if (stat.size > 0 && stat.size < 2 * 1024 * 1024) {
          const f: fileIo.File = fileIo.openSync(this.filePath, fileIo.OpenMode.READ_ONLY);
          const ab: ArrayBuffer = new ArrayBuffer(stat.size);
          fileIo.readSync(f.fd, ab);
          fileIo.closeSync(f);
          const parsed: PersistedStateV2 = JSON.parse(buffer.from(ab).toString('utf-8')) as PersistedStateV2;
          if (parsed && Array.isArray(parsed.instances) && parsed.instances.length > 0) {
            this.instances = parsed.instances;
            this.nextIndex = parsed.nextIndex > 0 ? parsed.nextIndex : parsed.instances.length;
            if (parsed.globalSettings) {
              const def: GlobalSettings = defaultGlobalSettings();
              this.settings = {
                autoBootDefault: parsed.globalSettings.autoBootDefault ?? def.autoBootDefault,
                batchStartIntervalSec: parsed.globalSettings.batchStartIntervalSec ?? def.batchStartIntervalSec,
                windowArrangeMode: parsed.globalSettings.windowArrangeMode ?? def.windowArrangeMode,
                backupDir: parsed.globalSettings.backupDir ?? def.backupDir,
                sortBy: parsed.globalSettings.sortBy ?? def.sortBy,
                viewMode: parsed.globalSettings.viewMode ?? def.viewMode,
              };
            }
            return;
          }
        }
      }
    } catch (_e) {
    }
    this.instances = [this.createDefaultPrimary(filesDir)];
    this.nextIndex = 1;
    this.settings = defaultGlobalSettings();
    this.persist();
  }

  private persist(): void {
    if (this.filePath.length === 0) return;
    try {
      const payload: PersistedStateV2 = {
        version: 2,
        nextIndex: this.nextIndex,
        instances: this.instances,
        globalSettings: this.settings,
      };
      const f: fileIo.File = fileIo.openSync(
        this.filePath,
        fileIo.OpenMode.CREATE | fileIo.OpenMode.TRUNC | fileIo.OpenMode.WRITE_ONLY
      );
      fileIo.writeSync(f.fd, JSON.stringify(payload));
      fileIo.closeSync(f);
    } catch (_e) {
    }
  }

  public getAll(filesDir: string): DeviceInstance[] {
    this.ensureLoaded(filesDir);
    const out: DeviceInstance[] = [];
    for (let i: number = 0; i < this.instances.length; i++) {
      out.push(cloneInstanceRecord(this.instances[i]));
    }
    return out;
  }

  public getSorted(filesDir: string, sortBy: string): DeviceInstance[] {
    const list: DeviceInstance[] = this.getAll(filesDir);
    list.sort((a: DeviceInstance, b: DeviceInstance): number => {
      if (sortBy === 'name') {
        return a.name.localeCompare(b.name);
      }
      if (sortBy === 'time') {
        return b.createdAt - a.createdAt;
      }
      return a.index - b.index;
    });
    return list;
  }

  public getById(filesDir: string, id: string): DeviceInstance | undefined {
    this.ensureLoaded(filesDir);
    for (let i: number = 0; i < this.instances.length; i++) {
      if (this.instances[i].id === id) {
        return cloneInstanceRecord(this.instances[i]);
      }
    }
    return undefined;
  }

  public createInstances(filesDir: string, opts: CreateInstanceOptions): DeviceInstance[] {
    this.ensureLoaded(filesDir);
    const created: DeviceInstance[] = [];
    const total: number = Math.max(1, Math.min(5, opts.count));
    const isTablet: boolean = opts.deviceType === 'tablet';
    const resKey: string = isTablet ? '1600x900' : '720x1280';
    const w: number = isTablet ? 1600 : 720;
    const h: number = isTablet ? 900 : 1280;
    const dpi: number = isTablet ? 280 : 320;
    let profile: string = 'Balanced';
    let cpuCores: number = 4;
    let memMb: number = 4096;
    if (opts.scenario === 'extreme') {
      profile = 'Eco';
      cpuCores = 2;
      memMb = 2048;
    } else if (opts.scenario === 'performance') {
      profile = 'Performance';
      cpuCores = 8;
      memMb = 6144;
    }
    const baseName: string = opts.namePrefix.trim().length > 0
      ? opts.namePrefix.trim()
      : (isTablet ? 'Android 14 · 平板设备' : 'Android 14 · 手机设备');

    for (let i: number = 0; i < total; i++) {
      const idx: number = this.nextIndex;
      this.nextIndex += 1;
      const now: number = Date.now() + i;
      const id: string = `inst_${now}_${idx}`;
      const inst: DeviceInstance = {
        id: id,
        index: idx,
        name: total > 1 ? `${baseName} #${idx}` : `${baseName} ${idx}`,
        deviceType: opts.deviceType,
        androidVersion: opts.androidVersion,
        dataSubdir: `instances/${id}`,
        runMode: opts.runMode,
        resolutionKey: resKey,
        width: w,
        height: h,
        dpi: dpi,
        refreshRate: opts.refreshRate,
        gpuMode: opts.gpuMode,
        profile: profile,
        cpuCores: cpuCores,
        memLimitMb: memMb,
        scenario: opts.scenario,
        compactDisk: opts.compactDisk,
        enableAudio: true,
        enableRoot: true,
        keepAlive: true,
        preventSleep: false,
        autoGamepad: true,
        deviceBrand: '',
        deviceModel: '',
        adbPort: 5555,
        createdAt: now,
        lastRunAt: 0,
      };
      this.instances.push(inst);
      created.push(cloneInstanceRecord(inst));
    }
    this.persist();
    return created;
  }

  public addClonedInstance(filesDir: string, src: DeviceInstance): DeviceInstance {
    this.ensureLoaded(filesDir);
    const idx: number = this.nextIndex;
    this.nextIndex += 1;
    const now: number = Date.now();
    const id: string = `inst_${now}_${idx}`;
    const copy: DeviceInstance = cloneInstanceRecord(src);
    copy.id = id;
    copy.index = idx;
    copy.name = `${src.name} 副本`;
    copy.dataSubdir = `instances/${id}`;
    copy.createdAt = now;
    copy.lastRunAt = 0;
    this.instances.push(copy);
    this.persist();
    return cloneInstanceRecord(copy);
  }

  public updateInstance(filesDir: string, updated: DeviceInstance): void {
    this.ensureLoaded(filesDir);
    for (let i: number = 0; i < this.instances.length; i++) {
      if (this.instances[i].id === updated.id) {
        this.instances[i] = cloneInstanceRecord(updated);
        if (updated.id === 'inst_0') {
          instanceConfigStore.save(filesDir, {
            runMode: updated.runMode,
            resolutionKey: updated.resolutionKey,
            refreshRate: updated.refreshRate,
            gpuMode: updated.gpuMode,
            profile: updated.profile,
          });
        }
        this.persist();
        return;
      }
    }
  }

  public renameInstance(filesDir: string, id: string, newName: string): void {
    const trimmed: string = newName.trim();
    if (trimmed.length === 0) return;
    this.ensureLoaded(filesDir);
    for (let i: number = 0; i < this.instances.length; i++) {
      if (this.instances[i].id === id) {
        this.instances[i].name = trimmed.substring(0, 40);
        this.persist();
        return;
      }
    }
  }

  public markLastRun(filesDir: string, id: string): void {
    this.ensureLoaded(filesDir);
    for (let i: number = 0; i < this.instances.length; i++) {
      if (this.instances[i].id === id) {
        this.instances[i].lastRunAt = Date.now();
        this.persist();
        return;
      }
    }
  }

  public deleteInstance(filesDir: string, id: string): boolean {
    this.ensureLoaded(filesDir);
    if (this.instances.length <= 1) {
      return false;
    }
    const next: DeviceInstance[] = [];
    let removed: boolean = false;
    for (let i: number = 0; i < this.instances.length; i++) {
      if (this.instances[i].id === id) {
        removed = true;
      } else {
        next.push(this.instances[i]);
      }
    }
    if (removed) {
      this.instances = next;
      this.persist();
    }
    return removed;
  }

  public getGlobalSettings(filesDir: string): GlobalSettings {
    this.ensureLoaded(filesDir);
    return {
      autoBootDefault: this.settings.autoBootDefault,
      batchStartIntervalSec: this.settings.batchStartIntervalSec,
      windowArrangeMode: this.settings.windowArrangeMode,
      backupDir: this.settings.backupDir,
      sortBy: this.settings.sortBy,
      viewMode: this.settings.viewMode,
    };
  }

  public saveGlobalSettings(filesDir: string, next: GlobalSettings): void {
    this.ensureLoaded(filesDir);
    this.settings = {
      autoBootDefault: next.autoBootDefault,
      batchStartIntervalSec: next.batchStartIntervalSec,
      windowArrangeMode: next.windowArrangeMode,
      backupDir: next.backupDir,
      sortBy: next.sortBy,
      viewMode: next.viewMode,
    };
    this.persist();
  }
}

export const instanceManager: InstanceManagerStore = new InstanceManagerStore();
