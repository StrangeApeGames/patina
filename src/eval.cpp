#include "eval.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>

#include "image.h"
#include "library.h"
#include "noise.h"
#include "project.h"

namespace pt {

const ChannelInfo kChannels[C_COUNT] = {
    {"basecolor", 3, {0.5f, 0.5f, 0.5f}, true, "albedo / base color (authored as sRGB: \"#rrggbb\", [r,g,b] 0..1, or a color source)"},
    {"metallic", 1, {0.f, 0.f, 0.f}, false, "0 = dielectric, 1 = metal"},
    {"roughness", 1, {0.5f, 0.f, 0.f}, false, "0 = mirror, 1 = fully rough"},
    {"normal", 3, {0.f, 0.f, 1.f}, false, "tangent-space normal detail from normal-map images (procedural bumps go in height)"},
    {"height", 1, {0.f, 0.f, 0.f}, false, "relative height; 0 = neutral, +1 = height_depth above; converted into the normal map"},
    {"ao", 1, {1.f, 0.f, 0.f}, false, "extra occlusion multiplied with the baked AO (1 = none)"},
    {"emissive", 3, {0.f, 0.f, 0.f}, true, "emitted color (sRGB) times intensity"},
    {"opacity", 1, {1.f, 0.f, 0.f}, false, "1 = opaque"},
};

int channel_index(const std::string& raw) {
  std::string n = to_lower(raw);
  for (int i = 0; i < C_COUNT; i++) if (n == kChannels[i].name) return i;
  if (n == "base_color" || n == "albedo" || n == "color" || n == "diffuse") return C_BASECOLOR;
  if (n == "metalness" || n == "metal") return C_METALLIC;
  if (n == "rough") return C_ROUGHNESS;
  if (n == "normal_map") return C_NORMAL;
  if (n == "bump" || n == "displacement") return C_HEIGHT;
  if (n == "occlusion" || n == "ambient_occlusion") return C_AO;
  if (n == "emission" || n == "emit") return C_EMISSIVE;
  if (n == "alpha" || n == "transparency") return C_OPACITY;
  return -1;
}

static std::vector<std::string> channel_names() {
  std::vector<std::string> v;
  for (auto& c : kChannels) v.push_back(c.name);
  return v;
}

void Stack::init(size_t n_) {
  n = n_;
  for (int c = 0; c < C_COUNT; c++) {
    int k = kChannels[c].comps;
    ch[c].resize(n * k);
    float d[3];
    for (int j = 0; j < k; j++) d[j] = kChannels[c].color && c == C_BASECOLOR ? srgb_to_linear(kChannels[c].def[j]) : kChannels[c].def[j];
    for (size_t i = 0; i < n; i++)
      for (int j = 0; j < k; j++) ch[c][i * k + j] = d[j];
    used[c] = false;
  }
}

template <class F>
static void par(size_t n, F&& f) {
  parallel_for((int64_t)n, [&](int64_t b, int64_t e) {
    for (int64_t i = b; i < e; i++) f((size_t)i);
  });
}

// ---------------------------------------------------------------- context
struct Ctx {
  const Project* proj = nullptr;
  const Baked* bk = nullptr;
  const Mesh* mesh = nullptr;
  const SampleSet* ss = nullptr;
  size_t n = 0;
  vec3 center, bmin, bsize;
  float ext = 1;
  float uv_to_object = -1;  // object-size units per UV unit in this texture set (lazy, see uv_scale)
  Stack* stack = nullptr;
  std::unordered_map<std::string, std::vector<float>>* masks = nullptr;
  std::vector<std::string>* warnings = nullptr;
  std::unordered_set<std::string> warned;
  std::string path;
  const EvalOptions* opt = nullptr;
  std::unordered_set<std::string> wanted;  // layer ids whose masks must be recorded
  Json* layer_stats = nullptr;

  void warn(const std::string& msg) {
    std::string m = path.empty() ? msg : path + ": " + msg;
    if (warned.insert(m).second) warnings->push_back(m);
  }
  [[noreturn]] void error(const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    throw Error(path.empty() ? std::string(buf) : path + ": " + buf);
  }
};

struct PathScope {
  Ctx& c;
  std::string saved;
  PathScope(Ctx& c_, const std::string& add) : c(c_), saved(c_.path) { c.path += add; }
  ~PathScope() { c.path = saved; }
};

// ---------------------------------------------------------------- parsing helpers
static bool parse_vec3(const Json& j, vec3& out) {
  if (j.is_number()) { out = vec3(j.as_float()); return true; }
  if (j.is_array() && j.size() == 3 && j[0].is_number() && j[1].is_number() && j[2].is_number()) {
    out = {j[0].as_float(), j[1].as_float(), j[2].as_float()};
    return true;
  }
  return false;
}

static bool named_dir(const std::string& s0, vec3& d) {
  std::string s = to_lower(s0);
  if (s == "up" || s == "+y" || s == "y" || s == "top") d = {0, 1, 0};
  else if (s == "down" || s == "-y" || s == "bottom") d = {0, -1, 0};
  else if (s == "front" || s == "+z" || s == "z" || s == "forward") d = {0, 0, 1};
  else if (s == "back" || s == "-z" || s == "backward") d = {0, 0, -1};
  else if (s == "right" || s == "+x" || s == "x") d = {1, 0, 0};
  else if (s == "left" || s == "-x") d = {-1, 0, 0};
  else return false;
  return true;
}

static vec3 parse_dir(const Json& j, vec3 def, Ctx& c, const char* what) {
  if (j.is_null()) return def;
  vec3 d;
  if (j.is_string()) {
    if (!named_dir(j.as_str(), d)) c.error("%s: unknown direction '%s' (use up/down/front/back/left/right or [x,y,z])", what, j.as_str().c_str());
    return d;
  }
  if (parse_vec3(j, d) && length2(d) > 1e-12f) return normalize(d);
  c.error("%s: expected a direction like \"up\" or [x,y,z]", what);
}

// Points default to normalized bounding-box coordinates ([0,0,0] = min corner, [1,1,1] = max corner).
static vec3 to_world_point(const Json& j, const std::string& space, Ctx& c, vec3 def_bbox, const char* what) {
  vec3 v;
  if (j.is_null()) v = def_bbox;
  else if (!parse_vec3(j, v)) c.error("%s: expected [x,y,z]", what);
  if (space == "world") return v;
  return c.bmin + v * c.bsize;
}
static float to_world_len(float v, const std::string& space, Ctx& c) { return space == "world" ? v : v * c.ext; }

static bool hex_nibble(char ch, int& v) {
  if (ch >= '0' && ch <= '9') v = ch - '0';
  else if (ch >= 'a' && ch <= 'f') v = ch - 'a' + 10;
  else if (ch >= 'A' && ch <= 'F') v = ch - 'A' + 10;
  else return false;
  return true;
}

// Parses an sRGB color; returns linear.
static bool parse_color_srgb(const Json& j, vec3& srgb) {
  if (j.is_string()) {
    std::string s = to_lower(j.as_str());
    static const struct { const char* n; const char* hex; } named[] = {
        {"white", "#ffffff"}, {"black", "#000000"}, {"gray", "#808080"}, {"grey", "#808080"}, {"red", "#c0392b"},
        {"green", "#2e8b3a"}, {"blue", "#2c5aa0"}, {"yellow", "#e8c11c"}, {"orange", "#e07b24"}, {"brown", "#6b4423"},
        {"rust", "#8b4a2b"}, {"gold", "#ffd88a"}, {"silver", "#fcfaf5"}, {"copper", "#fab494"}, {"iron", "#c4c7c7"},
        {"steel", "#b8b9ba"}, {"aluminum", "#f5f6f6"}, {"aluminium", "#f5f6f6"}, {"brass", "#d6b97b"}, {"chrome", "#c4c5c5"},
    };
    for (auto& nm : named) if (s == nm.n) { s = nm.hex; break; }
    if (s.empty() || s[0] != '#') return false;
    s = s.substr(1);
    int v[8];
    if (s.size() == 3) {
      for (int i = 0; i < 3; i++) if (!hex_nibble(s[i], v[i])) return false;
      srgb = {v[0] * 17 / 255.f, v[1] * 17 / 255.f, v[2] * 17 / 255.f};
      return true;
    }
    if (s.size() == 6 || s.size() == 8) {
      for (int i = 0; i < 6; i++) if (!hex_nibble(s[i], v[i])) return false;
      srgb = {(v[0] * 16 + v[1]) / 255.f, (v[2] * 16 + v[3]) / 255.f, (v[4] * 16 + v[5]) / 255.f};
      return true;
    }
    return false;
  }
  if (j.is_number()) { srgb = vec3(saturate(j.as_float())); return true; }
  vec3 v;
  if (parse_vec3(j, v)) {
    if (v.x > 1.001f || v.y > 1.001f || v.z > 1.001f) v = v / 255.f;  // tolerate 0..255
    srgb = {saturate(v.x), saturate(v.y), saturate(v.z)};
    return true;
  }
  return false;
}

static vec3 srgb_vec_to_linear(vec3 c) { return {srgb_to_linear(c.x), srgb_to_linear(c.y), srgb_to_linear(c.z)}; }

struct Gradient {
  std::vector<float> pos;
  std::vector<vec3> col;  // sRGB
  vec3 at(float t) const {
    if (col.empty()) return vec3(t);
    if (t <= pos.front()) return col.front();
    if (t >= pos.back()) return col.back();
    for (size_t k = 1; k < pos.size(); k++)
      if (t <= pos[k]) {
        float f = (t - pos[k - 1]) / std::fmax(1e-6f, pos[k] - pos[k - 1]);
        return lerp(col[k - 1], col[k], f);
      }
    return col.back();
  }
};

static Gradient parse_gradient(const Json& j, Ctx& c) {
  Gradient g;
  if (!j.is_array() || j.size() == 0) c.error("gradient must be an array of colors or [position, color] pairs");
  for (size_t k = 0; k < j.size(); k++) {
    const Json& e = j[k];
    vec3 col;
    float p = j.size() == 1 ? 0.f : (float)k / (j.size() - 1);
    if (e.is_array() && e.size() == 2 && e[0].is_number()) {
      p = e[0].as_float();
      if (!parse_color_srgb(e[1], col)) c.error("gradient[%zu]: bad color", k);
    } else if (!parse_color_srgb(e, col)) {
      c.error("gradient[%zu]: bad color (use \"#rrggbb\")", k);
    }
    g.pos.push_back(p);
    g.col.push_back(col);
  }
  // keep sorted
  std::vector<size_t> ord(g.pos.size());
  for (size_t i = 0; i < ord.size(); i++) ord[i] = i;
  std::sort(ord.begin(), ord.end(), [&](size_t a, size_t b) { return g.pos[a] < g.pos[b]; });
  Gradient s;
  for (size_t i : ord) { s.pos.push_back(g.pos[i]); s.col.push_back(g.col[i]); }
  return s;
}

// ---------------------------------------------------------------- blend modes
enum BlendMode { B_NORMAL, B_MULTIPLY, B_ADD, B_SUBTRACT, B_SCREEN, B_OVERLAY, B_SOFT_LIGHT, B_MAX, B_MIN, B_DIFFERENCE, B_DIVIDE, B_SIGNED_ADD, B_COMBINE, B_REPLACE };
static const struct { const char* name; BlendMode m; const char* doc; } kBlendModes[] = {
    {"normal", B_NORMAL, "layer over stack (default)"},
    {"replace", B_REPLACE, "same as normal"},
    {"multiply", B_MULTIPLY, "stack * layer"},
    {"add", B_ADD, "stack + layer (alias: linear_dodge)"},
    {"subtract", B_SUBTRACT, "stack - layer"},
    {"screen", B_SCREEN, "1-(1-a)(1-b)"},
    {"overlay", B_OVERLAY, "contrast blend"},
    {"soft_light", B_SOFT_LIGHT, "gentle contrast blend"},
    {"max", B_MAX, "lighten (alias: lighten)"},
    {"min", B_MIN, "darken (alias: darken)"},
    {"difference", B_DIFFERENCE, "|a-b|"},
    {"divide", B_DIVIDE, "a/b"},
    {"signed_add", B_SIGNED_ADD, "a + 2*(b-0.5): values above 0.5 raise, below lower"},
    {"combine", B_COMBINE, "normal channel only: reoriented normal blending (default for normal)"},
};

static bool parse_blend(const std::string& s0, BlendMode& m) {
  std::string s = to_lower(s0);
  if (s == "linear_dodge") s = "add";
  if (s == "lighten") s = "max";
  if (s == "darken") s = "min";
  if (s == "normal_combine" || s == "rnm" || s == "detail") s = "combine";
  for (auto& b : kBlendModes) if (s == b.name) { m = b.m; return true; }
  return false;
}
static BlendMode blend_or_fail(const std::string& s, Ctx& c) {
  BlendMode m;
  if (!parse_blend(s, m)) {
    std::vector<std::string> names;
    for (auto& b : kBlendModes) names.push_back(b.name);
    std::string dym = did_you_mean(s, names);
    c.error("unknown blend mode '%s'%s", s.c_str(), dym.empty() ? "" : (" (did you mean '" + dym + "'?)").c_str());
  }
  return m;
}

static inline float blend_scalar(BlendMode m, float a, float b) {
  switch (m) {
    case B_NORMAL: case B_REPLACE: case B_COMBINE: return b;
    case B_MULTIPLY: return a * b;
    case B_ADD: return a + b;
    case B_SUBTRACT: return a - b;
    case B_SCREEN: return 1.f - (1.f - a) * (1.f - b);
    case B_OVERLAY: return a < 0.5f ? 2.f * a * b : 1.f - 2.f * (1.f - a) * (1.f - b);
    case B_SOFT_LIGHT: return (1.f - 2.f * b) * a * a + 2.f * b * a;
    case B_MAX: return std::fmax(a, b);
    case B_MIN: return std::fmin(a, b);
    case B_DIFFERENCE: return std::fabs(a - b);
    case B_DIVIDE: return b > 1e-5f ? a / b : a;
    case B_SIGNED_ADD: return a + 2.f * (b - 0.5f);
  }
  return b;
}

// Reoriented normal mapping (Barre-Brisebois & Hill).
static inline vec3 rnm(vec3 base, vec3 detail) {
  vec3 t = base + vec3(0, 0, 1);
  vec3 u = detail * vec3(-1, -1, 1);
  return normalize(t * (dot(t, u) / t.z) - u);
}

// ---------------------------------------------------------------- field registry
using FieldFn = void (*)(const Json& s, Ctx& c, float* out);
struct ParamDoc { const char* name; const char* def; const char* doc; };
struct FieldDef {
  const char* type;
  const char* category;
  const char* summary;
  std::vector<ParamDoc> params;
  FieldFn fn;
};
static const std::vector<FieldDef>& field_defs();
static void eval_field(const Json& s, Ctx& c, float* out);

static const char* kCommonKeys[] = {"type", "blend", "opacity", "enabled", "invert", "levels", "contrast", "power", "threshold", "blur",
                                     "multiply", "add", "clamp", "range", "gradient", "colors", "intensity", "name", "comment", "field", "note"};

static void check_keys(const Json& s, const std::vector<ParamDoc>& params, Ctx& c, const char* what) {
  if (!s.is_object()) return;
  for (auto& kv : s.members()) {
    const std::string& k = kv.first;
    bool ok = false;
    for (auto* ck : kCommonKeys) if (k == ck) { ok = true; break; }
    for (auto& p : params) if (k == p.name) { ok = true; break; }
    if (ok) continue;
    std::vector<std::string> cands;
    for (auto* ck : kCommonKeys) cands.push_back(ck);
    for (auto& p : params) cands.push_back(p.name);
    std::string dym = did_you_mean(k, cands);
    c.warn(strf("unknown key '%s' in %s%s (ignored)", k.c_str(), what, dym.empty() ? "" : (", did you mean '" + dym + "'?").c_str()));
  }
}

// ---------------------------------------------------------------- noise
struct NoiseP {
  std::string kind;
  float scale;
  int octaves;
  float lac, gain;
  uint32_t seed;
  vec3 stretch{1, 1, 1};
  float warp = 0;
  vec3 offset{0, 0, 0};
  float jitter = 1, width = 0.08f, size = 0.35f;
  vec3 axis{0, 1, 0};  // rings
  bool world = false;
  bool uv = false;  // x = u, y = v, z = per-island offset (boards/planks laid out along V)
  float uvk = 1;     // uv space: UV units -> object-size units, so `scale` means the same in every space
};

// Object-size units per UV unit, from the texel footprint of this texture set's samples.
static float uv_scale(Ctx& c) {
  if (c.uv_to_object < 0) {
    double sum = 0;
    size_t n = 0;
    for (size_t i = 0; i < c.n; i++)
      if (c.ss->interior[i]) { sum += std::sqrt((double)c.ss->len_u[i] * c.ss->len_v[i]); n++; }
    c.uv_to_object = n ? (float)(sum / n * c.ss->res / c.ext) : 1.f;
  }
  return c.uv_to_object;
}

static NoiseP parse_noise(const Json& s, const char* def_kind, float def_scale, Ctx& c) {
  NoiseP P;
  P.kind = to_lower(s.str("noise", def_kind));
  P.scale = s.numf("scale", def_scale);
  P.octaves = std::clamp(s.integer("octaves", 5), 1, 10);
  P.lac = s.numf("lacunarity", 2.f);
  P.gain = s.numf("gain", 0.5f);
  P.seed = (uint32_t)s.integer("seed", 0) * 0x9E3779B1u + 0x51ED27u;
  if (const Json* st = s.find("stretch")) {
    vec3 d;
    if (st->is_string() && named_dir(st->as_str(), d)) {
      // a direction name stretches features 12x along that axis (grain, brushing, drips)
      P.stretch = vec3(1.f);
      P.stretch[std::fabs(d.x) > 0.5f ? 0 : (std::fabs(d.y) > 0.5f ? 1 : 2)] = 12.f;
    } else if (st->is_object()) {
      vec3 ad = parse_dir((*st)["axis"], vec3(0, 1, 0), c, "stretch.axis");
      P.stretch = vec3(1.f);
      P.stretch[std::fabs(ad.x) > 0.5f ? 0 : (std::fabs(ad.y) > 0.5f ? 1 : 2)] = st->numf("factor", 12.f);
    } else if (!parse_vec3(*st, P.stretch)) {
      c.error("stretch must be [x,y,z], a direction name like \"up\", or {\"axis\":\"up\",\"factor\":12}");
    }
  }
  if (s.has("axis")) P.axis = parse_dir(s["axis"], vec3(0, 1, 0), c, "axis");
  if (const Json* of = s.find("offset")) { if (!parse_vec3(*of, P.offset)) c.error("offset must be [x,y,z]"); }
  P.warp = s.numf("warp", 0.f);
  P.jitter = s.numf("jitter", 1.f);
  P.width = s.numf("width", 0.08f);
  P.size = s.numf("size", 0.35f);
  std::string space = s.str("space", "object");
  if (space != "object" && space != "world" && space != "uv") c.error("space must be object, world or uv");
  P.world = space == "world";
  P.uv = space == "uv";
  if (P.uv) P.uvk = uv_scale(c);
  P.stretch = vmax(P.stretch, vec3(1e-3f));
  static const char* kinds[] = {"fbm", "perlin", "value", "ridged", "turbulence", "cells", "voronoi", "cracks", "dots", "white", "rings"};
  bool ok = false;
  for (auto* k : kinds) if (P.kind == k) ok = true;
  if (!ok) {
    std::vector<std::string> v(std::begin(kinds), std::end(kinds));
    std::string dym = did_you_mean(P.kind, v);
    c.error("unknown noise '%s'%s; kinds: fbm perlin value ridged turbulence cells voronoi cracks dots white rings", P.kind.c_str(),
            dym.empty() ? "" : (" (did you mean '" + dym + "'?)").c_str());
  }
  return P;
}

static inline vec3 noise_domain(const NoiseP& P, const Ctx& c, size_t i) {
  vec3 p;
  if (P.uv) {
    p = (vec3(c.ss->uv[i].x, c.ss->uv[i].y, 0.f) * P.uvk + P.offset) * P.scale / P.stretch;
    // every UV island (board) gets its own slice of the noise; z in 0.5..3.5 keeps rings around
    // "up" (= V) reading as straight grain rather than the ring centers
    uint32_t h = hash_u32((uint32_t)c.mesh->tri_island[c.ss->tri[i]] * 7919u + P.seed);
    p += vec3(hash_float(h) * 97.f, hash_float(h ^ 0x68bc21ebu) * 97.f, 0.5f + 3.f * hash_float(h ^ 0x2f693b52u));
  } else {
    p = P.world ? c.ss->pos[i] : (c.ss->pos[i] - c.center) / c.ext;
    p = (p + P.offset) * P.scale / P.stretch;
  }
  if (P.warp != 0.f) p += fbm3_vec(p * 0.5f, 3, P.seed ^ 0x5bd1e995u) * P.warp;
  return p;
}

// Calibrated so outputs spread over ~[0.05, 0.95] with mean 0.5.
static inline float noise_at(const NoiseP& P, vec3 p) {
  const std::string& k = P.kind;
  switch (k[0]) {
    case 'f': return 0.5f + 0.5f * std::tanh(fbm3(p, P.octaves, P.lac, P.gain, P.seed) * 3.0f);
    case 'p': return 0.5f + 0.5f * std::tanh(perlin3(p, P.seed) * 2.2f);
    case 'v':
      if (k == "value") {
        float s = 0, a = 1, nrm = 0;
        vec3 q = p;
        for (int o = 0; o < P.octaves; o++) { s += value3(q, P.seed + o * 131u) * a; nrm += a; a *= P.gain; q = q * P.lac; }
        return 0.5f + 0.5f * std::tanh(s / nrm * 2.2f);
      } else {
        Worley w = worley3(p, P.jitter, P.seed);
        return hash_float(w.id);
      }
    case 'r':
      if (k == "rings") {
        // growth rings around `axis` (sawtooth: 0 = early wood, ->1 = late wood, sharp boundary)
        vec3 radial = p - P.axis * dot(p, P.axis);
        float r = length(radial);
        return r - std::floor(r);
      }
      return ridged3(p, P.octaves, P.lac, P.gain, P.seed);
    case 't': return turbulence3(p, P.octaves, P.lac, P.gain, P.seed);
    case 'c':
      if (k == "cells") { Worley w = worley3(p, P.jitter, P.seed); return saturate(w.f1 / 0.95f); }
      else { Worley w = worley3(p, P.jitter, P.seed); return 1.f - smoothstep(0.f, P.width, w.f2 - w.f1); }
    case 'd': { Worley w = worley3(p, P.jitter, P.seed); float r = P.size * (0.6f + 0.8f * hash_float(w.id ^ 0x1b873593u)); return 1.f - smoothstep(r * 0.8f, r, w.f1); }
    case 'w': return hash_float(hash_u32((uint32_t)(int)std::floor(p.x) * 73856093u ^ (uint32_t)(int)std::floor(p.y) * 19349663u ^ (uint32_t)(int)std::floor(p.z) * 83492791u ^ P.seed));
  }
  return 0.5f;
}

static void f_noise(const Json& s, Ctx& c, float* out) {
  NoiseP P = parse_noise(s, "fbm", 4.f, c);
  par(c.n, [&](size_t i) { out[i] = noise_at(P, noise_domain(P, c, i)); });
}

// ---------------------------------------------------------------- baked-map fields
static void f_constant(const Json& s, Ctx& c, float* out) {
  float v = s.numf("value", 1.f);
  std::fill(out, out + c.n, v);
}

static inline float convex_of(float curv) { return saturate((curv - 0.5f) * 2.f); }
static inline float concave_of(float curv) { return saturate((0.5f - curv) * 2.f); }

static void f_curvature(const Json& s, Ctx& c, float* out) {
  std::string mode = s.str("mode", "convex");
  const float* cv = c.ss->curvature.data();
  if (mode == "convex") par(c.n, [&](size_t i) { out[i] = convex_of(cv[i]); });
  else if (mode == "concave") par(c.n, [&](size_t i) { out[i] = concave_of(cv[i]); });
  else if (mode == "both") par(c.n, [&](size_t i) { out[i] = std::fabs(cv[i] - 0.5f) * 2.f; });
  else if (mode == "raw") par(c.n, [&](size_t i) { out[i] = cv[i]; });
  else c.error("curvature mode must be convex, concave, both or raw");
}
static void f_ao(const Json& s, Ctx& c, float* out) {
  const float* a = c.ss->ao.data();
  par(c.n, [&](size_t i) { out[i] = a[i]; });
}
static void f_cavity(const Json& s, Ctx& c, float* out) {
  const float* a = c.ss->ao.data();
  par(c.n, [&](size_t i) { out[i] = 1.f - a[i]; });
}
static void f_thickness(const Json& s, Ctx& c, float* out) {
  const float* a = c.ss->thickness.data();
  par(c.n, [&](size_t i) { out[i] = a[i]; });
}

static void f_bake(const Json& s, Ctx& c, float* out) {
  std::string map = to_lower(s.str("map", "ao"));
  const SampleSet& ss = *c.ss;
  if (map == "ao") par(c.n, [&](size_t i) { out[i] = ss.ao[i]; });
  else if (map == "curvature") par(c.n, [&](size_t i) { out[i] = ss.curvature[i]; });
  else if (map == "convexity") par(c.n, [&](size_t i) { out[i] = convex_of(ss.curvature[i]); });
  else if (map == "concavity") par(c.n, [&](size_t i) { out[i] = concave_of(ss.curvature[i]); });
  else if (map == "thickness") par(c.n, [&](size_t i) { out[i] = ss.thickness[i]; });
  else if (map == "height") par(c.n, [&](size_t i) { out[i] = ss.height.empty() ? 0.5f : ss.height[i]; });
  else if (map.rfind("position_", 0) == 0 && map.size() == 10) {
    int ax = map[9] - 'x';
    if (ax < 0 || ax > 2) c.error("bad map '%s'", map.c_str());
    par(c.n, [&](size_t i) { out[i] = c.bsize[ax] > 0 ? (ss.pos[i][ax] - c.bmin[ax]) / c.bsize[ax] : 0.f; });
  } else if (map.rfind("normal_", 0) == 0 && map.size() == 8) {
    int ax = map[7] - 'x';
    if (ax < 0 || ax > 2) c.error("bad map '%s'", map.c_str());
    par(c.n, [&](size_t i) { out[i] = ss.nrm[i][ax] * 0.5f + 0.5f; });
  } else if (map == "island") {
    uint32_t seed = (uint32_t)s.integer("seed", 0);
    par(c.n, [&](size_t i) { out[i] = hash_float((uint32_t)c.mesh->tri_island[ss.tri[i]] * 7919u + seed * 104729u + 1u); });
  } else if (map == "uv_u") par(c.n, [&](size_t i) { out[i] = ss.uv[i].x; });
  else if (map == "uv_v") par(c.n, [&](size_t i) { out[i] = ss.uv[i].y; });
  else c.error("unknown bake map '%s' (ao curvature convexity concavity thickness height position_x|y|z normal_x|y|z island uv_u uv_v)", map.c_str());
}

// ---------------------------------------------------------------- generators
static void f_edge_wear(const Json& s, Ctx& c, float* out) {
  float amount = s.numf("amount", 0.5f), width = s.numf("width", 0.5f), breakup = s.numf("breakup", 0.6f), soft = std::fmax(1e-3f, s.numf("softness", 0.06f));
  NoiseP P = parse_noise(s, "fbm", 10.f, c);
  float gamma = lerp(3.0f, 0.3f, saturate(width));
  float t = 1.f - amount;
  const float* cv = c.ss->curvature.data();
  par(c.n, [&](size_t i) {
    float e = std::pow(convex_of(cv[i]), gamma);
    float nz = breakup > 0 ? noise_at(P, noise_domain(P, c, i)) : 0.5f;
    out[i] = smoothstep(-soft, soft, e + (nz - 0.5f) * breakup - t);
  });
}

static void f_dirt(const Json& s, Ctx& c, float* out) {
  float amount = s.numf("amount", 0.5f), aow = s.numf("ao_weight", 1.f), cw = s.numf("concavity_weight", 1.f);
  float breakup = s.numf("breakup", 0.5f), soft = std::fmax(1e-3f, s.numf("softness", 0.1f));
  NoiseP P = parse_noise(s, "fbm", 6.f, c);
  float t = 1.f - amount;
  const SampleSet& ss = *c.ss;
  par(c.n, [&](size_t i) {
    float occ = std::pow(saturate(1.f - ss.ao[i]), 0.6f) * aow;
    float d = std::fmax(occ, concave_of(ss.curvature[i]) * cw);
    float nz = breakup > 0 ? noise_at(P, noise_domain(P, c, i)) : 0.5f;
    out[i] = smoothstep(-soft, soft, d + (nz - 0.5f) * breakup - t);
  });
}

static void f_gradient(const Json& s, Ctx& c, float* out) {
  const Json& ax = s["axis"];
  float from = s.numf("from", 0.f), to = s.numf("to", 1.f);
  float breakup = s.numf("breakup", 0.f);
  NoiseP P = parse_noise(s, "fbm", 6.f, c);
  // named axes use normalized bbox coordinates; vectors use centered object coordinates
  int a = 1;
  bool flip = false, named = true;
  vec3 dir{0, 1, 0};
  if (ax.is_null() || ax.is_string()) {
    vec3 d;
    if (!named_dir(ax.is_null() ? "up" : ax.as_str(), d)) c.error("axis: unknown direction '%s'", ax.as_str().c_str());
    a = std::fabs(d.x) > 0.5f ? 0 : (std::fabs(d.y) > 0.5f ? 1 : 2);
    flip = d[a] < 0;
  } else {
    named = false;
    dir = parse_dir(ax, vec3(0, 1, 0), c, "axis");
  }
  float span = to - from;
  if (std::fabs(span) < 1e-6f) span = 1e-6f;
  par(c.n, [&](size_t i) {
    float t;
    if (named) {
      t = c.bsize[a] > 0 ? (c.ss->pos[i][a] - c.bmin[a]) / c.bsize[a] : 0.f;
      if (flip) t = 1.f - t;
    } else {
      t = dot((c.ss->pos[i] - c.center) / c.ext, dir) + 0.5f;
    }
    if (breakup > 0) t += (noise_at(P, noise_domain(P, c, i)) - 0.5f) * breakup;
    out[i] = saturate((t - from) / span);
  });
}

static void f_direction(const Json& s, Ctx& c, float* out) {
  vec3 d = parse_dir(s["direction"], vec3(0, 1, 0), c, "direction");
  float lo = s.numf("min", 0.3f), hi = s.numf("max", 0.8f);
  float breakup = s.numf("breakup", 0.f);
  NoiseP P = parse_noise(s, "fbm", 6.f, c);
  par(c.n, [&](size_t i) {
    float v = dot(c.ss->nrm[i], d);
    if (breakup > 0) v += (noise_at(P, noise_domain(P, c, i)) - 0.5f) * breakup;
    out[i] = smoothstep(lo, hi, v);
  });
}

static void f_sphere(const Json& s, Ctx& c, float* out) {
  std::string space = s.str("space", "bbox");
  vec3 ctr = to_world_point(s["center"], space, c, vec3(0.5f), "center");
  float r = to_world_len(s.numf("radius", 0.25f), space, c);
  float fall = saturate(s.numf("falloff", 0.3f));
  par(c.n, [&](size_t i) {
    float d = length(c.ss->pos[i] - ctr);
    out[i] = 1.f - smoothstep(r * (1.f - fall), r, d);
  });
}

static void f_box(const Json& s, Ctx& c, float* out) {
  std::string space = s.str("space", "bbox");
  vec3 lo = to_world_point(s["min"], space, c, vec3(0.f), "min"), hi = to_world_point(s["max"], space, c, vec3(1.f), "max");
  vec3 a = vmin(lo, hi), b = vmax(lo, hi);
  float fall = to_world_len(s.numf("falloff", 0.02f), space, c);
  par(c.n, [&](size_t i) {
    vec3 p = c.ss->pos[i];
    vec3 q = vmax(a - p, p - b);  // positive outside
    float d = std::fmax(q.x, std::fmax(q.y, q.z));
    out[i] = fall > 0 ? 1.f - smoothstep(-fall * 0.5f, fall * 0.5f, d) : (d <= 0 ? 1.f : 0.f);
  });
}

static void f_plane(const Json& s, Ctx& c, float* out) {
  std::string space = s.str("space", "bbox");
  vec3 pt = to_world_point(s["point"], space, c, vec3(0.5f), "point");
  vec3 nrm = parse_dir(s["normal"], vec3(0, 1, 0), c, "normal");
  float fall = to_world_len(s.numf("falloff", 0.02f), space, c);
  par(c.n, [&](size_t i) {
    float d = dot(c.ss->pos[i] - pt, nrm);
    out[i] = fall > 0 ? smoothstep(-fall * 0.5f, fall * 0.5f, d) : (d >= 0 ? 1.f : 0.f);
  });
}

static void f_select(const Json& s, Ctx& c, float* out) {
  const Mesh& m = *c.mesh;
  std::vector<uint8_t> part_ok(m.part_names.size(), 0);
  bool any_part = false;
  if (const Json* parts = s.find("parts")) {
    any_part = true;
    int matched = 0;
    for (auto& pj : parts->items()) {
      bool hit = false;
      for (size_t p = 0; p < m.part_names.size(); p++)
        if (glob_match(pj.as_str().c_str(), m.part_names[p].c_str())) { part_ok[p] = 1; hit = true; }
      if (hit) matched++;
      else {
        std::string dym = did_you_mean(pj.as_str(), m.part_names);
        c.warn(strf("select: part pattern '%s' matched nothing%s", pj.as_str().c_str(), dym.empty() ? "" : (" (did you mean '" + dym + "'?)").c_str()));
      }
    }
  }
  std::unordered_set<int> islands;
  bool any_island = false;
  if (const Json* is = s.find("islands")) {
    any_island = true;
    for (auto& v : is->items()) islands.insert(v.as_int(-1));
  }
  if (!any_part && !any_island) c.error("select needs \"parts\" (names/globs) and/or \"islands\" (ids)");
  const SampleSet& ss = *c.ss;
  par(c.n, [&](size_t i) {
    uint32_t t = ss.tri[i];
    bool ok = true;
    if (any_part) ok = ok && part_ok[m.tri_part[t]];
    if (any_island) ok = ok && islands.count(m.tri_island[t]);
    out[i] = ok ? 1.f : 0.f;
  });
}

static void f_island_random(const Json& s, Ctx& c, float* out) {
  uint32_t seed = (uint32_t)s.integer("seed", 0);
  const SampleSet& ss = *c.ss;
  par(c.n, [&](size_t i) { out[i] = hash_float((uint32_t)c.mesh->tri_island[ss.tri[i]] * 7919u + seed * 104729u + 1u); });
}

static void f_part_random(const Json& s, Ctx& c, float* out) {
  uint32_t seed = (uint32_t)s.integer("seed", 0);
  const SampleSet& ss = *c.ss;
  par(c.n, [&](size_t i) { out[i] = hash_float((uint32_t)c.mesh->tri_part[ss.tri[i]] * 6151u + seed * 104729u + 3u); });
}

static void rot_basis(uint32_t h, vec3& a, vec3& b, vec3& d) {
  // random orthonormal basis from a hash
  float z = hash_float(h) * 2.f - 1.f, phi = hash_float(h ^ 0xabcdefu) * 2.f * kPi;
  float r = std::sqrt(std::fmax(0.f, 1.f - z * z));
  d = {r * std::cos(phi), r * std::sin(phi), z};
  onb(d, a, b);
  float ang = hash_float(h ^ 0x1234567u) * 2.f * kPi;
  vec3 a2 = a * std::cos(ang) + b * std::sin(ang);
  b = cross(d, a2);
  a = a2;
}

// Straight scratch segments: each 3D cell may hold a few random segments; distance is measured in the
// sample's tangent plane, so any surface near a segment shows a clean straight line (not noise worms).
static void f_scratches(const Json& s, Ctx& c, float* out) {
  float scale = std::fmax(0.5f, s.numf("scale", 6.f)), len = std::fmax(0.05f, s.numf("length", 1.f));
  float width = saturate(s.numf("width", 0.5f));
  float density = saturate(s.numf("density", 0.5f));
  int per_cell = std::clamp(s.integer("layers", 3), 1, 8);
  uint32_t seed = (uint32_t)s.integer("seed", 0) * 7777u + 99u;
  vec3 bias{0, 0, 0};
  bool has_bias = s.has("direction");
  if (has_bias) bias = parse_dir(s["direction"], vec3(1, 0, 0), c, "direction");
  float half_w = 0.004f + 0.02f * width;  // in cell units
  par(c.n, [&](size_t i) {
    vec3 p = (c.ss->pos[i] - c.center) / c.ext * scale;
    vec3 n = c.ss->nrm[i];
    int cx = (int)std::floor(p.x), cy = (int)std::floor(p.y), cz = (int)std::floor(p.z);
    float v = 0;
    for (int dz = -1; dz <= 1; dz++)
      for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++) {
          uint32_t h0 = hash_u32((uint32_t)(cx + dx) * 0x8da6b343U ^ hash_u32((uint32_t)(cy + dy) * 0xd8163841U ^ hash_u32((uint32_t)(cz + dz) * 0xcb1ab31fU ^ seed)));
          for (int k = 0; k < per_cell; k++) {
            uint32_t h = hash_u32(h0 + (uint32_t)k * 0x9e3779b9U);
            if (hash_float(h) > density) continue;
            vec3 ctr{(float)(cx + dx) + hash_float(h ^ 0x1u), (float)(cy + dy) + hash_float(h ^ 0x2u), (float)(cz + dz) + hash_float(h ^ 0x3u)};
            vec3 v0 = p - ctr;
            float dn = dot(v0, n);
            if (std::fabs(dn) > 0.6f) continue;  // segment too far above/below this surface
            float z = hash_float(h ^ 0x4u) * 2.f - 1.f, phi = hash_float(h ^ 0x5u) * 2.f * kPi;
            float r = std::sqrt(std::fmax(0.f, 1.f - z * z));
            vec3 dir{r * std::cos(phi), r * std::sin(phi), z};
            if (has_bias) dir = normalize(lerp(dir, bias * (dot(dir, bias) >= 0 ? 1.f : -1.f), 0.85f));
            vec3 dt = dir - n * dot(dir, n);
            float dl = length(dt);
            if (dl < 0.2f) continue;  // segment points into the surface: would be a dot
            dt = dt / dl;
            vec3 vt = v0 - n * dn;
            float L = len * (0.3f + 0.7f * hash_float(h ^ 0x6u)) * 0.5f;
            float t = clampf(dot(vt, dt), -L, L);
            float d = length(vt - dt * t);
            float taper = 1.f - smoothstep(0.6f * L, L, std::fabs(dot(vt, dt)));  // thin out towards the ends
            float w = half_w * (0.5f + hash_float(h ^ 0x7u)) * (0.4f + 0.6f * taper);
            float line = (1.f - smoothstep(w * 0.5f, w, d)) * (0.5f + 0.5f * hash_float(h ^ 0x8u));
            v = std::fmax(v, line);
          }
        }
    out[i] = v;
  });
}

static void f_streaks(const Json& s, Ctx& c, float* out) {
  vec3 d = parse_dir(s["direction"], vec3(0, -1, 0), c, "direction");
  float scale = s.numf("scale", 8.f), len = std::fmax(1.f, s.numf("length", 8.f));
  float amount = s.numf("amount", 0.5f), soft = std::fmax(1e-3f, s.numf("softness", 0.15f));
  uint32_t seed = (uint32_t)s.integer("seed", 0) * 131u + 5u;
  vec3 a, b;
  onb(d, a, b);
  float t = 1.f - amount;
  par(c.n, [&](size_t i) {
    vec3 p = (c.ss->pos[i] - c.center) / c.ext * scale;
    vec3 q{dot(p, a), dot(p, d) / len, dot(p, b)};
    float streak = 0.5f + 0.5f * std::tanh(fbm3(q, 4, 2.f, 0.5f, seed) * 3.f);
    float fine = ridged3(q * vec3(3.f, 0.5f, 3.f), 3, 2.f, 0.5f, seed + 1);
    float v = streak * 0.7f + fine * 0.3f;
    float vert = saturate(1.f - std::fabs(dot(c.ss->nrm[i], d)) * 1.1f);
    out[i] = smoothstep(-soft, soft, v * vert - t);
  });
}

static void f_grunge(const Json& s, Ctx& c, float* out) {
  std::string style = to_lower(s.str("style", "smudge"));
  float scale = s.numf("scale", 6.f);
  float amount = saturate(s.numf("amount", 0.5f));
  uint32_t seed = (uint32_t)s.integer("seed", 0) * 2654435761u + 17u;
  auto P = [&](vec3 p) { return (p - c.center) / c.ext * scale; };
  if (style == "smudge") {
    par(c.n, [&](size_t i) {
      vec3 p = P(c.ss->pos[i]);
      p += fbm3_vec(p * 0.5f, 3, seed) * 0.8f;
      float v = 0.5f + 0.5f * std::tanh(fbm3(p, 5, 2.f, 0.55f, seed + 1) * 3.f);
      out[i] = smoothstep(1.f - amount - 0.2f, 1.f - amount + 0.2f, v);
    });
  } else if (style == "spots") {
    par(c.n, [&](size_t i) {
      vec3 p = P(c.ss->pos[i]) * 2.f;
      Worley w = worley3(p, 1.f, seed);
      float r = 0.1f + 0.3f * hash_float(w.id);
      float keep = hash_float(w.id ^ 0x85ebca6bu) < amount ? 1.f : 0.f;
      float edge = fbm3(p * 3.f, 3, 2.f, 0.5f, seed + 2) * 0.08f;
      out[i] = keep * (1.f - smoothstep(r * 0.7f, r, w.f1 + edge));
    });
  } else if (style == "patches") {
    par(c.n, [&](size_t i) {
      vec3 p = P(c.ss->pos[i]);
      p += fbm3_vec(p, 3, seed) * 0.35f;
      Worley w = worley3(p, 1.f, seed + 3);
      out[i] = hash_float(w.id) < amount ? 1.f : 0.f;
    });
  } else if (style == "cracks") {
    par(c.n, [&](size_t i) {
      vec3 p = P(c.ss->pos[i]);
      p += fbm3_vec(p * 2.f, 3, seed) * 0.15f;
      Worley w = worley3(p, 1.f, seed + 4);
      float crack = 1.f - smoothstep(0.f, 0.03f + 0.08f * amount, w.f2 - w.f1);
      float gate = smoothstep(0.45f, 0.55f, 0.5f + 0.5f * std::tanh(fbm3(p * 0.5f, 3, 2.f, 0.5f, seed + 5) * 3.f) + amount - 0.5f);
      out[i] = crack * gate;
    });
  } else if (style == "speckle") {
    par(c.n, [&](size_t i) {
      // small round specks of varying size (not grid cells, which read as pixels/cubes up close)
      vec3 p = P(c.ss->pos[i]) * 20.f;
      p += fbm3_vec(p * 0.7f, 2, seed) * 0.25f;
      Worley w = worley3(p, 1.f, seed + 8);
      float r = 0.18f + 0.22f * hash_float(w.id ^ 0x27d4eb2fu);
      float keep = hash_float(w.id) < amount ? 1.f : 0.f;
      out[i] = keep * (1.f - smoothstep(r * 0.6f, r, w.f1));
    });
  } else if (style == "rust") {
    par(c.n, [&](size_t i) {
      vec3 p = P(c.ss->pos[i]);
      p += fbm3_vec(p, 3, seed) * 0.5f;
      float big = 0.5f + 0.5f * std::tanh(fbm3(p, 5, 2.1f, 0.55f, seed + 6) * 3.f);
      float pits = 1.f - saturate(worley3(p * 4.f, 1.f, seed + 7).f1 / 0.9f);
      float v = big * 0.8f + pits * 0.35f;
      out[i] = smoothstep(1.f - amount - 0.12f, 1.f - amount + 0.12f, v);
    });
  } else {
    c.error("unknown grunge style '%s' (smudge, spots, patches, cracks, speckle, rust)", style.c_str());
  }
}

static void f_light(const Json& s, Ctx& c, float* out) {
  vec3 L = parse_dir(s["direction"], normalize(vec3(0.4f, 1.f, 0.5f)), c, "direction");
  float wrap = saturate(s.numf("wrap", 0.f));
  par(c.n, [&](size_t i) { out[i] = saturate((dot(c.ss->nrm[i], L) + wrap) / (1.f + wrap)); });
}

static void f_checker(const Json& s, Ctx& c, float* out) {
  float sc = s.numf("scale", 8.f);
  par(c.n, [&](size_t i) {
    vec2 uv = c.ss->uv[i];
    int k = (int)std::floor(uv.x * sc) + (int)std::floor(uv.y * sc);
    out[i] = (k & 1) ? 1.f : 0.f;
  });
}

// ---------------------------------------------------------------- images
struct ImageSampler {
  std::shared_ptr<const Image> img;
  std::string proj;
  float tile_x = 1, tile_y = 1, off_x = 0, off_y = 0, rc = 1, rs = 0, scale = 1, sharp = 4;
  vec3 axis{0, 0, 1};
  vec3 pa, pb;  // planar basis
};

static ImageSampler make_sampler(const Json& s, Ctx& c) {
  ImageSampler S;
  std::string path = s.str("path", s.str("image", ""));
  if (path.empty()) c.error("image needs \"path\"");
  S.img = cached_image(c.proj->resolve(path));
  S.proj = to_lower(s.str("projection", "uv"));
  if (const Json* t = s.find("tile")) {
    if (t->is_number()) S.tile_x = S.tile_y = t->as_float();
    else { S.tile_x = (*t)[0].as_float(1); S.tile_y = (*t)[1].as_float(1); }
  }
  if (const Json* o = s.find("offset")) { S.off_x = (*o)[0].as_float(0); S.off_y = (*o)[1].as_float(0); }
  float rot = s.numf("rotation", 0.f) * kPi / 180.f;
  S.rc = std::cos(rot);
  S.rs = std::sin(rot);
  S.scale = s.numf("scale", 2.f);
  S.sharp = s.numf("sharpness", 4.f);
  if (S.proj == "planar") {
    S.axis = parse_dir(s["axis"], vec3(0, 0, 1), c, "axis");
    vec3 up = std::fabs(S.axis.y) > 0.9f ? vec3(0, 0, -1) : vec3(0, 1, 0);
    S.pa = normalize(cross(up, S.axis));
    S.pb = cross(S.axis, S.pa);
  } else if (S.proj != "uv" && S.proj != "triplanar") {
    c.error("projection must be uv, triplanar or planar");
  }
  return S;
}

static inline vec4 sample_uvimg(const ImageSampler& S, float u, float v) {
  float x = u * S.tile_x + S.off_x, y = v * S.tile_y + S.off_y;
  float xr = x * S.rc - y * S.rs, yr = x * S.rs + y * S.rc;
  return S.img->sample_bilinear(xr, yr, true);
}

static vec4 sample_image(const ImageSampler& S, const Ctx& c, size_t i) {
  if (S.proj == "uv") { vec2 uv = c.ss->uv[i]; return sample_uvimg(S, uv.x, uv.y); }
  vec3 p = (c.ss->pos[i] - c.center) / c.ext * S.scale;
  if (S.proj == "planar") return sample_uvimg(S, dot(p, S.pa), -dot(p, S.pb));
  vec3 n = c.ss->nrm[i];
  float wx = std::pow(std::fabs(n.x), S.sharp), wy = std::pow(std::fabs(n.y), S.sharp), wz = std::pow(std::fabs(n.z), S.sharp);
  float ws = wx + wy + wz + 1e-6f;
  vec4 a = sample_uvimg(S, p.z * (n.x >= 0 ? -1.f : 1.f), -p.y), b = sample_uvimg(S, p.x, p.z), d = sample_uvimg(S, p.x * (n.z >= 0 ? 1.f : -1.f), -p.y);
  float fx = wx / ws, fy = wy / ws, fz = wz / ws;
  return {a.x * fx + b.x * fy + d.x * fz, a.y * fx + b.y * fy + d.y * fz, a.z * fx + b.z * fy + d.z * fz, a.w * fx + b.w * fy + d.w * fz};
}

static float pick_channel(vec4 v, const std::string& ch) {
  if (ch == "r") return v.x;
  if (ch == "g") return v.y;
  if (ch == "b") return v.z;
  if (ch == "a") return v.w;
  return 0.2126f * v.x + 0.7152f * v.y + 0.0722f * v.z;
}

static void f_image(const Json& s, Ctx& c, float* out) {
  ImageSampler S = make_sampler(s, c);
  std::string ch = to_lower(s.str("channel", S.img->has_alpha() ? "a" : "luma"));
  par(c.n, [&](size_t i) { out[i] = pick_channel(sample_image(S, c, i), ch); });
}

// ---------------------------------------------------------------- decals (planar projection with occlusion)
struct Decal {
  std::shared_ptr<const Image> img;
  vec3 center, F, U, R;
  float w, h, depth, angle;
  bool occlusion;
  float eps;
};

static Decal make_decal(const Json& s, Ctx& c) {
  Decal D;
  std::string path = s.str("image", s.str("path", ""));
  if (path.empty()) c.error("decal needs \"image\"");
  D.img = cached_image(c.proj->resolve(path));
  std::string space = s.str("space", "bbox");
  const Json& pos = s.has("position") ? s["position"] : s["center"];
  D.center = to_world_point(pos, space, c, vec3(0.5f), "position");
  D.F = parse_dir(s["facing"], vec3(0, 0, 1), c, "facing");
  vec3 up_hint = std::fabs(D.F.y) > 0.9f ? vec3(0, 0, -1) : vec3(0, 1, 0);
  vec3 up = parse_dir(s["up"], up_hint, c, "up");
  D.U = normalize(up - D.F * dot(up, D.F));
  if (length2(D.U) < 1e-6f) D.U = normalize(up_hint - D.F * dot(up_hint, D.F));
  D.R = cross(D.U, D.F);
  float rot = s.numf("rotation", 0.f) * kPi / 180.f;
  vec3 R2 = D.R * std::cos(rot) + D.U * std::sin(rot);
  D.U = D.U * std::cos(rot) - D.R * std::sin(rot);
  D.R = R2;
  float aspect = D.img->h > 0 ? (float)D.img->w / D.img->h : 1.f;
  const Json& size = s["size"];
  if (size.is_array()) { D.w = to_world_len(size[0].as_float(0.3f), space, c); D.h = to_world_len(size[1].as_float(0.3f), space, c); }
  else { D.w = to_world_len(size.as_float(0.3f), space, c); D.h = D.w / aspect; }
  D.depth = s.has("depth") ? to_world_len(s.numf("depth", 0.1f), space, c) : std::fmax(D.w, D.h) * 0.5f;
  D.angle = s.numf("angle", 0.15f);
  D.occlusion = s.boolean("occlusion", true);
  D.eps = c.ext * 1e-4f;
  return D;
}

static inline bool decal_at(const Decal& D, const Ctx& c, size_t i, vec4& col, float& weight) {
  vec3 p = c.ss->pos[i];
  vec3 l = p - D.center;
  float x = dot(l, D.R) / D.w + 0.5f, y = dot(l, D.U) / D.h + 0.5f, z = dot(l, D.F);
  if (x < 0 || x > 1 || y < 0 || y > 1 || std::fabs(z) > D.depth) return false;
  float nd = dot(c.ss->nrm[i], D.F);
  if (nd < D.angle) return false;
  if (D.occlusion) {
    float tmax = D.depth - z;
    if (tmax > D.eps && c.bk->bvh.occluded(p + c.ss->fnrm[i] * D.eps, D.F, 0.f, tmax)) return false;
  }
  col = D.img->sample_bilinear(x, 1.f - y, false);
  weight = smoothstep(D.angle, D.angle + 0.25f, nd);
  return true;
}

static void f_decal(const Json& s, Ctx& c, float* out) {
  Decal D = make_decal(s, c);
  std::string ch = to_lower(s.str("channel", D.img->has_alpha() ? "a" : "luma"));
  par(c.n, [&](size_t i) {
    vec4 col;
    float w;
    out[i] = decal_at(D, c, i, col, w) ? pick_channel(col, ch) * w : 0.f;
  });
}

// ---------------------------------------------------------------- paint strokes (3D, in object space)
static void f_paint(const Json& s, Ctx& c, float* out) {
  std::string space = s.str("space", "bbox");
  struct Seg { vec3 a, b; float r, hard, op; vec3 lo, hi; };
  std::vector<Seg> segs;
  auto add_stroke = [&](const Json& st, size_t k) {
    const Json& pts = st.has("points") ? st["points"] : st["at"].is_null() ? Json() : Json::array({st["at"]});
    if (!pts.is_array() || pts.size() == 0) c.error("paint.strokes[%zu] needs \"points\": [[x,y,z], ...]", k);
    float r = to_world_len(st.numf("radius", s.numf("radius", 0.03f)), space, c);
    float hard = saturate(st.numf("hardness", s.numf("hardness", 0.7f)));
    float op = st.numf("opacity", 1.f);
    std::vector<vec3> P;
    for (auto& pj : pts.items()) P.push_back(to_world_point(pj, space, c, vec3(0.5f), "points"));
    if (P.size() == 1) P.push_back(P[0]);
    for (size_t i = 0; i + 1 < P.size(); i++) {
      Seg sg{P[i], P[i + 1], r, hard, op, vmin(P[i], P[i + 1]) - vec3(r), vmax(P[i], P[i + 1]) + vec3(r)};
      segs.push_back(sg);
    }
  };
  if (const Json* st = s.find("strokes")) for (size_t k = 0; k < st->size(); k++) add_stroke((*st)[k], k);
  if (const Json* dabs = s.find("dabs")) for (size_t k = 0; k < dabs->size(); k++) add_stroke((*dabs)[k], k);
  if (segs.empty()) c.error("paint needs \"strokes\": [{\"points\": [[x,y,z],...], \"radius\": r}] (bbox coords by default)");
  par(c.n, [&](size_t i) {
    vec3 p = c.ss->pos[i];
    float v = 0;
    for (auto& sg : segs) {
      if (p.x < sg.lo.x || p.y < sg.lo.y || p.z < sg.lo.z || p.x > sg.hi.x || p.y > sg.hi.y || p.z > sg.hi.z) continue;
      vec3 ab = sg.b - sg.a;
      float l2 = length2(ab);
      float t = l2 > 0 ? clampf(dot(p - sg.a, ab) / l2, 0.f, 1.f) : 0.f;
      float d = length(p - (sg.a + ab * t));
      float f = 1.f - smoothstep(sg.r * sg.hard, sg.r, d);
      v = std::fmax(v, f * sg.op);
    }
    out[i] = v;
  });
}

// ---------------------------------------------------------------- references
static void f_layer_ref(const Json& s, Ctx& c, float* out) {
  std::string id = s.str("layer", "");
  auto it = c.masks->find(id);
  if (it == c.masks->end() || it->second.size() != c.n)
    c.error("layer mask '%s' is not available here: reference a layer that appears earlier (below) in the same texture set", id.c_str());
  std::copy(it->second.begin(), it->second.end(), out);
}

static void f_stack(const Json& s, Ctx& c, float* out) {
  std::string chn = s.str("channel", "height");
  int ch = channel_index(chn);
  if (ch < 0) c.error("stack: unknown channel '%s'", chn.c_str());
  const Stack& st = *c.stack;
  int k = kChannels[ch].comps;
  if (k == 1) par(c.n, [&](size_t i) { out[i] = st.ch[ch][i]; });
  else if (ch == C_NORMAL) par(c.n, [&](size_t i) { out[i] = st.ch[ch][i * 3 + 2]; });
  else par(c.n, [&](size_t i) { const float* v = &st.ch[ch][i * 3]; out[i] = 0.2126f * v[0] + 0.7152f * v[1] + 0.0722f * v[2]; });
}

static bool eval_mask_stack(const Json& mask, Ctx& c, std::vector<float>& out);

static void f_combine(const Json& s, Ctx& c, float* out) {
  std::vector<float> m;
  if (!eval_mask_stack(s["effects"], c, m)) { std::fill(out, out + c.n, 1.f); return; }
  std::copy(m.begin(), m.end(), out);
}

// ---------------------------------------------------------------- registry
static const std::vector<FieldDef>& field_defs() {
  static const std::vector<FieldDef> defs = {
      {"constant", "basic", "A constant value.", {{"value", "1", "value"}}, f_constant},
      {"noise", "procedural", "3D procedural noise sampled at the surface position (seamless across UV seams), or in UV space per island.",
       {{"noise", "\"fbm\"", "fbm | perlin | value | ridged | turbulence | cells | voronoi | cracks | dots | white | rings (wood growth rings around `axis`)"},
        {"scale", "4", "features per object size (object and uv space) or per unit (world space)"},
        {"octaves", "5", "fbm/ridged/turbulence/value detail levels"}, {"lacunarity", "2", "frequency multiplier per octave"},
        {"gain", "0.5", "amplitude multiplier per octave"}, {"seed", "0", "integer seed"},
        {"stretch", "[1,1,1]", "feature size multiplier per axis, e.g. [1,6,1] = streaks along Y; or \"up\" (12x along an axis)"},
        {"axis", "\"up\"", "rings: the grain axis"},
        {"warp", "0", "domain warp strength (0..2)"}, {"offset", "[0,0,0]", "domain offset"},
        {"jitter", "1", "cells/voronoi/cracks/dots randomness"}, {"width", "0.08", "cracks line width"},
        {"size", "0.35", "dots radius (in cells)"},
        {"space", "\"object\"", "object (normalized size) | world (mesh units) | uv (x=u, y=v, each UV island offset: wood grain along V)"}},
       f_noise},
      {"curvature", "mesh", "Baked curvature. convex = outer edges, concave = creases.", {{"mode", "\"convex\"", "convex | concave | both | raw (0.5 = flat)"}}, f_curvature},
      {"ao", "mesh", "Baked ambient occlusion (1 = open, 0 = occluded).", {}, f_ao},
      {"cavity", "mesh", "1 - ambient occlusion (crevices are 1).", {}, f_cavity},
      {"thickness", "mesh", "Baked thickness (1 = thick, 0 = thin parts).", {}, f_thickness},
      {"bake", "mesh", "Raw baked/geometric map.",
       {{"map", "\"ao\"", "ao | curvature | convexity | concavity | thickness | height (high poly, 0.5 = low surface) | position_x|y|z (bbox 0..1) | normal_x|y|z | island | uv_u | uv_v"},
        {"seed", "0", "for island"}},
       f_bake},
      {"edge_wear", "generator", "Worn convex edges (like Substance's Metal Edge Wear): curvature + noise breakup.",
       {{"amount", "0.5", "0..1 how much wear"}, {"width", "0.5", "0..1 how far wear reaches from edges"},
        {"breakup", "0.6", "noise breakup strength"}, {"softness", "0.06", "edge softness"}, {"scale", "10", "breakup noise scale"},
        {"noise", "\"fbm\"", "breakup noise kind"}, {"seed", "0", "seed"}},
       f_edge_wear},
      {"dirt", "generator", "Dirt/grime gathering in occluded crevices and concave creases.",
       {{"amount", "0.5", "0..1"}, {"ao_weight", "1", "weight of occlusion"}, {"concavity_weight", "1", "weight of concave curvature"},
        {"breakup", "0.5", "noise breakup"}, {"softness", "0.1", "softness"}, {"scale", "6", "noise scale"}, {"seed", "0", "seed"}},
       f_dirt},
      {"gradient", "generator", "Linear gradient along an axis (0 at `from`, 1 at `to`). Named axes use bbox 0..1 coordinates.",
       {{"axis", "\"up\"", "up|down|left|right|front|back or [x,y,z]"}, {"from", "0", "start"}, {"to", "1", "end"},
        {"breakup", "0", "noise breakup"}, {"scale", "6", "noise scale"}, {"seed", "0", "seed"}},
       f_gradient},
      {"direction", "generator", "Surfaces facing a direction (dust/snow/moss on top: direction up).",
       {{"direction", "\"up\"", "direction"}, {"min", "0.3", "facing dot product where the mask starts"},
        {"max", "0.8", "facing dot product where the mask is full"}, {"breakup", "0", "noise breakup"}, {"scale", "6", "noise scale"}, {"seed", "0", "seed"}},
       f_direction},
      {"sphere", "region", "Spherical 3D region.",
       {{"center", "[0.5,0.5,0.5]", "center (bbox coords by default)"}, {"radius", "0.25", "radius (fraction of largest dimension)"},
        {"falloff", "0.3", "soft edge as fraction of radius"}, {"space", "\"bbox\"", "bbox | world"}},
       f_sphere},
      {"box", "region", "Axis-aligned 3D box region.",
       {{"min", "[0,0,0]", "min corner (bbox coords)"}, {"max", "[1,1,1]", "max corner"}, {"falloff", "0.02", "soft edge width"}, {"space", "\"bbox\"", "bbox | world"}},
       f_box},
      {"plane", "region", "Half-space in front of a plane.",
       {{"point", "[0.5,0.5,0.5]", "point on plane"}, {"normal", "\"up\"", "side that is 1"}, {"falloff", "0.02", "soft edge width"}, {"space", "\"bbox\"", "bbox | world"}},
       f_plane},
      {"select", "region", "Hard selection by object/part name (glob) and/or UV island id.",
       {{"parts", "[]", "part names or globs, e.g. [\"Handle*\"] (see inspect)"}, {"islands", "[]", "UV island ids"}}, f_select},
      {"island_random", "procedural", "A random value per UV island (variation between planks/tiles).", {{"seed", "0", "seed"}}, f_island_random},
      {"part_random", "procedural", "A random value per mesh part.", {{"seed", "0", "seed"}}, f_part_random},
      {"scratches", "generator", "Straight, thin scratches of random length and orientation (optionally biased along a direction).",
       {{"scale", "6", "cells per object size (more, smaller scratches)"}, {"length", "1", "scratch length in cells"},
        {"width", "0.5", "0..1 line width"}, {"density", "0.5", "0..1 chance of a scratch per slot"},
        {"layers", "3", "scratch slots per cell"}, {"direction", "", "optional bias, e.g. \"right\" for directional sanding marks"}, {"seed", "0", "seed"}},
       f_scratches},
      {"streaks", "generator", "Vertical streaks/drips on non-horizontal faces (rust runs, water stains).",
       {{"direction", "\"down\"", "flow direction"}, {"scale", "8", "streak frequency"}, {"length", "8", "elongation"},
        {"amount", "0.5", "0..1"}, {"softness", "0.15", "softness"}, {"seed", "0", "seed"}},
       f_streaks},
      {"grunge", "generator", "Ready-made grunge patterns.",
       {{"style", "\"smudge\"", "smudge | spots | patches | cracks | speckle | rust"}, {"amount", "0.5", "0..1 coverage"},
        {"scale", "6", "pattern scale"}, {"seed", "0", "seed"}},
       f_grunge},
      {"light", "generator", "Directional light gradient (highlights/sun bleaching).",
       {{"direction", "[0.4,1,0.5]", "light direction"}, {"wrap", "0", "0..1 wrap around"}}, f_light},
      {"checker", "procedural", "UV-space checker (debugging UVs).", {{"scale", "8", "squares per UV unit"}}, f_checker},
      {"image", "image", "Grayscale from an image file.",
       {{"path", "", "image file (relative to project)"}, {"projection", "\"uv\"", "uv | triplanar | planar"},
        {"channel", "\"luma\"", "r|g|b|a|luma (default a if the image has alpha)"}, {"tile", "1", "uv tiling (number or [x,y])"},
        {"offset", "[0,0]", "uv offset"}, {"rotation", "0", "degrees"}, {"scale", "2", "triplanar/planar tiling per object size"},
        {"sharpness", "4", "triplanar blend sharpness"}, {"axis", "\"front\"", "planar projection axis"}},
       f_image},
      {"decal", "image", "Project an image onto the surface from a direction (logos, labels, stencils); occlusion-aware.",
       {{"image", "", "image file (alpha = mask)"}, {"position", "[0.5,0.5,1]", "decal center (bbox coords)"},
        {"facing", "\"front\"", "which surfaces receive it / projection comes from this side"}, {"up", "\"up\"", "image up direction"},
        {"size", "0.3", "width (fraction of largest dimension) or [w,h]"}, {"rotation", "0", "degrees"},
        {"depth", "size/2", "how far along the projection the decal reaches"}, {"angle", "0.15", "min facing dot product"},
        {"occlusion", "true", "blocked by geometry closer to the projector"}, {"channel", "\"a\"", "r|g|b|a|luma"},
        {"space", "\"bbox\"", "bbox | world"}},
       f_decal},
      {"paint", "paint", "3D brush strokes (polylines) in object space - how agents 'paint'.",
       {{"strokes", "[]", "[{\"points\": [[x,y,z],...], \"radius\": 0.03, \"hardness\": 0.7, \"opacity\": 1}]"},
        {"dabs", "[]", "[{\"at\": [x,y,z], \"radius\": r}] single brush dabs"}, {"radius", "0.03", "default radius (fraction of largest dimension)"},
        {"hardness", "0.7", "default hardness"}, {"space", "\"bbox\"", "bbox | world"}},
       f_paint},
      {"layer", "reference", "The final mask of another layer lower in the stack (anchor-style reuse).", {{"layer", "", "layer id"}}, f_layer_ref},
      {"stack", "reference", "The accumulated channel value of the layers below (e.g. height for wear in raised areas).",
       {{"channel", "\"height\"", "channel name (colors give luminance)"}}, f_stack},
      {"combine", "basic", "A nested mask stack used as a single field.", {{"effects", "[]", "list of fields with blend/opacity"}}, f_combine},
  };
  return defs;
}

std::vector<std::string> field_type_names() {
  std::vector<std::string> v;
  for (auto& d : field_defs()) v.push_back(d.type);
  return v;
}

// ---------------------------------------------------------------- modifiers
static void blur_samples(Ctx& c, float* v, float radius) {
  const SampleSet& ss = *c.ss;
  int res = ss.res;
  int r = std::max(1, (int)std::lround(radius * res / 1024.f));
  std::vector<float> img((size_t)res * res), tmp((size_t)res * res);
  to_image(ss, v, 1, img.data());
  for (int pass = 0; pass < 2; pass++) {
    parallel_for(res, 8, [&](int64_t y0, int64_t y1) {
      for (int64_t y = y0; y < y1; y++) {
        const float* row = &img[y * res];
        float* dst = &tmp[y * res];
        double acc = 0;
        for (int x = -r; x <= r; x++) acc += row[std::clamp(x, 0, res - 1)];
        for (int x = 0; x < res; x++) {
          dst[x] = (float)(acc / (2 * r + 1));
          acc += row[std::min(x + r + 1, res - 1)] - row[std::max(x - r, 0)];
        }
      }
    });
    parallel_for(res, 8, [&](int64_t x0, int64_t x1) {
      for (int64_t x = x0; x < x1; x++) {
        double acc = 0;
        for (int y = -r; y <= r; y++) acc += tmp[(size_t)std::clamp(y, 0, res - 1) * res + x];
        for (int y = 0; y < res; y++) {
          img[(size_t)y * res + x] = (float)(acc / (2 * r + 1));
          acc += tmp[(size_t)std::min(y + r + 1, res - 1) * res + x] - tmp[(size_t)std::max(y - r, 0) * res + x];
        }
      }
    });
  }
  from_image(ss, img.data(), 1, v);
}

static void apply_modifiers(const Json& s, Ctx& c, float* v) {
  if (!s.is_object()) return;
  size_t n = c.n;
  if (const Json* b = s.find("blur")) { if (b->as_float() > 0) blur_samples(c, v, b->as_float()); }
  if (const Json* lv = s.find("levels")) {
    float ilo = 0, ihi = 1, g = 1, olo = 0, ohi = 1;
    if (lv->is_array()) { ilo = (*lv)[0].as_float(0); ihi = (*lv)[1].as_float(1); }
    else if (lv->is_object()) {
      const Json& in = (*lv)["in"];
      const Json& o = (*lv)["out"];
      if (in.is_array()) { ilo = in[0].as_float(0); ihi = in[1].as_float(1); }
      if (o.is_array()) { olo = o[0].as_float(0); ohi = o[1].as_float(1); }
      g = lv->numf("gamma", 1.f);
    } else c.error("levels must be [in_low, in_high] or {\"in\":[lo,hi],\"gamma\":g,\"out\":[lo,hi]}");
    float span = std::fabs(ihi - ilo) < 1e-6f ? 1e-6f : ihi - ilo;
    float ig = 1.f / std::fmax(1e-3f, g);
    par(n, [&](size_t i) { float t = saturate((v[i] - ilo) / span); if (g != 1.f) t = std::pow(t, ig); v[i] = olo + t * (ohi - olo); });
  }
  if (const Json* ct = s.find("contrast")) {
    float k = std::tan((clampf(ct->as_float(), -0.99f, 0.99f) + 1.f) * kPi / 4.f);
    par(n, [&](size_t i) { v[i] = saturate((v[i] - 0.5f) * k + 0.5f); });
  }
  if (const Json* pw = s.find("power")) { float e = pw->as_float(1); par(n, [&](size_t i) { v[i] = std::pow(std::fmax(0.f, v[i]), e); }); }
  if (s.boolean("invert", false)) par(n, [&](size_t i) { v[i] = 1.f - v[i]; });
  if (const Json* th = s.find("threshold")) {
    float t = th->is_object() ? th->numf("value", 0.5f) : th->as_float(0.5f);
    float sf = th->is_object() ? th->numf("softness", 0.02f) : 0.02f;
    par(n, [&](size_t i) { v[i] = smoothstep(t - sf, t + sf, v[i]); });
  }
  if (const Json* m = s.find("multiply")) { float k = m->as_float(1); par(n, [&](size_t i) { v[i] *= k; }); }
  if (const Json* a = s.find("add")) { float k = a->as_float(0); par(n, [&](size_t i) { v[i] += k; }); }
  if (s.boolean("clamp", true)) par(n, [&](size_t i) { v[i] = saturate(v[i]); });
}

static void eval_field(const Json& s, Ctx& c, float* out) {
  if (s.is_number()) { std::fill(out, out + c.n, s.as_float()); return; }
  if (!s.is_object()) c.error("expected a field object (e.g. {\"type\":\"noise\"}) or a number, got %s", s.type_name());
  std::string type = s.str("type", "");
  if (type.empty()) {
    if (s.has("value")) { std::fill(out, out + c.n, s.numf("value", 1.f)); apply_modifiers(s, c, out); return; }
    c.error("field object is missing \"type\" (one of: %s)", [] {
      std::string all;
      for (auto& d : field_defs()) { if (!all.empty()) all += ", "; all += d.type; }
      return all;
    }().c_str());
  }
  const FieldDef* def = nullptr;
  for (auto& d : field_defs()) if (type == d.type) { def = &d; break; }
  if (!def) {
    std::string dym = did_you_mean(type, field_type_names());
    c.error("unknown field type '%s'%s", type.c_str(), dym.empty() ? " (run `patina library` for the list)" : (" (did you mean '" + dym + "'?)").c_str());
  }
  check_keys(s, def->params, c, (std::string("'") + type + "'").c_str());
  def->fn(s, c, out);
  apply_modifiers(s, c, out);
}

// Mask stack: effects evaluated bottom-to-top, each blended onto the running mask.
static bool eval_mask_stack(const Json& mask, Ctx& c, std::vector<float>& out) {
  if (mask.is_null()) return false;
  if (mask.is_number()) { out.assign(c.n, saturate(mask.as_float())); return true; }
  Json single;
  const Json* fx = &mask;
  if (mask.is_object()) { single = Json::array(); single.push(mask); fx = &single; }
  if (!fx->is_array()) c.error("mask must be a list of effects, a single effect object, or a number");
  if (fx->size() == 0) return false;
  out.assign(c.n, 0.f);
  std::vector<float> f(c.n);
  bool first = true;
  for (size_t k = 0; k < fx->size(); k++) {
    const Json& e = (*fx)[k];
    if (e.is_object() && !e.boolean("enabled", true)) continue;
    PathScope ps(c, strf(".mask[%zu]", k));
    eval_field(e, c, f.data());
    if (const Json* r = e.is_object() ? e.find("range") : nullptr) {  // same remap as for channels, then 0..1
      if (!r->is_array() || r->size() != 2) c.error("range must be [low, high]");
      float lo = (*r)[0].as_float(0), hi = (*r)[1].as_float(1);
      float* fv = f.data();
      par(c.n, [&](size_t i) { fv[i] = saturate(lo + fv[i] * (hi - lo)); });
    }
    std::string mode = e.is_object() ? e.str("blend", "normal") : "normal";
    float op = e.is_object() ? e.numf("opacity", 1.f) : 1.f;
    BlendMode bm = blend_or_fail(mode, c);
    if (!first && (bm == B_NORMAL || bm == B_REPLACE) && op >= 1.f)
      c.warn("this effect uses blend \"normal\" at full opacity, so it hides the effects below it; use \"multiply\" (intersect), \"max\"/\"add\" (union) or \"subtract\"");
    float* o = out.data();
    const float* fv = f.data();
    par(c.n, [&](size_t i) { o[i] = saturate(lerp(o[i], blend_scalar(bm, o[i], fv[i]), op)); });
    first = false;
  }
  return true;
}

// ---------------------------------------------------------------- channel content
static void eval_color_content(const Json& v, Ctx& c, float* out) {
  size_t n = c.n;
  vec3 srgb;
  if (v.is_string() || v.is_array() || v.is_number()) {
    if (!parse_color_srgb(v, srgb)) c.error("bad color %s (use \"#rrggbb\" or [r,g,b] in 0..1)", v.dump().c_str());
    vec3 lin = srgb_vec_to_linear(srgb);
    par(n, [&](size_t i) { out[i * 3] = lin.x; out[i * 3 + 1] = lin.y; out[i * 3 + 2] = lin.z; });
    return;
  }
  if (!v.is_object()) c.error("bad color source");
  float k = v.numf("intensity", 1.f);
  const Json* grad = v.find("gradient");
  if (!grad) grad = v.find("colors");
  if (v.has("color") && !grad) {
    if (!parse_color_srgb(v["color"], srgb)) c.error("bad color %s", v["color"].dump().c_str());
    vec3 lin = srgb_vec_to_linear(srgb) * k;
    par(n, [&](size_t i) { out[i * 3] = lin.x; out[i * 3 + 1] = lin.y; out[i * 3 + 2] = lin.z; });
    return;
  }
  std::string type = v.str("type", "");
  if (!grad && type == "image") {
    ImageSampler S = make_sampler(v, c);
    bool srgb_img = v.boolean("srgb", true);
    par(n, [&](size_t i) {
      vec4 col = sample_image(S, c, i);
      vec3 rgb{col.x, col.y, col.z};
      if (srgb_img) rgb = srgb_vec_to_linear(rgb);
      out[i * 3] = rgb.x * k; out[i * 3 + 1] = rgb.y * k; out[i * 3 + 2] = rgb.z * k;
    });
    return;
  }
  if (!grad && type == "decal") {
    Decal D = make_decal(v, c);
    par(n, [&](size_t i) {
      vec4 col;
      float w;
      vec3 rgb = decal_at(D, c, i, col, w) ? srgb_vec_to_linear({col.x, col.y, col.z}) : vec3(0.f);
      out[i * 3] = rgb.x * k; out[i * 3 + 1] = rgb.y * k; out[i * 3 + 2] = rgb.z * k;
    });
    return;
  }
  // gradient map of a scalar field (inline or under "field")
  std::vector<float> f(n);
  const Json& fs = v.has("field") ? v["field"] : v;
  if (!fs.has("type") && !fs.is_number()) c.error("color source needs \"color\", or a field (\"type\") with a \"gradient\"");
  Json stripped = fs;
  if (&fs == &v) { stripped.erase("gradient"); stripped.erase("colors"); stripped.erase("intensity"); }
  eval_field(stripped, c, f.data());
  Gradient g;
  if (grad) g = parse_gradient(*grad, c);
  par(n, [&](size_t i) {
    vec3 s = grad ? g.at(f[i]) : vec3(saturate(f[i]));
    vec3 lin = srgb_vec_to_linear(s) * k;
    out[i * 3] = lin.x; out[i * 3 + 1] = lin.y; out[i * 3 + 2] = lin.z;
  });
}

static void eval_scalar_content(const Json& v, Ctx& c, float* out) {
  if (v.is_number()) { std::fill(out, out + c.n, v.as_float()); return; }
  if (!v.is_object()) c.error("expected a number or a field object, got %s", v.type_name());
  if (!v.has("type")) {
    if (v.has("value")) { std::fill(out, out + c.n, v.numf("value", 0.f)); return; }
    c.error("expected a number or a field object with \"type\"");
  }
  eval_field(v, c, out);
  if (const Json* r = v.find("range")) {
    if (!r->is_array() || r->size() != 2) c.error("range must be [low, high]");
    float lo = (*r)[0].as_float(0), hi = (*r)[1].as_float(1);
    par(c.n, [&](size_t i) { out[i] = lo + out[i] * (hi - lo); });
  }
}

static void eval_normal_content(const Json& v, Ctx& c, float* out) {
  if (!v.is_object() || v.str("type", "") != "image")
    c.error("the normal channel takes a normal-map image: {\"type\":\"image\",\"path\":\"n.png\",\"format\":\"opengl\"}; use \"height\" for procedural bumps");
  ImageSampler S = make_sampler(v, c);
  bool dx = to_lower(v.str("format", "opengl")) == "directx";
  float strength = v.numf("strength", 1.f);
  par(c.n, [&](size_t i) {
    vec4 col = sample_image(S, c, i);
    vec3 nn{col.x * 2.f - 1.f, col.y * 2.f - 1.f, col.z * 2.f - 1.f};
    if (dx) nn.y = -nn.y;
    nn.x *= strength;
    nn.y *= strength;
    nn = normalize(nn);
    out[i * 3] = nn.x; out[i * 3 + 1] = nn.y; out[i * 3 + 2] = nn.z;
  });
}

static void blend_into(Stack& st, int ch, const float* src, const float* mask, float opacity, BlendMode mode) {
  int k = kChannels[ch].comps;
  float* dst = st.ch[ch].data();
  if (ch == C_NORMAL) {
    par(st.n, [&](size_t i) {
      float w = (mask ? mask[i] : 1.f) * opacity;
      if (w <= 0) return;
      vec3 a{dst[i * 3], dst[i * 3 + 1], dst[i * 3 + 2]}, b{src[i * 3], src[i * 3 + 1], src[i * 3 + 2]};
      vec3 r = mode == B_COMBINE ? rnm(a, b) : b;
      vec3 o = normalize(lerp(a, r, w));
      dst[i * 3] = o.x; dst[i * 3 + 1] = o.y; dst[i * 3 + 2] = o.z;
    });
    return;
  }
  par(st.n, [&](size_t i) {
    float w = (mask ? mask[i] : 1.f) * opacity;
    if (w <= 0) return;
    for (int j = 0; j < k; j++) {
      float a = dst[i * k + j];
      dst[i * k + j] = lerp(a, blend_scalar(mode, a, src[i * k + j]), w);
    }
  });
}

// ---------------------------------------------------------------- layers
static const char* kLayerKeys[] = {"id", "name", "type", "enabled", "opacity", "blend", "blend_modes", "channel_opacity", "channels", "mask",
                                    "layers", "material", "params", "comment", "note", "image", "position", "center", "facing", "up",
                                    "size", "rotation", "depth", "angle", "occlusion", "space", "channel"};

static void eval_layer_list(const Json& layers, Ctx& c, Stack& st);

// Collect ids referenced by {"type":"layer"} fields so only those masks are kept in memory.
static void scan_layer_refs(const Json& j, std::unordered_set<std::string>& out) {
  if (j.is_array()) { for (auto& v : j.items()) scan_layer_refs(v, out); return; }
  if (!j.is_object()) return;
  if (j.str("type", "") == "layer" && j["layer"].is_string()) out.insert(j["layer"].as_str());
  for (auto& kv : j.members()) scan_layer_refs(kv.second, out);
}

static void record_mask(Ctx& c, const std::string& id, const std::vector<float>* m, float opacity) {
  if (id.empty()) return;
  bool want = c.wanted.count("*") || c.wanted.count(id);
  if (!want) return;
  std::vector<float> v(c.n);
  if (m) par(c.n, [&](size_t i) { v[i] = (*m)[i] * opacity; });
  else std::fill(v.begin(), v.end(), opacity);
  (*c.masks)[id] = std::move(v);
}

static void eval_fill_channels(const Json& L, Ctx& c, Stack& st, const std::vector<float>* mask, float opacity) {
  const Json& chans = L["channels"];
  if (chans.is_null()) return;
  if (!chans.is_object()) c.error("\"channels\" must be an object like {\"basecolor\": \"#aa5533\", \"roughness\": 0.4}");
  for (auto& kv : chans.members()) {
    if (kv.second.is_null()) continue;
    int ch = channel_index(kv.first);
    if (ch < 0) {
      std::string dym = did_you_mean(kv.first, channel_names());
      c.error("unknown channel '%s'%s; channels: basecolor metallic roughness normal height ao emissive opacity", kv.first.c_str(),
              dym.empty() ? "" : (" (did you mean '" + dym + "'?)").c_str());
    }
    PathScope ps(c, ".channels." + kv.first);
    std::vector<float> buf(c.n * kChannels[ch].comps);
    if (ch == C_NORMAL) eval_normal_content(kv.second, c, buf.data());
    else if (kChannels[ch].comps == 3) eval_color_content(kv.second, c, buf.data());
    else eval_scalar_content(kv.second, c, buf.data());
    std::string mode_s = ch == C_NORMAL ? "combine" : "normal";
    if (const Json* bm = L.find("blend_modes"); bm && (*bm)[kv.first].is_string()) mode_s = (*bm)[kv.first].as_str();
    else if (L["blend"].is_string() && ch != C_NORMAL) mode_s = L["blend"].as_str();
    BlendMode mode = blend_or_fail(mode_s, c);
    float chop = 1.f;
    if (const Json* co = L.find("channel_opacity")) chop = (*co).num(kv.first, 1.0);
    blend_into(st, ch, buf.data(), mask ? mask->data() : nullptr, opacity * chop, mode);
    st.used[ch] = true;
  }
}

static void eval_folder(const Json& children, Ctx& c, Stack& st, const std::vector<float>* mask, float opacity) {
  if (!mask && opacity >= 1.f) {
    eval_layer_list(children, c, st);
    return;
  }
  Stack inner = st;
  Stack* saved = c.stack;
  c.stack = &inner;
  eval_layer_list(children, c, inner);
  c.stack = saved;
  for (int ch = 0; ch < C_COUNT; ch++) {
    if (!inner.used[ch]) continue;
    int k = kChannels[ch].comps;
    float* d = st.ch[ch].data();
    const float* s = inner.ch[ch].data();
    par(st.n, [&](size_t i) {
      float w = (mask ? (*mask)[i] : 1.f) * opacity;
      for (int j = 0; j < k; j++) d[i * k + j] = lerp(d[i * k + j], s[i * k + j], w);
      if (ch == C_NORMAL) {
        vec3 v = normalize({d[i * 3], d[i * 3 + 1], d[i * 3 + 2]});
        d[i * 3] = v.x; d[i * 3 + 1] = v.y; d[i * 3 + 2] = v.z;
      }
    });
    st.used[ch] = true;
  }
}

static void eval_layer(const Json& L, size_t index, Ctx& c, Stack& st) {
  if (!L.is_object()) c.error("layer %zu must be an object", index);
  std::string id = L.str("id", "");
  PathScope ps(c, strf(".layers[%zu]%s", index, id.empty() ? "" : ("(" + id + ")").c_str()));
  if (!L.boolean("enabled", true)) return;
  Timer tm;
  for (auto& kv : L.members()) {
    bool ok = false;
    for (auto* k : kLayerKeys) if (kv.first == k) { ok = true; break; }
    if (!ok) {
      std::vector<std::string> cands(std::begin(kLayerKeys), std::end(kLayerKeys));
      std::string dym = did_you_mean(kv.first, cands);
      c.warn(strf("unknown layer key '%s'%s (ignored)", kv.first.c_str(), dym.empty() ? "" : (", did you mean '" + dym + "'?").c_str()));
    }
  }
  std::string type = L.str("type", L.has("layers") ? "folder" : (L.has("material") ? "smart" : "fill"));
  float opacity = saturate(L.numf("opacity", 1.f));
  std::vector<float> mask;
  bool has_mask = eval_mask_stack(L["mask"], c, mask);

  if (type == "fill" || type == "paint") {
    eval_fill_channels(L, c, st, has_mask ? &mask : nullptr, opacity);
  } else if (type == "folder" || type == "group") {
    const Json& ch = L["layers"];
    if (!ch.is_array()) c.error("folder needs \"layers\": [...]");
    eval_folder(ch, c, st, has_mask ? &mask : nullptr, opacity);
  } else if (type == "smart" || type == "smart_material") {
    std::vector<std::string> w;
    Json folder = expand_smart_material(L, id.empty() ? strf("layer%zu", index) : id, c.proj->dir, w);
    for (auto& s : w) c.warn(s);
    scan_layer_refs(folder, c.wanted);
    eval_folder(folder["layers"], c, st, has_mask ? &mask : nullptr, opacity);
  } else if (type == "decal") {
    Decal D = make_decal(L, c);
    std::vector<float> dm(c.n), rgb(c.n * 3);
    std::string chs = to_lower(L.str("channel", D.img->has_alpha() ? "a" : "luma"));
    par(c.n, [&](size_t i) {
      vec4 col;
      float w;
      if (decal_at(D, c, i, col, w)) {
        dm[i] = pick_channel(col, chs) * w * (has_mask ? mask[i] : 1.f);
        vec3 lin = srgb_vec_to_linear({col.x, col.y, col.z});
        rgb[i * 3] = lin.x; rgb[i * 3 + 1] = lin.y; rgb[i * 3 + 2] = lin.z;
      } else {
        dm[i] = 0;
      }
    });
    // the image colors go into basecolor unless the layer sets basecolor itself (stencil mode)
    if (!L["channels"].has("basecolor")) {
      blend_into(st, C_BASECOLOR, rgb.data(), dm.data(), opacity, blend_or_fail(L.str("blend", "normal"), c));
      st.used[C_BASECOLOR] = true;
    }
    eval_fill_channels(L, c, st, &dm, opacity);
    mask.swap(dm);
    has_mask = true;
  } else {
    c.error("unknown layer type '%s' (fill, folder, smart, decal)", type.c_str());
  }
  record_mask(c, id, has_mask ? &mask : nullptr, opacity);
  if (c.layer_stats && !id.empty()) c.layer_stats->set(id, tm.ms());
}

static void eval_layer_list(const Json& layers, Ctx& c, Stack& st) {
  if (layers.is_null()) return;
  if (!layers.is_array()) c.error("\"layers\" must be an array (bottom layer first)");
  for (size_t i = 0; i < layers.size(); i++) eval_layer(layers[i], i, c, st);
}

// ---------------------------------------------------------------- entry
std::vector<float> evaluate_mask(const Json& mask, const Baked& bk, int set, const std::string& base_dir,
                                 std::vector<std::string>* warnings) {
  Project p;
  p.dir = base_dir;
  p.doc = Json::object();
  const SampleSet& ss = bk.sets[set];
  Ctx c;
  c.proj = &p;
  c.bk = &bk;
  c.mesh = bk.mesh.get();
  c.ss = &ss;
  c.n = ss.size();
  c.center = c.mesh->center();
  c.bmin = c.mesh->bmin;
  c.bsize = c.mesh->size();
  c.ext = c.mesh->max_extent();
  std::unordered_map<std::string, std::vector<float>> masks;
  std::vector<std::string> local_warnings;
  c.masks = &masks;
  c.warnings = warnings ? warnings : &local_warnings;
  EvalOptions opt;
  c.opt = &opt;
  Stack st;
  st.init(c.n);
  c.stack = &st;
  c.path = "bake.normal";
  std::vector<float> out;
  if (!eval_mask_stack(mask, c, out)) out.assign(c.n, 1.f);
  return out;
}

SetResult evaluate_set(const Project& proj, const Baked& bk, int set, const EvalOptions& opt) {
  SetResult r;
  const SampleSet& ss = bk.sets[set];
  r.set = set;
  r.name = ss.name;
  r.res = ss.res;
  Timer total;
  Ctx c;
  c.proj = &proj;
  c.bk = &bk;
  c.mesh = bk.mesh.get();
  c.ss = &ss;
  c.n = ss.size();
  c.center = c.mesh->center();
  c.bmin = c.mesh->bmin;
  c.bsize = c.mesh->size();
  c.ext = c.mesh->max_extent();
  c.masks = &r.masks;
  c.warnings = &r.warnings;
  c.opt = &opt;
  Json lstats = Json::object();
  c.layer_stats = &lstats;
  c.path = "texture_sets." + ss.name;

  c.wanted = opt.record_masks;
  scan_layer_refs(proj.layers(ss.name), c.wanted);
  r.stack.init(c.n);
  c.stack = &r.stack;
  eval_layer_list(proj.layers(ss.name), c, r.stack);

  // height -> normal (uses true texel world size, so height_depth is physical)
  Stack& st = r.stack;
  std::vector<float> nfinal(c.n * 3);
  float H = proj.height_depth() * c.ext;
  if (st.used[C_HEIGHT]) {
    int res = ss.res;
    std::vector<float> himg((size_t)res * res);
    to_image(ss, st.ch[C_HEIGHT].data(), 1, himg.data());
    par(c.n, [&](size_t i) {
      int t = ss.texel[i], x = t % res, y = t / res;
      auto h = [&](int xx, int yy) { return himg[(size_t)std::clamp(yy, 0, res - 1) * res + std::clamp(xx, 0, res - 1)]; };
      float dhdt = (h(x + 1, y) - h(x - 1, y)) / (2.f * ss.len_u[i]);
      float dhdb = (h(x, y - 1) - h(x, y + 1)) / (2.f * ss.len_v[i]);
      vec3 nh = normalize({-H * dhdt, -H * dhdb, 1.f});
      nfinal[i * 3] = nh.x; nfinal[i * 3 + 1] = nh.y; nfinal[i * 3 + 2] = nh.z;
    });
  } else {
    par(c.n, [&](size_t i) { nfinal[i * 3] = 0; nfinal[i * 3 + 1] = 0; nfinal[i * 3 + 2] = 1; });
  }
  bool baked = !ss.nmap.empty();
  if (baked) {  // baked normal (high poly / bevel) is the base; layer height detail sits on top of it
    par(c.n, [&](size_t i) {
      vec3 o = rnm(ss.nmap[i], vec3(nfinal[i * 3], nfinal[i * 3 + 1], nfinal[i * 3 + 2]));
      nfinal[i * 3] = o.x; nfinal[i * 3 + 1] = o.y; nfinal[i * 3 + 2] = o.z;
    });
  }
  if (st.used[C_NORMAL]) {
    par(c.n, [&](size_t i) {
      vec3 a{nfinal[i * 3], nfinal[i * 3 + 1], nfinal[i * 3 + 2]}, b{st.ch[C_NORMAL][i * 3], st.ch[C_NORMAL][i * 3 + 1], st.ch[C_NORMAL][i * 3 + 2]};
      vec3 o = rnm(a, b);
      nfinal[i * 3] = o.x; nfinal[i * 3 + 1] = o.y; nfinal[i * 3 + 2] = o.z;
    });
  }
  st.ch[C_NORMAL].swap(nfinal);
  st.used[C_NORMAL] = st.used[C_NORMAL] || st.used[C_HEIGHT] || baked;
  // final AO = baked AO x painted AO; clamp ranges
  par(c.n, [&](size_t i) {
    st.ch[C_AO][i] = saturate(ss.ao[i] * st.ch[C_AO][i]);
    for (int j = 0; j < 3; j++) {
      st.ch[C_BASECOLOR][i * 3 + j] = saturate(st.ch[C_BASECOLOR][i * 3 + j]);
      st.ch[C_EMISSIVE][i * 3 + j] = std::fmax(0.f, st.ch[C_EMISSIVE][i * 3 + j]);
    }
    st.ch[C_METALLIC][i] = saturate(st.ch[C_METALLIC][i]);
    st.ch[C_ROUGHNESS][i] = saturate(st.ch[C_ROUGHNESS][i]);
    st.ch[C_OPACITY][i] = saturate(st.ch[C_OPACITY][i]);
  });
  st.used[C_AO] = true;
  Json stats = Json::object();
  stats.set("samples", (int64_t)c.n);
  stats.set("resolution", ss.res);
  stats.set("eval_ms", total.ms());
  stats.set("layer_ms", lstats);
  r.stats = stats;
  return r;
}

SetMaps make_maps(const SampleSet& ss, const SetResult& r, const std::vector<std::string>& extra) {
  SetMaps m;
  m.name = r.name;
  m.set = r.set;
  m.res = r.res;
  size_t npx = (size_t)r.res * r.res;
  for (int ch = 0; ch < C_COUNT; ch++) {
    int k = kChannels[ch].comps;
    m.ch[ch].resize(npx * k);
    to_image(ss, r.stack.ch[ch].data(), k, m.ch[ch].data());
    m.used[ch] = r.stack.used[ch];
  }
  for (auto& e : extra) {
    std::vector<float> img(npx);
    if (e.rfind("mask:", 0) == 0) {
      auto it = r.masks.find(e.substr(5));
      if (it == r.masks.end()) continue;
      to_image(ss, it->second.data(), 1, img.data());
    } else if (e == "bake_ao") to_image(ss, ss.ao.data(), 1, img.data());
    else if (e == "curvature") to_image(ss, ss.curvature.data(), 1, img.data());
    else if (e == "thickness") to_image(ss, ss.thickness.data(), 1, img.data());
    else if (e == "bake_height") {
      if (ss.height.empty()) continue;
      to_image(ss, ss.height.data(), 1, img.data());
    } else if (e == "bake_misses") {
      if (ss.nmiss.empty()) continue;
      std::vector<float> f(ss.size());
      for (size_t i = 0; i < f.size(); i++) f[i] = ss.nmiss[i];
      to_image(ss, f.data(), 1, img.data());
    } else if (e == "bake_normal") {  // 3 components
      if (ss.nmap.empty()) continue;
      std::vector<float> f(ss.size() * 3);
      for (size_t i = 0; i < ss.size(); i++) { f[i * 3] = ss.nmap[i].x; f[i * 3 + 1] = ss.nmap[i].y; f[i * 3 + 2] = ss.nmap[i].z; }
      img.resize(npx * 3);
      to_image(ss, f.data(), 3, img.data());
    } else continue;
    m.extra[e] = std::move(img);
  }
  return m;
}

Json field_catalog() {
  Json j = Json::object();
  for (auto& d : field_defs()) {
    Json f = Json::object();
    f.set("category", d.category);
    f.set("summary", d.summary);
    Json p = Json::object();
    for (auto& pd : d.params) p.set(pd.name, strf("%s%s%s", pd.def, *pd.def ? " - " : "", pd.doc));
    f.set("params", p);
    j.set(d.type, f);
  }
  return j;
}

Json blend_mode_list() {
  Json j = Json::object();
  for (auto& b : kBlendModes) j.set(b.name, b.doc);
  return j;
}

}  // namespace pt
