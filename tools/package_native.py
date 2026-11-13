#!/usr/bin/env python3
"""Build a native-only EDVR release archive from validated build outputs.

Before anything is staged, the version stamped into each EDVR binary being
packaged (d3d11.dll, openvr_api.dll, the installer: the FileVersion and
ProductVersion strings of their VERSIONINFO, which build.bat sets from `git
describe --tags --always --dirty`) is compared with the release version asked
for. The rule: both strings must be exactly "v<version>" -- what git prints on
a clean checkout of the tag v<version> -- or exactly "<version>". Anything
that mentions "dirty" is refused first, as a build from a tree with
uncommitted changes (the kind that has reached a field tester twice); a build
a few commits past the tag ("v0.17.0-2-g650d8a9"), "unknown" (no git), another
release's tag and a binary with no version stamp are refused as mismatches.
--dry-run makes the same check and writes nothing.

Beside the archives and split assets it writes SHA256SUMS: one "<sha256>  <name>"
line, LF-terminated, for every file a release attaches except the manifest's
.sig, which is made after packaging. release.yml checks it against its own list.
"""
import argparse
import hashlib
import io
import json
import os
import re
import shutil
import struct
import subprocess
import tempfile
import time
import zipfile
from pathlib import Path
from typing import NamedTuple, Optional

import sign_manifest
from gen_installer_rc import EMBEDDED_ASSET, IDR_FILE_BASE, IDR_MANIFEST, descriptor_bytes, embedded_manifest

ROOT = Path(__file__).resolve().parents[1]

# What a release carries under these archive names is stamped with EDVR's own
# version by build.bat (tools\gen_installer_rc.py writes the VERSIONINFO); the
# other files (NVIDIA's runtime, Microsoft's loader) are not ours to stamp.
STAMPED = ("d3d11.dll", "openvr/openvr_api.dll", "edvr-installer.exe")
INSTALLER = "edvr-installer.exe"
LINUX_WRAPPER = "edvr-installer-linux.py"
LINUX_LAUNCHER = "edvr_installer.sh"
# The launcher's Linux package: the same launcher exe the Windows release publishes, run inside Elite's
# Proton prefix by the installer wrapper's --launcher mode. A convenience package, not in the signed manifest.
LAUNCHER_LINUX_WRAPPER = "ed-up-launcher-linux.py"
LAUNCHER_LINUX_SH = "ed-up-launcher.sh"
LAUNCHER_LINUX_README = "README-launcher-linux.txt"
UNIX_EXECUTABLE = (LINUX_WRAPPER, LINUX_LAUNCHER, LAUNCHER_LINUX_WRAPPER, LAUNCHER_LINUX_SH)
STAMP_KEYS = ("FileVersion", "ProductVersion")

# The split assets published beside the three archives (docs\design-launcher,
# "Updates"). The core zip carries the files that change every release; DLSS and
# XeSS go in zips named by a hash of their contents, so an unchanged runtime keeps
# its asset name from release to release.
MANIFEST = sign_manifest.MANIFEST_NAME
SUMS_NAME = "SHA256SUMS"
LAUNCHER_ASSET = "ed-up-launcher.exe"
LAUNCHER_VERSION_HEADER = ("src", "installer", "launcher_version.h")
# The licence of the code direct launch is ported from (min-ed-launcher, MIT). The launcher exe carries it
# as RCDATA 120 and prints it with --licenses; the core zip carries it as MIN-ED-LAUNCHER-LICENSE.txt.
LAUNCHER_LICENSE_FILE = ("third_party", "min_ed_launcher", "LICENSE")
LAUNCHER_LICENSE_NAME = "MIN-ED-LAUNCHER-LICENSE.txt"
LAUNCHER_LICENSE_RESOURCE = 120
KEYS_HEADER = ("src", "installer", "update_keys.h")


class PackagedFile(NamedTuple):
    """One file of the manifest's `files`: the one place a payload file is declared."""
    id: str            # [a-z0-9_]{1,32}, the stable name the install record is keyed by
    source: str        # its path inside its zip (and an unpacked --payload folder)
    group: str         # the zip that holds it: core, dlss or xess
    root: str          # target.root: game or openvr
    path: str          # target.path under that root
    when: Optional[dict]  # None means always; keys profile and adapter, as in the schema
    existing: str      # how a file already at the target is treated


# The profiles an install covers: both writes vr and flat, so d3d11.dll (every profile) lists both.
_VR, _FLAT, _EITHER, _NVIDIA = {"profile": ["vr"]}, {"profile": ["flat"]}, {"profile": ["vr", "flat"]}, {"adapter": "nvidia"}
# What src\installer\plan.cpp's planInstall does with each file today:
#   d3d11.dll        every profile; a foreign DLL at the target is chained to
#   openvr_api.dll   VR profiles only (flat keeps the game's own runtime); the game's original is kept
#   openxr_loader / licence   VR profiles only, replaced
#   edvr.ini / edvr-flat.ini  merged into the settings already there; each for its own profile
#   nvngx_dlss.dll   only where the survey found an NVIDIA adapter; a foreign copy is left alone
#   libxess.dll and its two notices   every install; foreign copies are left alone
FILES = (
    PackagedFile("graphics", "d3d11.dll", "core", "game", "d3d11.dll", _EITHER, "chain"),
    PackagedFile("runtime", "openvr/openvr_api.dll", "core", "openvr", "openvr_api.dll", _VR, "runtime"),
    PackagedFile("openxr_loader", "openvr/openxr_loader.dll", "core", "openvr", "openxr_loader.dll", _VR, "replace"),
    PackagedFile("openxr_license", "openvr/OPENXR-LOADER-LICENSE.txt", "core", "openvr",
                 "OPENXR-LOADER-LICENSE.txt", _VR, "replace"),
    PackagedFile("settings_vr", "edvr.ini", "core", "game", "edvr.ini", _VR, "settings"),
    PackagedFile("settings_flat", "edvr-flat.ini", "core", "game", "edvr-flat.ini", _FLAT, "settings"),
    PackagedFile("dlss", "nvngx_dlss.dll", "dlss", "game", "nvngx_dlss.dll", _NVIDIA, "owned"),
    PackagedFile("xess", "libxess.dll", "xess", "game", "libxess.dll", None, "owned"),
    PackagedFile("xess_license", "INTEL-XESS-LICENSE.txt", "xess", "game", "INTEL-XESS-LICENSE.txt", None, "owned"),
    PackagedFile("xess_third_party", "INTEL-XESS-THIRD-PARTY.txt", "xess", "game", "INTEL-XESS-THIRD-PARTY.txt",
                 None, "owned"),
)
# Members that ship in a zip without an entry in the manifest's files: notices, and the profile
# descriptor, which the planner no longer reads (it writes its own).
_EXTRA_MEMBERS = {"core": ("edvr_profile.ini", "LICENSE.txt", "DXBC-HASH-LICENSE.txt", "MINIZ-LICENSE.txt",
                           "FIDELITYFX-SDK-DX11-LICENSE.txt", "MIN-ED-LAUNCHER-LICENSE.txt"),
                  "dlss": ("NVIDIA-DLSS-LICENSE.txt",), "xess": ()}


def _group_names(group):
    return tuple(f.source for f in FILES if f.group == group) + _EXTRA_MEMBERS[group]


CORE_NAMES = _group_names("core")
DLSS_NAMES = _group_names("dlss")
XESS_NAMES = _group_names("xess")


def manifest_file_entry(file, asset, data):
    """The manifest's files[] entry for `file`, held by zip `asset`, whose bytes are `data`."""
    entry = {"id": file.id, "asset": asset, "path": file.source, "sha256": hashlib.sha256(data).hexdigest(),
             "size": len(data), "target": {"root": file.root, "path": file.path}}
    if file.when is not None:
        entry["when"] = json.loads(json.dumps(file.when))
    entry["existing"] = file.existing
    return entry
SPLIT_PATTERNS = ("ed-up-dlss-*.zip", "ed-up-xess-*.zip")


def _version(value):
    if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+(?:[-+][A-Za-z0-9.-]+)?", value or ""):
        raise ValueError("version must be semantic, for example 0.3.0")
    return value


def _inside(path, base):
    try:
        return os.path.commonpath([os.path.realpath(path), os.path.realpath(base)]) == os.path.realpath(base)
    except ValueError:
        return False


def _files(root, no_dlss):
    build = root / "build"
    result = [(build / "edvr_openxr_graphics.dll", "d3d11.dll"),
              (build / INSTALLER, INSTALLER),
              (build / "edvr_profile_vr.ini", "edvr_profile.ini"),
              (build / "edvr_openxr_runtime.dll", "openvr/openvr_api.dll"),
              (build / "openxr_loader.dll", "openvr/openxr_loader.dll"),
              (build / "OPENXR-LOADER-LICENSE.txt", "openvr/OPENXR-LOADER-LICENSE.txt"),
              (root / "release" / "README.txt", "README.txt"),
              (root / "release" / "README-linux.txt", "README-linux.txt"),
              (root / "tools" / "install_proton.py", LINUX_WRAPPER),
              (root / LINUX_LAUNCHER, LINUX_LAUNCHER),
              (root / "release" / "OPENVR.txt", "openvr/READ-ME-FIRST.txt")]
    # Historical --no-dlss permits builds made without the SDK. It cannot
    # remove a runtime already embedded in the installer we distribute.
    if not no_dlss or (build / "nvngx_dlss.dll").is_file():
        result.append((build / "nvngx_dlss.dll", "nvngx_dlss.dll"))
        result.append((build / "NVIDIA-DLSS-LICENSE.txt", "NVIDIA-DLSS-LICENSE.txt"))
    if (build / "libxess.dll").is_file():
        result.extend(((build / "libxess.dll", "libxess.dll"),
                       (build / "INTEL-XESS-LICENSE.txt", "INTEL-XESS-LICENSE.txt"),
                       (build / "INTEL-XESS-THIRD-PARTY.txt", "INTEL-XESS-THIRD-PARTY.txt")))
    # Both settings files ship: edvr.ini for VR, and edvr-flat.ini for the flat
    # runtime (config.cpp reads edvr-flat.ini first). Each keeps its own name.
    result.extend([(root / "edvr.ini", "edvr.ini"),
                   (build / "edvr-flat.ini", "edvr-flat.ini"),
                   (root / "LICENSE", "LICENSE.txt")])
    result.append((root / "third_party" / "dxbc_hash" / "LICENSE.TXT", "DXBC-HASH-LICENSE.txt"))
    result.append((root / "third_party" / "miniz" / "LICENSE", "MINIZ-LICENSE.txt"))
    result.append((root.joinpath(*LAUNCHER_LICENSE_FILE), LAUNCHER_LICENSE_NAME))
    # AMD's FSR3 D3D11 port (MIT). Unlike NVIDIA's runtime it ships no DLL --
    # it is statically linked into d3d11.dll -- so its presence in a build is
    # exactly this notice, which build.bat copies beside the binaries when it
    # links the port and deletes on every no-port path. MIT's notice
    # requirement applies to the release because the code is in the binary we
    # distribute (the review of 2026-09-16, F10).
    ffx_notice = build / "FIDELITYFX-SDK-DX11-LICENSE.txt"
    if ffx_notice.is_file():
        result.append((ffx_notice, "FIDELITYFX-SDK-DX11-LICENSE.txt"))
    return result


def version_stamp_problems(version, stamps):
    """Why the binaries in `stamps` cannot be packaged as release `version`:
    a list of messages, empty when they can. `stamps` is [(name, mapping)],
    the mapping holding the FileVersion and ProductVersion strings a binary's
    VERSIONINFO carries, or None for a binary that has no stamp. The rule is
    in the module docstring: exactly "v<version>" or "<version>", and "dirty"
    named for what it is before anything else."""
    accepted = ("v" + version, version)
    problems = []
    for name, stamp in stamps:
        if not stamp:
            problems.append("%s carries no version stamp (no VERSIONINFO FileVersion or "
                            "ProductVersion), so it cannot be told from any other build" % name)
            continue
        by_value = {}
        for key in STAMP_KEYS:
            value = stamp.get(key)
            by_value.setdefault(None if value is None else str(value), []).append(key)
        for value, keys in by_value.items():
            which = "/".join(keys)
            if value is None:
                problems.append("%s has no %s" % (name, which))
            elif "dirty" in value.lower():
                problems.append("%s is stamped %s (%s): built from a tree with uncommitted "
                                "changes" % (name, value, which))
            elif value not in accepted:
                problems.append("%s is stamped %s (%s), not release v%s" % (name, value, which, version))
    return problems


def _stamped_versions(path):
    """{"FileVersion": ..., "ProductVersion": ...} as the VERSIONINFO of the
    file at `path` says (a key it lacks is left out), or None when the file has
    no version resource. Read with the Windows version API; the file is not
    loaded or run."""
    if os.name != "nt":
        raise ValueError("reading a binary's version stamp requires Windows")
    import ctypes
    from ctypes import wintypes
    api = ctypes.WinDLL("version", use_last_error=True)
    api.GetFileVersionInfoSizeW.argtypes = [wintypes.LPCWSTR, ctypes.POINTER(wintypes.DWORD)]
    api.GetFileVersionInfoSizeW.restype = wintypes.DWORD
    api.GetFileVersionInfoW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p]
    api.GetFileVersionInfoW.restype = wintypes.BOOL
    api.VerQueryValueW.argtypes = [ctypes.c_void_p, wintypes.LPCWSTR,
                                   ctypes.POINTER(ctypes.c_void_p), ctypes.POINTER(wintypes.UINT)]
    api.VerQueryValueW.restype = wintypes.BOOL
    size = api.GetFileVersionInfoSizeW(str(path), None)
    if not size:
        return None
    block = ctypes.create_string_buffer(size)
    if not api.GetFileVersionInfoW(str(path), 0, size, block):
        return None
    data, length = ctypes.c_void_p(), wintypes.UINT()

    def query(sub_block):
        if api.VerQueryValueW(block, sub_block, ctypes.byref(data), ctypes.byref(length)) \
                and length.value and data.value:
            return data.value, length.value
        return None

    languages = []
    found = query("\\VarFileInfo\\Translation")
    if found:
        raw = ctypes.string_at(found[0], found[1])
        languages = ["%04x%04x" % struct.unpack_from("<HH", raw, offset)
                     for offset in range(0, len(raw) - 3, 4)]
    for language in languages or ["040904b0", "040904e4", "000004b0"]:
        stamp = {}
        for key in STAMP_KEYS:
            found = query("\\StringFileInfo\\%s\\%s" % (language, key))
            if found:
                stamp[key] = ctypes.wstring_at(found[0])
        if stamp:
            return stamp
    return {}


def _check_version_stamps(version, files):
    """Refuse (ValueError) unless every EDVR binary in `files` is stamped as
    release `version` (version_stamp_problems)."""
    stamps = [(name, _stamped_versions(source)) for source, name in files if name in STAMPED]
    problems = version_stamp_problems(version, stamps)
    if problems:
        raise ValueError(
            "these binaries are not release %s:\n  %s\n  build.bat stamps them from `git describe "
            "--tags --always --dirty`: check out the tag v%s with a clean tree, run build.bat, "
            "then package." % (version, "\n  ".join(problems), version))
    print("[edvr] version stamps: %s all read v%s" % (", ".join(name for name, _ in stamps), version))


def _embedded_resource(executable, resource_id, required=True):
    """Read RCDATA with Windows datafile flags; never execute the installer."""
    if os.name != "nt":
        raise ValueError("installer resource validation requires Windows")
    import ctypes
    from ctypes import wintypes
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.LoadLibraryExW.argtypes = [wintypes.LPCWSTR, wintypes.HANDLE, wintypes.DWORD]
    kernel.LoadLibraryExW.restype = wintypes.HMODULE
    kernel.FindResourceW.argtypes = [wintypes.HMODULE, ctypes.c_void_p, ctypes.c_void_p]
    kernel.FindResourceW.restype = wintypes.HANDLE
    kernel.SizeofResource.argtypes = [wintypes.HMODULE, wintypes.HANDLE]
    kernel.SizeofResource.restype = wintypes.DWORD
    kernel.LoadResource.argtypes = [wintypes.HMODULE, wintypes.HANDLE]
    kernel.LoadResource.restype = wintypes.HANDLE
    kernel.LockResource.argtypes = [wintypes.HANDLE]
    kernel.LockResource.restype = ctypes.c_void_p
    kernel.FreeLibrary.argtypes = [wintypes.HMODULE]
    kernel.FreeLibrary.restype = wintypes.BOOL
    handle = kernel.LoadLibraryExW(str(executable), None, 0x2 | 0x20)
    if not handle:
        raise OSError(ctypes.get_last_error(), "LoadLibraryExW failed")
    try:
        resource = kernel.FindResourceW(handle, resource_id, 10)
        if not resource:
            error = ctypes.get_last_error()
            # Mandatory RCDATA is checked first; only an absent resource is an
            # expected result when probing the optional DLSS payload, or an exe
            # that carries no RCDATA at all (the launcher).
            if not required and error in (1813, 1814):  # ERROR_RESOURCE_TYPE_NOT_FOUND, ..._NAME_NOT_FOUND
                return None
            raise ValueError("installer resource %d is missing (Windows error %d)" %
                             (resource_id, error))
        size = kernel.SizeofResource(handle, resource); loaded = kernel.LoadResource(handle, resource)
        pointer = kernel.LockResource(loaded)
        if not size or not pointer: raise ValueError("installer resource %d is empty" % resource_id)
        return ctypes.string_at(pointer, size)
    finally:
        kernel.FreeLibrary(handle)


def _validate_installer_resources(executable, files):
    """The installer carries the schema-2 manifest as RCDATA 100 and each file of its `files` list as RCDATA
    101 + its index (docs/design-manifest-schema2-2026-11-12.md, "One engine"). Checked here, without
    running it: the manifest is valid and lists exactly the table's files the package carries, in the
    table's order; each entry is what the table and the release file say; each resource is byte for byte
    the release file; and no resource follows them."""
    by_name = {name: source for source, name in files}
    expected = [file for file in FILES if file.source in by_name]
    raw = _embedded_resource(executable, IDR_MANIFEST)
    try:
        manifest = json.loads(raw.decode("ascii"))
    except (UnicodeDecodeError, ValueError) as error:
        raise ValueError("installer resource %d (the manifest) is not ASCII JSON: %s" % (IDR_MANIFEST, error))
    problems = sign_manifest.validate_manifest(manifest)
    if problems:
        raise ValueError("the installer's embedded manifest is not valid: " + "; ".join(problems))
    listed = [entry["id"] for entry in manifest["files"]]
    if listed != [file.id for file in expected]:
        raise ValueError("the installer's manifest lists %s but the package carries %s"
                         % (listed, [file.id for file in expected]))
    for index, (file, entry) in enumerate(zip(expected, manifest["files"])):
        resource_id = IDR_FILE_BASE + index
        data = by_name[file.source].read_bytes()
        if _embedded_resource(executable, resource_id) != data:
            raise ValueError("installer resource %d differs from %s" % (resource_id, file.source))
        if entry != manifest_file_entry(file, EMBEDDED_ASSET, data):
            raise ValueError("the installer's manifest entry for %s differs from the table and %s"
                             % (file.id, file.source))
    after = IDR_FILE_BASE + len(expected)
    for resource_id in range(after, after + 16):
        if _embedded_resource(executable, resource_id, required=False) is not None:
            raise ValueError("installer carries resource %d, which no file of its manifest uses" % resource_id)
    if by_name["edvr_profile.ini"].read_bytes() != descriptor_bytes():
        raise ValueError("loose descriptor does not declare the packaged scope")
    if "nvngx_dlss.dll" in by_name:
        notice = by_name.get("NVIDIA-DLSS-LICENSE.txt")
        if notice is None or not notice.read_bytes().strip():
            raise ValueError("the embedded DLSS runtime requires its NVIDIA license notice")


def _write_zip_entry(output, source, name):
    if name in UNIX_EXECUTABLE:
        # zipfile.write inherits Windows' non-executable mode. Record the Unix
        # mode explicitly so extracted wrappers can also be run directly.
        info = zipfile.ZipInfo(name)
        info.create_system = 3
        info.external_attr = 0o100755 << 16
        info.compress_type = zipfile.ZIP_DEFLATED
        output.writestr(info, source.read_bytes())
    else:
        output.write(source, name)


def _split_zip_bytes(members):
    """A zip of (name, bytes) members that is the same bytes on every run: sorted
    names, a fixed date, no platform attributes, deflate level 9."""
    buffer = io.BytesIO()
    with zipfile.ZipFile(buffer, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as output:
        for name, data in sorted(members):
            info = zipfile.ZipInfo(name, (1980, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.create_system = 0
            info.external_attr = 0
            output.writestr(info, data, compresslevel=9)
    return buffer.getvalue()


def _content_asset_name(prefix, members):
    """prefix-<16 hex>.zip: the hash is taken over the sorted lines
    name, NUL, sha256 of the file, newline."""
    lines = b"".join(name.encode("ascii") + b"\0" + hashlib.sha256(data).hexdigest().encode("ascii") + b"\n"
                     for name, data in sorted(members))
    return "%s-%s.zip" % (prefix, hashlib.sha256(lines).hexdigest()[:16])


def _launcher_version(root):
    """The version string in src\\installer\\launcher_version.h: the first quoted
    x.y.z in the file."""
    text = root.joinpath(*LAUNCHER_VERSION_HEADER).read_text(encoding="utf-8")
    found = re.search(r'"([0-9]+\.[0-9]+\.[0-9]+(?:[-+][A-Za-z0-9.-]+)?)"', text)
    if not found:
        raise ValueError("no launcher version string in " + "\\".join(LAUNCHER_VERSION_HEADER))
    return found.group(1)


def _validate_launcher(root):
    """Refuse (ValueError) a release whose launcher is not one. The launcher is an asset like
    the others, but it is also what installed copies replace themselves with, so a bad one is
    worse than a missing one:

      * build\\ed-up-launcher.exe exists (build.bat writes it);
      * its FileVersion and ProductVersion are exactly src\\installer\\launcher_version.h's;
      * it carries none of the installer's payload resources, 100 to 111, and carries the min-ed-launcher
        licence as resource 120, byte for byte the file in third_party;
      * src\\installer\\update_keys.h lists two distinct keys, and neither is the public test key
        of RFC 6979, whose private half is in every library's tests."""
    launcher = root / "build" / LAUNCHER_ASSET
    if not launcher.is_file():
        raise ValueError("build\\%s is missing: build.bat writes it, and a release without a launcher "
                         "leaves installed copies with nothing to update to" % LAUNCHER_ASSET)
    version = _launcher_version(root)
    stamp = _stamped_versions(launcher) or {}
    problems = []
    for key in STAMP_KEYS:
        if stamp.get(key) != version:
            problems.append("%s %s is %s, but launcher_version.h says %s" % (LAUNCHER_ASSET, key, stamp.get(key), version))
    for resource_id in range(IDR_MANIFEST, IDR_FILE_BASE + len(FILES) + 1):
        if _embedded_resource(launcher, resource_id, required=False) is not None:
            problems.append("%s carries resource %d: the launcher has no embedded payload" % (LAUNCHER_ASSET, resource_id))
    licence = _embedded_resource(launcher, LAUNCHER_LICENSE_RESOURCE, required=False)
    licence_file = root.joinpath(*LAUNCHER_LICENSE_FILE)
    if licence is None:
        problems.append("%s does not carry resource %d, the min-ed-launcher licence" % (LAUNCHER_ASSET, LAUNCHER_LICENSE_RESOURCE))
    elif not licence_file.is_file() or licence != licence_file.read_bytes():
        problems.append("%s resource %d differs from %s" % (LAUNCHER_ASSET, LAUNCHER_LICENSE_RESOURCE, "/".join(LAUNCHER_LICENSE_FILE)))
    keys = sign_manifest.header_keys(root.joinpath(*KEYS_HEADER))
    if len(keys) != 2:
        problems.append("update_keys.h lists %d keys; a release needs the main key and the recovery key" % len(keys))
    if len(set(keys)) != len(keys):
        problems.append("update_keys.h lists the same key twice")
    if bytes.fromhex(sign_manifest.RFC_PUBLIC) in keys:
        problems.append("update_keys.h lists the public test key, so anyone could sign an update")
    if problems:
        raise ValueError("the launcher cannot be released:\n  " + "\n  ".join(problems))


def _split_outputs(root, version, files):
    """The split assets and the unsigned manifest as [(file name, bytes)], all in
    memory so a dry run builds and checks them and writes nothing. The launcher
    exe, build\\ed-up-launcher.exe, is required (_validate_launcher)."""
    present = {name: source for source, name in files}
    data = {name: present[name].read_bytes() for name in CORE_NAMES + DLSS_NAMES + XESS_NAMES
            if name in present}
    groups = [("ed-up-core-%s.zip" % version, [name for name in CORE_NAMES if name in data])]
    if DLSS_NAMES[0] in data:
        groups.append(("ed-up-dlss", [name for name in DLSS_NAMES if name in data]))
    if XESS_NAMES[0] in data:
        groups.append(("ed-up-xess", [name for name in XESS_NAMES if name in data]))
    outputs, assets, held = [], [], {}
    for name, members in groups:
        pairs = [(member, data[member]) for member in members]
        if not name.endswith(".zip"):
            name = _content_asset_name(name, pairs)
        blob = _split_zip_bytes(pairs)
        outputs.append((name, blob))
        assets.append({"name": name, "sha256": hashlib.sha256(blob).hexdigest(), "size": len(blob)})
        held.update((member, name) for member in members)
    stamp = _stamped_versions(present["d3d11.dll"]) or {}
    if not stamp.get("FileVersion"):
        raise ValueError("d3d11.dll carries no FileVersion for the manifest's build")
    manifest = {"schema": sign_manifest.MANIFEST_SCHEMA, "version": version, "build": stamp["FileVersion"],
                "tag": "v" + version,
                "issued": int(os.environ["SOURCE_DATE_EPOCH"]) if os.environ.get("SOURCE_DATE_EPOCH")
                else int(time.time())}
    launcher = root / "build" / LAUNCHER_ASSET
    if not launcher.is_file():
        raise ValueError("build\\%s is missing" % LAUNCHER_ASSET)
    blob = launcher.read_bytes()
    manifest["launcher"] = {"version": _launcher_version(root), "asset": LAUNCHER_ASSET,
                            "sha256": hashlib.sha256(blob).hexdigest(), "size": len(blob)}
    outputs.append((LAUNCHER_ASSET, blob))
    manifest["assets"] = assets
    manifest["files"] = [manifest_file_entry(file, held[file.source], data[file.source])
                         for file in FILES if file.source in held]
    problems = sign_manifest.validate_manifest(manifest)
    if problems:
        raise ValueError("the manifest is not valid:\n  " + "\n  ".join(problems))
    outputs.append((MANIFEST, (json.dumps(manifest, indent=2, ensure_ascii=True) + "\n").encode("ascii")))
    return outputs


def _sha256_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def sha256sums_bytes(entries):
    """[(published name, path holding its bytes)] -> SHA256SUMS, ASCII with LF line ends."""
    return "".join("%s  %s\n" % (_sha256_file(path), name) for name, path in entries).encode("ascii")


def _launcher_linux_members(root):
    """(source, archive name) of the launcher's Linux ZIP."""
    return [(root / "build" / LAUNCHER_ASSET, LAUNCHER_ASSET),
            (root / "tools" / "install_proton.py", LAUNCHER_LINUX_WRAPPER),
            (root / LAUNCHER_LINUX_SH, LAUNCHER_LINUX_SH),
            (root / "release" / LAUNCHER_LINUX_README, LAUNCHER_LINUX_README)]


def package(root, version, no_dlss=False, dry_run=False):
    version = _version(version); root = root.resolve()
    dist = root / "dist"; stage = dist / (".edvr-stage-" + version); final_stage = dist / ("ed-up-" + version)
    archive = dist / ("ed-up-" + version + ".zip"); temporary_archive = dist / (".edvr-" + version + ".zip.tmp")
    if not _inside(stage, dist) or not _inside(final_stage, dist) or not _inside(archive, dist):
        raise ValueError("resolved staging destination escapes repository dist")
    files = _files(root, no_dlss)
    missing = [str(src) for src, _ in files if not src.is_file()]
    if missing:
        raise ValueError("missing release payload:\n  " + "\n  ".join(missing))
    launcher_linux = _launcher_linux_members(root)
    # The launcher exe itself is checked, with a clearer message, by _validate_launcher below.
    missing = [str(src) for src, name in launcher_linux if name != LAUNCHER_ASSET and not src.is_file()]
    if missing:
        raise ValueError("missing release payload:\n  " + "\n  ".join(missing))
    wrapper = (root / "tools" / "install_proton.py").read_bytes()
    if not wrapper.startswith(b"#!/usr/bin/env python3\n") or b"\r" in wrapper:
        raise ValueError("Linux installer wrapper needs a Python 3 shebang and LF line endings")
    launcher = (root / LINUX_LAUNCHER).read_bytes()
    if not launcher.startswith(b"#!/bin/sh\n") or b"\r" in launcher:
        raise ValueError("Linux shell launcher needs a /bin/sh shebang and LF line endings")
    launcher_sh = (root / LAUNCHER_LINUX_SH).read_bytes()
    if not launcher_sh.startswith(b"#!/bin/sh\n") or b"\r" in launcher_sh:
        raise ValueError("launcher Linux shell script needs a /bin/sh shebang and LF line endings")
    _check_version_stamps(version, files)
    try:
        import openxr_pe
        openxr_pe.native_graphics_exports(str(files[0][0]))
        openxr_pe.native_exports(str(root / "build" / "edvr_openxr_runtime.dll"))
    except (ImportError, OSError, ValueError) as exc:
        raise ValueError("native payload validation failed: %s" % exc)
    try:
        from fetch_openxr_loader import verify
        verify(str(root / "build"))
    except (ImportError, OSError, ValueError) as exc:
        raise ValueError("bundled OpenXR loader validation failed: %s" % exc)
    _validate_installer_resources(root / "build" / INSTALLER, files)
    _validate_launcher(root)
    print("[edvr] package plan: %s" % archive)
    for _, name in files: print("       %s" % name)
    installer_archive = dist / ("ed-up-installer-" + version + ".zip")
    linux_archive = dist / ("ed-up-installer-linux-" + version + ".zip")
    print("[edvr] installer plan: %s (%s)" % (installer_archive, INSTALLER))
    print("[edvr] Linux installer plan: %s (%s, %s, %s, README-linux.txt)" %
          (linux_archive, INSTALLER, LINUX_WRAPPER, LINUX_LAUNCHER))
    launcher_linux_archive = dist / ("ed-up-launcher-linux-" + version + ".zip")
    print("[edvr] Linux launcher plan: %s (%s)" % (
        launcher_linux_archive, ", ".join(name for _, name in launcher_linux)))
    split = _split_outputs(root, version, files)
    for name, blob in split:
        print("[edvr] split asset plan: %s (%d bytes)" % (dist / name, len(blob)))
    sums = dist / SUMS_NAME
    print("[edvr] checksum plan: %s (%d files)" % (sums, 4 + len(split)))
    if dry_run:
        print("[edvr] dry run: wrote nothing."); return 0
    dist.mkdir(parents=True, exist_ok=True)
    if stage.exists(): shutil.rmtree(stage)
    temporary_installer_archive = dist / (".edvr-installer-" + version + ".zip.tmp")
    temporary_linux_archive = dist / (".edvr-installer-linux-" + version + ".zip.tmp")
    temporary_launcher_linux = dist / (".edvr-launcher-linux-" + version + ".zip.tmp")
    split_temporaries = [dist / (".edvr-" + name + ".tmp") for name, _ in split]
    temporary_sums = dist / (".edvr-" + SUMS_NAME + ".tmp")
    for temporary in (temporary_archive, temporary_installer_archive, temporary_linux_archive,
                      temporary_launcher_linux, *split_temporaries, temporary_sums):
        if temporary.exists(): temporary.unlink()
    try:
        for (_, blob), temporary in zip(split, split_temporaries):
            temporary.write_bytes(blob)
        for source, name in files:
            destination = stage / Path(name)
            if not _inside(destination, stage): raise ValueError("payload path traversal")
            destination.parent.mkdir(parents=True, exist_ok=True); shutil.copy2(source, destination)
        with zipfile.ZipFile(temporary_archive, "w", compression=zipfile.ZIP_DEFLATED) as output:
            for path in sorted(stage.rglob("*")):
                if path.is_file(): _write_zip_entry(output, path, path.relative_to(stage).as_posix())
        with zipfile.ZipFile(temporary_installer_archive, "w", compression=zipfile.ZIP_DEFLATED) as output:
            output.write(stage / INSTALLER, INSTALLER)
        with zipfile.ZipFile(temporary_linux_archive, "w", compression=zipfile.ZIP_DEFLATED) as output:
            for name in (INSTALLER, LINUX_WRAPPER, LINUX_LAUNCHER, "README-linux.txt"):
                _write_zip_entry(output, stage / name, name)
        with zipfile.ZipFile(temporary_launcher_linux, "w", compression=zipfile.ZIP_DEFLATED) as output:
            for source, name in launcher_linux:
                _write_zip_entry(output, source, name)
        # Hashed from the temporaries, which are moved into place unchanged below.
        sum_entries = [(archive.name, temporary_archive), (installer_archive.name, temporary_installer_archive),
                       (linux_archive.name, temporary_linux_archive),
                       (launcher_linux_archive.name, temporary_launcher_linux)]
        sum_entries += [(name, temporary) for (name, _), temporary in zip(split, split_temporaries)]
        temporary_sums.write_bytes(sha256sums_bytes(sum_entries))
    except Exception:
        for temporary in (temporary_archive, temporary_installer_archive, temporary_linux_archive,
                          temporary_launcher_linux, *split_temporaries, temporary_sums):
            if temporary.exists(): temporary.unlink()
        raise
    old_stage = dist / (".edvr-old-stage-" + version)
    archives = ((temporary_archive, archive, dist / (".edvr-old-" + version + ".zip")),
                (temporary_installer_archive, installer_archive,
                 dist / (".edvr-old-installer-" + version + ".zip")),
                (temporary_linux_archive, linux_archive,
                 dist / (".edvr-old-installer-linux-" + version + ".zip")),
                (temporary_launcher_linux, launcher_linux_archive,
                 dist / (".edvr-old-launcher-linux-" + version + ".zip")))
    archives += tuple((temporary, dist / name, dist / (".edvr-old-" + name))
                      for temporary, (name, _) in zip(split_temporaries, split))
    # Last, so SHA256SUMS is replaced only once every file it lists has been.
    archives += ((temporary_sums, sums, dist / (".edvr-old-" + SUMS_NAME)),)
    # DLSS and XeSS zips from earlier packages are named by content, so a changed
    # runtime leaves the old name behind; it goes in the same replace-or-roll-back.
    new_names = {name for name, _ in split}
    stale = sorted(path for pattern in SPLIT_PATTERNS for path in dist.glob(pattern)
                   if path.name not in new_names)
    stale_backups = [dist / (".edvr-old-stale-%d" % number) for number in range(len(stale))]
    if old_stage.exists(): shutil.rmtree(old_stage)
    for backup in [backup for _, _, backup in archives] + stale_backups:
        if backup.exists(): backup.unlink()
    if final_stage.exists(): final_stage.replace(old_stage)
    replaced, backed_up = [], []
    try:
        stage.replace(final_stage)
        for temporary, destination, backup in archives:
            if destination.exists():
                os.replace(destination, backup)
                backed_up.append((destination, backup))
            os.replace(temporary, destination)
            replaced.append(destination)
        for path, backup in zip(stale, stale_backups):
            os.replace(path, backup)
            backed_up.append((path, backup))
    except Exception:
        if final_stage.exists(): shutil.rmtree(final_stage)
        if old_stage.exists(): old_stage.replace(final_stage)
        for destination in replaced:
            destination.unlink()
        for destination, backup in backed_up:
            os.replace(backup, destination)
        for temporary, _, _ in archives:
            if temporary.exists(): temporary.unlink()
        raise
    if old_stage.exists(): shutil.rmtree(old_stage)
    for backup in [backup for _, _, backup in archives] + stale_backups:
        if backup.exists(): backup.unlink()
    for written in (archive, installer_archive, linux_archive, launcher_linux_archive,
                    *(dist / name for name, _ in split), sums):
        print("[edvr] wrote %s (%d bytes)" % (written, written.stat().st_size))
    return 0


def self_test():
    def stamp(value, product=None):
        return {"FileVersion": value, "ProductVersion": value if product is None else product}

    def refused(version, stamps, *needles):
        problems = version_stamp_problems(version, stamps)
        text = " | ".join(problems)
        assert problems and all(needle in text for needle in needles), (version, stamps, problems)
        return text

    # The release version check, on fixture strings (no binaries): the rule is
    # exactly "v<version>" or "<version>" in both FileVersion and ProductVersion.
    assert version_stamp_problems("1.2.3", [("d3d11.dll", stamp("v1.2.3")),
                                            ("openvr/openvr_api.dll", stamp("1.2.3"))]) == []
    assert version_stamp_problems("1.2.3-rc.1", [("d3d11.dll", stamp("v1.2.3-rc.1"))]) == []
    assert version_stamp_problems("1.2.3", []) == []
    text = refused("1.2.3", [("d3d11.dll", stamp("v1.2.2"))], "d3d11.dll is stamped v1.2.2",
                   "(FileVersion/ProductVersion)", "not release v1.2.3")
    assert "uncommitted" not in text
    for wrong in ("v1.2.30", "v1.2.3.1", "v1.2.3-1-g0123abc", "1.2.3-rc.1", "unknown", "", " v1.2.3"):
        refused("1.2.3", [("d3d11.dll", stamp(wrong))], "not release v1.2.3")
    refused("1.2.3-rc.1", [("d3d11.dll", stamp("v1.2.3"))], "d3d11.dll is stamped v1.2.3 ")
    for dirty in ("v1.2.3-dirty", "v1.2.3-2-g0123abc-dirty", "V1.2.3-DIRTY", "unknown-dirty"):
        text = refused("1.2.3", [("d3d11.dll", stamp(dirty))], "is stamped " + dirty, "uncommitted changes")
        assert "not release" not in text, text
    text = refused("1.2.3", [("d3d11.dll", stamp("v1.2.3", "v1.2.3-dirty"))], "(ProductVersion)", "uncommitted")
    assert "FileVersion" not in text, text
    refused("1.2.3", [("d3d11.dll", None)], "d3d11.dll carries no version stamp")
    refused("1.2.3", [("d3d11.dll", {})], "d3d11.dll carries no version stamp")
    refused("1.2.3", [("d3d11.dll", {"FileVersion": "v1.2.3"})], "d3d11.dll has no ProductVersion")
    text = refused("1.2.3", [("d3d11.dll", stamp("v1.2.3")), ("openvr/openvr_api.dll", stamp("v1.2.3-dirty")),
                             ("edvr-installer.exe", None)],
                   "openvr/openvr_api.dll is stamped", "edvr-installer.exe carries no version stamp")
    assert "d3d11.dll" not in text, "a binary that matches is not named: " + text
    if os.name == "nt":
        # The real reader: a system DLL carries strings, a file with no version
        # resource carries none.
        system = _stamped_versions(Path(os.environ.get("SystemRoot", r"C:\Windows")) / "System32" / "kernel32.dll")
        assert system and system.get("FileVersion") and system.get("ProductVersion"), system
        with tempfile.TemporaryDirectory(prefix="edvr-package-stamp-") as folder:
            bare = Path(folder) / "bare.dll"
            bare.write_bytes(b"MZ, but no version resource")
            assert _stamped_versions(bare) is None

    with tempfile.TemporaryDirectory(prefix="edvr-package-test-") as temp:
        # Windows TEMP may use an 8.3 alias; match package()'s resolved paths.
        root = Path(temp).resolve(); (root / "build").mkdir(); (root / "release").mkdir()
        for source, _ in _files(root, True):
            source.parent.mkdir(parents=True, exist_ok=True); source.write_bytes(b"payload")
        wrapper_source = root / "tools" / "install_proton.py"
        wrapper_source.write_bytes(b"#!/usr/bin/env python3\nprint('fixture')\n")
        launcher_source = root / LINUX_LAUNCHER
        launcher_source.write_bytes((ROOT / LINUX_LAUNCHER).read_bytes())
        launcher_sh_source = root / LAUNCHER_LINUX_SH
        launcher_sh_source.write_bytes((ROOT / LAUNCHER_LINUX_SH).read_bytes())
        launcher_readme_source = root / "release" / LAUNCHER_LINUX_README
        launcher_readme_source.write_bytes(b"launcher readme")
        vr_descriptor = b"[install]\r\nschema = 2\r\nscope = both\r\n"
        (root / "build" / "edvr_profile_vr.ini").write_bytes(vr_descriptor)
        # The launcher: an exe, its version header and the keys header. The release's own
        # update_keys.h is copied, so the self-test also holds the repository to two real keys.
        (root / "build" / LAUNCHER_ASSET).write_bytes(b"launcher exe")
        header = root.joinpath(*LAUNCHER_VERSION_HEADER)
        header.parent.mkdir(parents=True, exist_ok=True)
        header.write_text('inline constexpr const char* kLauncherVersion = "0.1.0";\n', encoding="ascii")
        keys_header = root.joinpath(*KEYS_HEADER)
        keys_header.write_bytes((ROOT.joinpath(*KEYS_HEADER)).read_bytes())
        real_keys_text = keys_header.read_text(encoding="utf-8")
        (root / "build" / "OPENXR-LOADER-LICENSE.txt").write_bytes(b"notice")
        import openxr_pe
        old_native, old_graphics = openxr_pe.native_exports, openxr_pe.native_graphics_exports
        old_verify = None
        import fetch_openxr_loader
        old_verify = fetch_openxr_loader.verify
        old_resource = globals()['_embedded_resource']
        # What a faithful installer built from the files now on disk carries: the manifest at 100 and each file
        # after it. refresh() recomputes it from the disk; `changes` maps a release file name to other bytes
        # for the installer to hold, or to None for the installer to lack the file.
        resources = {}

        def refresh(changes=None):
            changes = changes or {}
            on_disk = {name: source for source, name in _files(root, True) if source.is_file()}
            held = []
            for file in FILES:
                if file.source in changes:
                    data = changes[file.source]
                elif file.source in on_disk:
                    data = on_disk[file.source].read_bytes()
                else:
                    continue
                if data is not None:
                    held.append((file, data))
            resources.clear()
            resources[IDR_MANIFEST] = embedded_manifest(held, "0.0.0-test", issued=1)
            for index, (file, data) in enumerate(held):
                resources[IDR_FILE_BASE + index] = data
        refresh()
        # The launcher carries the min-ed-launcher licence as resource 120, the bytes of the file in third_party;
        # a case puts a payload resource into it, or spoils the licence.
        launcher_resources = {LAUNCHER_LICENSE_RESOURCE: root.joinpath(*LAUNCHER_LICENSE_FILE).read_bytes()}

        def fake_resource(executable, resource_id, required=True):
            if Path(executable).name == LAUNCHER_ASSET:
                return launcher_resources.get(resource_id)
            if required and resource_id not in resources:
                raise ValueError("missing fixture resource")
            return resources.get(resource_id)
        openxr_pe.native_exports = lambda path: None; openxr_pe.native_graphics_exports = lambda path: None
        fetch_openxr_loader.verify = lambda path: None
        globals()['_embedded_resource'] = fake_resource
        # The fixture binaries are not real PEs. Each reads as stamped with the
        # release being packaged, unless a case overrides one by file name; the
        # local package() below tells the reader which release that is, so the
        # older cases (every version they package is different) need no change.
        old_stamps = globals()['_stamped_versions']
        stamps = {"version": "0.0.0", "override": {}}

        def fake_stamps(path):
            if path.name in stamps["override"]:
                return stamps["override"][path.name]
            if path.name == LAUNCHER_ASSET:   # its own number, not the release's: launcher_version.h says 0.1.0
                return {"FileVersion": "0.1.0", "ProductVersion": "0.1.0"}
            return stamp("v" + stamps["version"])

        real_package = globals()['package']

        def package(root, version, *args, **kwargs):
            stamps["version"] = version
            return real_package(root, version, *args, **kwargs)

        globals()['_stamped_versions'] = fake_stamps
        try:
            assert package(root, "1.2.3", no_dlss=True, dry_run=True) == 0
            assert not (root / "dist").exists()
            try: package(root, "../escape", dry_run=True); raise AssertionError("bad version accepted")
            except ValueError: pass
            assert package(root, "1.2.3", no_dlss=True) == 0
            # SHA256SUMS lists what release.yml's rule lists (the four ZIPs, the manifest, the manifest's
            # assets and its launcher), each with its hash, in LF lines, and no .sig.
            sums_bytes = (root / "dist" / SUMS_NAME).read_bytes()
            assert sums_bytes.endswith(b"\n") and b"\r" not in sums_bytes
            listed = {}
            for line in sums_bytes.decode("ascii").splitlines():
                assert re.fullmatch(r"[0-9a-f]{64}  \S+", line), line
                digest, name = line.split("  ", 1)
                listed[name] = digest
            manifest_doc = json.loads((root / "dist" / MANIFEST).read_text(encoding="ascii"))
            expected = {name + "-1.2.3.zip" for name in
                        ("ed-up", "ed-up-installer", "ed-up-installer-linux", "ed-up-launcher-linux")}
            expected |= {MANIFEST} | {asset["name"] for asset in manifest_doc["assets"]}
            if manifest_doc.get("launcher"): expected.add(manifest_doc["launcher"]["asset"])
            assert set(listed) == expected, sorted(listed)
            for name, digest in listed.items():
                assert hashlib.sha256((root / "dist" / name).read_bytes()).hexdigest() == digest, name
            assert not [p for p in (root / "dist").iterdir() if p.name.startswith(".edvr-")]
            with zipfile.ZipFile(root / "dist" / "ed-up-installer-1.2.3.zip") as installer_archive:
                assert set(installer_archive.namelist()) == {"edvr-installer.exe"}
                assert installer_archive.read("edvr-installer.exe") == b"payload"
            with zipfile.ZipFile(root / "dist" / "ed-up-installer-linux-1.2.3.zip") as linux_archive:
                assert set(linux_archive.namelist()) == {INSTALLER, LINUX_WRAPPER, LINUX_LAUNCHER,
                                                        "README-linux.txt"}
                assert linux_archive.read(INSTALLER) == b"payload"
                assert linux_archive.read(LINUX_WRAPPER) == wrapper_source.read_bytes()
                assert linux_archive.read(LINUX_LAUNCHER) == launcher_source.read_bytes()
                assert linux_archive.read("README-linux.txt") == b"payload"
                for name in (LINUX_WRAPPER, LINUX_LAUNCHER):
                    mode = linux_archive.getinfo(name).external_attr >> 16
                    assert linux_archive.getinfo(name).create_system == 3
                    assert mode == 0o100755, oct(mode)
                if os.name != "nt":
                    extracted = root / "release with spaces"
                    linux_archive.extractall(extracted)
                    extracted_launcher = extracted / LINUX_LAUNCHER
                    extracted_launcher.chmod(mode & 0o777)
            # The launcher's Linux ZIP: the launcher exe (the Windows asset's bytes), the wrapper's bytes, the
            # shell script and the README, with Unix modes on the script and the wrapper only.
            with zipfile.ZipFile(root / "dist" / "ed-up-launcher-linux-1.2.3.zip") as launcher_archive:
                assert set(launcher_archive.namelist()) == {LAUNCHER_ASSET, LAUNCHER_LINUX_WRAPPER,
                                                           LAUNCHER_LINUX_SH, LAUNCHER_LINUX_README}
                assert launcher_archive.read(LAUNCHER_ASSET) == b"launcher exe"
                assert launcher_archive.read(LAUNCHER_ASSET) == (root / "dist" / LAUNCHER_ASSET).read_bytes()
                assert launcher_archive.read(LAUNCHER_LINUX_WRAPPER) == wrapper_source.read_bytes()
                assert launcher_archive.read(LAUNCHER_LINUX_SH) == launcher_sh_source.read_bytes()
                assert launcher_archive.read(LAUNCHER_LINUX_README) == b"launcher readme"
                for name in (LAUNCHER_LINUX_WRAPPER, LAUNCHER_LINUX_SH):
                    assert launcher_archive.getinfo(name).create_system == 3
                    assert launcher_archive.getinfo(name).external_attr >> 16 == 0o100755
                assert launcher_archive.getinfo(LAUNCHER_ASSET).external_attr >> 16 != 0o100755
                if os.name != "nt":
                    launcher_extracted = root / "launcher release with spaces"
                    launcher_archive.extractall(launcher_extracted)
                    (launcher_extracted / LAUNCHER_LINUX_SH).chmod(0o755)
            if os.name != "nt":
                fake_bin = root / "fake bin"
                fake_bin.mkdir()
                fake_python = fake_bin / "python3"
                fake_python.write_bytes(b"#!/bin/sh\nprintf '%s\\n' \"$@\" > \"$EDVR_ARGS_FILE\"\nexit 37\n")
                fake_python.chmod(0o755)
                elsewhere = root / "other cwd"
                elsewhere.mkdir()
                args_file = root / "arguments"
                env = dict(os.environ, PATH=str(fake_bin), EDVR_ARGS_FILE=str(args_file))

                def check_launch(script, wrapper, args):
                    result = subprocess.run([str(script), *args], cwd=elsewhere, env=env,
                                            capture_output=True, check=False)
                    assert result.returncode == 37, result.stderr
                    assert args_file.read_bytes().splitlines() == [str(wrapper).encode(),
                                                                    *(arg.encode() for arg in args)]

                check_launch(extracted_launcher, extracted / LINUX_WRAPPER, [])
                check_launch(extracted_launcher, extracted / LINUX_WRAPPER,
                             ["--preview", "--installer", "a path/with spaces.exe", ""])
                source_dir = root / "source with spaces"
                (source_dir / "tools").mkdir(parents=True)
                source_launcher = source_dir / LINUX_LAUNCHER
                shutil.copy2(launcher_source, source_launcher)
                source_launcher.chmod(0o755)
                source_wrapper = source_dir / "tools" / "install_proton.py"
                shutil.copy2(wrapper_source, source_wrapper)
                check_launch(source_launcher, source_wrapper, ["--dry-run"])
                check_launch(source_launcher, source_wrapper, ["--self-test"])
                source_wrapper.unlink()
                result = subprocess.run([str(source_launcher)], cwd=elsewhere, env=env,
                                        capture_output=True, check=False)
                assert result.returncode != 0 and b"Python wrapper missing" in result.stderr
                result = subprocess.run([str(extracted_launcher)], cwd=elsewhere,
                                        env=dict(env, PATH=""), capture_output=True, check=False)
                assert result.returncode != 0 and b"python3 is required" in result.stderr
                # The launcher script puts --launcher before the player's own arguments.
                launcher_script = launcher_extracted / LAUNCHER_LINUX_SH
                launcher_wrapper = launcher_extracted / LAUNCHER_LINUX_WRAPPER
                for args in ([], ["--dry-run", "--", "--vr", "a b"]):
                    result = subprocess.run([str(launcher_script), *args], cwd=elsewhere, env=env,
                                            capture_output=True, check=False)
                    assert result.returncode == 37, result.stderr
                    assert args_file.read_bytes().splitlines() == [
                        str(launcher_wrapper).encode(), b"--launcher", *(arg.encode() for arg in args)]
                launcher_source_dir = root / "launcher source with spaces"
                (launcher_source_dir / "tools").mkdir(parents=True)
                source_sh = launcher_source_dir / LAUNCHER_LINUX_SH
                shutil.copy2(launcher_sh_source, source_sh)
                source_sh.chmod(0o755)
                shutil.copy2(wrapper_source, launcher_source_dir / "tools" / "install_proton.py")
                result = subprocess.run([str(source_sh), "--dry-run"], cwd=elsewhere, env=env,
                                        capture_output=True, check=False)
                assert result.returncode == 37 and args_file.read_bytes().splitlines() == [
                    str(launcher_source_dir / "tools" / "install_proton.py").encode(), b"--launcher", b"--dry-run"]
                (launcher_source_dir / "tools" / "install_proton.py").unlink()
                result = subprocess.run([str(source_sh)], cwd=elsewhere, env=env,
                                        capture_output=True, check=False)
                assert result.returncode != 0 and b"Python wrapper missing" in result.stderr
                result = subprocess.run([str(launcher_script)], cwd=elsewhere,
                                        env=dict(env, PATH=""), capture_output=True, check=False)
                assert result.returncode != 0 and b"python3 is required" in result.stderr
            assert not (root / "dist" / "ed-up-installer-1.2.3.exe").exists()
            artifacts = [root / "dist" / (name + "-1.2.3.zip") for name in
                         ("ed-up", "ed-up-installer", "ed-up-installer-linux", "ed-up-launcher-linux")]
            artifacts.append(root / "dist" / SUMS_NAME)
            old_archives = {path: path.read_bytes() for path in artifacts}
            for missing_file in (wrapper_source, launcher_source, root / "release" / "README-linux.txt",
                                 launcher_sh_source, launcher_readme_source):
                saved = missing_file.read_bytes()
                missing_file.unlink()
                for dry_run in (True, False):
                    try:
                        package(root, "1.2.3", no_dlss=True, dry_run=dry_run)
                        raise AssertionError("missing Linux release payload accepted")
                    except ValueError as error:
                        assert str(missing_file) in str(error), str(error)
                assert all(path.read_bytes() == old_archives[path] for path in artifacts)
                assert not (root / "dist" / ".edvr-stage-1.2.3").exists()
                missing_file.write_bytes(saved)
            launcher_source.write_bytes(b"#!/bin/sh\r\nexit 0\r\n")
            for dry_run in (True, False):
                try:
                    package(root, "1.2.3", no_dlss=True, dry_run=dry_run)
                    raise AssertionError("CRLF shell launcher accepted")
                except ValueError as error:
                    assert "shell launcher" in str(error), str(error)
            launcher_source.write_bytes((ROOT / LINUX_LAUNCHER).read_bytes())
            launcher_sh_source.write_bytes(b"#!/bin/sh\r\nexit 0\r\n")
            for dry_run in (True, False):
                try:
                    package(root, "1.2.3", no_dlss=True, dry_run=dry_run)
                    raise AssertionError("CRLF launcher shell script accepted")
                except ValueError as error:
                    assert "launcher Linux shell script" in str(error), str(error)
            launcher_sh_source.write_bytes((ROOT / LAUNCHER_LINUX_SH).read_bytes())
            assert all(path.read_bytes() == old_archives[path] for path in artifacts)
            assert not (root / "dist" / ".edvr-stage-1.2.3").exists()
            old_replace = os.replace
            linux_temporary = root / "dist" / ".edvr-installer-linux-1.2.3.zip.tmp"
            linux_destination = root / "dist" / "ed-up-installer-linux-1.2.3.zip"

            def fail_linux_publish(source, destination):
                if Path(source) == linux_temporary and Path(destination) == linux_destination:
                    raise OSError("simulated Linux archive publish failure")
                return old_replace(source, destination)

            os.replace = fail_linux_publish
            try:
                try:
                    package(root, "1.2.3", no_dlss=True)
                    raise AssertionError("failed Linux publish accepted")
                except OSError as error:
                    assert "simulated Linux archive publish failure" in str(error)
            finally:
                os.replace = old_replace
            assert all(path.read_bytes() == old_archives[path] for path in artifacts)
            assert not linux_temporary.exists()
            # The launcher's ZIP is published last of the four, so a failure there rolls the other three back.
            launcher_temporary = root / "dist" / ".edvr-launcher-linux-1.2.3.zip.tmp"
            launcher_destination = root / "dist" / "ed-up-launcher-linux-1.2.3.zip"

            def fail_launcher_publish(source, destination):
                if Path(source) == launcher_temporary and Path(destination) == launcher_destination:
                    raise OSError("simulated launcher archive publish failure")
                return old_replace(source, destination)

            os.replace = fail_launcher_publish
            try:
                try:
                    package(root, "1.2.3", no_dlss=True)
                    raise AssertionError("failed launcher publish accepted")
                except OSError as error:
                    assert "simulated launcher archive publish failure" in str(error)
            finally:
                os.replace = old_replace
            assert all(path.read_bytes() == old_archives[path] for path in artifacts)
            assert not launcher_temporary.exists()
            assert not [p for p in (root / "dist").iterdir() if p.name.startswith(".edvr-old")]
            (root / "build" / "edvr_openxr_runtime.dll").unlink()
            try:
                package(root, "1.2.3", no_dlss=True)
                raise AssertionError("missing native payload accepted")
            except ValueError:
                pass
            assert all(path.read_bytes() == old_archives[path] for path in artifacts)
            with zipfile.ZipFile(root / "dist" / "ed-up-1.2.3.zip") as archive:
                names = set(archive.namelist())
                assert names == {"d3d11.dll", "edvr_profile.ini", "openvr/openvr_api.dll", "openvr/openxr_loader.dll",
                                 "openvr/OPENXR-LOADER-LICENSE.txt", "edvr-installer.exe",
                                 "README.txt", "openvr/READ-ME-FIRST.txt", "edvr.ini", "edvr-flat.ini",
                                  "README-linux.txt", LINUX_WRAPPER, LINUX_LAUNCHER,
                                  "LICENSE.txt", "DXBC-HASH-LICENSE.txt", "MINIZ-LICENSE.txt", LAUNCHER_LICENSE_NAME}
                assert archive.read(LINUX_WRAPPER) == wrapper_source.read_bytes()
                assert archive.getinfo(LINUX_WRAPPER).external_attr >> 16 == 0o100755
                assert archive.read(LINUX_LAUNCHER) == launcher_source.read_bytes()
                assert archive.getinfo(LINUX_LAUNCHER).external_attr >> 16 == 0o100755
                with zipfile.ZipFile(root / "dist" / "ed-up-installer-linux-1.2.3.zip") as linux_archive:
                    assert archive.read(INSTALLER) == linux_archive.read(INSTALLER)
            (root / "build" / "edvr_openxr_runtime.dll").write_bytes(b"payload")
            dlss = root / "build" / "nvngx_dlss.dll"
            notice = root / "build" / "NVIDIA-DLSS-LICENSE.txt"
            refresh({"nvngx_dlss.dll": b"embedded-dlss"})
            try:
                package(root, "1.2.4", no_dlss=True)
                raise AssertionError("embedded runtime accepted without its loose payload")
            except ValueError:
                pass
            assert not (root / "dist" / ".edvr-stage-1.2.4").exists()
            dlss.write_bytes(b"embedded-dlss")
            try:
                package(root, "1.2.4", no_dlss=True)
                raise AssertionError("embedded runtime accepted without its notice")
            except ValueError:
                pass
            notice.write_bytes(b"NVIDIA runtime notice")
            assert package(root, "1.2.4", no_dlss=True) == 0
            with zipfile.ZipFile(root / "dist" / "ed-up-1.2.4.zip") as archive:
                assert archive.read("nvngx_dlss.dll") == b"embedded-dlss"
                assert archive.read("NVIDIA-DLSS-LICENSE.txt") == notice.read_bytes()
            for bad in (None, b"different-installer-runtime"):
                refresh({"nvngx_dlss.dll": bad})
                try:
                    package(root, "1.2.5", no_dlss=True)
                    raise AssertionError("mismatched embedded runtime accepted")
                except ValueError:
                    pass
            refresh()
            notice.write_bytes(b" \n")
            try:
                package(root, "1.2.5", no_dlss=True)
                raise AssertionError("empty DLSS notice accepted")
            except ValueError:
                pass
            # AMD's FSR3 port notice: absent from every archive above (no
            # such file in the fixture build), carried when build.bat has put
            # one there. The port links statically, so this notice IS the
            # release's only trace of it.
            notice.write_bytes(b"NVIDIA runtime notice")
            ffx = root / "build" / "FIDELITYFX-SDK-DX11-LICENSE.txt"
            assert not ffx.exists()
            ffx.write_bytes(b"MIT, FidelityFX SDK DX11 port")
            assert package(root, "1.2.6", no_dlss=True) == 0
            with zipfile.ZipFile(root / "dist" / "ed-up-1.2.6.zip") as archive:
                assert archive.read("FIDELITYFX-SDK-DX11-LICENSE.txt") == ffx.read_bytes()
            ffx.unlink()
            assert package(root, "1.2.7", no_dlss=True) == 0
            with zipfile.ZipFile(root / "dist" / "ed-up-1.2.7.zip") as archive:
                assert "FIDELITYFX-SDK-DX11-LICENSE.txt" not in set(archive.namelist())

            # The release version check through package(): read before anything is
            # staged, so a refused release writes nothing -- dry run or not -- and
            # the message names the binary and what it is stamped.
            graphics, runtime = "edvr_openxr_graphics.dll", "edvr_openxr_runtime.dll"
            for label, override, needle in (
                    ("a dirty DLL", {graphics: stamp("v3.0.0-dirty")},
                     "d3d11.dll is stamped v3.0.0-dirty"),
                    ("a runtime from another release", {runtime: stamp("v2.9.9")},
                     "openvr/openvr_api.dll is stamped v2.9.9"),
                    ("a dev-build installer", {"edvr-installer.exe": stamp("v3.0.0-4-g0123abc")},
                     "edvr-installer.exe is stamped v3.0.0-4-g0123abc"),
                    ("an unstamped DLL", {graphics: None}, "d3d11.dll carries no version stamp")):
                stamps["override"] = {name: value for name, value in override.items()}
                for dry_run in (True, False):
                    try:
                        package(root, "3.0.0", no_dlss=True, dry_run=dry_run)
                        raise AssertionError("%s accepted (dry_run=%s)" % (label, dry_run))
                    except ValueError as error:
                        assert needle in str(error) and "3.0.0" in str(error), (label, str(error))
                assert not (root / "dist" / "ed-up-3.0.0.zip").exists(), "a refused release wrote: " + label
                assert not (root / "dist" / "ed-up-installer-linux-3.0.0.zip").exists()
                assert not (root / "dist" / "ed-up-launcher-linux-3.0.0.zip").exists()
                assert not (root / "dist" / ".edvr-launcher-linux-3.0.0.zip.tmp").exists()
                assert not (root / "dist" / ".edvr-stage-3.0.0").exists(), "a refused release staged: " + label
                assert not (root / "dist" / "ed-up-3.0.0").exists(), "a refused release staged: " + label
            stamps["override"] = {}
            assert package(root, "3.0.0", no_dlss=True, dry_run=True) == 0
            assert not (root / "dist" / "ed-up-3.0.0.zip").exists()
            assert not (root / "dist" / "ed-up-installer-linux-3.0.0.zip").exists()
            assert not (root / "dist" / ".edvr-installer-linux-3.0.0.zip.tmp").exists()
            assert not (root / "dist" / "ed-up-launcher-linux-3.0.0.zip").exists()
            assert not (root / "dist" / ".edvr-launcher-linux-3.0.0.zip.tmp").exists()
            assert package(root, "3.0.0", no_dlss=True) == 0 and (root / "dist" / "ed-up-3.0.0.zip").is_file()

            # One release, one installer executable: there is no flat archive,
            # and --profile is no longer an option. The VR archive carries both settings files.
            with zipfile.ZipFile(root / "dist" / "ed-up-1.2.7.zip") as archive:
                assert "edvr-flat.ini" in set(archive.namelist()) and "edvr.ini" in set(archive.namelist())
            import contextlib, io
            refusal = io.StringIO()  # argparse's usage text: expected, so not echoed raw
            try:
                with contextlib.redirect_stderr(refusal):
                    main(["1.2.8", "--root", str(root), "--profile", "flat"])
                raise AssertionError("--profile flat accepted")
            except SystemExit as error:
                assert error.code == 2
                assert "unrecognized arguments: --profile flat" in refusal.getvalue()
                print("expected-error: argparse refused --profile flat (exit 2)")
            assert not (root / "dist" / "ed-up-flat-1.2.8.zip").exists()
            # The installer's own copy of each settings file is checked against that
            # file, under its own name: edvr-flat.ini is not edvr.ini.
            refresh({"edvr-flat.ini": b"[fix]\r\ntemporal_aa = dlss\r\n"})
            try:
                package(root, "1.2.8", no_dlss=True)
                raise AssertionError("installer embedding other flat settings than edvr-flat.ini accepted")
            except ValueError as error:
                assert "edvr-flat.ini" in str(error), str(error)
            refresh({"edvr-flat.ini": None})
            try:
                package(root, "1.2.8", no_dlss=True)
                raise AssertionError("installer without edvr-flat.ini accepted")
            except ValueError:
                pass
            # A file in the installer that the table does not list, or one more resource than the manifest lists.
            refresh()
            resources[IDR_FILE_BASE + len([1 for key in resources if key >= IDR_FILE_BASE])] = b"stray"
            try:
                package(root, "1.2.8", no_dlss=True)
                raise AssertionError("installer with a stray resource accepted")
            except ValueError as error:
                assert "no file of its manifest uses" in str(error), str(error)
            refresh()
            with zipfile.ZipFile(root / "dist" / "ed-up-installer-1.2.7.zip") as installer_archive:
                assert set(installer_archive.namelist()) == {"edvr-installer.exe"}
            xess_files = {108: "libxess.dll", 109: "INTEL-XESS-LICENSE.txt",
                          110: "INTEL-XESS-THIRD-PARTY.txt"}
            for resource_id, name in xess_files.items():
                contents = ("XeSS fixture " + name).encode("ascii")
                (root / "build" / name).write_bytes(contents)
            refresh()
            assert package(root, "3.0.3", no_dlss=True) == 0
            with zipfile.ZipFile(root / "dist" / "ed-up-3.0.3.zip") as archive:
                for name in xess_files.values():
                    assert archive.read(name) == (root / "build" / name).read_bytes()
            refresh({"libxess.dll": b"wrong XeSS"})
            try:
                package(root, "3.0.4", no_dlss=True)
                raise AssertionError("installer with mismatched XeSS runtime accepted")
            except ValueError:
                pass

            # The split assets and the unsigned manifest, beside the three archives.
            refresh()
            old_epoch = os.environ.get("SOURCE_DATE_EPOCH")
            os.environ["SOURCE_DATE_EPOCH"] = "1794000000"
            dist = root / "dist"
            dlss_names = {"nvngx_dlss.dll", "NVIDIA-DLSS-LICENSE.txt"}
            xess_names = set(xess_files.values())
            core_names = {"d3d11.dll", "openvr/openvr_api.dll", "openvr/openxr_loader.dll",
                          "openvr/OPENXR-LOADER-LICENSE.txt", "edvr.ini", "edvr-flat.ini",
                          "edvr_profile.ini", "LICENSE.txt", "DXBC-HASH-LICENSE.txt", "MINIZ-LICENSE.txt", LAUNCHER_LICENSE_NAME}

            def snapshot():
                return {path.relative_to(dist).as_posix(): (path.read_bytes() if path.is_file() else None)
                        for path in dist.rglob("*")}

            def split_assets(directory):
                return {kind: sorted(p.name for p in directory.glob("ed-up-%s-*.zip" % kind))
                        for kind in ("dlss", "xess")}

            def members(path):
                with zipfile.ZipFile(path) as opened:
                    return set(opened.namelist())

            before = snapshot()
            assert package(root, "4.0.0", no_dlss=True, dry_run=True) == 0
            assert snapshot() == before, "a dry run wrote under dist"
            assert package(root, "4.0.0", no_dlss=True) == 0
            core = dist / "ed-up-core-4.0.0.zip"
            found = split_assets(dist)
            assert len(found["dlss"]) == 1 and len(found["xess"]) == 1, found
            dlss_zip, xess_zip = dist / found["dlss"][0], dist / found["xess"][0]
            assert members(core) == core_names, members(core)
            assert members(dlss_zip) == dlss_names and members(xess_zip) == xess_names
            assert not list(dist.rglob("*.sig")), "packaging wrote a signature"
            assert not [p for p in dist.iterdir() if p.name.startswith(".")], "a temporary was left"
            # The three archives keep their member lists.
            with zipfile.ZipFile(dist / "ed-up-4.0.0.zip") as archive:
                assert set(archive.namelist()) == core_names | dlss_names | xess_names | {
                    INSTALLER, "README.txt", "openvr/READ-ME-FIRST.txt", "README-linux.txt",
                    LINUX_WRAPPER, LINUX_LAUNCHER}
            assert members(dist / "ed-up-installer-4.0.0.zip") == {INSTALLER}
            assert members(dist / "ed-up-installer-linux-4.0.0.zip") == {
                INSTALLER, LINUX_WRAPPER, LINUX_LAUNCHER, "README-linux.txt"}
            assert members(dist / "ed-up-launcher-linux-4.0.0.zip") == {
                LAUNCHER_ASSET, LAUNCHER_LINUX_WRAPPER, LAUNCHER_LINUX_SH, LAUNCHER_LINUX_README}
            # The manifest: valid, ASCII, LF, the real hashes, and the launcher the release carries.
            manifest_bytes = (dist / MANIFEST).read_bytes()
            assert manifest_bytes.decode("ascii") and b"\r" not in manifest_bytes and manifest_bytes.endswith(b"\n")
            parsed, problems = sign_manifest.load_manifest(manifest_bytes)
            assert problems == [], problems
            assert (parsed["schema"], parsed["version"], parsed["build"], parsed["tag"], parsed["issued"]) == (
                sign_manifest.MANIFEST_SCHEMA, "4.0.0", "v4.0.0", "v4.0.0", 1794000000)
            assert "min_launcher" not in parsed
            assert parsed["launcher"] == {"version": "0.1.0", "asset": LAUNCHER_ASSET,
                                          "sha256": hashlib.sha256(b"launcher exe").hexdigest(), "size": 12}
            assert (dist / LAUNCHER_ASSET).read_bytes() == b"launcher exe"
            assert [asset["name"] for asset in parsed["assets"]] == [core.name, dlss_zip.name, xess_zip.name]
            assert [entry["id"] for entry in parsed["files"]] == [file.id for file in FILES]
            assert "edvr_profile.ini" not in {entry["path"] for entry in parsed["files"]}
            by_id = {entry["id"]: entry for entry in parsed["files"]}
            assert by_id["graphics"]["existing"] == "chain" and by_id["graphics"]["when"] == {"profile": ["vr", "flat"]}
            assert by_id["runtime"]["target"] == {"root": "openvr", "path": "openvr_api.dll"}
            assert by_id["dlss"]["when"] == {"adapter": "nvidia"} and "when" not in by_id["xess"]
            lines = []
            assert sign_manifest.verify_directory(dist, allow_unsigned=True, out=lines.append), lines
            dlss = next(entry for entry in parsed["files"] if entry["id"] == "dlss")
            assert dlss["asset"] == dlss_zip.name and dlss["path"] == "nvngx_dlss.dll"
            # Two runs give the same bytes, and a new version keeps the DLSS and XeSS names.
            first = {path: path.read_bytes() for path in (core, dlss_zip, xess_zip, dist / MANIFEST)}
            assert package(root, "4.0.0", no_dlss=True) == 0
            assert all(path.read_bytes() == data for path, data in first.items()), "packaging is not deterministic"
            assert package(root, "4.0.1", no_dlss=True) == 0
            assert split_assets(dist) == found and (dist / "ed-up-core-4.0.1.zip").is_file()
            assert dlss_zip.read_bytes() == first[dlss_zip] and xess_zip.read_bytes() == first[xess_zip]
            assert json.loads((dist / MANIFEST).read_text(encoding="ascii"))["version"] == "4.0.1"
            # A changed runtime gets a new name and the old one goes.
            dlss_dll = root / "build" / "nvngx_dlss.dll"
            kept = dlss_dll.read_bytes()
            dlss_dll.write_bytes(b"another DLSS runtime"); refresh()
            assert package(root, "4.0.1", no_dlss=True) == 0
            renamed = split_assets(dist)
            assert len(renamed["dlss"]) == 1 and renamed["dlss"] != found["dlss"] and renamed["xess"] == found["xess"]
            assert sign_manifest.verify_directory(dist, allow_unsigned=True, out=lambda line: None)
            # No DLSS runtime in the tree: no DLSS asset and no dlss file.
            dlss_dll.unlink(); notice.unlink(); refresh()
            assert package(root, "4.0.2", no_dlss=True) == 0
            assert split_assets(dist)["dlss"] == [] and split_assets(dist)["xess"] == found["xess"]
            parsed = json.loads((dist / MANIFEST).read_text(encoding="ascii"))
            assert "dlss" not in {entry["id"] for entry in parsed["files"]}
            assert all("dlss" not in asset["name"] for asset in parsed["assets"])
            assert sign_manifest.verify_directory(dist, allow_unsigned=True, out=lambda line: None)
            with zipfile.ZipFile(dist / "ed-up-4.0.2.zip") as archive:
                assert not dlss_names & set(archive.namelist())
            dlss_dll.write_bytes(kept); refresh()
            notice.write_bytes(b"NVIDIA runtime notice")
            # A failed publish puts the earlier split assets back.
            before = snapshot()
            old_replace = os.replace
            target = dist / "ed-up-core-4.0.3.zip"

            def fail_core_publish(source, destination):
                if Path(destination) == target:
                    raise OSError("simulated split asset publish failure")
                return old_replace(source, destination)

            os.replace = fail_core_publish
            try:
                try:
                    package(root, "4.0.3", no_dlss=True)
                    raise AssertionError("failed split publish accepted")
                except OSError as error:
                    assert "simulated split asset" in str(error)
            finally:
                os.replace = old_replace
            after = snapshot()
            assert {k: v for k, v in after.items() if not k.startswith("ed-up-4.0.3")
                    and not k.startswith("ed-up-installer")} == \
                {k: v for k, v in before.items() if not k.startswith("ed-up-4.0.3")
                 and not k.startswith("ed-up-installer")}, "a failed publish changed dist"
            assert not target.exists() and not [p for p in dist.iterdir() if p.name.startswith(".")]
            # The launcher exe is listed and copied.
            assert package(root, "4.0.4", no_dlss=True) == 0
            parsed = json.loads((dist / MANIFEST).read_text(encoding="ascii"))
            assert parsed["launcher"] == {"version": "0.1.0", "asset": LAUNCHER_ASSET,
                                          "sha256": hashlib.sha256(b"launcher exe").hexdigest(), "size": 12}
            assert (dist / LAUNCHER_ASSET).read_bytes() == b"launcher exe"
            assert sign_manifest.verify_directory(dist, allow_unsigned=True, out=lambda line: None)
            header.write_text("// no version here\n", encoding="ascii")
            try:
                package(root, "4.0.5", no_dlss=True, dry_run=True)
                raise AssertionError("launcher without a version accepted")
            except ValueError as error:
                assert "launcher version" in str(error)
            header.write_text('inline constexpr const char* kLauncherVersion = "0.1.0";\n', encoding="ascii")

            # The launcher is required, and a bad one is refused, each for its own reason.
            def refused_launcher(label, needle):
                try:
                    package(root, "4.0.6", no_dlss=True, dry_run=True)
                except ValueError as error:
                    assert needle in str(error), (label, str(error))
                    return str(error)
                raise AssertionError("accepted: " + label)

            launcher_exe = root / "build" / LAUNCHER_ASSET
            launcher_exe.unlink()
            print("expected-error: " + refused_launcher("no launcher", "ed-up-launcher.exe is missing").splitlines()[0])
            launcher_exe.write_bytes(b"launcher exe")
            stamps["override"][LAUNCHER_ASSET] = {"FileVersion": "0.0.9", "ProductVersion": "0.1.0"}
            print("expected-error: " + refused_launcher("stale launcher version", "FileVersion is 0.0.9").splitlines()[1].strip())
            stamps["override"][LAUNCHER_ASSET] = None
            refused_launcher("launcher without a version resource", "FileVersion is None")
            del stamps["override"][LAUNCHER_ASSET]
            for resource_id in (100, 101, 102, 111):
                launcher_resources[resource_id] = b"payload"
                refused_launcher("launcher with resource %d" % resource_id, "carries resource %d" % resource_id)
                del launcher_resources[resource_id]
            good_licence = launcher_resources.pop(LAUNCHER_LICENSE_RESOURCE)
            refused_launcher("launcher without the licence", "does not carry resource 120")
            launcher_resources[LAUNCHER_LICENSE_RESOURCE] = good_licence + b" "
            refused_launcher("launcher with another licence", "resource 120 differs from third_party/min_ed_launcher/LICENSE")
            launcher_resources[LAUNCHER_LICENSE_RESOURCE] = good_licence
            rfc = sign_manifest.RFC_PUBLIC.lower()
            real_pair = [k for k in re.findall(r'"([0-9a-f]{128})"', real_keys_text)]
            assert len(real_pair) == 2 and real_pair[0] != rfc and real_pair[1] != rfc, "the repository's own keys"
            keys_header.write_text('inline constexpr const char* kUpdatePublicKeys[] = {"%s", "%s", nullptr};\n'
                                   % (real_pair[0], rfc), encoding="ascii")
            print("expected-error: " + refused_launcher("the test key", "public test key").splitlines()[1].strip())
            keys_header.write_text('inline constexpr const char* kUpdatePublicKeys[] = {"%s", nullptr};\n' % real_pair[0],
                                   encoding="ascii")
            refused_launcher("one key", "lists 1 keys")
            keys_header.write_text('inline constexpr const char* kUpdatePublicKeys[] = {"%s", "%s", nullptr};\n'
                                   % (real_pair[0], real_pair[0]), encoding="ascii")
            refused_launcher("the same key twice", "same key twice")
            keys_header.write_text(real_keys_text, encoding="utf-8")
            assert package(root, "4.0.6", no_dlss=True, dry_run=True) == 0   # and the good one passes again

            # Every table entry is a legal target and the table passes the manifest's own rules, on a
            # manifest of nothing but the table.
            legal = {"schema": sign_manifest.MANIFEST_SCHEMA, "version": "1.0.0", "build": "b", "tag": "t",
                     "issued": 1,
                     "assets": [{"name": name + ".zip", "sha256": "0" * 64, "size": 1}
                                for name in ("core", "dlss", "xess")],
                     "files": [manifest_file_entry(file, file.group + ".zip", b"x") for file in FILES]}
            assert sign_manifest.validate_manifest(legal) == [], sign_manifest.validate_manifest(legal)
            assert len({file.id for file in FILES}) == len(FILES)
            for file in FILES:
                assert sign_manifest.target_path_problem(file.id, file.path) is None, file
                assert file.root in sign_manifest.ROOTS and file.existing in sign_manifest.EXISTING, file
                assert file.group in _EXTRA_MEMBERS, file
            # The schema number is the one the C++ parser reads.
            found = re.search(r"kManifestSchema\s*=\s*(\d+)\s*;", ROOT.joinpath(
                "src", "installer", "install_manifest.h").read_text(encoding="utf-8"))
            assert found and int(found.group(1)) == sign_manifest.MANIFEST_SCHEMA, \
                "install_manifest.h's kManifestSchema and sign_manifest.MANIFEST_SCHEMA differ"
            if old_epoch is None:
                del os.environ["SOURCE_DATE_EPOCH"]
            else:
                os.environ["SOURCE_DATE_EPOCH"] = old_epoch
        finally:
            openxr_pe.native_exports, openxr_pe.native_graphics_exports = old_native, old_graphics
            fetch_openxr_loader.verify = old_verify
            globals()['_embedded_resource'] = old_resource
            globals()['_stamped_versions'] = old_stamps
    print("package_native: self-test passed"); return 0


def check_installer(root):
    """Run after linking, so stale outputs cannot fail the early self-test."""
    root = root.resolve()
    executable = root / "build" / INSTALLER
    _validate_installer_resources(executable, _files(root, True))
    if _embedded_resource(executable, 65535, required=False) is not None:
        raise ValueError("unexpected resource 65535 in the native installer")
    print("package_native: actual installer resources match the release files")
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("version", nargs="?"); parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument("--no-dlss", action="store_true",
                        help="allow a build without DLSS; retain the DLL and notice when embedded")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--check-installer", action="store_true",
                        help="verify the built installer's actual resources without executing it")
    args = parser.parse_args(argv)
    if args.self_test: return self_test()
    if args.check_installer: return check_installer(args.root)
    return package(args.root, args.version, args.no_dlss, args.dry_run)


if __name__ == "__main__":
    try: raise SystemExit(main())
    except (OSError, ValueError, zipfile.BadZipFile) as exc:
        print("[edvr] packaging failed: %s" % exc); raise SystemExit(1)
