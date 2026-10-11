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

# GPU baker: `bake --verify` bakes on the GPU and on the CPU and compares every map. Runs on this machine's GPU
# when it has one, otherwise on Direct3D's software rasterizer (CI), and is skipped where there is no backend.
gpu_of() { "$BIN" bake "$1" --verify --compact | json "d['verify']['gpu']"; }
gpu_env=${PATINA_GPU-}
[ "$(gpu_of "$W/crate.patina.json")" != "None" ] || export PATINA_GPU=warp
if [ "$(gpu_of "$W/crate.patina.json")" != "None" ]; then
  for p in crate bake bake_fixed bake_cage bevel; do  # AO + thickness, projection (auto skew, fixed skew, cage mesh), bevel
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
export PATINA_HOME="$W/home/.patina" CLAUDE_CONFIG_DIR="$W/home/.claude" CODEX_HOME="$W/home/.codex"
"$BIN" install --no-path --no-mcp --compact >/dev/null || fail "install"
for f in "$CLAUDE_CONFIG_DIR/skills/patina/SKILL.md" "$CLAUDE_CONFIG_DIR/skills/patina/AGENT_GUIDE.md" \
         "$CODEX_HOME/skills/patina/SKILL.md" "$CODEX_HOME/skills/patina/AGENT_GUIDE.md" \
         "$PATINA_HOME/tools/blender/patina_blender.py"; do
  [ -f "$f" ] || fail "install did not write $f"
done
grep -q '^name: patina$' "$CLAUDE_CONFIG_DIR/skills/patina/SKILL.md" || fail "skill frontmatter"
ok "install (binary, Claude Code and Codex skills, blender bridge)"
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
  ok "update: check, verified install, checksum mismatch rejected"

  # auto-update: when the installed binary's MCP server starts it finds the release, verifies it and installs it in
  # the background. An install shows as the skill being written again.
  INSTALLED="$PATINA_HOME/bin/$(basename "$EXE")"
  SKILL="$CLAUDE_CONFIG_DIR/skills/patina/SKILL.md"
  STATE="$PATINA_HOME/update_check.json"
  serve() {  # $1 = binary: runs its MCP server until its update check has finished (or 30 s pass)
    rm -f "$STATE" "$SKILL"
    ( echo '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}'
      for _ in $(seq 150); do [ -f "$STATE" ] && [ ! -d "$PATINA_HOME/update.lock" ] && break; sleep 0.2; done ) | "$1" mcp >/dev/null
  }
  unset PATINA_NO_UPDATE_CHECK
  export PATINA_RELEASES_API=$(release_json)
  rm -rf "$CODEX_HOME/skills/patina"  # a skill the user removed stays removed
  serve "$INSTALLED"
  [ -f "$SKILL" ] || fail "the MCP server did not install the newer release"
  [ ! -d "$CODEX_HOME/skills/patina" ] || fail "an unattended update must only renew the skills that are installed"
  [[ "$("$INSTALLED" version)" != *"is available"* ]] || fail "an installed release must not be announced"
  export PATINA_NO_AUTO_UPDATE=1
  serve "$INSTALLED"
  [ ! -f "$SKILL" ] || fail "PATINA_NO_AUTO_UPDATE must not install"
  [[ "$("$INSTALLED" version)" == *"99.0.0 is available"* ]] || fail "PATINA_NO_AUTO_UPDATE must still announce the release"
  unset PATINA_NO_AUTO_UPDATE
  serve "$BIN"
  [ ! -f "$SKILL" ] || fail "a build that is not the installed copy must not install"
  export PATINA_RELEASES_API=$(release_json 0000000000000000000000000000000000000000000000000000000000000000)
  serve "$INSTALLED"
  [ ! -f "$SKILL" ] || fail "a release with a bad checksum must not be installed"
  [[ "$("$INSTALLED" version)" == *"automatic update failed: checksum mismatch"* ]] || fail "a failed auto-update must be reported"
  unset PATINA_RELEASES_API
  export PATINA_NO_UPDATE_CHECK=1
  ok "auto-update: installs in the background, opt-out, dev builds, checksum mismatch reported"
fi
unset PATINA_HOME CLAUDE_CONFIG_DIR CODEX_HOME

echo "== $pass passed"
