// `patina view <project>`: native window (Metal on macOS, D3D11 on Windows) that shows the asset with
// the exact renderer agents use, live-reloads whenever an agent edits the project, and lets a human
// orbit, switch channels/masks and toggle layers (viewer-only, never written to disk).
#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>

#include "sokol_app.h"
#include "sokol_gfx.h"
#include "sokol_glue.h"
#include "sokol_log.h"

#include "imgui.h"
#define SOKOL_IMGUI_IMPL
#include "sokol_imgui.h"

#include "commands.h"
#include "core.h"
#include "image.h"
#include "project.h"
#include "render.h"

namespace pt {

namespace {

struct Camera {
  float az = 35, el = 22, zoom = 1;
  bool has_target = false;
  vec3 target{0, 0, 0};
};

struct Viewer {
  std::string project;

  // ---- shared with the worker thread
  std::mutex m;
  std::condition_variable cv;
  bool quit = false;
  uint64_t eval_gen = 1, eval_done = 0;      // bump eval_gen to request re-evaluation
  uint64_t render_gen = 1, render_done = 0;  // bump render_gen to request a new frame
  std::shared_ptr<EvaluatedProject> ev;
  std::string error;
  Json layer_outline;  // doc for the layer panel
  std::vector<uint8_t> frame;  // RGBA8
  int frame_w = 0, frame_h = 0;
  bool frame_new = false;
  double last_render_ms = 0;

  // requested state (main thread writes under lock, worker reads)
  Camera cam;
  std::string mode = "lit";
  int resolution = 1024;
  int view_w = 800, view_h = 600;
  bool interactive = false;
  std::map<std::string, bool> disabled;  // viewer-only layer toggles

  // ---- main thread only
  sg_image img{};
  sg_view view{};
  sg_sampler smp{};
  int img_w = 0, img_h = 0;
  int64_t proj_mtime = 0, mesh_mtime = 0;
  double last_poll = 0, last_input = 0;
  bool dragging = false, panning = false;
  float mx = 0, my = 0;
  std::thread worker;
  bool idle_hq_done = false;
};

Viewer* g = nullptr;

void set_disabled(Json& layers, const std::map<std::string, bool>& dis) {
  if (!layers.is_array()) return;
  for (auto& L : layers.items()) {
    if (!L.is_object()) continue;
    auto it = dis.find(L.str("id", ""));
    if (it != dis.end() && it->second) L.set("enabled", false);
    if (L.find("layers")) set_disabled(L.ref("layers"), dis);
  }
}

void worker_loop(Viewer* v) {
  uint64_t seen_eval = 0, seen_render = 0;
  std::string last_mode;
  for (;;) {
    uint64_t eg, rg;
    Camera cam;
    std::string mode;
    int res, w, h;
    bool inter;
    std::map<std::string, bool> dis;
    {
      std::unique_lock<std::mutex> lk(v->m);
      v->cv.wait(lk, [&] { return v->quit || v->eval_gen != seen_eval || v->render_gen != seen_render; });
      if (v->quit) return;
      eg = v->eval_gen; rg = v->render_gen; cam = v->cam; mode = v->mode; res = v->resolution; w = v->view_w; h = v->view_h;
      inter = v->interactive; dis = v->disabled;
    }
    if (eg != seen_eval || mode != last_mode) {
      // (re)evaluate: mode matters because mask views need their mask recorded
      try {
        Json doc = load_project(v->project).doc;
        Json* sets = doc.find("texture_sets");
        if (sets) for (auto& kv : sets->members()) if (kv.second.find("layers")) set_disabled(kv.second.ref("layers"), dis);
        auto ev = std::make_shared<EvaluatedProject>(evaluate_project_maps(v->project, &doc, res, mode));
        std::lock_guard<std::mutex> lk(v->m);
        v->ev = ev;
        v->layer_outline = load_project(v->project).doc;
        v->error.clear();
      } catch (const std::exception& e) {
        std::lock_guard<std::mutex> lk(v->m);
        v->error = e.what();
      }
      seen_eval = eg;
      last_mode = mode;
      rg = 0;  // force a render
    }
    std::shared_ptr<EvaluatedProject> ev;
    { std::lock_guard<std::mutex> lk(v->m); ev = v->ev; }
    if (!ev || w < 8 || h < 8) { seen_render = v->render_gen; continue; }
    Timer t;
    RenderOptions ro;
    // interactive frames render at reduced resolution; the idle frame is supersampled
    float scale = inter ? 0.5f : 1.f;
    ro.width = std::max(8, (int)(w * scale));
    ro.height = std::max(8, (int)(h * scale));
    ro.ssaa = inter ? 1 : 2;
    ro.mode = mode;
    ro.shadows = !inter;
    ViewSpec vs;
    vs.azimuth = cam.az;
    vs.elevation = cam.el;
    vs.zoom = cam.zoom;
    vs.has_world_target = cam.has_target;
    vs.target_world = cam.target;
    RgbImage img = render_view(*ev->bk->mesh, ev->bk->bvh, ev->maps, vs, ro);
    std::vector<uint8_t> rgba((size_t)img.w * img.h * 4);
    for (size_t i = 0; i < (size_t)img.w * img.h; i++) {
      rgba[i * 4] = img.px[i * 3]; rgba[i * 4 + 1] = img.px[i * 3 + 1]; rgba[i * 4 + 2] = img.px[i * 3 + 2]; rgba[i * 4 + 3] = 255;
    }
    {
      std::lock_guard<std::mutex> lk(v->m);
      v->frame.swap(rgba);
      v->frame_w = img.w;
      v->frame_h = img.h;
      v->frame_new = true;
      v->last_render_ms = t.ms();
    }
    seen_render = rg == 0 ? v->render_gen : rg;
  }
}

void request_render(bool interactive) {
  std::lock_guard<std::mutex> lk(g->m);
  g->interactive = interactive;
  g->render_gen++;
  g->cv.notify_all();
}
void request_eval() {
  std::lock_guard<std::mutex> lk(g->m);
  g->eval_gen++;
  g->cv.notify_all();
}

void upload_frame() {
  std::vector<uint8_t> px;
  int w, h;
  {
    std::lock_guard<std::mutex> lk(g->m);
    if (!g->frame_new) return;
    g->frame_new = false;
    px = g->frame;
    w = g->frame_w;
    h = g->frame_h;
  }
  if (w != g->img_w || h != g->img_h) {
    if (g->img.id) { sg_destroy_view(g->view); sg_destroy_image(g->img); }
    sg_image_desc d{};
    d.width = w;
    d.height = h;
    d.pixel_format = SG_PIXELFORMAT_RGBA8;
    d.usage.dynamic_update = true;
    g->img = sg_make_image(&d);
    sg_view_desc vd{};
    vd.texture.image = g->img;
    g->view = sg_make_view(&vd);
    g->img_w = w;
    g->img_h = h;
  }
  sg_image_data data{};
  data.mip_levels[0] = {px.data(), px.size()};
  sg_update_image(g->img, &data);
}

void poll_files() {
  double now = now_seconds();
  if (now - g->last_poll < 0.25) return;
  g->last_poll = now;
  int64_t pm = file_mtime_ns(g->project);
  int64_t mm = 0;
  try {
    Project p = load_project(g->project);
    mm = file_mtime_ns(p.mesh_path());
  } catch (...) {}
  if (pm != g->proj_mtime || mm != g->mesh_mtime) {
    g->proj_mtime = pm;
    g->mesh_mtime = mm;
    request_eval();
  }
}

const char* kModes[] = {"lit", "clay", "wireframe", "basecolor", "roughness", "metallic", "normal", "height", "ao", "emissive", "curvature", "thickness", "bake_ao", "bake_height", "bake_normal", "bake_misses", "islands", "parts", "uv_checker"};

void draw_layers(const Json& layers, int depth) {
  if (!layers.is_array()) return;
  for (int i = (int)layers.size() - 1; i >= 0; i--) {  // top of the stack first, like Substance
    const Json& L = layers[i];
    if (!L.is_object()) continue;
    std::string id = L.str("id", "?");
    std::string type = L.str("type", L.has("layers") ? "folder" : (L.has("material") ? "smart" : "fill"));
    ImGui::PushID(id.c_str());
    bool on;
    {
      std::lock_guard<std::mutex> lk(g->m);
      on = !g->disabled[id] && L.boolean("enabled", true);
    }
    ImGui::Indent(depth * 12.f);
    if (ImGui::Checkbox("##on", &on)) {
      { std::lock_guard<std::mutex> lk(g->m); g->disabled[id] = !on; }
      request_eval();
    }
    ImGui::SameLine();
    std::string label = id + "  [" + (type == "smart" ? L.str("material", "smart") : type) + "]";
    bool sel;
    { std::lock_guard<std::mutex> lk(g->m); sel = g->mode == "mask:" + id; }
    if (ImGui::Selectable(label.c_str(), sel)) {
      std::lock_guard<std::mutex> lk(g->m);
      g->mode = sel ? "lit" : "mask:" + id;
      g->eval_gen++;
      g->cv.notify_all();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("click: show this layer's mask; again: back to lit");
    ImGui::Unindent(depth * 12.f);
    if (type == "folder") draw_layers(L["layers"], depth + 1);
    ImGui::PopID();
  }
}

void init_cb() {
  sg_desc desc{};
  desc.environment = sglue_environment();
  desc.logger.func = slog_func;
  sg_setup(&desc);
  simgui_desc_t sd{};
  sd.logger.func = slog_func;
  simgui_setup(&sd);
  ImGuiStyle& st = ImGui::GetStyle();
  ImGui::StyleColorsDark();
  st.WindowRounding = 6;
  st.FrameRounding = 4;
  sg_sampler_desc smd{};
  smd.min_filter = SG_FILTER_LINEAR;
  smd.mag_filter = SG_FILTER_LINEAR;
  smd.wrap_u = SG_WRAP_CLAMP_TO_EDGE;
  smd.wrap_v = SG_WRAP_CLAMP_TO_EDGE;
  g->smp = sg_make_sampler(&smd);
  g->worker = std::thread(worker_loop, g);
}

void frame_cb() {
  poll_files();
  int fw = sapp_width(), fh = sapp_height();
  float dpi = sapp_dpi_scale();
  // viewport = whole window (the panel floats on top)
  {
    std::lock_guard<std::mutex> lk(g->m);
    int vw = (int)(fw / dpi * std::min(dpi, 1.5f)), vh = (int)(fh / dpi * std::min(dpi, 1.5f));
    if (vw != g->view_w || vh != g->view_h) {
      g->view_w = vw;
      g->view_h = vh;
      g->render_gen++;
      g->cv.notify_all();
    }
  }
  // after interaction stops, render one high quality frame
  if (!g->dragging && !g->panning && g->last_input > 0 && now_seconds() - g->last_input > 0.15 && !g->idle_hq_done) {
    g->idle_hq_done = true;
    request_render(false);
  }
  upload_frame();

  simgui_frame_desc_t fd{};
  fd.width = fw;
  fd.height = fh;
  fd.delta_time = sapp_frame_duration();
  fd.dpi_scale = dpi;
  simgui_new_frame(&fd);

  if (g->img.id) {
    ImVec2 size = ImGui::GetIO().DisplaySize;
    ImGui::GetBackgroundDrawList()->AddImage((ImTextureID)simgui_imtextureid_with_sampler(g->view, g->smp), ImVec2(0, 0), size);
  }

  ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(ImVec2(330, 560), ImGuiCond_FirstUseEver);
  ImGui::Begin("Patina");
  std::shared_ptr<EvaluatedProject> ev;
  std::string err, mode;
  Json outline;
  double rms;
  int res;
  {
    std::lock_guard<std::mutex> lk(g->m);
    ev = g->ev;
    err = g->error;
    outline = g->layer_outline;
    rms = g->last_render_ms;
    mode = g->mode;
    res = g->resolution;
  }
  ImGui::TextWrapped("%s", path_filename(g->project).c_str());
  if (ev) ImGui::Text("bake %.0f ms  eval %.0f ms  render %.0f ms", ev->bake_ms, ev->eval_ms, rms);
  else ImGui::Text("evaluating...");
  if (!err.empty()) { ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0.4f, 0.4f, 1)); ImGui::TextWrapped("%s", err.c_str()); ImGui::PopStyleColor(); }
  if (ev && ev->warnings.size()) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0.85f, 0.3f, 1));
    for (auto& w : ev->warnings.items()) ImGui::TextWrapped("%s", w.as_str().c_str());
    ImGui::PopStyleColor();
  }
  ImGui::Separator();
  if (ImGui::BeginCombo("view", mode.c_str())) {
    for (auto* md : kModes)
      if (ImGui::Selectable(md, mode == md)) {
        std::lock_guard<std::mutex> lk(g->m);
        g->mode = md;
        g->eval_gen++;
        g->cv.notify_all();
      }
    ImGui::EndCombo();
  }
  const int resolutions[] = {256, 512, 1024, 2048};
  if (ImGui::BeginCombo("textures", strf("%d", res).c_str())) {
    for (int r : resolutions)
      if (ImGui::Selectable(strf("%d", r).c_str(), r == res)) {
        std::lock_guard<std::mutex> lk(g->m);
        g->resolution = r;
        g->eval_gen++;
        g->cv.notify_all();
      }
    ImGui::EndCombo();
  }
  if (ImGui::Button("reset camera")) {
    std::lock_guard<std::mutex> lk(g->m);
    g->cam = Camera();
    g->render_gen++;
    g->cv.notify_all();
  }
  ImGui::SameLine();
  if (ImGui::Button("screenshot") && g->frame_w > 0) {
    std::lock_guard<std::mutex> lk(g->m);
    std::vector<uint8_t> rgb((size_t)g->frame_w * g->frame_h * 3);
    for (size_t i = 0; i < (size_t)g->frame_w * g->frame_h; i++) for (int k = 0; k < 3; k++) rgb[i * 3 + k] = g->frame[i * 4 + k];
    std::string out = path_join(path_join(path_dir(path_abs(g->project)), "renders"), path_stem(g->project) + "_viewer.png");
    make_dirs(path_dir(out));
    try { save_png_rgb8(out, g->frame_w, g->frame_h, rgb.data()); } catch (...) {}
  }
  ImGui::Separator();
  ImGui::TextDisabled("layers (top first) - click a name for its mask");
  if (outline.is_object())
    for (auto& kv : outline["texture_sets"].members()) {
      if (ImGui::CollapsingHeader(kv.first.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) draw_layers(kv.second["layers"], 0);
    }
  ImGui::Separator();
  ImGui::TextDisabled("drag: orbit  right/middle drag: pan  wheel: zoom  F: reset");
  ImGui::TextDisabled("live-reloads when the project file changes");
  ImGui::End();

  sg_pass pass{};
  pass.action.colors[0].load_action = SG_LOADACTION_CLEAR;
  pass.action.colors[0].clear_value = {0.12f, 0.125f, 0.135f, 1.f};
  pass.swapchain = sglue_swapchain();
  sg_begin_pass(&pass);
  simgui_render();
  sg_end_pass();
  sg_commit();
}

void event_cb(const sapp_event* e) {
  if (simgui_handle_event(e)) {
    if (e->type != SAPP_EVENTTYPE_MOUSE_UP) return;
  }
  bool want_mouse = ImGui::GetIO().WantCaptureMouse;
  switch (e->type) {
    case SAPP_EVENTTYPE_MOUSE_DOWN:
      if (want_mouse) break;
      if (e->mouse_button == SAPP_MOUSEBUTTON_LEFT) g->dragging = true;
      else g->panning = true;
      g->mx = e->mouse_x;
      g->my = e->mouse_y;
      break;
    case SAPP_EVENTTYPE_MOUSE_UP:
      g->dragging = g->panning = false;
      break;
    case SAPP_EVENTTYPE_MOUSE_MOVE: {
      float dx = e->mouse_x - g->mx, dy = e->mouse_y - g->my;
      g->mx = e->mouse_x;
      g->my = e->mouse_y;
      if (!g->dragging && !g->panning) break;
      std::shared_ptr<EvaluatedProject> ev;
      {
        std::lock_guard<std::mutex> lk(g->m);
        ev = g->ev;
        if (g->dragging) {
          g->cam.az -= dx * 0.35f;
          g->cam.el = clampf(g->cam.el + dy * 0.35f, -89.f, 89.f);
        } else if (ev) {
          const Mesh& m = *ev->bk->mesh;
          if (!g->cam.has_target) { g->cam.target = m.center(); g->cam.has_target = true; }
          float az = g->cam.az * kPi / 180.f, el = g->cam.el * kPi / 180.f;
          vec3 fwd = -vec3(std::sin(az) * std::cos(el), std::sin(el), std::cos(az) * std::cos(el));
          vec3 right = normalize(cross(fwd, vec3(0, 1, 0)));
          vec3 up = cross(right, fwd);
          float k = m.radius() * 2.2f / std::max(1.f, (float)sapp_height()) / g->cam.zoom;
          g->cam.target = g->cam.target - right * (dx * k) + up * (dy * k);
        }
      }
      g->last_input = now_seconds();
      g->idle_hq_done = false;
      request_render(true);
      break;
    }
    case SAPP_EVENTTYPE_MOUSE_SCROLL:
      if (want_mouse) break;
      {
        std::lock_guard<std::mutex> lk(g->m);
        g->cam.zoom = clampf(g->cam.zoom * std::pow(1.1f, e->scroll_y), 0.2f, 40.f);
      }
      g->last_input = now_seconds();
      g->idle_hq_done = false;
      request_render(true);
      break;
    case SAPP_EVENTTYPE_KEY_DOWN:
      if (ImGui::GetIO().WantCaptureKeyboard) break;
      if (e->key_code == SAPP_KEYCODE_F) {
        std::lock_guard<std::mutex> lk(g->m);
        g->cam = Camera();
        g->render_gen++;
        g->cv.notify_all();
      } else if (e->key_code == SAPP_KEYCODE_ESCAPE) {
        sapp_request_quit();
      } else if (e->key_code >= SAPP_KEYCODE_1 && e->key_code <= SAPP_KEYCODE_9) {
        int idx = e->key_code - SAPP_KEYCODE_1;
        if (idx < (int)(sizeof(kModes) / sizeof(kModes[0]))) {
          std::lock_guard<std::mutex> lk(g->m);
          g->mode = kModes[idx];
          g->eval_gen++;
          g->cv.notify_all();
        }
      }
      break;
    case SAPP_EVENTTYPE_RESIZED:
      request_render(false);
      break;
    default:
      break;
  }
}

void cleanup_cb() {
  {
    std::lock_guard<std::mutex> lk(g->m);
    g->quit = true;
    g->cv.notify_all();
  }
  if (g->worker.joinable()) g->worker.join();
  simgui_shutdown();
  sg_shutdown();
}

}  // namespace

int run_viewer(const std::string& project) {
  if (!file_exists(project)) fail("project not found: %s", project.c_str());
  static Viewer v;
  v.project = path_abs(project);
  g = &v;
  static std::string title = "Patina - " + path_filename(project);
  sapp_desc d{};
  d.init_cb = init_cb;
  d.frame_cb = frame_cb;
  d.event_cb = event_cb;
  d.cleanup_cb = cleanup_cb;
  d.width = 1400;
  d.height = 900;
  d.window_title = title.c_str();
  d.high_dpi = true;
  d.logger.func = slog_func;
  sapp_run(&d);
  return 0;
}

}  // namespace pt
