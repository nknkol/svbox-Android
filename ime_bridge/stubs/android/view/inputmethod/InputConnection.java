/* SPDX-License-Identifier: MIT */
package android.view.inputmethod;

public interface InputConnection {
    int CURSOR_UPDATE_IMMEDIATE = 1;
    int CURSOR_UPDATE_MONITOR = 2;

    boolean commitText(CharSequence text, int newCursorPosition);
    boolean setComposingText(CharSequence text, int newCursorPosition);
    boolean finishComposingText();
    boolean deleteSurroundingText(int beforeLength, int afterLength);
    boolean performEditorAction(int editorAction);
    boolean performContextMenuAction(int id);
    boolean requestCursorUpdates(int cursorUpdateMode);
    CharSequence getSelectedText(int flags);
}
