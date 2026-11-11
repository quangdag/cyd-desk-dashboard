#!/usr/bin/env python3
"""
Claude Code PermissionRequest hook -> answer permission prompts on the CYD while you're away.

Claude Code runs this right before it would ask you for permission and pipes the request on stdin.
If you have been away from the keyboard and mouse for a minute and a CYD is online, the request
shows on the CYD with Deny / Allow; your answer is returned to Claude Code. In every other case
(you're at the PC, no CYD, no answer within 90 s, you come back, server not running) this prints
nothing, and Claude Code shows its normal prompt - the CYD can only answer, never decide on its own.

    settings.json: "PermissionRequest": [{"hooks": [{"type": "command", "timeout": 120,
                    "command": "python", "args": ["-I", ".../pc_server/permission.py"]}]}]
"""
import json
import os
import re
import sys
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
SECRETS = HERE.parent / "ESP32_tft_clock_weather" / "secrets.h"
SERVER = "http://127.0.0.1:7842"
WAIT_S = 90
SKIP_TOOLS = {"AskUserQuestion", "ExitPlanMode"}  # need your own input, not a yes / no


def token() -> str:
    t = os.environ.get("CLAUDE_USAGE_TOKEN", "")
    if t:
        return t
    try:
        m = re.search(r'CLAUDE_TOKEN\[\]\s*=\s*"([^"]*)"', SECRETS.read_text(encoding="utf-8"))
        if m and m.group(1):
            return m.group(1)
    except OSError:
        pass
    try:
        return (HERE / "token.txt").read_text(encoding="utf-8").strip()
    except OSError:
        return ""


def describe(tool: str, inp: dict) -> str:
    """One short line for the CYD card."""
    if not isinstance(inp, dict):
        return ""
    if tool == "Bash":
        return " ".join(str(inp.get("command") or "").split())
    for key in ("file_path", "notebook_path", "path"):
        if inp.get(key):
            return f"{Path(str(inp[key])).name}  ({Path(str(inp[key])).parent.name})"
    if inp.get("url"):
        return str(inp["url"])
    return json.dumps(inp, ensure_ascii=False)[:160]


def post(path: str, body: dict, timeout: float) -> dict:
    req = urllib.request.Request(SERVER + path, data=json.dumps(body).encode(), method="POST",
                                 headers={"Content-Type": "application/json", "Authorization": f"Bearer {token()}"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.loads(r.read().decode())


def main():
    try:
        hook = json.loads(sys.stdin.buffer.read().decode("utf-8", "replace") or "{}")
    except ValueError:
        return
    tool = str(hook.get("tool_name") or "")
    if not tool or tool in SKIP_TOOLS:
        return
    project = Path(hook.get("cwd") or os.getcwd()).name
    detail = describe(tool, hook.get("tool_input") or {})
    opened = post("/v1/permission/request", {"tool": tool, "project": project, "detail": detail[:160],
                                             "timeout_s": WAIT_S}, timeout=3)
    pid = opened.get("id")
    if not pid:
        return  # you're at the PC, or no CYD online -> normal prompt
    decision = post("/v1/permission/wait", {"id": pid, "timeout_s": WAIT_S}, timeout=WAIT_S + 10).get("decision")
    if decision not in ("allow", "deny"):
        return
    out = {"behavior": decision}
    if decision == "deny":
        out["message"] = "Denied from the CYD desk display."
    print(json.dumps({"hookSpecificOutput": {"hookEventName": "PermissionRequest", "decision": out}}))


if __name__ == "__main__":
    try:
        main()
    except Exception:
        pass  # server not running etc. -> no decision, Claude Code asks as usual
    sys.exit(0)
