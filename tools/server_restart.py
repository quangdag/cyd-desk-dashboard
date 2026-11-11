#!/usr/bin/env python3
"""
Restart the PC server so it re-reads calendar.txt, token.txt or new code (Windows).

    python tools/server_restart.py [server args...]     default: --plan pro

Stops only the python / pythonw process whose command line runs claude_usage_server.py (other Python programs
are left alone), then starts it again through ensure_server.py, which also writes pc_server/server.log.
"""
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FIND = ("Get-CimInstance Win32_Process -Filter \"Name='pythonw.exe' OR Name='python.exe'\" | "
        "Where-Object { $_.CommandLine -match 'claude_usage_server\\.py' } | "
        "ForEach-Object { Stop-Process -Id $_.ProcessId -Force; $_.ProcessId }")


def main() -> int:
    args = sys.argv[1:] or ["--plan", "pro"]
    out = subprocess.run(["powershell", "-NoProfile", "-Command", FIND], capture_output=True, text=True).stdout.split()
    print("stopped:", ", ".join(out) or "nothing running")
    time.sleep(1.5)  # let the port close before the new copy binds it
    subprocess.run([sys.executable, "-I", str(ROOT / "pc_server" / "ensure_server.py"), *args], check=True)
    time.sleep(3)
    log = ROOT / "pc_server" / "server.log"
    if log.exists():
        print("".join(log.read_text(encoding="utf-8", errors="replace").splitlines(keepends=True)[-3:]), end="")
    return 0


if __name__ == "__main__":
    sys.exit(main())
