// What the shared installer and updater (selfupdate.cpp) need to know about Patina, and the Blender bridge that an
// install carries besides the binary.
#include "commands.h"
#include "core.h"
#include "selfupdate.h"

namespace pt {

const char* patina_version() { return PATINA_VERSION; }

std::string patina_home() { return tool_home(); }

const ToolInfo& tool_info() {
  static const ToolInfo t = {
      .name = "patina",
      .title = "Patina",
      .version = PATINA_VERSION,
      .repo = "parodyband/patina",
      .env = "PATINA",
      .asset_windows_x64 = "patina-windows-x64.exe",  // see .github/workflows/build.yml
      .asset_macos_arm64 = "patina-macos-arm64",
      .skill_description =
          "Texture 3D models with Patina, a headless Substance-Painter-style PBR texturing engine (layer stacks, "
          "smart materials, edge wear/dirt/rust masks, paint strokes, decals, previews). Use when asked to texture, paint, "
          "weather or age a 3D asset (.glb/.gltf/.obj or a Blender object), or to make PBR texture maps for Blender, Unreal, "
          "Unity, Godot, glTF or USD.",
      .guide_summary = "recipes, every mask field, export presets and common mistakes",
  };
  return t;
}

static const unsigned char kBridgeScript[] = {
#include "patina_blender.inc"
};

std::string installed_bridge_script() {
  std::string path = path_join(patina_home(), "tools/blender/patina_blender.py");
  std::string cur, want = (const char*)kBridgeScript;
  if (!read_file(path, cur) || cur != want) {
    make_dirs(path_dir(path));
    write_file_or_throw(path, want);
  }
  return path;
}

void install_extras(Json& result) { result.set("blender_bridge", installed_bridge_script()); }

}  // namespace pt
