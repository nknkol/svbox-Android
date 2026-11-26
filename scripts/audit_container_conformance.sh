#!/system/bin/sh
# Standardized Linux/Android Container Conformance Audit Script
# Run identically on svbox-Android, Waydroid (LXC), or QEMU/AOSP Android 14 to diff interfaces.

sec() {
    echo ""
    echo "============================================================"
    echo "=== $1 ==="
    echo "============================================================"
}

sec "1. KERNEL & UNAME & GLOBAL /proc"
echo "[uname -a]"
uname -a 2>&1
echo "[/proc/version]"
cat /proc/version 2>&1
echo "[/proc/cmdline]"
cat /proc/cmdline 2>&1
echo "[/proc/uptime]"
cat /proc/uptime 2>&1
echo "[/proc/loadavg]"
cat /proc/loadavg 2>&1
echo "[/proc/stat (head 6)]"
head -n 6 /proc/stat 2>&1
echo "[/proc/filesystems]"
cat /proc/filesystems 2>&1
echo "[/proc/modules (head 10)]"
head -n 10 /proc/modules 2>&1
echo "[/proc/swaps]"
cat /proc/swaps 2>&1
echo "[/proc/cpuinfo (first core + Hardware/Serial/Model)]"
head -n 15 /proc/cpuinfo 2>&1
grep -iE "Hardware|Serial|Model|Revision" /proc/cpuinfo 2>&1 | head -n 10

sec "2. /proc/self/* & /proc/1/* & APP PID COMPARISON"
echo "[/proc/self/status]"
cat /proc/self/status 2>&1
echo "[/proc/self/stat]"
cat /proc/self/stat 2>&1
echo "[/proc/self/wchan]"
cat /proc/self/wchan 2>&1; echo ""
echo "[/proc/self/cgroup]"
cat /proc/self/cgroup 2>&1
echo "[/proc/self/ns]"
ls -la /proc/self/ns 2>&1
echo "[/proc/self/attr/*]"
for f in current prev exec fscreate keycreate sockcreate; do
    printf "  attr/%s: " "$f"
    cat "/proc/self/attr/$f" 2>&1
    echo ""
done
echo "[/proc/self/oom_score_adj & limits (head 8)]"
printf "  oom_score_adj: "; cat /proc/self/oom_score_adj 2>&1
head -n 8 /proc/self/limits 2>&1
echo "[/proc/self/smaps_rollup]"
cat /proc/self/smaps_rollup 2>&1
echo "[/proc/self/fdinfo/0]"
cat /proc/self/fdinfo/0 2>&1
echo "[/proc/1/cmdline & status (Name/PPid/Pid)]"
tr '\0' ' ' < /proc/1/cmdline 2>&1; echo ""
grep -E "^(Name|Pid|PPid|Uid|Seccomp|NoNewPrivs):" /proc/1/status 2>&1
echo "[/proc PID count & sample]"
ls /proc 2>&1 | grep -E '^[0-9]+$' | wc -l
ps -ef 2>&1 | head -n 25

sec "3. /proc/<app_pid>/* (CROSS-PID & UNPRIVILEGED APP CHECK)"
APID=$(pidof com.android.launcher3 2>/dev/null || pidof com.android.systemui 2>/dev/null || pidof com.microsoft.emmx 2>/dev/null | awk '{print $1}')
echo "Sample App PID: $APID"
if [ -n "$APID" ]; then
    echo "[/proc/$APID/status (key fields)]"
    grep -E "^(Name|Pid|PPid|TracerPid|Uid|Gid|Groups|NStgid|NSpid|Cap|NoNewPrivs|Seccomp|SigCgt|SigIgn|SigBlk|Cpus_allowed):" "/proc/$APID/status" 2>&1
    echo "[/proc/$APID/cgroup]"
    cat "/proc/$APID/cgroup" 2>&1
    echo "[/proc/$APID/maps suspicious/special lines]"
    grep -iE "svbox|harmony|ohos|shmm|kshare|el2|hnp|memfd|deleted|inject|userdata|rootfs|vvar|vdso|binder" "/proc/$APID/maps" 2>&1 | head -n 30
    echo "[/proc/$APID/smaps suspicious lines]"
    grep -iE "svbox|harmony|ohos|shmm|kshare|el2|memfd|deleted" "/proc/$APID/smaps" 2>&1 | head -n 15
    echo "[/proc/$APID/fd suspicious links]"
    ls -la "/proc/$APID/fd" 2>&1 | grep -iE "svbox|el2|memfd|deleted|inject|hnp|binder|ipc" | head -n 20
    echo "[/proc/$APID/environ (keys only)]"
    tr '\0' '\n' < "/proc/$APID/environ" 2>&1 | cut -d= -f1 | tr '\n' ' '; echo ""
fi

sec "4. MOUNTS & MOUNTINFO & STATFS"
echo "[/proc/self/mounts]"
cat /proc/self/mounts 2>&1
echo "[/proc/self/mountinfo (head 35)]"
head -n 35 /proc/self/mountinfo 2>&1
echo "[stat -f / /system /data /dev /proc /sys /sdcard]"
for p in / /system /data /dev /proc /sys /sdcard; do
    printf "  %-10s: " "$p"
    stat -f -c "type=%T(0x%t) bsize=%s blocks=%b avail=%a" "$p" 2>&1
done

sec "5. /dev DEVICES & STAT TYPES"
ls -la /dev 2>&1 | head -n 45
echo "[key device node types]"
for d in /dev/binder /dev/hwbinder /dev/vndbinder /dev/ashmem /dev/null /dev/zero /dev/urandom /dev/ptmx /dev/fuse /dev/kmsg /dev/goldfish_pipe /dev/qemu_pipe /dev/input/event0; do
    if [ -e "$d" ]; then
        stat -c "  %-20s mode=%A(%a) type=%F rdev=%t:%T owner=%u:%g" "$d" 2>&1
    else
        echo "  $d : MISSING"
    fi
done

sec "6. /sys FILESYSTEM & HARDWARE TOPOLOGY"
echo "[ls /sys]"
ls -la /sys 2>&1
echo "[/sys/devices/system/cpu]"
ls -la /sys/devices/system/cpu 2>&1 | head -n 20
for f in possible present online kernel_max cpu0/cpufreq/scaling_cur_freq cpu0/cpufreq/cpuinfo_max_freq cpu0/cpufreq/stats/time_in_state; do
    printf "  cpu/%s: " "$f"
    cat "/sys/devices/system/cpu/$f" 2>&1 | head -n 2
done
echo "[/sys/class/power_supply]"
ls -la /sys/class/power_supply 2>&1
echo "[/sys/class/thermal]"
ls -la /sys/class/thermal 2>&1
echo "[/sys/class/net]"
ls -la /sys/class/net 2>&1
for n in /sys/class/net/*; do
    [ -e "$n/address" ] && printf "  %s addr=%s operstate=%s\n" "$n" "$(cat "$n/address" 2>&1)" "$(cat "$n/operstate" 2>&1)"
done
echo "[/sys/fs/selinux]"
ls -la /sys/fs/selinux 2>&1
echo "[/sys/block]"
ls -la /sys/block 2>&1

sec "7. NETWORKING (/proc/net, ip, sockets)"
echo "[ip addr]"
ip addr 2>&1
echo "[ip route]"
ip route 2>&1
echo "[/proc/net/dev]"
cat /proc/net/dev 2>&1
echo "[/proc/net/route]"
cat /proc/net/route 2>&1
echo "[/proc/net/arp]"
cat /proc/net/arp 2>&1
echo "[/proc/net/unix (first 20)]"
head -n 20 /proc/net/unix 2>&1
echo "[/proc/net/tcp (first 15)]"
head -n 15 /proc/net/tcp 2>&1

sec "8. ANDROID PROPERTIES & BUILD.PROP CONSISTENCY & SELINUX & ROOT"
echo "[getenforce & su checks]"
printf "  getenforce: "; getenforce 2>&1
for s in /system/bin/su /system/xbin/su /sbin/su /vendor/bin/su /data/local/tmp/su; do
    [ -e "$s" ] && echo "  SU_EXISTS: $s"
done
echo "[emulator / container / debug / suspicious properties]"
getprop 2>&1 | grep -iE "qemu|goldfish|ranchu|redroid|svbox|harmony|ohos|emulation|emulator|generic|test-keys|userdebug|eng\.|debuggable|adb\.secure|ro\.secure|verifiedboot|vbmeta|flash\.locked|ro\.hardware|ro\.boot\.|ro\.build\.|ro\.product\.|ro\.serialno|dalvik\.vm|persist\.sys\.usb|init\.svc\.adbd" | sort
echo "[build.prop vs getprop spot check]"
for p in /system/build.prop /vendor/build.prop /product/build.prop /system_ext/build.prop /odm/build.prop /default.prop; do
    if [ -f "$p" ]; then
        echo "  $p: $(grep -E '^(ro\.build\.fingerprint|ro\.build\.type|ro\.build\.tags|ro\.product\.model|ro\.hardware|ro\.debuggable)=' "$p" | tr '\n' ' ')"
    else
        echo "  $p: MISSING"
    fi
done

sec "9. ANDROID SERVICES, FEATURES, GPU, BATTERY, SENSORS, IDS"
echo "[service list count & key services]"
service list 2>&1 | wc -l
for svc in activity window SurfaceFlinger input audio battery batteryproperties sensorservice wifi connectivity location telephony.registry keystore android.system.keystore2.IKeystoreService/default gatekeeper credstore gpu virtualdevice drm.drmManager media.camera; do
    printf "  %-48s: " "$svc"
    service check "$svc" 2>&1
done
echo "[dumpsys battery]"
dumpsys battery 2>&1 | head -n 20
echo "[dumpsys sensorservice (head 20)]"
dumpsys sensorservice 2>&1 | head -n 20
echo "[dumpsys SurfaceFlinger GLES]"
dumpsys SurfaceFlinger 2>&1 | grep -iE "GLES:|Vulkan|Display|refresh" | head -n 15
echo "[settings secure android_id & bluetooth_address & adb_enabled]"
printf "  android_id: "; settings get secure android_id 2>&1
printf "  adb_enabled: "; settings get global adb_enabled 2>&1
printf "  development_settings_enabled: "; settings get global development_settings_enabled 2>&1
printf "  device_provisioned: "; settings get global device_provisioned 2>&1
echo "[pm list features (head 30)]"
pm list features 2>&1 | sort | head -n 30
echo "[total pm features]"
pm list features 2>&1 | wc -l
