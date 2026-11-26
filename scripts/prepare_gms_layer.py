#!/usr/bin/env python3
"""Assemble a lean, toggleable Android 14 ARM64 GMS read-only overlay layer (gms_layer14/)
from an official MindTheGapps 14.0.0-arm64 release archive without modifying rootfs14/.

Usage:
  python3 scripts/prepare_gms_layer.py [path/to/MindTheGapps-14.0.0-arm64.zip] [output_dir]
"""
import os
import shutil
import sys
import zipfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
DEFAULT_ZIP = REPO / "gms_cache" / "MindTheGapps-14.0.0-arm64.zip"
DEFAULT_OUT = REPO / "gms_layer14"

# Core GMS + Google Play packages needed for Play Store, Play Services, GSF, Account Sign-In.
# Excludes SetupWizard (which hijacks container launcher) and heavy Google Search/Assistant bloat (Velvet/Titan).
KEEP_APPS = {
    "priv-app/GmsCore",
    "priv-app/Phonesky",
    "priv-app/GoogleServicesFramework",
    "priv-app/GooglePartnerSetup",
    "priv-app/GoogleFeedback",
    "app/GoogleContactsSyncAdapter",
    "app/GoogleCalendarSyncAdapter",
}

KEEP_OVERLAYS = {
    "GmsOverlay.apk",
    "GmsSettingsOverlay.apk",
    "GmsSettingsProviderOverlay.apk",
}


def map_dest_rel(name: str) -> str | None:
    """Map archive paths under system/product/* and system/system_ext/* into system/* so
    svbox's --lower union stack exposes every APK and XML cleanly without being shadowed by
    direct /product or /system_ext bind mounts."""
    if name.startswith("system/product/"):
        sub = name[len("system/product/"):]
    elif name.startswith("system/system_ext/"):
        sub = name[len("system/system_ext/"):]
    elif name.startswith("system/"):
        sub = name[len("system/"):]
    else:
        return None

    parts = sub.split("/")
    if len(parts) >= 2 and parts[0] in ("priv-app", "app"):
        app_key = f"{parts[0]}/{parts[1]}"
        if app_key not in KEEP_APPS:
            return None
        return "system/" + sub

    if len(parts) >= 2 and parts[0] == "overlay":
        if parts[-1] not in KEEP_OVERLAYS:
            return None
        return "system/" + sub

    if parts[0] == "etc" and len(parts) >= 3:
        if parts[1] in ("permissions", "default-permissions", "sysconfig", "security"):
            return "system/" + sub
        return None

    if parts[0] in ("framework", "lib64"):
        return "system/" + sub

    return None


def main() -> int:
    zip_path = Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_ZIP
    out_dir = Path(sys.argv[2]) if len(sys.argv) > 2 else DEFAULT_OUT

    if not zip_path.is_file():
        print(f"ERROR: MindTheGapps archive not found: {zip_path}", file=sys.stderr)
        return 1

    if out_dir.exists():
        shutil.rmtree(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    extracted_files = 0
    total_bytes = 0
    with zipfile.ZipFile(zip_path, "r") as zf:
        for info in zf.infolist():
            if info.is_dir():
                continue
            rel = map_dest_rel(info.filename)
            if not rel:
                continue
            dst = out_dir / rel
            dst.parent.mkdir(parents=True, exist_ok=True)
            data = zf.read(info.filename)
            if rel.endswith(".xml"):
                # Rewrite /product/framework -> /system/framework if referenced
                text = data.decode("utf-8", errors="ignore")
                text = text.replace("/product/framework/", "/system/framework/")
                text = text.replace("/system_ext/framework/", "/system/framework/")
                data = text.encode("utf-8")
            dst.write_bytes(data)
            os.chmod(dst, 0o644)
            extracted_files += 1
            total_bytes += len(data)

    # Ensure standard directory permissions
    for root, dirs, _ in os.walk(out_dir):
        os.chmod(root, 0o755)
        for d in dirs:
            os.chmod(os.path.join(root, d), 0o755)

    # Write marker metadata for runtime/UI inspection
    (out_dir / ".gms_ready").write_text(
        f"source={zip_path.name}\nfiles={extracted_files}\nbytes={total_bytes}\n",
        encoding="utf-8",
    )
    print(f"Assembled {out_dir}: {extracted_files} files, {total_bytes / (1024 * 1024):.1f} MiB")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
