import { fileIo } from '@kit.CoreFileKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import native from 'libentry.so';

const TAG: string = 'SvboxKeymap';

export type KeymapItemKind = 'dpad' | 'tap' | 'rmb_move';

export interface KeymapItem {
  id: string;
  kind: KeymapItemKind;
  label: string;
  keyCode: number;
  upKey: number;
  downKey: number;
  leftKey: number;
  rightKey: number;
  nx: number;
  ny: number;
  radius: number;
  direct: boolean;
}

export interface KeymapProfile {
  pkg: string;
  enabled: boolean;
  showHints: boolean;
  items: KeymapItem[];
}

export interface KeyChoice {
  label: string;
  code: number;
}

export const QUICK_KEY_CHOICES: KeyChoice[] = [
  { label: 'Q', code: 2033 },
  { label: 'W', code: 2039 },
  { label: 'E', code: 2021 },
  { label: 'R', code: 2034 },
  { label: 'T', code: 2036 },
  { label: 'A', code: 2017 },
  { label: 'S', code: 2035 },
  { label: 'D', code: 2020 },
  { label: 'F', code: 2022 },
  { label: 'G', code: 2023 },
  { label: 'Z', code: 2042 },
  { label: 'X', code: 2040 },
  { label: 'C', code: 2019 },
  { label: 'V', code: 2038 },
  { label: 'B', code: 2018 },
  { label: 'J', code: 2026 },
  { label: 'K', code: 2027 },
  { label: 'L', code: 2028 },
  { label: '1', code: 2001 },
  { label: '2', code: 2002 },
  { label: '3', code: 2003 },
  { label: '4', code: 2004 },
  { label: 'Space', code: 2050 },
  { label: 'Shift', code: 2047 },
  { label: 'Tab', code: 2049 },
  { label: 'Enter', code: 2054 },
];

export function keyCodeToLabel(code: number): string {
  for (let i: number = 0; i < QUICK_KEY_CHOICES.length; i++) {
    if (QUICK_KEY_CHOICES[i].code === code) {
      return QUICK_KEY_CHOICES[i].label;
    }
  }
  if (code >= 2017 && code <= 2042) {
    return String.fromCharCode(65 + (code - 2017));
  }
  if (code >= 2000 && code <= 2009) {
    return String(code - 2000);
  }
  return `K${code}`;
}

export function createMobaPreset(pkg: string): KeymapProfile {
  return {
    pkg,
    enabled: true,
    showHints: true,
    items: [
      {
        id: 'dpad_wasd',
        kind: 'dpad',
        label: 'WASD',
        keyCode: 0,
        upKey: 2039,
        downKey: 2035,
        leftKey: 2017,
        rightKey: 2020,
        nx: 0.16,
        ny: 0.74,
        radius: 0.11,
        direct: false,
      },
      {
        id: 'tap_atk',
        kind: 'tap',
        label: 'Space',
        keyCode: 2050,
        upKey: 0,
        downKey: 0,
        leftKey: 0,
        rightKey: 0,
        nx: 0.89,
        ny: 0.82,
        radius: 0.05,
        direct: true,
      },
      {
        id: 'tap_s1',
        kind: 'tap',
        label: 'Q',
        keyCode: 2033,
        upKey: 0,
        downKey: 0,
        leftKey: 0,
        rightKey: 0,
        nx: 0.77,
        ny: 0.82,
        radius: 0.05,
        direct: true,
      },
      {
        id: 'tap_s2',
        kind: 'tap',
        label: 'E',
        keyCode: 2021,
        upKey: 0,
        downKey: 0,
        leftKey: 0,
        rightKey: 0,
        nx: 0.81,
        ny: 0.66,
        radius: 0.05,
        direct: true,
      },
      {
        id: 'tap_s3',
        kind: 'tap',
        label: 'R',
        keyCode: 2034,
        upKey: 0,
        downKey: 0,
        leftKey: 0,
        rightKey: 0,
        nx: 0.90,
        ny: 0.56,
        radius: 0.05,
        direct: true,
      },
      {
        id: 'tap_sum',
        kind: 'tap',
        label: 'F',
        keyCode: 2022,
        upKey: 0,
        downKey: 0,
        leftKey: 0,
        rightKey: 0,
        nx: 0.67,
        ny: 0.86,
        radius: 0.05,
        direct: true,
      },
      {
        id: 'tap_heal',
        kind: 'tap',
        label: 'B',
        keyCode: 2018,
        upKey: 0,
        downKey: 0,
        leftKey: 0,
        rightKey: 0,
        nx: 0.56,
        ny: 0.88,
        radius: 0.05,
        direct: true,
      },
    ],
  };
}

export function createFpsPreset(pkg: string): KeymapProfile {
  return {
    pkg,
    enabled: true,
    showHints: true,
    items: [
      {
        id: 'dpad_wasd',
        kind: 'dpad',
        label: 'WASD',
        keyCode: 0,
        upKey: 2039,
        downKey: 2035,
        leftKey: 2017,
        rightKey: 2020,
        nx: 0.17,
        ny: 0.72,
        radius: 0.12,
        direct: false,
      },
      {
        id: 'tap_jump',
        kind: 'tap',
        label: 'Space',
        keyCode: 2050,
        upKey: 0,
        downKey: 0,
        leftKey: 0,
        rightKey: 0,
        nx: 0.88,
        ny: 0.70,
        radius: 0.05,
        direct: true,
      },
      {
        id: 'tap_reload',
        kind: 'tap',
        label: 'R',
        keyCode: 2034,
        upKey: 0,
        downKey: 0,
        leftKey: 0,
        rightKey: 0,
        nx: 0.80,
        ny: 0.84,
        radius: 0.05,
        direct: true,
      },
      {
        id: 'tap_act',
        kind: 'tap',
        label: 'F',
        keyCode: 2022,
        upKey: 0,
        downKey: 0,
        leftKey: 0,
        rightKey: 0,
        nx: 0.68,
        ny: 0.56,
        radius: 0.05,
        direct: true,
      },
      {
        id: 'tap_crouch',
        kind: 'tap',
        label: 'C',
        keyCode: 2019,
        upKey: 0,
        downKey: 0,
        leftKey: 0,
        rightKey: 0,
        nx: 0.88,
        ny: 0.86,
        radius: 0.05,
        direct: true,
      },
    ],
  };
}

export function createGeneralPreset(pkg: string): KeymapProfile {
  return {
    pkg,
    enabled: true,
    showHints: true,
    items: [
      {
        id: 'tap_ok',
        kind: 'tap',
        label: 'Space',
        keyCode: 2050,
        upKey: 0,
        downKey: 0,
        leftKey: 0,
        rightKey: 0,
        nx: 0.50,
        ny: 0.82,
        radius: 0.05,
        direct: true,
      },
      {
        id: 'tap_left',
        kind: 'tap',
        label: 'A',
        keyCode: 2017,
        upKey: 0,
        downKey: 0,
        leftKey: 0,
        rightKey: 0,
        nx: 0.20,
        ny: 0.50,
        radius: 0.05,
        direct: true,
      },
      {
        id: 'tap_right',
        kind: 'tap',
        label: 'D',
        keyCode: 2020,
        upKey: 0,
        downKey: 0,
        leftKey: 0,
        rightKey: 0,
        nx: 0.80,
        ny: 0.50,
        radius: 0.05,
        direct: true,
      },
    ],
  };
}

export function serializeKeymapRules(profile: KeymapProfile): string {
  const lines: string[] = [];
  for (let i: number = 0; i < profile.items.length; i++) {
    const it: KeymapItem = profile.items[i];
    if (it.kind === 'dpad') {
      lines.push(
        `dpad|${it.upKey}|${it.downKey}|${it.leftKey}|${it.rightKey}|` +
        `${it.nx.toFixed(4)}|${it.ny.toFixed(4)}|${it.radius.toFixed(4)}`
      );
    } else if (it.kind === 'tap') {
      lines.push(`tap|${it.keyCode}|${it.nx.toFixed(4)}|${it.ny.toFixed(4)}`);
    } else if (it.kind === 'rmb_move') {
      lines.push(
        `rmb|${it.nx.toFixed(4)}|${it.ny.toFixed(4)}|${it.radius.toFixed(4)}|${it.direct ? 1 : 0}`
      );
    }
  }
  return lines.join('\n');
}

export function applyKeymapToNative(slot: number, profile: KeymapProfile): void {
  try {
    const spec: string = profile.enabled ? serializeKeymapRules(profile) : '';
    native.setKeymapConfig(slot, profile.enabled, spec);
  } catch (err) {
    hilog.warn(0x0000, TAG, 'applyKeymapToNative failed: %{public}s', String(err));
  }
}

class KeymapStore {
  private loadedDir: string = '';
  private profiles: Record<string, KeymapProfile> = {};

  private load(filesDir: string): void {
    if (this.loadedDir === filesDir) return;
    this.loadedDir = filesDir;
    this.profiles = {};
    const path: string = `${filesDir}/keymaps.json`;
    try {
      if (!fileIo.accessSync(path)) return;
      const raw: string = fileIo.readTextSync(path);
      const parsed: Record<string, KeymapProfile> = JSON.parse(raw) as Record<string, KeymapProfile>;
      if (parsed && typeof parsed === 'object') {
        this.profiles = parsed;
      }
    } catch (err) {
      hilog.warn(0x0000, TAG, 'load keymaps failed: %{public}s', String(err));
    }
  }

  private save(filesDir: string): void {
    const path: string = `${filesDir}/keymaps.json`;
    try {
      const f: fileIo.File = fileIo.openSync(
        path,
        fileIo.OpenMode.CREATE | fileIo.OpenMode.WRITE_ONLY | fileIo.OpenMode.TRUNC
      );
      fileIo.writeSync(f.fd, JSON.stringify(this.profiles));
      fileIo.closeSync(f);
    } catch (err) {
      hilog.warn(0x0000, TAG, 'save keymaps failed: %{public}s', String(err));
    }
  }

  getProfile(filesDir: string, pkg: string): KeymapProfile {
    this.load(filesDir);
    const key: string = pkg.length > 0 ? pkg : '__emulator__';
    const existing: KeymapProfile | undefined = this.profiles[key];
    if (existing) {
      return {
        pkg: existing.pkg,
        enabled: existing.enabled,
        showHints: existing.showHints,
        items: existing.items.map((it: KeymapItem): KeymapItem => ({
          id: it.id,
          kind: it.kind,
          label: it.label,
          keyCode: it.keyCode,
          upKey: it.upKey,
          downKey: it.downKey,
          leftKey: it.leftKey,
          rightKey: it.rightKey,
          nx: it.nx,
          ny: it.ny,
          radius: it.radius,
          direct: it.direct,
        })),
      };
    }
    return {
      pkg: key,
      enabled: false,
      showHints: true,
      items: [],
    };
  }

  saveProfile(filesDir: string, profile: KeymapProfile): void {
    this.load(filesDir);
    const key: string = profile.pkg.length > 0 ? profile.pkg : '__emulator__';
    this.profiles[key] = profile;
    this.save(filesDir);
  }
}

export const keymapStore: KeymapStore = new KeymapStore();

// ===================== Lightweight Automation / Macro Engine =====================

export type MacroStepKind = 'tap' | 'swipe' | 'wait' | 'nav';

export interface MacroStep {
  id: string;
  kind: MacroStepKind;
  label: string;
  nx: number;
  ny: number;
  nx2: number;
  ny2: number;
  durationMs: number;
  navKey: string;
}

export interface MacroScript {
  id: string;
  name: string;
  pkg: string;
  loopCount: number; // 0 = infinite
  intervalMs: number;
  steps: MacroStep[];
}

class MacroStore {
  private loadedDir: string = '';
  private scripts: MacroScript[] = [];

  private load(filesDir: string): void {
    if (this.loadedDir === filesDir) return;
    this.loadedDir = filesDir;
    this.scripts = [];
    const path: string = `${filesDir}/macros.json`;
    try {
      if (!fileIo.accessSync(path)) return;
      const raw: string = fileIo.readTextSync(path);
      const parsed: MacroScript[] = JSON.parse(raw) as MacroScript[];
      if (Array.isArray(parsed)) {
        this.scripts = parsed;
      }
    } catch (err) {
      hilog.warn(0x0000, TAG, 'load macros failed: %{public}s', String(err));
    }
  }

  private save(filesDir: string): void {
    const path: string = `${filesDir}/macros.json`;
    try {
      const f: fileIo.File = fileIo.openSync(
        path,
        fileIo.OpenMode.CREATE | fileIo.OpenMode.WRITE_ONLY | fileIo.OpenMode.TRUNC
      );
      fileIo.writeSync(f.fd, JSON.stringify(this.scripts));
      fileIo.closeSync(f);
    } catch (err) {
      hilog.warn(0x0000, TAG, 'save macros failed: %{public}s', String(err));
    }
  }

  getForPackage(filesDir: string, pkg: string): MacroScript[] {
    this.load(filesDir);
    const key: string = pkg.length > 0 ? pkg : '__emulator__';
    const out: MacroScript[] = [];
    for (let i: number = 0; i < this.scripts.length; i++) {
      if (this.scripts[i].pkg === key || this.scripts[i].pkg === '__global__') {
        out.push(this.scripts[i]);
      }
    }
    return out;
  }

  upsert(filesDir: string, script: MacroScript): void {
    this.load(filesDir);
    let idx: number = -1;
    for (let i: number = 0; i < this.scripts.length; i++) {
      if (this.scripts[i].id === script.id) {
        idx = i;
        break;
      }
    }
    if (idx >= 0) {
      this.scripts[idx] = script;
    } else {
      this.scripts.push(script);
    }
    this.save(filesDir);
  }

  remove(filesDir: string, id: string): void {
    this.load(filesDir);
    this.scripts = this.scripts.filter((s: MacroScript): boolean => s.id !== id);
    this.save(filesDir);
  }
}

export const macroStore: MacroStore = new MacroStore();

function sleepMs(ms: number): Promise<void> {
  return new Promise<void>((resolve) => {
    setTimeout(resolve, Math.max(10, ms));
  });
}

export class MacroRunner {
  private running: boolean = false;
  private runToken: number = 0;

  isRunning(): boolean {
    return this.running;
  }

  stop(slot: number): void {
    this.runToken += 1;
    if (this.running) {
      this.running = false;
      try {
        native.injectNormTouch(slot, 520, 2, 0.5, 0.5);
      } catch (_e) {
      }
    }
  }

  async run(
    slot: number,
    script: MacroScript,
    onProgress: (text: string) => void,
    onDone: () => void
  ): Promise<void> {
    this.stop(slot);
    if (script.steps.length === 0) {
      onDone();
      return;
    }
    const token: number = ++this.runToken;
    this.running = true;
    const totalLoops: number = script.loopCount; // 0 = infinite
    let loopIdx: number = 0;

    try {
      while (this.running && token === this.runToken) {
        loopIdx += 1;
        for (let i: number = 0; i < script.steps.length; i++) {
          if (!this.running || token !== this.runToken) break;
          const st: MacroStep = script.steps[i];
          const loopLabel: string = totalLoops > 0 ? `${loopIdx}/${totalLoops}` : `#${loopIdx}`;
          onProgress(`第 ${loopLabel} 轮 · 步骤 ${i + 1}/${script.steps.length} (${st.label})`);

          if (st.kind === 'tap') {
            native.injectNormTouch(slot, 520, 0, st.nx, st.ny);
            await sleepMs(Math.max(40, Math.min(800, st.durationMs || 60)));
            native.injectNormTouch(slot, 520, 2, st.nx, st.ny);
            await sleepMs(60);
          } else if (st.kind === 'swipe') {
            const dur: number = Math.max(120, Math.min(3000, st.durationMs || 320));
            const segs: number = Math.max(6, Math.min(24, Math.floor(dur / 20)));
            native.injectNormTouch(slot, 520, 0, st.nx, st.ny);
            for (let k: number = 1; k <= segs; k++) {
              if (!this.running || token !== this.runToken) break;
              const t: number = k / segs;
              const cx: number = st.nx + (st.nx2 - st.nx) * t;
              const cy: number = st.ny + (st.ny2 - st.ny) * t;
              native.injectNormTouch(slot, 520, 1, cx, cy);
              await sleepMs(Math.max(12, Math.floor(dur / segs)));
            }
            native.injectNormTouch(slot, 520, 2, st.nx2, st.ny2);
            await sleepMs(60);
          } else if (st.kind === 'wait') {
            const totalWait: number = Math.max(50, st.durationMs || 500);
            let waited: number = 0;
            while (waited < totalWait && this.running && token === this.runToken) {
              const chunk: number = Math.min(100, totalWait - waited);
              await sleepMs(chunk);
              waited += chunk;
            }
          } else if (st.kind === 'nav') {
            if (slot === 0) {
              native.sendNavKey(st.navKey || 'back');
            } else {
              const kc: number = st.navKey === 'home' ? 3 : 4;
              await native.wmRpcAsync(`SEND_KEY ${slot} ${kc}`);
            }
            await sleepMs(120);
          }
        }
        if (totalLoops > 0 && loopIdx >= totalLoops) break;
        const gap: number = Math.max(100, script.intervalMs || 400);
        let waited: number = 0;
        while (waited < gap && this.running && token === this.runToken) {
          const chunk: number = Math.min(100, gap - waited);
          await sleepMs(chunk);
          waited += chunk;
        }
      }
    } catch (err) {
      hilog.warn(0x0000, TAG, 'MacroRunner error: %{public}s', String(err));
    } finally {
      if (token === this.runToken) {
        this.running = false;
        try {
          native.injectNormTouch(slot, 520, 2, 0.5, 0.5);
        } catch (_e) {
        }
        onDone();
      }
    }
  }
}
