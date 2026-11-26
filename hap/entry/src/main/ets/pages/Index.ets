import { common, Want } from '@kit.AbilityKit';
import { fileIo, picker } from '@kit.CoreFileKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { LengthUnit } from '@kit.ArkUI';
import native, { RuntimeStatus } from 'libentry.so';
import {
  AndroidAppItem,
  AppWindowPref,
  appWindowPrefs,
  computeShortcutId,
  findPresetByKey,
  getCachedIconsDir,
  loadCachedAppList,
  ResolutionPresetItem,
  saveCachedAppList
} from '../common/AppWindowPrefs';
import {
  pinAndroidAppToDesktop,
  queryPinnedShortcutIds,
  removeAndroidAppShortcut,
  ShortcutOpResult
} from '../common/DesktopShortcut';
import {
  ControlSize,
  FontSize,
  IconSize,
  Motion,
  Radius,
  Spacing
} from '../common/DesignTokens';
import { arrangeOpenWindows } from '../common/WindowHelper';
import { applyAndroidClipboardToHost } from '../common/InputBridgeHelper';
import {
  getDeviceFormLabel,
  isMobileDevice,
  supportsMultiInstance
} from '../common/DeviceProfile';
import {
  CreateInstanceOptions,
  defaultGlobalSettings,
  DeviceInstance,
  formatGpuModeLabel,
  GlobalSettings,
  instanceManager
} from '../model/InstanceModel';
import { CreateInstanceSheet } from '../components/CreateInstanceDialog';
import { DeviceSettingsSheet } from '../components/DeviceSettingsDialog';
import { RenameDialog } from '../components/RenameDialog';
import { BackupRestoreSheet } from '../components/BackupRestoreDialog';
import { GlobalSettingsSheet } from '../components/GlobalSettingsDialog';

const TAG: string = 'SvboxMain';
const NAV_RAIL_ITEM_H: number = 58;
const NAV_RAIL_ITEM_GAP: number = 8;

interface ListAppsReply {
  ok?: boolean;
  apps?: AndroidAppItem[];
}

@Entry
@Component
struct Index {
  @StorageLink('mainDecorRightAvoidVp') decorRightAvoidVp: number = 148;
  @StorageLink('androidApps') appList: AndroidAppItem[] = [];

  // Navigation & View Mode
  @State private activeNav: string = 'devices'; // 'devices' | 'apps'
  @State private instances: DeviceInstance[] = [];
  @State private globalSettings: GlobalSettings = defaultGlobalSettings();
  @State private sortBy: string = 'index';      // 'index' | 'time' | 'name'
  @State private viewMode: string = 'card';     // 'card' | 'compact'

  // Responsive Breakpoint ('sm' < 600vp Phone, 'md' 600..839vp Foldable/Tablet, 'lg' >= 840vp PC)
  @State private winWidthVp: number = 1040;
  @State private bp: string = 'lg';

  // Hover & Batch State
  @State private hoveredInstId: string = '';
  @State private hoveredAppPkg: string = '';
  @State private batchMode: boolean = false;
  @State private selectedIds: string[] = [];

  // Half-Modal Sheet Visibility & Context
  @State private showCreateSheet: boolean = false;
  @State private showDeviceSettingsSheet: boolean = false;
  @State private showBackupSheet: boolean = false;
  @State private showGlobalSettingsSheet: boolean = false;
  @State private editingInstance: DeviceInstance | undefined = undefined;

  // Live Runtime State
  @State private running: boolean = false;
  @State private paused: boolean = false;
  @State private wmAgentReady: boolean = false;
  @State private installingApk: boolean = false;
  @State private activeInstanceId: string = 'inst_0';
  @State private refreshRate: number = 120;
  @State private fps: number = 0;
  @State private cpuPercent: number = 0;
  @State private memMb: number = 0;
  @State private hostMemMb: number = 0;
  @State private gfxMemMb: number = 0;
  @State private memLimitMb: number = 4096;
  @State private activeProcs: number = 0;
  @State private gpuRenderer: string = '';
  @State private statusText: string = '就绪';
  @State private cloningBusy: boolean = false;

  // Integrated Apps Center State
  @State private iconsDir: string = '';
  @State private loadingApps: boolean = false;
  @State private searchQuery: string = '';
  @State private appFilter: string = 'all';
  @State private pinnedIds: string[] = [];
  @State private prefEpoch: number = 0;

  // Lightweight Rename Dialog Controller
  private renameDialogCtrl: CustomDialogController | null = null;

  private pollTimer: number = -1;
  private lastLaunchSeq: number = 0;
  private appsFetchedOnce: boolean = false;
  private svboxPath: string = '/storage/Users/currentUser/Workspace/svbox/build/svbox';
  private androidDir: string = '/storage/Users/currentUser/Workspace/svbox-Android';

  private runAnimated(curve: ICurve, duration: number, fn: () => void): void {
    try {
      this.getUIContext().animateTo({ duration: duration, curve: curve }, fn);
    } catch (_e) {
      fn();
    }
  }

  private showToast(msg: string): void {
    try {
      this.getUIContext().getPromptAction().showToast({ message: msg, duration: 2200 });
    } catch (_e) {
    }
  }

  private getSandboxTempDir(): string {
    const dir: string | undefined = AppStorage.get<string>('sandboxTempDir');
    return dir && dir.length > 0 ? dir : '/data/storage/el2/base/haps/entry/temp';
  }

  private getSandboxFilesDir(): string {
    const dir: string | undefined = AppStorage.get<string>('sandboxFilesDir');
    return dir && dir.length > 0 ? dir : '/data/storage/el2/base/haps/entry/files';
  }

  private reloadInstances(animated: boolean = false): void {
    const filesDir: string = this.getSandboxFilesDir();
    const allSorted: DeviceInstance[] = instanceManager.getSorted(filesDir, this.sortBy);
    // Mobile devices run strictly in single-container mode (primary inst_0 only)
    const nextList: DeviceInstance[] = supportsMultiInstance()
      ? allSorted
      : (allSorted.length > 0 ? [allSorted[0]] : []);
    if (animated) {
      this.runAnimated(Motion.SPRING_SMOOTH, Motion.DUR_MEDIUM, () => {
        this.instances = nextList;
      });
    } else {
      this.instances = nextList;
    }
  }

  private getInstanceById(id: string): DeviceInstance | undefined {
    for (let i: number = 0; i < this.instances.length; i++) {
      if (this.instances[i].id === id) return this.instances[i];
    }
    return undefined;
  }

  private isInstanceRunning(inst: DeviceInstance): boolean {
    return this.running && this.activeInstanceId === inst.id;
  }

  private isSelected(id: string): boolean {
    return this.selectedIds.indexOf(id) >= 0;
  }

  private toggleSelection(id: string): void {
    const idx: number = this.selectedIds.indexOf(id);
    if (idx >= 0) {
      const next: string[] = [];
      for (let i: number = 0; i < this.selectedIds.length; i++) {
        if (this.selectedIds[i] !== id) next.push(this.selectedIds[i]);
      }
      this.selectedIds = next;
    } else {
      const next: string[] = this.selectedIds.slice();
      next.push(id);
      this.selectedIds = next;
    }
  }

  private toggleSelectAll(): void {
    if (this.selectedIds.length === this.instances.length) {
      this.selectedIds = [];
    } else {
      const all: string[] = [];
      for (let i: number = 0; i < this.instances.length; i++) {
        all.push(this.instances[i].id);
      }
      this.selectedIds = all;
    }
  }

  aboutToAppear(): void {
    const filesDir: string = this.getSandboxFilesDir();
    const gs: GlobalSettings = instanceManager.getGlobalSettings(filesDir);
    this.globalSettings = gs;
    this.sortBy = gs.sortBy;
    this.viewMode = gs.viewMode;
    this.reloadInstances(false);

    this.iconsDir = native.getIconsDir();
    this.lastLaunchSeq = AppStorage.get<number>('launchSeq') ?? 0;
    this.refreshPinnedShortcuts();

    const primary: DeviceInstance | undefined = this.getInstanceById('inst_0') ?? this.instances[0];
    this.loadOfflineAppsForInstance(primary);
    if (primary) {
      let dirty: boolean = false;
      const rawGpu: string | undefined = AppStorage.get<string>('gpuMode');
      if (rawGpu === 'swiftshader' || rawGpu === 'gles' || rawGpu === 'vulkan') {
        primary.gpuMode = rawGpu;
        dirty = true;
      }
      const rawRes: string | undefined = AppStorage.get<string>('resolution');
      if (rawRes && rawRes.length > 0) {
        const p: ResolutionPresetItem = findPresetByKey(rawRes);
        primary.resolutionKey = p.key;
        primary.width = p.w;
        primary.height = p.h;
        dirty = true;
      }
      const rawHz: number | undefined = AppStorage.get<number>('refreshRate');
      if (rawHz === 60 || rawHz === 90 || rawHz === 120) {
        primary.refreshRate = rawHz;
        dirty = true;
      }
      const reqUi: string | undefined = AppStorage.get<string>('requestedUiMode');
      if (reqUi === 'emulator' || reqUi === 'multiwindow') {
        primary.runMode = reqUi;
        dirty = true;
      }
      const reqRoot: string | undefined = AppStorage.get<string>('requestedEnableRoot');
      if (reqRoot === 'true' || reqRoot === 'false') {
        primary.enableRoot = (reqRoot === 'true');
        AppStorage.setOrCreate<string>('requestedEnableRoot', '');
        dirty = true;
      }
      const reqSyscall: string | undefined = AppStorage.get<string>('requestedSyscallMode');
      if (reqSyscall === 'adaptive' || reqSyscall === 'pure_seccomp' ||
          reqSyscall === 'hybrid' || reqSyscall === 'inline') {
        primary.syscallMode = reqSyscall;
        AppStorage.setOrCreate<string>('requestedSyscallMode', '');
        dirty = true;
      }
      const reqDiskGb: number | undefined = AppStorage.get<number>('requestedDiskLimitGb');
      if (reqDiskGb === 8 || reqDiskGb === 16 || reqDiskGb === 32 || reqDiskGb === 64 || reqDiskGb === 128) {
        primary.diskLimitGb = reqDiskGb;
        AppStorage.setOrCreate<number>('requestedDiskLimitGb', 0);
        dirty = true;
      }
      const reqGms: string | undefined = AppStorage.get<string>('requestedEnableGms');
      if (reqGms === 'true' || reqGms === 'false') {
        primary.enableGms = (reqGms === 'true');
        AppStorage.setOrCreate<string>('requestedEnableGms', '');
        dirty = true;
      }
      if (dirty) {
        instanceManager.updateInstance(filesDir, primary);
        this.reloadInstances(false);
      }
      AppStorage.setOrCreate<number>('refreshRate', primary.refreshRate);

      // On mobile devices, land directly on the one-tap offline app grid (single-container freeform mode)
      if (isMobileDevice() && !AppStorage.get<string>('autoStart')) {
        this.activeNav = 'apps';
      }

      const autoStart: string | undefined = AppStorage.get<string>('autoStart');
      if (autoStart === 'bootanim' || autoStart === 'full') {
        setTimeout(() => {
          this.startDeviceInstance(primary, autoStart);
        }, 260);
      } else if (this.globalSettings.autoBootDefault) {
        setTimeout(() => {
          const cur: RuntimeStatus = native.getStatus();
          if (!cur.running) {
            this.startDeviceInstance(primary, 'full');
          }
        }, 320);
      }
    }

    this.pollTimer = setInterval(() => {
      this.pollRuntime();
    }, 500);
  }

  aboutToDisappear(): void {
    if (this.pollTimer !== -1) {
      clearInterval(this.pollTimer);
      this.pollTimer = -1;
    }
  }

  private pollRuntime(): void {
    try {
      const reqUi: string | undefined = AppStorage.get<string>('requestedUiMode');
      if (reqUi === 'emulator') {
        AppStorage.setOrCreate<string>('requestedUiMode', '');
        const act: DeviceInstance | undefined = this.getInstanceById(this.activeInstanceId) ?? this.instances[0];
        if (act) this.openEmulatorWindow(act);
      } else if (reqUi === 'multiwindow') {
        AppStorage.setOrCreate<string>('requestedUiMode', '');
      }

      const seq: number = AppStorage.get<number>('launchSeq') ?? 0;
      if (seq > this.lastLaunchSeq) {
        this.lastLaunchSeq = seq;
        const target: DeviceInstance | undefined = this.getInstanceById(this.activeInstanceId) ?? this.instances[0];
        if (target) {
          const reqGpu: string | undefined = AppStorage.get<string>('gpuMode');
          if (reqGpu === 'swiftshader' || reqGpu === 'gles' || reqGpu === 'vulkan') {
            target.gpuMode = reqGpu;
            AppStorage.setOrCreate<string>('gpuMode', '');
          }
          const reqRes: string | undefined = AppStorage.get<string>('resolution');
          if (reqRes && reqRes.length > 0) {
            const p: ResolutionPresetItem = findPresetByKey(reqRes);
            target.resolutionKey = p.key;
            target.width = p.w;
            target.height = p.h;
            AppStorage.setOrCreate<string>('resolution', '');
          }
          const reqHz: number | undefined = AppStorage.get<number>('requestedRefreshRate');
          if (reqHz === 60 || reqHz === 90 || reqHz === 120) {
            target.refreshRate = reqHz;
            AppStorage.setOrCreate<number>('requestedRefreshRate', 0);
          }
          const reqSyscall: string | undefined = AppStorage.get<string>('requestedSyscallMode');
          if (reqSyscall === 'adaptive' || reqSyscall === 'pure_seccomp' ||
              reqSyscall === 'hybrid' || reqSyscall === 'inline') {
            target.syscallMode = reqSyscall;
            AppStorage.setOrCreate<string>('requestedSyscallMode', '');
          }
          const reqDiskGb: number | undefined = AppStorage.get<number>('requestedDiskLimitGb');
          if (reqDiskGb === 8 || reqDiskGb === 16 || reqDiskGb === 32 || reqDiskGb === 64 || reqDiskGb === 128) {
            target.diskLimitGb = reqDiskGb;
            AppStorage.setOrCreate<number>('requestedDiskLimitGb', 0);
          }
          const reqGms: string | undefined = AppStorage.get<string>('requestedEnableGms');
          if (reqGms === 'true' || reqGms === 'false') {
            target.enableGms = (reqGms === 'true');
            AppStorage.setOrCreate<string>('requestedEnableGms', '');
          }
          instanceManager.updateInstance(this.getSandboxFilesDir(), target);
          this.reloadInstances(false);

          const mode: string | undefined = AppStorage.get<string>('autoStart');
          if (mode === 'bootanim' || mode === 'full') {
            this.startDeviceInstance(target, mode);
          } else if (mode === 'restart') {
            native.stopContainer();
            this.running = false;
            setTimeout(() => {
              this.startDeviceInstance(target, 'full');
            }, 320);
          }
        }
      }

      const st: RuntimeStatus = native.getStatus();
      const runningChanged: boolean = st.running !== this.running;
      if (runningChanged) {
        this.runAnimated(Motion.SPRING_SMOOTH, Motion.DUR_SHORT, () => {
          this.running = st.running;
          this.paused = st.paused;
          this.wmAgentReady = st.wmAgentReady;
        });
      } else {
        this.running = st.running;
        this.paused = st.paused;
        this.wmAgentReady = st.wmAgentReady;
      }
      this.fps = st.fps;
      if (st.running && st.activeInstanceId && st.activeInstanceId.length > 0) {
        this.activeInstanceId = st.activeInstanceId;
      }
      if (st.running && (st.refreshRate === 60 || st.refreshRate === 90 || st.refreshRate === 120)) {
        this.refreshRate = st.refreshRate;
        AppStorage.setOrCreate<number>('refreshRate', this.refreshRate);
      }
      this.cpuPercent = st.cpuPercent;
      this.memMb = st.memMb;
      this.hostMemMb = st.hostMemMb ?? 0;
      this.gfxMemMb = st.gfxMemMb ?? 0;
      this.memLimitMb = st.memLimitMb;
      this.activeProcs = st.activeProcs;
      this.gpuRenderer = st.gpuRenderer;
      this.statusText = st.statusText;

      if (this.wmAgentReady && !this.appsFetchedOnce) {
        this.appsFetchedOnce = true;
        this.refreshApps(true);
      }
      if (!this.running) {
        this.appsFetchedOnce = false;
      }
      if (this.wmAgentReady) {
        this.drainWmEvents();
      }
    } catch (_e) {
    }
  }

  private loadOfflineAppsForInstance(inst: DeviceInstance | undefined): void {
    const filesDir: string = this.getSandboxFilesDir();
    const subdir: string = inst ? inst.dataSubdir : 'userdata14';
    const cachedDir: string = getCachedIconsDir(filesDir, subdir);
    if (this.iconsDir.length === 0) {
      this.iconsDir = cachedDir;
    }
    const cached: AndroidAppItem[] = loadCachedAppList(filesDir, subdir);
    if (cached.length > 0) {
      this.appList = cached;
      AppStorage.setOrCreate<AndroidAppItem[]>('androidApps', cached);
    }
  }

  private async refreshApps(forceExtractIcons: boolean): Promise<void> {
    if (!this.wmAgentReady || this.loadingApps) return;
    this.loadingApps = true;
    try {
      const filesDir: string = this.getSandboxFilesDir();
      const activeInst: DeviceInstance | undefined =
        this.getInstanceById(this.activeInstanceId) ?? this.getInstanceById('inst_0') ?? this.instances[0];
      const runtimeIcons: string = native.getIconsDir();
      this.iconsDir = runtimeIcons.length > 0
        ? runtimeIcons
        : getCachedIconsDir(filesDir, activeInst ? activeInst.dataSubdir : 'userdata14');
      const raw: string = await native.wmRpcAsync(forceExtractIcons ? 'LIST_APPS 1' : 'LIST_APPS 0');
      const rep: ListAppsReply = JSON.parse(raw) as ListAppsReply;
      if (rep.ok && rep.apps) {
        this.appList = rep.apps;
        AppStorage.setOrCreate<AndroidAppItem[]>('androidApps', rep.apps);
        saveCachedAppList(filesDir, activeInst ? activeInst.dataSubdir : 'userdata14', rep.apps);
      }
    } catch (err) {
      hilog.warn(0x0000, TAG, 'refreshApps error: %{public}s', String(err));
    } finally {
      this.loadingApps = false;
    }
  }

  private async pickAndInstallOfflineApk(): Promise<void> {
    if (this.installingApk) return;
    if (!this.running) {
      this.showToast('请先启动目标 Android 设备，再安装本地 APK');
      return;
    }
    const activeInst: DeviceInstance | undefined =
      this.getInstanceById(this.activeInstanceId) ?? this.getInstanceById('inst_0') ?? this.instances[0];
    const filesDir: string = this.getSandboxFilesDir();
    const subdir: string = activeInst ? activeInst.dataSubdir : 'userdata14';

    try {
      const ctx: common.UIAbilityContext = this.getUIContext().getHostContext() as common.UIAbilityContext;
      const docPicker: picker.DocumentViewPicker = new picker.DocumentViewPicker(ctx);
      const opts: picker.DocumentSelectOptions = new picker.DocumentSelectOptions();
      opts.maxSelectNumber = 1;
      opts.fileSuffixFilters = ['Android 安装包|.apk'];
      const uris: string[] = await docPicker.select(opts);
      if (!uris || uris.length === 0 || !uris[0]) {
        return;
      }
      const srcUri: string = uris[0];
      this.installingApk = true;
      this.showToast('正在拷贝并安装本地 APK...');

      const tmpDir: string = `${filesDir}/${subdir}/data/local/tmp`;
      try {
        fileIo.mkdirSync(tmpDir, true);
      } catch (_mk) {
      }
      const stageName: string = `_offline_${Date.now()}.apk`;
      const hostStagePath: string = `${tmpDir}/${stageName}`;
      const guestStagePath: string = `/data/local/tmp/${stageName}`;

      const inFile: fileIo.File = fileIo.openSync(srcUri, fileIo.OpenMode.READ_ONLY);
      const outFile: fileIo.File = fileIo.openSync(
        hostStagePath,
        fileIo.OpenMode.CREATE | fileIo.OpenMode.TRUNC | fileIo.OpenMode.WRITE_ONLY
      );
      try {
        const chunk: ArrayBuffer = new ArrayBuffer(256 * 1024);
        let nread: number = fileIo.readSync(inFile.fd, chunk);
        while (nread > 0) {
          fileIo.writeSync(outFile.fd, chunk, { length: nread });
          nread = fileIo.readSync(inFile.fd, chunk);
        }
      } finally {
        fileIo.closeSync(outFile);
        fileIo.closeSync(inFile);
      }

      const res: string = await native.installLocalApkAsync(guestStagePath);
      if (res === 'OK') {
        this.showToast('离线 APK 安装成功，正在刷新应用列表...');
        if (this.wmAgentReady) {
          await this.refreshApps(true);
        }
      } else {
        this.showToast(`安装失败：${res}`);
      }
    } catch (err) {
      hilog.error(0x0000, TAG, 'pickAndInstallOfflineApk error: %{public}s', String(err));
      this.showToast(`离线安装异常：${String(err)}`);
    } finally {
      this.installingApk = false;
    }
  }

  private async refreshPinnedShortcuts(): Promise<void> {
    try {
      this.pinnedIds = await queryPinnedShortcutIds();
    } catch (_e) {
    }
  }

  private drainWmEvents(): void {
    try {
      const raw: string = native.pollWmEvents();
      if (!raw || raw === '[]') return;
      const items: Record<string, Object>[] = JSON.parse(raw) as Record<string, Object>[];
      for (let i: number = 0; i < items.length; i++) {
        const ev: Record<string, Object> = items[i];
        const evName: string = String(ev['event'] ?? '');
        const pkg: string = String(ev['packageName'] ?? '');
        if (evName === 'CROSS_APP_LAUNCH' && pkg.length > 0) {
          this.openAndroidAppWindow(
            pkg,
            String(ev['component'] ?? ''),
            String(ev['label'] ?? pkg),
            Number(ev['taskId'] ?? -1)
          );
        } else if (evName === 'CLIPBOARD_CHANGED') {
          const b64: string = String(ev['b64'] ?? '');
          if (b64.length > 0) {
            applyAndroidClipboardToHost(b64);
          }
        }
      }
    } catch (_e) {
    }
  }

  private async openAndroidAppWindow(pkg: string, comp: string, label: string, moveTaskId: number): Promise<void> {
    try {
      // Single-container multi-window: clicking any offline cached app auto-boots the primary container silently.
      if (!this.running) {
        const primary: DeviceInstance | undefined =
          this.getInstanceById(this.activeInstanceId) ?? this.getInstanceById('inst_0') ?? this.instances[0];
        if (primary) {
          this.activeInstanceId = primary.id;
          AppStorage.setOrCreate<number>('refreshRate', primary.refreshRate);
          native.setInstanceConfig(
            primary.id, primary.dataSubdir, primary.dpi, primary.enableAudio,
            primary.enableRoot, primary.deviceBrand, primary.deviceModel, primary.adbPort, primary.enableGms
          );
          native.setResourceConfig(
            primary.profile, '', primary.memLimitMb, primary.diskLimitGb, primary.cpuCores
          );
          native.setGpuMode(primary.gpuMode);
          native.setSyscallMode(primary.syscallMode || 'adaptive');
          native.setRefreshRate(primary.refreshRate);
          this.statusText = native.startContainer(
            this.getSandboxTempDir(),
            this.svboxPath,
            this.androidDir,
            'full',
            primary.width,
            primary.height
          );
          native.setDisplayVisible(0, false);
          this.running = true;
          instanceManager.markLastRun(this.getSandboxFilesDir(), primary.id);
        }
      }
      const ctx: common.UIAbilityContext = this.getUIContext().getHostContext() as common.UIAbilityContext;
      const want: Want = {
        bundleName: 'org.svbox.android',
        abilityName: 'AppWindowAbility',
        parameters: {
          'androidPkg': pkg,
          'androidComp': comp,
          'androidLabel': label.length > 0 ? label : pkg,
          'moveTaskId': String(moveTaskId),
        },
      };
      await ctx.startAbility(want);
    } catch (err) {
      hilog.error(0x0000, TAG, 'openAndroidAppWindow failed: %{public}s', String(err));
      this.showToast(`打开应用失败：${String(err)}`);
    }
  }

  private async openEmulatorWindow(inst: DeviceInstance): Promise<void> {
    try {
      const ctx: common.UIAbilityContext = this.getUIContext().getHostContext() as common.UIAbilityContext;
      const want: Want = {
        bundleName: 'org.svbox.android',
        abilityName: 'EmulatorWindowAbility',
        parameters: {
          'resolutionKey': inst.resolutionKey,
          'instanceName': inst.name,
          'instanceId': inst.id,
        },
      };
      await ctx.startAbility(want);
    } catch (err) {
      hilog.error(0x0000, TAG, 'openEmulatorWindow failed: %{public}s', String(err));
      this.showToast(`打开模拟器窗口失败：${String(err)}`);
    }
  }

  private startDeviceInstance(inst: DeviceInstance, bootMode: string): void {
    if (this.running && this.activeInstanceId !== inst.id) {
      const runningInst: DeviceInstance | undefined = this.getInstanceById(this.activeInstanceId);
      const curName: string = runningInst ? runningInst.name : '当前设备';
      this.getUIContext().showAlertDialog({
        title: '切换运行设备',
        message: `「${curName}」正在运行中。停止当前设备并启动「${inst.name}」吗？`,
        primaryButton: { value: '取消', action: () => {} },
        secondaryButton: {
          value: '切换并启动',
          action: () => {
            native.stopContainer();
            this.running = false;
            this.appsFetchedOnce = false;
            this.appList = [];
            AppStorage.setOrCreate<AndroidAppItem[]>('androidApps', []);
            setTimeout(() => {
              this.doStartDeviceInstance(inst, bootMode);
            }, 350);
          }
        }
      });
      return;
    }
    this.doStartDeviceInstance(inst, bootMode);
  }

  private doStartDeviceInstance(inst: DeviceInstance, bootMode: string): void {
    this.activeInstanceId = inst.id;
    AppStorage.setOrCreate<number>('refreshRate', inst.refreshRate);
    try {
      native.setInstanceConfig(
        inst.id,
        inst.dataSubdir,
        inst.dpi,
        inst.enableAudio,
        inst.enableRoot,
        inst.deviceBrand,
        inst.deviceModel,
        inst.adbPort,
        inst.enableGms
      );
      native.setResourceConfig(inst.profile, '', inst.memLimitMb, inst.diskLimitGb, inst.cpuCores);
      native.setGpuMode(inst.gpuMode);
      native.setSyscallMode(inst.syscallMode || 'adaptive');
      native.setRefreshRate(inst.refreshRate);
      this.statusText = native.startContainer(
        this.getSandboxTempDir(),
        this.svboxPath,
        this.androidDir,
        bootMode,
        inst.width,
        inst.height
      );
      hilog.info(0x0000, TAG, 'startContainer(%{public}s, %{public}s) -> %{public}s',
        inst.id, inst.dataSubdir, this.statusText);
      instanceManager.markLastRun(this.getSandboxFilesDir(), inst.id);
      if (inst.runMode === 'emulator') {
        native.setDisplayVisible(0, true);
        this.openEmulatorWindow(inst);
      } else {
        native.setDisplayVisible(0, false);
        this.showToast(`已启动「${inst.name}」`);
      }
    } catch (e) {
      hilog.error(0x0000, TAG, 'doStartDeviceInstance error: %{public}s', String(e));
      this.showToast(`启动失败：${String(e)}`);
    }
  }

  private stopActiveInstance(): void {
    try {
      this.statusText = native.stopContainer();
      this.runAnimated(Motion.SPRING_SMOOTH, Motion.DUR_SHORT, () => {
        this.running = false;
        this.paused = false;
        this.wmAgentReady = false;
      });
      this.appsFetchedOnce = false;
      const activeInst: DeviceInstance | undefined =
        this.getInstanceById(this.activeInstanceId) ?? this.getInstanceById('inst_0') ?? this.instances[0];
      this.loadOfflineAppsForInstance(activeInst);
      this.showToast('设备已关闭');
    } catch (e) {
      this.showToast(`停止失败：${String(e)}`);
    }
  }

  // --- Half-Modal Sheet Openers ---
  private openCreateSheet(): void {
    this.showCreateSheet = true;
  }

  private openDeviceSettingsSheet(inst: DeviceInstance): void {
    this.editingInstance = inst;
    this.showDeviceSettingsSheet = true;
  }

  private openBackupSheet(inst: DeviceInstance): void {
    this.editingInstance = inst;
    this.showBackupSheet = true;
  }

  private openGlobalSettingsSheet(): void {
    this.showGlobalSettingsSheet = true;
  }

  private openRenameDialog(inst: DeviceInstance): void {
    this.renameDialogCtrl = new CustomDialogController({
      builder: RenameDialog({
        initialName: inst.name,
        onConfirm: (newName: string) => {
          instanceManager.renameInstance(this.getSandboxFilesDir(), inst.id, newName);
          this.reloadInstances(true);
          this.showToast(`已重命名为「${newName}」`);
        }
      }),
      alignment: DialogAlignment.Center,
      autoCancel: true,
      customStyle: true
    });
    this.renameDialogCtrl.open();
  }

  private async cloneDeviceInstance(inst: DeviceInstance): Promise<void> {
    if (this.cloningBusy) return;
    this.cloningBusy = true;
    this.showToast(`正在复制「${inst.name}」的数据分区...`);
    const filesDir: string = this.getSandboxFilesDir();
    const copy: DeviceInstance = instanceManager.addClonedInstance(filesDir, inst);
    try {
      const ok: boolean = await native.cloneInstanceAsync(filesDir, inst.dataSubdir, copy.dataSubdir);
      this.reloadInstances(true);
      this.showToast(ok ? `已复制生成「${copy.name}」` : `已创建「${copy.name}」（空数据）`);
    } catch (e) {
      this.reloadInstances(true);
      this.showToast(`复制完成但有警告：${String(e)}`);
    } finally {
      this.cloningBusy = false;
    }
  }

  private cleanDeviceDisk(inst: DeviceInstance): void {
    if (this.isInstanceRunning(inst)) {
      this.showToast('请先停止该设备后再执行磁盘清理');
      return;
    }
    try {
      const freed: number = native.cleanInstanceCache(this.getSandboxFilesDir(), inst.dataSubdir);
      if (freed < 0) {
        this.showToast('请先停止该设备后再执行磁盘清理');
        return;
      }
      this.showToast(`「${inst.name}」清理完成，释放 ${freed.toFixed(1)} MB`);
    } catch (e) {
      this.showToast(`清理失败：${String(e)}`);
    }
  }

  private confirmDeleteDevice(inst: DeviceInstance): void {
    if (this.instances.length <= 1) {
      this.showToast('至少需要保留一台设备');
      return;
    }
    if (this.isInstanceRunning(inst)) {
      this.showToast('请先停止该设备再删除');
      return;
    }
    this.getUIContext().showAlertDialog({
      title: '删除设备确认',
      message: `确定要删除「${inst.name}」及其独立数据目录吗？此操作不可恢复。`,
      primaryButton: { value: '取消', action: () => {} },
      secondaryButton: {
        value: '确认删除',
        action: () => {
          const filesDir: string = this.getSandboxFilesDir();
          native.deleteInstanceData(filesDir, inst.dataSubdir);
          instanceManager.deleteInstance(filesDir, inst.id);
          this.reloadInstances(true);
          this.showToast(`已删除「${inst.name}」`);
        }
      }
    });
  }

  private async handleArrangeWindows(): Promise<void> {
    const count: number = await arrangeOpenWindows(this.globalSettings.windowArrangeMode);
    if (count > 0) {
      this.showToast(`已排列 ${count} 个活跃窗口`);
    } else {
      this.showToast('当前没有已打开的模拟器或应用窗口');
    }
  }

  private updateSort(nextSort: string): void {
    if (this.sortBy === nextSort) return;
    this.sortBy = nextSort;
    this.globalSettings.sortBy = nextSort;
    instanceManager.saveGlobalSettings(this.getSandboxFilesDir(), this.globalSettings);
    this.reloadInstances(true);
  }

  private updateViewMode(nextMode: string): void {
    if (this.viewMode === nextMode) return;
    this.runAnimated(Motion.SPRING_SMOOTH, Motion.DUR_SHORT, () => {
      this.viewMode = nextMode;
    });
    this.globalSettings.viewMode = nextMode;
    instanceManager.saveGlobalSettings(this.getSandboxFilesDir(), this.globalSettings);
  }

  private setBatchModeAnimated(enabled: boolean): void {
    this.runAnimated(Motion.SPRING_SNAPPY, Motion.DUR_SHORT, () => {
      this.batchMode = enabled;
      if (!enabled) {
        this.selectedIds = [];
      }
    });
  }

  private switchNavAnimated(nextNav: string): void {
    if (this.activeNav === nextNav) return;
    this.runAnimated(Motion.SPRING_SNAPPY, Motion.DUR_MEDIUM, () => {
      this.activeNav = nextNav;
    });
    if (nextNav === 'apps' && this.wmAgentReady && this.appList.length === 0) {
      this.refreshApps(false);
    }
  }

  // --- Batch Actions ---
  private batchCleanSelected(): void {
    if (this.selectedIds.length === 0) {
      this.showToast('请先勾选设备');
      return;
    }
    let totalFreed: number = 0;
    let cleanedCount: number = 0;
    let skippedRunning: number = 0;
    const filesDir: string = this.getSandboxFilesDir();
    for (let i: number = 0; i < this.selectedIds.length; i++) {
      const inst: DeviceInstance | undefined = this.getInstanceById(this.selectedIds[i]);
      if (!inst) continue;
      if (this.isInstanceRunning(inst)) {
        skippedRunning++;
        continue;
      }
      const freed: number = native.cleanInstanceCache(filesDir, inst.dataSubdir);
      if (freed >= 0) {
        totalFreed += freed;
        cleanedCount++;
      }
    }
    if (cleanedCount === 0 && skippedRunning > 0) {
      this.showToast('所选设备正在运行，请先停止后再执行磁盘清理');
    } else if (skippedRunning > 0) {
      this.showToast(`已清理 ${cleanedCount} 台设备（释放 ${totalFreed.toFixed(1)} MB），已跳过 ${skippedRunning} 台运行中设备`);
    } else {
      this.showToast(`已批量清理 ${cleanedCount} 台设备，共释放 ${totalFreed.toFixed(1)} MB`);
    }
  }

  private batchDeleteSelected(): void {
    if (this.selectedIds.length === 0) {
      this.showToast('请先勾选设备');
      return;
    }
    const deletable: DeviceInstance[] = [];
    for (let i: number = 0; i < this.selectedIds.length; i++) {
      const inst: DeviceInstance | undefined = this.getInstanceById(this.selectedIds[i]);
      if (inst && !this.isInstanceRunning(inst) && inst.id !== 'inst_0') {
        deletable.push(inst);
      }
    }
    if (deletable.length === 0) {
      this.showToast('没有可删除的设备（默认设备与运行中设备受保护）');
      return;
    }
    this.getUIContext().showAlertDialog({
      title: '批量删除设备',
      message: `将永久删除 ${deletable.length} 台非运行设备及其隔离数据，是否继续？`,
      primaryButton: { value: '取消', action: () => {} },
      secondaryButton: {
        value: '确认删除',
        action: () => {
          const filesDir: string = this.getSandboxFilesDir();
          for (let i: number = 0; i < deletable.length; i++) {
            native.deleteInstanceData(filesDir, deletable[i].dataSubdir);
            instanceManager.deleteInstance(filesDir, deletable[i].id);
          }
          this.setBatchModeAnimated(false);
          this.reloadInstances(true);
          this.showToast(`已删除 ${deletable.length} 台设备`);
        }
      }
    });
  }

  // --- Integrated Apps Helpers ---
  private getFilteredApps(): AndroidAppItem[] {
    const q: string = this.searchQuery.trim().toLowerCase();
    const out: AndroidAppItem[] = [];
    for (let i: number = 0; i < this.appList.length; i++) {
      const a: AndroidAppItem = this.appList[i];
      if (this.appFilter === 'user' && a.isSystem) continue;
      if (this.appFilter === 'system' && !a.isSystem) continue;
      if (q.length > 0) {
        const hay: string = `${a.label} ${a.packageName}`.toLowerCase();
        if (hay.indexOf(q) < 0) continue;
      }
      out.push(a);
    }
    return out;
  }

  private isAppPinned(pkg: string): boolean {
    return this.pinnedIds.indexOf(computeShortcutId(pkg)) >= 0;
  }

  private getEffectiveModeLabel(app: AndroidAppItem): string {
    const pref: AppWindowPref = appWindowPrefs.getPref(
      this.getSandboxFilesDir(), app.packageName, app, this.refreshRate
    );
    if (pref.mode === 'adaptive') return '支持·自由';
    if (pref.mode === 'fixed_landscape') return '兼容·横';
    return '兼容·竖';
  }

  private cycleAppWindowPref(app: AndroidAppItem): void {
    const dir: string = this.getSandboxFilesDir();
    const cur: AppWindowPref = appWindowPrefs.getPref(dir, app.packageName, app, this.refreshRate);
    let nextMode: string = 'adaptive';
    let nextPreset: string = cur.presetKey;
    let nextW: number = cur.winW;
    let nextH: number = cur.winH;
    if (cur.mode === 'adaptive') {
      nextMode = 'fixed_portrait';
      nextPreset = '720x1280';
      nextW = 860;
      nextH = 1380;
    } else if (cur.mode === 'fixed_portrait') {
      nextMode = 'fixed_landscape';
      nextPreset = '1280x720';
      nextW = 1480;
      nextH = 940;
    } else {
      nextMode = 'adaptive';
    }
    appWindowPrefs.savePref(dir, app.packageName, {
      mode: nextMode,
      presetKey: nextPreset,
      refreshRate: cur.refreshRate,
      winW: nextW,
      winH: nextH,
    });
    this.prefEpoch += 1;
    const lbl: string = nextMode === 'adaptive' ? '自由窗口' : (nextMode === 'fixed_landscape' ? '固定横屏' : '固定竖屏');
    this.showToast(`${app.label}：已设为${lbl}`);
  }

  private async toggleDesktopShortcut(app: AndroidAppItem): Promise<void> {
    const ctx: common.UIAbilityContext = this.getUIContext().getHostContext() as common.UIAbilityContext;
    if (this.isAppPinned(app.packageName)) {
      const r: ShortcutOpResult = await removeAndroidAppShortcut(ctx, app.packageName);
      this.showToast(r.message);
      this.refreshPinnedShortcuts();
      return;
    }
    const iconPath: string = this.iconsDir.length > 0 && app.iconFile.length > 0
      ? `${this.iconsDir}/${app.iconFile}` : '';
    const r: ShortcutOpResult = await pinAndroidAppToDesktop(ctx, app, iconPath);
    this.showToast(r.message);
    this.refreshPinnedShortcuts();
  }

  private getAppsColumnsTemplate(): string {
    if (this.bp === 'sm') return '1fr 1fr';
    if (this.bp === 'md') return '1fr 1fr 1fr 1fr';
    if (this.winWidthVp >= 1220) return '1fr 1fr 1fr 1fr 1fr 1fr';
    return '1fr 1fr 1fr 1fr 1fr';
  }

  private computeSheetWidth(preferred: number): Dimension {
    if (this.bp === 'sm') return '100%';
    return Math.max(360, Math.min(preferred, Math.floor(this.winWidthVp - 40)));
  }

  private computeSheetType(): SheetType {
    return this.bp === 'sm' ? SheetType.BOTTOM : SheetType.CENTER;
  }

  private updateBreakpoint(wVp: number): void {
    if (wVp <= 0) return;
    this.winWidthVp = wVp;
    const nextBp: string = wVp < 600 ? 'sm' : (wVp < 840 ? 'md' : 'lg');
    if (nextBp !== this.bp) {
      this.runAnimated(Motion.SPRING_SMOOTH, Motion.DUR_SHORT, () => {
        this.bp = nextBp;
      });
    }
  }

  // --- Half-Modal Sheet Builders ---
  @Builder
  private createSheetBuilder() {
    CreateInstanceSheet({
      compactLayout: this.bp === 'sm',
      onConfirm: (opts: CreateInstanceOptions) => {
        this.showCreateSheet = false;
        const created: DeviceInstance[] = instanceManager.createInstances(this.getSandboxFilesDir(), opts);
        this.reloadInstances(true);
        this.showToast(`已成功创建 ${created.length} 台新设备`);
      },
      onCancel: () => {
        this.showCreateSheet = false;
      }
    })
  }

  @Builder
  private deviceSettingsSheetBuilder() {
    DeviceSettingsSheet({
      instance: this.editingInstance,
      filesDir: this.getSandboxFilesDir(),
      isInstanceRunning: this.editingInstance ? this.isInstanceRunning(this.editingInstance) : false,
      compactLayout: this.winWidthVp < 680,
      onSave: (updated: DeviceInstance) => {
        const prevDiskGb: number = this.editingInstance ? this.editingInstance.diskLimitGb : updated.diskLimitGb;
        this.showDeviceSettingsSheet = false;
        instanceManager.updateInstance(this.getSandboxFilesDir(), updated);
        this.reloadInstances(true);
        const runningNow: boolean = this.isInstanceRunning(updated);
        if (runningNow) {
          try {
            native.setRefreshRate(updated.refreshRate);
            native.setResourceConfig(updated.profile, '', updated.memLimitMb, updated.diskLimitGb, updated.cpuCores);
          } catch (_e) {
          }
        }
        if (runningNow && updated.diskLimitGb !== prevDiskGb) {
          this.showToast(`已保存存储容量（${updated.diskLimitGb} GB），重启设备后无损生效`);
        } else {
          this.showToast(`已保存「${updated.name}」设置`);
        }
      },
      onCancel: () => {
        this.showDeviceSettingsSheet = false;
      },
      onToast: (msg: string) => {
        this.showToast(msg);
      }
    })
  }

  @Builder
  private backupSheetBuilder() {
    BackupRestoreSheet({
      instance: this.editingInstance,
      filesDir: this.getSandboxFilesDir(),
      backupDir: this.globalSettings.backupDir,
      isInstanceRunning: this.editingInstance ? this.isInstanceRunning(this.editingInstance) : false,
      onFinished: (msg: string) => {
        this.showToast(msg);
      },
      onClose: () => {
        this.showBackupSheet = false;
      }
    })
  }

  @Builder
  private globalSettingsSheetBuilder() {
    GlobalSettingsSheet({
      initialSettings: this.globalSettings,
      compactLayout: this.bp === 'sm',
      onSave: (next: GlobalSettings) => {
        this.showGlobalSettingsSheet = false;
        this.globalSettings = next;
        instanceManager.saveGlobalSettings(this.getSandboxFilesDir(), next);
        this.showToast('全局设置已保存');
      },
      onCancel: () => {
        this.showGlobalSettingsSheet = false;
      }
    })
  }

  // --- Menus ---
  @Builder
  private sortMenuBuilder() {
    Menu() {
      MenuItem({ content: '按编号排序' })
        .selected(this.sortBy === 'index')
        .onClick(() => { this.updateSort('index'); })
      MenuItem({ content: '按创建时间排序' })
        .selected(this.sortBy === 'time')
        .onClick(() => { this.updateSort('time'); })
      MenuItem({ content: '按名称排序' })
        .selected(this.sortBy === 'name')
        .onClick(() => { this.updateSort('name'); })
    }
  }

  @Builder
  private batchMoreMenuBuilder() {
    Menu() {
      MenuItem({ content: '批量清理缓存' })
        .onClick(() => { this.batchCleanSelected(); })
      MenuItem({ content: '批量删除所选设备' })
        .onClick(() => { this.batchDeleteSelected(); })
    }
  }

  @Builder
  private deviceMoreMenuBuilder(inst: DeviceInstance) {
    Menu() {
      MenuItem({ content: '设备设置' })
        .onClick(() => { this.openDeviceSettingsSheet(inst); })
      MenuItem({ content: '打开模拟器窗口' })
        .onClick(() => {
          if (!this.isInstanceRunning(inst)) {
            this.startDeviceInstance(inst, 'full');
          } else {
            this.openEmulatorWindow(inst);
          }
        })
      MenuItem({ content: '磁盘清理' })
        .enabled(!this.isInstanceRunning(inst))
        .onClick(() => { this.cleanDeviceDisk(inst); })
      MenuItem({ content: '重命名' })
        .onClick(() => { this.openRenameDialog(inst); })
      if (supportsMultiInstance()) {
        MenuItem({ content: '复制设备' })
          .onClick(() => { this.cloneDeviceInstance(inst); })
      }
      MenuItem({ content: '备份与还原' })
        .onClick(() => { this.openBackupSheet(inst); })
      if (supportsMultiInstance()) {
        MenuItem({ content: '删除设备' })
          .enabled(inst.id !== 'inst_0' && !this.isInstanceRunning(inst))
          .onClick(() => { this.confirmDeleteDevice(inst); })
      }
    }
  }

  // --- Left Navigation Rail Item ---
  @Builder
  private navRailItem(key: string, label: string, iconRes: Resource, badgeCount: number) {
    Column({ space: Spacing.XS }) {
      Stack({ alignContent: Alignment.TopEnd }) {
        Stack({ alignContent: Alignment.Center }) {
          SymbolGlyph(iconRes)
            .fontSize(IconSize.MD)
            .fontColor([
              this.activeNav === key
                ? $r('sys.color.icon_emphasize')
                : $r('sys.color.icon_secondary')
            ])
        }
        .width(44)
        .height(32)

        if (badgeCount > 0) {
          Text(`${badgeCount}`)
            .fontSize(FontSize.BADGE)
            .fontWeight(FontWeight.Medium)
            .fontColor($r('sys.color.font_on_primary'))
            .padding({ left: Spacing.XS + 1, right: Spacing.XS + 1, top: 1, bottom: 1 })
            .borderRadius(Radius.SM)
            .backgroundColor($r('sys.color.brand'))
            .offset({ x: 4, y: -2 })
            .transition(Motion.pillPopTransition())
        }
      }

      Text(label)
        .fontSize(FontSize.MICRO)
        .fontWeight(this.activeNav === key ? FontWeight.Medium : FontWeight.Normal)
        .fontColor(
          this.activeNav === key
            ? $r('sys.color.font_emphasize')
            : $r('sys.color.font_secondary')
        )
    }
    .width('100%')
    .height(NAV_RAIL_ITEM_H)
    .justifyContent(FlexAlign.Center)
    .alignItems(HorizontalAlign.Center)
    .borderRadius(Radius.MD)
    .hoverEffect(HoverEffect.Highlight)
    .clickEffect({ level: ClickEffectLevel.LIGHT, scale: 0.94 })
    .onClick(() => {
      this.switchNavAnimated(key);
    })
  }

  @Builder
  private specChip(label: string) {
    Text(label)
      .fontSize(FontSize.MICRO)
      .fontColor($r('sys.color.font_secondary'))
      .padding({ left: Spacing.SM, right: Spacing.SM, top: Spacing.XS, bottom: Spacing.XS })
      .borderRadius(Radius.BADGE)
      .backgroundColor($r('sys.color.background_secondary'))
  }

  @Builder
  private deviceMobileCard(inst: DeviceInstance) {
    Column({ space: Spacing.MD }) {
      Row({ space: Spacing.MD }) {
        if (this.batchMode) {
          Checkbox()
            .select(this.isSelected(inst.id))
            .selectedColor($r('sys.color.brand'))
            .width(20)
            .height(20)
            .transition(Motion.checkboxSlideTransition())
            .onChange(() => {
              this.toggleSelection(inst.id);
            })
        }

        // Compact Mobile Thumbnail (52 x 52 vp)
        Stack({ alignContent: Alignment.Center }) {
          Column()
            .width(52)
            .height(52)
            .borderRadius(Radius.MD)
            .linearGradient({
              angle: 135,
              colors: this.isInstanceRunning(inst)
                ? [['#0A59F7', 0.0], ['#38BDF8', 1.0]]
                : [['#2B3242', 0.0], ['#1B202C', 1.0]]
            })
          SymbolGlyph(inst.deviceType === 'tablet' ? $r('sys.symbol.pad_fill') : $r('sys.symbol.phone_1'))
            .fontSize(IconSize.MD)
            .fontColor([this.isInstanceRunning(inst) ? '#FFFFFF' : '#CBD5E1'])
        }
        .flexShrink(0)
        .onClick(() => {
          if (this.isInstanceRunning(inst)) {
            this.openEmulatorWindow(inst);
          }
        })

        Column({ space: Spacing.XS }) {
          Row({ space: Spacing.SM }) {
            Text(`#${inst.index}`)
              .fontSize(FontSize.MICRO)
              .fontWeight(FontWeight.Medium)
              .fontColor($r('sys.color.font_secondary'))
            Text(inst.name)
              .fontSize(FontSize.BODY_LG)
              .fontWeight(FontWeight.Bold)
              .fontColor($r('sys.color.font_primary'))
              .maxLines(1)
              .textOverflow({ overflow: TextOverflow.Ellipsis })
              .layoutWeight(1)
          }
          .width('100%')

          Row({ space: Spacing.SM }) {
            Row({ space: Spacing.XS }) {
              Circle({ width: 6, height: 6 })
                .fill(
                  this.isInstanceRunning(inst)
                    ? (this.paused ? $r('sys.color.alert') : $r('sys.color.confirm'))
                    : $r('sys.color.font_fourth')
                )
              Text(
                this.isInstanceRunning(inst)
                  ? (this.paused ? '已暂停' : (this.wmAgentReady ? '运行中' : '启动中'))
                  : '未开机'
              )
                .fontSize(FontSize.MICRO)
                .fontColor(
                  this.isInstanceRunning(inst)
                    ? (this.paused ? $r('sys.color.alert') : $r('sys.color.confirm'))
                    : $r('sys.color.font_tertiary')
                )
            }

            Text(inst.runMode === 'emulator' ? '传统模拟器' : '自由多窗')
              .fontSize(FontSize.BADGE)
              .fontWeight(FontWeight.Medium)
              .fontColor($r('sys.color.font_emphasize'))
              .padding({ left: Spacing.SM - 2, right: Spacing.SM - 2, top: 2, bottom: 2 })
              .borderRadius(Radius.BADGE)
              .backgroundColor($r('sys.color.comp_emphasize_tertiary'))
          }
        }
        .layoutWeight(1)
        .alignItems(HorizontalAlign.Start)

        Row({ space: Spacing.SM }) {
          if (this.isInstanceRunning(inst)) {
            Button({ type: ButtonType.Circle }) {
              SymbolGlyph(inst.runMode === 'emulator' ? $r('sys.symbol.display') : $r('sys.symbol.square_grid_2x2'))
                .fontSize(IconSize.SM)
                .fontColor([$r('sys.color.icon_on_primary')])
            }
            .width(ControlSize.BTN_MD)
            .height(ControlSize.BTN_MD)
            .backgroundColor($r('sys.color.brand'))
            .clickEffect({ level: ClickEffectLevel.LIGHT, scale: 0.92 })
            .onClick(() => {
              if (inst.runMode === 'emulator') {
                this.openEmulatorWindow(inst);
              } else {
                this.switchNavAnimated('apps');
              }
            })

            Button({ type: ButtonType.Circle }) {
              SymbolGlyph($r('sys.symbol.power'))
                .fontSize(IconSize.SM)
                .fontColor([$r('sys.color.warning')])
            }
            .width(ControlSize.BTN_MD)
            .height(ControlSize.BTN_MD)
            .backgroundColor('#1AE84026')
            .clickEffect({ level: ClickEffectLevel.LIGHT, scale: 0.92 })
            .onClick(() => {
              this.stopActiveInstance();
            })
          } else {
            Button({ type: ButtonType.Circle }) {
              SymbolGlyph($r('sys.symbol.play_fill'))
                .fontSize(IconSize.SM)
                .fontColor([$r('sys.color.icon_on_primary')])
            }
            .width(ControlSize.BTN_MD)
            .height(ControlSize.BTN_MD)
            .backgroundColor($r('sys.color.brand'))
            .clickEffect({ level: ClickEffectLevel.LIGHT, scale: 0.92 })
            .onClick(() => {
              this.startDeviceInstance(inst, 'full');
            })
          }

          Button({ type: ButtonType.Circle }) {
            SymbolGlyph($r('sys.symbol.dot_grid_1x2'))
              .fontSize(IconSize.SM)
              .fontColor([$r('sys.color.icon_secondary')])
          }
          .width(ControlSize.BTN_MD)
          .height(ControlSize.BTN_MD)
          .backgroundColor($r('sys.color.comp_background_tertiary'))
          .clickEffect({ level: ClickEffectLevel.LIGHT, scale: 0.92 })
          .bindMenu(this.deviceMoreMenuBuilder(inst))
        }
      }
      .width('100%')
      .alignItems(VerticalAlign.Center)

      if (this.viewMode === 'card') {
        Flex({
          wrap: FlexWrap.Wrap,
          space: { main: { value: Spacing.SM, unit: LengthUnit.VP }, cross: { value: Spacing.XS + 2, unit: LengthUnit.VP } }
        }) {
          this.specChip(inst.androidVersion)
          this.specChip(`${inst.cpuCores} 核`)
          this.specChip(`${Math.round(inst.memLimitMb / 1024)} GB`)
          this.specChip(formatGpuModeLabel(inst.gpuMode))
          this.specChip(`${inst.refreshRate}Hz`)
          this.specChip(`${inst.width}×${inst.height}`)
        }
        .width('100%')
      }
    }
    .width('100%')
    .padding(Spacing.MD)
    .borderRadius(Radius.CARD)
    .backgroundColor($r('sys.color.comp_background_primary'))
    .border({
      width: this.isInstanceRunning(inst) ? 1.2 : (this.hoveredInstId === inst.id ? 1.0 : 0.6),
      color: this.isInstanceRunning(inst) || this.hoveredInstId === inst.id
        ? $r('sys.color.brand')
        : $r('sys.color.comp_divider')
    })
    .shadow(this.hoveredInstId === inst.id ? ShadowStyle.OUTER_DEFAULT_SM : ShadowStyle.OUTER_DEFAULT_XS)
    .animation({ duration: Motion.DUR_MICRO, curve: Motion.STD_CURVE })
    .onHover((isHover: boolean) => {
      this.hoveredInstId = isHover ? inst.id : (this.hoveredInstId === inst.id ? '' : this.hoveredInstId);
    })
  }

  @Builder
  private deviceCardRow(inst: DeviceInstance) {
    Row({ space: Spacing.LG }) {
      if (this.batchMode) {
        Checkbox()
          .select(this.isSelected(inst.id))
          .selectedColor($r('sys.color.brand'))
          .width(20)
          .height(20)
          .transition(Motion.checkboxSlideTransition())
          .onChange(() => {
            this.toggleSelection(inst.id);
          })
      }

      // Preview Thumbnail (adapts slightly between md Tablet and lg PC)
      Stack({ alignContent: Alignment.TopStart }) {
        Column() {
          SymbolGlyph(inst.deviceType === 'tablet' ? $r('sys.symbol.pad_fill') : $r('sys.symbol.phone_1'))
            .fontSize(IconSize.LG)
            .fontColor([
              this.isInstanceRunning(inst) ? '#FFFFFF' : '#CBD5E1'
            ])
        }
        .width('100%')
        .height('100%')
        .justifyContent(FlexAlign.Center)
        .alignItems(HorizontalAlign.Center)
        .borderRadius(Radius.MD)
        .linearGradient({
          angle: 135,
          colors: this.isInstanceRunning(inst)
            ? [['#0A59F7', 0.0], ['#38BDF8', 1.0]]
            : [['#2B3242', 0.0], ['#1B202C', 1.0]]
        })

        Text(`# ${inst.index}`)
          .fontSize(FontSize.BADGE)
          .fontWeight(FontWeight.Medium)
          .fontColor('#FFFFFF')
          .padding({ left: Spacing.SM - 2, right: Spacing.SM - 2, top: 2, bottom: 2 })
          .borderRadius(Radius.BADGE)
          .backgroundColor('#66000000')
          .margin({ left: Spacing.SM - 2, top: Spacing.SM - 2 })

        Row({ space: Spacing.XS }) {
          Circle({ width: 6, height: 6 })
            .fill(
              this.isInstanceRunning(inst)
                ? (this.paused ? '#F59E0B' : '#4ADE80')
                : '#94A3B8'
            )
          Text(
            this.isInstanceRunning(inst)
              ? (this.paused ? '已暂停' : (this.wmAgentReady ? '运行中' : '启动中'))
              : '未开机'
          )
            .fontSize(FontSize.BADGE)
            .fontColor('#FFFFFF')
        }
        .padding({ left: Spacing.SM - 2, right: Spacing.SM, top: 2, bottom: 2 })
        .borderRadius(Radius.BADGE)
        .backgroundColor('#7A000000')
        .position({ x: 6, y: this.bp === 'md' ? 44 : 52 })
      }
      .width(this.bp === 'md' ? 104 : 128)
      .height(this.bp === 'md' ? 68 : 76)
      .flexShrink(0)
      .clickEffect({ level: ClickEffectLevel.LIGHT, scale: 0.96 })
      .onClick(() => {
        if (this.isInstanceRunning(inst)) {
          this.openEmulatorWindow(inst);
        }
      })

      // Middle: Title + Wrapping Spec Chips + Telemetry
      Column({ space: Spacing.SM }) {
        Row({ space: Spacing.SM }) {
          Text(inst.name)
            .fontSize(FontSize.TITLE_SM)
            .fontWeight(FontWeight.Bold)
            .fontColor($r('sys.color.font_primary'))
            .maxLines(1)
            .textOverflow({ overflow: TextOverflow.Ellipsis })
            .constraintSize({ maxWidth: this.bp === 'md' ? 180 : 280 })

          if (this.isInstanceRunning(inst)) {
            Row({ space: Spacing.XS }) {
              Circle({ width: 6, height: 6 })
                .fill(this.paused ? $r('sys.color.alert') : $r('sys.color.confirm'))
              Text(this.paused ? '已暂停' : (this.wmAgentReady ? '运行中' : '启动中'))
                .fontSize(FontSize.MICRO)
                .fontWeight(FontWeight.Medium)
                .fontColor(this.paused ? $r('sys.color.alert') : $r('sys.color.confirm'))
            }
            .padding({ left: Spacing.SM, right: Spacing.SM, top: 2, bottom: 2 })
            .borderRadius(Radius.MD)
            .backgroundColor(this.paused ? '#1FED6F21' : '#1F64BB5C')
            .transition(Motion.pillPopTransition())
          }

          Text(inst.runMode === 'emulator' ? '传统模拟器' : '自由多窗')
            .fontSize(FontSize.BADGE)
            .fontWeight(FontWeight.Medium)
            .fontColor($r('sys.color.font_emphasize'))
            .padding({ left: Spacing.SM, right: Spacing.SM, top: 2, bottom: 2 })
            .borderRadius(Radius.BADGE)
            .backgroundColor($r('sys.color.comp_emphasize_tertiary'))
        }
        .alignItems(VerticalAlign.Center)

        Flex({
          wrap: FlexWrap.Wrap,
          space: { main: { value: Spacing.SM, unit: LengthUnit.VP }, cross: { value: Spacing.XS, unit: LengthUnit.VP } }
        }) {
          this.specChip(inst.androidVersion)
          this.specChip(`${inst.cpuCores} 核`)
          this.specChip(`${Math.round(inst.memLimitMb / 1024)} GB`)
          this.specChip(formatGpuModeLabel(inst.gpuMode))
          this.specChip(`${inst.refreshRate}Hz`)
        }
        .width('100%')

        Text(
          `${inst.width}×${inst.height} · ${inst.dpi} DPI` +
          (this.isInstanceRunning(inst)
            ? ` · ${this.fps} FPS · ${this.activeProcs} 进程 · PSS ${this.memMb} MB` +
              (this.gfxMemMb > 0 ? ` (显存 ${this.gfxMemMb} MB)` : '')
            : ` · 分区 ${inst.dataSubdir}`)
        )
          .fontSize(FontSize.CAPTION)
          .fontColor($r('sys.color.font_tertiary'))
          .maxLines(1)
          .textOverflow({ overflow: TextOverflow.Ellipsis })
      }
      .layoutWeight(1)
      .alignItems(HorizontalAlign.Start)

      // Right: Unified Circular Actions
      Row({ space: Spacing.SM }) {
        if (this.isInstanceRunning(inst)) {
          if (this.bp === 'lg') {
            Button({ type: ButtonType.Capsule }) {
              Row({ space: Spacing.XS + 2 }) {
                SymbolGlyph(inst.runMode === 'emulator' ? $r('sys.symbol.display') : $r('sys.symbol.square_grid_2x2'))
                  .fontSize(IconSize.SM)
                  .fontColor([$r('sys.color.icon_on_primary')])
                Text(inst.runMode === 'emulator' ? '打开窗口' : '应用中心')
                  .fontSize(FontSize.CAPTION)
                  .fontWeight(FontWeight.Medium)
                  .fontColor($r('sys.color.font_on_primary'))
              }
            }
            .height(ControlSize.BTN_MD)
            .padding({ left: Spacing.LG, right: Spacing.LG })
            .backgroundColor($r('sys.color.brand'))
            .hoverEffect(HoverEffect.Highlight)
            .clickEffect({ level: ClickEffectLevel.LIGHT, scale: 0.95 })
            .onClick(() => {
              if (inst.runMode === 'emulator') {
                this.openEmulatorWindow(inst);
              } else {
                this.switchNavAnimated('apps');
              }
            })
          } else {
            Button({ type: ButtonType.Circle }) {
              SymbolGlyph(inst.runMode === 'emulator' ? $r('sys.symbol.display') : $r('sys.symbol.square_grid_2x2'))
                .fontSize(IconSize.SM)
                .fontColor([$r('sys.color.icon_on_primary')])
            }
            .width(ControlSize.BTN_MD)
            .height(ControlSize.BTN_MD)
            .backgroundColor($r('sys.color.brand'))
            .hoverEffect(HoverEffect.Highlight)
            .clickEffect({ level: ClickEffectLevel.LIGHT, scale: 0.92 })
            .onClick(() => {
              if (inst.runMode === 'emulator') {
                this.openEmulatorWindow(inst);
              } else {
                this.switchNavAnimated('apps');
              }
            })
          }

          Button({ type: ButtonType.Circle }) {
            SymbolGlyph($r('sys.symbol.clean_fill'))
              .fontSize(IconSize.SM)
              .fontColor([$r('sys.color.brand')])
          }
          .width(ControlSize.BTN_MD)
          .height(ControlSize.BTN_MD)
          .backgroundColor($r('sys.color.comp_background_tertiary'))
          .hoverEffect(HoverEffect.Highlight)
          .clickEffect({ level: ClickEffectLevel.LIGHT, scale: 0.92 })
          .onClick(() => {
            native.trimMemory();
            this.showToast('正在释放闲置缓存内存...');
          })

          Button({ type: ButtonType.Circle }) {
            SymbolGlyph(this.paused ? $r('sys.symbol.play_fill') : $r('sys.symbol.pause_fill'))
              .fontSize(IconSize.SM)
              .fontColor([this.paused ? $r('sys.color.confirm') : $r('sys.color.alert')])
          }
          .width(ControlSize.BTN_MD)
          .height(ControlSize.BTN_MD)
          .backgroundColor($r('sys.color.comp_background_tertiary'))
          .hoverEffect(HoverEffect.Highlight)
          .clickEffect({ level: ClickEffectLevel.LIGHT, scale: 0.92 })
          .onClick(() => {
            this.statusText = native.setPaused(!this.paused);
          })

          Button({ type: ButtonType.Circle }) {
            SymbolGlyph($r('sys.symbol.power'))
              .fontSize(IconSize.SM)
              .fontColor([$r('sys.color.warning')])
          }
          .width(ControlSize.BTN_MD)
          .height(ControlSize.BTN_MD)
          .backgroundColor('#1AE84026')
          .hoverEffect(HoverEffect.Highlight)
          .clickEffect({ level: ClickEffectLevel.LIGHT, scale: 0.92 })
          .onClick(() => {
            this.stopActiveInstance();
          })
        } else {
          Button({ type: ButtonType.Circle }) {
            SymbolGlyph($r('sys.symbol.play_fill'))
              .fontSize(IconSize.MD)
              .fontColor([$r('sys.color.icon_on_primary')])
          }
          .width(ControlSize.BTN_LG)
          .height(ControlSize.BTN_LG)
          .backgroundColor($r('sys.color.brand'))
          .hoverEffect(HoverEffect.Highlight)
          .clickEffect({ level: ClickEffectLevel.LIGHT, scale: 0.92 })
          .onClick(() => {
            this.startDeviceInstance(inst, 'full');
          })
        }

        Button({ type: ButtonType.Circle }) {
          SymbolGlyph($r('sys.symbol.dot_grid_1x2'))
            .fontSize(IconSize.SM)
            .fontColor([$r('sys.color.icon_secondary')])
        }
        .width(ControlSize.BTN_MD)
        .height(ControlSize.BTN_MD)
        .backgroundColor($r('sys.color.comp_background_tertiary'))
        .hoverEffect(HoverEffect.Highlight)
        .clickEffect({ level: ClickEffectLevel.LIGHT, scale: 0.92 })
        .bindMenu(this.deviceMoreMenuBuilder(inst))
      }
      .alignItems(VerticalAlign.Center)
    }
    .width('100%')
    .padding(Spacing.LG)
    .borderRadius(Radius.CARD)
    .backgroundColor($r('sys.color.comp_background_primary'))
    .border({
      width: this.isInstanceRunning(inst) ? 1.2 : (this.hoveredInstId === inst.id ? 1.0 : 0.6),
      color: this.isInstanceRunning(inst) || this.hoveredInstId === inst.id
        ? $r('sys.color.brand')
        : $r('sys.color.comp_divider')
    })
    .shadow(this.hoveredInstId === inst.id ? ShadowStyle.OUTER_DEFAULT_SM : ShadowStyle.OUTER_DEFAULT_XS)
    .animation({ duration: Motion.DUR_MICRO, curve: Motion.STD_CURVE })
    .onHover((isHover: boolean) => {
      this.hoveredInstId = isHover ? inst.id : (this.hoveredInstId === inst.id ? '' : this.hoveredInstId);
    })
  }

  @Builder
  private deviceCompactRow(inst: DeviceInstance) {
    Row({ space: Spacing.MD }) {
      if (this.batchMode) {
        Checkbox()
          .select(this.isSelected(inst.id))
          .selectedColor($r('sys.color.brand'))
          .width(18)
          .height(18)
          .transition(Motion.checkboxSlideTransition())
          .onChange(() => {
            this.toggleSelection(inst.id);
          })
      }

      Text(`#${inst.index}`)
        .fontSize(FontSize.CAPTION)
        .fontWeight(FontWeight.Medium)
        .fontColor($r('sys.color.font_secondary'))
        .width(28)

      Circle({ width: 8, height: 8 })
        .fill(this.isInstanceRunning(inst) ? $r('sys.color.confirm') : $r('sys.color.font_fourth'))

      Text(inst.name)
        .fontSize(FontSize.BODY_LG)
        .fontWeight(FontWeight.Medium)
        .fontColor($r('sys.color.font_primary'))
        .layoutWeight(1)
        .maxLines(1)
        .textOverflow({ overflow: TextOverflow.Ellipsis })

      Text(`${inst.androidVersion} · ${inst.cpuCores}核 ${Math.round(inst.memLimitMb / 1024)}G · ${formatGpuModeLabel(inst.gpuMode)} · ${inst.refreshRate}Hz`)
        .fontSize(FontSize.CAPTION)
        .fontColor($r('sys.color.font_tertiary'))

      if (this.isInstanceRunning(inst)) {
        Button('打开', { type: ButtonType.Capsule })
          .height(ControlSize.BTN_SM)
          .padding({ left: Spacing.MD, right: Spacing.MD })
          .fontSize(FontSize.CAPTION)
          .fontColor($r('sys.color.font_on_primary'))
          .backgroundColor($r('sys.color.brand'))
          .hoverEffect(HoverEffect.Highlight)
          .clickEffect({ level: ClickEffectLevel.LIGHT, scale: 0.94 })
          .onClick(() => { this.openEmulatorWindow(inst); })

        Button({ type: ButtonType.Circle }) {
          SymbolGlyph($r('sys.symbol.power'))
            .fontSize(IconSize.SM)
            .fontColor([$r('sys.color.warning')])
        }
        .width(ControlSize.BTN_SM)
        .height(ControlSize.BTN_SM)
        .backgroundColor('#1AE84026')
        .hoverEffect(HoverEffect.Highlight)
        .clickEffect({ level: ClickEffectLevel.LIGHT, scale: 0.92 })
        .onClick(() => { this.stopActiveInstance(); })
      } else {
        Button({ type: ButtonType.Circle }) {
          SymbolGlyph($r('sys.symbol.play_fill'))
            .fontSize(IconSize.SM)
            .fontColor([$r('sys.color.icon_on_primary')])
        }
        .width(ControlSize.BTN_SM)
        .height(ControlSize.BTN_SM)
        .backgroundColor($r('sys.color.brand'))
        .hoverEffect(HoverEffect.Highlight)
        .clickEffect({ level: ClickEffectLevel.LIGHT, scale: 0.92 })
        .onClick(() => { this.startDeviceInstance(inst, 'full'); })
      }

      Button({ type: ButtonType.Circle }) {
        SymbolGlyph($r('sys.symbol.dot_grid_1x2'))
          .fontSize(IconSize.SM)
          .fontColor([$r('sys.color.icon_secondary')])
      }
      .width(ControlSize.BTN_SM)
      .height(ControlSize.BTN_SM)
      .backgroundColor($r('sys.color.comp_background_tertiary'))
      .hoverEffect(HoverEffect.Highlight)
      .clickEffect({ level: ClickEffectLevel.LIGHT, scale: 0.92 })
      .bindMenu(this.deviceMoreMenuBuilder(inst))
    }
    .width('100%')
    .height(56)
    .padding({ left: Spacing.LG, right: Spacing.MD })
    .borderRadius(Radius.MD)
    .backgroundColor($r('sys.color.comp_background_primary'))
    .border({
      width: this.isInstanceRunning(inst) || this.hoveredInstId === inst.id ? 1.0 : 0.6,
      color: this.isInstanceRunning(inst) || this.hoveredInstId === inst.id
        ? $r('sys.color.brand')
        : $r('sys.color.comp_divider')
    })
    .animation({ duration: Motion.DUR_MICRO, curve: Motion.STD_CURVE })
    .onHover((isHover: boolean) => {
      this.hoveredInstId = isHover ? inst.id : (this.hoveredInstId === inst.id ? '' : this.hoveredInstId);
    })
  }

  @Builder
  private devicesWorkspace() {
    Column({ space: this.bp === 'sm' ? Spacing.MD : Spacing.LG }) {
      // Responsive Action Toolbar (Normal Mode <-> Batch Mode)
      if (!this.batchMode) {
        Flex({
          wrap: FlexWrap.Wrap,
          justifyContent: FlexAlign.SpaceBetween,
          alignItems: ItemAlign.Center,
          space: { main: { value: Spacing.SM, unit: LengthUnit.VP }, cross: { value: Spacing.SM, unit: LengthUnit.VP } }
        }) {
          Row({ space: Spacing.SM }) {
            if (supportsMultiInstance()) {
              Button({ type: ButtonType.Capsule }) {
                Row({ space: Spacing.XS + 2 }) {
                  SymbolGlyph($r('sys.symbol.plus'))
                    .fontSize(IconSize.SM)
                    .fontColor([$r('sys.color.icon_on_primary')])
                  Text(this.bp === 'sm' ? '新建' : '新建设备')
                    .fontSize(FontSize.BODY_MD)
                    .fontWeight(FontWeight.Medium)
                    .fontColor($r('sys.color.font_on_primary'))
                }
              }
              .height(ControlSize.BTN_MD)
              .padding({ left: Spacing.MD, right: Spacing.LG })
              .backgroundColor($r('sys.color.brand'))
              .hoverEffect(HoverEffect.Highlight)
              .clickEffect({ level: ClickEffectLevel.LIGHT, scale: 0.95 })
              .onClick(() => {
                this.openCreateSheet();
              })

              Button({ type: ButtonType.Capsule }) {
                Row({ space: Spacing.XS + 2 }) {
                  SymbolGlyph($r('sys.symbol.checkmark_square'))
                    .fontSize(IconSize.SM)
                    .fontColor([$r('sys.color.icon_primary')])
                  Text(this.bp === 'sm' ? '批量' : '批量操作')
                    .fontSize(FontSize.BODY_MD)
                    .fontColor($r('sys.color.font_primary'))
                }
              }
              .height(ControlSize.BTN_MD)
              .padding({ left: Spacing.MD, right: Spacing.MD })
              .backgroundColor($r('sys.color.comp_background_primary'))
              .border({ width: 0.6, color: $r('sys.color.comp_divider') })
              .hoverEffect(HoverEffect.Highlight)
              .clickEffect({ level: ClickEffectLevel.LIGHT, scale: 0.95 })
              .onClick(() => {
                this.setBatchModeAnimated(true);
              })
            } else {
              Text('移动设备单容器模式 · 自由多窗')
                .fontSize(FontSize.CAPTION)
                .fontWeight(FontWeight.Medium)
                .fontColor($r('sys.color.font_emphasize'))
                .padding({ left: Spacing.MD, right: Spacing.MD, top: Spacing.XS + 2, bottom: Spacing.XS + 2 })
                .borderRadius(Radius.MD)
                .backgroundColor($r('sys.color.comp_emphasize_tertiary'))
            }

            if (this.bp !== 'sm') {
              Button({ type: ButtonType.Capsule }) {
                Row({ space: Spacing.XS + 2 }) {
                  SymbolGlyph($r('sys.symbol.rectangle_split_2x1'))
                    .fontSize(IconSize.SM)
                    .fontColor([$r('sys.color.icon_primary')])
                  Text('排列窗口')
                    .fontSize(FontSize.BODY_MD)
                    .fontColor($r('sys.color.font_primary'))
                }
              }
              .height(ControlSize.BTN_MD)
              .padding({ left: Spacing.MD, right: Spacing.MD })
              .backgroundColor($r('sys.color.comp_background_primary'))
              .border({ width: 0.6, color: $r('sys.color.comp_divider') })
              .hoverEffect(HoverEffect.Highlight)
              .clickEffect({ level: ClickEffectLevel.LIGHT, scale: 0.95 })
              .onClick(() => {
                this.handleArrangeWindows();
              })
            }
          }

          Row({ space: Spacing.SM }) {
            Button({ type: ButtonType.Capsule }) {
              Row({ space: Spacing.XS + 2 }) {
                SymbolGlyph($r('sys.symbol.sort'))
                  .fontSize(IconSize.SM)
                  .fontColor([$r('sys.color.icon_secondary')])
                Text(this.sortBy === 'time' ? '按时间' : (this.sortBy === 'name' ? '按名称' : '按编号'))
                  .fontSize(FontSize.CAPTION)
                  .fontColor($r('sys.color.font_secondary'))
                SymbolGlyph($r('sys.symbol.chevron_down'))
                  .fontSize(IconSize.XS)
                  .fontColor([$r('sys.color.icon_tertiary')])
              }
            }
            .height(ControlSize.BTN_MD)
            .padding({ left: Spacing.MD, right: Spacing.MD })
            .backgroundColor($r('sys.color.comp_background_primary'))
            .border({ width: 0.6, color: $r('sys.color.comp_divider') })
            .hoverEffect(HoverEffect.Highlight)
            .clickEffect({ level: ClickEffectLevel.LIGHT, scale: 0.96 })
            .bindMenu(this.sortMenuBuilder())

            Stack({ alignContent: Alignment.Start }) {
              Column()
                .width(34)
                .height(28)
                .borderRadius(14)
                .backgroundColor($r('sys.color.comp_emphasize_tertiary'))
                .translate({ x: this.viewMode === 'card' ? 0 : 36 })
                .animation({ duration: Motion.DUR_SHORT, curve: Motion.SPRING_SNAPPY })

              Row({ space: 2 }) {
                Button({ type: ButtonType.Normal }) {
                  SymbolGlyph($r('sys.symbol.rectangle_grid_1x2'))
                    .fontSize(IconSize.SM)
                    .fontColor([
                      this.viewMode === 'card'
                        ? $r('sys.color.icon_emphasize')
                        : $r('sys.color.icon_secondary')
                    ])
                }
                .width(34)
                .height(28)
                .padding(0)
                .borderRadius(14)
                .backgroundColor(Color.Transparent)
                .onClick(() => { this.updateViewMode('card'); })

                Button({ type: ButtonType.Normal }) {
                  SymbolGlyph($r('sys.symbol.list_bullet'))
                    .fontSize(IconSize.SM)
                    .fontColor([
                      this.viewMode === 'compact'
                        ? $r('sys.color.icon_emphasize')
                        : $r('sys.color.icon_secondary')
                    ])
                }
                .width(34)
                .height(28)
                .padding(0)
                .borderRadius(14)
                .backgroundColor(Color.Transparent)
                .onClick(() => { this.updateViewMode('compact'); })
              }
            }
            .padding(3)
            .borderRadius(Radius.CAPSULE)
            .backgroundColor($r('sys.color.comp_background_primary'))
            .border({ width: 0.6, color: $r('sys.color.comp_divider') })
          }
        }
        .width('100%')
        .transition(Motion.toolbarSwapTransition())
      } else {
        Flex({
          wrap: FlexWrap.Wrap,
          justifyContent: FlexAlign.SpaceBetween,
          alignItems: ItemAlign.Center,
          space: { main: { value: Spacing.SM, unit: LengthUnit.VP }, cross: { value: Spacing.SM, unit: LengthUnit.VP } }
        }) {
          Row({ space: Spacing.SM }) {
            Row({ space: Spacing.XS + 2 }) {
              Checkbox()
                .select(this.selectedIds.length > 0 && this.selectedIds.length === this.instances.length)
                .selectedColor($r('sys.color.brand'))
                .width(18)
                .height(18)
                .onChange(() => {
                  this.toggleSelectAll();
                })
              Text(`全选 (${this.selectedIds.length}/${this.instances.length})`)
                .fontSize(FontSize.BODY_MD)
                .fontWeight(FontWeight.Medium)
                .fontColor($r('sys.color.font_primary'))
            }
            .onClick(() => { this.toggleSelectAll(); })

            Button('启动', { type: ButtonType.Capsule })
              .height(ControlSize.BTN_SM)
              .padding({ left: Spacing.MD, right: Spacing.MD })
              .fontSize(FontSize.CAPTION)
              .fontColor($r('sys.color.font_on_primary'))
              .backgroundColor($r('sys.color.brand'))
              .enabled(this.selectedIds.length > 0)
              .hoverEffect(HoverEffect.Highlight)
              .clickEffect({ level: ClickEffectLevel.LIGHT, scale: 0.95 })
              .onClick(() => {
                const first: DeviceInstance | undefined = this.getInstanceById(this.selectedIds[0]);
                if (first) this.startDeviceInstance(first, 'full');
              })

            Button('关闭', { type: ButtonType.Capsule })
              .height(ControlSize.BTN_SM)
              .padding({ left: Spacing.MD, right: Spacing.MD })
              .fontSize(FontSize.CAPTION)
              .fontColor($r('sys.color.font_primary'))
              .backgroundColor($r('sys.color.comp_background_primary'))
              .border({ width: 0.6, color: $r('sys.color.comp_divider') })
              .enabled(this.running)
              .hoverEffect(HoverEffect.Highlight)
              .clickEffect({ level: ClickEffectLevel.LIGHT, scale: 0.95 })
              .onClick(() => { this.stopActiveInstance(); })

            Button({ type: ButtonType.Capsule }) {
              Row({ space: Spacing.XS }) {
                Text('更多')
                  .fontSize(FontSize.CAPTION)
                  .fontColor($r('sys.color.font_primary'))
                SymbolGlyph($r('sys.symbol.chevron_down'))
                  .fontSize(IconSize.XS)
                  .fontColor([$r('sys.color.icon_secondary')])
              }
            }
            .height(ControlSize.BTN_SM)
            .padding({ left: Spacing.MD, right: Spacing.SM + 2 })
            .backgroundColor($r('sys.color.comp_background_primary'))
            .border({ width: 0.6, color: $r('sys.color.comp_divider') })
            .hoverEffect(HoverEffect.Highlight)
            .bindMenu(this.batchMoreMenuBuilder())
          }

          Button('取消批量', { type: ButtonType.Capsule })
            .height(ControlSize.BTN_SM)
            .padding({ left: Spacing.MD, right: Spacing.MD })
            .fontSize(FontSize.CAPTION)
            .fontColor($r('sys.color.font_emphasize'))
            .backgroundColor($r('sys.color.comp_emphasize_tertiary'))
            .hoverEffect(HoverEffect.Highlight)
            .clickEffect({ level: ClickEffectLevel.LIGHT, scale: 0.95 })
            .onClick(() => {
              this.setBatchModeAnimated(false);
            })
        }
        .width('100%')
        .padding({ left: Spacing.MD, right: Spacing.MD, top: Spacing.SM, bottom: Spacing.SM })
        .borderRadius(Radius.CARD)
        .backgroundColor($r('sys.color.comp_background_primary'))
        .border({ width: 0.6, color: $r('sys.color.comp_divider') })
        .transition(Motion.toolbarSwapTransition())
      }

      // Instance List with safe top/bottom shadow breathing room & subtle 3vp scrollbar
      List({ space: this.viewMode === 'card' ? Spacing.MD : Spacing.SM }) {
        ForEach(
          this.instances,
          (inst: DeviceInstance) => {
            ListItem() {
              if (this.bp === 'sm') {
                this.deviceMobileCard(inst)
              } else if (this.viewMode === 'card') {
                this.deviceCardRow(inst)
              } else {
                this.deviceCompactRow(inst)
              }
            }
            .transition(Motion.cardTransition())
          },
          (inst: DeviceInstance): string =>
            `${inst.id}_${inst.name}_${inst.gpuMode}_${inst.refreshRate}_${inst.cpuCores}_${inst.memLimitMb}_${this.running ? this.activeInstanceId : 'idle'}_${this.paused ? 1 : 0}_${this.batchMode ? 1 : 0}_${this.selectedIds.length}_${this.viewMode}_${this.bp}`
        )
      }
      .width('100%')
      .layoutWeight(1)
      .padding({ top: Spacing.XS, bottom: Spacing.MD })
      .scrollBar(BarState.Auto)
      .scrollBarWidth(3)
      .scrollBarColor($r('sys.color.comp_divider'))
    }
    .width('100%')
    .height('100%')
    .padding({
      left: this.bp === 'sm' ? Spacing.LG : Spacing.XL,
      right: this.bp === 'sm' ? Spacing.LG : Spacing.XL,
      top: Spacing.LG,
      bottom: Spacing.SM
    })
  }

  @Builder
  private appsWorkspace() {
    Column({ space: this.bp === 'sm' ? Spacing.MD : Spacing.LG }) {
      Flex({
        wrap: FlexWrap.Wrap,
        alignItems: ItemAlign.Center,
        space: { main: { value: Spacing.SM, unit: LengthUnit.VP }, cross: { value: Spacing.SM, unit: LengthUnit.VP } }
      }) {
        Row({ space: Spacing.SM }) {
          SymbolGlyph($r('sys.symbol.magnifyingglass'))
            .fontSize(IconSize.SM)
            .fontColor([$r('sys.color.icon_tertiary')])
          TextInput({ placeholder: this.bp === 'sm' ? '搜索应用...' : '搜索已安装的 Android 应用名称或包名...', text: this.searchQuery })
            .layoutWeight(1)
            .height(ControlSize.INPUT_H)
            .fontSize(FontSize.BODY_MD)
            .fontColor($r('sys.color.font_primary'))
            .placeholderColor($r('sys.color.font_tertiary'))
            .backgroundColor(Color.Transparent)
            .padding({ left: 0, right: 0 })
            .onChange((val: string) => {
              this.searchQuery = val;
            })
        }
        .width(this.bp === 'sm' ? '100%' : undefined)
        .flexGrow(this.bp === 'sm' ? 0 : 1)
        .constraintSize({ minWidth: 200 })
        .height(ControlSize.BTN_MD)
        .padding({ left: Spacing.LG, right: Spacing.MD })
        .borderRadius(Radius.CAPSULE)
        .backgroundColor($r('sys.color.comp_background_primary'))
        .border({ width: 0.6, color: $r('sys.color.comp_divider') })
        .alignItems(VerticalAlign.Center)

        Row({ space: Spacing.SM }) {
          Row({ space: Spacing.XS }) {
            Button('全部', { type: ButtonType.Normal })
              .height(28)
              .padding({ left: Spacing.MD, right: Spacing.MD })
              .borderRadius(14)
              .fontSize(FontSize.CAPTION)
              .fontColor(this.appFilter === 'all' ? $r('sys.color.font_on_primary') : $r('sys.color.font_secondary'))
              .backgroundColor(this.appFilter === 'all' ? $r('sys.color.brand') : Color.Transparent)
              .animation({ duration: Motion.DUR_MICRO, curve: Motion.STD_CURVE })
              .onClick(() => {
                this.runAnimated(Motion.SPRING_SNAPPY, Motion.DUR_SHORT, () => { this.appFilter = 'all'; });
              })
            Button('第三方', { type: ButtonType.Normal })
              .height(28)
              .padding({ left: Spacing.MD, right: Spacing.MD })
              .borderRadius(14)
              .fontSize(FontSize.CAPTION)
              .fontColor(this.appFilter === 'user' ? $r('sys.color.font_on_primary') : $r('sys.color.font_secondary'))
              .backgroundColor(this.appFilter === 'user' ? $r('sys.color.brand') : Color.Transparent)
              .animation({ duration: Motion.DUR_MICRO, curve: Motion.STD_CURVE })
              .onClick(() => {
                this.runAnimated(Motion.SPRING_SNAPPY, Motion.DUR_SHORT, () => { this.appFilter = 'user'; });
              })
            Button('系统', { type: ButtonType.Normal })
              .height(28)
              .padding({ left: Spacing.MD, right: Spacing.MD })
              .borderRadius(14)
              .fontSize(FontSize.CAPTION)
              .fontColor(this.appFilter === 'system' ? $r('sys.color.font_on_primary') : $r('sys.color.font_secondary'))
              .backgroundColor(this.appFilter === 'system' ? $r('sys.color.brand') : Color.Transparent)
              .animation({ duration: Motion.DUR_MICRO, curve: Motion.STD_CURVE })
              .onClick(() => {
                this.runAnimated(Motion.SPRING_SNAPPY, Motion.DUR_SHORT, () => { this.appFilter = 'system'; });
              })
          }
          .padding(3)
          .borderRadius(Radius.CAPSULE)
          .backgroundColor($r('sys.color.comp_background_primary'))
          .border({ width: 0.6, color: $r('sys.color.comp_divider') })

          Button({ type: ButtonType.Capsule }) {
            Row({ space: Spacing.XS + 2 }) {
              SymbolGlyph($r('sys.symbol.plus_square'))
                .fontSize(IconSize.SM)
                .fontColor([this.running ? $r('sys.color.icon_emphasize') : $r('sys.color.icon_tertiary')])
              if (this.bp !== 'sm') {
                Text(this.installingApk ? '安装中...' : '安装 APK')
                  .fontSize(FontSize.CAPTION)
                  .fontColor(this.running ? $r('sys.color.font_emphasize') : $r('sys.color.font_tertiary'))
              }
            }
          }
          .height(ControlSize.BTN_MD)
          .padding({ left: Spacing.MD, right: Spacing.MD })
          .backgroundColor($r('sys.color.comp_background_primary'))
          .border({ width: 0.6, color: $r('sys.color.comp_divider') })
          .enabled(this.running && !this.installingApk)
          .opacity(this.running ? 1.0 : 0.55)
          .hoverEffect(HoverEffect.Highlight)
          .clickEffect({ level: ClickEffectLevel.LIGHT, scale: 0.95 })
          .onClick(() => {
            this.pickAndInstallOfflineApk();
          })

          Button({ type: ButtonType.Capsule }) {
            Row({ space: Spacing.XS + 2 }) {
              SymbolGlyph($r('sys.symbol.arrow_clockwise'))
                .fontSize(IconSize.SM)
                .fontColor([$r('sys.color.icon_primary')])
              if (this.bp !== 'sm') {
                Text('刷新')
                  .fontSize(FontSize.CAPTION)
                  .fontColor($r('sys.color.font_primary'))
              }
            }
          }
          .height(ControlSize.BTN_MD)
          .padding({ left: Spacing.MD, right: Spacing.MD })
          .backgroundColor($r('sys.color.comp_background_primary'))
          .border({ width: 0.6, color: $r('sys.color.comp_divider') })
          .enabled(this.wmAgentReady && !this.loadingApps)
          .hoverEffect(HoverEffect.Highlight)
          .clickEffect({ level: ClickEffectLevel.LIGHT, scale: 0.95 })
          .onClick(() => {
            this.refreshApps(true);
          })
        }
      }
      .width('100%')

      if (!this.wmAgentReady && this.appList.length > 0) {
        Row({ space: Spacing.SM }) {
          if (this.running) {
            LoadingProgress()
              .width(16)
              .height(16)
              .color($r('sys.color.brand'))
            Text('Android 14 正在启动，即将同步最新应用列表...')
              .fontSize(FontSize.CAPTION)
              .fontColor($r('sys.color.font_emphasize'))
          } else {
            SymbolGlyph($r('sys.symbol.info_circle'))
              .fontSize(IconSize.SM)
              .fontColor([$r('sys.color.icon_emphasize')])
            Text('离线应用列表（单容器自由多窗）：点击任意应用图标即可自动在后台启动设备并打开窗口')
              .fontSize(FontSize.CAPTION)
              .fontColor($r('sys.color.font_emphasize'))
              .layoutWeight(1)
          }
        }
        .width('100%')
        .padding({ left: Spacing.MD, right: Spacing.MD, top: Spacing.SM, bottom: Spacing.SM })
        .borderRadius(Radius.MD)
        .backgroundColor($r('sys.color.comp_emphasize_tertiary'))
      }

      if (!this.wmAgentReady && this.appList.length === 0) {
        Column({ space: Spacing.MD }) {
          if (this.running) {
            LoadingProgress()
              .width(ControlSize.BTN_MD)
              .height(ControlSize.BTN_MD)
              .color($r('sys.color.brand'))
            Text('Android 14 正在启动，应用列表即将自动同步...')
              .fontSize(FontSize.BODY_LG)
              .fontColor($r('sys.color.font_secondary'))
          } else {
            SymbolGlyph($r('sys.symbol.square_grid_2x2'))
              .fontSize(IconSize.XL)
              .fontColor([$r('sys.color.icon_tertiary')])
            Text('尚未缓存离线应用列表')
              .fontSize(FontSize.TITLE_SM)
              .fontWeight(FontWeight.Medium)
              .fontColor($r('sys.color.font_primary'))
            Text('首次启动设备完成应用同步后，关机状态下也可直接在此点击应用自动开机并打开独立窗口')
              .fontSize(FontSize.CAPTION)
              .fontColor($r('sys.color.font_tertiary'))
            Button('返回设备列表启动', { type: ButtonType.Capsule })
              .height(ControlSize.BTN_MD)
              .padding({ left: Spacing.XL, right: Spacing.XL })
              .fontSize(FontSize.BODY_MD)
              .fontColor($r('sys.color.font_on_primary'))
              .backgroundColor($r('sys.color.brand'))
              .hoverEffect(HoverEffect.Highlight)
              .clickEffect({ level: ClickEffectLevel.LIGHT, scale: 0.96 })
              .onClick(() => { this.switchNavAnimated('devices'); })
          }
        }
        .width('100%')
        .layoutWeight(1)
        .justifyContent(FlexAlign.Center)
      } else if (this.getFilteredApps().length === 0) {
        Column({ space: Spacing.SM }) {
          Text(this.loadingApps ? '正在同步应用与图标...' : '没有找到匹配的 Android 应用')
            .fontSize(FontSize.BODY_LG)
            .fontColor($r('sys.color.font_secondary'))
        }
        .width('100%')
        .layoutWeight(1)
        .justifyContent(FlexAlign.Center)
      } else {
        Grid() {
          ForEach(
            this.getFilteredApps(),
            (app: AndroidAppItem) => {
              GridItem() {
                Column({ space: Spacing.MD }) {
                  Column({ space: Spacing.SM }) {
                    if (this.iconsDir.length > 0 && app.iconFile.length > 0) {
                      Image(`file://${this.iconsDir}/${app.iconFile}`)
                        .width(56)
                        .height(56)
                        .borderRadius(Radius.MD + 2)
                        .objectFit(ImageFit.Contain)
                    } else {
                      Row() {
                        Text(app.label.length > 0 ? app.label.substring(0, 1).toUpperCase() : 'A')
                          .fontSize(IconSize.LG)
                          .fontWeight(FontWeight.Bold)
                          .fontColor($r('sys.color.font_on_primary'))
                      }
                      .width(56)
                      .height(56)
                      .borderRadius(Radius.MD + 2)
                      .backgroundColor($r('sys.color.brand'))
                      .justifyContent(FlexAlign.Center)
                      .alignItems(VerticalAlign.Center)
                    }

                    Text(app.label)
                      .fontSize(FontSize.BODY_MD)
                      .fontWeight(FontWeight.Medium)
                      .fontColor($r('sys.color.font_primary'))
                      .maxLines(1)
                      .textOverflow({ overflow: TextOverflow.Ellipsis })
                      .textAlign(TextAlign.Center)
                      .width('100%')
                  }
                  .width('100%')
                  .alignItems(HorizontalAlign.Center)
                  .onClick(() => {
                    this.openAndroidAppWindow(app.packageName, app.component, app.label, -1);
                  })

                  Row({ space: Spacing.SM }) {
                    Button({ type: ButtonType.Normal }) {
                      Row({ space: Spacing.XS }) {
                        SymbolGlyph($r('sys.symbol.screen_rotation'))
                          .fontSize(IconSize.XS)
                          .fontColor([$r('sys.color.icon_secondary')])
                        Text(this.getEffectiveModeLabel(app))
                          .fontSize(FontSize.BADGE)
                          .fontColor($r('sys.color.font_secondary'))
                      }
                    }
                    .height(ControlSize.BTN_XS)
                    .padding({ left: Spacing.SM, right: Spacing.SM })
                    .borderRadius(Radius.MD)
                    .backgroundColor($r('sys.color.background_secondary'))
                    .clickEffect({ level: ClickEffectLevel.LIGHT, scale: 0.94 })
                    .onClick(() => { this.cycleAppWindowPref(app); })

                    Button({ type: ButtonType.Normal }) {
                      Row({ space: Spacing.XS }) {
                        SymbolGlyph(this.isAppPinned(app.packageName) ? $r('sys.symbol.pin_fill') : $r('sys.symbol.pin'))
                          .fontSize(IconSize.XS)
                          .fontColor([
                            this.isAppPinned(app.packageName)
                              ? $r('sys.color.confirm')
                              : $r('sys.color.icon_emphasize')
                          ])
                        Text(this.isAppPinned(app.packageName) ? '已加桌' : '加桌')
                          .fontSize(FontSize.BADGE)
                          .fontColor(
                            this.isAppPinned(app.packageName)
                              ? $r('sys.color.confirm')
                              : $r('sys.color.font_emphasize')
                          )
                      }
                    }
                    .height(ControlSize.BTN_XS)
                    .padding({ left: Spacing.SM, right: Spacing.SM })
                    .borderRadius(Radius.MD)
                    .backgroundColor(
                      this.isAppPinned(app.packageName)
                        ? '#1F64BB5C'
                        : $r('sys.color.comp_emphasize_tertiary')
                    )
                    .clickEffect({ level: ClickEffectLevel.LIGHT, scale: 0.94 })
                    .onClick(() => { this.toggleDesktopShortcut(app); })
                  }
                  .justifyContent(FlexAlign.Center)
                }
                .width('100%')
                .padding({ left: Spacing.MD, right: Spacing.MD, top: Spacing.LG, bottom: Spacing.MD })
                .borderRadius(Radius.CARD)
                .backgroundColor($r('sys.color.comp_background_primary'))
                .border({
                  width: this.hoveredAppPkg === app.packageName ? 1.0 : 0.6,
                  color: this.hoveredAppPkg === app.packageName
                    ? $r('sys.color.brand')
                    : $r('sys.color.comp_divider')
                })
                .shadow(this.hoveredAppPkg === app.packageName ? ShadowStyle.OUTER_DEFAULT_SM : ShadowStyle.OUTER_DEFAULT_XS)
                .animation({ duration: Motion.DUR_MICRO, curve: Motion.STD_CURVE })
                .clickEffect({ level: ClickEffectLevel.LIGHT, scale: 0.97 })
                .onHover((isHover: boolean) => {
                  this.hoveredAppPkg = isHover ? app.packageName : (this.hoveredAppPkg === app.packageName ? '' : this.hoveredAppPkg);
                })
              }
              .transition(Motion.cardTransition())
            },
            (app: AndroidAppItem): string => `${app.packageName}_${this.prefEpoch}_${this.pinnedIds.length}`
          )
        }
        .columnsTemplate(this.getAppsColumnsTemplate())
        .columnsGap(Spacing.MD)
        .rowsGap(Spacing.MD)
        .padding({ top: Spacing.XS, bottom: Spacing.MD })
        .scrollBar(BarState.Auto)
        .scrollBarWidth(3)
        .scrollBarColor($r('sys.color.comp_divider'))
        .width('100%')
        .layoutWeight(1)
      }
    }
    .width('100%')
    .height('100%')
    .padding({
      left: this.bp === 'sm' ? Spacing.LG : Spacing.XL,
      right: this.bp === 'sm' ? Spacing.LG : Spacing.XL,
      top: Spacing.LG,
      bottom: Spacing.SM
    })
  }

  @Builder
  private mobileBottomTabItem(key: string, label: string, iconRes: Resource, badgeCount: number, onTap: () => void) {
    Column({ space: 2 }) {
      Stack({ alignContent: Alignment.TopEnd }) {
        SymbolGlyph(iconRes)
          .fontSize(IconSize.MD)
          .fontColor([
            this.activeNav === key
              ? $r('sys.color.icon_emphasize')
              : $r('sys.color.icon_secondary')
          ])
        if (badgeCount > 0) {
          Text(`${badgeCount}`)
            .fontSize(9)
            .fontColor($r('sys.color.font_on_primary'))
            .padding({ left: 4, right: 4, top: 1, bottom: 1 })
            .borderRadius(7)
            .backgroundColor($r('sys.color.brand'))
            .offset({ x: 8, y: -4 })
        }
      }
      Text(label)
        .fontSize(FontSize.BADGE)
        .fontWeight(this.activeNav === key ? FontWeight.Medium : FontWeight.Normal)
        .fontColor(
          this.activeNav === key
            ? $r('sys.color.font_emphasize')
            : $r('sys.color.font_secondary')
        )
    }
    .layoutWeight(1)
    .height('100%')
    .justifyContent(FlexAlign.Center)
    .alignItems(HorizontalAlign.Center)
    .clickEffect({ level: ClickEffectLevel.LIGHT, scale: 0.94 })
    .onClick(onTap)
  }

  build() {
    Column({ space: 0 }) {
      // 1. Integrated HarmonyOS Title Bar (adapts cleanly across Phone / Tablet / PC)
      // Anchor for Global Settings Half-Modal Sheet
      Row({ space: Spacing.SM }) {
        Row({ space: Spacing.SM }) {
          Stack({ alignContent: Alignment.Center }) {
            Column()
              .width(28)
              .height(28)
              .borderRadius(Radius.SM)
              .backgroundColor($r('sys.color.brand'))
            SymbolGlyph($r('sys.symbol.pad_fill'))
              .fontSize(IconSize.SM)
              .fontColor([$r('sys.color.icon_on_primary')])
          }
          Text('svbox Android')
            .fontSize(FontSize.TITLE_SM)
            .fontWeight(FontWeight.Bold)
            .fontColor($r('sys.color.font_primary'))
            .maxLines(1)
            .textOverflow({ overflow: TextOverflow.Ellipsis })

          if (this.bp !== 'sm') {
            Text(getDeviceFormLabel())
              .fontSize(FontSize.BADGE)
              .fontWeight(FontWeight.Medium)
              .fontColor($r('sys.color.font_emphasize'))
              .padding({ left: Spacing.SM, right: Spacing.SM, top: 2, bottom: 2 })
              .borderRadius(Radius.BADGE)
              .backgroundColor($r('sys.color.comp_emphasize_tertiary'))
          }
        }
        .alignItems(VerticalAlign.Center)
        .layoutWeight(1)

        if (this.running && this.bp === 'lg') {
          Row({ space: Spacing.SM }) {
            Circle({ width: 7, height: 7 })
              .fill(this.paused ? $r('sys.color.alert') : $r('sys.color.confirm'))
            Text(`${this.refreshRate}Hz · CPU ${Math.round(this.cpuPercent)}% · PSS ${this.memMb} MB` +
              (this.hostMemMb > 0 ? ` (+宿主 ${this.hostMemMb} MB)` : ''))
              .fontSize(FontSize.MICRO)
              .fontColor($r('sys.color.font_secondary'))
          }
          .padding({ left: Spacing.MD, right: Spacing.MD, top: Spacing.XS + 1, bottom: Spacing.XS + 1 })
          .borderRadius(Radius.MD)
          .backgroundColor($r('sys.color.background_secondary'))
          .transition(Motion.pillPopTransition())
        }
      }
      .width('100%')
      .height(ControlSize.TITLE_BAR_H)
      .padding({ left: Spacing.LG, right: Math.max(152, this.decorRightAvoidVp) })
      .backgroundColor($r('sys.color.comp_background_primary'))
      .border({ width: { bottom: 0.6 }, color: $r('sys.color.comp_divider') })
      .alignItems(VerticalAlign.Center)
      .bindSheet($$this.showGlobalSettingsSheet, this.globalSettingsSheetBuilder(), {
        preferType: this.computeSheetType(),
        width: this.computeSheetWidth(580),
        height: this.bp === 'sm' ? SheetSize.LARGE : 520,
        blurStyle: BlurStyle.COMPONENT_ULTRA_THICK,
        dragBar: this.bp === 'sm',
        showClose: true,
        title: { title: '设置中心', subtitle: 'svbox Android · HarmonyOS' },
        shadow: ShadowStyle.OUTER_DEFAULT_LG
      })

      // 2. Main Responsive Workspace (Left Rail on md/lg, Full Width on sm)
      // Anchor for Create Instance Half-Modal Sheet (permanent across all breakpoints)
      Row({ space: 0 }) {
        if (this.bp !== 'sm') {
          Column({ space: 0 }) {
            Stack({ alignContent: Alignment.TopStart }) {
              Column()
                .width('100%')
                .height(NAV_RAIL_ITEM_H)
                .borderRadius(Radius.MD)
                .backgroundColor($r('sys.color.comp_emphasize_tertiary'))
                .translate({ y: this.activeNav === 'devices' ? 0 : (NAV_RAIL_ITEM_H + NAV_RAIL_ITEM_GAP) })
                .animation({ duration: Motion.DUR_SHORT, curve: Motion.SPRING_SNAPPY })

              Column({ space: NAV_RAIL_ITEM_GAP }) {
                this.navRailItem('devices', '设备', $r('sys.symbol.display'), 0)
                this.navRailItem('apps', '应用', $r('sys.symbol.square_grid_2x2'), this.appList.length)
              }
              .width('100%')
            }
            .width('100%')

            Blank()

            Column({ space: Spacing.XS }) {
              Button({ type: ButtonType.Circle }) {
                SymbolGlyph($r('sys.symbol.gearshape'))
                  .fontSize(IconSize.MD)
                  .fontColor([$r('sys.color.icon_secondary')])
              }
              .width(ControlSize.BTN_LG)
              .height(ControlSize.BTN_LG)
              .backgroundColor(Color.Transparent)
              .hoverEffect(HoverEffect.Highlight)
              .clickEffect({ level: ClickEffectLevel.LIGHT, scale: 0.92 })
              .onClick(() => {
                this.openGlobalSettingsSheet();
              })

              Text('设置')
                .fontSize(FontSize.MICRO)
                .fontColor($r('sys.color.font_tertiary'))
            }
            .alignItems(HorizontalAlign.Center)
            .padding({ bottom: Spacing.XS })
          }
          .width(this.bp === 'md' ? 72 : ControlSize.NAV_RAIL_W)
          .height('100%')
          .padding({ left: Spacing.SM, right: Spacing.SM, top: Spacing.LG, bottom: Spacing.MD })
          .backgroundColor($r('sys.color.comp_background_primary'))
          .border({ width: { right: 0.6 }, color: $r('sys.color.comp_divider') })
        }

        // Right Workspace Canvas with Page Switch Motion
        // Anchor for Device Settings Half-Modal Sheet
        Stack() {
          if (this.activeNav === 'devices') {
            Column() {
              this.devicesWorkspace()
            }
            .width('100%')
            .height('100%')
            .transition(Motion.pageSwitchTransition())
          } else {
            Column() {
              this.appsWorkspace()
            }
            .width('100%')
            .height('100%')
            .transition(Motion.pageSwitchTransition())
          }
        }
        .layoutWeight(1)
        .height('100%')
        .backgroundColor($r('sys.color.background_secondary'))
        .bindSheet($$this.showDeviceSettingsSheet, this.deviceSettingsSheetBuilder(), {
          preferType: this.computeSheetType(),
          width: this.computeSheetWidth(760),
          height: this.bp === 'sm' ? SheetSize.LARGE : 580,
          blurStyle: BlurStyle.COMPONENT_ULTRA_THICK,
          dragBar: this.bp === 'sm',
          showClose: true,
          title: {
            title: '设备设置',
            subtitle: this.editingInstance ? `${this.editingInstance.name} (#${this.editingInstance.index})` : ''
          },
          shadow: ShadowStyle.OUTER_DEFAULT_LG
        })
      }
      .width('100%')
      .layoutWeight(1)
      .bindSheet($$this.showCreateSheet, this.createSheetBuilder(), {
        preferType: this.computeSheetType(),
        width: this.computeSheetWidth(620),
        height: this.bp === 'sm' ? SheetSize.LARGE : 590,
        blurStyle: BlurStyle.COMPONENT_ULTRA_THICK,
        dragBar: this.bp === 'sm',
        showClose: true,
        title: { title: '新建 Android 设备', subtitle: '支持批量创建，每台设备拥有独立隔离数据分区' },
        shadow: ShadowStyle.OUTER_DEFAULT_LG
      })

      // 3. Bottom Bar: HarmonyOS Mobile Bottom Navigation (sm) OR Desktop Status Footer (md/lg)
      // Anchor for Backup & Restore Half-Modal Sheet
      if (this.bp === 'sm') {
        Row() {
          this.mobileBottomTabItem('devices', '设备', $r('sys.symbol.display'), 0, () => {
            this.switchNavAnimated('devices');
          })
          this.mobileBottomTabItem('apps', '应用', $r('sys.symbol.square_grid_2x2'), this.appList.length, () => {
            this.switchNavAnimated('apps');
          })
          this.mobileBottomTabItem('settings', '设置', $r('sys.symbol.gearshape'), 0, () => {
            this.openGlobalSettingsSheet();
          })
        }
        .width('100%')
        .height(56)
        .backgroundColor($r('sys.color.comp_background_primary'))
        .border({ width: { top: 0.6 }, color: $r('sys.color.comp_divider') })
        .bindSheet($$this.showBackupSheet, this.backupSheetBuilder(), {
          preferType: SheetType.BOTTOM,
          width: '100%',
          height: SheetSize.LARGE,
          blurStyle: BlurStyle.COMPONENT_ULTRA_THICK,
          dragBar: true,
          showClose: true,
          title: {
            title: '备份与还原',
            subtitle: this.editingInstance ? `${this.editingInstance.name} (#${this.editingInstance.index})` : ''
          },
          shadow: ShadowStyle.OUTER_DEFAULT_LG
        })
      } else {
        Row({ space: Spacing.MD }) {
          Text(`共 ${this.instances.length} 台设备 · ${this.running ? '1 台运行中' : '全部已停止'}`)
            .fontSize(FontSize.MICRO)
            .fontColor($r('sys.color.font_secondary'))

          if (this.cloningBusy) {
            Row({ space: Spacing.XS + 2 }) {
              LoadingProgress().width(14).height(14).color($r('sys.color.brand'))
              Text('正在复制设备数据...')
                .fontSize(FontSize.MICRO)
                .fontColor($r('sys.color.font_emphasize'))
            }
            .transition(Motion.pillPopTransition())
          } else if (this.statusText.length > 0) {
            Text(`· ${this.statusText === 'Idle' ? '就绪' : this.statusText}`)
              .fontSize(FontSize.MICRO)
              .fontColor($r('sys.color.font_tertiary'))
              .maxLines(1)
              .textOverflow({ overflow: TextOverflow.Ellipsis })
              .layoutWeight(1)
          } else {
            Blank()
          }

          if (this.bp === 'lg') {
            Text(this.gpuRenderer.length > 0 ? this.gpuRenderer : 'Maleoon Vulkan · svbox Runtime')
              .fontSize(FontSize.MICRO)
              .fontColor($r('sys.color.font_tertiary'))
              .maxLines(1)
          }
        }
        .width('100%')
        .height(ControlSize.FOOTER_BAR_H)
        .padding({ left: Spacing.LG, right: Spacing.LG })
        .backgroundColor($r('sys.color.comp_background_primary'))
        .border({ width: { top: 0.6 }, color: $r('sys.color.comp_divider') })
        .alignItems(VerticalAlign.Center)
        .bindSheet($$this.showBackupSheet, this.backupSheetBuilder(), {
          preferType: SheetType.CENTER,
          width: this.computeSheetWidth(540),
          height: 520,
          blurStyle: BlurStyle.COMPONENT_ULTRA_THICK,
          dragBar: false,
          showClose: true,
          title: {
            title: '备份与还原',
            subtitle: this.editingInstance ? `${this.editingInstance.name} (#${this.editingInstance.index})` : ''
          },
          shadow: ShadowStyle.OUTER_DEFAULT_LG
        })
      }
    }
    .width('100%')
    .height('100%')
    .backgroundColor($r('sys.color.background_secondary'))
    .onAreaChange((_oldVal: Area, newVal: Area) => {
      this.updateBreakpoint(Number(newVal.width));
    })
  }
}
