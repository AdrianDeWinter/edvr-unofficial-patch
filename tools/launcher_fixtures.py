#!/usr/bin/env python3
"""Write the fixture folder tools\\launcher_test reads.

  python tools\\launcher_fixtures.py --out DIR     write the fixtures into DIR
  python tools\\launcher_fixtures.py --out DIR --dry-run   say what it would do; write nothing
  python tools\\launcher_fixtures.py --self-test   write them to a temp folder and check them

Everything is signed with the RFC 6979 test key from sign_manifest.py, whose
public half is written to test_key.txt for the rig to trust. The launcher's real
keys (update_keys.h) are never used and no private key is read: the test key is
published in RFC 6979 itself and signs nothing real.

What the rig gets:

  test_key.txt, other_key.txt   public keys (128 hex): the trusted one and a stranger
  signing.txt                   name, manifest file, signature file, expected verdict
  shapes.txt                    name and the verdict of sign_manifest.load_manifest on
                                shapes\\<name>.json (signed by the test key as
                                shapes\\<name>.sig): the C++ validator must agree on every
                                line, which is how the two cannot drift apart
  inflate\\inflate.zip + entries.txt   every deflate block kind, empty, 5 MB random and 5 MB
                                compressible entries, with each entry's size and sha256
  inflate\\bomb.zip              an entry that records 1000 bytes and inflates to 100 MB
  releases\\<name>\\              release folders (manifest, signature, assets, launcher)

Writes only inside --out, which it empties first. It empties only a folder it wrote
itself (the .launcher-fixtures marker in it, or, from before the marker, all of
test_key.txt, other_key.txt, signing.txt and shapes.txt), an empty folder, or a
path that does not exist yet. Any other folder, a link, or a file is refused and
left exactly as it was.
"""
import argparse
import copy
import hashlib
import io
import json
import os
import random
import shutil
import struct
import sys
import tempfile
import zipfile
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import package_native as pn  # noqa: E402
import sign_manifest as sm  # noqa: E402

TEST_PRIVATE = int(sm.RFC_KEY, 16)
OTHER_PRIVATE = int(sm.OTHER_KEY, 16)
TEST_PUBLIC = sm.p256_public(TEST_PRIVATE).hex()
OTHER_PUBLIC = sm.p256_public(OTHER_PRIVATE).hex()


def sign_text(data, private=TEST_PRIVATE):
    return sm.p256_sign(private, data).hex() + "\n"


def write(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data if isinstance(data, bytes) else data.encode("ascii"))


# ---- zips, written by hand so each stream's block kind is chosen ---------------

def raw_deflate(data, level=6, strategy=zlib.Z_DEFAULT_STRATEGY):
    comp = zlib.compressobj(level, zlib.DEFLATED, -15, 8, strategy)
    return comp.compress(data) + comp.flush()


def build_zip(entries):
    """entries: (name, method, compressed bytes, recorded size, crc). Returns the zip bytes."""
    body, central = bytearray(), bytearray()
    for name, method, compressed, size, crc in entries:
        encoded = name.encode("ascii")
        offset = len(body)
        body += struct.pack("<IHHHHHIIIHH", 0x04034b50, 20, 0, method, 0, 0x21, crc, len(compressed), size,
                            len(encoded), 0) + encoded + compressed
        central += struct.pack("<IHHHHHHIIIHHHHHII", 0x02014b50, 20, 20, 0, method, 0, 0x21, crc,
                               len(compressed), size, len(encoded), 0, 0, 0, 0, 0, offset) + encoded
    end = struct.pack("<IHHHHIIH", 0x06054b50, 0, 0, len(entries), len(entries), len(central), len(body), 0)
    return bytes(body) + bytes(central) + end


def entry(name, raw, method, compressed=None):
    return (name, method, raw if compressed is None else compressed, len(raw), zlib.crc32(raw) & 0xffffffff)


def words(seed, count):
    rng = random.Random(seed)
    vocabulary = ["".join(rng.choice("abcdefghijklmnopqrstuvwxyz") for _ in range(rng.randint(2, 9)))
                  for _ in range(400)]
    return " ".join(rng.choice(vocabulary) for _ in range(count)).encode("ascii")


def first_block_type(compressed):
    return (compressed[0] >> 1) & 3 if compressed else None


def inflate_entries():
    rng = random.Random(11)
    stored = bytes(rng.getrandbits(8) for _ in range(300))
    text_fixed = words(1, 400)
    text_dynamic = words(2, 40000)
    random_level0 = rng.randbytes(70000)
    random_5mb = rng.randbytes(5 << 20)
    pattern = words(3, 2000)
    compressible = (pattern * ((5 << 20) // len(pattern) + 1))[: 5 << 20]
    return [
        entry("stored.bin", stored, 0),
        entry("empty-stored.bin", b"", 0),
        entry("empty-deflate.bin", b"", 8, b"\x03\x00"),
        entry("fixed.txt", text_fixed, 8, raw_deflate(text_fixed, 9, zlib.Z_FIXED)),
        entry("dynamic.txt", text_dynamic, 8, raw_deflate(text_dynamic, 9)),
        entry("level0.bin", random_level0, 8, raw_deflate(random_level0, 0)),
        entry("random-5mb.bin", random_5mb, 8, raw_deflate(random_5mb, 6)),
        entry("compressible-5mb.bin", compressible, 8, raw_deflate(compressible, 9)),
    ]


def write_inflate(out):
    entries = inflate_entries()
    write(out / "inflate" / "inflate.zip", build_zip(entries))
    lines = []
    raw_by_name = {}
    for name, method, compressed, size, crc in entries:
        # Recover the raw bytes through zlib to record their hash (also proves the stream).
        raw = compressed if method == 0 else zlib.decompress(compressed, -15)
        assert len(raw) == size
        raw_by_name[name] = raw
        kind = "stored" if method == 0 else {0: "block0", 1: "block1", 2: "block2"}.get(first_block_type(compressed), "?")
        lines.append("%s\t%d\t%s\t%s" % (name, size, hashlib.sha256(raw).hexdigest(), kind))
    write(out / "inflate" / "entries.txt", "\n".join(lines) + "\n")
    # The bomb records 1000 bytes and its stream inflates to 100 MB of zeros.
    bomb_raw = bytes(100 << 20)
    bomb = raw_deflate(bomb_raw, 9)
    write(out / "inflate" / "bomb.zip", build_zip([("bomb.bin", 8, bomb, 1000, zlib.crc32(bomb_raw[:1000]) & 0xffffffff)]))
    return entries


# ---- signing cases -------------------------------------------------------------

def good_manifest():
    manifest, zip_bytes = sm._fixture_manifest()
    return manifest, zip_bytes


def write_signing(out):
    manifest, _ = good_manifest()
    data = sm._dump(manifest)
    good_sig = sign_text(data)
    folder = out / "signing"
    rows = []

    def case(name, manifest_bytes, sig_text, expect):
        write(folder / (name + ".json"), manifest_bytes)
        write(folder / (name + ".sig"), sig_text)
        rows.append("%s\t%s.json\t%s.sig\t%s" % (name, name, name, expect))

    case("good", data, good_sig, "accept")
    case("good-crlf", data, good_sig[:-1] + "\r\n", "accept")
    case("good-no-newline", data, good_sig[:-1], "accept")
    tampered = data.replace(b"1794000000", b"1794000001")
    assert tampered != data and len(tampered) == len(data)
    case("manifest-byte-flipped", tampered, good_sig, "refuse")
    case("manifest-appended", data + b" ", good_sig, "refuse")
    raw_sig = good_sig.strip()
    for index in (0, 31, 32, 63, 64, 127):
        flipped = list(raw_sig)
        flipped[index] = "0" if flipped[index] != "0" else "1"
        case("sig-flipped-%d" % index, data, "".join(flipped) + "\n", "refuse")
    case("sig-uppercase", data, raw_sig.upper() + "\n", "refuse")
    case("sig-short", data, raw_sig[:-1] + "\n", "refuse")
    case("sig-long", data, raw_sig + "0\n", "refuse")
    case("sig-two-newlines", data, raw_sig + "\n\n", "refuse")
    case("sig-leading-space", data, " " + raw_sig + "\n", "refuse")
    case("sig-non-hex", data, "g" + raw_sig[1:] + "\n", "refuse")
    case("sig-empty", data, "", "refuse")
    case("sig-zero", data, "0" * 128 + "\n", "refuse")
    case("sig-r-is-n", data, ("%064x" % sm.N) + raw_sig[64:] + "\n", "refuse")
    case("sig-other-message", data, sign_text(data + b" "), "refuse")
    case("unknown-key", data, sign_text(data, OTHER_PRIVATE), "refuse")
    write(out / "signing.txt", "\n".join(rows) + "\n")
    return rows


# ---- schema shapes -------------------------------------------------------------

def package_table_manifest():
    """A manifest of nothing but package_native.FILES, with one zip per group: the real table must pass."""
    manifest = copy.deepcopy(good_manifest()[0])
    manifest["assets"] = [{"name": group + ".zip", "sha256": "0" * 64, "size": 1} for group in ("core", "dlss", "xess")]
    manifest["files"] = [pn.manifest_file_entry(f, f.group + ".zip", b"x") for f in pn.FILES]
    return manifest


def shape_cases():
    base, _ = good_manifest()
    launcher = {"version": "0.1.0", "asset": "ed-up-launcher.exe", "sha256": "0" * 64, "size": 1}
    cases = []

    def add(name, data):
        cases.append((name, data if isinstance(data, bytes) else data.encode("ascii")))

    def mutate(name, change):
        clone = copy.deepcopy(base)
        change(clone)
        add(name, sm._dump(clone))

    def second(**changes):
        """A change that appends a copy of files[0] (graphics) as another id, with `changes`."""
        def change(m):
            m["files"].append(dict(copy.deepcopy(m["files"][0]), id="other", **changes))
        return change

    def with_target(root, path):
        """Appends a replace file (id other) with this target, to test target rules on a file with no name of its own."""
        def change(m):
            m["files"].append(dict(copy.deepcopy(m["files"][0]), id="other", existing="replace",
                                   target={"root": root, "path": path}))
        return change

    text = sm._dump(base).decode("ascii")
    add("valid", sm._dump(base))
    add("valid-launcher", sm._dump(dict(base, launcher=launcher)))
    assert '"size": 8,\n' in text
    add("valid-minus-zero-size", text.replace('"size": 8,\n', '"size": -0,\n', 1))
    add("valid-escaped-version", text.replace('"version": "1.0.0"', '"version": "\\u0031.0.0"', 1))
    add("valid-escaped-path", text.replace('"path": "d3d11.dll"', '"path": "\\u0064\\u0033d11.dll"', 1))
    add("valid-compact", json.dumps(base, separators=(",", ":")))
    add("valid-package-table", sm._dump(package_table_manifest()))

    # schema: read alone first, so a newer one is refused whatever else the file holds.
    mutate("schema-1", lambda m: m.update(schema=1))
    mutate("schema-0", lambda m: m.update(schema=0))
    mutate("schema-3", lambda m: m.update(schema=3))
    mutate("schema-4", lambda m: m.update(schema=4))
    add("schema-3-junk-body", '{"schema": 3, "anything": [1, 2, {"x": null}]}')
    add("schema-3-bare", '{"schema": 3}')
    mutate("schema-true", lambda m: m.update(schema=True))
    mutate("schema-float", lambda m: m.update(schema=2.0))
    mutate("schema-text", lambda m: m.update(schema="2"))
    mutate("schema-null", lambda m: m.update(schema=None))
    mutate("schema-missing", lambda m: m.pop("schema"))
    mutate("schema-negative", lambda m: m.update(schema=-2))
    mutate("min-launcher-present", lambda m: m.update(min_launcher="0.1.0"))

    for key in ("version", "build", "tag"):
        mutate(key + "-number", lambda m, key=key: m.update({key: 3}))
        mutate(key + "-empty", lambda m, key=key: m.update({key: ""}))
        mutate(key + "-missing", lambda m, key=key: m.pop(key))
        mutate(key + "-null", lambda m, key=key: m.update({key: None}))
    for index, bad in enumerate((0, -1, "1", 1.5, True, None, 1794000000.0)):
        mutate("issued-%d" % index, lambda m, bad=bad: m.update(issued=bad))
    mutate("issued-missing", lambda m: m.pop("issued"))
    mutate("unknown-key", lambda m: m.update(extra=1))
    mutate("launcher-null", lambda m: m.update(launcher=None))
    mutate("launcher-short", lambda m: m.update(launcher={"version": "1"}))
    mutate("launcher-extra-key", lambda m: m.update(launcher=dict(launcher, more=1)))
    mutate("launcher-sha-upper", lambda m: m.update(launcher=dict(launcher, sha256="A" * 64)))
    mutate("launcher-asset-path", lambda m: m.update(launcher=dict(launcher, asset="a/b.exe")))
    mutate("launcher-asset-dots", lambda m: m.update(launcher=dict(launcher, asset="..")))
    mutate("launcher-size-negative", lambda m: m.update(launcher=dict(launcher, size=-1)))
    mutate("launcher-version-empty", lambda m: m.update(launcher=dict(launcher, version="")))
    mutate("assets-empty", lambda m: m.update(assets=[]))
    mutate("assets-missing", lambda m: m.pop("assets"))
    mutate("assets-not-list", lambda m: m.update(assets={}))
    mutate("files-empty", lambda m: m.update(files=[]))
    mutate("files-missing", lambda m: m.pop("files"))
    mutate("files-not-list", lambda m: m.update(files={}))
    mutate("files-entry-not-object", lambda m: m["files"].append("graphics"))
    mutate("asset-extra-key", lambda m: m["assets"][0].update(more=1))
    mutate("asset-missing-size", lambda m: m["assets"][0].pop("size"))
    mutate("asset-sha-short", lambda m: m["assets"][0].update(sha256="ab"))
    mutate("asset-sha-upper", lambda m: m["assets"][0].update(sha256="A" * 64))
    mutate("asset-size-text", lambda m: m["assets"][0].update(size="3"))
    mutate("asset-size-negative", lambda m: m["assets"][0].update(size=-5))
    mutate("file-sha-not-hex", lambda m: m["files"][0].update(sha256="g" * 64))
    mutate("file-size-negative", lambda m: m["files"][0].update(size=-1))
    mutate("file-size-float", lambda m: m["files"][0].update(size=1.5))
    mutate("file-extra-key", lambda m: m["files"][0].update(more=1))
    for index, bad in enumerate(("a b.zip", "a/b.zip", "a\\b.zip", "", "..", ".", "...", "\u00e9.zip", "a.zip\n", "a:b.zip", 5)):
        mutate("asset-name-%d" % index, lambda m, bad=bad: m["assets"][0].update(name=bad))
    # files[].path: the path inside the zip, the older zip-path rule.
    for index, bad in enumerate(("", ".", "..", "/abs", "a//b", "a/./b", "a/../b", "a/", "a\\b", "C:/x", "a:b",
                                 "a\x01b", "caf\u00e9", "a\x7fb", "../x", 5, None)):
        mutate("path-%d" % index, lambda m, bad=bad: m["files"][0].update(path=bad))
    mutate("path-nested-ok", lambda m: m["files"][1].update(path="Openvr/win64/openvr_api.dll"))
    mutate("path-trailing-dot-in-zip-ok", lambda m: m["files"][0].update(path="d3d11.dll."))
    mutate("asset-duplicate", lambda m: m["assets"].append(dict(m["assets"][0])))
    mutate("file-asset-unlisted", lambda m: m["files"][0].update(asset="nowhere.zip"))
    mutate("file-asset-number", lambda m: m["files"][0].update(asset=3))

    # id
    mutate("id-missing", lambda m: m["files"][0].pop("id"))
    mutate("id-old-item-key", lambda m: (m["files"][0].pop("id"), m["files"][0].update(item="native_graphics")))
    for name, bad in (("empty", ""), ("upper", "Graphics"), ("dash", "a-b"), ("too-long", "a" * 33),
                      ("non-ascii", "caf\u00e9"), ("number", 5), ("null", None), ("space", "a b")):
        mutate("id-" + name, lambda m, bad=bad: m["files"][0].update(id=bad))
    mutate("id-longest-ok", lambda m: m["files"][0].update(id="a" * 32))
    mutate("id-digits-underscore-ok", lambda m: m["files"][0].update(id="_0_9_"))
    mutate("id-duplicate", lambda m: m["files"].append(dict(
        copy.deepcopy(m["files"][0]), target={"root": "game", "path": "other.dll"}, existing="replace")))

    # target
    mutate("target-missing", lambda m: m["files"][0].pop("target"))
    mutate("target-not-object", lambda m: m["files"][0].update(target="d3d11.dll"))
    mutate("target-null", lambda m: m["files"][0].update(target=None))
    mutate("target-extra-key", lambda m: m["files"][0]["target"].update(mode="x"))
    mutate("target-root-settings", lambda m: m["files"][0]["target"].update(root="settings"))
    mutate("target-root-upper", lambda m: m["files"][0]["target"].update(root="Game"))
    mutate("target-root-missing", lambda m: m["files"][0]["target"].pop("root"))
    mutate("target-path-missing", lambda m: m["files"][0]["target"].pop("path"))
    for name, good in (("nested", "sub/dir/File-1.dll"), ("console", "console.txt"), ("com10", "com10.dll"),
                       ("nul-prefix", "nullx.dll"), ("double-ext", "x.dll.bak"), ("dot-inside", "a.b/c.d"),
                       ("com-prefix-dir", "com1x/a.dll")):
        mutate("target-path-ok-" + name, with_target("game", good))
    for name, bad in (("empty", ""), ("dot", "."), ("dotdot", ".."), ("dotdot-lead", "../x.dll"),
                      ("dotdot-mid", "a/../x.dll"), ("absolute", "/abs.dll"), ("drive", "C:/x.dll"),
                      ("drive-relative", "C:x.dll"), ("colon", "a:b"), ("empty-segment", "a//b"),
                      ("dot-segment", "a/./b"), ("backslash", "a\\b"), ("nul", "NUL"), ("nul-lower-ext", "nul.txt"),
                      ("con-txt", "con.txt"), ("con-mixed", "Con"), ("com1", "COM1"), ("com0", "com0"),
                      ("lpt9", "lpt9.dll"), ("aux-dir", "a/AUX/b"), ("aux-multi-ext", "aux.tar.gz"),
                      ("prn", "prn.dll"), ("ads", "x.dll:stream"), ("trailing-dot", "x.dll."),
                      ("trailing-space", "x.dll "), ("dir-trailing-dot", "a./b"), ("dir-trailing-space", "a /b"),
                      ("trailing-slash", "a/"), ("control", "a\x01b"), ("non-ascii", "caf\u00e9"),
                      ("star", "a*b"), ("question", "a?b"), ("quote", 'a"b'), ("less", "a<b"), ("greater", "a>b"),
                      ("pipe", "a|b"), ("number", 5), ("null", None)):
        mutate("target-path-" + name, with_target("game", bad))

    # when
    mutate("when-absent", lambda m: m["files"][0].pop("when"))
    mutate("when-profile-vr", lambda m: m["files"][0].update(when={"profile": ["vr"]}))
    mutate("when-profile-flat", lambda m: m["files"][0].update(when={"profile": ["flat"]}))
    mutate("when-profile-both", lambda m: m["files"][0].update(when={"profile": ["flat", "vr"]}))
    mutate("when-adapter", lambda m: m["files"][0].update(when={"adapter": "nvidia"}))
    mutate("when-both-keys", lambda m: m["files"][0].update(when={"profile": ["flat"], "adapter": "nvidia"}))
    for name, bad in (("empty", {}), ("list", []), ("null", None), ("text", "vr"), ("route", {"route": "x"}),
                      ("option", {"profile": ["vr"], "option": "x"}), ("profile-empty", {"profile": []}),
                      ("profile-text", {"profile": "vr"}), ("profile-unknown", {"profile": ["both"]}),
                      ("profile-dup", {"profile": ["vr", "vr"]}), ("profile-upper", {"profile": ["VR"]}),
                      ("profile-number", {"profile": [1]}), ("adapter-amd", {"adapter": "amd"}),
                      ("adapter-true", {"adapter": True}), ("adapter-list", {"adapter": ["nvidia"]})):
        mutate("when-" + name, lambda m, bad=bad: m["files"][0].update(when=bad))

    # existing, and the constraints each behaviour carries
    mutate("existing-replace", lambda m: m["files"][0].update(existing="replace"))
    mutate("existing-owned", lambda m: m["files"][0].update(existing="owned"))
    mutate("existing-chain", lambda m: m["files"][0].update(existing="chain"))
    mutate("existing-runtime", lambda m: m["files"][1].update(existing="runtime"))
    mutate("existing-settings", lambda m: m["files"][2].update(existing="settings"))
    mutate("existing-missing", lambda m: m["files"][0].pop("existing"))
    for name, bad in (("unknown", "merge"), ("upper", "Replace"), ("empty", ""), ("number", 1), ("null", None),
                      ("list", ["replace"])):
        mutate("existing-" + name, lambda m, bad=bad: m["files"][0].update(existing=bad))
    mutate("constraint-second-chain", second(target={"root": "game", "path": "other.dll"}, existing="chain"))
    mutate("constraint-second-runtime", lambda m: m["files"].append(dict(
        copy.deepcopy(m["files"][1]), id="other", target={"root": "openvr", "path": "other.dll"})))
    mutate("constraint-runtime-in-game-root", lambda m: m["files"][1]["target"].update(root="game"))
    mutate("constraint-settings-no-when", lambda m: m["files"][2].pop("when"))
    mutate("constraint-settings-both-profiles", lambda m: m["files"][2].update(when={"profile": ["vr", "flat"]}))
    mutate("constraint-settings-adapter-only", lambda m: m["files"][2].update(when={"adapter": "nvidia"}))
    mutate("constraint-settings-profile-with-adapter-ok", lambda m: m["files"][2].update(
        when={"profile": ["vr"], "adapter": "nvidia"}))
    mutate("constraint-second-settings-same-profile", lambda m: m["files"].append(dict(
        copy.deepcopy(m["files"][2]), id="other", target={"root": "game", "path": "other.ini"})))
    mutate("constraint-settings-per-profile-ok", lambda m: m["files"].append(dict(
        copy.deepcopy(m["files"][2]), id="settings_flat", when={"profile": ["flat"]},
        target={"root": "game", "path": "edvr-flat.ini"})))

    # chain and runtime work at today's names only
    mutate("constraint-chain-other-name", lambda m: m["files"][0]["target"].update(path="other.dll"))
    mutate("constraint-chain-in-openvr-root", lambda m: m["files"][0]["target"].update(root="openvr"))
    mutate("constraint-chain-nested", lambda m: m["files"][0]["target"].update(path="sub/d3d11.dll"))
    mutate("constraint-chain-name-case-ok", lambda m: m["files"][0]["target"].update(path="D3D11.DLL"))
    mutate("constraint-runtime-other-name", lambda m: m["files"][1]["target"].update(path="other.dll"))
    mutate("constraint-runtime-nested", lambda m: m["files"][1]["target"].update(path="sub/openvr_api.dll"))
    mutate("constraint-runtime-name-case-ok", lambda m: m["files"][1]["target"].update(path="OpenVR_API.dll"))

    # a game target may not alias the openvr root, and the installer's own names and folders are reserved
    for name, root, path, ok in (
            ("game-under-openvr", "game", "openvr/x.dll", False), ("game-under-openvr-win64", "game", "Openvr/win64/x.dll", False),
            ("game-under-openvr-upper", "game", "OPENVR/x.dll", False), ("game-openvr-sibling-ok", "game", "openvr-extra/x.dll", True),
            ("game-openvr-prefix-ok", "game", "openvrx.dll", True), ("game-openvr-leaf-ok", "game", "openvr", True),
            ("openvr-root-openvr-folder-ok", "openvr", "openvr/x.dll", True),
            ("profile-descriptor", "game", "edvr_profile.ini", False), ("profile-descriptor-case", "game", "EDVR_Profile.INI", False),
            ("startup-config", "game", "edvr_openxr.ini", False), ("startup-config-openvr", "openvr", "edvr_openxr.ini", False),
            ("base-copy", "game", "edvr.ini.base", False), ("base-copy-nested-case", "game", "sub/y.BASE", False),
            ("openvr-original", "openvr", "openvr_api_orig.dll", False), ("openvr-original-game", "game", "openvr_api_orig.dll", False),
            ("chain-name", "game", "d3d11_edhm.dll", False), ("chain-name-case", "game", "D3D11_Other.DLL", False),
            ("chain-name-empty-star", "game", "d3d11_.dll", False),
            ("record-folder", "game", "edvr_install/x.ini", False), ("record-folder-case", "game", "Edvr_Install/x.ini", False),
            ("backup-folder", "game", "edvr_backup/a/b.dll", False), ("backup-folder-openvr-root", "openvr", "edvr_backup/b.dll", False),
            ("chain-lookalike-ok", "game", "d3d11x.dll", True), ("chain-lookalike-ext-ok", "game", "d3d11_x.dll.bak", True),
            ("chain-name-nested-ok", "game", "sub/d3d11_x.dll", True), ("descriptor-nested-ok", "game", "sub/edvr_profile.ini", True),
            ("descriptor-lookalike-ok", "game", "edvr_profile.ini2", True), ("base-lookalike-ok", "game", "x.basement", True),
            ("folder-lookalike-ok", "game", "edvr_installer/x", True), ("folder-name-only-ok", "game", "edvr_install", True)):
        assert ok == name.endswith("-ok"), name
        mutate("reserved-" + name, with_target(root, path))

    # overlapping targets
    mutate("overlap-same-path", second(existing="replace"))
    mutate("overlap-case-folded", second(existing="replace", target={"root": "game", "path": "D3D11.DLL"}))
    mutate("overlap-one-without-when", lambda m: m["files"].append(dict(
        {k: v for k, v in copy.deepcopy(m["files"][0]).items() if k != "when"}, id="other", existing="replace")))
    mutate("overlap-adapter-does-not-exclude", second(existing="replace", when={"profile": ["vr"], "adapter": "nvidia"}))
    mutate("overlap-shared-profile", lambda m: (m["files"][0].update(when={"profile": ["vr"]}, existing="replace"),
                                                 m["files"].append(dict(copy.deepcopy(m["files"][0]), id="other",
                                                                        when={"profile": ["flat", "vr"]}))))
    mutate("overlap-other-root-ok", second(existing="replace", target={"root": "openvr", "path": "d3d11.dll"}))
    mutate("overlap-other-path-ok", second(existing="replace", target={"root": "game", "path": "d3d11.dll.bak"}))
    mutate("overlap-disjoint-profiles-ok", lambda m: (m["files"][0].update(when={"profile": ["vr"]}, existing="replace"),
                                                      m["files"].append(dict(copy.deepcopy(m["files"][0]), id="other",
                                                                             when={"profile": ["flat"]}))))

    add("root-array", "[]")
    add("root-string", '"x"')
    add("root-null", "null")
    add("root-empty", "")
    add("truncated", text[:40])
    add("not-json", "{")
    add("trailing-junk", text + "x")
    add("trailing-value", text.strip() + " {}")
    add("duplicate-key", text.replace('"schema": 2,', '"schema": 2, "schema": 2,', 1))
    add("duplicate-key-nested", text.replace('"size": 8,\n', '"size": 8, "size": 8,\n', 1))
    add("duplicate-key-escaped", text.replace('"schema": 2,', '"schema": 2, "sch\\u0065ma": 2,', 1))
    add("leading-zero", text.replace('"schema": 2', '"schema": 02', 1))
    add("nan-value", text.replace('"issued": 1794000000', '"issued": NaN', 1))
    add("trailing-comma", text.replace('"schema": 2,', '"schema": 2,,', 1))
    add("single-quotes", text.replace('"schema"', "'schema'", 1))
    add("control-char-in-string", text.replace('"version": "1.0.0"', '"version": "1.\t0.0"', 1))
    add("bom", b"\xef\xbb\xbf" + sm._dump(base))
    add("non-ascii-raw", sm._dump(base).replace(b'"1.0.0"', '"\u00e9"'.encode("utf-8"), 1))
    add("lone-surrogate", text.replace('"version": "1.0.0"', '"version": "\\ud800"', 1))
    return cases


def write_shapes(out):
    rows = []
    for index, (name, data) in enumerate(shape_cases()):
        label = "%03d-%s" % (index, name)
        verdict = "accept" if not sm.load_manifest(data)[1] else "refuse"
        write(out / "shapes" / (label + ".json"), data)
        write(out / "shapes" / (label + ".sig"), sign_text(data))
        rows.append("%s\t%s" % (label, verdict))
    write(out / "shapes.txt", "\n".join(rows) + "\n")
    return rows


# ---- releases ------------------------------------------------------------------

def deterministic_zip(members):
    buffer = io.BytesIO()
    with zipfile.ZipFile(buffer, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        for name in sorted(members):
            info = zipfile.ZipInfo(name, date_time=(2020, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            archive.writestr(info, members[name])
    return buffer.getvalue()


def release(out, name, version, issued, graphics, launcher_version=None, schema=None):
    """One release folder: a core zip, a DLSS zip (identical across releases), a
    signed manifest and, if asked, a launcher entry. `schema` overrides the
    manifest's schema after it was validated as schema 2, for a release the
    launcher cannot read. Returns the folder."""
    folder = out / "releases" / name
    rng = random.Random(5)
    dlss = deterministic_zip({"nvngx_dlss.dll": rng.randbytes(60000)})
    dlss_name = "ed-up-dlss-%s.zip" % hashlib.sha256(dlss).hexdigest()[:12]
    core_members = {"d3d11.dll": graphics, "Openvr/win64/openvr_api.dll": b"runtime " + version.encode(),
                    "edvr.ini": b"[fix]\nversion=" + version.encode() + b"\n"}
    core = deterministic_zip(core_members)
    core_name = "ed-up-core-%s.zip" % version
    by_id = {f.id: f for f in pn.FILES}
    runtime = by_id["runtime"]._replace(source="Openvr/win64/openvr_api.dll")  # this fixture's zip layout
    files = [
        pn.manifest_file_entry(by_id["graphics"], core_name, graphics),
        pn.manifest_file_entry(runtime, core_name, core_members["Openvr/win64/openvr_api.dll"]),
        pn.manifest_file_entry(by_id["settings_vr"], core_name, core_members["edvr.ini"]),
    ]
    nvngx = zipfile.ZipFile(io.BytesIO(dlss)).read("nvngx_dlss.dll")
    files.append(pn.manifest_file_entry(by_id["dlss"], dlss_name, nvngx))
    manifest = {"schema": sm.MANIFEST_SCHEMA, "version": version, "build": "v" + version, "tag": "v" + version,
                "issued": issued,
                "assets": [{"name": core_name, "sha256": hashlib.sha256(core).hexdigest(), "size": len(core)},
                           {"name": dlss_name, "sha256": hashlib.sha256(dlss).hexdigest(), "size": len(dlss)}],
                "files": files}
    write(folder / core_name, core)
    write(folder / dlss_name, dlss)
    if launcher_version:
        exe = b"MZ fake launcher " + launcher_version.encode()
        manifest["launcher"] = {"version": launcher_version, "asset": "ed-up-launcher.exe",
                                "sha256": hashlib.sha256(exe).hexdigest(), "size": len(exe)}
        write(folder / "ed-up-launcher.exe", exe)
    assert not sm.validate_manifest(manifest), sm.validate_manifest(manifest)
    if schema is not None:
        manifest["schema"] = schema
    data = sm._dump(manifest)
    write(folder / sm.MANIFEST_NAME, data)
    write(folder / sm.SIGNATURE_NAME, sign_text(data))
    return folder


def write_releases(out):
    release(out, "relA", "1.0.0", 1794000000, b"graphics one")
    release(out, "relB", "1.1.0", 1794000100, b"graphics two", launcher_version="0.2.0")
    release(out, "relOld", "0.9.0", 1790000000, b"graphics old")
    release(out, "relC", "1.2.0", 1794000200, b"graphics three")
    release(out, "relD", "1.3.0", 1794000300, b"graphics four", launcher_version="0.3.0")
    # A release of a schema newer than the launcher reads: signed, with every asset present, so a
    # download would show in the rig; the launcher must refuse it before it fetches anything.
    release(out, "relSchema3", "1.4.0", 1794000400, b"graphics five", launcher_version="0.4.0", schema=3)


MARKER = ".launcher-fixtures"
# What a folder written before the marker existed always holds.
EARLIER_OUTPUTS = ("test_key.txt", "other_key.txt", "signing.txt", "shapes.txt")


def out_problem(out):
    """Why --out may not be emptied and written, or None."""
    out = Path(out)
    if os.path.islink(out) or (hasattr(out, "is_junction") and out.is_junction()):
        return "%s is a link; it is not followed or emptied" % out
    if not out.exists():
        return None
    if not out.is_dir():
        return "%s is not a folder" % out
    if (out / MARKER).is_file() or all((out / name).is_file() for name in EARLIER_OUTPUTS):
        return None
    if any(out.iterdir()):
        return "%s holds files this tool did not write (no %s in it), so it is not emptied" % (out, MARKER)
    return None


def generate(out):
    out = Path(out)
    problem = out_problem(out)
    if problem:
        raise ValueError(problem)
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    write(out / MARKER, "Written by tools\\launcher_fixtures.py, which empties this folder on its next run.\n")
    write(out / "test_key.txt", TEST_PUBLIC)
    write(out / "other_key.txt", OTHER_PUBLIC)
    write_signing(out)
    write_shapes(out)
    write_inflate(out)
    write_releases(out)


# ---- self-test -----------------------------------------------------------------

def self_test():
    assert TEST_PUBLIC not in "".join(k.hex() for k in sm.header_keys()), "the test key is in update_keys.h"
    assert OTHER_PUBLIC not in "".join(k.hex() for k in sm.header_keys())
    with tempfile.TemporaryDirectory(prefix="edvr-launcher-fixtures-") as temp:
        out = Path(temp) / "fx"
        generate(out)

        # Signing cases: the good one verifies with the test key, the others as recorded.
        rows = [line.split("\t") for line in (out / "signing.txt").read_text().splitlines()]
        names = {row[0] for row in rows}
        assert {"good", "unknown-key", "manifest-byte-flipped", "sig-uppercase"} <= names
        public = bytes.fromhex(TEST_PUBLIC)
        good = [r for r in rows if r[0] == "good"][0]
        data = (out / "signing" / good[1]).read_bytes()
        assert sm.p256_verify(public, data, bytes.fromhex((out / "signing" / good[2]).read_text().strip()))
        unknown = [r for r in rows if r[0] == "unknown-key"][0]
        assert not sm.p256_verify(public, data, bytes.fromhex((out / "signing" / unknown[2]).read_text().strip()))
        assert {r[3] for r in rows} == {"accept", "refuse"}

        # Shape cases: both verdicts are present, each file is signed, and the
        # recorded verdict is the Python validator's.
        shapes = [line.split("\t") for line in (out / "shapes.txt").read_text().splitlines()]
        assert len(shapes) == 271, len(shapes)
        verdicts = {v for _, v in shapes}
        assert verdicts == {"accept", "refuse"}, verdicts
        by_name = dict(shapes)
        assert by_name["000-valid"] == "accept" and by_name["001-valid-launcher"] == "accept"
        assert by_name["%03d-bom" % [i for i, (n, _) in enumerate(shape_cases()) if n == "bom"][0]] == "refuse"
        for label, verdict in shapes[::17]:
            body = (out / "shapes" / (label + ".json")).read_bytes()
            assert (verdict == "accept") == (not sm.load_manifest(body)[1])
            assert sm.p256_verify(public, body, bytes.fromhex((out / "shapes" / (label + ".sig")).read_text().strip()))
        names_by_verdict = {}
        for label, verdict in shapes:
            names_by_verdict.setdefault(label.split("-", 1)[1], verdict)
        # Every existing value and every when key is accepted, and the packager's own table too.
        for name in ("existing-replace", "existing-owned", "existing-chain", "existing-runtime", "existing-settings",
                     "when-profile-vr", "when-profile-flat", "when-profile-both", "when-adapter",
                     "when-both-keys", "valid-package-table", "constraint-chain-name-case-ok",
                     "constraint-runtime-name-case-ok", "reserved-game-openvr-sibling-ok", "reserved-chain-lookalike-ok",
                     "reserved-descriptor-nested-ok"):
            assert names_by_verdict[name] == "accept", name
        for name in ("schema-1", "schema-3", "schema-missing", "target-path-nul", "target-path-con-txt",
                     "target-path-ads", "target-path-trailing-dot", "target-path-dotdot", "target-path-absolute",
                     "target-path-drive", "id-duplicate", "overlap-same-path", "min-launcher-present",
                     "constraint-chain-other-name", "constraint-chain-in-openvr-root", "constraint-runtime-other-name",
                     "reserved-game-under-openvr", "reserved-profile-descriptor", "reserved-base-copy", "reserved-chain-name",
                     "reserved-record-folder", "reserved-backup-folder", "reserved-openvr-original"):
            assert names_by_verdict[name] == "refuse", name

        # Inflate fixtures: readable by Python's zipfile, hashes as recorded, all block kinds.
        recorded = [line.split("\t") for line in (out / "inflate" / "entries.txt").read_text().splitlines()]
        with zipfile.ZipFile(out / "inflate" / "inflate.zip") as archive:
            assert archive.testzip() is None
            for name, size, sha, kind in recorded:
                content = archive.read(name)
                assert len(content) == int(size) and hashlib.sha256(content).hexdigest() == sha, name
        kinds = {kind for _, _, _, kind in recorded}
        assert {"stored", "block0", "block1", "block2"} <= kinds, kinds
        assert {r[0] for r in recorded} >= {"empty-stored.bin", "empty-deflate.bin", "random-5mb.bin", "compressible-5mb.bin"}
        sizes = {r[0]: int(r[1]) for r in recorded}
        assert sizes["random-5mb.bin"] == sizes["compressible-5mb.bin"] == 5 << 20
        bomb = (out / "inflate" / "bomb.zip").read_bytes()
        assert len(bomb) < 1 << 20
        stream = bomb[30 + len("bomb.bin"):bomb.index(b"PK\x01\x02")]
        assert len(zlib.decompress(stream, -15)) == 100 << 20

        # Releases: every manifest validates and verifies, every asset and member matches,
        # relA and relB share the DLSS zip, the others differ as the rig expects.
        folders = {p.name: p for p in (out / "releases").iterdir()}
        assert set(folders) == {"relA", "relB", "relOld", "relC", "relD", "relSchema3"}
        for name, folder in folders.items():
            if name == "relSchema3":
                continue  # a schema the launcher cannot read: the Python verdict is a refusal, as it should be
            lines = []
            assert sm.verify_directory(folder, keys=[public], out=lines.append), (name, lines)
        s3 = (folders["relSchema3"] / sm.MANIFEST_NAME).read_bytes()
        assert json.loads(s3)["schema"] == 3 and sm.load_manifest(s3)[1] == ["manifest schema 3, launcher reads 2"]
        assert sm.p256_verify(public, s3, bytes.fromhex((folders["relSchema3"] / sm.SIGNATURE_NAME).read_text().strip()))
        assert all((folders["relSchema3"] / n).is_file() for n in
                   (a["name"] for a in json.loads(s3)["assets"])), "the schema-3 release keeps its assets, so a download would show"
        dlss = lambda f: sorted(p.name for p in f.glob("ed-up-dlss-*.zip"))
        assert dlss(folders["relA"]) == dlss(folders["relB"]) and len(dlss(folders["relA"])) == 1
        a = json.loads((folders["relA"] / sm.MANIFEST_NAME).read_text())
        b = json.loads((folders["relB"] / sm.MANIFEST_NAME).read_text())
        assert a["assets"][1] == b["assets"][1] and a["assets"][0] != b["assets"][0]
        assert "launcher" not in a and b["launcher"]["version"] == "0.2.0"
        assert b["issued"] > a["issued"] > json.loads((folders["relOld"] / sm.MANIFEST_NAME).read_text())["issued"]
        assert "min_launcher" not in json.loads((folders["relC"] / sm.MANIFEST_NAME).read_text())
        again = Path(temp) / "again"
        generate(again)
        assert (again / "releases" / "relA" / sm.MANIFEST_NAME).read_bytes() == (folders["relA"] / sm.MANIFEST_NAME).read_bytes(), "not deterministic"

        # --out: a folder this tool did not write is refused and left as it was, with or without --dry-run.
        stranger = Path(temp) / "stranger"
        stranger.mkdir()
        (stranger / "keep.txt").write_text("mine")
        assert main(["--out", str(stranger)]) == 2 and (stranger / "keep.txt").read_text() == "mine"
        assert main(["--out", str(stranger), "--dry-run"]) == 2 and sorted(p.name for p in stranger.iterdir()) == ["keep.txt"]
        not_a_folder = Path(temp) / "a-file"
        not_a_folder.write_text("file")
        assert main(["--out", str(not_a_folder)]) == 2 and not_a_folder.read_text() == "file"
        # --dry-run writes nothing at all: not for a new folder, not for one of its own.
        fresh = Path(temp) / "fresh"
        assert main(["--out", str(fresh), "--dry-run"]) == 0 and not fresh.exists()
        before = sorted((str(p.relative_to(again)), p.stat().st_size) for p in again.rglob("*"))
        assert main(["--out", str(again), "--dry-run"]) == 0
        assert sorted((str(p.relative_to(again)), p.stat().st_size) for p in again.rglob("*")) == before
        # Its own folder is emptied and written again; so is one from before the marker, and an empty one.
        (again / "leftover.txt").write_text("old")
        assert main(["--out", str(again)]) == 0 and not (again / "leftover.txt").exists() and (again / MARKER).is_file()
        (again / MARKER).unlink()
        assert out_problem(again) is None, "a folder from before the marker is the tool's own"
        (again / "signing.txt").unlink()
        assert out_problem(again) is not None, "without the marker and the earlier outputs it is not"
        empty = Path(temp) / "empty"
        empty.mkdir()
        assert out_problem(empty) is None
    print("launcher_fixtures: self-test passed")
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", type=Path, help="folder to write the fixtures into (emptied first, if it is the tool's own)")
    parser.add_argument("--dry-run", action="store_true", help="say what would be emptied and written; write and delete nothing")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args(argv)
    if args.self_test:
        return self_test()
    if not args.out:
        parser.print_help()
        return 2
    problem = out_problem(args.out)
    if problem:
        print("launcher_fixtures: refused: " + problem)
        return 2
    if args.dry_run:
        print("launcher_fixtures: dry run: would %s %s and write the fixtures into it; nothing was written"
              % ("empty" if args.out.exists() else "create", args.out))
        return 0
    generate(args.out)
    print("launcher_fixtures: wrote %s" % args.out)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
