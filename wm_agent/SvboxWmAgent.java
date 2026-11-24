/* SPDX-License-Identifier: MIT */
package org.svbox.wm;

import android.app.ActivityManager;
import android.app.TaskStackListener;
import android.content.ComponentName;
import android.hardware.display.IVirtualDisplayCallback;
import android.view.Surface;

import java.io.File;
import java.io.FileOutputStream;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.util.ArrayList;
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
    private Object atm;
    private Object am;

    private final Object slotLock = new Object();
    private final Map<Integer, DisplaySlot> slotsByIdx = new HashMap<Integer, DisplaySlot>();
    private final Map<Integer, DisplaySlot> slotsByAndroidId = new HashMap<Integer, DisplaySlot>();
    private final CopyOnWriteArrayList<Integer> eventFds = new CopyOnWriteArrayList<Integer>();
    private final Set<Integer> notifiedTasks = Collections.newSetFromMap(new ConcurrentHashMap<Integer, Boolean>());

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
        atm = asInterface("android.app.IActivityTaskManager$Stub",
                getSvc.invoke(null, "activity_task"));
        am = asInterface("android.app.IActivityManager$Stub",
                getSvc.invoke(null, "activity"));

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
        int displayId = knownDisplayId;
        if (displayId < 0) {
            displayId = findDisplayForTask(taskId);
        }
        if (displayId <= 0) return;

        DisplaySlot slot;
        synchronized (slotLock) {
            slot = slotsByAndroidId.get(displayId);
        }
        if (slot == null || slot.boundPkg == null || slot.boundPkg.isEmpty()) return;
        if (pkg.equals(slot.boundPkg)) return;
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

    private int findDisplayForTask(int taskId) {
        if (atm == null) return -1;
        try {
            @SuppressWarnings("unchecked")
            List<Object> tasks = (List<Object>) atm.getClass()
                    .getMethod("getAllRootTaskInfos").invoke(atm);
            if (tasks == null) return -1;
            for (Object t : tasks) {
                int tid = getIntField(t, "taskId", -1);
                if (tid == taskId) {
                    return getIntField(t, "displayId", -1);
                }
                Object childIds = getFieldObj(t, "childTaskIds");
                if (childIds instanceof int[]) {
                    for (int cid : (int[]) childIds) {
                        if (cid == taskId) {
                            return getIntField(t, "displayId", -1);
                        }
                    }
                }
            }
        } catch (Throwable ignored) {}
        return -1;
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
                return cmdSendKey(slot, keyCode);
            } else if ("CLOSE_APP".equals(cmd) && parts.length >= 2) {
                int slot = Integer.parseInt(parts[1]);
                String pkg = parts.length >= 3 ? parts[2] : "";
                return cmdCloseApp(slot, pkg);
            } else if ("GET_TASKS".equals(cmd)) {
                int slot = parts.length >= 2 ? Integer.parseInt(parts[1]) : -1;
                return cmdGetTasks(slot);
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
                displayMgr, config, cb, null, "android")).intValue();

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

        // Resolve launcher component if not supplied
        if (comp == null || comp.isEmpty()) {
            comp = resolveLauncherComponent(pkg);
        }

        // Launch cleanly via IActivityTaskManager with ActivityOptions.setLaunchDisplayId(displayId)
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
        int rc = ((Integer) startAsUser.invoke(atm,
                null, "android", null, intent, null, null, null, 0, 0, null, bundle, 0)).intValue();
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
        atm.getClass().getMethod("moveRootTaskToDisplay", int.class, int.class)
                .invoke(atm, taskId, targetDisplayId);
        try {
            atm.getClass().getMethod("focusTopTask", int.class).invoke(atm, targetDisplayId);
        } catch (Throwable ignored) {}
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

    private String cmdSendKey(int slot, int keyCode) throws Exception {
        int displayId = 0;
        if (slot > 0) {
            synchronized (slotLock) {
                DisplaySlot ds = slotsByIdx.get(slot);
                if (ds == null) return "{\"ok\":false,\"error\":\"slot not active\"}";
                displayId = ds.androidDisplayId;
            }
        }
        long now = ((Long) Class.forName("android.os.SystemClock")
                .getMethod("uptimeMillis").invoke(null)).longValue();
        Class<?> keCls = Class.forName("android.view.KeyEvent");
        Class<?> ieCls = Class.forName("android.view.InputEvent");
        Constructor<?> keCtor = keCls.getConstructor(
                long.class, long.class, int.class, int.class, int.class,
                int.class, int.class, int.class, int.class, int.class);
        Method setDisp = ieCls.getMethod("setDisplayId", int.class);
        Method inject = inputMgr.getClass().getMethod("injectInputEvent", ieCls, int.class);

        Object down = keCtor.newInstance(now, now, 0 /* ACTION_DOWN */, keyCode, 0, 0, -1, 0, 0, 0x101 /* KEYBOARD */);
        setDisp.invoke(down, displayId);
        inject.invoke(inputMgr, down, 0 /* ASYNC */);

        Object up = keCtor.newInstance(now, now + 15, 1 /* ACTION_UP */, keyCode, 0, 0, -1, 0, 0, 0x101);
        setDisp.invoke(up, displayId);
        inject.invoke(inputMgr, up, 0 /* ASYNC */);
        return "{\"ok\":true,\"displayId\":" + displayId + ",\"keyCode\":" + keyCode + "}";
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

                String label = pkg;
                try {
                    Object cs = ri.getClass().getMethod("loadLabel",
                            Class.forName("android.content.pm.PackageManager")).invoke(ri, pm);
                    if (cs != null && cs.toString().length() > 0) label = cs.toString();
                } catch (Throwable ignored) {}

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
                if (!hasIcon || forceIcons) {
                    hasIcon = exportAppIconPng(ri, pm, iconOut);
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
        sb.append("]}");
        return sb.toString();
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
