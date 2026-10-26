# Patina

**A headless, agent-first texturing engine: Substance Painter for AI agents.**

Patina textures 3D assets (from Blender, or anything that exports glTF/OBJ) with Substance-style
layer stacks:

- fill layers, folders and smart materials
- mask stacks of generators: edge wear, dirt, curvature, AO, gradients, 3D noise, grunge, scratches, streaks
- 3D paint strokes and projected decals

It ships as one native C++ binary for **macOS and Windows** with a **CLI**, an **MCP server** and a
**native viewer**. There is no GUI to drive and no web stack, and a GPU is optional: bakes are ray traced on
one when it is there and on every CPU core when it is not.

![sci-fi panel textured by Patina](docs/images/panel.png)

## Why

Substance Painter is built for people with a mouse. Agents need:

| Agents need | Patina |
|---|---|
| A way to express intent | A **declarative project file**: every layer, mask and parameter is JSON an agent can read, diff and patch. |
| Eyes | A **built-in renderer** that returns images the agent can look at: lit PBR, clay, per-channel, and "where does this layer apply" mask views. About 100 ms per preview. |
| Reproducibility | **Deterministic output**: the same project gives bit-identical textures every time. (GPU and CPU bakes agree to float rounding, not to the bit; `PATINA_GPU=0` pins the CPU.) |
| Scale | **Parallelism everywhere**: baking, evaluation, rendering and PNG encoding use every core, MCP tool calls run concurrently, and `batch`/`variants` fan out across assets and alternatives. |
| Bakes that don't interrupt the loop | A **GPU baker**: AO, thickness, the high-to-low projection and the bevel are ray traced in compute shaders, about 10x faster than 32 CPU threads end to end, with an automatic CPU fallback and `bake --verify` to compare the two. |
| No UV seams | **3D procedural noise evaluated at the surface position**, so noise is seamless across seams. |
| Normal maps without the fuss | A **high-to-low normal baker** with automatic cage, automatic anti-skew and `_low`/`_high` name matching (no bleed between parts), plus a **bevel shader** that bakes rounded edges from the low poly alone. MikkTSpace throughout. The same projection bakes a 16-bit **height map** for parallax and tessellation. |
| Engine-ready output | Export presets for Blender, glTF (ORM), Unreal, Unity HDRP/URP and Godot, plus a textured `.glb`, the mesh as **USD** (`.usda`, `.usdc` or `.usdz`) with materials that read the exported maps, and a Blender bridge that builds the materials. |

<p>
<img src="docs/images/barrel.png" width="49%" alt="stylized barrel with three texture sets">
<img src="docs/images/crate_blender.png" width="24%" alt="Patina textures rendered in Blender Cycles">
<img src="docs/images/panel_mask.png" width="24%" alt="mask debug view">
</p>

*Left: three texture sets on one mesh (oak staves and heads with UV-space grain along every board, forged iron
hoops with rust), lit by the built-in studio HDRI. Middle: Patina's
export rendered in Blender Cycles through the bridge. Right: `mode=mask:<layer>` shows exactly where a layer applies.*

![stylized beer barrel on a cradle with a brass tap](docs/images/beer_barrel.png)

*The same barrel as a stylized beer barrel: six texture sets (staves, heads, hoops, rivets, brass tap, cradle),
painted edges from curvature, a burned "XXX" brand painted with 3D strokes, and wet wood under the tap
([project](examples/projects/beer_barrel.patina.json)).*

![16 smart materials rendered by one variants call](docs/images/gallery.png)

*One `variants` call renders 16 smart materials side by side, evaluated in parallel in 0.7 s.*

## Performance

Measured on an M5 Pro (6P + 12E cores) at 2048² per texture set:

| Asset | Tris | Cold bake (AO, thickness, curvature) | Layer eval | 4-view preview | Full export |
|-------|------|--------------------------------------|------------|----------------|-------------|
| Suzanne | 15.7k | 0.86 s | 0.15 s | 22 ms | 0.27 s |
| Barrel, previous 6k-tri asset (2 texture sets) | 6k | 1.25 s | 0.8 s | 45 ms | ~2 s |

- Previews evaluate at 1024² by default: about 0.1–0.4 s per edit-render loop.
- Bakes are cached in memory and on disk (`.patina/`).
- A 5-asset cold `batch` export at 2K finishes in about 6 s of wall time.

**GPU baking.** On Windows the ray-traced maps are baked in Direct3D 11 compute shaders, so any DX11 GPU
works (as Toolbag's ray tracing backend does on cards without ray tracing hardware), with the CPU baker as
reference and fallback (as Painter keeps CPU raytracing for when GPU raytracing is off). Cold bakes on an
RTX 4070 Ti SUPER, against the same build on the 32 threads of a Ryzen 9 7950X3D:

| Asset | Resolution | CPU | GPU | |
|-------|-----------|-----|-----|---|
| Suzanne (15.7k tris) | 2048² | 2.24 s | 0.30 s | 7.5x |
| Suzanne | 4096² | 8.5 s | 0.70 s | 12x |
| Beer barrel (43k tris, 6 texture sets) | 6 × 2048² | 15.0 s | 1.26 s | 12x |
| Beer barrel | 6 × 4096² | 57.9 s | 4.3 s | 13x |
| Bolt plate, high-to-low normals + bevel shader + AO from the high poly | 2 × 2048² | 9.8 s | 1.2 s | 8x |

- AO and thickness alone are 25–30x faster (Suzanne at 2K: 2.1 s to 0.08 s). What remains is CPU work:
  rasterizing the UVs, curvature and filtering, plus about 0.2 s for the driver to start, most of it hidden
  behind the rasterizer. Bakes under 512² texels in total stay on the CPU, which finishes them sooner.
- Both devices run the same estimators with the same sample sequences. `patina bake <project> --verify` bakes on
  both and reports how far apart each map is; the test suite runs it on its AO, projection and bevel projects.
- Any failure on the GPU (driver reset, out of video memory) re-bakes on the CPU and is reported in the stats.
- A bake does not own the GPU: dispatches last a few milliseconds and the GPU is left idle for a tenth of that
  after each one, so the desktop and other applications get their turn during a long bake.
- `PATINA_GPU=0` turns the GPU off, `PATINA_GPU=<part of an adapter name>` picks one, and `PATINA_GPU=warp` uses
  Direct3D's software rasterizer, which is how the tests exercise the kernels on machines without a GPU.
  macOS bakes on the CPU for now.

## Install

Paste this to your agent (Claude Code, Codex, or anything else with a shell):

```text
Install Patina for me (https://github.com/parodyband/patina).

1. Run the installer for this machine:
   - Windows: powershell -NoProfile -ExecutionPolicy Bypass -Command "irm https://raw.githubusercontent.com/parodyband/patina/main/install.ps1 | iex"
   - macOS (Apple silicon): curl -fsSL https://raw.githubusercontent.com/parodyband/patina/main/install.sh | sh
   It downloads the latest release, checks its SHA-256 and prints JSON describing what it set up.
2. In that JSON, find yourself under "agents" and check that "registered" is true. If you are not listed, or it
   is false, add the MCP server under "mcp_server" to your own configuration.
3. Run the binary at "exe" with `version`, tell me the version, and remind me to restart you so the Patina tools load.
```

Or run the installer yourself:

```bash
curl -fsSL https://raw.githubusercontent.com/parodyband/patina/main/install.sh | sh      # macOS (Apple silicon)
```

```powershell
irm https://raw.githubusercontent.com/parodyband/patina/main/install.ps1 | iex           # Windows
```

It puts the binary in `~/.patina/bin` (added to your PATH). For Claude Code and Codex, whichever are on the
machine, it writes the `patina` skill (`~/.claude/skills/patina`, `~/.codex/skills/patina`) and registers the MCP
server for every project. Any other agent can run `~/.patina/bin/patina mcp` as a stdio MCP server.

**Updating.** `patina update` downloads the newest release, checks it against the release's SHA-256 sums and
reinstalls, which also refreshes the skills and guide. Your agent runs it for you: when the MCP server starts it
looks for a newer release, at most once every six hours, and from then on asks the agent to run the update;
`patina version` tells you too. The new version is used the next time the agent starts. Set
`PATINA_NO_UPDATE_CHECK=1` to turn the check off. Patina never downloads or installs anything by itself.

From a source build, `build/patina install` does the same setup with your own binary. Flags: `--no-skill`,
`--no-mcp`, `--no-path`.

**Releasing.** Bump `VERSION` in `CMakeLists.txt`, then push a matching tag (`git tag v0.3.0 && git push --tags`).
CI builds and tests both platforms and publishes the binaries with `SHA256SUMS.txt` as a GitHub release. Agents
are told about a release within hours and asked to install it, so a tag is a deployment.

## Build

You need CMake ≥ 3.20 and a C++20 compiler: Xcode clang on macOS, or MSVC 2022 on Windows. There is no
package manager; dependencies are vendored single headers (cgltf, stb, sokol, Dear ImGui) and one git submodule,
[LightUSD](https://github.com/StrangeApeGames/LightUSD) (the library that was tinyusdz), which writes the USD export.

```bash
git submodule update --init  # third_party/LightUSD (or clone with --recursive)
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build          # ~5 s clean build on 18 cores, after LightUSD has been built once
bash tests/run_tests.sh      # end-to-end suite (~3 s)
```

The first build also compiles the 70 LightUSD files that export needs. That took 65 s with `-j 8` on a 16-core
desktop, and happens once: they are not rebuilt when Patina's own code changes. Without the submodule, or with
`-DPATINA_USD=OFF`, Patina builds without USD export and `export --usd` says so.

On Windows, run these from a "x64 Native Tools Command Prompt for VS 2022" (or a Developer PowerShell
with `-arch=x64`) so `cl` is on the path, and run the tests from Git Bash. The binary is `build\patina.exe`.
The build compiles the GPU bake kernels (`src/shaders/bake.hlsl`) with the Windows SDK's `fxc`, which sits
next to `rc.exe`; nothing is compiled at run time.

CI builds and tests on macOS (clang) and Windows (MSVC) on every push.

## Use it from an agent (MCP)

`patina install` registers the server for all projects. To register it by hand:

```bash
claude mcp add --scope user patina -- /path/to/patina mcp
```

This repo also ships a `.mcp.json` that points at your dev build (`build/patina`); inside the repo it takes
precedence over the installed one. On Windows the build moves a running `patina.exe` aside before linking, so
you can rebuild while Claude Code has the dev server open.

**Tools**

| Tool | What it does |
|---|---|
| `inspect` | describe a mesh |
| `new_project` | create a project |
| `get_project` | read a project |
| `edit` | apply edit operations |
| `render` | render a preview (returns an image) |
| `variants` | compare alternatives side by side |
| `bake` | bake mesh maps |
| `export` | write textures, and optionally a textured `.glb` or the mesh as USD |
| `library` | list materials, fields and presets, or read the guide with `topic="guide"` |
| `validate` | check a project |
| `batch` | run many commands at once |
| `blender` | drive Blender headless |

The server tells agents to read `library(topic="guide")`, i.e. [docs/AGENT_GUIDE.md](docs/AGENT_GUIDE.md), which is embedded in the binary.

## Use it from the CLI

Every command prints JSON.

```bash
patina inspect examples/assets/crate.glb
patina new work/crate.patina.json --mesh examples/assets/crate.glb --smart painted_metal
patina edit work/crate.patina.json '[{"op":"add","layer":{"id":"dust","type":"smart","material":"dust_overlay"}}]'
patina render work/crate.patina.json --views iso,front --mode lit      # -> work/renders/crate.png
patina render work/crate.patina.json --mode mask:dust                   # where does the dust go?
patina export work/crate.patina.json --preset unreal --glb
patina export work/crate.patina.json --preset gltf --usd usdz           # mesh + materials as USD (usda | usdc | usdz)
patina batch --do export a.patina.json b.patina.json c.patina.json -j 8
patina blender apply --project work/crate.patina.json --out crate.blend --render crate.png
patina view work/crate.patina.json                                      # native viewer
patina guide                                                            # the agent guide
```

## Viewer

`patina view <project>` opens a native window: Metal on macOS, D3D11 on Windows.

- It uses the same renderer agents see.
- It live-reloads whenever an agent edits the project, so a human can watch agents work.
- Controls: orbit, pan and zoom; switch channels and bake views.
- Click a layer to see its mask, or toggle layers on and off (viewer-only; nothing is written).

![viewer](docs/images/viewer.png)

## A project file

```json
{
  "patina": 1,
  "mesh": "../assets/panel.glb",
  "resolution": 2048,
  "texture_sets": {
    "Panel": {"layers": [
      {"id": "base", "type": "smart", "material": "painted_metal", "params": {"color": "#56616b", "wear": 0.45}},
      {"id": "stencil", "type": "decal", "image": "../images/stencil_p47.png", "position": [0.3, 1, 0.3], "facing": "up", "size": 0.34},
      {"id": "scratches", "type": "smart", "material": "scratches_overlay", "params": {"amount": 0.35}},
      {"id": "light", "channels": {"emissive": {"color": "#27e36b", "intensity": 3}},
       "mask": [{"type": "sphere", "center": [0.82, 1, 0.18], "radius": 0.035}]}
    ]}
  }
}
```

## Layout

| Path | What it holds |
|---|---|
| `src/mesh.*` | glTF/GLB/OBJ loading, tangents, UV islands, inspection |
| `src/bvh.*` | SAH BVH with early split clipping |
| `src/bake.*` | UV rasterization with gutter samples, AO and thickness (ray traced on a subsampled lattice), edge-integral curvature, padding, disk cache |
| `src/bake_normal.cpp` | high-to-low normal projection (cage, skew, bake groups), bevel shader, height map, AO, thickness and curvature of the high poly |
| `src/gpu.*`, `src/bake_gpu.*` | GPU compute layer (Direct3D 11) and the host side of the bake kernels |
| `src/shaders/bake.hlsl` | the bake kernels: BVH traversal and the ray-traced estimators, mirroring the CPU ones |
| `src/eval.*` | fields, generators, mask stacks, blend modes, height→normal |
| `src/library.cpp` | smart materials (parameterized templates) and export presets |
| `src/render.*` | tiled CPU PBR rasterizer and contact sheets |
| `src/export.*` | channel packing, manifest, textured GLB |
| `src/usd.*` | USD export (`.usda`, `.usdc`, `.usdz`): meshes and UsdPreviewSurface materials, written and read back through LightUSD |
| `third_party/LightUSD` | submodule: our fork of LightUSD, on the branch and commit the engine's cooker uses |
| `src/commands.*` | command implementations shared by the CLI and MCP; session caches |
| `src/mcp.cpp` | MCP server (stdio JSON-RPC, concurrent tool calls) |
| `src/selfupdate.*` | `install` / `update` and the release check: installed binary, agent skills, MCP registration, verified downloads. Identical to the copy in [Chisel](https://github.com/parodyband/chisel); change both together |
| `src/tool.cpp` | what `selfupdate` needs to know about Patina, and the Blender bridge an install carries |
| `src/viewer.cpp` | sokol + Dear ImGui viewer |
| `tools/blender/` | Blender bridge (`export` .blend→.glb, `apply` manifest→Principled BSDF) |
| `examples/` | test assets (generated by `examples/make_assets.py` in Blender) and showcase projects |
| `docs/` | agent guide, research notes |

See [docs/RESEARCH.md](docs/RESEARCH.md) for how Patina relates to Substance Painter, ArmorPaint and AI texture generators.
