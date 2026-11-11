#!/usr/bin/env python3
"""
Start claude_usage_server.py in the background unless it is already running. Safe to call any number
of times: used by the Windows Startup shortcut (at login) and by a Claude Code SessionStart hook (every
time Claude Code starts), so the CYD always has its server.

    pythonw ensure_server.py [server options...]      e.g. --plan pro --reset-weekday 5 --reset-hour 2

Options are passed through to the server. Without --log, the server logs to server.log next to it.

Windows: a hook runs inside the Claude app's job object, and Windows stops every process in a job when the job
closes - so a server started from there would stop with that Claude session (the CYD then shows OFFLINE).
The server must therefore start outside the job: breakaway when the job allows it, otherwise through WMI
(Win32_Process.Create), whose processes start in the same desktop session but belong to no job.
"""
import os
import socket
import subprocess
import sys
from datetime import datetime
from pathlib import Path

HERE = Path(__file__).resolve().parent
SERVER = HERE / "claude_usage_server.py"

DETACHED = 0x00000008 | 0x00000200 | 0x08000000   # DETACHED_PROCESS | NEW_PROCESS_GROUP | CREATE_NO_WINDOW
BREAKAWAY = 0x01000000                            # CREATE_BREAKAWAY_FROM_JOB
NO_WINDOW = 0x08000000
# The command line travels in environment variables, so paths with spaces or quotes need no PowerShell escaping.
WMI_CREATE = ("$r = Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments "
              "@{CommandLine = $env:CYD_CMD; CurrentDirectory = $env:CYD_CWD}; exit [int]$r.ReturnValue")


def port_from(args: list[str]) -> int:
    if "--port" in args:
        try:
            return int(args[args.index("--port") + 1])
        except (IndexError, ValueError):
            pass
    return 7842


def running(port: int) -> bool:
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=0.5):
            return True
    except OSError:
        return False


def in_job() -> bool:
    """True when this process belongs to a Windows job object (e.g. started by a Claude Code hook)."""
    import ctypes
    from ctypes import wintypes
    kernel32 = ctypes.WinDLL("kernel32")
    # 64-bit handles: without these declarations ctypes passes the pseudo-handle as a 32-bit int (error 6)
    kernel32.GetCurrentProcess.restype = wintypes.HANDLE
    kernel32.IsProcessInJob.argtypes = [wintypes.HANDLE, wintypes.HANDLE, ctypes.POINTER(wintypes.BOOL)]
    result = wintypes.BOOL()
    if not kernel32.IsProcessInJob(kernel32.GetCurrentProcess(), None, ctypes.byref(result)):
        return True  # unknown: assume the worst so the launcher tries to leave the job
    return bool(result.value)


def wmi_create(cmd: list[str], cwd: Path) -> bool:
    """Start cmd through WMI: same user and desktop session, but in no job. False if WMI is unavailable."""
    env = dict(os.environ, CYD_CMD=subprocess.list2cmdline(cmd), CYD_CWD=str(cwd))
    try:
        r = subprocess.run(["powershell", "-NoProfile", "-NonInteractive", "-Command", WMI_CREATE], env=env,
                           creationflags=NO_WINDOW, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL, timeout=30)
    except (OSError, subprocess.TimeoutExpired):
        return False
    return r.returncode == 0


def popen_detached(cmd: list[str], cwd: Path, extra_flags: int = 0):
    subprocess.Popen(cmd, cwd=cwd, creationflags=DETACHED | extra_flags, close_fds=True,
                     stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def launch_windows(cmd: list[str], cwd: Path, job=in_job, popen=popen_detached, wmi=wmi_create) -> str:
    """Start the server so it outlives whatever started this script. Returns how it was started."""
    if not job():
        popen(cmd, cwd)
        return "direct"
    try:
        popen(cmd, cwd, BREAKAWAY)
        return "breakaway from job"
    except OSError:
        pass  # the job forbids breakaway (the Claude app's job does)
    if wmi(cmd, cwd):
        return "WMI (outside the job)"
    popen(cmd, cwd)
    return "inside the job: stops when the program that started it closes"


def log_line(args: list[str], text: str):
    try:
        path = Path(args[args.index("--log") + 1])
        with open(path, "a", encoding="utf-8") as f:
            f.write(f"{datetime.now():%H:%M:%S} ensure_server: started {text}\n")
    except (ValueError, IndexError, OSError):
        pass


def main():
    args = sys.argv[1:]
    if running(port_from(args)):
        return
    if "--log" not in args:
        args += ["--log", str(HERE / "server.log")]
    exe = Path(sys.executable)
    pythonw = exe.with_name("pythonw.exe")  # no console window
    cmd = [str(pythonw if pythonw.exists() else exe), str(SERVER), *args]
    if os.name == "nt":
        log_line(args, launch_windows(cmd, HERE))
    else:
        subprocess.Popen(cmd, cwd=HERE, start_new_session=True,
                         stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


if __name__ == "__main__":
    main()
