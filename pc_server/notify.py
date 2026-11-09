#!/usr/bin/env python3
"""
Claude Code hook -> CYD notification.

Claude Code runs this on its "Stop" (finished a turn) and "Notification" (waiting
for you / needs permission) hooks and pipes the hook JSON on stdin. We forward a
short event to the local claude_usage_server, which pushes it to the CYD.

    python notify.py stop | notification

Never blocks or breaks Claude Code: short timeout, prints nothing, always exits 0.
"""
import json
import os
import re
import sys
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
SECRETS = HERE.parent / "ESP32_tft_clock_weather" / "secrets.h"


def main():
    kind = sys.argv[1] if len(sys.argv) > 1 else "stop"
    try:
        # Claude Code sends UTF-8; Windows' default stdin codec (cp1252) would garble Vietnamese
        hook = json.loads(sys.stdin.buffer.read().decode("utf-8", "replace") or "{}")
    except ValueError:
        hook = {}
    if hook.get("stop_hook_active"):
        return  # Claude is continuing because of another Stop hook - not really done
    project = Path(hook.get("cwd") or os.getcwd()).name
    message = hook.get("message") or ""

    token = os.environ.get("CLAUDE_USAGE_TOKEN", "")
    if not token:
        try:
            m = re.search(r'CLAUDE_TOKEN\[\]\s*=\s*"([^"]*)"', SECRETS.read_text(encoding="utf-8"))
            token = m.group(1) if m else ""
        except OSError:
            pass

    body = json.dumps({"type": kind, "project": project, "message": message}).encode()
    req = urllib.request.Request("http://127.0.0.1:7842/v1/event", data=body, method="POST",
                                 headers={"Content-Type": "application/json", "Authorization": f"Bearer {token}"})
    urllib.request.urlopen(req, timeout=1.5).read()


if __name__ == "__main__":
    try:
        main()
    except Exception:
        pass  # server not running etc. - stay silent
    sys.exit(0)
