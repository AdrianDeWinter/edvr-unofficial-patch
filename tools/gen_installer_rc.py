#!/usr/bin/env python3
"""Generate the build's resource scripts, version header and release settings files.

What it writes, one mode per run:

  * --version-header: edvr_version.h, the build's version string as an include.
  * --version-rc graphics|runtime: the VERSIONINFO resource of d3d11.dll and
    of openvr_api.dll.
  * --launcher-rc: launcher.rc for ed-up-launcher.exe (manifest, icon, its own
    VERSIONINFO from src/installer/launcher_version.h, and the min-ed-launcher
    licence). The launcher carries no embedded payload.
  * --release-files: build\\edvr-flat.ini and build\\edvr_profile_vr.ini, the two
    files the release packs that nothing else makes. The native graphics and
    runtime DLLs, the OpenXR loader and its notice, and any XeSS notices are
    checked first, so a build that is missing one fails here and not at
    packaging.

Usage:
  python tools/gen_installer_rc.py --release-files --build <build dir>
  python tools/gen_installer_rc.py --launcher-rc --root <repo> --out <gen dir>
  python tools/gen_installer_rc.py --version-rc graphics --out <gen dir> --version <string>
  python tools/gen_installer_rc.py --version-header --out <gen dir> --version <string>
"""

import argparse
import os
import re
import struct
import subprocess
import sys

# Resource ids of an embedded payload, as src/installer/payload.cpp reads one: the schema-2 manifest is RCDATA 100 and
# each file of its `files` list is RCDATA 101 + its index in that list (docs/design-manifest-schema2-2026-11-12.md,
# "One engine"). Nothing here writes them any more; package_native.py uses them to refuse a launcher that carries any.
IDR_MANIFEST = 100
IDR_FILE_BASE = 101
RT_MANIFEST = 24


def flat_ini_bytes():
    """The shipped edvr-flat.ini: the flat profile's own default settings."""
    return (b'# Experimental flat temporal profile: visual qualification in progress.\r\n'
            b'# F8 opens the AA menu: Off / TAA / DLSS / FSR3 / FSR4 / DittoSR / XeSS-SR and DLSS model presets.\r\n'
            b'# temporal_aa: off, on (TAA), dlaa (native SS), dlss, fsr, fsr4, dittosr, xess. Game SS controls render scale.\r\n'
            b'# XeSS-SR needs libxess.dll beside the game; the launcher installs it when the release was built with the SDK.\r\n'
            b'# Press NumLock while the issue is visible to collect a diagnostic capture.\r\n'
            b'[fix]\r\ntemporal_aa = off\r\ntemporal_aa_model = k\r\nui_quality = 100\r\n\r\n'
            b'[visual]\r\n'
            b'# intro_video: screen plays the launch movie as the game makes it; skip refuses it at launch.\r\n'
            b'# moved-from: fix.intro_video (default screen)\r\n'
            b'intro_video = screen\r\n\r\n'
            b'# compare_aa: F7 turns AA off to compare and back; runtime only, nothing is written.\r\n'
            b'[hotkey]\r\nmenu = F8\r\ncompare_aa = F7\r\ndump_draws = NUMLOCK\r\n\r\n'
            b'[diag]\r\n'
            b'# Temporary F1 origin/source/optical/world capture. Restart the game after changing this value.\r\n'
            b'# Does not change the production origin-ambiguity reset. Flat startup only.\r\n'
            b'flat_origin_capture = off\r\n\r\n'
            b'[log]\r\nenabled = 1\r\n\r\n'
            b'[advanced]\r\n'
            b'# The mod EDVR chains through (EDHM, ReShade) is set once, in edvr.ini, and applies to the flat profile too.\r\n'
            b'# Leave this empty to use that. A file name here overrides it for flat; none runs flat without the chain.\r\n'
            b'real_dll =\r\n')


def descriptor_bytes():
    """The edvr_profile.ini the release's zips carry, schema 2: scope both, the one the
    install engine's default names. package_native.py checks the loose descriptor in the
    release against these same bytes; the planner writes its own copy of the text."""
    return b'[install]\r\nschema = 2\r\nscope = both\r\n'


def parse_version_tuple(describe):
    """Parse a `git describe` string into a numeric (major, minor, patch,
    commits) tuple for FILEVERSION/PRODUCTVERSION.

    'v0.17.0-rc.2-16-g2c6e820' -> (0, 17, 0, 16): the trailing '-<n>-g<hash>'
    `git describe` appends past the tag is the commit count since it; a
    '-rc.N' pre-release suffix and a trailing '-dirty' are not fields
    VERSIONINFO has room for and are dropped either way. A bare tag
    ('v0.16.2', 'v0.17.0-rc.3') has no '-<n>-g<hash>' suffix and reads as
    zero commits since it. Anything that does not start with a dotted
    major.minor.patch -- 'unknown' (no repo) included -- is unparsable and
    becomes the all-zero version Windows shows for "no version". Every field
    is clamped to 0..65535, VERSIONINFO's numeric field width.
    """
    m = re.match(r'^v?(\d+)\.(\d+)\.(\d+)(?:-.*)?$', describe or '')
    if not m:
        return (0, 0, 0, 0)
    major, minor, patch = (int(m.group(i)) for i in (1, 2, 3))
    tail = re.search(r'-(\d+)-g[0-9a-f]+(?:-dirty)?$', describe)
    commits = int(tail.group(1)) if tail else 0

    def clamp(n):
        return max(0, min(65535, n))
    return tuple(clamp(n) for n in (major, minor, patch, commits))


def origin_url_https():
    """The origin remote as an https://github.com/... link with no trailing
    .git, for a Comments string that gives someone skeptical of an unsigned
    binary somewhere to check it. `git remote get-url origin` rather than a
    hand-typed constant for the same reason EDVR_VER is `git describe`: a
    repo that moves keeps a hand-typed URL that is quietly wrong. No git, no
    repo, or no `origin` remote falls back to the project's known home rather
    than failing a build over a Comments string.
    """
    fallback = 'https://github.com/characterecho-sean/ed-unofficial-patch'
    try:
        url = subprocess.check_output(
            ['git', 'remote', 'get-url', 'origin'],
            stderr=subprocess.DEVNULL, timeout=10,
        ).decode('utf-8', 'replace').strip()
    except Exception:
        return fallback
    if url.endswith('.git'):
        url = url[:-len('.git')]
    m = re.match(r'^git@([^:]+):(.+)$', url)
    if m:
        return 'https://%s/%s' % (m.group(1), m.group(2))
    return url if url.startswith('http://') or url.startswith('https://') else fallback


def versioninfo_lines(version, file_description, internal_name, original_filename,
                      file_type, comments):
    """The VERSIONINFO block text, the single writer for it: every .rc file
    this module produces -- the launcher's launcher.rc and the two
    DLLs' own standalone version_*.rc -- calls this rather than keeping its
    own copy of the eleven lines of StringFileInfo boilerplate.
    """
    major, minor, patch, commits = parse_version_tuple(version)
    return [
        '1 VERSIONINFO',
        'FILEVERSION %d,%d,%d,%d' % (major, minor, patch, commits),
        'PRODUCTVERSION %d,%d,%d,%d' % (major, minor, patch, commits),
        'FILEFLAGSMASK 0x3fL',
        'FILEFLAGS 0x0L',
        'FILEOS 0x40004L',
        'FILETYPE %s' % file_type,
        'FILESUBTYPE 0x0L',
        'BEGIN',
        '    BLOCK "StringFileInfo"',
        '    BEGIN',
        '        BLOCK "040904b0"',
        '        BEGIN',
        '            VALUE "CompanyName", "ED-UP (unofficial)"',
        '            VALUE "FileDescription", "%s"' % file_description,
        '            VALUE "FileVersion", "%s"' % version,
        '            VALUE "InternalName", "%s"' % internal_name,
        '            VALUE "OriginalFilename", "%s"' % original_filename,
        '            VALUE "ProductName", "ED Unofficial Patch"',
        '            VALUE "ProductVersion", "%s"' % version,
        '            VALUE "LegalCopyright", "Copyright (c) 2026 characterecho-sean. MIT License."',
        '            VALUE "Comments", "%s"' % comments,
        '        END',
        '    END',
        '    BLOCK "VarFileInfo"',
        '    BEGIN',
        '        VALUE "Translation", 0x409, 1200',
        '    END',
        'END',
    ]


LAUNCHER_VERSION_HEADER = ('src', 'installer', 'launcher_version.h')
# min-ed-launcher's MIT licence, which direct launch's ported code obliges the launcher to carry.
# src/installer/launcher_main.cpp reads this id; package_native.py checks the bytes.
IDR_MIN_ED_LAUNCHER_LICENSE = 120
LAUNCHER_LICENSE_FILE = ('third_party', 'min_ed_launcher', 'LICENSE')
LAUNCHER_COMMENTS = 'Source and releases: https://github.com/characterecho-sean/ed-unofficial-patch'


def launcher_version(root):
    """The version in src/installer/launcher_version.h: the first quoted x.y.z. It is the
    launcher's own number, not `git describe`: the launcher's bytes then depend on its
    sources and nothing else, so a release that did not change it ships the same exe."""
    with open(os.path.join(root, *LAUNCHER_VERSION_HEADER), encoding='utf-8') as stream:
        text = stream.read()
    found = re.search(r'"([0-9]+\.[0-9]+\.[0-9]+(?:[-+][A-Za-z0-9.-]+)?)"', text)
    if not found:
        raise ValueError('no launcher version string in ' + '\\'.join(LAUNCHER_VERSION_HEADER))
    return found.group(1)


def write_launcher_rc(root, out, dry_run):
    """--launcher-rc: launcher.rc with the manifest, the icon, a VERSIONINFO and one RCDATA, the
    licence of the code direct launch is ported from (id 120, printed by --licenses). The launcher
    carries no payload (package_native.py refuses one that does, resources 100 to 111), and nothing in the
    file depends on the build's `git describe` or on the clock, so two builds of the same sources
    are the same bytes."""
    manifest = os.path.join(root, 'src', 'installer', 'installer.manifest')
    license_path = os.path.join(root, *LAUNCHER_LICENSE_FILE)
    try:
        version = launcher_version(root)
    except (OSError, ValueError) as exc:
        print('gen_installer_rc: ERROR: %s' % exc)
        return 1
    if not os.path.isfile(license_path):
        print('gen_installer_rc: ERROR: %s is missing: the launcher carries it' % license_path)
        return 1
    try:
        import xml.etree.ElementTree as ElementTree
        ElementTree.parse(manifest)
    except Exception as exc:  # noqa: BLE001 -- any parse failure is fatal here
        print('gen_installer_rc: ERROR: %s is not valid XML: %s' % (manifest, exc))
        return 1
    if dry_run:
        print('gen_installer_rc: dry run (launcher %s); wrote nothing' % version)
        return 0
    os.makedirs(out, exist_ok=True)
    icon_path = os.path.join(out, 'edvr_installer.ico')
    with open(icon_path, 'wb') as f:
        f.write(ico_bytes())
    lines = [
        '// Generated by tools/gen_installer_rc.py. Do not edit; do not commit.',
        '',
        '1 %d "%s"' % (RT_MANIFEST, rc_path(manifest)),
        '1 ICON "%s"' % rc_path(icon_path),
        '%d RCDATA "%s"' % (IDR_MIN_ED_LAUNCHER_LICENSE, rc_path(license_path)),
        '',
    ] + versioninfo_lines(
        version,
        file_description='ED-UP Launcher',
        internal_name='ed-up-launcher',
        original_filename='ed-up-launcher.exe',
        file_type='0x1L',
        comments=LAUNCHER_COMMENTS) + ['']
    out_path = os.path.join(out, 'launcher.rc')
    with open(out_path, 'w', encoding='utf-8', newline='\r\n') as f:
        f.write('\n'.join(lines))
    print('gen_installer_rc: wrote %s (launcher %s, no embedded payload, the min-ed-launcher licence)' % (out_path, version))
    return 0


def write_version_rc(which, version, out, dry_run):
    """--version-rc graphics|runtime: a standalone .rc holding only a
    VERSIONINFO, for the two DLLs that otherwise ship with no resource naming
    them at all. No payload, no manifest, no icon, no --root/--build
    validation: a file that carries nothing but version strings needs none.
    """
    comments = 'Source and releases: %s' % origin_url_https()
    if which == 'graphics':
        lines = versioninfo_lines(
            version,
            file_description=('ED Unofficial Patch for Elite Dangerous: Direct3D 11 '
                              'proxy (forwards to the system d3d11.dll)'),
            internal_name='edvr_openxr_graphics',
            original_filename='d3d11.dll',
            file_type='0x2L',
            comments=comments)
        filename = 'version_graphics.rc'
    else:
        lines = versioninfo_lines(
            version,
            file_description=('ED Unofficial Patch for Elite Dangerous: OpenXR runtime '
                              'speaking the OpenVR ABI'),
            internal_name='edvr_openxr_runtime',
            original_filename='openvr_api.dll',
            file_type='0x2L',
            comments=comments)
        filename = 'version_runtime.rc'

    if dry_run:
        print('gen_installer_rc: dry run (%s); wrote nothing' % which)
        return 0
    os.makedirs(out, exist_ok=True)
    out_path = os.path.join(out, filename)
    header = ['// Generated by tools/gen_installer_rc.py. Do not edit; do not commit.', '']
    with open(out_path, 'w', encoding='utf-8', newline='\r\n') as f:
        f.write('\n'.join(header + lines + ['']))
    print('gen_installer_rc: wrote %s' % out_path)
    return 0


def c_escape(text):
    """A string as the body of a C string literal: backslash and double quote."""
    return text.replace('\\', '\\\\').replace('"', '\\"')


def version_header_text(version):
    """The text of edvr_version.h for this version string."""
    return ('// Generated by tools/gen_installer_rc.py. Do not edit; do not commit.\n'
            '#pragma once\n'
            '#define EDVR_VERSION_STRING "%s"\n' % c_escape(version))


def write_version_header(version, out, dry_run):
    """--version-header: edvr_version.h, so the version is an include rather
    than a /D flag on every translation unit. A flag changes with every commit
    and recompiles everything; the header is rewritten only when its content
    differs, so an unchanged version leaves the file (and its mtime) alone.
    """
    out_path = os.path.join(out, 'edvr_version.h')
    text = version_header_text(version)
    data = text.encode('utf-8')
    try:
        with open(out_path, 'rb') as f:
            same = f.read() == data
    except OSError:
        same = False
    if same:
        print('gen_installer_rc: unchanged %s' % out_path)
        return 0
    if dry_run:
        print('gen_installer_rc: dry run (version header); wrote nothing')
        return 0
    os.makedirs(out, exist_ok=True)
    with open(out_path, 'wb') as f:
        f.write(data)
    print('gen_installer_rc: wrote %s' % out_path)
    return 0


def rc_path(path):
    """A path as an .rc string.

    Forward slashes: inside a quoted rc string a backslash is an escape, so
    "C:\\dir\\file.dll" would have to be doubled. rc.exe accepts forward
    slashes on Windows and this way there is nothing to double.
    """
    return os.path.abspath(path).replace('\\', '/')


def draw_icon(size):
    """One icon image, as BGRA rows bottom-up: two overlapping stereo circles.

    Drawn rather than committed so the repository stays free of binary assets
    for something this small. The mark is the same idea as the fixes: two eyes
    that should agree, overlapping.
    """
    px = [[(0, 0, 0, 0)] * size for _ in range(size)]

    def blend(dst, src):
        sa = src[3] / 255.0
        if sa <= 0:
            return dst
        out = []
        for i in range(3):
            out.append(int(round(src[i] * sa + dst[i] * (1 - sa))))
        out.append(max(dst[3], src[3]))
        return tuple(out)

    r = size * 0.5
    # Rounded background square.
    radius = size * 0.22
    for y in range(size):
        for x in range(size):
            cx = min(max(x + 0.5, radius), size - radius)
            cy = min(max(y + 0.5, radius), size - radius)
            dx, dy = x + 0.5 - cx, y + 0.5 - cy
            if dx * dx + dy * dy <= radius * radius + 0.5:
                px[y][x] = (28, 22, 18, 255)  # BGRA: near-black navy

    def circle(ox, oy, rad, colour):
        for y in range(size):
            for x in range(size):
                dx, dy = x + 0.5 - ox, y + 0.5 - oy
                d = (dx * dx + dy * dy) ** 0.5
                if d <= rad:
                    # A soft edge so small sizes do not look ragged.
                    a = 255 if d <= rad - 1 else int(255 * (rad - d))
                    if a > 0:
                        px[y][x] = blend(px[y][x], (colour[0], colour[1], colour[2], a))

    eye = size * 0.235
    circle(r - size * 0.145, r, eye, (255, 175, 87))    # BGR: cyan-blue #57afff
    circle(r + size * 0.145, r, eye, (67, 159, 255))    # BGR: amber #ff9f43
    return px


def ico_bytes():
    """A .ico holding 16, 32 and 48 pixel images."""
    images = []
    for size in (16, 32, 48):
        px = draw_icon(size)
        # BITMAPINFOHEADER with doubled height, then BGRA bottom-up, then the
        # AND mask (all zero: the alpha channel carries transparency).
        header = struct.pack('<IiiHHIIiiII', 40, size, size * 2, 1, 32, 0, size * size * 4,
                             0, 0, 0, 0)
        body = bytearray()
        for y in range(size - 1, -1, -1):
            for x in range(size):
                b, g, r, a = px[y][x]
                body += struct.pack('<BBBB', b, g, r, a)
        mask_row = ((size + 31) // 32) * 4
        body += bytes(mask_row * size)
        images.append((size, header + bytes(body)))

    out = bytearray(struct.pack('<HHH', 0, 1, len(images)))
    offset = 6 + 16 * len(images)
    for size, data in images:
        out += struct.pack('<BBBBHHII', size % 256, size % 256, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
    for _size, data in images:
        out += data
    return bytes(out)


def write_release_files(build, dry_run):
    """--release-files: check what the release packs from `build`, then write the two files it
    packs that nothing else makes: edvr-flat.ini (the flat profile's own settings) and
    edvr_profile_vr.ini (the profile descriptor the zips carry as edvr_profile.ini).

    The native graphics and runtime DLLs and the OpenXR loader with its notice are mandatory, and
    the DLLs and the loader are validated here, so a build that lacks one fails now and not at
    packaging. XeSS runtimes are optional, but one without its two Intel notices is refused."""
    d3d11 = os.path.join(build, 'edvr_openxr_graphics.dll')
    runtime = os.path.join(build, 'edvr_openxr_runtime.dll')
    loader = os.path.join(build, 'openxr_loader.dll')
    notice = os.path.join(build, 'OPENXR-LOADER-LICENSE.txt')
    required = [("native graphics", d3d11), ("native runtime", runtime), ("OpenXR loader", loader),
                ("OpenXR loader notice", notice)]
    missing = [(label, path) for label, path in required if not os.path.isfile(path)]
    if missing:
        for label, path in missing:
            print('gen_installer_rc: ERROR: %s (%s) is not there; build it first' % (label, path))
        return 1
    try:
        import openxr_pe
        openxr_pe.native_graphics_exports(d3d11)
        openxr_pe.native_exports(runtime)
        from fetch_openxr_loader import verify
        verify(build)
    except (ImportError, OSError, ValueError) as exc:
        print('gen_installer_rc: ERROR: native payload validation failed: %s' % exc)
        return 1

    # XeSS-SR, XeSS Frame Generation and Xe Low Latency all ship under the same two Intel notices.
    any_xess = any(os.path.exists(os.path.join(build, name)) for name in ('libxess.dll', 'libxess_fg.dll', 'libxell.dll'))
    if any_xess and (not os.path.isfile(os.path.join(build, 'INTEL-XESS-LICENSE.txt'))
                     or not os.path.isfile(os.path.join(build, 'INTEL-XESS-THIRD-PARTY.txt'))):
        print('gen_installer_rc: ERROR: XeSS runtime lacks license notices')
        return 1

    if dry_run:
        print('gen_installer_rc: dry run; validated the release inputs and wrote nothing')
        return 0
    with open(os.path.join(build, 'edvr-flat.ini'), 'wb') as f:
        f.write(flat_ini_bytes())
    with open(os.path.join(build, 'edvr_profile_vr.ini'), 'wb') as f:
        f.write(descriptor_bytes())
    print('gen_installer_rc: wrote edvr-flat.ini and edvr_profile_vr.ini in %s' % build)
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument('--root')
    ap.add_argument('--build')
    ap.add_argument('--out')
    ap.add_argument('--version', default='unknown')
    ap.add_argument('--release-files', action='store_true',
                    help='check the native pair, the loader and any XeSS notices in --build, then write '
                         'edvr-flat.ini and edvr_profile_vr.ini there')
    ap.add_argument('--version-rc', choices=('graphics', 'runtime'),
                    help='write a standalone VERSIONINFO .rc for this DLL')
    ap.add_argument('--launcher-rc', action='store_true',
                    help='write launcher.rc (manifest, icon and VERSIONINFO from '
                         'src/installer/launcher_version.h, no payload) into --out; needs --root')
    ap.add_argument('--version-header', action='store_true',
                    help='write edvr_version.h (#define EDVR_VERSION_STRING) into --out, '
                         'only when its content changes')
    ap.add_argument('--dry-run', action='store_true')
    ap.add_argument('--self-test', action='store_true')
    args = ap.parse_args(argv)
    if args.self_test:
        return self_test()
    if args.launcher_rc:
        if not args.out or not args.root:
            ap.error('--root and --out are required with --launcher-rc')
        return write_launcher_rc(args.root, args.out, args.dry_run)
    if args.version_header:
        if not args.out:
            ap.error('--out is required with --version-header')
        return write_version_header(args.version, args.out, args.dry_run)
    if args.version_rc:
        if not args.out:
            ap.error('--out is required with --version-rc')
        return write_version_rc(args.version_rc, args.version, args.out, args.dry_run)
    if args.release_files:
        if not args.build:
            ap.error('--build is required with --release-files')
        return write_release_files(args.build, args.dry_run)
    ap.error('one of --release-files, --version-rc, --launcher-rc or --version-header is required')


def self_test():
    import tempfile

    # The describe-string -> numeric-tuple parser, used for FILEVERSION and
    # PRODUCTVERSION in every VERSIONINFO block this module writes.
    assert parse_version_tuple('v0.17.0-rc.2-16-g2c6e820') == (0, 17, 0, 16)
    assert parse_version_tuple('v0.16.2') == (0, 16, 2, 0)
    assert parse_version_tuple('v0.17.0-rc.3') == (0, 17, 0, 0)
    assert parse_version_tuple('unknown') == (0, 0, 0, 0)
    assert parse_version_tuple('garbage') == (0, 0, 0, 0)
    assert parse_version_tuple('v1.2.3-70000-gabcdef1') == (1, 2, 3, 65535)  # clamped
    assert parse_version_tuple('0.1.0') == (0, 1, 0, 0)  # the launcher's own number has no "v" and no suffix

    with tempfile.TemporaryDirectory(prefix='edvr-rc-test-') as scratch:
        root = os.path.join(scratch, 'root'); build = os.path.join(root, 'build')
        os.makedirs(build)
        for name in ('edvr_openxr_graphics.dll', 'edvr_openxr_runtime.dll', 'openxr_loader.dll',
                     'OPENXR-LOADER-LICENSE.txt'):
            with open(os.path.join(build, name), 'wb') as stream: stream.write(b'x')
        import openxr_pe, fetch_openxr_loader
        old_g, old_r, old_v = openxr_pe.native_graphics_exports, openxr_pe.native_exports, fetch_openxr_loader.verify
        openxr_pe.native_graphics_exports = lambda path: None; openxr_pe.native_exports = lambda path: None
        fetch_openxr_loader.verify = lambda path: None
        try:
            # --release-files: a dry run checks and writes nothing.
            assert main(['--release-files', '--build', build, '--dry-run']) == 0
            assert not os.path.exists(os.path.join(build, 'edvr-flat.ini'))
            assert not os.path.exists(os.path.join(build, 'edvr_profile_vr.ini'))
            os.remove(os.path.join(build, 'edvr_openxr_runtime.dll'))
            import contextlib, io
            missing = io.StringIO()  # the refusal under test, relabelled below
            with contextlib.redirect_stdout(missing), contextlib.redirect_stderr(missing):
                code = main(['--release-files', '--build', build])
            assert code == 1 and not os.path.exists(os.path.join(build, 'edvr-flat.ini'))
            assert 'ERROR: native runtime' in missing.getvalue()
            print('expected-error: ' + missing.getvalue().strip().replace('gen_installer_rc: ERROR: ', '', 1))
            # The flat edition is retired: --profile is no longer an option at all.
            with open(os.path.join(build, 'edvr_openxr_runtime.dll'), 'wb') as stream: stream.write(b'x')
            refusal = io.StringIO()  # argparse's usage text: expected, so not echoed raw
            try:
                with contextlib.redirect_stderr(refusal):
                    main(['--release-files', '--build', build, '--profile', 'flat'])
            except SystemExit as exc:
                assert exc.code == 2
                assert 'unrecognized arguments: --profile flat' in refusal.getvalue()
                print('expected-error: argparse refused --profile flat (exit 2)')
            else:
                raise AssertionError('--profile flat was accepted')
            # An XeSS runtime without its Intel notices is refused; with them it is accepted.
            with open(os.path.join(build, 'libxess.dll'), 'wb') as stream: stream.write(b'libxess.dll')
            no_notices = io.StringIO()
            with contextlib.redirect_stdout(no_notices):
                assert main(['--release-files', '--build', build]) == 1
            assert 'XeSS runtime lacks license notices' in no_notices.getvalue()
            assert not os.path.exists(os.path.join(build, 'edvr-flat.ini'))
            print('expected-error: ' + no_notices.getvalue().strip().replace('gen_installer_rc: ERROR: ', '', 1))
            for name in ('INTEL-XESS-LICENSE.txt', 'INTEL-XESS-THIRD-PARTY.txt', 'libxess_fg.dll', 'libxell.dll'):
                with open(os.path.join(build, name), 'wb') as stream: stream.write(name.encode('ascii'))
            assert main(['--release-files', '--build', build]) == 0
            with open(os.path.join(build, 'edvr-flat.ini'), 'rb') as stream:
                flat_ini = stream.read()
                assert flat_ini == flat_ini_bytes()
                assert b'temporal_aa = off' in flat_ini
                assert b'[diag]\r\n' in flat_ini and b'flat_origin_capture = off\r\n' in flat_ini
                assert b'Restart the game after changing this value.' in flat_ini
                assert b'fsr, fsr4, dittosr, xess. Game SS controls render scale.' in flat_ini
                assert b'FSR3 / FSR4 / DittoSR / XeSS-SR' in flat_ini
                assert b'XeSS-SR needs libxess.dll' in flat_ini
                assert b'temporal_aa_model = k' in flat_ini and b'menu = F8' in flat_ini
                assert b'dump_draws = NUMLOCK' in flat_ini and b'dump_draws = F10' not in flat_ini
                assert b'compare_aa = F7' in flat_ini and b'nothing is written' in flat_ini
                assert b'intro_video = screen' in flat_ini and b'skip refuses it at launch' in flat_ini
                # The key lives in [visual] (2026-10-22), and the annotation sits directly above it so the install engine's merge
                # carries a flat user's old [fix] intro_video to the new name (iniedit.cpp movedKeys: a section line or a blank
                # clears a pending annotation).
                assert (b'[visual]\r\n# intro_video: screen plays the launch movie as the game makes it; skip refuses it at launch.\r\n'
                        b'# moved-from: fix.intro_video (default screen)\r\nintro_video = screen\r\n') in flat_ini
                assert b'[fix]\r\ntemporal_aa = off\r\ntemporal_aa_model = k\r\nui_quality = 100\r\n\r\n' in flat_ini
            with open(os.path.join(build, 'edvr_profile_vr.ini'), 'rb') as stream:
                assert stream.read() == descriptor_bytes()
            assert descriptor_bytes() == b'[install]\r\nschema = 2\r\nscope = both\r\n'
            # The flat edition's README is folded into release\README.txt; the build
            # writes no second README.
            assert not os.path.exists(os.path.join(build, 'edvr-flat-README.txt'))
            # No mode named: argparse refuses, so a call that forgot its flag cannot pass for a write.
            nothing = io.StringIO()
            try:
                with contextlib.redirect_stderr(nothing):
                    main(['--build', build])
            except SystemExit as exc:
                assert exc.code == 2 and 'one of --release-files' in nothing.getvalue()
            else:
                raise AssertionError('a call with no mode was accepted')
        finally:
            openxr_pe.native_graphics_exports, openxr_pe.native_exports = old_g, old_r
            fetch_openxr_loader.verify = old_v

        # --version-rc: a standalone VERSIONINFO .rc, no native payload involved.
        rc_out = os.path.join(root, 'rc_out')
        assert main(['--version-rc', 'graphics', '--version', 'v0.17.0-rc.2-16-g2c6e820',
                     '--out', rc_out, '--dry-run']) == 0 and not os.path.exists(rc_out)
        assert main(['--version-rc', 'graphics', '--version', 'v0.17.0-rc.2-16-g2c6e820',
                     '--out', rc_out]) == 0
        with open(os.path.join(rc_out, 'version_graphics.rc'), 'r', encoding='utf-8') as stream:
            graphics_text = stream.read()
        assert 'OriginalFilename", "d3d11.dll"' in graphics_text
        assert 'FILEVERSION 0,17,0,16' in graphics_text

        assert main(['--version-rc', 'runtime', '--version', 'v0.16.2', '--out', rc_out]) == 0
        with open(os.path.join(rc_out, 'version_runtime.rc'), 'r', encoding='utf-8') as stream:
            runtime_text = stream.read()
        assert 'OriginalFilename", "openvr_api.dll"' in runtime_text
        assert 'FILEVERSION 0,16,2,0' in runtime_text

        # --launcher-rc: the launcher's own version, a manifest and an icon, and no payload.
        lroot = os.path.join(scratch, 'lroot')
        os.makedirs(os.path.join(lroot, 'src', 'installer'))
        with open(os.path.join(lroot, 'src', 'installer', 'installer.manifest'), 'w') as stream:
            stream.write('<assembly></assembly>')
        header = os.path.join(lroot, 'src', 'installer', 'launcher_version.h')
        with open(header, 'w') as stream:
            stream.write('#pragma once\n#define EDUP_LAUNCHER_VERSION "0.1.0"\n')
        lout = os.path.join(lroot, 'out')
        # The licence the launcher carries is required: without it there is nothing to embed.
        import contextlib, io
        no_license = io.StringIO()
        with contextlib.redirect_stdout(no_license):
            assert main(['--launcher-rc', '--root', lroot, '--out', lout]) == 1
        assert 'min_ed_launcher' in no_license.getvalue() and not os.path.exists(lout)
        os.makedirs(os.path.join(lroot, 'third_party', 'min_ed_launcher'))
        with open(os.path.join(lroot, 'third_party', 'min_ed_launcher', 'LICENSE'), 'w') as stream:
            stream.write('MIT License\n')
        assert main(['--launcher-rc', '--root', lroot, '--out', lout, '--dry-run']) == 0 and not os.path.exists(lout)
        assert main(['--launcher-rc', '--root', lroot, '--out', lout, '--version', 'v9.9.9-5-gabc-dirty']) == 0
        with open(os.path.join(lout, 'launcher.rc'), encoding='utf-8') as stream:
            launcher_text = stream.read()
        assert 'OriginalFilename", "ed-up-launcher.exe"' in launcher_text
        assert 'FileDescription", "ED-UP Launcher"' in launcher_text
        assert 'FileVersion", "0.1.0"' in launcher_text and 'ProductVersion", "0.1.0"' in launcher_text
        assert 'FILEVERSION 0,1,0,0' in launcher_text
        # No payload, only the licence as 120, and not the build's describe.
        assert launcher_text.count('RCDATA') == 1 and '120 RCDATA "' in launcher_text and 'LICENSE"' in launcher_text
        assert '9.9.9' not in launcher_text
        assert '24 "' in launcher_text and 'ICON' in launcher_text
        with open(os.path.join(lout, 'launcher.rc'), 'rb') as stream:
            first = stream.read()
        assert main(['--launcher-rc', '--root', lroot, '--out', lout, '--version', 'unknown']) == 0
        with open(os.path.join(lout, 'launcher.rc'), 'rb') as stream:
            assert stream.read() == first  # the build's version never reaches it
        with open(header, 'w') as stream:
            stream.write('#pragma once\n#define EDUP_LAUNCHER_VERSION "0.2.0"\n')
        assert main(['--launcher-rc', '--root', lroot, '--out', lout]) == 0
        with open(os.path.join(lout, 'launcher.rc'), encoding='utf-8') as stream:
            assert 'FILEVERSION 0,2,0,0' in stream.read()  # it follows launcher_version.h
        with open(header, 'w') as stream:
            stream.write('#pragma once\n')
        silent = io.StringIO()
        with contextlib.redirect_stdout(silent):
            assert main(['--launcher-rc', '--root', lroot, '--out', lout]) == 1
        assert 'no launcher version string' in silent.getvalue()
        print('expected-error: ' + silent.getvalue().strip().replace('gen_installer_rc: ERROR: ', '', 1))

        # --version-header: escaped define, rewritten only when the version changes.
        vh_out = os.path.join(root, 'vh_out')
        vh_path = os.path.join(vh_out, 'edvr_version.h')
        assert main(['--version-header', '--version', 'v1', '--out', vh_out,
                     '--dry-run']) == 0 and not os.path.exists(vh_out)
        assert main(['--version-header', '--version', 'v1.0-3-gabc-dirty',
                     '--out', vh_out]) == 0
        with open(vh_path, 'rb') as stream:
            vh_bytes = stream.read()
        assert b'#pragma once' in vh_bytes
        assert b'#define EDVR_VERSION_STRING "v1.0-3-gabc-dirty"\n' in vh_bytes
        os.utime(vh_path, (1000000000, 1000000000))
        assert main(['--version-header', '--version', 'v1.0-3-gabc-dirty',
                     '--out', vh_out]) == 0
        assert os.stat(vh_path).st_mtime == 1000000000   # unchanged: not rewritten
        assert main(['--version-header', '--version', 'v1.0-4-gdef',
                     '--out', vh_out]) == 0
        assert os.stat(vh_path).st_mtime != 1000000000
        with open(vh_path, 'rb') as stream:
            assert b'"v1.0-4-gdef"' in stream.read()
        assert main(['--version-header', '--version', 'a"b\\c', '--out', vh_out]) == 0
        with open(vh_path, 'rb') as stream:
            assert b'#define EDVR_VERSION_STRING "a\\"b\\\\c"\n' in stream.read()
    print('gen_installer_rc: self-test passed')
    return 0


if __name__ == '__main__':
    sys.exit(main())
