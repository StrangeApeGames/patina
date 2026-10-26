// Install, self-update and the agent skills.
//
// Shared code: this file and selfupdate.h are identical in Patina (github.com/StrangeApeGames/patina) and Mirette
// (github.com/StrangeApeGames/mirette), so change both copies together. What differs between the tools is in each
// one's tool.cpp.
//
//   <tool> install   copies this binary to ~/.<tool>/bin, puts that folder on the user's PATH and sets up every
//                    agent found on the machine (Claude Code, Codex): the skill in the agent's skills folder and
//                    the MCP server registered for every project.
//   <tool> update    downloads the latest GitHub release for this platform, checks its SHA-256 against the
//                    release's SHA256SUMS.txt and runs the new binary's `install`, so the skill and guide
//                    always match the binary.
//
// Nothing is installed unless `install` or `update` is run. When the MCP server starts it looks for a newer
// release, at most once every few hours, and from then on tells the agent that one is out and to run
// `<tool> update` itself. <TOOL>_NO_UPDATE_CHECK=1 turns the check off.
//
// The server must not download and install a release on its own. That was tried: a long-lived process driven
// over a pipe that fetches an executable and runs it is what Windows Defender's behaviour monitoring calls a
// trojan (Trojan:Win32/Bearfoos.A!ml), and Defender removed every copy of the binary. The same download run by
// `update` from a shell is not flagged.
//
// Downloads go through the system curl (built into macOS and Windows 10+). A private repo refuses requests
// without a login, so when the anonymous request fails and GitHub's CLI (`gh`) is installed, the release is
// fetched through `gh` instead, with the user's own GitHub login.
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <thread>

#include "core.h"
#include "json.h"
#include "selfupdate.h"

#if defined(_WIN32)
#include <windows.h>
#include <cwctype>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace TOOL_NS {

namespace {

namespace fs = std::filesystem;

const int64_t kCheckEverySeconds = 6 * 3600;
const int64_t kStaleSeconds = 15 * 60;  // a download this old was left by a process that was killed

std::string env(const std::string& k) {
  const char* v = std::getenv(k.c_str());
  return v ? v : "";
}

// <ENV>_HOME, <ENV>_NO_UPDATE_CHECK, ...
std::string tool_env(const char* suffix) { return env(std::string(tool_info().env) + "_" + suffix); }

std::string home_dir() {
#if defined(_WIN32)
  std::string h = env("USERPROFILE");
#else
  std::string h = env("HOME");
#endif
  if (h.empty()) fail("cannot find the home directory (set %s_HOME)", tool_info().env);
  return h;
}

std::string bin_dir() { return path_join(tool_home(), "bin"); }

std::string installed_exe() {
#if defined(_WIN32)
  return path_join(bin_dir(), std::string(tool_info().name) + ".exe");
#else
  return path_join(bin_dir(), tool_info().name);
#endif
}

// Release asset for this platform.
const char* asset_name() {
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
  return tool_info().asset_windows_x64;
#elif defined(__APPLE__) && defined(__aarch64__)
  return tool_info().asset_macos_arm64;
#else
  return "";
#endif
}

bool same_path(const std::string& a, const std::string& b) {
  std::error_code ec;
  if (fs::equivalent(a, b, ec)) return true;  // also sees through short names and links
#if defined(_WIN32)
  std::string x = to_lower(path_abs(a)), y = to_lower(path_abs(b));
  for (auto& c : x) if (c == '\\') c = '/';
  for (auto& c : y) if (c == '\\') c = '/';
  return x == y;
#else
  return path_abs(a) == path_abs(b);
#endif
}

int64_t age_seconds(const std::string& path) {
  std::error_code ec;
  auto t = fs::last_write_time(path, ec);
  if (ec) return 0;
  return std::chrono::duration_cast<std::chrono::seconds>(fs::file_time_type::clock::now() - t).count();
}

int process_id() {
#if defined(_WIN32)
  return (int)GetCurrentProcessId();
#else
  return (int)getpid();
#endif
}

// ---------------------------------------------------------------- processes & downloads
std::string quote(const std::string& s) {
#if defined(_WIN32)
  return "\"" + s + "\"";
#else
  std::string r = "'";
  for (char c : s) { if (c == '\'') r += "'\\''"; else r += c; }
  return r + "'";
#endif
}

// Runs a shell command and returns its stdout. stderr is discarded and stdin is empty: an MCP server's own stdin
// carries the protocol, and a child must not read from it.
std::string run_capture(const std::string& cmd, int* status) {
#if defined(_WIN32)
  // cmd.exe looks in the current directory before PATH unless told not to, and this may run inside any repo.
  _putenv("NoDefaultCurrentDirectoryInExePath=1");
  FILE* f = _popen(("\"" + cmd + " 2>NUL <NUL\"").c_str(), "r");
#else
  FILE* f = popen((cmd + " 2>/dev/null </dev/null").c_str(), "r");
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

bool has_command(const char* cli) {
  int status = 0;
  run_capture(std::string(cli) + " --version", &status);
  return status == 0;
}

std::string curl_cmd(int timeout_s) {
#if defined(_WIN32)
  // Windows' own curl (Schannel + the Windows certificate store); a curl found first on PATH (e.g. Git's
  // mingw curl when run from Git Bash) may lack a usable CA bundle
  std::string sys = env("SystemRoot").empty() ? "C:\\Windows" : env("SystemRoot");
  std::string curl = file_exists(sys + "\\System32\\curl.exe") ? quote(sys + "\\System32\\curl.exe") : "curl.exe";
#else
  std::string curl = "curl";
#endif
  return strf("%s -fsSL --connect-timeout 5 --max-time %d -H \"User-Agent: %s/%s\"", curl.c_str(), timeout_s,
              tool_info().name, tool_info().version);
}

// URLs come from the release JSON and go onto a command line.
void check_url(const std::string& url) {
  bool scheme = url.rfind("https://", 0) == 0 || url.rfind("file://", 0) == 0;
  if (!scheme || url.find_first_of("\"'`$&|<>^\\ \t\r\n") != std::string::npos) fail("refusing to download from an unexpected URL: %s", url.c_str());
}

std::string http_get(const std::string& url, int timeout_s) {
  check_url(url);
  int status = 0;
  std::string body = run_capture(curl_cmd(timeout_s) + " " + quote(url), &status);
  if (status != 0) fail("download failed: %s (is curl installed and the network reachable?)", url.c_str());
  return body;
}

void http_download(const std::string& url, const std::string& file) {
  check_url(url);
  int status = 0;
  run_capture(curl_cmd(600) + " -o " + quote(file) + " " + quote(url), &status);
  if (status != 0 || !file_exists(file)) fail("download failed: %s", url.c_str());
}

// ---------------------------------------------------------------- versions & releases
std::vector<int> parse_version(std::string s) {
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

bool is_newer(const std::string& candidate, const std::string& current) {
  return !candidate.empty() && parse_version(candidate) > parse_version(current);
}

struct Release {
  std::string version, tag, page, asset_url, sums_url;
  bool via_gh = false;  // the repo is private: download through `gh` too
};

// Asks GitHub's CLI, signed in as the user, for a private repo's latest release.
std::string gh_latest_release() {
  if (!has_command("gh"))
    fail("cannot reach the releases of %s. If the repo is private, install GitHub's CLI (https://cli.github.com) and "
         "sign in with `gh auth login` as someone who can see it, then try again",
         tool_info().repo);
  int status = 0;
  std::string body = run_capture(strf("gh api repos/%s/releases/latest", tool_info().repo), &status);
  if (status != 0)
    fail("cannot reach the releases of %s, even through gh. Check `gh auth status`: the account must be able to see "
         "the repo (for StrangeApeGames repos, be a member of the organisation)",
         tool_info().repo);
  return body;
}

// gh release download; `file` "-" writes to stdout, which is returned.
std::string gh_download(const Release& rel, const char* name, const std::string& file) {
  for (char c : rel.tag)  // the tag comes from the release JSON and goes onto a command line
    if (!std::isalnum((unsigned char)c) && c != '.' && c != '-' && c != '_') fail("unexpected release tag: %s", rel.tag.c_str());
  int status = 0;
  std::string out = run_capture(strf("gh release download %s --repo %s --pattern %s --clobber --output %s", rel.tag.c_str(),
                                     tool_info().repo, name, file == "-" ? "-" : quote(file).c_str()),
                                &status);
  if (status != 0) fail("download failed: %s from %s %s (through gh)", name, tool_info().repo, rel.tag.c_str());
  return out;
}

Release fetch_latest_release(int timeout_s) {
  Json j;
  std::string err;
  // <TOOL>_RELEASES_API points at another release JSON (forks, tests; curl also accepts file:// URLs)
  std::string api = tool_env("RELEASES_API");
  bool custom = !api.empty();
  if (!custom) api = strf("https://api.github.com/repos/%s/releases/latest", tool_info().repo);
  Release r;
  std::string body;
  try {
    body = http_get(api, timeout_s);
  } catch (const Error&) {
    if (custom) throw;
    body = gh_latest_release();  // a private repo answers 404 to a request without a login
    r.via_gh = true;
  }
  if (!Json::try_parse(body, j, err)) fail("unexpected response from the GitHub releases API");
  r.tag = j.str("tag_name");
  r.version = r.tag;
  if (!r.version.empty() && (r.version[0] == 'v' || r.version[0] == 'V')) r.version.erase(0, 1);
  r.page = j.str("html_url");
  for (auto& a : j["assets"].items()) {
    std::string name = a.str("name");
    if (name == asset_name()) r.asset_url = a.str("browser_download_url");
    if (name == "SHA256SUMS.txt") r.sums_url = a.str("browser_download_url");
  }
  if (r.version.empty()) fail("no release found for %s", tool_info().repo);
  return r;
}

// ~/.<tool>/update_check.json: when the releases were last looked at, and what was found.
struct State {
  int64_t checked_at = 0;
  std::string latest;  // newest release seen, "" when GitHub has never been reached
};

std::string state_path() { return path_join(tool_home(), "update_check.json"); }

State read_state() {
  State s;
  try {
    std::string text, err;
    Json j;
    if (read_file(state_path(), text) && Json::try_parse(text, j, err)) {
      s.checked_at = (int64_t)j.num("checked_at", 0);
      s.latest = j.str("latest");
    }
  } catch (...) {
  }
  return s;
}

bool check_is_due() {
  int64_t since = (int64_t)std::time(nullptr) - read_state().checked_at;
  return since < 0 || since >= kCheckEverySeconds;  // a time in the future: the clock was wrong once
}

void write_state(const std::string& latest) {
  Json j = Json::object();
  j.set("checked_at", (int64_t)std::time(nullptr));
  j.set("latest", latest);
  try {
    make_dirs(tool_home());
    write_file_or_throw(state_path(), j.dump(2));
  } catch (...) {
  }
}

// ---------------------------------------------------------------- install
// Replaces `target` with `data`, even while `target` is running.
void replace_executable(const std::string& target, const std::string& data) {
  std::string tmp = strf("%s.new-%d", target.c_str(), process_id());
  write_file_or_throw(tmp, data);
  if (file_size(tmp) != (int64_t)data.size()) {  // a full disk must not become the installed binary
    std::remove(tmp.c_str());
    fail("cannot write %s (is the disk full?)", tmp.c_str());
  }
#if defined(_WIN32)
  if (MoveFileExA(tmp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING)) return;
  // A running .exe can't be overwritten, but it can be renamed out of the way; the next install removes it.
  std::string old = strf("%s.old-%lld-%d", target.c_str(), (long long)std::time(nullptr), process_id());
  if (!MoveFileExA(target.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING)) {
    std::remove(tmp.c_str());
    fail("cannot replace %s", target.c_str());
  }
  for (int attempt = 0; attempt < 20; attempt++) {  // a virus scanner can hold the new file for a moment
    if (MoveFileExA(tmp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING)) return;
    Sleep(100);
  }
  MoveFileExA(old.c_str(), target.c_str(), 0);  // put the old binary back rather than leave none
  std::remove(tmp.c_str());
  fail("cannot write %s", target.c_str());
#else
  chmod(tmp.c_str(), 0755);
  // rename() is atomic, and running processes keep the old file
  if (std::rename(tmp.c_str(), target.c_str()) != 0) {
    std::remove(tmp.c_str());
    fail("cannot write %s", target.c_str());
  }
#endif
}

// What earlier installs left behind: running binaries that were renamed out of the way, and downloads or
// half-written binaries from a process that was killed.
void remove_leftovers() {
  for (auto& f : list_dir(bin_dir())) {
    std::string name = path_filename(f);
    bool old = name.find(".old-") != std::string::npos;
    bool abandoned = name.find(".new") != std::string::npos && age_seconds(f) > kStaleSeconds;
    if (old || abandoned) std::remove(f.c_str());
  }
  for (auto& f : list_dir(path_join(tool_home(), "downloads")))
    if (age_seconds(f) > kStaleSeconds) std::remove(f.c_str());
}

std::string skill_markdown(const std::string& exe) {
  const ToolInfo& t = tool_info();
  std::string md;
  md += strf("---\nname: %s\ndescription: %s\n---\n\n", t.name, t.skill_description);
  md += strf("# %s %s\n\n", t.title, t.version);
  md += strf("Use the `%s` MCP tools when they are available. Otherwise run the CLI, which has the same commands and "
             "prints JSON (renders are written to disk as PNGs; open them to look):\n\n",
             t.name);
  md += strf("```\n\"%s\" --help\n```\n\n", exe.c_str());
  md += mcp_instructions();
  md += strf("\n\nBefore non-trivial work, read [AGENT_GUIDE.md](AGENT_GUIDE.md) in this folder: %s.\n\n", t.guide_summary);
  md += strf("When %s says a newer version is available, run `\"%s\" update` in a shell: it downloads the release, checks "
             "its SHA-256 and reinstalls, which also refreshes this skill. Then tell the user that the new version loads "
             "the next time the agent starts.\n\n",
             t.title, exe.c_str());
  md += strf("<!-- Written by `%s install`; overwritten on every update. Edit docs/AGENT_GUIDE.md in the %s repo instead. -->\n",
             t.name, t.title);
  return md;
}

// Adds `dir` to the user's PATH for new terminals. Returns a short status.
std::string add_to_path(const std::string& dir) {
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
  text += strf("\n# added by %s install\nexport PATH=\"%s:$PATH\"\n", tool_info().name, dir.c_str());
  write_file_or_throw(rc, text);
  return "added to ~/.zprofile (open a new terminal)";
#endif
}

// An agent this installer sets up: a skill in its skills folder and an MCP server registered through its CLI.
struct Agent {
  const char* id;   // key in the install result
  const char* cli;
  std::string home;      // its config folder
  bool home_from_env;    // the user pointed at it, so it counts as present even before it exists
  std::string mcp_remove, mcp_add;
};

std::vector<Agent> known_agents(const std::string& exe) {
  std::string name = tool_info().name, server = quote(exe) + " mcp";
  std::string claude = env("CLAUDE_CONFIG_DIR"), codex = env("CODEX_HOME");
  return {
      // --scope user: every project. A project's own .mcp.json entry (like a repo's dev build) still wins.
      {"claude", "claude", claude.empty() ? path_join(home_dir(), ".claude") : claude, !claude.empty(),
       "claude mcp remove --scope user " + name, "claude mcp add --scope user " + name + " -- " + server},
      {"codex", "codex", codex.empty() ? path_join(home_dir(), ".codex") : codex, !codex.empty(),
       "codex mcp remove " + name, "codex mcp add " + name + " -- " + server},
  };
}

Json register_mcp(const Agent& agent, bool has_cli) {
  Json r = Json::object();
  r.set("command", agent.mcp_add);
  if (!has_cli) {
    r.set("registered", false);
    r.set("note", strf("`%s` is not on PATH; run the command above once it is, or add the server to its config by hand", agent.cli));
    return r;
  }
  int status = 0;
  run_capture(agent.mcp_remove, &status);  // replace any older registration
  run_capture(agent.mcp_add, &status);
  r.set("registered", status == 0);
  if (status != 0) r.set("note", "the command above failed; run it yourself to see why");
  return r;
}

// The digest SHA256SUMS.txt lists for `name`, or "". Lines are "<sha256>  <name>" (or "<sha256> *<name>").
std::string listed_digest(const std::string& sums, const std::string& name) {
  size_t pos = 0;
  while (pos < sums.size()) {
    size_t end = sums.find('\n', pos);
    if (end == std::string::npos) end = sums.size();
    std::string line = sums.substr(pos, end - pos);
    pos = end + 1;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.find(' ') != 64) continue;
    size_t start = line.find_first_not_of(" *", 64);
    if (start != std::string::npos && line.substr(start) == name) return to_lower(line.substr(0, 64));
  }
  return "";
}

// Downloads the release's binary, checks it against the release's SHA256SUMS.txt and runs its `install`, so the
// skill and guide come from the new version. Returns what that install printed.
Json download_and_install(const Release& rel, const std::string& install_flags) {
  if (rel.asset_url.empty()) fail("release %s has no build for this platform (%s)", rel.version.c_str(), asset_name());
  if (rel.sums_url.empty()) fail("release %s has no SHA256SUMS.txt; refusing to install an unverified binary", rel.version.c_str());
  std::string dir = path_join(tool_home(), "downloads");
  make_dirs(dir);
  std::string file = path_join(dir, strf("%d-%s", process_id(), asset_name()));  // per process: updates can overlap
  std::string data, sums;
  if (rel.via_gh) {
    gh_download(rel, asset_name(), file);
    sums = gh_download(rel, "SHA256SUMS.txt", "-");
  } else {
    http_download(rel.asset_url, file);
    sums = http_get(rel.sums_url, 30);
  }
  if (!read_file(file, data)) fail("cannot read %s", file.c_str());
  if (listed_digest(sums, asset_name()) != sha256_hex(data.data(), data.size())) {
    std::remove(file.c_str());
    fail("checksum mismatch for %s; the download was discarded", asset_name());
  }
#if !defined(_WIN32)
  chmod(file.c_str(), 0755);
#endif
  int status = 0;
  std::string out = run_capture(quote(file) + " install --compact" + install_flags, &status);
  std::remove(file.c_str());
  Json installed;
  std::string err;
  if (status != 0 || !Json::try_parse(out, installed, err)) fail("installing %s failed: %s", rel.version.c_str(), out.c_str());
  return installed;
}

}  // namespace

std::string tool_home() {
  std::string h = tool_env("HOME");
  return h.empty() ? path_join(home_dir(), std::string(".") + tool_info().name) : path_abs(h);
}

CommandOutput cmd_install(const Json& a) {
  const ToolInfo& t = tool_info();
  std::string target = installed_exe();
  make_dirs(bin_dir());
  remove_leftovers();

  Json r = Json::object();
  r.set("ok", true);
  r.set("version", t.version);
  std::string self = exe_path();
  if (!same_path(self, target)) {
    std::string data;
    if (!read_file(self, data)) fail("cannot read %s", self.c_str());
    replace_executable(target, data);
  }
  r.set("exe", target);
  install_extras(r);

  bool skill = !a.boolean("no_skill", false), mcp = !a.boolean("no_mcp", false);
  Json agents = Json::object();
  for (auto& agent : known_agents(target)) {
    bool has_cli = mcp && has_command(agent.cli);
    if (!agent.home_from_env && !is_directory(agent.home) && !has_cli) continue;  // not on this machine
    Json j = Json::object();
    if (skill) {
      std::string dir = path_join(agent.home, std::string("skills/") + t.name);
      make_dirs(dir);
      write_file_or_throw(path_join(dir, "SKILL.md"), skill_markdown(target));
      write_file_or_throw(path_join(dir, "AGENT_GUIDE.md"), std::string(agent_guide()));
      j.set("skill", dir);
    }
    if (mcp) j.set("mcp", register_mcp(agent, has_cli));
    agents.set(agent.id, j);
  }
  r.set("agents", agents);
  Json server = Json::object();  // for an agent this installer does not know: the MCP server to register
  server.set("command", target);
  server.set("args", Json::array({Json("mcp")}));
  r.set("mcp_server", server);
  if (!a.boolean("no_path", false)) r.set("path", add_to_path(bin_dir()));
  write_state(t.version);  // just installed: nothing newer to announce until the next check
  r.set("next", "Restart the agent so it loads the MCP server and the skill.");
  CommandOutput o;
  o.result = r;
  return o;
}

CommandOutput cmd_update(const Json& a) {
  const ToolInfo& t = tool_info();
  Release rel = fetch_latest_release(30);
  write_state(rel.version);
  bool newer = is_newer(rel.version, t.version);
  Json r = Json::object();
  r.set("ok", true);
  r.set("current", t.version);
  r.set("latest", rel.version);
  r.set("update_available", newer);
  r.set("release", rel.page);
  CommandOutput o;
  o.result = r;
  if (a.boolean("check", false) || (!newer && !a.boolean("force", false))) return o;

  std::string flags;
  for (const char* k : {"no_skill", "no_mcp", "no_path"})
    if (a.boolean(k, false)) flags += std::string(" --") + k;  // main() maps --no-skill to no_skill too
  Json installed = download_and_install(rel, flags);
  r.set("updated_to", rel.version);
  r.set("install", installed);
  o.result = r;
  return o;
}

// Looks only. See the note at the top of this file before making it do more.
void start_update_check() {
  try {
    if (!tool_env("NO_UPDATE_CHECK").empty() || !check_is_due()) return;
    std::thread([] {
      try {
        write_state(fetch_latest_release(10).version);
      } catch (...) {
        // Offline: try again in a few hours rather than on every start, and keep what the last check found.
        write_state(read_state().latest);
      }
    }).detach();
  } catch (...) {
  }
}

std::string update_notice() {
  try {
    const ToolInfo& t = tool_info();
    std::string latest = read_state().latest;
    if (!is_newer(latest, t.version)) return "";
    return strf("%s %s is available (this is %s). Run `%s update` to upgrade.", t.title, latest.c_str(), t.version, t.name);
  } catch (...) {
    return "";
  }
}

std::string update_request() {
  try {
    const ToolInfo& t = tool_info();
    std::string latest = read_state().latest;
    if (!is_newer(latest, t.version)) return "";
    std::string said = strf("%s %s is available (this is %s).", t.title, latest.c_str(), t.version);
    // Only the installed copy is the agent's to update; a build in a repo belongs to whoever is working on it.
    if (!same_path(exe_path(), installed_exe())) return said + " Mention this to the user.";
    return said + strf(" Update it now: run `\"%s\" update` in a shell. It downloads the release, checks its SHA-256 and "
                       "reinstalls. Then tell the user that it was updated and that the new version loads the next time "
                       "you start.",
                       installed_exe().c_str());
  } catch (...) {
    return "";
  }
}

}  // namespace TOOL_NS
