// Direct3D 11 compute backend (see gpu.h). d3d11.dll and dxgi.dll are loaded on demand, so the binary still
// starts, and bakes on the CPU, on machines without them. The kernels are compiled to bytecode at build time.
#include "gpu.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <future>
#include <map>
#include <mutex>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

namespace pt {
namespace {

// src/shaders/bake.hlsl, one header per kernel (CMakeLists.txt)
#include "bake_surface.h"
#include "bake_ao_high.h"
#include "bake_project.h"
#include "bake_bevel.h"

struct Bytecode { const BYTE* data; size_t size; };
const Bytecode kKernels[(int)GpuKernel::Count] = {
    {g_bake_surface, sizeof g_bake_surface}, {g_bake_ao_high, sizeof g_bake_ao_high},
    {g_bake_project, sizeof g_bake_project}, {g_bake_bevel, sizeof g_bake_bevel},
};

template <class T>
struct Com {
  T* p = nullptr;
  Com() = default;
  Com(const Com&) = delete;
  Com& operator=(const Com&) = delete;
  ~Com() { reset(); }
  void reset() { if (p) { p->Release(); p = nullptr; } }
  T* operator->() const { return p; }
  T** out() { reset(); return &p; }
};

// Destroyed in reverse order at exit: g_device first, which waits for a warm-up still running, and that task
// ends by locking g_m and writing g_reason.
std::mutex g_m;                    // guards the state below
std::string g_reason;
std::atomic<bool> g_failed{false};
bool g_started = false;
std::shared_future<Gpu*> g_device;  // the device is intentionally leaked: it lives for the process

// PATINA_GPU, lowercased; "" = pick the best hardware adapter
std::string wanted() {
  const char* e = std::getenv("PATINA_GPU");
  std::string want = e ? to_lower(e) : "";
  if (want == "1" || want == "on" || want == "auto" || want == "true" || want == "yes") want.clear();
  return want;
}
bool is_off(const std::string& want) { return want == "0" || want == "off" || want == "cpu" || want == "false" || want == "no"; }

std::string narrow(const wchar_t* w) {
  int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  std::string s(n > 0 ? n - 1 : 0, '\0');
  if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
  return s;
}

struct D3D11Buffer : Gpu::Buffer {
  Com<ID3D11Buffer> buf;
  Com<ID3D11ShaderResourceView> srv;
};

class D3D11Gpu : public Gpu {
 public:
  Com<ID3D11Device> dev;
  Com<ID3D11DeviceContext> ctx;
  std::string name_;

  const std::string& name() const override { return name_; }
  const char* backend() const override { return "d3d11"; }

  BufferRef upload(const void* data, size_t count, uint32_t stride) override {
    auto b = std::make_shared<D3D11Buffer>();
    std::vector<uint8_t> dummy;
    if (count == 0) {  // Direct3D has no empty buffers
      dummy.assign(stride, 0);
      data = dummy.data();
      count = 1;
    }
    if (count * (uint64_t)stride > 0xffffffffull) lost(E_OUTOFMEMORY, "a buffer over 4 GB");
    D3D11_BUFFER_DESC d = {};
    d.ByteWidth = (UINT)(count * stride);
    d.Usage = D3D11_USAGE_IMMUTABLE;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    d.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    d.StructureByteStride = stride;
    D3D11_SUBRESOURCE_DATA init = {data, 0, 0};
    check(dev->CreateBuffer(&d, &init, b->buf.out()), strf("a %.0f MB buffer", d.ByteWidth / 1048576.0).c_str());
    check(dev->CreateShaderResourceView(b->buf.p, nullptr, b->srv.out()), "a buffer view");
    return b;
  }

  double run(GpuKernel kernel, const void* constants, uint32_t constants_size, const BufferRef* scene, int scene_count, const void* in,
             uint32_t in_stride, size_t n, void* out, uint32_t out_stride) override {
    if (n == 0) return 0;
    double ms = dispatch(kernel, constants, constants_size, scene, scene_count, in, in_stride, n, out, out_stride);
    rest(ms * kRestShare);
    return ms;
  }

  void trim() override {
    std::lock_guard<std::mutex> lk(m_);
    io_.clear();
    ctx->ClearState();
    ctx->Flush();  // Direct3D frees released buffers on the next flush
  }

 private:
  // A bake must not own the GPU: after every dispatch it is left idle for this share of the dispatch's time, so
  // the desktop and other applications get their turn (the baker keeps dispatches to a few milliseconds).
  static constexpr double kRestShare = 0.1;

  // Sleep() can oversleep by 15 ms; a high-resolution waitable timer (Windows 10 1803+) does not. Without
  // one there is no pause.
  static void rest(double ms) {
    if (ms < 0.2) return;
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
    if (!timer) return;
    LARGE_INTEGER due;
    due.QuadPart = -(LONGLONG)(std::min(ms, 5.0) * 10000.0);  // relative, in 100 ns
    if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(timer, 100);
    CloseHandle(timer);
  }

  double dispatch(GpuKernel kernel, const void* constants, uint32_t constants_size, const BufferRef* scene, int scene_count, const void* in,
                  uint32_t in_stride, size_t n, void* out, uint32_t out_stride) {
    std::lock_guard<std::mutex> lk(m_);
    if (g_failed) throw GpuError("the GPU device failed earlier");
    Timer timer;
    Com<ID3D11ComputeShader>& cs = shaders_[(int)kernel];
    if (!cs.p) check(dev->CreateComputeShader(kKernels[(int)kernel].data, kKernels[(int)kernel].size, nullptr, cs.out()), "a compute shader");

    Io& io = io_[((uint64_t)in_stride << 32) | out_stride];
    if (io.cap < n) {
      size_t cap = std::max<size_t>(n, 4096);
      io.cap = 0;  // if a buffer below cannot be created, the next run starts over instead of using half a set
      D3D11_BUFFER_DESC d = {};
      d.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
      d.ByteWidth = (UINT)(cap * in_stride);
      d.StructureByteStride = in_stride;
      d.Usage = D3D11_USAGE_DYNAMIC;
      d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
      d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
      check(dev->CreateBuffer(&d, nullptr, io.in.out()), "an input buffer");
      check(dev->CreateShaderResourceView(io.in.p, nullptr, io.in_srv.out()), "an input view");
      d.ByteWidth = (UINT)(cap * out_stride);
      d.StructureByteStride = out_stride;
      d.Usage = D3D11_USAGE_DEFAULT;
      d.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
      d.CPUAccessFlags = 0;
      check(dev->CreateBuffer(&d, nullptr, io.out.out()), "an output buffer");
      check(dev->CreateUnorderedAccessView(io.out.p, nullptr, io.out_uav.out()), "an output view");
      d.Usage = D3D11_USAGE_STAGING;
      d.BindFlags = 0;
      d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      check(dev->CreateBuffer(&d, nullptr, io.staging.out()), "a readback buffer");
      io.cap = cap;
    }
    if (!cb_.p || cb_size_ < constants_size) {
      D3D11_BUFFER_DESC d = {};
      d.ByteWidth = (constants_size + 15) & ~15u;
      d.Usage = D3D11_USAGE_DEFAULT;
      d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
      check(dev->CreateBuffer(&d, nullptr, cb_.out()), "a constant buffer");
      cb_size_ = d.ByteWidth;
    }
    std::vector<uint8_t> padded(cb_size_, 0);
    memcpy(padded.data(), constants, constants_size);
    ctx->UpdateSubresource(cb_.p, 0, nullptr, padded.data(), 0, 0);

    D3D11_MAPPED_SUBRESOURCE map = {};
    check(ctx->Map(io.in.p, 0, D3D11_MAP_WRITE_DISCARD, 0, &map), "writing the input buffer");
    memcpy(map.pData, in, n * in_stride);
    ctx->Unmap(io.in.p, 0);

    ID3D11ShaderResourceView* srvs[kSceneSlots + 1] = {};
    for (int k = 0; k < scene_count && k < kSceneSlots; k++)
      if (scene[k]) srvs[k] = static_cast<D3D11Buffer*>(scene[k].get())->srv.p;
    srvs[kSceneSlots] = io.in_srv.p;
    ctx->CSSetShader(cs.p, nullptr, 0);
    ctx->CSSetConstantBuffers(0, 1, &cb_.p);
    ctx->CSSetShaderResources(0, kSceneSlots + 1, srvs);
    ctx->CSSetUnorderedAccessViews(0, 1, &io.out_uav.p, nullptr);
    ctx->Dispatch((UINT)((n + 63) / 64), 1, 1);
    ID3D11UnorderedAccessView* no_uav = nullptr;
    ctx->CSSetUnorderedAccessViews(0, 1, &no_uav, nullptr);
    ID3D11ShaderResourceView* no_srvs[kSceneSlots + 1] = {};
    ctx->CSSetShaderResources(0, kSceneSlots + 1, no_srvs);

    D3D11_BOX box = {0, 0, 0, (UINT)(n * out_stride), 1, 1};
    ctx->CopySubresourceRegion(io.staging.p, 0, 0, 0, 0, io.out.p, 0, &box);
    check(ctx->Map(io.staging.p, 0, D3D11_MAP_READ, 0, &map), "reading the result");  // waits for the dispatch
    memcpy(out, map.pData, n * out_stride);
    ctx->Unmap(io.staging.p, 0);
    check(dev->GetDeviceRemovedReason(), "the dispatch");
    return timer.ms();
  }

  struct Io {  // per (input stride, output stride): reused across dispatches, grown on demand
    Com<ID3D11Buffer> in, out, staging;
    Com<ID3D11ShaderResourceView> in_srv;
    Com<ID3D11UnorderedAccessView> out_uav;
    size_t cap = 0;
  };
  std::mutex m_;  // the immediate context is single-threaded
  Com<ID3D11ComputeShader> shaders_[(int)GpuKernel::Count];
  std::map<uint64_t, Io> io_;
  Com<ID3D11Buffer> cb_;
  uint32_t cb_size_ = 0;

  [[noreturn]] void lost(HRESULT hr, const char* what) {
    HRESULT removed = dev->GetDeviceRemovedReason();
    std::string msg = strf("GPU (%s): %s failed (0x%08lx)", name_.c_str(), what, (unsigned long)hr);
    if (FAILED(removed)) {  // driver reset or crash: nothing on this device is usable any more
      msg += strf(", device removed (0x%08lx)", (unsigned long)removed);
      g_failed = true;
      std::lock_guard<std::mutex> lk(g_m);
      g_reason = msg;
    }
    throw GpuError(msg);
  }
  void check(HRESULT hr, const char* what) {
    if (FAILED(hr)) lost(hr, what);
  }
};

Gpu* create(std::string& reason) {
  std::string want = wanted();
  if (is_off(want)) { reason = "disabled by PATINA_GPU"; return nullptr; }
  bool warp = want == "warp" || want == "software";

  HMODULE d3d11 = LoadLibraryA("d3d11.dll"), dxgi = LoadLibraryA("dxgi.dll");
  auto create_device = d3d11 ? (PFN_D3D11_CREATE_DEVICE)GetProcAddress(d3d11, "D3D11CreateDevice") : nullptr;
  typedef HRESULT(WINAPI * CreateFactory)(REFIID, void**);
  auto create_factory = dxgi ? (CreateFactory)GetProcAddress(dxgi, "CreateDXGIFactory1") : nullptr;
  if (!create_device || !create_factory) { reason = "Direct3D 11 is not installed"; return nullptr; }

  // the hardware adapter with the most dedicated memory (the discrete GPU on hybrid laptops)
  Com<IDXGIAdapter1> best;
  std::string best_name;
  if (!warp) {
    Com<IDXGIFactory1> factory;
    if (FAILED(create_factory(__uuidof(IDXGIFactory1), (void**)factory.out()))) { reason = "DXGI is unavailable"; return nullptr; }
    SIZE_T best_mem = 0;
    for (UINT i = 0;; i++) {
      Com<IDXGIAdapter1> a;
      if (factory->EnumAdapters1(i, a.out()) == DXGI_ERROR_NOT_FOUND) break;
      DXGI_ADAPTER_DESC1 d = {};
      if (!a.p || FAILED(a->GetDesc1(&d)) || (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) continue;
      if (d.VendorId == 0x1414 && d.DeviceId == 0x8c) continue;  // Microsoft Basic Render Driver: WARP, not always flagged as software (VMs)
      std::string nm = narrow(d.Description);
      if (!want.empty() && to_lower(nm).find(want) == std::string::npos) continue;
      if (!best.p || d.DedicatedVideoMemory > best_mem) {
        best_mem = d.DedicatedVideoMemory;
        best_name = nm;
        best.reset();
        best.p = a.p;
        a.p = nullptr;
      }
    }
    if (!best.p) { reason = want.empty() ? "no hardware GPU found" : "no GPU matches PATINA_GPU=" + want; return nullptr; }
  }

  auto gpu = std::make_unique<D3D11Gpu>();
  D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0, got = {};
  HRESULT hr = create_device(best.p, warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, &level, 1, D3D11_SDK_VERSION,
                             gpu->dev.out(), &got, gpu->ctx.out());
  if (FAILED(hr)) {
    reason = strf("%s cannot create a Direct3D 11 device (0x%08lx)", warp ? "WARP" : best_name.c_str(), (unsigned long)hr);
    return nullptr;
  }
  gpu->name_ = warp ? "Microsoft WARP (software)" : best_name;
  return gpu.release();
}

}  // namespace

bool Gpu::enabled() { return !is_off(wanted()); }

void Gpu::warm_up() {
  std::lock_guard<std::mutex> lk(g_m);
  if (g_started) return;
  g_started = true;
  try {
    g_device = std::async(std::launch::async, [] {
      std::string reason;
      Gpu* g = create(reason);
      std::lock_guard<std::mutex> lk2(g_m);
      g_reason = reason;
      return g;
    }).share();
  } catch (const std::exception&) {  // no thread to start it on: no GPU for this process
    std::promise<Gpu*> none;
    none.set_value(nullptr);
    g_device = none.get_future().share();
    g_reason = "cannot start a thread for the GPU device";
  }
}

Gpu* Gpu::get() {
  warm_up();
  std::shared_future<Gpu*> device;
  {
    std::lock_guard<std::mutex> lk(g_m);
    device = g_device;
  }
  Gpu* g = device.get();
  return g_failed ? nullptr : g;
}

std::string Gpu::unavailable_reason() {
  Gpu* g = get();
  std::lock_guard<std::mutex> lk(g_m);
  return g ? "" : g_reason;
}

}  // namespace pt
