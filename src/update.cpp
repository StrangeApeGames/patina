// Install, self-update and the global Claude Code skill.
//
//   patina install   copies this binary to ~/.patina/bin, writes the Blender bridge next to it, writes the
//                    `patina` skill to ~/.claude/skills/patina, registers the MCP server for every project
//                    (`claude mcp add --scope user`) and puts ~/.patina/bin on the user's PATH.
//   patina update    downloads the latest GitHub release for this platform, checks its SHA-256 against the
//                    release's SHA256SUMS.txt and runs the new binary's `install`, so the skill and guide
//                    always match the binary.
//
// Downloads go through the system curl (built into macOS and Windows 10+). Long-running processes (the MCP
// server) check for a newer release at most once a day in the background; the result is cached in
// ~/.patina/update_check.json and shown as a notice. Nothing is ever updated without `patina update`.
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <thread>

#include "commands.h"
#include "core.h"
#include "json.h"

#if defined(_WIN32)
#include <windows.h>
#include <cwctype>
#else
#include <sys/stat.h>
#endif

namespace pt {

static const char* kRepo = "parodyband/patina";

const char* patina_version() { return PATINA_VERSION; }

static std::string env(const char* k) {
  const char* v = std::getenv(k);
  return v ? v : "";
}

static std::string home_dir() {
#if defined(_WIN32)
  std::string h = env("USERPROFILE");
#else
  std::string h = env("HOME");
#endif
  if (h.empty()) fail("cannot find the home directory (set PATINA_HOME)");
  return h;
}

std::string patina_home() {
  std::string h = env("PATINA_HOME");
  return h.empty() ? path_join(home_dir(), ".patina") : path_abs(h);
}

static std::string claude_dir() {
  std::string c = env("CLAUDE_CONFIG_DIR");
  return c.empty() ? path_join(home_dir(), ".claude") : c;
}

static std::string bin_dir() { return path_join(patina_home(), "bin"); }

static std::string installed_exe() {
#if defined(_WIN32)
  return path_join(bin_dir(), "patina.exe");
#else
  return path_join(bin_dir(), "patina");
#endif
}

// Release asset for this platform (see .github/workflows/build.yml).
static const char* asset_name() {
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
  return "patina-windows-x64.exe";
#elif defined(__APPLE__) && defined(__aarch64__)
  return "patina-macos-arm64";
#else
  return "";
#endif
}

static bool same_path(const std::string& a, const std::string& b) {
#if defined(_WIN32)
  std::string x = to_lower(path_abs(a)), y = to_lower(path_abs(b));
  for (auto& c : x) if (c == '\\') c = '/';
  for (auto& c : y) if (c == '\\') c = '/';
  return x == y;
#else
  return path_abs(a) == path_abs(b);
#endif
}

// ---------------------------------------------------------------- processes & downloads
// Runs a shell command and returns its stdout; stderr is discarded.
static std::string run_capture(const std::string& cmd, int* status) {
#if defined(_WIN32)
  FILE* f = _popen(("\"" + cmd + " 2>NUL\"").c_str(), "r");
#else
  FILE* f = popen((cmd + " 2>/dev/null").c_str(), "r");
#endif
  if (!f) { *status = -1; return ""; }
  std::string out;
  char buf[4096];
  size_t n;
  while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
#if defined(_WIN32)
  *status = _pclose(f);
#else
  *status = pclose(f);
#endif
  return out;
}

static std::string curl_cmd(int timeout_s) {
#if defined(_WIN32)
  // Windows' own curl (Schannel + the Windows certificate store); a curl found first on PATH (e.g. Git's
  // mingw curl when run from Git Bash) may lack a usable CA bundle
  std::string sys = env("SystemRoot").empty() ? "C:\\Windows" : env("SystemRoot");
  std::string curl = file_exists(sys + "\\System32\\curl.exe") ? shell_quote(sys + "\\System32\\curl.exe") : "curl.exe";
#else
  std::string curl = "curl";
#endif
  return strf("%s -fsSL --connect-timeout 5 --max-time %d -H \"User-Agent: patina/%s\"", curl.c_str(), timeout_s, PATINA_VERSION);
}

static std::string http_get(const std::string& url, int timeout_s) {
  int status = 0;
  std::string body = run_capture(curl_cmd(timeout_s) + " " + shell_quote(url), &status);
  if (status != 0) fail("download failed: %s (is curl installed and the network reachable?)", url.c_str());
  return body;
}

static void http_download(const std::string& url, const std::string& file) {
  int status = 0;
  run_capture(curl_cmd(600) + " -o " + shell_quote(file) + " " + shell_quote(url), &status);
  if (status != 0 || !file_exists(file)) fail("download failed: %s", url.c_str());
}

// ---------------------------------------------------------------- versions & releases
static std::vector<int> parse_version(std::string s) {
  if (!s.empty() && (s[0] == 'v' || s[0] == 'V')) s.erase(0, 1);
  std::vector<int> v;
  size_t i = 0;
  while (i < s.size() && std::isdigit((unsigned char)s[i])) {
    int n = 0;
    while (i < s.size() && std::isdigit((unsigned char)s[i])) n = n * 10 + (s[i++] - '0');
    v.push_back(n);
    if (i < s.size() && s[i] == '.') i++;
  }
  while (v.size() < 3) v.push_back(0);
  return v;
}

static bool is_newer(const std::string& candidate, const std::string& current) {
  return !candidate.empty() && parse_version(candidate) > parse_version(current);
}

struct Release {
  std::string version, page, asset_url, sums_url;
};

static Release fetch_latest_release(int timeout_s) {
  Json j;
  std::string err;
  // PATINA_RELEASES_API points at another release JSON (forks, tests; curl also accepts file:// URLs)
  std::string api = env("PATINA_RELEASES_API");
  if (api.empty()) api = strf("https://api.github.com/repos/%s/releases/latest", kRepo);
  std::string body = http_get(api, timeout_s);
  if (!Json::try_parse(body, j, err)) fail("unexpected response from the GitHub releases API");
  Release r;
  r.version = j.str("tag_name");
  if (!r.version.empty() && (r.version[0] == 'v' || r.version[0] == 'V')) r.version.erase(0, 1);
  r.page = j.str("html_url");
  for (auto& a : j["assets"].items()) {
    std::string name = a.str("name");
    if (name == asset_name()) r.asset_url = a.str("browser_download_url");
    if (name == "SHA256SUMS.txt") r.sums_url = a.str("browser_download_url");
  }
  if (r.version.empty()) fail("no release found for %s", kRepo);
  return r;
}

static std::string cache_path() { return path_join(patina_home(), "update_check.json"); }

static void write_cache(const std::string& latest) {
  Json c = Json::object();
  c.set("checked_at", (int64_t)std::time(nullptr));
  c.set("latest", latest);
  try {
    make_dirs(patina_home());
    write_file_or_throw(cache_path(), c.dump(2));
  } catch (...) {
  }
}

void start_update_check() {
  if (!env("PATINA_NO_UPDATE_CHECK").empty()) return;
  try {
    std::string text;
    Json c;
    std::string err;
    if (read_file(cache_path(), text) && Json::try_parse(text, c, err) &&
        std::time(nullptr) - (int64_t)c.num("checked_at", 0) < 24 * 3600)
      return;
  } catch (...) {
    return;
  }
  std::thread([] {
    try {
      write_cache(fetch_latest_release(10).version);
    } catch (...) {
      write_cache("");  // offline: try again tomorrow rather than on every start
    }
  }).detach();
}

std::string update_notice() {
  try {
    std::string text, err;
    Json c;
    if (!read_file(cache_path(), text) || !Json::try_parse(text, c, err)) return "";
    std::string latest = c.str("latest");
    if (!is_newer(latest, PATINA_VERSION)) return "";
    return strf("Patina %s is available (this is %s). Run `patina update` to upgrade.", latest.c_str(), PATINA_VERSION);
  } catch (...) {
    return "";
  }
}

// ---------------------------------------------------------------- install
static const unsigned char kBridgeScript[] = {
#include "patina_blender.inc"
};

std::string installed_bridge_script() {
  std::string path = path_join(patina_home(), "tools/blender/patina_blender.py");
  std::string cur, want = (const char*)kBridgeScript;
  if (!read_file(path, cur) || cur != want) {
    make_dirs(path_dir(path));
    write_file_or_throw(path, want);
  }
  return path;
}

// Replaces `target` with `data`, even while `target` is running.
static void replace_executable(const std::string& target, const std::string& data) {
  std::string tmp = target + ".new";
  write_file_or_throw(tmp, data);
#if defined(_WIN32)
  if (file_exists(target) && !DeleteFileA(target.c_str())) {
    // A running .exe can't be deleted or overwritten, but it can be renamed; the next install removes it.
    std::string old = strf("%s.old-%lld", target.c_str(), (long long)std::time(nullptr));
    if (!MoveFileExA(target.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING)) fail("cannot replace %s", target.c_str());
  }
  if (!MoveFileExA(tmp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING)) fail("cannot write %s", target.c_str());
#else
  chmod(tmp.c_str(), 0755);
  // rename() is atomic, and running processes keep the old file
  if (std::rename(tmp.c_str(), target.c_str()) != 0) fail("cannot write %s", target.c_str());
#endif
}

static std::string skill_markdown(const std::string& exe) {
  std::string md;
  md += "---\n";
  md += "name: patina\n";
  md += "description: Texture 3D models with Patina, a headless Substance-Painter-style PBR texturing engine (layer stacks, "
        "smart materials, edge wear/dirt/rust masks, paint strokes, decals, previews). Use when asked to texture, paint, "
        "weather or age a 3D asset (.glb/.gltf/.obj or a Blender object), or to make PBR texture maps for Blender, Unreal, "
        "Unity, Godot, glTF or USD.\n";
  md += "---\n\n";
  md += strf("# Patina %s\n\n", PATINA_VERSION);
  md += "Use the `patina` MCP tools when they are available. Otherwise run the CLI, which has the same commands and prints JSON "
        "(renders are written to disk as PNGs; open them to look):\n\n";
  md += strf("```\n\"%s\" --help\n```\n\n", exe.c_str());
  md += mcp_instructions();
  md += "\n\nBefore non-trivial work, read [AGENT_GUIDE.md](AGENT_GUIDE.md) in this folder: recipes, every mask field, "
        "export presets and common mistakes.\n\n";
  md += "If Patina reports that a newer version is available, tell the user; `patina update` installs it and refreshes this skill.\n\n";
  md += "<!-- Written by `patina install`; overwritten on every update. Edit docs/AGENT_GUIDE.md in the Patina repo instead. -->\n";
  return md;
}

// Adds `dir` to the user's PATH for new terminals. Returns a short status.
static std::string add_to_path(const std::string& dir) {
#if defined(_WIN32)
  auto widen = [](const std::string& s) {
    int n = MultiByteToWideChar(CP_ACP, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_ACP, 0, s.c_str(), -1, &w[0], n);
    return w;
  };
  auto lower = [](std::wstring s) { for (auto& c : s) c = (wchar_t)towlower(c); return s; };
  std::wstring wdir = widen(dir);
  HKEY key;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Environment", 0, KEY_READ | KEY_WRITE, &key) != ERROR_SUCCESS) return "failed: cannot open HKCU\\Environment";
  DWORD type = REG_EXPAND_SZ, size = 0;
  std::wstring path;
  if (RegQueryValueExW(key, L"Path", nullptr, &type, nullptr, &size) == ERROR_SUCCESS && size > 0) {
    path.resize(size / sizeof(wchar_t));
    RegQueryValueExW(key, L"Path", nullptr, &type, (BYTE*)&path[0], &size);
    while (!path.empty() && path.back() == L'\0') path.pop_back();
  } else {
    type = REG_EXPAND_SZ;
  }
  std::wstring target = lower(wdir);
  size_t start = 0;
  while (start <= path.size()) {
    size_t end = path.find(L';', start);
    if (end == std::wstring::npos) end = path.size();
    std::wstring entry = lower(path.substr(start, end - start));
    while (!entry.empty() && (entry.back() == L'\\' || entry.back() == L'/')) entry.pop_back();
    if (entry == target) { RegCloseKey(key); return "already on PATH"; }
    start = end + 1;
  }
  if (!path.empty() && path.back() != L';') path += L';';
  path += wdir;
  LONG rc = RegSetValueExW(key, L"Path", 0, type, (const BYTE*)path.c_str(), (DWORD)((path.size() + 1) * sizeof(wchar_t)));
  RegCloseKey(key);
  if (rc != ERROR_SUCCESS) return "failed: cannot write the user PATH";
  SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)L"Environment", SMTO_ABORTIFHUNG, 2000, nullptr);
  return "added to the user PATH (open a new terminal)";
#else
  std::string cur = ":" + env("PATH") + ":";
  if (cur.find(":" + dir + ":") != std::string::npos) return "already on PATH";
  std::string rc = path_join(home_dir(), ".zprofile");
  std::string text;
  read_file(rc, text);
  if (text.find(dir) != std::string::npos) return "already in ~/.zprofile (open a new terminal)";
  if (!text.empty() && text.back() != '\n') text += '\n';
  text += "\n# added by patina install\nexport PATH=\"" + dir + ":$PATH\"\n";
  write_file_or_throw(rc, text);
  return "added to ~/.zprofile (open a new terminal)";
#endif
}

// Registers the MCP server for every project. Project .mcp.json entries (like this repo's dev build) still win.
static Json register_mcp(const std::string& exe) {
  Json r = Json::object();
  std::string add = "claude mcp add --scope user patina -- " + shell_quote(exe) + " mcp";
  r.set("command", add);
  int status = 0;
  run_capture("claude --version", &status);
  if (status != 0) {
    r.set("registered", false);
    r.set("note", "Claude Code CLI (`claude`) not found; run the command above once it is installed");
    return r;
  }
  run_capture("claude mcp remove --scope user patina", &status);  // replace any older registration
  run_capture(add, &status);
  r.set("registered", status == 0);
  return r;
}

CommandOutput cmd_install(const Json& a) {
  std::string target = installed_exe();
  make_dirs(bin_dir());
  for (auto& f : list_dir(bin_dir()))  // leftovers from replacing a running binary
    if (path_filename(f).find(".old-") != std::string::npos || path_ext(f) == ".new") std::remove(f.c_str());

  Json r = Json::object();
  r.set("ok", true);
  r.set("version", patina_version());
  std::string self = exe_path();
  if (!same_path(self, target)) {
    std::string data;
    if (!read_file(self, data)) fail("cannot read %s", self.c_str());
    replace_executable(target, data);
  }
  r.set("exe", target);
  r.set("blender_bridge", installed_bridge_script());

  if (!a.boolean("no_skill", false)) {
    std::string dir = path_join(claude_dir(), "skills/patina");
    make_dirs(dir);
    write_file_or_throw(path_join(dir, "SKILL.md"), skill_markdown(target));
    write_file_or_throw(path_join(dir, "AGENT_GUIDE.md"), std::string(agent_guide()));
    r.set("skill", dir);
  }
  if (!a.boolean("no_mcp", false)) r.set("mcp", register_mcp(target));
  if (!a.boolean("no_path", false)) r.set("path", add_to_path(bin_dir()));
  write_cache(patina_version());  // just installed: nothing newer to announce until the next check
  CommandOutput o;
  o.result = r;
  return o;
}

// ---------------------------------------------------------------- update
CommandOutput cmd_update(const Json& a) {
  Release rel = fetch_latest_release(30);
  write_cache(rel.version);
  bool newer = is_newer(rel.version, PATINA_VERSION);
  Json r = Json::object();
  r.set("ok", true);
  r.set("current", patina_version());
  r.set("latest", rel.version);
  r.set("update_available", newer);
  r.set("release", rel.page);
  CommandOutput o;
  o.result = r;
  if (a.boolean("check", false) || (!newer && !a.boolean("force", false))) return o;

  if (rel.asset_url.empty()) fail("release %s has no build for this platform (%s)", rel.version.c_str(), asset_name());
  if (rel.sums_url.empty()) fail("release %s has no SHA256SUMS.txt; refusing to install an unverified binary", rel.version.c_str());
  std::string dir = path_join(patina_home(), "downloads");
  make_dirs(dir);
  std::string file = path_join(dir, asset_name());
  http_download(rel.asset_url, file);

  std::string data, sums = http_get(rel.sums_url, 30);
  if (!read_file(file, data)) fail("cannot read %s", file.c_str());
  std::string digest = sha256_hex(data.data(), data.size());
  bool verified = false;
  size_t pos = 0;
  while (pos < sums.size()) {  // lines: "<sha256>  <name>"
    size_t end = sums.find('\n', pos);
    if (end == std::string::npos) end = sums.size();
    std::string line = sums.substr(pos, end - pos);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.size() > 66 && line.substr(line.size() - std::string(asset_name()).size()) == asset_name())
      verified = to_lower(line.substr(0, 64)) == digest;
    pos = end + 1;
  }
  if (!verified) {
    std::remove(file.c_str());
    fail("checksum mismatch for %s; the download was discarded", asset_name());
  }
#if !defined(_WIN32)
  chmod(file.c_str(), 0755);
#endif
  // The new binary installs itself, so the skill and guide come from the new version.
  std::string cmd = shell_quote(file) + " install --compact";
  for (const char* k : {"no_skill", "no_mcp", "no_path"})
    if (a.boolean(k, false)) cmd += std::string(" --") + k;  // main() maps --no-skill to no_skill too
  int status = 0;
  std::string out = run_capture(cmd, &status);
  std::remove(file.c_str());
  Json installed;
  std::string err;
  if (status != 0 || !Json::try_parse(out, installed, err)) fail("installing %s failed: %s", rel.version.c_str(), out.c_str());
  r.set("updated_to", rel.version);
  r.set("install", installed);
  o.result = r;
  return o;
}

}  // namespace pt
