#!/usr/bin/env python3
"""
Check the commits you are about to push for private values before `git push` (the repo is public).

    python tools/secret_scan.py [RANGE]        default range: origin/main..HEAD

Compares every added line against the real values in the gitignored files (secrets.h Wi-Fi name, password
and token, pc_server/token.txt, pc_server/calendar.txt) and a few generic key patterns. Prints only which
kind matched, never the value. Exit code 1 if anything is found.
"""
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def git(*args: str) -> str:
    return subprocess.run(["git", "-C", str(ROOT), *args], capture_output=True, text=True,
                          encoding="utf-8", errors="replace").stdout


def private_values() -> dict:
    vals = {}
    secrets = ROOT / "ESP32_tft_clock_weather" / "secrets.h"
    if secrets.exists():
        text = secrets.read_text(encoding="utf-8")
        for key in ("WIFI_SSID", "WIFI_PASSWORD", "CLAUDE_TOKEN"):
            m = re.search(key + r'\[\]\s*=\s*"([^"]*)"', text)
            if m and len(m.group(1)) >= 4:
                vals[f"secrets.h {key}"] = m.group(1)
    token = ROOT / "pc_server" / "token.txt"
    if token.exists() and len(token.read_text(encoding="utf-8").strip()) >= 4:
        vals["token.txt"] = token.read_text(encoding="utf-8").strip()
    cal = ROOT / "pc_server" / "calendar.txt"
    if cal.exists():
        url = cal.read_text(encoding="utf-8").strip().splitlines()[0] if cal.read_text(encoding="utf-8").strip() else ""
        if url:
            vals["calendar.txt URL"] = url
            m = re.search(r"private-[0-9a-f]+", url)
            if m:
                vals["calendar private id"] = m.group(0)
    return vals


GENERIC = re.compile(r"(sk-ant-[A-Za-z0-9_-]{8}|ghp_[A-Za-z0-9]{20}|github_pat_[A-Za-z0-9_]{20}|"
                     r"AKIA[0-9A-Z]{16}|private-[0-9a-f]{20,}|-----BEGIN [A-Z ]*PRIVATE KEY-----)")
FORBIDDEN = re.compile(r"(^|/)(secrets\.h|token\.txt|calendar\.txt|calibration\.json|server\.log|\.credentials\.json)$")


def main() -> int:
    rng = sys.argv[1] if len(sys.argv) > 1 else "origin/main..HEAD"
    files = git("diff", "--name-only", rng.replace("..", "...", 1) if "..." not in rng else rng).split()
    added = "\n".join(l for l in git("log", "-p", rng).splitlines() if l.startswith("+") and not l.startswith("+++"))
    hits = [name for name, v in private_values().items() if v in added]
    generic = sorted({m.group(0)[:12] + "..." for m in GENERIC.finditer(added)})
    forbidden = [f for f in files if FORBIDDEN.search(f)]
    print(f"range {rng}: {len(files)} files changed")
    print("private values:", ", ".join(hits) or "none")
    print("key patterns:  ", ", ".join(generic) or "none")
    print("private files: ", ", ".join(forbidden) or "none")
    if hits or generic or forbidden:
        print("NOT safe to push")
        return 1
    print("OK to push")
    return 0


if __name__ == "__main__":
    sys.exit(main())
