#!/usr/bin/env python3
"""E4 Bring-Up Harness: Boot stock Android 11 servicemanager + hwservicemanager +
zygote64 --start-system-server inside svbox on HarmonyOS PC."""

import os
import sys
import time
import mmap
import array
import signal
import socket
import struct
import shutil
import threading
import subprocess

SVBOX = "/storage/Users/currentUser/Workspace/svbox/build/svbox"
ANDROID_DIR = "/storage/Users/currentUser/Workspace/svbox-Android"
ROOTFS = f"{ANDROID_DIR}/rootfs"
BINDERD = f"{ANDROID_DIR}/binderd/svbox-binderd"
PROPD = f"{ANDROID_DIR}/propd/svbox-propd"

# HarmonyOS PC: UNIX sockets must live under app sandbox temp, never /tmp or ~
RUN_BASE = "/data/storage/el2/base/haps/entry/temp/svbox-android-e4"
SOCK_DIR = f"{RUN_BASE}/sockets"
PROP_DIR = f"{RUN_BASE}/__properties__"
DATA_DIR = f"{RUN_BASE}/data"
CACHE_DIR = f"{RUN_BASE}/cache"
ACCT_DIR = f"{RUN_BASE}/acct"
CGROUP_DIR = f"{RUN_BASE}/cgroup"
DEV_CPUSET = f"{RUN_BASE}/cpuset"
DEV_STUNE = f"{RUN_BASE}/stune"
MNT_DIR = f"{RUN_BASE}/mnt"
STORAGE_DIR = f"{RUN_BASE}/storage"
SYSPOWER_DIR = f"{RUN_BASE}/sys_power"
DEV_INPUT_DIR = f"{RUN_BASE}/dev_input"
IPC_DIR = f"{RUN_BASE}/ipc"
OVERRIDE_PROP = f"{RUN_BASE}/override.prop"
LOG_FILE = f"{RUN_BASE}/logcat.txt"

BOOTCLASSPATH = (
    "/apex/com.android.art/javalib/core-oj.jar:"
    "/apex/com.android.art/javalib/core-libart.jar:"
    "/apex/com.android.art/javalib/core-icu4j.jar:"
    "/apex/com.android.art/javalib/okhttp.jar:"
    "/apex/com.android.art/javalib/bouncycastle.jar:"
    "/apex/com.android.art/javalib/apache-xml.jar:"
    "/system/framework/framework.jar:"
    "/system/framework/ext.jar:"
    "/system/framework/telephony-common.jar:"
    "/system/framework/voip-common.jar:"
    "/system/framework/ims-common.jar:"
    "/system/framework/framework-atb-backward-compatibility.jar:"
    "/apex/com.android.conscrypt/javalib/conscrypt.jar:"
    "/apex/com.android.media/javalib/updatable-media.jar:"
    "/apex/com.android.mediaprovider/javalib/framework-mediaprovider.jar:"
    "/apex/com.android.os.statsd/javalib/framework-statsd.jar:"
    "/apex/com.android.permission/javalib/framework-permission.jar:"
    "/apex/com.android.sdkext/javalib/framework-sdkextensions.jar:"
    "/apex/com.android.wifi/javalib/framework-wifi.jar:"
    "/apex/com.android.tethering/javalib/framework-tethering.jar"
)

DEX2OATBOOTCLASSPATH = (
    "/apex/com.android.art/javalib/core-oj.jar:"
    "/apex/com.android.art/javalib/core-libart.jar:"
    "/apex/com.android.art/javalib/core-icu4j.jar:"
    "/apex/com.android.art/javalib/okhttp.jar:"
    "/apex/com.android.art/javalib/bouncycastle.jar:"
    "/apex/com.android.art/javalib/apache-xml.jar:"
    "/system/framework/framework.jar:"
    "/system/framework/ext.jar:"
    "/system/framework/telephony-common.jar:"
    "/system/framework/voip-common.jar:"
    "/system/framework/ims-common.jar:"
    "/system/framework/framework-atb-backward-compatibility.jar"
)

SYSTEMSERVERCLASSPATH = (
    "/system/framework/com.android.location.provider.jar:"
    "/system/framework/services.jar:"
    "/system/framework/ethernet-service.jar:"
    "/apex/com.android.permission/javalib/service-permission.jar:"
    "/apex/com.android.ipsec/javalib/android.net.ipsec.ike.jar"
)

PRIO_MAP = {2: "V", 3: "D", 4: "I", 5: "W", 6: "E", 7: "F"}
BUF_MAP = {0: "M", 1: "R", 2: "E", 3: "S", 4: "C"}

def svbox_base_cmd():
    return [
        SVBOX,
        "--root", ROOTFS,
        "-u", "0:0",
        "--cap-add", "ALL",
        "--kernel-sysname", "Linux",
        "--kernel-release", "5.10.0-android11",
        "--hostname", "localhost",
        "--emulate-mounts",
        "--mount-scratch", f"{RUN_BASE}/tmpfs",
        "-v", "/proc:/proc",
        "-v", "/dev/null:/dev/null",
        "-v", "/dev/zero:/dev/zero",
        "-v", "/dev/urandom:/dev/urandom",
        "-v", "/dev/random:/dev/random",
        "-v", "/dev/ashmem:/dev/ashmem",
        "-v", f"{ROOTFS}/system/apex:/apex",
        "-v", f"{ROOTFS}/system/apex/com.android.art.debug:/apex/com.android.art",
        "-v", f"{ROOTFS}/system/apex/com.android.vndk.current:/apex/com.android.vndk.v30",
        "-v", f"{PROP_DIR}:/dev/__properties__",
        "-v", f"{SOCK_DIR}:/dev/socket",
        "-v", f"{DEV_INPUT_DIR}:/dev/input",
        "-v", f"{IPC_DIR}:/ipc",
        "-v", f"{DATA_DIR}:/data",
        "-v", f"{CACHE_DIR}:/cache",
        "-v", f"{ACCT_DIR}:/acct",
        "-v", f"{CGROUP_DIR}:/sys/fs/cgroup",
        "-v", f"{SYSPOWER_DIR}:/sys/power",
        "-v", f"{DEV_CPUSET}:/dev/cpuset",
        "-v", f"{DEV_STUNE}:/dev/stune",
        "-v", f"{MNT_DIR}:/mnt",
        "-v", f"{STORAGE_DIR}:/storage",
        "--vdev", f"/dev/binder:{SOCK_DIR}/binder.sock:binder",
        "--vdev", f"/dev/hwbinder:{SOCK_DIR}/hwbinder.sock:binder",
        "--vdev", f"/dev/vndbinder:{SOCK_DIR}/vndbinder.sock:binder",
        "-e", "ANDROID_BOOTLOGO=1",
        "-e", "ANDROID_ROOT=/system",
        "-e", "ANDROID_ASSETS=/system/app",
        "-e", "ANDROID_DATA=/data",
        "-e", "ANDROID_STORAGE=/storage",
        "-e", "ANDROID_ART_ROOT=/apex/com.android.art",
        "-e", "ANDROID_I18N_ROOT=/apex/com.android.i18n",
        "-e", "ANDROID_TZDATA_ROOT=/apex/com.android.tzdata",
        "-e", "ANDROID_RUNTIME_ROOT=/apex/com.android.runtime",
        "-e", "EXTERNAL_STORAGE=/sdcard",
        "-e", "ASEC_MOUNTPOINT=/mnt/asec",
        "-e", f"BOOTCLASSPATH={BOOTCLASSPATH}",
        "-e", f"DEX2OATBOOTCLASSPATH={DEX2OATBOOTCLASSPATH}",
        "-e", f"SYSTEMSERVERCLASSPATH={SYSTEMSERVERCLASSPATH}",
        "-e", "PATH=/product/bin:/apex/com.android.runtime/bin:/apex/com.android.art/bin:/system_ext/bin:/system/bin:/system/xbin:/vendor/bin",
    ]

def start_logdw(sock_path, stop_ev, log_fp):
    if os.path.exists(sock_path):
        os.unlink(sock_path)
    s = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
    s.bind(sock_path)
    s.settimeout(0.2)

    def loop():
        while not stop_ev.is_set():
            try:
                data = s.recv(8192)
            except socket.timeout:
                continue
            except OSError:
                break
            if len(data) < 12:
                continue
            log_id, tid, sec, nsec = struct.unpack("<BHII", data[:11])
            if log_id == 2:  # binary events log
                continue
            prio = data[11]
            body = data[12:]
            parts = body.split(b"\x00", 2)
            tag = parts[0].decode("utf-8", errors="replace") if len(parts) > 0 else "?"
            msg = parts[1].decode("utf-8", errors="replace").rstrip("\n") if len(parts) > 1 else ""
            p = PRIO_MAP.get(prio, str(prio))
            b = BUF_MAP.get(log_id, str(log_id))
            line = f"[{b} {p}/{tag}({tid})] {msg}"
            log_fp.write(line + "\n")
            log_fp.flush()
            if prio >= 5 or tag in (
                "Zygote", "Zygote64", "AndroidRuntime", "SystemServer",
                "ServiceManager", "ActivityManager", "PackageManager",
                "SystemServiceManager", "zygote64", "Binder", "DEBUG",
                "art", "dalvikvm64", "libprocessgroup", "Process",
            ):
                print(line, flush=True)
        s.close()

    t = threading.Thread(target=loop, daemon=True)
    t.start()
    return t

def start_hwc_receiver(sock_path, stop_ev, stats):
    def recv_exact(s, n):
        buf = bytearray()
        while len(buf) < n:
            chunk = s.recv(n - len(buf))
            if not chunk:
                return None
            buf.extend(chunk)
        return bytes(buf)

    def loop():
        while not stop_ev.is_set():
            if not os.path.exists(sock_path):
                time.sleep(0.1)
                continue
            try:
                s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                s.connect(sock_path)
                s.settimeout(1.0)
                print(f"[HWC-STREAM] Connected to {sock_path}!", flush=True)
            except OSError:
                time.sleep(0.2)
                continue

            while not stop_ev.is_set():
                try:
                    hdr = recv_exact(s, 64)
                    if not hdr:
                        break
                    msg, ancdata, flags, addr = s.recvmsg(16, socket.CMSG_SPACE(array.array("i").itemsize))
                    prime_fd = -1
                    for cmsg_level, cmsg_type, cmsg_data in ancdata:
                        if cmsg_level == socket.SOL_SOCKET and cmsg_type == socket.SCM_RIGHTS:
                            fds = array.array("i")
                            fds.frombytes(cmsg_data[:len(cmsg_data) - (len(cmsg_data) % fds.itemsize)])
                            if fds:
                                prime_fd = fds[0]
                    fields = struct.unpack("<16i", hdr)
                    stats["frames"] += 1
                    n = stats["frames"]
                    if n <= 3 or n % 30 == 0:
                        nz = -1
                        if prime_fd >= 0:
                            try:
                                sz = os.fstat(prime_fd).st_size
                                if sz <= 0:
                                    sz = max(fields[6], 720 * 1280 * 4)
                                mm = mmap.mmap(prime_fd, min(sz, 16 * 1024 * 1024), prot=mmap.PROT_READ)
                                sample = mm[:min(len(mm), 65536)]
                                nz = sum(1 for b in sample if b != 0)
                                stats["last_size"] = len(mm)
                                stats["last_nz"] = nz
                                mm.close()
                            except Exception as e:
                                nz = -2
                                print(f"[HWC-STREAM] mmap err: {e}", flush=True)
                        print(f"[HWC-STREAM] frame #{n}: fd={prime_fd} fields={fields[:12]} sample_nz={nz}", flush=True)
                    if prime_fd >= 0:
                        os.close(prime_fd)
                    s.sendall(b"ok")
                except socket.timeout:
                    continue
                except OSError as e:
                    print(f"[HWC-STREAM] recv err: {e}", flush=True)
                    break
            s.close()

    t = threading.Thread(target=loop, daemon=True)
    t.start()
    return t

def prep_dirs():
    if os.path.exists(RUN_BASE):
        shutil.rmtree(RUN_BASE, ignore_errors=True)
    for d in [
        SOCK_DIR, PROP_DIR, CACHE_DIR, ACCT_DIR, CGROUP_DIR, SYSPOWER_DIR, DEV_INPUT_DIR, IPC_DIR,
        DEV_CPUSET, DEV_STUNE, MNT_DIR, STORAGE_DIR, f"{RUN_BASE}/tmpfs",
        f"{DATA_DIR}/system",
        f"{DATA_DIR}/system/users/0",
        f"{DATA_DIR}/system/dropbox",
        f"{DATA_DIR}/system/usagestats",
        f"{DATA_DIR}/system_de/0",
        f"{DATA_DIR}/system_ce/0",
        f"{DATA_DIR}/misc/profiles/cur/0",
        f"{DATA_DIR}/misc/profiles/ref",
        f"{DATA_DIR}/misc/apexdata",
        f"{DATA_DIR}/misc/keystore",
        f"{DATA_DIR}/misc/installd",
        f"{DATA_DIR}/misc/user/0",
        f"{DATA_DIR}/misc/media",
        f"{DATA_DIR}/misc/credstore",
        f"{DATA_DIR}/misc_ce/0",
        f"{DATA_DIR}/misc_de/0",
        f"{DATA_DIR}/vendor_ce/0",
        f"{DATA_DIR}/vendor_de/0",
        f"{DATA_DIR}/media/0",
        f"{DATA_DIR}/user/0",
        f"{DATA_DIR}/user_de/0",
        f"{DATA_DIR}/app",
        f"{DATA_DIR}/app-private",
        f"{DATA_DIR}/app-ephemeral",
        f"{DATA_DIR}/app-asec",
        f"{DATA_DIR}/app-lib",
        f"{DATA_DIR}/app-staging",
        f"{DATA_DIR}/data",
        f"{DATA_DIR}/dalvik-cache/arm64",
        f"{DATA_DIR}/dalvik-cache/profiles",
        f"{DATA_DIR}/resource-cache",
        f"{DATA_DIR}/anr",
        f"{DATA_DIR}/tombstones",
        f"{DATA_DIR}/local/tmp",
        f"{DATA_DIR}/property",
        f"{DATA_DIR}/apex",
        f"{DATA_DIR}/apex/active",
        f"{DATA_DIR}/apex/backup",
        f"{DATA_DIR}/apex/decompressed",
        f"{DATA_DIR}/rollback",
        f"{DATA_DIR}/rollback-observer",
        f"{DATA_DIR}/server_configurable_flags",
        f"{DATA_DIR}/backup",
        f"{DATA_DIR}/cache",
        f"{DATA_DIR}/incremental",
        f"{DATA_DIR}/ota",
        f"{DATA_DIR}/ota_package",
        f"{DATA_DIR}/preloads",
        f"{DATA_DIR}/ss",
        f"{DATA_DIR}/vendor",
        f"{DEV_CPUSET}/foreground",
        f"{DEV_CPUSET}/background",
        f"{DEV_CPUSET}/system-background",
        f"{DEV_CPUSET}/top-app",
        f"{DEV_STUNE}/foreground",
        f"{DEV_STUNE}/background",
        f"{DEV_STUNE}/top-app",
        f"{MNT_DIR}/user/0",
        f"{MNT_DIR}/runtime/default",
        f"{STORAGE_DIR}/emulated/0",
    ]:
        os.makedirs(d, exist_ok=True)
    # Write Redroid guest-mode display & runtime property overrides
    with open(OVERRIDE_PROP, "w") as f:
        f.write(
            "ro.hardware.egl=swiftshader\n"
            "ro.hardware.gralloc=redroid\n"
            "ro.hardware.vulkan=pastel\n"
            "ro.opengles.version=196610\n"
            "ro.sf.lcd_density=320\n"
            "debug.renderengine.backend=gles\n"
            "debug.sf.nobootanimation=0\n"
            "debug.stagefright.ccodec=0\n"
            "ro.boot.redroid_width=720\n"
            "ro.boot.redroid_height=1280\n"
            "ro.boot.redroid_fps=30\n"
            "ro.boot.use_redroid_stream=1\n"
            "bpf.progs_loaded=1\n"
            "log.tag.libprocessgroup=F\n"
            "sys.use_memfd=false\n"
        )
    # Create dummy /sys/power files for android.system.suspend@1.0-service
    for fn, content in [
        ("wake_lock", ""),
        ("wake_unlock", ""),
        ("state", "mem\n"),
        ("wakeup_count", "0\n"),
    ]:
        with open(os.path.join(SYSPOWER_DIR, fn), "w") as f:
            f.write(content)
    # Create dummy cgroup/cpuset control files so libprocessgroup doesn't fail fatally
    for sub in ["", "foreground", "background", "system-background", "top-app"]:
        for fn in ["tasks", "cgroup.procs", "cpus", "mems"]:
            p = os.path.join(DEV_CPUSET, sub, fn) if sub else os.path.join(DEV_CPUSET, fn)
            if not os.path.exists(p):
                with open(p, "w") as f:
                    if fn == "cpus":
                        f.write("0-7\n")
                    elif fn == "mems":
                        f.write("0\n")

def make_listen_unix(path):
    if os.path.exists(path):
        os.unlink(path)
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.bind(path)
    s.listen(16)
    os.set_inheritable(s.fileno(), True)
    return s

def main():
    duration = int(sys.argv[1]) if len(sys.argv) > 1 else 25
    prep_dirs()
    log_fp = open(LOG_FILE, "w", buffering=1)
    stop_ev = threading.Event()
    procs = []
    socks = []

    try:
        # 1. Start logdw receiver & HWC frame stream receiver
        start_logdw(f"{SOCK_DIR}/logdw", stop_ev, log_fp)
        hwc_stats = {"frames": 0, "last_size": 0, "last_nz": 0}
        start_hwc_receiver(f"{IPC_DIR}/hwcomposer.sock", stop_ev, hwc_stats)

        # 2. Start svbox-propd
        prop_files = [
            p for p in [
                f"{ROOTFS}/system/build.prop",
                f"{ROOTFS}/vendor/build.prop",
                f"{ROOTFS}/vendor/default.prop",
                f"{ROOTFS}/default.prop",
                OVERRIDE_PROP,
            ] if os.path.exists(p)
        ]
        p_propd = subprocess.Popen(
            [PROPD, "--dir", PROP_DIR, "--sock", f"{SOCK_DIR}/property_service"] + prop_files,
            stderr=subprocess.PIPE, text=True
        )
        procs.append(("propd", p_propd))

        # 3. Start binder daemons
        for name, sock in [
            ("binderd", f"{SOCK_DIR}/binder.sock"),
            ("hwbinderd", f"{SOCK_DIR}/hwbinder.sock"),
            ("vndbinderd", f"{SOCK_DIR}/vndbinder.sock"),
        ]:
            args = [BINDERD, "-v", sock] if name == "binderd" else [BINDERD, sock]
            p = subprocess.Popen(
                args,
                stderr=open(f"{RUN_BASE}/{name}.err", "w"),
            )
            procs.append((name, p))

        # Wait for sockets
        for s in [
            f"{SOCK_DIR}/property_service",
            f"{SOCK_DIR}/binder.sock",
            f"{SOCK_DIR}/hwbinder.sock",
            f"{SOCK_DIR}/vndbinder.sock",
        ]:
            for _ in range(50):
                if os.path.exists(s):
                    break
                time.sleep(0.05)

        # 4. Start servicemanager & hwservicemanager
        p_sm = subprocess.Popen(
            svbox_base_cmd() + ["system/bin/servicemanager"],
            stdout=open(f"{RUN_BASE}/sm.out", "w"),
            stderr=open(f"{RUN_BASE}/sm.err", "w"),
        )
        procs.append(("servicemanager", p_sm))

        p_hwsm = subprocess.Popen(
            svbox_base_cmd() + ["system/bin/hwservicemanager"],
            stdout=open(f"{RUN_BASE}/hwsm.out", "w"),
            stderr=open(f"{RUN_BASE}/hwsm.err", "w"),
        )
        procs.append(("hwservicemanager", p_hwsm))
        # Wait until binderd logs SET_CONTEXT_MGR (up to 5s)
        for _ in range(100):
            try:
                with open(f"{RUN_BASE}/binderd.err") as bf:
                    if "SET_CONTEXT_MGR" in bf.read():
                        break
            except Exception:
                pass
            time.sleep(0.05)
        time.sleep(0.2)

        # 4b. Start core native daemons & mock/software HALs
        vendor_env = ["-e", "LD_LIBRARY_PATH=/vendor/lib64:/vendor/lib64/hw:/system/lib64:/apex/com.android.vndk.current/lib64"]
        for hname, hbin in [
            ("installd", ["system/bin/installd"]),
            ("storaged", ["system/bin/storaged"]),
            ("vold", ["system/bin/vold", "--blkid_context=u:r:blkid:s0", "--blkid_untrusted_context=u:r:blkid_untrusted:s0", "--fsck_context=u:r:fsck:s0", "--fsck_untrusted_context=u:r:fsck_untrusted:s0"]),
            ("idmap2d", ["system/bin/idmap2d"]),
            ("keystore", ["system/bin/keystore", "/data/misc/keystore"]),
            ("suspend_hal", ["system/bin/hw/android.system.suspend@1.0-service"]),
            ("hidl_alloc", ["system/bin/hw/android.hidl.allocator@1.0-service"]),
            ("power_stats_hal", ["vendor/bin/hw/android.hardware.power.stats@1.0-service.mock"]),
            ("power_hal", ["vendor/bin/hw/android.hardware.power-service.example"]),
            ("health_hal", ["vendor/bin/hw/android.hardware.health@2.1-service"]),
            ("gatekeeper_hal", ["vendor/bin/hw/android.hardware.gatekeeper@1.0-service.software"]),
            ("keymaster_hal", ["vendor/bin/hw/android.hardware.keymaster@3.0-service"]),
            ("sensors_hal", ["vendor/bin/hw/android.hardware.sensors@2.1-service.mock"]),
            ("gralloc_hal", ["vendor/bin/hw/android.hardware.graphics.allocator@2.0-service"]),
            ("hwc_hal", ["vendor/bin/hw/android.hardware.graphics.composer@2.1-service"]),
        ]:
            extra = vendor_env if hbin[0].startswith("vendor/") else []
            p = subprocess.Popen(
                svbox_base_cmd() + extra + hbin,
                stdout=open(f"{RUN_BASE}/{hname}.out", "w"),
                stderr=open(f"{RUN_BASE}/{hname}.err", "w"),
            )
            procs.append((hname, p))
        time.sleep(1.0)

        # 4b2. Start netd (with inherited dnsproxyd/mdns/fwmarkd sockets)
        s_dns = make_listen_unix(f"{SOCK_DIR}/dnsproxyd")
        s_mdns = make_listen_unix(f"{SOCK_DIR}/mdns")
        s_fw = make_listen_unix(f"{SOCK_DIR}/fwmarkd")
        socks.extend([s_dns, s_mdns, s_fw])
        p_netd = subprocess.Popen(
            svbox_base_cmd() + [
                "-e", f"ANDROID_SOCKET_dnsproxyd={s_dns.fileno()}",
                "-e", f"ANDROID_SOCKET_mdns={s_mdns.fileno()}",
                "-e", f"ANDROID_SOCKET_fwmarkd={s_fw.fileno()}",
                "system/bin/netd",
            ],
            pass_fds=(s_dns.fileno(), s_mdns.fileno(), s_fw.fileno()),
            stdout=open(f"{RUN_BASE}/netd.out", "w"),
            stderr=open(f"{RUN_BASE}/netd.err", "w"),
        )
        procs.append(("netd", p_netd))
        time.sleep(0.5)

        # 4c. Start SurfaceFlinger (uses gralloc.redroid + hwcomposer.redroid + SwiftShader GLES)
        sf_env = ["-e", "LD_LIBRARY_PATH=/system/lib64:/vendor/lib64:/vendor/lib64/egl:/vendor/lib64/hw:/apex/com.android.vndk.current/lib64"]
        p_sf = subprocess.Popen(
            svbox_base_cmd() + sf_env + ["system/bin/surfaceflinger"],
            stdout=open(f"{RUN_BASE}/sf.out", "w"),
            stderr=open(f"{RUN_BASE}/sf.err", "w"),
        )
        procs.append(("surfaceflinger", p_sf))
        time.sleep(1.5)

        # 4d. Start bootanimation briefly so SurfaceFlinger actively composites frames onto /ipc/hwcomposer.sock
        p_ba = subprocess.Popen(
            svbox_base_cmd() + sf_env + ["system/bin/bootanimation"],
            stdout=open(f"{RUN_BASE}/ba.out", "w"),
            stderr=open(f"{RUN_BASE}/ba.err", "w"),
        )
        procs.append(("bootanimation", p_ba))
        time.sleep(1.0)

        # Verify servicemanager is up
        chk = subprocess.run(
            svbox_base_cmd() + ["system/bin/service", "list"],
            capture_output=True, text=True, timeout=10,
        )
        print(f"=== Pre-zygote service list (rc={chk.returncode}) ===\n{chk.stdout.strip()}", flush=True)

        # 5. Prepare zygote inherited sockets
        s_zyg = make_listen_unix(f"{SOCK_DIR}/zygote")
        s_usap = make_listen_unix(f"{SOCK_DIR}/usap_pool_primary")
        socks.extend([s_zyg, s_usap])
        zfd = s_zyg.fileno()
        ufd = s_usap.fileno()

        # 6. Launch zygote64 --start-system-server!
        # Android Zygote checks /proc/self/fd whitelist before forking system_server:
        # stdin/stdout/stderr MUST be /dev/null (just like Android init), and no extra FDs.
        zyg_cmd = svbox_base_cmd() + [
            "-e", f"ANDROID_SOCKET_zygote={zfd}",
            "-e", f"ANDROID_SOCKET_usap_pool_primary={ufd}",
            "system/bin/app_process64",
            "-Xzygote", "/system/bin",
            "--zygote", "--start-system-server", "--socket-name=zygote",
        ]
        print(f"=== Launching zygote64 (zfd={zfd}, ufd={ufd}) ===", flush=True)
        p_zyg = subprocess.Popen(
            zyg_cmd,
            pass_fds=(zfd, ufd),
            close_fds=True,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        procs.append(("zygote64", p_zyg))

        # Monitor for `duration` seconds and poll `service list` periodically
        t0 = time.time()
        last_poll = 0
        while time.time() - t0 < duration:
            if p_zyg.poll() is not None:
                print(f"!!! zygote64 exited early with code {p_zyg.returncode} at +{time.time()-t0:.1f}s", flush=True)
                break
            now = time.time() - t0
            if now - last_poll >= 5.0:
                last_poll = now
                try:
                    r = subprocess.run(
                        svbox_base_cmd() + ["system/bin/service", "list"],
                        capture_output=True, text=True, timeout=5,
                    )
                    lines = [l for l in r.stdout.splitlines() if l.strip()]
                    print(f"=== [+{now:.1f}s] service list ({len(lines)} lines, rc={r.returncode}) ===", flush=True)
                    for l in lines[:120]:
                        print("  " + l, flush=True)
                except Exception as e:
                    print(f"=== [+{now:.1f}s] service list err: {e} ===", flush=True)
            time.sleep(0.5)

        # Check sys.boot_completed & HWC frame stats
        bc = subprocess.run(
            svbox_base_cmd() + ["system/bin/getprop", "sys.boot_completed"],
            capture_output=True, text=True, timeout=5,
        )
        print(f"\n=== Boot & HWC Summary ===\nsys.boot_completed='{bc.stdout.strip()}' hwc_stats={hwc_stats}", flush=True)

        # End-to-end live Binder IPC verification against system_server!
        print("\n=== Live IPC check: cmd package list packages ===", flush=True)
        try:
            pm = subprocess.run(
                svbox_base_cmd() + ["system/bin/cmd", "package", "list", "packages"],
                capture_output=True, text=True, timeout=10,
            )
            pkgs = [l for l in pm.stdout.splitlines() if l.strip()]
            print(f"cmd package list packages: rc={pm.returncode}, total={len(pkgs)} packages", flush=True)
            for l in pkgs[:20]:
                print("  " + l, flush=True)
            if pm.stderr.strip():
                print("stderr:", pm.stderr.strip()[:400], flush=True)
        except Exception as e:
            print(f"cmd package list packages err: {e}", flush=True)

        print("\n=== Final binderd.err (tail) ===", flush=True)
        with open(f"{RUN_BASE}/binderd.err") as f:
            print(f.read()[-1500:], flush=True)

    finally:
        stop_ev.set()
        for name, p in reversed(procs):
            try:
                p.terminate()
            except Exception:
                pass
        time.sleep(0.3)
        for name, p in reversed(procs):
            try:
                p.kill()
                p.wait(timeout=1)
            except Exception:
                pass
        for s in socks:
            try:
                s.close()
            except Exception:
                pass
        log_fp.close()

if __name__ == "__main__":
    main()
