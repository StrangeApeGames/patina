// Texture export (preset-driven channel packing), manifest.json, and textured GLB and USD output.
#pragma once
#include "eval.h"
#include "project.h"

namespace pt {

// Writes textures for all texture sets. Returns a JSON summary (files, manifest path, timings).
// usd_format: "" for none, or "usda", "usdc" or "usdz" to also write the mesh with materials that read those
// textures (see usd.h).
Json export_textures(const Project& p, const Baked& bk, const std::vector<SetResult>& results, const std::string& preset,
                     const std::string& out_dir, bool write_glb, const std::string& usd_format = "");

// Standalone GLB with embedded PBR textures (baseColor, ORM, normal, emissive).
void write_textured_glb(const std::string& path, const Mesh& m, const std::vector<SetMaps>& maps, float height_depth_world);

}  // namespace pt
