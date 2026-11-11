#!/usr/bin/env python3
"""
Is the PC server running, and will it survive? (Windows)

    python tools/server_status.py [PORT]        default port 7842

Prints the process listening on the port and whether it sits inside a job object. A server inside a job (started
from a Claude Code hook before ensure_server.py learnt to leave the job) stops when that Claude session closes, and
the CYD then shows OFFLINE: restart it with `make server-restart`.
"""
import ctypes
import subprocess
import sys
from ctypes import wintypes
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def listener_pid(port: int) -> int:
    out = subprocess.run(["powershell", "-NoProfile", "-Command",
                          f"(Get-NetTCPConnection -LocalPort {port} -State Listen -ErrorAction SilentlyContinue)"
                          ".OwningProcess"], capture_output=True, text=True).stdout.split()
    return int(out[0]) if out else 0


def pid_in_job(pid: int):
    k = ctypes.WinDLL("kernel32")
    k.OpenProcess.restype = wintypes.HANDLE
    k.IsProcessInJob.argtypes = [wintypes.HANDLE, wintypes.HANDLE, ctypes.POINTER(wintypes.BOOL)]
    h = k.OpenProcess(0x1000, False, pid)  # PROCESS_QUERY_LIMITED_INFORMATION
    if not h:
        return None
    result = wintypes.BOOL()
    ok = k.IsProcessInJob(h, None, ctypes.byref(result))
    k.CloseHandle(h)
    return bool(result.value) if ok else None


def main() -> int:
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 7842
    pid = listener_pid(port)
    if not pid:
        print(f"server: NOT running on port {port} - the CYD shows OFFLINE. Start it: make server-restart")
        return 1
    job = pid_in_job(pid)
    print(f"server: running, pid {pid}, port {port}")
    if job:
        print("        inside a job: it stops when the program that started it closes. Fix: make server-restart")
    elif job is False:
        print("        independent of Claude Code: keeps running until logout")
    log = ROOT / "pc_server" / "server.log"
    if log.exists():
        lines = log.read_text(encoding="utf-8", errors="replace").splitlines()
        starts = [l for l in lines if "--- start" in l or "ensure_server: started" in l][-2:]
        print("log:   ", "\n        ".join(starts + lines[-2:]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
