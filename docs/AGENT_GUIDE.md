# Patina agent guide

Patina textures 3D assets the way Substance Painter does (texture sets, layer stacks, masks,
generators, smart materials), but every step is a JSON edit plus a render you can look at.
This guide is everything an agent needs; `library` lists every parameter.

## 1. Mental model

| Substance Painter        | Patina                                                              |
|--------------------------|---------------------------------------------------------------------|
| Texture set              | `texture_sets.<MaterialName>` (one per material in the mesh)        |
| Bake mesh maps           | automatic + cached (AO, curvature, thickness, position, normals)    |
| Fill layer               | `{"channels": {...}, "mask": [...]}`                                 |
| Folder                   | `{"type": "folder", "layers": [...], "mask": [...]}`                 |
| Smart material           | `{"type": "smart", "material": "painted_metal", "params": {...}}`    |
| Black mask + generator   | `"mask": [{"type": "edge_wear", ...}]`                               |
| Paint / projection tool  | `paint` strokes (3D polylines) and `decal` projections               |
| Anchor point             | `{"type": "layer", "layer": "<id>"}` / `{"type": "stack", ...}`      |
| Viewport                 | `render` (returns an image), `patina view` for humans                |
| Export presets           | `export` with preset blender/gltf/unreal/unity_hdrp/unity_urp/godot  |

Layers are listed **bottom to top**: later layers paint over earlier ones.

## 2. Workflow

1. `inspect(mesh)`: read texture sets, parts (with normalized bounds), and UV warnings.
2. `new_project(project, mesh, smart="painted_metal")`: gives each texture set a starting material.
3. `edit(project, ops)`: add or tune layers. Edits are validated by evaluating before they are saved.
4. `render(project)`: look at the result. It is fast (~0.1–0.5 s), so do this after every meaningful edit.
   - Debug where a layer applies with `mode="mask:<layer_id>"`.
   - Check height work with `mode="clay"`.
   - Get close-ups with `views=[{"azimuth":30,"elevation":10,"zoom":3,"target":[0.5,0.9,0.5]}]` and a higher `resolution` (e.g. 2048): fine grain, rust and edge wear only read up close.
   - Lighting is a studio HDRI (`environment="studio"`). Turn it with `env_rotation` to move reflections across metal, or pass a path to your own equirectangular `.hdr`.
5. `variants(project, variants=[...])`: when unsure, render 2–6 alternatives in one call, then apply the best with `edit`.
6. `export(project, preset, glb=true)`: writes PNGs plus `manifest.json` (and a textured `.glb`). `usd=true` also writes the mesh as USD (see §10).
7. `blender(action="apply", project=..., out="x.blend", render="x.png")`: builds Principled BSDF materials.

## 3. Coordinates and units

- **Axes (glTF):** +Y up, +Z front, +X right. Blender's +Z up becomes +Y, and Blender's front (−Y) becomes +Z.
- **Points** default to **normalized bounding-box coordinates**: `[0,0,0]` is the min corner and `[1,1,1]` the max corner.
  - `[0.5, 1, 0.5]` is the center of the top.
  - `[0.5, 0.5, 1]` is the center of the front.
  - `inspect` gives each part's bbox in these coordinates.
- **Lengths** (radius, size, falloff, depth) are fractions of the object's largest dimension.
- Set `"space": "world"` on a field to use mesh units instead.
- **Noise `scale`** is the number of features across the object (object space), so it is size-independent.
- **UV space** (`"space":"uv"` on a noise field): x = U, y = V, and every UV island gets its own offset. Use it for
  boards, planks and staves laid out lengthwise along V: grain then follows each board, which 3D noise around one
  axis cannot do on a barrel or a floor. `scale` still means features per object size.
- **Directions** are `"up" | "down" | "front" | "back" | "left" | "right"` or `[x,y,z]`.

## 4. Project file

```json
{
  "patina": 1,
  "name": "crate",
  "mesh": "crate.glb",
  "resolution": 2048,
  "height_depth": 0.005,
  "bake": {"ao_samples": 48, "ao_distance": 0.3, "curvature_radius": 0.012},
  "export": {"preset": "blender", "dir": "textures/crate"},
  "texture_sets": {
    "Crate": {"resolution": 2048, "layers": [ ...bottom -> top... ]}
  }
}
```

- `height_depth`: world depth of height = 1, as a fraction of the object size. It controls normal-map strength.
- `curvature_radius` sets how wide "edges" are, for all edge generators.

## 5. Layers

**Fill layer (default type)**

```json
{"id": "paint", "name": "Paint",
 "channels": {"basecolor": "#2f5d8c", "roughness": 0.45, "metallic": 0, "height": 0.2},
 "mask": [ ...effects... ],
 "opacity": 1, "blend": "normal",
 "blend_modes": {"height": "add"}, "channel_opacity": {"roughness": 0.5},
 "enabled": true}
```

- A layer only affects the channels it lists; everything else passes through.
- Channels:

  | Channel | Values |
  |---|---|
  | `basecolor` | sRGB color |
  | `metallic` | 0..1 |
  | `roughness` | 0..1 |
  | `height` | 0 = neutral, ±values raise/lower |
  | `emissive` | color × intensity |
  | `opacity` | 0..1 |
  | `ao` | multiplier |
  | `normal` | normal-map image only |

**Channel values**

| Form | Meaning |
|---|---|
| `0.4` | constant scalar |
| `"#aa5533"`, `[0.6,0.3,0.2]` | constant sRGB color (named colors like `"rust"` or `"steel"` also work) |
| `{"type":"noise","noise":"fbm","scale":8,"range":[0.3,0.6]}` | a field remapped into a range (scalars) |
| `{"type":"noise","scale":6,"gradient":["#3a1d10","#8f4f27"]}` | a field mapped to colors (colors) |
| `{"color":"#27e36b","intensity":3}` | color with intensity (emissive) |
| `{"type":"image","path":"tex.png","projection":"triplanar","scale":4}` | tiling image; `uv`, `triplanar` or `planar` |
| `{"type":"image","path":"n.png","format":"opengl","strength":1}` | normal channel only |

**Other layer types**

- **Folder:** `{"type":"folder","layers":[...],"mask":[...]}`. The folder mask limits all of its children.
- **Smart material:**
  - `{"type":"smart","material":"rusted_metal","params":{"rust":0.7}}` expands into a folder.
  - Inner layer ids become `<id>/<inner>`, for example `mask:rust/streaks`.
  - A mask on the smart layer limits the whole material.
- **Decal:** `{"type":"decal","image":"logo.png","position":[0.5,0.6,1],"facing":"front","size":0.3,"up":"up","channels":{"roughness":0.3}}`.
  - It projects the image's colors (alpha = mask) from the facing side and is blocked by geometry closer to the projector.
  - Add `channels.basecolor` to use the image as a stencil only.

## 6. Masks

A mask is a stack of effects evaluated **bottom to top**, starting from black:

```json
"mask": [
  {"type": "dirt", "amount": 0.6},
  {"type": "grunge", "style": "smudge", "amount": 0.7, "blend": "multiply"},
  {"type": "box", "min": [0,0,0], "max": [1,0.3,1], "blend": "max", "opacity": 0.5}
]
```

- Each effect blends onto the result with `blend` (default `normal`, which replaces what is below) and `opacity`.
- Use `multiply` to intersect, `max`/`add` to union, and `subtract` to cut away.
- Patina warns when a `normal`-blended effect hides the effects below it.
- A layer with no mask covers everything.

**Generators and fields** (run `library(topic="fields")` for all parameters)

| Type | What it gives |
|------|---------------|
| `edge_wear` | worn convex edges: `amount`, `width`, `breakup` |
| `dirt` | grime in crevices: `amount` |
| `curvature` | `convex` / `concave` / `both` / `raw` |
| `ao`, `cavity`, `thickness` | baked maps |
| `gradient` | along an axis, `from`..`to` in bbox units (`"axis":"up"`) |
| `direction` | faces pointing a direction (dust/snow/moss on top) |
| `noise` | `fbm`, `perlin`, `value`, `ridged`, `turbulence`, `cells`, `voronoi`, `cracks`, `dots`, `white`, `rings` (wood rings around `axis`); 3D and seamless. `stretch` accepts `[x,y,z]` or a direction name such as `"up"` |
| `grunge` | `smudge`, `spots`, `patches`, `cracks`, `speckle`, `rust` |
| `scratches` | straight scratches: `density`, `length`, `width`, optional `direction` |
| `streaks` | vertical runs on walls (rust/water) |
| `sphere`, `box`, `plane` | 3D regions (bbox coords) with `falloff` |
| `select` | `parts` (names/globs from `inspect`) and/or UV `islands` |
| `island_random`, `part_random` | random value per UV island / part (per-plank variation) |
| `paint` | 3D brush strokes: `strokes:[{"points":[[x,y,z],...],"radius":0.03,"hardness":0.7}]` |
| `decal` | projected image alpha as a mask |
| `image` | image luminance/alpha in UV, triplanar or planar projection |
| `layer`, `stack` | reuse another layer's mask, or the accumulated channel below |
| `combine` | a nested mask stack used as one field |

**Modifiers on any field**

- They apply in this order: `blur` → `levels` [lo,hi] or {in,gamma,out} → `contrast` → `power` → `invert` → `threshold` → `multiply` → `add` → `clamp`.
- Example: `{"type":"cavity","levels":[0.1,0.5],"invert":true}`.

## 7. Recipes

**Worn painted metal with rust in crevices**

```json
[{"op":"add","set":"Body","layer":{"id":"paint","type":"smart","material":"painted_metal","params":{"color":"#3b5d2a","wear":0.5}}},
 {"op":"add","set":"Body","layer":{"id":"rust","type":"smart","material":"rust_overlay","params":{"amount":0.4}}}]
```

**Dust on top surfaces**: `{"type":"smart","material":"dust_overlay","params":{"amount":0.6}}`

**A logo on the front**

```json
{"id":"logo","type":"decal","image":"logo.png","position":[0.5,0.55,1],"facing":"front","size":0.35}
```

**Paint a stripe by hand (3D stroke, bbox coords)**

```json
{"id":"stripe","channels":{"basecolor":"#d8c21a","roughness":0.5},
 "mask":[{"type":"paint","strokes":[{"points":[[0,0.5,1],[1,0.5,1]],"radius":0.03,"hardness":0.8}]}]}
```

**Different material per part**

```json
{"id":"grip","channels":{"basecolor":"#1c1c1e","roughness":0.9},"mask":[{"type":"select","parts":["Handle*"]}]}
```

**Only the bottom 30% gets mud**

```json
"mask":[{"type":"gradient","axis":"up","from":0.3,"to":0.05,"breakup":0.3},{"type":"cavity","blend":"max","opacity":0.5}]
```

**Glowing panel light**

```json
"channels":{"emissive":{"color":"#27e36b","intensity":3}}
```

together with a `sphere` or `box` mask.

**Wear only in raised paint**: `{"type":"stack","channel":"height","levels":[0.05,0.2]}`

**Wood boards/staves with grain along each board** (UV islands laid out lengthwise along V)

```json
{"id":"oak","type":"smart","material":"wood","params":{"space":"uv","rings":40,"light":"#9c6c40","dark":"#52321b"}}
```

**Metals: base color is the reflectance.** Keep bare metal bright (`#8a8680`..`#c8c8c8`, metallic 1); a dark
metallic base renders black. For blackened or painted iron, put a dark non-metal layer (forge scale, paint) on
top and wear it off at the edges. For hoops, bands, hinges and fittings start from
`{"type":"smart","material":"wrought_iron","params":{"rust":0.4,"wear":0.5}}` (see `examples/projects/barrel.patina.json`).

**Mask coverage**: `range` remaps a mask effect before blending (clamped to 0..1): `[0.7, 1.8]` covers most of
the surface, `[-0.5, 1]` leaves gaps.

## 8. Look before you ship

- `render` returns an image, so check it.
  - Views: `front back left right top bottom iso iso_back iso_left iso_back_left low`, or `"az:el"`.
  - Modes:

    | Mode | Shows |
    |---|---|
    | `lit` | final look |
    | `clay` | shape / height detail |
    | `basecolor`, `roughness`, `metallic`, `normal`, `height`, `ao` | one channel |
    | `curvature`, `thickness`, `bake_ao` | bakes |
    | `mask:<id>` | where a layer applies, in red |
    | `islands`, `parts` | UV islands / mesh parts |
    | `uv_checker` | UV distortion |

  - `sheet=true` shows the flat texture maps.
- Iterate cheaply: previews render at `min(1024, resolution)` and bakes are cached per mesh and settings.
- Check that metals read as metal (metallic 1 with a mid-to-low roughness), paint is dielectric (metallic 0), and nothing is pure black or white in basecolor.

## 9. Parallel work

- MCP tool calls run concurrently. Texture many assets at once, one project each.
- `batch(jobs=[{"command":"export","args":{"project":"a.patina.json"}}, ...], parallel=8)`.
  - CLI equivalent: `patina batch --do export a.patina.json b.patina.json -j 8`.
- `variants` evaluates alternatives in parallel and returns one comparison image.
- Projects are independent files, so several agents can each own one; edits to one project are serialized.

## 10. Export presets

| Preset       | Files                                                                          | Normal  |
|--------------|--------------------------------------------------------------------------------|---------|
| `blender`    | BaseColor, Metallic, Roughness, Normal, Height (16-bit), AO, [Emissive, Opacity] | OpenGL  |
| `gltf`       | BaseColor(+A), ORM (R=AO G=Rough B=Metal), Normal, [Emissive]; `glb=true`      | OpenGL  |
| `unreal`     | T_BC, T_ORM, T_N, [T_E]                                                        | DirectX |
| `unity_hdrp` | BaseMap, MaskMap (R=Metal G=AO B=Detail A=Smooth), Normal                      | OpenGL  |
| `unity_urp`  | BaseMap, MetallicSmoothness, Occlusion, Normal                                 | OpenGL  |
| `godot`      | albedo, orm, normal                                                            | OpenGL  |
| `maps`       | every channel and bake as separate files                                       | OpenGL  |

- `manifest.json` lists every file per texture set. `blender(action="apply")` reads it.
- The normal map already contains the height detail.

### USD

`export(project, preset, usd=true)` (CLI `--usd`) also writes the mesh as USD next to the textures, for Blender, Maya,
Houdini, usdview or an engine's USD importer.

| `usd`              | File          | Use it for |
|--------------------|---------------|------------|
| `true` or `"usda"` | `<name>.usda` | Text you can read and diff. Reads the PNGs beside it. |
| `"usdc"`           | `<name>.usdc` | The same in binary, and much smaller: a quarter of the size for a 40k-triangle mesh. |
| `"usdz"`           | `<name>.usdz` | One file with the textures inside, to send or to view on its own. |

- **Mesh:** one `Mesh` per part under a root named after the project, +Y up, in meters, with normals and UVs (`st`). A
  part with several texture sets gets one `GeomSubset` per set.
- **Materials:** one `UsdPreviewSurface` material per texture set, in `<root>/Materials`, named after the set. Each one
  reads the maps the preset wrote, whatever their packing (base color, roughness, metallic, occlusion and normal,
  plus opacity and emissive when the set uses them).
- **Names:** USD names allow only letters, digits and `_`, so `Cube.001` becomes `Cube_001`. The original is kept as
  the prim's `displayName`.
- **Pick the preset for where the USD is going.** `gltf`, `godot` and `blender` store OpenGL normals and roughness,
  which is what USD expects, so they import everywhere. `unreal` (DirectX normals) and `unity_*` (smoothness) are
  read through a remap on the texture that some importers skip. The result's `usd.warnings` says so when it applies.
- The result's `usd` block has the path and what the file holds (`meshes`, `triangles`, `materials`, `textures`).
  Export reads every USD file back before it reports success, so those counts are what is in the file.

## 11. Custom smart materials

Put JSON files in `<project dir>/library/` or in a directory on `$PATINA_LIBRARY`:

```json
{"smart_materials": {"my_paint": {
  "description": "...",
  "params": {"color": {"default": "#aa3322", "doc": "paint color"}, "wear": {"default": 0.4}},
  "layers": [{"id": "p", "channels": {"basecolor": "${color}"}, "mask": [{"type": "edge_wear", "amount": "${wear}", "invert": true}]}]
}}}
```

- `"${param}"` substitutes a parameter value.
- `"${wear*0.5+0.1}"` evaluates arithmetic.
- `"extends": "other_material"` inherits another material's layers with new defaults.

## 12. Baking normals (high poly and bevel shader)

Add a `normal` block to the project's `bake` settings. The baked tangent-space normal (MikkTSpace, same as
Blender/Unreal/Unity) becomes the base of every texture set's normal map; layer height and normal detail
sit on top.

With a high poly, AO, thickness and curvature are measured on the high-poly surface too (as Toolbag bakes
them): each texel is measured where it lands on the high poly, so `edge_wear`, `dirt` and every other mask
follow its bevels, bolts and creases and not the low poly's edges. Without one (bevel only), the maps come
from the low poly and the bevelled normal adds to the curvature.

```json
"bake": {"normal": {"high": "crate_high.glb", "cage": 0.02, "depth": 0.02, "bevel": {"radius": 0.006}}}
```

| Key | Default | Meaning |
|---|---|---|
| `high` | none | high-poly mesh (path relative to the project). Leave it out to bake only the bevel shader from the low poly. |
| `match` | `"name"` | bake groups by part name: everything before a `high`/`low` token (also `hi`/`lo`/`hp`/`lp`, case-insensitive) is the group, so `Plate_low` bakes only from `Plate_high` and `Plate_high_bolts`. No bleed between parts. `"all"`: every low part sees the whole high mesh. |
| `cage` / `depth` | 0.02 / 0.02 | how far outside / inside the low surface rays search (fraction of object size). Raise them when `bake_misses` shows red. |
| `skew` | `"auto"` | ray direction. `auto` uses averaged (cage) normals at hard edges, so there are no gaps, and the low's own normal elsewhere, so details on big faces are not skewed. A number blends 0 = averaged, 1 = shading normal. |
| `skew_distance` | 0.03 | auto: distance from hard edges over which rays turn. |
| `samples` | 4 | supersamples per texel. |
| `bevel` | off | bevel shader: `{"radius": 0.006, "samples": 64, "min_angle": 10, "scope": "part"}` rounds edges sharper than `min_angle`. It runs on the high poly when there is one, otherwise on the low. `scope: "all"` also rounds where separate parts meet. |
| `curvature` | 1 | how much the baked normal adds to the low poly's curvature map (bevel-only bakes, or `curvature_from_high: false`). |
| `cage_mask` | none | paint the cage (Toolbag's offset map): `{"to": 0.06, "mask": [effects...]}` grows the cage toward `to` where the mask is 1. Any mask effect works: `paint` strokes, `box`, `sphere`, `select` parts, `gradient`. |
| `skew_mask` | none | paint the skew: `{"to": 1, "mask": [...]}` pushes skew toward `to` where the mask is 1. |
| `cage_mesh` | none | explicit cage: a pushed copy of the low with the same triangles (e.g. exported from Blender). Rays go from the cage through the low; `cage`, `skew` and the masks are ignored. |
| `ao_from_high` | true | AO and thickness of the high poly (bolts and creases occlude). The whole high mesh occludes, so parts ground each other. `false` keeps the low poly's. |
| `curvature_from_high` | true | curvature of the high-poly surface: soft where its bevels are wide, none along the low poly's facets, and details that only exist on the high poly show. Each separate piece of the high poly (a bolt pushed into a plate) has its own curvature. `false` keeps the low poly's hard edges plus what the baked normal adds. |
| `ignore_backfaces`, `denoise` | true | |

- Check `render(mode="bake_normal")`, `render(mode="bake_misses")`, `render(mode="bake_ao")` and `render(mode="curvature")` before texturing.
- `bake` reports where the maps came from in `stats.normal.ao` and `stats.normal.curvature` (`"high"` or `"low"`). A texel that finds no high-poly surface (a miss) keeps the low poly's values.
- Misses in one area only? Paint the cage there (`cage_mask`) instead of growing it everywhere, which invites bleed from neighbouring surfaces.
- Misses under a detail that rests on a surface (a bolt head, a strap) mean rays start inside it: the cage must clear the tallest detail.
- High sample counts multiply: `samples` x `bevel.samples` rays per texel near an edge. The defaults (4 x 64) are enough for almost everything; 64 x 1024 keeps a GPU busy for minutes.
- `bake` reports groups, misses and warnings (for example, a low part with no `_high` partner).
- Low-poly rules still apply: triangulate, split UVs at hard edges, keep the high poly inside the cage.
- A bevel-only bake is the fast path for hard-surface lows without a high poly: rounded edges and edge wear in one line.

**GPU baking.** AO, thickness, the high-to-low projection and the bevel are ray traced on the GPU when the machine
has one (Windows, Direct3D 11) and on all CPU cores otherwise; `bake` reports which in `stats.device`. Nothing
needs configuring: a GPU that fails mid-bake falls back to the CPU (`stats.gpu_error`), and both devices run the
same estimators, so their maps agree to float rounding. `bake(verify=true)` (`patina bake <project> --verify`)
bakes both ways and reports per-map differences, `ok` and the speedup. `PATINA_GPU=0` keeps every bake on the
CPU, for output that does not depend on a GPU or its driver; `PATINA_GPU=<part of an adapter name>` picks a GPU.

