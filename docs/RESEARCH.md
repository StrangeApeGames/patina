# Research notes: why Patina looks the way it does

These are condensed notes from the research behind Patina's design (September 2026). Sources are inline.
Items marked *(unverified)* came from third-party sources or convention, not official docs.

## Substance 3D Painter's model, and how Patina maps it

**Texture sets and channels**
- Painter has one texture set per material. Each set has its own resolution, channels and baked maps.
- Default channels: Base Color, Roughness, Metallic, Normal, Height, plus optional Opacity/Emissive/User channels ([docs](https://substance3d.adobe.com/documentation/spdoc/texture-set-settings-29130771.html)).
- **Patina:** `texture_sets.<material>` with basecolor/metallic/roughness/normal/height/ao/emissive/opacity.

**Layers**
- Painter has paint, fill, folder and instance layers, each with a blend mode and opacity per channel.
- Effects stack on content or on masks: generator, paint, fill, levels, filter, anchor point, compare mask, color selection ([effects](https://experienceleague.adobe.com/en/docs/substance-3d-painter/using/effects/effects)).
- **Patina:** fill / folder / smart / decal layers. Masks are effect stacks with blend and opacity. Paint is 3D strokes. Anchors are `layer` / `stack` fields.

**Baked mesh maps**
- Painter bakes normal, world-space normal, ID, AO, curvature, position, thickness, height, bent normals and opacity ([identifiers](https://experienceleague.adobe.com/en/docs/substance-3d-painter/using/content/creating-custom-effects/mesh-map)).
- Its generators consume curvature, AO, world-space normal, position and thickness *(per-generator mapping from [CG Journal](https://cg-journal.com/en/substance-3d-painter-generator-all-list/), unverified)*.
- **Patina bakes:**
  - AO and thickness, ray traced with a BVH.
  - Curvature, from an edge integral over dihedral angles.
  - Position, normals and UV islands, taken straight from the surface samples.

**GPU baking**
- Painter added GPU raytracing for the AO and thickness bakers in 2019.2: DXR when available, otherwise OptiX. It can be turned off in the baking options, which is also the advice when a high poly does not fit in video memory and the bake fails ([2019.2 notes](https://experienceleague.adobe.com/en/docs/substance-3d-painter/using/release-notes/old-versions/version-2019-2)). Adobe puts the gain at 25x or more ([GPU raytracing](https://helpx.adobe.com/substance-3d-bake/features/gpu-raytracing.html), *unverified*: the page did not load, the figure is from a search summary).
- Toolbag's baker is GPU-accelerated, sorts meshes into bake groups, and has painted cage offset and skew ([baking](https://marmoset.co/toolbag/baking)). Toolbag 4 gave it a new ray tracing backend with native support for RTX cards that also runs on "all modern GPUs" ([CG Channel](https://www.cgchannel.com/2021/04/marmoset-ships-toolbag-4-0/)).
- **Patina:** the ray-traced maps (AO, thickness, high-to-low projection, bevel) run in compute shaders with a software BVH, so any DX11 GPU works, like Toolbag's backend on cards without ray tracing hardware. The CPU baker stays as the reference, as in Painter: it takes over when there is no GPU or the GPU fails, without a setting to flip, and `bake --verify` compares the two.

**Blend modes**
- About 25 standard modes, plus Normal Map Combine/Detail (Whiteout/RNM) for normals, all in linear space ([list](https://experienceleague.adobe.com/en/docs/substance-3d-painter/using/interface/layer-stack/blending-modes)).
- **Patina:** 13 standard modes, plus `combine` (RNM) for normals.

**Export templates**
- There are about 35 defaults ([presets](https://experienceleague.adobe.com/en/docs/substance-3d-painter/using/export/output-templates/default-output-templates/default-presets)).
- Common packing conventions:
  - Unreal ORM: R=AO, G=Rough, B=Metal, DirectX normal.
  - Unity HDRP mask map: R=Metal, G=AO, B=Detail, A=Smoothness ([Unity](https://docs.unity3d.com/Packages/com.unity.render-pipelines.high-definition@17.0/manual/Mask-Map-and-Detail-Map.html)).
  - glTF ORM with OpenGL normals.
- **Patina:** the blender, gltf, unreal, unity_hdrp, unity_urp, godot and maps presets.

**Automation and headless use**
- Painter has a Python API (layerstack/source modules since 10.0) and remote scripting over HTTP (`--enable-remote-scripting`, port 60041) ([docs](https://experienceleague.adobe.com/en/docs/substance-3d-painter/using/scripting-and-development/scripts-and-plugins/remote-control-with-scripting)).
- It has **no headless mode**: it always opens a GUI and needs a GPU.
- A request for a CLI with exit codes remains open ([feature request](https://community.adobe.com/feature-requests-60/run-python-script-at-startup-1186065)).

## Existing agent tooling, and the gaps Patina fills

**Substance Painter MCP servers**
- [diffdaff](https://github.com/diffdaff/substance-painter-mcp), [elliezu](https://github.com/elliezu/SubstancePainterMCP) and [dcc-mcp](https://github.com/dcc-mcp/dcc-mcp-substance3d-painter) all drive one live GUI instance, one operation at a time.
- The most complete one has no screenshot tool, so agents work blind.

**[blender-mcp](https://github.com/ahujasid/blender-mcp)**
- It covers material assignment, Poly Haven textures, arbitrary bpy code and viewport screenshots.
- It has no procedural texturing layer model, and it supports a single instance.

**Substance Automation Toolkit** ([sbsbaker](https://substance3d.adobe.com/documentation/sat/command-line-tools/sbsbaker), sbsrender)
- It is truly headless, but it only handles Designer graphs.
- It is enterprise-licensed.

**[ArmorPaint](https://www.cgchannel.com/2026/09/armorpaint-1-0-is-out-after-eight-years-in-early-access/)**
- 1.0 shipped in September 2026 under the zlib licence.
- It is the closest open-source analogue, with `--background`/`--script` CLI flags, but it is GUI-first.

**AI texture generators**

| Tool | Limitation |
|---|---|
| [Meshy retexture](https://docs.meshy.ai/en/api/retexture) | no seed |
| [Hunyuan3D-Paint](https://github.com/Tencent-Hunyuan/Hunyuan3D-2.1) | re-unwraps UVs with xatlas |
| [Stable Fast 3D](https://arxiv.org/abs/2408.00653) | single roughness/metal value per object |

These tools generate color images. They don't give editable, layered, deterministic PBR material stacks on your UVs.
- **Patina:** decals and `image` fields let an agent bring generated images *into* a layered material.

**Documented agent pain points**
- No visual feedback ([BlenderGym](https://arxiv.org/abs/2504.01786): VLM agents struggle with material edits without verification).
- GUI-bound tools.
- Non-determinism.
- Destroyed UVs.
- Seams and multi-view inconsistency.

## Design decisions

1. **Headless by default, CPU first.**
   - Agents run on servers and in CI; a GPU and a window are optional.
   - Everything (bake, evaluate, render) runs on all CPU cores.
   - The baker also runs its ray tracing on a GPU when there is one. The CPU path is the reference: the GPU kernels mirror it statement by statement and must match it to float rounding.
   - The viewer uses the GPU only to put the CPU-rendered frame on screen, so humans and agents see identical pixels.
2. **Declarative documents over imperative sessions.**
   - A project file is the whole state. Edits are JSON ops validated by evaluation before saving.
   - Many agents can work on many projects at once without a shared GUI session.
3. **Surface-space procedurals.**
   - Every noise or grunge pattern is evaluated in 3D at the texel's surface position. This gives Substance's triplanar-quality seamlessness for free.
   - Generators work in normalized bounding-box space, so parameters transfer between assets of any scale.
4. **Curvature from dihedral edges, not vertex normals.**
   - Hard-surface low-poly meshes (most game assets and AI-generated meshes) have almost no vertex-curvature signal.
   - The edge integral gives crisp convex/concave masks that are resolution- and tessellation-independent.
5. **Visual verification is a first-class tool.**
   - `render` returns images: lit, clay, per-channel, per-layer mask, UV debug.
   - `variants` compares alternatives in one call.
6. **Native, fast-building C++.**
   - Vendored single-header deps and a ~5 s clean build.
   - One binary provides the CLI, the MCP server and the viewer.
