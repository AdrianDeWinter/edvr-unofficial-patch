#!/usr/bin/env python3
"""Compile third_party/dittosr/shaders to Direct3D 11 shader model 5 bytecode.

Writes src/d3d11/dittosr_shader_bytecode.h. Uses fxc when FXC is set or fxc is
on PATH, otherwise d3dcompiler_47.dll's D3DCompile on Windows. DXC promotes
cs_5_0 to shader model 6 and writes DXIL, which Direct3D 11 will not create;
this script rejects a DXIL container. The DLL creates its compute shaders from
this bytecode, so a player's machine does not compile HLSL.
"""
import os
import pathlib
import re
import shutil
import struct
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
SRC = ROOT / "third_party" / "dittosr" / "shaders"
OUT = ROOT / "src" / "d3d11" / "dittosr_shader_bytecode.h"
PASSES = (
    ("convert", ""),
    ("motion", ""),
    ("upscale", "#define ANTI_SHIMMER 1\n"),
    ("sharpen", "#define ANTI_SHIMMER 1\n"),
)


def fxc_path() -> str:
    env = os.environ.get("FXC")
    if env:
        return env
    return shutil.which("fxc") or shutil.which("fxc.exe") or ""


def source(name: str, defines: str) -> str:
    common = (SRC / "common.hlsli").read_text()
    body = (SRC / f"{name}.hlsl").read_text().replace('#include "common.hlsli"\n', "")
    return common + "\n" + defines + body


def _blob_bytes(pointer) -> bytes:
    import ctypes
    if not pointer:
        return b""
    vtable = ctypes.cast(ctypes.cast(pointer, ctypes.POINTER(ctypes.c_void_p))[0], ctypes.POINTER(ctypes.c_void_p))
    get_pointer = ctypes.CFUNCTYPE(ctypes.c_void_p, ctypes.c_void_p)(vtable[3])
    get_size = ctypes.CFUNCTYPE(ctypes.c_size_t, ctypes.c_void_p)(vtable[4])
    return ctypes.string_at(get_pointer(pointer), get_size(pointer))


def compile_d3d(text: str, name: str) -> bytes:
    """D3DCompile from d3dcompiler_47.dll, the compiler the rest of the build uses. Optimization level 3."""
    import ctypes
    compiler = ctypes.WinDLL("d3dcompiler_47.dll")
    compiler.D3DCompile.argtypes = [
        ctypes.c_void_p, ctypes.c_size_t, ctypes.c_char_p, ctypes.c_void_p, ctypes.c_void_p,
        ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint, ctypes.c_uint,
        ctypes.POINTER(ctypes.c_void_p), ctypes.POINTER(ctypes.c_void_p),
    ]
    compiler.D3DCompile.restype = ctypes.c_long
    encoded = text.encode("utf-8")
    code = ctypes.c_void_p()
    errors = ctypes.c_void_p()
    hr = compiler.D3DCompile(encoded, len(encoded), name.encode("ascii"), None, None,
                             b"main", b"cs_5_0", 1 << 15, 0, ctypes.byref(code), ctypes.byref(errors))
    if hr < 0:
        detail = _blob_bytes(errors.value).decode("utf-8", "replace")
        raise SystemExit(f"gen_dittosr_shaders: {name} did not compile\n{detail}")
    return _blob_bytes(code.value)


def compile_pass(name: str, defines: str, directory: pathlib.Path) -> bytes:
    src = directory / f"{name}.hlsl"
    blob = directory / f"{name}.cso"
    src.write_text(source(name, defines))
    fxc = fxc_path()
    if fxc:
        run = subprocess.run([fxc, "/nologo", "/T", "cs_5_0", "/E", "main", "/O3", "/Fo", str(blob), str(src)],
                             capture_output=True, text=True)
        if run.returncode != 0:
            sys.stderr.write(run.stdout + run.stderr)
            raise SystemExit(f"gen_dittosr_shaders: {name} did not compile")
        data = blob.read_bytes()
    elif sys.platform == "win32":
        data = compile_d3d(src.read_text(encoding="utf-8"), name)
    else:
        raise SystemExit(
            "gen_dittosr_shaders: set FXC to fxc.exe, or run this on Windows where d3dcompiler_47.dll can compile "
            "cs_5_0. DXC writes shader model 6 DXIL for that target, and Direct3D 11 will not create it.")
    shader_model_5(data, name)
    return data


def shader_model_5(data: bytes, name: str) -> None:
    """DXBC is the container for both DXBC and DXIL. A cs_5_0 pass has a SHEX or SHDR chunk whose version
    token is 0x00050050 and no DXIL chunk. Magic alone does not prove the model."""
    if len(data) < 32 or data[:4] != b"DXBC":
        raise SystemExit(f"gen_dittosr_shaders: {name} is not a DXBC container")
    count = struct.unpack_from("<I", data, 28)[0]
    if 32 + count * 4 > len(data):
        raise SystemExit(f"gen_dittosr_shaders: {name} chunk table is truncated")
    saw = False
    for index in range(count):
        offset = struct.unpack_from("<I", data, 32 + index * 4)[0]
        if offset + 12 > len(data):
            raise SystemExit(f"gen_dittosr_shaders: {name} chunk {index} is truncated")
        kind = data[offset:offset + 4]
        if kind == b"DXIL":
            raise SystemExit(f"gen_dittosr_shaders: {name} is DXIL, not shader model 5")
        if kind in (b"SHEX", b"SHDR"):
            version = struct.unpack_from("<I", data, offset + 8)[0]
            if version != 0x00050050:
                raise SystemExit(f"gen_dittosr_shaders: {name} version token is 0x{version:08x}, not cs_5_0")
            saw = True
    if not saw:
        raise SystemExit(f"gen_dittosr_shaders: {name} has no shader chunk")


def c_array(name: str, data: bytes) -> str:
    lines = [f"inline constexpr unsigned char kDitto{name.capitalize()}Dxbc[] = {{"]
    row = []
    for i, byte in enumerate(data):
        row.append(f"0x{byte:02x}")
        if len(row) == 16:
            lines.append("    " + ", ".join(row) + ",")
            row = []
    if row:
        lines.append("    " + ", ".join(row) + ",")
    lines.append("};")
    return "\n".join(lines)


def verify_header(path: pathlib.Path) -> int:
    text = path.read_text(encoding="ascii")
    arrays = re.findall(r"kDitto(\w+)Dxbc\[\] = \{([^}]*)\}", text, re.S)
    if len(arrays) != 4:
        raise SystemExit(f"gen_dittosr_shaders: {path} holds {len(arrays)} passes, expected 4")
    for name, body in arrays:
        data = bytes(int(token, 16) for token in re.findall(r"0x([0-9a-fA-F]{2})", body))
        shader_model_5(data, name)
    print("gen_dittosr_shaders: four passes are cs_5_0 DXBC")
    return 0


def _container(kind: bytes, version: int) -> bytes:
    blob = bytearray(48)
    blob[0:4] = b"DXBC"
    struct.pack_into("<I", blob, 28, 1)
    struct.pack_into("<I", blob, 32, 36)
    blob[36:40] = kind
    struct.pack_into("<I", blob, 44, version)
    return bytes(blob)


def _expect_reject(data: bytes, needle: str) -> None:
    try:
        shader_model_5(data, "probe")
    except SystemExit as exc:
        if needle not in str(exc):
            raise SystemExit(f"gen_dittosr_shaders: expected {needle} in {exc}") from exc
    else:
        raise SystemExit(f"gen_dittosr_shaders: expected rejection ({needle})")


def self_test() -> int:
    _expect_reject(_container(b"DXIL", 0), "DXIL")
    _expect_reject(_container(b"SHEX", 0x00060050), "not cs_5_0")
    shader_model_5(_container(b"SHEX", 0x00050050), "probe")
    return verify_header(OUT)


def header_arrays(path: pathlib.Path) -> dict:
    text = path.read_text(encoding="ascii")
    found = {}
    for name, body in re.findall(r"kDitto(\w+)Dxbc\[\] = \{([^}]*)\}", text, re.S):
        found[name.lower()] = bytes(int(token, 16) for token in re.findall(r"0x([0-9a-fA-F]{2})", body))
    return found


def check_bytes() -> int:
    """Recompile the four passes and require the committed header to match. Windows uses D3DCompile; Linux needs FXC."""
    committed = header_arrays(OUT)
    with tempfile.TemporaryDirectory() as tmp:
        directory = pathlib.Path(tmp)
        for name, defines in PASSES:
            fresh = compile_pass(name, defines, directory)
            old = committed.get(name)
            if old != fresh:
                raise SystemExit(
                    f"gen_dittosr_shaders: {name} does not match src/d3d11/dittosr_shader_bytecode.h "
                    f"({len(fresh)} compiled, {0 if old is None else len(old)} committed). "
                    "Regenerate with python tools/gen_dittosr_shaders.py")
    print("gen_dittosr_shaders: committed bytecode matches a fresh cs_5_0 compile")
    return 0


def main() -> int:
    if "--check-bytes" in sys.argv:
        return check_bytes()
    if "--self-test" in sys.argv or "--verify-header" in sys.argv:
        return self_test()
    parts = [
        "// Generated by tools/gen_dittosr_shaders.py from third_party/dittosr/shaders.",
        "// DittoSR 0.5.0, shader model 5. Do not edit; regenerate after a shader change.",
        "#pragma once",
        "",
        "#include <cstddef>",
        "",
    ]
    with tempfile.TemporaryDirectory() as tmp:
        directory = pathlib.Path(tmp)
        for name, defines in PASSES:
            data = compile_pass(name, defines, directory)
            parts.append(c_array(name, data))
            parts.append("")
    OUT.write_text("\n".join(parts) + "\n")
    print(f"wrote {OUT.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
