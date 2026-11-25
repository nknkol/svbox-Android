import { common, Want } from '@kit.AbilityKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import native, { RuntimeStatus } from 'libentry.so';
import {
  AndroidAppItem,
  AppWindowPref,
  appWindowPrefs,
  computeShortcutId,
  findPresetByKey,
  ResolutionPresetItem
} from '../common/AppWindowPrefs';
import {
  pinAndroidAppToDesktop,
  queryPinnedShortcutIds,
  removeAndroidAppShortcut,
  ShortcutOpResult
} from '../common/DesktopShortcut';
import { arrangeOpenWindows } from '../common/WindowHelper';
import {
  CreateInstanceOptions,
  defaultGlobalSettings,
  DeviceInstance,
  formatGpuModeLabel,
  GlobalSettings,
  instanceManager
} from '../model/InstanceModel';
import { CreateInstanceDialog } from '../components/CreateInstanceDialog';
import { DeviceSettingsDialog } from '../components/DeviceSettingsDialog';
import { RenameDialog } from '../components/RenameDialog';
import { BackupRestoreDialog } from '../components/BackupRestoreDialog';
import { GlobalSettingsDialog } from '../components/GlobalSettingsDialog';

const TAG: string = 'SvboxMain';

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

  // Batch Mode
  @State private batchMode: boolean = false;
  @State private selectedIds: string[] = [];

  // Live Runtime State
  @State private running: boolean = false;
  @State private paused: boolean = false;
  @State private wmAgentReady: boolean = false;
  @State private activeInstanceId: string = 'inst_0';
  @State private refreshRate: number = 120;
  @State private fps: number = 0;
  @State private cpuPercent: number = 0;
  @State private memMb: number = 0;
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

  // Dialog Controllers (owned by @Component struct)
  private createDialogCtrl: CustomDialogController | null = null;
  private settingsDialogCtrl: CustomDialogController | null = null;
  private renameDialogCtrl: CustomDialogController | null = null;
  private backupDialogCtrl: CustomDialogController | null = null;
  private globalDialogCtrl: CustomDialogController | null = null;

  private pollTimer: number = -1;
  private lastLaunchSeq: number = 0;
  private appsFetchedOnce: boolean = false;
  private svboxPath: string = '/storage/Users/currentUser/Workspace/svbox/build/svbox';
  private androidDir: string = '/storage/Users/currentUser/Workspace/svbox-Android';

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

  private reloadInstances(): void {
    const filesDir: string = this.getSandboxFilesDir();
    this.instances = instanceManager.getSorted(filesDir, this.sortBy);
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
    this.reloadInstances();

    this.iconsDir = native.getIconsDir();
    this.lastLaunchSeq = AppStorage.get<number>('launchSeq') ?? 0;
    this.refreshPinnedShortcuts();

    // Apply CLI / Want launch overrides onto primary instance #0 if provided
    const primary: DeviceInstance | undefined = this.getInstanceById('inst_0') ?? this.instances[0];
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
      if (dirty) {
        instanceManager.updateInstance(filesDir, primary);
        this.reloadInstances();
      }
      AppStorage.setOrCreate<number>('refreshRate', primary.refreshRate);

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
          instanceManager.updateInstance(this.getSandboxFilesDir(), target);
          this.reloadInstances();

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
      this.running = st.running;
      this.paused = st.paused;
      this.wmAgentReady = st.wmAgentReady;
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

  private async refreshApps(forceExtractIcons: boolean): Promise<void> {
    if (!this.wmAgentReady || this.loadingApps) return;
    this.loadingApps = true;
    try {
      this.iconsDir = native.getIconsDir();
      const raw: string = await native.wmRpcAsync(forceExtractIcons ? 'LIST_APPS 1' : 'LIST_APPS 0');
      const rep: ListAppsReply = JSON.parse(raw) as ListAppsReply;
      if (rep.ok && rep.apps) {
        this.appList = rep.apps;
        AppStorage.setOrCreate<AndroidAppItem[]>('androidApps', rep.apps);
      }
    } catch (err) {
      hilog.warn(0x0000, TAG, 'refreshApps error: %{public}s', String(err));
    } finally {
      this.loadingApps = false;
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
        }
      }
    } catch (_e) {
    }
  }

  private async openAndroidAppWindow(pkg: string, comp: string, label: string, moveTaskId: number): Promise<void> {
    try {
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
    // If another instance is currently running, ask user before switching
    if (this.running && this.activeInstanceId !== inst.id) {
      const runningInst: DeviceInstance | undefined = this.getInstanceById(this.activeInstanceId);
      const curName: string = runningInst ? runningInst.name : '当前设备';
      this.getUIContext().showAlertDialog({
        title: '切换运行设备',
        message: `「${curName}」正在运行中。停止当前设备并启动「${inst.name}」吗？`,
        primaryButton: {
          value: '取消',
          action: () => {}
        },
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
        inst.adbPort
      );
      native.setResourceConfig(inst.profile, '', inst.memLimitMb);
      native.setGpuMode(inst.gpuMode);
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
      this.running = false;
      this.paused = false;
      this.wmAgentReady = false;
      this.appsFetchedOnce = false;
      this.appList = [];
      AppStorage.setOrCreate<AndroidAppItem[]>('androidApps', []);
      this.showToast('设备已关闭');
    } catch (e) {
      this.showToast(`停止失败：${String(e)}`);
    }
  }

  // --- Dialog Openers ---
  private openCreateDialog(): void {
    this.createDialogCtrl = new CustomDialogController({
      builder: CreateInstanceDialog({
        onConfirm: (opts: CreateInstanceOptions) => {
          const created: DeviceInstance[] = instanceManager.createInstances(this.getSandboxFilesDir(), opts);
          this.reloadInstances();
          this.showToast(`已成功创建 ${created.length} 台新设备`);
        }
      }),
      alignment: DialogAlignment.Center,
      autoCancel: true,
      customStyle: true
    });
    this.createDialogCtrl.open();
  }

  private openDeviceSettingsDialog(inst: DeviceInstance): void {
    this.settingsDialogCtrl = new CustomDialogController({
      builder: DeviceSettingsDialog({
        instance: inst,
        filesDir: this.getSandboxFilesDir(),
        isInstanceRunning: this.isInstanceRunning(inst),
        onSave: (updated: DeviceInstance) => {
          instanceManager.updateInstance(this.getSandboxFilesDir(), updated);
          this.reloadInstances();
          if (this.isInstanceRunning(updated)) {
            try {
              native.setRefreshRate(updated.refreshRate);
            } catch (_e) {
            }
          }
          this.showToast(`已保存「${updated.name}」设置`);
        },
        onToast: (msg: string) => {
          this.showToast(msg);
        }
      }),
      alignment: DialogAlignment.Center,
      autoCancel: true,
      customStyle: true
    });
    this.settingsDialogCtrl.open();
  }

  private openRenameDialog(inst: DeviceInstance): void {
    this.renameDialogCtrl = new CustomDialogController({
      builder: RenameDialog({
        initialName: inst.name,
        onConfirm: (newName: string) => {
          instanceManager.renameInstance(this.getSandboxFilesDir(), inst.id, newName);
          this.reloadInstances();
          this.showToast(`已重命名为「${newName}」`);
        }
      }),
      alignment: DialogAlignment.Center,
      autoCancel: true,
      customStyle: true
    });
    this.renameDialogCtrl.open();
  }

  private openBackupDialog(inst: DeviceInstance): void {
    this.backupDialogCtrl = new CustomDialogController({
      builder: BackupRestoreDialog({
        instance: inst,
        filesDir: this.getSandboxFilesDir(),
        backupDir: this.globalSettings.backupDir,
        isInstanceRunning: this.isInstanceRunning(inst),
        onFinished: (msg: string) => {
          this.showToast(msg);
        }
      }),
      alignment: DialogAlignment.Center,
      autoCancel: false,
      customStyle: true
    });
    this.backupDialogCtrl.open();
  }

  private openGlobalSettingsDialog(): void {
    this.globalDialogCtrl = new CustomDialogController({
      builder: GlobalSettingsDialog({
        initialSettings: this.globalSettings,
        onSave: (next: GlobalSettings) => {
          this.globalSettings = next;
          instanceManager.saveGlobalSettings(this.getSandboxFilesDir(), next);
          this.showToast('全局设置已保存');
        }
      }),
      alignment: DialogAlignment.Center,
      autoCancel: true,
      customStyle: true
    });
    this.globalDialogCtrl.open();
  }

  private async cloneDeviceInstance(inst: DeviceInstance): Promise<void> {
    if (this.cloningBusy) return;
    this.cloningBusy = true;
    this.showToast(`正在复制「${inst.name}」的数据分区...`);
    const filesDir: string = this.getSandboxFilesDir();
    const copy: DeviceInstance = instanceManager.addClonedInstance(filesDir, inst);
    try {
      const ok: boolean = await native.cloneInstanceAsync(filesDir, inst.dataSubdir, copy.dataSubdir);
      this.reloadInstances();
      this.showToast(ok ? `已复制生成「${copy.name}」` : `已创建「${copy.name}」（空数据）`);
    } catch (e) {
      this.reloadInstances();
      this.showToast(`复制完成但有警告：${String(e)}`);
    } finally {
      this.cloningBusy = false;
    }
  }

  private cleanDeviceDisk(inst: DeviceInstance): void {
    try {
      const freed: number = native.cleanInstanceCache(this.getSandboxFilesDir(), inst.dataSubdir);
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
      primaryButton: {
        value: '取消',
        action: () => {}
      },
      secondaryButton: {
        value: '确认删除',
        action: () => {
          const filesDir: string = this.getSandboxFilesDir();
          native.deleteInstanceData(filesDir, inst.dataSubdir);
          instanceManager.deleteInstance(filesDir, inst.id);
          this.reloadInstances();
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
    this.sortBy = nextSort;
    this.globalSettings.sortBy = nextSort;
    instanceManager.saveGlobalSettings(this.getSandboxFilesDir(), this.globalSettings);
    this.reloadInstances();
  }

  private updateViewMode(nextMode: string): void {
    this.viewMode = nextMode;
    this.globalSettings.viewMode = nextMode;
    instanceManager.saveGlobalSettings(this.getSandboxFilesDir(), this.globalSettings);
  }

  // --- Batch Actions ---
  private batchCleanSelected(): void {
    if (this.selectedIds.length === 0) {
      this.showToast('请先勾选设备');
      return;
    }
    let totalFreed: number = 0;
    const filesDir: string = this.getSandboxFilesDir();
    for (let i: number = 0; i < this.selectedIds.length; i++) {
      const inst: DeviceInstance | undefined = this.getInstanceById(this.selectedIds[i]);
      if (inst) {
        totalFreed += native.cleanInstanceCache(filesDir, inst.dataSubdir);
      }
    }
    this.showToast(`已批量清理 ${this.selectedIds.length} 台设备，共释放 ${totalFreed.toFixed(1)} MB`);
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
          this.selectedIds = [];
          this.batchMode = false;
          this.reloadInstances();
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
    if (pref.mode === 'adaptive') return '自由';
    if (pref.mode === 'fixed_landscape') return '横屏';
    return '竖屏';
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

  // --- Builders ---
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
        .onClick(() => { this.openDeviceSettingsDialog(inst); })
      MenuItem({ content: '打开模拟器窗口' })
        .onClick(() => {
          if (!this.isInstanceRunning(inst)) {
            this.startDeviceInstance(inst, 'full');
          } else {
            this.openEmulatorWindow(inst);
          }
        })
      MenuItem({ content: '磁盘清理' })
        .onClick(() => { this.cleanDeviceDisk(inst); })
      MenuItem({ content: '重命名' })
        .onClick(() => { this.openRenameDialog(inst); })
      MenuItem({ content: '复制设备' })
        .onClick(() => { this.cloneDeviceInstance(inst); })
      MenuItem({ content: '备份与还原' })
        .onClick(() => { this.openBackupDialog(inst); })
      MenuItem({ content: '删除设备' })
        .enabled(inst.id !== 'inst_0' && !this.isInstanceRunning(inst))
        .onClick(() => { this.confirmDeleteDevice(inst); })
    }
  }

  @Builder
  private navRailItem(key: string, label: string, iconRes: Resource, badgeCount: number) {
    Column({ space: 4 }) {
      Stack({ alignContent: Alignment.TopEnd }) {
        Stack({ alignContent: Alignment.Center }) {
          Column()
            .width(44)
            .height(36)
            .borderRadius(12)
            .backgroundColor(
              this.activeNav === key
                ? $r('sys.color.comp_emphasize_tertiary')
                : Color.Transparent
            )
          SymbolGlyph(iconRes)
            .fontSize(20)
            .fontColor([
              this.activeNav === key
                ? $r('sys.color.icon_emphasize')
                : $r('sys.color.icon_secondary')
            ])
        }

        if (badgeCount > 0) {
          Text(`${badgeCount}`)
            .fontSize(9)
            .fontWeight(FontWeight.Medium)
            .fontColor($r('sys.color.font_on_primary'))
            .padding({ left: 5, right: 5, top: 1, bottom: 1 })
            .borderRadius(8)
            .backgroundColor($r('sys.color.brand'))
            .offset({ x: 4, y: -2 })
        }
      }

      Text(label)
        .fontSize(11)
        .fontWeight(this.activeNav === key ? FontWeight.Medium : FontWeight.Normal)
        .fontColor(
          this.activeNav === key
            ? $r('sys.color.font_emphasize')
            : $r('sys.color.font_secondary')
        )
    }
    .width('100%')
    .padding({ top: 6, bottom: 6 })
    .borderRadius(12)
    .stateStyles({
      pressed: { .backgroundColor($r('sys.color.interactive_pressed')) }
    })
    .onClick(() => {
      this.activeNav = key;
      if (key === 'apps' && this.wmAgentReady && this.appList.length === 0) {
        this.refreshApps(false);
      }
    })
  }

  @Builder
  private specChip(label: string) {
    Text(label)
      .fontSize(11)
      .fontColor($r('sys.color.font_secondary'))
      .padding({ left: 8, right: 8, top: 3, bottom: 3 })
      .borderRadius(6)
      .backgroundColor($r('sys.color.background_secondary'))
  }

  @Builder
  private deviceCardRow(inst: DeviceInstance) {
    Row({ space: 16 }) {
      if (this.batchMode) {
        Checkbox()
          .select(this.isSelected(inst.id))
          .selectedColor($r('sys.color.brand'))
          .width(20)
          .height(20)
          .onChange(() => {
            this.toggleSelection(inst.id);
          })
      }

      // MuMu-Style Live/Idle Preview Thumbnail (124 x 74 vp)
      Stack({ alignContent: Alignment.TopStart }) {
        Column({ space: 2 }) {
          SymbolGlyph(inst.deviceType === 'tablet' ? $r('sys.symbol.pad_fill') : $r('sys.symbol.phone_1'))
            .fontSize(24)
            .fontColor([
              this.isInstanceRunning(inst) ? '#FFFFFF' : '#CBD5E1'
            ])
        }
        .width('100%')
        .height('100%')
        .justifyContent(FlexAlign.Center)
        .alignItems(HorizontalAlign.Center)
        .borderRadius(12)
        .linearGradient({
          angle: 135,
          colors: this.isInstanceRunning(inst)
            ? [['#0A59F7', 0.0], ['#38BDF8', 1.0]]
            : [['#2B3242', 0.0], ['#1B202C', 1.0]]
        })

        // Top-left #Index pill
        Text(`# ${inst.index}`)
          .fontSize(10)
          .fontWeight(FontWeight.Medium)
          .fontColor('#FFFFFF')
          .padding({ left: 6, right: 6, top: 2, bottom: 2 })
          .borderRadius(6)
          .backgroundColor('#66000000')
          .margin({ left: 6, top: 6 })

        // Bottom-left status pill
        Row({ space: 4 }) {
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
            .fontSize(10)
            .fontColor('#FFFFFF')
        }
        .padding({ left: 6, right: 7, top: 2, bottom: 2 })
        .borderRadius(6)
        .backgroundColor('#7A000000')
        .position({ x: 6, y: 50 })
      }
      .width(124)
      .height(74)
      .flexShrink(0)
      .onClick(() => {
        if (this.isInstanceRunning(inst)) {
          this.openEmulatorWindow(inst);
        }
      })

      // Middle: Instance Title + Specs + Metadata
      Column({ space: 7 }) {
        Row({ space: 8 }) {
          Text(inst.name)
            .fontSize(15)
            .fontWeight(FontWeight.Bold)
            .fontColor($r('sys.color.font_primary'))
            .maxLines(1)
            .textOverflow({ overflow: TextOverflow.Ellipsis })
            .constraintSize({ maxWidth: 260 })

          if (this.isInstanceRunning(inst)) {
            Row({ space: 5 }) {
              Circle({ width: 6, height: 6 })
                .fill(this.paused ? $r('sys.color.alert') : $r('sys.color.confirm'))
              Text(this.paused ? '已暂停' : (this.wmAgentReady ? '运行中' : '启动中'))
                .fontSize(11)
                .fontWeight(FontWeight.Medium)
                .fontColor(this.paused ? $r('sys.color.alert') : $r('sys.color.confirm'))
            }
            .padding({ left: 8, right: 8, top: 2, bottom: 2 })
            .borderRadius(10)
            .backgroundColor(this.paused ? '#1FED6F21' : '#1F64BB5C')
          }

          Text(inst.runMode === 'emulator' ? '传统模拟器' : '自由多窗')
            .fontSize(10)
            .fontWeight(FontWeight.Medium)
            .fontColor($r('sys.color.font_emphasize'))
            .padding({ left: 7, right: 7, top: 2, bottom: 2 })
            .borderRadius(6)
            .backgroundColor($r('sys.color.comp_emphasize_tertiary'))
        }
        .alignItems(VerticalAlign.Center)

        // Spec Chips Row
        Row({ space: 6 }) {
          this.specChip(inst.androidVersion)
          this.specChip(`${inst.cpuCores} 核`)
          this.specChip(`${Math.round(inst.memLimitMb / 1024)} GB`)
          this.specChip(formatGpuModeLabel(inst.gpuMode))
          this.specChip(`${inst.refreshRate}Hz`)
        }

        // Subtitle Line
        Text(
          `${inst.width}×${inst.height} · ${inst.dpi} DPI` +
          (this.isInstanceRunning(inst)
            ? ` · ${this.fps} FPS · ${this.activeProcs} 进程 · 内存 ${this.memMb} MB`
            : ` · 分区 ${inst.dataSubdir}`)
        )
          .fontSize(12)
          .fontColor($r('sys.color.font_tertiary'))
          .maxLines(1)
          .textOverflow({ overflow: TextOverflow.Ellipsis })
      }
      .layoutWeight(1)
      .alignItems(HorizontalAlign.Start)

      // Right: MuMu-Style Circular Action Group + HarmonyOS Polish
      Row({ space: 8 }) {
        if (this.isInstanceRunning(inst)) {
          Button({ type: ButtonType.Capsule }) {
            Row({ space: 5 }) {
              SymbolGlyph(inst.runMode === 'emulator' ? $r('sys.symbol.display') : $r('sys.symbol.square_grid_2x2'))
                .fontSize(14)
                .fontColor([$r('sys.color.font_on_primary')])
              Text(inst.runMode === 'emulator' ? '打开窗口' : '应用中心')
                .fontSize(12)
                .fontWeight(FontWeight.Medium)
                .fontColor($r('sys.color.font_on_primary'))
            }
          }
          .height(36)
          .padding({ left: 14, right: 14 })
          .backgroundColor($r('sys.color.brand'))
          .onClick(() => {
            if (inst.runMode === 'emulator') {
              this.openEmulatorWindow(inst);
            } else {
              this.activeNav = 'apps';
            }
          })

          Button({ type: ButtonType.Circle }) {
            SymbolGlyph(this.paused ? $r('sys.symbol.play_fill') : $r('sys.symbol.pause_fill'))
              .fontSize(16)
              .fontColor([this.paused ? $r('sys.color.confirm') : $r('sys.color.alert')])
          }
          .width(36)
          .height(36)
          .backgroundColor($r('sys.color.comp_background_tertiary'))
          .onClick(() => {
            this.statusText = native.setPaused(!this.paused);
          })

          Button({ type: ButtonType.Circle }) {
            SymbolGlyph($r('sys.symbol.power'))
              .fontSize(16)
              .fontColor([$r('sys.color.warning')])
          }
          .width(36)
          .height(36)
          .backgroundColor('#1AE84026')
          .onClick(() => {
            this.stopActiveInstance();
          })
        } else {
          Button({ type: ButtonType.Circle }) {
            SymbolGlyph($r('sys.symbol.play_fill'))
              .fontSize(17)
              .fontColor([$r('sys.color.icon_on_primary')])
          }
          .width(40)
          .height(40)
          .backgroundColor($r('sys.color.brand'))
          .onClick(() => {
            this.startDeviceInstance(inst, 'full');
          })
        }

        // More (...) Menu Button
        Button({ type: ButtonType.Circle }) {
          SymbolGlyph($r('sys.symbol.dot_grid_1x2'))
            .fontSize(16)
            .fontColor([$r('sys.color.icon_secondary')])
        }
        .width(36)
        .height(36)
        .backgroundColor($r('sys.color.comp_background_tertiary'))
        .bindMenu(this.deviceMoreMenuBuilder(inst))
      }
      .alignItems(VerticalAlign.Center)
    }
    .width('100%')
    .padding({ left: 16, right: 16, top: 14, bottom: 14 })
    .borderRadius(16)
    .backgroundColor($r('sys.color.comp_background_primary'))
    .border({
      width: this.isInstanceRunning(inst) ? 1.2 : 0.6,
      color: this.isInstanceRunning(inst) ? $r('sys.color.brand') : $r('sys.color.comp_divider')
    })
    .shadow({ radius: 10, color: '#0A000000', offsetY: 2 })
  }

  @Builder
  private deviceCompactRow(inst: DeviceInstance) {
    Row({ space: 12 }) {
      if (this.batchMode) {
        Checkbox()
          .select(this.isSelected(inst.id))
          .selectedColor($r('sys.color.brand'))
          .width(18)
          .height(18)
          .onChange(() => {
            this.toggleSelection(inst.id);
          })
      }

      Text(`#${inst.index}`)
        .fontSize(12)
        .fontWeight(FontWeight.Medium)
        .fontColor($r('sys.color.font_secondary'))
        .width(30)

      Circle({ width: 8, height: 8 })
        .fill(this.isInstanceRunning(inst) ? $r('sys.color.confirm') : $r('sys.color.font_fourth'))

      Text(inst.name)
        .fontSize(14)
        .fontWeight(FontWeight.Medium)
        .fontColor($r('sys.color.font_primary'))
        .layoutWeight(1)
        .maxLines(1)
        .textOverflow({ overflow: TextOverflow.Ellipsis })

      Text(`${inst.androidVersion} · ${inst.cpuCores}核 ${Math.round(inst.memLimitMb / 1024)}G · ${formatGpuModeLabel(inst.gpuMode)} · ${inst.refreshRate}Hz`)
        .fontSize(12)
        .fontColor($r('sys.color.font_tertiary'))

      if (this.isInstanceRunning(inst)) {
        Button('打开', { type: ButtonType.Capsule })
          .height(30)
          .padding({ left: 12, right: 12 })
          .fontSize(12)
          .fontColor($r('sys.color.font_on_primary'))
          .backgroundColor($r('sys.color.brand'))
          .onClick(() => {
            this.openEmulatorWindow(inst);
          })
        Button({ type: ButtonType.Circle }) {
          SymbolGlyph($r('sys.symbol.power'))
            .fontSize(14)
            .fontColor([$r('sys.color.warning')])
        }
        .width(30)
        .height(30)
        .backgroundColor('#1AE84026')
        .onClick(() => { this.stopActiveInstance(); })
      } else {
        Button({ type: ButtonType.Circle }) {
          SymbolGlyph($r('sys.symbol.play_fill'))
            .fontSize(14)
            .fontColor([$r('sys.color.icon_on_primary')])
        }
        .width(30)
        .height(30)
        .backgroundColor($r('sys.color.brand'))
        .onClick(() => { this.startDeviceInstance(inst, 'full'); })
      }

      Button({ type: ButtonType.Circle }) {
        SymbolGlyph($r('sys.symbol.dot_grid_1x2'))
          .fontSize(14)
          .fontColor([$r('sys.color.icon_secondary')])
      }
      .width(30)
      .height(30)
      .backgroundColor($r('sys.color.comp_background_tertiary'))
      .bindMenu(this.deviceMoreMenuBuilder(inst))
    }
    .width('100%')
    .height(54)
    .padding({ left: 16, right: 14 })
    .borderRadius(12)
    .backgroundColor($r('sys.color.comp_background_primary'))
    .border({ width: 0.6, color: $r('sys.color.comp_divider') })
  }

  @Builder
  private devicesWorkspace() {
    Column({ space: 14 }) {
      // MuMu-Style Action Toolbar (Normal Mode vs Batch Mode)
      if (!this.batchMode) {
        Row({ space: 10 }) {
          // Primary CTA: + 新建设备
          Button({ type: ButtonType.Capsule }) {
            Row({ space: 6 }) {
              SymbolGlyph($r('sys.symbol.plus'))
                .fontSize(15)
                .fontColor([$r('sys.color.icon_on_primary')])
              Text('新建设备')
                .fontSize(13)
                .fontWeight(FontWeight.Medium)
                .fontColor($r('sys.color.font_on_primary'))
            }
          }
          .height(36)
          .padding({ left: 16, right: 18 })
          .backgroundColor($r('sys.color.brand'))
          .onClick(() => {
            this.openCreateDialog();
          })

          // Batch Operations Button
          Button({ type: ButtonType.Capsule }) {
            Row({ space: 6 }) {
              SymbolGlyph($r('sys.symbol.checkmark_square'))
                .fontSize(14)
                .fontColor([$r('sys.color.icon_primary')])
              Text('批量操作')
                .fontSize(13)
                .fontColor($r('sys.color.font_primary'))
            }
          }
          .height(36)
          .padding({ left: 14, right: 16 })
          .backgroundColor($r('sys.color.comp_background_primary'))
          .border({ width: 0.6, color: $r('sys.color.comp_divider') })
          .onClick(() => {
            this.batchMode = true;
            this.selectedIds = [];
          })

          // Arrange Windows Button
          Button({ type: ButtonType.Capsule }) {
            Row({ space: 6 }) {
              SymbolGlyph($r('sys.symbol.rectangle_split_2x1'))
                .fontSize(14)
                .fontColor([$r('sys.color.icon_primary')])
              Text('排列窗口')
                .fontSize(13)
                .fontColor($r('sys.color.font_primary'))
            }
          }
          .height(36)
          .padding({ left: 14, right: 16 })
          .backgroundColor($r('sys.color.comp_background_primary'))
          .border({ width: 0.6, color: $r('sys.color.comp_divider') })
          .onClick(() => {
            this.handleArrangeWindows();
          })

          Blank()

          // Sort Dropdown
          Button({ type: ButtonType.Capsule }) {
            Row({ space: 5 }) {
              SymbolGlyph($r('sys.symbol.sort'))
                .fontSize(14)
                .fontColor([$r('sys.color.icon_secondary')])
              Text(this.sortBy === 'time' ? '按时间' : (this.sortBy === 'name' ? '按名称' : '按编号'))
                .fontSize(12)
                .fontColor($r('sys.color.font_secondary'))
              SymbolGlyph($r('sys.symbol.chevron_down'))
                .fontSize(11)
                .fontColor([$r('sys.color.icon_tertiary')])
            }
          }
          .height(34)
          .padding({ left: 12, right: 12 })
          .backgroundColor($r('sys.color.comp_background_primary'))
          .border({ width: 0.6, color: $r('sys.color.comp_divider') })
          .bindMenu(this.sortMenuBuilder())

          // Segmented Card / Compact View Switch
          Row({ space: 2 }) {
            Button({ type: ButtonType.Normal }) {
              SymbolGlyph($r('sys.symbol.rectangle_grid_1x2'))
                .fontSize(14)
                .fontColor([
                  this.viewMode === 'card'
                    ? $r('sys.color.icon_emphasize')
                    : $r('sys.color.icon_secondary')
                ])
            }
            .width(32)
            .height(28)
            .borderRadius(14)
            .backgroundColor(
              this.viewMode === 'card'
                ? $r('sys.color.comp_emphasize_tertiary')
                : Color.Transparent
            )
            .onClick(() => { this.updateViewMode('card'); })

            Button({ type: ButtonType.Normal }) {
              SymbolGlyph($r('sys.symbol.list_bullet'))
                .fontSize(14)
                .fontColor([
                  this.viewMode === 'compact'
                    ? $r('sys.color.icon_emphasize')
                    : $r('sys.color.icon_secondary')
                ])
            }
            .width(32)
            .height(28)
            .borderRadius(14)
            .backgroundColor(
              this.viewMode === 'compact'
                ? $r('sys.color.comp_emphasize_tertiary')
                : Color.Transparent
            )
            .onClick(() => { this.updateViewMode('compact'); })
          }
          .padding(3)
          .borderRadius(17)
          .backgroundColor($r('sys.color.comp_background_primary'))
          .border({ width: 0.6, color: $r('sys.color.comp_divider') })
        }
        .width('100%')
        .alignItems(VerticalAlign.Center)
      } else {
        // Batch Operations Toolbar (MuMu 0b068f59 style)
        Row({ space: 10 }) {
          Row({ space: 6 }) {
            Checkbox()
              .select(this.selectedIds.length > 0 && this.selectedIds.length === this.instances.length)
              .selectedColor($r('sys.color.brand'))
              .width(18)
              .height(18)
              .onChange(() => {
                this.toggleSelectAll();
              })
            Text(`全选 (${this.selectedIds.length}/${this.instances.length})`)
              .fontSize(13)
              .fontWeight(FontWeight.Medium)
              .fontColor($r('sys.color.font_primary'))
          }
          .padding({ left: 6, right: 6 })
          .onClick(() => { this.toggleSelectAll(); })

          Button('启动首台所选', { type: ButtonType.Capsule })
            .height(34)
            .padding({ left: 14, right: 14 })
            .fontSize(12)
            .fontColor($r('sys.color.font_on_primary'))
            .backgroundColor($r('sys.color.brand'))
            .enabled(this.selectedIds.length > 0)
            .onClick(() => {
              const first: DeviceInstance | undefined = this.getInstanceById(this.selectedIds[0]);
              if (first) this.startDeviceInstance(first, 'full');
            })

          Button('关闭运行设备', { type: ButtonType.Capsule })
            .height(34)
            .padding({ left: 14, right: 14 })
            .fontSize(12)
            .fontColor($r('sys.color.font_primary'))
            .backgroundColor($r('sys.color.comp_background_primary'))
            .border({ width: 0.6, color: $r('sys.color.comp_divider') })
            .enabled(this.running)
            .onClick(() => { this.stopActiveInstance(); })

          Button({ type: ButtonType.Capsule }) {
            Row({ space: 4 }) {
              Text('更多')
                .fontSize(12)
                .fontColor($r('sys.color.font_primary'))
              SymbolGlyph($r('sys.symbol.chevron_down'))
                .fontSize(11)
                .fontColor([$r('sys.color.icon_secondary')])
            }
          }
          .height(34)
          .padding({ left: 14, right: 12 })
          .backgroundColor($r('sys.color.comp_background_primary'))
          .border({ width: 0.6, color: $r('sys.color.comp_divider') })
          .bindMenu(this.batchMoreMenuBuilder())

          Blank()

          Button('取消批量操作', { type: ButtonType.Capsule })
            .height(34)
            .padding({ left: 14, right: 14 })
            .fontSize(12)
            .fontColor($r('sys.color.font_emphasize'))
            .backgroundColor($r('sys.color.comp_emphasize_tertiary'))
            .onClick(() => {
              this.batchMode = false;
              this.selectedIds = [];
            })
        }
        .width('100%')
        .padding({ left: 12, right: 12, top: 6, bottom: 6 })
        .borderRadius(14)
        .backgroundColor($r('sys.color.comp_background_primary'))
        .border({ width: 0.6, color: $r('sys.color.comp_divider') })
      }

      // Instance List
      List({ space: this.viewMode === 'card' ? 12 : 8 }) {
        ForEach(
          this.instances,
          (inst: DeviceInstance) => {
            ListItem() {
              if (this.viewMode === 'card') {
                this.deviceCardRow(inst)
              } else {
                this.deviceCompactRow(inst)
              }
            }
          },
          (inst: DeviceInstance): string =>
            `${inst.id}_${inst.name}_${inst.gpuMode}_${inst.refreshRate}_${inst.cpuCores}_${inst.memLimitMb}_${this.running ? this.activeInstanceId : 'idle'}_${this.paused ? 1 : 0}_${this.batchMode ? 1 : 0}_${this.selectedIds.length}_${this.viewMode}`
        )
      }
      .width('100%')
      .layoutWeight(1)
      .scrollBar(BarState.Auto)
    }
    .width('100%')
    .height('100%')
    .padding({ left: 20, right: 20, top: 16, bottom: 12 })
  }

  @Builder
  private appsWorkspace() {
    Column({ space: 14 }) {
      // Search + Filter + Refresh Bar
      Row({ space: 12 }) {
        Row({ space: 8 }) {
          SymbolGlyph($r('sys.symbol.magnifyingglass'))
            .fontSize(15)
            .fontColor([$r('sys.color.icon_tertiary')])
          TextInput({ placeholder: '搜索已安装的 Android 应用名称或包名...', text: this.searchQuery })
            .layoutWeight(1)
            .height(34)
            .fontSize(13)
            .fontColor($r('sys.color.font_primary'))
            .placeholderColor($r('sys.color.font_tertiary'))
            .backgroundColor(Color.Transparent)
            .padding({ left: 0, right: 0 })
            .onChange((val: string) => {
              this.searchQuery = val;
            })
        }
        .layoutWeight(1)
        .height(38)
        .padding({ left: 14, right: 12 })
        .borderRadius(19)
        .backgroundColor($r('sys.color.comp_background_primary'))
        .border({ width: 0.6, color: $r('sys.color.comp_divider') })
        .alignItems(VerticalAlign.Center)

        // Segmented Filter
        Row({ space: 2 }) {
          Button('全部', { type: ButtonType.Normal })
            .height(30)
            .padding({ left: 12, right: 12 })
            .borderRadius(15)
            .fontSize(12)
            .fontColor(this.appFilter === 'all' ? $r('sys.color.font_on_primary') : $r('sys.color.font_secondary'))
            .backgroundColor(this.appFilter === 'all' ? $r('sys.color.brand') : Color.Transparent)
            .onClick(() => { this.appFilter = 'all'; })
          Button('第三方', { type: ButtonType.Normal })
            .height(30)
            .padding({ left: 12, right: 12 })
            .borderRadius(15)
            .fontSize(12)
            .fontColor(this.appFilter === 'user' ? $r('sys.color.font_on_primary') : $r('sys.color.font_secondary'))
            .backgroundColor(this.appFilter === 'user' ? $r('sys.color.brand') : Color.Transparent)
            .onClick(() => { this.appFilter = 'user'; })
          Button('系统', { type: ButtonType.Normal })
            .height(30)
            .padding({ left: 12, right: 12 })
            .borderRadius(15)
            .fontSize(12)
            .fontColor(this.appFilter === 'system' ? $r('sys.color.font_on_primary') : $r('sys.color.font_secondary'))
            .backgroundColor(this.appFilter === 'system' ? $r('sys.color.brand') : Color.Transparent)
            .onClick(() => { this.appFilter = 'system'; })
        }
        .padding(3)
        .borderRadius(18)
        .backgroundColor($r('sys.color.comp_background_primary'))
        .border({ width: 0.6, color: $r('sys.color.comp_divider') })

        Button({ type: ButtonType.Capsule }) {
          Row({ space: 5 }) {
            SymbolGlyph($r('sys.symbol.arrow_clockwise'))
              .fontSize(13)
              .fontColor([$r('sys.color.icon_primary')])
            Text('刷新')
              .fontSize(12)
              .fontColor($r('sys.color.font_primary'))
          }
        }
        .height(36)
        .padding({ left: 14, right: 14 })
        .backgroundColor($r('sys.color.comp_background_primary'))
        .border({ width: 0.6, color: $r('sys.color.comp_divider') })
        .enabled(this.wmAgentReady && !this.loadingApps)
        .onClick(() => {
          this.refreshApps(true);
        })
      }
      .width('100%')

      // Grid / Empty States
      if (!this.wmAgentReady) {
        Column({ space: 14 }) {
          if (this.running) {
            LoadingProgress()
              .width(36)
              .height(36)
              .color($r('sys.color.brand'))
            Text('Android 14 正在启动，应用列表即将自动同步...')
              .fontSize(14)
              .fontColor($r('sys.color.font_secondary'))
          } else {
            SymbolGlyph($r('sys.symbol.square_grid_2x2'))
              .fontSize(42)
              .fontColor([$r('sys.color.icon_tertiary')])
            Text('当前没有运行中的 Android 设备')
              .fontSize(15)
              .fontWeight(FontWeight.Medium)
              .fontColor($r('sys.color.font_primary'))
            Text('启动任意设备后，即可在此以独立鸿蒙窗口直接运行所有 Android 应用')
              .fontSize(12)
              .fontColor($r('sys.color.font_tertiary'))
            Button('返回设备列表启动', { type: ButtonType.Capsule })
              .height(36)
              .padding({ left: 20, right: 20 })
              .fontSize(13)
              .fontColor($r('sys.color.font_on_primary'))
              .backgroundColor($r('sys.color.brand'))
              .onClick(() => { this.activeNav = 'devices'; })
          }
        }
        .width('100%')
        .layoutWeight(1)
        .justifyContent(FlexAlign.Center)
      } else if (this.getFilteredApps().length === 0) {
        Column({ space: 8 }) {
          Text(this.loadingApps ? '正在同步应用与图标...' : '没有找到匹配的 Android 应用')
            .fontSize(14)
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
                Column({ space: 10 }) {
                  Column({ space: 8 }) {
                    if (this.iconsDir.length > 0 && app.iconFile.length > 0) {
                      Image(`file://${this.iconsDir}/${app.iconFile}`)
                        .width(54)
                        .height(54)
                        .borderRadius(14)
                        .objectFit(ImageFit.Contain)
                    } else {
                      Row() {
                        Text(app.label.length > 0 ? app.label.substring(0, 1).toUpperCase() : 'A')
                          .fontSize(22)
                          .fontWeight(FontWeight.Bold)
                          .fontColor($r('sys.color.font_on_primary'))
                      }
                      .width(54)
                      .height(54)
                      .borderRadius(14)
                      .backgroundColor($r('sys.color.brand'))
                      .justifyContent(FlexAlign.Center)
                      .alignItems(VerticalAlign.Center)
                    }

                    Text(app.label)
                      .fontSize(13)
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

                  Row({ space: 6 }) {
                    Button({ type: ButtonType.Normal }) {
                      Row({ space: 3 }) {
                        SymbolGlyph($r('sys.symbol.screen_rotation'))
                          .fontSize(11)
                          .fontColor([$r('sys.color.icon_secondary')])
                        Text(this.getEffectiveModeLabel(app))
                          .fontSize(10)
                          .fontColor($r('sys.color.font_secondary'))
                      }
                    }
                    .height(24)
                    .padding({ left: 8, right: 8 })
                    .borderRadius(12)
                    .backgroundColor($r('sys.color.background_secondary'))
                    .onClick(() => { this.cycleAppWindowPref(app); })

                    Button({ type: ButtonType.Normal }) {
                      Row({ space: 3 }) {
                        SymbolGlyph(this.isAppPinned(app.packageName) ? $r('sys.symbol.pin_fill') : $r('sys.symbol.pin'))
                          .fontSize(11)
                          .fontColor([
                            this.isAppPinned(app.packageName)
                              ? $r('sys.color.confirm')
                              : $r('sys.color.icon_emphasize')
                          ])
                        Text(this.isAppPinned(app.packageName) ? '已加桌' : '加桌')
                          .fontSize(10)
                          .fontColor(
                            this.isAppPinned(app.packageName)
                              ? $r('sys.color.confirm')
                              : $r('sys.color.font_emphasize')
                          )
                      }
                    }
                    .height(24)
                    .padding({ left: 8, right: 8 })
                    .borderRadius(12)
                    .backgroundColor(
                      this.isAppPinned(app.packageName)
                        ? '#1F64BB5C'
                        : $r('sys.color.comp_emphasize_tertiary')
                    )
                    .onClick(() => { this.toggleDesktopShortcut(app); })
                  }
                  .justifyContent(FlexAlign.Center)
                }
                .width('100%')
                .padding({ left: 10, right: 10, top: 14, bottom: 12 })
                .borderRadius(16)
                .backgroundColor($r('sys.color.comp_background_primary'))
                .border({ width: 0.6, color: $r('sys.color.comp_divider') })
                .shadow({ radius: 8, color: '#08000000', offsetY: 2 })
              }
            },
            (app: AndroidAppItem): string => `${app.packageName}_${this.prefEpoch}_${this.pinnedIds.length}`
          )
        }
        .columnsTemplate('1fr 1fr 1fr 1fr 1fr')
        .columnsGap(12)
        .rowsGap(12)
        .width('100%')
        .layoutWeight(1)
      }
    }
    .width('100%')
    .height('100%')
    .padding({ left: 20, right: 20, top: 16, bottom: 12 })
  }

  build() {
    Column({ space: 0 }) {
      // Integrated HarmonyOS PC Title Bar (48vp, native 3-button avoidance)
      Row({ space: 10 }) {
        Row({ space: 10 }) {
          Stack({ alignContent: Alignment.Center }) {
            Column()
              .width(28)
              .height(28)
              .borderRadius(8)
              .backgroundColor($r('sys.color.brand'))
            SymbolGlyph($r('sys.symbol.pad_fill'))
              .fontSize(16)
              .fontColor([$r('sys.color.icon_on_primary')])
          }
          Text('svbox Android')
            .fontSize(15)
            .fontWeight(FontWeight.Bold)
            .fontColor($r('sys.color.font_primary'))

          Text('HarmonyOS PC')
            .fontSize(10)
            .fontWeight(FontWeight.Medium)
            .fontColor($r('sys.color.font_emphasize'))
            .padding({ left: 7, right: 7, top: 2, bottom: 2 })
            .borderRadius(6)
            .backgroundColor($r('sys.color.comp_emphasize_tertiary'))
        }
        .alignItems(VerticalAlign.Center)

        Blank()

        if (this.running) {
          Row({ space: 8 }) {
            Circle({ width: 7, height: 7 })
              .fill(this.paused ? $r('sys.color.alert') : $r('sys.color.confirm'))
            Text(`${this.refreshRate}Hz · CPU ${Math.round(this.cpuPercent)}% · ${this.memMb} MB`)
              .fontSize(11)
              .fontColor($r('sys.color.font_secondary'))
          }
          .padding({ left: 10, right: 10, top: 5, bottom: 5 })
          .borderRadius(12)
          .backgroundColor($r('sys.color.background_secondary'))
        }
      }
      .width('100%')
      .height(48)
      .padding({ left: 16, right: Math.max(152, this.decorRightAvoidVp) })
      .backgroundColor($r('sys.color.comp_background_primary'))
      .border({ width: { bottom: 0.6 }, color: $r('sys.color.comp_divider') })
      .alignItems(VerticalAlign.Center)

      // Main Two-Column HarmonyOS PC + MuMu Layout
      Row({ space: 0 }) {
        // Slim Left Navigation Rail (72vp)
        Column({ space: 8 }) {
          this.navRailItem('devices', '设备', $r('sys.symbol.display'), 0)
          this.navRailItem('apps', '应用', $r('sys.symbol.square_grid_2x2'), this.appList.length)

          Blank()

          Column({ space: 4 }) {
            Button({ type: ButtonType.Circle }) {
              SymbolGlyph($r('sys.symbol.gearshape'))
                .fontSize(18)
                .fontColor([$r('sys.color.icon_secondary')])
            }
            .width(38)
            .height(38)
            .backgroundColor(Color.Transparent)
            .stateStyles({
              pressed: { .backgroundColor($r('sys.color.interactive_pressed')) }
            })
            .onClick(() => {
              this.openGlobalSettingsDialog();
            })

            Text('设置')
              .fontSize(10)
              .fontColor($r('sys.color.font_tertiary'))
          }
          .alignItems(HorizontalAlign.Center)
          .padding({ bottom: 4 })
        }
        .width(72)
        .height('100%')
        .padding({ left: 8, right: 8, top: 14, bottom: 10 })
        .backgroundColor($r('sys.color.comp_background_primary'))
        .border({ width: { right: 0.6 }, color: $r('sys.color.comp_divider') })

        // Right Workspace Canvas
        Column({ space: 0 }) {
          if (this.activeNav === 'devices') {
            this.devicesWorkspace()
          } else {
            this.appsWorkspace()
          }
        }
        .layoutWeight(1)
        .height('100%')
        .backgroundColor($r('sys.color.background_secondary'))
      }
      .width('100%')
      .layoutWeight(1)

      // Clean HarmonyOS Bottom Status Bar (30vp)
      Row({ space: 12 }) {
        Text(`共 ${this.instances.length} 台设备 · ${this.running ? '1 台运行中' : '全部已停止'}`)
          .fontSize(11)
          .fontColor($r('sys.color.font_secondary'))

        if (this.cloningBusy) {
          Row({ space: 5 }) {
            LoadingProgress().width(14).height(14).color($r('sys.color.brand'))
            Text('正在复制设备数据...')
              .fontSize(11)
              .fontColor($r('sys.color.font_emphasize'))
          }
        } else if (this.statusText.length > 0) {
          Text(`· ${this.statusText === 'Idle' ? '就绪' : this.statusText}`)
            .fontSize(11)
            .fontColor($r('sys.color.font_tertiary'))
            .maxLines(1)
            .textOverflow({ overflow: TextOverflow.Ellipsis })
            .layoutWeight(1)
        } else {
          Blank()
        }

        Text(this.gpuRenderer.length > 0 ? this.gpuRenderer : 'Maleoon Vulkan · svbox Runtime')
          .fontSize(11)
          .fontColor($r('sys.color.font_tertiary'))
          .maxLines(1)
      }
      .width('100%')
      .height(30)
      .padding({ left: 16, right: 16 })
      .backgroundColor($r('sys.color.comp_background_primary'))
      .border({ width: { top: 0.6 }, color: $r('sys.color.comp_divider') })
      .alignItems(VerticalAlign.Center)
    }
    .width('100%')
    .height('100%')
    .backgroundColor($r('sys.color.background_secondary'))
  }
}
