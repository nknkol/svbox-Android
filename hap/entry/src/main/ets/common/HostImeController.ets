import { inputMethod } from '@kit.IMEKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { window } from '@kit.ArkUI';
import native, { ImeState } from 'libentry.so';
import { syncHostClipboardToAndroid, utf8ToBase64 } from './InputBridgeHelper';
import { isPcDevice } from './DeviceProfile';

const TAG: string = 'SvboxHostIme';

// Android InputType bitmasks
const ANDROID_TYPE_MASK_CLASS: number = 0x0000000f;
const ANDROID_TYPE_MASK_VARIATION: number = 0x00000ff0;
const ANDROID_TYPE_CLASS_TEXT: number = 0x00000001;
const ANDROID_TYPE_CLASS_NUMBER: number = 0x00000002;
const ANDROID_TYPE_CLASS_PHONE: number = 0x00000003;
const ANDROID_TYPE_CLASS_DATETIME: number = 0x00000004;
const ANDROID_TYPE_TEXT_FLAG_MULTI_LINE: number = 0x00020000;
const ANDROID_TYPE_TEXT_VARIATION_URI: number = 0x00000010;
const ANDROID_TYPE_TEXT_VARIATION_EMAIL_ADDRESS: number = 0x00000020;
const ANDROID_TYPE_TEXT_VARIATION_PASSWORD: number = 0x00000080;
const ANDROID_TYPE_TEXT_VARIATION_VISIBLE_PASSWORD: number = 0x00000090;
const ANDROID_TYPE_TEXT_VARIATION_WEB_EMAIL_ADDRESS: number = 0x000000d0;
const ANDROID_TYPE_TEXT_VARIATION_WEB_PASSWORD: number = 0x000000e0;
const ANDROID_TYPE_NUMBER_VARIATION_PASSWORD: number = 0x00000010;
const ANDROID_IME_MASK_ACTION: number = 0x000000ff;

interface SlotImeWindowInfo {
  slot: number;
  win: window.Window;
  topBarVp: number;
  active: boolean;
}

/**
 * On HarmonyOS PC (2in1), password/number/phone/datetime inputs should use direct
 * physical keyboard passthrough rather than HarmonyOS XiaoYi IME:
 *  - XiaoYi refuses physical key interception for `pwdkbd` ("current device is pwdkbd on pc, return")
 *    and ignores numeric keys ("ignore keyCodeNumber").
 *  - Users never compose Chinese Pinyin inside passwords, OTP codes, numbers, or phone fields.
 * On touch-only mobile/tablet devices, we still attach the system IME so the soft keyboard pops up.
 */
function shouldBypassHostImeOnPc(inputType: number): boolean {
  if (!isPcDevice()) return false;
  const cls: number = inputType & ANDROID_TYPE_MASK_CLASS;
  const variation: number = inputType & ANDROID_TYPE_MASK_VARIATION;
  if (cls === ANDROID_TYPE_CLASS_NUMBER ||
      cls === ANDROID_TYPE_CLASS_PHONE ||
      cls === ANDROID_TYPE_CLASS_DATETIME) {
    return true;
  }
  if (cls === ANDROID_TYPE_CLASS_TEXT) {
    if (variation === ANDROID_TYPE_TEXT_VARIATION_PASSWORD ||
        variation === ANDROID_TYPE_TEXT_VARIATION_VISIBLE_PASSWORD ||
        variation === ANDROID_TYPE_TEXT_VARIATION_WEB_PASSWORD) {
      return true;
    }
  }
  return false;
}

function mapAndroidInputType(inputType: number, forceGenericOnPc: boolean): inputMethod.TextInputType {
  // On HarmonyOS PC, XiaoYi's password/email/url sub-layouts emit "unknown layout id" or
  // "GetImeByInputType type: 3 not supported". Use standard TEXT / MULTILINE on PC so
  // CN/EN switching works smoothly without triggering broken pc-pwdkbd paths.
  if (forceGenericOnPc || isPcDevice()) {
    if ((inputType & ANDROID_TYPE_MASK_CLASS) === ANDROID_TYPE_CLASS_TEXT &&
        (inputType & ANDROID_TYPE_TEXT_FLAG_MULTI_LINE) !== 0) {
      return inputMethod.TextInputType.MULTILINE;
    }
    return inputMethod.TextInputType.TEXT;
  }
  const cls: number = inputType & ANDROID_TYPE_MASK_CLASS;
  const variation: number = inputType & ANDROID_TYPE_MASK_VARIATION;
  if (cls === ANDROID_TYPE_CLASS_NUMBER) {
    if (variation === ANDROID_TYPE_NUMBER_VARIATION_PASSWORD) {
      return inputMethod.TextInputType.NUMBER_PASSWORD;
    }
    return inputMethod.TextInputType.NUMBER;
  }
  if (cls === ANDROID_TYPE_CLASS_PHONE) {
    return inputMethod.TextInputType.PHONE;
  }
  if (cls === ANDROID_TYPE_CLASS_DATETIME) {
    return inputMethod.TextInputType.DATETIME;
  }
  if (cls === ANDROID_TYPE_CLASS_TEXT) {
    if (variation === ANDROID_TYPE_TEXT_VARIATION_URI) {
      return inputMethod.TextInputType.URL;
    }
    if (variation === ANDROID_TYPE_TEXT_VARIATION_EMAIL_ADDRESS ||
        variation === ANDROID_TYPE_TEXT_VARIATION_WEB_EMAIL_ADDRESS) {
      return inputMethod.TextInputType.EMAIL_ADDRESS;
    }
    if (variation === ANDROID_TYPE_TEXT_VARIATION_PASSWORD ||
        variation === ANDROID_TYPE_TEXT_VARIATION_VISIBLE_PASSWORD ||
        variation === ANDROID_TYPE_TEXT_VARIATION_WEB_PASSWORD) {
      return inputMethod.TextInputType.VISIBLE_PASSWORD;
    }
    if ((inputType & ANDROID_TYPE_TEXT_FLAG_MULTI_LINE) !== 0) {
      return inputMethod.TextInputType.MULTILINE;
    }
  }
  return inputMethod.TextInputType.TEXT;
}

function mapAndroidEnterKey(imeOptions: number, inputType: number): inputMethod.EnterKeyType {
  const action: number = imeOptions & ANDROID_IME_MASK_ACTION;
  switch (action) {
    case 2: return inputMethod.EnterKeyType.GO;
    case 3: return inputMethod.EnterKeyType.SEARCH;
    case 4: return inputMethod.EnterKeyType.SEND;
    case 5: return inputMethod.EnterKeyType.NEXT;
    case 6: return inputMethod.EnterKeyType.DONE;
    case 7: return inputMethod.EnterKeyType.PREVIOUS;
    default:
      if ((inputType & ANDROID_TYPE_TEXT_FLAG_MULTI_LINE) !== 0) {
        return inputMethod.EnterKeyType.NEWLINE;
      }
      return inputMethod.EnterKeyType.DONE;
  }
}

/**
 * WSA-style shared HarmonyOS system IME controller.
 *
 * Watches Android SvboxHostIme focus events via zero-IPC atomic state in C++,
 * attaches the focused HarmonyOS window directly to HarmonyOS's native system IME
 * (XiaoYi/Sogou/etc.) without any intermediate ArkUI TextInput bridge, and streams
 * preedit/commit/delete/action/cursor events straight into Android's InputConnection.
 */
class HostImeManager {
  private slots: Map<number, SlotImeWindowInfo> = new Map<number, SlotImeWindowInfo>();
  private controller: inputMethod.InputMethodController | undefined = undefined;
  private preAttachBound: boolean = false;
  private postAttachBound: boolean = false;
  private pollTimer: number = -1;
  private lastSeq: number = -1;
  private lastCursorSeq: number = -1;
  private attached: boolean = false;
  private attachedSlot: number = -1;
  private attachedInputType: number = -1;
  private attachedImeOptions: number = -1;
  private attachBusy: boolean = false;
  private manualForced: boolean = false;
  private manualBypassedSlot: number = -1;
  private bypassLostFocusAtMs: number = 0;

  // Stored bound listener references so ctrl.off(type, fn) unregisters cleanly without ImsaKit nullptr warnings
  private readonly insertTextCb = (text: string): void => {
    if (!native.isWmAgentReady() || !text || text.length === 0) return;
    const slot: number = this.attachedSlot >= 0 ? this.attachedSlot : 0;
    native.wmRpcAsync(`IME_COMMIT ${utf8ToBase64(text)} ${slot}`).catch(() => {});
  };

  private readonly deleteLeftCb = (length: number): void => {
    if (!native.isWmAgentReady()) return;
    const n: number = Math.max(1, Math.floor(length));
    native.wmRpcAsync(`IME_DELETE ${n} 0`).catch(() => {});
  };

  private readonly deleteRightCb = (length: number): void => {
    if (!native.isWmAgentReady()) return;
    const n: number = Math.max(1, Math.floor(length));
    native.wmRpcAsync(`IME_DELETE 0 ${n}`).catch(() => {});
  };

  private readonly sendFunctionKeyCb = (fnKey: inputMethod.FunctionKey): void => {
    if (!native.isWmAgentReady()) return;
    const ek: number = fnKey ? Number(fnKey.enterKeyType) : 0;
    native.wmRpcAsync(`IME_ACTION ${ek}`).catch(() => {});
  };

  private readonly moveCursorCb = (direction: inputMethod.Direction): void => {
    if (!native.isWmAgentReady()) return;
    native.wmRpcAsync(`IME_MOVE ${Number(direction)}`).catch(() => {});
  };

  private readonly extendActionCb = (action: inputMethod.ExtendAction): void => {
    if (!native.isWmAgentReady()) return;
    if (action === inputMethod.ExtendAction.PASTE) {
      syncHostClipboardToAndroid(true).then(() => {
        native.wmRpcAsync('IME_EXTEND 5').catch(() => {});
      }).catch(() => {
        native.wmRpcAsync('IME_EXTEND 5').catch(() => {});
      });
    } else {
      native.wmRpcAsync(`IME_EXTEND ${Number(action)}`).catch(() => {});
    }
  };

  private readonly getLeftTextCb = (_length: number): string => {
    return '';
  };

  private readonly getRightTextCb = (_length: number): string => {
    return '';
  };

  private readonly getTextIndexCb = (): number => {
    return 0;
  };

  private ensureController(): inputMethod.InputMethodController | undefined {
    if (!this.controller) {
      try {
        this.controller = inputMethod.getController();
      } catch (err) {
        hilog.error(0x0000, TAG, 'inputMethod.getController failed: %{public}s', String(err));
        return undefined;
      }
    }
    if (this.controller && !this.preAttachBound) {
      this.bindPreAttachListeners(this.controller);
      this.preAttachBound = true;
    }
    return this.controller;
  }

  private bindPreAttachListeners(ctrl: inputMethod.InputMethodController): void {
    // Per @ohos.inputMethod spec, setPreviewText & finishTextPreview must be registered BEFORE attach().
    try {
      ctrl.on('setPreviewText', (text: string, _range: inputMethod.Range) => {
        if (!native.isWmAgentReady()) return;
        const payload: string = text && text.length > 0 ? utf8ToBase64(text) : '-';
        native.wmRpcAsync(`IME_PREEDIT ${payload}`).catch(() => {});
      });
    } catch (err) {
      hilog.warn(0x0000, TAG, 'on(setPreviewText) warn: %{public}s', String(err));
    }

    try {
      ctrl.on('finishTextPreview', () => {
        if (!native.isWmAgentReady()) return;
        native.wmRpcAsync('IME_FINISH_PREEDIT').catch(() => {});
      });
    } catch (err) {
      hilog.warn(0x0000, TAG, 'on(finishTextPreview) warn: %{public}s', String(err));
    }
  }

  private bindPostAttachListeners(ctrl: inputMethod.InputMethodController): void {
    if (this.postAttachBound) return;
    // Per HarmonyOS IMF runtime, insertText/delete/functionKey/moveCursor require an attached client first.
    try {
      ctrl.on('insertText', this.insertTextCb);
    } catch (err) {
      hilog.warn(0x0000, TAG, 'on(insertText) warn: %{public}s', String(err));
    }

    try {
      ctrl.on('deleteLeft', this.deleteLeftCb);
    } catch (err) {
      hilog.warn(0x0000, TAG, 'on(deleteLeft) warn: %{public}s', String(err));
    }

    try {
      ctrl.on('deleteRight', this.deleteRightCb);
    } catch (err) {
      hilog.warn(0x0000, TAG, 'on(deleteRight) warn: %{public}s', String(err));
    }

    try {
      ctrl.on('sendFunctionKey', this.sendFunctionKeyCb);
    } catch (err) {
      hilog.warn(0x0000, TAG, 'on(sendFunctionKey) warn: %{public}s', String(err));
    }

    try {
      ctrl.on('moveCursor', this.moveCursorCb);
    } catch (err) {
      hilog.warn(0x0000, TAG, 'on(moveCursor) warn: %{public}s', String(err));
    }

    try {
      ctrl.on('handleExtendAction', this.extendActionCb);
    } catch (err) {
      hilog.warn(0x0000, TAG, 'on(handleExtendAction) warn: %{public}s', String(err));
    }

    try {
      ctrl.on('getLeftTextOfCursor', this.getLeftTextCb);
      ctrl.on('getRightTextOfCursor', this.getRightTextCb);
      ctrl.on('getTextIndexAtCursor', this.getTextIndexCb);
    } catch (_e) {
    }
    this.postAttachBound = true;
  }

  private unbindPostAttachListeners(ctrl: inputMethod.InputMethodController): void {
    if (!this.postAttachBound) return;
    try { ctrl.off('insertText', this.insertTextCb); } catch (_e) {}
    try { ctrl.off('deleteLeft', this.deleteLeftCb); } catch (_e) {}
    try { ctrl.off('deleteRight', this.deleteRightCb); } catch (_e) {}
    try { ctrl.off('sendFunctionKey', this.sendFunctionKeyCb); } catch (_e) {}
    try { ctrl.off('moveCursor', this.moveCursorCb); } catch (_e) {}
    try { ctrl.off('handleExtendAction', this.extendActionCb); } catch (_e) {}
    try { ctrl.off('getLeftTextOfCursor', this.getLeftTextCb); } catch (_e) {}
    try { ctrl.off('getRightTextOfCursor', this.getRightTextCb); } catch (_e) {}
    try { ctrl.off('getTextIndexAtCursor', this.getTextIndexCb); } catch (_e) {}
    this.postAttachBound = false;
  }

  public registerWindow(slot: number, win: window.Window, topBarVp: number): void {
    this.slots.set(slot, {
      slot: slot,
      win: win,
      topBarVp: topBarVp,
      active: true,
    });
    this.ensurePolling();
  }

  public unregisterWindow(slot: number): void {
    this.slots.delete(slot);
    if (this.manualBypassedSlot === slot) {
      this.manualBypassedSlot = -1;
      this.bypassLostFocusAtMs = 0;
    }
    if (this.attached && this.attachedSlot === slot) {
      this.detachCurrent();
    }
    if (this.slots.size === 0 && this.pollTimer !== -1) {
      clearInterval(this.pollTimer);
      this.pollTimer = -1;
    }
  }

  public setWindowActive(slot: number, active: boolean): void {
    const info: SlotImeWindowInfo | undefined = this.slots.get(slot);
    if (info) {
      info.active = active;
    }
    if (!active && this.attached && this.attachedSlot === slot) {
      this.detachCurrent();
    } else if (active) {
      // Re-sync if Android already has an editable non-bypassed field focused in this window
      try {
        const st: ImeState = native.getImeState();
        if (st.active && st.slot === slot && !this.attached &&
            this.manualBypassedSlot !== slot && !shouldBypassHostImeOnPc(st.inputType)) {
          this.attachToSlot(slot, st.inputType, st.imeOptions, st.lastTapX, st.lastTapY, false);
        }
      } catch (_e) {
      }
    }
  }

  public isAttachedForSlot(slot: number): boolean {
    return this.attached && this.attachedSlot === slot;
  }

  public async toggleManualAttach(slot: number): Promise<boolean> {
    if (this.attached && this.attachedSlot === slot) {
      // User explicitly chose direct keyboard passthrough — remember until they leave text input
      // or click the keyboard icon again so focus hops (e.g. multi-box OTP) do not hijack it back.
      this.manualBypassedSlot = slot;
      this.bypassLostFocusAtMs = 0;
      await this.detachCurrent();
      return false;
    }
    this.manualBypassedSlot = -1;
    this.bypassLostFocusAtMs = 0;
    const st: ImeState = native.getImeState();
    const ok: boolean = await this.attachToSlot(
      slot,
      st.active && st.slot === slot ? st.inputType : ANDROID_TYPE_CLASS_TEXT,
      st.active && st.slot === slot ? st.imeOptions : 0,
      st.lastTapX,
      st.lastTapY,
      true
    );
    return ok;
  }

  private ensurePolling(): void {
    if (this.pollTimer !== -1) return;
    this.pollTimer = setInterval(() => {
      this.pollOnce();
    }, 60);
  }

  private pollOnce(): void {
    if (this.slots.size === 0) return;
    try {
      const st: ImeState = native.getImeState();

      // Clear manual bypass only after Android genuinely leaves text input for >= 1.5s,
      // preventing brief active=0 -> active=1 focus hops across OTP boxes from re-attaching.
      if (this.manualBypassedSlot >= 0) {
        if (!st.active) {
          const now: number = Date.now();
          if (this.bypassLostFocusAtMs === 0) {
            this.bypassLostFocusAtMs = now;
          } else if (now - this.bypassLostFocusAtMs >= 1500) {
            this.manualBypassedSlot = -1;
            this.bypassLostFocusAtMs = 0;
          }
        } else {
          this.bypassLostFocusAtMs = 0;
        }
      }

      if (st.seq !== this.lastSeq) {
        this.lastSeq = st.seq;
        if (st.active) {
          if (this.manualBypassedSlot === st.slot) {
            // Respect user's explicit release into direct keyboard passthrough.
          } else if (!this.manualForced && shouldBypassHostImeOnPc(st.inputType)) {
            if (this.attached && this.attachedSlot === st.slot) {
              hilog.info(0x0000, TAG, 'Detaching Host IME for direct ASCII/password/number field inputType=0x%{public}s',
                st.inputType.toString(16));
              this.detachCurrent();
            }
          } else {
            const target: SlotImeWindowInfo | undefined = this.slots.get(st.slot);
            if (target && target.active) {
              if (this.attached && this.attachedSlot === st.slot) {
                // Already attached to this slot: avoid re-calling ctrl.attach(newEditBox=true) on
                // redundant WebView showSoftInput/onStartInput storms.
                if (this.attachedInputType !== st.inputType || this.attachedImeOptions !== st.imeOptions) {
                  this.updateAttachedAttribute(st.inputType, st.imeOptions);
                }
              } else {
                this.attachToSlot(st.slot, st.inputType, st.imeOptions, st.lastTapX, st.lastTapY, false);
              }
            }
          }
        } else if (!this.manualForced && this.attached) {
          this.detachCurrent();
        }
      }
      if (st.cursorSeq !== this.lastCursorSeq) {
        this.lastCursorSeq = st.cursorSeq;
        if (this.attached && this.attachedSlot === st.cursorSlot && st.cursorX >= 0 && st.cursorY >= 0) {
          this.updateCursorForSlot(st.cursorSlot, st.cursorX, st.cursorY, st.cursorH);
        }
      }
    } catch (_e) {
    }
  }

  private updateAttachedAttribute(inputType: number, imeOptions: number): void {
    const ctrl: inputMethod.InputMethodController | undefined = this.controller;
    if (!ctrl || !this.attached) return;
    this.attachedInputType = inputType;
    this.attachedImeOptions = imeOptions;
    try {
      ctrl.updateAttribute({
        textInputType: mapAndroidInputType(inputType, this.manualForced),
        enterKeyType: mapAndroidEnterKey(imeOptions, inputType),
      }).catch(() => {});
    } catch (_e) {
    }
  }

  private computeScreenCursor(
    info: SlotImeWindowInfo,
    localX: number,
    localY: number,
    heightPx: number,
    isAndroidCoord: boolean
  ): inputMethod.CursorInfo {
    let winLeft: number = 0;
    let winTop: number = 0;
    let winW: number = 820;
    let winH: number = 1280;
    let topBarPx: number = Math.round(info.topBarVp * 2);
    try {
      const props: window.WindowProperties = info.win.getWindowProperties();
      winLeft = props.windowRect.left;
      winTop = props.windowRect.top;
      winW = props.windowRect.width > 0 ? props.windowRect.width : winW;
      winH = props.windowRect.height > 0 ? props.windowRect.height : winH;
    } catch (_e) {
    }
    try {
      topBarPx = Math.round(info.win.getUIContext().vp2px(info.topBarVp));
    } catch (_e) {
    }
    const stageH: number = Math.max(120, winH - topBarPx);
    let sx: number = localX;
    let sy: number = localY;
    let sh: number = heightPx > 0 ? heightPx : 36;
    if (isAndroidCoord && localX >= 0 && localY >= 0) {
      try {
        const ds = native.getDisplayStats(info.slot);
        let dw: number = ds.width > 0 ? ds.width : 720;
        let dh: number = ds.height > 0 ? ds.height : 1280;
        // If stage aspect is flipped relative to native display (rotated Display 0), swap dw/dh
        if ((winW > stageH) !== (dw > dh)) {
          const tmp: number = dw;
          dw = dh;
          dh = tmp;
        }
        sx = localX * (winW / Math.max(1, dw));
        sy = localY * (stageH / Math.max(1, dh));
        sh = sh * (stageH / Math.max(1, dh));
      } catch (_e) {
      }
    }
    const cx: number = sx >= 0 ? Math.min(Math.max(12, sx), Math.max(12, winW - 12)) : Math.floor(winW * 0.35);
    const cy: number = sy >= 0
      ? Math.min(Math.max(12, sy), Math.max(12, stageH - 12))
      : Math.floor(stageH * 0.35);
    return {
      left: Math.round(winLeft + cx),
      top: Math.round(winTop + topBarPx + cy),
      width: 2,
      height: Math.max(40, Math.min(96, Math.round(sh + 12))),
    };
  }

  private updateCursorForSlot(slot: number, x: number, y: number, h: number): void {
    const info: SlotImeWindowInfo | undefined = this.slots.get(slot);
    const ctrl: inputMethod.InputMethodController | undefined = this.controller;
    if (!info || !ctrl || !this.attached) return;
    try {
      const cur: inputMethod.CursorInfo = this.computeScreenCursor(info, x, y, h, true);
      ctrl.updateCursor(cur).catch(() => {});
    } catch (_e) {
    }
  }

  private async attachToSlot(
    slot: number,
    inputType: number,
    imeOptions: number,
    tapX: number,
    tapY: number,
    isManual: boolean
  ): Promise<boolean> {
    if (this.attachBusy) return this.attached;
    const info: SlotImeWindowInfo | undefined = this.slots.get(slot);
    if (!info) return false;
    const ctrl: inputMethod.InputMethodController | undefined = this.ensureController();
    if (!ctrl) return false;

    this.attachBusy = true;
    try {
      if (this.attached && this.attachedSlot !== slot) {
        native.setHostImeActive(this.attachedSlot, false);
        this.unbindPostAttachListeners(ctrl);
        try {
          await ctrl.detach();
        } catch (_e) {
        }
        this.attached = false;
        this.attachedSlot = -1;
        this.attachedInputType = -1;
        this.attachedImeOptions = -1;
      }

      let winId: number = 0;
      try {
        winId = info.win.getWindowProperties().id;
      } catch (_e) {
      }

      const cursor: inputMethod.CursorInfo = this.computeScreenCursor(info, tapX, tapY, 36, false);
      const textConfig: inputMethod.TextConfig = {
        inputAttribute: {
          textInputType: mapAndroidInputType(inputType, isManual),
          enterKeyType: mapAndroidEnterKey(imeOptions, inputType),
        },
        cursorInfo: cursor,
        windowId: winId > 0 ? winId : undefined,
        newEditBox: true,
      };

      // Mark active BEFORE await ctrl.attach so game touch keymaps are suppressed immediately
      // with zero async race window while IMF attaches.
      native.setHostImeActive(slot, true);
      await ctrl.attach(true, textConfig);
      this.bindPostAttachListeners(ctrl);
      if (winId > 0) {
        try {
          await ctrl.setCallingWindow(winId);
        } catch (_e) {
        }
      }
      try {
        await ctrl.updateCursor(cursor);
      } catch (_e) {
      }

      this.attached = true;
      this.attachedSlot = slot;
      this.attachedInputType = inputType;
      this.attachedImeOptions = imeOptions;
      this.manualForced = isManual;
      AppStorage.setOrCreate<number>('hostImeAttachedSlot', slot);
      hilog.info(0x0000, TAG, 'Host IME attached slot=%{public}d winId=%{public}d inputType=0x%{public}s cursor=(%{public}d,%{public}d) manual=%{public}d',
        slot, winId, inputType.toString(16), cursor.left, cursor.top, isManual ? 1 : 0);
      return true;
    } catch (err) {
      hilog.warn(0x0000, TAG, 'Host IME attach(slot=%{public}d) warn: %{public}s', slot, String(err));
      native.setHostImeActive(slot, false);
      return false;
    } finally {
      this.attachBusy = false;
    }
  }

  public async detachCurrent(): Promise<void> {
    if (!this.attached && this.attachedSlot < 0) return;
    const prevSlot: number = this.attachedSlot;
    this.attached = false;
    this.attachedSlot = -1;
    this.attachedInputType = -1;
    this.attachedImeOptions = -1;
    this.manualForced = false;
    if (prevSlot >= 0) {
      try {
        native.setHostImeActive(prevSlot, false);
      } catch (_e) {
      }
    }
    AppStorage.setOrCreate<number>('hostImeAttachedSlot', -1);
    if (this.controller) {
      this.unbindPostAttachListeners(this.controller);
      try {
        await this.controller.detach();
        hilog.info(0x0000, TAG, 'Host IME detached (prevSlot=%{public}d)', prevSlot);
      } catch (_e) {
      }
    }
  }
}

export const hostImeManager: HostImeManager = new HostImeManager();
