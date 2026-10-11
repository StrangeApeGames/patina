// Bounding volume hierarchy over triangles (binned SAH) for AO/thickness baking, decal occlusion and preview shadows.
#pragma once
#include "core.h"

namespace pt {

struct BVH {
  struct Node {
    float bmin[3];
    uint32_t left_first;  // interior: index of left child (right = left+1); leaf: first triangle
    float bmax[3];
    uint32_t count;  // 0 = interior
  };
  struct Tri { vec3 v0, e1, e2; };

  std::vector<Node> nodes;
  std::vector<Tri> tris;          // in BVH order
  std::vector<uint32_t> tri_id;   // original triangle index per BVH-ordered triangle

  void build(const std::vector<vec3>& pos, const std::vector<uint32_t>& idx);
  bool empty() const { return nodes.empty(); }
  // Which side of a triangle a ray may hit: its front (geometric normal against the ray) or its back.
  enum Facing { kAny, kFront, kBack };
  // Any hit with tmin < t < tmax.
  bool occluded(vec3 o, vec3 d, float tmin, float tmax, Facing facing = kAny) const;
  // Closest hit; returns false if none. `tri` is the original triangle index.
  bool intersect(vec3 o, vec3 d, float tmin, float tmax, float& t, uint32_t& tri, float& u, float& v, Facing facing = kAny) const;
};

}  // namespace pt
