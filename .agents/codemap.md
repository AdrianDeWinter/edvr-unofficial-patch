# Codebase map

Architectural snapshot generated with `codemap 4.9.0` against
[`.codemap/config.json`](../.codemap/config.json) on `main` (`3f33debe`, 1,780
tracked source files across C++, C, HLSL, Python, C#, Batch, and Shell; 524 hub
files with 3+ importers).

## Live `codemap` commands

When `codemap` is installed, query the live dependency graph instead of relying
on this snapshot:

```bash
codemap .                        # filtered structure + top hubs (.codemap/config.json)
codemap --hubs .                 # all hub files (3+ importers)
codemap --importers <file>       # blast radius before editing a hub or shared header
codemap --deps .                 # dependency flow across source files
codemap --diff --ref main        # changed files + hub warnings vs main
codemap blast-radius --ref main  # review bundle: diff + deps + importers
codemap find "<symbol>"          # locate definitions and importer counts
codemap context --for "<task>"   # subsystem routing + matched skills (JSON)
```

Graph coverage: complete for C++, C, Python, and Bash; partial for C# (13 files
under `tools/cpu_profile/`, where same-package references need symbol
resolution).

## Subsystems

| Subsystem | Scope (`codemap .`) | Key entry points | Authoritative docs |
|---|---|---|---|
| `src/d3d11/` | 729 files (`d3d11.dll` proxy) | `d3d11_proxy.cpp`, `device_hook.cpp`, `vscreen.cpp`, `temporal_pass.cpp`, `flat_temporal.cpp`, `native_on_foot.cpp`, `ui_layer.cpp`, `menu.cpp`, `dlaa.cpp`, `fsr3_engine.cpp`, `fsr4_engine.cpp`, `dittosr_engine.cpp`, `xess_engine.cpp` | `docs/architecture.md`, `docs/engine-render-pipeline.md`, `docs/anti-aliasing.md`, `docs/camera-struct.md`, `docs/game-binary-map.md`, `docs/fixes.md`, `GLOSSARY.md` |
| `src/common/` | 118 files (shared runtime) | `config.cpp`, `iniedit.cpp`, `log.cpp`, `guard.cpp`, `code_hook.cpp`, `vtable_hook.cpp`, `iat_hook.cpp`, `game_build.cpp`, `frame_flag.cpp`, `proxy.cpp` | `docs/fixes.md`, `docs/settings-menu.md`, `docs/troubleshooting.md`, `GLOSSARY.md` |
| `src/openxr/` & `src/openvr/compat/` | 111 + 2 files (`openvr_api.dll`) | `runtime_exports.cpp`, `native_module.cpp`, `openvr_system.cpp`, `openvr_compositor.cpp`, `d3d11_stereo.cpp`, `session_binding.cpp`, `published_session.cpp` | `docs/openxr-port.md`, `docs/openxr-implementation-status.md`, `docs/canted-projection.md`, `docs/eye-tracking.md` |
| `src/installer/` & `ed-up-launcher.sh` | 83 + 1 files (`ed-up-launcher.exe`) | `launcher_main.cpp`, `launcher_session.cpp`, `launcher_plan.cpp`, `launcher_core.cpp`, `gui.cpp`, `plan.cpp`, `apply.cpp`, `detect.cpp`, `probe.cpp`, `edhm_d3dx.cpp`, `updater.cpp`, `mirror.cpp` | `docs/installer.md`, `docs/manual-install.md`, `docs/settings-menu.md`, `docs/epic-linux-preview.md` |
| `src/plugins/` & `include/` | 4 + 1 files (plugin ABI) | `include/edvr_plugin_api.h`, `src/plugins/plugin_manager.cpp` | `docs/architecture.md` |
| `tools/`, `build.bat`, `package.bat` | 729 files (Python CLIs, C++ test rigs, `tools/cpu_profile/` C#) | `build.bat`, `package.bat`, `tools/run_jobs.py`, `tools/install_edup.py`, `tools/edup_log.py`, `tools/linux_toolchain.py`, `tools/check_*.py` | `docs/building.md`, `CODING_STANDARDS.md`, `CONTRIBUTING.md` |

## High-impact hubs by subsystem

Changes to these files affect wide portions of the tree; check importers
(`codemap --importers <file>`) before altering signatures, defaults, or
invariants.

### `src/common/` (69 hubs)

- `src/common/log.h` (`←190`)
- `src/common/config.h` (`←114`)
- `src/common/runtime_profile.h` (`←85`)
- `src/common/guard.h` (`←60`)
- `src/common/system_d3d11.h` (`←52`)
- `src/common/frame_flag.h` (`←36`)
- `src/common/game_build.h` (`←36`)
- `src/common/timing.h` (`←29`)
- `src/common/code_hook.h` (`←26`)
- `src/common/iniedit.h` (`←22`)

### `src/d3d11/` (330 hubs)

- `src/d3d11/native_on_foot_core.h` (`←80`)
- `src/d3d11/binding_shadow.h` (`←51`)
- `src/d3d11/shader_swap.h` (`←36`)
- `src/d3d11/vscreen.h` (`←34`)
- `src/d3d11/gpu_census.h` (`←27`)
- `src/d3d11/ui_layer_math.h` (`←27`)
- `src/d3d11/journal_watch.h` (`←26`)
- `src/d3d11/flat_compute_readback.h` (`←24`)
- `src/d3d11/gpu_timing.h` (`←23`)
- `src/d3d11/ui_layer.h` (`←23`)

### `src/openxr/` & `src/openvr/compat/` (49 + 1 hubs)

- `src/openvr/compat/openvr_v0_9_20.h` (`←20`)
- `src/openxr/geometry_snapshot.h` (`←14`)
- `src/openxr/native_trace.h` (`←12`)
- `src/openxr/owner_service.h` (`←9`)
- `src/openxr/native_cpu_trace.h` (`←7`)
- `src/openxr/gpu_work_observer.h`, `native_runtime_host.h`,
  `present_work_queue.h`, `projection_math.h`, `render_thread_dispatcher.h`,
  `space_pose.h` (`←6` each)

### `src/installer/` (40 hubs)

- `src/installer/update_util.h` (`←22`)
- `src/installer/detect.h` (`←16`)
- `src/installer/probe.h` (`←16`)
- `src/installer/mirror.h` (`←12`)
- `src/installer/launch_route.h` (`←11`)
- `src/installer/update_json.h` (`←11`)
- `src/installer/launcher_core.h` (`←9`)
- `src/installer/launcher_state.h`, `state.h`, `update_http.h` (`←8` each)

### `include/`, `src/plugins/`, and `tools/` (1 + 1 + 33 hubs)

- `include/edvr_plugin_api.h` (`←9`)
- `src/plugins/plugin_manager.h` (`←5`)
- `tools/dump_io.py` (`←20`)
- `tools/rig_mutants_lib.py` (`←9`)
- `tools/openxr_pe.py` (`←7`)
- `tools/edup_env.py`, `tools/edup_log.py`, `tools/eye_draw_snapshot.py`,
  `tools/log_session.py` (`←5` each)
- `tools/install_edup.py`, `tools/fetch_openxr_loader.py` (`←3` each)
