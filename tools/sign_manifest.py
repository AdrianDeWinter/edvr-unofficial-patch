#!/usr/bin/env python3
"""Sign and verify the release manifest, ed-up-manifest.json.

The manifest is ASCII JSON and is signed as its exact bytes: P-256 ECDSA over
the SHA-256 of the file, with a deterministic nonce (RFC 6979), written to
ed-up-manifest.json.sig as 128 lower-case hex characters (r then s) and a
newline. The launcher checks the signature before it parses anything, through
Windows' BCrypt, and trusts the public keys listed in src\\installer\\update_keys.h.
Only the Python standard library is used.

  generate            make one key and print its private scalar and public key
                      once. Writes no file.
  sign MANIFEST       sign the file, after checking the manifest and that the
                      key's public half is in update_keys.h. The key comes from
                      a hidden prompt, or from one line on stdin with --key-stdin
                      so a password manager's command line can supply it.
                      --dry-run does everything except write the .sig.
  verify DIR          check the signature, the manifest's schema and the size and
                      hash of every asset, the launcher if listed, and every file
                      inside every zip. --allow-unsigned skips only the signature.

`generate` and `sign` refuse (exit 2) unless stdout is a real terminal, so a
session whose output is captured can never see a private key. Nothing derived
from the key is ever printed, including in errors. `verify` exits 0 when every
check passes and 3 otherwise.

The private key lives in the maintainer's password manager. It is never in a
repository.
"""
import argparse
import contextlib
import getpass
import hashlib
import hmac
import io
import json
import os
import re
import secrets
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "src" / "installer" / "update_keys.h"
MANIFEST_NAME = "ed-up-manifest.json"
SIGNATURE_NAME = MANIFEST_NAME + ".sig"

# The one schema the launcher reads: kManifestSchema in src\installer\install_manifest.h, which
# the self-test reads as text and compares with this number.
MANIFEST_SCHEMA = 2
SCHEMA_HEADER = ROOT / "src" / "installer" / "install_manifest.h"

# NIST P-256.
P = 0xffffffff00000001000000000000000000000000ffffffffffffffffffffffff
A = P - 3
B = 0x5ac635d8aa3a93e7b3ebbd55769886bc651d06b0cc53b0f63bce3c3e27d2604b
N = 0xffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551
G = (0x6b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296,
     0x4fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5)

HEX64 = re.compile(r"[0-9a-f]{64}")
HEX128 = re.compile(r"[0-9a-f]{128}")
ASSET_NAME = re.compile(r"[A-Za-z0-9._-]+")
FILE_ID = re.compile(r"[a-z0-9_]{1,32}")
PROFILES = ("vr", "flat")
ROOTS = ("game", "openvr")
EXISTING = ("replace", "owned", "chain", "runtime", "settings")
# The device names Windows reserves as a path segment's stem, any case.
RESERVED_NAMES = frozenset(["con", "prn", "aux", "nul"] + ["com%d" % n for n in range(10)] +
                           ["lpt%d" % n for n in range(10)])


# ---- the curve ---------------------------------------------------------------

def _on_curve(point):
    x, y = point
    return 0 <= x < P and 0 <= y < P and (y * y - (x * x * x + A * x + B)) % P == 0


def _add(left, right):
    if left is None:
        return right
    if right is None:
        return left
    (x1, y1), (x2, y2) = left, right
    if x1 == x2:
        if (y1 + y2) % P == 0:
            return None
        slope = (3 * x1 * x1 + A) * pow(2 * y1, -1, P) % P
    else:
        slope = (y2 - y1) * pow(x2 - x1, -1, P) % P
    x3 = (slope * slope - x1 - x2) % P
    return (x3, (slope * (x1 - x3) - y1) % P)


def _mul(scalar, point):
    result = None
    while scalar:
        if scalar & 1:
            result = _add(result, point)
        point = _add(point, point)
        scalar >>= 1
    return result


def _bytes32(value):
    return value.to_bytes(32, "big")


def p256_public(priv_int):
    """The public key for a private scalar: x then y, 64 bytes."""
    if not 1 <= priv_int < N:
        raise ValueError("private key out of range")
    x, y = _mul(priv_int, G)
    return _bytes32(x) + _bytes32(y)


def _rfc6979_k(priv_int, digest):
    """The nonce of RFC 6979 section 3.2 for P-256 and SHA-256 (qlen = hlen = 256)."""
    x = _bytes32(priv_int)
    h1 = _bytes32(int.from_bytes(digest, "big") % N)
    v, k = b"\x01" * 32, b"\x00" * 32
    k = hmac.new(k, v + b"\x00" + x + h1, hashlib.sha256).digest()
    v = hmac.new(k, v, hashlib.sha256).digest()
    k = hmac.new(k, v + b"\x01" + x + h1, hashlib.sha256).digest()
    v = hmac.new(k, v, hashlib.sha256).digest()
    while True:
        v = hmac.new(k, v, hashlib.sha256).digest()
        candidate = int.from_bytes(v, "big")
        if 1 <= candidate < N:
            return candidate
        k = hmac.new(k, v + b"\x00", hashlib.sha256).digest()
        v = hmac.new(k, v, hashlib.sha256).digest()


def p256_sign(priv_int, message_bytes):
    """ECDSA over SHA-256 of the message: r then s, 64 bytes."""
    if not 1 <= priv_int < N:
        raise ValueError("private key out of range")
    digest = hashlib.sha256(message_bytes).digest()
    z = int.from_bytes(digest, "big")
    k = _rfc6979_k(priv_int, digest)
    r = _mul(k, G)[0] % N
    s = pow(k, -1, N) * (z + r * priv_int) % N
    if r == 0 or s == 0:
        raise ValueError("degenerate signature")
    return _bytes32(r) + _bytes32(s)


def p256_verify(pub_xy_bytes_64, message_bytes, sig_64):
    """True when sig_64 (r then s) signs the message under the public key.
    A key that is not 64 bytes or not on the curve, and an r or s outside
    1..n-1, are refused."""
    if len(pub_xy_bytes_64) != 64 or len(sig_64) != 64:
        return False
    point = (int.from_bytes(pub_xy_bytes_64[:32], "big"), int.from_bytes(pub_xy_bytes_64[32:], "big"))
    r, s = int.from_bytes(sig_64[:32], "big"), int.from_bytes(sig_64[32:], "big")
    if not _on_curve(point) or not 1 <= r < N or not 1 <= s < N:
        return False
    z = int.from_bytes(hashlib.sha256(message_bytes).digest(), "big")
    w = pow(s, -1, N)
    total = _add(_mul(z * w % N, G), _mul(r * w % N, point))
    return total is not None and total[0] % N == r


# ---- the keys the launcher trusts --------------------------------------------

def header_keys(header=None):
    """The public keys in update_keys.h, each as 64 bytes. Only quoted strings of
    exactly 128 hex characters count; comments are ignored."""
    text = Path(header or HEADER).read_text(encoding="utf-8")
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    text = re.sub(r"//[^\n]*", "", text)
    return [bytes.fromhex(found.lower())
            for found in re.findall(r'"([0-9a-fA-F]{128})"', text)]


# ---- the manifest ------------------------------------------------------------

def _is_int(value):
    return isinstance(value, int) and not isinstance(value, bool)


def _is_text(value):
    return isinstance(value, str) and bool(value)


def _hex_problem(where, value):
    if not isinstance(value, str) or not HEX64.fullmatch(value):
        return "%s must be 64 lower-case hex characters" % where
    return None


def _name_problem(where, value):
    if not isinstance(value, str) or not ASSET_NAME.fullmatch(value) or not value.strip("."):
        return "%s must match [A-Za-z0-9._-]+" % where
    return None


def _path_problem(where, value):
    if not _is_text(value):
        return "%s must be a non-empty string" % where
    if "\\" in value or ":" in value or any(ord(c) < 0x20 or ord(c) > 0x7e for c in value):
        return "%s has a backslash, a colon or a control character" % where
    if any(part in ("", ".", "..") for part in value.split("/")):
        return "%s must be a relative path with no empty, . or .. segment" % where
    return None


def target_path_problem(where, value):
    """Why `value` is not a legal target path (the rule of install_manifest.cpp's
    isTargetPath), or None. A backslash, a colon (drives and alternate streams), a
    control or non-ASCII character, a character Windows bars from a name, an
    empty, . or .. segment, a segment ending in a dot or a space, and a reserved
    device name (CON, PRN, AUX, NUL, COM0-9, LPT0-9, any case, with or without an
    extension) as a segment's stem are all refused."""
    if not _is_text(value):
        return "%s must be a non-empty string" % where
    if "\\" in value or ":" in value:
        return "%s has a backslash or a colon" % where
    if any(ord(c) < 0x20 or ord(c) > 0x7e for c in value):
        return "%s has a control or non-ASCII character" % where
    if any(c in '<>"|?*' for c in value):
        return "%s has a character Windows bars from a name" % where
    for part in value.split("/"):
        if part in ("", ".", ".."):
            return "%s must be a relative path with no empty, . or .. segment" % where
        if part[-1] in ". ":
            return "%s has a segment ending in a dot or a space" % where
        if part.split(".")[0].rstrip(" ").lower() in RESERVED_NAMES:
            return "%s has a segment that is a reserved device name" % where
    return None


def _shape(where, obj, keys, optional=()):
    if not isinstance(obj, dict):
        return ["%s must be an object" % where]
    problems = ["%s lacks %s" % (where, key) for key in keys if key not in obj]
    problems += ["%s has unknown key %s" % (where, key) for key in obj
                 if key not in keys and key not in optional]
    return problems


def when_can_overlap(first, second):
    """True when two (validated) `when` objects, or None for no `when`, can both
    hold on some install: a missing profile condition holds for every profile, two
    lists must share a value, and the adapter never excludes."""
    a = (first or {}).get("profile")
    b = (second or {}).get("profile")
    return a is None or b is None or bool(set(a) & set(b))


def _when_problems(where, when):
    problems = _shape(where, when, (), ("profile", "adapter"))
    if problems:
        return problems
    if not when:
        return ["%s is empty" % where]
    if "profile" in when:
        profile = when["profile"]
        if not isinstance(profile, list) or not profile:
            problems.append("%s.profile must be a non-empty list" % where)
        elif any(not isinstance(v, str) or v not in PROFILES for v in profile):
            problems.append("%s.profile holds a value that is not vr or flat" % where)
        elif len(set(profile)) != len(profile):
            problems.append("%s.profile repeats a value" % where)
    if "adapter" in when and when["adapter"] != "nvidia":
        problems.append("%s.adapter must be nvidia" % where)
    return problems


def _file_problems(index, entry, asset_names):
    """Problems with one files[] entry; the entry's id is in each message once known."""
    where = "files[%d]" % index
    sub = _shape(where, entry, ("id", "asset", "path", "sha256", "size", "target", "existing"), ("when",))
    if sub:
        return sub
    if not isinstance(entry["id"], str) or not FILE_ID.fullmatch(entry["id"]):
        return ["%s.id must match [a-z0-9_]{1,32}" % where]
    label = "file %s" % entry["id"]
    problems = []
    if not isinstance(entry["asset"], str) or entry["asset"] not in asset_names:
        problems.append("%s names no listed asset" % label)
    problem = _path_problem(label + " path", entry["path"])
    if problem:
        problems.append(problem)
    problem = _hex_problem(label + " sha256", entry["sha256"])
    if problem:
        problems.append(problem)
    if not _is_int(entry["size"]) or entry["size"] < 0:
        problems.append("%s size must be a non-negative integer" % label)
    target = entry["target"]
    sub = _shape(label + " target", target, ("root", "path"))
    problems += sub
    if not sub:
        if target["root"] not in ROOTS:
            problems.append("%s target.root must be game or openvr" % label)
        problem = target_path_problem(label + " target.path", target["path"])
        if problem:
            problems.append(problem)
    if "when" in entry:
        problems += _when_problems(label + " when", entry["when"])
    if not isinstance(entry["existing"], str) or entry["existing"] not in EXISTING:
        problems.append("%s existing must be replace, owned, chain, runtime or settings" % label)
    return problems


def _reserved_target_problem(target):
    """Why a target may not be a payload file, or None: a name or folder the installer itself writes
    (the fixed steps), or, under the game root, a path into the default Openvr folder, where it would
    alias the openvr root. Case-folded, as Windows compares names. install_manifest.cpp's
    reservedTargetReason gives the same verdict."""
    path = target["path"].lower()
    leaf = path.rsplit("/", 1)[-1]
    top_level = "/" not in path
    if target["root"] == "game" and path.startswith("openvr/"):
        return "is under the openvr folder, which the openvr root names"
    if path.startswith(("edvr_install/", "edvr_backup/")):
        return "is inside a folder the installer keeps its record and backups in"
    if top_level and path in ("edvr_profile.ini", "edvr_openxr.ini", "openvr_api_orig.dll"):
        return "is a file the installer writes itself"
    if leaf.endswith(".base"):
        return "is a .base copy the installer keeps"
    if top_level and len(path) >= 10 and path.startswith("d3d11_") and path.endswith(".dll"):
        return "is a name a chained graphics DLL is parked under"
    return None


def _cross_file_problems(files):
    """The behaviour constraints and the overlap check, over files that each passed
    _file_problems and have distinct ids."""
    problems = []
    chain = runtime = None
    settings = {}
    for entry in files:
        existing = entry["existing"]
        if existing == "chain":
            if chain is not None:
                problems.append("file %s is a second chain file (after %s)" % (entry["id"], chain))
            chain = chain or entry["id"]
            if entry["target"]["root"] != "game" or entry["target"]["path"].lower() != "d3d11.dll":
                problems.append("file %s is a chain file and must target game d3d11.dll" % entry["id"])
        elif existing == "runtime":
            if runtime is not None:
                problems.append("file %s is a second runtime file (after %s)" % (entry["id"], runtime))
            runtime = runtime or entry["id"]
            if entry["target"]["root"] != "openvr" or entry["target"]["path"].lower() != "openvr_api.dll":
                problems.append("file %s is a runtime file and must target openvr openvr_api.dll" % entry["id"])
        elif existing == "settings":
            profile = (entry.get("when") or {}).get("profile")
            if profile not in (["vr"], ["flat"]):
                problems.append("file %s is a settings file and needs when.profile exactly [vr] or [flat]"
                                % entry["id"])
            elif profile[0] in settings:
                problems.append("file %s is a second settings file for its profile (after %s)"
                                % (entry["id"], settings[profile[0]]))
            else:
                settings[profile[0]] = entry["id"]
    for entry in files:
        reason = _reserved_target_problem(entry["target"])
        if reason:
            problems.append("file %s target %s" % (entry["id"], reason))
    for i, first in enumerate(files):
        for second in files[i + 1:]:
            if (first["target"]["root"] == second["target"]["root"] and
                    first["target"]["path"].lower() == second["target"]["path"].lower() and
                    when_can_overlap(first.get("when"), second.get("when"))):
                problems.append("files %s and %s have the same target" % (first["id"], second["id"]))
    return problems


def validate_manifest(obj):
    """Why `obj` (a parsed manifest) breaks schema 2 (docs\\design-manifest-schema2-2026-11-12.md),
    as a list of strings; empty when it is sound. install_manifest.cpp's parseManifest gives the
    same verdict. A schema above MANIFEST_SCHEMA is one problem and nothing else is checked, as
    the launcher reads `schema` alone first."""
    if not isinstance(obj, dict):
        return ["manifest must be an object"]
    schema = obj.get("schema")
    if not _is_int(schema):
        return ["schema must be a whole number"]
    if schema > MANIFEST_SCHEMA:
        return ["manifest schema %d, launcher reads %d" % (schema, MANIFEST_SCHEMA)]
    if schema != MANIFEST_SCHEMA:
        return ["schema %d is no longer read; this launcher reads %d" % (schema, MANIFEST_SCHEMA)]
    problems = _shape("manifest", obj, ("schema", "version", "build", "tag", "issued", "assets", "files"),
                      ("launcher",))
    for key in ("version", "build", "tag"):
        if key in obj and not _is_text(obj[key]):
            problems.append("%s must be a non-empty string" % key)
    if "issued" in obj and (not _is_int(obj["issued"]) or obj["issued"] <= 0):
        problems.append("issued must be a positive integer")
    launcher = obj.get("launcher")
    if "launcher" in obj:
        sub = _shape("launcher", launcher, ("version", "asset", "sha256", "size"))
        problems += sub
        if not sub:
            if not _is_text(launcher["version"]):
                problems.append("launcher.version must be a non-empty string")
            problems += [p for p in (_name_problem("launcher.asset", launcher["asset"]),
                                     _hex_problem("launcher.sha256", launcher["sha256"])) if p]
            if not _is_int(launcher["size"]) or launcher["size"] < 0:
                problems.append("launcher.size must be a non-negative integer")
    asset_names = set()
    assets = obj.get("assets")
    if "assets" in obj:
        if not isinstance(assets, list) or not assets:
            problems.append("assets must be a non-empty list")
        else:
            for index, asset in enumerate(assets):
                where = "assets[%d]" % index
                sub = _shape(where, asset, ("name", "sha256", "size"))
                problems += sub
                if sub:
                    continue
                problem = _name_problem(where + ".name", asset["name"])
                if problem:
                    problems.append(problem)
                elif asset["name"] in asset_names:
                    problems.append("%s.name %s is listed twice" % (where, asset["name"]))
                else:
                    asset_names.add(asset["name"])
                problem = _hex_problem(where + ".sha256", asset["sha256"])
                if problem:
                    problems.append(problem)
                if not _is_int(asset["size"]) or asset["size"] < 0:
                    problems.append("%s.size must be a non-negative integer" % where)
    files = obj.get("files")
    if "files" in obj:
        if not isinstance(files, list) or not files:
            problems.append("files must be a non-empty list")
        else:
            seen, sound = set(), []
            for index, entry in enumerate(files):
                found = _file_problems(index, entry, asset_names)
                problems += found
                if not found:
                    if entry["id"] in seen:
                        problems.append("file %s is listed twice" % entry["id"])
                    else:
                        seen.add(entry["id"])
                        sound.append(entry)
            if not problems:
                problems += _cross_file_problems(sound)
    return problems


def _no_duplicate_keys(pairs):
    seen = {}
    for key, value in pairs:
        if key in seen:
            raise ValueError("duplicate key %s" % key)
        seen[key] = value
    return seen


def load_manifest(data):
    """(object, problems) for the manifest's bytes: pure ASCII, one JSON object
    with no duplicate keys, then validate_manifest."""
    try:
        text = data.decode("ascii")
    except UnicodeDecodeError:
        return None, ["the manifest is not pure ASCII"]
    try:
        obj = json.loads(text, object_pairs_hook=_no_duplicate_keys)
    except ValueError as exc:
        return None, ["the manifest is not valid JSON: %s" % exc]
    return obj, validate_manifest(obj)


# ---- verifying a release folder ----------------------------------------------

def _sha256_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def read_signature(text):
    """The 64 signature bytes from a .sig file's text, or None when malformed."""
    text = text.strip()
    if HEX128.fullmatch(text):
        return bytes.fromhex(text)
    return None


def verify_directory(directory, allow_unsigned=False, keys=None, out=print):
    """Check a release folder; True when every check passes. One line per check
    goes to `out`. `keys` defaults to the keys in update_keys.h."""
    directory = Path(directory)
    ok = True

    def report(passed, text):
        nonlocal ok
        ok = ok and passed
        out("%s %s" % ("ok:  " if passed else "FAIL:", text))
        return passed

    try:
        data = (directory / MANIFEST_NAME).read_bytes()
    except OSError:
        report(False, "%s is missing from %s" % (MANIFEST_NAME, directory))
        return False
    if allow_unsigned:
        out("WARNING: --allow-unsigned: the signature is NOT checked. Do not publish on this result.")
    else:
        keys = header_keys() if keys is None else keys
        try:
            signature = read_signature((directory / SIGNATURE_NAME).read_text(encoding="ascii"))
        except (OSError, UnicodeDecodeError):
            signature = None
            report(False, "%s is missing or unreadable" % SIGNATURE_NAME)
        else:
            if signature is None:
                report(False, "%s is not 128 hex characters" % SIGNATURE_NAME)
            elif not keys:
                report(False, "no public key is listed in update_keys.h to check %s against" % SIGNATURE_NAME)
            else:
                report(any(p256_verify(key, data, signature) for key in keys),
                       "signature on %s matches a listed key" % MANIFEST_NAME)
    obj, problems = load_manifest(data)
    if problems:
        for problem in problems:
            report(False, "schema: " + problem)
        return False
    report(True, "schema of %s" % MANIFEST_NAME)

    def check_file(label, name, sha256, size):
        path = directory / name
        if not path.is_file():
            return report(False, "%s %s is missing" % (label, name))
        if path.stat().st_size != size:
            return report(False, "%s %s has size %d, the manifest says %d" %
                          (label, name, path.stat().st_size, size))
        if _sha256_file(path) != sha256:
            return report(False, "%s %s does not match its sha256" % (label, name))
        return report(True, "%s %s: size and sha256" % (label, name))

    good_assets = set()
    for asset in obj["assets"]:
        if check_file("asset", asset["name"], asset["sha256"], asset["size"]):
            good_assets.add(asset["name"])
    if "launcher" in obj:
        launcher = obj["launcher"]
        check_file("launcher", launcher["asset"], launcher["sha256"], launcher["size"])
    by_asset = {}
    for entry in obj["files"]:
        by_asset.setdefault(entry["asset"], []).append(entry)
    for name, entries in by_asset.items():
        if name not in good_assets:
            report(False, "zip %s was not opened: the asset itself failed" % name)
            continue
        try:
            with zipfile.ZipFile(directory / name) as archive:
                members = {info.filename: info for info in archive.infolist()}
                for entry in entries:
                    info = members.get(entry["path"])
                    label = "%s in %s" % (entry["path"], name)
                    if info is None:
                        report(False, "%s is missing" % label)
                    elif info.file_size != entry["size"]:
                        report(False, "%s has size %d, the manifest says %d" %
                               (label, info.file_size, entry["size"]))
                    elif hashlib.sha256(archive.read(info)).hexdigest() != entry["sha256"]:
                        report(False, "%s does not match its sha256" % label)
                    else:
                        report(True, "%s (%s): size and sha256" % (label, entry["id"]))
        except (OSError, zipfile.BadZipFile) as exc:
            report(False, "zip %s cannot be read: %s" % (name, exc))
    return ok


# ---- signing -----------------------------------------------------------------

def parse_private_key(text):
    """The private scalar from 64 hex characters, or None. Never raises with the
    input in the message."""
    text = text.strip()
    if not HEX64.fullmatch(text.lower()):
        return None
    try:
        value = int(text, 16)
    except ValueError:
        return None
    return value if 1 <= value < N else None


def sign_file(manifest, key_text, keys, dry_run=False):
    """Sign `manifest`. Returns (exit code, message); the message never carries
    key material. Writes <manifest>.sig atomically unless dry_run."""
    manifest = Path(manifest)
    priv = parse_private_key(key_text)
    if priv is None:
        return 1, "the key is not 64 hex characters in range"
    if not keys:
        return 1, "src\\installer\\update_keys.h lists no public key yet; add the key's public half first"
    if p256_public(priv) not in keys:
        return 1, "the key's public half is not listed in src\\installer\\update_keys.h"
    try:
        data = manifest.read_bytes()
    except OSError as exc:
        return 1, "cannot read %s: %s" % (manifest, exc.strerror or "error")
    _, problems = load_manifest(data)
    if problems:
        return 1, "the manifest is not valid:\n  " + "\n  ".join(problems)
    signature = p256_sign(priv, data)
    if not p256_verify(p256_public(priv), data, signature):
        return 1, "the signature did not verify; nothing was written"
    target = manifest.with_name(manifest.name + ".sig")
    if dry_run:
        return 0, "dry run: would write %s; nothing was written" % target
    temporary = target.with_name("." + target.name + ".tmp")
    try:
        temporary.write_bytes((signature.hex() + "\n").encode("ascii"))
        os.replace(temporary, target)
    except OSError as exc:
        with contextlib.suppress(OSError):
            temporary.unlink()
        return 1, "cannot write %s: %s" % (target, exc.strerror or "error")
    return 0, "wrote %s" % target


def _is_terminal():
    return sys.stdout.isatty()


def _refuse_without_terminal(command):
    if _is_terminal():
        return False
    print("sign_manifest: %s needs a real terminal on stdout, so an automated session "
          "cannot capture a key. Run it in your own terminal." % command)
    return True


def cmd_generate():
    if _refuse_without_terminal("generate"):
        return 2
    priv = secrets.randbelow(N - 1) + 1
    print("private key (64 hex): %064x" % priv)
    print("public key (128 hex): %s" % p256_public(priv).hex())
    print("Put the private key in your password manager now and nowhere else; "
          "put the public key in src\\installer\\update_keys.h.")
    return 0


def cmd_sign(args):
    if _refuse_without_terminal("sign"):
        return 2
    if args.key_stdin:
        key_text = sys.stdin.readline().lstrip("\ufeff")
    else:
        key_text = getpass.getpass("private key (64 hex, hidden): ")
    code, message = sign_file(args.manifest, key_text, header_keys(), args.dry_run)
    print("sign_manifest: " + message)
    return code


def cmd_verify(args):
    return 0 if verify_directory(args.directory, args.allow_unsigned) else 3


# ---- self-test ---------------------------------------------------------------

RFC_KEY = "C9AFA9D845BA75166B5C215767B1D6934E50C3DB36E89B127B8A622B120F6721"
RFC_PUBLIC = ("60FED4BA255A9D31C961EB74C6356D68C049B8923B61FA6CE669622E60F29FB6"
              "7903FE1008B8BC99A41AE9E95628BC64F2F1B20C2D7E9F5177A3C294D4462299")
RFC_VECTORS = {
    b"sample": ("EFD48B2AACB6A8FD1140DD9CD45E81D69D2C877B56AAF991C34D0EA84EAF3716",
                "F7CB1C942D657C41D436C7A1B6E29F65F3E900DBB9AFF4064DC4AB2F843ACDA8"),
    b"test": ("F1ABB023518351CD71D881567B1EA663ED3EFCF6C5132B354F28D3B0B7D38367",
              "019F4113742A2B14BD25926B49C649155F267E60D3814B4C0CC84250E46F0083"),
}
# A key for the self-test alone; it signs nothing real.
TEST_KEY = "1f" * 31 + "2e"
OTHER_KEY = "2a" * 31 + "07"


def _fixture_manifest(zip_name="ed-up-core-1.0.0.zip", members=None, zip_bytes=None):
    """(manifest object, zip bytes) for a one-zip release in the test."""
    if members is None:
        members = {"d3d11.dll": b"graphics", "openvr/openvr_api.dll": b"runtime", "edvr.ini": b"[fix]\n"}
    if zip_bytes is None:
        buffer = io.BytesIO()
        with zipfile.ZipFile(buffer, "w", compression=zipfile.ZIP_DEFLATED) as archive:
            for name, content in members.items():
                archive.writestr(name, content)
        zip_bytes = buffer.getvalue()
    places = {
        "d3d11.dll": ("graphics", {"root": "game", "path": "d3d11.dll"}, {"profile": ["vr", "flat"]}, "chain"),
        "openvr/openvr_api.dll": ("runtime", {"root": "openvr", "path": "openvr_api.dll"}, {"profile": ["vr"]},
                                  "runtime"),
        "edvr.ini": ("settings_vr", {"root": "game", "path": "edvr.ini"}, {"profile": ["vr"]}, "settings")}
    manifest = {
        "schema": MANIFEST_SCHEMA, "version": "1.0.0", "build": "v1.0.0", "tag": "v1.0.0", "issued": 1794000000,
        "assets": [{"name": zip_name, "sha256": hashlib.sha256(zip_bytes).hexdigest(), "size": len(zip_bytes)}],
        "files": [{"id": places[name][0], "asset": zip_name, "path": name,
                   "sha256": hashlib.sha256(content).hexdigest(), "size": len(content),
                   "target": dict(places[name][1]), "when": dict(places[name][2]), "existing": places[name][3]}
                  for name, content in members.items() if name in places]}
    return manifest, zip_bytes


def _dump(manifest):
    return (json.dumps(manifest, indent=2, ensure_ascii=True) + "\n").encode("ascii")


def _write_release(directory, manifest, zip_bytes, zip_name="ed-up-core-1.0.0.zip"):
    (directory / zip_name).write_bytes(zip_bytes)
    (directory / MANIFEST_NAME).write_bytes(_dump(manifest))


def _header_text(keys):
    lines = ["#pragma once", "// comment mentioning \"%s\" is not a key" % ("ab" * 64),
             "inline constexpr const char* kUpdatePublicKeys[] = {"]
    lines += ['    "%s",' % key.hex() for key in keys]
    return "\n".join(lines + ["    nullptr,", "};", ""])


def self_test():
    # RFC 6979 appendix A.2.5: P-256 with SHA-256.
    priv = int(RFC_KEY, 16)
    assert p256_public(priv) == bytes.fromhex(RFC_PUBLIC)
    for message, (r, s) in RFC_VECTORS.items():
        signature = p256_sign(priv, message)
        assert signature == bytes.fromhex(r + s), message
        assert p256_verify(bytes.fromhex(RFC_PUBLIC), message, signature)
        assert not p256_verify(bytes.fromhex(RFC_PUBLIC), message + b"x", signature)
        for index in (0, 31, 32, 63):
            flipped = bytearray(signature)
            flipped[index] ^= 1
            assert not p256_verify(bytes.fromhex(RFC_PUBLIC), message, bytes(flipped))
    public = bytes.fromhex(RFC_PUBLIC)
    good = p256_sign(priv, b"sample")
    for r, s in ((0, int.from_bytes(good[32:], "big")), (N, int.from_bytes(good[32:], "big")),
                 (int.from_bytes(good[:32], "big"), 0), (int.from_bytes(good[:32], "big"), N),
                 (int.from_bytes(good[:32], "big"), N + 5), (2 ** 256 - 1, 2 ** 256 - 1)):
        assert not p256_verify(public, b"sample", _bytes32(r) + _bytes32(s))
    assert not p256_verify(public, b"sample", good[:63])
    off_curve = public[:63] + bytes([public[63] ^ 1])
    assert not p256_verify(off_curve, b"sample", good)
    assert not p256_verify(public[:63], b"sample", good)
    for bad_private in (0, N, N + 1):
        try:
            p256_sign(bad_private, b"x")
            raise AssertionError("out of range private key accepted")
        except ValueError:
            pass

    # validate_manifest accepts the fixture and rejects each bad shape.
    manifest, zip_bytes = _fixture_manifest()
    assert validate_manifest(manifest) == []
    launcher = {"version": "0.1.0", "asset": "ed-up-launcher.exe", "sha256": "0" * 64, "size": 1}
    assert validate_manifest(dict(manifest, launcher=launcher)) == []

    def mutated(change):
        copy = json.loads(json.dumps(manifest))
        change(copy)
        return validate_manifest(copy)

    def rejects(label, change):
        assert mutated(change), label

    # schema is read alone first: 1, 3, missing, a bool and a float are all refused, and a schema
    # above ours is the one verdict that names the number.
    assert MANIFEST_SCHEMA == 2
    for bad in (1, 0, 3, True, 2.0, "2"):
        rejects("schema %r" % (bad,), lambda m, bad=bad: m.update(schema=bad))
    rejects("schema missing", lambda m: m.pop("schema"))
    assert mutated(lambda m: m.update(schema=3)) == ["manifest schema 3, launcher reads 2"]
    assert mutated(lambda m: (m.update(schema=3), m.update(extra=1))) == ["manifest schema 3, launcher reads 2"]
    rejects("min_launcher is gone", lambda m: m.update(min_launcher="0.1.0"))
    for key in ("version", "build", "tag"):
        rejects(key + " number", lambda m, key=key: m.update({key: 3}))
        rejects(key + " empty", lambda m, key=key: m.update({key: ""}))
        rejects(key + " missing", lambda m, key=key: m.pop(key))
    for bad in (0, -1, "1", 1.5, True):
        rejects("issued %r" % (bad,), lambda m, bad=bad: m.update(issued=bad))
    rejects("unknown key", lambda m: m.update(extra=1))
    rejects("launcher shape", lambda m: m.update(launcher={"version": "1"}))
    rejects("launcher sha", lambda m: m.update(launcher=dict(launcher, sha256="A" * 64)))
    rejects("launcher asset", lambda m: m.update(launcher=dict(launcher, asset="a/b.exe")))
    rejects("assets empty", lambda m: m.update(assets=[]))
    rejects("files empty", lambda m: m.update(files=[]))
    rejects("asset sha short", lambda m: m["assets"][0].update(sha256="ab"))
    rejects("asset sha upper", lambda m: m["assets"][0].update(sha256="A" * 64))
    rejects("asset size", lambda m: m["assets"][0].update(size="3"))
    rejects("file sha", lambda m: m["files"][0].update(sha256="g" * 64))
    rejects("file size negative", lambda m: m["files"][0].update(size=-1))
    for bad in ("a b.zip", "a/b.zip", "a\\b.zip", "", "..", ".", "\u00e9.zip", "a.zip\n"):
        rejects("asset name %r" % bad, lambda m, bad=bad: m["assets"][0].update(name=bad))
    for bad in ("", ".", "..", "/abs", "a//b", "a/./b", "a/../b", "a/", "a\\b", "C:/x", "a:b",
                "a\x01b", "caf\u00e9", 5):
        rejects("path %r" % (bad,), lambda m, bad=bad: m["files"][0].update(path=bad))
    rejects("file without id", lambda m: m["files"][0].pop("id"))
    rejects("file with the old item key", lambda m: m["files"][0].update(item="native_graphics"))
    for bad in ("", "Graphics", "a-b", "a" * 33, "caf\u00e9", 5, None):
        rejects("id %r" % (bad,), lambda m, bad=bad: m["files"][0].update(id=bad))
    assert not mutated(lambda m: m["files"][0].update(id="a" * 32))
    rejects("duplicate id", lambda m: m["files"].append(
        dict(m["files"][0], target={"root": "game", "path": "other.dll"})))
    # target: its shape, its root and the path rule.
    rejects("target missing", lambda m: m["files"][0].pop("target"))
    rejects("target not an object", lambda m: m["files"][0].update(target="d3d11.dll"))
    rejects("target unknown key", lambda m: m["files"][0]["target"].update(mode="x"))
    rejects("target root settings", lambda m: m["files"][0]["target"].update(root="settings"))
    rejects("target root missing", lambda m: m["files"][0]["target"].pop("root"))
    def with_target(root, path):
        """Appends a replace file with this target, so a target rule is tried on a file with no name of its own."""
        return lambda m: m["files"].append(dict(m["files"][0], id="other", existing="replace",
                                                target={"root": root, "path": path}))

    assert not mutated(with_target("game", "sub/dir/File-1.dll"))
    assert not mutated(with_target("game", "console.txt"))
    assert not mutated(with_target("game", "com10.dll"))
    for bad in ("", ".", "..", "../x.dll", "a/../x.dll", "/abs.dll", "C:/x.dll", "C:x.dll", "a:b", "a//b",
                "a/./b", "a\\b", "NUL", "nul.txt", "con.txt", "Con", "COM1", "lpt9.dll", "a/AUX/b", "aux.tar.gz",
                "com0", "x.dll:stream", "x.dll.", "x.dll ", "a./b", "a /b", "a\x01b", "caf\u00e9", "a*b", "a?b",
                'a"b', "a<b", "a>b", "a|b", 5, None):
        rejects("target path %r" % (bad,), with_target("game", bad))
    # chain and runtime work at today's names only; a game target may not alias the openvr root; the installer's own names are reserved.
    rejects("chain at another name", lambda m: m["files"][0]["target"].update(path="other.dll"))
    rejects("chain in the openvr root", lambda m: m["files"][0]["target"].update(root="openvr"))
    rejects("chain nested", lambda m: m["files"][0]["target"].update(path="sub/d3d11.dll"))
    assert not mutated(lambda m: m["files"][0]["target"].update(path="D3D11.DLL"))
    rejects("runtime at another name", lambda m: m["files"][1]["target"].update(path="other.dll"))
    rejects("runtime nested", lambda m: m["files"][1]["target"].update(path="sub/openvr_api.dll"))
    assert not mutated(lambda m: m["files"][1]["target"].update(path="OpenVR_API.dll"))
    for root, bad in (("game", "openvr/x.dll"), ("game", "Openvr/win64/x.dll"), ("game", "edvr_profile.ini"),
                      ("game", "EDVR_Profile.INI"), ("game", "edvr_openxr.ini"), ("openvr", "edvr_openxr.ini"),
                      ("game", "edvr.ini.base"), ("game", "sub/y.BASE"), ("openvr", "openvr_api_orig.dll"),
                      ("game", "d3d11_edhm.dll"), ("game", "D3D11_Other.DLL"), ("game", "d3d11_.dll"),
                      ("game", "edvr_install/x.ini"), ("game", "edvr_backup/a/b.dll")):
        rejects("reserved target %s %s" % (root, bad), with_target(root, bad))
    for root, good in (("game", "openvr-extra/x.dll"), ("game", "openvrx.dll"), ("openvr", "openvr/x.dll"),
                       ("game", "d3d11x.dll"), ("game", "d3d11_x.dll.bak"), ("game", "sub/d3d11_x.dll"),
                       ("game", "sub/edvr_profile.ini"), ("game", "x.basement"), ("game", "edvr_installer/x")):
        assert not mutated(with_target(root, good)), (root, good)
    # when: absent means always; each key alone and together; an empty one, an unknown key or value is an error.
    assert not mutated(lambda m: m["files"][0].pop("when"))
    assert not mutated(lambda m: m["files"][0].update(when={"adapter": "nvidia"}))
    assert not mutated(lambda m: m["files"][0].update(when={"profile": ["flat"], "adapter": "nvidia"}))
    assert not mutated(lambda m: m["files"][0].update(when={"profile": ["flat", "vr"]}))
    for bad in ({}, [], None, "vr", {"route": "x"}, {"profile": []}, {"profile": "vr"}, {"profile": ["both"]},
                {"profile": ["vr", "vr"]}, {"profile": ["VR"]}, {"profile": [1]}, {"adapter": "amd"},
                {"adapter": True}, {"adapter": ["nvidia"]}, {"profile": ["vr"], "option": "x"}):
        rejects("when %r" % (bad,), lambda m, bad=bad: m["files"][0].update(when=bad))
    # existing: each value (with the constraints its behaviour carries), and nothing else.
    for ok in ("replace", "owned"):
        assert not mutated(lambda m, ok=ok: m["files"][0].update(existing=ok)), ok
    for bad in ("Replace", "merge", "", None, 1, ["replace"]):
        rejects("existing %r" % (bad,), lambda m, bad=bad: m["files"][0].update(existing=bad))
    rejects("existing missing", lambda m: m["files"][0].pop("existing"))
    rejects("second chain file", lambda m: m["files"].append(dict(
        m["files"][0], id="other", target={"root": "game", "path": "other.dll"})))
    rejects("second runtime file", lambda m: m["files"].append(dict(
        m["files"][1], id="other", target={"root": "openvr", "path": "other.dll"})))
    rejects("runtime in the game root", lambda m: m["files"][1]["target"].update(root="game"))
    rejects("settings without when", lambda m: m["files"][2].pop("when"))
    rejects("settings both profiles", lambda m: m["files"][2].update(when={"profile": ["vr", "flat"]}))
    rejects("settings adapter only", lambda m: m["files"][2].update(when={"adapter": "nvidia"}))
    rejects("second settings file for a profile", lambda m: m["files"].append(dict(
        m["files"][2], id="other", target={"root": "game", "path": "other.ini"})))
    assert not mutated(lambda m: m["files"].append(dict(
        m["files"][2], id="settings_flat", when={"profile": ["flat"]},
        target={"root": "game", "path": "edvr-flat.ini"})))
    # Overlapping targets: same root and case-folded path when both can hold; a different root, disjoint
    # profiles or a different path do not overlap, and an adapter never separates two files.
    rejects("overlap", lambda m: m["files"].append(dict(
        m["files"][0], id="other", existing="replace", target={"root": "game", "path": "D3D11.DLL"})))
    rejects("overlap, one without when", lambda m: m["files"].append(dict(
        {k: v for k, v in m["files"][0].items() if k != "when"}, id="other", existing="replace")))
    rejects("overlap, adapter does not exclude", lambda m: m["files"].append(dict(
        m["files"][0], id="other", existing="replace", when={"profile": ["vr"], "adapter": "nvidia"})))
    assert not mutated(lambda m: m["files"].append(dict(
        m["files"][0], id="other", target={"root": "openvr", "path": "d3d11.dll"}, existing="replace")))
    assert not mutated(lambda m: (m["files"][0].update(when={"profile": ["vr"]}, existing="replace"),
                                  m["files"].append(dict(m["files"][0], id="other", when={"profile": ["flat"]}))))
    rejects("duplicate asset", lambda m: m["assets"].append(dict(m["assets"][0])))
    rejects("file asset unlisted", lambda m: m["files"][0].update(asset="nowhere.zip"))
    rejects("not a list", lambda m: m.update(files={}))
    assert validate_manifest([]) and validate_manifest("x") and validate_manifest(None)
    # The schema number is the one the C++ parser reads.
    header_text = SCHEMA_HEADER.read_text(encoding="utf-8")
    found = re.search(r"kManifestSchema\s*=\s*(\d+)\s*;", header_text)
    assert found and int(found.group(1)) == MANIFEST_SCHEMA, \
        "sign_manifest.MANIFEST_SCHEMA and kManifestSchema in install_manifest.h differ"
    # load_manifest: ASCII only, valid JSON, no duplicate keys.
    assert load_manifest(_dump(manifest))[1] == []
    assert load_manifest(_dump(manifest).replace(b'"1.0.0"', '"\u00e9"'.encode("utf-8"), 1))[1]
    assert load_manifest(b"{")[1] and load_manifest(b'{"schema": 2, "schema": 2}')[1]
    assert load_manifest(b"\xef\xbb\xbf" + _dump(manifest))[1]

    with tempfile.TemporaryDirectory(prefix="edvr-sign-manifest-") as temp:
        temp = Path(temp)
        # Header parsing: none, one and two keys; comments and short strings are not keys.
        key_a, key_b = p256_public(int(TEST_KEY, 16)), p256_public(int(OTHER_KEY, 16))
        header = temp / "update_keys.h"
        header.write_text(_header_text([]), encoding="utf-8")
        assert header_keys(header) == []
        header.write_text(_header_text([key_a, key_b]), encoding="utf-8")
        assert header_keys(header) == [key_a, key_b]
        header.write_text(_header_text([key_a]).replace(key_a.hex(), key_a.hex()[:-2]), encoding="utf-8")
        assert header_keys(header) == []
        real = header_keys(ROOT / "src" / "installer" / "update_keys.h")
        assert all(len(key) == 64 and _on_curve((int.from_bytes(key[:32], "big"), int.from_bytes(key[32:], "big")))
                   for key in real), "update_keys.h holds a key that is not on the curve"

        # Sign and verify a release folder through the internal functions.
        release = temp / "release"
        release.mkdir()
        _write_release(release, manifest, zip_bytes)
        manifest_path = release / MANIFEST_NAME
        sig_path = release / SIGNATURE_NAME
        assert sign_file(manifest_path, TEST_KEY, [], dry_run=False)[0] == 1
        assert "lists no public key" in sign_file(manifest_path, TEST_KEY, [], False)[1]
        assert not sig_path.exists()
        assert sign_file(manifest_path, OTHER_KEY, [key_a], False)[0] == 1 and not sig_path.exists()
        for bad_key in ("", "zz" * 32, TEST_KEY[:-1], "00" * 32, "ff" * 32):
            code, message = sign_file(manifest_path, bad_key, [key_a], False)
            assert code == 1 and not sig_path.exists(), bad_key
        before = sorted(p.name for p in release.iterdir())
        code, message = sign_file(manifest_path, TEST_KEY, [key_a], dry_run=True)
        assert code == 0 and "nothing was written" in message
        assert sorted(p.name for p in release.iterdir()) == before, "--dry-run wrote"
        code, message = sign_file(manifest_path, TEST_KEY + "\n", [key_b, key_a], False)
        assert code == 0, message
        assert TEST_KEY not in message
        text = sig_path.read_bytes()
        assert re.fullmatch(rb"[0-9a-f]{128}\n", text), text
        assert not [p for p in release.iterdir() if p.name.endswith(".tmp")]
        lines = []
        assert verify_directory(release, keys=[key_a], out=lines.append), lines
        assert not verify_directory(release, keys=[key_b], out=lambda line: None)
        assert not verify_directory(release, keys=[], out=lambda line: None)
        invalid = temp / "invalid"
        invalid.mkdir()
        bad_manifest = dict(manifest, schema=MANIFEST_SCHEMA + 1)
        _write_release(invalid, bad_manifest, zip_bytes)
        assert sign_file(invalid / MANIFEST_NAME, TEST_KEY, [key_a], False)[0] == 1
        assert not (invalid / SIGNATURE_NAME).exists()

        # Tampering: the signature, the manifest, an asset, a zip member.
        def tampered(label, change, allow_unsigned=False):
            folder = temp / ("t-" + label)
            folder.mkdir()
            for entry in release.iterdir():
                (folder / entry.name).write_bytes(entry.read_bytes())
            change(folder)
            return verify_directory(folder, allow_unsigned=allow_unsigned, keys=[key_a], out=lambda line: None)

        assert tampered("none", lambda f: None)
        assert not tampered("manifest", lambda f: (f / MANIFEST_NAME).write_bytes(
            (f / MANIFEST_NAME).read_bytes().replace(b"1794000000", b"1794000001")))
        assert not tampered("nosig", lambda f: (f / SIGNATURE_NAME).unlink())
        assert not tampered("badsig", lambda f: (f / SIGNATURE_NAME).write_text("zz\n"))
        assert tampered("nosig-allowed", lambda f: (f / SIGNATURE_NAME).unlink(), allow_unsigned=True)
        lines = []
        folder = temp / "t-nosig-allowed"
        verify_directory(folder, allow_unsigned=True, keys=[key_a], out=lines.append)
        assert any("NOT checked" in line for line in lines)
        zip_file = "ed-up-core-1.0.0.zip"
        assert not tampered("asset-byte", lambda f: (f / zip_file).write_bytes(
            (f / zip_file).read_bytes() + b"\0"), allow_unsigned=True)
        assert not tampered("asset-missing", lambda f: (f / zip_file).unlink(), allow_unsigned=True)
        other_members = {"d3d11.dll": b"GRAPHICS", "openvr/openvr_api.dll": b"runtime", "edvr.ini": b"[fix]\n"}
        other_zip = _fixture_manifest(members=other_members)[1]
        # A zip member changed in place with the same size, and the manifest's asset entry
        # rewritten to match the new zip, so only the member check can catch it.
        forged_manifest, _ = _fixture_manifest(members=other_members)
        for entry, original in zip(forged_manifest["files"], manifest["files"]):
            entry["sha256"], entry["size"] = original["sha256"], original["size"]
        forged = temp / "forged"
        forged.mkdir()
        _write_release(forged, forged_manifest, other_zip)
        lines = []
        assert not verify_directory(forged, allow_unsigned=True, out=lines.append)
        assert any("d3d11.dll in" in line and line.startswith("FAIL") for line in lines), lines
        assert any(line.startswith("ok:") and "openvr/openvr_api.dll in" in line for line in lines), lines
        missing_member = temp / "missing-member"
        missing_member.mkdir()
        short_manifest, short_zip = _fixture_manifest()
        short_manifest["files"][0]["path"] = "gone.dll"
        _write_release(missing_member, short_manifest, short_zip)
        assert not verify_directory(missing_member, allow_unsigned=True, out=lambda line: None)
        launcher_manifest = dict(manifest, launcher={
            "version": "0.1.0", "asset": "ed-up-launcher.exe",
            "sha256": hashlib.sha256(b"exe").hexdigest(), "size": 3})
        with_launcher = temp / "with-launcher"
        with_launcher.mkdir()
        _write_release(with_launcher, launcher_manifest, zip_bytes)
        assert not verify_directory(with_launcher, allow_unsigned=True, out=lambda line: None)
        (with_launcher / "ed-up-launcher.exe").write_bytes(b"exe")
        assert verify_directory(with_launcher, allow_unsigned=True, out=lambda line: None)
        (with_launcher / "ed-up-launcher.exe").write_bytes(b"EXE")
        assert not verify_directory(with_launcher, allow_unsigned=True, out=lambda line: None)
        assert not verify_directory(temp / "nowhere", allow_unsigned=True, out=lambda line: None)

        # main(): --key-stdin with a BOM, --dry-run, and the terminal refusals, all in
        # process with the terminal check and the header replaced.
        global HEADER, _is_terminal
        old_header, old_terminal, old_stdin = HEADER, _is_terminal, sys.stdin
        main_dir = temp / "main"
        main_dir.mkdir()
        _write_release(main_dir, manifest, zip_bytes)
        header.write_text(_header_text([key_a]), encoding="utf-8")
        HEADER = header
        try:
            _is_terminal = lambda: True
            sys.stdin = io.StringIO("\ufeff  " + TEST_KEY + "  \r\n")
            captured = io.StringIO()
            with contextlib.redirect_stdout(captured):
                assert main(["sign", str(main_dir / MANIFEST_NAME), "--key-stdin", "--dry-run"]) == 0
            assert not (main_dir / SIGNATURE_NAME).exists(), "--dry-run wrote"
            assert not [p for p in main_dir.iterdir() if p.name.startswith(".")]
            assert TEST_KEY not in captured.getvalue()
            sys.stdin = io.StringIO("\ufeff  " + TEST_KEY + "  \r\n")
            with contextlib.redirect_stdout(io.StringIO()):
                assert main(["sign", str(main_dir / MANIFEST_NAME), "--key-stdin"]) == 0
            assert verify_directory(main_dir, keys=[key_a], out=lambda line: None)
            sys.stdin = io.StringIO("not a key\n")
            captured = io.StringIO()
            with contextlib.redirect_stdout(captured):
                assert main(["sign", str(main_dir / MANIFEST_NAME), "--key-stdin"]) == 1
            assert "not a key" not in captured.getvalue()
            captured = io.StringIO()
            with contextlib.redirect_stdout(captured):
                assert main(["verify", str(main_dir)]) == 0
            with contextlib.redirect_stdout(io.StringIO()):
                assert main(["verify", str(main_dir), "--allow-unsigned"]) == 0
                (main_dir / SIGNATURE_NAME).write_text("0" * 128 + "\n")
                assert main(["verify", str(main_dir)]) == 3
            captured = io.StringIO()
            with contextlib.redirect_stdout(captured):
                assert main(["generate"]) == 0
            found = re.findall(r"[0-9a-f]{64}", captured.getvalue())
            shown_public = re.search(r"[0-9a-f]{128}", captured.getvalue()).group(0)
            assert p256_public(int(found[0], 16)).hex() == shown_public
            _is_terminal = lambda: False
            sys.stdin = io.StringIO(TEST_KEY + "\n")
            before = sorted(p.name for p in main_dir.iterdir())
            with contextlib.redirect_stdout(io.StringIO()):
                assert main(["sign", str(main_dir / MANIFEST_NAME), "--key-stdin"]) == 2
                assert main(["generate"]) == 2
            assert sorted(p.name for p in main_dir.iterdir()) == before
        finally:
            HEADER, _is_terminal, sys.stdin = old_header, old_terminal, old_stdin

        # The real refusal: a subprocess with piped stdout is not a terminal.
        script = str(Path(__file__).resolve())
        refused_dir = temp / "refused"
        refused_dir.mkdir()
        _write_release(refused_dir, manifest, zip_bytes)
        for command, stdin in ((["generate"], ""),
                               (["sign", str(refused_dir / MANIFEST_NAME), "--key-stdin"], TEST_KEY + "\n"),
                               (["sign", str(refused_dir / MANIFEST_NAME), "--key-stdin", "--dry-run"],
                                TEST_KEY + "\n")):
            result = subprocess.run([sys.executable, script] + command, input=stdin.encode("ascii"),
                                    capture_output=True, check=False, timeout=60)
            output = (result.stdout + result.stderr).decode("utf-8", "replace")
            assert result.returncode == 2, (command, result.returncode, output)
            assert not re.search(r"[0-9a-fA-F]{64}", output), "key material in the refusal"
            assert "terminal" in output
        assert not (refused_dir / SIGNATURE_NAME).exists()
        result = subprocess.run([sys.executable, script, "verify", str(refused_dir), "--allow-unsigned"],
                                capture_output=True, check=False, timeout=60)
        assert result.returncode == 0, result.stdout
    print("sign_manifest: self-test passed")
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--self-test", action="store_true")
    commands = parser.add_subparsers(dest="command")
    commands.add_parser("generate", help="make one key and print it once (needs a terminal)")
    sign = commands.add_parser("sign", help="sign a manifest (needs a terminal)")
    sign.add_argument("manifest", type=Path)
    sign.add_argument("--key-stdin", action="store_true", help="read the key as one line on stdin")
    sign.add_argument("--dry-run", action="store_true", help="do everything except write the .sig")
    verify = commands.add_parser("verify", help="check a release folder")
    verify.add_argument("directory", type=Path)
    verify.add_argument("--allow-unsigned", action="store_true", help="skip only the signature check")
    args = parser.parse_args(argv)
    if args.self_test:
        return self_test()
    if args.command == "generate":
        return cmd_generate()
    if args.command == "sign":
        return cmd_sign(args)
    if args.command == "verify":
        return cmd_verify(args)
    parser.print_help()
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
