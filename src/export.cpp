#include "export.h"

#include <algorithm>
#include <mutex>

#include "image.h"
#include "library.h"
#include "usd.h"

namespace pt {

namespace {
// Value of a named source at texel t (for export packing).
struct Sources {
  const SetMaps* m;
  bool directx;
  bool get(const std::string& name, size_t t, float* out, int comps_wanted) const {
    auto one = [&](float v) { out[0] = v; return true; };
    if (name == "one") return one(1.f);
    if (name == "zero") return one(0.f);
    if (name == "metallic") return one(m->ch[C_METALLIC][t]);
    if (name == "roughness") return one(m->ch[C_ROUGHNESS][t]);
    if (name == "smoothness") return one(1.f - m->ch[C_ROUGHNESS][t]);
    if (name == "ao") return one(m->ch[C_AO][t]);
    if (name == "opacity") return one(m->ch[C_OPACITY][t]);
    if (name == "height") return one(saturate(0.5f + 0.5f * m->ch[C_HEIGHT][t]));
    if (name == "bake_ao" || name == "curvature" || name == "thickness") {
      auto it = m->extra.find(name);
      return one(it == m->extra.end() ? 0.f : it->second[t]);
    }
    if (name == "basecolor" || name == "emissive") {
      const float* c = &m->ch[name == "basecolor" ? C_BASECOLOR : C_EMISSIVE][t * 3];
      if (comps_wanted == 1) return one(0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2]);
      out[0] = c[0]; out[1] = c[1]; out[2] = c[2];
      return true;
    }
    if (name == "normal") {
      const float* n = &m->ch[C_NORMAL][t * 3];
      out[0] = n[0] * 0.5f + 0.5f;
      out[1] = (directx ? -n[1] : n[1]) * 0.5f + 0.5f;
      out[2] = n[2] * 0.5f + 0.5f;
      return true;
    }
    return false;
  }
};

bool channel_used(const SetMaps& m, const std::string& ch) {
  int c = channel_index(ch);
  return c >= 0 && m.used[c];
}
}  // namespace

namespace {
// What a preset map holds for one texture set: a color in rgb (with an optional alpha source), or one source per channel.
struct MapLayout {
  std::string rgb;
  std::string src[4];
  int comps = 1;
  // The source written to channel k when there is no rgb color. A channel the preset leaves out repeats the
  // first, and a missing alpha is one.
  std::string source(int k) const { return src[k].empty() ? (k == 3 ? std::string("one") : src[0]) : src[k]; }
};

const char* const kChannelNames[4] = {"r", "g", "b", "a"};

MapLayout map_layout(const Json& spec, const SetMaps& m) {
  MapLayout l;
  l.rgb = spec.str("rgb", "");
  for (int k = 0; k < 4; k++) l.src[k] = spec.str(kChannelNames[k], "");
  if (!spec.str("alpha_if_used", "").empty() && !channel_used(m, spec.str("alpha_if_used"))) l.src[3].clear();
  if (!l.rgb.empty()) l.comps = l.src[3].empty() ? 3 : 4;
  else if (!l.src[3].empty()) l.comps = 4;
  else if (!l.src[2].empty() || !l.src[1].empty()) l.comps = 3;
  else l.comps = 1;
  return l;
}

// A preset map that was written for a texture set, and the file it went to.
struct WrittenMap {
  const Json* spec;
  std::string file;
};

// The two packings a USD material can only read by remapping a texture's values, which not every importer does.
struct UsdRemaps {
  bool green_flipped = false;       // DirectX normals
  bool smoothness_inverted = false;  // smoothness where USD wants roughness
};

// The USD material for one texture set: each UsdPreviewSurface input, found in the maps the preset wrote. It
// looks where write_map put things (both use map_layout), so it follows any packing: ORM, a Unity mask map
// with smoothness in alpha, separate files. An input the preset does not write is left at USD's default.
UsdMaterial usd_material(const SetMaps& m, const std::vector<WrittenMap>& written, bool directx, UsdRemaps& remaps) {
  UsdMaterial mat;
  mat.set = m.set;
  mat.name = m.name;
  auto image_of = [&](const WrittenMap& w) {
    for (size_t i = 0; i < mat.images.size(); i++)
      if (mat.images[i].file == w.file) return (int)i;
    UsdImage image;
    image.key = w.spec->str("key", path_stem(w.file));
    image.file = w.file;
    image.srgb = w.spec->boolean("srgb", false);
    mat.images.push_back(image);
    return (int)mat.images.size() - 1;
  };
  auto find_color = [&](const std::string& source) -> UsdInput {
    for (const WrittenMap& w : written)
      if (map_layout(*w.spec, m).rgb == source) return {image_of(w), "rgb"};
    return {};
  };
  // `channel` gets the index of the channel in the image, for a remap of just that channel
  auto find_channel = [&](const std::string& source, int* channel = nullptr) -> UsdInput {
    for (const WrittenMap& w : written) {
      MapLayout l = map_layout(*w.spec, m);
      for (int k = l.rgb.empty() ? 0 : 3; k < l.comps; k++) {
        if (l.source(k) != source) continue;
        if (channel) *channel = k;
        return {image_of(w), kChannelNames[k]};
      }
    }
    return {};
  };

  mat.basecolor = find_color("basecolor");
  mat.metallic = find_channel("metallic");
  mat.occlusion = find_channel("ao");
  mat.roughness = find_channel("roughness");
  if (mat.roughness.image < 0) {
    int k = 0;
    mat.roughness = find_channel("smoothness", &k);
    if (mat.roughness.image >= 0) {  // roughness = 1 - smoothness
      mat.images[mat.roughness.image].scale[k] = -1.f;
      mat.images[mat.roughness.image].bias[k] = 1.f;
      remaps.smoothness_inverted = true;
    }
  }
  mat.normal = find_color("normal");
  if (mat.normal.image >= 0) {
    // a stored 0..1 becomes the -1..1 vector, with green flipped when the preset stores DirectX normals
    UsdImage& image = mat.images[mat.normal.image];
    float sign[3] = {1.f, directx ? -1.f : 1.f, 1.f};
    for (int k = 0; k < 3; k++) {
      image.scale[k] = 2.f * sign[k];
      image.bias[k] = -sign[k];
    }
    remaps.green_flipped = remaps.green_flipped || directx;
  }
  if (m.used[C_EMISSIVE]) mat.emissive = find_color("emissive");
  if (m.used[C_OPACITY]) mat.opacity = find_channel("opacity");
  return mat;
}
}  // namespace

static void write_map(const Json& spec, const SetMaps& m, bool directx, const std::string& path) {
  MapLayout layout = map_layout(spec, m);
  const std::string& rgb = layout.rgb;
  const std::string* src = layout.src;
  int comps = layout.comps;
  size_t npx = (size_t)m.res * m.res;
  std::vector<float> img(npx * comps);
  Sources S{&m, directx};
  parallel_for((int64_t)npx, [&](int64_t b, int64_t e) {
    float tmp[3];
    for (int64_t t = b; t < e; t++) {
      float* o = &img[t * comps];
      if (!rgb.empty()) {
        S.get(rgb, t, tmp, 3);
        o[0] = tmp[0]; o[1] = tmp[1]; o[2] = tmp[2];
        if (comps == 4) { S.get(src[3], t, tmp, 1); o[3] = tmp[0]; }
      } else {
        for (int k = 0; k < comps; k++) {
          S.get(layout.source(k), t, tmp, 1);
          o[k] = tmp[0];
        }
      }
    }
  });
  save_png(path, m.res, m.res, comps, img.data(), spec.boolean("srgb", false), spec.integer("bits", 8));
}

Json export_textures(const Project& p, const Baked& bk, const std::vector<SetResult>& results, const std::string& preset_name,
                     const std::string& out_dir, bool write_glb, const std::string& usd_format) {
  const Json* preset = find_export_preset(preset_name);
  if (!preset) {
    std::string dym = did_you_mean(preset_name, builtin_library()["export_presets"].keys());
    fail("unknown export preset '%s'%s (blender, gltf, unreal, unity_hdrp, unity_urp, godot, maps)", preset_name.c_str(),
         dym.empty() ? "" : (" - did you mean '" + dym + "'?").c_str());
  }
  Timer tm;
  make_dirs(out_dir);
  bool directx = preset->str("normal", "opengl") == "directx";
  const Mesh& mesh = *bk.mesh;
  float ext = mesh.max_extent();

  std::vector<SetMaps> maps(results.size());
  std::vector<std::string> extra = {"bake_ao", "curvature", "thickness"};
  for (size_t i = 0; i < results.size(); i++) maps[i] = make_maps(bk.sets[results[i].set], results[i], extra);

  Json manifest = Json::object();
  manifest.set("patina", 1);
  manifest.set("preset", preset_name);
  manifest.set("mesh", p.mesh_path());
  Json sets = Json::object();
  Json files = Json::array();
  struct Task { const Json* spec; const SetMaps* m; std::string path; };
  std::vector<Task> tasks;
  std::vector<UsdMaterial> usd_materials;
  UsdRemaps usd_remaps;
  for (auto& m : maps) {
    Json sj = Json::object();
    sj.set("material", m.name);
    Json fj = Json::object();
    std::vector<WrittenMap> written;
    for (auto& spec : (*preset)["maps"].items()) {
      std::string if_used = spec.str("if_used", "");
      if (!if_used.empty() && !channel_used(m, if_used)) continue;
      std::string fname = spec.str("file", "{set}.png");
      size_t pos = fname.find("{set}");
      if (pos != std::string::npos) fname.replace(pos, 5, sanitize_filename(m.name));
      tasks.push_back({&spec, &m, path_join(out_dir, fname)});
      written.push_back({&spec, fname});
      fj.set(spec.str("key", fname), fname);
      files.push(path_join(out_dir, fname));
    }
    if (!usd_format.empty()) usd_materials.push_back(usd_material(m, written, directx, usd_remaps));
    sj.set("files", fj);
    sj.set("normal_format", directx ? "directx" : "opengl");
    sj.set("height_depth", p.height_depth() * ext);
    sj.set("resolution", m.res);
    sets.set(m.name, sj);
  }
  manifest.set("texture_sets", sets);

  // write all files concurrently (each PNG encode is itself parallel)
  std::mutex err_m;
  std::string first_err;
  parallel_for((int64_t)tasks.size(), 1, [&](int64_t b, int64_t e) {
    for (int64_t i = b; i < e; i++) {
      try {
        write_map(*tasks[i].spec, *tasks[i].m, directx, tasks[i].path);
      } catch (const std::exception& ex) {
        std::lock_guard<std::mutex> lk(err_m);
        if (first_err.empty()) first_err = ex.what();
      }
    }
  });
  if (!first_err.empty()) fail("%s", first_err.c_str());

  std::string glb_path;
  if (write_glb) {
    glb_path = path_join(out_dir, sanitize_filename(p.name()) + ".glb");
    write_textured_glb(glb_path, mesh, maps, p.height_depth() * ext);
    manifest.set("glb", glb_path);
    files.push(glb_path);
  }
  Json usd;
  if (!usd_format.empty()) {
    // after the images: the materials read them from this folder, and a .usdz copies them in
    std::string usd_path = path_join(out_dir, sanitize_filename(p.name()) + "." + usd_format);
    usd = write_usd(usd_path, usd_format, p.name(), mesh, usd_materials);
    usd.set("path", usd_path);
    // Both remaps are what the USD spec provides for this and render correctly where it is followed. Checked
    // against Blender 5.2's importer, which applies the first and ignores the second.
    Json usd_warnings = Json::array();
    const char* advice = "Export with a preset that stores OpenGL normals and roughness (gltf, godot, blender) when the USD file is the deliverable.";
    if (usd_remaps.green_flipped)
      usd_warnings.push(strf("the '%s' preset stores DirectX normals, so the USD materials flip green with the texture's scale and "
                             "bias. OpenUSD's usdchecker reports that as non-standard and an importer may ignore it. %s",
                             preset_name.c_str(), advice));
    if (usd_remaps.smoothness_inverted)
      usd_warnings.push(strf("the '%s' preset stores smoothness, so the USD materials read roughness as 1 - smoothness with the texture's "
                             "scale and bias. Some importers ignore that (Blender's does) and show the wrong roughness. %s",
                             preset_name.c_str(), advice));
    if (usd_warnings.size()) usd.set("warnings", usd_warnings);
    manifest.set("usd", usd_path);
    files.push(usd_path);
  }
  std::string mpath = path_join(out_dir, "manifest.json");
  write_file_or_throw(mpath, manifest.dump(2) + "\n");

  Json r = Json::object();
  r.set("out_dir", path_abs(out_dir));
  r.set("preset", preset_name);
  r.set("manifest", mpath);
  r.set("files", files);
  if (!usd.is_null()) r.set("usd", usd);
  r.set("export_ms", tm.ms());
  return r;
}

// ---------------------------------------------------------------- GLB
namespace {
struct GlbBuilder {
  std::string bin;
  Json buffer_views = Json::array();
  Json accessors = Json::array();
  int add_view(const void* data, size_t n, int target) {
    while (bin.size() % 4) bin += '\0';
    Json v = Json::object();
    v.set("buffer", 0);
    v.set("byteOffset", (int64_t)bin.size());
    v.set("byteLength", (int64_t)n);
    if (target) v.set("target", target);
    bin.append((const char*)data, n);
    buffer_views.push(v);
    return (int)buffer_views.size() - 1;
  }
  int add_accessor(int view, int comp_type, size_t count, const char* type, const Json& mn = Json(), const Json& mx = Json()) {
    Json a = Json::object();
    a.set("bufferView", view);
    a.set("componentType", comp_type);
    a.set("count", (int64_t)count);
    a.set("type", type);
    if (!mn.is_null()) { a.set("min", mn); a.set("max", mx); }
    accessors.push(a);
    return (int)accessors.size() - 1;
  }
};
}  // namespace

void write_textured_glb(const std::string& path, const Mesh& m, const std::vector<SetMaps>& maps, float) {
  GlbBuilder g;
  Json images = Json::array(), textures = Json::array(), materials = Json::array(), meshes = Json::array(), nodes = Json::array();
  Json samplers = Json::array();
  {
    Json s = Json::object();
    s.set("magFilter", 9729);
    s.set("minFilter", 9987);
    s.set("wrapS", 10497);
    s.set("wrapT", 10497);
    samplers.push(s);
  }
  // encode textures per set (in parallel)
  struct Tex { std::string base, orm, normal, emissive; bool alpha = false; };
  std::vector<Tex> tex(maps.size());
  parallel_for((int64_t)maps.size(), 1, [&](int64_t b, int64_t e) {
    for (int64_t i = b; i < e; i++) {
      const SetMaps& sm = maps[i];
      int res = sm.res;
      size_t npx = (size_t)res * res;
      bool alpha = sm.used[C_OPACITY];
      tex[i].alpha = alpha;
      std::vector<uint8_t> rgb(npx * 3), rgba(alpha ? npx * 4 : 0);
      auto enc = [&](auto fn) {
        parallel_for((int64_t)npx, [&](int64_t b2, int64_t e2) { for (int64_t t = b2; t < e2; t++) fn(t); });
      };
      if (alpha) {
        enc([&](int64_t t) {
          for (int k = 0; k < 3; k++) rgba[t * 4 + k] = (uint8_t)std::lround(linear_to_srgb(sm.ch[C_BASECOLOR][t * 3 + k]) * 255.f);
          rgba[t * 4 + 3] = (uint8_t)std::lround(saturate(sm.ch[C_OPACITY][t]) * 255.f);
        });
        tex[i].base = encode_png8(res, res, 4, rgba.data());
      } else {
        enc([&](int64_t t) { for (int k = 0; k < 3; k++) rgb[t * 3 + k] = (uint8_t)std::lround(linear_to_srgb(sm.ch[C_BASECOLOR][t * 3 + k]) * 255.f); });
        tex[i].base = encode_png_rgb8(res, res, rgb.data());
      }
      enc([&](int64_t t) {
        rgb[t * 3] = (uint8_t)std::lround(saturate(sm.ch[C_AO][t]) * 255.f);
        rgb[t * 3 + 1] = (uint8_t)std::lround(saturate(sm.ch[C_ROUGHNESS][t]) * 255.f);
        rgb[t * 3 + 2] = (uint8_t)std::lround(saturate(sm.ch[C_METALLIC][t]) * 255.f);
      });
      tex[i].orm = encode_png_rgb8(res, res, rgb.data());
      enc([&](int64_t t) { for (int k = 0; k < 3; k++) rgb[t * 3 + k] = (uint8_t)std::lround(saturate(sm.ch[C_NORMAL][t * 3 + k] * 0.5f + 0.5f) * 255.f); });
      tex[i].normal = encode_png_rgb8(res, res, rgb.data());
      if (sm.used[C_EMISSIVE]) {
        enc([&](int64_t t) { for (int k = 0; k < 3; k++) rgb[t * 3 + k] = (uint8_t)std::lround(linear_to_srgb(sm.ch[C_EMISSIVE][t * 3 + k]) * 255.f); });
        tex[i].emissive = encode_png_rgb8(res, res, rgb.data());
      }
    }
  });

  auto add_image = [&](const std::string& png) {
    int view = g.add_view(png.data(), png.size(), 0);
    Json im = Json::object();
    im.set("bufferView", view);
    im.set("mimeType", "image/png");
    images.push(im);
    Json t = Json::object();
    t.set("sampler", 0);
    t.set("source", (int)images.size() - 1);
    textures.push(t);
    return (int)textures.size() - 1;
  };
  std::vector<int> set_material(m.set_names.size(), -1);
  for (size_t i = 0; i < maps.size(); i++) {
    Json mat = Json::object();
    mat.set("name", maps[i].name);
    Json pbr = Json::object();
    Json bt = Json::object();
    bt.set("index", add_image(tex[i].base));
    pbr.set("baseColorTexture", bt);
    int orm = add_image(tex[i].orm);
    Json mr = Json::object();
    mr.set("index", orm);
    pbr.set("metallicRoughnessTexture", mr);
    pbr.set("metallicFactor", 1.0);
    pbr.set("roughnessFactor", 1.0);
    mat.set("pbrMetallicRoughness", pbr);
    Json nt = Json::object();
    nt.set("index", add_image(tex[i].normal));
    mat.set("normalTexture", nt);
    Json ot = Json::object();
    ot.set("index", orm);
    mat.set("occlusionTexture", ot);
    if (!tex[i].emissive.empty()) {
      Json et = Json::object();
      et.set("index", add_image(tex[i].emissive));
      mat.set("emissiveTexture", et);
      mat.set("emissiveFactor", Json::array({Json(1), Json(1), Json(1)}));
    }
    if (tex[i].alpha) mat.set("alphaMode", "BLEND");
    materials.push(mat);
    set_material[maps[i].set] = (int)materials.size() - 1;
  }

  // one node per part, one primitive per texture set
  for (size_t part = 0; part < m.part_names.size(); part++) {
    Json prims = Json::array();
    for (size_t set = 0; set < m.set_names.size(); set++) {
      std::vector<uint32_t> remap(m.pos.size(), UINT32_MAX);
      std::vector<float> P, N, UV, T;
      std::vector<uint32_t> I;
      vec3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
      for (size_t t = 0; t < m.tri_count(); t++) {
        if (m.tri_part[t] != part || m.tri_set[t] != set) continue;
        for (int k = 0; k < 3; k++) {
          uint32_t v = m.idx[t * 3 + k];
          if (remap[v] == UINT32_MAX) {
            remap[v] = (uint32_t)(P.size() / 3);
            P.insert(P.end(), {m.pos[v].x, m.pos[v].y, m.pos[v].z});
            N.insert(N.end(), {m.nrm[v].x, m.nrm[v].y, m.nrm[v].z});
            UV.insert(UV.end(), {m.uv[v].x, m.uv[v].y});
            T.insert(T.end(), {m.tan[v].x, m.tan[v].y, m.tan[v].z, m.tan[v].w});
            lo = vmin(lo, m.pos[v]);
            hi = vmax(hi, m.pos[v]);
          }
          I.push_back(remap[v]);
        }
      }
      if (I.empty()) continue;
      size_t nv = P.size() / 3;
      Json attrs = Json::object();
      attrs.set("POSITION", g.add_accessor(g.add_view(P.data(), P.size() * 4, 34962), 5126, nv, "VEC3",
                                           Json::array({Json(lo.x), Json(lo.y), Json(lo.z)}), Json::array({Json(hi.x), Json(hi.y), Json(hi.z)})));
      attrs.set("NORMAL", g.add_accessor(g.add_view(N.data(), N.size() * 4, 34962), 5126, nv, "VEC3"));
      attrs.set("TANGENT", g.add_accessor(g.add_view(T.data(), T.size() * 4, 34962), 5126, nv, "VEC4"));
      attrs.set("TEXCOORD_0", g.add_accessor(g.add_view(UV.data(), UV.size() * 4, 34962), 5126, nv, "VEC2"));
      Json prim = Json::object();
      prim.set("attributes", attrs);
      prim.set("indices", g.add_accessor(g.add_view(I.data(), I.size() * 4, 34963), 5125, I.size(), "SCALAR"));
      if (set_material[set] >= 0) prim.set("material", set_material[set]);
      prims.push(prim);
    }
    if (prims.size() == 0) continue;
    Json mesh = Json::object();
    mesh.set("name", m.part_names[part]);
    mesh.set("primitives", prims);
    meshes.push(mesh);
    Json node = Json::object();
    node.set("name", m.part_names[part]);
    node.set("mesh", (int)meshes.size() - 1);
    nodes.push(node);
  }

  Json doc = Json::object();
  Json asset = Json::object();
  asset.set("version", "2.0");
  asset.set("generator", "patina");
  doc.set("asset", asset);
  doc.set("scene", 0);
  Json scene = Json::object();
  Json sn = Json::array();
  for (size_t i = 0; i < nodes.size(); i++) sn.push((int)i);
  scene.set("nodes", sn);
  doc.set("scenes", Json::array({scene}));
  doc.set("nodes", nodes);
  doc.set("meshes", meshes);
  doc.set("materials", materials);
  doc.set("samplers", samplers);
  doc.set("textures", textures);
  doc.set("images", images);
  doc.set("accessors", g.accessors);
  doc.set("bufferViews", g.buffer_views);
  while (g.bin.size() % 4) g.bin += '\0';
  Json buf = Json::object();
  buf.set("byteLength", (int64_t)g.bin.size());
  doc.set("buffers", Json::array({buf}));
  std::string js = doc.dump();
  while (js.size() % 4) js += ' ';

  std::string out;
  auto u32 = [&](uint32_t v) { out.append((const char*)&v, 4); };
  u32(0x46546C67u);  // "glTF"
  u32(2);
  u32((uint32_t)(12 + 8 + js.size() + 8 + g.bin.size()));
  u32((uint32_t)js.size());
  u32(0x4E4F534Au);  // JSON
  out += js;
  u32((uint32_t)g.bin.size());
  u32(0x004E4942u);  // BIN
  out += g.bin;
  write_file_or_throw(path, out);
}

}  // namespace pt
