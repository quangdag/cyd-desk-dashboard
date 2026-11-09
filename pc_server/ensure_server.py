#!/usr/bin/env python3
"""
Start claude_usage_server.py in the background unless it is already running. Safe to call any number
of times: used by the Windows Startup shortcut (at login) and by a Claude Code SessionStart hook (every
time Claude Code starts), so the CYD always has its server.

    pythonw ensure_server.py [server options...]      e.g. --plan pro --reset-weekday 5 --reset-hour 2

Options are passed through to the server. Without --log, the server logs to server.log next to it.
"""
import os
import socket
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
SERVER = HERE / "claude_usage_server.py"


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
        # Detached and out of the caller's job, so it outlives the hook / login script that started it
        flags = 0x00000008 | 0x00000200 | 0x08000000  # DETACHED_PROCESS | NEW_PROCESS_GROUP | NO_WINDOW
        for extra in (0x01000000, 0):                  # CREATE_BREAKAWAY_FROM_JOB if the job allows it
            try:
                subprocess.Popen(cmd, cwd=HERE, creationflags=flags | extra, close_fds=True,
                                 stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                return
            except OSError:
                continue
    else:
        subprocess.Popen(cmd, cwd=HERE, start_new_session=True,
                         stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


if __name__ == "__main__":
    main()
