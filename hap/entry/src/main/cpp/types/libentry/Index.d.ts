export interface RuntimeStatus {
  running: boolean;
  paused: boolean;
  connected: boolean;
  wmAgentReady: boolean;
  gmsAvailable?: boolean;
  gmsEnabled?: boolean;
  vsyncActive: boolean;
  inputReady: boolean;
  audioActive: boolean;
  frames: number;
  width: number;
  height: number;
  fps: number;
  refreshRate: number;
  cpuPercent: number;
  memMb: number;
  hostMemMb?: number;
  gfxMemMb?: number;
  memLimitMb: number;
  activeProcs: number;
  hostCpuCores?: number;
  profile: string;
  gpuMode: string;
  syscallMode: string;
  gpuRenderer: string;
  cpuset: string;
  topology: string;
  statusText: string;
  activeInstanceId: string;
  shareDir: string;
}

export interface DisplayStats {
  displaySlot: number;
  attached: number;
  visible: number;
  width: number;
  height: number;
  framesPosted: number;
  fps: number;
  topBarColor: number;
}

export const startContainer: (
  sandboxTempDir: string,
  svboxPath: string,
  androidDir: string,
  mode: string,
  width: number,
  height: number
) => string;

export const attachHwcSocket: (sockPath: string, width: number, height: number) => string;
export const stopContainer: () => string;
export const sendNavKey: (key: string) => boolean;
export const sendScroll: (x: number, y: number, deltaY: number) => boolean;
export const sendDisplayScroll: (slot: number, x: number, y: number, deltaY: number) => boolean;
export const setResourceConfig: (
  profile: string,
  cpuset: string,
  memMb: number,
  diskLimitGb?: number,
  cpuCores?: number
) => boolean;
export const setInstanceConfig: (
  instanceId: string,
  dataSubdir: string,
  dpi: number,
  enableAudio: boolean,
  enableRoot: boolean,
  brand: string,
  model: string,
  adbPort: number,
  enableGms?: boolean
) => boolean;
export const getActiveInstanceId: () => string;
export const getInstanceDiskMb: (filesDir: string, dataSubdir: string) => number;
export const cleanInstanceCache: (filesDir: string, dataSubdir: string) => number;
export const cloneInstanceAsync: (filesDir: string, srcSubdir: string, dstSubdir: string) => Promise<boolean>;
export const deleteInstanceData: (filesDir: string, dataSubdir: string) => boolean;
export const backupInstanceAsync: (
  filesDir: string,
  dataSubdir: string,
  outTarPath: string,
  compress: boolean
) => Promise<string>;
export const restoreInstanceAsync: (
  filesDir: string,
  dataSubdir: string,
  inTarPath: string
) => Promise<string>;
export const listBackupFiles: (backupDir: string) => string[];
export const ensureDir: (path: string) => boolean;
export const setGpuMode: (gpuMode: string) => boolean;
export const setSyscallMode: (syscallMode: string) => boolean;
export const setRefreshRate: (hz: number) => boolean;
export const setDisplayRefreshRate: (slot: number, hz: number) => boolean;
export const setPaused: (paused: boolean) => string;
export const trimMemory: () => string;
export const setVisible: (visible: boolean) => boolean;
export const setDisplayVisible: (slot: number, visible: boolean) => boolean;
export const saveRawSnapshot: (path: string, width: number, height: number, pixels: ArrayBuffer) => boolean;
export const runGuestCmd: (cmd: string, outPath: string) => boolean;
export const installLocalApkAsync: (guestApkPath: string) => Promise<string>;
export const dumpProcs: (outPath: string) => boolean;
export const injectTap: (x: number, y: number) => boolean;
export const isWmAgentReady: () => boolean;
export const getIconsDir: () => string;
export const wmRpc: (cmd: string) => string;
export const wmRpcAsync: (cmd: string) => Promise<string>;
export const allocDisplaySlot: (pkg: string) => number;
export const findDisplaySlot: (pkg: string) => number;
export const releaseDisplaySlot: (slot: number) => boolean;
export const pollWmEvents: () => string;
export const getDisplayStats: (slot: number) => DisplayStats;
export const getStatus: () => RuntimeStatus;
