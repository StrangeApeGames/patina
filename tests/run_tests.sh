#!/usr/bin/env bash
# End-to-end tests for the patina binary. usage: tests/run_tests.sh [path/to/patina]
set -euo pipefail
cd "$(dirname "$0")/.."
BIN=${1:-build/patina}
# Windows often has only `python` (and a Store stub named python3 that fails).
if [ -z "${PY:-}" ]; then python3 -c "" 2>/dev/null && PY=python3 || PY=python; fi
W=ci_work
export PATINA_NO_UPDATE_CHECK=1  # no network calls from the MCP server during tests
rm -rf "$W" && mkdir -p "$W"
pass=0
ok() { pass=$((pass + 1)); echo "  ok  $1"; }
fail() { echo "  FAIL $1"; exit 1; }
json() { "$PY" -c "import json,sys; d=json.load(sys.stdin); print($1)"; }

echo "== patina tests ($BIN)"
"$BIN" version >/dev/null && ok "version"

# inspect every example asset
for a in crate barrel beer_barrel hammer suzanne panel; do
  tris=$("$BIN" inspect "examples/assets/$a.glb" --compact | json "d['triangles']")
  [ "$tris" -gt 0 ] || fail "inspect $a"
done
ok "inspect 6 assets"

# new + edit + validate
"$BIN" new "$W/crate.patina.json" --mesh examples/assets/crate.glb --resolution 512 --smart painted_metal --compact >/dev/null || fail "new"
"$BIN" edit "$W/crate.patina.json" '[{"op":"add","layer":{"id":"dust","type":"smart","material":"dust_overlay"}},
  {"op":"add","layer":{"id":"stripe","channels":{"basecolor":"#d8c21a"},"mask":[{"type":"paint","strokes":[{"points":[[0,0.5,1],[1,0.5,1]],"radius":0.03}]}]}},
  {"op":"update","id":"painted_metal","patch":{"params":{"wear":0.7}}},
  {"op":"move","id":"stripe","index":0}]' --compact >/dev/null || fail "edit"
ok "new + edit"
[ "$("$BIN" validate "$W/crate.patina.json" --compact | json "d['ok']")" = "True" ] || fail "validate"
ok "validate"

# errors are reported as JSON with a non-zero exit code and helpful suggestions
set +e
out=$("$BIN" edit "$W/crate.patina.json" '[{"op":"add","layer":{"channels":{"basecolour":"#fff"}}}]' --compact)
code=$?
set -e
[ $code -ne 0 ] || fail "bad edit should fail"
echo "$out" | grep -q "did you mean 'basecolor'" || fail "typo suggestion"
ok "typo -> did-you-mean error, project left untouched"

# render all modes
for mode in lit clay wireframe basecolor roughness metallic normal height ao curvature thickness bake_ao islands uv_checker mask:dust; do
  "$BIN" render "$W/crate.patina.json" --views iso --size 128 --mode "$mode" --out "$W/r_${mode/:/_}.png" --compact >/dev/null || fail "render $mode"
done
ok "render 15 modes"
"$BIN" render "$W/crate.patina.json" --sheet --size 96 --out "$W/sheet.png" --compact >/dev/null || fail "sheet"
"$BIN" render "$W/crate.patina.json" --uv-layout --size 128 --out "$W/uvs.png" --compact >/dev/null || fail "uv layout"
ok "texture sheet + UV layout"

# every smart material evaluates
mats=$("$BIN" library --topic smart_materials --compact | json "' '.join(d['smart_materials'].keys())")
for m in $mats; do
  "$BIN" edit "$W/crate.patina.json" "[{\"op\":\"replace\",\"layers\":[{\"id\":\"m\",\"type\":\"smart\",\"material\":\"$m\"}]}]" --compact >/dev/null || fail "smart material $m"
done
ok "all smart materials evaluate ($(echo $mats | wc -w | tr -d ' '))"

# export presets + determinism
"$BIN" new "$W/barrel.patina.json" --mesh examples/assets/barrel.glb --resolution 256 --smart rusted_metal --compact >/dev/null
for preset in blender gltf unreal unity_hdrp unity_urp godot maps; do
  "$BIN" export "$W/barrel.patina.json" --preset "$preset" --out "$W/tex_$preset" --compact >/dev/null || fail "export $preset"
  [ -f "$W/tex_$preset/manifest.json" ] || fail "manifest $preset"
done
ok "7 export presets"
"$BIN" export "$W/barrel.patina.json" --preset gltf --glb --out "$W/det1" --compact >/dev/null
"$BIN" bake "$W/barrel.patina.json" --force --compact >/dev/null
"$BIN" export "$W/barrel.patina.json" --preset gltf --glb --out "$W/det2" --compact >/dev/null
for f in "$W"/det1/*.png "$W"/det1/*.glb; do
  cmp -s "$f" "$W/det2/$(basename "$f")" || fail "non-deterministic output: $(basename "$f")"
done
ok "exports are bit-identical across runs (including a forced re-bake)"

# USD export through LightUSD: text, binary and one-file package. The mesh has one part with two materials, which
# USD binds through GeomSubsets, names that are not valid USD prim names, and (being an OBJ) a "Default" texture
# set with no triangles, which must get no material.
cat > "$W/usd_mesh.obj" <<'EOF'
o Body.001
v 0 0 0
v 1 0 0
v 1 1 0
v 0 1 0
vt 0.05 0.05
vt 0.45 0.05
vt 0.45 0.45
vt 0.05 0.45
vn 0 0 1
usemtl Wood Planks
f 1/1/1 2/2/1 3/3/1
usemtl 2nd-Iron
f 1/1/1 3/3/1 4/4/1
o Lid
v 0 0 1
v 1 0 1
v 0 1 1
vt 0.55 0.55
vt 0.95 0.55
vt 0.55 0.95
f 5/5/1 6/6/1 7/7/1
EOF
"$BIN" new "$W/usd.patina.json" --mesh "$W/usd_mesh.obj" --resolution 64 --smart painted_metal --compact >/dev/null || fail "new usd project"
"$BIN" edit "$W/usd.patina.json" '[{"op":"add","set":"Wood Planks","layer":{"id":"glow","channels":{"emissive":"#ff8800","opacity":0.5}}}]' --compact >/dev/null || fail "edit usd project"
set +e; out=$("$BIN" export "$W/usd.patina.json" --preset gltf --usd --out "$W/usd_a" --compact); set -e
if echo "$out" | grep -q "no USD export"; then
  # the submodule was not checked out, or -DPATINA_USD=OFF. A CI build must have it.
  [ -z "${CI:-}" ] || fail "CI build without USD export"
  echo "  skip USD export (this build has no LightUSD)"
else
  # 2 meshes, 3 triangles, 2 materials, 7 images: base color, ORM and normal each, plus the glow's emissive.
  # export itself reads every file back with LightUSD before it reports these.
  usd_counts() { json "(d['usd']['format'], d['usd']['meshes'], d['usd']['triangles'], d['usd']['materials'], d['usd']['textures'], 'warnings' in d['usd'])"; }
  [ "$(echo "$out" | usd_counts)" = "('usda', 2, 3, 2, 7, False)" ] || fail "usd export: $out"
  [ "$(echo "$out" | json "d['usd']['path'] in d['files']")" = "True" ] || fail "usd file not listed"
  for f in usdc usdz; do
    out=$("$BIN" export "$W/usd.patina.json" --preset gltf --usd $f --out "$W/usd_a" --compact) || fail "export --usd $f: $out"
    [ "$(echo "$out" | usd_counts)" = "('$f', 2, 3, 2, 7, False)" ] || fail "usd $f export: $out"
  done
  grep -q '"usd"' "$W/usd_a/manifest.json" || fail "usd missing from the manifest"
  # the layout of each file, and OpenUSD itself (with its compliance checker) where a pxr module or Blender is installed
  "$PY" tests/usd_check.py --expect 2,3,2,7 "$W/usd_a/usd.usda" "$W/usd_a/usd.usdc" "$W/usd_a/usd.usdz" || fail "usd files"
  for want in 'def Mesh "Body_001"' 'displayName = "Body.001"' 'def GeomSubset "Wood_Planks"' 'def Material "_2nd_Iron"' \
              'subsetFamily:materialBind:familyType = "partition"' 'inputs:scale = (2, 2, 2, 1)' 'inputs:opacity.connect' 'inputs:emissiveColor.connect'; do
    grep -qF "$want" "$W/usd_a/usd.usda" || fail "usda has no: $want"
  done
  ! grep -q 'def Material "Default"' "$W/usd_a/usd.usda" || fail "a texture set with no triangles got a material"
  # the Lid's own three points, and its UVs as the OBJ has them: an OBJ's V runs up the image like USD's, so a
  # wrong flip between Patina's top-left UVs and USD's would show here
  "$PY" - "$W/usd_a/usd.usda" <<'EOF' || fail "usd mesh data"
import re, sys
lid = open(sys.argv[1]).read().split('def Mesh "Lid"')[1]
def values(decl):
    return [float(v) for v in re.findall(r"-?\d+\.?\d*(?:e-?\d+)?", re.search(re.escape(decl) + r" = \[([^\]]*)\]", lid).group(1))]
same = lambda got, want: len(got) == len(want) and all(abs(a - b) < 1e-6 for a, b in zip(got, want))
ok = same(values("point3f[] points"), [0, 0, 1, 1, 0, 1, 0, 1, 1]) and same(values("normal3f[] normals"), [0, 0, 1] * 3) \
    and same(values("texCoord2f[] primvars:st"), [0.55, 0.55, 0.95, 0.55, 0.55, 0.95]) and values("int[] faceVertexIndices") == [0, 1, 2]
sys.exit(0 if ok else 1)
EOF
  for f in usda usdc usdz; do
    "$BIN" export "$W/usd.patina.json" --preset gltf --usd $f --out "$W/usd_b" --compact >/dev/null
    cmp -s "$W/usd_a/usd.$f" "$W/usd_b/usd.$f" || fail "non-deterministic output: usd.$f"
  done
  # other packings are read through the texture's scale and bias, and export says which importers may not follow
  out=$("$BIN" export "$W/usd.patina.json" --preset unreal --usd --out "$W/usd_unreal" --compact)
  grep -qF 'inputs:scale = (2, -2, 2, 1)' "$W/usd_unreal/usd.usda" || fail "DirectX normals must have green flipped"
  [ "$(echo "$out" | json "len(d['usd']['warnings'])")" = "1" ] || fail "DirectX preset warning"
  out=$("$BIN" export "$W/usd.patina.json" --preset unity_urp --usd --out "$W/usd_unity" --compact)
  grep -qF 'inputs:roughness.connect = </usd/Materials/Wood_Planks/Texture_metallic_smoothness.outputs:a>' "$W/usd_unity/usd.usda" || fail "roughness from smoothness"
  grep -qF 'inputs:scale = (1, 1, 1, -1)' "$W/usd_unity/usd.usda" || fail "smoothness must be inverted"
  [ "$(echo "$out" | json "len(d['usd']['warnings'])")" = "1" ] || fail "smoothness preset warning"
  out=$("$BIN" export "$W/usd.patina.json" --preset blender --usd usdc --out "$W/usd_blender" --compact)
  [ "$(echo "$out" | usd_counts)" = "('usdc', 2, 3, 2, 12, False)" ] || fail "usd with separate maps: $out"
  # exports run concurrently, and LightUSD must only ever be in one of them at a time
  r=$("$BIN" batch --do export "$W/crate.patina.json" "$W/barrel.patina.json" "$W/usd.patina.json" --preset gltf --usd usdz -j 3 --compact | json "d['succeeded']")
  [ "$r" = "3" ] || fail "concurrent usd exports"
  # a flag after --usd is not its value, and a format that does not exist says which do
  "$BIN" export "$W/usd.patina.json" --usd -o "$W/usd_flag" --compact >/dev/null && [ -f "$W/usd_flag/usd.usda" ] || fail "--usd followed by -o"
  set +e; out=$("$BIN" export "$W/usd.patina.json" --usd usdx --out "$W/usd_bad" --compact); code=$?; set -e
  [ $code -ne 0 ] && echo "$out" | grep -q "unknown usd format 'usdx'" || fail "unknown usd format must be reported"
  ok "USD export: usda, usdc, usdz, material subsets, packed presets, deterministic"
fi

# batch in parallel
"$BIN" new "$W/suzanne.patina.json" --mesh examples/assets/suzanne.glb --resolution 256 --smart leather --compact >/dev/null
r=$("$BIN" batch --do render "$W/crate.patina.json" "$W/barrel.patina.json" "$W/suzanne.patina.json" --size 128 -j 3 --compact | json "d['succeeded']")
[ "$r" = "3" ] || fail "batch"
ok "batch render x3"

# normal baking: high -> low with _low/_high name matching, anti-skew, bevel shader
cat > "$W/bake.patina.json" <<EOF
{"patina": 1, "name": "bake", "mesh": "../examples/assets/boltplate_low.glb", "resolution": 256,
 "bake": {"normal": {"high": "../examples/assets/boltplate_high.glb", "cage": 0.03, "depth": 0.03, "bevel": {"radius": 0.004, "samples": 16}}},
 "texture_sets": {"Plate": {"layers": [{"id": "p", "type": "smart", "material": "painted_metal"}]},
                  "Bar": {"layers": [{"id": "b", "type": "smart", "material": "iron"}]}}}
EOF
out=$("$BIN" bake "$W/bake.patina.json" --force --compact)
[ "$(echo "$out" | json "d['stats']['normal']['miss_fraction'] == 0")" = "True" ] || fail "normal bake misses"
[ "$(echo "$out" | json "sorted((g['low'], g['high'][0]) for g in d['stats']['normal']['groups'])")" = "[('Bar_low', 'Bar_high'), ('Plate_low', 'Plate_high')]" ] || fail "bake groups by name"
[ "$(echo "$out" | json "len(d['stats']['normal']['warnings'])")" = "0" ] || fail "bake warnings"
for mode in bake_normal bake_misses lit; do
  "$BIN" render "$W/bake.patina.json" --views iso --size 96 --mode $mode --out "$W/bake_$mode.png" --compact >/dev/null || fail "render $mode"
done
sed 's/"cage": 0.03, "depth": 0.03, "bevel": {"radius": 0.004, "samples": 16}/"skew": 0.5, "match": "all"/' "$W/bake.patina.json" > "$W/bake_fixed.patina.json"
"$BIN" bake "$W/bake_fixed.patina.json" --force --compact >/dev/null || fail "bake with fixed skew"
sed 's/"high": "..\/examples\/assets\/boltplate_high.glb", //' "$W/bake.patina.json" > "$W/bevel.patina.json"
"$BIN" export "$W/bevel.patina.json" --preset gltf --out "$W/tex_bevel" --compact >/dev/null || fail "bevel-only bake + export"
ok "normal bake: name-matched groups, no misses, anti-skew, bevel shader"

# explicit cage mesh, painted cage (mask stack), AO traced from the high poly
sed 's/"cage": 0.03, "depth": 0.03/"cage_mesh": "..\/examples\/assets\/boltplate_cage.glb", "depth": 0.03/' "$W/bake.patina.json" > "$W/bake_cage.patina.json"
out=$("$BIN" bake "$W/bake_cage.patina.json" --force --compact)
[ "$(echo "$out" | json "(d['stats']['normal']['cage'], d['stats']['normal']['miss_fraction'] == 0, d['stats']['normal']['ao'])")" = "('mesh', True, 'high')" ] || fail "cage mesh bake"
sed 's/"cage": 0.03, "depth": 0.03/"cage": 0.005, "depth": 0.03/' "$W/bake.patina.json" > "$W/bake_tight.patina.json"
sed 's/"cage": 0.03, "depth": 0.03/"cage": 0.005, "depth": 0.03, "cage_mask": {"to": 0.03, "mask": [{"type": "select", "parts": ["Bar_low"]}]}/' "$W/bake.patina.json" > "$W/bake_painted.patina.json"
tight=$("$BIN" bake "$W/bake_tight.patina.json" --force --compact | json "d['stats']['normal']['sets']['Bar']['miss_fraction']")
painted=$("$BIN" bake "$W/bake_painted.patina.json" --force --compact | json "d['stats']['normal']['sets']['Bar']['miss_fraction']")
"$PY" -c "import sys; sys.exit(0 if float('$painted') < float('$tight') and float('$tight') > 0 else 1)" || fail "painted cage should fix misses ($tight -> $painted)"
ok "explicit cage mesh, painted cage mask, AO from the high poly"

# with a high poly the mesh maps describe its surface: the crease around the plate's raised panel only exists
# there, so it is in the curvature map when that comes from the high poly and not when it comes from the low
out=$("$BIN" bake "$W/bake.patina.json" --force --dump "$W/maps_high" --compact)
[ "$(echo "$out" | json "(d['stats']['normal']['ao'], d['stats']['normal']['curvature'])")" = "('high', 'high')" ] || fail "mesh maps should come from the high poly"
sed 's/"cage": 0.03,/"cage": 0.03, "curvature_from_high": false, "ao_from_high": false,/' "$W/bake.patina.json" > "$W/bake_lowmaps.patina.json"
out=$("$BIN" bake "$W/bake_lowmaps.patina.json" --force --dump "$W/maps_low" --compact)
[ "$(echo "$out" | json "(d['stats']['normal']['ao'], d['stats']['normal']['curvature'])")" = "('low', 'low')" ] || fail "mesh maps should stay on the low poly"
concave_high=$("$PY" tests/map_stats.py "$W/maps_high/Plate_curvature.png" | cut -d' ' -f1)
concave_low=$("$PY" tests/map_stats.py "$W/maps_low/Plate_curvature.png" | cut -d' ' -f1)
"$PY" -c "import sys; sys.exit(0 if float('$concave_high') > 0.005 and float('$concave_low') < 0.001 else 1)" || fail "curvature should follow the high poly (concave: $concave_high from the high, $concave_low from the low)"
ok "AO, thickness and curvature from the high poly"

# GPU baker: `bake --verify` bakes on the GPU and on the CPU and compares every map. Runs on this machine's GPU
# when it has one, otherwise on Direct3D's software rasterizer (CI), and is skipped where there is no backend.
gpu_of() { "$BIN" bake "$1" --verify --compact | json "d['verify']['gpu']"; }
gpu_env=${PATINA_GPU-}
[ "$(gpu_of "$W/crate.patina.json")" != "None" ] || export PATINA_GPU=warp
if [ "$(gpu_of "$W/crate.patina.json")" != "None" ]; then
  # AO + thickness, projection with the maps on the high poly (auto skew, fixed skew, cage mesh) and on the low, bevel
  for p in crate bake bake_fixed bake_cage bake_lowmaps bevel; do
    out=$("$BIN" bake "$W/$p.patina.json" --verify --compact)
    [ "$(echo "$out" | json "d['verify']['ok']")" = "True" ] || fail "GPU and CPU bakes of $p differ: $out"
  done
  [ "$("$BIN" bake "$W/crate.patina.json" --force --compact | json "d['stats']['device']")" = "gpu" ] || fail "bake should use the GPU"
  [ "$("$BIN" bake "$W/crate.patina.json" --compact | json "(d['stats']['device'], d['stats']['from_cache'])")" = "('gpu', True)" ] || fail "GPU bake cache"
  [ "$(PATINA_GPU=0 "$BIN" bake "$W/crate.patina.json" --compact | json "d['stats']['device']")" = "cpu" ] || fail "PATINA_GPU=0 must bake on the CPU"
  ok "GPU baker matches the CPU baker ($(gpu_of "$W/crate.patina.json"))"
else
  why=$("$BIN" bake "$W/crate.patina.json" --verify --compact | json "d['verify']['reason']")
  # a Windows build must have the backend: without fxc at configure time it is left out with only a warning
  case "$(uname -s):$why" in MINGW*backend*|MSYS*backend*|CYGWIN*backend*) fail "Windows build without the GPU baker: $why" ;; esac
  echo "  skip GPU baker ($why)"
fi
[ -n "$gpu_env" ] && export PATINA_GPU=$gpu_env || unset PATINA_GPU

# example project: UV-space wood grain, mask range, all three lighting environments
[ "$("$BIN" validate examples/projects/barrel.patina.json --compact | json "d['ok']")" = "True" ] || fail "validate barrel example"
[ "$("$BIN" validate examples/projects/beer_barrel.patina.json --compact | json "d['ok']")" = "True" ] || fail "validate beer barrel example"
"$BIN" render examples/projects/beer_barrel.patina.json --views iso --size 128 --resolution 256 --out "$W/beer.png" --compact >/dev/null || fail "render beer barrel"
for env in studio procedural third_party/hdri/studio_small_09_1k.hdr; do
  "$BIN" render examples/projects/barrel.patina.json --views iso --size 128 --resolution 256 --environment "$env"     --out "$W/barrel_env.png" --compact >/dev/null || fail "render environment $env"
done
set +e; out=$("$BIN" render examples/projects/barrel.patina.json --size 64 --resolution 128 --environment nope.hdr --out "$W/x.png" --compact); set -e
echo "$out" | grep -q "not found" || fail "missing environment must be reported"
ok "barrel example: uv-space wood, studio/procedural/.hdr environments"

# MCP
"$PY" tests/mcp_smoke.py "$BIN" "$W/mcp" >/dev/null || fail "mcp smoke"
ok "mcp smoke (concurrent calls)"

# install + update into a sandbox home (no PATH or MCP registration); the "release" is served over file:// URLs
EXE="$BIN"; [ -f "$EXE.exe" ] && EXE="$EXE.exe"
export PATINA_HOME="$W/home/.patina" CLAUDE_CONFIG_DIR="$W/home/.claude"
"$BIN" install --no-path --no-mcp --compact >/dev/null || fail "install"
for f in "$CLAUDE_CONFIG_DIR/skills/patina/SKILL.md" "$CLAUDE_CONFIG_DIR/skills/patina/AGENT_GUIDE.md" \
         "$PATINA_HOME/tools/blender/patina_blender.py"; do
  [ -f "$f" ] || fail "install did not write $f"
done
grep -q '^name: patina$' "$CLAUDE_CONFIG_DIR/skills/patina/SKILL.md" || fail "skill frontmatter"
ok "install (binary, skill, blender bridge)"
case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*) ASSET=patina-windows-x64.exe ;;
  Darwin) [ "$(uname -m)" = arm64 ] && ASSET=patina-macos-arm64 || ASSET= ;;
  *) ASSET= ;;
esac
if [ -n "$ASSET" ]; then
  rel="$W/release"; mkdir -p "$rel"; cp "$EXE" "$rel/$ASSET"
  release_json() {  # $1 = checksum to publish (defaults to the real one)
    "$PY" - "$rel" "$ASSET" "${1:-}" <<'EOF'
import hashlib, json, pathlib, sys
rel, asset, digest = pathlib.Path(sys.argv[1]).resolve(), sys.argv[2], sys.argv[3]
digest = digest or hashlib.sha256((rel / asset).read_bytes()).hexdigest()
(rel / "SHA256SUMS.txt").write_text(f"{digest}  {asset}\n")
assets = [{"name": n, "browser_download_url": (rel / n).as_uri()} for n in (asset, "SHA256SUMS.txt")]
(rel / "latest.json").write_text(json.dumps({"tag_name": "v99.0.0", "html_url": "", "assets": assets}))
print((rel / "latest.json").as_uri())
EOF
  }
  export PATINA_RELEASES_API=$(release_json)
  [ "$("$BIN" update --check --compact | json "d['update_available']")" = "True" ] || fail "update --check"
  [ "$("$BIN" update --no-path --no-mcp --compact | json "d['updated_to']")" = "99.0.0" ] || fail "update"
  export PATINA_RELEASES_API=$(release_json 0000000000000000000000000000000000000000000000000000000000000000)
  set +e; out=$("$BIN" update --no-path --no-mcp --compact); code=$?; set -e
  [ $code -ne 0 ] && echo "$out" | grep -q "checksum mismatch" || fail "bad checksum must be rejected"
  unset PATINA_RELEASES_API
  ok "update: check, verified install, checksum mismatch rejected"
fi
unset PATINA_HOME CLAUDE_CONFIG_DIR

echo "== $pass passed"
