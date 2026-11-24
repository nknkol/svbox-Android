#!/usr/bin/env python3
"""Package svbox, svbox-binderd, and svbox-propd into a HarmonyOS PC private HNP."""
import json
import os
import sys
import zipfile


def main() -> None:
    repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    svbox_bin = sys.argv[1] if len(sys.argv) > 1 else "/storage/Users/currentUser/Workspace/svbox/build/svbox"
    svbox_net_bin = os.path.join(os.path.dirname(os.path.abspath(svbox_bin)), "svbox-net")
    out_path = os.path.join(repo, "hap", "hnp", "arm64-v8a", "svbox.hnp")
    os.makedirs(os.path.dirname(out_path), exist_ok=True)

    cfg = {
        "type": "hnp-config",
        "name": "svbox",
        "version": "0.1.0",
        "install": {
            "links": [
                {"source": "bin/svbox", "target": "svbox"},
                {"source": "bin/svbox-net", "target": "svbox-net"},
                {"source": "bin/svbox-binderd", "target": "svbox-binderd"},
                {"source": "bin/svbox-propd", "target": "svbox-propd"},
                {"source": "bin/svbox-netd", "target": "svbox-netd"},
            ]
        },
    }

    bins = [
        (svbox_bin, "svbox_0.1.0/bin/svbox"),
        (svbox_net_bin, "svbox_0.1.0/bin/svbox-net"),
        (os.path.join(repo, "binderd", "svbox-binderd"), "svbox_0.1.0/bin/svbox-binderd"),
        (os.path.join(repo, "propd", "svbox-propd"), "svbox_0.1.0/bin/svbox-propd"),
        (os.path.join(repo, "netd", "svbox-netd"), "svbox_0.1.0/bin/svbox-netd"),
        (os.path.join(repo, "audio_hal", "audio.primary.svbox.so"), "svbox_0.1.0/bin/audio.primary.svbox.so"),
        (os.path.join(repo, "audio_hal", "svbox-audio-service"), "svbox_0.1.0/bin/svbox-audio-service"),
        (os.path.join(repo, "camera_stub", "svbox-cameraserver"), "svbox_0.1.0/bin/svbox-cameraserver"),
        (os.path.join(repo, "wm_agent", "libsvbox_wm_jni.so"), "svbox_0.1.0/bin/libsvbox_wm_jni.so"),
        (os.path.join(repo, "wm_agent", "svbox-wm-agent.jar"), "svbox_0.1.0/bin/svbox-wm-agent.jar"),
    ]

    guest_gpu_dir = os.path.join(repo, "gpu", "build_guest", "out", "lib64")
    guest_gpu_files = [
        "libandroidemu.so",
        "libOpenglCodecCommon.so",
        "lib_renderControl_enc.so",
        "libGLESv1_enc.so",
        "libGLESv2_enc.so",
        "libvulkan_enc.so",
        "libOpenglSystemCommon.so",
        "libGoldfishProfiler.so",
        "egl/libEGL_emulation.so",
        "egl/libGLESv1_CM_emulation.so",
        "egl/libGLESv2_emulation.so",
        "hw/gralloc.ranchu.so",
        "hw/hwcomposer.ranchu.so",
        "hw/vulkan.ranchu.so",
    ]
    has_gpu_guest = all(os.path.exists(os.path.join(guest_gpu_dir, rel)) for rel in guest_gpu_files)

    import shutil
    import subprocess
    import tempfile

    llvm_strip = "/data/service/hnp/hmos-clt.org/hmos-clt_1.0.0/sdk/default/openharmony/native/llvm/bin/llvm-strip"
    if not os.path.exists(llvm_strip):
        llvm_strip = shutil.which("llvm-strip") or ""

    with zipfile.ZipFile(out_path, "w", zipfile.ZIP_DEFLATED) as zf:
        dirs_to_add = ["svbox_0.1.0/", "svbox_0.1.0/bin/"]
        if has_gpu_guest:
            dirs_to_add.extend([
                "svbox_0.1.0/gpu_guest/",
                "svbox_0.1.0/gpu_guest/lib64/",
                "svbox_0.1.0/gpu_guest/lib64/egl/",
                "svbox_0.1.0/gpu_guest/lib64/hw/",
            ])
        for d in dirs_to_add:
            zi = zipfile.ZipInfo(d)
            zi.external_attr = (0o040755) << 16
            zf.writestr(zi, b"")

        zi = zipfile.ZipInfo("svbox_0.1.0/hnp.json")
        zi.external_attr = (0o100644) << 16
        zf.writestr(zi, (json.dumps(cfg, indent=2) + "\n").encode("utf-8"))

        for src, dst in bins:
            if not os.path.exists(src):
                raise FileNotFoundError(f"Missing binary: {src}")
            zi = zipfile.ZipInfo(dst)
            zi.external_attr = (0o100755) << 16
            with open(src, "rb") as f:
                zf.writestr(zi, f.read())

        if has_gpu_guest:
            with tempfile.TemporaryDirectory() as td:
                for rel in guest_gpu_files:
                    src = os.path.join(guest_gpu_dir, rel)
                    dst = f"svbox_0.1.0/gpu_guest/lib64/{rel}"
                    payload_path = src
                    if llvm_strip:
                        stripped = os.path.join(td, os.path.basename(rel))
                        shutil.copy2(src, stripped)
                        subprocess.run([llvm_strip, "--strip-unneeded", stripped], check=True)
                        payload_path = stripped
                    zi = zipfile.ZipInfo(dst)
                    zi.external_attr = (0o100644) << 16
                    with open(payload_path, "rb") as f:
                        zf.writestr(zi, f.read())
            print(f"Bundled {len(guest_gpu_files)} Android 11 gfxstream guest libs into HNP")
        else:
            print("Warning: gpu/build_guest/out/lib64 not found; skipping gpu_guest libs")

    print(f"Built {out_path} ({os.path.getsize(out_path)} bytes)")

    if len(sys.argv) > 2:
        hap_path = sys.argv[2]
        if not os.path.exists(hap_path):
            raise FileNotFoundError(f"Missing HAP: {hap_path}")
        tmp_path = hap_path + ".tmp"
        with zipfile.ZipFile(hap_path, "r") as zin, zipfile.ZipFile(tmp_path, "w") as zout:
            for item in zin.infolist():
                if item.filename.startswith("hnp/"):
                    continue
                zout.writestr(item, zin.read(item.filename))
            zi_dir = zipfile.ZipInfo("hnp/arm64-v8a/")
            zi_dir.external_attr = (0o040755) << 16
            zout.writestr(zi_dir, b"")
            zi_hnp = zipfile.ZipInfo("hnp/arm64-v8a/svbox.hnp")
            zi_hnp.compress_type = zipfile.ZIP_DEFLATED
            zi_hnp.external_attr = (0o100644) << 16
            with open(out_path, "rb") as f:
                zout.writestr(zi_hnp, f.read())
        os.replace(tmp_path, hap_path)
        print(f"Injected hnp/arm64-v8a/svbox.hnp into {hap_path}")


if __name__ == "__main__":
    main()
