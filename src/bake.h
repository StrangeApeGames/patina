// Mesh-map baking into UV space: surface samples per texel + AO, thickness, curvature (and height from a high poly).
#pragma once
#include <functional>
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
  // With a high poly the mesh maps describe its surface, as in Toolbag: each texel is measured where it lands
  // on the high poly. Texels that find no high-poly surface keep the low poly's values.
  bool ao_from_high = true;         // AO and thickness (the whole high mesh occludes: parts ground each other)
  bool curvature_from_high = true;  // curvature of the high-poly surface instead of the low poly's edges
  // Height map (with a high poly): how far the high-poly surface lies above or below the low's, for parallax,
  // tessellation and displacement. 0.5 = on the low surface, 0 and 1 = height_range below and above it.
  float height_range = 0.f;         // fraction of the largest dimension; 0 = auto (the farthest any texel lands)
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
  std::vector<float> height;    // baked high-poly height, 0.5 = low surface (see height_range); empty = no high poly
  float height_range = 0;       // world units for height 0 / 1 (the same for every set)
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
struct GpuSample;
struct GpuScene;
// Normal bake (bake_normal.cpp): fills SampleSet::nmap/nmiss. With a high poly it also bakes the height map and
// moves AO, thickness and curvature onto the high-poly surface; without one the bevelled normal adds to the curvature.
// gpu = null bakes on the CPU.
void bake_normals(Baked& bk, const Mesh* high, const BakeSettings& s, Json& stats, Gpu* gpu);

// AO and thickness of the surface in `bvh` at n points of it; point(j) is the j-th one: position, seed, smooth
// normal and the geometric normal on the same side. `ext` is the size the settings' distances are fractions of.
// Traced on the GPU (bvh is entry `scene_bvh` of `scene`) or, with gpu == null, on all cores.
void trace_ao_thickness(const BVH& bvh, const BakeSettings& s, float ext, Gpu* gpu, const GpuScene& scene, uint32_t scene_bvh, size_t n,
                        const std::function<GpuSample(size_t)>& point, float* ao, float* thickness);

// Edge-integral curvature of a mesh: every edge with dihedral angle theta contributes theta * length, smoothed
// by a kernel of radius r in 3D. Crisp, resolution-independent convex/concave masks on low-poly hard-surface
// meshes (where vertex curvature fails) and ~mean curvature on smooth dense ones.
class CurvatureField {
 public:
  // Edges flatter than min_angle_deg are ignored. With `solid` (a BVH over all of m's triangles, in order) the
  // mesh is taken as separate pieces of surface, the way a high poly is put together: an edge then only counts
  // for points of its own piece (tri_shell), and not at all where it is buried in another piece (the rim of a
  // bolt sunk into a plate).
  void build(const Mesh& m, float r, float min_angle_deg, const BVH* solid = nullptr);
  // Curvature at p, a point of the surface with normal n: 0.5 = flat, above convex, below concave.
  float at(vec3 p, vec3 n, float gain, uint32_t shell = kAnyShell) const;
  size_t sample_count() const { return sw_.size(); }

  static constexpr uint32_t kAnyShell = UINT32_MAX;
  std::vector<uint32_t> tri_shell;  // connected piece of surface per triangle (only with `solid`)

 private:
  size_t cell_of(vec3 p, int& x, int& y, int& z) const;
  float r_ = 1, cell_ = 1;
  vec3 lo_{0, 0, 0};
  int gx_ = 1, gy_ = 1, gz_ = 1;
  std::vector<uint32_t> start_;   // cell -> first edge sample
  std::vector<uint8_t> reach_;    // cell -> an edge sample within reach (most texels are nowhere near an edge)
  std::vector<vec3> sp_, sn_;     // edge samples in cell order: position, normal
  std::vector<float> sw_;         // signed angle * length
  std::vector<uint32_t> shell_;
};
// Edge-aware 3x3 smoothing of per-sample scalars (never across UV gaps or distant surfaces).
void denoise_samples(const SampleSet& ss, std::vector<float>& v);

// Scatter per-sample values into a full padded image (every texel filled from its nearest sample).
void to_image(const SampleSet& ss, const float* samples, int comps, float* out_image);
// Gather image values back to samples.
void from_image(const SampleSet& ss, const float* image, int comps, float* out_samples);

}  // namespace pt
