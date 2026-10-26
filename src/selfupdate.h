// Install, self-update and the agent skills.
//
// Shared code: this file and selfupdate.cpp are identical in Patina (github.com/StrangeApeGames/patina) and Mirette
// (github.com/StrangeApeGames/mirette), so change both copies together. What differs between the tools is in each
// one's tool.cpp.
#pragma once
#include <string>

#include "commands.h"  // CommandOutput, agent_guide(), mcp_instructions(), and TOOL_NS: the tool's namespace

namespace TOOL_NS {

struct ToolInfo {
  const char* name;     // "patina": the binary, the MCP server, the skill folder and the ~/.<name> home
  const char* title;    // "Patina"
  const char* version;
  const char* repo;     // GitHub "owner/repo" whose releases carry the binaries and SHA256SUMS.txt
  const char* env;      // "PATINA": prefix of <env>_HOME, <env>_NO_UPDATE_CHECK and <env>_RELEASES_API
  const char* asset_windows_x64;  // release asset per platform, "" where there is no build
  const char* asset_macos_arm64;
  const char* skill_description;  // SKILL.md front matter: what the tool is and when an agent should load it
  const char* guide_summary;      // what AGENT_GUIDE.md covers, finishing "read AGENT_GUIDE.md in this folder: "
};

// Defined by each tool (tool.cpp).
const ToolInfo& tool_info();
void install_extras(Json& result);  // installs what the tool carries besides the binary and the skills

std::string tool_home();  // ~/.<name>, or $<env>_HOME
CommandOutput cmd_install(const Json& args);
CommandOutput cmd_update(const Json& args);
// MCP server start: looks for a newer release in the background, at most every few hours. It installs nothing.
void start_update_check();
// What the last check found, or "" when this binary is up to date. update_notice is for a person ("<Title> X is
// available ... Run `<name> update`"); update_request goes in the MCP instructions and asks the agent to run the
// update itself.
std::string update_notice();
std::string update_request();

}  // namespace TOOL_NS
