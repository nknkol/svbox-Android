export interface RuntimeStatus {
  running: boolean;
  connected: boolean;
  frames: number;
  width: number;
  height: number;
  fps: number;
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
export const getStatus: () => RuntimeStatus;
