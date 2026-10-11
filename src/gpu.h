// Minimal GPU compute layer for the baker: structured buffers in, one compute kernel, a structured buffer out.
// Backend: Direct3D 11 compute shaders on Windows, which run on any DX11 GPU. Elsewhere Gpu::get() is null and
// the baker runs on the CPU.
#pragma once
#include <memory>
#include <string>

#include "core.h"

namespace pt {

// The device was lost, ran out of memory or rejected a call. The baker catches this and re-bakes on the CPU.
struct GpuError : Error {
  using Error::Error;
};

enum class GpuKernel { Surface, AoHigh, Project, Bevel, Count };

class Gpu {
 public:
  struct Buffer { virtual ~Buffer() = default; };  // immutable structured buffer on the device
  using BufferRef = std::shared_ptr<Buffer>;
  static constexpr int kSceneSlots = 8;  // kernels read scene buffers from slots 0..7 and their input from slot 8

  // The process-wide device, or nullptr when there is none: no hardware adapter, PATINA_GPU=0, a build
  // without a backend, or a device that failed earlier in this process. Thread-safe.
  // PATINA_GPU: 0|off|cpu = never, warp = software rasterizer (tests), anything else = adapter name substring.
  static Gpu* get();
  static std::string unavailable_reason();  // why get() is null ("" when it is not)
  // False when get() is certain to be null (PATINA_GPU=0 or no backend). Cheap: never creates the device.
  static bool enabled();
  // Starts creating the device on a background thread (a driver takes ~0.2 s to start); get() waits for it.
  // Call it as soon as a bake is known to be needed, then do other work.
  static void warm_up();

  virtual ~Gpu() = default;
  virtual const std::string& name() const = 0;     // adapter description
  virtual const char* backend() const = 0;         // "d3d11"
  virtual BufferRef upload(const void* data, size_t count, uint32_t stride) = 0;
  // Runs `kernel` over n elements and blocks until the result is back. `scene[k]` binds to slot k (null entries
  // are skipped), `in` (n * in_stride bytes) to slot 8 and `constants` to the constant buffer; n * out_stride
  // bytes are written to `out`. Safe to call from many threads (calls are serialized). Returns the milliseconds
  // the device took, without the wait for other callers. Throws GpuError.
  virtual double run(GpuKernel kernel, const void* constants, uint32_t constants_size, const BufferRef* scene, int scene_count,
                     const void* in, uint32_t in_stride, size_t n, void* out, uint32_t out_stride) = 0;
  // Frees the input and output buffers run() keeps between calls. The baker calls it after a bake, so an idle
  // MCP server holds no video memory.
  virtual void trim() = 0;
};

}  // namespace pt
