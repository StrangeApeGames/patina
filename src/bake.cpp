#include "bake.h"

#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <mutex>
#include <thread>

#include "bake_gpu.h"

namespace pt {

// ---------------------------------------------------------------- settings
void BakeSettings::from_json(const Json& j) {
  if (!j.is_object()) return;
  ao_samples = std::clamp(j.integer("ao_samples", ao_samples), 4, 1024);
  ao_distance = std::clamp(j.numf("ao_distance", ao_distance), 1e-4f, 10.f);
  thickness_samples = std::clamp(j.integer("thickness_samples", thickness_samples), 4, 512);
  thickness_distance = std::clamp(j.numf("thickness_distance", thickness_distance), 1e-4f, 10.f);
  curvature_radius = std::clamp(j.numf("curvature_radius", curvature_radius), 1e-4f, 0.5f);
  curvature_gain = std::clamp(j.numf("curvature_gain", curvature_gain), 0.01f, 100.f);
  curvature_min_angle = std::clamp(j.numf("curvature_min_angle", curvature_min_angle), 0.f, 90.f);
  if (const Json* n = j.find("normal")) normal.from_json(*n);
}
Json BakeSettings::to_json() const {
  Json j = Json::object();
  j.set("ao_samples", ao_samples);
  j.set("ao_distance", ao_distance);
  j.set("thickness_samples", thickness_samples);
  j.set("thickness_distance", thickness_distance);
  j.set("curvature_radius", curvature_radius);
  j.set("curvature_gain", curvature_gain);
  j.set("curvature_min_angle", curvature_min_angle);
  if (normal.active()) j.set("normal", normal.to_json());
  return j;
}
uint64_t BakeSettings::hash() const { return fnv1a(to_json().dump()); }

// ---------------------------------------------------------------- UV rasterization
namespace {
constexpr int kTile = 32;
constexpr float kGutter = 1.5f;  // texels around triangles that receive closest-point samples

struct Hit {
  int32_t tri;
  float w0, w1, w2;  // barycentric weights of vertices 0,1,2
  float dist;        // 0 = inside
};

inline float edge_fn(vec2 a, vec2 b, vec2 p) { return (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x); }

// Closest point on triangle (2D) to p, returned as barycentrics + distance.
inline float closest_on_tri(vec2 a, vec2 b, vec2 c, vec2 p, float& w0, float& w1, float& w2) {
  float best = 1e30f;
  auto seg = [&](vec2 A, vec2 B, int ia, int ib) {
    vec2 ab = B - A;
    float l2 = ab.x * ab.x + ab.y * ab.y;
    float t = l2 > 0 ? clampf(((p.x - A.x) * ab.x + (p.y - A.y) * ab.y) / l2, 0.f, 1.f) : 0.f;
    vec2 q = A + ab * t;
    float d = std::sqrt((p.x - q.x) * (p.x - q.x) + (p.y - q.y) * (p.y - q.y));
    if (d < best) {
      best = d;
      float w[3] = {0, 0, 0};
      w[ia] = 1.f - t;
      w[ib] = t;
      w0 = w[0]; w1 = w[1]; w2 = w[2];
    }
  };
  seg(a, b, 0, 1);
  seg(b, c, 1, 2);
  seg(c, a, 2, 0);
  return best;
}
}  // namespace

static void raster_set(const Mesh& m, int set, int res, SampleSet& ss) {
  int tiles = (res + kTile - 1) / kTile;
  std::vector<std::vector<uint32_t>> bins((size_t)tiles * tiles);
  float fres = (float)res;
  for (size_t t = 0; t < m.tri_count(); t++) {
    if (m.tri_set[t] != set) continue;
    vec2 a = m.uv[m.idx[t * 3]] * fres, b = m.uv[m.idx[t * 3 + 1]] * fres, c = m.uv[m.idx[t * 3 + 2]] * fres;
    if (std::fabs(edge_fn(a, b, c)) < 1e-9f) continue;
    float x0 = std::fmin(a.x, std::fmin(b.x, c.x)) - kGutter, x1 = std::fmax(a.x, std::fmax(b.x, c.x)) + kGutter;
    float y0 = std::fmin(a.y, std::fmin(b.y, c.y)) - kGutter, y1 = std::fmax(a.y, std::fmax(b.y, c.y)) + kGutter;
    int tx0 = std::max(0, (int)std::floor(x0) / kTile), tx1 = std::min(tiles - 1, (int)std::floor(x1) / kTile);
    int ty0 = std::max(0, (int)std::floor(y0) / kTile), ty1 = std::min(tiles - 1, (int)std::floor(y1) / kTile);
    if (x1 < 0 || y1 < 0 || x0 >= fres || y0 >= fres) continue;
    for (int ty = ty0; ty <= ty1; ty++)
      for (int tx = tx0; tx <= tx1; tx++) bins[(size_t)ty * tiles + tx].push_back((uint32_t)t);
  }

  std::vector<std::vector<Hit>> tile_hits((size_t)tiles * tiles);
  std::vector<std::vector<int32_t>> tile_texels((size_t)tiles * tiles);
  parallel_for((int64_t)tiles * tiles, 1, [&](int64_t b0, int64_t b1) {
    std::vector<Hit> local(kTile * kTile);
    for (int64_t ti = b0; ti < b1; ti++) {
      auto& bin = bins[ti];
      if (bin.empty()) continue;
      int tx = (int)(ti % tiles), ty = (int)(ti / tiles);
      int px0 = tx * kTile, py0 = ty * kTile;
      for (auto& h : local) { h.tri = -1; h.dist = 1e30f; }
      for (uint32_t t : bin) {
        vec2 a = m.uv[m.idx[t * 3]] * fres, b = m.uv[m.idx[t * 3 + 1]] * fres, c = m.uv[m.idx[t * 3 + 2]] * fres;
        float area = edge_fn(a, b, c);
        float inv_area = 1.0f / area;
        // barycentric weight -> signed distance (texels) to the opposite edge's line; a texel further than the
        // gutter outside any of the three lines cannot be a gutter sample of this triangle
        auto to_texels = [&](vec2 p, vec2 q) { return std::fabs(area) / std::sqrt((q.x - p.x) * (q.x - p.x) + (q.y - p.y) * (q.y - p.y)); };
        float k0 = to_texels(b, c), k1 = to_texels(c, a), k2 = to_texels(a, b);
        const float kFar = -(kGutter + 0.25f);  // the margin covers float error on long thin triangles
        int x0 = std::max(px0, (int)std::floor(std::fmin(a.x, std::fmin(b.x, c.x)) - kGutter));
        int x1 = std::min(std::min(px0 + kTile, res) - 1, (int)std::floor(std::fmax(a.x, std::fmax(b.x, c.x)) + kGutter));
        int y0 = std::max(py0, (int)std::floor(std::fmin(a.y, std::fmin(b.y, c.y)) - kGutter));
        int y1 = std::min(std::min(py0 + kTile, res) - 1, (int)std::floor(std::fmax(a.y, std::fmax(b.y, c.y)) + kGutter));
        for (int y = y0; y <= y1; y++)
          for (int x = x0; x <= x1; x++) {
            vec2 p{x + 0.5f, y + 0.5f};
            float w0 = edge_fn(b, c, p) * inv_area, w1 = edge_fn(c, a, p) * inv_area, w2 = 1.f - w0 - w1;
            Hit& h = local[(y - py0) * kTile + (x - px0)];
            if (w0 >= -1e-6f && w1 >= -1e-6f && w2 >= -1e-6f) {
              if (h.dist > 0.f) h = {(int32_t)t, w0, w1, w2, 0.f};
            } else if (h.dist > 0.f) {
              if (w0 * k0 < kFar || w1 * k1 < kFar || w2 * k2 < kFar) continue;
              float c0, c1, c2;
              float d = closest_on_tri(a, b, c, p, c0, c1, c2);
              if (d < kGutter && d < h.dist) h = {(int32_t)t, c0, c1, c2, d};
            }
          }
      }
      auto& out = tile_hits[ti];
      auto& tex = tile_texels[ti];
      size_t covered = 0;
      for (const Hit& h : local) covered += h.tri >= 0;
      out.reserve(covered);
      tex.reserve(covered);
      for (int ly = 0; ly < kTile; ly++)
        for (int lx = 0; lx < kTile; lx++) {
          const Hit& h = local[ly * kTile + lx];
          if (h.tri < 0) continue;
          int x = px0 + lx, y = py0 + ly;
          if (x >= res || y >= res) continue;
          out.push_back(h);
          tex.push_back(y * res + x);
        }
    }
  });

  size_t total = 0;
  std::vector<size_t> offset(tile_hits.size());
  for (size_t i = 0; i < tile_hits.size(); i++) { offset[i] = total; total += tile_hits[i].size(); }
  // each array is sized (and so zero-filled and paged in) on its own thread
  const std::function<void()> resize[] = {
      [&] { ss.texel.resize(total); }, [&] { ss.interior.resize(total); }, [&] { ss.tri.resize(total); }, [&] { ss.pos.resize(total); },
      [&] { ss.nrm.resize(total); },   [&] { ss.fnrm.resize(total); },     [&] { ss.tan.resize(total); }, [&] { ss.uv.resize(total); },
      [&] { ss.len_u.resize(total); }, [&] { ss.len_v.resize(total); },
  };
  parallel_for((int64_t)std::size(resize), 1, [&](int64_t b, int64_t e) {
    for (int64_t k = b; k < e; k++) resize[k]();
  });

  parallel_for((int64_t)tile_hits.size(), 4, [&](int64_t b0, int64_t b1) {
    // per-triangle quantities, kept across the run of samples that share the triangle
    uint32_t cur = UINT32_MAX, i0 = 0, i1 = 0, i2 = 0;
    vec3 face_n;
    float len_u = 0, len_v = 0;
    for (int64_t ti = b0; ti < b1; ti++) {
      size_t o = offset[ti];
      for (size_t k = 0; k < tile_hits[ti].size(); k++) {
        const Hit& h = tile_hits[ti][k];
        size_t i = o + k;
        uint32_t t = (uint32_t)h.tri;
        if (t != cur) {
          cur = t;
          i0 = m.idx[t * 3], i1 = m.idx[t * 3 + 1], i2 = m.idx[t * 3 + 2];
          vec3 e1 = m.pos[i1] - m.pos[i0], e2 = m.pos[i2] - m.pos[i0];
          face_n = normalize(cross(e1, e2));
          // world units per UV unit along u and v (glTF v, pointing down the image)
          float du1 = m.uv[i1].x - m.uv[i0].x, dv1 = m.uv[i1].y - m.uv[i0].y;
          float du2 = m.uv[i2].x - m.uv[i0].x, dv2 = m.uv[i2].y - m.uv[i0].y;
          float det = du1 * dv2 - du2 * dv1;
          if (std::fabs(det) > 1e-20f) {
            vec3 dpdu = (e1 * dv2 - e2 * dv1) / det;
            vec3 dpdv = (e2 * du1 - e1 * du2) / det;
            len_u = std::fmax(1e-9f, length(dpdu) / res);
            len_v = std::fmax(1e-9f, length(dpdv) / res);
          } else {
            len_u = len_v = 1e-3f;
          }
        }
        ss.texel[i] = tile_texels[ti][k];
        ss.interior[i] = h.dist == 0.f;
        ss.tri[i] = t;
        ss.pos[i] = m.pos[i0] * h.w0 + m.pos[i1] * h.w1 + m.pos[i2] * h.w2;
        vec3 n = normalize(m.nrm[i0] * h.w0 + m.nrm[i1] * h.w1 + m.nrm[i2] * h.w2);
        ss.nrm[i] = n;
        ss.fnrm[i] = dot(face_n, n) < 0 ? -face_n : face_n;
        vec3 tg = m.tan[i0].xyz() * h.w0 + m.tan[i1].xyz() * h.w1 + m.tan[i2].xyz() * h.w2;
        tg = normalize(tg - n * dot(n, tg));
        ss.tan[i] = vec4(tg, m.tan[i0].w);
        ss.uv[i] = m.uv[i0] * h.w0 + m.uv[i1] * h.w1 + m.uv[i2] * h.w2;
        ss.len_u[i] = len_u;
        ss.len_v[i] = len_v;
      }
    }
  });
  size_t interior = 0;
  for (auto v : ss.interior) interior += v;
  ss.interior_count = interior;
}

// Nearest-sample map: pads every texel with the closest covered texel (Euclidean), so filtering and
// mipmapping never bleed background into islands. A separable exact distance transform (Felzenszwalb &
// Huttenlocher): the nearest covered texel within each column, then the lower envelope of the resulting
// parabolas along each row. Both passes run on all cores.
static void build_padding(SampleSet& ss) {
  int res = ss.res;
  size_t total = (size_t)res * res;
  ss.pad.assign(total, -1);
  if (ss.size() == 0) return;
  parallel_for((int64_t)ss.size(), [&](int64_t b, int64_t e) {
    for (int64_t i = b; i < e; i++) ss.pad[ss.texel[i]] = (int32_t)i;
  });
  // near_y[y * res + x]: row of the closest covered texel in column x (-1: the column is empty)
  std::vector<int32_t> near_y(total);
  parallel_for(res, 64, [&](int64_t x0, int64_t x1) {
    for (int y = 0; y < res; y++)
      for (int64_t x = x0; x < x1; x++) {
        size_t t = (size_t)y * res + x;
        near_y[t] = ss.pad[t] >= 0 ? y : (y > 0 ? near_y[t - res] : -1);
      }
    for (int y = res - 2; y >= 0; y--)
      for (int64_t x = x0; x < x1; x++) {
        size_t t = (size_t)y * res + x;
        int up = near_y[t], down = near_y[t + res];
        if (down >= 0 && (up < 0 || std::abs(down - y) < y - up)) near_y[t] = down;
      }
  });
  std::vector<int32_t> pad(total);
  parallel_for(res, 8, [&](int64_t y0, int64_t y1) {
    std::vector<int> v(res);          // columns whose parabolas form the lower envelope
    std::vector<double> z(res + 1);   // where each one takes over from the previous
    for (int64_t y = y0; y < y1; y++) {
      const int32_t* ny = &near_y[(size_t)y * res];
      int32_t* out = &pad[(size_t)y * res];
      auto f = [&](int x) { double d = (double)(ny[x] - y); return d * d + (double)x * x; };
      int k = -1;
      for (int x = 0; x < res; x++) {
        if (ny[x] < 0) continue;
        double start = -1e300;
        while (k >= 0) {
          start = (f(x) - f(v[k])) / (2.0 * (x - v[k]));
          if (start > z[k]) break;
          k--;
        }
        if (k < 0) start = -1e300;
        k++;
        v[k] = x;
        z[k] = start;
      }
      if (k < 0) { std::fill(out, out + res, -1); continue; }  // unreachable: some column has a sample
      int j = 0;
      for (int x = 0; x < res; x++) {
        while (j < k && z[j + 1] < x) j++;
        out[x] = ss.pad[(size_t)ny[v[j]] * res + v[j]];
      }
    }
  });
  ss.pad.swap(pad);
}

void to_image(const SampleSet& ss, const float* samples, int comps, float* out) {
  size_t n = (size_t)ss.res * ss.res;
  parallel_for((int64_t)n, [&](int64_t b, int64_t e) {
    for (int64_t t = b; t < e; t++) {
      int32_t s = ss.pad.empty() ? -1 : ss.pad[t];
      for (int c = 0; c < comps; c++) out[t * comps + c] = s >= 0 ? samples[(size_t)s * comps + c] : 0.f;
    }
  });
}

void from_image(const SampleSet& ss, const float* img, int comps, float* out) {
  parallel_for((int64_t)ss.size(), [&](int64_t b, int64_t e) {
    for (int64_t i = b; i < e; i++)
      for (int c = 0; c < comps; c++) out[i * comps + c] = img[(size_t)ss.texel[i] * comps + c];
  });
}

// Small 3x3 tent blur in texel space (via padding) to remove Monte Carlo grain.
void denoise_samples(const SampleSet& ss, std::vector<float>& v) {
  if (ss.size() == 0) return;
  int res = ss.res;
  std::vector<float> out(ss.size());
  parallel_for((int64_t)ss.size(), [&](int64_t b, int64_t e) {
    for (int64_t i = b; i < e; i++) {
      int t = ss.texel[i], x = t % res, y = t / res;
      float s = 0, w = 0;
      for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++) {
          int nx = std::clamp(x + dx, 0, res - 1), ny = std::clamp(y + dy, 0, res - 1);
          float ww = (dx == 0 ? 2.f : 1.f) * (dy == 0 ? 2.f : 1.f);
          // only blend with samples on the same triangle island neighborhood (avoid bleeding across UV gaps)
          int32_t src = ss.pad[ny * res + nx];
          if (src < 0 || length2(ss.pos[src] - ss.pos[i]) > 16.f * (ss.len_u[i] * ss.len_u[i] + ss.len_v[i] * ss.len_v[i])) continue;
          s += v[src] * ww;
          w += ww;
        }
      out[i] = w > 0 ? s / w : v[i];
    }
  });
  v.swap(out);
}

// ---------------------------------------------------------------- AO & thickness
// Indices of the non-zero entries of `pick`, ascending.
static void compact(const std::vector<uint8_t>& pick, std::vector<uint32_t>& ids) {
  const size_t kBlock = 1 << 16;
  size_t n = pick.size(), blocks = (n + kBlock - 1) / kBlock;
  std::vector<size_t> start(blocks + 1, 0);
  parallel_for((int64_t)blocks, 1, [&](int64_t b, int64_t e) {
    for (int64_t k = b; k < e; k++) {
      size_t c = 0;
      for (size_t i = k * kBlock, end = std::min(n, i + kBlock); i < end; i++) c += pick[i] != 0;
      start[k + 1] = c;
    }
  });
  for (size_t k = 0; k < blocks; k++) start[k + 1] += start[k];
  ids.resize(start[blocks]);
  parallel_for((int64_t)blocks, 1, [&](int64_t b, int64_t e) {
    for (int64_t k = b; k < e; k++) {
      size_t o = start[k];
      for (size_t i = k * kBlock, end = std::min(n, i + kBlock); i < end; i++) if (pick[i]) ids[o++] = (uint32_t)i;
    }
  });
}

// Evaluates smooth, expensive per-sample quantities on a 2x2-subsampled texel lattice, then fills the
// remaining samples by bilinear interpolation from lattice neighbors that are close in 3D and share
// the normal; anything else (creases, island borders, thin features) is computed exactly. ~3.5x fewer rays.
// compute(ids, values) evaluates a batch of samples exactly: values[k][j] is map k at sample ids[j].
template <size_t K, class F>
static void eval_subsampled(const SampleSet& ss, const std::array<std::vector<float>*, K>& out, F&& compute) {
  int res = ss.res;
  size_t n = ss.size();
  for (auto* o : out) o->assign(n, 0.f);
  std::vector<uint8_t> is_prim(n, 0), exact(n, 0);
  std::vector<uint32_t> ids;
  std::array<std::vector<float>, K> vals;
  auto run = [&](const std::vector<uint8_t>& pick) {
    compact(pick, ids);
    for (auto& v : vals) v.resize(ids.size());
    compute(ids, vals);
    parallel_for((int64_t)ids.size(), [&](int64_t b, int64_t e) {
      for (int64_t j = b; j < e; j++)
        for (size_t k = 0; k < K; k++) (*out[k])[ids[j]] = vals[k][j];
    });
  };
  parallel_for((int64_t)n, [&](int64_t b, int64_t e) {
    for (int64_t i = b; i < e; i++) {
      int t = ss.texel[i];
      is_prim[i] = ((t % res) % 2 == 0) && ((t / res) % 2 == 0);
    }
  });
  run(is_prim);
  parallel_for((int64_t)n, 64, [&](int64_t b, int64_t e) {
    for (int64_t i = b; i < e; i++) {
      if (is_prim[i]) continue;
      int t = ss.texel[i], x = t % res, y = t / res;
      int x0 = x & ~1, y0 = y & ~1;
      float fx = (x - x0) * 0.5f, fy = (y - y0) * 0.5f;
      float lim = 3.f * std::fmax(ss.len_u[i], ss.len_v[i]);
      float acc[K] = {}, wsum = 0;
      for (int k = 0; k < 4; k++) {
        int cx = x0 + (k & 1) * 2, cy = y0 + (k >> 1) * 2;
        float w = ((k & 1) ? fx : 1.f - fx) * ((k >> 1) ? fy : 1.f - fy);
        if (w <= 0 || cx >= res || cy >= res) continue;
        int32_t s = ss.pad[(size_t)cy * res + cx];
        if (s < 0 || ss.texel[s] != cy * res + cx || !is_prim[s]) continue;
        if (length2(ss.pos[s] - ss.pos[i]) > lim * lim || dot(ss.nrm[s], ss.nrm[i]) < 0.97f) continue;
        for (size_t m = 0; m < K; m++) acc[m] += (*out[m])[s] * w;
        wsum += w;
      }
      if (wsum > 0.6f) {
        for (size_t m = 0; m < K; m++) (*out[m])[i] = acc[m] / wsum;
      } else {
        exact[i] = 1;
      }
    }
  });
  run(exact);
}

static inline float radical_inverse(uint32_t b) {
  b = (b << 16u) | (b >> 16u);
  b = ((b & 0x55555555u) << 1u) | ((b & 0xAAAAAAAAu) >> 1u);
  b = ((b & 0x33333333u) << 2u) | ((b & 0xCCCCCCCCu) >> 2u);
  b = ((b & 0x0F0F0F0Fu) << 4u) | ((b & 0xF0F0F0F0u) >> 4u);
  b = ((b & 0x00FF00FFu) << 8u) | ((b & 0xFF00FF00u) >> 8u);
  return (float)b * 2.3283064365386963e-10f;
}

// The surface kernel in shaders/bake.hlsl mirrors the two estimators here.
void trace_ao_thickness(const BVH& bvh, const BakeSettings& s, float ext, Gpu* gpu, const GpuScene& scene, uint32_t scene_bvh, size_t count,
                        const std::function<GpuSample(size_t)>& point, float* ao_out, float* thickness_out) {
  float eps = ext * 2e-5f;
  float ao_maxd = s.ao_distance * ext, th_maxd = s.thickness_distance * ext;
  int ao_n = s.ao_samples, th_n = s.thickness_samples;
  auto ao_value = [&](int hits, int valid) { return valid ? 1.f - (float)hits / valid : 1.f; };
  auto thickness_value = [&](float sum) { return saturate(sum / (th_n * th_maxd)); };
  auto ao = [&](const GpuSample& p) {
    vec3 n = p.nrm, fn = p.fnrm;
    vec3 o = p.pos + fn * eps + n * eps;
    vec3 t, bt;
    onb(n, t, bt);
    float r1 = hash_float(p.seed * 2u + 17u), r2 = hash_float(p.seed * 2u + 91u);
    int hits = 0, valid = 0;
    for (int k = 0; k < ao_n; k++) {
      float u1 = (k + r1) / ao_n;
      float u2 = radical_inverse((uint32_t)k) + r2;
      u2 -= std::floor(u2);
      float r = std::sqrt(u1), phi = 2.f * kPi * u2;
      vec3 d = t * (r * std::cos(phi)) + bt * (r * std::sin(phi)) + n * std::sqrt(std::fmax(0.f, 1.f - u1));
      if (dot(d, fn) < 0.01f) continue;
      valid++;
      if (bvh.occluded(o, d, 0.f, ao_maxd)) hits++;
    }
    return ao_value(hits, valid);
  };
  auto thickness = [&](const GpuSample& p) {
    vec3 n = -p.nrm, fn = -p.fnrm;
    vec3 o = p.pos + fn * eps;
    vec3 t, bt;
    onb(n, t, bt);
    float r1 = hash_float(p.seed * 2u + 7u), r2 = hash_float(p.seed * 2u + 3u);
    float sum = 0;
    for (int k = 0; k < th_n; k++) {
      float u1 = (k + r1) / th_n;
      float u2 = radical_inverse((uint32_t)k) + r2;
      u2 -= std::floor(u2);
      // narrower cone than AO (thickness measures "straight through")
      float ct = 1.f - u1 * 0.6f, st = std::sqrt(std::fmax(0.f, 1.f - ct * ct)), phi = 2.f * kPi * u2;
      vec3 d = t * (st * std::cos(phi)) + bt * (st * std::sin(phi)) + n * ct;
      float th, uu, vv;
      uint32_t tri;
      sum += bvh.intersect(o, d, 0.f, th_maxd, th, tri, uu, vv) ? th : th_maxd;
    }
    return thickness_value(sum);
  };
  if (!gpu) {
    parallel_for((int64_t)count, 64, [&](int64_t b, int64_t e) {
      for (int64_t j = b; j < e; j++) ao_out[j] = ao(point((size_t)j));
    });
    parallel_for((int64_t)count, 64, [&](int64_t b, int64_t e) {
      for (int64_t j = b; j < e; j++) thickness_out[j] = thickness(point((size_t)j));
    });
    return;
  }
  GpuParams p;
  p.eps = eps;
  p.bvh = scene_bvh;
  p.samples = (uint32_t)ao_n;
  p.maxd = ao_maxd;
  p.thickness_samples = (uint32_t)th_n;
  p.thickness_maxd = th_maxd;
  gpu_run(*gpu, GpuKernel::Surface, p, scene, count, sizeof(GpuSample), sizeof(GpuSurfaceOut), (uint64_t)ao_n + th_n,
          [&](size_t first, size_t chunk, void* dst) {
            GpuSample* r = (GpuSample*)dst;
            parallel_for((int64_t)chunk, 4096, [&](int64_t b, int64_t e) {
              for (int64_t j = b; j < e; j++) r[j] = point(first + (size_t)j);
            });
          },
          [&](size_t first, size_t chunk, const void* src) {
            const GpuSurfaceOut* r = (const GpuSurfaceOut*)src;
            for (size_t j = 0; j < chunk; j++) {
              ao_out[first + j] = ao_value((int)(r[j].ao >> 16), (int)(r[j].ao & 0xffffu));
              thickness_out[first + j] = thickness_value(r[j].thickness);
            }
          });
}

// AO and thickness of the low poly (`scene` holds its BVH).
static void bake_ao_thickness(const Mesh& m, const BVH& bvh, SampleSet& ss, const BakeSettings& s, Gpu* gpu, const GpuScene& scene) {
  eval_subsampled<2>(ss, {&ss.ao, &ss.thickness}, [&](const std::vector<uint32_t>& ids, std::array<std::vector<float>, 2>& out) {
    trace_ao_thickness(bvh, s, m.max_extent(), gpu, scene, 0, ids.size(),
                       [&](size_t j) { uint32_t i = ids[j]; return GpuSample{ss.pos[i], i, ss.nrm[i], 0.f, ss.fnrm[i], 0.f}; },
                       out[0].data(), out[1].data());
  });
  parallel_for(2, 1, [&](int64_t b, int64_t e) {
    for (int64_t k = b; k < e; k++) denoise_samples(ss, k == 0 ? ss.ao : ss.thickness);
  });
}

// ---------------------------------------------------------------- curvature
namespace {
struct EdgeSamples {
  std::vector<vec3> p, n;
  std::vector<float> w;
  std::vector<uint32_t> shell;
};
}  // namespace

size_t CurvatureField::cell_of(vec3 p, int& x, int& y, int& z) const {
  x = std::clamp((int)((p.x - lo_.x) / cell_), 0, gx_ - 1);
  y = std::clamp((int)((p.y - lo_.y) / cell_), 0, gy_ - 1);
  z = std::clamp((int)((p.z - lo_.z) / cell_), 0, gz_ - 1);
  return ((size_t)z * gy_ + y) * gx_ + x;
}

void CurvatureField::build(const Mesh& m, float r, float min_angle_deg, const BVH* solid) {
  r_ = r;
  size_t nt = m.tri_count();
  uint32_t wc = 0;
  std::vector<uint32_t> weld = weld_by_position(m, &wc);
  // representative position for each welded vertex
  std::vector<vec3> wpos(wc);
  for (size_t v = 0; v < m.pos.size(); v++) wpos[weld[v]] = m.pos[v];
  auto face_n = [&](uint32_t t, vec3& n) {
    vec3 c = cross(m.pos[m.idx[t * 3 + 1]] - m.pos[m.idx[t * 3]], m.pos[m.idx[t * 3 + 2]] - m.pos[m.idx[t * 3]]);
    float l = length(c);
    n = l > 1e-20f ? c / l : vec3(0, 0, 1);
    return l > 1e-20f;
  };

  // connected pieces of surface: triangles joined through welded vertices
  size_t shells = 0;
  if (solid) {
    std::vector<uint32_t> parent(wc);
    for (uint32_t v = 0; v < wc; v++) parent[v] = v;
    auto find = [&](uint32_t v) {
      while (parent[v] != v) v = parent[v] = parent[parent[v]];
      return v;
    };
    for (size_t t = 0; t < nt; t++) {
      uint32_t a = find(weld[m.idx[t * 3]]);
      for (int k = 1; k < 3; k++) parent[find(weld[m.idx[t * 3 + k]])] = a;
    }
    tri_shell.resize(nt);
    std::vector<uint8_t> seen(wc, 0);
    for (size_t t = 0; t < nt; t++) {
      tri_shell[t] = find(weld[m.idx[t * 3]]);
      shells += !seen[tri_shell[t]];
      seen[tri_shell[t]] = 1;
    }
  }

  // every triangle side, filed under the lower of its two welded vertices: an edge's two sides end up together
  struct Side { uint32_t hi, tri; };
  std::vector<uint32_t> first(wc + 1, 0);
  auto for_sides = [&](auto&& fn) {
    for (size_t t = 0; t < nt; t++)
      for (int e = 0; e < 3; e++) {
        uint32_t a = weld[m.idx[t * 3 + e]], b = weld[m.idx[t * 3 + (e + 1) % 3]];
        if (a != b) fn(std::min(a, b), std::max(a, b), (uint32_t)t);
      }
  };
  for_sides([&](uint32_t lo, uint32_t, uint32_t) { first[lo + 1]++; });
  for (uint32_t v = 0; v < wc; v++) first[v + 1] += first[v];
  std::vector<Side> sides(first[wc]);
  {
    std::vector<uint32_t> fill(first.begin(), first.end() - 1);
    for_sides([&](uint32_t lo, uint32_t hi, uint32_t t) { sides[fill[lo]++] = {hi, t}; });
  }

  // edge samples, a block of vertices at a time (in vertex order whatever the thread count)
  float min_angle = std::fmax(min_angle_deg * kPi / 180.f, 1e-4f);
  float spacing = r * 0.25f;
  const int64_t kBlock = 4096;
  std::vector<EdgeSamples> blocks(((size_t)wc + kBlock - 1) / kBlock);
  parallel_for((int64_t)wc, kBlock, [&](int64_t v0, int64_t v1) {
    for (int64_t k = v0 / kBlock; k * kBlock < v1; k++) {
      EdgeSamples& es = blocks[k];
      for (uint32_t lo = (uint32_t)(k * kBlock), end = (uint32_t)std::min<int64_t>((k + 1) * kBlock, wc); lo < end; lo++) {
        Side* s0 = sides.data() + first[lo], *s1 = sides.data() + first[lo + 1];
        std::sort(s0, s1, [](const Side& x, const Side& y) { return x.hi != y.hi ? x.hi < y.hi : x.tri < y.tri; });
        for (Side* s = s0, *next; s < s1; s = next) {
          for (next = s + 1; next < s1 && next->hi == s->hi;) next++;
          if (next - s != 2) continue;  // an open border, or more than two faces on one edge
          uint32_t t0 = s[0].tri, t1 = s[1].tri;
          vec3 n0, n1;
          if (!face_n(t0, n0) || !face_n(t1, n1)) continue;
          float theta = std::atan2(length(cross(n0, n1)), dot(n0, n1));
          if (theta < min_angle) continue;
          vec3 a = wpos[lo], b = wpos[s->hi];
          vec3 mid = (a + b) * 0.5f;
          vec3 c1 = (m.pos[m.idx[t1 * 3]] + m.pos[m.idx[t1 * 3 + 1]] + m.pos[m.idx[t1 * 3 + 2]]) / 3.f;
          bool convex = dot(n0, c1 - mid) < 0.f;
          float signed_theta = convex ? theta : -theta;
          float L = length(b - a);
          int cnt = std::max(1, (int)std::ceil(L / spacing));
          float seg = L / cnt;
          vec3 en = normalize(n0 + n1);
          for (int j = 0; j < cnt; j++) {
            es.p.push_back(a + (b - a) * ((j + 0.5f) / cnt));
            es.n.push_back(en);
            es.w.push_back(signed_theta * seg);
            if (solid) es.shell.push_back(tri_shell[t0]);
          }
        }
      }
    }
  });
  EdgeSamples es;
  {
    size_t total = 0;
    for (auto& blk : blocks) total += blk.w.size();
    es.p.reserve(total);
    es.n.reserve(total);
    es.w.reserve(total);
    if (solid) es.shell.reserve(total);
    for (auto& blk : blocks) {
      es.p.insert(es.p.end(), blk.p.begin(), blk.p.end());
      es.n.insert(es.n.end(), blk.n.begin(), blk.n.end());
      es.w.insert(es.w.end(), blk.w.begin(), blk.w.end());
      es.shell.insert(es.shell.end(), blk.shell.begin(), blk.shell.end());
      blk = EdgeSamples();
    }
  }

  // An edge inside another piece's solid is not part of the visible surface: looking out from it along its
  // normal, the first thing seen is then the inside of a wall.
  if (solid && shells > 1 && !solid->empty()) {
    float lift = m.max_extent() * 1e-4f;
    std::vector<uint8_t> keep(es.w.size());
    parallel_for((int64_t)es.w.size(), 256, [&](int64_t b, int64_t e) {
      for (int64_t i = b; i < e; i++) {
        float t, u, v;
        uint32_t tri;
        vec3 fn;
        keep[i] = !(solid->intersect(es.p[i] + es.n[i] * lift, es.n[i], 0.f, 1e30f, t, tri, u, v) && face_n(tri, fn) && dot(fn, es.n[i]) > 0.f);
      }
    });
    size_t k = 0;
    for (size_t i = 0; i < keep.size(); i++) {
      if (!keep[i]) continue;
      es.p[k] = es.p[i];
      es.n[k] = es.n[i];
      es.w[k] = es.w[i];
      es.shell[k] = es.shell[i];
      k++;
    }
    es.p.resize(k);
    es.n.resize(k);
    es.w.resize(k);
    es.shell.resize(k);
  }

  // the samples in a grid of cells no smaller than the kernel
  lo_ = m.bmin - vec3(r);
  vec3 sz = m.bmax + vec3(r) - lo_;
  cell_ = std::fmax(r, std::fmax(sz.x, std::fmax(sz.y, sz.z)) / 192.f);
  gx_ = std::max(1, (int)std::ceil(sz.x / cell_));
  gy_ = std::max(1, (int)std::ceil(sz.y / cell_));
  gz_ = std::max(1, (int)std::ceil(sz.z / cell_));
  size_t ncell = (size_t)gx_ * gy_ * gz_, n = es.w.size();
  start_.assign(ncell + 1, 0);
  std::vector<uint32_t> cell_id(n);
  for (size_t i = 0; i < n; i++) {
    int x, y, z;
    cell_id[i] = (uint32_t)cell_of(es.p[i], x, y, z);
    start_[cell_id[i] + 1]++;
  }
  for (size_t c = 0; c < ncell; c++) start_[c + 1] += start_[c];
  sp_.resize(n);
  sn_.resize(n);
  sw_.resize(n);
  shell_.resize(es.shell.size());
  reach_.assign(ncell, 0);
  std::vector<uint32_t> fill(start_.begin(), start_.end() - 1);
  for (size_t i = 0; i < n; i++) {
    uint32_t k = fill[cell_id[i]]++;
    sp_[k] = es.p[i];
    sn_[k] = es.n[i];
    sw_[k] = es.w[i];
    if (!shell_.empty()) shell_[k] = es.shell[i];
    int cx = (int)(cell_id[i] % gx_), cy = (int)(cell_id[i] / gx_ % gy_), cz = (int)(cell_id[i] / ((size_t)gx_ * gy_));
    for (int z = std::max(0, cz - 1); z <= std::min(gz_ - 1, cz + 1); z++)
      for (int y = std::max(0, cy - 1); y <= std::min(gy_ - 1, cy + 1); y++)
        for (int x = std::max(0, cx - 1); x <= std::min(gx_ - 1, cx + 1); x++) reach_[((size_t)z * gy_ + y) * gx_ + x] = 1;
  }
}

float CurvatureField::at(vec3 p, vec3 n, float gain, uint32_t shell) const {
  int cx, cy, cz;
  if (reach_.empty() || !reach_[cell_of(p, cx, cy, cz)]) return 0.5f;  // flat
  bool any_shell = shell == kAnyShell || shell_.empty();
  float r2 = r_ * r_;
  float acc = 0;
  for (int z = std::max(0, cz - 1); z <= std::min(gz_ - 1, cz + 1); z++)
    for (int y = std::max(0, cy - 1); y <= std::min(gy_ - 1, cy + 1); y++)
      for (int x = std::max(0, cx - 1); x <= std::min(gx_ - 1, cx + 1); x++) {
        size_t c = ((size_t)z * gy_ + y) * gx_ + x;
        for (uint32_t k = start_[c]; k < start_[c + 1]; k++) {
          float d2 = length2(p - sp_[k]);
          if (d2 >= r2) continue;
          if (dot(sn_[k], n) < -0.05f) continue;  // other side of a thin wall
          if (!any_shell && shell_[k] != shell) continue;
          float w = 1.f - d2 / r2;
          acc += w * w * sw_[k];
        }
      }
  float norm = 3.f / (kPi * r2);  // 1 / integral of (1-d^2/r^2)^2 over the disk
  return 0.5f + 0.5f * std::tanh(acc * norm * r_ * gain);
}

// ---------------------------------------------------------------- disk cache
static std::string cache_path(const std::string& dir, uint64_t key) { return path_join(dir, strf("bake-%016llx.bin", (unsigned long long)key)); }

// The maps are read and written straight from the sample arrays: a 4K project is hundreds of megabytes.
static bool load_cache(const std::string& path, std::vector<SampleSet>& sets) {
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) return false;
  auto rd = [&](void* dst, size_t n) { return n == 0 || fread(dst, 1, n, f) == n; };
  auto load = [&] {
    uint32_t magic = 0, count = 0;
    if (!rd(&magic, 4) || magic != 0x32544150u || !rd(&count, 4) || count != sets.size()) return false;
    for (auto& ss : sets) {
      uint64_t n = 0, nn = 0;
      if (!rd(&n, 8) || n != ss.size()) return false;
      ss.ao.resize(n); ss.thickness.resize(n); ss.curvature.resize(n);
      if (!rd(ss.ao.data(), n * 4) || !rd(ss.thickness.data(), n * 4) || !rd(ss.curvature.data(), n * 4)) return false;
      if (!rd(&nn, 8) || (nn != 0 && nn != n)) return false;
      ss.nmap.resize(nn); ss.nmiss.resize(nn);
      if (!rd(ss.nmap.data(), nn * sizeof(vec3)) || !rd(ss.nmiss.data(), nn)) return false;
    }
    return true;
  };
  bool ok = load();
  fclose(f);
  if (!ok)  // a damaged file: leave no half-read maps behind
    for (auto& ss : sets) { ss.ao.clear(); ss.thickness.clear(); ss.curvature.clear(); ss.nmap.clear(); ss.nmiss.clear(); }
  return ok;
}

static void save_cache(const std::string& path, const std::vector<SampleSet>& sets) {
  make_dirs(path_dir(path));
  // write to a temp file then rename, so concurrent readers never see partial files
  std::string tmp = path + strf(".tmp%u", (unsigned)hash_u32((uint32_t)(now_seconds() * 1e6)));
  FILE* f = fopen(tmp.c_str(), "wb");
  if (!f) fail("cannot write '%s'", path.c_str());
  bool ok = true;
  auto wr = [&](const void* src, size_t n) { ok = ok && (n == 0 || fwrite(src, 1, n, f) == n); };
  uint32_t magic = 0x32544150u, count = (uint32_t)sets.size();
  wr(&magic, 4);
  wr(&count, 4);
  for (auto& ss : sets) {
    uint64_t n = ss.size(), nn = ss.nmap.size();
    wr(&n, 8);
    wr(ss.ao.data(), n * 4);
    wr(ss.thickness.data(), n * 4);
    wr(ss.curvature.data(), n * 4);
    wr(&nn, 8);
    wr(ss.nmap.data(), nn * sizeof(vec3));
    wr(ss.nmiss.data(), nn);
  }
  ok = fclose(f) == 0 && ok;
  std::error_code ec;
  if (ok) std::filesystem::rename(tmp, path, ec);
  if (!ok || ec) {
    std::filesystem::remove(tmp, ec);
    fail("cannot write '%s'", path.c_str());
  }
}

// Cache files are written by one background thread: a disk takes longer over a big bake than the GPU does,
// and nothing waits for the file. Queued writes finish before the process exits.
namespace {
struct CacheWriter {
  std::mutex m;
  std::condition_variable cv;
  std::deque<std::pair<std::string, std::shared_ptr<const Baked>>> queue;
  std::thread thread;
  bool quit = false;

  static CacheWriter& get() {
    static CacheWriter* w = new CacheWriter;  // intentionally leaked: lives for the process
    return *w;
  }
  void push(const std::string& path, std::shared_ptr<const Baked> bk) {
    std::lock_guard<std::mutex> lk(m);
    if (!thread.joinable()) {
      thread = std::thread([this] { run(); });
      std::atexit([] { CacheWriter::get().finish(); });
    }
    queue.emplace_back(path, std::move(bk));
    cv.notify_all();
  }
  void run() {
    std::unique_lock<std::mutex> lk(m);
    for (;;) {
      cv.wait(lk, [&] { return quit || !queue.empty(); });
      if (queue.empty()) return;
      auto job = std::move(queue.front());
      queue.pop_front();
      lk.unlock();
      try { save_cache(job.first, job.second->sets); } catch (const std::exception&) { /* the cache is best-effort */ }
      job.second.reset();
      lk.lock();
    }
  }
  void finish() {
    { std::lock_guard<std::mutex> lk(m); quit = true; }
    cv.notify_all();
    if (thread.joinable()) thread.join();
  }
};
}  // namespace

// ---------------------------------------------------------------- entry
// Auto bakes on the GPU from this many texels (all texture sets together): a smaller bake is done on the CPU
// before a GPU driver has even started.
static constexpr int64_t kGpuMinTexels = 512 * 512;

std::shared_ptr<Baked> bake_mesh(std::shared_ptr<const Mesh> mesh, const std::vector<int>& res_per_set, const BakeSettings& s,
                                 const std::string& cache_dir, bool force, BakeDevice device) {
  const Mesh& m = *mesh;
  auto bk = std::make_shared<Baked>();
  bk->mesh = mesh;
  bk->settings = s;
  Json stats = Json::object();
  Timer total;

  int64_t texels = 0;
  for (int r : res_per_set) texels += (int64_t)r * r;
  bool want_gpu = device == BakeDevice::Gpu || (device == BakeDevice::Auto && texels >= kGpuMinTexels && Gpu::enabled());

  // bump when baker output changes so stale disk caches are never reused
  static const uint64_t kBakerVersion = 7;
  uint64_t key = fnv1a(&kBakerVersion, sizeof kBakerVersion, m.content_hash ^ s.hash());
  for (int r : res_per_set) key = fnv1a(&r, sizeof r, key);
  // the high poly and the cage count by their file contents; the high poly itself is only loaded for a real bake
  auto add_file = [&](const std::string& path, const char* what) {
    std::string bytes;
    if (!read_file(path, bytes)) fail("%s not found: '%s'", what, path.c_str());
    uint64_t h = fnv1a(bytes);
    key = fnv1a(&h, sizeof h, key);
  };
  if (!s.normal.high.empty()) add_file(s.normal.high, "mesh file");
  if (!s.normal.cage_mesh.empty()) add_file(s.normal.cage_mesh, "cage mesh");
  // GPU and CPU bakes differ in the last float bits, so each device keeps its own cache entry. Either one
  // will do when it exists, and looking for the file first avoids starting the GPU just to find out.
  auto device_key = [&](bool on_gpu) { uint8_t d = on_gpu; return fnv1a(&d, 1, key); };
  bool on_gpu = false;
  std::string cached_file;
  if (!force && !cache_dir.empty())
    for (bool d : {true, false}) {
      std::string f = cache_path(cache_dir, device_key(d));
      if ((d && !want_gpu) || (!d && device == BakeDevice::Gpu) || !file_exists(f)) continue;
      cached_file = f;
      on_gpu = d;
      break;
    }
  if (cached_file.empty() && want_gpu) Gpu::warm_up();  // the driver starts while the mesh is rasterized

  Timer t;
  bk->bvh.build(m.pos, m.idx);
  stats.set("bvh_ms", t.ms());

  t = Timer();
  bk->sets.resize(m.set_names.size());
  for (size_t si = 0; si < m.set_names.size(); si++) {
    SampleSet& ss = bk->sets[si];
    ss.name = m.set_names[si];
    ss.set = (int)si;
    ss.res = res_per_set[si];
    raster_set(m, (int)si, ss.res, ss);
    build_padding(ss);
  }
  stats.set("raster_ms", t.ms());

  Gpu* gpu = nullptr;
  bool cached = !cached_file.empty() && load_cache(cached_file, bk->sets);
  if (!cached) {
    std::shared_ptr<Mesh> high;
    if (!s.normal.high.empty()) {
      Timer t;
      high = std::make_shared<Mesh>(load_mesh(s.normal.high, true));
      stats.set("high_load_ms", t.ms());
    }
    // curvature never touches the GPU, so it goes first: by now the driver is usually still starting
    auto bake_geometry = [&] {
      Timer t;
      CurvatureField field;
      field.build(m, s.curvature_radius * m.max_extent(), s.curvature_min_angle);
      for (auto& ss : bk->sets) {
        ss.curvature.resize(ss.size());
        parallel_for((int64_t)ss.size(), 256, [&](int64_t b, int64_t e) {
          for (int64_t i = b; i < e; i++) ss.curvature[i] = field.at(ss.pos[i], ss.nrm[i], s.curvature_gain);
        });
      }
      stats.set("curvature_ms", t.ms());
      stats.set("curvature_edge_samples", (int64_t)field.sample_count());
    };
    auto bake_traced = [&](Gpu* on) {
      GpuScene low;
      if (on) {
        low.add(bk->bvh);
        low.upload(*on);
      }
      Timer t;
      for (auto& ss : bk->sets) bake_ao_thickness(m, bk->bvh, ss, s, on, low);
      stats.set("ao_thickness_ms", t.ms());
      if (s.normal.active()) bake_normals(*bk, high.get(), s, stats, on);
    };
    bake_geometry();
    if (want_gpu) gpu = Gpu::get();
    if (device == BakeDevice::Gpu && !gpu) fail("no GPU to bake on: %s", Gpu::unavailable_reason().c_str());
    struct Scratch {  // however this bake ends, the device keeps none of its buffers
      Gpu* gpu;
      ~Scratch() { if (gpu) gpu->trim(); }
    } scratch{gpu};
    try {
      bake_traced(gpu);
    } catch (const GpuError& e) {
      // driver reset, out of video memory...: every map is rebuilt from scratch on the CPU
      if (device == BakeDevice::Gpu) throw;
      stats.set("gpu_error", e.what());
      gpu = nullptr;
      bake_geometry();  // the normal bake had started on the curvature
      bake_traced(nullptr);
    }
    on_gpu = gpu != nullptr;
    if (!cache_dir.empty()) {
      // a fresh bake replaces what either device cached before (--force after an input the key does not cover changed)
      std::error_code ec;
      std::filesystem::remove(cache_path(cache_dir, device_key(!on_gpu)), ec);
      CacheWriter::get().push(cache_path(cache_dir, device_key(on_gpu)), bk);
    }
  }
  bk->key = device_key(on_gpu);
  stats.set("device", on_gpu ? "gpu" : "cpu");  // which device traced the maps (also when they came from the cache)
  if (gpu) stats.set("gpu", strf("%s (%s)", gpu->name().c_str(), gpu->backend()));
  stats.set("from_cache", cached);
  Json sets = Json::object();
  for (auto& ss : bk->sets) {
    Json sj = Json::object();
    sj.set("resolution", ss.res);
    sj.set("samples", (int64_t)ss.size());
    sj.set("coverage", (double)ss.interior_count / ((double)ss.res * ss.res));
    sets.set(ss.name, sj);
  }
  stats.set("texture_sets", sets);
  stats.set("total_ms", total.ms());
  stats.set("threads", thread_count());
  bk->stats = stats;
  return bk;
}

}  // namespace pt
