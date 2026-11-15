#!/usr/bin/env python3
"""Run EDVR's Windows installer in Elite's existing Steam or Epic Wine/Proton prefix."""

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
    p.add_argument("--installer", type=Path, help="edvr-installer.exe (beside script or build/ by default)")
    p.add_argument("--launcher", action="store_true",
                   help="run ed-up-launcher.exe instead of the installer; arguments after a literal -- go to it")
    p.add_argument("--launcher-exe", type=Path,
                   help="ed-up-launcher.exe (beside script or build/ by default); implies nothing without --launcher")
    p.add_argument("--steam-dir", type=Path, help="Steam root; otherwise use STEAM_DIR or known locations")
    p.add_argument("--store", choices=("auto", "steam", "epic"), default="auto",
                   help="select the store; auto refuses when both stores have matching installs")
    p.add_argument("--proton", type=Path,
                   help="host Proton directory or script; overrides the prefix record and bypasses Flatpak")
    p.add_argument("--prefix", type=Path,
                   help="existing Epic Wine prefix; with --store steam, Elite's Proton compatdata root (containing pfx/)")
    p.add_argument("--wine", type=Path,
                   help="explicit host Wine binary for Epic (bypasses a recorded Flatpak runner)")
    p.add_argument("--game-dir", type=Path, help="folder holding EliteDangerous64.exe inside the installed game")
    actions = p.add_mutually_exclusive_group()
    for name in ("install", "repair", "uninstall", "collect-logs"):
        actions.add_argument("--" + name, action="store_true")
    actions.add_argument("--preview", action="store_true",
                         help="start Proton/Wine to show the Windows install plan without changing game files")
    p.add_argument("--scope", choices=("both", "vr", "flat"))
    p.add_argument("--replace-settings", action="store_true")
    p.add_argument("--remove-settings", action="store_true")
    offline = p.add_mutually_exclusive_group()
    offline.add_argument("--dry-run", action="store_true",
                         help="resolve and print the exact command; start no processes and write nothing")
    offline.add_argument("--self-test", action="store_true", help=argparse.SUPPRESS)
    return p


LAUNCHER_REFUSED = ("install", "repair", "uninstall", "collect_logs", "preview", "scope",
                    "replace_settings", "remove_settings", "installer")


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
    for name in LAUNCHER_REFUSED:
        if getattr(args, name):
            raise ValueError("--%s is an installer option; it cannot be used with --launcher" %
                             name.replace("_", "-"))
    forwarded = list(args.passthrough or [])
    if any(word.split("=", 1)[0].lower() in ("--dir", "--store") for word in forwarded):
        raise ValueError("launcher arguments cannot override the selected --dir or --store")
    return forwarded


def installer_args(args):
    if getattr(args, "passthrough", None) is not None and not args.launcher:
        raise ValueError("arguments after -- are forwarded to the launcher; use --launcher")
    if args.launcher_exe is not None and not args.launcher:
        raise ValueError("--launcher-exe requires --launcher")
    if args.launcher:
        return launcher_args(args)
    action = next((name for name in ("install", "repair", "uninstall", "collect_logs", "preview")
                   if getattr(args, name)), None)
    if args.scope and action not in ("install", "repair", "preview"):
        raise ValueError("--scope requires --install, --repair or --preview")
    if args.replace_settings and action not in ("install", "repair", "preview"):
        raise ValueError("--replace-settings requires --install, --repair or --preview")
    if args.remove_settings and action != "uninstall":
        raise ValueError("--remove-settings requires --uninstall")
    result = (["--install", "--dry-run"] if action == "preview" else
              (["--" + action.replace("_", "-")] if action else []))
    if args.scope:
        result += ["--scope", args.scope]
    if args.replace_settings:
        result.append("--replace-settings")
    if args.remove_settings:
        result.append("--remove-settings")
    return result


def launcher_path(args, script):
    if args.launcher_exe is not None:
        return args.launcher_exe.expanduser().resolve()
    adjacent = script.resolve().parent / "ed-up-launcher.exe"
    return adjacent if adjacent.is_file() else script.resolve().parents[1] / "build" / "ed-up-launcher.exe"


def installer_path(args, script):
    if args.launcher:
        return launcher_path(args, script)
    if args.installer is not None:
        return args.installer.expanduser().resolve()
    adjacent = script.resolve().parent / "edvr-installer.exe"
    return adjacent if adjacent.is_file() else script.resolve().parents[1] / "build" / "edvr-installer.exe"


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
        raise ValueError("Elite Steam app %s has no valid install%s; check --steam-dir and --game-dir" %
                         (APP_ID, detail))
    if len(matches) > 1:
        raise ValueError("multiple Elite installs found; pass --steam-dir and --game-dir to select one")
    return matches[0]


def compatdata_path(library, environ, explicit=None):
    identities = [environ[key] for key in ("SteamAppId", "SteamGameId", "STEAM_COMPAT_APP_ID")
                  if environ.get(key)]
    # A non-Steam installer shortcut carries its own valid prefix. Only Elite's
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


def optional_steam_roots(args, home, environ):
    try:
        return roots(args, home, environ)
    except ValueError:
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


def flatpak_host_file(path, app_id, home, environ, selected=None):
    """Resolve sandbox /app and /usr paths from installed deployment metadata, without a process."""
    path = Path(path)
    if not path.is_absolute():
        raise ValueError("recorded runner must be an absolute path: %s" % path)
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
                     "repair the manager or pass --wine/--proton with an installed host runner" % (app_id, path))


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


def flatpak_path_runner(binary, fallback, app_id, home, environ, sandbox_env):
    for directory in sandbox_env["PATH"].split(":"):
        candidate = Path(directory) / binary
        try:
            if flatpak_host_file(candidate, app_id, home, environ).is_file():
                return candidate
        except ValueError:
            continue
    if binary != "umu-run":
        raise ValueError("Flatpak %s is missing from its effective PATH; pass a host runner override" % binary)
    return fallback


def existing_prefix(path):
    path = path.expanduser().resolve()
    if (path / "pfx/drive_c").is_dir():
        prefix, compatdata = (path / "pfx").resolve(), path
    else:
        prefix, compatdata = path, None
    if not all((prefix / name).exists() for name in ("drive_c", "dosdevices", "system.reg", "user.reg")):
        raise ValueError("existing Epic prefix is incomplete at %s; launch Elite in its original manager first" % path)
    return prefix, compatdata


def clean_epic_env(environ):
    result = dict(environ)
    for key in list(result):
        upper = key.upper()
        if upper.startswith(("STEAM", "WINE", "PROTON", "UMU_", "LGDRY_")) or upper in ("GAMEID", "STORE", "RUNTIMEPATH"):
            del result[key]
    return result


def umu_runtime(proton_host, home, environ, flatpak_id):
    """Require the selected tool's installed runtime rather than letting UMU install one."""
    manifest = proton_host.parent / "toolmanifest.vdf"
    tool = section(vdf(manifest), "manifest", manifest)
    appid = tool.get("require_tool_appid")
    if appid is None:
        return None
    variants = {"1391110": "steamrt2", "1628350": "steamrt3", "4183110": "steamrt4", "4185400": "steamrt4-arm64"}
    if appid not in variants:
        raise ValueError("unsupported UMU runtime app %s in %s; select a supported installed runner" % (appid, manifest))
    data_key = "HOST_XDG_DATA_HOME" if flatpak_id else "XDG_DATA_HOME"
    data = Path(environ.get(data_key, str(home / ".local/share")))
    runtime = data / "umu" / variants[appid]
    if not all((runtime / name).exists() for name in (
            "_v2-entry-point", "VERSIONS.txt", "pressure-vessel", ".installed.ok")):
        raise ValueError("UMU runtime missing or incomplete at %s; launch Elite in its original manager first" % runtime)
    return runtime


def build_epic_plan(args, installer, forwarded, home, env_in):
    library = epic_discovery()

    if args.launcher:
        raise ValueError("Epic --launcher is not supported by this wrapper; launch Elite through its original "
                         "Heroic, Legendary, Lutris or Epic manager to preserve Epic authentication")
    if args.wine and args.proton:
        raise ValueError("choose one Epic runner override: --wine or --proton")
    try:
        selected = library.select_install(args.game_dir, home=home, environ=env_in)
    except ValueError:
        # A complete explicit binding is also the escape hatch for competing owners.
        if not (args.game_dir and args.prefix and (args.wine or args.proton)):
            raise
        selected = None
    game = selected.game_dir if selected else args.game_dir.expanduser().resolve()
    if not (game / "EliteDangerous64.exe").is_file():
        raise ValueError("EliteDangerous64.exe missing in --game-dir %s" % game)
    install_root = selected.install_root if selected else game
    configured = args.prefix or (selected.compatdata or selected.prefix if selected else None)
    if configured is None:
        raise ValueError("Epic prefix is not recorded; pass --prefix PATH for Elite's existing prefix")
    prefix, compatdata = existing_prefix(configured)
    flatpak_id = selected.flatpak_id if selected and not (args.wine or args.proton) else None
    recorded_env = dict(selected.env) if selected else {}
    kind = "wine" if args.wine else ("proton" if args.proton else selected.runner_kind)
    if kind == "proton" and compatdata is None:
        raise ValueError("selected Proton needs an existing compatdata root with pfx/; "
                         "pass that root with --prefix or select --wine for a raw Wine prefix")
    if args.wine:
        runner = args.wine.expanduser().resolve()
    elif args.proton:
        runner = proton_path(args, compatdata)
    elif selected.manager == "epic-preview":
        # The native launcher caches several runtimes. The prefix's ordinary
        # Proton record identifies the one Elite actually used most recently.
        runner = proton_path(args, compatdata)
    else:
        runner = selected.runner
    sandbox_env = {}
    if flatpak_id:
        lookup = "umu-run" if kind == "umu" else getattr(selected, "runner_lookup", None)
        if lookup:
            sandbox_env = flatpak_runtime_environment(flatpak_id, home, env_in)
            runner = flatpak_path_runner(lookup, runner, flatpak_id, home, env_in, sandbox_env)
    if kind not in ("wine", "proton", "umu") or runner is None:
        raise ValueError("Epic runner is not resolved; pass --wine PATH or --proton PATH for Elite's installed runner")
    runner = Path(runner).expanduser()
    runner_host = flatpak_host_file(runner, flatpak_id, home, env_in) if flatpak_id else runner
    if not runner.is_absolute() or not runner_host.is_file():
        raise ValueError("recorded Epic runner is missing: %s; pass --wine PATH or --proton PATH" % runner)
    env = clean_epic_env(env_in)
    env.update(recorded_env)
    env.update(sandbox_env)
    # Identity and prefix always belong to the selected Epic game, never the invoking Steam shortcut.
    for key in list(env):
        if key.upper().startswith("STEAM"):
            del env[key]
    env["WINEPREFIX"] = str(prefix)
    env["STEAM_COMPAT_INSTALL_PATH"] = str(install_root)
    runtime = None
    if kind == "wine":
        env.pop("PROTONPATH", None)
        command = [str(runner)]
    elif kind == "proton":
        steam_roots = optional_steam_roots(args, home, env_in)
        # Heroic with UMU disabled runs its selected Proton directly. A Steam
        # toolmanifest dependency describes Steam's wrapper, not this manager's
        # runinprefix route; that verb also skips Proton's Steam-dependent prefix setup.
        command = [str(runner), "runinprefix"]
        env["STEAM_COMPAT_DATA_PATH"] = str(compatdata)
        if steam_roots:
            env["STEAM_COMPAT_CLIENT_INSTALL_PATH"] = str(steam_roots[0])
    else:
        raw_proton = recorded_env.get("PROTONPATH", "")
        proton_root = Path(raw_proton)
        if not raw_proton or not proton_root.is_absolute():
            raise ValueError("UMU needs a recorded absolute installed PROTONPATH; pass --proton PATH")
        proton = proton_root / "proton"
        proton_host = flatpak_host_file(proton, flatpak_id, home, env_in) if flatpak_id else proton
        if not proton_host.is_file():
            raise ValueError("recorded UMU Proton is missing at %s; pass --proton PATH" % proton)
        runtime = umu_runtime(proton_host, home, env, flatpak_id)
        env.update(WINEPREFIX=str(compatdata or prefix), PROTONPATH=str(proton_root),
                   PROTON_VERB="runinprefix", UMU_RUNTIME_UPDATE="0", PROTONFIXES_DISABLE="1",
                   GAMEID="umu-0", STORE="egs")
        command = [str(runner)]
    game_windows = prefix_windows_path(game.resolve(), prefix)
    command += [str(installer), "--store", "epic", "--dir", game_windows] + forwarded
    if flatpak_id:
        flatpak = shutil.which("flatpak", path=env_in.get("PATH"))
        if not flatpak:
            raise ValueError("Flatpak executable missing; install the recorded manager or pass a host runner override")
        permissions = {installer.resolve(), install_root.resolve(), configured.expanduser().resolve()}
        if kind == "umu":
            permissions.add(Path(env["HOST_XDG_DATA_HOME"]) / "umu")
        for candidate in (runner, Path(recorded_env.get("PROTONPATH", "/app")), runtime):
            if candidate and candidate.is_absolute() and not inside(candidate, Path("/app")) and not inside(candidate, Path("/usr")):
                permissions.add(candidate if candidate.is_dir() else candidate.parent)
        command = [flatpak, "run", "--command=" + command[0]] + [
            "--filesystem=" + str(path) for path in sorted(permissions, key=str)
        ] + ["--env=%s=%s" % (key, env[key]) for key in sorted(env)
             if key in recorded_env or key in sandbox_env or key.startswith(("WINE", "PROTON", "UMU_", "STEAM_COMPAT_")) or key in ("GAMEID", "STORE")
        ] + [flatpak_id] + command[1:]
    return command, env, (game, compatdata or prefix, runner, runtime)


def build_plan(args, *, script=Path(__file__), home=Path.home(), environ=None):
    env_in = os.environ if environ is None else environ
    forwarded = installer_args(args)
    installer = installer_path(args, script)
    if not installer.is_file():
        raise ValueError("%s not found: %s (pass %s PATH)" % (
            "launcher" if args.launcher else "installer", installer,
            "--launcher-exe" if args.launcher else "--installer"))
    store = args.store
    if store == "auto":
        epic = epic_discovery().discover(home=home, environ=env_in)
        if args.game_dir:
            epic = [item for item in epic if item.game_dir.resolve() == args.game_dir.expanduser().resolve()]
        steam_roots = optional_steam_roots(args, home, env_in)
        steam_error = None
        try:
            find_game(args, steam_roots)
            has_steam = True
        except ValueError as exc:
            steam_error = exc
            has_steam = "multiple" in str(exc)
        if epic and has_steam:
            raise ValueError("multiple Steam/Epic installs found; pass --store steam or --store epic and --game-dir")
        if epic or args.prefix or args.wine:
            store = "epic"
        elif has_steam:
            store = "steam"
        elif steam_roots:
            raise steam_error
        else:
            raise ValueError("no valid Elite Steam or Epic install found; pass --store epic --game-dir PATH "
                             "--prefix PATH and --wine PATH (or --proton PATH) for an unowned install")
    if store == "epic":
        return build_epic_plan(args, installer, forwarded, home, env_in)
    if args.wine:
        raise ValueError("--wine is an Epic option; use --store epic")
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
        selected = flatpak_deployment(flatpak_id, home, env_in, strict=True)
        sandbox_env = flatpak_runtime_environment(flatpak_id, home, env_in, selected)
        host_file = lambda path: flatpak_host_file(path, flatpak_id, home, env_in, selected)
    proton = proton_path(args, compatdata, host_file)
    runtime = runtime_path(proton, [steam_root], host_file)
    game_windows = windows_path(game, compatdata)
    command = ([str(runtime), "--verb=run", "--"] if runtime else [])
    command += [str(proton), "runinprefix", str(installer), "--store", "steam", "--dir", game_windows] + forwarded
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
        permissions = {installer.resolve(), steam_root, compatdata, *libraries(steam_root)}
        if not inside(proton, Path("/app")) and not inside(proton, Path("/usr")):
            permissions.add(proton.parent)
        env.update(sandbox_env)
        command = [flatpak, "run", selector, "--arch=" + relative.parts[0],
                   "--branch=" + relative.parts[1], "--command=" + command[0]] + [
            "--filesystem=" + str(path) for path in sorted(permissions, key=str)
        ] + ["--env=%s=%s" % (key, env[key]) for key in sorted(env)
             if key in sandbox_env or key in ("SteamAppId", "SteamGameId") or key.startswith("STEAM_COMPAT_")
        ] + [flatpak_id] + command[1:]
    return command, env, (game, compatdata, proton, runtime)


def run(args, *, script=Path(__file__), home=Path.home(), environ=None,
        launch=subprocess.run, platform=sys.platform, geteuid=getattr(os, "geteuid", lambda: 1)):
    if platform != "linux" and not args.dry_run:
        raise ValueError("run this wrapper on Linux; use edvr-installer.exe on Windows")
    command, env, (game, compatdata, proton, runtime) = build_plan(
        args, script=script, home=home, environ=environ)
    if args.dry_run:
        print("EDVR resolved launch plan (nothing executed or written):")
        print("  program: %s" % ("launcher" if args.launcher else "installer"))
        print("  game: %s" % game)
        print("  prefix: %s" % compatdata)
        print("  runner: %s" % proton)
        print("  runtime: %s" % (runtime or "none"))
        for key in ("SteamAppId", "SteamGameId", "STEAM_COMPAT_APP_ID",
                    "STEAM_COMPAT_CLIENT_INSTALL_PATH", "STEAM_COMPAT_DATA_PATH",
                    "STEAM_COMPAT_INSTALL_PATH", "STEAM_COMPAT_LIBRARY_PATHS"):
            if key in env:
                print("  %s=%s" % (key, shlex.quote(env[key])))
        for key in ("WINEPREFIX", "PROTONPATH", "PROTON_VERB", "UMU_RUNTIME_UPDATE"):
            if key in env:
                print("  %s=%s" % (key, shlex.quote(env[key])))
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
    from contextlib import redirect_stdout

    # Default exe resolution mirrors installer_path: beside the script, else <repo>/build.
    launcher_args_ns = parse("--launcher")
    assert launcher_path(launcher_args_ns, script) == script.resolve().parents[1] / "build" / "ed-up-launcher.exe"
    exe = script.parent / "ed-up-launcher.exe"
    exe.write_bytes(b"exe")
    assert launcher_path(launcher_args_ns, script) == exe.resolve()
    override = root / "elsewhere" / "my-launcher.exe"
    assert launcher_path(parse("--launcher", "--launcher-exe", str(override)), script) == override.resolve()

    # Passthrough: split at the first --, kept verbatim, later -- stay in it.
    ns = parse("--launcher", "--dry-run", "--", "--direct-dry-run", "--vr", "--", "--channel", "rc")
    assert ns.dry_run and ns.passthrough == ["--direct-dry-run", "--vr", "--", "--channel", "rc"]
    assert parse("--launcher").passthrough is None
    assert launcher_args(parse("--launcher")) == []

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
    assert "edvr-installer.exe" not in shlex.join(command) and "--install" not in command
    assert "program: launcher" in buf.getvalue() and shlex.join(command) in buf.getvalue()
    assert process_env["SteamAppId"] == APP_ID and "WINEPREFIX" not in process_env
    bare = build_plan(parse("--launcher", "--dry-run"), script=script, home=home, environ=env)[0]
    assert bare[-5:-1] == [str(exe.resolve()), "--store", "steam", "--dir"] and bare[-1] != "--install"
    # A real launcher run uses the same command and returns the launcher's exit code.
    assert run(parse("--launcher", "--", "--channel", "rc"), script=script, home=home, environ=env,
               launch=launch, platform="linux", geteuid=lambda: 1000) == 23
    assert calls[-1][0][-2:] == ["--channel", "rc"] and str(exe.resolve()) in calls[-1][0]
    calls.pop()
    # A missing launcher names --launcher-exe, not --installer.
    exe.unlink()
    try:
        build_plan(parse("--launcher", "--dry-run"), script=script, home=home, environ=env)
        raise AssertionError("missing launcher accepted")
    except ValueError as exc:
        assert "--launcher-exe" in str(exc) and "--installer" not in str(exc)
    exe.write_bytes(b"exe")

    # Installer-only options are refused in launcher mode.
    for refused in (("--install",), ("--repair",), ("--uninstall",), ("--collect-logs",), ("--preview",),
                    ("--scope", "flat"), ("--replace-settings",), ("--remove-settings",),
                    ("--installer", str(exe))):
        try:
            launcher_args(parse("--launcher", *refused))
            raise AssertionError("launcher accepted %s" % refused[0])
        except ValueError as exc:
            assert refused[0] in str(exc) and "--launcher" in str(exc)
    # Passthrough and --launcher-exe are refused without --launcher.
    for stray in (("--", "--vr"), ("--install", "--", "--vr"), ("--launcher-exe", str(exe))):
        try:
            installer_args(parse(*stray))
            raise AssertionError("accepted %s without --launcher" % stray[0])
        except ValueError as exc:
            assert "--launcher" in str(exc)
    # The installer path is unchanged by all of this.
    assert installer_args(parse("--install")) == ["--install"]


def epic_self_test():
    import io
    import json
    from contextlib import redirect_stdout
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
        exe = script.parent / "edvr-installer.exe"
        exe.write_bytes(b"installer")
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

        def save_settings(base, binary, kind="wine", umu=False, path=settings):
            path.write_text(json.dumps({"ELITE": {"winePrefix": str(base), "wineVersion": {
                "bin": str(binary), "name": "selected", "type": kind}, "disableUMU": not umu}}), encoding="utf-8")

        save_settings(prefix, wine)
        env = {"PATH": "/mock", "HOME": str(home), "SteamAppId": "359320", "SteamGameId": "wrong",
               "STEAM_COMPAT_DATA_PATH": "/wrong", "WINEPREFIX": "/wrong", "PROTONPATH": "/wrong",
               "EPIC_AUTH_TOKEN": "secret-not-for-output"}
        expected_windows = "G:\\Products\\elite-dangerous-odyssey-64"
        expected = [str(wine), str(exe), "--store", "epic", "--dir", expected_windows,
                    "--install", "--scope", "flat"]
        args = parse("--dry-run", "--install", "--scope", "flat")
        command, launch_env, _ = build_plan(args, script=script, home=home, environ=env)
        assert command == expected
        assert launch_env["WINEPREFIX"] == str(prefix)
        assert "PROTONPATH" not in launch_env and "SteamAppId" not in launch_env
        assert "SteamGameId" not in launch_env and "STEAM_COMPAT_DATA_PATH" not in launch_env
        before = {str(p): (p.read_bytes() if p.is_file() else None) for p in root.rglob("*")}
        calls = []

        def launch(command, **kwargs):
            calls.append((command, kwargs))
            return subprocess.CompletedProcess(command, 17)

        output = io.StringIO()
        with redirect_stdout(output):
            assert run(args, script=script, home=home, environ=env, launch=launch,
                       platform="linux", geteuid=lambda: 1000) == 0
        assert not calls and before == {str(p): (p.read_bytes() if p.is_file() else None) for p in root.rglob("*")}
        assert "secret-not-for-output" not in output.getvalue() and shlex.join(expected) in output.getvalue()
        assert run(parse("--store", "epic", "--install", "--scope", "flat"), script=script,
                   home=home, environ=env, launch=launch, platform="linux", geteuid=lambda: 1000) == 17
        assert calls[0][0] == expected and calls[0][1]["env"]["WINEPREFIX"] == str(prefix)

        def refuses(words, reason):
            try:
                build_plan(parse(*words), script=script, home=home, environ=env)
                raise AssertionError("accepted invalid Epic plan: %r" % (words,))
            except ValueError as exc:
                assert reason in str(exc), str(exc)

        refuses(("--store", "epic", "--launcher"), "original")
        refuses(("--launcher", "--", "--store=steam"), "override")
        refuses(("--launcher", "--", "--dir", "C:\\other"), "override")
        refuses(("--store", "epic", "--wine", str(root / "missing")), "missing")
        (prefix / "system.reg").unlink()
        refuses(("--store", "epic"), "incomplete")
        (prefix / "system.reg").write_bytes(b"registry")

        # Two recorded owners of one product need a choice; a full manual binding is that choice.
        competing = root / "competing-legendary"
        competing.mkdir()
        other_prefix = home / "Other existing prefix"
        (other_prefix / "drive_c").mkdir(parents=True)
        (other_prefix / "dosdevices").mkdir()
        (other_prefix / "dosdevices/g:").symlink_to(install, target_is_directory=True)
        for name in ("system.reg", "user.reg"):
            (other_prefix / name).write_bytes(b"registry")
        competing_installed = competing / "installed.json"
        competing_installed.write_text(json.dumps({"ELITE": {
            "title": "Elite Dangerous", "install_path": str(install)}}), encoding="utf-8")
        (competing / "config.ini").write_text(
            "[ELITE]\nwine_prefix=" + str(other_prefix) + "\nwine_executable=" + str(wine)
            + "\n[ELITE.env]\nWINEDEBUG=owner-only\n", encoding="utf-8")
        env["LEGENDARY_CONFIG_PATH"] = str(competing)
        incoming_prefix = env.pop("WINEPREFIX")
        refuses(("--store", "epic", "--game-dir", str(game)), "configurations match")
        command, launch_env, _ = build_plan(parse("--store", "epic", "--game-dir", str(game),
            "--prefix", str(prefix), "--wine", str(wine)), script=script, home=home, environ=env)
        assert command == expected[:-3] and launch_env["WINEPREFIX"] == str(prefix)
        assert "WINEDEBUG" not in launch_env
        env["WINEPREFIX"] = incoming_prefix
        env.pop("LEGENDARY_CONFIG_PATH")
        competing_installed.unlink()

        # A real Steam install and Epic install require a store selection; game-dir disambiguates.
        steam = home / ".local/share/Steam"
        steam_game = steam / "steamapps/common/Elite Dangerous" / PRODUCT
        steam_game.mkdir(parents=True)
        (steam_game / "EliteDangerous64.exe").write_bytes(b"game")
        (steam / "steamapps/libraryfolders.vdf").write_text('"libraryfolders" {}', encoding="utf-8")
        steam_manifest = steam / "steamapps/appmanifest_359320.acf"
        steam_manifest.write_text('"AppState" { "appid" "359320" "installdir" "Elite Dangerous" }', encoding="utf-8")
        refuses(("--dry-run",), "multiple Steam/Epic")
        assert build_plan(parse("--game-dir", str(game)), script=script, home=home, environ=env)[0][:2] == expected[:2]
        steam_manifest.unlink()
        (steam / "steamapps/libraryfolders.vdf").unlink()

        # The configured Proton root remains distinct from its actual Wine prefix.
        compat = home / "Existing Proton root"
        compat.mkdir()
        prefix.rename(compat / "pfx")
        prefix = compat / "pfx"
        proton = home / "Selected Proton/proton"
        proton.parent.mkdir()
        proton.write_bytes(b"runner")
        toolmanifest = proton.parent / "toolmanifest.vdf"
        toolmanifest.write_text('"manifest" { "require_tool_appid" "1628350" }', encoding="utf-8")
        save_settings(compat, proton, "proton")
        command, launch_env, _ = build_plan(parse("--store", "epic"), script=script, home=home, environ=env)
        assert command == [str(proton), "runinprefix", str(exe), "--store", "epic", "--dir", expected_windows]
        assert launch_env["STEAM_COMPAT_DATA_PATH"] == str(compat)
        assert launch_env["WINEPREFIX"] == str(prefix)
        assert "STEAM_COMPAT_CLIENT_INSTALL_PATH" not in launch_env

        # UMU uses the manager's pinned Proton and existing runtime, with no automatic selection.
        umu = config / "tools/runtimes/umu/umu_run.py"
        umu.parent.mkdir(parents=True)
        umu.write_bytes(b"runner")
        save_settings(compat, proton, "proton", umu=True)
        toolmanifest.write_text('"manifest" { "require_tool_appid" "1628350" }', encoding="utf-8")
        refuses(("--store", "epic"), "UMU runtime missing")
        runtime = home / ".local/share/umu/steamrt3"
        (runtime / "pressure-vessel").mkdir(parents=True)
        for name in ("_v2-entry-point", "VERSIONS.txt", ".installed.ok"):
            (runtime / name).write_bytes(b"runtime")
        command, launch_env, _ = build_plan(parse("--store", "epic"), script=script, home=home, environ=env)
        assert command == [str(umu), str(exe), "--store", "epic", "--dir", expected_windows]
        assert {key: launch_env[key] for key in ("WINEPREFIX", "PROTONPATH", "PROTON_VERB", "UMU_RUNTIME_UPDATE", "GAMEID", "STORE")} == {
            "WINEPREFIX": str(compat), "PROTONPATH": str(proton.parent), "PROTON_VERB": "runinprefix",
            "UMU_RUNTIME_UPDATE": "0", "GAMEID": "umu-0", "STORE": "egs"}
        assert "SteamAppId" not in launch_env

        # A fully explicit unowned Wine prefix does not need any launcher metadata or Steam.
        installed.unlink()
        command, _, _ = build_plan(parse("--store", "epic", "--game-dir", str(game), "--prefix", str(prefix),
                                        "--wine", str(wine)), script=script, home=home, environ=env)
        assert command == expected[:-3]

        # A Flatpak-only Wine binary is checked in its installed deployment and executed inside it.
        flat_id = "com.heroicgameslauncher.hgl"
        flat_config = home / ".var/app" / flat_id / "config/heroic"
        flat_installed = flat_config / "legendaryConfig/legendary/installed.json"
        flat_installed.parent.mkdir(parents=True)
        flat_installed.write_text(json.dumps({"ELITE": {"title": "Elite Dangerous", "install_path": str(install)}}), encoding="utf-8")
        flat_settings = flat_config / "GamesConfig/ELITE.json"
        flat_settings.parent.mkdir()
        save_settings(prefix, Path("/app/bin/wine"), path=flat_settings)
        deployed_wine = home / ".local/share/flatpak/app" / flat_id / "current/active/files/bin/wine"
        deployed_wine.parent.mkdir(parents=True)
        deployed_wine.write_bytes(b"runner")
        fake_flatpak = root / "bin/flatpak"
        fake_flatpak.parent.mkdir()
        fake_flatpak.write_bytes(b"runner")
        fake_flatpak.chmod(0o700)
        env["PATH"] = str(fake_flatpak.parent)
        command, launch_env, _ = build_plan(parse("--store", "epic"), script=script, home=home, environ=env)
        assert command[:3] == [str(fake_flatpak), "run", "--command=/app/bin/wine"]
        assert command[-6:] == [flat_id, str(exe), "--store", "epic", "--dir", expected_windows]
        assert "--env=WINEPREFIX=" + str(prefix) in command
        assert "--filesystem=" + str(exe) in command and "--filesystem=" + str(install) in command
        assert "secret-not-for-output" not in shlex.join(command)
        deployed_wine.unlink()
        refuses(("--store", "epic"), "installed Flatpak")

        # Flatpak exports the host XDG data directory as HOST_XDG_DATA_HOME.
        # Heroic searches its sandbox PATH before the bundled UMU fallback.
        shared_data = home / "Custom Shared Data"
        flat_deployment = shared_data / "flatpak/app" / flat_id / "current/active"
        flat_deployment.mkdir(parents=True)
        (flat_deployment / "metadata").write_text(
            "[Environment]\nPATH=/app/libexec/manager:/app/bin:/usr/bin\n", encoding="utf-8")
        sandbox_umu = flat_deployment / "files/libexec/manager/umu-run"
        sandbox_umu.parent.mkdir(parents=True)
        sandbox_umu.write_bytes(b"sandbox runner")
        app_bin_umu = flat_deployment / "files/bin/umu-run"
        app_bin_umu.parent.mkdir(parents=True)
        app_bin_umu.write_bytes(b"secondary sandbox runner")
        host_umu = fake_flatpak.parent / "umu-run"
        host_umu.write_bytes(b"host runner must not run inside Flatpak")
        host_umu.chmod(0o700)
        bundled_umu = flat_config / "tools/runtimes/umu/umu_run.py"
        bundled_umu.parent.mkdir(parents=True)
        bundled_umu.write_bytes(b"fallback runner")
        save_settings(compat, proton, "proton", umu=True, path=flat_settings)
        flat_runtime = home / "Custom Shared Data/umu/steamrt3"
        (flat_runtime / "pressure-vessel").mkdir(parents=True)
        for name in ("_v2-entry-point", "VERSIONS.txt", ".installed.ok"):
            (flat_runtime / name).write_bytes(b"runtime")
        flat_env = dict(env, XDG_DATA_HOME=str(shared_data))
        flat_env.pop("HOST_XDG_DATA_HOME", None)
        command, launch_env, details = build_plan(parse("--store", "epic"), script=script, home=home, environ=flat_env)
        assert command[:3] == [str(fake_flatpak), "run", "--command=/app/libexec/manager/umu-run"]
        assert details[3] == flat_runtime
        assert launch_env["HOST_XDG_DATA_HOME"] == str(shared_data)
        assert "--env=HOST_XDG_DATA_HOME=" + str(shared_data) in command
        assert "--env=PATH=/app/libexec/manager:/app/bin:/usr/bin" in command
        assert "--env=PROTONPATH=" + str(proton.parent) in command
        assert "--env=PROTON_VERB=runinprefix" in command and "--env=UMU_RUNTIME_UPDATE=0" in command
        assert str(host_umu) not in command and str(bundled_umu) not in command

        # Persistent overrides select the same UMU that Heroic finds, with
        # per-app values overriding global values without exporting secrets.
        overrides = shared_data / "flatpak/overrides"
        overrides.mkdir()
        (overrides / "global").write_text("[Environment]\nPATH=/app/bin\n", encoding="utf-8")
        custom_umu = flat_deployment / "files/custom runneré/umu-run"
        custom_umu.parent.mkdir()
        custom_umu.write_bytes(b"selected overridden runner")
        for indentation in (" \t ", "\r\f "):
            (overrides / flat_id).write_text(
                "[Environment]\nTOKEN=must-not-be-exported\n" + indentation
                + "PATH=/app/custom\\srunneré:/app/bin\n", encoding="utf-8")
            before_plan = {str(path.relative_to(root)): (path.lstat().st_mtime_ns, path.lstat().st_size)
                           for path in root.rglob("*")}
            command, launch_env, details = build_plan(parse("--store", "epic"), script=script, home=home, environ=flat_env)
            assert before_plan == {str(path.relative_to(root)): (path.lstat().st_mtime_ns, path.lstat().st_size)
                                   for path in root.rglob("*")}
            assert command[:3] == [str(fake_flatpak), "run", "--command=/app/custom runneré/umu-run"]
            assert "--env=PATH=/app/custom runneré:/app/bin" in command
            assert "must-not-be-exported" not in shlex.join(command) and "TOKEN" not in launch_env
        for unsupported in ("\u00a0PATH=/app/bin", "PATH\u00a0=/app/bin", "PATH=\u00a0/app/bin",
                            "\vPATH=/app/bin", "NOTE=fixture\x00value", "  keyless continuation"):
            (overrides / flat_id).write_text(
                "[Environment]\nTOKEN=must-not-be-exported\n" + unsupported + "\n", encoding="utf-8")
            try:
                build_plan(parse("--store", "epic"), script=script, home=home, environ=flat_env)
                raise AssertionError("unsupported Flatpak syntax accepted: %r" % unsupported)
            except ValueError as exc:
                assert "override" in str(exc)
        (overrides / flat_id).write_text(
            "[Environment]\nPATH=/app/bin\n[Context]\nunset-environment=PATH;\n", encoding="utf-8")
        try:
            build_plan(parse("--store", "epic"), script=script, home=home, environ=flat_env)
            raise AssertionError("unset Flatpak PATH accepted")
        except ValueError as exc:
            assert "PATH" in str(exc) and "override" in str(exc)
        (overrides / flat_id).unlink()
        (overrides / "global").unlink()

        # System overrides apply only to system deployments. Within them the
        # effective order is runtime, app, system-global, system-app,
        # user-global, user-app; a later set also replaces an earlier unset.
        system_installation = root / "system-flatpak"
        system_overrides = system_installation / "overrides"
        system_overrides.mkdir(parents=True)
        (system_overrides / "global").write_text("[Environment]\nPATH=/app/unselected\n", encoding="utf-8")
        system_env = dict(flat_env, FLATPAK_SYSTEM_DIR=str(system_installation),
                          FLATPAK_USER_DIR=str(shared_data / "flatpak"))
        command, _, _ = build_plan(parse("--store", "epic"), script=script, home=home, environ=system_env)
        assert command[:3] == [str(fake_flatpak), "run", "--command=/app/libexec/manager/umu-run"]
        system_deployment = system_installation / "app" / flat_id / "current/active"
        system_deployment.parent.mkdir(parents=True)
        flat_deployment.rename(system_deployment)
        for name in ("runtime", "system-global", "system-app", "user-global", "user-app"):
            binary = system_deployment / "files" / name / "umu-run"
            binary.parent.mkdir()
            binary.write_bytes(b"override runner")
        runtime_deployment = system_installation / "runtime/org.example.Platform/x86_64/1/active"
        (runtime_deployment / "files").mkdir(parents=True)
        (runtime_deployment / "metadata").write_text("[Environment]\nPATH=/app/runtime\n", encoding="utf-8")
        (system_deployment / "metadata").write_text(
            "[Application]\nruntime=org.example.Platform/x86_64/1\n", encoding="utf-8")
        (system_overrides / "global").unlink()
        command, _, _ = build_plan(parse("--store", "epic"), script=script, home=home, environ=system_env)
        assert command[:3] == [str(fake_flatpak), "run", "--command=/app/runtime/umu-run"]
        user_runtime = shared_data / "flatpak/runtime/org.example.Platform/x86_64/1/active"
        user_runtime.parent.mkdir(parents=True)
        runtime_deployment.rename(user_runtime)
        (runtime_deployment / "files").mkdir(parents=True)
        (runtime_deployment / "metadata").write_text("[Environment]\nPATH=/app/system-global\n", encoding="utf-8")
        command, _, _ = build_plan(parse("--store", "epic"), script=script, home=home, environ=system_env)
        assert command[:3] == [str(fake_flatpak), "run", "--command=/app/runtime/umu-run"]
        user_metadata = user_runtime / "metadata"
        recorded_metadata = user_metadata.read_bytes()
        user_metadata.unlink()
        try:
            build_plan(parse("--store", "epic"), script=script, home=home, environ=system_env)
            raise AssertionError("corrupt user runtime silently selected a system runtime")
        except ValueError as exc:
            assert "runtime metadata is missing" in str(exc), str(exc)
        user_metadata.write_bytes(recorded_metadata)
        for path, directory in ((system_overrides / "global", "system-global"),
                                (system_overrides / flat_id, "system-app"),
                                (overrides / "global", "user-global"), (overrides / flat_id, "user-app")):
            path.write_text("[Environment]\nPATH=/app/" + directory + "\n", encoding="utf-8")
            command, _, _ = build_plan(parse("--store", "epic"), script=script, home=home, environ=system_env)
            assert command[:3] == [str(fake_flatpak), "run", "--command=/app/" + directory + "/umu-run"]
        (overrides / "global").write_text("[Context]\nunset-environment=PATH;HOST_XDG_DATA_HOME;\n", encoding="utf-8")
        (overrides / flat_id).write_text(
            "[Environment]\nPATH=/app/user-app\nHOST_XDG_DATA_HOME=" + str(shared_data) + "\n", encoding="utf-8")
        command, _, details = build_plan(parse("--store", "epic"), script=script, home=home, environ=system_env)
        assert command[:3] == [str(fake_flatpak), "run", "--command=/app/user-app/umu-run"]
        assert details[3] == flat_runtime
        (overrides / flat_id).write_text("[Environment]\nPATH=/app/bad\\q\n", encoding="utf-8")
        try:
            build_plan(parse("--store", "epic"), script=script, home=home, environ=system_env)
            raise AssertionError("invalid Flatpak escape accepted")
        except ValueError as exc:
            assert "escape" in str(exc) and "override" in str(exc)
        (overrides / flat_id).unlink()
        (overrides / "global").unlink()

        # Lutris's System Wine means `wine` on the sandbox PATH; a custom
        # absolute /usr/bin/wine selection remains that exact executable.
        flat_installed.unlink()
        manifests = prefix / "drive_c/ProgramData/Epic/EpicGamesLauncher/Data/Manifests"
        manifests.mkdir(parents=True)
        (manifests / "elite.item").write_text(json.dumps({"AppName": "ELITE", "DisplayName": "Elite Dangerous",
                                                        "InstallLocation": "G:\\"}), encoding="utf-8")
        lutris_id = "net.lutris.Lutris"
        flat_lutris = home / ".var/app" / lutris_id / "config/lutris/games/epic.yml"
        flat_lutris.parent.mkdir(parents=True)
        lutris_game = "game:\n  prefix: " + json.dumps(str(prefix)) + "\n"
        flat_lutris.write_text(lutris_game + "wine:\n  version: system\n", encoding="utf-8")
        lutris_deployment = home / ".local/share/flatpak/app" / lutris_id / "current/active"
        app_wine = lutris_deployment / "files/bin/wine"
        app_wine.parent.mkdir(parents=True)
        app_wine.write_bytes(b"selected system wine")
        (lutris_deployment / "metadata").write_text(
            "[Application]\nruntime=org.gnome.Platform/x86_64/51\n[Environment]\nPATH=/app/bin:/usr/bin\n", encoding="utf-8")
        runtime_wine = home / ".local/share/flatpak/runtime/org.gnome.Platform/x86_64/51/active/files/bin/wine"
        runtime_wine.parent.mkdir(parents=True)
        runtime_wine.write_bytes(b"unselected runtime wine")
        (runtime_wine.parent.parent.parent / "metadata").write_text("[Runtime]\n", encoding="utf-8")
        command, _, _ = build_plan(parse("--store", "epic"), script=script, home=home, environ=env)
        assert command[:3] == [str(fake_flatpak), "run", "--command=/app/bin/wine"]
        # System Wine follows a persistent user-global PATH before the
        # deployment default; an explicit custom executable stays exact.
        lutris_overrides = home / ".local/share/flatpak/overrides"
        lutris_overrides.mkdir()
        (lutris_overrides / "global").write_text("[Environment]\nPATH=/usr/bin:/app/bin\n", encoding="utf-8")
        command, _, _ = build_plan(parse("--store", "epic"), script=script, home=home, environ=env)
        assert command[:3] == [str(fake_flatpak), "run", "--command=/usr/bin/wine"]
        (lutris_overrides / "global").write_text("[Environment]\nPATH=/app/missing\n", encoding="utf-8")
        refuses(("--store", "epic"), "effective PATH")
        (lutris_overrides / "global").unlink()
        flat_lutris.write_text(lutris_game + "wine:\n  version: custom\n  custom_wine_path: /usr/bin/wine\n", encoding="utf-8")
        command, _, _ = build_plan(parse("--store", "epic"), script=script, home=home, environ=env)
        assert command[:3] == [str(fake_flatpak), "run", "--command=/usr/bin/wine"]
        flat_lutris.unlink()

        # Standalone Legendary and Lutris metadata drive the same bound installer route.
        legendary = home / ".config/legendary"
        legendary.mkdir()
        legendary_installed = legendary / "installed.json"
        legendary_installed.write_text(json.dumps({"ELITE": {
            "title": "Elite Dangerous", "install_path": str(install)}}), encoding="utf-8")
        (legendary / "config.ini").write_text(
            "[ELITE]\nwine_prefix=" + str(prefix) + "\nwine_executable=" + str(wine)
            + "\n[ELITE.env]\nWINEDEBUG=-all\n", encoding="utf-8")
        manager_env = dict(env)
        manager_env.pop("WINEPREFIX")
        command, launch_env, _ = build_plan(parse("--store", "epic"), script=script,
                                           home=home, environ=manager_env)
        assert command == expected[:-3] and launch_env["WINEDEBUG"] == "-all"
        legendary_installed.unlink()
        lutris = home / ".config/lutris"
        (lutris / "games").mkdir(parents=True)
        (lutris / "games/epic.yml").write_text(
            "game:\n  prefix: " + json.dumps(str(prefix)) + "\n"
            "wine:\n  version: custom\n  custom_wine_path: " + json.dumps(str(wine)) + "\n", encoding="utf-8")
        manifests = prefix / "drive_c/ProgramData/Epic/EpicGamesLauncher/Data/Manifests"
        manifests.mkdir(parents=True, exist_ok=True)
        (manifests / "elite.item").write_text(json.dumps({"AppName": "ELITE", "DisplayName": "Elite Dangerous",
                                                        "InstallLocation": "G:\\"}), encoding="utf-8")
        command, launch_env, _ = build_plan(parse("--store", "epic"), script=script,
                                           home=home, environ=manager_env)
        assert command == expected[:-3] and launch_env["WINEPREFIX"] == str(prefix)


def preview_self_test():
    """Native Epic uses the game's last-run Proton, not another cached runtime."""
    import io
    import json
    from contextlib import redirect_stdout
    from tempfile import TemporaryDirectory

    if os.name == "nt":
        return
    with TemporaryDirectory(dir="/dev/shm" if Path("/dev/shm").is_dir() else None) as tmp:
        root = Path(tmp).resolve()
        home = root / "home"
        data = home / "Custom data é"
        native = data / "com.epicgames.eglv2"
        install = home / "Games é/Custom name"
        game = install / PRODUCT
        game.mkdir(parents=True)
        (game / "EliteDangerous64.exe").write_bytes(b"game")
        compat = native / "compat/compatdata/ELITE"
        prefix = compat / "pfx"
        (prefix / "drive_c").mkdir(parents=True)
        (prefix / "dosdevices").mkdir()
        (prefix / "dosdevices/g:").symlink_to(install, target_is_directory=True)
        for name in ("system.reg", "user.reg"):
            (prefix / name).write_bytes(b"registry")
        proton = native / "compat/runtime/GE-Proton10-34/proton"
        other = native / "compat/runtime/GE-Proton10-35/proton"
        for runner in (proton, other):
            runner.parent.mkdir(parents=True)
            runner.write_bytes(b"runner")
        (compat / "config_info").write_text("\n".join((
            "GE-Proton10-34", str(proton.parent / "files/share/fonts"),
            str(proton.parent / "files/lib"), "", "0", "0", "0",
            str(proton.parent / "files/share/default_pfx"))), encoding="utf-8")
        (native / "installations.jsonl").write_text(
            json.dumps({"version": 1, "writtenAt": "2026-10-16T08:40:42Z", "entries": 0}) + "\n" +
            json.dumps({"installationId": "one", "complete": True, "dir": str(install),
                        "platform": "Windows", "artifactId": "ELITE", "itemId": "elite"}) + "\n",
            encoding="utf-8")
        script = root / "install_proton.py"
        installer = root / "edvr-installer.exe"
        installer.write_bytes(b"installer")
        env = {"HOME": str(home), "XDG_DATA_HOME": str(data), "PATH": "/missing",
               "SteamAppId": "wrong", "STEAM_COMPAT_DATA_PATH": "/wrong",
               "EPIC_AUTH_TOKEN": "must-not-appear"}
        args = parse_argv(["--dry-run"])
        command, resolved, _ = build_plan(args, script=script, home=home, environ=env)
        assert command == [str(proton), "runinprefix", str(installer), "--store", "epic",
                           "--dir", "G:\\Products\\elite-dangerous-odyssey-64"]
        assert resolved["STEAM_COMPAT_DATA_PATH"] == str(compat)
        assert resolved["WINEPREFIX"] == str(prefix)
        assert "SteamAppId" not in resolved and "STEAM_COMPAT_CLIENT_INSTALL_PATH" not in resolved
        before = {str(p): (p.read_bytes() if p.is_file() else None) for p in root.rglob("*")}
        output = io.StringIO()

        def never_launch(*args, **kwargs):
            raise AssertionError("dry-run started the native Epic runner")

        with redirect_stdout(output):
            assert run(args, script=script, home=home, environ=env, launch=never_launch,
                       platform="linux", geteuid=lambda: 1000) == 0
        assert "must-not-appear" not in output.getvalue()
        assert before == {str(p): (p.read_bytes() if p.is_file() else None) for p in root.rglob("*")}
        try:
            build_plan(parse_argv(["--store", "epic", "--prefix", str(prefix)]),
                       script=script, home=home, environ=env)
            raise AssertionError("native Epic accepted a raw pfx without its Proton root")
        except ValueError as exc:
            assert "compatdata root with pfx/" in str(exc), str(exc)
        (compat / "config_info").unlink()
        try:
            build_plan(args, script=script, home=home, environ=env)
            raise AssertionError("native Epic guessed an unrecorded Proton runtime")
        except ValueError as exc:
            assert "Proton record missing" in str(exc), str(exc)
        override = parse_argv(["--store", "epic", "--proton", str(other)])
        assert build_plan(override, script=script, home=home, environ=env)[0][:2] == [str(other), "runinprefix"]


def steam_context_self_test(only=None):
    """Exercise real Linux prefixes/deployments without starting any installed binary."""
    if os.name == "nt":
        return
    import io
    from contextlib import redirect_stdout
    from tempfile import TemporaryDirectory

    parse = lambda *words: parse_argv(list(words))
    failures = []
    for case in ("prefix", "store", "flatpak"):
        if only is not None and case != only:
            continue
        with TemporaryDirectory() as tmp:
            root = Path(tmp).resolve()
            home = root / "home"
            steam = home / (".var/app/com.valvesoftware.Steam/.local/share/Steam" if case == "flatpak"
                            else ".local/share/Steam")
            script = root / "tools/install_proton.py"
            script.parent.mkdir()
            for name in ("edvr-installer.exe", "ed-up-launcher.exe"):
                (script.parent / name).write_bytes(b"fixture only")
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
            env = {"STEAM_DIR": str(steam), "PATH": str(root / "bin"),
                   "FLATPAK_SYSTEM_DIR": str(root / "system-flatpak")}

            def plan(*words, environment=None):
                return build_plan(parse("--store", "steam", *words), script=script, home=home,
                                  environ=env if environment is None else environment)

            def refuses(reason, *words):
                try:
                    plan(*words)
                    raise AssertionError("accepted invalid Steam context: " + reason)
                except ValueError as exc:
                    assert reason in str(exc), str(exc)

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
                    for words in ((), ("--install", "--scope", "flat"), ("--launcher",)):
                        output = dry_run(*words)
                        command, _, _ = plan(*words)
                        start = command.index("runinprefix")
                        expected_exe = script.parent / ("ed-up-launcher.exe" if "--launcher" in words
                                                         else "edvr-installer.exe")
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
                    for words in ((), ("--install", "--scope", "flat"), ("--launcher",)):
                        output = dry_run(*words)
                        command, resolved, _ = plan(*words)
                        assert shlex.join(command) in output
                        assert command[:3] == [str(flatpak), "run", "--user"]
                        assert "--arch=x86_64" in command and "--branch=stable" in command
                        assert "--command=" + str(runtime / "_v2-entry-point") in command
                        start = command.index(flat_id)
                        assert command[start + 1:start + 6] == ["--verb=run", "--", str(recorded / "proton"),
                                                               "runinprefix", str(script.parent / (
                                                               "ed-up-launcher.exe" if "--launcher" in words else "edvr-installer.exe"))]
                        assert "--env=STEAM_COMPAT_CLIENT_INSTALL_PATH=" + str(steam) in command
                        assert "--env=STEAM_COMPAT_DATA_PATH=" + str(compat) in command
                        assert "--filesystem=" + str(library) in command
                        assert resolved["SteamAppId"] == APP_ID
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
    assert parse("--store", "epic", "--prefix", "/existing", "--wine", "/selected/wine").store == "epic"
    assert installer_args(parse("--preview", "--scope", "flat", "--replace-settings")) == [
        "--install", "--dry-run", "--scope", "flat", "--replace-settings"]
    assert installer_args(parse()) == []
    assert installer_args(parse("--uninstall", "--remove-settings")) == ["--uninstall", "--remove-settings"]
    for invalid in (("--dry-run", "--self-test"), ("--install", "--preview")):
        with redirect_stderr(io.StringIO()):
            try:
                parse(*invalid)
                raise AssertionError("accepted mutually exclusive arguments")
            except SystemExit as exc:
                assert exc.code == 2
    for invalid in (("--scope", "vr"), ("--remove-settings",), ("--preview", "--remove-settings")):
        try:
            installer_args(parse(*invalid))
            raise AssertionError("accepted invalid action flags")
        except ValueError:
            pass

    with TemporaryDirectory() as tmp:
        # Windows TEMP may use an 8.3 alias; match build_plan()'s resolved paths.
        root = Path(tmp).resolve()
        home = root / "home"
        steam = home / ".local/share/Steam"
        other = home / "other Steam"
        script = root / "tools/install_proton.py"
        script.parent.mkdir()
        exe = script.parent / "edvr-installer.exe"
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
        args = parse("--dry-run", "--preview", "--scope", "flat")
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
            assert command[-4:] == ["--install", "--dry-run", "--scope", "flat"]
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
            assert run(parse("--preview"), script=script, home=home, environ=env,
                       launch=launch, platform="linux", geteuid=lambda: 1000) == 23
            assert calls[-1][0][-2:] == ["--install", "--dry-run"]
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
                run(parse("--preview"), script=script, home=home, environ=env,
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
    epic_self_test()
    preview_self_test()
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
