#!/usr/bin/env python3
"""Run the ED-UP launcher (ed-up-launcher.exe) in Elite's existing Steam Wine/Proton prefix."""

import argparse
import configparser
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys


APP_ID = "359320"
PRODUCT = "Products/elite-dangerous-odyssey-64"
MAX_VDF = 4 * 1024 * 1024


def parser():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--launcher", action="store_true",
                   help="accepted and ignored: this wrapper only runs the launcher. ed-up-launcher.sh passes it. "
                        "Arguments after a literal -- go to the launcher")
    p.add_argument("--launcher-exe", type=Path,
                   help="ed-up-launcher.exe (beside script or build/ by default)")
    p.add_argument("--steam-dir", type=Path, help="Steam root; otherwise use STEAM_DIR or known locations")
    p.add_argument("--store", choices=("auto", "steam", "epic"), default="auto",
                   help="select the store; auto refuses when both stores have matching installs. "
                        "epic is recognised and refused: the launcher is not supported for Epic installs here")
    p.add_argument("--proton", type=Path,
                   help="host Proton directory or script; overrides the prefix record and bypasses Flatpak")
    p.add_argument("--prefix", type=Path,
                   help="Elite's Proton compatdata root (containing pfx/)")
    p.add_argument("--game-dir", type=Path, help="folder holding EliteDangerous64.exe inside the installed game")
    offline = p.add_mutually_exclusive_group()
    offline.add_argument("--dry-run", action="store_true",
                         help="resolve and print the exact command; start no processes and write nothing")
    offline.add_argument("--self-test", action="store_true", help=argparse.SUPPRESS)
    return p


def parse_argv(argv=None):
    """Parse argv, splitting it at the first literal -- before argparse sees it.

    args.passthrough is None when there was no --, else the words after it."""
    words = list(sys.argv[1:] if argv is None else argv)
    passthrough = None
    if "--" in words:
        cut = words.index("--")
        words, passthrough = words[:cut], words[cut + 1:]
    args = parser().parse_args(words)
    args.passthrough = passthrough
    return args


def launcher_args(args):
    """The words after -- that go to the launcher, which may not override the --dir or --store this
    wrapper selects."""
    forwarded = list(args.passthrough or [])
    if any(word.split("=", 1)[0].lower() in ("--dir", "--store") for word in forwarded):
        raise ValueError("launcher arguments cannot override the selected --dir or --store")
    return forwarded


def launcher_path(args, script):
    if args.launcher_exe is not None:
        return args.launcher_exe.expanduser().resolve()
    adjacent = script.resolve().parent / "ed-up-launcher.exe"
    return adjacent if adjacent.is_file() else script.resolve().parents[1] / "build" / "ed-up-launcher.exe"


def vdf(path):
    """Read the small KeyValues subset in Steam's local manifests, without regex scoping."""
    if not path.is_file() or path.stat().st_size > MAX_VDF:
        raise ValueError("missing or oversized Steam manifest: %s" % path)
    data = path.read_text(encoding="utf-8-sig", errors="replace")
    tokens = []
    i = 0
    while i < len(data):
        if data[i].isspace():
            i += 1
        elif data.startswith("//", i):
            end = data.find("\n", i)
            i = len(data) if end < 0 else end + 1
        elif data[i] in "{}":
            tokens.append(data[i]); i += 1
        elif data[i] == '"':
            i += 1; value = []
            while i < len(data) and data[i] != '"':
                if data[i] == "\\" and i + 1 < len(data) and data[i + 1] in ('"', "\\"):
                    i += 1
                value.append(data[i]); i += 1
            if i == len(data):
                raise ValueError("unterminated quote in %s" % path)
            tokens.append("".join(value)); i += 1
        else:
            raise ValueError("unexpected VDF character in %s" % path)
    at = 0

    def section(nested=False):
        nonlocal at
        result = {}
        while at < len(tokens):
            if tokens[at] == "}":
                if not nested:
                    raise ValueError("unexpected VDF close in %s" % path)
                at += 1
                return result
            key = tokens[at]; at += 1
            if key == "{" or at == len(tokens):
                raise ValueError("missing VDF value in %s" % path)
            value = tokens[at]; at += 1
            if value == "{":
                value = section(True)
            elif value == "}":
                raise ValueError("unexpected VDF close in %s" % path)
            if key in result:
                raise ValueError("duplicate VDF key %s in %s" % (key, path))
            result[key] = value
        if nested:
            raise ValueError("unclosed VDF section in %s" % path)
        return result

    parsed = section()
    if at != len(tokens):
        raise ValueError("trailing VDF content in %s" % path)
    return parsed


def section(mapping, key, path):
    value = mapping.get(key)
    if not isinstance(value, dict):
        raise ValueError("%s has no %s section" % (path, key))
    return value


def safe_leaf(value, label):
    if not isinstance(value, str) or value in ("", ".", "..") or re.search(r"[/\\\x00]", value):
        raise ValueError("unsafe %s in Steam manifest: %r" % (label, value))
    return value


def roots(args, home, environ):
    explicit = args.steam_dir or (Path(environ["STEAM_DIR"]) if environ.get("STEAM_DIR") else None)
    if explicit is not None:
        candidates = [explicit]
    else:
        candidates = [home / ".steam/steam", home / ".steam/root", home / ".local/share/Steam"]
        if environ.get("XDG_DATA_HOME"):
            candidates.append(Path(environ["XDG_DATA_HOME"]) / "Steam")
        candidates.append(home / ".var/app/com.valvesoftware.Steam/.local/share/Steam")
    found = []
    for candidate in candidates:
        path = candidate.expanduser().resolve()
        if path not in found and (path / "steamapps/libraryfolders.vdf").is_file():
            found.append(path)
    if not found:
        raise ValueError("Steam root not found; pass --steam-dir PATH (launch Steam once first)")
    return found


def libraries(steam_root):
    folders = section(vdf(steam_root / "steamapps/libraryfolders.vdf"), "libraryfolders", "libraryfolders.vdf")
    result = []
    for key, entry in folders.items():
        if not key.isdigit() or not isinstance(entry, dict):
            continue
        raw = entry.get("path")
        if not isinstance(raw, str) or not raw:
            continue
        library = Path(raw).expanduser().resolve()
        if library not in result and (library / "steamapps").is_dir():
            result.append(library)
    if steam_root not in result and (steam_root / "steamapps").is_dir():
        result.insert(0, steam_root)
    return result


def inside(path, parent):
    try:
        path.relative_to(parent)
        return True
    except ValueError:
        return False


class SteamNoInstall(ValueError):
    """The inspected Steam inventory has no matching valid game."""


class SteamAmbiguousInstalls(ValueError):
    """Steam owns matching games but needs an explicit selection."""


def find_game(args, steam_roots):
    requested = args.game_dir.expanduser().resolve() if args.game_dir else None
    matches = []
    rejected = []
    for steam_root in steam_roots:
        for library in libraries(steam_root):
            manifest = library / "steamapps" / ("appmanifest_%s.acf" % APP_ID)
            if not manifest.is_file():
                continue
            state = section(vdf(manifest), "AppState", manifest)
            if state.get("appid") != APP_ID:
                raise ValueError("wrong appid in %s" % manifest)
            install_root = (library / "steamapps/common" / safe_leaf(state.get("installdir"), "installdir")).resolve()
            if not inside(install_root, (library / "steamapps/common").resolve()):
                raise ValueError("game install escapes Steam library: %s" % install_root)
            game = requested or (install_root / PRODUCT).resolve()
            if requested and not inside(requested, install_root):
                continue
            if not inside(game, install_root):
                rejected.append("game folder escapes Steam install: %s" % game)
                continue
            if not (game / "EliteDangerous64.exe").is_file():
                rejected.append("EliteDangerous64.exe missing in %s" % game)
                continue
            match = (steam_root, library, install_root, game)
            if match not in matches:
                matches.append(match)
    if not matches:
        detail = ("; " + "; ".join(rejected[:3])) if rejected else ""
        raise SteamNoInstall("Elite Steam app %s has no valid install%s; check --steam-dir and --game-dir" %
                         (APP_ID, detail))
    if len(matches) > 1:
        raise SteamAmbiguousInstalls("multiple Elite installs found; pass --steam-dir and --game-dir to select one")
    return matches[0]


def compatdata_path(library, environ, explicit=None):
    identities = [environ[key] for key in ("SteamAppId", "SteamGameId", "STEAM_COMPAT_APP_ID")
                  if environ.get(key)]
    # A non-Steam shortcut carries its own valid prefix. Only Elite's
    # unambiguous Steam identity can authorize an inherited custom location.
    raw = environ.get("STEAM_COMPAT_DATA_PATH") if identities and all(
        value == APP_ID for value in identities) else None
    path = (explicit or (Path(raw) if raw else library / "steamapps/compatdata" / APP_ID)).expanduser().resolve()
    if not (path / "pfx/drive_c").is_dir():
        if explicit:
            raise ValueError("--prefix needs Elite's existing Proton compatdata root containing pfx/drive_c: %s" % path)
        raise ValueError("Elite Proton prefix missing at %s; launch Elite once through Steam" % path)
    return path


def recorded_proton(compatdata, host_file=None):
    record = compatdata / "config_info"
    if not record.is_file() or record.stat().st_size > 1024 * 1024:
        raise ValueError("Proton record missing in %s; launch Elite once or pass --proton PATH" % compatdata)
    lines = record.read_text(encoding="utf-8", errors="replace").splitlines()
    if len(lines) < 8:
        raise ValueError("incomplete Proton record in %s; launch Elite once or pass --proton PATH" % record)

    def base_of(value, suffixes):
        cleaned = value.rstrip("/")
        for suffix in suffixes:
            if cleaned.startswith("/") and cleaned.endswith(suffix):
                base = Path(cleaned[:-len(suffix)])
                return base if host_file else base.resolve()
        return None

    defaults = tuple("/%s/share/%s" % (tree, name)
                     for tree in ("files", "dist")
                     for name in ("default_pfx", "default_pfx_arm64"))
    # Proton 9 and older add a files/lib64 line, which moves default_pfx off line 8.
    base = None
    for line in lines:
        base = base_of(line, defaults)
        if base is not None:
            break
    resolve_file = host_file or (lambda path: path)
    if base is None or not resolve_file(base / "proton").is_file():
        raise ValueError("recorded Proton is missing or stale in %s; launch Elite once or pass --proton PATH" % record)
    fonts = base_of(lines[1], ("/files/share/fonts", "/dist/share/fonts"))
    library = base_of(lines[2], ("/files/lib", "/dist/lib"))
    if fonts != base or library != base:
        raise ValueError("inconsistent Proton paths in %s; launch Elite once or pass --proton PATH" % record)
    return base


def proton_path(args, compatdata, host_file=None):
    if args.proton is None:
        return recorded_proton(compatdata, host_file) / "proton"
    chosen = args.proton.expanduser().resolve()
    script = chosen / "proton" if chosen.is_dir() else chosen
    if script.name != "proton" or not script.is_file():
        raise ValueError("--proton needs a Proton directory or its proton script: %s" % chosen)
    return script


def runtime_path(proton, steam_roots, host_file=None):
    manifest = (host_file or (lambda path: path))(proton.parent / "toolmanifest.vdf")
    if not manifest.is_file():
        raise ValueError("Proton toolmanifest.vdf missing beside %s; reinstall it through Steam or pass --proton PATH" % proton)
    tool = section(vdf(manifest), "manifest", manifest)
    appid = tool.get("require_tool_appid")
    if appid is None:
        return None
    if not isinstance(appid, str) or not appid.isdigit():
        raise ValueError("invalid require_tool_appid in %s" % manifest)
    found = []
    for steam_root in steam_roots:
        for library in libraries(steam_root):
            app = library / "steamapps" / ("appmanifest_%s.acf" % appid)
            if not app.is_file():
                continue
            state = section(vdf(app), "AppState", app)
            if state.get("appid") != appid:
                raise ValueError("wrong appid in %s" % app)
            common = (library / "steamapps/common").resolve()
            runtime_root = (common / safe_leaf(state.get("installdir"), "runtime installdir")).resolve()
            if not inside(runtime_root, common):
                raise ValueError("runtime install escapes Steam library: %s" % runtime_root)
            candidate = (runtime_root / "_v2-entry-point").resolve()
            if not inside(candidate, runtime_root):
                raise ValueError("runtime entry escapes Steam install: %s" % candidate)
            if candidate.is_file() and candidate not in found:
                found.append(candidate)
    if len(found) != 1:
        raise ValueError("Proton requires Steam runtime app %s, but found %d; install it through Steam" %
                         (appid, len(found)))
    return found[0]


def prefix_windows_path(host_path, prefix):
    """Translate through the selected prefix's existing drive mappings only."""
    dos = prefix / "dosdevices"
    found = []
    if not dos.is_dir():
        raise ValueError("Wine prefix has no dosdevices: %s" % dos)
    for link in dos.iterdir():
        if not re.fullmatch(r"[a-zA-Z]:", link.name) or not link.is_symlink():
            continue
        target = link.resolve()
        if target.is_dir() and inside(host_path, target):
            found.append((len(target.parts), link.name[0].upper(), host_path.relative_to(target)))
    if not found:
        raise ValueError("no existing Wine drive maps %s in %s" % (host_path, dos))
    _, drive, relative = max(found, key=lambda item: (item[0], item[1]))
    tail = "\\".join(relative.parts)
    return drive + ":\\" + tail


def windows_path(host_path, compatdata):
    return prefix_windows_path(host_path, compatdata / "pfx")


def optional_steam_roots(args, home, environ, *, require_explicit=False):
    try:
        return roots(args, home, environ)
    except ValueError:
        if require_explicit and (args.steam_dir is not None or environ.get("STEAM_DIR")):
            raise
        return []


def epic_discovery():
    # Importing a sibling module must not create __pycache__ during --dry-run.
    previous = sys.dont_write_bytecode
    try:
        sys.dont_write_bytecode = True
        import epic_library
        return epic_library
    finally:
        sys.dont_write_bytecode = previous


def flatpak_installations(home, environ):
    data = Path(environ.get("XDG_DATA_HOME", str(home / ".local/share")))
    installations = (Path(environ.get("FLATPAK_USER_DIR") or data / "flatpak"),
                     Path(environ.get("FLATPAK_SYSTEM_DIR") or "/var/lib/flatpak"))
    if any(not path.is_absolute() for path in installations):
        raise ValueError("Flatpak installation directories must be absolute; pass a host runner override")
    return installations


def require_default_flatpak_installations(environ):
    """Refuse uninspected system roots before validating a Steam sandbox runner."""
    # Flatpak 1.16.1 reads regular *.conf files here, including symlinks,
    # and extension lookup searches every configured system installation.
    # github.com/flatpak/flatpak/blob/1.16.1/common/flatpak-dir.c#L1798-L1899
    config_root = Path(environ.get("FLATPAK_CONFIG_DIR", "/etc/flatpak"))
    recovery = "; pass --proton with an installed host runner"
    if not config_root.is_absolute():
        raise ValueError("Flatpak installation configuration root must be absolute" + recovery)
    # GIO normalizes '..' lexically before following links, unlike resolve().
    if ".." in config_root.parts:
        raise ValueError("Flatpak installation configuration root has ambiguous parent navigation" + recovery)
    try:
        # An absent optional config tree means no extra installations. Walk
        # missing components to distinguish that from a dangling symlink or
        # an inaccessible ancestor, which cannot establish the search roots.
        existing = config_root
        while True:
            try:
                existing.stat()
                break
            except FileNotFoundError:
                if existing.is_symlink():
                    raise ValueError("Flatpak installation configuration has a dangling reference: %s%s" %
                                     (existing, recovery))
                existing = existing.parent
        if existing != config_root:
            return
        config_root = config_root.resolve(strict=True)
        with os.scandir(config_root) as entries:
            for entry in entries:
                if entry.name != "installations.d":
                    continue
                if not entry.is_dir():
                    raise ValueError("Flatpak installation configuration directory is invalid: %s%s" % (entry.path, recovery))
                with os.scandir(entry.path) as configs:
                    for config in configs:
                        if config.name.endswith(".conf") and config.is_file():
                            raise ValueError("Flatpak installation configuration includes unsupported system roots: %s%s" %
                                             (config.path, recovery))
    except (OSError, RuntimeError) as exc:
        raise ValueError("Flatpak installation configuration is invalid or unreadable: %s%s" % (config_root, recovery)) from exc


def flatpak_deployment(app_id, home, environ, strict=False):
    found = []
    for installation in flatpak_installations(home, environ):
        application = installation / "app" / app_id
        current = application / "current/active"
        deployments = ([current] if strict and (current / "files").is_dir() else
                       [current] + sorted(application.glob("*/*/active")))
        for deployment in deployments:
            if (deployment / "files").is_dir():
                if not strict:
                    return installation, deployment
                resolved = deployment.resolve()
                if not inside(resolved, application.resolve()):
                    raise ValueError("Flatpak application deployment escapes its installation: %s" % deployment)
                candidate = (installation, resolved)
                if candidate not in found:
                    found.append(candidate)
    if strict and len(found) == 1:
        return found[0]
    if found:
        raise ValueError("ambiguous installed Flatpak %s deployments; select an installed host --proton" % app_id)
    raise ValueError("installed Flatpak %s deployment is missing; repair the manager or pass a host runner override" % app_id)


def flatpak_metadata(deployment):
    return flatpak_keyfile(deployment / "metadata")


def flatpak_keyfile(path):
    # GKeyFile has no inherited DEFAULT section, interpolation, or colon
    # delimiter. A missing optional file is different from an unreadable one.
    config = configparser.ConfigParser(interpolation=None, delimiters=("=",),
                                       comment_prefixes=("#",), default_section="\x00", strict=False)
    config.optionxform = str
    try:
        with path.open(encoding="utf-8", newline="") as stream:
            contents = stream.read(1024 * 1024 + 1)
        if len(contents) > 1024 * 1024:
            raise ValueError("Flatpak metadata or override file is too large: %s; pass a host runner override" % path)
        if "\x00" in contents:
            raise ValueError("unsupported Flatpak NUL character; pass a host runner override")
        ascii_space = " \t\r\f"
        lines = [line[:-1] if line.endswith("\r") else line for line in contents.split("\n")]
        for line in lines:
            left = line.lstrip(ascii_space)
            if left != left.lstrip():
                raise ValueError("unsupported Flatpak whitespace; pass a host runner override")
            if "=" in left and not left.startswith(("#", "[")):
                key, value = left.split("=", 1)
                if key.rstrip(ascii_space) != key.rstrip() or value.lstrip(ascii_space) != value.lstrip():
                    raise ValueError("unsupported Flatpak whitespace; pass a host runner override")
            match = re.match(r"\s*(?:PATH|HOST_XDG_DATA_HOME|runtime)\s*=(.*)$", line)
            if match and match[1] != match[1].rstrip():
                raise ValueError("unsupported Flatpak trailing whitespace; pass a host runner override")
        # GKeyFile ignores indentation on each physical line. Python's INI
        # parser instead treats an indented key as the preceding value's
        # continuation, so normalize indentation before parsing the keyfile.
        config.read_string("\n".join(line.lstrip(ascii_space) for line in lines))
    except FileNotFoundError:
        pass
    except (OSError, UnicodeError, configparser.Error) as exc:
        raise ValueError("Flatpak metadata or override file is unreadable or invalid: %s; "
                         "pass a host runner override" % path) from exc
    return config


def flatpak_string(value):
    # g_key_file_get_string unescapes these characters; shell expansion is
    # never performed. Refuse unsupported spellings rather than guess a path.
    escapes = {"s": " ", "n": "\n", "t": "\t", "r": "\r", "\\": "\\"}
    if "\n" in value or "\r" in value:
        raise ValueError("unsupported Flatpak multiline environment; pass a host runner override")
    result = []
    index = 0
    while index < len(value):
        char = value[index]
        if char == "\\":
            index += 1
            if index == len(value) or value[index] not in escapes:
                raise ValueError("unsupported Flatpak environment escape; pass a host runner override")
            char = escapes[value[index]]
        result.append(char)
        index += 1
    return "".join(result)


def flatpak_runtime_deployment(config, home, environ):
    runtime = config.get("Application", "runtime", fallback="")
    parts = runtime.split("/")
    if len(parts) != 3 or any(not re.fullmatch(r"[A-Za-z0-9_.-]+", part) for part in parts):
        raise ValueError("installed Flatpak runtime metadata is missing or invalid; pass a host runner override")
    # flatpak run resolves its runtime independently of the app deployment,
    # searching the user installation before the default system installation.
    for installation in flatpak_installations(home, environ):
        deployment = installation / "runtime" / Path(*parts) / "active"
        if (deployment / "files").is_dir():
            if not (deployment / "metadata").is_file():
                raise ValueError("installed Flatpak runtime metadata is missing; pass a host runner override")
            return deployment
    raise ValueError("installed Flatpak runtime metadata is missing; pass a host runner override")


def flatpak_extension_file(path, deployment, home, environ):
    """Resolve an unconditional app extension mount, refusing ambiguous deployments."""
    # Flatpak binds an extension's files directory at its declared mount;
    # subdirectories appends the extension ID's suffix to that directory.
    # github.com/flatpak/flatpak/blob/main/common/flatpak-run.c (flatpak_run_add_extension_args)
    config = flatpak_metadata(deployment)
    runtime = config.get("Application", "runtime", fallback="").split("/")
    if len(runtime) != 3:
        raise ValueError("installed Flatpak runtime metadata is missing or invalid")
    for section_name in config.sections():
        if not section_name.startswith("Extension "):
            continue
        extension_id = section_name[len("Extension "):]
        directory = Path(flatpak_string(config.get(section_name, "directory", fallback="")))
        if directory.is_absolute() or ".." in directory.parts or not directory.parts:
            continue
        mount = Path("/app") / directory
        if not inside(path, mount):
            continue
        if config.has_option(section_name, "enable-if") or config.has_option(section_name, "subdirectory-suffix"):
            raise ValueError("unsupported conditional Flatpak extension mount; pass an installed host --proton")
        relative = path.relative_to(mount)
        full_extension_id = extension_id
        subdirectories = config.get(section_name, "subdirectories", fallback="false")
        if subdirectories == "true" and relative.parts:
            extension_id += "." + relative.parts[0]
            relative = Path(*relative.parts[1:])
        elif subdirectories != "false":
            raise ValueError("unsupported Flatpak extension subdirectories")
        versions = config.get(section_name, "versions", fallback=config.get(
            section_name, "version", fallback="")).split(";")
        versions = list(dict.fromkeys(value for value in versions if value))
        if not versions or any(not re.fullmatch(r"[A-Za-z0-9_.-]+", value)
                               for value in (extension_id, runtime[1], *versions)):
            raise ValueError("invalid Flatpak extension metadata; pass an installed host --proton")
        found = []
        for installation in flatpak_installations(home, environ):
            for version in versions:
                # add_extension() prefers full extensions and also mounts local
                # unmaintained extensions. Refuse either shadow rather than
                # validating a subextension that Flatpak will not execute.
                # github.com/flatpak/flatpak/blob/1.16.1/common/flatpak-dir-utils.c#L433-L457
                full = installation / "runtime" / full_extension_id / runtime[1] / version / "active/files"
                if subdirectories == "true" and full.is_dir():
                    raise ValueError("full Flatpak extension shadows the recorded subextension: %s; "
                                     "pass --proton with an installed host runner" % full_extension_id)
                for identifier in {full_extension_id, extension_id}:
                    unmaintained = installation / "extension" / identifier / runtime[1] / version
                    if unmaintained.exists() or unmaintained.is_symlink():
                        raise ValueError("unmaintained Flatpak extension may shadow the recorded runner: %s; "
                                         "pass --proton with an installed host runner" % unmaintained)
                extension = installation / "runtime" / extension_id / runtime[1] / version / "active"
                if (extension / "files").is_dir():
                    if not (extension / "metadata").is_file():
                        raise ValueError("installed Flatpak extension metadata is missing")
                    candidate = extension.resolve() / "files" / relative
                    if candidate not in found:
                        found.append(candidate)
        if len(found) > 1:
            raise ValueError("ambiguous installed Flatpak extension for %s; pass an installed host --proton" % path)
        if len(found) == 1 and found[0].is_file():
            return found[0]
        raise ValueError("recorded runner file is missing from installed Flatpak extension: %s" % path)
    return None


def flatpak_sandbox_path(path):
    """Translate a recorded Pressure Vessel parent mount to the outer Flatpak namespace."""
    for tree in ("app", "usr"):
        if inside(path, Path("/run/parent") / tree):
            return Path("/") / path.relative_to("/run/parent")
    return path


def flatpak_host_file(path, app_id, home, environ, selected=None):
    """Resolve sandbox /app and /usr paths from installed deployment metadata, without a process."""
    path = Path(path)
    if not path.is_absolute():
        raise ValueError("recorded runner must be an absolute path: %s" % path)
    if selected:
        path = flatpak_sandbox_path(path)
    _, deployment = selected or flatpak_deployment(app_id, home, environ)
    if not inside(path, Path("/app")) and not inside(path, Path("/usr")):
        return path
    if inside(path, Path("/app")):
        candidate = deployment / "files" / path.relative_to("/app")
    else:
        runtime_deployment = flatpak_runtime_deployment(flatpak_metadata(deployment), home, environ)
        candidate = runtime_deployment / "files" / path.relative_to("/usr")
    if selected and inside(path, Path("/app")):
        extension = flatpak_extension_file(path, deployment, home, environ)
        if extension is not None:
            return extension
    if candidate.is_file():
        return candidate
    raise ValueError("recorded runner file is missing from installed Flatpak %s: %s; "
                     "repair the manager or pass --proton with an installed host runner" % (app_id, path))


def flatpak_runtime_environment(app_id, home, environ, selected=None):
    installation, deployment = selected or flatpak_deployment(app_id, home, environ)
    config = flatpak_metadata(deployment)
    user_installation, system_installation = flatpak_installations(home, environ)
    layers = []
    runtime = config.get("Application", "runtime", fallback="")
    if runtime:
        layers.append(flatpak_metadata(flatpak_runtime_deployment(config, home, environ)))
    layers.append(config)
    # flatpak_deploy_get_overrides merges system-global, system-app,
    # user-global, user-app, with system layers only for system installs:
    # github.com/flatpak/flatpak/blob/main/common/flatpak-dir.c
    override_dirs = ([system_installation / "overrides"] if installation != user_installation else [])
    override_dirs.append(user_installation / "overrides")
    for directory in override_dirs:
        layers.extend(flatpak_keyfile(directory / name) for name in ("global", app_id))
    values = {"PATH": "/app/bin:/usr/bin", "HOST_XDG_DATA_HOME": (
        environ.get("XDG_DATA_HOME") or str(home / ".local/share"))}
    for layer in layers:
        for key in values:
            if layer.has_option("Environment", key):
                values[key] = flatpak_string(layer.get("Environment", key))
        # Context.unset-environment wins over Environment in the same file;
        # a later layer can set it again (flatpak_context_load_metadata).
        unset = layer.get("Context", "unset-environment", fallback="")
        if "\\" in unset:
            raise ValueError("unsupported Flatpak unset-environment; pass a host runner override")
        for key in unset.split(";"):
            if key in values:
                values[key] = None
    path = values["PATH"]
    if path is None:
        raise ValueError("Flatpak PATH is unset; pass a host runner override")
    if any(not Path(part).is_absolute() or "\x00" in part for part in path.split(":")):
        raise ValueError("installed Flatpak PATH must contain absolute directories; pass a host runner override")
    host_data = values["HOST_XDG_DATA_HOME"]
    if host_data is None or not Path(host_data).is_absolute() or "\x00" in host_data:
        raise ValueError("Flatpak HOST_XDG_DATA_HOME must be an absolute directory; pass a host runner override")
    # flatpak-run exports host XDG_DATA_HOME as HOST_XDG_DATA_HOME. UMU's
    # container=flatpak branch uses that value, not the sandbox XDG_DATA_HOME:
    # github.com/Open-Wine-Components/umu-launcher/blob/main/umu/umu_consts.py
    return {"PATH": path, "HOST_XDG_DATA_HOME": host_data}


EPIC_REFUSAL = ("Epic is not supported by this wrapper; launch Elite through its original "
                "Heroic, Legendary, Lutris or Epic manager to preserve Epic authentication")


def build_plan(args, *, script=Path(__file__), home=Path.home(), environ=None):
    env_in = os.environ if environ is None else environ
    forwarded = launcher_args(args)
    launcher = launcher_path(args, script)
    if not launcher.is_file():
        raise ValueError("launcher not found: %s (pass --launcher-exe PATH)" % launcher)
    store = args.store
    if store == "auto":
        epic = epic_discovery().matching_installs(args.game_dir, home=home, environ=env_in)
        steam_roots = optional_steam_roots(args, home, env_in, require_explicit=True)
        steam_error = None
        try:
            find_game(args, steam_roots)
            has_steam = True
        except SteamAmbiguousInstalls as exc:
            steam_error = exc
            has_steam = True
        except SteamNoInstall as exc:
            steam_error = exc
            has_steam = False
        if epic and has_steam:
            raise ValueError("multiple Steam/Epic installs found; pass --store steam and --game-dir")
        if epic:
            store = "epic"
        elif has_steam:
            store = "steam"
        elif steam_roots:
            raise steam_error
        else:
            raise ValueError("no valid Elite Steam install found; launch Steam and Elite once, "
                             "or pass --steam-dir PATH")
    if store == "epic":
        raise ValueError(EPIC_REFUSAL)
    steam_roots = roots(args, home, env_in)
    steam_root, library, install_root, game = find_game(args, steam_roots)
    compatdata = compatdata_path(library, env_in, args.prefix)
    flatpak_id = "com.valvesoftware.Steam"
    selected = None
    host_file = None
    sandbox_env = {}
    flatpak_data = home / ".var/app" / flatpak_id
    if not args.proton and (inside(steam_root, flatpak_data.resolve()) or
                            steam_root == (flatpak_data / ".local/share/Steam").resolve()):
        require_default_flatpak_installations(env_in)
        selected = flatpak_deployment(flatpak_id, home, env_in, strict=True)
        sandbox_env = flatpak_runtime_environment(flatpak_id, home, env_in, selected)
        host_file = lambda path: flatpak_host_file(path, flatpak_id, home, env_in, selected)
    proton = proton_path(args, compatdata, host_file)
    runtime = runtime_path(proton, [steam_root], host_file)
    game_windows = windows_path(game, compatdata)
    sandbox_proton = flatpak_sandbox_path(proton) if selected else proton
    payload_proton = sandbox_proton
    if selected and runtime and (inside(sandbox_proton, Path("/app")) or inside(sandbox_proton, Path("/usr"))):
        # Pressure Vessel's Flatpak subsandbox exposes the parent /app and
        # /usr under /run/parent and forwards the payload argv unchanged.
        # gitlab.steamos.cloud/steamrt/steam-runtime-tools/-/blob/e8724b354362b197f56772f5d88e49d70104826e/pressure-vessel/runtime.c#L352-362
        payload_proton = Path("/run/parent") / sandbox_proton.relative_to("/")
    command = ([str(runtime), "--verb=run", "--"] if runtime else [])
    command += [str(payload_proton), "runinprefix", str(launcher), "--store", "steam", "--dir", game_windows] + forwarded
    env = dict(env_in)
    for key in list(env):
        if key.startswith("WINE") or key == "PROTON_VERSION":
            del env[key]
    env.update(SteamAppId=APP_ID, SteamGameId=APP_ID, STEAM_COMPAT_APP_ID=APP_ID,
               STEAM_COMPAT_CLIENT_INSTALL_PATH=str(steam_root), STEAM_COMPAT_DATA_PATH=str(compatdata),
               STEAM_COMPAT_INSTALL_PATH=str(install_root),
               STEAM_COMPAT_LIBRARY_PATHS=":".join(str(lib / "steamapps") for lib in libraries(steam_root)))
    if selected:
        installation, deployment = selected
        config = flatpak_metadata(deployment)
        relative = deployment.relative_to((installation / "app" / flatpak_id).resolve())
        if (len(relative.parts) != 3 or relative.parts[0] == "current" or
                any(not re.fullmatch(r"[A-Za-z0-9_.-]+", part) for part in relative.parts[:2]) or
                config.get("Application", "name", fallback="") != flatpak_id):
            raise ValueError("installed Flatpak Steam app reference is missing or invalid; pass an installed host --proton")
        flatpak = shutil.which("flatpak", path=env_in.get("PATH"))
        if not flatpak:
            raise ValueError("Flatpak executable missing; install the selected Steam Flatpak")
        user_installation, _ = flatpak_installations(home, env_in)
        selector = "--user" if installation == user_installation else "--system"
        permissions = {launcher.resolve(), steam_root, compatdata, *libraries(steam_root)}
        if not inside(sandbox_proton, Path("/app")) and not inside(sandbox_proton, Path("/usr")):
            permissions.add(sandbox_proton.parent)
        env.update(sandbox_env)
        command = [flatpak, "run", selector, "--arch=" + relative.parts[0],
                   "--branch=" + relative.parts[1], "--command=" + command[0]] + [
            "--filesystem=" + str(path) for path in sorted(permissions, key=str)
        ] + ["--unset-env=" + key for key in sorted(env_in.keys() - env.keys())
        ] + ["--env=%s=%s" % (key, env[key]) for key in sorted(env)
             if key in sandbox_env or key in ("SteamAppId", "SteamGameId") or key.startswith("STEAM_COMPAT_")
        ] + [flatpak_id] + command[1:]
        env = dict(env_in)
    return command, env, (game, compatdata, proton, runtime)


def run(args, *, script=Path(__file__), home=Path.home(), environ=None,
        launch=subprocess.run, platform=sys.platform, geteuid=getattr(os, "geteuid", lambda: 1)):
    if platform != "linux" and not args.dry_run:
        raise ValueError("run this wrapper on Linux; use ed-up-launcher.exe on Windows")
    command, env, (game, compatdata, proton, runtime) = build_plan(
        args, script=script, home=home, environ=environ)
    if args.dry_run:
        print("EDVR resolved launch plan (nothing executed or written):")
        print("  program: launcher")
        print("  game: %s" % game)
        print("  prefix: %s" % compatdata)
        print("  runner: %s" % proton)
        print("  runtime: %s" % (runtime or "none"))
        # Flatpak's sandbox environment is printed in argv; its host process
        # retains the invoking environment, which may contain stale identities.
        display_env = {} if command[1:2] == ["run"] else env
        for key in ("SteamAppId", "SteamGameId", "STEAM_COMPAT_APP_ID",
                    "STEAM_COMPAT_CLIENT_INSTALL_PATH", "STEAM_COMPAT_DATA_PATH",
                    "STEAM_COMPAT_INSTALL_PATH", "STEAM_COMPAT_LIBRARY_PATHS"):
            if key in display_env:
                print("  %s=%s" % (key, shlex.quote(display_env[key])))
        print("  command: %s" % shlex.join(command))
        return 0
    if geteuid() == 0:
        raise ValueError("run as the game owner without sudo")
    try:
        return launch(command, env=env, check=False).returncode
    except OSError as exc:
        raise ValueError("could not start the selected Wine/Proton runtime: %s" % exc) from exc


def launcher_self_test(parse, root, script, home, env, launch, calls):
    import io
    from contextlib import redirect_stderr, redirect_stdout

    # Default exe resolution: beside the script, else <repo>/build.
    exe = script.parent / "ed-up-launcher.exe"
    exe.unlink()
    launcher_args_ns = parse("--launcher")
    assert launcher_path(launcher_args_ns, script) == script.resolve().parents[1] / "build" / "ed-up-launcher.exe"
    exe.write_bytes(b"exe")
    assert launcher_path(launcher_args_ns, script) == exe.resolve()
    override = root / "elsewhere" / "my-launcher.exe"
    assert launcher_path(parse("--launcher", "--launcher-exe", str(override)), script) == override.resolve()
    # The launcher is the only program: with or without the accepted --launcher, the same exe is chosen.
    assert launcher_path(parse(), script) == exe.resolve()

    # Passthrough: split at the first --, kept verbatim, later -- stay in it.
    ns = parse("--launcher", "--dry-run", "--", "--direct-dry-run", "--vr", "--", "--channel", "rc")
    assert ns.dry_run and ns.passthrough == ["--direct-dry-run", "--vr", "--", "--channel", "rc"]
    assert parse("--launcher").passthrough is None
    assert launcher_args(parse("--launcher")) == [] and launcher_args(parse()) == []
    # What the wrapper selects, --dir and --store, cannot be overridden from the passthrough.
    for override_words in (("--store=steam",), ("--STORE", "epic"), ("--dir", "C:\\other"), ("--dir=C:\\other",)):
        try:
            launcher_args(parse("--", *override_words))
            raise AssertionError("launcher accepted %s" % override_words[0])
        except ValueError as exc:
            assert "override" in str(exc)

    # Explicit store identity survives custom drive aliases; passthrough stays verbatim.
    plan_args = parse("--launcher", "--dry-run", "--", "--direct-dry-run", "--vr")
    before = set(root.rglob("*"))
    buf = io.StringIO()
    count = len(calls)
    with redirect_stdout(buf):
        assert run(plan_args, script=script, home=home, environ=env, launch=launch,
                   platform="linux", geteuid=lambda: 1000) == 0
    assert len(calls) == count and set(root.rglob("*")) == before, "launcher --dry-run touched something"
    command, process_env, _details = build_plan(plan_args, script=script, home=home, environ=env)
    start = command.index("runinprefix")
    assert command[start:start + 5] == ["runinprefix", str(exe.resolve()), "--store", "steam", "--dir"]
    assert command[start + 6:] == ["--direct-dry-run", "--vr"]
    assert "--install" not in command
    assert "program: launcher" in buf.getvalue() and shlex.join(command) in buf.getvalue()
    assert process_env["SteamAppId"] == APP_ID and "WINEPREFIX" not in process_env
    bare = build_plan(parse("--launcher", "--dry-run"), script=script, home=home, environ=env)[0]
    assert bare[-5:-1] == [str(exe.resolve()), "--store", "steam", "--dir"]
    # A real launcher run uses the same command and returns the launcher's exit code.
    assert run(parse("--launcher", "--", "--channel", "rc"), script=script, home=home, environ=env,
               launch=launch, platform="linux", geteuid=lambda: 1000) == 23
    assert calls[-1][0][-2:] == ["--channel", "rc"] and str(exe.resolve()) in calls[-1][0]
    calls.pop()
    # A missing launcher names --launcher-exe.
    exe.unlink()
    try:
        build_plan(parse("--launcher", "--dry-run"), script=script, home=home, environ=env)
        raise AssertionError("missing launcher accepted")
    except ValueError as exc:
        assert "--launcher-exe" in str(exc)
    exe.write_bytes(b"exe")

    # The installer's options are gone: argparse refuses each (exit 2), so an old command line cannot
    # quietly start the launcher instead.
    for gone in (("--install",), ("--repair",), ("--uninstall",), ("--collect-logs",), ("--preview",),
                 ("--scope", "flat"), ("--replace-settings",), ("--remove-settings",),
                 ("--installer", str(exe)), ("--wine", "/selected/wine")):
        with redirect_stderr(io.StringIO()) as refusal:
            try:
                parse(*gone)
                raise AssertionError("accepted removed option %s" % gone[0])
            except SystemExit as exc:
                assert exc.code == 2 and "unrecognized arguments" in refusal.getvalue(), gone


def epic_self_test():
    """Epic installs are recognised and refused: the launcher is started for Steam only, and an Epic game
    is started through its original manager."""
    import json
    from tempfile import TemporaryDirectory

    if os.name == "nt":
        # Wine drive links and Linux Flatpak deployments require POSIX filesystem semantics.
        return
    parse = lambda *words: parse_argv(list(words))
    with TemporaryDirectory(dir="/dev/shm" if Path("/dev/shm").is_dir() else None) as tmp:
        root = Path(tmp).resolve()
        home = root / "home"
        script = root / "tools/install_proton.py"
        script.parent.mkdir()
        (script.parent / "ed-up-launcher.exe").write_bytes(b"launcher")
        install = home / "Custom Elite's $() folder"
        game = install / PRODUCT
        game.mkdir(parents=True)
        (game / "EliteDangerous64.exe").write_bytes(b"game")
        prefix = home / "Existing Wine prefix"
        (prefix / "drive_c").mkdir(parents=True)
        (prefix / "dosdevices").mkdir()
        (prefix / "dosdevices/g:").symlink_to(install, target_is_directory=True)
        for name in ("system.reg", "user.reg"):
            (prefix / name).write_bytes(b"registry")
        wine = home / "Selected Wine's $() runner/bin/wine"
        wine.parent.mkdir(parents=True)
        wine.write_bytes(b"runner")
        config = home / ".config/heroic"
        installed = config / "legendaryConfig/legendary/installed.json"
        installed.parent.mkdir(parents=True)
        installed.write_text(json.dumps({"ELITE": {"app_name": "ELITE", "title": "Elite Dangerous",
                             "install_path": str(install), "executable": "EDLaunch.exe"}}), encoding="utf-8")
        settings = config / "GamesConfig/ELITE.json"
        settings.parent.mkdir()
        settings.write_text(json.dumps({"ELITE": {"winePrefix": str(prefix), "wineVersion": {
            "bin": str(wine), "name": "selected", "type": "wine"}, "disableUMU": True}}), encoding="utf-8")
        env = {"PATH": "/mock", "HOME": str(home), "EPIC_AUTH_TOKEN": "secret-not-for-output"}

        def snapshot():
            return {str(p): (p.read_bytes() if p.is_file() else None) for p in root.rglob("*")}

        def refuses(words, reason):
            try:
                build_plan(parse(*words), script=script, home=home, environ=env)
                raise AssertionError("accepted an Epic install: %r" % (words,))
            except ValueError as exc:
                assert reason in str(exc), str(exc)
                assert "secret-not-for-output" not in str(exc)

        # Only Epic is installed: found by detection, then refused, with or without --store.
        before = snapshot()
        refuses(("--dry-run",), "original")
        refuses(("--store", "epic"), "original")
        refuses(("--launcher", "--store", "epic", "--game-dir", str(game)), "original")
        refuses(("--game-dir", str(game)), "original")
        assert snapshot() == before, "refusing an Epic install wrote files"

        # A real Steam install beside it needs the player's choice.
        steam = home / ".local/share/Steam"
        steam_game = steam / "steamapps/common/Elite Dangerous" / PRODUCT
        steam_game.mkdir(parents=True)
        (steam_game / "EliteDangerous64.exe").write_bytes(b"game")
        (steam / "steamapps/libraryfolders.vdf").write_text('"libraryfolders" {}', encoding="utf-8")
        (steam / "steamapps/appmanifest_359320.acf").write_text(
            '"AppState" { "appid" "359320" "installdir" "Elite Dangerous" }', encoding="utf-8")
        before = snapshot()
        refuses(("--dry-run",), "multiple Steam/Epic")
        assert snapshot() == before, "refusing an ambiguous install wrote files"


def auto_selection_self_test():
    """Select stores from confirmed ownership, without starting any binary."""
    if os.name == "nt":
        return
    import io
    import json
    from contextlib import redirect_stderr, redirect_stdout
    from tempfile import TemporaryDirectory

    cases = ("steam-hint", "steam-env-hint", "epic-root", "epic-product", "steam-prefix",
             "both-stores", "both-prefix", "ambiguous-steam", "malformed-steam", "epic-overflow-auto")
    failures = []
    for case in cases:
        with TemporaryDirectory() as tmp:
            root = Path(tmp).resolve()
            home = root / "home"
            script = root / "tools/install_proton.py"
            script.parent.mkdir()
            (script.parent / "ed-up-launcher.exe").write_bytes(b"fixture")
            steam = home / ".local/share/Steam"
            steam_game = steam / "steamapps/common/Elite Dangerous" / PRODUCT
            steam_game.mkdir(parents=True)
            (steam_game / "EliteDangerous64.exe").write_bytes(b"fixture")
            (steam / "steamapps/libraryfolders.vdf").write_text('"libraryfolders" {}', encoding="utf-8")
            manifest = steam / "steamapps/appmanifest_359320.acf"
            manifest.write_text('"AppState" { "appid" "359320" "installdir" "Elite Dangerous" }', encoding="utf-8")
            compat = root / "explicit compatdata"
            prefix = compat / "pfx"
            (prefix / "drive_c").mkdir(parents=True)
            (prefix / "dosdevices").mkdir()
            (prefix / "dosdevices/z:").symlink_to(root, target_is_directory=True)
            (prefix / "system.reg").write_bytes(b"registry")
            (prefix / "user.reg").write_bytes(b"registry")
            proton = root / "Proton/proton"
            proton.parent.mkdir()
            proton.write_bytes(b"fixture")
            (proton.parent / "toolmanifest.vdf").write_text('"manifest" {}', encoding="utf-8")
            wine = root / "wine"
            wine.write_bytes(b"fixture")
            epic_root = home / "Epic Elite"
            epic_game = epic_root / PRODUCT
            epic_game.mkdir(parents=True)
            (epic_game / "EliteDangerous64.exe").write_bytes(b"fixture")
            legendary = home / ".config/legendary"
            legendary.mkdir(parents=True)
            installed = legendary / "installed.json"
            installed.write_text(json.dumps({"ELITE": {"install_path": str(epic_root)}}), encoding="utf-8")
            (legendary / "config.ini").write_text(
                "[ELITE]\nwine_prefix=" + str(prefix) + "\nwine_executable=" + str(wine) + "\n", encoding="utf-8")
            env = {"PATH": "/fixture/host", "HOME": str(home)}
            words = []
            expected_error = None
            expected_store = "steam"
            if case in ("steam-hint", "steam-env-hint"):
                expected_error = "Steam root not found; pass --steam-dir"
                if case == "steam-hint":
                    words = ["--steam-dir", str(root / "missing Steam")]
                else:
                    env["STEAM_DIR"] = str(root / "missing Steam")
            elif case in ("epic-root", "epic-product"):
                # Epic is found by detection and refused: the launcher is for Steam installs.
                expected_error = "Epic is not supported by this wrapper"
                words = ["--game-dir", str(epic_root if case == "epic-root" else epic_game)]
            elif case == "steam-prefix":
                installed.unlink()
                words = ["--prefix", str(compat), "--proton", str(proton)]
            elif case in ("both-stores", "both-prefix", "ambiguous-steam"):
                expected_error = "multiple Steam/Epic installs"
                if case == "both-prefix":
                    words = ["--prefix", str(compat)]
                if case == "ambiguous-steam":
                    second = home / ".steam/steam"
                    second_game = second / "steamapps/common/Another Elite" / PRODUCT
                    second_game.mkdir(parents=True)
                    (second_game / "EliteDangerous64.exe").write_bytes(b"fixture")
                    (second / "steamapps/libraryfolders.vdf").write_text('"libraryfolders" {}', encoding="utf-8")
                    (second / "steamapps/appmanifest_359320.acf").write_text(
                        '"AppState" { "appid" "359320" "installdir" "Another Elite" }', encoding="utf-8")
            elif case == "malformed-steam":
                manifest.write_text('"AppState" { "appid" "wrong" }', encoding="utf-8")
                expected_error = "wrong appid"
            elif case == "epic-overflow-auto":
                games = home / ".config/lutris/games"
                games.mkdir(parents=True)
                for number in range(513):
                    (games / (str(number) + ".yml")).write_bytes(b"fixture")
                expected_error = "512"

            def snapshot():
                return {str(path.relative_to(root)): ("link", str(path.readlink())) if path.is_symlink()
                        else ("file", path.read_bytes()) if path.is_file() else ("directory",)
                        for path in root.rglob("*")}

            before = snapshot()
            output, errors = io.StringIO(), io.StringIO()
            original_run = globals()["run"]

            def fixture_run(args):
                return original_run(args, script=script, home=home, environ=env,
                    launch=lambda *_a, **_k: (_ for _ in ()).throw(AssertionError("dry-run started a process")))

            globals()["run"] = fixture_run
            try:
                with redirect_stdout(output), redirect_stderr(errors):
                    status = main(["--dry-run", *words])
                if expected_error:
                    assert status == 2 and expected_error in errors.getvalue(), (status, errors.getvalue())
                else:
                    assert status == 0, errors.getvalue()
                    command, launch_env, _ = build_plan(parse_argv(words), script=script, home=home, environ=env)
                    assert command[command.index("--store") + 1] == expected_store, command
                    if case == "steam-prefix":
                        assert launch_env["STEAM_COMPAT_DATA_PATH"] == str(compat)
                    assert shlex.join(command) in output.getvalue()
                assert snapshot() == before, "auto dry-run wrote files"
            except (AssertionError, ValueError) as exc:
                failures.append(case + ": " + str(exc))
            finally:
                globals()["run"] = original_run
    assert not failures, "; ".join(failures)


def steam_context_self_test(only=None):
    """Exercise real Linux prefixes/deployments without starting any installed binary."""
    if os.name == "nt":
        return
    import io
    from contextlib import redirect_stdout
    from tempfile import TemporaryDirectory

    parse = lambda *words: parse_argv(list(words))
    failures = []
    for case in ("prefix", "store", "flatpak", "flatpak-usr", "flatpak-full",
                 "flatpak-local-full", "flatpak-local-sub", "flatpak-parent-app", "flatpak-parent-usr",
                 "flatpak-config", "flatpak-config-link", "flatpak-config-dangling", "flatpak-config-relative",
                 "flatpak-config-file", "flatpak-config-installations-file", "flatpak-config-unreadable",
                 "flatpak-config-missing", "flatpak-config-parent"):
        if only is not None and case != only:
            continue
        with TemporaryDirectory() as tmp:
            root = Path(tmp).resolve()
            home = root / "home"
            steam = home / (".var/app/com.valvesoftware.Steam/.local/share/Steam" if case.startswith("flatpak")
                            else ".local/share/Steam")
            script = root / "tools/install_proton.py"
            script.parent.mkdir()
            (script.parent / "ed-up-launcher.exe").write_bytes(b"fixture only")
            game_root = root / "external Steam library/steamapps/common/Custom title"
            game = game_root / PRODUCT
            game.mkdir(parents=True)
            (game / "EliteDangerous64.exe").write_bytes(b"fixture only")
            library = game_root.parents[2]
            (library / "steamapps/appmanifest_359320.acf").write_text(
                '\"AppState\" { \"appid\" \"359320\" \"installdir\" \"Custom title\" }', encoding="utf-8")
            (steam / "steamapps").mkdir(parents=True)
            (steam / "steamapps/libraryfolders.vdf").write_text(
                '\"libraryfolders\" { \"0\" { \"path\" \"' + str(library) + '\" } }', encoding="utf-8")
            compat = library / "steamapps/compatdata/359320"
            alternate = root / "explicit Elite prefix"
            shortcut = root / "shortcut prefix"
            proton = root / "installed Proton"
            proton.mkdir()
            (proton / "proton").write_bytes(b"fixture only")
            (proton / "toolmanifest.vdf").write_text('\"manifest\" { \"version\" \"2\" }', encoding="utf-8")

            def record(prefix, selected=proton):
                (prefix / "config_info").write_text("\n".join(("11.0-100",
                    str(selected / "files/share/fonts"), str(selected / "files/lib"), str(steam),
                    "0", "0", "0", str(selected / "files/share/default_pfx"))) + "\n", encoding="utf-8")

            for prefix in (compat, alternate, shortcut):
                (prefix / "pfx/drive_c").mkdir(parents=True)
                (prefix / "pfx/dosdevices").mkdir()
                (prefix / "pfx/dosdevices/q:").symlink_to(library, target_is_directory=True)
                record(prefix)
            config_root = root / "flatpak-config"
            config_root.mkdir()
            env = {"STEAM_DIR": str(steam), "PATH": str(root / "bin"),
                   "FLATPAK_SYSTEM_DIR": str(root / "system-flatpak"), "FLATPAK_CONFIG_DIR": str(config_root)}

            def plan(*words, environment=None):
                return build_plan(parse("--store", "steam", *words), script=script, home=home,
                                  environ=env if environment is None else environment)

            def refuses(reason, *words):
                try:
                    plan(*words)
                    raise AssertionError("accepted invalid Steam context: " + reason)
                except ValueError as exc:
                    assert reason in str(exc), str(exc)
                    return str(exc)

            def snapshot():
                return {str(path.relative_to(root)): ("link", str(path.readlink())) if path.is_symlink()
                        else ("file", path.read_bytes()) if path.is_file() else ("directory",)
                        for path in root.rglob("*")}

            def dry_run(*words):
                before = snapshot()
                output = io.StringIO()
                original_run = globals()["run"]

                def fixture_run(args):
                    return original_run(args, script=script, home=home, environ=env,
                        launch=lambda *_a, **_k: (_ for _ in ()).throw(AssertionError("dry-run started a process")))

                globals()["run"] = fixture_run
                try:
                    with redirect_stdout(output):
                        assert main(["--store", "steam", "--dry-run", *words]) == 0
                finally:
                    globals()["run"] = original_run
                assert snapshot() == before, "Steam dry-run wrote files"
                return output.getvalue()

            try:
                if case == "prefix":
                    env.update(STEAM_COMPAT_DATA_PATH=str(shortcut), SteamAppId="1000000001",
                               SteamGameId="4294967296000000001", STEAM_COMPAT_APP_ID="1000000001")
                    assert plan()[1]["STEAM_COMPAT_DATA_PATH"] == str(compat), "inherited shortcut prefix selected"
                    assert plan("--prefix", str(alternate))[1]["STEAM_COMPAT_DATA_PATH"] == str(alternate)
                    dry_run("--prefix", str(alternate))
                    elite_env = dict(env, SteamAppId=APP_ID, SteamGameId=APP_ID, STEAM_COMPAT_APP_ID=APP_ID,
                                     STEAM_COMPAT_DATA_PATH=str(alternate))
                    assert plan(environment=elite_env)[1]["STEAM_COMPAT_DATA_PATH"] == str(alternate)
                    elite_env["SteamGameId"] = "4294967296000000001"
                    assert plan(environment=elite_env)[1]["STEAM_COMPAT_DATA_PATH"] == str(compat)
                    markerless = {key: value for key, value in env.items()
                                  if key not in ("SteamAppId", "SteamGameId", "STEAM_COMPAT_APP_ID")}
                    assert plan(environment=markerless)[1]["STEAM_COMPAT_DATA_PATH"] == str(compat)
                    refuses("compatdata root", "--prefix", str(alternate / "pfx"))
                elif case == "store":
                    for words in ((), ("--launcher",)):
                        output = dry_run(*words)
                        command, _, _ = plan(*words)
                        start = command.index("runinprefix")
                        expected_exe = script.parent / "ed-up-launcher.exe"
                        assert command[start:start + 6] == ["runinprefix", str(expected_exe), "--store", "steam",
                                                          "--dir", "Q:\\steamapps\\common\\Custom title\\Products\\elite-dangerous-odyssey-64"], command
                        assert shlex.join(command) in output
                else:
                    flat_id = "com.valvesoftware.Steam"
                    installation = home / ".local/share/flatpak"
                    app = installation / "app" / flat_id
                    deployment = app / "x86_64/stable/fixture-commit"
                    (deployment / "files").mkdir(parents=True)
                    (app / "x86_64/stable/active").symlink_to("fixture-commit", target_is_directory=True)
                    (app / "current").symlink_to("x86_64/stable", target_is_directory=True)
                    metadata = ("[Application]\nname=" + flat_id + "\nruntime=org.example.Platform/x86_64/1\n"
                                "[Extension com.valvesoftware.Steam.CompatibilityTool]\n"
                                "directory=share/steam/compatibilitytools.d\nsubdirectories=true\n"
                                "version=stable\nversions=stable;beta;test;\n")
                    (deployment / "metadata").write_text(metadata, encoding="utf-8")
                    platform = installation / "runtime/org.example.Platform/x86_64/1/active"
                    (platform / "files").mkdir(parents=True)
                    (platform / "metadata").write_text("[Runtime]\nname=org.example.Platform\n", encoding="utf-8")
                    extension_id = flat_id + ".CompatibilityTool.Proton-GE"
                    extension = installation / "runtime" / extension_id / "x86_64/stable/active"
                    (extension / "files").mkdir(parents=True)
                    (extension / "files/proton").write_bytes(b"fixture only")
                    (extension / "metadata").write_text("[Runtime]\nname=" + extension_id + "\n", encoding="utf-8")
                    (extension / "files/toolmanifest.vdf").write_text(
                        '\"manifest\" { \"require_tool_appid\" \"1628350\" }', encoding="utf-8")
                    recorded = Path("/app/share/steam/compatibilitytools.d/Proton-GE")
                    record(compat, recorded)
                    runtime = library / "steamapps/common/SteamLinuxRuntime_sniper"
                    runtime.mkdir()
                    (runtime / "_v2-entry-point").write_bytes(b"fixture only")
                    (library / "steamapps/appmanifest_1628350.acf").write_text(
                        '\"AppState\" { \"appid\" \"1628350\" \"installdir\" \"SteamLinuxRuntime_sniper\" }', encoding="utf-8")
                    flatpak = root / "bin/flatpak"
                    flatpak.parent.mkdir()
                    flatpak.write_bytes(b"fixture only")
                    flatpak.chmod(0o700)
                    env.update(WINEARCH="host-arch", WINEPREFIX="must-not-print-unset-value",
                               WINEDEBUG="host-debug", PROTON_VERSION="host-proton", SteamAppId="wrong-host-id")
                    if case == "flatpak-config-missing":
                        config_root.rmdir()
                        output = dry_run()
                        assert "--command=" + str(runtime / "_v2-entry-point") in plan()[0]
                        assert "/run/parent/app/share/steam/compatibilitytools.d/Proton-GE/proton" in output
                        continue
                    if case.startswith("flatpak-config"):
                        from unittest.mock import patch

                        if case == "flatpak-config-parent":
                            # GIO collapses '..' lexically, while resolve() follows
                            # the symlink first. The lexical config has an extra
                            # installation; the physical sibling is empty.
                            (root / "elsewhere/child").mkdir(parents=True)
                            (root / "config-alias").symlink_to(root / "elsewhere/child", target_is_directory=True)
                            (root / "elsewhere/flatpak-config").mkdir()
                            (config_root / "installations.d").mkdir()
                            (config_root / "installations.d/named.conf").write_text(
                                '[Installation "extra"]\nPath=' + str(root / "extra-system") + "\n", encoding="utf-8")
                            env["FLATPAK_CONFIG_DIR"] = str(root / "config-alias/../flatpak-config")
                        elif case in ("flatpak-config", "flatpak-config-link"):
                            configs = config_root / "installations.d"
                            configs.mkdir()
                            extra = root / "extra.conf"
                            extra.write_text('[Installation "extra"]\nPath=' + str(root / "extra-system") + "\n", encoding="utf-8")
                            if case == "flatpak-config":
                                extra.rename(configs / "extra.conf")
                            else:
                                (configs / "extra.conf").symlink_to(extra)
                        elif case == "flatpak-config-dangling":
                            config_root.rmdir()
                            config_root.symlink_to(root / "missing-config", target_is_directory=True)
                        elif case == "flatpak-config-relative":
                            env["FLATPAK_CONFIG_DIR"] = os.path.relpath(config_root)
                        elif case == "flatpak-config-file":
                            config_root.rmdir()
                            config_root.write_bytes(b"not a directory")
                        elif case == "flatpak-config-installations-file":
                            (config_root / "installations.d").write_bytes(b"not a directory")
                        before = snapshot()
                        if case == "flatpak-config-unreadable":
                            original_scandir = os.scandir

                            def unreadable_config(path):
                                if Path(path) == config_root:
                                    raise PermissionError("fixture access denied")
                                return original_scandir(path)

                            with patch.object(os, "scandir", unreadable_config):
                                error = refuses("Flatpak installation configuration")
                        else:
                            error = refuses("Flatpak installation configuration")
                        if case == "flatpak-config-relative":
                            assert "root must be absolute" in error, error
                        assert "--proton" in error and "host runner" in error
                        assert snapshot() == before, "refused Steam configuration wrote files"
                        # A host runner is an explicit recovery from unsupported sandbox configuration.
                        assert plan("--proton", str(proton))[0][:2] == [str(proton / "proton"), "runinprefix"]
                        continue
                    if case in ("flatpak-full", "flatpak-local-full", "flatpak-local-sub"):
                        # Flatpak prefers full extension mounts to subextensions, and local
                        # unmaintained mounts can also replace the installed runner's files.
                        base_id = flat_id + ".CompatibilityTool"
                        if case == "flatpak-full":
                            shadow = installation / "runtime" / base_id / "x86_64/stable/active/files/Proton-GE"
                        elif case == "flatpak-local-full":
                            shadow = installation / "extension" / base_id / "x86_64/stable/Proton-GE"
                        else:
                            shadow = installation / "extension" / extension_id / "x86_64/stable"
                        shadow.mkdir(parents=True)
                        if case == "flatpak-full":
                            (shadow.parents[1] / "metadata").write_text("[Runtime]\nname=" + base_id + "\n", encoding="utf-8")
                        (shadow / "proton").write_bytes(b"different shadowing runner")
                        (shadow / "toolmanifest.vdf").write_text(
                            '\"manifest\" { \"require_tool_appid\" \"999999\" }', encoding="utf-8")
                        refuses("shadow")
                        continue
                    if case in ("flatpak-usr", "flatpak-parent-usr"):
                        recorded = Path("/usr/share/Proton")
                        extension = platform
                        runner_files = platform / "files/share/Proton"
                        runner_files.mkdir(parents=True)
                        (runner_files / "proton").write_bytes(b"fixture only")
                        (runner_files / "toolmanifest.vdf").write_text(
                            '\"manifest\" { \"require_tool_appid\" \"1628350\" }', encoding="utf-8")
                        record(compat, recorded)
                        expected_payload = "/run/parent/usr/share/Proton/proton"
                    else:
                        runner_files = extension / "files"
                        expected_payload = "/run/parent/app/share/steam/compatibilitytools.d/Proton-GE/proton"
                    if case in ("flatpak-parent-app", "flatpak-parent-usr"):
                        recorded = Path("/run/parent") / recorded.relative_to("/")
                        record(compat, recorded)
                    # Model the nested namespace: the parent app/runtime is under
                    # /run/parent, not the nested runtime's /app or /usr.
                    nested = root / "runtime namespace"
                    nested_runner = nested / Path(expected_payload).relative_to("/")
                    nested_runner.parent.parent.mkdir(parents=True)
                    nested_runner.parent.symlink_to(runner_files, target_is_directory=True)
                    for words in ((), ("--launcher",)):
                        output = dry_run(*words)
                        command, resolved, details = plan(*words)
                        assert details[2] == recorded / "proton", "recorded runner details changed"
                        assert shlex.join(command) in output
                        assert command[:3] == [str(flatpak), "run", "--user"]
                        assert "--arch=x86_64" in command and "--branch=stable" in command
                        assert "--command=" + str(runtime / "_v2-entry-point") in command
                        start = command.index(flat_id)
                        assert command[start + 1:start + 6] == ["--verb=run", "--", expected_payload,
                                                               "runinprefix", str(script.parent / "ed-up-launcher.exe")]
                        payload = nested / Path(command[start + 3]).relative_to("/")
                        assert payload.read_bytes() == b"fixture only", "runner is unavailable in runtime namespace"
                        assert "--env=STEAM_COMPAT_CLIENT_INSTALL_PATH=" + str(steam) in command
                        assert "--env=STEAM_COMPAT_DATA_PATH=" + str(compat) in command
                        assert "--filesystem=" + str(library) in command
                        assert "--env=SteamAppId=" + APP_ID in command
                        assert "--env=SteamGameId=" + APP_ID in command
                        assert "--env=STEAM_COMPAT_APP_ID=" + APP_ID in command
                        assert resolved == env, "Steam sandbox environment leaked into host flatpak process"
                        for key in ("WINEARCH", "WINEPREFIX", "WINEDEBUG", "PROTON_VERSION"):
                            assert "--unset-env=" + key in command, "Steam inherited a removed host variable: " + key
                        assert "--unset-env=SteamAppId" not in command
                        assert "must-not-print-unset-value" not in output
                        assert "--env=PATH=/app/bin:/usr/bin" in command
                    if case in ("flatpak-usr", "flatpak-parent-usr", "flatpak-parent-app"):
                        (runner_files / "toolmanifest.vdf").write_text('\"manifest\" { \"version\" \"2\" }', encoding="utf-8")
                        direct = ("/app/share/steam/compatibilitytools.d/Proton-GE/proton" if case == "flatpak-parent-app"
                                  else "/usr/share/Proton/proton")
                        assert "--command=" + direct in plan()[0]
                        continue
                    (extension / "files/proton").unlink()
                    refuses("missing")
                    (extension / "files/proton").write_bytes(b"fixture only")
                    duplicate = installation / "runtime" / extension_id / "x86_64/beta/active"
                    (duplicate / "files").mkdir(parents=True)
                    (duplicate / "metadata").write_text("[Runtime]\nname=" + extension_id + "\n", encoding="utf-8")
                    (duplicate / "files/proton").write_bytes(b"fixture only")
                    refuses("ambiguous")
                    shutil.rmtree(duplicate.parents[0])
                    second = Path(env["FLATPAK_SYSTEM_DIR"]) / "app" / flat_id / "x86_64/stable/active"
                    (second / "files").mkdir(parents=True)
                    (second / "metadata").write_text(metadata, encoding="utf-8")
                    refuses("ambiguous")
                    shutil.rmtree(second.parents[3])
                    # Without a Steam runtime dependency, Proton itself is the sandbox command.
                    (extension / "files/toolmanifest.vdf").write_text('\"manifest\" { \"version\" \"2\" }', encoding="utf-8")
                    assert "--command=" + str(recorded / "proton") in plan()[0]
                    (extension / "files/toolmanifest.vdf").write_text(
                        '\"manifest\" { \"require_tool_appid\" \"1628350\" }', encoding="utf-8")
                    # The system deployment is pinned even when the app data remains in ~/.var/app.
                    system_app = Path(env["FLATPAK_SYSTEM_DIR"]) / "app" / flat_id
                    system_app.parent.mkdir(parents=True)
                    app.rename(system_app)
                    assert plan()[0][:3] == [str(flatpak), "run", "--system"]
                    system_app.rename(app)
                    platform.rename(platform.with_name("unavailable"))
                    refuses("runtime metadata is missing")
                    # An explicit host override recovers a missing Flatpak deployment without fallback guessing.
                    shutil.rmtree(app)
                    override, _, _ = plan("--proton", str(proton))
                    assert override[:2] == [str(proton / "proton"), "runinprefix"]
                    refuses("deployment is missing")
            except (AssertionError, ValueError) as exc:
                failures.append(case + ": " + str(exc))
    assert not failures, "; ".join(failures)


def self_test():
    import io
    from contextlib import redirect_stderr, redirect_stdout
    from tempfile import TemporaryDirectory

    parse = lambda *words: parse_argv(list(words))
    assert parse("--store", "epic", "--prefix", "/existing").store == "epic"
    assert launcher_args(parse()) == [] and launcher_args(parse("--launcher")) == []
    assert launcher_args(parse("--launcher", "--", "--vr", "a b")) == ["--vr", "a b"]
    for invalid in (("--dry-run", "--self-test"),):
        with redirect_stderr(io.StringIO()):
            try:
                parse(*invalid)
                raise AssertionError("accepted mutually exclusive arguments")
            except SystemExit as exc:
                assert exc.code == 2

    with TemporaryDirectory() as tmp:
        # Windows TEMP may use an 8.3 alias; match build_plan()'s resolved paths.
        root = Path(tmp).resolve()
        home = root / "home"
        steam = home / ".local/share/Steam"
        other = home / "other Steam"
        script = root / "tools/install_proton.py"
        script.parent.mkdir()
        exe = script.parent / "ed-up-launcher.exe"
        exe.write_bytes(b"exe")
        proton = steam / "steamapps/common/Proton's $() Experimental"
        if os.name == "nt":
            proton = Path("//?/" + proton.resolve().as_posix())
        (proton / "files/share/default_pfx").mkdir(parents=True)
        (proton / "proton").write_bytes(b"script")
        (proton / "toolmanifest.vdf").write_text(
            '"manifest" { "require_tool_appid" "4183110" }', encoding="utf-8")
        runtime = steam / "steamapps/common/SteamLinuxRuntime_4"
        runtime.mkdir(parents=True)
        (runtime / "_v2-entry-point").write_bytes(b"script")
        (steam / "steamapps/appmanifest_4183110.acf").write_text(
            '"AppState" { "appid" "4183110" "installdir" "SteamLinuxRuntime_4" }', encoding="utf-8")
        game_root = steam / "steamapps/common/Elite's $() Dangerous"
        game = game_root / PRODUCT
        game.mkdir(parents=True)
        (game / "EliteDangerous64.exe").write_bytes(b"game")
        (steam / "steamapps/appmanifest_359320.acf").write_text(
            '"AppState" { "appid" "359320" "installdir" "Elite\'s $() Dangerous" }', encoding="utf-8")
        (steam / "steamapps/libraryfolders.vdf").write_text(
            '"libraryfolders" { "0" { "path" "' + str(steam) + '" "apps" { "359320" "1" } } }',
            encoding="utf-8")
        compat = steam / "steamapps/compatdata/359320"
        (compat / "pfx/drive_c").mkdir(parents=True)
        record_lines = ["11.0-100", str(proton / "files/share/fonts"),
                        str(proton / "files/lib"), str(steam), "0", "0", "0",
                        str(proton / "files/share/default_pfx")]
        (compat / "config_info").write_text(
            "\n".join(path.replace("\\", "/") for path in record_lines) + "\n", encoding="utf-8")
        dos = compat / "pfx/dosdevices"
        dos.mkdir()
        if os.name != "nt":
            (dos / "z:").symlink_to("/", target_is_directory=True)
            (dos / "s:").symlink_to(steam, target_is_directory=True)
        else:
            # Build gate on Windows has no POSIX symlink privilege. Mock the
            # mapping at the function seam; the parser/runner tests still run.
            original_windows_path = globals()["windows_path"]
            globals()["windows_path"] = lambda path, _compat: "S:\\" + "\\".join(path.relative_to(steam).parts)
        env = {"WINEPREFIX": "/wrong", "WINESERVER": "/wrong/server", "PROTON_VERSION": "old",
               "STEAM_DIR": str(steam), "PATH": "/mock"}
        args = parse("--dry-run")
        before = set(root.rglob("*"))
        buf = io.StringIO()
        calls = []

        def launch(command, **kwargs):
            calls.append((command, kwargs))
            return subprocess.CompletedProcess(command, 23)

        try:
            with redirect_stdout(buf):
                assert run(args, script=script, home=home, environ=env, launch=launch,
                           platform="linux", geteuid=lambda: 1000) == 0
            assert not calls and set(root.rglob("*")) == before
            command, process_env, details = build_plan(args, script=script, home=home, environ=env)
            assert command[:3] == [str(runtime / "_v2-entry-point"), "--verb=run", "--"]
            assert command[3:9] == [str(proton / "proton"), "runinprefix", str(exe), "--store", "steam", "--dir"]
            assert "waitforexitandrun" not in command
            assert command[-2] == "--dir" and "--install" not in command
            assert str(game) in buf.getvalue() and shlex.join(command) in buf.getvalue()
            assert "WINEPREFIX" not in process_env and "WINESERVER" not in process_env
            assert "PROTON_VERSION" not in process_env and process_env["SteamAppId"] == APP_ID
            assert process_env["STEAM_COMPAT_DATA_PATH"] == str(compat)
            inconsistent = list(record_lines)
            inconsistent[1] = str(other / "files/share/fonts")
            (compat / "config_info").write_text(
                "\n".join(path.replace("\\", "/") for path in inconsistent) + "\n", encoding="utf-8")
            try:
                recorded_proton(compat)
                raise AssertionError("inconsistent Proton record accepted")
            except ValueError as exc:
                assert "inconsistent" in str(exc)
            arm64_record = list(record_lines)
            arm64_record[7] = str(proton / "files/share/default_pfx_arm64")
            (compat / "config_info").write_text(
                "\n".join(path.replace("\\", "/") for path in arm64_record) + "\n", encoding="utf-8")
            assert recorded_proton(compat) == proton
            proton9_record = ["9.0-203", str(proton / "files/share/fonts"),
                              str(proton / "files/lib"), str(proton / "files/lib64"),
                              str(steam), "1793056620.0", "1793056626.0", "1516738214.0",
                              str(proton / "files/share/default_pfx")]
            (compat / "config_info").write_text(
                "\n".join(path.replace("\\", "/") for path in proton9_record) + "\n", encoding="utf-8")
            assert recorded_proton(compat) == proton
            (compat / "config_info").write_text(
                "\n".join(path.replace("\\", "/") for path in record_lines) + "\n", encoding="utf-8")
            assert run(parse(), script=script, home=home, environ=env,
                       launch=launch, platform="linux", geteuid=lambda: 1000) == 23
            assert calls[-1][0][-2] == "--dir" and str(exe) in calls[-1][0]
            if os.name != "nt":
                original_game = game.with_name("original-product")
                game.rename(original_game)
                outside = root / "outside product"
                outside.mkdir()
                game.symlink_to(outside, target_is_directory=True)
                try:
                    find_game(parse(), [steam])
                    raise AssertionError("default game symlink escape accepted")
                except ValueError as exc:
                    assert "escapes Steam install" in str(exc)
                game.unlink()
                original_game.rename(game)
                entry = runtime / "_v2-entry-point"
                entry.unlink()
                outside_entry = root / "outside runtime"
                outside_entry.write_bytes(b"script")
                entry.symlink_to(outside_entry)
                try:
                    runtime_path(proton / "proton", [steam])
                    raise AssertionError("runtime entry symlink escape accepted")
                except ValueError as exc:
                    assert "runtime entry escapes" in str(exc)
                entry.unlink()
                entry.write_bytes(b"script")
            try:
                run(parse(), script=script, home=home, environ=env,
                    launch=launch, platform="linux", geteuid=lambda: 0)
                raise AssertionError("root accepted")
            except ValueError as exc:
                assert "sudo" in str(exc)
            assert len(calls) == 1
            launcher_self_test(parse, root, script, home, env, launch, calls)
            (compat / "config_info").unlink()
            try:
                build_plan(args, script=script, home=home, environ=env)
                raise AssertionError("missing record accepted")
            except ValueError as exc:
                assert "--proton" in str(exc)
            assert build_plan(parse("--proton", str(proton), "--dry-run"), script=script,
                              home=home, environ=env)[0][3] == str(proton / "proton")
            (compat / "pfx/drive_c").rmdir()
            try:
                build_plan(parse("--proton", str(proton), "--dry-run"), script=script,
                           home=home, environ=env)
                raise AssertionError("missing prefix accepted")
            except ValueError as exc:
                assert "launch Elite once" in str(exc)
            (compat / "pfx/drive_c").mkdir()
            (proton / "toolmanifest.vdf").write_text(
                '"manifest" { "require_tool_appid" "999999" }', encoding="utf-8")
            try:
                build_plan(parse("--proton", str(proton), "--dry-run"), script=script,
                           home=home, environ=env)
                raise AssertionError("missing runtime accepted")
            except ValueError as exc:
                assert "999999" in str(exc)
            (proton / "toolmanifest.vdf").unlink()
            try:
                build_plan(parse("--proton", str(proton), "--dry-run"), script=script,
                           home=home, environ=env)
                raise AssertionError("missing toolmanifest accepted")
            except ValueError as exc:
                assert "toolmanifest.vdf" in str(exc)
            (proton / "toolmanifest.vdf").write_text('"manifest" { "version" "2" }', encoding="utf-8")
            assert build_plan(parse("--proton", str(proton), "--dry-run"), script=script,
                              home=home, environ=env)[0][0] == str(proton / "proton")
            second = other / "steamapps/common/Elite Dangerous" / PRODUCT
            second.mkdir(parents=True)
            (second / "EliteDangerous64.exe").write_bytes(b"game")
            (other / "steamapps/appmanifest_359320.acf").write_text(
                '"AppState" { "appid" "359320" "installdir" "Elite Dangerous" }', encoding="utf-8")
            (steam / "steamapps/libraryfolders.vdf").write_text(
                '"libraryfolders" { "0" { "path" "' + str(steam) + '" } '
                '"1" { "path" "' + str(other) + '" } }', encoding="utf-8")
            (second / "EliteDangerous64.exe").unlink()
            assert find_game(parse(), [steam])[3] == game  # stale second manifest is skipped
            (second / "EliteDangerous64.exe").write_bytes(b"game")
            try:
                find_game(parse(), [steam])
                raise AssertionError("ambiguous libraries accepted")
            except ValueError as exc:
                assert "multiple" in str(exc)
            assert find_game(parse("--game-dir", str(game)), [steam])[3] == game
            try:
                safe_leaf("../escape", "test")
                raise AssertionError("unsafe leaf accepted")
            except ValueError:
                pass
            try:
                vdf(root / "missing.vdf")
                raise AssertionError("missing VDF accepted")
            except ValueError:
                pass
        finally:
            if os.name == "nt":
                globals()["windows_path"] = original_windows_path
    auto_selection_self_test()
    epic_self_test()
    steam_context_self_test()
    print("install_proton: self-test passed")


def main(argv=None):
    args = parse_argv(argv)
    if args.self_test:
        self_test()
        return 0
    try:
        return run(args)
    except ValueError as exc:
        print("EDVR: " + str(exc), file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
