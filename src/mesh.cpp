#include "mesh.h"

#include <algorithm>
#include <cstdio>
#include <unordered_map>

#include "cgltf.h"
#include "mikktspace.h"

namespace pt {

int Mesh::find_set(const std::string& name) const {
  for (size_t i = 0; i < set_names.size(); i++) if (set_names[i] == name) return (int)i;
  return -1;
}
int Mesh::find_part(const std::string& name) const {
  for (size_t i = 0; i < part_names.size(); i++) if (part_names[i] == name) return (int)i;
  return -1;
}

void compute_bounds(Mesh& m) {
  if (m.pos.empty()) return;
  vec3 lo = m.pos[0], hi = m.pos[0];
  for (auto& p : m.pos) { lo = vmin(lo, p); hi = vmax(hi, p); }
  m.bmin = lo;
  m.bmax = hi;
}

static uint16_t intern(std::vector<std::string>& names, const std::string& n) {
  for (size_t i = 0; i < names.size(); i++) if (names[i] == n) return (uint16_t)i;
  names.push_back(n);
  return (uint16_t)(names.size() - 1);
}

// ---------------------------------------------------------------- glTF
static void load_gltf(Mesh& m, const std::string& path, bool& had_normals) {
  cgltf_options opt{};
  cgltf_data* data = nullptr;
  cgltf_result r = cgltf_parse_file(&opt, path.c_str(), &data);
  if (r != cgltf_result_success) fail("cannot parse glTF '%s' (cgltf error %d)", path.c_str(), (int)r);
  struct Guard { cgltf_data* d; ~Guard() { cgltf_free(d); } } guard{data};
  r = cgltf_load_buffers(&opt, data, path.c_str());
  if (r != cgltf_result_success) fail("cannot load buffers of '%s' (cgltf error %d)", path.c_str(), (int)r);

  had_normals = true;
  bool any_missing_uv = false;

  auto emit_node = [&](cgltf_node* node) {
    if (!node->mesh) return;
    float world[16];
    cgltf_node_transform_world(node, world);
    // normal matrix = inverse transpose of upper 3x3
    float a = world[0], b = world[4], c = world[8], d = world[1], e = world[5], f = world[9], g = world[2], h = world[6], i = world[10];
    float det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    float inv[9] = {(e * i - f * h), -(d * i - f * g), (d * h - e * g), -(b * i - c * h), (a * i - c * g), -(a * h - b * g), (b * f - c * e), -(a * f - c * d), (a * e - b * d)};
    // inv (cofactor matrix) rows map directly to the normal transform: n' = C * n (then normalize)
    bool flip = det < 0;
    std::string part_name = node->name ? node->name : (node->mesh->name ? node->mesh->name : strf("Part%d", (int)m.part_names.size()));
    uint16_t part = intern(m.part_names, part_name);

    for (cgltf_size pi = 0; pi < node->mesh->primitives_count; pi++) {
      cgltf_primitive& prim = node->mesh->primitives[pi];
      if (prim.type != cgltf_primitive_type_triangles) {
        m.warnings.push_back(strf("skipped non-triangle primitive in '%s'", part_name.c_str()));
        continue;
      }
      cgltf_accessor *apos = nullptr, *anrm = nullptr, *auv = nullptr;
      for (cgltf_size ai = 0; ai < prim.attributes_count; ai++) {
        auto& at = prim.attributes[ai];
        if (at.type == cgltf_attribute_type_position) apos = at.data;
        else if (at.type == cgltf_attribute_type_normal) anrm = at.data;
        else if (at.type == cgltf_attribute_type_texcoord && at.index == 0) auv = at.data;
      }
      if (!apos) continue;
      if (!auv) any_missing_uv = true;
      if (!anrm) had_normals = false;

      std::string set_name = prim.material && prim.material->name ? prim.material->name
                             : prim.material ? strf("Material%d", (int)cgltf_material_index(data, prim.material))
                                             : std::string("Default");
      uint16_t set = intern(m.set_names, set_name);

      size_t base = m.pos.size();
      size_t nv = apos->count;
      std::vector<float> tmp(nv * 3);
      cgltf_accessor_unpack_floats(apos, tmp.data(), nv * 3);
      for (size_t v = 0; v < nv; v++) {
        float x = tmp[v * 3], y = tmp[v * 3 + 1], z = tmp[v * 3 + 2];
        m.pos.push_back({world[0] * x + world[4] * y + world[8] * z + world[12], world[1] * x + world[5] * y + world[9] * z + world[13],
                         world[2] * x + world[6] * y + world[10] * z + world[14]});
      }
      if (anrm && anrm->count == nv) {
        cgltf_accessor_unpack_floats(anrm, tmp.data(), nv * 3);
        for (size_t v = 0; v < nv; v++) {
          float x = tmp[v * 3], y = tmp[v * 3 + 1], z = tmp[v * 3 + 2];
          vec3 n{inv[0] * x + inv[1] * y + inv[2] * z, inv[3] * x + inv[4] * y + inv[5] * z, inv[6] * x + inv[7] * y + inv[8] * z};
          if (flip) n = -n;  // cofactor matrix = det * inverse-transpose
          m.nrm.push_back(normalize(n));
        }
      } else {
        m.nrm.resize(m.pos.size(), vec3(0, 0, 0));
      }
      if (auv && auv->count == nv) {
        std::vector<float> t2(nv * 2);
        cgltf_accessor_unpack_floats(auv, t2.data(), nv * 2);
        for (size_t v = 0; v < nv; v++) m.uv.push_back({t2[v * 2], t2[v * 2 + 1]});
      } else {
        m.uv.resize(m.pos.size(), vec2(0, 0));
      }
      std::vector<uint32_t> ind;
      if (prim.indices) {
        ind.resize(prim.indices->count);
        for (cgltf_size k = 0; k < prim.indices->count; k++) ind[k] = (uint32_t)cgltf_accessor_read_index(prim.indices, k);
      } else {
        ind.resize(nv);
        for (size_t k = 0; k < nv; k++) ind[k] = (uint32_t)k;
      }
      for (size_t k = 0; k + 2 < ind.size(); k += 3) {
        uint32_t i0 = (uint32_t)base + ind[k], i1 = (uint32_t)base + ind[k + 1], i2 = (uint32_t)base + ind[k + 2];
        if (flip) std::swap(i1, i2);
        m.idx.push_back(i0); m.idx.push_back(i1); m.idx.push_back(i2);
        m.tri_set.push_back(set);
        m.tri_part.push_back(part);
      }
    }
  };

  std::function<void(cgltf_node*)> walk = [&](cgltf_node* n) {
    emit_node(n);
    for (cgltf_size k = 0; k < n->children_count; k++) walk(n->children[k]);
  };
  cgltf_scene* scene = data->scene ? data->scene : (data->scenes_count ? &data->scenes[0] : nullptr);
  if (scene) {
    for (cgltf_size k = 0; k < scene->nodes_count; k++) walk(scene->nodes[k]);
  } else {
    for (cgltf_size k = 0; k < data->nodes_count; k++) if (!data->nodes[k].parent) walk(&data->nodes[k]);
  }
  if (any_missing_uv)
    m.warnings.push_back("some primitives have no UVs (TEXCOORD_0); unwrap in Blender (Smart UV Project) or export with `patina blender export --auto-uv`");
}

// ---------------------------------------------------------------- OBJ
static void load_obj(Mesh& m, const std::string& path, bool& had_normals) {
  std::string text;
  if (!read_file(path, text)) fail("cannot read '%s'", path.c_str());
  std::vector<vec3> P, N;
  std::vector<vec2> T;
  std::unordered_map<uint64_t, uint32_t> vmap;
  uint16_t cur_set = intern(m.set_names, "Default");
  bool used_default = false;
  uint16_t cur_part = intern(m.part_names, path_stem(path));
  had_normals = true;
  const char* p = text.c_str();
  const char* end = p + text.size();
  std::vector<uint32_t> poly;
  while (p < end) {
    const char* line = p;
    while (p < end && *p != '\n') p++;
    std::string ln(line, p - line);
    if (p < end) p++;
    if (!ln.empty() && ln.back() == '\r') ln.pop_back();
    if (ln.size() < 2) continue;
    const char* s = ln.c_str();
    if (s[0] == 'v' && s[1] == ' ') { vec3 v; sscanf(s + 2, "%f %f %f", &v.x, &v.y, &v.z); P.push_back(v); }
    else if (s[0] == 'v' && s[1] == 'n') { vec3 v; sscanf(s + 3, "%f %f %f", &v.x, &v.y, &v.z); N.push_back(v); }
    else if (s[0] == 'v' && s[1] == 't') { vec2 v; sscanf(s + 3, "%f %f", &v.x, &v.y); T.push_back({v.x, 1.0f - v.y}); }
    else if (strncmp(s, "usemtl ", 7) == 0) { cur_set = intern(m.set_names, s + 7); }
    else if ((s[0] == 'o' || s[0] == 'g') && s[1] == ' ') { cur_part = intern(m.part_names, s + 2); }
    else if (s[0] == 'f' && s[1] == ' ') {
      poly.clear();
      const char* q = s + 2;
      while (*q) {
        while (*q == ' ' || *q == '\t') q++;
        if (!*q) break;
        long vi = 0, ti = 0, ni = 0;
        vi = strtol(q, (char**)&q, 10);
        if (*q == '/') { q++; if (*q != '/') ti = strtol(q, (char**)&q, 10); if (*q == '/') { q++; ni = strtol(q, (char**)&q, 10); } }
        while (*q && *q != ' ' && *q != '\t') q++;
        if (vi < 0) vi = (long)P.size() + vi + 1;
        if (ti < 0) ti = (long)T.size() + ti + 1;
        if (ni < 0) ni = (long)N.size() + ni + 1;
        if (vi <= 0 || vi > (long)P.size()) fail("OBJ '%s': bad vertex index", path.c_str());
        uint64_t key = ((uint64_t)vi << 42) ^ ((uint64_t)ti << 21) ^ (uint64_t)ni;
        auto it = vmap.find(key);
        uint32_t id;
        if (it == vmap.end()) {
          id = (uint32_t)m.pos.size();
          m.pos.push_back(P[vi - 1]);
          m.uv.push_back(ti > 0 && ti <= (long)T.size() ? T[ti - 1] : vec2(0, 0));
          if (ni > 0 && ni <= (long)N.size()) m.nrm.push_back(normalize(N[ni - 1]));
          else { m.nrm.push_back(vec3(0, 0, 0)); had_normals = false; }
          vmap.emplace(key, id);
        } else id = it->second;
        poly.push_back(id);
      }
      for (size_t k = 2; k < poly.size(); k++) {
        m.idx.push_back(poly[0]); m.idx.push_back(poly[k - 1]); m.idx.push_back(poly[k]);
        m.tri_set.push_back(cur_set);
        m.tri_part.push_back(cur_part);
        if (cur_set == 0) used_default = true;
      }
    }
  }
  if (T.empty()) m.warnings.push_back("OBJ has no texture coordinates; unwrap it first");
  (void)used_default;
}

// ---------------------------------------------------------------- normals/tangents
void compute_normals_if_missing(Mesh& m, bool had_normals) {
  if (had_normals) {
    bool zero = false;
    for (auto& n : m.nrm) if (length2(n) < 1e-12f) { zero = true; break; }
    if (!zero) return;
  }
  // smooth normals over position-welded vertices, area weighted
  uint32_t wc = 0;
  auto weld = weld_by_position(m, &wc);
  std::vector<vec3> acc(wc, vec3(0, 0, 0));
  for (size_t t = 0; t < m.tri_count(); t++) {
    uint32_t a = m.idx[t * 3], b = m.idx[t * 3 + 1], c = m.idx[t * 3 + 2];
    vec3 fn = cross(m.pos[b] - m.pos[a], m.pos[c] - m.pos[a]);
    acc[weld[a]] += fn; acc[weld[b]] += fn; acc[weld[c]] += fn;
  }
  for (size_t v = 0; v < m.pos.size(); v++)
    if (length2(m.nrm[v]) < 1e-12f) m.nrm[v] = normalize(acc[weld[v]]);
}

// MikkTSpace (the tangent basis Blender, Unreal, Unity and Toolbag use), so baked normal maps match
// engines exactly. Mikk assigns tangents per triangle corner; vertices whose corners disagree are split.
void compute_tangents(Mesh& m) {
  struct Data { Mesh* m; std::vector<vec4> corner; };
  Data d{&m, std::vector<vec4>(m.idx.size(), vec4(1, 0, 0, 1))};
  SMikkTSpaceInterface in = {};
  in.m_getNumFaces = [](const SMikkTSpaceContext* c) { return (int)((Data*)c->m_pUserData)->m->tri_count(); };
  in.m_getNumVerticesOfFace = [](const SMikkTSpaceContext*, const int) { return 3; };
  in.m_getPosition = [](const SMikkTSpaceContext* c, float out[], const int f, const int v) {
    const Mesh& mm = *((Data*)c->m_pUserData)->m;
    vec3 p = mm.pos[mm.idx[f * 3 + v]];
    out[0] = p.x; out[1] = p.y; out[2] = p.z;
  };
  in.m_getNormal = [](const SMikkTSpaceContext* c, float out[], const int f, const int v) {
    const Mesh& mm = *((Data*)c->m_pUserData)->m;
    vec3 n = mm.nrm[mm.idx[f * 3 + v]];
    out[0] = n.x; out[1] = n.y; out[2] = n.z;
  };
  in.m_getTexCoord = [](const SMikkTSpaceContext* c, float out[], const int f, const int v) {
    const Mesh& mm = *((Data*)c->m_pUserData)->m;
    vec2 uv = mm.uv[mm.idx[f * 3 + v]];
    out[0] = uv.x; out[1] = 1.f - uv.y;  // Mikk (like Blender/OpenGL) wants V up
  };
  in.m_setTSpaceBasic = [](const SMikkTSpaceContext* c, const float t[], const float sign, const int f, const int v) {
    ((Data*)c->m_pUserData)->corner[f * 3 + v] = vec4(t[0], t[1], t[2], sign);
  };
  SMikkTSpaceContext ctx = {&in, &d};
  genTangSpaceDefault(&ctx);

  size_t nv = m.pos.size();
  m.tan.assign(nv, vec4(0, 0, 0, 0));
  std::vector<uint8_t> assigned(nv, 0);
  std::unordered_map<uint32_t, std::vector<uint32_t>> splits;  // original vertex -> its copies
  auto same = [](vec4 a, vec4 b) { return a.w == b.w && dot(a.xyz(), b.xyz()) > 0.9999f; };
  for (size_t k = 0; k < m.idx.size(); k++) {
    uint32_t v = m.idx[k];
    vec4 t = d.corner[k];
    if (!assigned[v]) { m.tan[v] = t; assigned[v] = 1; continue; }
    if (same(m.tan[v], t)) continue;
    uint32_t found = UINT32_MAX;
    for (uint32_t c : splits[v]) if (same(m.tan[c], t)) { found = c; break; }
    if (found == UINT32_MAX) {
      found = (uint32_t)m.pos.size();
      m.pos.push_back(m.pos[v]);
      m.nrm.push_back(m.nrm[v]);
      m.uv.push_back(m.uv[v]);
      m.tan.push_back(t);
      splits[v].push_back(found);
    }
    m.idx[k] = found;
  }
}

std::vector<uint32_t> weld_by_position(const Mesh& m, uint32_t* out_count) {
  float q = 1.0f / (m.max_extent() * 1e-5f);
  std::unordered_map<uint64_t, uint32_t> map;
  map.reserve(m.pos.size());
  std::vector<uint32_t> id(m.pos.size());
  uint32_t n = 0;
  for (size_t v = 0; v < m.pos.size(); v++) {
    vec3 p = m.pos[v];
    int64_t x = (int64_t)std::llround(p.x * q), y = (int64_t)std::llround(p.y * q), z = (int64_t)std::llround(p.z * q);
    uint64_t key = (uint64_t)(x * 73856093) ^ (uint64_t)(y * 19349663) ^ (uint64_t)(z * 83492791);
    key ^= (uint64_t)x << 40 ^ (uint64_t)y << 20;
    auto it = map.find(key);
    if (it == map.end()) { map.emplace(key, n); id[v] = n++; }
    else id[v] = it->second;
  }
  if (out_count) *out_count = n;
  return id;
}

// UV islands: triangles connected through edges whose endpoints share both position and UV.
void compute_topology(Mesh& m) {
  size_t nt = m.tri_count();
  float qp = 1.0f / (m.max_extent() * 1e-5f);
  float qu = 1e5f;
  std::unordered_map<uint64_t, uint32_t> map;
  std::vector<uint32_t> vid(m.pos.size());
  uint32_t n = 0;
  for (size_t v = 0; v < m.pos.size(); v++) {
    vec3 p = m.pos[v];
    vec2 u = m.uv[v];
    uint64_t h = 1469598103934665603ull;
    int64_t k[5] = {std::llround(p.x * qp), std::llround(p.y * qp), std::llround(p.z * qp), std::llround(u.x * qu), std::llround(u.y * qu)};
    h = fnv1a(k, sizeof k, h);
    auto it = map.find(h);
    if (it == map.end()) { map.emplace(h, n); vid[v] = n++; }
    else vid[v] = it->second;
  }
  std::vector<int> parent(nt);
  for (size_t t = 0; t < nt; t++) parent[t] = (int)t;
  std::function<int(int)> find = [&](int x) {
    while (parent[x] != x) { parent[x] = parent[parent[x]]; x = parent[x]; }
    return x;
  };
  std::unordered_map<uint64_t, uint32_t> edges;
  edges.reserve(nt * 3);
  for (size_t t = 0; t < nt; t++) {
    for (int e = 0; e < 3; e++) {
      uint32_t a = vid[m.idx[t * 3 + e]], b = vid[m.idx[t * 3 + (e + 1) % 3]];
      if (a > b) std::swap(a, b);
      uint64_t key = ((uint64_t)a << 32) | b;
      auto it = edges.find(key);
      if (it == edges.end()) edges.emplace(key, (uint32_t)t);
      else {
        int ra = find((int)t), rb = find((int)it->second);
        if (ra != rb) parent[ra] = rb;
      }
    }
  }
  m.tri_island.assign(nt, 0);
  std::unordered_map<int, int> remap;
  for (size_t t = 0; t < nt; t++) {
    int r = find((int)t);
    auto it = remap.find(r);
    if (it == remap.end()) { int id = (int)remap.size(); remap.emplace(r, id); m.tri_island[t] = id; }
    else m.tri_island[t] = it->second;
  }
  m.island_count = (int)remap.size();
}

Mesh load_mesh(const std::string& path, bool geometry_only) {
  if (!file_exists(path)) fail("mesh file not found: '%s'", path.c_str());
  Mesh m;
  m.path = path_abs(path);
  std::string ext = path_ext(path);
  bool had_normals = true;
  if (ext == ".glb" || ext == ".gltf") load_gltf(m, path, had_normals);
  else if (ext == ".obj") load_obj(m, path, had_normals);
  else fail("unsupported mesh format '%s' (use .glb, .gltf or .obj; from Blender: File > Export > glTF 2.0)", ext.c_str());
  if (m.idx.empty()) fail("mesh '%s' has no triangles", path.c_str());
  std::string bytes;
  read_file(path, bytes);
  m.content_hash = fnv1a(bytes);
  compute_bounds(m);
  compute_normals_if_missing(m, had_normals);
  if (geometry_only) return m;
  compute_tangents(m);
  compute_topology(m);
  return m;
}

// ---------------------------------------------------------------- inspection
Json mesh_info(const Mesh& m) {
  Json j = Json::object();
  j.set("path", m.path);
  j.set("triangles", (int64_t)m.tri_count());
  j.set("vertices", (int64_t)m.pos.size());
  auto v3 = [](vec3 v) { return Json::array({Json(v.x), Json(v.y), Json(v.z)}); };
  Json bb = Json::object();
  bb.set("min", v3(m.bmin));
  bb.set("max", v3(m.bmax));
  bb.set("size", v3(m.size()));
  bb.set("center", v3(m.center()));
  j.set("bounds", bb);
  j.set("up_axis", "+Y (glTF). Blender +Z maps to +Y; Blender front (-Y) maps to +Z");
  j.set("uv_islands", m.island_count);
  // topology: thin "sliver" triangles (smallest angle under 10 degrees) shade and stream badly in games
  {
    size_t slivers = 0, nt = m.tri_count();
    std::vector<float> mins;
    mins.reserve(nt);
    for (size_t t = 0; t < nt; t++) {
      vec3 p[3] = {m.pos[m.idx[t * 3]], m.pos[m.idx[t * 3 + 1]], m.pos[m.idx[t * 3 + 2]]};
      float mn = 180.f;
      for (int k = 0; k < 3; k++) {
        vec3 a = p[(k + 1) % 3] - p[k], b = p[(k + 2) % 3] - p[k];
        float la = length(a), lb = length(b);
        if (la < 1e-12f || lb < 1e-12f) { mn = 0; break; }
        mn = std::fmin(mn, std::acos(clampf(dot(a, b) / (la * lb), -1.f, 1.f)) * 180.f / kPi);
      }
      mins.push_back(mn);
      if (mn < 10.f) slivers++;
    }
    Json tj = Json::object();
    tj.set("sliver_triangles", (int64_t)slivers);
    tj.set("sliver_fraction", nt ? (double)slivers / nt : 0.0);
    if (!mins.empty()) {
      std::sort(mins.begin(), mins.end());
      tj.set("min_angle_p5_deg", mins[mins.size() / 20]);
      tj.set("min_angle_median_deg", mins[mins.size() / 2]);
    }
    j.set("topology", tj);
  }

  // per texture set: triangle count, uv area, bounds, 3D surface area, parts
  Json sets = Json::object();
  for (size_t s = 0; s < m.set_names.size(); s++) {
    double uv_area = 0, surf = 0;
    vec2 ulo{1e9f, 1e9f}, uhi{-1e9f, -1e9f};
    int tris = 0;
    std::vector<int> parts;
    int flipped = 0;
    for (size_t t = 0; t < m.tri_count(); t++) {
      if (m.tri_set[t] != s) continue;
      tris++;
      vec2 a = m.uv[m.idx[t * 3]], b = m.uv[m.idx[t * 3 + 1]], c = m.uv[m.idx[t * 3 + 2]];
      double ar = 0.5 * ((double)(b.x - a.x) * (c.y - a.y) - (double)(c.x - a.x) * (b.y - a.y));
      if (ar > 0) flipped++;  // with V down, counter-clockwise 3D winding gives negative area
      uv_area += std::fabs(ar);
      for (auto u : {a, b, c}) { ulo.x = std::fmin(ulo.x, u.x); ulo.y = std::fmin(ulo.y, u.y); uhi.x = std::fmax(uhi.x, u.x); uhi.y = std::fmax(uhi.y, u.y); }
      vec3 pa = m.pos[m.idx[t * 3]], pb = m.pos[m.idx[t * 3 + 1]], pc = m.pos[m.idx[t * 3 + 2]];
      surf += 0.5 * length(cross(pb - pa, pc - pa));
      int part = m.tri_part[t];
      if (std::find(parts.begin(), parts.end(), part) == parts.end()) parts.push_back(part);
    }
    Json sj = Json::object();
    sj.set("triangles", tris);
    sj.set("uv_area", uv_area);
    sj.set("surface_area", surf);
    sj.set("uv_bounds", Json::array({Json(ulo.x), Json(ulo.y), Json(uhi.x), Json(uhi.y)}));
    // texel density at 2048: texels per world unit
    if (surf > 0) sj.set("texels_per_unit_at_2k", 2048.0 * std::sqrt(uv_area / surf));
    Json pj = Json::array();
    for (int p : parts) pj.push(m.part_names[p]);
    sj.set("parts", pj);
    sj.set("mirrored_triangles", flipped);
    // UV overlap: rasterize this set's UV triangles (texel centres strictly inside) and count texels
    // covered more than once; texel density spread: area-weighted p5..p95 of per-triangle density
    // relative to the set's average (1.0 = perfectly uniform)
    const int R = 512;
    std::vector<uint8_t> cover((size_t)R * R, 0);
    std::vector<std::pair<float, float>> dens;  // (relative density, 3D area)
    double avg = surf > 0 ? std::sqrt(uv_area / surf) : 0;
    for (size_t t = 0; t < m.tri_count(); t++) {
      if (m.tri_set[t] != s) continue;
      vec2 a = m.uv[m.idx[t * 3]] * (float)R, b = m.uv[m.idx[t * 3 + 1]] * (float)R, c = m.uv[m.idx[t * 3 + 2]] * (float)R;
      float d = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
      vec3 pa = m.pos[m.idx[t * 3]], pb = m.pos[m.idx[t * 3 + 1]], pc = m.pos[m.idx[t * 3 + 2]];
      float a3 = 0.5f * length(cross(pb - pa, pc - pa));
      if (a3 > 1e-12f && avg > 0) dens.push_back({(float)(std::sqrt(std::fabs(d) * 0.5 / ((double)R * R) / a3) / avg), a3});
      if (std::fabs(d) < 1e-9f) continue;
      int x0 = std::max(0, (int)std::floor(std::fmin(a.x, std::fmin(b.x, c.x)))), x1 = std::min(R - 1, (int)std::ceil(std::fmax(a.x, std::fmax(b.x, c.x))));
      int y0 = std::max(0, (int)std::floor(std::fmin(a.y, std::fmin(b.y, c.y)))), y1 = std::min(R - 1, (int)std::ceil(std::fmax(a.y, std::fmax(b.y, c.y))));
      for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
          float px = x + 0.5f, py = y + 0.5f;
          float w0 = ((b.x - px) * (c.y - py) - (b.y - py) * (c.x - px)) / d;
          float w1 = ((c.x - px) * (a.y - py) - (c.y - py) * (a.x - px)) / d;
          float w2 = 1.f - w0 - w1;
          const float e = 1e-4f;
          if (w0 > e && w1 > e && w2 > e) { uint8_t& cv = cover[(size_t)y * R + x]; if (cv < 255) cv++; }
        }
    }
    size_t covered = 0, over = 0;
    for (uint8_t cv : cover) { covered += cv > 0; over += cv > 1; }
    double overlap = covered ? (double)over / covered : 0.0;
    sj.set("uv_overlap_fraction", overlap);
    if (!dens.empty()) {
      std::sort(dens.begin(), dens.end());
      double tot = 0;
      for (auto& d : dens) tot += d.second;
      auto pct = [&](double q) { double acc = 0; for (auto& d : dens) { acc += d.second; if (acc >= q * tot) return (double)d.first; } return (double)dens.back().first; };
      double p5 = pct(0.05), p95 = pct(0.95);
      sj.set("texel_density_p5_p95", Json::array({Json(p5), Json(p95)}));
      sj.set("texel_density_spread", p5 > 1e-9 ? p95 / p5 : 1e9);
    }
    Json warn = Json::array();
    if (overlap > 0.001) warn.push(strf("%.1f%% of covered texels are shared by overlapping UV triangles", overlap * 100.0));
    if (ulo.x < -0.001f || ulo.y < -0.001f || uhi.x > 1.001f || uhi.y > 1.001f)
      warn.push("UVs extend outside 0..1 (UDIMs/tiling are not supported yet); texels outside the tile are ignored");
    if (uv_area > 1.02) warn.push("total UV area > 1: UV islands overlap (mirrored/stacked UVs share texels)");
    if (sj.has("texel_density_spread") && sj["texel_density_spread"].as_num() > 2.0)
      warn.push(strf("uneven texel density: p95/p5 = %.1f (islands scaled differently or stretched)", sj["texel_density_spread"].as_num()));
    if (uv_area < 0.2) warn.push("UV area < 20% of the texture: low texel usage, consider re-packing");
    sj.set("warnings", warn);
    sets.set(m.set_names[s], sj);
  }
  j.set("texture_sets", sets);

  Json parts = Json::object();
  for (size_t p = 0; p < m.part_names.size(); p++) {
    vec3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
    int tris = 0;
    for (size_t t = 0; t < m.tri_count(); t++) {
      if (m.tri_part[t] != p) continue;
      tris++;
      for (int k = 0; k < 3; k++) { lo = vmin(lo, m.pos[m.idx[t * 3 + k]]); hi = vmax(hi, m.pos[m.idx[t * 3 + k]]); }
    }
    Json pj = Json::object();
    pj.set("triangles", tris);
    // bounds in normalized bbox coordinates (what layer parameters use by default)
    vec3 sz = m.size();
    auto nb = [&](vec3 v) {
      return Json::array({Json(sz.x > 0 ? (v.x - m.bmin.x) / sz.x : 0), Json(sz.y > 0 ? (v.y - m.bmin.y) / sz.y : 0), Json(sz.z > 0 ? (v.z - m.bmin.z) / sz.z : 0)});
    };
    pj.set("bbox_min", nb(lo));
    pj.set("bbox_max", nb(hi));
    parts.set(m.part_names[p], pj);
  }
  j.set("parts", parts);
  Json w = Json::array();
  for (auto& s : m.warnings) w.push(s);
  if (j["topology"]["sliver_fraction"].as_num() > 0.05)
    w.push(strf("%.0f%% of triangles are slivers (smallest angle under 10 degrees): fan-filled n-gons or long thin strips; "
                "re-topologize large flat faces into quads", j["topology"]["sliver_fraction"].as_num() * 100.0));
  j.set("warnings", w);
  return j;
}

}  // namespace pt
