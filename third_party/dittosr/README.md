# DittoSR 0.5.0

Pinned to [Vancete/DittoSR v0.5.0](https://github.com/Vancete/DittoSR/releases/tag/v0.5.0)
(`a66b88c0cb3217fa924bcecdfa3c2ae59638207b`). MIT, with Snapdragon Game Super
Resolution 2 under BSD-3-Clause (`licenses/LICENSE-snapdragon-gsr.txt`). The
release zip carries those two texts as `DITTOSR-LICENSE.txt` and
`DITTOSR-GSR-LICENSE.txt`.

ED-UP compiles the four upscale passes (`convert`, `motion`, `upscale`,
`sharpen`) as Direct3D 11 shader model 5. The upstream project ships them as
shader model 6 for DirectX 12 and Vulkan. Two `select()` calls in
`shaders/common.hlsli` are written out as component tests so `D3DCompile` can
build them. History is RGBA16F rather than RGB10A2, because a 10-bit
unordered-access view is not dependable on Direct3D 11. Ditto Relight is not
included.

`src/d3d11/dittosr_shader_bytecode.h` is these passes compiled to shader model 5
with fxc or D3DCompile. DXC promotes `cs_5_0` to shader model 6 and writes
DXIL, which Direct3D 11 will not create; `tools/gen_dittosr_shaders.py` rejects
that container.

Regenerate after a shader edit with `python tools/gen_dittosr_shaders.py`.
On Windows, the tool uses `fxc` if available and falls back to
`d3dcompiler_47.dll`. On Linux, set `FXC` to a runnable compiler. With Wine,
point it at an executable wrapper that runs `fxc.exe` and converts the input
and output paths with `winepath`. A host with a PE execution handler can
invoke `fxc.exe` directly if the compiler can resolve those paths.
`python tools/gen_dittosr_shaders.py --check-bytes` recompiles all four passes
and compares them with the committed header.
