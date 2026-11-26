import { buffer } from '@kit.ArkTS';
import { pasteboard } from '@kit.BasicServicesKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import native from 'libentry.so';

const TAG: string = 'SvboxInputBridge';

let gLastSyncedClip: string = '';

export function utf8ToBase64(text: string): string {
  return buffer.from(text, 'utf-8').toString('base64');
}

export function base64ToUtf8(b64: string): string {
  try {
    return buffer.from(b64, 'base64').toString('utf-8');
  } catch (_e) {
    return '';
  }
}

export async function readHostClipboardText(): Promise<string> {
  try {
    const sysPb: pasteboard.SystemPasteboard = pasteboard.getSystemPasteboard();
    const data: pasteboard.PasteData = await sysPb.getData();
    if (!data) return '';
    const txt: string = data.getPrimaryText();
    return typeof txt === 'string' ? txt : '';
  } catch (_e) {
    return '';
  }
}

export async function syncHostClipboardToAndroid(force: boolean = false): Promise<string> {
  if (!native.isWmAgentReady()) return '';
  try {
    const txt: string = await readHostClipboardText();
    if (txt.length === 0) return '';
    if (!force && txt === gLastSyncedClip) return txt;
    gLastSyncedClip = txt;
    await native.wmRpcAsync(`SET_CLIPBOARD ${utf8ToBase64(txt)}`);
    return txt;
  } catch (err) {
    hilog.warn(0x0000, TAG, 'syncHostClipboardToAndroid failed: %{public}s', String(err));
    return '';
  }
}

export async function applyAndroidClipboardToHost(b64: string): Promise<void> {
  if (!b64 || b64.length === 0) return;
  const txt: string = base64ToUtf8(b64);
  if (txt.length === 0 || txt === gLastSyncedClip) return;
  try {
    gLastSyncedClip = txt;
    const sysPb: pasteboard.SystemPasteboard = pasteboard.getSystemPasteboard();
    const pd: pasteboard.PasteData = pasteboard.createData(pasteboard.MIMETYPE_TEXT_PLAIN, txt);
    await sysPb.setData(pd);
  } catch (err) {
    hilog.warn(0x0000, TAG, 'applyAndroidClipboardToHost failed: %{public}s', String(err));
  }
}

export async function sendTextToAndroid(displaySlot: number, text: string): Promise<boolean> {
  if (!native.isWmAgentReady() || text.length === 0) return false;
  try {
    gLastSyncedClip = text;
    const raw: string = await native.wmRpcAsync(`INPUT_TEXT ${displaySlot} ${utf8ToBase64(text)}`);
    return raw.indexOf('"ok":true') >= 0;
  } catch (err) {
    hilog.warn(0x0000, TAG, 'sendTextToAndroid failed: %{public}s', String(err));
    return false;
  }
}

export async function pasteHostClipboardToAndroid(displaySlot: number): Promise<boolean> {
  if (!native.isWmAgentReady()) return false;
  try {
    const txt: string = await syncHostClipboardToAndroid(true);
    if (txt.length === 0) return false;
    await native.wmRpcAsync(`SEND_KEY ${displaySlot} 279`);
    return true;
  } catch (err) {
    hilog.warn(0x0000, TAG, 'pasteHostClipboardToAndroid failed: %{public}s', String(err));
    return false;
  }
}
