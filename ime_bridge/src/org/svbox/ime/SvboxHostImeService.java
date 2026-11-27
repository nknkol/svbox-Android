/* SPDX-License-Identifier: MIT */
package org.svbox.ime;

import android.app.Dialog;
import android.content.Context;
import android.graphics.Matrix;
import android.inputmethodservice.InputMethodService;
import android.net.LocalSocket;
import android.net.LocalSocketAddress;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.Display;
import android.view.KeyEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.Window;
import android.view.inputmethod.CursorAnchorInfo;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputConnection;

import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.Base64;
import java.util.concurrent.LinkedBlockingQueue;

/**
 * Headless WSA-style Android 14 InputMethodService that delegates text composition
 * directly to the HarmonyOS host system IME via SvboxWmAgent (@svbox_ime).
 *
 * Renders zero soft-keyboard UI inside Android so app layouts never shift and
 * HarmonyOS's native IME (XiaoYi/Sogou/etc.) types directly into Android's live
 * InputConnection (including inline Pinyin preedit/composing spans).
 */
public class SvboxHostImeService extends InputMethodService {
    private static final String TAG = "SvboxHostIme";
    private static final String ABSTRACT_SOCK = "svbox_ime";
    private static final int TYPE_MASK_CLASS = 0x0000000f;
    private static final int TYPE_TEXT_FLAG_MULTI_LINE = 0x00020000;

    private static final int ID_SELECT_ALL = 0x0102001f;
    private static final int ID_CUT = 0x01020020;
    private static final int ID_COPY = 0x01020021;
    private static final int ID_PASTE = 0x01020022;

    private Handler mainHandler;
    private volatile boolean destroyed = false;
    private volatile LocalSocket activeSock = null;
    private final LinkedBlockingQueue<String> txQueue = new LinkedBlockingQueue<String>(256);

    private volatile boolean focusActive = false;
    private volatile int focusDisplayId = 0;
    private volatile int focusInputType = 0;
    private volatile int focusImeOptions = 0;
    private volatile int focusShowView = 0;
    private volatile String focusPkg = "-";
    private long lastCursorSentMs = 0L;

    @Override
    public void onCreate() {
        super.onCreate();
        mainHandler = new Handler(Looper.getMainLooper());
        destroyed = false;
        Thread ioThread = new Thread(new Runnable() {
            @Override
            public void run() {
                ipcLoop();
            }
        }, "svbox-ime-ipc");
        ioThread.setDaemon(true);
        ioThread.start();
        Log.i(TAG, "SvboxHostImeService created");
    }

    @Override
    public void onDestroy() {
        destroyed = true;
        sendFocusState(false, focusDisplayId, 0, 0, 0, "-");
        closeActiveSocket();
        super.onDestroy();
        Log.i(TAG, "SvboxHostImeService destroyed");
    }

    @Override
    public boolean onEvaluateInputViewShown() {
        // Always allow onStartInputView/onFinishInputView even when virtual hardware
        // keyboard (svbox-keyboard) is attached, while rendering 0px UI inside Android.
        return true;
    }

    @Override
    public boolean onEvaluateFullscreenMode() {
        return false;
    }

    @Override
    public View onCreateInputView() {
        View empty = new View(this);
        empty.setLayoutParams(new ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0));
        empty.setVisibility(View.GONE);
        return empty;
    }

    @Override
    public void onComputeInsets(Insets outInsets) {
        super.onComputeInsets(outInsets);
        int decorH = 0;
        try {
            Dialog dlg = getWindow();
            if (dlg != null && dlg.getWindow() != null && dlg.getWindow().getDecorView() != null) {
                decorH = dlg.getWindow().getDecorView().getHeight();
            }
        } catch (Throwable ignored) {}
        outInsets.contentTopInsets = decorH;
        outInsets.visibleTopInsets = decorH;
        outInsets.touchableRegion.setEmpty();
        outInsets.touchableInsets = Insets.TOUCHABLE_INSETS_REGION;
    }

    private int resolveDisplayId() {
        try {
            Display d = getDisplay();
            if (d != null) {
                int did = d.getDisplayId();
                if (did >= 0) return did;
            }
        } catch (Throwable ignored) {}
        try {
            Dialog dlg = getWindow();
            if (dlg != null && dlg.getWindow() != null) {
                Context wctx = dlg.getWindow().getContext();
                if (wctx != null) {
                    Display d = wctx.getDisplay();
                    if (d != null) {
                        int did = d.getDisplayId();
                        if (did >= 0) return did;
                    }
                }
            }
        } catch (Throwable ignored) {}
        return 0;
    }

    private void notifyInputStarted(EditorInfo info, boolean showView) {
        int displayId = resolveDisplayId();
        boolean editable = (info != null) && ((info.inputType & TYPE_MASK_CLASS) != 0);
        if (editable) {
            String pkg = (info.packageName != null && !info.packageName.isEmpty())
                    ? info.packageName.replace(' ', '_') : "-";
            sendFocusState(true, displayId, info.inputType, info.imeOptions, showView ? 1 : 0, pkg);
            try {
                InputConnection ic = getCurrentInputConnection();
                if (ic != null) {
                    ic.requestCursorUpdates(
                            InputConnection.CURSOR_UPDATE_IMMEDIATE | InputConnection.CURSOR_UPDATE_MONITOR);
                }
            } catch (Throwable ignored) {}
        } else {
            sendFocusState(false, displayId, 0, 0, 0, "-");
        }
    }

    @Override
    public void onStartInput(EditorInfo attribute, boolean restarting) {
        super.onStartInput(attribute, restarting);
        notifyInputStarted(attribute, false);
    }

    @Override
    public void onStartInputView(EditorInfo info, boolean restarting) {
        super.onStartInputView(info, restarting);
        notifyInputStarted(info, true);
    }

    @Override
    public void onFinishInputView(boolean finishingInput) {
        super.onFinishInputView(finishingInput);
        if (finishingInput) {
            sendFocusState(false, resolveDisplayId(), 0, 0, 0, "-");
        }
    }

    @Override
    public void onFinishInput() {
        super.onFinishInput();
        sendFocusState(false, resolveDisplayId(), 0, 0, 0, "-");
    }

    @Override
    public void onUpdateCursorAnchorInfo(CursorAnchorInfo info) {
        super.onUpdateCursorAnchorInfo(info);
        if (info == null || !focusActive) return;
        long now = System.currentTimeMillis();
        if (now - lastCursorSentMs < 60L) return;
        float x = info.getInsertionMarkerHorizontal();
        float top = info.getInsertionMarkerTop();
        float bot = info.getInsertionMarkerBottom();
        if (Float.isNaN(x) || Float.isNaN(top) || Float.isNaN(bot)) return;
        float[] pts = new float[] { x, top, x, bot };
        try {
            Matrix m = info.getMatrix();
            if (m != null) {
                m.mapPoints(pts);
            }
        } catch (Throwable ignored) {}
        int ix = Math.round(pts[0]);
        int iy = Math.round(pts[1]);
        int ih = Math.max(16, Math.round(pts[3] - pts[1]));
        if (ix < 0 || iy < 0) return;
        lastCursorSentMs = now;
        enqueueTx("IME_CURSOR " + resolveDisplayId() + " " + ix + " " + iy + " " + ih);
    }

    private void sendFocusState(boolean active, int displayId, int inputType, int imeOptions, int showView, String pkg) {
        focusActive = active;
        focusDisplayId = displayId;
        focusInputType = inputType;
        focusImeOptions = imeOptions;
        focusShowView = showView;
        focusPkg = (pkg != null && !pkg.isEmpty()) ? pkg : "-";
        enqueueTx(formatFocusLine());
    }

    private String formatFocusLine() {
        return "IME_FOCUS " + (focusActive ? "1" : "0")
                + " " + focusDisplayId
                + " " + focusInputType
                + " " + focusImeOptions
                + " " + focusShowView
                + " " + focusPkg;
    }

    private void enqueueTx(String line) {
        if (line == null) return;
        if (!txQueue.offer(line)) {
            txQueue.poll();
            txQueue.offer(line);
        }
    }

    private void closeActiveSocket() {
        LocalSocket s = activeSock;
        activeSock = null;
        if (s != null) {
            try { s.close(); } catch (Throwable ignored) {}
        }
    }

    private void ipcLoop() {
        while (!destroyed) {
            LocalSocket sock = null;
            Thread writer = null;
            try {
                sock = new LocalSocket();
                sock.connect(new LocalSocketAddress(ABSTRACT_SOCK, LocalSocketAddress.Namespace.ABSTRACT));
                activeSock = sock;
                final OutputStream os = sock.getOutputStream();
                final LocalSocket boundSock = sock;

                txQueue.clear();
                enqueueTx("IME_REGISTER");
                if (focusActive) {
                    enqueueTx(formatFocusLine());
                }

                writer = new Thread(new Runnable() {
                    @Override
                    public void run() {
                        try {
                            while (!destroyed && activeSock == boundSock) {
                                String msg = txQueue.take();
                                byte[] bytes = (msg + "\n").getBytes(StandardCharsets.UTF_8);
                                os.write(bytes);
                                os.flush();
                            }
                        } catch (Throwable ignored) {
                            try { boundSock.close(); } catch (Throwable ignored2) {}
                        }
                    }
                }, "svbox-ime-tx");
                writer.setDaemon(true);
                writer.start();

                Log.i(TAG, "Connected to @" + ABSTRACT_SOCK);
                BufferedReader br = new BufferedReader(
                        new InputStreamReader(sock.getInputStream(), StandardCharsets.UTF_8));
                String line;
                while (!destroyed && (line = br.readLine()) != null) {
                    final String cmdLine = line.trim();
                    if (!cmdLine.isEmpty()) {
                        mainHandler.post(new Runnable() {
                            @Override
                            public void run() {
                                handleHostCommand(cmdLine);
                            }
                        });
                    }
                }
            } catch (Throwable t) {
                if (!destroyed) {
                    try { Thread.sleep(500); } catch (InterruptedException ignored) {}
                }
            } finally {
                if (activeSock == sock) {
                    activeSock = null;
                }
                if (sock != null) {
                    try { sock.close(); } catch (Throwable ignored) {}
                }
                if (writer != null) {
                    writer.interrupt();
                }
            }
        }
    }

    private void handleHostCommand(String line) {
        try {
            String[] parts = line.split("\\s+", 3);
            String cmd = parts[0];
            InputConnection ic = getCurrentInputConnection();
            if ("HIDE".equals(cmd)) {
                requestHideSelf(0);
                return;
            }
            if (ic == null) {
                return;
            }
            if ("COMMIT".equals(cmd)) {
                String text = parts.length >= 2 ? decodeB64(parts[1]) : "";
                ic.commitText(text, 1);
            } else if ("PREEDIT".equals(cmd)) {
                String text = parts.length >= 2 ? decodeB64(parts[1]) : "";
                if (text.isEmpty()) {
                    ic.setComposingText("", 1);
                    ic.finishComposingText();
                } else {
                    ic.setComposingText(text, 1);
                }
            } else if ("FINISH_PREEDIT".equals(cmd)) {
                ic.finishComposingText();
            } else if ("DELETE".equals(cmd) && parts.length >= 3) {
                int left = parseIntSafe(parts[1], 0);
                int right = parseIntSafe(parts[2], 0);
                if (left > 0) {
                    CharSequence sel = null;
                    try { sel = ic.getSelectedText(0); } catch (Throwable ignored) {}
                    if (sel != null && sel.length() > 0) {
                        ic.commitText("", 1);
                        left -= 1;
                    }
                    if (left > 0) {
                        boolean ok = ic.deleteSurroundingText(left, 0);
                        if (!ok) {
                            for (int i = 0; i < Math.min(left, 32); i++) {
                                sendDownUpKeyEvents(KeyEvent.KEYCODE_DEL);
                            }
                        }
                    }
                }
                if (right > 0) {
                    boolean ok = ic.deleteSurroundingText(0, right);
                    if (!ok) {
                        for (int i = 0; i < Math.min(right, 32); i++) {
                            sendDownUpKeyEvents(KeyEvent.KEYCODE_FORWARD_DEL);
                        }
                    }
                }
            } else if ("ACTION".equals(cmd)) {
                int ohosEnter = parts.length >= 2 ? parseIntSafe(parts[1], 0) : 0;
                ic.finishComposingText();
                EditorInfo ei = getCurrentInputEditorInfo();
                int inType = ei != null ? ei.inputType : focusInputType;
                int imeOpts = ei != null ? ei.imeOptions : focusImeOptions;
                boolean multiLine = (inType & TYPE_TEXT_FLAG_MULTI_LINE) != 0;
                boolean noEnterAct = (imeOpts & EditorInfo.IME_FLAG_NO_ENTER_ACTION) != 0;
                if (ohosEnter == 8 || (multiLine && noEnterAct)) {
                    ic.commitText("\n", 1);
                    return;
                }
                int actionId = mapOhosEnterToAndroidAction(ohosEnter);
                if (actionId == EditorInfo.IME_ACTION_UNSPECIFIED) {
                    actionId = imeOpts & EditorInfo.IME_MASK_ACTION;
                }
                if (actionId >= EditorInfo.IME_ACTION_GO && actionId <= EditorInfo.IME_ACTION_PREVIOUS) {
                    if (!ic.performEditorAction(actionId)) {
                        if (!sendDefaultEditorAction(true)) {
                            sendDownUpKeyEvents(KeyEvent.KEYCODE_ENTER);
                        }
                    }
                } else {
                    if (!sendDefaultEditorAction(true)) {
                        sendDownUpKeyEvents(KeyEvent.KEYCODE_ENTER);
                    }
                }
            } else if ("MOVE".equals(cmd) && parts.length >= 2) {
                int dir = parseIntSafe(parts[1], 0);
                if (dir == 1) sendDownUpKeyEvents(KeyEvent.KEYCODE_DPAD_UP);
                else if (dir == 2) sendDownUpKeyEvents(KeyEvent.KEYCODE_DPAD_DOWN);
                else if (dir == 3) sendDownUpKeyEvents(KeyEvent.KEYCODE_DPAD_LEFT);
                else if (dir == 4) sendDownUpKeyEvents(KeyEvent.KEYCODE_DPAD_RIGHT);
            } else if ("EXTEND".equals(cmd) && parts.length >= 2) {
                int act = parseIntSafe(parts[1], -1);
                if (act == 0) ic.performContextMenuAction(ID_SELECT_ALL);
                else if (act == 3) ic.performContextMenuAction(ID_CUT);
                else if (act == 4) ic.performContextMenuAction(ID_COPY);
                else if (act == 5) ic.performContextMenuAction(ID_PASTE);
            }
        } catch (Throwable t) {
            Log.w(TAG, "handleHostCommand(" + line + ") failed: " + t);
        }
    }

    private static int mapOhosEnterToAndroidAction(int ohosEnter) {
        switch (ohosEnter) {
            case 2: return EditorInfo.IME_ACTION_GO;
            case 3: return EditorInfo.IME_ACTION_SEARCH;
            case 4: return EditorInfo.IME_ACTION_SEND;
            case 5: return EditorInfo.IME_ACTION_NEXT;
            case 6: return EditorInfo.IME_ACTION_DONE;
            case 7: return EditorInfo.IME_ACTION_PREVIOUS;
            default: return EditorInfo.IME_ACTION_UNSPECIFIED;
        }
    }

    private static String decodeB64(String b64) {
        if (b64 == null || b64.isEmpty() || "-".equals(b64)) return "";
        try {
            return new String(Base64.getDecoder().decode(b64), StandardCharsets.UTF_8);
        } catch (Throwable ignored) {
            return "";
        }
    }

    private static int parseIntSafe(String s, int def) {
        try {
            return Integer.parseInt(s);
        } catch (Throwable ignored) {
            return def;
        }
    }
}
