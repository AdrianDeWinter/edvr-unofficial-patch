#!/usr/bin/env python3
"""Read-only Linux Epic install discovery; no launcher clients or credentials.

Metadata proves the store and prefix owner; EliteDangerous64.exe proves that
the product is installed. Discovery never launches a manager or creates files.
"""

from __future__ import annotations

import argparse
import configparser
from dataclasses import dataclass, field
import json
import os
from pathlib import Path
import re
import shutil
import sys
import tempfile
from urllib.parse import quote


GAME_EXE = "EliteDangerous64.exe"


class InventoryLimitError(ValueError):
    """Discovery is incomplete because a directory exceeded its scan bound."""

    def __init__(self, path, limit):
        self.path = path
        self.limit = limit
        super().__init__("Epic discovery directory exceeds the %d-entry scan limit: %s; "
                         "reduce the directory entries and retry." % (limit, path))


@dataclass
class EpicInstall:
    game_dir: Path
    install_root: Path
    prefix: Path
    compatdata: Path | None = None
    runner: Path | None = None
    runner_kind: str = "unknown"
    manager: str = "wine"
    app_name: str = ""
    flatpak_id: str | None = None
    launch_uri: str | None = None
    env: dict = field(default_factory=dict)
    source: Path | None = None
    config_source: Path | None = None
    runner_lookup: str | None = None


_SAFE_ENV = frozenset(("DXVK_LOG_LEVEL", "DXVK_ASYNC", "DXVK_HUD", "VKD3D_CONFIG",
                       "WINEDEBUG", "WINEESYNC", "WINEFSYNC", "PROTON_USE_WINED3D",
                       "PROTON_NO_ESYNC", "PROTON_NO_FSYNC", "PROTON_ENABLE_NVAPI"))


def _read(path):
    try:
        if path.stat().st_size <= 1024 * 1024:
            return path.read_text(encoding="utf-8-sig")
    except (OSError, UnicodeError):
        pass
    return None


def _json(path):
    try:
        value = json.loads(_read(path) or "null")
        return value if isinstance(value, dict) else {}
    except (ValueError, RecursionError):
        return {}


def _path(value, home):
    if not isinstance(value, str) or not value or "\x00" in value:
        return None
    value = value.replace("${HOME}", str(home)).replace("$HOME", str(home))
    if value == "~" or value.startswith("~/"):
        value = str(home) + value[1:]
    path = Path(value)
    try:
        return path.resolve() if path.is_absolute() else None
    except (OSError, RuntimeError):
        return None


def _children(path, limit=512):
    try:
        # Stop enumeration at a fixed bound, rather than traversing a library.
        result = []
        for entry in path.iterdir():
            result.append(entry)
            if len(result) > limit:
                raise InventoryLimitError(path, limit)
        return sorted(result, key=lambda p: p.name)
    except OSError:
        return []


def _prefix(value, home):
    configured = _path(value, home)
    if configured is None:
        return None, None
    if (configured / "pfx/drive_c").is_dir():
        actual = _path(str(configured / "pfx"), home)
        return (actual, configured) if actual else (None, None)
    if (configured / "drive_c").is_dir():
        return configured, None
    return None, None


def _products(root):
    if root is None:
        return []
    if (root / GAME_EXE).is_file():
        return [root]
    leaves = [p.resolve() for p in _children(root / "Products") if (p / GAME_EXE).is_file()]
    odyssey = [p for p in leaves if "odyssey" in p.name.lower()]
    return odyssey or leaves


def _runner(value, home, flatpak_id=None):
    if flatpak_id and isinstance(value, str) and value.startswith(("/app/", "/usr/")):
        if "\x00" not in value and ".." not in Path(value).parts:
            # The wrapper validates against this app's installed deployment,
            # not the host's unrelated /usr or /app directory.
            return Path(value)
        return None
    path = _path(value, home)
    return path if path is not None and path.is_file() else None


def _on_path(executable, home, environ):
    selected = shutil.which(executable, path=environ["PATH"]) if environ.get("PATH") else None
    runner = _runner(selected, home)
    return runner if runner is not None and os.access(runner, os.X_OK) else None


def _safe_env(value):
    return {k: v for k, v in value.items() if k in _SAFE_ENV and isinstance(v, str)
            and "\x00" not in v} if isinstance(value, dict) else {}


def _installed(path, home):
    for app, record in _json(path).items():
        if not isinstance(app, str) or not app or "/" in app or "\\" in app or app in (".", ".."):
            continue
        if not isinstance(record, dict) or record.get("app_name", app) != app:
            continue
        if record.get("is_dlc", False) is not False:
            continue
        if record.get("platform", "Windows") != "Windows":
            continue
        root = _path(record.get("install_path"), home)
        for product in _products(root):
            yield app, root, product


def _heroic_default_fallback(config, flatpak_id, home, environ, found_products):
    """Fallback for Heroic installs when Legendary's installed.json is absent or lacks the entry
    but Heroic's default game folder and prefix are present (#224)."""
    default_roots = (home / "Games/Heroic/EliteDangerous", home / "Games/Heroic/Elite Dangerous")
    default_prefixes = (home / "Games/Heroic/Prefixes/default/Elite Dangerous",
                        home / "Games/Heroic/Prefixes/default/EliteDangerous")
    matched_prefix = None
    matched_compat = None
    for candidate in default_prefixes:
        prefix, compatdata = _prefix(str(candidate), home)
        if prefix is not None:
            matched_prefix, matched_compat = prefix, compatdata
            break
    if matched_prefix is None:
        return
    # Prefer runner metadata from Heroic's GamesConfig if any entry targets this prefix.
    runner, kind, runtime_env, game_config = None, "unknown", {}, None
    for cfg_file in _children(config / "GamesConfig"):
        if cfg_file.suffix.lower() != ".json":
            continue
        data = _json(cfg_file)
        app = cfg_file.stem
        cfg = data.get(app)
        if not isinstance(cfg, dict):
            continue
        cfg_prefix, _ = _prefix(cfg.get("winePrefix"), home)
        if cfg_prefix != matched_prefix:
            continue
        version = cfg.get("wineVersion", {})
        version = version if isinstance(version, dict) else {}
        runner = _runner(version.get("bin"), home, flatpak_id)
        kind = version.get("type") if version.get("type") in ("wine", "proton") else "unknown"
        options = cfg.get("enviromentOptions", [])
        runtime_env = _safe_env({item["key"]: item.get("value") for item in options
                                 if isinstance(item, dict) and isinstance(item.get("key"), str)}) if isinstance(options, list) else {}
        if kind == "proton" and cfg.get("disableUMU", False) is False:
            if runner:
                runtime_env["PROTONPATH"] = str(runner.parent)
            runner = _on_path("umu-run", home, environ) if flatpak_id is None else None
            if runner is None:
                runner = _runner(str(config / "tools/runtimes/umu/umu_run.py"), home)
            kind = "umu"
        if kind in ("proton", "umu") and matched_compat is None:
            matched_compat = _path(cfg.get("winePrefix"), home)
        game_config = cfg_file
        break
    for root in default_roots:
        resolved = _path(str(root), home)
        for product in _products(resolved):
            if product in found_products:
                continue
            found_products.add(product)
            yield EpicInstall(product, resolved, matched_prefix, matched_compat, runner, kind,
                              "heroic", "", flatpak_id, None, runtime_env, resolved, game_config)


def _heroic(config, flatpak_id, home, environ):
    installed = config / "legendaryConfig/legendary/installed.json"
    found_products = set()
    for app, root, product in _installed(installed, home):
        game_config = config / "GamesConfig" / (app + ".json")
        cfg = _json(game_config).get(app)
        if not isinstance(cfg, dict):
            continue
        prefix, compatdata = _prefix(cfg.get("winePrefix"), home)
        if prefix is None:
            continue
        version = cfg.get("wineVersion", {})
        version = version if isinstance(version, dict) else {}
        runner = _runner(version.get("bin"), home, flatpak_id)
        kind = version.get("type") if version.get("type") in ("wine", "proton") else "unknown"
        options = cfg.get("enviromentOptions", [])
        runtime_env = _safe_env({item["key"]: item.get("value") for item in options
                                 if isinstance(item, dict) and isinstance(item.get("key"), str)}) if isinstance(options, list) else {}
        if kind == "proton" and cfg.get("disableUMU", False) is False:
            if runner:
                runtime_env["PROTONPATH"] = str(runner.parent)
            runner = _on_path("umu-run", home, environ) if flatpak_id is None else None
            if runner is None:
                runner = _runner(str(config / "tools/runtimes/umu/umu_run.py"), home)
            kind = "umu"
        if kind in ("proton", "umu") and compatdata is None:
            # UMU also uses raw Wine prefixes (pfx may be a self symlink).
            compatdata = _path(cfg.get("winePrefix"), home)
        found_products.add(product)
        yield EpicInstall(product, root, prefix, compatdata, runner, kind, "heroic", app,
                          flatpak_id, "heroic://launch?appName=" + quote(app, safe="") + "&runner=legendary",
                          runtime_env, installed, game_config)
    if config.is_dir():
        yield from _heroic_default_fallback(config, flatpak_id, home, environ, found_products)


def _legendary(config, home, environ):
    parser = configparser.ConfigParser(interpolation=None)
    parser.optionxform = str
    text = _read(config / "config.ini")
    try:
        if text is not None:
            parser.read_string(text)
    except configparser.Error:
        return

    def value(section, key, fallback=""):
        return parser.get(section, key, fallback=fallback) if parser.has_section(section) else fallback

    installed = config / "installed.json"
    for app, root, product in _installed(installed, home):
        game_env = dict(parser.items("default.env")) if parser.has_section("default.env") else {}
        if parser.has_section(app + ".env"):
            game_env.update(parser.items(app + ".env"))
        prefix_value = (environ.get("WINEPREFIX") or value(app, "wine_prefix")
                        or game_env.get("WINEPREFIX") or str(home / ".wine"))
        prefix, compatdata = _prefix(prefix_value, home)
        if prefix is None:
            continue
        runner = _runner(value(app, "wine_executable", value("default", "wine_executable")), home)
        wrapper = value(app, "wrapper", value("default", "wrapper")).strip()
        no_wine = value(app, "no_wine", value("default", "no_wine")).lower() in ("true", "yes", "1", "on")
        if wrapper or no_wine:
            runner = None
        yield EpicInstall(product, root, prefix, compatdata, runner, "wine" if runner else "unknown",
                          "legendary", app, env=_safe_env(game_env), source=installed,
                          config_source=config / "config.ini")


def _yaml_double_quoted(scalar):
    """Decode a single-line YAML quoted scalar, with no YAML interpretation."""
    escapes = {"0": "\0", "a": "\a", "b": "\b", "t": "\t", "n": "\n",
               "v": "\v", "f": "\f", "r": "\r", "e": "\x1b", " ": " ",
               '"': '"', "/": "/", "\\": "\\", "N": "\x85", "_": "\xa0",
               "L": "\u2028", "P": "\u2029"}
    decoded = []
    index = 1
    while index < len(scalar):
        char = scalar[index]
        index += 1
        if char == '"':
            return "".join(decoded), index
        if char == "\\":
            if index >= len(scalar):
                break
            escape = scalar[index]
            index += 1
            if escape in ("x", "u", "U"):
                digits = {"x": 2, "u": 4, "U": 8}[escape]
                value = scalar[index:index + digits]
                if not re.fullmatch("[0-9A-Fa-f]{%d}" % digits, value):
                    raise ValueError("Invalid YAML Unicode escape")
                codepoint = int(value, 16)
                char = chr(codepoint)
                index += digits
            elif escape in escapes:
                char = escapes[escape]
            else:
                raise ValueError("Unsupported YAML escape")
        elif ord(char) < 0x20 and char != "\t":
            raise ValueError("Invalid YAML quoted character")
        if 0xD800 <= ord(char) <= 0xDFFF:
            raise ValueError("Invalid YAML Unicode scalar")
        decoded.append(char)
    raise ValueError("Unterminated YAML quoted scalar")


def _yaml_mapping(path):
    """Read Lutris's scalar mapping subset; reject YAML requiring interpretation.

    Selected fields accept no tags, anchors, lists or multiline values. Other
    branches (including launch_configs lists) are skipped without interpretation.
    """
    text = _read(path)
    if text is None:
        return {}
    result = {}
    stack = [(-1, result, ())]
    selected = {(): {"game", "wine", "system", "runner"},
                ("game",): {"prefix"},
                ("wine",): {"version", "custom_wine_path"},
                ("system",): {"env"},
                ("system", "env"): _SAFE_ENV}
    skip_indent = None
    try:
        lines = text.splitlines()
        if len(lines) > 4096:
            return {}
        for line in lines:
            if not line.strip() or line.lstrip().startswith("#") or line.strip() in ("---", "..."):
                continue
            indent = len(line) - len(line.lstrip(" "))
            if skip_indent is not None and (indent > skip_indent or
                    (indent == skip_indent and re.match(r"-(?:\s|$)", line[indent:]))):
                continue
            skip_indent = None
            match = re.fullmatch(r"( *)([A-Za-z_][A-Za-z0-9_.-]*):(?: +(.*))?", line)
            if not match or len(match[1]) > 64:
                return {}
            indent, key, scalar = len(match[1]), match[2], match[3]
            while indent <= stack[-1][0]:
                stack.pop()
            parent, context = stack[-1][1:]
            if key not in selected.get(context, set()):
                skip_indent = indent
                continue
            if key in parent:
                return {}
            if scalar is None or scalar.strip() == "{}":
                parent[key] = {}
                stack.append((indent, parent[key], context + (key,)))
                continue
            scalar = scalar.strip()
            if scalar.startswith('"'):
                value, end = _yaml_double_quoted(scalar)
                tail = scalar[end:].strip()
                if not isinstance(value, str) or (tail and not tail.startswith("#")):
                    return {}
            elif scalar.startswith("'"):
                quoted = re.fullmatch(r"'((?:[^']|'')*)'(?:\s*#.*)?", scalar)
                if not quoted:
                    return {}
                value = quoted[1].replace("''", "'")
            else:
                if scalar.startswith(("!", "&", "*", "[", "{", "|", ">")):
                    return {}
                value = re.split(r"\s+#", scalar, maxsplit=1)[0].strip()
                if value in ("null", "~"):
                    value = None
            parent[key] = value
        return result
    except (ValueError, IndexError, RecursionError):
        return {}


def _mapping(value):
    return value if isinstance(value, dict) else {}


def _case_path(root, parts):
    for part in parts:
        if part in ("", "."):
            continue
        if part == "..":
            return None
        exact = root / part
        if exact.exists():
            root = exact
            continue
        matches = [p for p in _children(root) if p.name.casefold() == part.casefold()]
        if len(matches) != 1:
            return None
        root = matches[0]
    try:
        return root.resolve()
    except (OSError, RuntimeError):
        return None


def _manifest_location(value, prefix, home):
    if not isinstance(value, str) or not value:
        return None
    if re.match(r"^[A-Za-z]:[\\/]", value):
        letter = value[0].lower()
        drive = prefix / "dosdevices" / (letter + ":")
        if not drive.exists():
            if letter != "c":
                return None
            drive = prefix / "drive_c"
        return _case_path(drive, re.split(r"[\\/]", value[3:]))
    return _path(value, home)


def _manifests(prefix, compatdata, home, **metadata):
    folder = prefix / "drive_c/ProgramData/Epic/EpicGamesLauncher/Data/Manifests"
    for source in _children(folder):
        if source.suffix.lower() != ".item":
            continue
        record = _json(source)
        app = record.get("AppName", "")
        if not isinstance(app, str) or not app or record.get("bIsIncompleteInstall", False) is not False:
            continue
        main_app = record.get("MainGameAppName", app)
        if not isinstance(main_app, str) or (main_app and main_app != app):
            continue
        root = _manifest_location(record.get("InstallLocation"), prefix, home)
        for product in _products(root):
            yield EpicInstall(product, root, prefix, compatdata, app_name=app, source=source, **metadata)


def _lutris(config, data, flatpak_id, home, environ):
    defaults = _mapping(_yaml_mapping(config / "runners/wine.yml").get("wine"))
    runner_dir = data / "runners"
    ini = configparser.ConfigParser(interpolation=None)
    try:
        text = _read(config / "lutris.conf")
        if text:
            ini.read_string(text)
            custom = _path(ini.get("lutris", "runner_dir", fallback=""), home)
            if custom:
                runner_dir = custom
    except configparser.Error:
        return
    for source in _children(config / "games"):
        if source.suffix.lower() not in (".yml", ".yaml"):
            continue
        record = _yaml_mapping(source)
        game = _mapping(record.get("game"))
        if record.get("runner", "wine") != "wine":
            continue
        prefix, compatdata = _prefix(game.get("prefix"), home)
        if prefix is None:
            continue
        wine = dict(defaults, **_mapping(record.get("wine")))
        version = wine.get("version", "")
        runner, kind, lookup = None, "unknown", None
        if version == "custom":
            runner = _runner(wine.get("custom_wine_path"), home, flatpak_id)
        elif version == "system":
            if flatpak_id:
                kind, lookup = "wine", "wine"
            else:
                runner = _on_path("wine", home, environ)
        elif isinstance(version, str) and version not in ("", ".", "..", "system", "ge-proton") and "/" not in version and "\\" not in version:
            runner = _runner(str(runner_dir / "wine" / version / "bin/wine"), home)
        if runner:
            kind = "wine"
        elif version == "ge-proton":
            kind = "umu"
        env = _safe_env(_mapping(record.get("system")).get("env"))
        yield from _manifests(prefix, compatdata, home, runner=runner, runner_kind=kind,
                              manager="lutris", flatpak_id=flatpak_id, env=env, config_source=source,
                              runner_lookup=lookup)


def _epic_preview(data, home):
    # Preview 0.115.0 uses a version-1 JSONL transaction journal. The header's
    # entries count precedes appended transactions; it is not the line count.
    base = data / "com.epicgames.eglv2"
    source = base / "installations.jsonl"
    text = _read(source)
    if not text:
        return
    try:
        rows = [json.loads(line) for line in text.splitlines() if line.strip()]
    except (ValueError, RecursionError):
        return
    if not rows or not isinstance(rows[0], dict):
        return
    header = rows[0]
    if type(header.get("version")) is not int or header["version"] != 1 or "installationId" in header:
        return
    current = {}
    for record in rows[1:]:
        if not isinstance(record, dict):
            return
        identity = record.get("installationId")
        if not isinstance(identity, str) or not identity or "\x00" in identity:
            return
        if type(record.get("deleted", False)) is not bool:
            return
        # Do not skip damaged transactions: a later tombstone or incomplete
        # update may invalidate an earlier complete installation.
        current[identity] = record
    for record in current.values():
        if record.get("deleted", False) or record.get("complete") is not True or record.get("platform") != "Windows":
            continue
        app = record.get("artifactId")
        if not isinstance(app, str) or not app or app in (".", "..") or any(c in app for c in ("/", "\\", ":", "\x00")):
            continue
        root = _path(record.get("dir"), home)
        configured = base / "compat/compatdata" / app
        if not (configured / "pfx/drive_c").is_dir():
            continue
        prefix, compatdata = _prefix(str(configured), home)
        if prefix is None or compatdata is None:
            continue
        for product in _products(root):
            # The wrapper resolves the existing prefix's config_info through
            # its shared Proton resolver; installed runtime order proves nothing.
            yield EpicInstall(product, root, prefix, compatdata, runner_kind="proton",
                              manager="epic-preview", app_name=app, source=source, config_source=source)


def discover(home=None, environ=None):
    """Return confirmed products with the prefix and runner of their metadata owner.

    Conflicting launch configurations remain separate, so selection fails closed.
    ``home`` and ``environ`` are injectable to keep discovery tests hermetic.
    """
    home = Path(home if home is not None else Path.home()).resolve()
    environ = dict(os.environ if environ is None else environ)
    config = _path(environ.get("XDG_CONFIG_HOME"), home) or home / ".config"
    data = _path(environ.get("XDG_DATA_HOME"), home) or home / ".local/share"
    installs = list(_heroic(config / "heroic", None, home, environ))
    heroic_flat = home / ".var/app/com.heroicgameslauncher.hgl/config/heroic"
    installs.extend(_heroic(heroic_flat, "com.heroicgameslauncher.hgl", home, environ))
    legendary = _path(environ.get("LEGENDARY_CONFIG_PATH"), home) or config / "legendary"
    installs.extend(_legendary(legendary, home, environ))
    lutris = config / "lutris" if (config / "lutris").is_dir() else data / "lutris"
    installs.extend(_lutris(lutris, data / "lutris", None, home, environ))
    flat = home / ".var/app/net.lutris.Lutris"
    lutris_flat = flat / "config/lutris" if (flat / "config/lutris").is_dir() else flat / "data/lutris"
    installs.extend(_lutris(lutris_flat, flat / "data/lutris", "net.lutris.Lutris", home, environ))
    installs.extend(_epic_preview(data, home))
    # Generic Wine is a last resort: never replace a manager's runner metadata.
    owned = {x.prefix for x in installs}
    candidates = [home / ".wine"]
    explicit = _path(environ.get("WINEPREFIX"), home)
    if explicit:
        candidates.append(explicit)
    for first in _children(home / "Games", 128):
        candidates.append(first)
        candidates.extend(_children(first, 128))
    seen = set()
    for candidate in candidates:
        prefix, compatdata = _prefix(str(candidate), home)
        if prefix is not None and prefix not in owned and prefix not in seen:
            installs.extend(_manifests(prefix, compatdata, home))
            seen.add(prefix)
    # A duplicate manifest or manager alias is not a second game install.
    unique = {}
    for install in installs:
        key = (install.game_dir, install.prefix, install.compatdata, install.runner,
               install.runner_kind, install.runner_lookup, install.manager, install.flatpak_id,
               tuple(sorted(install.env.items())))
        unique.setdefault(key, install)
    return sorted(unique.values(), key=lambda x: (str(x.game_dir), x.manager, str(x.prefix)))


def epic_dirs(home=None, environ=None):
    return sorted({x.game_dir for x in discover(home, environ)}, key=str)


def matching_installs(game_path=None, home=None, environ=None):
    """Return discovered launch configurations matching a product or install root."""
    installs = discover(home, environ)
    if game_path is not None:
        selected = Path(game_path).expanduser().resolve()
        installs = [x for x in installs if selected in (x.game_dir, x.install_root)]
    return installs


def select_install(game_path=None, home=None, environ=None):
    installs = matching_installs(game_path, home, environ)
    if not installs:
        raise ValueError("No confirmed Linux Epic Elite install found; specify a known Epic game path and prefix.")
    if len(installs) != 1:
        raise ValueError("Several Epic installs or launch configurations match; select a game path and resolve conflicting manager metadata.")
    return installs[0]


def self_test():
    """Hermetic fixtures exercise products, their ownership and launch metadata."""
    with tempfile.TemporaryDirectory(prefix="edvr_epic_") as temporary:
        home = Path(temporary).resolve()
        host_games = home
        if os.name == "nt" and not str(home).startswith("\\\\?\\"):
            # Host game paths need a consistent namespace distinct from the
            # guest C: manifest paths whose Wine-drive mapping is tested below.
            host_path = str(home)
            host_games = Path("\\\\?\\UNC\\" + host_path[2:] if host_path.startswith("\\\\") else "\\\\?\\" + host_path)
        config = home / "custom config"
        env = {"XDG_CONFIG_HOME": str(config), "XDG_DATA_HOME": str(home / "data")}

        def write(path, value):
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(json.dumps(value), encoding="utf-8")

        def prefix(name, proton=False):
            base = home / name
            actual = base / "pfx" if proton else base
            (actual / "drive_c").mkdir(parents=True)
            (actual / "system.reg").write_text("WINE REGISTRY Version 2\n", encoding="utf-8")
            return base, actual

        def game(name):
            root = host_games / name
            leaf = root / "Products" / "elite-dangerous-odyssey-64"
            leaf.mkdir(parents=True)
            (leaf / GAME_EXE).write_bytes(b"game fixture")
            return root, leaf

        def executable(name):
            path = home / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("fixture", encoding="utf-8")
            path.chmod(0o755)
            return path

        def manifest(actual, root, app="EliteApp"):
            write(actual / "drive_c/ProgramData/Epic/EpicGamesLauncher/Data/Manifests/elite.item",
                  {"AppName": app, "InstallLocation": str(root)})

        # Native Preview journals record current installations independently of
        # their display title; the existing artifact prefix owns the binding.
        for custom_data in (False, True):
            native_home = home / ("preview custom" if custom_data else "preview default")
            native_data = native_home / ("données Épic" if custom_data else ".local/share")
            native_env = {"XDG_DATA_HOME": str(native_data)} if custom_data else {}
            native_base = native_data / "com.epicgames.eglv2"
            native_source = native_base / "installations.jsonl"
            native_root = native_home / "custom drive/Élite renommé"
            native_leaf = native_root / "Products/elite-dangerous-odyssey-64"
            native_leaf.mkdir(parents=True)
            (native_leaf / GAME_EXE).write_bytes(b"native fixture")
            native_compat = native_base / "compat/compatdata/EliteArtifact"
            native_prefix = native_compat / "pfx"
            (native_prefix / "drive_c").mkdir(parents=True)
            for version in ("GE-Proton10-1", "GE-Proton10-2"):
                decoy = native_base / "compat/runtime" / version / "proton"
                decoy.parent.mkdir(parents=True)
                decoy.write_text("unselected runtime fixture", encoding="utf-8")
            record = {"installationId": "A1", "complete": True, "dir": str(native_root),
                      "platform": "Windows", "itemId": "EliteItem", "artifactId": "EliteArtifact"}

            def journal(*records, version=1):
                rows = [{"version": version, "writtenAt": "2026-10-16T08:40:42Z", "entries": 0}, *records]
                native_source.write_text("\n".join(json.dumps(row) for row in rows) + "\n", encoding="utf-8")

            assert discover(native_home, native_env) == [], "a directory alone is not Epic identity"
            journal(record)
            got = select_install(home=native_home, environ=native_env)
            assert (got.game_dir, got.install_root, got.prefix, got.compatdata, got.manager,
                    got.app_name, got.runner_kind, got.runner, got.source) == (
                        native_leaf, native_root, native_prefix, native_compat, "epic-preview",
                        "EliteArtifact", "proton", None, native_source)
            # Latest state wins, including sparse deletion records. An invalid
            # trailing record must never resurrect a previously complete install.
            for latest in ({"installationId": "A1", "deleted": True},
                           dict(record, complete=False), dict(record, platform="Linux"),
                           dict(record, complete="true"), dict(record, artifactId="../EliteArtifact"),
                           dict(record, dir=str(native_home / "deleted game"))):
                journal(record, latest)
                assert discover(native_home, native_env) == [], latest
            journal(record, {"installationId": "A1", "deleted": True}, record)
            assert select_install(home=native_home, environ=native_env).game_dir == native_leaf
            for version in (2, True, "1"):
                journal(record, version=version)
                assert discover(native_home, native_env) == [], "unsupported journal version"
            journal(record)
            with native_source.open("a", encoding="utf-8") as stream:
                stream.write('{"installationId":"A1",')
            assert discover(native_home, native_env) == [], "damaged latest state"
            journal(record)
            (native_prefix / "drive_c").rmdir()
            assert discover(native_home, native_env) == [], "missing existing prefix"
            (native_prefix / "drive_c").mkdir()
            (native_leaf / GAME_EXE).unlink()
            assert discover(native_home, native_env) == [], "missing game executable"
            (native_leaf / GAME_EXE).write_bytes(b"native fixture")
            other_compat = native_base / "compat/compatdata/OtherArtifact"
            (other_compat / "pfx/drive_c").mkdir(parents=True)
            journal(record, dict(record, installationId="A2", artifactId="OtherArtifact"))
            assert len(discover(native_home, native_env)) == 2
            try:
                select_install(native_leaf, native_home, native_env)
                raise AssertionError("conflicting native prefixes selected silently")
            except ValueError:
                pass
            journal(record)
            native_before = {str(p.relative_to(native_home)): (p.lstat().st_mtime_ns,
                             p.read_bytes() if p.is_file() else None) for p in native_home.rglob("*")}
            discover(native_home, native_env)
            native_after = {str(p.relative_to(native_home)): (p.lstat().st_mtime_ns,
                            p.read_bytes() if p.is_file() else None) for p in native_home.rglob("*")}
            assert native_before == native_after, "native discovery wrote files"

        heroic = config / "heroic"
        root, leaf = game("custom jeux Élite")
        base, actual = prefix("prefix Unicode é", True)
        runner = executable("runners/GE-Proton/proton")
        installed = {"EliteApp": {"app_name": "EliteApp", "title": "A renamed title",
                                 "install_path": str(root), "platform": "Windows", "is_dlc": False}}
        write(heroic / "legendaryConfig/legendary/installed.json", installed)
        cfg = heroic / "GamesConfig/EliteApp.json"
        write(cfg, {"EliteApp": {"winePrefix": str(base), "wineVersion": {"bin": str(runner), "type": "proton"}, "disableUMU": True,
                                 "enviromentOptions": [{"key": "WINEESYNC", "value": "1"}, {"key": "SECRET_TOKEN", "value": "hidden"}]}})
        got = select_install(home=home, environ=env)
        assert (got.game_dir, got.prefix, got.compatdata, got.runner, got.runner_kind, got.manager) == (leaf, actual, base, runner, "proton", "heroic")
        assert matching_installs(root, home, env) == [got]
        assert matching_installs(leaf, home, env) == [got]
        assert matching_installs(home / "unowned product", home, env) == []
        assert got.launch_uri == "heroic://launch?appName=EliteApp&runner=legendary"
        assert got.env == {"WINEESYNC": "1"}
        # Heroic chooses the provided PATH's UMU before its bundled default.
        bundled_umu = executable("custom config/heroic/tools/runtimes/umu/umu_run.py")
        preferred_umu = executable("preferred UMU/umu-run.exe" if os.name == "nt" else "preferred UMU/umu-run")
        write(cfg, {"EliteApp": {"winePrefix": str(base), "wineVersion": {"bin": str(runner), "type": "proton"}, "disableUMU": False}})
        got = select_install(home=home, environ=dict(env, PATH=str(preferred_umu.parent)))
        assert (got.runner, got.runner_kind, got.env) == (preferred_umu, "umu", {"PROTONPATH": str(runner.parent)})
        assert select_install(home=home, environ=env).runner == bundled_umu
        bundled_umu.unlink()
        assert select_install(home=home, environ=env).runner is None
        # Metadata nesting belongs to its app ID, not an arbitrary winePrefix.
        write(cfg, {"OtherApp": {"winePrefix": str(base)}})
        assert discover(home, env) == []
        write(cfg, {"EliteApp": {"winePrefix": str(base), "wineVersion": {"bin": str(runner), "type": "proton"}, "disableUMU": True}})
        for invalid in ({"is_dlc": True}, {"platform": "Linux"}, {"install_path": 4}, {"app_name": "OtherApp"}):
            write(heroic / "legendaryConfig/legendary/installed.json", {"EliteApp": dict(installed["EliteApp"], **invalid)})
            assert discover(home, env) == [], invalid
        write(heroic / "legendaryConfig/legendary/installed.json", installed)
        # A manifest app ID must stay within GamesConfig on either host OS.
        # Give rejected IDs real configs so missing files cannot hide traversal.
        for app in ("../escaped", "nested/app", r"..\escaped", r"nested\app", "Élite 🚀"):
            app_config = heroic / "GamesConfig" / (app + ".json")
            write(app_config, {app: {"winePrefix": str(base),
                                   "wineVersion": {"bin": str(runner), "type": "proton"},
                                   "disableUMU": True}})
            write(heroic / "legendaryConfig/legendary/installed.json",
                  {app: dict(installed["EliteApp"], app_name=app)})
            matches = discover(home, env)
            if app == "Élite 🚀":
                assert len(matches) == 1 and matches[0].app_name == app
                assert matches[0].launch_uri == "heroic://launch?appName=%C3%89lite%20%F0%9F%9A%80&runner=legendary"
            else:
                assert matches == [], "unsafe Heroic app name selected: " + app
            app_config.unlink()
        write(heroic / "legendaryConfig/legendary/installed.json", installed)
        (leaf / GAME_EXE).unlink()
        assert discover(home, env) == []
        (leaf / GAME_EXE).write_bytes(b"game fixture")

        # A second confirmed store install is ambiguous, even on a Steamless host.
        other_root, other_leaf = game("flatpak custom location")
        _, other_prefix = prefix("flatpak prefix")
        wine = executable("custom wine/bin/wine")
        flatpak = home / ".var/app/com.heroicgameslauncher.hgl/config/heroic"
        write(flatpak / "legendaryConfig/legendary/installed.json", {"SecondApp": {"app_name": "SecondApp", "install_path": str(other_root)}})
        write(flatpak / "GamesConfig/SecondApp.json", {"SecondApp": {"winePrefix": str(other_prefix), "wineVersion": {"bin": str(wine), "type": "wine"}}})
        assert len(discover(home, env)) == 2
        try:
            select_install(home=home, environ=env)
            raise AssertionError("ambiguous installs selected silently")
        except ValueError:
            pass
        got = select_install(other_leaf, home, env)
        assert (got.flatpak_id, got.runner, got.prefix) == ("com.heroicgameslauncher.hgl", wine, other_prefix)
        if os.name != "nt":
            flatcfg = flatpak / "GamesConfig/SecondApp.json"
            write(flatcfg, {"SecondApp": {"winePrefix": str(other_prefix), "wineVersion": {"bin": "/app/bin/wine", "type": "wine"}}})
            assert select_install(other_leaf, home, env).runner == Path("/app/bin/wine")
            write(flatcfg, {"SecondApp": {"winePrefix": str(other_prefix), "wineVersion": {"bin": str(wine), "type": "wine"}}})

        # Standalone Legendary honors process prefix, then per-app, then env.
        legendary = home / "standalone Legendary"
        legroot, legleaf = game("Legendary install")
        _, legprefix = prefix("Legendary prefix")
        _, envprefix = prefix("environment prefix")
        write(legendary / "installed.json", {"LegApp": {"app_name": "LegApp", "install_path": str(legroot)}})
        (legendary / "config.ini").write_text("[default]\nwine_executable = " + str(wine) + "\nwine_prefix = /ignored-default\n[default.env]\nWINEPREFIX = " + str(envprefix) + "\nSECRET_TOKEN = never-export\n[LegApp]\nwine_prefix = " + str(legprefix) + "\n", encoding="utf-8")
        legenv = dict(env, LEGENDARY_CONFIG_PATH=str(legendary))
        got = select_install(legleaf, home, legenv)
        assert (got.prefix, got.runner, got.manager, got.env) == (legprefix, wine, "legendary", {})
        got = select_install(legleaf, home, dict(legenv, WINEPREFIX=str(envprefix)))
        assert got.prefix == envprefix
        with (legendary / "config.ini").open("a", encoding="utf-8") as stream:
            stream.write("wrapper = opaque shell wrapper\n")
        assert select_install(legleaf, home, legenv).runner_kind == "unknown"

        # Lutris owns the prefix/runner; the Epic manifest owns the product.
        lutris = config / "lutris"
        _, lutprefix = prefix("Lutris custom prefix")
        lutroot, lutleaf = game("Lutris game")
        manifest(lutprefix, lutroot)
        yml = lutris / "games/epic.yml"
        yml.parent.mkdir(parents=True)
        yml.write_text("game:\n  prefix: '" + str(lutprefix) + "'\n  exe: drive_c/EpicGamesLauncher.exe\nwine:\n  version: custom\n  custom_wine_path: '" + str(wine) + "'\nsystem:\n  env:\n    DXVK_LOG_LEVEL: none\n    SECRET_TOKEN: hidden\n", encoding="utf-8")
        got = select_install(lutleaf, home, legenv)
        assert (got.prefix, got.runner, got.manager, got.env) == (lutprefix, wine, "lutris", {"DXVK_LOG_LEVEL": "none"})
        # Lutris safe_dump emits YAML-only escapes in paths, including runners.
        _, unicode_prefix = prefix("Lutris Élite 🚀 prefix")
        unicode_wine = executable("Lutris Élite 🚀 runner/bin/wine")
        manifest(unicode_prefix, lutroot)
        escaped_prefix = home.as_posix() + r"/Lutris \xC9lite \U0001F680 prefix"
        escaped_wine = home.as_posix() + r"/Lutris \xC9lite \U0001F680 runner/bin/wine"
        original_yaml = yml.read_text(encoding="utf-8")
        yml.write_text('game:\n  prefix: "' + escaped_prefix + '"\nwine:\n  version: custom\n  custom_wine_path: "' + escaped_wine + '"\n', encoding="utf-8")
        got = select_install(lutleaf, home, legenv)
        assert (got.prefix, got.runner) == (unicode_prefix, unicode_wine)
        for invalid in (r'"\xZZ"', r'"\uD800"', r'"\U00110000"', r'"\q"',
                        '!!python/object/apply:os.system [unsupported]'):
            yml.write_text("game:\n  prefix: " + invalid + "\n", encoding="utf-8")
            assert _yaml_mapping(yml) == {}, "invalid YAML scalar was accepted"
        yml.write_text(original_yaml, encoding="utf-8")
        lutmanifest = lutprefix / "drive_c/ProgramData/Epic/EpicGamesLauncher/Data/Manifests/elite.item"
        base_manifest = json.loads(lutmanifest.read_text(encoding="utf-8"))
        write(lutmanifest, dict(base_manifest, MainGameAppName="DifferentBaseGame"))
        assert all(x.game_dir != lutleaf for x in discover(home, legenv))
        write(lutmanifest, dict(base_manifest, bIsIncompleteInstall=True))
        assert all(x.game_dir != lutleaf for x in discover(home, legenv))
        write(lutmanifest, base_manifest)
        (lutprefix / "drive_c/ProgramData/Epic/EpicGamesLauncher/Data/Manifests/elite.item").unlink()
        assert all(x.game_dir != lutleaf for x in discover(home, legenv))

        # Lutris runner defaults, custom runner_dir and Flatpak data fallback.
        manifest(lutprefix, lutroot)
        defaults = lutris / "runners/wine.yml"
        defaults.parent.mkdir(parents=True)
        defaults.write_text("wine:\n  version: inherited-ge\n", encoding="utf-8")
        selected_wine = executable("chosen runners/wine/inherited-ge/bin/wine")
        (lutris / "lutris.conf").write_text("[lutris]\nrunner_dir = " + str(home / "chosen runners") + "\n", encoding="utf-8")
        yml.write_text("game:\n  prefix: '" + str(lutprefix) + "'\n  launch_configs:\n    - name: Alternate\n      args: '--alternate'\n", encoding="utf-8")
        assert select_install(lutleaf, home, legenv).runner == selected_wine
        # PyYAML writes indentless lists. Skip their complete subtree, then
        # resume selected siblings without interpreting nested list contents.
        yml.write_text("game:\n  launch_configs:\n  - name: Alternate\n    game:\n      prefix: /not-the-owner\n    options:\n    - - nested\n      - values\n  - name: !!unsupported ignored\n  prefix: '" + str(lutprefix) + "'\n"
                       "ignored:\n- nested:\n  - value\nwine:\n  version: custom\n  custom_wine_path: '" + str(wine) + "'\n", encoding="utf-8")
        got = select_install(lutleaf, home, legenv)
        assert (got.prefix, got.runner, got.manager) == (lutprefix, wine, "lutris")
        for invalid in ("  prefix:\n  - unsupported\n", "  prefix: !!unsupported value\n"):
            yml.write_text("game:\n" + invalid, encoding="utf-8")
            assert all(x.game_dir != lutleaf for x in discover(home, legenv)), "selected YAML list or tag was accepted"
        yml.write_text("game:\n  prefix: [unsupported, prefix]\n", encoding="utf-8")
        assert all(x.game_dir != lutleaf for x in discover(home, legenv))
        yml.write_text("game:\n  prefix: '" + str(lutprefix) + "'\nwine:\n  version: system\n", encoding="utf-8")
        system_wine = executable("system Wine/bin/wine.exe" if os.name == "nt" else "system Wine/bin/wine")
        assert select_install(lutleaf, home, dict(legenv, PATH=str(system_wine.parent))).runner == system_wine
        assert select_install(lutleaf, home, legenv).runner is None
        yml.write_text("game:\n  prefix: '" + str(lutprefix) + "'\nwine:\n  version: ge-proton\n", encoding="utf-8")
        got = select_install(lutleaf, home, legenv)
        assert got.runner is None and got.runner_kind == "umu"
        # Keep the last allowed entry discoverable; an additional entry must
        # produce a diagnostic, never an empty or partial library.
        for index in range(511):
            (yml.parent / ("other-%03d.yml" % index)).write_text("runner: other\n", encoding="utf-8")
        assert select_install(lutleaf, home, legenv).prefix == lutprefix
        overflow = yml.parent / "overflow.yml"
        overflow.write_text("runner: other\n", encoding="utf-8")
        before_overflow = {p.name: (p.stat().st_mtime_ns, p.read_bytes()) for p in yml.parent.iterdir()}
        for discover_overflow in (lambda: discover(home, legenv),
                                  lambda: select_install(lutleaf, home, legenv)):
            try:
                discover_overflow()
                raise AssertionError("oversized Lutris inventory was silently accepted")
            except InventoryLimitError as exc:
                assert str(yml.parent) in str(exc) and "512-entry scan limit" in str(exc)
        import contextlib
        import io
        from unittest.mock import patch
        output, errors = io.StringIO(), io.StringIO()
        with patch.object(Path, "home", return_value=home), patch.dict(os.environ, legenv, clear=True), \
                contextlib.redirect_stdout(output), contextlib.redirect_stderr(errors):
            assert main([]) == 2
        assert output.getvalue() == ""
        assert str(yml.parent) in errors.getvalue() and "512-entry scan limit" in errors.getvalue()
        assert "Traceback" not in errors.getvalue()
        after_overflow = {p.name: (p.stat().st_mtime_ns, p.read_bytes()) for p in yml.parent.iterdir()}
        assert after_overflow == before_overflow, "failed discovery wrote files"
        for source in yml.parent.glob("other-*.yml"):
            source.unlink()
        overflow.unlink()
        (lutprefix / "drive_c/ProgramData/Epic/EpicGamesLauncher/Data/Manifests/elite.item").unlink()
        _, flatprefix = prefix("Lutris sandbox prefix")
        flatroot, flatleaf = game("Lutris sandbox game")
        manifest(flatprefix, flatroot)
        lutflat = home / ".var/app/net.lutris.Lutris/data/lutris"
        flatyaml = lutflat / "games/epic.yaml"
        flatyaml.parent.mkdir(parents=True)
        flatyaml.write_text("game:\n  prefix: " + json.dumps(str(flatprefix), ensure_ascii=False) + " # chosen prefix\nwine:\n  version: bundled\n", encoding="utf-8")
        flatwine = executable(".var/app/net.lutris.Lutris/data/lutris/runners/wine/bundled/bin/wine")
        got = select_install(flatleaf, home, legenv)
        assert (got.runner, got.flatpak_id, got.manager) == (flatwine, "net.lutris.Lutris", "lutris")
        flatyaml.write_text("game:\n  prefix: " + json.dumps(str(flatprefix), ensure_ascii=False) + "\nwine:\n  version: system\n", encoding="utf-8")
        got = select_install(flatleaf, home, legenv)
        assert (got.runner, got.runner_kind, got.runner_lookup) == (None, "wine", "wine")

        # UMU's self pfx symlink retains the configured compatdata root.
        _, umuprefix = prefix("UMU self prefix")
        if os.name != "nt":
            (umuprefix / "pfx").symlink_to(".", target_is_directory=True)
            assert _prefix(str(umuprefix), home) == (umuprefix, umuprefix)

        # A mapped Windows drive and case variants resolve without prefix guessing.
        if os.name != "nt":
            (flatprefix / "dosdevices").mkdir()
            (flatprefix / "dosdevices/d:").symlink_to(flatroot.parent, target_is_directory=True)
            assert _manifest_location("D:\\" + flatroot.name.upper(), flatprefix, home) == flatroot

        # Unowned Wine manifests prove Epic but cannot select a host Wine runner.
        _, raw = prefix(".wine")
        winroot = raw / "drive_c/Games/Space"
        winroot.mkdir(parents=True)
        (winroot / GAME_EXE).write_bytes(b"game fixture")
        manifest(raw, "C:\\Games\\Space")
        got = select_install(winroot, home, legenv)
        assert got.manager == "wine" and got.runner is None and got.prefix == raw
        (raw / "drive_c/ProgramData/Epic/EpicGamesLauncher/Data/Manifests/elite.item").unlink()
        assert all(x.game_dir != winroot for x in discover(home, legenv))

        # Bounded Games roots include known prefixes but never recurse indefinitely.
        bounded, bounded_prefix = prefix("Games/manager/prefix")
        boundedroot, boundedleaf = game("bounded game")
        manifest(bounded_prefix, boundedroot)
        assert select_install(boundedleaf, home, legenv).prefix == bounded
        if os.name != "nt":
            (home / "Games/loop").symlink_to(home / "Games", target_is_directory=True)
            (home / "Games/broken").symlink_to("broken", target_is_directory=True)
        assert len([x for x in discover(home, legenv) if x.game_dir == boundedleaf]) == 1

        # Two real products prefer Odyssey over legacy; missing executables never count.
        legacy = root / "Products/elite-dangerous-64"
        legacy.mkdir()
        (legacy / GAME_EXE).write_bytes(b"legacy fixture")
        assert _products(root) == [leaf]
        fake_root = home / "Elite Dangerous is only a title"
        fake_root.mkdir()
        assert _products(fake_root) == []

        # Heroic default install + prefix fallback (#224) when installed.json is absent.
        hdef_root = home / "Games/Heroic/EliteDangerous"
        hdef_leaf = hdef_root / "Products/elite-dangerous-odyssey-64"
        hdef_leaf.mkdir(parents=True)
        (hdef_leaf / GAME_EXE).write_bytes(b"heroic default")
        hdef_pfx, _ = prefix("Games/Heroic/Prefixes/default/Elite Dangerous")
        hdef_cfg = config / "heroic-fallback"
        (hdef_cfg / "GamesConfig").mkdir(parents=True)
        hdef_found = list(_heroic(hdef_cfg, None, home, legenv))
        assert len(hdef_found) == 1 and hdef_found[0].game_dir == hdef_leaf.resolve()
        assert hdef_found[0].prefix == hdef_pfx and hdef_found[0].manager == "heroic"
        assert hdef_found[0].runner is None and hdef_found[0].runner_kind == "unknown"

        # Heroic fallback matched-runner branch via GamesConfig targeting the default prefix.
        hdef_wine = executable("runners/heroic-wine/bin/wine")
        hdef_gc = hdef_cfg / "GamesConfig/9c203b6ed35846e8a4a9ff1e314f6593.json"
        hdef_gc.write_text(json.dumps({
            "9c203b6ed35846e8a4a9ff1e314f6593": {
                "winePrefix": str(hdef_pfx),
                "wineVersion": {"bin": str(hdef_wine), "type": "wine"},
                "enviromentOptions": [{"key": "DXVK_HUD", "value": "fps"}, {"key": "UNSAFE_VAR", "value": "1"}],
            }
        }), encoding="utf-8")
        hdef_runner = list(_heroic(hdef_cfg, None, home, legenv))
        assert len(hdef_runner) == 1
        assert hdef_runner[0].runner == hdef_wine and hdef_runner[0].runner_kind == "wine"
        assert hdef_runner[0].config_source == hdef_gc and hdef_runner[0].env == {"DXVK_HUD": "fps"}

        # Heroic fallback deduplicates against installed.json when both point at the default install.
        hdef_inst = hdef_cfg / "legendaryConfig/legendary/installed.json"
        hdef_inst.parent.mkdir(parents=True, exist_ok=True)
        hdef_inst.write_text(json.dumps({
            "9c203b6ed35846e8a4a9ff1e314f6593": {"install_path": str(hdef_root)}
        }), encoding="utf-8")
        hdef_dedup = list(_heroic(hdef_cfg, None, home, legenv))
        assert len(hdef_dedup) == 1 and hdef_dedup[0].app_name == "9c203b6ed35846e8a4a9ff1e314f6593"
        assert hdef_dedup[0].source == hdef_inst

        # Discovery leaves even absent configured directories absent.
        def snapshot():
            return {str(p.relative_to(home)): (p.lstat().st_mtime_ns, p.read_bytes() if p.is_file() else None)
                    for p in home.rglob("*") if not p.is_symlink()}
        before = snapshot()
        discover(home, dict(legenv, LEGENDARY_CONFIG_PATH=str(home / "absent")))
        after = snapshot()
        assert before == after, "discovery wrote files"
    print("epic_library self-test: PASS")
    return True


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args(argv)
    if args.self_test:
        return 0 if self_test() else 1
    try:
        installs = discover()
    except InventoryLimitError as exc:
        print("ED-UP: " + str(exc), file=sys.stderr)
        return 2
    for install in installs:
        print("%s [%s]" % (install.game_dir, install.manager))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
