#!/usr/bin/env python3
"""Read the eye dump's files, plain or compressed.

One press of the VR eye dump key writes gigabytes. Every .bin it writes into
edvr_logs\\eyes and edvr_logs\\pool is stored as NAME.z, two BMP pairs are
stored as an XOR delta against their twin, and this module is the one reader
every tool goes through. src\\common\\dump_file.h is the writer and holds the
container's layout (EDVRZ1: a header, then 4 MiB chunks, each its own zlib
stream). A dump from before the container is plain files, and read_bytes
reads those too, so old dumps keep working.

    import dump_io
    data = dump_io.read_bytes(r"...\\eye_164515939001_D00.bin")   # plain or .z
    dump_io.exists(path); dump_io.glob(r"...\\eye_*_D*.bin")

The path is always the ORIGINAL name (the one the file had before the
container). python tools\\dump_io.py --expand <dir> [--stamp S] [--dry-run]
writes the original files beside the .z ones, byte for byte, and deletes
nothing.
"""
from __future__ import annotations

import argparse
import glob as _glob
import os
import struct
import sys
import tempfile
import zlib

MAGIC = b"EDVRZ1\0\0"
VERSION = 1
FLAG_XOR = 1
CHUNK_BYTES = 4 << 20
_FIXED = 36   # the header up to and including baseNameBytes
Z_SUFFIX = ".z"


class DumpIoError(ValueError):
    """A dump file that cannot be trusted: named in the message, never silent. A ValueError, so the readers
    that already treat a bad capture as ValueError report it the same way."""


def _xor(a: bytes, b: bytes) -> bytes:
    n = len(a)
    if n != len(b):
        raise DumpIoError("xor of different lengths (%d, %d)" % (n, len(b)))
    if n == 0:
        return b""
    try:
        import numpy as np   # optional: speed only
        return (np.frombuffer(a, dtype=np.uint8) ^ np.frombuffer(b, dtype=np.uint8)).tobytes()
    except ImportError:
        return (int.from_bytes(a, "little") ^ int.from_bytes(b, "little")).to_bytes(n, "little")


def _decode_z(zpath: str, allow_xor: bool) -> bytes:
    with open(zpath, "rb") as fh:
        blob = fh.read()
    total = len(blob)
    name = os.path.basename(zpath)

    def bad(why):
        raise DumpIoError("%s: %s" % (zpath, why))

    if total < _FIXED:
        bad("truncated: %d bytes is shorter than the %d-byte header" % (total, _FIXED))
    if blob[:8] != MAGIC:
        bad("not an EDVRZ1 file (magic %r)" % (blob[:8],))
    version, flags, raw_bytes, chunk_raw, chunk_count, name_len = struct.unpack_from("<IIQIII", blob, 8)
    if version != VERSION:
        bad("version %d, this reader knows %d" % (version, VERSION))
    if flags & ~FLAG_XOR:
        bad("unknown flags 0x%x" % flags)
    if chunk_raw == 0:
        bad("chunkRawBytes is 0")
    if not flags & FLAG_XOR and name_len:
        bad("a base name without the XOR flag")
    if flags & FLAG_XOR and name_len == 0:
        bad("the XOR flag without a base name")
    pos = _FIXED + name_len
    if pos > total:
        bad("truncated inside the base name")
    base_name = blob[_FIXED:pos].decode("utf-8", "strict") if name_len else ""
    want_chunks = (raw_bytes + chunk_raw - 1) // chunk_raw
    if chunk_count != want_chunks:
        bad("chunkCount %d does not fit rawBytes %d at %d bytes a chunk (want %d)"
            % (chunk_count, raw_bytes, chunk_raw, want_chunks))
    out = []
    got = 0
    for i in range(chunk_count):
        if pos + 4 > total:
            bad("truncated before chunk %d of %d" % (i, chunk_count))
        (clen,) = struct.unpack_from("<I", blob, pos)
        pos += 4
        if clen == 0 or pos + clen > total:
            bad("chunk %d claims %d compressed bytes with %d left" % (i, clen, total - pos))
        expect = chunk_raw if i + 1 < chunk_count else raw_bytes - chunk_raw * (chunk_count - 1)
        d = zlib.decompressobj()
        try:
            piece = d.decompress(blob[pos:pos + clen], expect + 1)
            if not d.eof or d.unconsumed_tail:
                piece += d.flush()
        except zlib.error as exc:
            bad("chunk %d is corrupt (%s)" % (i, exc))
        if not d.eof or d.unused_data:
            bad("chunk %d is not one complete zlib stream of %d bytes" % (i, clen))
        if len(piece) != expect:
            bad("chunk %d decodes to %d bytes, expected %d" % (i, len(piece), expect))
        out.append(piece)
        got += len(piece)
        pos += clen
    if pos != total:
        bad("%d bytes of trailing data after the last chunk" % (total - pos))
    data = b"".join(out)
    if got != raw_bytes:
        bad("decoded %d bytes, header says %d" % (got, raw_bytes))
    if not flags & FLAG_XOR:
        return data
    if not allow_xor:
        bad("a delta against %r, and a delta's base must itself be plain" % base_name)
    if base_name != os.path.basename(base_name) or "/" in base_name or "\\" in base_name or base_name in (".", ".."):
        bad("base name %r is not a bare file name" % base_name)
    base_path = os.path.join(os.path.dirname(zpath), base_name)
    try:
        base = read_bytes(base_path, _allow_xor=False)
    except FileNotFoundError:
        raise DumpIoError("%s: its base file %s is missing; a delta cannot be read without it"
                          % (zpath, base_name)) from None
    if len(base) != raw_bytes:
        bad("its base %s is %d bytes, the delta decodes to %d" % (base_name, len(base), raw_bytes))
    return _xor(data, base)


def read_bytes(path, _allow_xor: bool = True) -> bytes:
    """The bytes of `path` (its original name): the plain file if there is one,
    else the decoded `path`.z. FileNotFoundError if neither. DumpIoError if the
    .z is not a whole, valid container."""
    path = os.fspath(path)
    if path.endswith(Z_SUFFIX) and os.path.isfile(path):
        return _decode_z(path, _allow_xor)   # the .z itself was named: it is a container, never plain bytes
    if os.path.isfile(path):
        with open(path, "rb") as fh:
            return fh.read()
    z = path + Z_SUFFIX
    if os.path.isfile(z):
        return _decode_z(z, _allow_xor)
    raise FileNotFoundError("neither %s nor %s exists" % (path, z))


def exists(path) -> bool:
    path = os.fspath(path)
    return os.path.isfile(path) or os.path.isfile(path + Z_SUFFIX)


def glob(pattern) -> list:
    """Original names matching `pattern`, for plain and .z files alike, once each."""
    pattern = os.fspath(pattern)
    found = set(_glob.glob(pattern))
    found.update(_glob.glob(pattern + Z_SUFFIX))
    names = set()
    for p in found:
        names.add(p[:-len(Z_SUFFIX)] if p.endswith(Z_SUFFIX) else p)
    return sorted(names)


def getsize(path) -> int:
    """The decoded size of `path`, without decoding more than the header."""
    path = os.fspath(path)
    if os.path.isfile(path):
        return os.path.getsize(path)
    z = path + Z_SUFFIX
    if not os.path.isfile(z):
        raise FileNotFoundError("neither %s nor %s exists" % (path, z))
    with open(z, "rb") as fh:
        head = fh.read(_FIXED)
    if len(head) < _FIXED or head[:8] != MAGIC:
        raise DumpIoError("%s: not an EDVRZ1 file" % z)
    return struct.unpack_from("<Q", head, 16)[0]


def write_z(path, data: bytes, base: str = None, level: int = 1) -> str:
    """Python's own writer of the container (tests, offline checks): `path` is the
    original name, the file lands at path + '.z'. With `base` (a plain file in the
    same directory) the data is stored as an XOR against it."""
    path = os.fspath(path)
    flags = 0
    name = b""
    body = data
    if base is not None:
        flags = FLAG_XOR
        name = os.path.basename(base).encode("utf-8")
        body = _xor(data, read_bytes(base, _allow_xor=False))
    chunks = [body[i:i + CHUNK_BYTES] for i in range(0, len(body), CHUNK_BYTES)]
    out = [MAGIC, struct.pack("<IIQIII", VERSION, flags, len(data), CHUNK_BYTES, len(chunks), len(name)), name]
    for c in chunks:
        z = zlib.compress(c, level)
        out.append(struct.pack("<I", len(z)))
        out.append(z)
    with open(path + Z_SUFFIX, "wb") as fh:
        fh.write(b"".join(out))
    return path + Z_SUFFIX


def expand(directory, stamp=None, dry_run=False, out=print) -> dict:
    """Write the original file beside every NAME.z in `directory` (not recursive)
    whose name contains `stamp`. Deletes nothing, overwrites nothing: an existing
    NAME that differs is reported and left. dry_run writes nothing at all."""
    directory = os.fspath(directory)
    res = {"written": 0, "same": 0, "conflict": 0, "error": 0, "bytes": 0}
    if not os.path.isdir(directory):
        raise DumpIoError("%s is not a directory" % directory)
    for fname in sorted(os.listdir(directory)):
        if not fname.endswith(Z_SUFFIX) or (stamp and stamp not in fname):
            continue
        target = os.path.join(directory, fname[:-len(Z_SUFFIX)])
        try:
            data = read_bytes(target)   # the plain file wins if it is there
            if os.path.isfile(target):
                if data == _decode_z(target + Z_SUFFIX, True):
                    res["same"] += 1
                else:
                    res["conflict"] += 1
                    out("conflict: %s exists and differs from %s; left alone" % (target, fname))
                continue
        except (DumpIoError, OSError) as exc:
            res["error"] += 1
            out("error: %s" % exc)
            continue
        if dry_run:
            out("would write %s (%d bytes)" % (target, len(data)))
        else:
            try:
                with open(target, "xb") as fh:
                    fh.write(data)
            except OSError as exc:
                res["error"] += 1
                out("error: %s: %s" % (target, exc))
                continue
            out("wrote %s (%d bytes)" % (target, len(data)))
        res["written"] += 1
        res["bytes"] += len(data)
    return res


def check_dir(directory, out=print) -> int:
    """The cross-language gate: for every NAME.expect in `directory` (the plain
    bytes the C++ writer was given), read_bytes(NAME) must equal it."""
    directory = os.fspath(directory)
    names = sorted(n for n in os.listdir(directory) if n.endswith(".expect"))
    if not names:
        out("FAIL: %s holds no .expect files; the C++ rig wrote nothing to check" % directory)
        return 1
    bad = 0
    for n in names:
        target = os.path.join(directory, n[:-len(".expect")])
        with open(os.path.join(directory, n), "rb") as fh:
            want = fh.read()
        try:
            got = read_bytes(target)
        except (DumpIoError, OSError) as exc:
            out("FAIL %s: %s" % (n[:-7], exc))
            bad += 1
            continue
        if got != want:
            out("FAIL %s: decoded %d bytes, expected %d, content differs" % (n[:-7], len(got), len(want)))
            bad += 1
        else:
            out("ok   %s (%d bytes, %s)" % (n[:-7], len(got), "plain" if os.path.isfile(target) else "z"))
    out("dump_io check-dir: %d of %d files decode to the C++ writer's input" % (len(names) - bad, len(names)))
    return 1 if bad else 0


# --- self-test -------------------------------------------------------------

def _self_test() -> int:
    import random
    fails = []

    def check(cond, what):
        if not cond:
            fails.append(what)
            print("FAIL:", what)

    def raises(exc, fn, what):
        try:
            fn()
        except exc:
            return
        except Exception as e:   # the wrong error is a failure too
            check(False, "%s raised %s" % (what, type(e).__name__))
            return
        check(False, "%s did not raise" % what)

    rng = random.Random(7)
    with tempfile.TemporaryDirectory() as tmp:
        def p(n):
            return os.path.join(tmp, n)

        incompressible = bytes(rng.getrandbits(8) for _ in range(300000))
        cases = {
            "empty": b"",
            "one": b"\x5a",
            "exact": bytes((i * 7) & 255 for i in range(CHUNK_BYTES)),
            "multi": bytes((i // 3) & 255 for i in range(CHUNK_BYTES * 2 + 12345)),
            "random": incompressible,
        }
        for name, data in cases.items():
            write_z(p(name + ".bin"), data)
            check(read_bytes(p(name + ".bin")) == data, "round trip: " + name)
            check(exists(p(name + ".bin")) and not os.path.exists(p(name + ".bin")), "exists() sees only the .z: " + name)
            check(getsize(p(name + ".bin")) == len(data), "getsize: " + name)
        # the chunk count is the layout the C++ writer wrote
        with open(p("multi.bin.z"), "rb") as fh:
            h = fh.read(_FIXED)
        check(struct.unpack_from("<I", h, 28)[0] == 3, "multi-chunk file has 3 chunks")
        # a plain file reads as itself and wins over a stale .z
        with open(p("plain.bin"), "wb") as fh:
            fh.write(b"plain bytes")
        write_z(p("plain.bin"), b"stale")
        check(read_bytes(p("plain.bin")) == b"plain bytes", "the plain file wins")
        check(read_bytes(p("plain.bin.z")) == b"stale", "naming the .z reads the container")
        os.remove(p("plain.bin.z"))
        # a delta against a base
        base = bytes(rng.getrandbits(8) for _ in range(5000))
        second = bytearray(base)
        for i in range(0, 5000, 97):
            second[i] ^= 0x55
        with open(p("T00.bmp"), "wb") as fh:
            fh.write(base)
        write_z(p("P00.bmp"), bytes(second), base=p("T00.bmp"))
        check(read_bytes(p("P00.bmp")) == bytes(second), "XOR delta round trip")
        check(os.path.getsize(p("P00.bmp.z")) < 1500, "the delta of nearly identical files is small")
        with open(p("T00.bmp"), "wb") as fh:
            fh.write(base[:-1])
        raises(DumpIoError, lambda: read_bytes(p("P00.bmp")), "base size mismatch")
        os.remove(p("T00.bmp"))
        raises(DumpIoError, lambda: read_bytes(p("P00.bmp")), "base missing")
        raises(FileNotFoundError, lambda: read_bytes(p("nothing.bin")), "no such file")
        # a delta cannot be a base
        with open(p("T00.bmp"), "wb") as fh:
            fh.write(base)
        write_z(p("P01.bmp"), bytes(second), base=p("T00.bmp"))
        os.replace(p("P01.bmp.z"), p("B.bmp.z"))
        raises(DumpIoError, lambda: write_z(p("P02.bmp"), b"x" * 5000, base=p("B.bmp")), "writer refuses a delta base")
        # corrupt and truncated
        good = open(p("multi.bin.z"), "rb").read()
        corrupt = bytearray(good)
        corrupt[200] ^= 0xFF
        with open(p("c1.bin.z"), "wb") as fh:
            fh.write(corrupt)
        raises(DumpIoError, lambda: read_bytes(p("c1.bin")), "corrupt chunk")
        for cut, label in ((10, "inside the header"), (len(good) - 5, "inside the last chunk"), (_FIXED + 2, "before the first chunk")):
            with open(p("t.bin.z"), "wb") as fh:
                fh.write(good[:cut])
            raises(DumpIoError, lambda: read_bytes(p("t.bin")), "truncated " + label)
        with open(p("t.bin.z"), "wb") as fh:
            fh.write(good + b"\0")
        raises(DumpIoError, lambda: read_bytes(p("t.bin")), "trailing byte")
        badmagic = b"EDVRZ2\0\0" + good[8:]
        with open(p("t.bin.z"), "wb") as fh:
            fh.write(badmagic)
        raises(DumpIoError, lambda: read_bytes(p("t.bin")), "bad magic")
        wrongver = good[:8] + struct.pack("<I", 2) + good[12:]
        with open(p("t.bin.z"), "wb") as fh:
            fh.write(wrongver)
        raises(DumpIoError, lambda: read_bytes(p("t.bin")), "unknown version")
        os.remove(p("t.bin.z"))
        # glob: plain and .z once each, as original names
        with open(p("g1.bin"), "wb") as fh:
            fh.write(b"a")
        write_z(p("g2.bin"), b"b")
        write_z(p("g1.bin"), b"stale")   # both forms of one name
        got = [os.path.basename(x) for x in glob(p("g*.bin"))]
        check(got == ["g1.bin", "g2.bin"], "glob dedupes and strips .z: %r" % got)
        # expand: byte-for-byte, writes nothing on a dry run, deletes nothing
        os.remove(p("g1.bin.z"))
        before = sorted(os.listdir(tmp))
        res = expand(tmp, dry_run=True, out=lambda s: None)
        check(sorted(os.listdir(tmp)) == before, "--expand --dry-run wrote nothing")
        check(res["written"] > 0, "the dry run counted what it would write")
        # the base T00.bmp is plain, so P00 is expandable once it is rewritten
        os.remove(p("c1.bin.z"))
        os.remove(p("B.bmp.z"))
        write_z(p("P00.bmp"), bytes(second), base=p("T00.bmp"))
        res = expand(tmp, out=lambda s: None)
        check(res["error"] == 0 and res["conflict"] == 0, "expand ran clean: %r" % (res,))
        check(open(p("multi.bin"), "rb").read() == cases["multi"], "expanded multi-chunk bytes equal")
        check(open(p("P00.bmp"), "rb").read() == bytes(second), "expanded delta bytes equal")
        check(os.path.isfile(p("multi.bin.z")), "expand deletes nothing")
        res = expand(tmp, out=lambda s: None)
        check(res["written"] == 0 and res["same"] > 0, "a second expand finds the same files and writes nothing")
        with open(p("multi.bin"), "wb") as fh:
            fh.write(b"changed")
        res = expand(tmp, out=lambda s: None)
        check(res["conflict"] == 1, "expand does not overwrite a different file")
        check(open(p("multi.bin"), "rb").read() == b"changed", "the differing file was left alone")
        # a stamp filter
        os.remove(p("empty.bin"))
        res = expand(tmp, stamp="nomatch", dry_run=True, out=lambda s: None)
        check(res["written"] == 0, "the stamp filter selects nothing")
    # check_dir: every .expect decodes
    with tempfile.TemporaryDirectory() as tmp:
        write_z(os.path.join(tmp, "a.bin"), b"hello" * 100)
        with open(os.path.join(tmp, "a.bin.expect"), "wb") as fh:
            fh.write(b"hello" * 100)
        check(check_dir(tmp, out=lambda s: None) == 0, "check_dir passes a good dir")
        with open(os.path.join(tmp, "a.bin.expect"), "wb") as fh:
            fh.write(b"hello")
        check(check_dir(tmp, out=lambda s: None) == 1, "check_dir fails a bad dir")
    if fails:
        print("dump_io self-test: %d failure(s)" % len(fails))
        return 1
    print("dump_io self-test: ok")
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--expand", metavar="DIR", help="write the original files beside the .z ones in DIR")
    ap.add_argument("--stamp", help="only files whose name contains this stamp")
    ap.add_argument("--dry-run", action="store_true", help="write nothing at all")
    ap.add_argument("--check-dir", metavar="DIR", help="the build gate: each NAME.expect must equal read_bytes(NAME)")
    ap.add_argument("--self-test", action="store_true")
    a = ap.parse_args(argv)
    if a.self_test:
        return _self_test()
    if a.check_dir:
        return check_dir(a.check_dir)
    if a.expand:
        try:
            res = expand(a.expand, a.stamp, a.dry_run)
        except DumpIoError as exc:
            print("error: %s" % exc)
            return 2
        print("%s: %d file(s) %s, %d identical already, %d conflict, %d error, %.1f MB"
              % (a.expand, res["written"], "would be written" if a.dry_run else "written", res["same"],
                 res["conflict"], res["error"], res["bytes"] / 1048576.0))
        return 1 if res["conflict"] or res["error"] else 0
    ap.print_help()
    return 2


if __name__ == "__main__":
    sys.exit(main())
