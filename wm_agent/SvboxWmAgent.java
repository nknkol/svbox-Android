/* SPDX-License-Identifier: MIT */
package org.svbox.wm;

import android.app.ActivityManager;
import android.app.TaskStackListener;
import android.content.ComponentName;
import android.hardware.display.IVirtualDisplayCallback;
import android.view.Surface;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStreamReader;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Base64;
import java.util.Collections;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.CopyOnWriteArrayList;

/**
 * Resident Android 14 multi-display, app-metadata, and cross-app task-routing agent.
 * Runs inside the container via app_process64 with zero modifications to read-only rootfs14/.
 */
public class SvboxWmAgent {
    static {
        String libDir = System.getProperty("svbox.jni.dir", "/svbox/bin");
        System.load(libDir + "/libsvbox_wm_jni.so");
    }

    private static native long nativeCreateReader(int slot, int width, int height, String presentSock);
    private static native Surface nativeGetReaderSurface(long handle);
    private static native long nativeGetReaderFrames(long handle);
    private static native void nativeDestroyReader(long handle);

    private static native int nativeListenUnix(String path);
    private static native int nativeAcceptUnix(int listenFd);
    private static native String nativeReadLine(int fd);
    private static native boolean nativeWriteLine(int fd, String line);
    private static native void nativeCloseFd(int fd);

    private static final Set<String> EXEMPT_CROSS_APP_PKGS = new HashSet<String>();
    static {
        EXEMPT_CROSS_APP_PKGS.add("android");
        EXEMPT_CROSS_APP_PKGS.add("com.android.systemui");
        EXEMPT_CROSS_APP_PKGS.add("com.android.permissioncontroller");
        EXEMPT_CROSS_APP_PKGS.add("com.google.android.permissioncontroller");
        EXEMPT_CROSS_APP_PKGS.add("com.android.intentresolver");
        EXEMPT_CROSS_APP_PKGS.add("com.android.documentsui");
        EXEMPT_CROSS_APP_PKGS.add("com.android.packageinstaller");
        EXEMPT_CROSS_APP_PKGS.add("com.google.android.packageinstaller");
        EXEMPT_CROSS_APP_PKGS.add("com.android.launcher3");
        EXEMPT_CROSS_APP_PKGS.add("com.android.quickstep");
        EXEMPT_CROSS_APP_PKGS.add("com.android.inputmethod.latin");
        EXEMPT_CROSS_APP_PKGS.add("org.svbox.ime");
    }

    private static final class DisplaySlot {
        int slot;
        int androidDisplayId;
        String uniqueId;
        String inputPort;
        String boundPkg;
        int width;
        int height;
        int dpi;
        int fps;
        long readerHandle;
        Surface surface;
        IVirtualDisplayCallback.Stub callback;
    }

    private final String ctrlSockPath;
    private final String presentSockPath;
    private final String iconsDir;

    private Object systemContext;
    private Object displayMgr;
    private Object inputMgr;
    private Object windowMgr;
    private Object atm;
    private Object am;
    private Object clipboardMgr;

    private final Object slotLock = new Object();
    private final Object resizeLock = new Object();
    private final Object clipLock = new Object();
    private final Object imeLock = new Object();
    private volatile int imeFd = -1;
    private volatile boolean imeFocused = false;
    private volatile int imeFocusSlot = 0;
    private volatile String lastClipText = "";
    private final Map<Integer, DisplaySlot> slotsByIdx = new HashMap<Integer, DisplaySlot>();
    private final Map<Integer, DisplaySlot> slotsByAndroidId = new HashMap<Integer, DisplaySlot>();
    private final CopyOnWriteArrayList<Integer> eventFds = new CopyOnWriteArrayList<Integer>();
    private final Set<Integer> notifiedTasks = Collections.newSetFromMap(new ConcurrentHashMap<Integer, Boolean>());
    private final Map<Integer, Long> taskFirstSeenMs = new ConcurrentHashMap<Integer, Long>();

    public SvboxWmAgent(String ctrlSockPath, String presentSockPath, String iconsDir) {
        this.ctrlSockPath = ctrlSockPath;
        this.presentSockPath = presentSockPath;
        this.iconsDir = iconsDir;
    }

    public static void main(String[] args) {
        String ctrlSock = args.length > 0 ? args[0] : "/ipc/wm_agent.sock";
        String presentSock = args.length > 1 ? args[1] : "/ipc/gfxstream.sock.present";
        String iconsDir = args.length > 2 ? args[2] : "/ipc/icons";
        SvboxWmAgent agent = new SvboxWmAgent(ctrlSock, presentSock, iconsDir);
        agent.run();
    }

    private void run() {
        try {
            Class<?> looperCls = Class.forName("android.os.Looper");
            try {
                looperCls.getMethod("prepareMainLooper").invoke(null);
            } catch (Throwable ignored) {}

            initServices();
            registerTaskListener();

            Thread serverThread = new Thread(new Runnable() {
                @Override
                public void run() {
                    serveLoop();
                }
            }, "svbox-wm-server");
            serverThread.setDaemon(false);
            serverThread.start();

            Thread clipThread = new Thread(new Runnable() {
                @Override
                public void run() {
                    clipboardWatchLoop();
                }
            }, "svbox-clip-watch");
            clipThread.setDaemon(true);
            clipThread.start();

            Thread imeThread = new Thread(new Runnable() {
                @Override
                public void run() {
                    serveImeLoop();
                }
            }, "svbox-ime-server");
            imeThread.setDaemon(true);
            imeThread.start();

            Thread imeEnsureThread = new Thread(new Runnable() {
                @Override
                public void run() {
                    ensureHostImeActiveLoop();
                }
            }, "svbox-ime-ensure");
            imeEnsureThread.setDaemon(true);
            imeEnsureThread.start();

            log("SvboxWmAgent ready: ctrl=" + ctrlSockPath + " present=" + presentSockPath);
            looperCls.getMethod("loop").invoke(null);
        } catch (Throwable t) {
            log("Fatal error in SvboxWmAgent: " + t);
            t.printStackTrace();
            System.exit(1);
        }
    }

    private void initServices() throws Exception {
        // Initialize ActivityThread system context for PackageManager / Drawable / Bitmap
        try {
            Class<?> atCls = Class.forName("android.app.ActivityThread");
            Object at = atCls.getMethod("systemMain").invoke(null);
            systemContext = atCls.getMethod("getSystemContext").invoke(at);
        } catch (Throwable t) {
            log("ActivityThread.systemMain warning: " + t);
        }

        Class<?> smCls = Class.forName("android.os.ServiceManager");
        Method getSvc = smCls.getMethod("getService", String.class);

        displayMgr = asInterface("android.hardware.display.IDisplayManager$Stub",
                getSvc.invoke(null, "display"));
        inputMgr = asInterface("android.hardware.input.IInputManager$Stub",
                getSvc.invoke(null, "input"));
        try {
            windowMgr = asInterface("android.view.IWindowManager$Stub",
                    getSvc.invoke(null, "window"));
        } catch (Throwable t) {
            log("IWindowManager init warning: " + t);
        }
        atm = asInterface("android.app.IActivityTaskManager$Stub",
                getSvc.invoke(null, "activity_task"));
        am = asInterface("android.app.IActivityManager$Stub",
                getSvc.invoke(null, "activity"));
        try {
            clipboardMgr = asInterface("android.content.IClipboard$Stub",
                    getSvc.invoke(null, "clipboard"));
        } catch (Throwable t) {
            log("IClipboard init warning: " + t);
        }

        File dir = new File(iconsDir);
        if (!dir.exists()) {
            dir.mkdirs();
        }
    }

    private static Object asInterface(String stubClassName, Object binder) throws Exception {
        if (binder == null) return null;
        Class<?> stubCls = Class.forName(stubClassName);
        Class<?> ibinderCls = Class.forName("android.os.IBinder");
        return stubCls.getMethod("asInterface", ibinderCls).invoke(null, binder);
    }

    private void registerTaskListener() {
        if (atm == null) return;
        try {
            TaskStackListener listener = new TaskStackListener() {
                @Override
                public void onTaskCreated(int taskId, ComponentName componentName) {
                    if (componentName != null) {
                        handleTaskEvent(taskId, -1, componentName.getPackageName(),
                                componentName.flattenToShortString());
                    }
                }

                @Override
                public void onTaskMovedToFront(ActivityManager.RunningTaskInfo info) {
                    if (info != null && info.topActivity != null) {
                        handleTaskEvent(info.taskId, info.displayId,
                                info.topActivity.getPackageName(),
                                info.topActivity.flattenToShortString());
                    }
                }

                @Override
                public void onTaskRemovalStarted(ActivityManager.RunningTaskInfo info) {
                    if (info == null) return;
                    final int dId = info.displayId;
                    notifiedTasks.remove(info.taskId);
                    taskFirstSeenMs.remove(info.taskId);
                    final DisplaySlot slot;
                    synchronized (slotLock) {
                        slot = slotsByAndroidId.get(dId);
                    }
                    if (slot != null) {
                        new Thread(new Runnable() {
                            @Override
                            public void run() {
                                try { Thread.sleep(250); } catch (InterruptedException ignored) {}
                                checkDisplayEmpty(slot);
                            }
                        }).start();
                    }
                }
            };
            Class<?> itlCls = Class.forName("android.app.ITaskStackListener");
            atm.getClass().getMethod("registerTaskStackListener", itlCls).invoke(atm, listener);
            log("Registered ITaskStackListener");
        } catch (Throwable t) {
            log("registerTaskStackListener warning: " + t);
        }
    }

    private void handleTaskEvent(int taskId, int knownDisplayId, String pkg, String comp) {
        if (pkg == null || pkg.isEmpty() || EXEMPT_CROSS_APP_PKGS.contains(pkg)) {
            return;
        }
        taskFirstSeenMs.putIfAbsent(Integer.valueOf(taskId), Long.valueOf(System.currentTimeMillis()));

        Object rootTask = findRootTaskInfo(taskId);
        int displayId = knownDisplayId;
        if (rootTask != null) {
            int rootTid = getIntField(rootTask, "taskId", -1);
            int rootDid = getIntField(rootTask, "displayId", -1);
            if (rootDid >= 0) displayId = rootDid;
            if (rootTid > 0) taskId = rootTid;
        } else if (displayId < 0) {
            displayId = findDisplayForTask(taskId);
        }
        if (displayId <= 0) return;

        DisplaySlot slot;
        synchronized (slotLock) {
            slot = slotsByAndroidId.get(displayId);
        }
        if (slot == null || slot.boundPkg == null || slot.boundPkg.isEmpty()) return;
        if (pkg.equals(slot.boundPkg)) return;

        // Guard against embedded activities (e.g. billing sheet, OAuth, Custom Tabs) running
        // inside the host app's own root task: never rip the host's own task out of its window!
        if (rootTask != null) {
            String basePkg = getComponentPackage(getFieldObj(rootTask, "baseActivity"));
            String origPkg = getComponentPackage(getFieldObj(rootTask, "origActivity"));
            if (slot.boundPkg.equals(basePkg) || slot.boundPkg.equals(origPkg)) {
                return;
            }
            int numAct = getIntField(rootTask, "numActivities", 1);
            if (numAct <= 0) return;
        }

        if (!notifiedTasks.add(taskId)) return;

        String label = resolveAppLabel(pkg);
        String ev = "{\"event\":\"CROSS_APP_LAUNCH\""
                + ",\"taskId\":" + taskId
                + ",\"fromSlot\":" + slot.slot
                + ",\"fromPkg\":" + jsonStr(slot.boundPkg)
                + ",\"packageName\":" + jsonStr(pkg)
                + ",\"component\":" + jsonStr(comp != null ? comp : "")
                + ",\"label\":" + jsonStr(label)
                + "}";
        log("Emit " + ev);
        broadcastEvent(ev);
    }

    private String getComponentPackage(Object compObj) {
        if (compObj == null) return null;
        try {
            return (String) compObj.getClass().getMethod("getPackageName").invoke(compObj);
        } catch (Throwable ignored) {
            return null;
        }
    }

    private Object findRootTaskInfo(int taskId) {
        if (atm == null || taskId <= 0) return null;
        try {
            @SuppressWarnings("unchecked")
            List<Object> tasks = (List<Object>) atm.getClass()
                    .getMethod("getAllRootTaskInfos").invoke(atm);
            if (tasks == null) return null;
            for (Object t : tasks) {
                int tid = getIntField(t, "taskId", -1);
                if (tid == taskId) return t;
                Object childIds = getFieldObj(t, "childTaskIds");
                if (childIds instanceof int[]) {
                    for (int cid : (int[]) childIds) {
                        if (cid == taskId) return t;
                    }
                }
            }
        } catch (Throwable ignored) {}
        return null;
    }

    private Object findLiveTaskForPackage(String pkg) {
        if (atm == null || pkg == null || pkg.isEmpty()) return null;
        try {
            @SuppressWarnings("unchecked")
            List<Object> tasks = (List<Object>) atm.getClass()
                    .getMethod("getAllRootTaskInfos").invoke(atm);
            if (tasks == null) return null;
            for (Object t : tasks) {
                int tid = getIntField(t, "taskId", -1);
                int numAct = getIntField(t, "numActivities", 0);
                if (tid <= 0 || numAct <= 0) continue;
                String basePkg = getComponentPackage(getFieldObj(t, "baseActivity"));
                String topPkg = getComponentPackage(getFieldObj(t, "topActivity"));
                String origPkg = getComponentPackage(getFieldObj(t, "origActivity"));
                if (pkg.equals(basePkg) || pkg.equals(topPkg) || pkg.equals(origPkg)) {
                    return t;
                }
            }
        } catch (Throwable ignored) {}
        return null;
    }

    private float getTransitionAnimationScale() {
        if (windowMgr == null) return 1.0f;
        try {
            Object val = windowMgr.getClass().getMethod("getAnimationScale", int.class)
                    .invoke(windowMgr, 1 /* TRANSITION_ANIMATION_SCALE */);
            if (val instanceof Float) {
                return ((Float) val).floatValue();
            }
        } catch (Throwable ignored) {}
        return 1.0f;
    }

    private long getSlotFrames(int slot) {
        if (slot <= 0) return 0;
        long handle = 0;
        synchronized (slotLock) {
            DisplaySlot ds = slotsByIdx.get(slot);
            if (ds != null) handle = ds.readerHandle;
        }
        return handle != 0 ? nativeGetReaderFrames(handle) : 0;
    }

    private int findDisplayForTask(int taskId) {
        Object t = findRootTaskInfo(taskId);
        return t != null ? getIntField(t, "displayId", -1) : -1;
    }

    private void checkDisplayEmpty(DisplaySlot slot) {
        if (atm == null) return;
        synchronized (slotLock) {
            if (!slotsByIdx.containsKey(slot.slot)) return;
        }
        try {
            @SuppressWarnings("unchecked")
            List<Object> tasks = (List<Object>) atm.getClass()
                    .getMethod("getAllRootTaskInfosOnDisplay", int.class)
                    .invoke(atm, slot.androidDisplayId);
            int realCount = 0;
            if (tasks != null) {
                for (Object t : tasks) {
                    if (getIntField(t, "numActivities", 0) > 0) realCount++;
                }
            }
            if (realCount == 0) {
                String ev = "{\"event\":\"DISPLAY_EMPTY\",\"slot\":" + slot.slot
                        + ",\"packageName\":" + jsonStr(slot.boundPkg != null ? slot.boundPkg : "")
                        + "}";
                broadcastEvent(ev);
            }
        } catch (Throwable ignored) {}
    }

    private void broadcastEvent(String jsonLine) {
        for (Integer fd : eventFds) {
            if (!nativeWriteLine(fd.intValue(), jsonLine)) {
                eventFds.remove(fd);
                nativeCloseFd(fd.intValue());
            }
        }
    }

    private void serveLoop() {
        int listenFd = nativeListenUnix(ctrlSockPath);
        if (listenFd < 0) {
            log("Failed to listen on " + ctrlSockPath);
            return;
        }
        while (true) {
            final int cfd = nativeAcceptUnix(listenFd);
            if (cfd < 0) continue;
            Thread t = new Thread(new Runnable() {
                @Override
                public void run() {
                    handleClient(cfd);
                }
            }, "svbox-wm-cli");
            t.setDaemon(true);
            t.start();
        }
    }

    private void handleClient(int fd) {
        boolean isEventSub = false;
        try {
            while (true) {
                String line = nativeReadLine(fd);
                if (line == null) break;
                line = line.trim();
                if (line.isEmpty()) continue;
                if ("SUBSCRIBE_EVENTS".equals(line)) {
                    isEventSub = true;
                    eventFds.add(Integer.valueOf(fd));
                    nativeWriteLine(fd, "{\"ok\":true,\"subscribed\":true}");
                    return; // keep fd open in eventFds
                }
                String reply = dispatchCommand(line);
                if (!nativeWriteLine(fd, reply)) break;
            }
        } catch (Throwable t) {
            log("handleClient error: " + t);
        } finally {
            if (!isEventSub) {
                nativeCloseFd(fd);
            }
        }
    }

    private String dispatchCommand(String line) {
        try {
            String[] parts = line.split("\\s+");
            String cmd = parts[0];
            if ("PING".equals(cmd)) {
                return "{\"ok\":true,\"pong\":true}";
            } else if ("LIST_APPS".equals(cmd)) {
                boolean forceIcons = parts.length > 1 && "1".equals(parts[1]);
                return cmdListApps(forceIcons);
            } else if ("CREATE_DISPLAY".equals(cmd) && parts.length >= 5) {
                int slot = Integer.parseInt(parts[1]);
                int w = Integer.parseInt(parts[2]);
                int h = Integer.parseInt(parts[3]);
                int dpi = Integer.parseInt(parts[4]);
                int fps = parts.length >= 6 ? Integer.parseInt(parts[5]) : 60;
                String pkg = parts.length >= 7 ? parts[6] : "";
                return cmdCreateDisplay(slot, w, h, dpi, fps, pkg);
            } else if ("RESIZE_DISPLAY".equals(cmd) && parts.length >= 5) {
                int slot = Integer.parseInt(parts[1]);
                int w = Integer.parseInt(parts[2]);
                int h = Integer.parseInt(parts[3]);
                int dpi = Integer.parseInt(parts[4]);
                return cmdResizeDisplay(slot, w, h, dpi);
            } else if ("DESTROY_DISPLAY".equals(cmd) && parts.length >= 2) {
                int slot = Integer.parseInt(parts[1]);
                return cmdDestroyDisplay(slot);
            } else if ("LAUNCH_APP".equals(cmd) && parts.length >= 3) {
                int slot = Integer.parseInt(parts[1]);
                String pkg = parts[2];
                String comp = parts.length >= 4 ? parts[3] : "";
                return cmdLaunchApp(slot, pkg, comp);
            } else if ("MOVE_TASK".equals(cmd) && parts.length >= 3) {
                int taskId = Integer.parseInt(parts[1]);
                int targetSlot = Integer.parseInt(parts[2]);
                String newPkg = parts.length >= 4 ? parts[3] : "";
                return cmdMoveTask(taskId, targetSlot, newPkg);
            } else if ("POP_OUT_EMULATOR".equals(cmd) && parts.length >= 2) {
                int targetSlot = Integer.parseInt(parts[1]);
                return cmdPopOutEmulator(targetSlot);
            } else if ("PULL_BACK_TO_EMULATOR".equals(cmd) && parts.length >= 2) {
                int fromSlot = Integer.parseInt(parts[1]);
                return cmdPullBackToEmulator(fromSlot);
            } else if ("FOCUS_DISPLAY".equals(cmd) && parts.length >= 2) {
                int slot = Integer.parseInt(parts[1]);
                return cmdFocusDisplay(slot);
            } else if ("SEND_KEY".equals(cmd) && parts.length >= 3) {
                int slot = Integer.parseInt(parts[1]);
                int keyCode = Integer.parseInt(parts[2]);
                return cmdSendKey(slot, keyCode, 0);
            } else if ("SEND_KEY_COMBO".equals(cmd) && parts.length >= 4) {
                int slot = Integer.parseInt(parts[1]);
                int keyCode = Integer.parseInt(parts[2]);
                int metaState = Integer.parseInt(parts[3]);
                return cmdSendKey(slot, keyCode, metaState);
            } else if ("SET_CLIPBOARD".equals(cmd) && parts.length >= 2) {
                return cmdSetClipboard(parts[1]);
            } else if ("GET_CLIPBOARD".equals(cmd)) {
                return cmdGetClipboard();
            } else if ("INPUT_TEXT".equals(cmd) && parts.length >= 3) {
                int slot = Integer.parseInt(parts[1]);
                return cmdInputText(slot, parts[2]);
            } else if ("IME_COMMIT".equals(cmd) && parts.length >= 2) {
                int slot = parts.length >= 3 ? parseIntSafe(parts[2], imeFocusSlot) : imeFocusSlot;
                return cmdImeCommit(parts[1], slot);
            } else if ("IME_PREEDIT".equals(cmd)) {
                String b64 = parts.length >= 2 ? parts[1] : "-";
                return cmdImeSend("PREEDIT " + b64);
            } else if ("IME_FINISH_PREEDIT".equals(cmd)) {
                return cmdImeSend("FINISH_PREEDIT");
            } else if ("IME_DELETE".equals(cmd) && parts.length >= 3) {
                int left = Integer.parseInt(parts[1]);
                int right = Integer.parseInt(parts[2]);
                return cmdImeDelete(left, right);
            } else if ("IME_ACTION".equals(cmd)) {
                int enterType = parts.length >= 2 ? Integer.parseInt(parts[1]) : 0;
                return cmdImeAction(enterType);
            } else if ("IME_MOVE".equals(cmd) && parts.length >= 2) {
                int dir = Integer.parseInt(parts[1]);
                return cmdImeSend("MOVE " + dir);
            } else if ("IME_EXTEND".equals(cmd) && parts.length >= 2) {
                int act = Integer.parseInt(parts[1]);
                return cmdImeSend("EXTEND " + act);
            } else if ("IME_HIDE".equals(cmd)) {
                return cmdImeSend("HIDE");
            } else if ("IME_STATUS".equals(cmd)) {
                return "{\"ok\":true,\"connected\":" + (imeFd >= 0 ? "true" : "false")
                        + ",\"focused\":" + (imeFocused ? "true" : "false")
                        + ",\"slot\":" + imeFocusSlot + "}";
            } else if ("SET_DISPLAY_ROTATION".equals(cmd) && parts.length >= 2) {
                int rot = Integer.parseInt(parts[1]);
                return cmdSetDisplayRotation(rot);
            } else if ("GET_DISPLAY_ROTATION".equals(cmd)) {
                return cmdGetDisplayRotation();
            } else if ("CLOSE_APP".equals(cmd) && parts.length >= 2) {
                int slot = Integer.parseInt(parts[1]);
                String pkg = parts.length >= 3 ? parts[2] : "";
                return cmdCloseApp(slot, pkg);
            } else if ("GET_TASKS".equals(cmd)) {
                int slot = parts.length >= 2 ? Integer.parseInt(parts[1]) : -1;
                return cmdGetTasks(slot);
            } else if ("TRIM_MEMORY".equals(cmd)) {
                return cmdTrimMemory();
            }
            return "{\"ok\":false,\"error\":\"unknown command\"}";
        } catch (Throwable t) {
            log("Command failed [" + line + "]: " + t);
            return "{\"ok\":false,\"error\":" + jsonStr(String.valueOf(t)) + "}";
        }
    }

    private String cmdCreateDisplay(int slot, int w, int h, int dpi, int fps, String pkg) throws Exception {
        if (slot < 1 || slot > 8) return "{\"ok\":false,\"error\":\"invalid slot\"}";
        if (w < 240) w = 240;
        if (h < 240) h = 240;
        if (dpi < 120) dpi = 240;
        if (fps < 30 || fps > 144) fps = 60;

        // Destroy any stale display in this slot first
        cmdDestroyDisplay(slot);

        long readerHandle = nativeCreateReader(slot, w, h, presentSockPath);
        if (readerHandle == 0) {
            return "{\"ok\":false,\"error\":\"AImageReader creation failed\"}";
        }
        Surface surface = nativeGetReaderSurface(readerHandle);
        if (surface == null) {
            nativeDestroyReader(readerHandle);
            return "{\"ok\":false,\"error\":\"AImageReader getWindow failed\"}";
        }

        int flags = getStaticInt("android.hardware.display.DisplayManager", "VIRTUAL_DISPLAY_FLAG_PUBLIC", 1)
                | getStaticInt("android.hardware.display.DisplayManager", "VIRTUAL_DISPLAY_FLAG_OWN_CONTENT_ONLY", 8)
                | getStaticInt("android.hardware.display.DisplayManager", "VIRTUAL_DISPLAY_FLAG_SUPPORTS_TOUCH", 64)
                | getStaticInt("android.hardware.display.DisplayManager", "VIRTUAL_DISPLAY_FLAG_ROTATES_WITH_CONTENT", 128)
                | getStaticInt("android.hardware.display.DisplayManager", "VIRTUAL_DISPLAY_FLAG_DESTROY_CONTENT_ON_REMOVAL", 256)
                | getStaticInt("android.hardware.display.DisplayManager", "VIRTUAL_DISPLAY_FLAG_TRUSTED", 1024);

        String dispName = "svbox-app-" + slot;
        String reqUniqueId = "svbox-vdisp-" + slot;
        Class<?> builderCls = Class.forName("android.hardware.display.VirtualDisplayConfig$Builder");
        Constructor<?> ctor = builderCls.getConstructor(String.class, int.class, int.class, int.class);
        Object builder = ctor.newInstance(dispName, w, h, dpi);
        builderCls.getMethod("setFlags", int.class).invoke(builder, flags);
        builderCls.getMethod("setSurface", Surface.class).invoke(builder, surface);
        try {
            builderCls.getMethod("setRequestedRefreshRate", float.class).invoke(builder, (float) fps);
        } catch (Throwable ignored) {}
        try {
            builderCls.getMethod("setUniqueId", String.class).invoke(builder, reqUniqueId);
        } catch (Throwable ignored) {}
        Object config = builderCls.getMethod("build").invoke(builder);

        IVirtualDisplayCallback.Stub cb = new IVirtualDisplayCallback.Stub() {
            @Override
            public void onPaused() {}
            @Override
            public void onResumed() {}
            @Override
            public void onStopped() {}
        };
        Class<?> cfgCls = Class.forName("android.hardware.display.VirtualDisplayConfig");
        Class<?> cbIface = Class.forName("android.hardware.display.IVirtualDisplayCallback");
        Class<?> projIface = Class.forName("android.media.projection.IMediaProjection");
        Method createMethod = displayMgr.getClass().getMethod(
                "createVirtualDisplay", cfgCls, cbIface, projIface, String.class);
        int androidDisplayId = ((Integer) createMethod.invoke(
                displayMgr, config, cb, null, "com.android.shell")).intValue();

        if (androidDisplayId <= 0) {
            surface.release();
            nativeDestroyReader(readerHandle);
            return "{\"ok\":false,\"error\":\"createVirtualDisplay returned " + androidDisplayId + "\"}";
        }

        // Read actual uniqueId from DisplayInfo and associate virtual evdev port svbox-disp-<slot>
        String actualUniqueId = reqUniqueId;
        try {
            Object dInfo = displayMgr.getClass().getMethod("getDisplayInfo", int.class)
                    .invoke(displayMgr, androidDisplayId);
            if (dInfo != null) {
                Object uid = getFieldObj(dInfo, "uniqueId");
                if (uid instanceof String && !((String) uid).isEmpty()) {
                    actualUniqueId = (String) uid;
                }
            }
        } catch (Throwable t) {
            log("getDisplayInfo warning: " + t);
        }

        String inputPort = "svbox-disp-" + slot;
        try {
            if (inputMgr != null) {
                inputMgr.getClass().getMethod("removeUniqueIdAssociation", String.class)
                        .invoke(inputMgr, inputPort);
                inputMgr.getClass().getMethod("addUniqueIdAssociation", String.class, String.class)
                        .invoke(inputMgr, inputPort, actualUniqueId);
            }
        } catch (Throwable t) {
            log("addUniqueIdAssociation(" + inputPort + " -> " + actualUniqueId + ") warning: " + t);
        }

        // Disable system decorations (nav/status bar) on single-app display so HarmonyOS title bar owns chrome
        try {
            Object wm = asInterface("android.view.IWindowManager$Stub",
                    Class.forName("android.os.ServiceManager").getMethod("getService", String.class)
                            .invoke(null, "window"));
            if (wm != null) {
                try {
                    wm.getClass().getMethod("setShouldShowSystemDecors", int.class, boolean.class)
                            .invoke(wm, androidDisplayId, false);
                } catch (Throwable ignored) {}
                try {
                    wm.getClass().getMethod("setDisplayImePolicy", int.class, int.class)
                            .invoke(wm, androidDisplayId, 0 /* DISPLAY_IME_POLICY_LOCAL */);
                } catch (Throwable ignored) {}
            }
        } catch (Throwable ignored) {}

        DisplaySlot ds = new DisplaySlot();
        ds.slot = slot;
        ds.androidDisplayId = androidDisplayId;
        ds.uniqueId = actualUniqueId;
        ds.inputPort = inputPort;
        ds.boundPkg = pkg;
        ds.width = w;
        ds.height = h;
        ds.dpi = dpi;
        ds.fps = fps;
        ds.readerHandle = readerHandle;
        ds.surface = surface;
        ds.callback = cb;

        synchronized (slotLock) {
            slotsByIdx.put(slot, ds);
            slotsByAndroidId.put(androidDisplayId, ds);
        }

        log("Created VirtualDisplay slot=" + slot + " displayId=" + androidDisplayId
                + " uniqueId=" + actualUniqueId + " " + w + "x" + h + "@" + dpi + "dpi " + fps + "Hz");
        return "{\"ok\":true,\"slot\":" + slot
                + ",\"displayId\":" + androidDisplayId
                + ",\"uniqueId\":" + jsonStr(actualUniqueId)
                + ",\"width\":" + w + ",\"height\":" + h + ",\"dpi\":" + dpi + "}";
    }

    private String cmdResizeDisplay(int slot, int w, int h, int dpi) throws Exception {
        if (w < 240) w = 240;
        if (h < 240) h = 240;
        if (dpi < 120) dpi = 240;

        synchronized (resizeLock) {
            DisplaySlot ds;
            synchronized (slotLock) {
                ds = slotsByIdx.get(slot);
            }
            if (ds == null) return "{\"ok\":false,\"error\":\"slot not active\"}";
            if (ds.width == w && ds.height == h && ds.dpi == dpi) {
                return "{\"ok\":true,\"slot\":" + slot + ",\"displayId\":" + ds.androidDisplayId
                        + ",\"width\":" + w + ",\"height\":" + h + ",\"dpi\":" + dpi + ",\"unchanged\":true}";
            }

            long newReader = nativeCreateReader(slot, w, h, presentSockPath);
            if (newReader == 0) {
                return "{\"ok\":false,\"error\":\"new AImageReader failed\"}";
            }
            Surface newSurface = nativeGetReaderSurface(newReader);
            if (newSurface == null) {
                nativeDestroyReader(newReader);
                return "{\"ok\":false,\"error\":\"new getWindow failed\"}";
            }

            Class<?> cbIface = Class.forName("android.hardware.display.IVirtualDisplayCallback");
            displayMgr.getClass().getMethod("resizeVirtualDisplay", cbIface, int.class, int.class, int.class)
                    .invoke(displayMgr, ds.callback, w, h, dpi);
            displayMgr.getClass().getMethod("setVirtualDisplaySurface", cbIface, Surface.class)
                    .invoke(displayMgr, ds.callback, newSurface);

            long oldReader = ds.readerHandle;
            Surface oldSurface = ds.surface;
            ds.readerHandle = newReader;
            ds.surface = newSurface;
            ds.width = w;
            ds.height = h;
            ds.dpi = dpi;

            if (oldSurface != null) oldSurface.release();
            if (oldReader != 0) nativeDestroyReader(oldReader);

            log("Resized VirtualDisplay slot=" + slot + " displayId=" + ds.androidDisplayId
                    + " -> " + w + "x" + h + "@" + dpi + "dpi");
            return "{\"ok\":true,\"slot\":" + slot + ",\"displayId\":" + ds.androidDisplayId
                    + ",\"width\":" + w + ",\"height\":" + h + ",\"dpi\":" + dpi + "}";
        }
    }

    private String cmdDestroyDisplay(int slot) {
        DisplaySlot ds;
        synchronized (slotLock) {
            ds = slotsByIdx.remove(slot);
            if (ds != null) {
                slotsByAndroidId.remove(ds.androidDisplayId);
            }
        }
        if (ds == null) {
            return "{\"ok\":true,\"slot\":" + slot + ",\"existed\":false}";
        }
        try {
            if (inputMgr != null && ds.inputPort != null) {
                inputMgr.getClass().getMethod("removeUniqueIdAssociation", String.class)
                        .invoke(inputMgr, ds.inputPort);
            }
        } catch (Throwable ignored) {}
        try {
            if (displayMgr != null && ds.callback != null) {
                Class<?> cbIface = Class.forName("android.hardware.display.IVirtualDisplayCallback");
                displayMgr.getClass().getMethod("releaseVirtualDisplay", cbIface)
                        .invoke(displayMgr, ds.callback);
            }
        } catch (Throwable ignored) {}
        try {
            if (ds.surface != null) ds.surface.release();
        } catch (Throwable ignored) {}
        if (ds.readerHandle != 0) {
            nativeDestroyReader(ds.readerHandle);
            ds.readerHandle = 0;
        }
        log("Destroyed VirtualDisplay slot=" + slot + " displayId=" + ds.androidDisplayId);
        return "{\"ok\":true,\"slot\":" + slot + ",\"existed\":true}";
    }

    private boolean moveTaskCleanly(final int taskId, final int targetSlot, final int targetDisplayId, String reason) throws Exception {
        Object info = findRootTaskInfo(taskId);
        if (info == null || getIntField(info, "numActivities", 0) <= 0) {
            log("moveTaskCleanly(" + reason + "): task=" + taskId + " vanished before move");
            return false;
        }
        final int srcDisplayId = getIntField(info, "displayId", -1);
        if (srcDisplayId == targetDisplayId) {
            try {
                atm.getClass().getMethod("focusTopTask", int.class).invoke(atm, targetDisplayId);
            } catch (Throwable ignored) {}
            return true;
        }

        // Wait for any in-flight Android 14 Shell Transition (DefaultTransitionHandler OPEN
        // animation on srcDisplayId) to finish before calling moveRootTaskToDisplay. Moving a
        // task mid-transition lets the old transition's finishTransaction reparent Surface(Task)
        // back onto srcDisplayId while WM bounds belong to targetDisplayId!
        Long seenAt = taskFirstSeenMs.get(Integer.valueOf(taskId));
        if (srcDisplayId > 0 && seenAt != null) {
            float animScale = getTransitionAnimationScale();
            long settleMs = Math.min(1200L, Math.max(250L, Math.round(850.0f * Math.max(0.25f, animScale))));
            long elapsed = System.currentTimeMillis() - seenAt.longValue();
            if (elapsed < settleMs) {
                long waitMs = settleMs - elapsed;
                log("Waiting " + waitMs + "ms for srcDisplayId=" + srcDisplayId
                        + " Shell Transition to settle before moving task=" + taskId);
                try { Thread.sleep(waitMs); } catch (InterruptedException ignored) {}
            }
            // Re-check task survived cold-start / transition
            info = findRootTaskInfo(taskId);
            if (info == null || getIntField(info, "numActivities", 0) <= 0) {
                log("moveTaskCleanly(" + reason + "): task=" + taskId + " exited during settle wait");
                return false;
            }
        }

        final long framesBefore = getSlotFrames(targetSlot);
        atm.getClass().getMethod("moveRootTaskToDisplay", int.class, int.class)
                .invoke(atm, taskId, targetDisplayId);
        try { Thread.sleep(60); } catch (InterruptedException ignored) {}
        if (srcDisplayId > 0 && srcDisplayId != targetDisplayId) {
            try {
                atm.getClass().getMethod("focusTopTask", int.class).invoke(atm, srcDisplayId);
            } catch (Throwable ignored) {}
            try { Thread.sleep(30); } catch (InterruptedException ignored) {}
        }
        try {
            atm.getClass().getMethod("focusTopTask", int.class).invoke(atm, targetDisplayId);
        } catch (Throwable ignored) {}

        // Self-healing watchdog: if an unusually slow Shell Transition (>900ms) STILL finished
        // after moveRootTaskToDisplay and yanked Surface(name=Task) back onto srcDisplayId,
        // targetSlot stays stuck at frames <= framesBefore + 1. Detect and reparent cleanly once.
        if (targetSlot > 0 && srcDisplayId > 0) {
            Thread heal = new Thread(new Runnable() {
                @Override
                public void run() {
                    try {
                        Thread.sleep(700);
                        DisplaySlot cur;
                        synchronized (slotLock) {
                            cur = slotsByIdx.get(targetSlot);
                        }
                        if (cur == null || cur.androidDisplayId != targetDisplayId) return;
                        Object tNow = findRootTaskInfo(taskId);
                        if (tNow == null || getIntField(tNow, "numActivities", 0) <= 0) return;
                        if (getIntField(tNow, "displayId", -1) != targetDisplayId) return;
                        long framesNow = getSlotFrames(targetSlot);
                        if (framesNow <= framesBefore + 1) {
                            log("Watchdog: slot=" + targetSlot + " displayId=" + targetDisplayId
                                    + " stuck at frames=" + framesNow + " after move of task=" + taskId
                                    + "; triggering clean reparent");
                            atm.getClass().getMethod("moveRootTaskToDisplay", int.class, int.class)
                                    .invoke(atm, taskId, 0);
                            Thread.sleep(180);
                            atm.getClass().getMethod("moveRootTaskToDisplay", int.class, int.class)
                                    .invoke(atm, taskId, targetDisplayId);
                            Thread.sleep(50);
                            try {
                                atm.getClass().getMethod("focusTopTask", int.class).invoke(atm, srcDisplayId);
                            } catch (Throwable ignored) {}
                            Thread.sleep(30);
                            try {
                                atm.getClass().getMethod("focusTopTask", int.class).invoke(atm, targetDisplayId);
                            } catch (Throwable ignored) {}
                        }
                    } catch (Throwable t) {
                        log("Watchdog reparent warning: " + t);
                    }
                }
            }, "svbox-reparent-check");
            heal.setDaemon(true);
            heal.start();
        }
        return true;
    }

    private int startActivityOnDisplay(String pkg, String comp, int displayId) throws Exception {
        Class<?> intentCls = Class.forName("android.content.Intent");
        Object intent = intentCls.getConstructor(String.class).newInstance("android.intent.action.MAIN");
        intentCls.getMethod("addCategory", String.class).invoke(intent, "android.intent.category.LAUNCHER");
        intentCls.getMethod("addFlags", int.class).invoke(intent, 0x10200000 /* NEW_TASK | RESET_TASK_IF_NEEDED */);
        if (comp != null && !comp.isEmpty()) {
            Class<?> cnCls = Class.forName("android.content.ComponentName");
            Object cn = cnCls.getMethod("unflattenFromString", String.class).invoke(null, comp);
            if (cn != null) {
                intentCls.getMethod("setComponent", cnCls).invoke(intent, cn);
            } else {
                intentCls.getMethod("setPackage", String.class).invoke(intent, pkg);
            }
        } else {
            intentCls.getMethod("setPackage", String.class).invoke(intent, pkg);
        }

        Class<?> aoCls = Class.forName("android.app.ActivityOptions");
        Object opts = aoCls.getMethod("makeBasic").invoke(null);
        aoCls.getMethod("setLaunchDisplayId", int.class).invoke(opts, displayId);
        Object bundle = aoCls.getMethod("toBundle").invoke(opts);

        Class<?> appThreadCls = Class.forName("android.app.IApplicationThread");
        Class<?> ibinderCls = Class.forName("android.os.IBinder");
        Class<?> profCls = Class.forName("android.app.ProfilerInfo");
        Class<?> bundleCls = Class.forName("android.os.Bundle");
        Method startAsUser = atm.getClass().getMethod("startActivityAsUser",
                appThreadCls, String.class, String.class, intentCls, String.class,
                ibinderCls, String.class, int.class, int.class, profCls, bundleCls, int.class);
        return ((Integer) startAsUser.invoke(atm,
                null, "com.android.shell", null, intent, null, null, null, 0, 0, null, bundle, 0)).intValue();
    }

    private String cmdLaunchApp(int slot, String pkg, String comp) throws Exception {
        int displayId = 0;
        if (slot > 0) {
            synchronized (slotLock) {
                DisplaySlot ds = slotsByIdx.get(slot);
                if (ds == null) return "{\"ok\":false,\"error\":\"slot not active\"}";
                ds.boundPkg = pkg;
                displayId = ds.androidDisplayId;
            }
        }

        // If the app ALREADY has a live task (e.g. on display 0 or another display), move it
        // cleanly onto our target VirtualDisplay instead of letting ActivityStarter bring the
        // old task to front on display 0 and leave this slot black!
        if (slot > 0) {
            Object existing = findLiveTaskForPackage(pkg);
            if (existing != null) {
                int existingTid = getIntField(existing, "taskId", -1);
                int existingDid = getIntField(existing, "displayId", -1);
                if (existingTid > 0) {
                    if (existingDid == displayId) {
                        try {
                            atm.getClass().getMethod("focusTopTask", int.class).invoke(atm, displayId);
                        } catch (Throwable ignored) {}
                        log("Reused existing task=" + existingTid + " for " + pkg + " already on slot=" + slot + " displayId=" + displayId);
                        return "{\"ok\":true,\"rc\":0,\"reused\":true,\"taskId\":" + existingTid + ",\"displayId\":" + displayId + "}";
                    } else if (existingDid >= 0) {
                        boolean moved = moveTaskCleanly(existingTid, slot, displayId, "launch-reuse:" + pkg);
                        if (moved) {
                            log("Moved existing task=" + existingTid + " (" + pkg + ") from displayId=" + existingDid
                                    + " to slot=" + slot + " displayId=" + displayId);
                            return "{\"ok\":true,\"rc\":0,\"moved\":true,\"taskId\":" + existingTid + ",\"displayId\":" + displayId + "}";
                        }
                    }
                }
            }
        }

        // Resolve launcher component if not supplied
        boolean callerProvidedComp = (comp != null && !comp.isEmpty());
        if (!callerProvidedComp) {
            comp = resolveLauncherComponent(pkg);
        }

        int rc = startActivityOnDisplay(pkg, comp, displayId);
        // Retry once with freshly resolved launcher component if cached component became stale (e.g. rc=-92)
        if (rc < 0 && callerProvidedComp) {
            String freshComp = resolveLauncherComponent(pkg);
            if (freshComp != null && !freshComp.isEmpty() && !freshComp.equals(comp)) {
                log("Retrying launch of " + pkg + " with fresh component " + freshComp + " (stale=" + comp + " rc=" + rc + ")");
                comp = freshComp;
                rc = startActivityOnDisplay(pkg, comp, displayId);
            }
        }

        // Post-launch verification for secondary displays: if Android routed a singleTask/reused
        // task onto display 0 (or another display) instead of displayId, pull it onto displayId!
        if (rc >= 0 && slot > 0) {
            for (int attempt = 0; attempt < 6; attempt++) {
                Object live = findLiveTaskForPackage(pkg);
                if (live != null) {
                    int liveTid = getIntField(live, "taskId", -1);
                    int liveDid = getIntField(live, "displayId", -1);
                    if (liveTid > 0) {
                        if (liveDid == displayId) {
                            break;
                        }
                        if (liveDid >= 0 && liveDid != displayId) {
                            log("Post-launch redirect: " + pkg + " task=" + liveTid
                                    + " landed on displayId=" + liveDid + " instead of " + displayId + "; moving");
                            moveTaskCleanly(liveTid, slot, displayId, "post-launch-fix:" + pkg);
                            break;
                        }
                    }
                }
                try { Thread.sleep(100); } catch (InterruptedException ignored) {}
            }
        }

        try {
            atm.getClass().getMethod("focusTopTask", int.class).invoke(atm, displayId);
        } catch (Throwable ignored) {}

        log("Launched " + pkg + " (" + comp + ") on slot=" + slot + " displayId=" + displayId + " rc=" + rc);
        return "{\"ok\":" + (rc >= 0 ? "true" : "false")
                + ",\"rc\":" + rc + ",\"displayId\":" + displayId
                + ",\"component\":" + jsonStr(comp != null ? comp : "") + "}";
    }

    private String cmdMoveTask(int taskId, int targetSlot, String newPkg) throws Exception {
        int targetDisplayId = 0;
        if (targetSlot > 0) {
            synchronized (slotLock) {
                DisplaySlot ds = slotsByIdx.get(targetSlot);
                if (ds == null) return "{\"ok\":false,\"error\":\"target slot not active\"}";
                if (newPkg != null && !newPkg.isEmpty()) {
                    ds.boundPkg = newPkg;
                }
                targetDisplayId = ds.androidDisplayId;
            }
        }
        boolean ok = moveTaskCleanly(taskId, targetSlot, targetDisplayId, "MOVE_TASK");
        if (!ok) {
            return "{\"ok\":false,\"taskId\":" + taskId + ",\"error\":\"task no longer exists\"}";
        }
        log("Moved task=" + taskId + " to slot=" + targetSlot + " displayId=" + targetDisplayId);
        return "{\"ok\":true,\"taskId\":" + taskId + ",\"displayId\":" + targetDisplayId + "}";
    }

    private String cmdPopOutEmulator(int targetSlot) throws Exception {
        int targetDisplayId;
        DisplaySlot ds;
        synchronized (slotLock) {
            ds = slotsByIdx.get(targetSlot);
            if (ds == null) return "{\"ok\":false,\"error\":\"target slot not active\"}";
            targetDisplayId = ds.androidDisplayId;
        }
        @SuppressWarnings("unchecked")
        List<Object> tasks = (List<Object>) atm.getClass()
                .getMethod("getAllRootTaskInfosOnDisplay", int.class).invoke(atm, 0);
        if (tasks != null) {
            for (Object t : tasks) {
                int tid = getIntField(t, "taskId", -1);
                Object top = getFieldObj(t, "topActivity");
                if (tid <= 0 || top == null) continue;
                String pkg = (String) top.getClass().getMethod("getPackageName").invoke(top);
                String comp = (String) top.getClass().getMethod("flattenToShortString").invoke(top);
                if (pkg == null || EXEMPT_CROSS_APP_PKGS.contains(pkg)) continue;
                ds.boundPkg = pkg;
                atm.getClass().getMethod("moveRootTaskToDisplay", int.class, int.class)
                        .invoke(atm, tid, targetDisplayId);
                try {
                    atm.getClass().getMethod("focusTopTask", int.class).invoke(atm, targetDisplayId);
                } catch (Throwable ignored) {}
                return "{\"ok\":true,\"taskId\":" + tid
                        + ",\"packageName\":" + jsonStr(pkg)
                        + ",\"component\":" + jsonStr(comp)
                        + ",\"label\":" + jsonStr(resolveAppLabel(pkg))
                        + ",\"displayId\":" + targetDisplayId + "}";
            }
        }
        return "{\"ok\":false,\"error\":\"no movable app task on display 0\"}";
    }

    private String cmdPullBackToEmulator(int fromSlot) throws Exception {
        DisplaySlot ds;
        synchronized (slotLock) {
            ds = slotsByIdx.get(fromSlot);
        }
        if (ds == null) return "{\"ok\":false,\"error\":\"slot not active\"}";
        @SuppressWarnings("unchecked")
        List<Object> tasks = (List<Object>) atm.getClass()
                .getMethod("getAllRootTaskInfosOnDisplay", int.class).invoke(atm, ds.androidDisplayId);
        int moved = 0;
        if (tasks != null) {
            for (Object t : tasks) {
                int tid = getIntField(t, "taskId", -1);
                if (tid > 0) {
                    atm.getClass().getMethod("moveRootTaskToDisplay", int.class, int.class)
                            .invoke(atm, tid, 0);
                    moved++;
                }
            }
        }
        try {
            atm.getClass().getMethod("focusTopTask", int.class).invoke(atm, 0);
        } catch (Throwable ignored) {}
        return "{\"ok\":true,\"moved\":" + moved + "}";
    }

    private String cmdFocusDisplay(int slot) throws Exception {
        int displayId = 0;
        if (slot > 0) {
            synchronized (slotLock) {
                DisplaySlot ds = slotsByIdx.get(slot);
                if (ds == null) return "{\"ok\":false,\"error\":\"slot not active\"}";
                displayId = ds.androidDisplayId;
            }
        }
        atm.getClass().getMethod("focusTopTask", int.class).invoke(atm, displayId);
        return "{\"ok\":true,\"displayId\":" + displayId + "}";
    }

    private String cmdSendKey(int slot, int keyCode, int metaState) throws Exception {
        int displayId = resolveDisplayIdForSlot(slot);
        if (displayId < 0) return "{\"ok\":false,\"error\":\"slot not active\"}";
        injectSingleKey(displayId, keyCode, metaState);
        return "{\"ok\":true,\"displayId\":" + displayId + ",\"keyCode\":" + keyCode + "}";
    }

    private int resolveDisplayIdForSlot(int slot) {
        if (slot <= 0) return 0;
        synchronized (slotLock) {
            DisplaySlot ds = slotsByIdx.get(slot);
            return ds != null ? ds.androidDisplayId : -1;
        }
    }

    private void injectSingleKey(int displayId, int keyCode, int metaState) throws Exception {
        long now = ((Long) Class.forName("android.os.SystemClock")
                .getMethod("uptimeMillis").invoke(null)).longValue();
        Class<?> keCls = Class.forName("android.view.KeyEvent");
        Class<?> ieCls = Class.forName("android.view.InputEvent");
        Constructor<?> keCtor = keCls.getConstructor(
                long.class, long.class, int.class, int.class, int.class,
                int.class, int.class, int.class, int.class, int.class);
        Method setDisp = ieCls.getMethod("setDisplayId", int.class);
        Method inject = inputMgr.getClass().getMethod("injectInputEvent", ieCls, int.class);

        Object down = keCtor.newInstance(now, now, 0 /* ACTION_DOWN */, keyCode, 0, metaState, -1, 0, 0, 0x101 /* KEYBOARD */);
        setDisp.invoke(down, displayId);
        inject.invoke(inputMgr, down, 0 /* ASYNC */);

        Object up = keCtor.newInstance(now, now + 15, 1 /* ACTION_UP */, keyCode, 0, metaState, -1, 0, 0, 0x101);
        setDisp.invoke(up, displayId);
        inject.invoke(inputMgr, up, 0 /* ASYNC */);
    }

    private boolean setAndroidClipboardText(String text) {
        if (clipboardMgr == null || text == null) return false;
        try {
            Class<?> cdCls = Class.forName("android.content.ClipData");
            Object clip = cdCls.getMethod("newPlainText", CharSequence.class, CharSequence.class)
                    .invoke(null, "svbox", text);
            Method[] methods = clipboardMgr.getClass().getMethods();
            for (Method m : methods) {
                if (!"setPrimaryClip".equals(m.getName())) continue;
                Class<?>[] pt = m.getParameterTypes();
                if (pt.length == 5) {
                    m.invoke(clipboardMgr, clip, "com.android.shell", null, 0, 0);
                    return true;
                } else if (pt.length == 4) {
                    m.invoke(clipboardMgr, clip, "com.android.shell", null, 0);
                    return true;
                } else if (pt.length == 3) {
                    m.invoke(clipboardMgr, clip, "com.android.shell", 0);
                    return true;
                }
            }
        } catch (Throwable t) {
            log("setAndroidClipboardText failed: " + t);
        }
        return false;
    }

    private String getAndroidClipboardText() {
        if (clipboardMgr == null) return null;
        try {
            Object clip = null;
            Method[] methods = clipboardMgr.getClass().getMethods();
            for (Method m : methods) {
                if (!"getPrimaryClip".equals(m.getName())) continue;
                Class<?>[] pt = m.getParameterTypes();
                if (pt.length == 4) {
                    clip = m.invoke(clipboardMgr, "com.android.shell", null, 0, 0);
                    break;
                } else if (pt.length == 3) {
                    clip = m.invoke(clipboardMgr, "com.android.shell", null, 0);
                    break;
                } else if (pt.length == 2) {
                    clip = m.invoke(clipboardMgr, "com.android.shell", 0);
                    break;
                }
            }
            if (clip == null) return "";
            int cnt = ((Integer) clip.getClass().getMethod("getItemCount").invoke(clip)).intValue();
            if (cnt <= 0) return "";
            Object item = clip.getClass().getMethod("getItemAt", int.class).invoke(clip, 0);
            if (item == null) return "";
            Object txt = item.getClass().getMethod("getText").invoke(item);
            return txt != null ? txt.toString() : "";
        } catch (Throwable ignored) {
            return null;
        }
    }

    private String cmdSetClipboard(String b64) {
        try {
            String text = new String(Base64.getDecoder().decode(b64), StandardCharsets.UTF_8);
            synchronized (clipLock) {
                lastClipText = text;
                boolean ok = setAndroidClipboardText(text);
                return "{\"ok\":" + (ok ? "true" : "false") + ",\"len\":" + text.length() + "}";
            }
        } catch (Throwable t) {
            return "{\"ok\":false,\"error\":" + jsonStr(String.valueOf(t)) + "}";
        }
    }

    private String cmdGetClipboard() {
        String text = getAndroidClipboardText();
        if (text == null) text = "";
        String b64 = Base64.getEncoder().encodeToString(text.getBytes(StandardCharsets.UTF_8));
        return "{\"ok\":true,\"b64\":" + jsonStr(b64) + ",\"len\":" + text.length() + "}";
    }

    private String cmdInputText(int slot, String b64) throws Exception {
        int displayId = resolveDisplayIdForSlot(slot);
        if (displayId < 0) return "{\"ok\":false,\"error\":\"slot not active\"}";
        String text = new String(Base64.getDecoder().decode(b64), StandardCharsets.UTF_8);
        if (text.isEmpty()) return "{\"ok\":true,\"len\":0}";

        if (sendToIme("COMMIT " + b64)) {
            return "{\"ok\":true,\"mode\":\"host_ime\",\"len\":" + text.length() + "}";
        }

        boolean pureAscii = text.length() <= 64;
        if (pureAscii) {
            for (int i = 0; i < text.length(); i++) {
                char c = text.charAt(i);
                if (c < 0x20 || c > 0x7E) {
                    pureAscii = false;
                    break;
                }
            }
        }
        if (pureAscii) {
            try {
                Class<?> kcmCls = Class.forName("android.view.KeyCharacterMap");
                Object kcm = kcmCls.getMethod("load", int.class).invoke(null, -1 /* VIRTUAL_KEYBOARD */);
                Object[] events = (Object[]) kcmCls.getMethod("getEvents", char[].class).invoke(kcm, (Object) text.toCharArray());
                if (events != null && events.length > 0) {
                    Class<?> ieCls = Class.forName("android.view.InputEvent");
                    Method setDisp = ieCls.getMethod("setDisplayId", int.class);
                    Method inject = inputMgr.getClass().getMethod("injectInputEvent", ieCls, int.class);
                    for (Object ev : events) {
                        setDisp.invoke(ev, displayId);
                        inject.invoke(inputMgr, ev, 0 /* ASYNC */);
                    }
                    return "{\"ok\":true,\"mode\":\"kcm\",\"len\":" + text.length() + "}";
                }
            } catch (Throwable t) {
                log("KeyCharacterMap inject fallback to clipboard paste: " + t);
            }
        }

        // Unicode / Chinese / Emoji path: write clipboard and inject KEYCODE_PASTE (279)
        synchronized (clipLock) {
            lastClipText = text;
            setAndroidClipboardText(text);
        }
        try { Thread.sleep(20); } catch (InterruptedException ignored) {}
        injectSingleKey(displayId, 279 /* KEYCODE_PASTE */, 0);
        return "{\"ok\":true,\"mode\":\"paste\",\"len\":" + text.length() + "}";
    }

    private int mapAndroidDisplayToSlot(int androidDisplayId) {
        if (androidDisplayId <= 0) return 0;
        synchronized (slotLock) {
            DisplaySlot ds = slotsByAndroidId.get(androidDisplayId);
            if (ds != null) return ds.slot;
        }
        return 0;
    }

    private void serveImeLoop() {
        int listenFd = nativeListenUnix("@svbox_ime");
        if (listenFd < 0) {
            log("Failed to listen on @svbox_ime");
            return;
        }
        log("Listening on @svbox_ime for SvboxHostImeService");
        while (true) {
            final int cfd = nativeAcceptUnix(listenFd);
            if (cfd < 0) continue;
            Thread t = new Thread(new Runnable() {
                @Override
                public void run() {
                    handleImeClient(cfd);
                }
            }, "svbox-ime-cli");
            t.setDaemon(true);
            t.start();
        }
    }

    private void ensureHostImeActiveLoop() {
        // Android 14 InputMethodManagerService.onUnlockUser(0) reloads persisted settings_secure.xml
        // right after user unlock, which can temporarily revert default_input_method back to LatinIME.
        // Re-assert org.svbox.ime/.SvboxHostImeService once user 0 is unlocked so it stays connected.
        int[] delaysMs = new int[] { 2000, 3000, 5000, 8000 };
        for (int delay : delaysMs) {
            try { Thread.sleep(delay); } catch (InterruptedException ignored) { return; }
            boolean booted = false;
            try {
                Class<?> spCls = Class.forName("android.os.SystemProperties");
                String v = (String) spCls.getMethod("get", String.class, String.class)
                        .invoke(null, "sys.boot_completed", "0");
                booted = "1".equals(v);
            } catch (Throwable ignored) {}
            if (imeFd >= 0 && booted) {
                return;
            }
            try {
                Process p = new ProcessBuilder("/system/bin/sh", "-c",
                        "ime enable org.svbox.ime/.SvboxHostImeService >/dev/null 2>&1; "
                                + "ime set org.svbox.ime/.SvboxHostImeService >/dev/null 2>&1")
                        .redirectErrorStream(true).start();
                p.waitFor();
            } catch (Throwable t) {
                log("ensureHostImeActiveLoop warn: " + t);
            }
        }
    }

    private void handleImeClient(int fd) {
        synchronized (imeLock) {
            if (imeFd >= 0 && imeFd != fd) {
                nativeCloseFd(imeFd);
            }
            imeFd = fd;
        }
        log("SvboxHostIme connected (fd=" + fd + ")");
        try {
            while (true) {
                String line = nativeReadLine(fd);
                if (line == null) break;
                line = line.trim();
                if (line.isEmpty() || "IME_REGISTER".equals(line)) continue;
                String[] p = line.split("\\s+");
                if ("IME_FOCUS".equals(p[0]) && p.length >= 6) {
                    boolean active = "1".equals(p[1]);
                    int did = parseIntSafe(p[2], 0);
                    int inputType = parseIntSafe(p[3], 0);
                    int imeOptions = parseIntSafe(p[4], 0);
                    int showView = parseIntSafe(p[5], 0);
                    String pkg = p.length >= 7 ? p[6] : "";
                    if ("-".equals(pkg)) pkg = "";
                    int slot = mapAndroidDisplayToSlot(did);
                    imeFocused = active;
                    imeFocusSlot = slot;
                    String ev = "{\"event\":\"IME_FOCUS\""
                            + ",\"active\":" + (active ? "true" : "false")
                            + ",\"slot\":" + slot
                            + ",\"displayId\":" + did
                            + ",\"inputType\":" + inputType
                            + ",\"imeOptions\":" + imeOptions
                            + ",\"showView\":" + showView
                            + ",\"packageName\":" + jsonStr(pkg)
                            + "}";
                    broadcastEvent(ev);
                } else if ("IME_CURSOR".equals(p[0]) && p.length >= 5) {
                    int did = parseIntSafe(p[1], 0);
                    int x = parseIntSafe(p[2], 0);
                    int y = parseIntSafe(p[3], 0);
                    int h = parseIntSafe(p[4], 24);
                    int slot = mapAndroidDisplayToSlot(did);
                    String ev = "{\"event\":\"IME_CURSOR\""
                            + ",\"slot\":" + slot
                            + ",\"x\":" + x
                            + ",\"y\":" + y
                            + ",\"h\":" + h
                            + "}";
                    broadcastEvent(ev);
                }
            }
        } catch (Throwable t) {
            log("handleImeClient error: " + t);
        } finally {
            boolean wasActive = false;
            int prevSlot = 0;
            synchronized (imeLock) {
                if (imeFd == fd) {
                    imeFd = -1;
                    wasActive = imeFocused;
                    prevSlot = imeFocusSlot;
                    imeFocused = false;
                }
            }
            nativeCloseFd(fd);
            log("SvboxHostIme disconnected (fd=" + fd + ")");
            if (wasActive) {
                broadcastEvent("{\"event\":\"IME_FOCUS\",\"active\":false,\"slot\":" + prevSlot
                        + ",\"displayId\":0,\"inputType\":0,\"imeOptions\":0,\"showView\":0,\"packageName\":\"\"}");
            }
        }
    }

    private boolean sendToIme(String line) {
        synchronized (imeLock) {
            if (imeFd < 0) return false;
            if (!nativeWriteLine(imeFd, line)) {
                nativeCloseFd(imeFd);
                imeFd = -1;
                imeFocused = false;
                return false;
            }
            return true;
        }
    }

    private String cmdImeSend(String msg) {
        if (sendToIme(msg)) {
            return "{\"ok\":true}";
        }
        return "{\"ok\":false,\"error\":\"ime not connected\"}";
    }

    private String cmdImeCommit(String b64, int fallbackSlot) throws Exception {
        if (imeFocused && sendToIme("COMMIT " + b64)) {
            return "{\"ok\":true,\"via\":\"ime\"}";
        }
        // Fallback to direct INPUT_TEXT if SvboxHostIme has no active InputConnection
        // (e.g. user manually attached host IME on a custom game canvas).
        return cmdInputText(fallbackSlot >= 0 ? fallbackSlot : imeFocusSlot, b64);
    }

    private String cmdImeDelete(int left, int right) throws Exception {
        if (sendToIme("DELETE " + left + " " + right)) {
            return "{\"ok\":true,\"via\":\"ime\"}";
        }
        int did = resolveDisplayIdForSlot(imeFocusSlot);
        if (did >= 0) {
            for (int i = 0; i < Math.min(left, 32); i++) injectSingleKey(did, 67 /* KEYCODE_DEL */, 0);
            for (int i = 0; i < Math.min(right, 32); i++) injectSingleKey(did, 112 /* KEYCODE_FORWARD_DEL */, 0);
            return "{\"ok\":true,\"via\":\"key\"}";
        }
        return "{\"ok\":false,\"error\":\"ime not connected\"}";
    }

    private String cmdImeAction(int enterType) throws Exception {
        if (sendToIme("ACTION " + enterType)) {
            return "{\"ok\":true,\"via\":\"ime\"}";
        }
        int did = resolveDisplayIdForSlot(imeFocusSlot);
        if (did >= 0) {
            injectSingleKey(did, 66 /* KEYCODE_ENTER */, 0);
            return "{\"ok\":true,\"via\":\"key\"}";
        }
        return "{\"ok\":false,\"error\":\"ime not connected\"}";
    }

    private String cmdSetDisplayRotation(int rot) {
        try {
            if (windowMgr != null) {
                if (rot < 0) {
                    try {
                        windowMgr.getClass().getMethod("setFixedToUserRotation", int.class, int.class)
                                .invoke(windowMgr, 0, 0 /* DEFAULT */);
                    } catch (Throwable ignored) {}
                    try {
                        windowMgr.getClass().getMethod("thawDisplayRotation", int.class)
                                .invoke(windowMgr, 0);
                    } catch (Throwable ignored) {}
                    return "{\"ok\":true,\"rotation\":0,\"locked\":false}";
                }
                int r = rot & 3;
                try {
                    windowMgr.getClass().getMethod("setFixedToUserRotation", int.class, int.class)
                            .invoke(windowMgr, 0, 2 /* ENABLED */);
                } catch (Throwable t) {
                    log("setFixedToUserRotation warning: " + t);
                }
                boolean frozen = false;
                try {
                    windowMgr.getClass().getMethod("freezeDisplayRotation", int.class, int.class)
                            .invoke(windowMgr, 0, r);
                    frozen = true;
                } catch (Throwable ignored) {}
                if (!frozen) {
                    try {
                        windowMgr.getClass().getMethod("freezeRotation", int.class)
                                .invoke(windowMgr, r);
                        frozen = true;
                    } catch (Throwable ignored) {}
                }
                if (frozen) {
                    return "{\"ok\":true,\"rotation\":" + r + ",\"locked\":true}";
                }
            }
            // Fallback via wm shell command
            int r = rot < 0 ? 0 : (rot & 3);
            String cmd = rot < 0
                    ? "wm fixed-to-user-rotation default; wm user-rotation free"
                    : ("wm fixed-to-user-rotation enabled; wm user-rotation lock " + r);
            Process p = new ProcessBuilder("/system/bin/sh", "-c", cmd).redirectErrorStream(true).start();
            p.waitFor();
            return "{\"ok\":true,\"rotation\":" + r + ",\"via\":\"shell\"}";
        } catch (Throwable t) {
            return "{\"ok\":false,\"error\":" + jsonStr(String.valueOf(t)) + "}";
        }
    }

    private String cmdGetDisplayRotation() {
        int rot = 0;
        try {
            if (windowMgr != null) {
                Object val = windowMgr.getClass().getMethod("getDefaultDisplayRotation").invoke(windowMgr);
                if (val instanceof Integer) rot = ((Integer) val).intValue();
            }
        } catch (Throwable ignored) {}
        return "{\"ok\":true,\"rotation\":" + rot + "}";
    }

    private static int parseIntSafe(String s, int def) {
        try {
            return Integer.parseInt(s);
        } catch (Throwable ignored) {
            return def;
        }
    }

    private void clipboardWatchLoop() {
        while (true) {
            try {
                Thread.sleep(450);
                if (eventFds.isEmpty() || clipboardMgr == null) continue;
                String cur = getAndroidClipboardText();
                if (cur == null || cur.isEmpty()) continue;
                boolean changed = false;
                synchronized (clipLock) {
                    if (!cur.equals(lastClipText)) {
                        lastClipText = cur;
                        changed = true;
                    }
                }
                if (changed && cur.length() <= 65536) {
                    String b64 = Base64.getEncoder().encodeToString(cur.getBytes(StandardCharsets.UTF_8));
                    String ev = "{\"event\":\"CLIPBOARD_CHANGED\",\"b64\":" + jsonStr(b64) + "}";
                    broadcastEvent(ev);
                }
            } catch (Throwable ignored) {
            }
        }
    }

    private String cmdCloseApp(int slot, String pkg) throws Exception {
        int displayId = 0;
        if (slot > 0) {
            synchronized (slotLock) {
                DisplaySlot ds = slotsByIdx.get(slot);
                if (ds != null) {
                    displayId = ds.androidDisplayId;
                    if (pkg.isEmpty() && ds.boundPkg != null) pkg = ds.boundPkg;
                }
            }
        }
        int removed = 0;
        if (displayId > 0) {
            @SuppressWarnings("unchecked")
            List<Object> tasks = (List<Object>) atm.getClass()
                    .getMethod("getAllRootTaskInfosOnDisplay", int.class).invoke(atm, displayId);
            if (tasks != null) {
                for (Object t : tasks) {
                    int tid = getIntField(t, "taskId", -1);
                    if (tid > 0) {
                        atm.getClass().getMethod("removeTask", int.class).invoke(atm, tid);
                        removed++;
                    }
                }
            }
        }
        return "{\"ok\":true,\"removed\":" + removed + ",\"packageName\":" + jsonStr(pkg) + "}";
    }

    private String cmdGetTasks(int slot) throws Exception {
        int displayId = -1;
        if (slot == 0) {
            displayId = 0;
        } else if (slot > 0) {
            synchronized (slotLock) {
                DisplaySlot ds = slotsByIdx.get(slot);
                if (ds == null) return "{\"ok\":false,\"error\":\"slot not active\"}";
                displayId = ds.androidDisplayId;
            }
        }
        @SuppressWarnings("unchecked")
        List<Object> tasks = (displayId >= 0)
                ? (List<Object>) atm.getClass().getMethod("getAllRootTaskInfosOnDisplay", int.class).invoke(atm, displayId)
                : (List<Object>) atm.getClass().getMethod("getAllRootTaskInfos").invoke(atm);

        StringBuilder sb = new StringBuilder();
        sb.append("{\"ok\":true,\"tasks\":[");
        boolean first = true;
        if (tasks != null) {
            for (Object t : tasks) {
                int tid = getIntField(t, "taskId", -1);
                int did = getIntField(t, "displayId", -1);
                Object top = getFieldObj(t, "topActivity");
                String pkg = "";
                String comp = "";
                if (top != null) {
                    pkg = (String) top.getClass().getMethod("getPackageName").invoke(top);
                    comp = (String) top.getClass().getMethod("flattenToShortString").invoke(top);
                }
                if (!first) sb.append(',');
                first = false;
                sb.append("{\"taskId\":").append(tid)
                        .append(",\"displayId\":").append(did)
                        .append(",\"packageName\":").append(jsonStr(pkg))
                        .append(",\"component\":").append(jsonStr(comp))
                        .append("}");
            }
        }
        sb.append("]}");
        return sb.toString();
    }

    private String cmdListApps(boolean forceIcons) throws Exception {
        if (systemContext == null) {
            return "{\"ok\":false,\"error\":\"systemContext unavailable\"}";
        }
        Object pm = systemContext.getClass().getMethod("getPackageManager").invoke(systemContext);
        Class<?> intentCls = Class.forName("android.content.Intent");
        Object queryIntent = intentCls.getConstructor(String.class).newInstance("android.intent.action.MAIN");
        intentCls.getMethod("addCategory", String.class).invoke(queryIntent, "android.intent.category.LAUNCHER");

        @SuppressWarnings("unchecked")
        List<Object> infos = (List<Object>) pm.getClass()
                .getMethod("queryIntentActivities", intentCls, int.class)
                .invoke(pm, queryIntent, 0);

        File iconDirFile = new File(iconsDir);
        if (!iconDirFile.exists()) iconDirFile.mkdirs();

        // Disk-backed pkg -> (lastUpdateTime, label) cache avoids opening APKs via
        // PackageManager.loadLabel/loadIcon (which permanently mmaps every APK in ResourcesManager).
        Map<String, String> labelCache = new HashMap<String, String>();
        Map<String, Long> mtimeCache = new HashMap<String, Long>();
        File labelCacheFile = new File(iconDirFile, "labels.tsv");
        if (labelCacheFile.exists()) {
            BufferedReader br = null;
            try {
                br = new BufferedReader(new InputStreamReader(new FileInputStream(labelCacheFile), StandardCharsets.UTF_8));
                String ln;
                while ((ln = br.readLine()) != null) {
                    int t1 = ln.indexOf('\t');
                    int t2 = t1 > 0 ? ln.indexOf('\t', t1 + 1) : -1;
                    if (t1 > 0 && t2 > t1) {
                        String p = ln.substring(0, t1);
                        try {
                            long mt = Long.parseLong(ln.substring(t1 + 1, t2));
                            String lb = ln.substring(t2 + 1);
                            mtimeCache.put(p, Long.valueOf(mt));
                            labelCache.put(p, lb);
                        } catch (NumberFormatException ignored) {}
                    }
                }
            } catch (Throwable ignored) {
            } finally {
                if (br != null) {
                    try { br.close(); } catch (Throwable ignored) {}
                }
            }
        }
        boolean cacheDirty = false;
        boolean touchedApk = false;
        Method getPkgInfo = null;
        try {
            getPkgInfo = pm.getClass().getMethod("getPackageInfo", String.class, int.class);
        } catch (Throwable ignored) {}

        Set<String> seenPkgs = new HashSet<String>();
        StringBuilder sb = new StringBuilder();
        sb.append("{\"ok\":true,\"apps\":[");
        boolean first = true;

        if (infos != null) {
            for (Object ri : infos) {
                Object ai = getFieldObj(ri, "activityInfo");
                if (ai == null) continue;
                String pkg = (String) getFieldObj(ai, "packageName");
                String clsName = (String) getFieldObj(ai, "name");
                if (pkg == null || pkg.isEmpty() || !seenPkgs.add(pkg)) continue;
                if ("com.android.launcher3".equals(pkg) || "com.android.quickstep".equals(pkg)) continue;

                long lastUpdate = 0L;
                if (getPkgInfo != null) {
                    try {
                        Object pi = getPkgInfo.invoke(pm, pkg, 0);
                        if (pi != null) {
                            Field f = findField(pi.getClass(), "lastUpdateTime");
                            if (f != null) lastUpdate = f.getLong(pi);
                        }
                    } catch (Throwable ignored) {}
                }

                Long cachedMt = mtimeCache.get(pkg);
                String cachedLb = labelCache.get(pkg);
                boolean metaFresh = !forceIcons && cachedMt != null && cachedMt.longValue() == lastUpdate
                        && cachedLb != null && cachedLb.length() > 0;

                String label = metaFresh ? cachedLb : pkg;
                if (!metaFresh) {
                    try {
                        Object cs = ri.getClass().getMethod("loadLabel",
                                Class.forName("android.content.pm.PackageManager")).invoke(ri, pm);
                        if (cs != null && cs.toString().length() > 0) label = cs.toString();
                        touchedApk = true;
                    } catch (Throwable ignored) {}
                    String cleanLabel = label.replace('\t', ' ').replace('\n', ' ').replace('\r', ' ');
                    labelCache.put(pkg, cleanLabel);
                    mtimeCache.put(pkg, Long.valueOf(lastUpdate));
                    cacheDirty = true;
                }

                Object appInfo = getFieldObj(ai, "applicationInfo");
                int appFlags = appInfo != null ? getIntField(appInfo, "flags", 0) : 0;
                int category = appInfo != null ? getIntField(appInfo, "category", -1) : -1;
                boolean isSystem = (appFlags & 1 /* FLAG_SYSTEM */) != 0;
                boolean isGame = (category == 0 /* CATEGORY_GAME */) || ((appFlags & 0x02000000 /* FLAG_IS_GAME */) != 0);
                int resizeMode = getIntField(ai, "resizeMode", 2);
                int screenOri = getIntField(ai, "screenOrientation", -1);

                // Determine default window policy:
                // "adaptive" | "fixed_landscape" | "fixed_portrait"
                boolean isLandscapeLocked = (screenOri == 0 || screenOri == 6 || screenOri == 8 || screenOri == 11);
                boolean isPortraitLocked = (screenOri == 1 || screenOri == 7 || screenOri == 9 || screenOri == 12);
                boolean resizeable = (resizeMode != 0) && !isGame && !isLandscapeLocked && !isPortraitLocked;
                String defaultMode = resizeable ? "adaptive"
                        : (isLandscapeLocked || isGame ? "fixed_landscape" : "fixed_portrait");

                String iconFileName = pkg + ".png";
                File iconOut = new File(iconDirFile, iconFileName);
                boolean hasIcon = iconOut.exists() && iconOut.length() > 0 && iconOut.length() <= 100 * 1024;
                if (!hasIcon || forceIcons || !metaFresh) {
                    hasIcon = exportAppIconPng(ri, pm, iconOut);
                    touchedApk = true;
                }

                if (!first) sb.append(',');
                first = false;
                sb.append("{")
                        .append("\"packageName\":").append(jsonStr(pkg))
                        .append(",\"component\":").append(jsonStr(pkg + "/" + clsName))
                        .append(",\"label\":").append(jsonStr(label))
                        .append(",\"iconFile\":").append(jsonStr(hasIcon ? iconFileName : ""))
                        .append(",\"resizeable\":").append(resizeable ? "true" : "false")
                        .append(",\"isGame\":").append(isGame ? "true" : "false")
                        .append(",\"isSystem\":").append(isSystem ? "true" : "false")
                        .append(",\"orientation\":").append(screenOri)
                        .append(",\"defaultMode\":").append(jsonStr(defaultMode))
                        .append("}");
            }
        }
        if (cacheDirty) {
            File tmp = new File(iconDirFile, "labels.tsv.tmp");
            FileOutputStream fos = null;
            try {
                fos = new FileOutputStream(tmp);
                StringBuilder cb = new StringBuilder();
                for (Map.Entry<String, String> e : labelCache.entrySet()) {
                    Long mt = mtimeCache.get(e.getKey());
                    cb.append(e.getKey()).append('\t')
                            .append(mt != null ? mt.longValue() : 0L).append('\t')
                            .append(e.getValue()).append('\n');
                }
                fos.write(cb.toString().getBytes(StandardCharsets.UTF_8));
                fos.close();
                fos = null;
                tmp.renameTo(labelCacheFile);
            } catch (Throwable ignored) {
                tmp.delete();
            } finally {
                if (fos != null) {
                    try { fos.close(); } catch (Throwable ignored) {}
                }
            }
        }
        if (touchedApk) {
            System.gc();
        }
        sb.append("]}");
        return sb.toString();
    }

    private String cmdTrimMemory() {
        Runtime rt = Runtime.getRuntime();
        long beforeKb = (rt.totalMemory() - rt.freeMemory()) / 1024L;
        System.gc();
        System.runFinalization();
        long afterKb = (rt.totalMemory() - rt.freeMemory()) / 1024L;
        return "{\"ok\":true,\"beforeKb\":" + beforeKb + ",\"afterKb\":" + afterKb + "}";
    }

    private boolean exportAppIconPng(Object resolveInfo, Object pm, File outFile) {
        try {
            Class<?> pmCls = Class.forName("android.content.pm.PackageManager");
            Object drawable = resolveInfo.getClass().getMethod("loadIcon", pmCls).invoke(resolveInfo, pm);
            if (drawable == null) return false;

            Class<?> bmpCls = Class.forName("android.graphics.Bitmap");
            Class<?> cfgCls = Class.forName("android.graphics.Bitmap$Config");
            Object argb8888 = cfgCls.getField("ARGB_8888").get(null);
            Class<?> fmtCls = Class.forName("android.graphics.Bitmap$CompressFormat");
            Object pngFmt = fmtCls.getField("PNG").get(null);
            Class<?> canvasCls = Class.forName("android.graphics.Canvas");
            Method createBmp = bmpCls.getMethod("createBitmap", int.class, int.class, cfgCls);
            Method setBounds = drawable.getClass().getMethod("setBounds", int.class, int.class, int.class, int.class);
            Method draw = drawable.getClass().getMethod("draw", canvasCls);
            Method compress = bmpCls.getMethod("compress", fmtCls, int.class, java.io.OutputStream.class);
            Method recycle = bmpCls.getMethod("recycle");

            int[] sizes = new int[] { 192, 128, 96 };
            for (int sz : sizes) {
                Object bmp = createBmp.invoke(null, sz, sz, argb8888);
                Object canvas = canvasCls.getConstructor(bmpCls).newInstance(bmp);
                setBounds.invoke(drawable, 0, 0, sz, sz);
                draw.invoke(drawable, canvas);
                File tmp = new File(outFile.getAbsolutePath() + ".tmp");
                FileOutputStream fos = new FileOutputStream(tmp);
                try {
                    compress.invoke(bmp, pngFmt, 100, fos);
                } finally {
                    fos.close();
                    recycle.invoke(bmp);
                }
                if (tmp.length() > 0 && tmp.length() <= 96 * 1024) {
                    tmp.renameTo(outFile);
                    return true;
                }
                if (sz == sizes[sizes.length - 1] && tmp.length() > 0) {
                    tmp.renameTo(outFile);
                    return true;
                }
                tmp.delete();
            }
        } catch (Throwable t) {
            log("exportAppIconPng failed for " + outFile.getName() + ": " + t);
        }
        return false;
    }

    private String resolveLauncherComponent(String pkg) {
        if (systemContext == null) return "";
        try {
            Object pm = systemContext.getClass().getMethod("getPackageManager").invoke(systemContext);
            Object launchIntent = pm.getClass().getMethod("getLaunchIntentForPackage", String.class)
                    .invoke(pm, pkg);
            if (launchIntent != null) {
                Object cn = launchIntent.getClass().getMethod("getComponent").invoke(launchIntent);
                if (cn != null) {
                    return (String) cn.getClass().getMethod("flattenToShortString").invoke(cn);
                }
            }
        } catch (Throwable ignored) {}
        return "";
    }

    private String resolveAppLabel(String pkg) {
        if (systemContext == null) return pkg;
        try {
            Object pm = systemContext.getClass().getMethod("getPackageManager").invoke(systemContext);
            Object appInfo = pm.getClass().getMethod("getApplicationInfo", String.class, int.class)
                    .invoke(pm, pkg, 0);
            if (appInfo != null) {
                Object cs = pm.getClass().getMethod("getApplicationLabel",
                        Class.forName("android.content.pm.ApplicationInfo")).invoke(pm, appInfo);
                if (cs != null && cs.toString().length() > 0) return cs.toString();
            }
        } catch (Throwable ignored) {}
        return pkg;
    }

    private static int getStaticInt(String className, String fieldName, int fallback) {
        try {
            return Class.forName(className).getField(fieldName).getInt(null);
        } catch (Throwable ignored) {
            return fallback;
        }
    }

    private static int getIntField(Object obj, String name, int fallback) {
        try {
            Field f = findField(obj.getClass(), name);
            if (f != null) return f.getInt(obj);
        } catch (Throwable ignored) {}
        return fallback;
    }

    private static Object getFieldObj(Object obj, String name) {
        try {
            Field f = findField(obj.getClass(), name);
            if (f != null) return f.get(obj);
        } catch (Throwable ignored) {}
        return null;
    }

    private static Field findField(Class<?> cls, String name) {
        Class<?> c = cls;
        while (c != null) {
            try {
                Field f = c.getDeclaredField(name);
                f.setAccessible(true);
                return f;
            } catch (NoSuchFieldException e) {
                c = c.getSuperclass();
            }
        }
        return null;
    }

    private static String jsonStr(String s) {
        if (s == null) return "\"\"";
        StringBuilder sb = new StringBuilder(s.length() + 2);
        sb.append('"');
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            switch (c) {
                case '"': sb.append("\\\""); break;
                case '\\': sb.append("\\\\"); break;
                case '\n': sb.append("\\n"); break;
                case '\r': sb.append("\\r"); break;
                case '\t': sb.append("\\t"); break;
                default:
                    if (c < 0x20) {
                        sb.append(String.format("\\u%04x", (int) c));
                    } else {
                        sb.append(c);
                    }
            }
        }
        sb.append('"');
        return sb.toString();
    }

    private static void log(String msg) {
        System.err.println("[SvboxWmAgent] " + msg);
        System.err.flush();
    }
}
