// Host side of the GPU bake kernels (src/shaders/bake.hlsl): the records the kernels read and write, BVHs
// flattened into device buffers, and a chunked runner. The estimators themselves live next to their CPU
// versions in bake.cpp and bake_normal.cpp.
#pragma once
#include <functional>

#include "bvh.h"
#include "gpu.h"

namespace pt {

// Constant buffer of every kernel (cbuffer Params).
struct GpuParams {
  uint32_t count = 0;    // elements in this dispatch (set by gpu_run)
  uint32_t samples = 0;  // AO rays per sample, or bevel samples
  float maxd = 0;        // AO ray length
  float eps = 0;
  uint32_t bvh = 0;      // BVH traced by the surface and AO-from-high kernels
  uint32_t grid = 1;     // project: supersamples per axis
  uint32_t flags = 0;    // kGpu* below
  float depth = 0;
  float bevel_r = 0;
  float edge_cell = 1;   // feature-edge grid
  float edge_radius = 0;
  uint32_t edge_mask = 0;
  uint32_t thickness_samples = 0;
  float thickness_maxd = 0;
  uint32_t pad[2] = {};
};
enum : uint32_t { kGpuAutoSkew = 1, kGpuCageMesh = 2, kGpuBackfaces = 4, kGpuAoHigh = 8, kGpuBevelDetect = 16 };

struct GpuSample { vec3 pos; uint32_t seed; vec3 nrm; float pad0; vec3 fnrm; float pad1; };  // AO and thickness in
struct GpuSurfaceOut { uint32_t ao; float thickness; };                                       // ... and out
struct GpuHit { vec3 p; uint32_t scramble; vec3 n; float pad; };                             // AO from the high poly
struct GpuBevelIn { vec3 pos; uint32_t seed; vec3 ns; uint32_t bvh; vec3 ng; float pad; };
struct GpuProjIn {
  vec3 pos; uint32_t seed;
  vec3 tg; float len_u;
  vec3 bt; float len_v;
  vec3 ns; float cg;
  vec3 nc; uint32_t group;
  vec3 dir; float pad0;
  vec3 pc; float pad1;
};
struct GpuProjOut { vec3 n; uint32_t flags; vec3 hit_p; float pad0; vec3 hit_n; float pad1; };
enum : uint32_t { kGpuOutMiss = 1, kGpuOutBevel = 2, kGpuOutHit = 4, kGpuOutNear = 8 };
struct GpuTriAttr { vec3 n0; uint32_t bevel_bvh; vec3 n1; uint32_t pad1; vec3 n2; uint32_t pad2; };
struct GpuEdgeCell { uint32_t key_lo, key_hi, start, count; };
struct GpuSeg { vec3 a; float pad0; vec3 b; float pad1; };

// Any number of BVHs in one set of device buffers; kernels pick one by the index add() returned.
struct GpuScene {
  // `orig` maps the BVH's triangle ids to triangles of the traced mesh (null = identity).
  uint32_t add(const BVH& bvh, const std::vector<uint32_t>* orig = nullptr);
  void upload(Gpu& gpu);  // call once after the last add(); frees the CPU copies

  // slots 4..7: per-triangle attributes and the feature-edge grid (projection and bevel kernels only)
  Gpu::BufferRef attr, edge_cells, edge_list, edge_segs;
  // The scene buffers in slot order, for Gpu::run.
  void bind(Gpu::BufferRef out[Gpu::kSceneSlots]) const;

 private:
  struct Entry { uint32_t node_base, tri_base, node_count, pad; };
  std::vector<BVH::Node> nodes_;
  std::vector<BVH::Tri> tris_;
  std::vector<uint32_t> orig_;
  std::vector<Entry> bvhs_;
  Gpu::BufferRef b_nodes_, b_tris_, b_orig_, b_bvhs_;
};

// Runs `kernel` over n elements in chunks: fill(first, count, dst) writes `count` input records for elements
// first.., store(first, count, src) receives their output records. Chunks are sized to take a few milliseconds
// each: Windows resets a GPU that does not answer for 2 s, and the desktop stutters long before that.
// `rays` is the most rays one element can trace; it caps a chunk whatever the previous ones took, because
// cheap elements (rays that leave the mesh at once) can be followed by expensive ones.
void gpu_run(Gpu& gpu, GpuKernel kernel, GpuParams params, const GpuScene& scene, size_t n, uint32_t in_stride, uint32_t out_stride,
             uint64_t rays, const std::function<void(size_t, size_t, void*)>& fill,
             const std::function<void(size_t, size_t, const void*)>& store);

}  // namespace pt
