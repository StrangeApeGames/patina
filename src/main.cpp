// patina - agent-first texturing engine. CLI entry point.
#include <cctype>
#include <cstdio>
#include <iostream>
#include <iterator>

#include "commands.h"
#include "core.h"
#include "json.h"
#include "selfupdate.h"

namespace pt {
int run_mcp_server();
#ifdef PATINA_HAS_VIEWER
int run_viewer(const std::string& project);
#endif
}  // namespace pt

using namespace pt;

static const char* kUsage = R"(patina - headless, agent-first texturing engine (Substance-Painter-style layers, CLI + MCP)

usage: patina <command> [args] [--flags]      (all commands print JSON)

  inspect  <mesh>                              describe texture sets, parts, UVs
  new      <project.json> --mesh <m.glb> [--resolution 2048] [--smart painted_metal]
  get      <project> [--outline]               print the project (or a compact outline)
  edit     <project> <ops.json | - | '[{...}]'> apply edit ops (see `patina library`)
  validate <project>                           check for errors/typos
  bake     <project> [--force] [--preview]     bake AO/curvature/thickness/normals (cached; on the GPU when there is one)
  bake     <project> --verify                  bake on the GPU and the CPU and compare the maps
  render   <project> [-o out.png] [--views iso,front] [--mode lit] [--size 512] [--sheet]
  variants <project> <variants.json>           render alternatives side by side
  export   <project> [--preset blender|gltf|unreal|unity_hdrp|unity_urp|godot|maps] [-o dir] [--glb]
  library  [--topic smart_materials|fields|channels|blend_modes|export_presets|render|modifiers]
  batch    <jobs.json> [-j 8]                  run many commands concurrently
  batch    --do render|export <p1> <p2> ...     same command on many projects
  blender  export|apply [--flags]              drive Blender headless (see docs)
  call     <command> '<json args>'             generic JSON call (identical to MCP tools)
  guide                                        print the agent guide (docs/AGENT_GUIDE.md)
  mcp                                          run the MCP server on stdio
  view     <project>                           native viewer window (live reload)
  bench    <mesh> [--resolution 2048]          performance benchmark
  install  [--no-skill] [--no-mcp] [--no-path] install to ~/.patina/bin + skill and MCP server for Claude Code and Codex
  update   [--check] [--force]                 install the latest release (the MCP server tells the agent when one is out)
  version                                      print the version and whether an update is available

environment: PATINA_THREADS, PATINA_GPU (0 = bake on the CPU, or part of an adapter name),
             PATINA_LIBRARY (extra smart material dirs), PATINA_BLENDER,
             PATINA_HOME (install dir, default ~/.patina), PATINA_NO_UPDATE_CHECK
)";

static Json parse_value(const std::string& s) {
  Json j;
  std::string err;
  if (!s.empty() && (s[0] == '[' || s[0] == '{' || s == "true" || s == "false" || s == "null" || s[0] == '-' || std::isdigit((unsigned char)s[0])))
    if (Json::try_parse(s, j, err)) return j;
  return Json(s);
}

static std::string read_arg_json_text(const std::string& a) {
  if (a == "-") {
    std::string s((std::istreambuf_iterator<char>(std::cin)), std::istreambuf_iterator<char>());
    return s;
  }
  if (!a.empty() && (a[0] == '[' || a[0] == '{')) return a;
  std::string s;
  if (!read_file(a, s)) fail("cannot read '%s'", a.c_str());
  return s;
}

int main(int argc, char** argv) {
  if (argc < 2 || std::string(argv[1]) == "-h" || std::string(argv[1]) == "--help" || std::string(argv[1]) == "help") {
    fputs(kUsage, stdout);
    return argc < 2 ? 1 : 0;
  }
  std::string cmd = argv[1];
  if (cmd == "version" || cmd == "--version") {
    printf("patina %s (%d threads)\n", patina_version(), thread_count());
    std::string notice = update_notice();
    if (!notice.empty()) printf("%s\n", notice.c_str());
    return 0;
  }
  if (cmd == "mcp") return run_mcp_server();
  if (cmd == "guide") { fputs(agent_guide(), stdout); return 0; }

  // generic flag parsing
  std::vector<std::string> pos;
  Json args = Json::object();
  bool compact = false;
  for (int i = 2; i < argc; i++) {
    std::string a = argv[i];
    if (a.size() > 1 && a[0] == '-' && !(a.size() > 1 && std::isdigit((unsigned char)a[1])) && a != "-") {
      std::string key = a.substr(a[1] == '-' ? 2 : 1);
      for (auto& c : key) if (c == '-') c = '_';
      if (key == "o") key = "out";
      if (key == "j") key = "parallel";
      if (key == "res") key = "resolution";
      if (key == "compact") { compact = true; continue; }
      if (i + 1 < argc && !(argv[i + 1][0] == '-' && argv[i + 1][1] == '-')) args.set(key, parse_value(argv[++i]));
      else args.set(key, true);
    } else {
      pos.push_back(a);
    }
  }

  try {
    std::string name = cmd;
    if (cmd == "call") {
      if (pos.empty()) fail("usage: patina call <command> '<json>'");
      name = pos[0];
      args = pos.size() > 1 ? Json::parse(read_arg_json_text(pos[1])) : Json::object();
    } else if (cmd == "view") {
#ifdef PATINA_HAS_VIEWER
      if (pos.empty()) fail("usage: patina view <project>");
      return run_viewer(pos[0]);
#else
      fail("this build has no viewer (configure with -DPATINA_VIEWER=ON)");
#endif
    } else if (cmd == "inspect") {
      if (!pos.empty()) args.set("mesh", pos[0]);
    } else if (cmd == "blender") {
      if (!pos.empty()) args.set("action", pos[0]);
    } else if (cmd == "edit") {
      if (!pos.empty()) args.set("project", pos[0]);
      if (pos.size() > 1) args.set("ops", Json::parse(read_arg_json_text(pos[1])));
      else if (args.has("op")) args.set("ops", Json::array({args["op"]}));
    } else if (cmd == "variants") {
      if (!pos.empty()) args.set("project", pos[0]);
      if (pos.size() > 1) {
        Json v = Json::parse(read_arg_json_text(pos[1]));
        args.set("variants", v.is_object() && v.has("variants") ? v["variants"] : v);
      }
    } else if (cmd == "batch") {
      if (args.has("do")) {
        Json jobs = Json::array();
        for (auto& p : pos) {
          Json job = Json::object();
          job.set("command", args["do"]);
          Json ja = Json::object();
          ja.set("project", p);
          for (auto& kv : args.members())
            if (kv.first != "do" && kv.first != "parallel") ja.set(kv.first, kv.second);
          job.set("args", ja);
          jobs.push(job);
        }
        Json b = Json::object();
        b.set("jobs", jobs);
        b.set("parallel", args.integer("parallel", 4));
        args = b;
      } else if (!pos.empty()) {
        Json j = Json::parse(read_arg_json_text(pos[0]));
        if (j.is_array()) { Json b = Json::object(); b.set("jobs", j); j = b; }
        if (args.has("parallel")) j.set("parallel", args["parallel"]);
        args = j;
      }
    } else if (cmd == "bench") {
      if (pos.empty()) fail("usage: patina bench <mesh>");
      // bench = new temp project + bake + render + export timings
      std::string dir = path_join(path_dir(path_abs(pos[0])), ".patina_bench");
      make_dirs(dir);
      std::string proj = path_join(dir, "bench.patina.json");
      Json na = Json::object();
      na.set("project", proj);
      na.set("mesh", path_abs(pos[0]));
      na.set("resolution", args.integer("resolution", 2048));
      na.set("smart", "painted_metal");
      na.set("overwrite", true);
      run_command("new", na);
      Json r = Json::object();
      Json ba = Json::object();
      ba.set("project", proj);
      ba.set("force", true);
      Timer t;
      r.set("bake", run_command("bake", ba).result["stats"]);
      Json ra = Json::object();
      ra.set("project", proj);
      ra.set("return_image", false);
      ra.set("resolution", args.integer("resolution", 2048));
      r.set("render", run_command("render", ra).result["timing"]);
      Json ea = Json::object();
      ea.set("project", proj);
      ea.set("out", path_join(dir, "textures"));
      Json ex = run_command("export", ea).result;
      Json et = Json::object();
      et.set("total_ms", ex["total_ms"]);
      et.set("export_ms", ex["export_ms"]);
      et.set("eval", ex["eval"]);
      r.set("export", et);
      r.set("threads", thread_count());
      printf("%s\n", r.dump(2).c_str());
      return 0;
    } else {
      if (!pos.empty() && !args.has("project")) args.set("project", pos[0]);
    }
    args.set("return_image", false);  // CLI writes images to disk
    // install/update are CLI-only: they are not MCP tools and not reachable through `call` or `batch`
    CommandOutput out = cmd == "install" ? cmd_install(args) : cmd == "update" ? cmd_update(args) : run_command(name, args);
    printf("%s\n", compact ? out.result.dump().c_str() : out.result.dump(2).c_str());
    return 0;
  } catch (const std::exception& e) {
    Json err = Json::object();
    err.set("ok", false);
    err.set("error", e.what());
    printf("%s\n", err.dump(2).c_str());
    return 1;
  }
}
