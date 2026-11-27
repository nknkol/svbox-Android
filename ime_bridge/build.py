#!/usr/bin/env python3
"""Build & v2-sign ime_bridge/SvboxHostIme.apk (Android 14 headless WSA-style Host IME)."""
import hashlib
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
R8_JAR = os.path.join(REPO, "wm_agent", ".tools", "r8.jar")
TOOLS_DIR = os.path.join(HERE, ".tools")
BUILD_DIR = os.path.join(HERE, "build")
OUT_APK = os.path.join(HERE, "SvboxHostIme.apk")

ANDROID_NS = "http://schemas.android.com/apk/res/android"

# Android framework attribute resource IDs (verified against Android 14 framework)
ATTR_RES_IDS = {
    "label": 0x01010001,
    "name": 0x01010003,
    "permission": 0x01010006,
    "exported": 0x01010010,
    "resource": 0x01010025,
    "minSdkVersion": 0x0101020c,
    "versionCode": 0x0101021b,
    "versionName": 0x0101021c,
    "imeSubtypeLocale": 0x010102ec,
    "imeSubtypeMode": 0x010102ed,
    "targetSdkVersion": 0x01010270,
    "isAsciiCapable": 0x010103e9,
    "supportsSwitchingToNextInputMethod": 0x010103eb,
    "directBootAware": 0x01010505,
}

TYPE_REFERENCE = 0x01
TYPE_STRING = 0x03
TYPE_INT_DEC = 0x10
TYPE_INT_BOOLEAN = 0x12


def run(cmd, **kw):
    print("+", " ".join(cmd))
    subprocess.run(cmd, check=True, **kw)


def build_string_pool(strings):
    """Build a UTF-16LE RES_STRING_POOL_TYPE (0x0001) chunk."""
    offsets = []
    blob = bytearray()
    for s in strings:
        offsets.append(len(blob))
        u16 = s.encode("utf-16le")
        nchars = len(u16) // 2
        if nchars >= 0x8000:
            raise ValueError("String too long for simple UTF-16 length encoding")
        blob += struct.pack("<H", nchars)
        blob += u16
        blob += b"\x00\x00"
    while len(blob) % 4 != 0:
        blob += b"\x00"

    header_size = 28
    strings_start = header_size + 4 * len(strings)
    chunk_size = strings_start + len(blob)
    hdr = struct.pack(
        "<HHIIIIIII",
        0x0001,         # RES_STRING_POOL_TYPE
        header_size,
        chunk_size,
        len(strings),   # stringCount
        0,              # styleCount
        0,              # flags (UTF-16)
        strings_start,
        0,              # stylesStart
        0,              # unused placeholder? Wait - count fields!
    )
    # Wait: ResStringPool_header is 28 bytes = H(2)+H(2)+I(4) [8] + 5*I [20] = 28 bytes!
    hdr = struct.pack(
        "<HHIIIIII",
        0x0001,
        header_size,
        chunk_size,
        len(strings),
        0,
        0,
        strings_start,
        0,
    )
    off_table = b"".join(struct.pack("<I", o) for o in offsets)
    return hdr + off_table + bytes(blob)


def encode_axml(root_elem):
    """Encode a simple XML tree (tag, attrs, children) into Android Binary XML (AXML).

    Each element is (tag_name, [(ns_or_None, attr_name, value_type, value), ...], [children...]).
    """
    # Collect all resource-backed android attribute names first so indices 0..N-1 match resource map
    res_attr_names = []
    other_strings = []

    def add_str(s):
        if s in res_attr_names:
            return
        if s not in other_strings:
            other_strings.append(s)

    def walk_collect(elem):
        tag, attrs, children = elem
        for ns, aname, vtype, val in attrs:
            if ns == ANDROID_NS and aname in ATTR_RES_IDS:
                if aname not in res_attr_names:
                    res_attr_names.append(aname)
        add_str(tag)
        for ns, aname, vtype, val in attrs:
            if ns is not None:
                add_str(ns)
            if not (ns == ANDROID_NS and aname in ATTR_RES_IDS):
                add_str(aname)
            if vtype == TYPE_STRING:
                add_str(str(val))
        for ch in children:
            walk_collect(ch)

    add_str("android")
    add_str(ANDROID_NS)
    walk_collect(root_elem)

    # Sort resource attributes by resource ID (standard aapt2 order)
    res_attr_names.sort(key=lambda n: ATTR_RES_IDS[n])
    strings = res_attr_names + [s for s in other_strings if s not in res_attr_names]
    str_idx = {s: i for i, s in enumerate(strings)}

    sp_chunk = build_string_pool(strings)
    res_ids = [ATTR_RES_IDS[n] for n in res_attr_names]
    rm_chunk = struct.pack("<HHI", 0x0180, 8, 8 + 4 * len(res_ids)) + b"".join(
        struct.pack("<I", rid) for rid in res_ids
    )

    body = bytearray()
    line_no = [1]

    # START_NAMESPACE
    body += struct.pack(
        "<HHIIIIII",
        0x0100, 16, 24,
        line_no[0], 0xFFFFFFFF,
        str_idx["android"], str_idx[ANDROID_NS],
        0,
    )[:24]
    # Exact 24-byte START_NAMESPACE: H(2)+H(2)+I(4)+I(4)+I(4)+I(4)+I(4) = 24 bytes
    body = bytearray(
        struct.pack(
            "<HHIIIII",
            0x0100, 16, 24,
            line_no[0], 0xFFFFFFFF,
            str_idx["android"], str_idx[ANDROID_NS],
        )
    )

    def emit_elem(elem):
        tag, attrs, children = elem
        line_no[0] += 1
        ln = line_no[0]
        # Sort android attrs by resource id, then non-ns attrs alphabetically
        sorted_attrs = sorted(
            attrs,
            key=lambda a: (0, ATTR_RES_IDS[a[1]]) if (a[0] == ANDROID_NS and a[1] in ATTR_RES_IDS) else (1, a[1]),
        )
        chunk_sz = 36 + 20 * len(sorted_attrs)
        body.extend(
            struct.pack(
                "<HHIIIIIHHHHHH",
                0x0102, 16, chunk_sz,
                ln, 0xFFFFFFFF,
                0xFFFFFFFF, str_idx[tag],
                20, 20, len(sorted_attrs),
                0, 0, 0,
            )
        )
        for ns, aname, vtype, val in sorted_attrs:
            ns_i = str_idx[ns] if ns is not None else 0xFFFFFFFF
            name_i = str_idx[aname]
            if vtype == TYPE_STRING:
                s_i = str_idx[str(val)]
                raw_i = s_i
                data = s_i
            elif vtype == TYPE_INT_BOOLEAN:
                raw_i = 0xFFFFFFFF
                data = 0xFFFFFFFF if val else 0
            else:
                raw_i = 0xFFFFFFFF
                data = int(val) & 0xFFFFFFFF
            body.extend(
                struct.pack("<IIIHBBI", ns_i, name_i, raw_i, 8, 0, vtype, data)
            )
        for ch in children:
            emit_elem(ch)
        line_no[0] += 1
        body.extend(
            struct.pack(
                "<HHIIIII",
                0x0103, 16, 24,
                line_no[0], 0xFFFFFFFF,
                0xFFFFFFFF, str_idx[tag],
            )
        )

    emit_elem(root_elem)

    line_no[0] += 1
    body.extend(
        struct.pack(
            "<HHIIIII",
            0x0101, 16, 24,
            line_no[0], 0xFFFFFFFF,
            str_idx["android"], str_idx[ANDROID_NS],
        )
    )

    total = 8 + len(sp_chunk) + len(rm_chunk) + len(body)
    return struct.pack("<HHI", 0x0003, 8, total) + sp_chunk + rm_chunk + bytes(body)


def encode_resources_arsc(pkg_name="org.svbox.ime"):
    """Build minimal resources.arsc mapping 0x7f010000 (xml/method) -> 'res/xml/method.xml'."""
    global_pool = build_string_pool(["res/xml/method.xml"])
    type_pool = build_string_pool(["xml"])
    key_pool = build_string_pool(["method"])

    # ResTable_typeSpec (type 0x0202, header 16, size 20)
    type_spec = struct.pack("<HHIBBHI", 0x0202, 16, 20, 1, 0, 0, 1) + struct.pack("<I", 0)

    # ResTable_type (type 0x0201)
    # Header = 8 (chunk) + 12 (id,flags,res,entryCount,entriesStart) + 64 (ResTable_config) = 84 bytes
    cfg = struct.pack("<I", 64) + b"\x00" * 60
    offsets = struct.pack("<I", 0)
    # ResTable_entry (8 bytes) + Res_value (8 bytes)
    entry = struct.pack("<HHIHBBI", 8, 0, 0, 8, 0, TYPE_STRING, 0)
    entries_start = 84 + len(offsets)
    type_chunk_sz = entries_start + len(entry)
    type_chunk = (
        struct.pack("<HHIBBHII", 0x0201, 84, type_chunk_sz, 1, 0, 0, 1, entries_start)
        + cfg
        + offsets
        + entry
    )

    # ResTable_package (type 0x0200, header 288)
    pkg_u16 = pkg_name.encode("utf-16le")
    pkg_u16 = pkg_u16[:254] + b"\x00" * (256 - min(len(pkg_u16), 254))
    pkg_hdr_sz = 288
    type_strings_off = pkg_hdr_sz
    key_strings_off = pkg_hdr_sz + len(type_pool)
    pkg_total = pkg_hdr_sz + len(type_pool) + len(key_pool) + len(type_spec) + len(type_chunk)
    pkg_hdr = (
        struct.pack("<HHII", 0x0200, pkg_hdr_sz, pkg_total, 0x7F)
        + pkg_u16
        + struct.pack("<IIIII", type_strings_off, 1, key_strings_off, 1, 0)
    )
    assert len(pkg_hdr) == 288
    pkg_chunk = pkg_hdr + type_pool + key_pool + type_spec + type_chunk

    table_total = 12 + len(global_pool) + len(pkg_chunk)
    table_hdr = struct.pack("<HHII", 0x0002, 12, table_total, 1)
    return table_hdr + global_pool + pkg_chunk


def build_dex():
    if not os.path.exists(R8_JAR):
        raise FileNotFoundError(f"Missing {R8_JAR}")
    classes_dir = os.path.join(BUILD_DIR, "classes")
    stubs_dir = os.path.join(BUILD_DIR, "stubs_classes")
    dex_out = os.path.join(BUILD_DIR, "dex")
    if os.path.exists(BUILD_DIR):
        shutil.rmtree(BUILD_DIR)
    os.makedirs(classes_dir)
    os.makedirs(stubs_dir)
    os.makedirs(dex_out)

    stub_files = []
    for root, _, files in os.walk(os.path.join(HERE, "stubs")):
        for f in files:
            if f.endswith(".java"):
                stub_files.append(os.path.join(root, f))

    src_files = []
    for root, _, files in os.walk(os.path.join(HERE, "src")):
        for f in files:
            if f.endswith(".java"):
                src_files.append(os.path.join(root, f))

    javac = shutil.which("javac") or "/storage/Users/currentUser/.harmonybrew/bin/javac"
    java = shutil.which("java") or "/storage/Users/currentUser/.harmonybrew/bin/java"

    run([javac, "--release", "11", "-d", stubs_dir] + stub_files)
    run([javac, "--release", "11", "-cp", stubs_dir, "-d", classes_dir] + src_files)

    class_files = []
    for root, _, files in os.walk(classes_dir):
        for f in files:
            if f.endswith(".class"):
                class_files.append(os.path.join(root, f))

    run([
        java, "-cp", R8_JAR, "com.android.tools.r8.D8",
        "--min-api", "29",
        "--output", dex_out,
    ] + class_files)
    dex_path = os.path.join(dex_out, "classes.dex")
    with open(dex_path, "rb") as f:
        return f.read()


def ensure_signing_key():
    os.makedirs(TOOLS_DIR, exist_ok=True)
    key_pem = os.path.join(TOOLS_DIR, "svbox-ime.key.pem")
    cert_der = os.path.join(TOOLS_DIR, "svbox-ime.cert.der")
    pub_der = os.path.join(TOOLS_DIR, "svbox-ime.pub.der")
    if not (os.path.exists(key_pem) and os.path.exists(cert_der) and os.path.exists(pub_der)):
        openssl = shutil.which("openssl") or "/storage/Users/currentUser/.harmonybrew/bin/openssl"
        cert_pem = os.path.join(TOOLS_DIR, "svbox-ime.cert.pem")
        run([
            openssl, "req", "-x509", "-newkey", "rsa:2048", "-nodes",
            "-keyout", key_pem, "-out", cert_pem,
            "-days", "10000", "-subj", "/CN=Svbox Host IME/O=svbox",
        ])
        run([openssl, "x509", "-in", cert_pem, "-outform", "DER", "-out", cert_der])
        run([openssl, "rsa", "-in", key_pem, "-pubout", "-outform", "DER", "-out", pub_der])
    return key_pem, cert_der, pub_der


def len_prefixed(data: bytes) -> bytes:
    return struct.pack("<I", len(data)) + data


def compute_v2_chunked_sha256(sections):
    """Compute APK Signature Scheme v2 CHUNKED_SHA256 digest over [section1, section3, section4]."""
    chunk_digests = []
    chunk_size = 1024 * 1024
    for sec in sections:
        off = 0
        n = len(sec)
        if n == 0:
            h = hashlib.sha256()
            h.update(b"\xa5" + struct.pack("<I", 0))
            chunk_digests.append(h.digest())
            continue
        while off < n:
            part = sec[off : off + chunk_size]
            h = hashlib.sha256()
            h.update(b"\xa5" + struct.pack("<I", len(part)))
            h.update(part)
            chunk_digests.append(h.digest())
            off += len(part)
    top = hashlib.sha256()
    top.update(b"\x5a" + struct.pack("<I", len(chunk_digests)))
    for d in chunk_digests:
        top.update(d)
    return top.digest()


def sign_apk_v2(apk_bytes: bytes, key_pem: str, cert_der_path: str, pub_der_path: str) -> bytes:
    """Insert a standards-compliant APK Signature Scheme v2 Signing Block (0x7109871a)."""
    # Find EOCD (PK\x05\x06) at end of file (no zip comment)
    eocd_off = apk_bytes.rfind(b"PK\x05\x06")
    if eocd_off < 0 or len(apk_bytes) - eocd_off < 22:
        raise RuntimeError("Invalid ZIP: EOCD not found")
    cd_size, cd_offset = struct.unpack_from("<II", apk_bytes, eocd_off + 12)
    if cd_offset + cd_size != eocd_off:
        raise RuntimeError(f"Unexpected ZIP layout: cd_offset={cd_offset}, cd_size={cd_size}, eocd={eocd_off}")

    sec1 = apk_bytes[:cd_offset]
    sec3 = apk_bytes[cd_offset:eocd_off]
    sec4 = apk_bytes[eocd_off:]  # EOCD already holds cd_offset == signing_block_start!

    sig_alg_id = 0x0103  # RSA PKCS#1 v1.5 with SHA-256
    digest = compute_v2_chunked_sha256([sec1, sec3, sec4])

    with open(cert_der_path, "rb") as f:
        cert_der = f.read()
    with open(pub_der_path, "rb") as f:
        pub_der = f.read()

    digest_entry = len_prefixed(struct.pack("<I", sig_alg_id) + len_prefixed(digest))
    digests = len_prefixed(digest_entry)
    certs = len_prefixed(len_prefixed(cert_der))
    attrs = len_prefixed(b"")
    signed_data = digests + certs + attrs

    openssl = shutil.which("openssl") or "/storage/Users/currentUser/.harmonybrew/bin/openssl"
    with tempfile.NamedTemporaryFile(delete=False) as tf:
        tf.write(signed_data)
        sd_path = tf.name
    try:
        sig_bytes = subprocess.check_output([
            openssl, "dgst", "-sha256", "-sign", key_pem, sd_path
        ])
    finally:
        os.unlink(sd_path)

    sig_entry = len_prefixed(struct.pack("<I", sig_alg_id) + len_prefixed(sig_bytes))
    signatures = len_prefixed(sig_entry)
    signer = len_prefixed(signed_data) + signatures + len_prefixed(pub_der)
    v2_value = len_prefixed(len_prefixed(signer))

    # Build APK Signing Block (padded to 4096-byte multiple like apksigner)
    pair_v2 = struct.pack("<QI", 4 + len(v2_value), 0x7109871A) + v2_value
    pairs = pair_v2
    # Total block = 8 (size1) + len(pairs) + 8 (size2) + 16 (magic)
    size_of_block = len(pairs) + 24
    # Optional Android Verity/alignment padding pair (0x42726577) to align CD to 4096 bytes
    total_block = 8 + size_of_block
    rem = (cd_offset + total_block) % 4096
    if rem != 0:
        pad_needed = 4096 - rem
        if pad_needed < 12:
            pad_needed += 4096
        pad_val = b"\x00" * (pad_needed - 12)
        pairs += struct.pack("<QI", 4 + len(pad_val), 0x42726577) + pad_val
        size_of_block = len(pairs) + 24

    sig_block = (
        struct.pack("<Q", size_of_block)
        + pairs
        + struct.pack("<Q", size_of_block)
        + b"APK Sig Block 42"
    )
    new_cd_offset = cd_offset + len(sig_block)
    new_eocd = sec4[:16] + struct.pack("<I", new_cd_offset) + sec4[20:]
    return sec1 + sig_block + sec3 + new_eocd


def build_aligned_zip(entries):
    """Build a ZIP archive where ZIP_STORED entries (e.g. resources.arsc) are 4-byte aligned."""
    buf = bytearray()
    cd_records = []
    for name, data, compress in entries:
        name_b = name.encode("utf-8")
        crc = zipfile.crc32(data) & 0xFFFFFFFF
        if compress:
            import zlib
            comp = zlib.compress(data, 9)[2:-4]
            method = 8
        else:
            comp = data
            method = 0

        local_off = len(buf)
        extra = b""
        if not compress:
            # Align data start (local_off + 30 + len(name_b) + len(extra)) to 4 bytes
            data_off = local_off + 30 + len(name_b)
            pad = (4 - (data_off % 4)) % 4
            if pad:
                extra = b"\x00" * pad

        buf += struct.pack(
            "<IHHHHHIIIHH",
            0x04034B50,
            20, 0, method, 0, 0,
            crc, len(comp), len(data),
            len(name_b), len(extra),
        )
        buf += name_b + extra + comp

        cd_records.append(
            struct.pack(
                "<IHHHHHHIIIHHHHHII",
                0x02014B50,
                20, 20, 0, method, 0, 0,
                crc, len(comp), len(data),
                len(name_b), 0, 0, 0, 0, 0,
                local_off,
            ) + name_b
        )

    cd_offset = len(buf)
    cd_blob = b"".join(cd_records)
    buf += cd_blob
    buf += struct.pack(
        "<IHHHHIIH",
        0x06054B50,
        0, 0, len(cd_records), len(cd_records),
        len(cd_blob), cd_offset, 0,
    )
    return bytes(buf)


def main():
    dex_bytes = build_dex()

    manifest_tree = (
        "manifest",
        [
            (None, "package", TYPE_STRING, "org.svbox.ime"),
            (ANDROID_NS, "versionCode", TYPE_INT_DEC, 1),
            (ANDROID_NS, "versionName", TYPE_STRING, "1.0"),
        ],
        [
            (
                "uses-sdk",
                [
                    (ANDROID_NS, "minSdkVersion", TYPE_INT_DEC, 29),
                    (ANDROID_NS, "targetSdkVersion", TYPE_INT_DEC, 34),
                ],
                [],
            ),
            (
                "application",
                [
                    (ANDROID_NS, "label", TYPE_STRING, "Svbox Host IME"),
                    (ANDROID_NS, "directBootAware", TYPE_INT_BOOLEAN, True),
                ],
                [
                    (
                        "service",
                        [
                            (ANDROID_NS, "name", TYPE_STRING, ".SvboxHostImeService"),
                            (ANDROID_NS, "label", TYPE_STRING, "Svbox Host IME"),
                            (ANDROID_NS, "permission", TYPE_STRING, "android.permission.BIND_INPUT_METHOD"),
                            (ANDROID_NS, "exported", TYPE_INT_BOOLEAN, True),
                            (ANDROID_NS, "directBootAware", TYPE_INT_BOOLEAN, True),
                        ],
                        [
                            (
                                "intent-filter",
                                [],
                                [
                                    (
                                        "action",
                                        [(ANDROID_NS, "name", TYPE_STRING, "android.view.InputMethod")],
                                        [],
                                    ),
                                ],
                            ),
                            (
                                "meta-data",
                                [
                                    (ANDROID_NS, "name", TYPE_STRING, "android.view.im"),
                                    (ANDROID_NS, "resource", TYPE_REFERENCE, 0x7F010000),
                                ],
                                [],
                            ),
                        ],
                    ),
                ],
            ),
        ],
    )

    method_tree = (
        "input-method",
        [
            (ANDROID_NS, "supportsSwitchingToNextInputMethod", TYPE_INT_BOOLEAN, True),
        ],
        [
            (
                "subtype",
                [
                    (ANDROID_NS, "imeSubtypeLocale", TYPE_STRING, "en_US"),
                    (ANDROID_NS, "imeSubtypeMode", TYPE_STRING, "keyboard"),
                    (ANDROID_NS, "isAsciiCapable", TYPE_INT_BOOLEAN, True),
                ],
                [],
            ),
        ],
    )

    manifest_axml = encode_axml(manifest_tree)
    method_axml = encode_axml(method_tree)
    arsc_bytes = encode_resources_arsc("org.svbox.ime")

    unsigned_apk = build_aligned_zip([
        ("AndroidManifest.xml", manifest_axml, True),
        ("res/xml/method.xml", method_axml, True),
        ("resources.arsc", arsc_bytes, False),  # MUST be uncompressed & 4-byte aligned on Android 11+
        ("classes.dex", dex_bytes, True),
    ])

    key_pem, cert_der, pub_der = ensure_signing_key()
    signed_apk = sign_apk_v2(unsigned_apk, key_pem, cert_der, pub_der)
    with open(OUT_APK, "wb") as f:
        f.write(signed_apk)
    print(f"Built {OUT_APK} ({len(signed_apk)} bytes, APK Signature Scheme v2)")


if __name__ == "__main__":
    main()
