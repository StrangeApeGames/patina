// Triangle meshes: loading (glTF/GLB/OBJ), tangents, topology, UV islands, inspection.
#pragma once
#include "core.h"
#include "json.h"

namespace pt {

struct Mesh {
  std::vector<vec3> pos;
  std::vector<vec3> nrm;
  std::vector<vec2> uv;   // glTF convention: (0,0) is the top-left of the texture image
  std::vector<vec4> tan;  // xyz tangent (+U), w = bitangent sign; bitangent points toward -V (image up)
  std::vector<uint32_t> idx;
  std::vector<uint16_t> tri_set;   // texture set (material) per triangle
  std::vector<uint16_t> tri_part;  // part (object/node) per triangle
  std::vector<std::string> set_names;
  std::vector<std::string> part_names;
  std::vector<int> tri_island;  // UV island per triangle (filled by compute_topology)
  int island_count = 0;
  vec3 bmin{0, 0, 0}, bmax{0, 0, 0};
  std::string path;
  uint64_t content_hash = 0;
  std::vector<std::string> warnings;

  size_t tri_count() const { return idx.size() / 3; }
  vec3 center() const { return (bmin + bmax) * 0.5f; }
  vec3 size() const { return bmax - bmin; }
  float max_extent() const { vec3 s = size(); return std::fmax(1e-6f, std::fmax(s.x, std::fmax(s.y, s.z))); }
  float radius() const { return length(size()) * 0.5f; }
  int find_set(const std::string& name) const;
  int find_part(const std::string& name) const;
};

// Loads .glb/.gltf/.obj. Node transforms are baked in (world space). Throws pt::Error.
// geometry_only skips tangents and UV islands: all a mesh that is only traced (a high poly) needs is its surface.
Mesh load_mesh(const std::string& path, bool geometry_only = false);
void compute_bounds(Mesh& m);
void compute_normals_if_missing(Mesh& m, bool had_normals);
void compute_tangents(Mesh& m);
void compute_topology(Mesh& m);  // UV islands

// Welded vertex ids by position (ignores UV/normal splits).
std::vector<uint32_t> weld_by_position(const Mesh& m, uint32_t* out_count);

// Structured description for agents: sets, parts (with bounds), UV health, etc.
Json mesh_info(const Mesh& m);

}  // namespace pt
