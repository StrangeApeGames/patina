#include "bake_gpu.h"

#include <algorithm>

namespace pt {

static_assert(sizeof(GpuParams) == 64 && sizeof(GpuSample) == 48 && sizeof(GpuBevelIn) == 48, "bake.hlsl layout");
static_assert(sizeof(GpuProjIn) == 112 && sizeof(GpuProjOut) == 32 && sizeof(GpuTriAttr) == 48, "bake.hlsl layout");
static_assert(sizeof(BVH::Node) == 32 && sizeof(BVH::Tri) == 36, "bake.hlsl layout");

uint32_t GpuScene::add(const BVH& bvh, const std::vector<uint32_t>* orig) {
  Entry e{(uint32_t)nodes_.size(), (uint32_t)tris_.size(), (uint32_t)bvh.nodes.size(), 0};
  if (nodes_.size() + bvh.nodes.size() > 0xffffffffull || tris_.size() + bvh.tris.size() > 0xffffffffull)
    throw GpuError("the meshes are too large for the GPU baker");
  nodes_.insert(nodes_.end(), bvh.nodes.begin(), bvh.nodes.end());
  tris_.insert(tris_.end(), bvh.tris.begin(), bvh.tris.end());
  orig_.reserve(orig_.size() + bvh.tri_id.size());
  for (uint32_t t : bvh.tri_id) orig_.push_back(orig ? (*orig)[t] : t);
  bvhs_.push_back(e);
  return (uint32_t)bvhs_.size() - 1;
}

void GpuScene::upload(Gpu& gpu) {
  b_nodes_ = gpu.upload(nodes_.data(), nodes_.size(), sizeof(BVH::Node));
  b_tris_ = gpu.upload(tris_.data(), tris_.size(), sizeof(BVH::Tri));
  b_orig_ = gpu.upload(orig_.data(), orig_.size(), sizeof(uint32_t));
  b_bvhs_ = gpu.upload(bvhs_.data(), bvhs_.size(), sizeof(Entry));
  nodes_ = {};
  tris_ = {};
  orig_ = {};
}

void GpuScene::bind(Gpu::BufferRef out[Gpu::kSceneSlots]) const {
  out[0] = b_nodes_; out[1] = b_tris_; out[2] = b_orig_; out[3] = b_bvhs_;
  out[4] = attr; out[5] = edge_cells; out[6] = edge_list; out[7] = edge_segs;
}

void gpu_run(Gpu& gpu, GpuKernel kernel, GpuParams params, const GpuScene& scene, size_t n, uint32_t in_stride, uint32_t out_stride,
             uint64_t rays, const std::function<void(size_t, size_t, void*)>& fill,
             const std::function<void(size_t, size_t, const void*)>& store) {
  if (n == 0) return;
  Gpu::BufferRef bufs[Gpu::kSceneSlots];
  scene.bind(bufs);
  constexpr double kTargetMs = 8.0;  // short enough for the desktop to draw its frames in between
  constexpr uint64_t kMaxRays = 32u << 20;  // per dispatch: well under a second even on an integrated GPU
  const size_t kMax = std::min({size_t(1) << 20, (size_t(64) << 20) / std::max(in_stride, out_stride),
                                (size_t)std::max<uint64_t>(kMaxRays / std::max<uint64_t>(rays, 1), 16)});
  const size_t kMin = std::min<size_t>(1024, kMax);
  size_t chunk = std::min<size_t>(4096, kMax);
  std::vector<uint8_t> in, out;
  for (size_t first = 0; first < n;) {
    size_t count = std::min(chunk, n - first);
    in.resize(count * in_stride);
    out.resize(count * out_stride);
    fill(first, count, in.data());
    params.count = (uint32_t)count;
    double ms = gpu.run(kernel, &params, sizeof params, bufs, Gpu::kSceneSlots, in.data(), in_stride, count, out.data(), out_stride);
    store(first, count, out.data());
    // the first dispatch also pays for the driver compiling the kernel, so it may only grow the chunk
    double f = std::clamp(kTargetMs / std::max(ms, 0.01), first == 0 ? 1.0 : 0.25, 4.0);
    first += count;
    chunk = std::clamp<size_t>((size_t)(count * f), kMin, kMax);
  }
}

}  // namespace pt
