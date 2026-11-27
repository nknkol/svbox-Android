/* SPDX-License-Identifier: MIT */
package android.inputmethodservice;

import android.app.Dialog;
import android.content.Context;
import android.graphics.Region;
import android.view.View;
import android.view.inputmethod.CursorAnchorInfo;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputConnection;

public class InputMethodService extends Context {
    public static final class Insets {
        public static final int TOUCHABLE_INSETS_REGION = 3;
        public int contentTopInsets;
        public int visibleTopInsets;
        public final Region touchableRegion = new Region();
        public int touchableInsets;
    }

    public void onCreate() {}
    public void onDestroy() {}
    public View onCreateInputView() { return null; }
    public boolean onEvaluateInputViewShown() { return false; }
    public boolean onEvaluateFullscreenMode() { return false; }
    public void onStartInput(EditorInfo attribute, boolean restarting) {}
    public void onStartInputView(EditorInfo info, boolean restarting) {}
    public void onFinishInputView(boolean finishingInput) {}
    public void onFinishInput() {}
    public void onUpdateCursorAnchorInfo(CursorAnchorInfo cursorAnchorInfo) {}
    public void onComputeInsets(Insets outInsets) {}
    public InputConnection getCurrentInputConnection() { return null; }
    public EditorInfo getCurrentInputEditorInfo() { return null; }
    public Dialog getWindow() { return null; }
    public void sendDownUpKeyEvents(int keyEventCode) {}
    public boolean sendDefaultEditorAction(boolean fromEnterKey) { return false; }
    public void requestHideSelf(int flags) {}
}
