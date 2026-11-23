export interface RuntimeStatus {
  running: boolean;
  paused: boolean;
  connected: boolean;
  vsyncActive: boolean;
  inputReady: boolean;
  audioActive: boolean;
  frames: number;
  width: number;
  height: number;
  fps: number;
  cpuPercent: number;
  memMb: number;
  memLimitMb: number;
  activeProcs: number;
  profile: string;
  gpuMode: string;
  gpuRenderer: string;
  cpuset: string;
  topology: string;
  statusText: string;
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
export const setResourceConfig: (profile: string, cpuset: string, memMb: number) => boolean;
export const setGpuMode: (gpuMode: string) => boolean;
export const setPaused: (paused: boolean) => string;
export const setVisible: (visible: boolean) => boolean;
export const saveRawSnapshot: (path: string, width: number, height: number, pixels: ArrayBuffer) => boolean;
export const getStatus: () => RuntimeStatus;
