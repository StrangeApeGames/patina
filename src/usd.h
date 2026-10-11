// USD export (.usda, .usdc, .usdz) through LightUSD, the library that was tinyusdz (third_party/LightUSD, our fork).
// One Mesh prim per part and one UsdPreviewSurface material per texture set, reading the images the export preset wrote.
// Only usd.cpp sees the library.
#pragma once
#include <string>
#include <vector>

#include "json.h"
#include "mesh.h"

namespace pt {

// An image the export preset wrote, as a USD material reads it. A texel is read as value * scale + bias, which
// is how a normal map becomes a vector, DirectX green is flipped, and smoothness is read as roughness.
struct UsdImage {
  std::string key;   // the preset's map key; names the texture node
  std::string file;  // file name, next to the .usd file
  bool srgb = false;
  float scale[4] = {1, 1, 1, 1};
  float bias[4] = {0, 0, 0, 0};
};

// Where one material input comes from. image < 0: the preset does not export it, so it keeps USD's default.
struct UsdInput {
  int image = -1;             // index into UsdMaterial::images
  const char* channels = "";  // "rgb", "r", "g", "b" or "a"
};

struct UsdMaterial {
  int set = 0;       // the mesh's texture set this material is bound to
  std::string name;  // the texture set's name
  std::vector<UsdImage> images;
  UsdInput basecolor, opacity, metallic, roughness, occlusion, normal, emissive;
};

// "usda" (text), "usdc" (binary) or "usdz" (one file with the images inside).
std::vector<std::string> usd_formats();

// False in a build without LightUSD (the submodule was not checked out, or -DPATINA_USD=OFF).
bool usd_available();

// Writes the mesh and its materials to `path`, then reads the file back and fails unless every mesh, triangle,
// material and image is still there. For usdz the images are read from the folder `path` is in. Returns
// {format, meshes, triangles, materials, textures, bytes}. Throws pt::Error.
Json write_usd(const std::string& path, const std::string& format, const std::string& name, const Mesh& m,
               const std::vector<UsdMaterial>& materials);

}  // namespace pt
