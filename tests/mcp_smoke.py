"""MCP smoke test: handshake, tools/list, and concurrent tools/call over stdio.

usage: python3 tests/mcp_smoke.py [path/to/patina] [work_dir]
"""
import json
import os
import subprocess
import sys
import time

BIN = sys.argv[1] if len(sys.argv) > 1 else "build/patina"
# CreateProcess won't resolve "build/patina" to build\patina.exe on Windows.
if os.name == "nt" and os.path.exists(BIN + ".exe"):
    BIN = os.path.abspath(BIN + ".exe")
WORK = sys.argv[2] if len(sys.argv) > 2 else "ci_work/mcp"
os.makedirs(WORK, exist_ok=True)
ASSETS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "examples", "assets")

p = subprocess.Popen([BIN, "mcp"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)


def send(msg):
    p.stdin.write(json.dumps(msg) + "\n")
    p.stdin.flush()


def recv():
    line = p.stdout.readline()
    if not line:
        raise SystemExit("server closed stdout")
    return json.loads(line)


def call(cid, name, args):
    send({"jsonrpc": "2.0", "id": cid, "method": "tools/call", "params": {"name": name, "arguments": args}})


send({"jsonrpc": "2.0", "id": 0, "method": "initialize",
      "params": {"protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": "smoke", "version": "1"}}})
init = recv()
assert init["result"]["serverInfo"]["name"] == "patina", init
assert "instructions" in init["result"]
send({"jsonrpc": "2.0", "method": "notifications/initialized"})
send({"jsonrpc": "2.0", "id": 1, "method": "tools/list"})
tools = [t["name"] for t in recv()["result"]["tools"]]
print("tools:", ", ".join(tools))
for required in ("inspect", "new_project", "edit", "render", "variants", "export", "library", "batch"):
    assert required in tools, required

# create projects (sequentially, they are needed by the concurrent calls below)
assets = ["crate", "barrel", "suzanne"]
for i, a in enumerate(assets):
    call(100 + i, "new_project", {"project": f"{WORK}/{a}.patina.json", "mesh": f"{ASSETS}/{a}.glb",
                                  "smart": "painted_metal", "overwrite": True, "resolution": 512})
    r = recv()
    assert not r["result"]["isError"], r

# fire several calls at once; responses may arrive in any order
t0 = time.time()
calls = {
    10: ("render", {"project": f"{WORK}/crate.patina.json", "views": "iso", "size": 256}),
    11: ("render", {"project": f"{WORK}/barrel.patina.json", "views": "iso", "size": 256}),
    12: ("render", {"project": f"{WORK}/suzanne.patina.json", "views": "front", "size": 256, "mode": "clay"}),
    13: ("library", {"topic": "guide"}),
    14: ("validate", {"project": f"{WORK}/barrel.patina.json"}),
    15: ("edit", {"project": f"{WORK}/does_not_exist.patina.json", "ops": []}),  # expected error
    16: ("variants", {"project": f"{WORK}/crate.patina.json", "size": 128, "variants": [
        {"label": "blue", "ops": [{"op": "update", "id": "painted_metal", "patch": {"params": {"color": "#224488"}}}]},
        {"label": "orange", "ops": [{"op": "update", "id": "painted_metal", "patch": {"params": {"color": "#dd7722", "wear": 0.8}}}]}]}),
    # USD is written by LightUSD, which must not print to stdout (it is the protocol) or run in two calls at once
    17: ("export", {"project": f"{WORK}/crate.patina.json", "preset": "gltf", "usd": "usdc"}),
    18: ("export", {"project": f"{WORK}/barrel.patina.json", "preset": "gltf", "usd": "usdz"}),
}
for cid, (name, args) in calls.items():
    call(cid, name, args)
results = {}
while len(results) < len(calls):
    r = recv()
    results[r["id"]] = r
    kinds = [c["type"] for c in r["result"]["content"]]
    print(f"  id {r['id']:>2} {calls[r['id']][0]:<9} isError={r['result']['isError']!s:<5} content={kinds} t={time.time() - t0:.2f}s")
for i in (10, 11, 12, 13, 14, 16):
    assert not results[i]["result"]["isError"], results[i]
assert results[15]["result"]["isError"]
for i in (10, 11, 12, 16):
    assert results[i]["result"]["content"][1]["type"] == "image"
for i in (17, 18):
    text = results[i]["result"]["content"][0]["text"]
    if results[i]["result"]["isError"]:
        assert "no USD export" in text, text  # a build without the LightUSD submodule
    else:
        assert json.loads(text)["usd"]["meshes"] > 0, text
p.stdin.close()
p.wait(timeout=30)
print("MCP smoke test passed")
