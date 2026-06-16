package com.supercell.phoenix;

import android.app.AlertDialog;
import android.app.NativeActivity;
import android.content.Context;
import android.content.DialogInterface;
import android.graphics.Color;
import android.os.Bundle;
import android.text.Editable;
import android.text.InputType;
import android.text.TextWatcher;
import android.util.SparseArray;
import android.view.KeyEvent;
import android.view.ContextThemeWrapper;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputConnection;
import android.view.inputmethod.InputConnectionWrapper;
import android.view.inputmethod.InputMethodManager;
import android.widget.EditText;
import android.widget.FrameLayout;

public class ClashNativeActivity extends NativeActivity {
    ClashHiddenInputView inputView;
    private final SparseArray<AlertDialog> guestPopups = new SparseArray<>();

    static {
        System.loadLibrary("g");
    }

    static native void nativeCommitText(String text);
    static native void nativeDeleteBackward();
    static native void nativeEnter();
    static native void nativePopupResult(int token, int buttonIndex);

    static void dispatchText(CharSequence text) {
        if (text == null || text.length() == 0) {
            return;
        }

        StringBuilder ascii = new StringBuilder(text.length());
        for (int i = 0; i < text.length();) {
            int codePoint = Character.codePointAt(text, i);
            i += Character.charCount(codePoint);

            if (codePoint == '\n' || codePoint == '\r') {
                flushText(ascii);
                nativeEnter();
                continue;
            }

            if (codePoint >= 0x20 && codePoint <= 0x7E) {
                ascii.append((char) codePoint);
            }
        }
        flushText(ascii);
    }

    private static void flushText(StringBuilder text) {
        if (text.length() == 0) {
            return;
        }
        nativeCommitText(text.toString());
        text.setLength(0);
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        installHiddenInputView();
    }

    void installHiddenInputView() {
        inputView = new ClashHiddenInputView(this);
        inputView.setSingleLine(true);
        inputView.setTextColor(Color.TRANSPARENT);
        inputView.setHintTextColor(Color.TRANSPARENT);
        inputView.setBackgroundColor(Color.TRANSPARENT);
        inputView.setCursorVisible(false);
        inputView.setAlpha(0.01f);
        inputView.setKeyboardActive(false);

        FrameLayout.LayoutParams params = new FrameLayout.LayoutParams(1, 1);
        params.leftMargin = 0;
        params.topMargin = 0;
        addContentView(inputView, params);
        inputView.clearFocus();
    }

    void ensureInputView() {
        if (inputView == null) {
            installHiddenInputView();
        }
    }

    public void showSoftKeyboard() {
        runOnUiThread(new ClashShowKeyboardRunnable(this));
    }

    public void hideSoftKeyboard() {
        runOnUiThread(new ClashHideKeyboardRunnable(this));
    }

    public void showGuestPopup(
        int token,
        String title,
        String message,
        String[] buttons,
        int cancelButtonIndex,
        boolean actionSheet
    ) {
        runOnUiThread(() -> {
            dismissGuestPopupInternal(token);

            final String[] resolvedButtons =
                buttons != null && buttons.length > 0 ? buttons : new String[] {"OK"};
            final int resolvedCancelButtonIndex =
                cancelButtonIndex >= 0 && cancelButtonIndex < resolvedButtons.length
                    ? cancelButtonIndex
                    : (resolvedButtons.length == 1 ? 0 : -1);

            Context themedContext = new ContextThemeWrapper(
                this, android.R.style.Theme_DeviceDefault_Light_Dialog_Alert);
            AlertDialog.Builder builder = new AlertDialog.Builder(themedContext);
            if (title != null && !title.isEmpty()) {
                builder.setTitle(title);
            }
            if (message != null && !message.isEmpty()) {
                builder.setMessage(message);
            }
            builder.setCancelable(resolvedCancelButtonIndex >= 0);
            if (actionSheet || resolvedButtons.length > 3) {
                builder.setItems(resolvedButtons, (dialog, which) -> {
                    guestPopups.remove(token);
                    ClashNativeActivity.nativePopupResult(token, which);
                });
            } else {
                final int[] positiveIndex = {-1};
                final int[] negativeIndex = {-1};
                final int[] neutralIndex = {-1};

                if (resolvedButtons.length == 1) {
                    positiveIndex[0] = 0;
                } else {
                    if (resolvedCancelButtonIndex >= 0) {
                        negativeIndex[0] = resolvedCancelButtonIndex;
                    }
                    for (int i = 0; i < resolvedButtons.length; i++) {
                        if (i == negativeIndex[0]) {
                            continue;
                        }
                        if (positiveIndex[0] < 0) {
                            positiveIndex[0] = i;
                        } else {
                            neutralIndex[0] = i;
                        }
                    }
                    if (negativeIndex[0] < 0 && resolvedButtons.length >= 2) {
                        negativeIndex[0] = 0;
                        if (positiveIndex[0] == 0) {
                            positiveIndex[0] = 1;
                        }
                    }
                }

                if (positiveIndex[0] >= 0) {
                    builder.setPositiveButton(
                        resolvedButtons[positiveIndex[0]],
                        (dialog, which) -> {
                            guestPopups.remove(token);
                            ClashNativeActivity.nativePopupResult(token, positiveIndex[0]);
                        });
                }
                if (negativeIndex[0] >= 0) {
                    builder.setNegativeButton(
                        resolvedButtons[negativeIndex[0]],
                        (dialog, which) -> {
                            guestPopups.remove(token);
                            ClashNativeActivity.nativePopupResult(token, negativeIndex[0]);
                        });
                }
                if (neutralIndex[0] >= 0) {
                    builder.setNeutralButton(
                        resolvedButtons[neutralIndex[0]],
                        (dialog, which) -> {
                            guestPopups.remove(token);
                            ClashNativeActivity.nativePopupResult(token, neutralIndex[0]);
                        });
                }
            }

            AlertDialog dialog = builder.create();
            dialog.setCanceledOnTouchOutside(false);
            dialog.setOnCancelListener(unused -> {
                guestPopups.remove(token);
                ClashNativeActivity.nativePopupResult(
                    token,
                    resolvedCancelButtonIndex >= 0 ? resolvedCancelButtonIndex : 0);
            });
            dialog.setOnDismissListener(unused -> guestPopups.remove(token));
            guestPopups.put(token, dialog);
            dialog.show();
            if (!actionSheet) {
                if (dialog.getButton(DialogInterface.BUTTON_POSITIVE) != null) {
                    dialog.getButton(DialogInterface.BUTTON_POSITIVE).setAllCaps(false);
                }
                if (dialog.getButton(DialogInterface.BUTTON_NEGATIVE) != null) {
                    dialog.getButton(DialogInterface.BUTTON_NEGATIVE).setAllCaps(false);
                }
                if (dialog.getButton(DialogInterface.BUTTON_NEUTRAL) != null) {
                    dialog.getButton(DialogInterface.BUTTON_NEUTRAL).setAllCaps(false);
                }
            }
        });
    }

    public void dismissGuestPopup(int token) {
        runOnUiThread(() -> dismissGuestPopupInternal(token));
    }

    private void dismissGuestPopupInternal(int token) {
        AlertDialog dialog = guestPopups.get(token);
        if (dialog == null) {
            return;
        }
        guestPopups.remove(token);
        dialog.setOnCancelListener(null);
        dialog.setOnDismissListener(null);
        dialog.dismiss();
    }
}

final class ClashHiddenInputView extends EditText {
    private boolean suppressTextChanges = false;
    private String lastText = "";

    ClashHiddenInputView(Context context) {
        super(context);
        addTextChangedListener(new ClashTextWatcher(this));
    }

    @Override
    public boolean onCheckIsTextEditor() {
        return isEnabled();
    }

    @Override
    public InputConnection onCreateInputConnection(EditorInfo outAttrs) {
        outAttrs.inputType = InputType.TYPE_CLASS_TEXT
            | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS
            | InputType.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD;
        outAttrs.imeOptions = EditorInfo.IME_ACTION_DONE | EditorInfo.IME_FLAG_NO_EXTRACT_UI;
        InputConnection base = super.onCreateInputConnection(outAttrs);
        return new ClashNativeInputConnection(base, this);
    }

    void setKeyboardActive(boolean active) {
        setEnabled(active);
        setFocusable(active);
        setFocusableInTouchMode(active);
        setClickable(active);
        setLongClickable(active);
    }

    void resetTextState() {
        suppressTextChanges = true;
        try {
            setText("");
            lastText = "";
        } finally {
            suppressTextChanges = false;
        }
    }

    void handleTextChanged(String text) {
        if (suppressTextChanges || text.equals(lastText)) {
            return;
        }

        int prefix = 0;
        final int maxPrefix = Math.min(lastText.length(), text.length());
        while (prefix < maxPrefix && lastText.charAt(prefix) == text.charAt(prefix)) {
            prefix++;
        }

        for (int i = lastText.length(); i > prefix; i--) {
            ClashNativeActivity.nativeDeleteBackward();
        }

        if (text.length() > prefix) {
            ClashNativeActivity.dispatchText(text.subSequence(prefix, text.length()));
        }

        lastText = text;
    }
}

final class ClashTextWatcher implements TextWatcher {
    private final ClashHiddenInputView inputView;

    ClashTextWatcher(ClashHiddenInputView inputView) {
        this.inputView = inputView;
    }

    @Override
    public void beforeTextChanged(CharSequence s, int start, int count, int after) {
    }

    @Override
    public void onTextChanged(CharSequence s, int start, int before, int count) {
    }

    @Override
    public void afterTextChanged(Editable editable) {
        inputView.handleTextChanged(editable.toString());
    }
}

final class ClashNativeInputConnection extends InputConnectionWrapper {
    private final ClashHiddenInputView inputView;

    ClashNativeInputConnection(InputConnection target, ClashHiddenInputView inputView) {
        super(target, true);
        this.inputView = inputView;
    }

    @Override
    public boolean commitText(CharSequence text, int newCursorPosition) {
        return super.commitText(text, newCursorPosition);
    }

    @Override
    public boolean setComposingText(CharSequence text, int newCursorPosition) {
        return super.setComposingText(text, newCursorPosition);
    }

    @Override
    public boolean finishComposingText() {
        return super.finishComposingText();
    }

    @Override
    public boolean deleteSurroundingText(int beforeLength, int afterLength) {
        if (beforeLength > 0 && inputView.length() == 0) {
            for (int i = 0; i < beforeLength; i++) {
                ClashNativeActivity.nativeDeleteBackward();
            }
            return true;
        }
        return super.deleteSurroundingText(beforeLength, afterLength);
    }

    @Override
    public boolean deleteSurroundingTextInCodePoints(int beforeLength, int afterLength) {
        if (beforeLength > 0 && inputView.length() == 0) {
            for (int i = 0; i < beforeLength; i++) {
                ClashNativeActivity.nativeDeleteBackward();
            }
            return true;
        }
        return super.deleteSurroundingTextInCodePoints(beforeLength, afterLength);
    }

    @Override
    public boolean sendKeyEvent(KeyEvent event) {
        if (event.getAction() == KeyEvent.ACTION_DOWN) {
            if (event.getKeyCode() == KeyEvent.KEYCODE_DEL) {
                if (inputView.length() > 0) {
                    return super.sendKeyEvent(event);
                }
                ClashNativeActivity.nativeDeleteBackward();
                return true;
            }
            if (event.getKeyCode() == KeyEvent.KEYCODE_ENTER) {
                ClashNativeActivity.nativeEnter();
                return true;
            }
        }
        return super.sendKeyEvent(event);
    }

    @Override
    public boolean performEditorAction(int editorAction) {
        ClashNativeActivity.nativeEnter();
        return true;
    }
}

final class ClashShowKeyboardRunnable implements Runnable {
    private final ClashNativeActivity activity;

    ClashShowKeyboardRunnable(ClashNativeActivity activity) {
        this.activity = activity;
    }

    @Override
    public void run() {
        activity.ensureInputView();
        activity.inputView.setKeyboardActive(true);
        activity.inputView.resetTextState();
        activity.inputView.requestFocus();
        activity.inputView.setSelection(activity.inputView.length());
        InputMethodManager imm = (InputMethodManager) activity.getSystemService(Context.INPUT_METHOD_SERVICE);
        if (imm != null) {
            imm.showSoftInput(activity.inputView, InputMethodManager.SHOW_IMPLICIT);
        }
    }
}

final class ClashHideKeyboardRunnable implements Runnable {
    private final ClashNativeActivity activity;

    ClashHideKeyboardRunnable(ClashNativeActivity activity) {
        this.activity = activity;
    }

    @Override
    public void run() {
        if (activity.inputView == null) {
            return;
        }
        InputMethodManager imm = (InputMethodManager) activity.getSystemService(Context.INPUT_METHOD_SERVICE);
        if (imm != null) {
            imm.hideSoftInputFromWindow(activity.inputView.getWindowToken(), 0);
        }
        activity.inputView.resetTextState();
        activity.inputView.clearFocus();
        activity.inputView.setKeyboardActive(false);
    }
}
