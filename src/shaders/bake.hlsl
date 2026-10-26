// Patina GPU bake kernels: Direct3D 11 compute (cs_5_0), compiled at build time by fxc, one kernel per
// KERNEL_* define (see CMakeLists.txt). Every kernel mirrors a CPU estimator in bake.cpp / bake_normal.cpp
// statement by statement (same sample sequences, same BVH traversal order), so both devices agree up to
// float rounding. Change them together: `patina bake <project> --verify` reports how far apart they are.

static const float kPi = 3.14159265358979323846;

// X4714: the two 64-entry traversal stacks per thread exceed fxc's advisory register budget
#pragma warning(disable : 4714)

// GpuParams in bake_gpu.h
cbuffer Params : register(b0) {
  uint g_count;         // elements in this dispatch
  uint g_samples;       // AO rays per sample, or bevel samples
  float g_maxd;         // AO ray length
  float g_eps;
  uint g_bvh;           // BVH traced by the surface kernel
  uint g_grid;          // project: supersamples per axis
  uint g_flags;
  float g_depth;        // project: how far inside the low surface rays still look
  float g_bevel_r;
  float g_edge_cell;    // feature-edge grid
  float g_edge_radius;
  uint g_edge_mask;
  uint g_thickness_samples;
  float g_thickness_maxd;
};

#define F_AUTO_SKEW 1u     // rays start along their own direction (auto skew without a painted skew)
#define F_CAGE_MESH 2u     // rays start on an explicit cage
#define F_BACKFACES 4u     // skip high-poly backfaces
#define F_HIT 8u           // report where the texel lands on the high poly (its mesh maps are measured there)
#define F_BEVEL_DETECT 16u // only report which samples reach a feature edge; the bevel itself runs in a second pass

struct Node { float3 bmin; uint left_first; float3 bmax; uint count; };  // BVH::Node
struct Tri { float3 v0, e1, e2; };                                      // BVH::Tri
struct Bvh { uint node_base, tri_base, node_count, pad; };
struct TriAttr { float3 n0; uint bevel_bvh; float3 n1; uint pad1; float3 n2; uint pad2; };
struct EdgeCell { uint key_lo, key_hi, start, count; };
struct Seg { float3 a; float pad0; float3 b; float pad1; };

StructuredBuffer<Node> g_nodes : register(t0);
StructuredBuffer<Tri> g_tris : register(t1);         // every BVH's triangles, in BVH order
StructuredBuffer<uint> g_tri_orig : register(t2);    // BVH triangle -> triangle of the traced mesh
StructuredBuffer<Bvh> g_bvhs : register(t3);
StructuredBuffer<TriAttr> g_attr : register(t4);     // per triangle of the traced mesh
StructuredBuffer<EdgeCell> g_edge_cells : register(t5);
StructuredBuffer<uint> g_edge_list : register(t6);
StructuredBuffer<Seg> g_edge_segs : register(t7);

// ---------------------------------------------------------------- core.h
uint hash_u32(uint x) {
  x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;
  return x;
}
float hash_float(uint x) { return (float)(hash_u32(x) >> 8) * (1.0 / 16777216.0); }
float radical_inverse(uint b) { return (float)reversebits(b) * 2.3283064365386963e-10; }

float3 nrmz(float3 v) {
  float l = sqrt(dot(v, v));
  return l > 1e-20 ? v * (1.0 / l) : float3(0, 0, 1);
}

void onb(float3 n, out float3 t, out float3 b) {
  float s = n.z >= 0 ? 1.0 : -1.0;
  float a = -1.0 / (s + n.z);
  float bb = n.x * n.y * a;
  t = float3(1.0 + s * n.x * n.x * a, s * bb, -s * n.x);
  b = float3(bb, s + n.y * n.y * a, -n.y);
}

float2 hammersley2(uint i, uint n, uint scramble) {
  float x = ((float)i + hash_float(scramble)) / (float)n;
  return float2(x - floor(x), (float)(reversebits(i) ^ scramble) * 2.3283064365386963e-10);
}

// ---------------------------------------------------------------- bvh.cpp
#define ANY 0    // BVH::Facing
#define FRONT 1
#define BACK 2
float inv1(float x) { return abs(x) > 1e-20 ? 1.0 / x : (x >= 0 ? 1e20 : -1e20); }
float3 safe_inv(float3 d) { return float3(inv1(d.x), inv1(d.y), inv1(d.z)); }

bool slab(Node n, float3 o, float3 inv, float tmin, float tmax, out float tnear) {
  float3 a = (n.bmin - o) * inv, b = (n.bmax - o) * inv;
  float3 lo = min(a, b), hi = max(a, b);
  float t0 = max(max(lo.x, lo.y), lo.z), t1 = min(min(hi.x, hi.y), hi.z);
  tnear = t0;
  return t1 >= t0 && t1 > tmin && t0 < tmax;
}

bool hit_tri(Tri tr, float3 o, float3 d, float tmin, float tmax, out float t, out float u, out float v) {
  float3 p = cross(d, tr.e2);
  float det = dot(tr.e1, p);
  float inv = 1.0 / det;
  float3 s = o - tr.v0;
  u = dot(s, p) * inv;
  float3 q = cross(s, tr.e1);
  v = dot(d, q) * inv;
  t = dot(tr.e2, q) * inv;
  return abs(det) >= 1e-14 && u >= 0 && u <= 1 && v >= 0 && u + v <= 1 && t > tmin && t < tmax;
}

bool faces(Tri tr, float3 d, int facing) { return facing == ANY || (dot(cross(tr.e1, tr.e2), d) > 0.0) == (facing == BACK); }

// Any hit with tmin < t < tmax (BVH::occluded).
bool occluded(Bvh b, float3 o, float3 d, float tmin, float tmax, int facing) {
  if (b.node_count == 0) return false;
  float3 inv = safe_inv(d);
  float tn;
  if (!slab(g_nodes[b.node_base], o, inv, tmin, tmax, tn)) return false;
  uint stack[64];
  int sp = 0;
  uint ni = 0;
  bool hit = false, done = false;
  [loop] while (!done) {
    Node n = g_nodes[b.node_base + ni];
    bool pop = false;
    if (n.count != 0) {
      [loop] for (uint i = 0; i < n.count && !hit; i++) {
        Tri tr = g_tris[b.tri_base + n.left_first + i];
        float t, u, v;
        if (hit_tri(tr, o, d, tmin, tmax, t, u, v) && faces(tr, d, facing)) hit = true;
      }
      if (hit) done = true;
      else pop = true;
    } else {
      uint a = n.left_first, c = a + 1;
      float ta, tc;
      bool ha = slab(g_nodes[b.node_base + a], o, inv, tmin, tmax, ta), hc = slab(g_nodes[b.node_base + c], o, inv, tmin, tmax, tc);
      if (ha && hc) {
        if (tc < ta) { uint x = a; a = c; c = x; }
        if (sp < 64) stack[sp++] = c;
        ni = a;
      } else if (ha) ni = a;
      else if (hc) ni = c;
      else pop = true;
    }
    if (pop) {
      if (sp == 0) done = true;
      else ni = stack[--sp];
    }
  }
  return hit;
}

// Closest hit (BVH::intersect). `ref` indexes g_tris / g_tri_orig.
bool intersect(Bvh b, float3 o, float3 d, float tmin, float tmax, int facing, out float t_out, out uint ref, out float u_out, out float v_out) {
  t_out = 0; ref = 0; u_out = 0; v_out = 0;
  if (b.node_count == 0) return false;
  float3 inv = safe_inv(d);
  float best = tmax;
  float tn;
  if (!slab(g_nodes[b.node_base], o, inv, tmin, best, tn)) return false;
  uint stack_n[64];
  float stack_t[64];
  stack_n[0] = 0; stack_t[0] = tn;
  int sp = 1;
  bool found = false;
  [loop] while (sp > 0) {
    sp--;
    uint ni = stack_n[sp];
    if (!(stack_t[sp] >= best)) {
      Node n = g_nodes[b.node_base + ni];
      if (n.count != 0) {
        [loop] for (uint i = 0; i < n.count; i++) {
          uint r = b.tri_base + n.left_first + i;
          Tri tr = g_tris[r];
          float t, u, v;
          if (hit_tri(tr, o, d, tmin, best, t, u, v) && faces(tr, d, facing)) {
            best = t; found = true; ref = r; u_out = u; v_out = v;
          }
        }
      } else {
        uint a = n.left_first, c = a + 1;
        float ta, tc;
        bool ha = slab(g_nodes[b.node_base + a], o, inv, tmin, best, ta), hc = slab(g_nodes[b.node_base + c], o, inv, tmin, best, tc);
        if (ha && hc) {
          if (ta < tc) { uint x = a; a = c; c = x; float y = ta; ta = tc; tc = y; }
          if (sp < 63) {
            stack_n[sp] = a; stack_t[sp] = ta; sp++;
            stack_n[sp] = c; stack_t[sp] = tc; sp++;
          }
        } else if (ha) {
          if (sp < 64) { stack_n[sp] = a; stack_t[sp] = ta; sp++; }
        } else if (hc) {
          if (sp < 64) { stack_n[sp] = c; stack_t[sp] = tc; sp++; }
        }
      }
    }
  }
  if (found) t_out = best;
  return found;
}

// ---------------------------------------------------------------- bake.cpp: AO and thickness of a surface
#ifdef KERNEL_SURFACE
struct SampleIn { float3 pos; uint seed; float3 nrm; float pad0; float3 fnrm; float pad1; };  // GpuSample
struct SurfaceOut { uint ao; float thickness; };  // hits << 16 | valid rays; summed distance through the mesh
StructuredBuffer<SampleIn> g_in : register(t8);
RWStructuredBuffer<SurfaceOut> g_out : register(u0);

uint ao_rays(SampleIn s, Bvh bvh) {
  float3 n = s.nrm, fn = s.fnrm;
  float3 o = s.pos + fn * g_eps + n * g_eps;
  float3 t, bt;
  onb(n, t, bt);
  float r1 = hash_float(s.seed * 2u + 17u), r2 = hash_float(s.seed * 2u + 91u);
  uint hits = 0, valid = 0;
  [loop] for (uint k = 0; k < g_samples; k++) {
    float u1 = ((float)k + r1) / (float)g_samples;
    float u2 = radical_inverse(k) + r2;
    u2 -= floor(u2);
    float r = sqrt(u1), phi = 2.0 * kPi * u2;
    float3 d = t * (r * cos(phi)) + bt * (r * sin(phi)) + n * sqrt(max(0.0, 1.0 - u1));
    if (dot(d, fn) < 0.01) continue;
    valid++;
    if (occluded(bvh, o, d, 0.0, g_maxd, ANY)) hits++;
  }
  return (hits << 16) | valid;
}

float thickness_rays(SampleIn s, Bvh bvh) {
  float3 n = -s.nrm, fn = -s.fnrm;
  float3 o = s.pos + fn * g_eps;
  float3 t, bt;
  onb(n, t, bt);
  float r1 = hash_float(s.seed * 2u + 7u), r2 = hash_float(s.seed * 2u + 3u);
  float sum = 0;
  [loop] for (uint k = 0; k < g_thickness_samples; k++) {
    float u1 = ((float)k + r1) / (float)g_thickness_samples;
    float u2 = radical_inverse(k) + r2;
    u2 -= floor(u2);
    // narrower cone than AO (thickness measures "straight through")
    float ct = 1.0 - u1 * 0.6, st = sqrt(max(0.0, 1.0 - ct * ct)), phi = 2.0 * kPi * u2;
    float3 d = t * (st * cos(phi)) + bt * (st * sin(phi)) + n * ct;
    float th, uu, vv;
    uint ref;
    sum += intersect(bvh, o, d, 0.0, g_thickness_maxd, ANY, th, ref, uu, vv) ? th : g_thickness_maxd;
  }
  return sum;
}

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  uint i = id.x;
  if (i >= g_count) return;
  SampleIn s = g_in[i];
  Bvh bvh = g_bvhs[g_bvh];
  SurfaceOut r;
  r.ao = ao_rays(s, bvh);
  r.thickness = thickness_rays(s, bvh);
  g_out[i] = r;
}
#endif

// ---------------------------------------------------------------- bake_normal.cpp: projection and bevel
#if defined(KERNEL_PROJECT) || defined(KERNEL_BEVEL)
float3 tri_normal(Tri t) { return nrmz(cross(t.e1, t.e2)); }
float3 smooth_normal(TriAttr a, float u, float v) { return nrmz(a.n0 * (1.0 - u - v) + a.n1 * u + a.n2 * v); }

// EdgeGrid::distance: distance to the nearest feature edge, capped at the grid's radius.
float edge_distance(float3 p) {
  int3 c = (int3)floor(p / g_edge_cell);
  uint3 k = (uint3)(c + 1048576);
  uint key_lo = (k.y << 21) ^ k.z, key_hi = (k.x << 10) ^ (k.y >> 11);  // the two halves of EdgeGrid::key
  float best = g_edge_radius;
  uint slot = hash_u32(key_lo ^ hash_u32(key_hi)) & g_edge_mask;
  bool done = false;
  [loop] while (!done) {
    EdgeCell e = g_edge_cells[slot];
    if (e.count == 0) done = true;
    else if (e.key_lo == key_lo && e.key_hi == key_hi) {
      [loop] for (uint j = 0; j < e.count; j++) {
        Seg s = g_edge_segs[g_edge_list[e.start + j]];
        float3 d = s.b - s.a;
        float t = saturate(dot(p - s.a, d) / max(dot(d, d), 1e-20));
        best = min(best, length(p - (s.a + d * t)));
      }
      done = true;
    } else slot = (slot + 1) & g_edge_mask;
  }
  return best;
}

// Bevel shader: a rounded-edge normal at p (geometric normal ng) from surfaces of `bvh` within R.
float3 bevel_normal(Bvh bvh, float3 p, float3 ng, float3 fallback, float R, uint S, uint seed) {
  float3 t0, b0;
  onb(ng, t0, b0);
  float3 axes[3] = {ng, t0, b0};
  const float axis_p[3] = {0.5, 0.25, 0.25};
  float3 sum = float3(0, 0, 0);
  float eps = R * 1e-4;
  [loop] for (uint j = 0; j < S; j++) {
    uint ax = j * 4 < S * 2 ? 0 : (j * 4 < S * 3 ? 1 : 2);
    uint j0 = ax == 0 ? 0 : (ax == 1 ? S / 2 : S * 3 / 4), jn = ax == 0 ? S / 2 : (ax == 1 ? S * 3 / 4 - S / 2 : S - S * 3 / 4);
    float2 xi = hammersley2(j - j0, max(jn, 1u), seed + ax * 0x68bc21ebu);
    float3 dN = axes[ax], dT = axes[(ax + 1) % 3], dB = axes[(ax + 2) % 3];
    float r = R * sqrt(xi.x), phi = 2.0 * kPi * xi.y;
    float3 origin = p + dT * (r * cos(phi)) + dB * (r * sin(phi)) + dN * R;
    float tmin = 0.0;
    bool more = true;
    [loop] for (int h = 0; h < 4 && more; h++) {
      float t, u, v;
      uint ref;
      if (!intersect(bvh, origin, -dN, tmin, 2.0 * R, ANY, t, ref, u, v)) more = false;
      else {
        tmin = t + eps;
        float3 d = (origin - dN * t) - p;
        float dist = length(d);
        float3 hg = tri_normal(g_tris[ref]);
        float3 hn = smooth_normal(g_attr[g_tri_orig[ref]], u, v);
        if (dist < R && !(dot(hn, ng) < -0.25)) {
          // power-heuristic MIS over the three projections
          float p2 = 0.0, ps = axis_p[ax] * abs(dot(axes[ax], hg));
          for (int k = 0; k < 3; k++) {
            float3 dp = d - axes[k] * dot(d, axes[k]);
            if (dot(dp, dp) < R * R) { float pk = axis_p[k] * abs(dot(axes[k], hg)); p2 += pk * pk; }
          }
          if (p2 > 1e-8) {
            float f = 1.0 - dist / R;
            sum += hn * (f * f * f * ps / p2);
          }
        }
      }
    }
  }
  return dot(sum, sum) > 1e-12 ? nrmz(sum) : fallback;
}
#endif

#ifdef KERNEL_BEVEL
// Bevel shader on the low poly alone; the CPU only sends samples within reach of a feature edge.
struct BevelIn { float3 pos; uint seed; float3 ns; uint bvh; float3 ng; float pad; };  // GpuBevelIn
StructuredBuffer<BevelIn> g_in : register(t8);
RWStructuredBuffer<float4> g_out : register(u0);

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  uint i = id.x;
  if (i >= g_count) return;
  BevelIn s = g_in[i];
  g_out[i] = float4(bevel_normal(g_bvhs[s.bvh], s.pos, s.ng, s.ns, g_bevel_r, g_samples, s.seed), 0);
}
#endif

#ifdef KERNEL_PROJECT
// Is o inside a solid? Looking away from the low surface, the first thing seen is then the inside of a wall.
bool inside(Bvh g, float3 o, float3 d) {
  float t, u, v;
  uint ref;
  if (!intersect(g, o, d, 0.0, 1e30, ANY, t, ref, u, v)) return false;
  return dot(tri_normal(g_tris[ref]), d) > 0.0;
}

// High -> low: every supersample casts a ray from the cage through the low surface and takes the first
// high-poly hit (see the header of bake_normal.cpp for cage, skew and bake groups).
struct ProjIn {  // GpuProjIn
  float3 pos; uint seed;
  float3 tg; float len_u;
  float3 bt; float len_v;
  float3 ns; float cg;      // low shading normal; cage distance
  float3 nc; uint group;    // cage normal; BVH of the matching high parts
  float3 dir; float pad0;   // rays travel along -dir
  float3 pc; float pad1;    // point on the explicit cage
};
struct ProjOut { float3 n; uint flags; float3 hit_p; uint hit_tri; float height; float3 pad; };  // GpuProjOut
#define O_MISS 1u    // most supersamples found no high-poly surface
#define O_BEVEL 2u   // the bevel shader changed the normal
#define O_HIT 4u     // hit_p / hit_tri hold where the texel lands on the high poly, height its mean height
#define O_NEAR 8u    // F_BEVEL_DETECT: a hit lies within bevel reach of a feature edge
StructuredBuffer<ProjIn> g_in : register(t8);
RWStructuredBuffer<ProjOut> g_out : register(u0);

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  uint i = id.x;
  if (i >= g_count) return;
  ProjIn s = g_in[i];
  Bvh g = g_bvhs[s.group];
  uint grid = g_grid;
  float3 acc = float3(0, 0, 0);
  uint miss = 0, hits = 0, flags = 0;
  float hsum = 0.0;
  float3 hit_p = float3(0, 0, 0);
  uint hit_tri = 0;
  [loop] for (uint sy = 0; sy < grid; sy++)
    [loop] for (uint sx = 0; sx < grid; sx++) {
      float ox = ((float)sx + 0.5) / (float)grid - 0.5, oy = ((float)sy + 0.5) / (float)grid - 0.5;
      float3 p = s.pos + s.tg * (ox * s.len_u) - s.bt * (oy * s.len_v);
      float3 o;
      float tmax;
      if (g_flags & F_AUTO_SKEW) { o = p + s.dir * s.cg; tmax = s.cg + g_depth; }
      else { o = p + s.nc * s.cg; tmax = (s.cg + g_depth) / max(dot(s.nc, s.dir), 0.25); }
      if (g_flags & F_CAGE_MESH) { o = s.pc + (p - s.pos); tmax = length(s.pos - s.pc) + g_depth; }
      float3 nh = s.ns;
      float tmin = 0.0, th, u, v;
      uint ref;
      bool cull = (g_flags & F_BACKFACES) != 0, found = false, more = true;
      [loop] for (int k = 0; k < 8 && more && !found; k++) {
        // the first high-poly surface facing the cage, unless a backface lies on it and the ray started inside a
        // solid (two parts touch there)
        if (!intersect(g, o, -s.dir, tmin, tmax, cull ? FRONT : ANY, th, ref, u, v)) more = false;
        else if (cull && occluded(g, o, -s.dir, th - g_eps, th + g_eps, BACK) && inside(g, o, s.dir)) tmin = th + g_eps;
        else found = true;
      }
      if (found) {
        float3 hg = tri_normal(g_tris[ref]);
        uint orig = g_tri_orig[ref];
        nh = smooth_normal(g_attr[orig], u, v);
        float3 hp = o - s.dir * th;
        hsum += dot(hp - p, s.ns);  // height above the low surface
        hits++;
        // the centre supersample's hit, or another one's when it misses
        if ((g_flags & F_HIT) && ((sx == grid / 2 && sy == grid / 2) || !(flags & O_HIT))) {
          hit_p = hp;
          hit_tri = orig;
          flags |= O_HIT;
        }
        if (g_bevel_r > 0.0 && edge_distance(hp) < g_bevel_r) {
          if (g_flags & F_BEVEL_DETECT) flags |= O_NEAR;
          else {
            float3 nb = bevel_normal(g_bvhs[g_attr[orig].bevel_bvh], hp, hg, nh, g_bevel_r, g_samples,
                                     s.seed + (sy * grid + sx) * 7919u);
            if (dot(nb, nh) < 0.99999) flags |= O_BEVEL;
            nh = nb;
          }
        }
      } else miss++;
      acc += nh;
    }
  if (miss * 2 > grid * grid) flags |= O_MISS;
  ProjOut r;
  r.n = nrmz(acc);
  r.flags = flags;
  r.hit_p = hit_p;
  r.hit_tri = hit_tri;
  r.height = hits ? hsum / (float)hits : 0.0;
  r.pad = float3(0, 0, 0);
  g_out[i] = r;
}
#endif
