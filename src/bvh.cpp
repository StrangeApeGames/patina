#include "bvh.h"

#include <algorithm>

namespace pt {

namespace {
struct AABB {
  vec3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
  void grow(vec3 p) { lo = vmin(lo, p); hi = vmax(hi, p); }
  void grow(const AABB& b) { lo = vmin(lo, b.lo); hi = vmax(hi, b.hi); }
  float area() const {
    vec3 e = hi - lo;
    if (e.x < 0) return 0;
    return 2.f * (e.x * e.y + e.y * e.z + e.z * e.x);
  }
};
constexpr int kBins = 16;
constexpr int kMaxLeaf = 4;
}  // namespace

// Clip a convex polygon to the slab lo <= p[axis] <= hi (Sutherland-Hodgman, two planes).
static void clip_poly(const std::vector<vec3>& in, int axis, float lo, float hi, std::vector<vec3>& out) {
  std::vector<vec3> tmp;
  auto clip = [&](const std::vector<vec3>& src, std::vector<vec3>& dst, float v, bool keep_below) {
    dst.clear();
    size_t n = src.size();
    for (size_t i = 0; i < n; i++) {
      vec3 a = src[i], b = src[(i + 1) % n];
      bool ina = keep_below ? a[axis] <= v : a[axis] >= v;
      bool inb = keep_below ? b[axis] <= v : b[axis] >= v;
      if (ina) dst.push_back(a);
      if (ina != inb) {
        float t = (v - a[axis]) / (b[axis] - a[axis]);
        dst.push_back(a + (b - a) * t);
      }
    }
  };
  clip(in, tmp, hi, true);
  clip(tmp, out, lo, false);
}

// Early split clipping (Ernst & Greiner): large triangles get several tight bounding boxes, which
// fixes the overlapping-slivers problem of long thin triangles (cylinders, planks).
static void split_ref(const std::vector<vec3>& poly, const AABB& box, float thresh, int depth, uint32_t tri, std::vector<AABB>& boxes,
                      std::vector<uint32_t>& ids) {
  if (box.area() <= thresh || depth >= 6 || poly.size() < 3) {
    boxes.push_back(box);
    ids.push_back(tri);
    return;
  }
  vec3 e = box.hi - box.lo;
  int axis = e.x > e.y ? (e.x > e.z ? 0 : 2) : (e.y > e.z ? 1 : 2);
  float mid = (box.lo[axis] + box.hi[axis]) * 0.5f;
  std::vector<vec3> l, r;
  clip_poly(poly, axis, box.lo[axis], mid, l);
  clip_poly(poly, axis, mid, box.hi[axis], r);
  for (auto* half : {&l, &r}) {
    if (half->size() < 3) continue;
    AABB b;
    for (auto& p : *half) b.grow(p);
    b.lo = vmax(b.lo, box.lo);
    b.hi = vmin(b.hi, box.hi);
    split_ref(*half, b, thresh, depth + 1, tri, boxes, ids);
  }
}

void BVH::build(const std::vector<vec3>& pos, const std::vector<uint32_t>& idx) {
  size_t ntri = idx.size() / 3;
  nodes.clear();
  tris.clear();
  tri_id.clear();
  if (ntri == 0) return;
  // references (possibly several per triangle)
  std::vector<AABB> tb;
  std::vector<uint32_t> ref_tri;
  {
    double mean = 0;
    std::vector<AABB> raw(ntri);
    for (size_t t = 0; t < ntri; t++) {
      for (int k = 0; k < 3; k++) raw[t].grow(pos[idx[t * 3 + k]]);
      mean += raw[t].area();
    }
    mean /= ntri;
    float thresh = (float)mean * 4.f;
    tb.reserve(ntri * 2);
    ref_tri.reserve(ntri * 2);
    std::vector<vec3> poly(3);
    for (size_t t = 0; t < ntri; t++) {
      if (raw[t].area() <= thresh || tb.size() > ntri * 4) {
        tb.push_back(raw[t]);
        ref_tri.push_back((uint32_t)t);
        continue;
      }
      for (int k = 0; k < 3; k++) poly[k] = pos[idx[t * 3 + k]];
      split_ref(poly, raw[t], thresh, 0, (uint32_t)t, tb, ref_tri);
    }
  }
  size_t nt = tb.size();
  std::vector<vec3> cen(nt);
  std::vector<uint32_t> order(nt);
  for (size_t t = 0; t < nt; t++) {
    cen[t] = (tb[t].lo + tb[t].hi) * 0.5f;
    order[t] = (uint32_t)t;
  }
  nodes.reserve(nt * 2);
  struct Work { uint32_t node, first, count; };
  std::vector<Work> stack;
  nodes.push_back({});
  stack.push_back({0, 0, (uint32_t)nt});

  auto set_bounds = [&](uint32_t ni, uint32_t first, uint32_t count) {
    AABB b;
    for (uint32_t i = first; i < first + count; i++) b.grow(tb[order[i]]);
    Node& n = nodes[ni];
    for (int k = 0; k < 3; k++) { n.bmin[k] = b.lo[k]; n.bmax[k] = b.hi[k]; }
    return b;
  };

  while (!stack.empty()) {
    Work w = stack.back();
    stack.pop_back();
    AABB nb = set_bounds(w.node, w.first, w.count);
    auto make_leaf = [&] { nodes[w.node].left_first = w.first; nodes[w.node].count = w.count; };
    if (w.count <= kMaxLeaf) { make_leaf(); continue; }

    AABB cb;
    for (uint32_t i = w.first; i < w.first + w.count; i++) cb.grow(cen[order[i]]);
    float best_cost = 1e30f;
    int best_axis = -1, best_split = 0;
    for (int axis = 0; axis < 3; axis++) {
      float lo = cb.lo[axis], hi = cb.hi[axis];
      if (hi - lo < 1e-12f) continue;
      AABB bins[kBins];
      int cnt[kBins] = {};
      float scale = kBins / (hi - lo);
      for (uint32_t i = w.first; i < w.first + w.count; i++) {
        uint32_t t = order[i];
        int b = std::min(kBins - 1, (int)((cen[t][axis] - lo) * scale));
        cnt[b]++;
        bins[b].grow(tb[t]);
      }
      float la[kBins - 1], ra[kBins - 1];
      int lc[kBins - 1], rc[kBins - 1];
      AABB l, r;
      int ls = 0, rs = 0;
      for (int i = 0; i < kBins - 1; i++) {
        ls += cnt[i]; l.grow(bins[i]); lc[i] = ls; la[i] = l.area();
        rs += cnt[kBins - 1 - i]; r.grow(bins[kBins - 1 - i]); rc[kBins - 2 - i] = rs; ra[kBins - 2 - i] = r.area();
      }
      for (int i = 0; i < kBins - 1; i++) {
        float c = lc[i] * la[i] + rc[i] * ra[i];
        if (lc[i] && rc[i] && c < best_cost) { best_cost = c; best_axis = axis; best_split = i; }
      }
    }
    float leaf_cost = w.count * nb.area();
    if (best_axis < 0 || (best_cost >= leaf_cost && w.count <= 16)) {
      if (best_axis < 0 && w.count > kMaxLeaf) {
        // all centroids coincide: split in the middle to keep leaves small
        uint32_t half = w.count / 2;
        uint32_t li = (uint32_t)nodes.size();
        nodes.push_back({}); nodes.push_back({});
        nodes[w.node].left_first = li; nodes[w.node].count = 0;
        stack.push_back({li, w.first, half});
        stack.push_back({li + 1, w.first + half, w.count - half});
        continue;
      }
      make_leaf();
      continue;
    }
    float lo = cb.lo[best_axis];
    float scale = kBins / (cb.hi[best_axis] - lo);
    auto mid_it = std::partition(order.begin() + w.first, order.begin() + w.first + w.count, [&](uint32_t t) {
      int b = std::min(kBins - 1, (int)((cen[t][best_axis] - lo) * scale));
      return b <= best_split;
    });
    uint32_t lcount = (uint32_t)(mid_it - (order.begin() + w.first));
    if (lcount == 0 || lcount == w.count) { make_leaf(); continue; }
    uint32_t li = (uint32_t)nodes.size();
    nodes.push_back({});
    nodes.push_back({});
    nodes[w.node].left_first = li;
    nodes[w.node].count = 0;
    stack.push_back({li, w.first, lcount});
    stack.push_back({li + 1, w.first + lcount, w.count - lcount});
  }

  tris.resize(nt);
  tri_id.resize(nt);
  for (size_t i = 0; i < nt; i++) {
    uint32_t t = ref_tri[order[i]];
    tri_id[i] = t;
    vec3 a = pos[idx[t * 3]], b = pos[idx[t * 3 + 1]], c = pos[idx[t * 3 + 2]];
    tris[i] = {a, b - a, c - a};
  }
}

static inline bool slab(const BVH::Node& n, vec3 o, vec3 inv, float tmin, float tmax, float& tnear) {
  float tx1 = (n.bmin[0] - o.x) * inv.x, tx2 = (n.bmax[0] - o.x) * inv.x;
  float t0 = std::fmin(tx1, tx2), t1 = std::fmax(tx1, tx2);
  float ty1 = (n.bmin[1] - o.y) * inv.y, ty2 = (n.bmax[1] - o.y) * inv.y;
  t0 = std::fmax(t0, std::fmin(ty1, ty2)); t1 = std::fmin(t1, std::fmax(ty1, ty2));
  float tz1 = (n.bmin[2] - o.z) * inv.z, tz2 = (n.bmax[2] - o.z) * inv.z;
  t0 = std::fmax(t0, std::fmin(tz1, tz2)); t1 = std::fmin(t1, std::fmax(tz1, tz2));
  tnear = t0;
  return t1 >= t0 && t1 > tmin && t0 < tmax;
}

static inline bool hit_tri(const BVH::Tri& tr, vec3 o, vec3 d, float tmin, float tmax, float& t, float& u, float& v) {
  vec3 p = cross(d, tr.e2);
  float det = dot(tr.e1, p);
  if (std::fabs(det) < 1e-14f) return false;
  float inv = 1.0f / det;
  vec3 s = o - tr.v0;
  u = dot(s, p) * inv;
  if (u < 0.f || u > 1.f) return false;
  vec3 q = cross(s, tr.e1);
  v = dot(d, q) * inv;
  if (v < 0.f || u + v > 1.f) return false;
  t = dot(tr.e2, q) * inv;
  return t > tmin && t < tmax;
}

static inline bool faces(const BVH::Tri& tr, vec3 d, BVH::Facing facing) {
  return facing == BVH::kAny || (dot(cross(tr.e1, tr.e2), d) > 0.f) == (facing == BVH::kBack);
}

static inline vec3 safe_inv(vec3 d) {
  auto f = [](float x) { return std::fabs(x) > 1e-20f ? 1.0f / x : (x >= 0 ? 1e20f : -1e20f); };
  return {f(d.x), f(d.y), f(d.z)};
}

bool BVH::occluded(vec3 o, vec3 d, float tmin, float tmax, Facing facing) const {
  if (nodes.empty()) return false;
  vec3 inv = safe_inv(d);
  uint32_t stack[64];
  int sp = 0;
  uint32_t ni = 0;
  float tn;
  if (!slab(nodes[0], o, inv, tmin, tmax, tn)) return false;
  for (;;) {
    const Node& n = nodes[ni];
    if (n.count) {
      float t, u, v;
      for (uint32_t i = n.left_first; i < n.left_first + n.count; i++)
        if (hit_tri(tris[i], o, d, tmin, tmax, t, u, v) && faces(tris[i], d, facing)) return true;
      if (sp == 0) return false;
      ni = stack[--sp];
      continue;
    }
    uint32_t a = n.left_first, b = a + 1;
    float ta, tb;
    bool ha = slab(nodes[a], o, inv, tmin, tmax, ta), hb = slab(nodes[b], o, inv, tmin, tmax, tb);
    if (ha && hb) {
      if (tb < ta) std::swap(a, b);
      if (sp < 64) stack[sp++] = b;
      ni = a;
    } else if (ha) ni = a;
    else if (hb) ni = b;
    else {
      if (sp == 0) return false;
      ni = stack[--sp];
    }
  }
}

bool BVH::intersect(vec3 o, vec3 d, float tmin, float tmax, float& t_out, uint32_t& tri, float& u_out, float& v_out, Facing facing) const {
  if (nodes.empty()) return false;
  vec3 inv = safe_inv(d);
  struct Entry { uint32_t node; float t; };
  Entry stack[64];
  int sp = 0;
  float best = tmax;
  bool found = false;
  float tn;
  if (!slab(nodes[0], o, inv, tmin, best, tn)) return false;
  stack[sp++] = {0, tn};
  while (sp) {
    Entry e = stack[--sp];
    if (e.t >= best) continue;
    const Node& n = nodes[e.node];
    if (n.count) {
      for (uint32_t i = n.left_first; i < n.left_first + n.count; i++) {
        float t, u, v;
        if (hit_tri(tris[i], o, d, tmin, best, t, u, v) && faces(tris[i], d, facing)) {
          best = t; found = true; tri = tri_id[i]; u_out = u; v_out = v;
        }
      }
      continue;
    }
    uint32_t a = n.left_first, b = a + 1;
    float ta, tb;
    bool ha = slab(nodes[a], o, inv, tmin, best, ta), hb = slab(nodes[b], o, inv, tmin, best, tb);
    if (ha && hb) {
      if (ta < tb) { std::swap(a, b); std::swap(ta, tb); }
      if (sp < 63) { stack[sp++] = {a, ta}; stack[sp++] = {b, tb}; }
    } else if (ha) { if (sp < 64) stack[sp++] = {a, ta}; }
    else if (hb) { if (sp < 64) stack[sp++] = {b, tb}; }
  }
  if (found) t_out = best;
  return found;
}

}  // namespace pt
