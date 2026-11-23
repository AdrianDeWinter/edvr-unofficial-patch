"""Fetch and verify ED-UP's pinned tailored 2DMigoto EDHM runtime.

    python tools/fetch_edhm_2dmigoto.py [--dest DIR] [--dry-run]
    python tools/fetch_edhm_2dmigoto.py --verify DIR
    python tools/fetch_edhm_2dmigoto.py --self-test

Downloads the tailored 2DMigoto d3d11_edhm.dll runtime and its GPLv3, PCRE2
and DirectXTK licence/notice files from Fenris159/EDHM_2DMigoto at one pinned
release tag and commit. Every file is checked against its pinned SHA-256 and
the DLL is checked as an x64 PE image before anything is moved into place.

Default destination is %LOCALAPPDATA%\\EDVR\\edhm-2dmigoto, shared by
checkouts; pass --dest third_party\\edhm_2dmigoto for a self-contained
checkout. --dry-run is strictly read-only.
"""
import argparse
import hashlib
import os
import shutil
import struct
import sys
import tempfile
import urllib.request
from pathlib import Path

REPO = "Fenris159/EDHM_2DMigoto"
VERSION = "0.1.8"
TAG = "v" + VERSION
COMMIT = "2cbeaa381c57f4848144d3a1510ed34f8f41f0e5"
RELEASE_URL = "https://github.com/%s/releases/download/%s" % (REPO, TAG)
RAW_URL = "https://raw.githubusercontent.com/%s/%s" % (REPO, COMMIT)
SOURCE_TREE_URL = "https://github.com/%s/tree/%s" % (REPO, COMMIT)
SOURCE_ARCHIVE_ASSET = "EDHM_2DMigoto-%s-source.tar.gz" % TAG
SOURCE_ARCHIVE_URL = RELEASE_URL + "/" + SOURCE_ARCHIVE_ASSET
SOURCE_ARCHIVE_SHA256 = "17bfffe33ff43439a72e003c87c530dfcf07aa4c19950fa3e8d027a57a4e1401"
SOURCE_ARCHIVE_NAME = "ed-up-edhm-source-%s.tar.gz" % VERSION

# relative staged path -> (download url, pinned lowercase SHA-256)
FILES = {
    "bin/d3d11_edhm.dll": (
        RELEASE_URL + "/d3d11_edhm.dll",
        "3480813a4ecffce90c7b29e57b62788ea0b1a6aff5be06120cdf237fea054eb4",
    ),
    SOURCE_ARCHIVE_NAME: (
        SOURCE_ARCHIVE_URL,
        SOURCE_ARCHIVE_SHA256,
    ),
    "LICENSE.GPL.txt": (
        RAW_URL + "/LICENSE.GPL.txt",
        "8ceb4b9ee5adedde47b31e975c1d90c73ad27b6b165a1dcd80c7c545eb65b903",
    ),
    "COPYING.txt": (
        RAW_URL + "/COPYING.txt",
        "89a11e354ee1a3789230baa7ff739df3d66f9563ee6246047030b1b33595c6bb",
    ),
    "AUTHORS.txt": (
        RAW_URL + "/AUTHORS.txt",
        "24d325a6aaef77e1b43436cab23a2cbb054f4fb325df679953c1886282271848",
    ),
    "THIRD_PARTY_NOTICES.md": (
        RAW_URL + "/THIRD_PARTY_NOTICES.md",
        "6e61efceb04cd4cbfb68f50c70565c6a94e682048a5c16262783c41b61e18517",
    ),
    "PCRE2_LICENCE.txt": (
        RAW_URL + "/pcre2/LICENCE",
        "262e2613b10caf71f08933c06a0f68ef1f1107c09df441277c02cb28f3a44d22",
    ),
    "DirectXTK_LICENSE.txt": (
        RAW_URL + "/DirectXTK/LICENSE",
        "d5f67216f1301a781226f9f4e15c9519e9126ba16aef602a256b387c10a5a851",
    ),
}

DLLS = ("bin/d3d11_edhm.dll",)
COMBINED_NOTICE = "EDHM-2DMIGOTO-THIRD-PARTY.txt"
NOTICE_PARTS = (
    "COPYING.txt",
    "AUTHORS.txt",
    "THIRD_PARTY_NOTICES.md",
    "PCRE2_LICENCE.txt",
    "DirectXTK_LICENSE.txt",
)


def pe_problem(rel, data):
    """Return why `data` is not an x64 PE image, or None."""
    if len(data) < 0x40 or data[:2] != b"MZ":
        return "%s is not a valid PE image" % rel
    pe_offset = struct.unpack_from("<I", data, 0x3C)[0]
    if pe_offset + 6 > len(data) or data[pe_offset:pe_offset + 4] != b"PE\0\0":
        return "%s has no valid PE header" % rel
    if struct.unpack_from("<H", data, pe_offset + 4)[0] != 0x8664:
        return "%s is not x64" % rel
    return None


def default_dest():
    base = os.environ.get("LOCALAPPDATA") or os.path.join(os.path.expanduser("~"), "AppData", "Local")
    return os.path.join(base, "EDVR", "edhm-2dmigoto")


def sha256_bytes(data):
    return hashlib.sha256(data).hexdigest()


def build_combined_notice(parts):
    """Assemble the deterministic third-party notice shipped beside d3d11_edhm.dll."""
    chunks = [
        "EDHM_2DMigoto (tailored 2DMigoto runtime for EDHM)\n"
        "Release:        %s (commit %s)\n"
        "Source tree:    %s\n"
        "Source archive: %s\n"
        "Archive SHA256: %s\n"
        "Build from src: Visual Studio 2022 / MSBuild Zip Release | x64 (see .github/workflows/release.yml at the pinned commit)\n"
        "License:        GNU General Public License v3.0 (see EDHM-2DMIGOTO-LICENSE.txt)\n"
        "\n"
        "EDHM (Elite Dangerous HUD Mod) is a separate community project\n"
        "(https://github.com/psychicEgg/EDHM). This optional runtime is a tailored\n"
        "3Dmigoto / XXMI compatibility build for EDHM users running ED-UP.\n"
        % (TAG, COMMIT, SOURCE_TREE_URL, SOURCE_ARCHIVE_URL, SOURCE_ARCHIVE_SHA256)
    ]
    for name in NOTICE_PARTS:
        text = parts[name].decode("utf-8", errors="replace").replace("\r\n", "\n").rstrip("\n")
        chunks.append("\n" + "=" * 72 + "\n" + name + "\n" + "=" * 72 + "\n\n" + text + "\n")
    return "".join(chunks).encode("utf-8")


def verify(dest, quiet=False):
    problems = []
    root = Path(dest)
    loaded = {}
    for rel, (_url, expected) in FILES.items():
        path = root / Path(rel)
        if not path.is_file():
            problems.append("missing: %s" % path)
            continue
        data = path.read_bytes()
        got = sha256_bytes(data)
        if got != expected:
            problems.append("%s SHA-256 is %s, pin is %s" % (rel, got.upper(), expected.upper()))
        else:
            loaded[rel] = data
    for dll_rel in DLLS:
        dll_path = root / dll_rel
        if dll_path.is_file():
            problem = pe_problem(dll_rel, dll_path.read_bytes())
            if problem:
                problems.append(problem)
    notice_path = root / COMBINED_NOTICE
    if not notice_path.is_file():
        problems.append("missing: %s" % notice_path)
    elif all(name in loaded for name in NOTICE_PARTS):
        expected_notice = build_combined_notice(loaded)
        if notice_path.read_bytes() != expected_notice:
            problems.append("%s does not match the pinned upstream notices" % COMBINED_NOTICE)
    if not problems and not quiet:
        print("EDHM_2DMigoto %s verified at %s (commit %s)" % (TAG, dest, COMMIT[:12]))
    return problems


def download_url(url, timeout=120):
    request = urllib.request.Request(url, headers={"User-Agent": "EDUP-pinned-edhm-fetch/1"})
    with urllib.request.urlopen(request, timeout=timeout) as response:
        return response.read()


def fetch(dest, dry_run=False, downloader=download_url):
    if dry_run:
        print("Would fetch EDHM_2DMigoto %s (commit %s) from %s" % (TAG, COMMIT, REPO))
        for rel, (url, digest) in FILES.items():
            print("Would stage %s from %s (SHA-256 %s)" % (rel, url, digest.upper()))
        print("Would write %s from the verified notices" % COMBINED_NOTICE)
        print("(dry run) no network, directories, or files were touched")
        return 0

    problems = verify(dest, quiet=True)
    if not problems:
        print("EDHM_2DMigoto %s already verified at %s" % (TAG, dest))
        return 0

    payload = {}
    for rel, (url, expected) in FILES.items():
        data = downloader(url)
        got = sha256_bytes(data)
        if got != expected:
            raise ValueError("downloaded %s SHA-256 is %s, pin is %s" %
                             (rel, got.upper(), expected.upper()))
        payload[rel] = data

    for dll_rel in DLLS:
        problem = pe_problem(dll_rel, payload[dll_rel])
        if problem:
            raise ValueError("downloaded " + problem)

    payload[COMBINED_NOTICE] = build_combined_notice(payload)

    parent = os.path.dirname(os.path.abspath(dest))
    staged = tempfile.mkdtemp(prefix=".edhm-stage-")
    backup = None
    publish = None
    try:
        for rel, data in payload.items():
            path = Path(staged) / Path(rel)
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
        os.makedirs(parent, exist_ok=True)
        publish = tempfile.mkdtemp(prefix=".edhm-publish-", dir=parent)
        shutil.rmtree(publish)
        shutil.copytree(staged, publish)
        if os.path.exists(dest):
            backup = dest + ".edhm-old"
            if os.path.exists(backup):
                raise OSError("refusing to overwrite existing backup %s" % backup)
            os.replace(dest, backup)
        try:
            os.replace(publish, dest)
        except Exception:
            if backup is not None:
                os.replace(backup, dest)
                backup = None
            raise
        if backup is not None:
            shutil.rmtree(backup)
            backup = None
    finally:
        shutil.rmtree(staged, ignore_errors=True)
        if publish is not None:
            shutil.rmtree(publish, ignore_errors=True)
    problems = verify(dest)
    if problems:
        raise ValueError("staged EDHM_2DMigoto failed verification: " + "; ".join(problems))
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dest", default=None, help="destination (default: %%LOCALAPPDATA%%\\EDVR\\edhm-2dmigoto)")
    ap.add_argument("--verify", default=None, metavar="DIR", help="verify an existing staged copy and exit")
    ap.add_argument("--dry-run", action="store_true", help="show the pinned files without network or writes")
    ap.add_argument("--print-source-archive", action="store_true", help="print the pinned GPL source archive filename and exit")
    ap.add_argument("--self-test", action="store_true", help="run offline fixture checks and exit")
    args = ap.parse_args(argv)
    if args.self_test:
        return self_test()
    if args.print_source_archive:
        print(SOURCE_ARCHIVE_NAME)
        return 0
    if args.verify:
        problems = verify(args.verify)
        for problem in problems:
            print("fetch_edhm_2dmigoto: " + problem)
        return 2 if problems else 0
    try:
        return fetch(args.dest or default_dest(), dry_run=args.dry_run)
    except (OSError, ValueError) as error:
        print("fetch_edhm_2dmigoto: " + str(error), file=sys.stderr)
        return 2


def self_test():
    import contextlib
    import io
    from unittest import mock
    failures = []

    def expect(name, condition, detail=""):
        if not condition:
            failures.append("%s: %s" % (name, detail))

    with tempfile.TemporaryDirectory(prefix="edvr-fetch-edhm-") as base:
        fixtures = {rel: (b"MZ" + b"\0" * (0x3C - 2) + struct.pack("<I", 0x40) +
                          b"\0" * (0x40 - 0x3C - 4) + b"PE\0\0" + struct.pack("<H", 0x8664)
                          if rel in DLLS else b"fixture:" + rel.encode("ascii"))
                    for rel in FILES}
        saved_files = dict(FILES)
        try:
            for rel, data in fixtures.items():
                url, _digest = FILES[rel]
                FILES[rel] = (url, sha256_bytes(data))
            by_url = {url: fixtures[rel] for rel, (url, _h) in FILES.items()}
            dest = os.path.join(base, "install")
            code = fetch(dest, downloader=lambda url: by_url[url])
            expect("staging", code == 0 and not verify(dest, quiet=True), "valid fixture did not stage")
            notice_text = (Path(dest) / COMBINED_NOTICE).read_text(encoding="utf-8")
            expect("combined-notice", TAG in notice_text and "PCRE2_LICENCE.txt" in notice_text
                   and "DirectXTK_LICENSE.txt" in notice_text and "EDHM" in notice_text,
                   "combined notice missing required attributions")
            Path(dest, "LICENSE.GPL.txt").write_bytes(b"corrupt")
            expect("corrupt-license", bool(verify(dest, quiet=True)), "corrupt license passed")
            Path(dest, "LICENSE.GPL.txt").write_bytes(fixtures["LICENSE.GPL.txt"])
            Path(dest, COMBINED_NOTICE).write_bytes(b"tampered notice")
            expect("corrupt-combined", bool(verify(dest, quiet=True)), "tampered combined notice passed")
            Path(dest, COMBINED_NOTICE).write_bytes(build_combined_notice(fixtures))
            Path(dest, "bin/d3d11_edhm.dll").write_bytes(b"not a pe")
            expect("corrupt-dll", bool(verify(dest, quiet=True)), "corrupt DLL passed")
            # Leave dest corrupt for the failed-replacement check so fetch() actually attempts replacement.
            pe32 = (b"MZ" + b"\0" * (0x3C - 2) + struct.pack("<I", 0x40) +
                    b"\0" * (0x40 - 0x3C - 4) + b"PE\0\0" + struct.pack("<H", 0x014C))
            expect("reject-x86-pe", pe_problem("bin/d3d11_edhm.dll", pe32) is not None, "x86 PE accepted")
            bad_dest = os.path.join(base, "failed")

            def bad_download(url):
                if url == FILES["bin/d3d11_edhm.dll"][0]:
                    return b"wrong dll"
                return by_url[url]

            try:
                fetch(bad_dest, downloader=bad_download)
                expect("failed-fetch", False, "bad DLL accepted")
            except ValueError:
                expect("failed-fetch", not os.path.exists(bad_dest), "failed fetch left destination")
            before = {str(p.relative_to(dest)): p.read_bytes()
                      for p in Path(dest).rglob("*") if p.is_file()}
            try:
                fetch(dest, downloader=bad_download)
                expect("failed-replacement", False, "bad replacement accepted")
            except ValueError:
                after = {str(p.relative_to(dest)): p.read_bytes()
                         for p in Path(dest).rglob("*") if p.is_file()}
                expect("failed-replacement", before == after, "failed fetch changed existing destination")
            Path(dest, "bin/d3d11_edhm.dll").write_bytes(fixtures["bin/d3d11_edhm.dll"])
            dry_parent = os.path.join(base, "dry-parent")
            dry_dest = os.path.join(dry_parent, "dry")
            output = io.StringIO()
            with contextlib.ExitStack() as guards:
                for name in ("os.mkdir", "os.makedirs", "os.replace", "os.rename",
                             "os.remove", "os.unlink", "os.rmdir",
                             "tempfile.mkdtemp", "tempfile.mkstemp",
                             "shutil.copyfile", "shutil.copytree", "shutil.rmtree",
                             "pathlib.Path.write_bytes", "pathlib.Path.write_text"):
                    guards.enter_context(mock.patch(name, side_effect=AssertionError("dry-run write: " + name)))
                guards.enter_context(contextlib.redirect_stdout(output))
                fetch(dry_dest, dry_run=True,
                      downloader=lambda _url: (_ for _ in ()).throw(AssertionError("network")))
            expect("dry-run", not os.path.exists(dry_parent) and "no network" in output.getvalue(),
                   "dry-run touched filesystem/network")
            src_out = io.StringIO()
            with contextlib.redirect_stdout(src_out):
                rc_print = main(["--print-source-archive"])
            expect("print-source-archive", rc_print == 0 and src_out.getvalue().strip() == SOURCE_ARCHIVE_NAME,
                   "unexpected --print-source-archive output: " + repr(src_out.getvalue()))
        finally:
            FILES.clear()
            FILES.update(saved_files)
    if failures:
        print("fetch_edhm_2dmigoto: self-test FAILED")
        for failure in failures:
            print("  " + failure)
        return 1
    print("fetch_edhm_2dmigoto: self-test OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
