// Mesh-map baking into UV space: surface samples per texel + AO, thickness, curvature.
#pragma once
#include <memory>

#include "bvh.h"
#include "json.h"
#include "mesh.h"

namespace pt {

// Tangent-space normal baking: high -> low (cage + anti-skew + name matching) and/or a bevel shader
// (rounded edges from ray sampling, like Blender's Bevel node). Lengths are fractions of the low mesh's
// largest dimension. Active when "high" is set or bevel_radius > 0.
struct NormalBakeSettings {
  std::string high;             // high-poly mesh (absolute path), "" = none
  std::string match = "name";   // name: *_low bakes only from *_high with the same base name | all
  float cage = 0.02f;           // how far outside the low surface rays start (automatic averaged-normal cage)
  float depth = 0.02f;          // how far inside the low surface rays still look
  float skew = -1.f;            // -1 = auto; 0 = averaged cage direction, 1 = low shading normal
  float skew_distance = 0.03f;  // auto: distance from hard edges over which rays turn from cage to shading normal
  bool ignore_backfaces = true;
  int samples = 4;              // supersamples per texel (1, 4, 9, 16)
  float bevel_radius = 0.f;     // bevel shader radius (0 = off)
  int bevel_samples = 64;
  float bevel_min_angle = 10.f;  // degrees: edges flatter than this are not rounded
  bool bevel_same_part = true;   // only round against the same part (touching parts stay crisp)
  bool denoise = true;
  float curvature = 1.f;         // how much the baked normal adds to the curvature map (edge wear follows it)
  Json cage_mask;                 // mask stack: where it is 1 the cage grows to cage_to (Toolbag's offset map)
  float cage_to = 0.06f;
  Json skew_mask;                 // mask stack: where it is 1 skew becomes skew_to (Toolbag's skew map)
  float skew_to = 1.f;
  std::string cage_mesh;          // explicit cage (absolute path), same triangles as the low; overrides cage/skew
  bool ao_from_high = true;       // trace AO from the high poly (whole mesh: parts occlude each other)
  std::string base_dir;           // project directory for mask files (not part of the settings hash)

  bool active() const { return !high.empty() || bevel_radius > 0.f; }
  void from_json(const Json& j);
  Json to_json() const;
};

struct BakeSettings {
  int ao_samples = 48;
  float ao_distance = 0.3f;         // fraction of the mesh's largest dimension
  int thickness_samples = 16;
  float thickness_distance = 0.3f;  // fraction of largest dimension
  float curvature_radius = 0.012f;  // fraction of largest dimension
  float curvature_gain = 1.0f;
  float curvature_min_angle = 1.5f;  // degrees; flatter edges are ignored
  NormalBakeSettings normal;

  void from_json(const Json& j);
  Json to_json() const;
  uint64_t hash() const;
};

// All covered texels of one texture set, stored compactly (structure of arrays).
// "Samples" include texels whose centers are inside a triangle (interior) and gutter texels within
// ~1.5px of a triangle edge, which get the closest surface point (removes seams under filtering).
struct SampleSet {
  std::string name;
  int set = 0;
  int res = 0;
  std::vector<int32_t> texel;     // y*res + x
  std::vector<uint8_t> interior;  // 1 = texel center inside triangle
  std::vector<uint32_t> tri;
  std::vector<vec3> pos, nrm, fnrm;  // world position, smooth normal, geometric normal
  std::vector<vec4> tan;             // tangent (+U) and bitangent sign
  std::vector<vec2> uv;
  std::vector<float> len_u, len_v;   // world length of one texel step along image x / y
  std::vector<float> ao, thickness, curvature;
  std::vector<vec3> nmap;       // baked tangent-space normal (MikkTSpace, +Y = OpenGL); empty = none
  std::vector<uint8_t> nmiss;   // 1 = no high-poly surface found (cage too tight or no matching part)
  std::vector<int32_t> pad;  // res*res -> nearest sample (padding / 2D neighborhood ops)
  size_t size() const { return texel.size(); }
  size_t interior_count = 0;
};

struct Baked {
  std::shared_ptr<const Mesh> mesh;
  BVH bvh;
  BakeSettings settings;
  std::vector<SampleSet> sets;  // indexed like mesh->set_names
  Json stats;
  uint64_t key = 0;
};

// Where the ray-traced maps (AO, thickness, high -> low projection, bevel) are computed. Like Painter's
// "GPU raytracing" option: the GPU when there is one, otherwise (or when it fails mid-bake) all CPU cores.
// Both run the same estimators and agree up to float rounding; stats report which one ran.
enum class BakeDevice {
  Auto,  // GPU if available (see Gpu::get) and the bake is big enough to pay for starting it
  Cpu,
  Gpu,   // fail instead of falling back
};

// res_per_set.size() == mesh->set_names.size(). cache_dir may be empty (no disk cache).
std::shared_ptr<Baked> bake_mesh(std::shared_ptr<const Mesh> mesh, const std::vector<int>& res_per_set, const BakeSettings& s,
                                 const std::string& cache_dir, bool force, BakeDevice device = BakeDevice::Auto);

class Gpu;
// Normal bake (bake_normal.cpp): fills SampleSet::nmap/nmiss and adds normal-derived curvature.
// gpu = null bakes on the CPU.
void bake_normals(Baked& bk, const Mesh* high, const BakeSettings& s, Json& stats, Gpu* gpu);
// Edge-aware 3x3 smoothing of per-sample scalars (never across UV gaps or distant surfaces).
void denoise_samples(const SampleSet& ss, std::vector<float>& v);

// Scatter per-sample values into a full padded image (every texel filled from its nearest sample).
void to_image(const SampleSet& ss, const float* samples, int comps, float* out_image);
// Gather image values back to samples.
void from_image(const SampleSet& ss, const float* image, int comps, float* out_samples);

}  // namespace pt
