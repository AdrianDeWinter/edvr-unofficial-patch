#!/usr/bin/env python3
"""A session's log read as one: its head file and its tail segments (src\\common\\log_rotate.h).

With log.max_mb in force, the log keeps a session's start in its own file, the head (edvr_<tag>_YYYYMMDD_HHMMSS.log, or the _mmm_pid form), and
writes the rest to rotating segments beside it, edvr_<tag>_YYYYMMDD_HHMMSS.tail-NNN.log, of which only the newest four stay on disk. After the head
ends, it also carries the first line of each kind written after that and the memory watch's lines, under a marker for the segment each belongs to.

stitch() puts that back in order: the head up to its end marker; then for k = 1..last the segment's own text when the file exists, else one GAP line
and the lines the head kept from that segment. Every tool that reads a flight log goes through read_log_text() (tools\\edvr_log.py owns the
names). The markers are written by Log in src\\common\\log.cpp; their texts are pinned there by tools\\config_test and here by --self-test.

    python tools\\log_session.py --self-test
    python tools\\log_session.py --check-dir <dir> --expect <file>     (the rig's handoff: tools\\config_test writes both)
"""

import os
import re
import sys
import tempfile

# The texts Log writes (src\common\log.cpp). No timestamp, and each starts "[edvr] " like the other notices.
HEAD_CUT = "[edvr] log head ends here at "
SEGMENT_TOP = "[edvr] tail segment "
KEPT_FROM = "[edvr] kept lines from tail segment "
KEPT_SPENT = ("[edvr] the kept-lines budget is spent (the first lines of new kinds, or the memory watch's lines, no longer fit here); "
              "later ones are only in the tail segments still on disk.")
HEAD_CUT_RE = re.compile(r"^\[edvr\] log head ends here at (?P<head>\d+) KB \(a quarter of log\.max_mb = (?P<mb>\d+) MB\); "
                         r"the log continues in (?P<first>\S+), only the newest 4 tail segments are kept, and what follows below is the "
                         r"first line of each kind written after this point and the memory watch's lines, up to (?P<kept>\d+) KB\.$")
SEGMENT_TOP_RE = re.compile(r"^\[edvr\] tail segment (?P<n>\d+) of (?P<head>\S+)\.(?: Deleted so far:(?P<deleted>.*)\.)?$")
GAP_FORMAT = "[edvr_log] GAP: tail segment %d was deleted; the first line of each kind kept from it follows"
VOLUME_RE = re.compile(r"^\[\d\d:\d\d:\d\d\.\d\d\d\] log volume \(last (?P<n>\d+) (?P<unit>minutes|ms)\): (?P<mb>\d+\.\d\d) MB in (?P<lines>\d+) lines\. "
                       r"Top kinds by size: \"(?P<kind>.*?)\" (?P<kmb>\d+\.\d\d) MB (?P<klines>\d+) lines")
STAMP_RE = re.compile(r"^\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\] ")
SEGMENT_FILE_RE = re.compile(r"^(\d{3,})\.log$", re.IGNORECASE)

TAIL_SEGMENTS = 4


def _read(path):
    with open(path, "rb") as f:
        return f.read().decode("utf-8", errors="replace")


def _with_newline(text):
    return text if not text or text.endswith("\n") else text + "\r\n"


def segment_files(head_path):
    """{n: path} of the tail segments beside a head log. The stem is exact, so the _mmm_pid form of another session is never taken for this one's."""
    directory, name = os.path.split(head_path)
    stem = name[:-4] if name.lower().endswith(".log") else name
    prefix = (stem + ".tail-").lower()
    found = {}
    try:
        names = os.listdir(directory or ".")
    except OSError:
        return found
    for n in names:
        if not n.lower().startswith(prefix):
            continue
        m = SEGMENT_FILE_RE.match(n[len(prefix):])
        if m:
            found[int(m.group(1))] = os.path.join(directory, n)
    return found


def split_head(text):
    """(the head's lines up to and including its end marker, {segment: its kept lines}, whether the marker is there, whether the kept-lines budget notice is)."""
    lines = text.splitlines(True)
    at = None
    for i, line in enumerate(lines):
        if line.startswith(HEAD_CUT):
            at = i
            break
    if at is None:
        return lines, {}, False, False
    kept = {}
    current = None
    spent = False
    for line in lines[at + 1:]:
        s = line.rstrip("\r\n")
        if s.startswith(KEPT_FROM):
            try:
                current = int(s[len(KEPT_FROM):])
            except ValueError:
                current = None
        elif s == KEPT_SPENT:
            spent = True
        elif s.startswith("[edvr] "):
            continue
        elif current is not None:
            kept.setdefault(current, []).append(line)
    return lines[:at + 1], kept, True, spent


def stitch(head_path, text=None):
    """(the session's text, info). info: cut (the head's end marker is there), last (the newest segment's number), on_disk and deleted (sorted
    segment numbers), spent (the kept-lines budget ran out)."""
    if text is None:
        text = _read(head_path)
    before, kept, cut, spent = split_head(text)
    files = segment_files(head_path)
    info = {"cut": cut, "last": 0, "on_disk": sorted(files), "deleted": [], "spent": spent}
    if not cut and not files:
        return text, info
    last = max(list(files) + list(kept) + [0])
    info["last"] = last
    info["deleted"] = [k for k in range(1, last + 1) if k not in files]
    parts = ["".join(before)]
    for k in range(1, last + 1):
        parts[-1] = _with_newline(parts[-1])
        if k in files:
            parts.append(_read(files[k]))
        else:
            parts.append((GAP_FORMAT % k) + "\r\n")
            parts.append("".join(kept.get(k, [])))
    return "".join(parts), info


def read_log_text(path):
    """A flight log's text: the stitched session for a head log, the file itself for anything else (a lone segment named by --file, a runtime log
    without a cut)."""
    text = _read(path)
    if re.search(r"\.tail-\d{3,}\.log$", path, re.IGNORECASE):
        return text
    if text.find("\n" + HEAD_CUT) < 0 and not text.startswith(HEAD_CUT) and not segment_files(path):
        return text
    return stitch(path, text)[0]


def _ranges(numbers):
    out = []
    for n in sorted(numbers):
        if out and out[-1][1] == n - 1:
            out[-1][1] = n
        else:
            out.append([n, n])
    return ", ".join(str(a) if a == b else "%d-%d" % (a, b) for a, b in out)


def describe(head_path):
    """For --list: what a head's tail looks like on disk, or "" for a log with no cut."""
    try:
        _, info = stitch(head_path)
    except OSError:
        return ""
    if not info["cut"] and not info["on_disk"]:
        return ""
    on = info["on_disk"]
    s = "%d tail segment(s) on disk" % len(on)
    if on:
        s += " (%s)" % _ranges(on)
    if info["deleted"]:
        s += ", deleted %s" % _ranges(info["deleted"])
    return s


def stamp_ms(line):
    m = STAMP_RE.match(line)
    return None if not m else ((int(m.group(1)) * 60 + int(m.group(2))) * 60 + int(m.group(3))) * 1000 + int(m.group(4))


def plan(max_bytes):
    """H, K, S of log_rotate.h's LogPlan, written again here on purpose: this is the other side of the cross-language check."""
    head = max_bytes // 4
    kept = max_bytes // 8
    tail = max_bytes - head - kept
    return head, kept, tail // TAIL_SEGMENTS


def check_dir(directory, head_re, expect_path):
    """The rig handoff. Reads <expect_path> (lines `tag<TAB>key<TAB>value`), then checks each tag's session in <directory> against what Log promises.
    Returns the number of failures; prints each."""
    fails = []
    checks = [0]

    def check(tag, ok, what):
        checks[0] += 1
        if not ok:
            fails.append("FAIL %s: %s" % (tag, what))

    expect = {}
    with open(expect_path, "r", encoding="utf-8") as f:
        for raw in f.read().splitlines():
            if not raw.strip():
                continue
            tag, key, value = (raw.split("\t") + ["", ""])[:3]
            expect.setdefault(tag, []).append((key, value))
    if not expect:
        fails.append("FAIL: %s names no session" % expect_path)
    heads = {}
    for name in sorted(os.listdir(directory)):
        m = head_re.match(name)
        if m:
            heads.setdefault(m.group("tag"), []).append(name)
    for tag, rows in sorted(expect.items()):
        names = heads.get(tag, [])
        check(tag, len(names) == 1, "want exactly one head log, found %r" % names)
        if len(names) != 1:
            continue
        head_name = names[0]
        head_path = os.path.join(directory, head_name)
        kv = {}
        for key, value in rows:
            kv.setdefault(key, []).append(value)
        max_bytes = int(kv["max_bytes"][0])
        head_limit, kept_limit, seg_limit = plan(max_bytes)
        slack = 8192   # the markers, a drop notice, a line wider than the room

        head_text = _read(head_path)
        before, kept, cut, _ = split_head(head_text)
        check(tag, cut, "the head has no end marker")
        check(tag, sum(1 for l in head_text.splitlines() if l.startswith(HEAD_CUT)) == 1, "the head's end marker is not exactly once")
        head_bytes = os.path.getsize(head_path)
        check(tag, head_bytes <= head_limit + kept_limit + slack, "head %d bytes > H %d + K %d + markers" % (head_bytes, head_limit, kept_limit))
        before_bytes = len("".join(before).encode("utf-8"))
        check(tag, before_bytes <= head_limit + slack, "the head up to its marker is %d bytes, over H %d" % (before_bytes, head_limit))
        files = segment_files(head_path)
        check(tag, 0 < len(files) <= TAIL_SEGMENTS, "%d segment files on disk, want 1..%d" % (len(files), TAIL_SEGMENTS))
        if files:
            numbers = sorted(files)
            check(tag, numbers == list(range(numbers[0], numbers[-1] + 1)), "the segments on disk are not consecutive: %r" % numbers)
        total = head_bytes
        for n, path in sorted(files.items()):
            size = os.path.getsize(path)
            total += size
            check(tag, size <= seg_limit + slack, "segment %d is %d bytes > S %d" % (n, size, seg_limit))
            first = _read(path).splitlines()[:1]
            m = SEGMENT_TOP_RE.match(first[0]) if first else None
            check(tag, bool(m) and int(m.group("n")) == n and m.group("head") == head_name,
                  "segment %d does not open with its marker: %r" % (n, first))
        check(tag, total <= max_bytes + 2 * slack, "the session is %d bytes on disk > max_mb %d" % (total, max_bytes))
        for path in [head_path] + [p for _, p in sorted(files.items())]:
            body = _read(path)
            check(tag, body.endswith("\r\n"), "%s does not end at a line" % os.path.basename(path))
            for i, line in enumerate(body.splitlines()):
                if not (STAMP_RE.match(line) or line.startswith("[edvr] ")):
                    check(tag, False, "%s line %d is not a whole line: %r" % (os.path.basename(path), i + 1, line[:60]))
                    break

        text, info = stitch(head_path, head_text)
        if files:
            last = info["last"]
            check(tag, sorted(files) == list(range(last - len(files) + 1, last + 1)), "the newest segments are not the ones on disk")
            check(tag, text.count("[edvr_log] GAP: tail segment ") == len(info["deleted"]), "a GAP line for each deleted segment, once")
            if last >= TAIL_SEGMENTS + 1:
                top = SEGMENT_TOP_RE.match(_read(files[last]).splitlines()[0])
                check(tag, bool(top) and top.group("deleted") is not None, "the newest segment's marker does not list what was deleted")
        last_ms = None
        for line in text.splitlines():
            ms = stamp_ms(line)
            if ms is None:
                continue
            if last_ms is not None and ms + 100 < last_ms and last_ms - ms < 12 * 3600 * 1000:
                check(tag, False, "stitched order goes back in time: %r after %d ms" % (line[:40], last_ms))
                break
            last_ms = ms
        lines = [l for l in text.splitlines() if l.strip()]
        for key, values in kv.items():
            for value in values:
                if key == "once":
                    n = sum(1 for l in lines if l.endswith(value))
                    check(tag, n == 1, "%r is on %d lines of the stitched session, want 1" % (value, n))
                elif key == "newest":
                    check(tag, bool(lines) and lines[-1].endswith(value), "the stitched session does not end with %r: %r" % (value, lines[-1:] ))
                elif key == "dropped":
                    check(tag, any(l.startswith("[edvr] ") and " log line(s) were DROPPED here" in l for l in lines), "no DROPPED notice")
                elif key == "volume":
                    check(tag, any(VOLUME_RE.match(l) for l in lines), "no log volume line")
                elif key == "spent":
                    check(tag, sum(1 for l in head_text.splitlines() if l == KEPT_SPENT) == 1, "the kept-lines budget notice is not exactly once")
                elif key == "pidform":
                    check(tag, re.search(r"_\d{3}_\d+\.log$", head_name) is not None and all(os.path.basename(p).startswith(head_name[:-4] + ".tail-") for p in files.values()),
                          "the _mmm_pid head's segments do not carry its name")
                elif key == "min_segments":
                    check(tag, info["last"] >= int(value), "only %d segments were opened, want %s" % (info["last"], value))
                elif key == "max_bytes":
                    pass
                else:
                    check(tag, False, "unknown expectation %r" % key)
    for f in fails:
        print(f)
    print("[log_session] check-dir: %d checks, %d failed" % (checks[0], len(fails)))
    return len(fails)


# ---------------------------------------------------------------------------------------------------------------------------------------------
# self-test


def _stamp(i):
    return "[10:%02d:%02d.%03d] " % (i // 60 % 60, i % 60, (i * 7) % 1000)


def _line(i, text):
    return _stamp(i) + text + "\r\n"


def _write(path, text):
    with open(path, "wb") as f:
        f.write(text.encode("utf-8"))


def _session(directory, stem, head_lines, segments, kept_by_segment, last, spent=False):
    """Write a head, the segments in `segments` ({n: [lines]}) and the head's kept lines (`kept_by_segment` {n: [lines]}), as Log would."""
    leaf = stem + ".log"
    head = "".join(head_lines)
    head += ("[edvr] log head ends here at 256 KB (a quarter of log.max_mb = 1 MB); the log continues in %s.tail-001.log, only the newest 4 tail "
             "segments are kept, and what follows below is the first line of each kind written after this point and the memory watch's lines, up "
             "to 128 KB.\r\n" % stem)
    for k in sorted(kept_by_segment):
        head += "[edvr] kept lines from tail segment %d\r\n" % k
        head += "".join(kept_by_segment[k])
    if spent:
        head += KEPT_SPENT + "\r\n"
    _write(os.path.join(directory, leaf), head)
    for n, lines in segments.items():
        deleted = "" if n <= 4 else " Deleted so far: segment 1 (163840 bytes, 10:00:00.000 to 10:00:09.000)."
        _write(os.path.join(directory, "%s.tail-%03d.log" % (stem, n)), "[edvr] tail segment %d of %s.%s\r\n" % (n, leaf, deleted) + "".join(lines))


def self_test():
    ok = True

    def fail(msg):
        nonlocal ok
        print("log_session self-test: " + msg)
        ok = False

    # The marker texts, typed out again here, are the ones src\common\log.cpp writes and tools\config_test pins.
    cut = ("[edvr] log head ends here at 256 KB (a quarter of log.max_mb = 1 MB); the log continues in edvr_gfx_20261110_060503.tail-001.log, only the "
           "newest 4 tail segments are kept, and what follows below is the first line of each kind written after this point and the memory watch's "
           "lines, up to 128 KB.")
    if not HEAD_CUT_RE.match(cut) or not cut.startswith(HEAD_CUT):
        fail("the head's end marker is not recognized")
    top = "[edvr] tail segment 6 of edvr_gfx_20261110_060503.log. Deleted so far: segment 1 (163840 bytes, 06:05:03.100 to 06:05:41.900); segment 2 (163840 bytes, 06:05:41.900 to 06:06:10.000)."
    m = SEGMENT_TOP_RE.match(top)
    if not m or m.group("n") != "6" or "segment 2 (" not in m.group("deleted"):
        fail("a segment's top marker is not recognized")
    if not SEGMENT_TOP_RE.match("[edvr] tail segment 1 of edvr_gfx_20261110_060503.log."):
        fail("segment 1's top marker (nothing deleted) is not recognized")
    if KEPT_FROM + "3" != "[edvr] kept lines from tail segment 3":
        fail("the kept-lines marker text changed")
    if KEPT_SPENT != ("[edvr] the kept-lines budget is spent (the first lines of new kinds, or the memory watch's lines, no longer fit here); later ones "
                      "are only in the tail segments still on disk."):
        fail("the budget-spent marker text changed")
    if GAP_FORMAT % 2 != "[edvr_log] GAP: tail segment 2 was deleted; the first line of each kind kept from it follows":
        fail("the GAP line's text changed")
    vol = '[10:05:00.250] log volume (last 5 minutes): 4.62 MB in 31204 lines. Top kinds by size: "DC frame " 2.10 MB 12000 lines; "x " 0.90 MB 400 lines'
    if not VOLUME_RE.match(vol):
        fail("the volume line is not recognized")

    tmp = tempfile.mkdtemp(prefix="log_session_")
    try:
        # No cut: the file as it is.
        plain = os.path.join(tmp, "edvr_gfx_20261110_060503.log")
        _write(plain, _line(1, "version 1") + _line(2, "b"))
        text, info = stitch(plain)
        if text != _line(1, "version 1") + _line(2, "b") or info["cut"] or info["last"] != 0 or describe(plain) != "":
            fail("a log with no cut is changed by the reader: %r %r" % (text, info))
        if read_log_text(plain) != text:
            fail("read_log_text changed a log with no cut")

        # A cut with every segment present: 1..3, and the head's kept lines (copies of first lines in the segments) are not repeated.
        d = os.path.join(tmp, "cut")
        os.mkdir(d)
        stem = "edvr_gfx_20261110_060503"
        _session(d, stem, [_line(1, "version 1"), _line(2, "head line")],
                 {1: [_line(3, "alpha #0"), _line(4, "x")], 2: [_line(5, "beta #0")], 3: [_line(6, "end")]},
                 {1: [_line(3, "alpha #0")], 2: [_line(5, "beta #0")]}, 3)
        text, info = stitch(os.path.join(d, stem + ".log"))
        want = [l for l in (_line(1, "version 1") + _line(2, "head line") + cut_line(stem)).splitlines()]
        got = [l for l in text.splitlines()]
        if got[:3] != want or text.count("alpha #0") != 1 or text.count("beta #0") != 1 or "GAP" in text:
            fail("a cut session is not stitched in order, once: %r" % got)
        if [l for l in got if l.startswith("[edvr] tail segment")] != ["[edvr] tail segment 1 of %s.log." % stem, "[edvr] tail segment 2 of %s.log." % stem,
                                                                      "[edvr] tail segment 3 of %s.log." % stem]:
            fail("the segments' own markers are not kept: %r" % got)
        if info["last"] != 3 or info["deleted"] != [] or describe(os.path.join(d, stem + ".log")) != "3 tail segment(s) on disk (1-3)":
            fail("describe/info of a whole session: %r %r" % (info, describe(os.path.join(d, stem + ".log"))))
        if read_log_text(os.path.join(d, stem + ".log")) != text:
            fail("read_log_text is not the stitched text")
        if read_log_text(os.path.join(d, stem + ".tail-002.log")).count("beta #0") != 1 or "alpha" in read_log_text(os.path.join(d, stem + ".tail-002.log")):
            fail("a segment named by --file is not read on its own")

        # Deleted segments with kept lines: 1 and 2 gone, 3..6 on disk; segment 2 kept nothing.
        d = os.path.join(tmp, "deleted")
        os.mkdir(d)
        _session(d, stem, [_line(1, "version 1")],
                 {3: [_line(30, "c")], 4: [_line(40, "d")], 5: [_line(50, "e")], 6: [_line(60, "f")]},
                 {1: [_line(10, "alpha #0"), _line(11, "gamma #0")], 3: [_line(30, "c")], 5: [_line(50, "e")], 6: [_line(60, "f")]}, 6)
        text, info = stitch(os.path.join(d, stem + ".log"))
        order = [l for l in text.splitlines() if not l.startswith("[edvr] tail") and not l.startswith("[edvr] log head")]
        want_order = [_line(1, "version 1").rstrip(), GAP_FORMAT % 1, _line(10, "alpha #0").rstrip(), _line(11, "gamma #0").rstrip(), GAP_FORMAT % 2,
                      _line(30, "c").rstrip(), _line(40, "d").rstrip(), _line(50, "e").rstrip(), _line(60, "f").rstrip()]
        if order != want_order:
            fail("deleted segments are not stitched as a GAP and their kept lines: %r" % order)
        if info["deleted"] != [1, 2] or describe(os.path.join(d, stem + ".log")) != "4 tail segment(s) on disk (3-6), deleted 1-2":
            fail("describe of a session with deleted segments: %r" % describe(os.path.join(d, stem + ".log")))

        # A segment missing in the middle (4 of 3..6), and the _mmm_pid form of the name.
        d = os.path.join(tmp, "middle")
        os.mkdir(d)
        pid_stem = "edvr_gfx_20261110_060503_123_4567"
        _session(d, pid_stem, [_line(1, "version 1")],
                 {3: [_line(30, "c")], 5: [_line(50, "e")], 6: [_line(60, "f")]},
                 {4: [_line(40, "delta #0")]}, 6)
        # Another session with the plain name beside it: its segments are not this one's.
        _write(os.path.join(d, "edvr_gfx_20261110_060503.tail-009.log"), "x\r\n")
        text, info = stitch(os.path.join(d, pid_stem + ".log"))
        if info["deleted"] != [1, 2, 4] or text.count(GAP_FORMAT % 4) != 1 or text.count("delta #0") != 1 or 9 in info["on_disk"]:
            fail("a missing middle segment or the _mmm_pid form: %r" % info)
        lines = text.splitlines()
        i = lines.index(GAP_FORMAT % 4)
        if lines[i + 1].find("delta #0") < 0 or lines[i - 1].find("c") < 0:
            fail("the GAP line for segment 4 is not between 3 and 5 with its kept line: %r" % lines)
        if sorted(segment_files(os.path.join(d, pid_stem + ".log"))) != [3, 5, 6]:
            fail("segment_files took another session's segments")

        # check_dir: a good session passes, and each broken one is named.
        d = os.path.join(tmp, "dir")
        os.mkdir(d)
        big = "y" * 900
        head_lines = [_line(1, "version 1")] + [_line(2, "head fill %d %s" % (i, big)) for i in range(280)]
        segs = {}
        for n in range(3, 7):
            segs[n] = [_line(10 * n, "seg %d line %d %s" % (n, i, big)) for i in range(150)] + [_line(10 * n + 1, "newest %d" % n)]
        _session(d, stem, head_lines, segs, {1: [_line(10, "alpha #0")], 2: [_line(20, "beta #0")], 3: [_line(30, "c")]}, 6)
        expect = os.path.join(d, "rotation.expect")
        _write(expect, "gfx\tmax_bytes\t1048576\ngfx\tonce\talpha #0\ngfx\tonce\tbeta #0\ngfx\tnewest\tnewest 6\ngfx\tmin_segments\t6\n")
        head_re = re.compile(r"^edvr_(?P<tag>[a-z0-9]+)_(?P<stamp>\d{8}_\d{6})(?:_(?P<ms>\d{3})_(?P<pid>\d+))?\.log$", re.IGNORECASE)
        import io
        import contextlib
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            bad = check_dir(d, head_re, expect)
        if bad != 0:
            fail("check_dir fails a good session:\n" + buf.getvalue())
        # Broken: a fifth segment on disk, a line split in a segment, a token twice, the wrong newest.
        _write(os.path.join(d, stem + ".tail-007.log"), "[edvr] tail segment 7 of %s.log.\r\n" % stem + _line(70, "newest 7"))
        os.rename(os.path.join(d, stem + ".tail-003.log"), os.path.join(d, stem + ".tail-003.keep"))
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            bad = check_dir(d, head_re, expect)
        out = buf.getvalue()
        if bad < 1 or "does not end with" not in out:
            fail("check_dir passes a session whose newest line is not the expected one: %s" % out)
        os.rename(os.path.join(d, stem + ".tail-003.keep"), os.path.join(d, stem + ".tail-003.log"))
        os.remove(os.path.join(d, stem + ".tail-007.log"))
        with open(os.path.join(d, stem + ".tail-006.log"), "ab") as f:
            f.write(b"half a line")
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            bad = check_dir(d, head_re, expect)
        if bad < 1 or "does not end at a line" not in buf.getvalue():
            fail("check_dir passes a segment that ends in half a line: %s" % buf.getvalue())
    finally:
        import shutil
        shutil.rmtree(tmp, ignore_errors=True)
    print("log_session self-test: %s" % ("ok" if ok else "FAILED"))
    return ok


def cut_line(stem):
    return ("[edvr] log head ends here at 256 KB (a quarter of log.max_mb = 1 MB); the log continues in %s.tail-001.log, only the newest 4 tail "
            "segments are kept, and what follows below is the first line of each kind written after this point and the memory watch's lines, up "
            "to 128 KB." % stem)


def main(argv=None):
    import argparse
    ap = argparse.ArgumentParser(description="Read a session's head and tail segments as one log; check a rig's session directory.")
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--check-dir", default=None, help="a directory tools\\config_test wrote sessions to")
    ap.add_argument("--expect", default=None, help="with --check-dir: the rotation.expect file beside them")
    ap.add_argument("--show", default=None, help="print the stitched session of this head log")
    args = ap.parse_args(argv)
    if args.self_test:
        return 0 if self_test() else 1
    if args.show:
        sys.stdout.write(read_log_text(args.show))
        return 0
    if args.check_dir:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import edvr_log
        return 1 if check_dir(args.check_dir, edvr_log.LOG_RE, args.expect or os.path.join(args.check_dir, "rotation.expect")) else 0
    ap.print_help()
    return 1


if __name__ == "__main__":
    sys.exit(main())
