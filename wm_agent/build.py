#!/usr/bin/env python3
"""Build wm_agent/libsvbox_wm_jni.so (Android 14 ARM64) and wm_agent/svbox-wm-agent.jar (DEX)."""
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
SYSROOT = os.path.join(REPO, "gpu", "ndk_sysroot", "sysroot")
CLANG_RES = os.path.join(REPO, "gpu", "ndk_sysroot", "clang_res")
R8_JAR = os.path.join(HERE, ".tools", "r8.jar")
BUILD_DIR = os.path.join(HERE, "build")

def run(cmd, **kw):
    print("+", " ".join(cmd))
    subprocess.run(cmd, check=True, **kw)

def build_jni():
    ohos_llvm = "/data/service/hnp/hmos-clt.org/hmos-clt_1.0.0/sdk/default/openharmony/native/llvm/bin/clang++"
    clangxx = ohos_llvm if os.path.exists(ohos_llvm) else (shutil.which("clang++") or "clang++")
    res_dir = os.path.join(CLANG_RES, "14.0.7")
    out_so = os.path.join(HERE, "libsvbox_wm_jni.so")
    src = os.path.join(HERE, "libsvbox_wm_jni.cpp")
    lib_dir = os.path.join(SYSROOT, "usr", "lib", "aarch64-linux-android", "30")
    cmd = [
        clangxx,
        "--target=aarch64-linux-android30",
        f"--sysroot={SYSROOT}",
        f"-resource-dir={res_dir}",
        "-stdlib=libc++",
        "-std=c++17", "-O2", "-shared", "-fPIC",
        "-fuse-ld=lld",
        f"-L{lib_dir}",
        f"-L{os.path.join(SYSROOT, 'usr', 'lib', 'aarch64-linux-android')}",
        "-Wl,--build-id=sha1",
        "-static-libstdc++",
        src,
        "-o", out_so,
        "-lmediandk", "-landroid", "-lnativewindow", "-llog",
    ]
    run(cmd)
    strip = "/data/service/hnp/hmos-clt.org/hmos-clt_1.0.0/sdk/default/openharmony/native/llvm/bin/llvm-strip"
    if os.path.exists(strip):
        run([strip, "--strip-unneeded", out_so])
    print(f"Built {out_so} ({os.path.getsize(out_so)} bytes)")

def build_dex():
    if not os.path.exists(R8_JAR):
        raise FileNotFoundError(f"Missing {R8_JAR}")
    classes_dir = os.path.join(BUILD_DIR, "classes")
    stubs_dir = os.path.join(BUILD_DIR, "stubs_classes")
    if os.path.exists(BUILD_DIR):
        shutil.rmtree(BUILD_DIR)
    os.makedirs(classes_dir)
    os.makedirs(stubs_dir)

    stub_files = []
    stubs_root = os.path.join(HERE, "stubs")
    for root, _, files in os.walk(stubs_root):
        for f in files:
            if f.endswith(".java"):
                stub_files.append(os.path.join(root, f))

    javac = shutil.which("javac") or "/storage/Users/currentUser/.harmonybrew/bin/javac"
    java = shutil.which("java") or "/storage/Users/currentUser/.harmonybrew/bin/java"

    if stub_files:
        run([javac, "--release", "11", "-d", stubs_dir] + stub_files)

    src_java = os.path.join(HERE, "SvboxWmAgent.java")
    run([javac, "--release", "11", "-cp", stubs_dir, "-d", classes_dir, src_java])

    class_files = []
    for root, _, files in os.walk(classes_dir):
        for f in files:
            if f.endswith(".class"):
                class_files.append(os.path.join(root, f))

    out_jar = os.path.join(HERE, "svbox-wm-agent.jar")
    if os.path.exists(out_jar):
        os.remove(out_jar)
    run([
        java, "-cp", R8_JAR, "com.android.tools.r8.D8",
        "--min-api", "29",
        "--output", out_jar,
    ] + class_files)
    print(f"Built {out_jar} ({os.path.getsize(out_jar)} bytes)")

if __name__ == "__main__":
    build_jni()
    build_dex()
