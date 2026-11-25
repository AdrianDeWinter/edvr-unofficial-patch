#!/usr/bin/env python3
"""Where a machine's fetched SDKs live, and the order build.bat looks for them.

    python tools\\sdk_paths.py
    python tools\\sdk_paths.py --self-test

The fetch tools (fetch_ngx.py, fetch_xess.py, fetch_edhm_2dmigoto.py,
fetch_fsr4.py, fetch_ffx_dx11.py) write to %LOCALAPPDATA%\\EDUP\\<sdk> by
default: default_dest() is the one owner of that folder. Before the rename to
ED-UP they wrote to %LOCALAPPDATA%\\EDVR\\<sdk>. No tool writes there now, but
build.bat still reads an SDK from there, so a machine that fetched earlier
keeps building. Nothing moves or deletes a folder at either location.

build.bat looks in this order, and takes the first copy it finds: the SDK's
environment variable, the checkout's third_party folder, EDUP, EDVR.

With no arguments this prints both folders of each SDK and whether each one
holds a copy. --self-test runs build.bat's own lookup lines against fixture
folders. Read-only: this tool writes nothing (the self-test writes only to a
temporary directory).
"""
import argparse
import contextlib
import io
import os
import shutil
import subprocess
import sys
import tempfile
from collections import namedtuple
from pathlib import Path
from unittest import mock

DIR_NAME = "EDUP"
LEGACY_DIR_NAME = "EDVR"    # the fallback build.bat still reads

# leaf: the folder name under EDUP and EDVR. tool: what fetches it. env: the variable that names a copy.
# vendored: the folder under third_party. marker: the file build.bat looks for.
# var, end: the batch variable the lookup sets, and the line after the lookup.
# none: the batch variable that the value "none" sets, for the two SDKs that take it.
Sdk = namedtuple("Sdk", "leaf tool env vendored marker var end none")
SDKS = (
    Sdk("ngx-sdk", "fetch_ngx.py", "EDUP_NGX_SDK", "ngx", "include/nvsdk_ngx.h", "NGX", "set NGXFLAGS=", None),
    Sdk("xess-sdk", "fetch_xess.py", "EDUP_XESS_SDK", "xess", "include/xess/xess.h", "XESS",
        "set XESSFLAGS=/DEDVR_HAVE_XESS=0", None),
    Sdk("edhm-2dmigoto", "fetch_edhm_2dmigoto.py", "EDUP_EDHM_2DMIGOTO", "edhm_2dmigoto", "bin/d3d11_edhm.dll",
        "EDHM2DM", "set EDHM_SOURCE_ARCHIVE=", None),
    Sdk("fsr4-sdk", "fetch_fsr4.py", "EDUP_FSR4_SDK", "fsr4",
        "include/FidelityFX/upscalers/include/ffx_upscale.h", "FSR4", "set FSR4FLAGS=/DEDVR_HAVE_FSR4=0", "FSR4_NONE"),
    Sdk("ffx-dx11", "fetch_ffx_dx11.py", "EDUP_FFX_DX11", "ffx-dx11",
        "include/FidelityFX/host/ffx_fsr3upscaler.h", "FFX", "set FSRFLAGS=", "FSR_NONE"),
)


def _local_app_data():
    return os.environ.get("LOCALAPPDATA") or os.path.join(os.path.expanduser("~"), "AppData", "Local")


def default_dest(leaf):
    """The folder a fetch tool writes to when the command line names none."""
    return os.path.join(_local_app_data(), DIR_NAME, leaf)


def legacy_dest(leaf):
    """Where a fetch before the rename put it: read by build.bat, never written."""
    return os.path.join(_local_app_data(), LEGACY_DIR_NAME, leaf)


def lookup_lines(build_bat_text, sdk):
    """build.bat's own lines that find `sdk`, or None when they are not there exactly once."""
    lines = build_bat_text.splitlines()
    start, end = "set %s=" % sdk.var, sdk.end
    if lines.count(start) != 1 or lines.count(end) != 1 or lines.index(end) <= lines.index(start):
        return None
    return lines[lines.index(start):lines.index(end)]


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--self-test", action="store_true", help="check the folders and build.bat's lookup, and exit")
    args = ap.parse_args(argv)
    if args.self_test:
        return self_test()
    for sdk in SDKS:
        for dest in (default_dest(sdk.leaf), legacy_dest(sdk.leaf)):
            found = os.path.isfile(os.path.join(dest, *sdk.marker.split("/")))
            print("%-14s %-8s %s" % (sdk.leaf, "present" if found else "absent", dest))
    return 0


# ---------------------------------------------------------------------------
# --self-test
# ---------------------------------------------------------------------------

def check_fetch_default(tool_default_dest, leaf, expect, tool_main=None):
    """For each fetch tool's own self-test: its default folder is under EDUP, with
    no copy on the machine and with one at the old EDVR folder, and neither asking
    for it nor the tool's own --dry-run (`tool_main`) makes or removes anything.
    `expect` takes (name, condition, detail)."""
    with tempfile.TemporaryDirectory(prefix="edup-sdk-default-") as base:
        with mock.patch.dict(os.environ, {"LOCALAPPDATA": base}):
            want = os.path.join(base, "EDUP", leaf)
            got = tool_default_dest()
            expect("default-dest", got == want, got)
            expect("default-dest-writes-nothing", os.listdir(base) == [], repr(os.listdir(base)))
            os.makedirs(os.path.join(base, "EDVR", leaf))
            got = tool_default_dest()
            expect("default-dest-beside-legacy", got == want, got)
            if tool_main is not None:
                said = io.StringIO()
                with contextlib.redirect_stdout(said):
                    code = tool_main(["--dry-run"])
                expect("default-dry-run", code == 0 and legacy_dest(leaf) not in said.getvalue(),
                       "%r %s" % (code, said.getvalue()))
            made = sorted(os.path.relpath(os.path.join(at, name), base)
                          for at, folders, files in os.walk(base) for name in folders + files)
            expect("default-dest-legacy-kept", made == ["EDVR", os.path.join("EDVR", leaf)], repr(made))
        with mock.patch.dict(os.environ):
            os.environ.pop("LOCALAPPDATA", None)
            got = tool_default_dest()
            expect("default-dest-no-localappdata",
                   got == os.path.join(os.path.expanduser("~"), "AppData", "Local", "EDUP", leaf), got)


def _run_lookup(lines, sdk, root, local, env, scratch):
    """Run build.bat's lookup lines under cmd; returns (the found folder, the none flag)."""
    script = os.path.join(scratch, "lookup.cmd")
    body = ["@echo off", 'set "ROOT=%s"' % root] + lines + ["call echo found=[%%%s%%]" % sdk.var]
    if sdk.none:
        body.append("call echo none=[%%%s%%]" % sdk.none)
    with open(script, "w", encoding="mbcs", newline="\r\n") as f:
        f.write("\n".join(body) + "\n")
    run_env = {k: v for k, v in os.environ.items() if k.upper() not in {s.env for s in SDKS}}
    run_env["LOCALAPPDATA"] = local
    if env:
        run_env[sdk.env] = env
    out = subprocess.run(["cmd.exe", "/d", "/c", script], env=run_env, stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT, text=True, check=True).stdout
    said = dict(line.split("=", 1) for line in out.splitlines() if "=" in line)
    return said.get("found", "[?]")[1:-1], said.get("none", "[]")[1:-1]


def _check_lookup(lines, sdk, expect):
    """build.bat's lookup of one SDK, with every copy laid down that can win or lose.
    Every folder has a space in its name, as a user name can."""
    with tempfile.TemporaryDirectory(prefix="edup sdk lookup ") as base:
        root, local, named = (os.path.join(base, name) for name in ("the root", "the local", "the named copy"))
        places = {
            "env": named,
            "third_party": os.path.join(root, "third_party", sdk.vendored),
            "EDUP": os.path.join(local, "EDUP", sdk.leaf),
            "EDVR": os.path.join(local, "EDVR", sdk.leaf),
        }

        def lay(present):
            for name in ("third_party", "EDUP", "EDVR"):
                marker = os.path.join(places[name], *sdk.marker.split("/"))
                if name in present:
                    os.makedirs(os.path.dirname(marker), exist_ok=True)
                    Path(marker).write_bytes(b"x")
                elif os.path.isfile(marker):
                    os.remove(marker)

        for present, env, want in (
                ((), None, None),
                (("EDVR",), None, "EDVR"),
                (("EDUP",), None, "EDUP"),
                (("EDUP", "EDVR"), None, "EDUP"),
                (("third_party", "EDVR"), None, "third_party"),
                (("third_party", "EDUP", "EDVR"), None, "third_party"),
                (("EDVR",), named, "env"),
                (("third_party", "EDUP", "EDVR"), named, "env")):
            lay(present)
            found, _none = _run_lookup(lines, sdk, root, local, env, base)
            expect("lookup-%s-%s%s" % (sdk.leaf, "+".join(present) or "nothing", "-env" if env else ""),
                   found == (places[want] if want else ""), "found %r, wanted the %s copy" % (found, want))
        if sdk.none:
            lay(("third_party", "EDUP", "EDVR"))
            found, none = _run_lookup(lines, sdk, root, local, "none", base)
            expect("lookup-%s-none" % sdk.leaf, (found, none) == ("", "1"), "found %r, flag %r" % (found, none))


def _check_dry_run(sdk, expect):
    """The fetch tool run as its own process, from a folder that holds only it and this
    module: --dry-run leaves that folder and LOCALAPPDATA as they were, with no import
    cache. ngx and ffx-dx11 ask git about a checkout that is not there; that fails quietly."""
    with tempfile.TemporaryDirectory(prefix="edup-sdk-dry-") as base:
        tools, local = os.path.join(base, "tools"), os.path.join(base, "local")
        os.makedirs(tools)
        os.makedirs(local)
        for name in ("sdk_paths.py", sdk.tool):
            shutil.copyfile(os.path.join(os.path.dirname(os.path.abspath(__file__)), name), os.path.join(tools, name))
        env = {k: v for k, v in os.environ.items() if k not in ("PYTHONDONTWRITEBYTECODE", "PYTHONPYCACHEPREFIX")}
        env["LOCALAPPDATA"] = local
        ran = subprocess.run([sys.executable, os.path.join(tools, sdk.tool), "--dry-run"], env=env, cwd=base,
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        left = sorted(os.path.relpath(os.path.join(at, name), base)
                      for at, folders, files in os.walk(base) for name in folders + files)
        wanted = sorted(["tools", "local"] + [os.path.join("tools", n) for n in ("sdk_paths.py", sdk.tool)])
        expect("dry-run-%s" % sdk.leaf, ran.returncode == 0 and left == wanted,
               "exit %s, left %r\n%s" % (ran.returncode, left, ran.stdout))


def self_test():
    failures = []

    def expect(name, condition, detail=""):
        if not condition:
            failures.append("%s: %s" % (name, detail))

    for sdk in SDKS:
        check_fetch_default(lambda: default_dest(sdk.leaf), sdk.leaf, expect)
        _check_dry_run(sdk, expect)
    with mock.patch.dict(os.environ, {"LOCALAPPDATA": "base"}):
        expect("legacy-dest", legacy_dest("x") == os.path.join("base", "EDVR", "x"), legacy_dest("x"))

    expect("extract-missing", lookup_lines("set NGX=\n", SDKS[0]) is None, "a lookup with no end was accepted")
    expect("extract-order", lookup_lines("set NGXFLAGS=\nset NGX=\n", SDKS[0]) is None, "a reversed lookup was accepted")
    build_bat = (Path(__file__).resolve().parent.parent / "build.bat").read_text(encoding="utf-8", errors="replace")
    for sdk in SDKS:
        lines = lookup_lines(build_bat, sdk)
        expect("lookup-" + sdk.leaf, lines is not None, "build.bat's lookup lines were not found")
        if lines is not None and os.name == "nt":    # cmd runs the lookup, so only Windows can
            _check_lookup(lines, sdk, expect)

    if failures:
        print("sdk_paths: self-test FAILED")
        for failure in failures:
            print("  " + failure)
        return 1
    print("sdk_paths: self-test OK%s" % ("" if os.name == "nt" else " (build.bat's lookup not run: no cmd.exe)"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
