// Tangent-space normal baking.
//
// High -> low: every texel of the low mesh casts rays from an automatic cage (the low surface pushed out
// along position-welded, averaged normals, so rays stay continuous across hard edges and there are no
// gaps) back through the surface, and takes the first high-poly hit. Ray direction ("skew"): averaged
// cage normals everywhere make details on large faces project at an angle (Substance's skew). Here the
// direction turns from the cage normal at hard edges to the low's shading normal away from them
// (skew = auto), or blends by a fixed amount. Parts are matched by name (Foo_low <- Foo_high) and each
// group traces its own acceleration structure, so a part can never pick up another part's surface.
//
// Bevel shader: rounded edges from ray sampling (after Cycles' Bevel node): rays through a disk around
// the point along the normal and both tangents, multiple importance sampling between the three
// projections and a cubic falloff. Only runs near feature edges; stays within the same part by default.
//
// The rays are traced on the GPU when there is one (shaders/bake.hlsl mirrors the estimators here), otherwise
// on all cores.
//
// Results are denoised with an edge-avoiding a-trous filter in UV space (guided by 3D position and
// normal, so nothing blurs across seams or islands) and encoded in the low's MikkTSpace basis.
//
// Mesh maps: with a high poly, AO, thickness and curvature are measured where each texel lands on the
// high-poly surface (as Toolbag bakes them), so masks follow its bevels, bolts and creases instead of the low
// poly's edges. Without one, the bevelled normal is turned into extra curvature so edge wear follows it.
//
// Height: how far above or below the low surface the high-poly hits lie (along the low's shading normal),
// averaged over a texel's supersamples, for parallax and tessellation. One range serves every texture set.
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <unordered_map>

#include "bake.h"
#include "bake_gpu.h"
#include "eval.h"

namespace pt {

void NormalBakeSettings::from_json(const Json& j) {
  if (!j.is_object()) return;
  high = j.str("high", high);
  match = to_lower(j.str("match", match));
  if (match != "name" && match != "all") fail("bake.normal.match must be \"name\" or \"all\"");
  cage = std::clamp(j.numf("cage", cage), 0.f, 1.f);
  depth = std::clamp(j.numf("depth", depth), 0.f, 1.f);
  if (const Json* sk = j.find("skew")) {
    if (sk->is_string() && to_lower(sk->as_str()) == "auto") skew = -1.f;
    else if (sk->is_number()) skew = saturate(sk->as_float());
    else fail("bake.normal.skew must be \"auto\" or 0..1");
  }
  skew_distance = std::clamp(j.numf("skew_distance", skew_distance), 1e-4f, 1.f);
  ignore_backfaces = j.boolean("ignore_backfaces", ignore_backfaces);
  samples = std::clamp(j.integer("samples", samples), 1, 64);
  denoise = j.boolean("denoise", denoise);
  curvature = std::clamp(j.numf("curvature", curvature), 0.f, 10.f);
  cage_mesh = j.str("cage_mesh", cage_mesh);
  ao_from_high = j.boolean("ao_from_high", ao_from_high);
  curvature_from_high = j.boolean("curvature_from_high", curvature_from_high);
  if (const Json* hr = j.find("height_range")) {
    if (hr->is_string() && to_lower(hr->as_str()) == "auto") height_range = 0.f;
    else if (hr->is_number() && hr->as_float() > 0) height_range = std::clamp(hr->as_float(), 1e-5f, 1.f);
    else fail("bake.normal.height_range must be \"auto\" or a fraction of the largest dimension (> 0)");
  }
  auto masked = [](const Json* m, const char* key, Json& mask, float& to, float lo, float hi) {
    if (!m) return;
    if (!m->is_object() || !m->has("mask")) fail("bake.normal.%s must be {\"to\": value, \"mask\": [effects...]}", key);
    mask = (*m)["mask"];
    to = std::clamp(m->numf("to", to), lo, hi);
  };
  masked(j.find("cage_mask"), "cage_mask", cage_mask, cage_to, 0.f, 1.f);
  masked(j.find("skew_mask"), "skew_mask", skew_mask, skew_to, 0.f, 1.f);
  if (const Json* b = j.find("bevel")) {
    if (b->is_number()) bevel_radius = b->as_float();
    else if (b->is_object()) {
      bevel_radius = b->numf("radius", 0.006f);
      bevel_samples = std::clamp(b->integer("samples", bevel_samples), 4, 1024);
      bevel_min_angle = std::clamp(b->numf("min_angle", bevel_min_angle), 0.f, 90.f);
      bevel_same_part = b->str("scope", "part") != "all";
    } else fail("bake.normal.bevel must be a radius or {\"radius\":0.006,\"samples\":32,\"scope\":\"part\"|\"all\"}");
    bevel_radius = std::clamp(bevel_radius, 0.f, 0.25f);
  }
}

Json NormalBakeSettings::to_json() const {
  Json j = Json::object();
  j.set("high", high);
  j.set("match", match);
  j.set("cage", cage);
  j.set("depth", depth);
  j.set("skew", skew < 0 ? Json("auto") : Json(skew));
  j.set("skew_distance", skew_distance);
  j.set("ignore_backfaces", ignore_backfaces);
  j.set("samples", samples);
  j.set("denoise", denoise);
  j.set("curvature", curvature);
  j.set("cage_mesh", cage_mesh);
  j.set("ao_from_high", ao_from_high);
  j.set("curvature_from_high", curvature_from_high);
  j.set("height_range", height_range > 0 ? Json(height_range) : Json("auto"));
  if (!cage_mask.is_null()) { Json m = Json::object(); m.set("to", cage_to); m.set("mask", cage_mask); j.set("cage_mask", m); }
  if (!skew_mask.is_null()) { Json m = Json::object(); m.set("to", skew_to); m.set("mask", skew_mask); j.set("skew_mask", m); }
  Json b = Json::object();
  b.set("radius", bevel_radius);
  b.set("samples", bevel_samples);
  b.set("min_angle", bevel_min_angle);
  b.set("scope", bevel_same_part ? "part" : "all");
  j.set("bevel", b);
  return j;
}

namespace {

inline vec3 tri_normal(const Mesh& m, uint32_t t) {
  vec3 a = m.pos[m.idx[t * 3]], b = m.pos[m.idx[t * 3 + 1]], c = m.pos[m.idx[t * 3 + 2]];
  return normalize(cross(b - a, c - a));
}

inline vec3 barycentric(const Mesh& m, uint32_t t, vec3 p) {
  vec3 a = m.pos[m.idx[t * 3]], b = m.pos[m.idx[t * 3 + 1]], c = m.pos[m.idx[t * 3 + 2]];
  vec3 v0 = b - a, v1 = c - a, v2 = p - a;
  float d00 = dot(v0, v0), d01 = dot(v0, v1), d11 = dot(v1, v1), d20 = dot(v2, v0), d21 = dot(v2, v1);
  float den = d00 * d11 - d01 * d01;
  if (std::fabs(den) < 1e-30f) return {1.f / 3, 1.f / 3, 1.f / 3};
  float v = (d11 * d20 - d01 * d21) / den, w = (d00 * d21 - d01 * d20) / den;
  return {1.f - v - w, v, w};
}

inline vec3 smooth_normal(const Mesh& m, uint32_t t, float u, float v) {
  vec3 n = m.nrm[m.idx[t * 3]] * (1.f - u - v) + m.nrm[m.idx[t * 3 + 1]] * u + m.nrm[m.idx[t * 3 + 2]] * v;
  return normalize(n);
}

// Bake group of a part name (Toolbag's rule, case-insensitive): everything before the first "high"/"low"
// token (also hi/lo/hp/lp) delimited by '_', '.', '-' or ' '; anything after it is a variation of the same
// group, so Plate_high_bolts joins Plate. Blender ".001" duplicate suffixes are ignored.
std::string match_name(std::string s) {
  s = to_lower(s);
  while (s.size() > 4 && s[s.size() - 4] == '.' && std::isdigit((unsigned char)s[s.size() - 1]) &&
         std::isdigit((unsigned char)s[s.size() - 2]) && std::isdigit((unsigned char)s[s.size() - 3]))
    s.resize(s.size() - 4);
  auto delim = [](char c) { return c == '_' || c == '.' || c == '-' || c == ' '; };
  size_t i = 0;
  while (i < s.size()) {
    size_t j = i;
    while (j < s.size() && !delim(s[j])) j++;
    std::string tok = s.substr(i, j - i);
    if (i > 0 && (tok == "high" || tok == "low" || tok == "hi" || tok == "lo" || tok == "hp" || tok == "lp")) return s.substr(0, i - 1);
    i = j + 1;
  }
  return s;
}

// A BVH over a subset of a mesh's triangles, reporting original triangle ids.
struct SubBVH {
  BVH bvh;
  std::vector<uint32_t> tri;  // local -> original triangle, ascending
  void build(const Mesh& m) {
    std::vector<uint32_t> idx;
    idx.reserve(tri.size() * 3);
    for (uint32_t t : tri) for (int k = 0; k < 3; k++) idx.push_back(m.idx[t * 3 + k]);
    if (!idx.empty()) bvh.build(m.pos, idx);
  }
  bool hit(vec3 o, vec3 d, float tmin, float tmax, float& t, uint32_t& orig, float& u, float& v, BVH::Facing facing = BVH::kAny) const {
    if (bvh.empty()) return false;
    uint32_t local;
    if (!bvh.intersect(o, d, tmin, tmax, t, local, u, v, facing)) return false;
    orig = tri[local];
    return true;
  }
};

// The BVHs one bake traces, each over some of a mesh's triangles. Bake groups, the bevel's scopes and the whole
// mesh are often the same triangles (a part per group, or one group for everything): those share one BVH, and
// all of them are built at the same time.
struct BvhSet {
  std::vector<SubBVH> list;
  int add(std::vector<uint32_t> tris) {  // ascending triangle ids; returns the index of their BVH
    for (size_t k = 0; k < list.size(); k++) if (list[k].tri == tris) return (int)k;
    list.emplace_back();
    list.back().tri = std::move(tris);
    return (int)list.size() - 1;
  }
  void build(const Mesh& m) {
    parallel_for((int64_t)list.size(), 1, [&](int64_t b, int64_t e) {
      for (int64_t k = b; k < e; k++) list[k].build(m);
    });
  }
  const SubBVH& operator[](int k) const { return list[k]; }
};

// Uniform grid of segments for "distance to the nearest edge" queries within `radius`.
struct EdgeGrid {
  float cell = 1, radius = 1;
  std::unordered_map<uint64_t, std::vector<uint32_t>> cells;
  std::vector<vec3> a, b;
  static uint64_t key(int x, int y, int z) {
    return ((uint64_t)(uint32_t)(x + 1048576) << 42) ^ ((uint64_t)(uint32_t)(y + 1048576) << 21) ^ (uint64_t)(uint32_t)(z + 1048576);
  }
  void build(float r) {
    radius = r;
    cell = std::fmax(r, 1e-6f);
    for (uint32_t s = 0; s < a.size(); s++) {
      vec3 d = b[s] - a[s];
      int steps = std::max(1, (int)std::ceil(length(d) / (cell * 0.5f)));
      uint64_t last = UINT64_MAX;
      for (int k = 0; k <= steps; k++) {
        vec3 p = a[s] + d * ((float)k / steps);
        int cx = (int)std::floor(p.x / cell), cy = (int)std::floor(p.y / cell), cz = (int)std::floor(p.z / cell);
        for (int z = cz - 1; z <= cz + 1; z++)
          for (int y = cy - 1; y <= cy + 1; y++)
            for (int x = cx - 1; x <= cx + 1; x++) {
              auto& v = cells[key(x, y, z)];
              if (v.empty() || v.back() != s) v.push_back(s);
            }
        (void)last;
      }
    }
  }
  // distance to the nearest segment, capped at `radius`
  float distance(vec3 p) const {
    auto it = cells.find(key((int)std::floor(p.x / cell), (int)std::floor(p.y / cell), (int)std::floor(p.z / cell)));
    float best = radius;
    if (it == cells.end()) return best;
    for (uint32_t s : it->second) {
      vec3 d = b[s] - a[s];
      float t = saturate(dot(p - a[s], d) / std::fmax(dot(d, d), 1e-20f));
      best = std::fmin(best, length(p - (a[s] + d * t)));
    }
    return best;
  }
};

// Edges of `m` where the surface creases (dihedral > min_angle_deg) or the normals are split (hard edges).
void feature_edges(const Mesh& m, const std::vector<uint32_t>& weld, float min_angle_deg, bool split_only, EdgeGrid& out) {
  struct E { uint32_t t; vec3 na, nb; uint32_t wa; };
  std::unordered_map<uint64_t, E> first;
  float cos_min = std::cos(min_angle_deg * kPi / 180.f);
  for (uint32_t t = 0; t < m.tri_count(); t++) {
    for (int k = 0; k < 3; k++) {
      uint32_t va = m.idx[t * 3 + k], vb = m.idx[t * 3 + (k + 1) % 3];
      uint32_t wa = weld[va], wb = weld[vb];
      if (wa == wb) continue;
      uint64_t key = wa < wb ? ((uint64_t)wa << 32 | wb) : ((uint64_t)wb << 32 | wa);
      auto it = first.find(key);
      if (it == first.end()) { first.emplace(key, E{t, m.nrm[va], m.nrm[vb], wa}); continue; }
      const E& e = it->second;
      vec3 na = e.wa == wa ? e.na : e.nb, nb = e.wa == wa ? e.nb : e.na;
      bool split = dot(na, m.nrm[va]) < 0.999f || dot(nb, m.nrm[vb]) < 0.999f;
      bool crease = !split_only && dot(tri_normal(m, e.t), tri_normal(m, t)) < cos_min;
      if (split || crease) { out.a.push_back(m.pos[va]); out.b.push_back(m.pos[vb]); }
    }
  }
}

// An EdgeGrid on the GPU: its cells as an open-addressing hash table over the two halves of EdgeGrid::key
// (the kernel's edge_distance probes it the same way), the segment lists end to end, and the segments.
void upload_edges(Gpu& gpu, const EdgeGrid& g, GpuScene& scene, GpuParams& p) {
  size_t cap = 1;
  while (cap < g.cells.size() * 2) cap <<= 1;
  std::vector<GpuEdgeCell> table(cap, GpuEdgeCell{0, 0, 0, 0});
  std::vector<uint32_t> list;
  for (auto& kv : g.cells) {
    if (kv.second.empty()) continue;
    uint32_t lo = (uint32_t)kv.first, hi = (uint32_t)(kv.first >> 32);
    size_t slot = hash_u32(lo ^ hash_u32(hi)) & (cap - 1);
    while (table[slot].count) slot = (slot + 1) & (cap - 1);
    table[slot] = {lo, hi, (uint32_t)list.size(), (uint32_t)kv.second.size()};
    list.insert(list.end(), kv.second.begin(), kv.second.end());
  }
  std::vector<GpuSeg> segs(g.a.size());
  for (size_t k = 0; k < segs.size(); k++) segs[k] = {g.a[k], 0.f, g.b[k], 0.f};
  scene.edge_cells = gpu.upload(table.data(), table.size(), sizeof(GpuEdgeCell));
  scene.edge_list = gpu.upload(list.data(), list.size(), sizeof(uint32_t));
  scene.edge_segs = gpu.upload(segs.data(), segs.size(), sizeof(GpuSeg));
  p.edge_cell = g.cell;
  p.edge_radius = g.radius;
  p.edge_mask = (uint32_t)(cap - 1);
}

inline vec2 hammersley2(uint32_t i, uint32_t n, uint32_t scramble) {
  uint32_t b = i;
  b = (b << 16u) | (b >> 16u);
  b = ((b & 0x55555555u) << 1u) | ((b & 0xAAAAAAAAu) >> 1u);
  b = ((b & 0x33333333u) << 2u) | ((b & 0xCCCCCCCCu) >> 2u);
  b = ((b & 0x0F0F0F0Fu) << 4u) | ((b & 0xF0F0F0F0u) >> 4u);
  b = ((b & 0x00FF00FFu) << 8u) | ((b & 0xFF00FF00u) >> 8u);
  float x = (i + hash_float(scramble)) / n;
  return {x - std::floor(x), (float)(b ^ scramble) * 2.3283064365386963e-10f};
}

// Bevel shader: a rounded-edge normal at p (geometric normal ng) from surfaces of `bvh` within `R`.
vec3 bevel_normal(const Mesh& m, const SubBVH& bvh, vec3 p, vec3 ng, vec3 fallback, float R, int S, uint32_t seed) {
  vec3 t0, b0;
  onb(ng, t0, b0);
  const vec3 axes[3] = {ng, t0, b0};
  const float axis_p[3] = {0.5f, 0.25f, 0.25f};
  vec3 sum{0, 0, 0};
  float eps = R * 1e-4f;
  for (int j = 0; j < S; j++) {
    // stratified projection axis (half along the normal, a quarter along each tangent)
    int ax = j * 4 < S * 2 ? 0 : (j * 4 < S * 3 ? 1 : 2);
    int j0 = ax == 0 ? 0 : (ax == 1 ? S / 2 : S * 3 / 4), jn = ax == 0 ? S / 2 : (ax == 1 ? S * 3 / 4 - S / 2 : S - S * 3 / 4);
    vec2 xi = hammersley2((uint32_t)(j - j0), (uint32_t)std::max(jn, 1), seed + (uint32_t)ax * 0x68bc21ebu);
    vec3 dN = axes[ax], dT = axes[(ax + 1) % 3], dB = axes[(ax + 2) % 3];
    float r = R * std::sqrt(xi.x), phi = 2.f * kPi * xi.y;
    vec3 origin = p + dT * (r * std::cos(phi)) + dB * (r * std::sin(phi)) + dN * R;
    float tmin = 0.f;
    for (int h = 0; h < 4; h++) {  // every surface the probe crosses within the disk's slab
      float t, u, v;
      uint32_t tri;
      if (!bvh.hit(origin, -dN, tmin, 2.f * R, t, tri, u, v)) break;
      tmin = t + eps;
      vec3 hp = origin - dN * t;
      vec3 d = hp - p;
      float dist = length(d);
      if (dist >= R) continue;
      vec3 hg = tri_normal(m, tri);
      vec3 hn = smooth_normal(m, tri, u, v);
      if (dot(hn, ng) < -0.25f) continue;  // back side of a thin wall
      // power-heuristic MIS over the three projections (as Cycles): w = p_sampled / sum(p_k^2)
      float p2 = 0, ps = axis_p[ax] * std::fabs(dot(axes[ax], hg));
      for (int k = 0; k < 3; k++) {
        vec3 dp = d - axes[k] * dot(d, axes[k]);
        if (dot(dp, dp) < R * R) { float pk = axis_p[k] * std::fabs(dot(axes[k], hg)); p2 += pk * pk; }
      }
      if (p2 <= 1e-8f) continue;
      float f = 1.f - dist / R;
      sum += hn * (f * f * f * ps / p2);
    }
  }
  return length2(sum) > 1e-12f ? normalize(sum) : fallback;
}

// Edge-avoiding a-trous (B3 spline, steps 1,2,4,8) over per-sample vectors in UV space.
void denoise_vectors(const SampleSet& ss, std::vector<vec3>& v, const std::vector<uint8_t>& active) {
  int res = ss.res;
  static const float k[5] = {1.f / 16, 1.f / 4, 3.f / 8, 1.f / 4, 1.f / 16};
  std::vector<vec3> cur = v, next(v.size());
  for (int step : {1, 2, 4, 8}) {
    parallel_for((int64_t)ss.size(), 256, [&](int64_t b, int64_t e) {
      for (int64_t i = b; i < e; i++) {
        if (!active[i]) { next[i] = cur[i]; continue; }
        int t = ss.texel[i], x = t % res, y = t / res;
        float texel = std::fmax(ss.len_u[i], ss.len_v[i]);
        float sp2 = 2.f * (texel * step) * (texel * step) * 2.f;
        vec3 acc{0, 0, 0};
        float wsum = 0;
        for (int dy = -2; dy <= 2; dy++)
          for (int dx = -2; dx <= 2; dx++) {
            int nx = x + dx * step, ny = y + dy * step;
            if (nx < 0 || ny < 0 || nx >= res || ny >= res) continue;
            int32_t s = ss.pad[(size_t)ny * res + nx];
            if (s < 0) continue;
            float w = k[dx + 2] * k[dy + 2];
            w *= std::exp(-length2(ss.pos[s] - ss.pos[i]) / sp2);           // same place in 3D
            float c = std::fmax(0.f, dot(ss.fnrm[s], ss.fnrm[i]));
            c *= c; c *= c; c *= c; c *= c; c *= c;  // ^32: same face orientation
            w *= c;
            acc += cur[s] * w;
            wsum += w;
          }
        next[i] = wsum > 0 ? normalize(acc) : cur[i];
      }
    });
    cur.swap(next);
  }
  v.swap(cur);
}

}  // namespace

void bake_normals(Baked& bk, const Mesh* high, const BakeSettings& bs, Json& stats, Gpu* gpu) {
  const Mesh& low = *bk.mesh;
  std::vector<SampleSet>& sets = bk.sets;
  const NormalBakeSettings& s = bs.normal;
  Timer total;
  float ext = low.max_extent();
  float cage = s.cage * ext, depth = s.depth * ext, R = s.bevel_radius * ext;
  float eps = ext * 1e-6f;
  Json st = Json::object();

  // averaged (cage) normals: angle-weighted face normals per welded position
  uint32_t nweld = 0;
  std::vector<uint32_t> weld = weld_by_position(low, &nweld);
  std::vector<vec3> avg(nweld, vec3(0.f));
  for (uint32_t t = 0; t < low.tri_count(); t++) {
    vec3 fn = tri_normal(low, t);
    for (int k = 0; k < 3; k++) {
      vec3 p = low.pos[low.idx[t * 3 + k]], a = low.pos[low.idx[t * 3 + (k + 1) % 3]], b = low.pos[low.idx[t * 3 + (k + 2) % 3]];
      float ang = std::acos(clampf(dot(normalize(a - p), normalize(b - p)), -1.f, 1.f));
      avg[weld[low.idx[t * 3 + k]]] += fn * ang;
    }
  }
  for (auto& n : avg) n = normalize(n);

  // hard edges (split normals) for auto skew
  EdgeGrid hard;
  bool auto_skew = s.skew < 0 && high;
  if (auto_skew) {
    feature_edges(low, weld, 180.f, true, hard);
    hard.build(s.skew_distance * ext);
  }
  // feature edges for the bevel shader (low only; with a high mesh the bevel runs on the high)
  EdgeGrid feat_low;
  if (R > 0 && !high) {
    feature_edges(low, weld, s.bevel_min_angle, false, feat_low);
    feat_low.build(R);
  }

  // Mesh maps on the high poly. For AO and thickness the whole high mesh occludes (parts ground each other,
  // like Toolbag's ignore groups); curvature is that of the piece of surface a texel lands on.
  bool ao_high = high && s.ao_from_high, curvature_high = high && s.curvature_from_high;
  bool maps_high = ao_high || curvature_high;

  // The BVHs: over the high poly when there is one (bake groups, bevel scopes, the whole mesh for its maps),
  // otherwise over the low (bevel scopes).
  const Mesh& traced = high ? *high : low;
  BvhSet bvhs;
  std::vector<int> low_group(low.part_names.size(), -1);  // low part -> BVH of the matching high parts
  std::vector<int> part_bvh;                              // part of the traced mesh -> BVH its bevel rounds against
  int whole = -1;                                         // BVH of every triangle of the traced mesh
  auto get_whole = [&]() {
    if (whole < 0) {
      std::vector<uint32_t> all(traced.tri_count());
      for (uint32_t t = 0; t < all.size(); t++) all[t] = t;
      whole = bvhs.add(std::move(all));
    }
    return whole;
  };
  std::vector<std::vector<uint32_t>> tris_of_part(traced.part_names.size());
  for (uint32_t t = 0; t < traced.tri_count(); t++) tris_of_part[traced.tri_part[t]].push_back(t);
  Json gj = Json::array();
  std::vector<std::string> warnings;
  std::vector<uint32_t> weld_high;
  EdgeGrid feat_high;
  if (high) {
    std::vector<uint8_t> high_used(high->part_names.size(), 0);
    for (size_t lp = 0; lp < low.part_names.size(); lp++) {
      Json g = Json::object();
      g.set("low", low.part_names[lp]);
      Json hs = Json::array();
      if (s.match == "name") {
        std::string base = match_name(low.part_names[lp]);
        std::vector<uint32_t> tris;
        for (size_t hp = 0; hp < high->part_names.size(); hp++)
          if (match_name(high->part_names[hp]) == base) {
            tris.insert(tris.end(), tris_of_part[hp].begin(), tris_of_part[hp].end());
            high_used[hp] = 1;
            hs.push(high->part_names[hp]);
          }
        if (!tris.empty()) {
          std::sort(tris.begin(), tris.end());
          low_group[lp] = bvhs.add(std::move(tris));
        } else {
          low_group[lp] = get_whole();
          hs.push("*");
          warnings.push_back("low part '" + low.part_names[lp] + "' has no matching high part (name it '" + base +
                             "_high'); it bakes against the whole high mesh");
        }
      } else {
        low_group[lp] = get_whole();
        hs.push("*");
      }
      g.set("high", hs);
      gj.push(g);
    }
    if (s.match == "name" && whole < 0)  // no low part bakes against the whole mesh
      for (size_t hp = 0; hp < high->part_names.size(); hp++)
        if (!high_used[hp]) warnings.push_back("high part '" + high->part_names[hp] + "' matches no low part and is ignored");
    if (R > 0) {
      weld_high = weld_by_position(*high, nullptr);
      feature_edges(*high, weld_high, s.bevel_min_angle, false, feat_high);
      feat_high.build(R);
    }
  }
  if (R > 0) {  // the bevel shader stays within a part, or sees the whole mesh
    if (s.bevel_same_part) for (auto& tris : tris_of_part) part_bvh.push_back(bvhs.add(tris));
    else get_whole();
  }
  if (maps_high) get_whole();
  {
    Timer t;
    bvhs.build(traced);
    st.set("bvh_ms", t.ms());
  }
  auto bevel_bvh = [&](uint32_t tri) { return s.bevel_same_part ? part_bvh[traced.tri_part[tri]] : whole; };
  auto bevel_at = [&](vec3 p, uint32_t tri, vec3 shading, uint32_t seed) {
    const EdgeGrid& fg = high ? feat_high : feat_low;
    if (fg.distance(p) >= R) return shading;  // nothing to round within reach
    return bevel_normal(traced, bvhs[bevel_bvh(tri)], p, tri_normal(traced, tri), shading, R, s.bevel_samples, seed);
  };

  // explicit cage: same triangles as the low (e.g. a pushed copy exported from the DCC)
  std::unique_ptr<Mesh> cage_m;
  if (!s.cage_mesh.empty() && high) {
    cage_m = std::make_unique<Mesh>(load_mesh(s.cage_mesh, true));
    if (cage_m->tri_count() != low.tri_count())
      fail("cage mesh '%s' has %zu triangles but the low poly has %zu: export the cage as a pushed copy of the low (same topology)",
           s.cage_mesh.c_str(), cage_m->tri_count(), low.tri_count());
  }
  CurvatureField high_curvature;
  if (curvature_high) {
    // every edge counts: on a dense mesh the many slight ones add up to its curvature
    Timer t;
    high_curvature.build(*high, bs.curvature_radius * ext, 0.f, &bvhs[whole].bvh);
    st.set("curvature_edge_samples", (int64_t)high_curvature.sample_count());
    st.set("curvature_ms", t.ms());
  }

  int grid = std::max(1, (int)std::lround(std::sqrt((float)s.samples)));

  // On the GPU: the BVHs in one scene (a BVH's index there is its index in `bvhs`), the traced mesh's normals,
  // and the feature edges the projection kernel rounds against.
  GpuScene scene;
  GpuParams edge_params;
  if (gpu) {
    for (auto& b : bvhs.list) scene.add(b.bvh, &b.tri);
    scene.upload(*gpu);
    std::vector<GpuTriAttr> attr(traced.tri_count());
    parallel_for((int64_t)attr.size(), [&](int64_t b, int64_t e) {
      for (int64_t t = b; t < e; t++) {
        uint32_t bb = R > 0 ? (uint32_t)bevel_bvh((uint32_t)t) : 0;
        attr[t] = {traced.nrm[traced.idx[t * 3]], bb, traced.nrm[traced.idx[t * 3 + 1]], 0, traced.nrm[traced.idx[t * 3 + 2]], 0};
      }
    });
    scene.attr = gpu->upload(attr.data(), attr.size(), sizeof(GpuTriAttr));
    if (R > 0 && high) upload_edges(*gpu, feat_high, scene, edge_params);
  }

  double project_ms = 0, maps_ms = 0;
  int64_t misses_total = 0, samples_total = 0;
  Json sets_j = Json::object();
  for (auto& ss : sets) {
    size_t n = ss.size();
    ss.nmap.assign(n, vec3(0, 0, 1));
    ss.nmiss.assign(n, 0);
    std::vector<uint8_t> denoise_mask(n, 0);
    std::vector<vec3> world(n);
    std::vector<float> cage_k, skew_k;  // painted cage / skew (mask stacks evaluated on this set's samples)
    if (high && !s.cage_mask.is_null()) cage_k = evaluate_mask(s.cage_mask, bk, ss.set, s.base_dir, &warnings);
    if (high && !s.skew_mask.is_null()) skew_k = evaluate_mask(s.skew_mask, bk, ss.set, s.base_dir, &warnings);
    std::vector<vec3> hit_p;  // where each texel lands on the high poly (for its mesh maps)
    std::vector<uint32_t> hit_tri;
    std::vector<uint8_t> hit_ok;
    if (high) { hit_p.assign(n, vec3(0.f)); hit_tri.assign(n, 0); hit_ok.assign(n, 0); ss.height.assign(n, 0.f); }
    auto sample_seed = [&](size_t i) { return hash_u32((uint32_t)ss.texel[i] * 2654435761u + (uint32_t)ss.set * 97u); };
    Timer stage;
    if (high) {
      // auto: at hard edges dir == nc on both sides, so starting along dir is gap-free and never shifts
      // details sideways; a fixed skew starts on the continuous cage (Toolbag) so edges stay closed
      bool start_on_dir = s.skew < 0 && skew_k.empty();
      // One texel's rays (cage, skew and bake group resolved); the estimator below and the GPU kernel read it.
      auto setup = [&](size_t i) {
        uint32_t t = ss.tri[i];
        vec3 bc = barycentric(low, t, ss.pos[i]);
        GpuProjIn in{};
        in.pos = ss.pos[i];
        in.seed = sample_seed(i);
        in.ns = normalize(ss.nrm[i]);
        in.nc = normalize(avg[weld[low.idx[t * 3]]] * bc.x + avg[weld[low.idx[t * 3 + 1]]] * bc.y + avg[weld[low.idx[t * 3 + 2]]] * bc.z);
        in.tg = ss.tan[i].xyz();
        in.bt = cross(in.ns, in.tg) * ss.tan[i].w;
        in.len_u = ss.len_u[i];
        in.len_v = ss.len_v[i];
        float w = s.skew >= 0 ? s.skew : smoothstep(0.f, s.skew_distance * ext, hard.distance(ss.pos[i]));
        if (!skew_k.empty()) w = lerp(w, s.skew_to, skew_k[i]);
        in.dir = normalize(lerp(in.nc, in.ns, w));
        in.cg = cage_k.empty() ? cage : lerp(cage, s.cage_to * ext, cage_k[i]);
        if (cage_m) {  // explicit cage point for this sample
          in.pc = cage_m->pos[cage_m->idx[t * 3]] * bc.x + cage_m->pos[cage_m->idx[t * 3 + 1]] * bc.y + cage_m->pos[cage_m->idx[t * 3 + 2]] * bc.z;
          vec3 d = ss.pos[i] - in.pc;
          if (length2(d) > 1e-16f) in.dir = -normalize(d);
        }
        in.group = (uint32_t)low_group[low.tri_part[t]];
        return in;
      };
      // Is o inside a solid? Looking away from the low surface, the first thing seen is then the inside of a wall.
      auto inside = [&](const SubBVH& g, vec3 o, vec3 d) {
        float t, u, v;
        uint32_t tri;
        return g.hit(o, d, 0.f, 1e30f, t, tri, u, v) && dot(tri_normal(*high, tri), d) > 0.f;
      };
      // CPU estimator, mirrored by the project kernel in shaders/bake.hlsl
      auto project = [&](const GpuProjIn& in) {
        GpuProjOut r{};
        const SubBVH& g = bvhs[(int)in.group];
        vec3 acc{0, 0, 0};
        int miss = 0, hits = 0;
        float hsum = 0;
        for (int sy = 0; sy < grid; sy++)
          for (int sx = 0; sx < grid; sx++) {
            float ox = (sx + 0.5f) / grid - 0.5f, oy = (sy + 0.5f) / grid - 0.5f;
            vec3 p = in.pos + in.tg * (ox * in.len_u) - in.bt * (oy * in.len_v);
            vec3 o = start_on_dir ? p + in.dir * in.cg : p + in.nc * in.cg;
            float tmax = start_on_dir ? in.cg + depth : (in.cg + depth) / std::fmax(dot(in.nc, in.dir), 0.25f);
            if (cage_m) {  // rays go from the cage point through the low surface
              o = in.pc + (p - in.pos);
              tmax = length(in.pos - in.pc) + depth;
            }
            float tmin = 0.f, th, u, v;
            uint32_t ht;
            vec3 nh;
            bool found = false;
            for (int k = 0; k < 8 && !found; k++) {
              // the first high-poly surface facing the cage ...
              if (!g.hit(o, -in.dir, tmin, tmax, th, ht, u, v, s.ignore_backfaces ? BVH::kFront : BVH::kAny)) break;
              // ... unless a backface lies on it and the ray started inside a solid: that is where two parts touch
              // (a bolt resting on a plate, the cage inside the bolt), not a visible surface. Testing a window
              // around the hit instead of comparing two distances keeps this independent of float rounding. A
              // two-sided card (the same faces twice, one flipped) passes: its rays start outside.
              if (s.ignore_backfaces && g.bvh.occluded(o, -in.dir, th - eps, th + eps, BVH::kBack) && inside(g, o, in.dir)) tmin = th + eps;
              else found = true;
            }
            if (found) {
              nh = smooth_normal(*high, ht, u, v);
              vec3 hp = o - in.dir * th;
              hsum += dot(hp - p, in.ns);  // height above the low surface
              hits++;
              // the centre supersample's hit, or another one's when it misses
              if ((sx == grid / 2 && sy == grid / 2) || !(r.flags & kGpuOutHit)) {
                r.hit_p = hp;
                r.hit_tri = ht;
                r.flags |= kGpuOutHit;
              }
              if (R > 0) {
                vec3 nb = bevel_at(hp, ht, nh, in.seed + (uint32_t)(sy * grid + sx) * 7919u);
                if (dot(nb, nh) < 0.99999f) r.flags |= kGpuOutBevel;
                nh = nb;
              }
            } else {
              miss++;
              nh = in.ns;
            }
            acc += nh;
          }
        if (miss * 2 > grid * grid) r.flags |= kGpuOutMiss;
        r.n = normalize(acc);
        r.height = hits ? hsum / hits : 0.f;
        return r;
      };
      auto finish = [&](size_t i, const GpuProjOut& r) {
        world[i] = r.n;
        ss.nmiss[i] = (r.flags & kGpuOutMiss) != 0;
        denoise_mask[i] = (r.flags & kGpuOutBevel) != 0;
        hit_p[i] = r.hit_p;
        hit_tri[i] = r.hit_tri;
        hit_ok[i] = (r.flags & kGpuOutHit) != 0;
        ss.height[i] = hit_ok[i] ? r.height : 0.f;
      };
      if (gpu) {
        GpuParams p = edge_params;
        p.samples = (uint32_t)s.bevel_samples;
        p.eps = eps;
        p.grid = (uint32_t)grid;
        p.depth = depth;
        p.bevel_r = R;
        p.flags = (start_on_dir ? kGpuAutoSkew : 0) | (cage_m ? kGpuCageMesh : 0) | (s.ignore_backfaces ? kGpuBackfaces : 0) | kGpuHit;
        // The bevel costs hundreds of rays where a hit is near a feature edge and nothing elsewhere. A first pass
        // projects every texel and reports the ones near an edge; a second pass bevels only those, so the
        // threads of a dispatch do similar amounts of work.
        std::vector<uint32_t> edge_ids;
        GpuParams first_pass = p;
        if (R > 0) first_pass.flags |= kGpuBevelDetect;
        uint64_t rays = (uint64_t)grid * grid * 3;  // per texel: every supersample's hit, backface window and inside test
        gpu_run(*gpu, GpuKernel::Project, first_pass, scene, n, sizeof(GpuProjIn), sizeof(GpuProjOut), rays,
                [&](size_t first, size_t count, void* dst) {
                  parallel_for((int64_t)count, 256, [&](int64_t b, int64_t e) {
                    for (int64_t j = b; j < e; j++) ((GpuProjIn*)dst)[j] = setup(first + j);
                  });
                },
                [&](size_t first, size_t count, const void* src) {
                  const GpuProjOut* r = (const GpuProjOut*)src;
                  parallel_for((int64_t)count, 4096, [&](int64_t b, int64_t e) {
                    for (int64_t j = b; j < e; j++) finish(first + j, r[j]);
                  });
                  for (size_t j = 0; j < count; j++) if (r[j].flags & kGpuOutNear) edge_ids.push_back((uint32_t)(first + j));
                });
        gpu_run(*gpu, GpuKernel::Project, p, scene, edge_ids.size(), sizeof(GpuProjIn), sizeof(GpuProjOut),
                rays + (uint64_t)grid * grid * s.bevel_samples * 4,  // the bevel: up to 4 hits per probe
                [&](size_t first, size_t count, void* dst) {
                  parallel_for((int64_t)count, 256, [&](int64_t b, int64_t e) {
                    for (int64_t j = b; j < e; j++) ((GpuProjIn*)dst)[j] = setup(edge_ids[first + j]);
                  });
                },
                [&](size_t first, size_t count, const void* src) {
                  parallel_for((int64_t)count, 4096, [&](int64_t b, int64_t e) {
                    for (int64_t j = b; j < e; j++) finish(edge_ids[first + j], ((const GpuProjOut*)src)[j]);
                  });
                });
      } else {
        parallel_for((int64_t)n, 64, [&](int64_t b, int64_t e) {
          for (int64_t i = b; i < e; i++) finish((size_t)i, project(setup((size_t)i)));
        });
      }
    } else {
      // bevel shader on the low poly: only samples within reach of a feature edge are rounded, and the whole
      // band is denoised so it fades out smoothly
      parallel_for((int64_t)n, 256, [&](int64_t b, int64_t e) {
        for (int64_t i = b; i < e; i++) {
          world[i] = normalize(ss.nrm[i]);
          denoise_mask[i] = feat_low.distance(ss.pos[i]) < R;
        }
      });
      std::vector<uint32_t> edge_ids;
      for (size_t i = 0; i < n; i++) if (denoise_mask[i]) edge_ids.push_back((uint32_t)i);
      if (gpu) {
        GpuParams p;
        p.samples = (uint32_t)s.bevel_samples;
        p.bevel_r = R;
        gpu_run(*gpu, GpuKernel::Bevel, p, scene, edge_ids.size(), sizeof(GpuBevelIn), 4 * sizeof(float), (uint64_t)s.bevel_samples * 4,
                [&](size_t first, size_t count, void* dst) {
                  parallel_for((int64_t)count, 1024, [&](int64_t b, int64_t e) {
                    for (int64_t j = b; j < e; j++) {
                      uint32_t i = edge_ids[first + j], t = ss.tri[i];
                      ((GpuBevelIn*)dst)[j] = {ss.pos[i], sample_seed(i), normalize(ss.nrm[i]), (uint32_t)bevel_bvh(t), tri_normal(low, t), 0.f};
                    }
                  });
                },
                [&](size_t first, size_t count, const void* src) {
                  const float* v = (const float*)src;
                  for (size_t j = 0; j < count; j++) world[edge_ids[first + j]] = normalize(vec3(v[j * 4], v[j * 4 + 1], v[j * 4 + 2]));
                });
      } else {
        parallel_for((int64_t)edge_ids.size(), 16, [&](int64_t b, int64_t e) {
          for (int64_t j = b; j < e; j++) {
            uint32_t i = edge_ids[j], t = ss.tri[i];
            world[i] = normalize(bevel_normal(low, bvhs[bevel_bvh(t)], ss.pos[i], tri_normal(low, t), normalize(ss.nrm[i]), R, s.bevel_samples, sample_seed(i)));
          }
        });
      }
    }
    int64_t misses = 0;
    for (uint8_t v : ss.nmiss) misses += v;
    if (s.denoise && R > 0) denoise_vectors(ss, world, denoise_mask);
    // encode in the low's tangent space: the exact inverse of a MikkTSpace shader, which uses the
    // unnormalized interpolated vT, vN and vB = sign * cross(vN, vT): n = normalize(x vT + y vB + z vN)
    parallel_for((int64_t)n, 256, [&](int64_t b, int64_t e) {
      for (int64_t i = b; i < e; i++) {
        uint32_t t = ss.tri[i];
        vec3 bc = barycentric(low, t, ss.pos[i]);
        uint32_t i0 = low.idx[t * 3], i1 = low.idx[t * 3 + 1], i2 = low.idx[t * 3 + 2];
        vec3 vN = low.nrm[i0] * bc.x + low.nrm[i1] * bc.y + low.nrm[i2] * bc.z;
        vec3 vT = low.tan[i0].xyz() * bc.x + low.tan[i1].xyz() * bc.y + low.tan[i2].xyz() * bc.z;
        vec3 vB = cross(vN, vT) * low.tan[i0].w;
        vec3 r0 = cross(vB, vN), r1 = cross(vN, vT), r2 = cross(vT, vB);
        float sg = dot(vT, r0) < 0 ? -1.f : 1.f;
        vec3 w = world[i];
        vec3 nt = vec3(dot(w, r0), dot(w, r1), dot(w, r2)) * sg;
        if (length2(nt) < 1e-20f) nt = vec3(0, 0, 1);
        nt = normalize(nt);
        nt.z = std::fmax(nt.z, 1e-3f);
        ss.nmap[i] = normalize(nt);
      }
    });
    project_ms += stage.ms();
    if (maps_high) {
      stage = Timer();
      std::vector<uint32_t> ids;
      for (size_t i = 0; i < n; i++) if (hit_ok[i]) ids.push_back((uint32_t)i);
      // the high poly's smooth normal there, and its geometric normal on the same side
      std::vector<vec3> hit_ns(ids.size()), hit_ng(ids.size());
      parallel_for((int64_t)ids.size(), 1024, [&](int64_t b, int64_t e) {
        for (int64_t j = b; j < e; j++) {
          uint32_t i = ids[j], t = hit_tri[i];
          vec3 bc = barycentric(*high, t, hit_p[i]), ng = tri_normal(*high, t);
          hit_ns[j] = smooth_normal(*high, t, bc.y, bc.z);
          hit_ng[j] = dot(ng, hit_ns[j]) < 0 ? -ng : ng;
        }
      });
      if (ao_high) {
        std::vector<float> ao(ids.size()), thickness(ids.size());
        trace_ao_thickness(bvhs[whole].bvh, bs, ext, gpu, scene, (uint32_t)whole, ids.size(),
                           [&](size_t j) { return GpuSample{hit_p[ids[j]], ids[j], hit_ns[j], 0.f, hit_ng[j], 0.f}; }, ao.data(), thickness.data());
        parallel_for((int64_t)ids.size(), 4096, [&](int64_t b, int64_t e) {
          for (int64_t j = b; j < e; j++) { ss.ao[ids[j]] = ao[j]; ss.thickness[ids[j]] = thickness[j]; }
        });
        denoise_samples(ss, ss.ao);
        denoise_samples(ss, ss.thickness);
      }
      if (curvature_high)
        parallel_for((int64_t)ids.size(), 256, [&](int64_t b, int64_t e) {
          for (int64_t j = b; j < e; j++) {
            uint32_t i = ids[j];
            ss.curvature[i] = high_curvature.at(hit_p[i], hit_ns[j], bs.curvature_gain, high_curvature.tri_shell[hit_tri[i]]);
          }
        });
      maps_ms += stage.ms();
    }
    Json sj = Json::object();
    sj.set("misses", misses);
    sj.set("miss_fraction", n ? (double)misses / n : 0.0);
    sets_j.set(ss.name, sj);
    misses_total += misses;
    samples_total += (int64_t)n;
  }

  // Height: world units -> 0..1 around 0.5, with one range for every set so they displace alike
  if (high) {
    float lo = 0, hi = 0;
    for (auto& ss : sets)
      for (float h : ss.height) { lo = std::fmin(lo, h); hi = std::fmax(hi, h); }
    float range = s.height_range > 0 ? s.height_range * ext : std::fmax(std::fmax(-lo, hi), ext * 1e-6f);
    for (auto& ss : sets) {
      ss.height_range = range;
      parallel_for((int64_t)ss.size(), 4096, [&](int64_t b, int64_t e) {
        for (int64_t i = b; i < e; i++) ss.height[i] = saturate(0.5f + 0.5f * ss.height[i] / range);
      });
    }
    Json hj = Json::object();
    hj.set("range", range);  // world units for height 0 and 1
    hj.set("min", lo);       // world units below / above the low surface, before clamping to the range
    hj.set("max", hi);
    st.set("height", hj);
  }

  // Curvature from the baked normals, when it is not that of a high poly already: divergence of the
  // tangent-space normal over the curvature radius, added to the low poly's geometric curvature so edge wear
  // and dirt follow the bevelled edges.
  if (s.curvature > 0 && !curvature_high) {
    float rc = bs.curvature_radius * ext;
    for (auto& ss : sets) {
      int res = ss.res;
      std::vector<float> img((size_t)res * res * 3);
      std::vector<float> flat(ss.size() * 3);
      for (size_t i = 0; i < ss.size(); i++) { flat[i * 3] = ss.nmap[i].x; flat[i * 3 + 1] = ss.nmap[i].y; flat[i * 3 + 2] = ss.nmap[i].z; }
      to_image(ss, flat.data(), 3, img.data());
      std::vector<float> div(ss.size(), 0.f);
      parallel_for((int64_t)ss.size(), 256, [&](int64_t b, int64_t e) {
        for (int64_t i = b; i < e; i++) {
          int t = ss.texel[i], x = t % res, y = t / res;
          float lim = 3.f * std::fmax(ss.len_u[i], ss.len_v[i]);
          auto at = [&](int xx, int yy, int c) {
            xx = std::clamp(xx, 0, res - 1);
            yy = std::clamp(yy, 0, res - 1);
            int32_t s2 = ss.pad[(size_t)yy * res + xx];
            if (s2 < 0 || length2(ss.pos[s2] - ss.pos[i]) > lim * lim) return img[(size_t)t * 3 + c];  // across a seam: one-sided
            return img[((size_t)yy * res + xx) * 3 + c];
          };
          float dx = (at(x + 1, y, 0) - at(x - 1, y, 0)) / (2.f * ss.len_u[i]);
          float dy = (at(x, y - 1, 1) - at(x, y + 1, 1)) / (2.f * ss.len_v[i]);
          div[i] = dx + dy;
        }
      });
      // average over the curvature radius (box, separable in texture space), then add to the geometric term
      int rad = std::max(1, (int)std::lround(rc / std::fmax(1e-9f, ss.len_u.empty() ? 1.f : ss.len_u[ss.size() / 2])));
      rad = std::min(rad, 32);
      std::vector<float> dimg((size_t)res * res), tmp((size_t)res * res);
      to_image(ss, div.data(), 1, dimg.data());
      // running sums (in double, so they do not drift), with the image edge repeated: along rows, then columns
      float inv = 1.f / (2 * rad + 1);
      parallel_for(res, 4, [&](int64_t y0, int64_t y1) {
        for (int64_t y = y0; y < y1; y++) {
          const float* row = &dimg[(size_t)y * res];
          double a = 0;
          for (int k = -rad; k <= rad; k++) a += row[std::clamp(k, 0, res - 1)];
          for (int x = 0; x < res; x++) {
            tmp[(size_t)y * res + x] = (float)a * inv;
            a += (double)row[std::min(x + 1 + rad, res - 1)] - row[std::max(x - rad, 0)];
          }
        }
      });
      parallel_for(res, 64, [&](int64_t x0, int64_t x1) {
        std::vector<double> a(x1 - x0, 0.0);
        for (int k = -rad; k <= rad; k++)
          for (int64_t x = x0; x < x1; x++) a[x - x0] += tmp[(size_t)std::clamp(k, 0, res - 1) * res + x];
        for (int y = 0; y < res; y++) {
          const float* add = &tmp[(size_t)std::min(y + 1 + rad, res - 1) * res];
          const float* sub = &tmp[(size_t)std::max(y - rad, 0) * res];
          for (int64_t x = x0; x < x1; x++) {
            dimg[(size_t)y * res + x] = (float)a[x - x0] * inv;
            a[x - x0] += (double)add[x] - sub[x];
          }
        }
      });
      std::vector<float> avgdiv(ss.size());
      from_image(ss, dimg.data(), 1, avgdiv.data());
      parallel_for((int64_t)ss.size(), [&](int64_t b, int64_t e) {
        for (int64_t i = b; i < e; i++) {
          float c = clampf(ss.curvature[i] * 2.f - 1.f, -0.999f, 0.999f);
          float pre = 0.5f * std::log((1.f + c) / (1.f - c));  // atanh
          pre += avgdiv[i] * rc * s.curvature * bs.curvature_gain;
          ss.curvature[i] = 0.5f + 0.5f * std::tanh(pre);
        }
      });
    }
  }

  st.set("sets", sets_j);
  st.set("miss_fraction", samples_total ? (double)misses_total / samples_total : 0.0);
  if (high) st.set("groups", gj);
  st.set("ao", ao_high ? "high" : "low");  // which mesh AO and thickness describe
  st.set("curvature", curvature_high ? "high" : "low");
  st.set("project_ms", project_ms);  // projection (or the low-poly bevel), denoising and encoding
  if (maps_high) st.set("maps_ms", maps_ms);
  if (cage_m) st.set("cage", "mesh");
  Json wj = Json::array();
  for (auto& w : warnings) wj.push(w);
  st.set("warnings", wj);
  st.set("ms", total.ms());
  stats.set("normal", st);
}

}  // namespace pt
