#!/usr/bin/env python3
"""Package svbox, svbox-binderd, and svbox-propd into a HarmonyOS PC private HNP."""
import json
import os
import sys
import zipfile


def main() -> None:
    repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    svbox_bin = sys.argv[1] if len(sys.argv) > 1 else "/storage/Users/currentUser/Workspace/svbox/build/svbox"
    out_path = os.path.join(repo, "hap", "hnp", "arm64-v8a", "svbox.hnp")
    os.makedirs(os.path.dirname(out_path), exist_ok=True)

    cfg = {
        "type": "hnp-config",
        "name": "svbox",
        "version": "0.1.0",
        "install": {
            "links": [
                {"source": "bin/svbox", "target": "svbox"},
                {"source": "bin/svbox-binderd", "target": "svbox-binderd"},
                {"source": "bin/svbox-propd", "target": "svbox-propd"},
                {"source": "bin/svbox-netd", "target": "svbox-netd"},
            ]
        },
    }

    bins = [
        (svbox_bin, "svbox_0.1.0/bin/svbox"),
        (os.path.join(repo, "binderd", "svbox-binderd"), "svbox_0.1.0/bin/svbox-binderd"),
        (os.path.join(repo, "propd", "svbox-propd"), "svbox_0.1.0/bin/svbox-propd"),
        (os.path.join(repo, "netd", "svbox-netd"), "svbox_0.1.0/bin/svbox-netd"),
    ]

    with zipfile.ZipFile(out_path, "w", zipfile.ZIP_DEFLATED) as zf:
        for d in ("svbox_0.1.0/", "svbox_0.1.0/bin/"):
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
