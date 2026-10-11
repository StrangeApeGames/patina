// USD export through LightUSD. This is the one file that includes the library.
//
// The stage is built from LightUSD's typed prims and written by its own writers: the text writer for .usda and
// the crate writer for .usdc and for the root layer of a .usdz. The two writers do not read a stage the same way,
// and the crate writer is marked experimental upstream, so each difference is handled where it comes up below
// and every file is parsed again before it is written to disk (see check_read_back).
#include "usd.h"

#include <map>
#include <mutex>
#include <unordered_set>

#include "commands.h"

#include "tinyusdz.hh"
#include "core/model-scope.hh"  // Scope
#include "usdc-writer.hh"

namespace pt {

namespace usd = tinyusdz;

std::vector<std::string> usd_formats() { return {"usda", "usdc", "usdz"}; }
bool usd_available() { return true; }

namespace {

// LightUSD cannot be used from two threads at once: its readers time themselves in one profiler that every
// call shares, with no lock, and upstream promises nothing for the rest. Commands run concurrently (batch, MCP
// calls), so one USD file at a time is built, written and read back. The rest of an export stays parallel.
std::mutex g_lightusd;

const char* kMaterialScope = "Materials";
const char* kSurfaceNode = "PreviewSurface";
const char* kUvReaderNode = "PrimvarReader_st";
const char* kUvPrimvar = "st";
const char* kMaterialFamily = "materialBind";  // the GeomSubset family USD binds materials through

// What was put into the stage, and what is counted again when the written file is read back.
struct Counts {
  int meshes = 0;
  int64_t triangles = 0;
  int materials = 0;  // with a surface shader connected
  int textures = 0;   // texture nodes that name a file
  int bindings = 0;   // meshes and subsets with a material bound
  bool operator==(const Counts& o) const {
    return meshes == o.meshes && triangles == o.triangles && materials == o.materials && textures == o.textures && bindings == o.bindings;
  }
};

// A USD prim name is letters, digits and underscores and does not start with a digit. A name already taken
// under the same parent gets a number.
std::string prim_name(const std::string& wanted, std::unordered_set<std::string>& taken) {
  std::string name;
  for (char c : wanted) {
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
    name += ok ? c : '_';
  }
  if (name.empty() || (name[0] >= '0' && name[0] <= '9')) name = "_" + name;
  std::string unique = name;
  for (int n = 1; !taken.insert(unique).second; n++) unique = name + "_" + std::to_string(n);
  return unique;
}

// The crate writer picks a prim's schema from this name, which only a prim read from a file has. Without it
// a material loses its surface output and a shader its inputs.
template <class T>
usd::Prim make_prim(const T& value, const std::string& authored_name) {
  usd::Prim prim(value);
  prim.prim_type_name() = prim.type_name();
  if (authored_name != prim.element_name()) prim.metas().set_displayName(authored_name);
  return prim;
}

void add_child(usd::Prim& parent, usd::Prim&& child) {
  std::string err;
  if (!parent.add_child(std::move(child), /* rename */ false, &err)) fail("USD export: %s", err.c_str());
}

void mark_material_bound(usd::Prim& prim) {
  usd::APISchemas schemas;
  schemas.listOpQual = usd::ListEditQual::Prepend;
  schemas.names.push_back({usd::APISchemas::APIName::MaterialBindingAPI, ""});
  prim.metas().set_apiSchemas(schemas);
}

usd::Relationship relationship_to(const std::string& prim_path) {
  usd::Relationship rel;
  rel.set(usd::Path(prim_path, ""));
  return rel;
}

// A UsdUVTexture node reading one exported image. `outputs` are the channel groups a material input reads.
usd::Prim texture_node(const UsdImage& image, const std::string& name, const std::string& reader_path,
                       const std::unordered_set<std::string>& outputs, bool text) {
  usd::UsdUVTexture tex;
  tex.file.set_value(usd::value::AssetPath("./" + image.file));
  tex.sourceColorSpace.set_value(image.srgb ? usd::UsdUVTexture::SourceColorSpace::SRGB : usd::UsdUVTexture::SourceColorSpace::Raw);
  tex.wrapS.set_value(usd::UsdUVTexture::Wrap::Repeat);
  tex.wrapT.set_value(usd::UsdUVTexture::Wrap::Repeat);
  bool remapped = false;
  for (int k = 0; k < 4; k++) remapped = remapped || image.scale[k] != 1.f || image.bias[k] != 0.f;
  if (remapped) {
    tex.scale.set_value(usd::value::float4{image.scale[0], image.scale[1], image.scale[2], image.scale[3]});
    tex.bias.set_value(usd::value::float4{image.bias[0], image.bias[1], image.bias[2], image.bias[3]});
  }
  usd::Path uv(reader_path, "outputs:result");
  if (text) {
    // The text writer prints the typed input as texCoord2f, which OpenUSD's usdchecker rejects: the
    // UsdUVTexture spec says float2. A plain float2 property prints as the spec has it.
    usd::Attribute st;
    st.set_type_name("float2");
    st.set_connection(uv);
    tex.props.emplace("inputs:st", usd::Property(st, /* custom */ false));
  } else {
    tex.st.set_connection(uv);  // the crate writer only writes the typed input, and writes it as float2
  }
  if (outputs.count("rgb")) tex.outputsRGB.set_authored(true);
  if (outputs.count("r")) tex.outputsR.set_authored(true);
  if (outputs.count("g")) tex.outputsG.set_authored(true);
  if (outputs.count("b")) tex.outputsB.set_authored(true);
  if (outputs.count("a")) tex.outputsA.set_authored(true);

  usd::Shader shader;
  shader.name = name;
  shader.info_id = usd::kUsdUVTexture;
  shader.value = std::move(tex);
  return make_prim(shader, name);
}

// A Material prim holding its whole network: the surface shader, the UV reader and one texture node per image.
usd::Prim material_prim(const UsdMaterial& mat, const std::string& name, const std::string& path, bool text, Counts& counts) {
  usd::Material material;
  material.name = name;
  material.surface.set(usd::Path(path + "/" + kSurfaceNode, "outputs:surface"));
  usd::Prim prim = make_prim(material, mat.name);
  counts.materials++;

  std::string reader_path = path + "/" + kUvReaderNode;
  std::unordered_set<std::string> taken = {kSurfaceNode, kUvReaderNode};
  std::vector<std::string> node_names;
  for (const UsdImage& image : mat.images) node_names.push_back(prim_name("Texture_" + image.key, taken));

  usd::UsdPreviewSurface surface;
  surface.outputsSurface.set_authored(true);
  std::vector<std::unordered_set<std::string>> outputs(mat.images.size());
  auto connect = [&](auto& input, const UsdInput& from) {
    if (from.image < 0) return;
    input.set_connection(usd::Path(path + "/" + node_names[from.image], std::string("outputs:") + from.channels));
    outputs[from.image].insert(from.channels);
  };
  connect(surface.diffuseColor, mat.basecolor);
  connect(surface.opacity, mat.opacity);
  connect(surface.metallic, mat.metallic);
  connect(surface.roughness, mat.roughness);
  connect(surface.occlusion, mat.occlusion);
  connect(surface.normal, mat.normal);
  connect(surface.emissiveColor, mat.emissive);
  usd::Shader surface_shader;
  surface_shader.name = kSurfaceNode;
  surface_shader.info_id = usd::kUsdPreviewSurface;
  surface_shader.value = std::move(surface);
  add_child(prim, make_prim(surface_shader, kSurfaceNode));

  usd::UsdPrimvarReader_float2 reader;
  reader.varname.set_value(std::string(kUvPrimvar));
  reader.result.set_authored(true);
  usd::Shader reader_shader;
  reader_shader.name = kUvReaderNode;
  reader_shader.info_id = usd::kUsdPrimvarReader_float2;
  reader_shader.value = std::move(reader);
  add_child(prim, make_prim(reader_shader, kUvReaderNode));

  for (size_t i = 0; i < mat.images.size(); i++) {
    add_child(prim, texture_node(mat.images[i], node_names[i], reader_path, outputs[i], text));
    counts.textures++;
  }
  return prim;
}

// One part of the mesh as a Mesh prim. Vertices are the part's own, in the order its triangles first use them.
// A part with one texture set is bound to that material; a part with several gets one GeomSubset per set.
usd::Prim mesh_prim(const Mesh& m, const std::vector<uint32_t>& tris, const std::string& name, const std::string& authored_name,
                    const std::vector<std::string>& set_material, const std::vector<std::string>& set_prim_name,
                    std::vector<uint32_t>& remap, Counts& counts) {
  std::vector<usd::value::point3f> points;
  std::vector<usd::value::normal3f> normals;
  std::vector<usd::value::texcoord2f> uvs;
  std::vector<int32_t> indices, face_counts(tris.size(), 3);
  std::map<int, std::vector<int32_t>> faces_of_set;  // ordered, so the file is the same on every run
  std::vector<uint32_t> used;
  vec3 lo(1e30f), hi(-1e30f);
  indices.reserve(tris.size() * 3);
  for (size_t f = 0; f < tris.size(); f++) {
    uint32_t t = tris[f];
    for (int k = 0; k < 3; k++) {
      uint32_t v = m.idx[t * 3 + k];
      if (remap[v] == UINT32_MAX) {
        remap[v] = (uint32_t)points.size();
        used.push_back(v);
        points.push_back({m.pos[v].x, m.pos[v].y, m.pos[v].z});
        normals.push_back({m.nrm[v].x, m.nrm[v].y, m.nrm[v].z});
        uvs.push_back({m.uv[v].x, 1.f - m.uv[v].y});  // USD's V runs up the image, the mesh's runs down it
        lo = vmin(lo, m.pos[v]);
        hi = vmax(hi, m.pos[v]);
      }
      indices.push_back((int32_t)remap[v]);
    }
    faces_of_set[m.tri_set[t]].push_back((int32_t)f);
  }
  for (uint32_t v : used) remap[v] = UINT32_MAX;

  usd::GeomMesh mesh;
  mesh.name = name;
  mesh.points.set_value(std::move(points));
  mesh.normals.set_value(std::move(normals));
  mesh.normals.metas().set_interpolation_enum(usd::Interpolation::Vertex);
  mesh.faceVertexCounts.set_value(std::move(face_counts));
  mesh.faceVertexIndices.set_value(std::move(indices));
  // Without this USD treats every mesh as a subdivision surface and ignores its normals.
  mesh.subdivisionScheme.set_value(usd::GeomMesh::SubdivisionScheme::SubdivisionSchemeNone);
  mesh.extent.set_value(usd::Extent({lo.x, lo.y, lo.z}, {hi.x, hi.y, hi.z}));
  {
    usd::GeomPrimvar st;
    st.set_name(kUvPrimvar);
    st.set_value(std::move(uvs));
    st.set_interpolation(usd::Interpolation::Vertex);
    std::string err;
    if (!mesh.set_primvar(st, &err)) fail("USD export: %s", err.c_str());
  }

  bool one_set = faces_of_set.size() == 1;
  if (one_set) {
    const std::string& material = set_material[faces_of_set.begin()->first];
    if (!material.empty()) {
      mesh.set_materialBinding(relationship_to(material));
      counts.bindings++;
    }
  } else {
    // Every face is in exactly one subset. The crate writer drops the mesh's typed family type, so it is
    // written as the plain property it is in the file.
    usd::Attribute family_type = usd::Attribute::Uniform(usd::value::token("partition"));
    mesh.props.emplace(std::string("subsetFamily:") + kMaterialFamily + ":familyType", usd::Property(family_type, /* custom */ false));
  }
  usd::Prim prim = make_prim(mesh, authored_name);
  if (one_set && mesh.has_materialBinding()) mark_material_bound(prim);
  counts.meshes++;
  counts.triangles += (int64_t)tris.size();
  if (one_set) return prim;

  std::unordered_set<std::string> taken;
  for (auto& [set, faces] : faces_of_set) {
    usd::GeomSubset subset;
    subset.name = prim_name(set_prim_name[set], taken);
    subset.familyName.set_value(usd::value::token(kMaterialFamily));
    subset.indices.set_value(faces);
    bool bound = !set_material[set].empty();
    if (bound) subset.set_materialBinding(relationship_to(set_material[set]));
    usd::Prim subset_prim = make_prim(subset, m.set_names[set]);
    if (bound) {
      mark_material_bound(subset_prim);
      counts.bindings++;
    }
    add_child(prim, std::move(subset_prim));
  }
  return prim;
}

void count_prims(const usd::Prim& prim, Counts& counts) {
  if (const usd::GeomMesh* mesh = prim.as<usd::GeomMesh>()) {
    counts.meshes++;
    counts.triangles += (int64_t)mesh->get_faceVertexCounts().size();
    if (mesh->has_materialBinding()) counts.bindings++;
  } else if (const usd::GeomSubset* subset = prim.as<usd::GeomSubset>()) {
    if (subset->has_materialBinding()) counts.bindings++;
  } else if (const usd::Material* material = prim.as<usd::Material>()) {
    if (material->surface.authored()) counts.materials++;
  } else if (const usd::Shader* shader = prim.as<usd::Shader>()) {
    const usd::UsdUVTexture* tex = shader->value.as<usd::UsdUVTexture>();
    if (tex && tex->file.authored()) counts.textures++;
  }
  for (const usd::Prim& child : prim.children()) count_prims(child, counts);
}

// Parses the bytes about to be written and fails unless everything that went in is still there. LightUSD's
// crate writer has silently dropped whole shader networks, and a file that looks written but is missing its
// materials is worse than an error.
void check_read_back(const std::string& bytes, const std::string& path, const Counts& wrote) {
  usd::Stage stage;
  usd::USDLoadOptions options;
  options.load_assets = false;
  std::string warn, err;
  if (!usd::LoadUSDFromMemory((const uint8_t*)bytes.data(), bytes.size(), path, &stage, &warn, &err, options))
    fail("USD export: LightUSD could not read back what it wrote for %s: %s", path_filename(path).c_str(), err.c_str());
  Counts read;
  for (const usd::Prim& root : stage.root_prims()) count_prims(root, read);
  if (!(read == wrote))
    fail("USD export: %s lost data when LightUSD wrote it: wrote %d meshes, %lld triangles, %d materials, %d textures and %d material "
         "bindings; read back %d, %lld, %d, %d and %d",
         path_filename(path).c_str(), wrote.meshes, (long long)wrote.triangles, wrote.materials, wrote.textures, wrote.bindings,
         read.meshes, (long long)read.triangles, read.materials, read.textures, read.bindings);
}

}  // namespace

Json write_usd(const std::string& path, const std::string& format, const std::string& name, const Mesh& m,
               const std::vector<UsdMaterial>& materials) {
  bool text = format == "usda";
  if (!text && format != "usdc" && format != "usdz") fail("unknown USD format '%s' (usda, usdc, usdz)", format.c_str());
  std::lock_guard<std::mutex> one_at_a_time(g_lightusd);
  Counts counts;

  std::unordered_set<std::string> root_names;
  std::string root_name = prim_name(name, root_names);
  std::string root_path = "/" + root_name;
  std::string scope_path = root_path + "/" + kMaterialScope;

  usd::Xform root;
  root.name = root_name;
  usd::Prim root_prim = make_prim(root, name);
  root_prim.metas().set_kind(usd::Kind::Component);

  std::vector<std::vector<uint32_t>> part_tris(m.part_names.size());
  std::vector<bool> set_has_triangles(m.set_names.size(), false);
  for (size_t t = 0; t < m.tri_count(); t++) {
    part_tris[m.tri_part[t]].push_back((uint32_t)t);
    set_has_triangles[m.tri_set[t]] = true;
  }

  // Materials first: meshes bind to them by path. A texture set with no triangles (an OBJ's unused "Default")
  // gets no material, so nothing in the file is unbound and a .usdz carries no image nothing reads.
  usd::Scope scope;
  scope.name = kMaterialScope;
  usd::Prim scope_prim = make_prim(scope, kMaterialScope);
  std::vector<std::string> set_material(m.set_names.size()), set_prim_name(m.set_names.size());
  std::vector<const UsdMaterial*> written_materials;
  std::unordered_set<std::string> material_names;
  for (const UsdMaterial& mat : materials) {
    if (!set_has_triangles[mat.set]) continue;
    set_prim_name[mat.set] = prim_name(mat.name, material_names);
    set_material[mat.set] = scope_path + "/" + set_prim_name[mat.set];
    add_child(scope_prim, material_prim(mat, set_prim_name[mat.set], set_material[mat.set], text, counts));
    written_materials.push_back(&mat);
  }
  for (size_t set = 0; set < m.set_names.size(); set++)
    if (set_prim_name[set].empty()) set_prim_name[set] = prim_name(m.set_names[set], material_names);

  std::unordered_set<std::string> child_names = {kMaterialScope};
  std::vector<uint32_t> remap(m.pos.size(), UINT32_MAX);
  for (size_t part = 0; part < m.part_names.size(); part++) {
    if (part_tris[part].empty()) continue;
    std::string mesh_name = prim_name(m.part_names[part], child_names);
    add_child(root_prim, mesh_prim(m, part_tris[part], mesh_name, m.part_names[part], set_material, set_prim_name, remap, counts));
  }
  if (!written_materials.empty()) add_child(root_prim, std::move(scope_prim));

  usd::Stage stage;
  stage.metas().defaultPrim = usd::value::token(root_name);
  stage.metas().upAxis.set_value(usd::Axis::Y);
  stage.metas().metersPerUnit.set_value(1.0);
  stage.metas().doc.value = std::string("Patina ") + patina_version();
  if (!stage.add_root_prim(std::move(root_prim), /* rename */ false)) fail("USD export: %s", stage.get_error().c_str());
  if (!stage.commit()) fail("USD export: %s", stage.get_error().c_str());

  // LightUSD's own file writers print to stdout, which is where commands print their JSON and where the MCP
  // server talks to its client. So each format is written to memory and saved here.
  std::string bytes, warn, err;
  if (text) {
    bytes = stage.ExportToString();
  } else {
    std::vector<uint8_t> out;
    bool ok;
    if (format == "usdc") {
      ok = usd::usdc::SaveAsUSDCToMemory(stage, &out, &warn, &err);
    } else {
      // every image a material reads goes into the package, under the name the material reads it by
      std::map<std::string, std::vector<uint8_t>> assets;
      std::string dir = path_dir(path);
      for (const UsdMaterial* mat : written_materials) {
        for (const UsdImage& image : mat->images) {
          std::string data;
          if (!read_file(path_join(dir, image.file), data)) fail("USD export: cannot read %s to put it in the .usdz", image.file.c_str());
          assets[image.file].assign(data.begin(), data.end());
        }
      }
      ok = usd::SaveAsUSDZToMemory(stage, assets, &out, &warn, &err);
    }
    if (!ok) fail("USD export: LightUSD could not write %s: %s", path_filename(path).c_str(), err.c_str());
    bytes.assign((const char*)out.data(), out.size());
  }
  check_read_back(bytes, path, counts);
  write_file_or_throw(path, bytes);

  Json r = Json::object();
  r.set("format", format);
  r.set("meshes", counts.meshes);
  r.set("triangles", counts.triangles);
  r.set("materials", counts.materials);
  r.set("textures", counts.textures);
  r.set("bytes", (int64_t)bytes.size());
  return r;
}

}  // namespace pt
