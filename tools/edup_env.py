#!/usr/bin/env python3
"""Environment variable names for ED-UP: EDUP_ is the name, EDVR_ is still read.

Every variable an ED-UP tool or build reads from the process environment is
spelled EDUP_<suffix>. Before the rename they were EDVR_<suffix>, and a user's
shell or script may still set those. get() reads the EDUP_ name first; when it
is unset and the EDVR_ name is set, it returns the EDVR_ value and says once,
on stderr, that the old name is deprecated:

    [edup] WARNING: EDVR_SIGN is deprecated; set EDUP_SIGN instead

Only user-set inputs fall back. Variables that our own code sets for a child
process (EDUP_RIG_STEP, EDUP_VER, ...) are renamed on both sides and have no
fallback.

    from edup_env import get
    sign = get("EDUP_SIGN")                       # os.environ
    sign = get("EDUP_SIGN", environ=env_dict)     # any mapping

    python tools\\edup_env.py --self-test

A value read from os.environ under its old name is also set under the new one
in this process, so the Python children it starts do not warn again.

This module writes nothing to disk: no files, no directories.
"""
import argparse
import io
import os
import sys
import tempfile

NEW_PREFIX = "EDUP_"
LEGACY_PREFIX = "EDVR_"
WARNING_PREFIX = "[edup] WARNING: "

_warned = set()


def legacy_name(name):
    """The EDVR_ spelling of an EDUP_ variable name."""
    if not name.startswith(NEW_PREFIX):
        raise ValueError("not an EDUP_ variable name: %r" % (name,))
    return LEGACY_PREFIX + name[len(NEW_PREFIX):]


def deprecation_message(name):
    return "%s is deprecated; set %s instead" % (legacy_name(name), name)


def reset_warnings():
    """Forget which names have warned (the self-test starts each case fresh)."""
    _warned.clear()


def get(name, default=None, environ=None, stream=None):
    """The value of the EDUP_ variable `name`, else the EDVR_ one (warning once), else `default`.

    An empty value counts as unset, as it always did for these variables.
    `environ` is any mapping; None reads os.environ. `stream` receives the
    warning; None writes to sys.stderr."""
    env = os.environ if environ is None else environ
    value = env.get(name)
    if value:
        return value
    value = env.get(legacy_name(name))
    if value:
        if name not in _warned:
            _warned.add(name)
            print(WARNING_PREFIX + deprecation_message(name), file=sys.stderr if stream is None else stream)
        if env is os.environ:
            os.environ[name] = value        # a child reads the new name and stays quiet
        return value
    return default


def self_test():
    failures = []

    def check(condition, why):
        if not condition:
            failures.append(why)

    reset_warnings()
    check(legacy_name("EDUP_SIGN") == "EDVR_SIGN", "legacy_name maps EDUP_SIGN to EDVR_SIGN")
    check(legacy_name("EDUP_RIG_TIMEOUT_SCALE") == "EDVR_RIG_TIMEOUT_SCALE", "legacy_name keeps the whole suffix")
    try:
        legacy_name("EDVR_SIGN")
        check(False, "legacy_name refuses a name without the EDUP_ prefix")
    except ValueError:
        pass

    # The new name wins when both are set, and nothing is said.
    quiet = io.StringIO()
    both = {"EDUP_SIGN": "new", "EDVR_SIGN": "old"}
    check(get("EDUP_SIGN", environ=both, stream=quiet) == "new", "the EDUP_ name wins when both are set")
    check(quiet.getvalue() == "", "no warning when the new name is set")

    # The legacy name alone is read, and warns exactly once.
    warn = io.StringIO()
    only_old = {"EDVR_SIGN": "old"}
    check(get("EDUP_SIGN", environ=only_old, stream=warn) == "old", "the EDVR_ value is returned when it is the only one")
    check(get("EDUP_SIGN", environ=only_old, stream=warn) == "old", "the EDVR_ value is still returned on a second read")
    said = warn.getvalue()
    check(said == "[edup] WARNING: EDVR_SIGN is deprecated; set EDUP_SIGN instead\n",
          "the deprecation line is exact and printed once, got %r" % (said,))

    # Neither set gives the default; an empty value is unset.
    check(get("EDUP_SIGN", "dflt", environ={}) == "dflt", "neither set gives the default")
    check(get("EDUP_SIGN", environ={}) is None, "neither set and no default gives None")
    check(get("EDUP_SIGN", "dflt", environ={"EDUP_SIGN": "", "EDVR_SIGN": ""}) == "dflt",
          "empty values count as unset")
    check(get("EDUP_SIGN", "dflt", environ={"EDUP_SIGN": "", "EDVR_SIGN": "old"}, stream=io.StringIO()) == "old",
          "an empty EDUP_ value falls back to a set EDVR_ value")

    # Warn-once is per name.
    reset_warnings()
    other = io.StringIO()
    get("EDUP_NO_COMPILE_CACHE", environ={"EDVR_NO_COMPILE_CACHE": "1"}, stream=other)
    get("EDUP_SIGN", environ={"EDVR_SIGN": "1"}, stream=other)
    check(other.getvalue().count(WARNING_PREFIX) == 2, "each name warns once")

    # Reads os.environ when no mapping is given.
    probe_new, probe_old = "EDUP_EDVR_ENV_SELF_TEST", "EDVR_EDVR_ENV_SELF_TEST"
    saved = {k: os.environ.get(k) for k in (probe_new, probe_old)}
    try:
        os.environ.pop(probe_new, None)
        os.environ[probe_old] = "from-os"
        reset_warnings()
        check(get(probe_new, stream=io.StringIO()) == "from-os", "with no mapping, os.environ is read")
        check(os.environ.get(probe_new) == "from-os", "a value from os.environ is handed on under the new name")
        os.environ[probe_new] = "new-os"
        check(get(probe_new, stream=io.StringIO()) == "new-os", "with no mapping, the EDUP_ name wins")
    finally:
        for k, v in saved.items():
            if v is None:
                os.environ.pop(k, None)
            else:
                os.environ[k] = v
    reset_warnings()

    # The helper writes nothing: a folder listing is the same before and after.
    with tempfile.TemporaryDirectory(prefix="edup_env_") as scratch:
        before = sorted(os.listdir(scratch))
        get("EDUP_SIGN", environ={"EDVR_SIGN": "1"}, stream=io.StringIO())
        reset_warnings()
        check(sorted(os.listdir(scratch)) == before, "the helper writes no files")

    if failures:
        for f in failures:
            print("FAIL: " + f)
        return 1
    print("edup_env self-test: all checks passed")
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description="EDUP_ environment names with the EDVR_ fallback.")
    parser.add_argument("--self-test", action="store_true", help="run the built-in checks")
    args = parser.parse_args(argv)
    if args.self_test:
        return self_test()
    parser.print_help()
    return 2


if __name__ == "__main__":
    sys.exit(main())
