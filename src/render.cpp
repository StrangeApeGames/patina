#include "render.h"

#include "envmap.h"

#include <algorithm>

extern "C" int patina_easy_font_print(float x, float y, const char* text, unsigned char color[4], void* vertex_buffer, int vbuf_size);

namespace pt {

// ---------------------------------------------------------------- views
static const struct { const char* name; float az, el; } kViews[] = {
    {"front", 0, 0}, {"back", 180, 0}, {"right", 90, 0}, {"left", -90, 0}, {"top", 0, 90}, {"bottom", 0, -90},
    {"iso", 35, 25}, {"iso_back", 215, 25}, {"iso_left", -35, 25}, {"iso_back_left", 145, 25}, {"low", 30, -12}};

std::vector<std::string> view_names() {
  std::vector<std::string> v;
  for (auto& k : kViews) v.push_back(k.name);
  return v;
}

static ViewSpec named_view(const std::string& n) {
  for (auto& k : kViews)
    if (n == k.name) { ViewSpec v; v.name = n; v.azimuth = k.az; v.elevation = k.el; return v; }
  // "az:el" shorthand
  float az, el;
  if (sscanf(n.c_str(), "%f:%f", &az, &el) == 2) { ViewSpec v; v.name = n; v.azimuth = az; v.elevation = el; return v; }
  std::string dym = did_you_mean(n, view_names());
  fail("unknown view '%s'%s (views: front back left right top bottom iso iso_back iso_left iso_back_left low, or \"azimuth:elevation\")", n.c_str(),
       dym.empty() ? "" : (" - did you mean '" + dym + "'?").c_str());
}

std::vector<ViewSpec> parse_views(const Json& views) {
  std::vector<ViewSpec> out;
  auto add_str = [&](const std::string& s) {
    size_t start = 0;
    for (size_t i = 0; i <= s.size(); i++)
      if (i == s.size() || s[i] == ',') {
        std::string t = s.substr(start, i - start);
        while (!t.empty() && t[0] == ' ') t.erase(0, 1);
        while (!t.empty() && t.back() == ' ') t.pop_back();
        if (!t.empty()) out.push_back(named_view(t));
        start = i + 1;
      }
  };
  if (views.is_null()) add_str("iso,iso_back,front,top");
  else if (views.is_string()) add_str(views.as_str());
  else if (views.is_array()) {
    for (auto& v : views.items()) {
      if (v.is_string()) add_str(v.as_str());
      else if (v.is_object()) {
        ViewSpec s = v.has("view") ? named_view(v.str("view")) : ViewSpec();
        s.azimuth = v.numf("azimuth", s.azimuth);
        s.elevation = v.numf("elevation", s.elevation);
        s.zoom = std::clamp(v.numf("zoom", 1.f), 0.1f, 50.f);
        s.name = v.str("name", s.name.empty() ? strf("%.0f:%.0f", s.azimuth, s.elevation) : s.name);
        if (v["target"].is_array()) {
          s.has_target = true;
          s.target_bbox = {v["target"][0].as_float(0.5f), v["target"][1].as_float(0.5f), v["target"][2].as_float(0.5f)};
          if (s.zoom != 1.f && s.name.find("zoom") == std::string::npos) s.name += strf(" x%.1f", s.zoom);
        }
        out.push_back(s);
      }
    }
  }
  if (out.empty()) fail("no views given");
  if (out.size() > 36) fail("too many views (max 36)");
  return out;
}

Json render_modes() {
  Json j = Json::object();
  j.set("lit", "full PBR preview (default)");
  j.set("clay", "neutral clay with the normal/height detail - check shape of height work");
  j.set("basecolor", "unlit base color");
  j.set("roughness", "grayscale roughness");
  j.set("metallic", "grayscale metallic");
  j.set("normal", "tangent-space normal colors");
  j.set("height", "height (0.5 = neutral)");
  j.set("ao", "final ambient occlusion");
  j.set("emissive", "emissive color");
  j.set("opacity", "opacity");
  j.set("curvature", "baked curvature (0.5 = flat)");
  j.set("thickness", "baked thickness");
  j.set("bake_ao", "baked ambient occlusion only");
  j.set("bake_height", "the height baked from the high poly (0.5 = on the low surface)");
  j.set("bake_normal", "the baked tangent-space normal map alone (high poly / bevel shader), as colors");
  j.set("bake_misses", "red where the normal bake found no high-poly surface: widen bake.normal cage/depth or fix part names");
  j.set("mask:<layer_id>", "a layer's effective mask in red over clay - debug where a layer applies");
  j.set("islands", "random color per UV island");
  j.set("uv_checker", "UV checker to inspect distortion/texel density");
  j.set("wireframe", "clay with every triangle edge drawn: topology, density and bevels at a glance");
  j.set("parts", "random color per mesh part");
  return j;
}

// ---------------------------------------------------------------- text
void draw_text(RgbImage& img, int x0, int y0, const std::string& text, uint8_t r, uint8_t g, uint8_t b, int scale) {
  static thread_local std::vector<char> buf(1 << 16);
  unsigned char col[4] = {255, 255, 255, 255};
  int quads = patina_easy_font_print(0, 0, text.c_str(), col, buf.data(), (int)buf.size());
  struct V { float x, y, z; unsigned char c[4]; };
  const V* v = (const V*)buf.data();
  for (int q = 0; q < quads; q++) {
    float qx0 = v[q * 4].x, qy0 = v[q * 4].y, qx1 = v[q * 4 + 2].x, qy1 = v[q * 4 + 2].y;
    int ax = x0 + (int)(qx0 * scale), ay = y0 + (int)(qy0 * scale), bx = x0 + (int)(qx1 * scale), by = y0 + (int)(qy1 * scale);
    for (int y = std::max(0, ay); y < std::min(img.h, by); y++)
      for (int x = std::max(0, ax); x < std::min(img.w, bx); x++) {
        uint8_t* p = &img.px[((size_t)y * img.w + x) * 3];
        p[0] = r; p[1] = g; p[2] = b;
      }
  }
}

static int text_width(const std::string& s, int scale) {
  // stb_easy_font: ~6px per glyph at scale 1 (approximation for layout)
  return (int)s.size() * 6 * scale;
}

// ---------------------------------------------------------------- sampling
static inline void sample_map(const std::vector<float>& img, int comps, int res, float u, float v, float* out) {
  if (img.empty()) { for (int k = 0; k < comps; k++) out[k] = 0; return; }
  float x = u * res - 0.5f, y = v * res - 0.5f;
  float fx = std::floor(x), fy = std::floor(y);
  int x0 = std::clamp((int)fx, 0, res - 1), y0 = std::clamp((int)fy, 0, res - 1);
  int x1 = std::min(x0 + 1, res - 1), y1 = std::min(y0 + 1, res - 1);
  float tx = saturate(x - fx), ty = saturate(y - fy);
  for (int k = 0; k < comps; k++) {
    float a = img[((size_t)y0 * res + x0) * comps + k], b = img[((size_t)y0 * res + x1) * comps + k];
    float c = img[((size_t)y1 * res + x0) * comps + k], d = img[((size_t)y1 * res + x1) * comps + k];
    out[k] = lerp(lerp(a, b, tx), lerp(c, d, tx), ty);
  }
}

// ---------------------------------------------------------------- shading
static inline vec3 aces(vec3 x) {
  auto f = [](float v) { return saturate((v * (2.51f * v + 0.03f)) / (v * (2.43f * v + 0.59f) + 0.14f)); };
  return {f(x.x), f(x.y), f(x.z)};
}

struct Lighting {
  vec3 R, U, F;  // camera basis (F = forward)
  vec3 key, rim, fill;
  vec3 key_col{2.9f, 2.8f, 2.65f}, rim_col{1.3f, 1.4f, 1.6f}, fill_col{0.45f, 0.47f, 0.5f};
  const EnvMap* hdri = nullptr;  // world-space environment; null = analytic studio (env below)
  float env_cos = 1, env_sin = 0, env_k = 1;
  vec3 to_env(vec3 d) const { return {d.x * env_cos - d.z * env_sin, d.y, d.x * env_sin + d.z * env_cos}; }
};

// Studio environment in camera space: dark floor, sharp horizon, gradient sky and several soft boxes
// (key, rim, overhead, vertical strip) so glossy metals have structure to reflect. Rougher surfaces see
// wider, dimmer lobes and a softer horizon.
static inline vec3 env(const Lighting& L, vec3 d, float rough) {
  float y = dot(d, L.U);
  float r2 = rough * rough;
  float hw = 0.03f + r2 * 0.9f;
  vec3 ground{0.055f, 0.052f, 0.05f}, horizon{0.34f, 0.35f, 0.37f}, zenith{0.62f, 0.65f, 0.7f};
  vec3 sky = lerp(horizon, zenith, saturate(y));
  vec3 c = lerp(ground, sky, smoothstep(-hw, hw, y));
  float spread = 0.1f + r2 * 1.1f;
  float s2 = spread * spread;
  float norm = 0.1f * 0.1f / s2;
  c += vec3(3.4f, 3.3f, 3.1f) * (std::exp((dot(d, L.key) - 1.f) / s2) * norm);
  c += vec3(1.6f, 1.7f, 1.9f) * (std::exp((dot(d, L.rim) - 1.f) / s2) * norm);
  c += vec3(1.6f, 1.6f, 1.6f) * (std::exp((dot(d, L.U) - 1.f) / (s2 * 1.6f)) * norm);
  // vertical strip light on the left: ignore most of the vertical component
  vec3 dh = normalize(d - L.U * (dot(d, L.U) * 0.8f));
  vec3 strip = normalize(L.R * -0.85f - L.F * 0.5f);
  c += vec3(2.2f, 2.2f, 2.3f) * (std::exp((dot(dh, strip) - 1.f) / (s2 * 0.35f)) * norm * 0.6f);
  return c;
}

static inline vec3 shade_pbr(const Lighting& L, vec3 N, vec3 V, vec3 base, float metal, float rough, float ao, float key_vis) {
  rough = std::fmax(0.03f, rough);
  float a = rough * rough, a2 = a * a;
  vec3 F0 = lerp(vec3(0.04f), base, metal);
  vec3 albedo = base * (1.f - metal);
  float NdotV = std::fmax(1e-4f, dot(N, V));
  vec3 col{0, 0, 0};
  auto light = [&](vec3 Ld, vec3 Lc, float vis) {
    float NdotL = dot(N, Ld);
    if (NdotL <= 0 || vis <= 0) return;
    vec3 H = normalize(Ld + V);
    float NdotH = std::fmax(0.f, dot(N, H)), VdotH = std::fmax(0.f, dot(V, H));
    float d = NdotH * NdotH * (a2 - 1.f) + 1.f;
    float D = a2 / (kPi * d * d);
    float k = (rough + 1.f) * (rough + 1.f) / 8.f;
    float G = (NdotV / (NdotV * (1 - k) + k)) * (NdotL / (NdotL * (1 - k) + k));
    float fw = std::pow(1.f - VdotH, 5.f);
    vec3 F = F0 + (vec3(1.f) - F0) * fw;
    vec3 spec = F * (D * G / (4.f * NdotV * NdotL + 1e-4f));
    vec3 kd = (vec3(1.f) - F) * (1.f - metal);
    col += (kd * albedo / kPi + spec) * Lc * (NdotL * vis);
  };
  light(L.key, L.key_col, key_vis);
  light(L.rim, L.rim_col, 1.f);
  light(L.fill, L.fill_col, 1.f);
  // image-based terms (Karis' analytic env BRDF approximation)
  vec3 Rv = normalize(N * (2.f * dot(N, V)) - V);
  vec4 c0{-1, -0.0275f, -0.572f, 0.022f}, c1{1, 0.0425f, 1.04f, -0.04f};
  vec4 r{rough * c0.x + c1.x, rough * c0.y + c1.y, rough * c0.z + c1.z, rough * c0.w + c1.w};
  float a004 = std::fmin(r.x * r.x, std::exp2(-9.28f * NdotV)) * r.x + r.y;
  float A = a004 * -1.04f + r.z, B = a004 * 1.04f + r.w;
  vec3 spec_ibl, diff_ibl;
  if (L.hdri) {
    spec_ibl = L.hdri->radiance(L.to_env(Rv), rough) * L.env_k * (F0 * A + vec3(B));
    diff_ibl = L.hdri->irradiance(L.to_env(N)) * L.env_k * albedo;
  } else {
    spec_ibl = env(L, Rv, rough) * (F0 * A + vec3(B));
    diff_ibl = env(L, N, 1.f) * albedo * 0.9f;
  }
  col += (spec_ibl * (0.5f + 0.5f * ao) + diff_ibl * ao);
  return col;
}

// ---------------------------------------------------------------- rasterizer
namespace {
constexpr int kTile = 32;
struct Frame {
  int W, H;
  std::vector<float> depth;
  std::vector<int32_t> tri;
  std::vector<float> b1, b2;  // perspective-correct barycentrics of v1, v2
};
struct SV { float x, y, z, iw; bool ok; };
}  // namespace

static void rasterize(const Mesh& m, const std::vector<SV>& sv, Frame& f) {
  int W = f.W, H = f.H;
  int tx = (W + kTile - 1) / kTile, ty = (H + kTile - 1) / kTile;
  size_t nt = m.tri_count();
  // bin triangles (parallel per chunk, then merge)
  int nchunks = std::max(1, std::min(thread_count() * 2, (int)(nt / 4096) + 1));
  std::vector<std::vector<std::vector<uint32_t>>> cb(nchunks, std::vector<std::vector<uint32_t>>((size_t)tx * ty));
  parallel_for(nchunks, 1, [&](int64_t c0, int64_t c1) {
    for (int64_t ch = c0; ch < c1; ch++) {
      size_t b = nt * ch / nchunks, e = nt * (ch + 1) / nchunks;
      for (size_t t = b; t < e; t++) {
        const SV &a = sv[m.idx[t * 3]], &bb = sv[m.idx[t * 3 + 1]], &c = sv[m.idx[t * 3 + 2]];
        if (!a.ok || !bb.ok || !c.ok) continue;
        float x0 = std::fmin(a.x, std::fmin(bb.x, c.x)), x1 = std::fmax(a.x, std::fmax(bb.x, c.x));
        float y0 = std::fmin(a.y, std::fmin(bb.y, c.y)), y1 = std::fmax(a.y, std::fmax(bb.y, c.y));
        if (x1 < 0 || y1 < 0 || x0 >= W || y0 >= H) continue;
        float area = (bb.x - a.x) * (c.y - a.y) - (bb.y - a.y) * (c.x - a.x);
        if (std::fabs(area) < 1e-12f) continue;
        int bx0 = std::max(0, (int)x0 / kTile), bx1 = std::min(tx - 1, (int)x1 / kTile);
        int by0 = std::max(0, (int)y0 / kTile), by1 = std::min(ty - 1, (int)y1 / kTile);
        for (int y = by0; y <= by1; y++)
          for (int x = bx0; x <= bx1; x++) cb[ch][(size_t)y * tx + x].push_back((uint32_t)t);
      }
    }
  });
  f.depth.assign((size_t)W * H, 1e30f);
  f.tri.assign((size_t)W * H, -1);
  f.b1.assign((size_t)W * H, 0);
  f.b2.assign((size_t)W * H, 0);
  parallel_for((int64_t)tx * ty, 1, [&](int64_t t0, int64_t t1) {
    for (int64_t ti = t0; ti < t1; ti++) {
      int bx = (int)(ti % tx), by = (int)(ti / tx);
      int px0 = bx * kTile, py0 = by * kTile, px1 = std::min(W, px0 + kTile), py1 = std::min(H, py0 + kTile);
      for (int ch = 0; ch < nchunks; ch++)
        for (uint32_t t : cb[ch][ti]) {
          const SV &a = sv[m.idx[t * 3]], &b = sv[m.idx[t * 3 + 1]], &c = sv[m.idx[t * 3 + 2]];
          float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
          float inv = 1.f / area;
          int x0 = std::max(px0, (int)std::floor(std::fmin(a.x, std::fmin(b.x, c.x))));
          int x1 = std::min(px1 - 1, (int)std::ceil(std::fmax(a.x, std::fmax(b.x, c.x))));
          int y0 = std::max(py0, (int)std::floor(std::fmin(a.y, std::fmin(b.y, c.y))));
          int y1 = std::min(py1 - 1, (int)std::ceil(std::fmax(a.y, std::fmax(b.y, c.y))));
          for (int y = y0; y <= y1; y++)
            for (int x = x0; x <= x1; x++) {
              float px = x + 0.5f, py = y + 0.5f;
              float w0 = ((b.x - px) * (c.y - py) - (b.y - py) * (c.x - px)) * inv;
              float w1 = ((c.x - px) * (a.y - py) - (c.y - py) * (a.x - px)) * inv;
              float w2 = 1.f - w0 - w1;
              if (w0 < 0 || w1 < 0 || w2 < 0) continue;
              float z = w0 * a.z + w1 * b.z + w2 * c.z;
              size_t i = (size_t)y * W + x;
              if (z >= f.depth[i]) continue;
              f.depth[i] = z;
              f.tri[i] = (int32_t)t;
              float p0 = w0 * a.iw, p1 = w1 * b.iw, p2 = w2 * c.iw;
              float s = 1.f / (p0 + p1 + p2);
              f.b1[i] = p1 * s;
              f.b2[i] = p2 * s;
            }
        }
    }
  });
}

RgbImage render_view(const Mesh& m, const BVH& bvh, const std::vector<SetMaps>& maps, const ViewSpec& v, const RenderOptions& o) {
  int ss = std::clamp(o.ssaa, 1, 4);
  int ow = o.width > 0 ? o.width : o.size, oh = o.height > 0 ? o.height : o.size;
  int W = ow * ss, H = oh * ss;
  vec3 target = v.has_world_target ? v.target_world : v.has_target ? m.bmin + v.target_bbox * m.size() : m.center();
  float radius = std::fmax(1e-4f, m.radius());
  float fov = 30.f * kPi / 180.f;
  float fit_fov = W >= H ? fov : 2.f * std::atan(std::tan(fov * 0.5f) * (float)W / H);
  float dist = radius / std::sin(fit_fov * 0.5f) * 1.04f / v.zoom;
  float az = v.azimuth * kPi / 180.f, el = clampf(v.elevation, -89.9f, 89.9f) * kPi / 180.f;
  vec3 dir{std::sin(az) * std::cos(el), std::sin(el), std::cos(az) * std::cos(el)};
  vec3 eye = target + dir * dist;
  vec3 up{0, 1, 0};
  if (std::fabs(v.elevation) > 80.f) up = v.elevation > 0 ? vec3(-std::sin(az), 0, -std::cos(az)) : vec3(std::sin(az), 0, std::cos(az));
  float zn = std::fmax(dist - radius * 2.5f, dist * 0.01f), zf = dist + radius * 2.5f;
  mat4 view = look_at(eye, target, up);
  mat4 proj = perspective(fov, (float)W / H, zn, zf);
  mat4 vp = proj * view;

  std::vector<SV> sv(m.pos.size());
  parallel_for((int64_t)m.pos.size(), [&](int64_t b, int64_t e) {
    for (int64_t i = b; i < e; i++) {
      vec4 c = vp.mul(vec4(m.pos[i], 1.f));
      SV s;
      s.ok = c.w > 1e-6f;
      float iw = s.ok ? 1.f / c.w : 0.f;
      s.x = (c.x * iw * 0.5f + 0.5f) * W;
      s.y = (1.f - (c.y * iw * 0.5f + 0.5f)) * H;
      s.z = c.z * iw;
      s.iw = iw;
      sv[i] = s;
    }
  });
  Frame f;
  f.W = W;
  f.H = H;
  rasterize(m, sv, f);

  Lighting L;
  L.F = normalize(target - eye);
  L.R = normalize(cross(L.F, up));
  L.U = cross(L.R, L.F);
  L.key = normalize(L.R * -0.55f + L.U * 0.7f - L.F * 0.55f);
  L.rim = normalize(L.R * 0.8f + L.U * 0.35f + L.F * 0.6f);
  L.fill = normalize(L.R * 0.75f - L.U * 0.1f - L.F * 0.6f);
  std::shared_ptr<const EnvMap> hdri;
  if (o.environment != "procedural") {
    hdri = load_environment(o.environment);
    L.hdri = hdri.get();
    float rot = o.env_rotation * kPi / 180.f;
    L.env_cos = std::cos(rot);
    L.env_sin = std::sin(rot);
    L.env_k = o.env_intensity;
    // the environment does most of the lighting; a dimmer key light keeps the shadow shape readable
    L.key_col = L.key_col * 0.35f;
    L.rim_col = vec3(0.f);
    L.fill_col = vec3(0.f);
  }

  std::string mode = o.mode;
  std::string mask_key;
  if (mode.rfind("mask:", 0) == 0 || mode == "bake_misses") mask_key = mode;
  float eps = m.max_extent() * 2e-4f;
  std::vector<vec3> hdr((size_t)W * H);
  parallel_for(H, 4, [&](int64_t y0, int64_t y1) {
    for (int64_t y = y0; y < y1; y++)
      for (int x = 0; x < W; x++) {
        size_t i = (size_t)y * W + x;
        int32_t t = f.tri[i];
        float fy = (float)y / H;
        if (t < 0) {
          // background: soft vertical gradient
          vec3 top{0.205f, 0.215f, 0.232f}, bot{0.115f, 0.12f, 0.13f};
          hdr[i] = lerp(top, bot, fy) * -1.f;  // negative marks "already display-referred"
          continue;
        }
        float b1 = f.b1[i], b2 = f.b2[i], b0 = 1.f - b1 - b2;
        uint32_t i0 = m.idx[t * 3], i1 = m.idx[t * 3 + 1], i2 = m.idx[t * 3 + 2];
        vec3 pos = m.pos[i0] * b0 + m.pos[i1] * b1 + m.pos[i2] * b2;
        vec3 nrm = normalize(m.nrm[i0] * b0 + m.nrm[i1] * b1 + m.nrm[i2] * b2);
        vec3 tg = m.tan[i0].xyz() * b0 + m.tan[i1].xyz() * b1 + m.tan[i2].xyz() * b2;
        float tw = m.tan[i0].w;
        vec2 uv = m.uv[i0] * b0 + m.uv[i1] * b1 + m.uv[i2] * b2;
        vec3 fn = normalize(cross(m.pos[i1] - m.pos[i0], m.pos[i2] - m.pos[i0]));
        vec3 V = normalize(eye - pos);
        if (dot(fn, V) < 0) { fn = -fn; nrm = -nrm; }
        int set = m.tri_set[t];
        const SetMaps* sm = set < (int)maps.size() && maps[set].res > 0 ? &maps[set] : nullptr;
        int res = sm ? sm->res : 0;
        float base[3] = {0.5f, 0.5f, 0.5f}, metal = 0, rough = 0.6f, ao = 1, nts[3] = {0, 0, 1}, em[3] = {0, 0, 0}, op = 1;
        if (sm) {
          sample_map(sm->ch[C_BASECOLOR], 3, res, uv.x, uv.y, base);
          sample_map(sm->ch[C_METALLIC], 1, res, uv.x, uv.y, &metal);
          sample_map(sm->ch[C_ROUGHNESS], 1, res, uv.x, uv.y, &rough);
          sample_map(sm->ch[C_AO], 1, res, uv.x, uv.y, &ao);
          sample_map(sm->ch[C_NORMAL], 3, res, uv.x, uv.y, nts);
          if (sm->used[C_EMISSIVE]) sample_map(sm->ch[C_EMISSIVE], 3, res, uv.x, uv.y, em);
          if (sm->used[C_OPACITY]) sample_map(sm->ch[C_OPACITY], 1, res, uv.x, uv.y, &op);
        } else {
          base[0] = base[1] = base[2] = 0.18f;
        }
        // MikkTSpace decode (what engines do): unnormalized interpolated vT, vN; vB = sign * cross(vN, vT)
        vec3 vN = m.nrm[i0] * b0 + m.nrm[i1] * b1 + m.nrm[i2] * b2;
        if (dot(vN, nrm) < 0) vN = -vN;
        vec3 bt = cross(vN, tg) * tw;
        vec3 N = normalize(tg * nts[0] + bt * nts[1] + vN * nts[2]);
        if (dot(N, V) < 0) N = normalize(N - V * (dot(N, V) * 1.01f));
        auto key_vis = [&]() {
          if (!o.shadows) return 1.f;
          if (dot(fn, L.key) <= 0) return 0.f;
          return bvh.occluded(pos + fn * eps, L.key, 0.f, 1e30f) ? 0.12f : 1.f;
        };
        vec3 c;
        auto gray = [](float g) { return vec3(g, g, g) * -1.f; };
        if (mode == "lit") {
          vec3 bc{base[0], base[1], base[2]};
          c = shade_pbr(L, N, V, bc, metal, rough, ao, key_vis()) * o.exposure + vec3(em[0], em[1], em[2]);
          if (op < 1.f) c = lerp(lerp(vec3(0.205f), vec3(0.115f), fy), c, op);
        } else if (mode == "clay" || mode == "wireframe" || !mask_key.empty() || mode == "islands" || mode == "parts" || mode == "uv_checker") {
          vec3 bc{0.22f, 0.22f, 0.22f};
          if (!mask_key.empty() && sm) {
            auto it = sm->extra.find(mask_key);
            float mv = 0;
            if (it != sm->extra.end()) sample_map(it->second, 1, res, uv.x, uv.y, &mv);
            bc = lerp(bc, vec3(0.9f, 0.02f, 0.01f), std::sqrt(saturate(mv)));
          } else if (mode == "islands" || mode == "parts") {
            uint32_t h = mode == "islands" ? (uint32_t)m.tri_island[t] * 2654435761u + 7u : (uint32_t)m.tri_part[t] * 2246822519u + 3u;
            bc = {0.15f + 0.7f * hash_float(h), 0.15f + 0.7f * hash_float(h ^ 0x9e37u), 0.15f + 0.7f * hash_float(h ^ 0x85ebu)};
          } else if (mode == "uv_checker") {
            int k = (int)std::floor(uv.x * 16) + (int)std::floor(uv.y * 16);
            bc = (k & 1) ? vec3(0.8f, 0.8f, 0.8f) : vec3(0.08f, 0.08f, 0.08f);
            if (((int)std::floor(uv.x * 2) + (int)std::floor(uv.y * 2)) & 1) bc = bc * vec3(1.f, 0.55f, 0.35f);
          }
          vec3 Nc = mode == "clay" || mode == "wireframe" || !mask_key.empty() ? N : nrm;
          c = shade_pbr(L, Nc, V, bc, 0.f, 0.55f, 1.f, key_vis()) * o.exposure;
          if (mode == "wireframe") {  // distance (pixels) to the nearest triangle edge on screen
            const SV &A = sv[i0], &B = sv[i1], &C = sv[i2];
            float px = x + 0.5f, py = y + 0.5f;
            float a2 = (B.x - A.x) * (C.y - A.y) - (B.y - A.y) * (C.x - A.x);
            if (std::fabs(a2) > 1e-6f) {
              float w0 = ((B.x - px) * (C.y - py) - (B.y - py) * (C.x - px)) / a2;
              float w1 = ((C.x - px) * (A.y - py) - (C.y - py) * (A.x - px)) / a2;
              float w2 = 1.f - w0 - w1;
              auto len = [](const SV& p, const SV& q) { return std::fmax(1e-6f, std::hypot(p.x - q.x, p.y - q.y)); };
              float d = std::fmin(std::fabs(w0 * a2) / len(B, C), std::fmin(std::fabs(w1 * a2) / len(C, A), std::fabs(w2 * a2) / len(A, B)));
              float lw = 0.75f * ss;
              float k = 1.f - smoothstep(lw * 0.5f, lw * 1.5f, d);
              c = lerp(c, vec3(0.015f, 0.02f, 0.03f), k * 0.9f);
            }
          }
        } else if (mode == "basecolor") {
          c = vec3(linear_to_srgb(base[0]), linear_to_srgb(base[1]), linear_to_srgb(base[2])) * -1.f;
        } else if (mode == "emissive") {
          c = vec3(linear_to_srgb(em[0]), linear_to_srgb(em[1]), linear_to_srgb(em[2])) * -1.f;
        } else if (mode == "roughness") c = gray(rough);
        else if (mode == "metallic") c = gray(metal);
        else if (mode == "ao") c = gray(ao);
        else if (mode == "opacity") c = gray(op);
        else if (mode == "normal") c = vec3(nts[0] * 0.5f + 0.5f, nts[1] * 0.5f + 0.5f, nts[2] * 0.5f + 0.5f) * -1.f;
        else if (mode == "bake_normal") {
          float bn[3] = {0, 0, 1};
          if (sm) {
            auto it = sm->extra.find(mode);
            if (it != sm->extra.end()) sample_map(it->second, 3, res, uv.x, uv.y, bn);
          }
          c = vec3(bn[0] * 0.5f + 0.5f, bn[1] * 0.5f + 0.5f, bn[2] * 0.5f + 0.5f) * -1.f;
        }
        else if (mode == "height" || mode == "curvature" || mode == "thickness" || mode == "bake_ao" || mode == "bake_height") {
          float h = mode == "bake_height" ? 0.5f : 0.f;
          if (sm) {
            if (mode == "height") { sample_map(sm->ch[C_HEIGHT], 1, res, uv.x, uv.y, &h); h = saturate(0.5f + 0.5f * h); }
            else {
              auto it = sm->extra.find(mode);
              if (it != sm->extra.end()) sample_map(it->second, 1, res, uv.x, uv.y, &h);
            }
          }
          c = gray(h);
        } else {
          c = vec3(1, 0, 1) * -1.f;
        }
        hdr[i] = c;
      }
  });

  RgbImage img;
  img.w = ow;
  img.h = oh;
  img.px.resize((size_t)img.w * img.h * 3);
  float inv = 1.f / (ss * ss);
  parallel_for(img.h, 8, [&](int64_t y0, int64_t y1) {
    for (int64_t y = y0; y < y1; y++)
      for (int x = 0; x < img.w; x++) {
        vec3 acc{0, 0, 0};
        for (int sy = 0; sy < ss; sy++)
          for (int sx = 0; sx < ss; sx++) {
            vec3 h = hdr[((size_t)y * ss + sy) * W + (size_t)x * ss + sx];
            // negative = display-referred (unlit modes/background), positive = linear HDR to tonemap.
            // (a display-referred 0 and an HDR 0 both map to black, so the sign trick is unambiguous)
            bool display = h.x < 0 || h.y < 0 || h.z < 0;
            vec3 d;
            if (display) d = h * -1.f;
            else { d = aces(h); d = {linear_to_srgb(d.x), linear_to_srgb(d.y), linear_to_srgb(d.z)}; }
            acc += d;
          }
        acc *= inv;
        uint8_t* p = &img.px[((size_t)y * img.w + x) * 3];
        p[0] = (uint8_t)std::lround(saturate(acc.x) * 255.f);
        p[1] = (uint8_t)std::lround(saturate(acc.y) * 255.f);
        p[2] = (uint8_t)std::lround(saturate(acc.z) * 255.f);
      }
  });
  return img;
}

RgbImage compose_grid(const std::vector<RgbImage>& tiles, const std::vector<std::string>& labels, int columns, const std::string& title) {
  if (tiles.empty()) return {};
  int n = (int)tiles.size();
  int cols = columns > 0 ? columns : (n <= 3 ? n : (n == 4 ? 2 : (n <= 6 ? 3 : 4)));
  int rows = (n + cols - 1) / cols;
  int tw = tiles[0].w, th = tiles[0].h;
  int gap = 4;
  int title_h = title.empty() ? 0 : 26;
  RgbImage out;
  out.w = cols * tw + (cols - 1) * gap;
  out.h = title_h + rows * th + (rows - 1) * gap;
  out.px.assign((size_t)out.w * out.h * 3, 18);
  for (int i = 0; i < n; i++) {
    int cx = (i % cols) * (tw + gap), cy = title_h + (i / cols) * (th + gap);
    const RgbImage& t = tiles[i];
    for (int y = 0; y < std::min(th, t.h); y++)
      memcpy(&out.px[((size_t)(cy + y) * out.w + cx) * 3], &t.px[(size_t)y * t.w * 3], (size_t)std::min(tw, t.w) * 3);
    if (i < (int)labels.size() && !labels[i].empty()) {
      int sc = tw >= 400 ? 2 : 1;
      draw_text(out, cx + 7, cy + 7, labels[i], 0, 0, 0, sc);
      draw_text(out, cx + 6, cy + 6, labels[i], 235, 235, 235, sc);
    }
  }
  if (!title.empty()) draw_text(out, 8, out.w >= 700 ? 6 : 9, title, 220, 220, 220, out.w >= 700 ? 2 : 1);
  return out;
}

RgbImage render_views(const Mesh& m, const BVH& bvh, const std::vector<SetMaps>& maps, const std::vector<ViewSpec>& views, const RenderOptions& o) {
  std::vector<RgbImage> tiles(views.size());
  parallel_for((int64_t)views.size(), 1, [&](int64_t b, int64_t e) {
    for (int64_t i = b; i < e; i++) tiles[i] = render_view(m, bvh, maps, views[i], o);
  });
  std::vector<std::string> labels;
  for (auto& v : views) labels.push_back(o.labels ? v.name : "");
  return compose_grid(tiles, labels, o.columns, o.labels ? o.title : "");
}

// ---------------------------------------------------------------- texture sheet
static RgbImage thumb_of(const std::vector<float>& img, int comps, int res, int thumb, int kind /*0 srgb color,1 gray,2 normal,3 height*/) {
  RgbImage t;
  t.w = t.h = thumb;
  t.px.resize((size_t)thumb * thumb * 3);
  int step = std::max(1, res / thumb);
  parallel_for(thumb, 4, [&](int64_t y0, int64_t y1) {
    for (int64_t y = y0; y < y1; y++)
      for (int x = 0; x < thumb; x++) {
        float acc[3] = {0, 0, 0};
        int cnt = 0;
        int sx0 = (int)((int64_t)x * res / thumb), sy0 = (int)((int64_t)y * res / thumb);
        for (int dy = 0; dy < step; dy++)
          for (int dx = 0; dx < step; dx++) {
            int sx = std::min(res - 1, sx0 + dx), sy = std::min(res - 1, sy0 + dy);
            for (int k = 0; k < comps; k++) acc[k] += img[((size_t)sy * res + sx) * comps + k];
            cnt++;
          }
        for (int k = 0; k < comps; k++) acc[k] /= cnt;
        float r, g, b;
        if (comps == 1) {
          float v = kind == 3 ? saturate(0.5f + 0.5f * acc[0]) : acc[0];
          r = g = b = v;
        } else if (kind == 2) {
          r = acc[0] * 0.5f + 0.5f; g = acc[1] * 0.5f + 0.5f; b = acc[2] * 0.5f + 0.5f;
        } else {
          r = linear_to_srgb(acc[0]); g = linear_to_srgb(acc[1]); b = linear_to_srgb(acc[2]);
        }
        uint8_t* p = &t.px[((size_t)y * thumb + x) * 3];
        p[0] = (uint8_t)std::lround(saturate(r) * 255); p[1] = (uint8_t)std::lround(saturate(g) * 255); p[2] = (uint8_t)std::lround(saturate(b) * 255);
      }
  });
  return t;
}

// Flat UV layout per texture set: every UV triangle edge over the set's (dimmed) base colour.
RgbImage render_uv_layout(const Mesh& m, const std::vector<SetMaps>& maps, int size, const std::string& title) {
  std::vector<RgbImage> tiles;
  std::vector<std::string> labels;
  for (size_t s = 0; s < m.set_names.size(); s++) {
    RgbImage img;
    const SetMaps* sm = s < maps.size() && maps[s].res > 0 ? &maps[s] : nullptr;
    if (sm) img = thumb_of(sm->ch[C_BASECOLOR], 3, sm->res, size, 0);
    else { img.w = img.h = size; img.px.assign((size_t)size * size * 3, 40); }
    for (auto& p : img.px) p = (uint8_t)(p * 0.45f);
    auto plot = [&](int x, int y) {
      if (x < 0 || y < 0 || x >= size || y >= size) return;
      uint8_t* q = &img.px[((size_t)y * size + x) * 3];
      q[0] = 255; q[1] = 214; q[2] = 110;
    };
    auto line = [&](vec2 a, vec2 b) {  // Bresenham
      int x0 = (int)std::floor(a.x * size), y0 = (int)std::floor(a.y * size), x1 = (int)std::floor(b.x * size), y1 = (int)std::floor(b.y * size);
      int dx = std::abs(x1 - x0), dy = -std::abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx + dy;
      for (int guard = 0; guard < 4 * size; guard++) {
        plot(x0, y0);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
      }
    };
    for (size_t t = 0; t < m.tri_count(); t++) {
      if (m.tri_set[t] != s) continue;
      vec2 a = m.uv[m.idx[t * 3]], b = m.uv[m.idx[t * 3 + 1]], c = m.uv[m.idx[t * 3 + 2]];
      line(a, b); line(b, c); line(c, a);
    }
    tiles.push_back(std::move(img));
    labels.push_back(m.set_names[s] + " UVs");
  }
  return compose_grid(tiles, labels, 0, title);
}

RgbImage render_texture_sheet(const std::vector<SetMaps>& maps, int thumb, const std::string& title) {
  std::vector<RgbImage> tiles;
  std::vector<std::string> labels;
  for (auto& sm : maps) {
    if (sm.res == 0) continue;
    struct { int ch; const char* label; int kind; } list[] = {{C_BASECOLOR, "basecolor", 0}, {C_ROUGHNESS, "roughness", 1}, {C_METALLIC, "metallic", 1},
                                                             {C_NORMAL, "normal", 2}, {C_HEIGHT, "height", 3}, {C_AO, "ao", 1}};
    for (auto& e : list) {
      tiles.push_back(thumb_of(sm.ch[e.ch], kChannels[e.ch].comps, sm.res, thumb, e.kind));
      labels.push_back(sm.name + " " + e.label);
    }
  }
  return compose_grid(tiles, labels, 6, title);
}

}  // namespace pt
