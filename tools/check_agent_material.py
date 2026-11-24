#!/usr/bin/env python3
"""Check that agent material stays where a clone can strip it, and that no
tracked text hides characters a reader cannot see.

AGENTS.md "Agent material": agent tool instruction files, .codemap\\ config,
and everything under .agents\\ (guides, handoff prompts, plans) must be
export-ignore in .gitattributes, so `git archive` and GitHub's source zips
leave them out. The checker recognizes conventional instruction and prompt
names; the placement rule also applies to agent-only material with other names.
docs\\ is written for people. When .codemap\\config.json is present, the
checker also rejects tracked/new files under .codemap\\skills\\ (agent skills
stay under .agents\\) and verifies every path listed under
routing.subsystems[].docs exists inside the repository.

Every tracked or new (untracked, not ignored) text file is also scanned for
characters that render as nothing: zero-width characters, bidirectional
controls and Unicode tag characters, the usual carriers of hidden text. A
byte-order mark is allowed as the first character of a file and nowhere else.

    python tools\\check_agent_material.py            # check the repository
    python tools\\check_agent_material.py --list     # print the agent material
    python tools\\check_agent_material.py --self-test

Exit 0 when clean, 1 on any finding, 2 on usage or git errors. Prints paths,
line numbers and code points, never file content. Read-only: this tool writes
nothing (the self-test writes only to a temporary directory).
"""
import json
import os
import re
import shutil
import stat
import subprocess
import sys
import tempfile
from unittest import mock

# Directories whose whole content is agent material, matched case-insensitively
# on any path component: on Windows .Codex and .codex are one folder.
AGENT_DIRS = {".agents", ".codemap", ".claude", ".codex", ".cursor", ".continue",
              ".gemini", ".windsurf", ".clinerules"}
# Agent directories that hold tracked project-level agent config/material;
# everything else in AGENT_DIRS holds tool-local session state and worktrees.
TRACKED_AGENT_DIRS = {".agents", ".codemap"}
# Instruction files agent tools read, by file name, anywhere in the tree.
AGENT_FILES = {"agents.md", "claude.md", "claude.local.md", "gemini.md",
               ".cursorrules", ".windsurfrules", "copilot-instructions.md"}
AGENT_PREFIXES = (".aider", ".clinerules", "handoff-prompt-")
AGENT_SUFFIXES = (".instructions.md", ".prompt.md")

# Soft hyphen, Arabic letter mark, Mongolian vowel separator, zero-width
# space/non-joiner/joiner, LRM/RLM, bidi embeddings and overrides, word joiner
# and the invisible operators, bidi isolates, BOM, and the tag characters.
# Escapes only: this file is scanned too.
INVISIBLE = re.compile(r"[\u00ad\u061c\u180e\u200b-\u200f\u202a-\u202e"
                       r"\u2060-\u2064\u2066-\u2069\ufeff\U000e0000-\U000e007f]")
BOM = "\ufeff"


class GitError(Exception):
    pass


def git(root, *args, stdin=None):
    try:
        result = subprocess.run(["git", "-C", root] + list(args), input=stdin,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    except OSError as e:
        raise GitError("git not runnable: %s" % e)
    if result.returncode:
        raise GitError("git %s failed: %s" % (" ".join(args),
                       result.stderr.decode("utf-8", "replace").strip()))
    return result.stdout


def work_tree(root):
    """True when root is the top of a git work tree. A build from a source zip
    has no .git and nothing to commit, so the check skips it (build_receipt.py
    falls back the same way)."""
    try:
        top = git(root, "rev-parse", "--show-toplevel").decode("utf-8", "replace").strip()
    except GitError:
        return False
    same = lambda p: os.path.normcase(os.path.realpath(p))
    return same(top) == same(root)


def _listed(root, *args):
    out = git(root, "ls-files", "-z", *args)
    return set(p for p in out.decode("utf-8", "surrogateescape").split("\0") if p)


def repo_files(root):
    """(tracked, new) as git spells them. New is untracked files git does not
    ignore, less nested repositories ("dir/") and anything under an agent tool's
    own folder (.claude, .codex, ...): those hold the tool's worktrees and
    session state on this machine, not the project (build_receipt.py excludes
    them too)."""
    tracked = _listed(root, "--cached")
    new = set(p for p in _listed(root, "--others", "--exclude-standard") - tracked
              if not p.endswith("/") and not in_local_agent_dir(p))
    return tracked, new


def in_local_agent_dir(rel):
    return any(p.lower() in AGENT_DIRS - TRACKED_AGENT_DIRS for p in rel.split("/")[:-1])


def is_agent_material(rel):
    parts = rel.split("/")
    if any(p.lower() in AGENT_DIRS for p in parts[:-1]):
        return True
    name = parts[-1].lower()
    return (name in AGENT_FILES or name.startswith(AGENT_PREFIXES)
            or name.endswith(AGENT_SUFFIXES))


def export_ignored(root, paths):
    """{path: True when .gitattributes sets export-ignore on it}."""
    if not paths:
        return {}
    out = git(root, "check-attr", "-z", "--stdin", "export-ignore",
              stdin="\0".join(paths).encode("utf-8", "surrogateescape") + b"\0")
    fields = out.decode("utf-8", "surrogateescape").split("\0")
    result = {}
    for i in range(0, len(fields) - 2, 3):
        result[fields[i]] = fields[i + 2] == "set"
    return result


def hidden_chars(text):
    """[(line, code point)] of every invisible character; a leading BOM is allowed."""
    found = []
    for m in INVISIBLE.finditer(text):
        if m.start() == 0 and m.group() == BOM:
            continue
        found.append((text.count("\n", 0, m.start()) + 1, ord(m.group())))
    return found


def check_codemap_config(root, rel):
    """Validate .codemap/config.json parses as JSON and every routing doc path
    resolves to an existing regular file inside root."""
    path = os.path.join(root, *rel.split("/"))
    if os.path.islink(path) or not os.path.isfile(path):
        return []
    try:
        with open(path, "r", encoding="utf-8") as f:
            data = json.load(f)
    except (OSError, UnicodeDecodeError, ValueError) as e:
        return ["%s: invalid JSON (%s)" % (rel, e)]
    if not isinstance(data, dict):
        return ["%s: top-level JSON value must be an object" % rel]
    routing = data.get("routing")
    if routing is None:
        return []
    if not isinstance(routing, dict):
        return ["%s: routing must be an object" % rel]
    subsystems = routing.get("subsystems")
    if subsystems is None:
        return []
    if not isinstance(subsystems, list):
        return ["%s: routing.subsystems must be a list" % rel]
    real_root = os.path.realpath(root)
    findings = []
    for idx, sub in enumerate(subsystems):
        if not isinstance(sub, dict):
            findings.append("%s: routing.subsystems[%d] must be an object" % (rel, idx))
            continue
        docs = sub.get("docs")
        if docs is None:
            continue
        if not isinstance(docs, list):
            findings.append("%s: routing.subsystems[%d].docs must be a list" % (rel, idx))
            continue
        for doc in docs:
            if not isinstance(doc, str) or not doc or doc.startswith(("/", "\\")) or ":" in doc.split("/")[0]:
                findings.append("%s: invalid routing doc path %r" % (rel, doc))
                continue
            parts = doc.replace("\\", "/").split("/")
            if ".." in parts:
                findings.append("%s: routing doc path escapes repository: %s" % (rel, doc))
                continue
            target = os.path.join(root, *parts)
            real_target = os.path.realpath(target)
            try:
                inside = os.path.commonpath([real_root, real_target]) == real_root
            except ValueError:
                inside = False
            if not inside:
                findings.append("%s: routing doc path escapes repository: %s" % (rel, doc))
            elif not os.path.isfile(target):
                findings.append("%s: routing doc does not exist: %s" % (rel, doc))
    return findings


def check(root):
    """(agent paths with their export-ignore state, findings, text files scanned)."""
    tracked, new = repo_files(root)
    if not tracked:
        raise GitError("git tracks no files under " + root)
    files = sorted(tracked | new)
    agent = [p for p in files if is_agent_material(p)]
    ignored = export_ignored(root, agent)
    findings = [("%s: agent material but not export-ignore in .gitattributes" % p) if p in tracked
                else ("%s: untracked agent file: export-ignore it in .gitattributes,"
                      " or .gitignore it if it is local" % p)
                for p in agent if not ignored.get(p)]
    for p in agent:
        lower = p.lower()
        if lower.startswith(".codemap/skills/"):
            findings.append("%s: agent skills belong under .agents/, not .codemap/skills/" % p)
        elif lower == ".codemap/config.json":
            findings.extend(check_codemap_config(root, p))
    scanned = 0
    for rel in files:
        path = os.path.join(root, rel)
        if os.path.islink(path) or not os.path.isfile(path):
            continue  # symlink target is not repository text; or tracked file deleted
        with open(path, "rb") as f:
            data = f.read()
        if b"\0" in data[:8192]:
            continue  # binary
        scanned += 1
        hits = hidden_chars(data.decode("utf-8", "replace"))
        if hits:
            points = sorted(set(cp for _, cp in hits))
            findings.append("%s:%d: %d invisible character%s (%s)" % (
                rel, hits[0][0], len(hits), "" if len(hits) == 1 else "s",
                ", ".join("U+%04X" % cp for cp in points)))
    return [(p, bool(ignored.get(p))) for p in agent], findings, scanned


def _remove_tree(path):
    """Delete a directory tree even where git left read-only object files in it (Windows)."""
    def clear_and_retry(function, target, _error):
        os.chmod(target, stat.S_IWRITE)
        function(target)
    if sys.version_info >= (3, 12):
        shutil.rmtree(path, onexc=clear_and_retry)
    else:
        shutil.rmtree(path, onerror=clear_and_retry)


def self_test():
    def expect(cond, what):
        if not cond:
            print("self-test FAILED: " + what)
            sys.exit(1)

    for rel in ("AGENTS.md", "CLAUDE.md", "docs/AGENTS.md", ".agents/handoff.md",
                ".codemap/config.json",
                "handoff-prompt-l2-draw-suppression.md",
                ".Codex/worktrees/x/a.cpp", ".claude/settings.json",
                ".github/copilot-instructions.md", ".aider.conf.yml",
                ".github/prompts/fix.prompt.md"):
        expect(is_agent_material(rel), "agent material: " + rel)
    for rel in ("docs/fixes.md", "README.md", "src/agents.cpp", "tools/agent_log.py",
                "docs/agents/notes.md", ".github/FUNDING.yml"):
        expect(not is_agent_material(rel), "not agent material: " + rel)

    expect(hidden_chars("plain\ntext\n") == [], "clean text")
    expect(hidden_chars(BOM + "line\n") == [], "a leading BOM is allowed")
    expect(hidden_chars("a\nb" + BOM + "\n") == [(2, 0xFEFF)], "a BOM past the start")
    expect(hidden_chars("a\u200bb") == [(1, 0x200B)], "zero-width space")
    expect(hidden_chars("x\r\ny\r\n\u202ez") == [(3, 0x202E)], "bidi override, CRLF lines")
    expect(hidden_chars("\U000e0041\U000e0042") == [(1, 0xE0041), (1, 0xE0042)], "tag characters")
    expect(hidden_chars("\u200e\u2066") == [(1, 0x200E), (1, 0x2066)], "LRM and an isolate")
    expect(hidden_chars("\u26a0\ufe0f caution") == [], "an emoji variation selector is not hidden text")
    with open(os.path.abspath(__file__), "rb") as f:
        expect(hidden_chars(f.read().decode("utf-8")) == [], "this tool's own source is clean")

    temporary = tempfile.mkdtemp(prefix="edvr-agent-material-")
    try:
        root = os.path.join(temporary, "repo")

        def write(rel, data):
            path = os.path.join(root, *rel.split("/"))
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "wb") as f:
                f.write(data if isinstance(data, bytes) else data.encode("utf-8"))

        os.makedirs(root)
        git(root, "init", "-q")
        write(".gitignore", ".claude/\n")
        write(".gitattributes", "AGENTS.md export-ignore\n")
        write("AGENTS.md", "notes\n")
        write(".agents/handoff.md", "next\n")
        write(".codemap/config.json", "{}\n")
        write(".claude/settings.local.json", "{}\n")      # ignored: not listed
        write("docs/design.md", BOM + "# Design\n")     # leading BOM: allowed
        write("src/a.cpp", "int a;\n\nint b; // \U000e0041\n")
        write("tools/trace.bin", b"\0\0\x01" + "\u200b".encode("utf-8"))  # binary: skipped
        git(root, "add", ".gitignore", ".gitattributes", "AGENTS.md", "docs/design.md",
            "src/a.cpp", "tools/trace.bin")
        write("CLAUDE.md", "@AGENTS.md\n")               # new, not yet added: still checked
        # An agent tool's local state, not ignored by the repository: never listed.
        write(".Codex/worktrees/w/notes.md", "x" + chr(0x200B) + "\n")
        os.makedirs(os.path.join(root, ".agents", "nested"))
        git(os.path.join(root, ".agents", "nested"), "init", "-q")  # a nested repository: ".agents/nested/"

        agent, findings, scanned = check(root)
        expect([p for p, _ in agent] == [".agents/handoff.md", ".codemap/config.json", "AGENTS.md", "CLAUDE.md"],
               "agent material listed, ignored paths left out: %r" % agent)
        expect(dict(agent) == {".agents/handoff.md": False, ".codemap/config.json": False,
                               "AGENTS.md": True, "CLAUDE.md": False},
               "export-ignore state read from .gitattributes: %r" % agent)
        expect(any(f.startswith(".agents/handoff.md: ") for f in findings), "missing export-ignore reported")
        expect(any(f.startswith(".codemap/config.json: ") for f in findings), "missing .codemap export-ignore reported")
        expect(any(f.startswith("CLAUDE.md: untracked agent file") for f in findings),
               "a new agent file is reported before git add")
        expect(any(f.startswith("src/a.cpp:3: 1 invisible character (U+E0041)") for f in findings),
               "tag character reported with its line: %r" % findings)
        expect(not any(f.startswith(("docs/", "tools/", "AGENTS.md", ".Codex/", ".agents/nested")) for f in findings),
               "no finding for a leading BOM, a binary file, an export-ignored agent file,"
               " a tool's local folder or a nested repository: %r" % findings)
        expect(len(findings) == 4, "exactly four findings: %r" % findings)
        expect(work_tree(root), "the repository root is a work tree")
        expect(not work_tree(os.path.join(root, "docs")), "a folder inside it is not its top")
        plain = os.path.join(temporary, "plain")
        os.makedirs(plain)
        expect(not work_tree(plain), "a tree without .git (a source zip) is skipped")

        write(".gitattributes", "AGENTS.md export-ignore\nCLAUDE.md export-ignore\n.agents/** export-ignore\n.codemap/** export-ignore\n")
        write("src/a.cpp", "int a;\n")
        write(".codemap/config.json", '{"routing":{"subsystems":[{"id":"d","docs":["docs/design.md"]}]}}\n')
        agent, findings, scanned = check(root)
        expect(findings == [], "clean once fixed: %r" % findings)
        expect(all(state for _, state in agent) and len(agent) == 4, "all four export-ignore")
        expect(scanned == 8, "eight text files scanned, the binary skipped: %d" % scanned)

        # .codemap/skills/ is rejected even when export-ignore is set.
        write(".codemap/skills/local.md", "# skill\n")
        expect(any(f.startswith(".codemap/skills/local.md: agent skills belong under .agents/")
                   for f in check(root)[1]),
               "skills under .codemap/skills/ rejected")
        os.remove(os.path.join(root, ".codemap", "skills", "local.md"))

        # .codemap/config.json routing docs must exist inside the repository, and JSON must parse.
        write(".codemap/config.json", '{"routing":{"subsystems":[{"id":"d","docs":["docs/missing.md","../escape.md"]}]}}\n')
        codemap_findings = check(root)[1]
        expect(any("docs/missing.md" in f for f in codemap_findings),
               "missing codemap routing doc reported: %r" % codemap_findings)
        expect(any("../escape.md" in f for f in codemap_findings),
               "escaping codemap routing doc reported: %r" % codemap_findings)
        write(".codemap/config.json", "{not json\n")
        expect(any(f.startswith(".codemap/config.json: invalid JSON") for f in check(root)[1]),
               "malformed codemap config reported")
        write(".codemap/config.json", '{"routing":{"subsystems":[{"id":"d","docs":["docs/design.md"]}]}}\n')
        expect(check(root)[1] == [], "valid codemap config clean again")

        # A tracked symlink stores a target path, not its target's text. Mock
        # the file type so this assertion works on Windows without symlink
        # creation privileges; first prove the target text would otherwise fail.
        write("link.txt", "outside\u200btext\n")
        git(root, "add", "link.txt")
        expect(any(f.startswith("link.txt:") for f in check(root)[1]),
               "symlink fixture has an otherwise-detected invisible character")
        link = os.path.join(root, "link.txt")
        real_islink = os.path.islink
        with mock.patch("os.path.islink", side_effect=lambda p: p == link or real_islink(p)):
            expect(check(root)[1] == [], "tracked symlink target is not scanned")
    finally:
        _remove_tree(temporary)
    print("check_agent_material self-test OK")


def main(argv):
    unknown = [a for a in argv if a not in ("--self-test", "--list")]
    if unknown:
        print("usage: check_agent_material.py [--list | --self-test]")
        return 2
    if "--self-test" in argv:
        self_test()
        return 0
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    if not work_tree(root):
        print("check_agent_material: %s is not a git work tree (a source archive?): skipped" % root)
        return 0
    try:
        agent, findings, scanned = check(root)
    except GitError as e:
        print("check_agent_material: %s" % e)
        return 2
    if "--list" in argv:
        for path, ignored in agent:
            print("%s%s" % (path, "" if ignored else "  NOT export-ignore"))
    for finding in findings:
        print(finding)
    if findings:
        print("%d agent-material finding%s" % (len(findings), "" if len(findings) == 1 else "s"))
        return 1
    print("agent material: %d path%s, all export-ignore; %d text files free of invisible characters"
          % (len(agent), "" if len(agent) == 1 else "s", scanned))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
