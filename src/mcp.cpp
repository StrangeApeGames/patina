// MCP server over stdio (newline-delimited JSON-RPC 2.0). Tool calls run concurrently on their own
// threads, so one agent (or several sharing a server) can bake/render/export many assets at once.
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

#include "commands.h"
#include "core.h"
#include "json.h"

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#endif

namespace pt {

static const char* kInstructions = R"~(Patina is a headless, agent-first texturing engine (a Substance Painter for agents).
Workflow:
1. inspect(mesh) - texture sets (materials), parts, bounds, UV health. Export meshes from Blender as .glb (or use blender(action="export")).
2. new_project(project="asset.patina.json", mesh="asset.glb", smart="painted_metal") - creates the project with a starting smart material per texture set.
3. edit(project, ops=[...]) - add/update/remove/move layers. Layers are bottom -> top. A fill layer sets channels
   (basecolor "#rrggbb", metallic, roughness, height, emissive, opacity, normal) under a mask stack of fields
   (edge_wear, dirt, curvature, ao, gradient, direction, noise, grunge, scratches, streaks, paint strokes, decals, select parts...).
   Smart layers: {"type":"smart","material":"rusted_metal","params":{...}}. See library() for everything.
4. render(project) - LOOK at the result (returns an image). mode="mask:<layer_id>" shows where a layer applies; mode="clay" shows height detail.
   variants(project, variants=[{label, ops}]) compares alternatives side by side in one call.
5. export(project, preset="blender"|"gltf"|"unreal"|"unity_hdrp"|"unity_urp"|"godot", glb=true) - writes PNGs + manifest.json.
   blender(action="apply", project=...) builds Principled BSDF materials in a .blend and can render it.
Coordinates: points are normalized bounding-box coords [x,y,z] in 0..1 (0 = min corner). +Y is up, +Z is front, +X is right.
Lengths (radius/size) are fractions of the object's largest dimension. Noise "scale" = features per object size.
Everything is deterministic (seeded) and cached; renders take ~100-500 ms, so iterate freely. Run independent assets in parallel (batch).
Read library(topic="guide") once for the full guide with recipes.)~";

const char* mcp_instructions() { return kInstructions; }

struct ToolDef { const char* name; const char* command; const char* description; const char* schema; };

static const ToolDef kTools[] = {
    {"inspect", "inspect", "Describe a mesh (.glb/.gltf/.obj): texture sets (materials), parts with normalized bounds, UV islands/coverage/overlap warnings.",
     R"~({"type":"object","properties":{"mesh":{"type":"string","description":"path to the mesh"}},"required":["mesh"]})~"},
    {"new_project", "new", "Create a project file for a mesh. Optionally start every texture set with a smart material.",
     R"~({"type":"object","properties":{
        "project":{"type":"string","description":"path of the project file to create, e.g. assets/crate.patina.json"},
        "mesh":{"type":"string","description":"path to .glb/.gltf/.obj"},
        "resolution":{"type":"integer","description":"texture size, default 2048"},
        "smart":{"description":"smart material name for all sets, {material, params}, or {SetName: name|{material,params}}"},
        "name":{"type":"string"},"overwrite":{"type":"boolean"}},"required":["project","mesh"]})~"},
    {"get_project", "get", "Read a project: the full JSON document, or outline=true for a compact layer list.",
     R"~({"type":"object","properties":{"project":{"type":"string"},"outline":{"type":"boolean"}},"required":["project"]})~"},
    {"edit", "edit",
     "Apply edit operations to a project's layer stacks (validated by evaluation before saving). Ops:\n"
     "{\"op\":\"add\",\"set\":\"Crate\",\"layer\":{...},\"index\"|\"before\"|\"after\"|\"parent\":...} (default: on top)\n"
     "{\"op\":\"update\",\"id\":\"rust\",\"patch\":{...}} (JSON merge patch; null deletes a key)\n"
     "{\"op\":\"remove\",\"id\":...} {\"op\":\"move\",\"id\":...,\"index\"|\"before\"|\"after\":...} {\"op\":\"duplicate\",\"id\":...,\"patch\":{...}}\n"
     "{\"op\":\"replace\",\"set\":\"Crate\",\"layers\":[...]} {\"op\":\"set\",\"path\":\"/resolution\",\"value\":4096}\n"
     "Layer example: {\"id\":\"rust\",\"channels\":{\"basecolor\":\"#6b3a1e\",\"roughness\":0.85,\"metallic\":0},"
     "\"mask\":[{\"type\":\"dirt\",\"amount\":0.5},{\"type\":\"grunge\",\"style\":\"rust\",\"blend\":\"multiply\"}]}",
     R"~({"type":"object","properties":{"project":{"type":"string"},"ops":{"type":"array","items":{"type":"object"}}},"required":["project","ops"]})~"},
    {"render", "render",
     "Render the textured asset (CPU PBR, headless) and return the image. views: comma list of front, back, left, right, top, bottom, "
     "iso, iso_back, iso_left, iso_back_left, low, or \"az:el\", or objects {azimuth, elevation, zoom, target:[x,y,z]} for close-ups. "
     "mode: lit | clay | basecolor | roughness | metallic | normal | height | ao | curvature | thickness | bake_ao | mask:<layer_id> | islands | parts | uv_checker. "
     "sheet=true shows the flat texture maps instead; uv_layout=true draws each texture set's UV layout. Lighting: a studio HDRI by default (environment=\"studio\"); "
     "environment can also be a path to an equirectangular .hdr or \"procedural\"; env_rotation (degrees) turns it to move reflections. "
     "A project's \"render\" block sets defaults.",
     R"~({"type":"object","properties":{"project":{"type":"string"},"views":{"description":"e.g. \"iso,iso_back,front,top\" by default"},
        "mode":{"type":"string"},"size":{"type":"integer","description":"pixels per view - default 512"},
        "resolution":{"type":"integer","description":"texture resolution used for the preview - default min of 1024 and the project resolution"},
        "sheet":{"type":"boolean"},"uv_layout":{"type":"boolean"},"out":{"type":"string"},"stats":{"type":"boolean"},"return_image":{"type":"boolean"},
        "environment":{"type":"string","description":"studio (default HDRI) | procedural | path to an .hdr"},
        "env_rotation":{"type":"number","description":"degrees around the up axis"},"env_intensity":{"type":"number"},
        "exposure":{"type":"number"}},"required":["project"]})~"},
    {"variants", "variants",
     "Evaluate several alternative edits in parallel (without saving) and return one comparison image. "
     "variants: [{\"label\":\"heavy rust\",\"ops\":[{\"op\":\"update\",\"id\":\"rust\",\"patch\":{\"params\":{\"rust\":0.8}}}]}, ...]. Apply the winner with edit.",
     R"~({"type":"object","properties":{"project":{"type":"string"},"variants":{"type":"array","items":{"type":"object"}},
        "views":{},"mode":{"type":"string"},"size":{"type":"integer"},"resolution":{"type":"integer"}},"required":["project","variants"]})~"},
    {"bake", "bake", "Bake (or re-bake with force=true) mesh maps: AO, curvature, thickness, normals. preview=true returns an image of them. Other commands bake automatically. Runs on the GPU when there is one (stats.device); verify=true bakes on GPU and CPU and reports how far apart the maps are.",
     R"~({"type":"object","properties":{"project":{"type":"string"},"force":{"type":"boolean"},"preview":{"type":"boolean"},"verify":{"type":"boolean"},"resolution":{"type":"integer"}},"required":["project"]})~"},
    {"export", "export", "Export final textures + manifest.json. preset: blender (default) | gltf | unreal | unity_hdrp | unity_urp | godot | maps. glb=true also writes a textured .glb.",
     R"~({"type":"object","properties":{"project":{"type":"string"},"preset":{"type":"string"},"out":{"type":"string"},"glb":{"type":"boolean"},
        "resolution":{"type":"integer"}},"required":["project"]})~"},
    {"library", "library", "List smart materials (with params), field/generator types (with params), channels, blend modes, export presets, render modes. topic narrows it: guide (full agent guide with recipes - read this first) | smart_materials | fields | channels | blend_modes | export_presets | render | modifiers.",
     R"~({"type":"object","properties":{"topic":{"type":"string"},"project":{"type":"string","description":"include the project's library/ folder"}}})~"},
    {"validate", "validate", "Check a project for errors and warnings (unknown keys, typos, missing parts) without rendering.",
     R"~({"type":"object","properties":{"project":{"type":"string"}},"required":["project"]})~"},
    {"batch", "batch", "Run many commands concurrently, e.g. export 20 assets at once: jobs=[{\"command\":\"export\",\"args\":{\"project\":\"a.patina.json\"}}, ...].",
     R"~({"type":"object","properties":{"jobs":{"type":"array","items":{"type":"object"}},"parallel":{"type":"integer","description":"max concurrent jobs, default 4"},
        "return_images":{"type":"boolean"}},"required":["jobs"]})~"},
    {"blender", "blender",
     "Drive Blender headless. action=export: {blend, out, objects?, auto_uv?} .blend -> .glb. action=apply: {project | manifest, mesh | blend, out, render?} builds Principled BSDF materials from exported textures, saves a .blend and optionally renders it.",
     R"~({"type":"object","properties":{"action":{"type":"string","enum":["export","apply"]},"blend":{"type":"string"},"out":{"type":"string"},
        "objects":{"type":"string"},"auto_uv":{"type":"boolean"},"project":{"type":"string"},"manifest":{"type":"string"},"mesh":{"type":"string"},
        "render":{"type":"string"},"engine":{"type":"string"},"samples":{"type":"integer"},"res":{"type":"integer"}},"required":["action"]})~"},
};

namespace {
std::mutex g_out_m;
void send(const Json& msg) {
  std::string s = msg.dump();
  std::lock_guard<std::mutex> lk(g_out_m);
  fwrite(s.data(), 1, s.size(), stdout);
  fputc('\n', stdout);
  fflush(stdout);
}
void send_result(const Json& id, const Json& result) {
  Json m = Json::object();
  m.set("jsonrpc", "2.0");
  m.set("id", id);
  m.set("result", result);
  send(m);
}
void send_error(const Json& id, int code, const std::string& msg) {
  Json m = Json::object();
  m.set("jsonrpc", "2.0");
  m.set("id", id);
  Json e = Json::object();
  e.set("code", code);
  e.set("message", msg);
  m.set("error", e);
  send(m);
}
}  // namespace

static Json tools_list() {
  Json tools = Json::array();
  for (auto& t : kTools) {
    Json j = Json::object();
    j.set("name", t.name);
    j.set("description", t.description);
    j.set("inputSchema", Json::parse(t.schema));
    tools.push(j);
  }
  Json r = Json::object();
  r.set("tools", tools);
  return r;
}

static Json call_tool(const std::string& name, const Json& args) {
  const ToolDef* def = nullptr;
  for (auto& t : kTools) if (name == t.name) def = &t;
  Json content = Json::array();
  Json result = Json::object();
  if (!def) {
    Json c = Json::object();
    c.set("type", "text");
    c.set("text", "unknown tool: " + name);
    content.push(c);
    result.set("content", content);
    result.set("isError", true);
    return result;
  }
  try {
    CommandOutput out = run_command(def->command, args);
    Json c = Json::object();
    c.set("type", "text");
    c.set("text", out.result.dump(1));
    content.push(c);
    for (auto& png : out.images) {
      Json im = Json::object();
      im.set("type", "image");
      im.set("data", base64_encode(png.data(), png.size()));
      im.set("mimeType", "image/png");
      content.push(im);
    }
    result.set("content", content);
    result.set("isError", false);
  } catch (const std::exception& e) {
    Json c = Json::object();
    c.set("type", "text");
    c.set("text", std::string("error: ") + e.what());
    content.push(c);
    result.set("content", content);
    result.set("isError", true);
  }
  return result;
}

int run_mcp_server() {
#if defined(_WIN32)
  // JSON-RPC lines must not get \r\n translation
  _setmode(_fileno(stdout), _O_BINARY);
  _setmode(_fileno(stdin), _O_BINARY);
#endif
  start_update_check();
  std::atomic<int> inflight{0};
  std::mutex cv_m;
  std::condition_variable cv;
  const int max_concurrent = 32;
  std::string line;
  while (std::getline(std::cin, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) continue;
    Json msg;
    std::string err;
    if (!Json::try_parse(line, msg, err)) {
      send_error(Json(), -32700, "parse error: " + err);
      continue;
    }
    std::string method = msg.str("method", "");
    const Json* idp = msg.find("id");
    Json id = idp ? *idp : Json();
    bool is_request = idp != nullptr;
    const Json& params = msg["params"];
    if (method == "initialize") {
      Json r = Json::object();
      std::string pv = params.str("protocolVersion", "2025-06-18");
      r.set("protocolVersion", pv);
      Json caps = Json::object();
      Json tools = Json::object();
      tools.set("listChanged", false);
      caps.set("tools", tools);
      r.set("capabilities", caps);
      Json info = Json::object();
      info.set("name", "patina");
      info.set("version", patina_version());
      r.set("serverInfo", info);
      std::string notice = update_notice();
      r.set("instructions", notice.empty() ? std::string(kInstructions)
                                           : std::string(kInstructions) + "\n\n" + notice + " Mention this to the user.");
      send_result(id, r);
    } else if (method == "ping") {
      if (is_request) send_result(id, Json::object());
    } else if (method == "tools/list") {
      send_result(id, tools_list());
    } else if (method == "tools/call") {
      std::string name = params.str("name", "");
      Json args = params["arguments"];
      {
        std::unique_lock<std::mutex> lk(cv_m);
        cv.wait(lk, [&] { return inflight.load() < max_concurrent; });
        inflight++;
      }
      std::thread([id, name, args, &inflight, &cv, &cv_m] {
        Json r = call_tool(name, args);
        send_result(id, r);
        { std::lock_guard<std::mutex> lk(cv_m); inflight--; }
        cv.notify_all();
      }).detach();
    } else if (method.rfind("notifications/", 0) == 0) {
      // initialized, cancelled, etc. - nothing to do
    } else if (is_request) {
      send_error(id, -32601, "method not found: " + method);
    }
  }
  // stdin closed: let running calls finish
  std::unique_lock<std::mutex> lk(cv_m);
  cv.wait(lk, [&] { return inflight.load() == 0; });
  return 0;
}

}  // namespace pt
