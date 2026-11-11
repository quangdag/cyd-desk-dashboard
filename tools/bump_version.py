#!/usr/bin/env python3
"""
Set the version everywhere it is written, so the Device page, the server and the web flasher agree.

    python tools/bump_version.py 2.0.4

Files: ESP32_tft_clock_weather.ino (FW_VERSION), pc_server/claude_usage_server.py (VERSION), web/manifest.json.
The release workflow also stamps manifest.json from the tag; this keeps the repo copy in step.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TARGETS = [
    ("ESP32_tft_clock_weather/ESP32_tft_clock_weather.ino", r'(#define FW_VERSION ")[^"]*(")'),
    ("pc_server/claude_usage_server.py", r'(^VERSION = ")[^"]*(")'),
    ("web/manifest.json", r'("version": ")[^"]*(")'),
]


def main() -> int:
    if len(sys.argv) != 2 or not re.fullmatch(r"\d+\.\d+\.\d+", sys.argv[1]):
        print(__doc__)
        return 2
    version = sys.argv[1]
    for rel, pattern in TARGETS:
        path = ROOT / rel
        with open(path, encoding="utf-8", newline="") as f:   # keep the file's own line endings
            text = f.read()
        new, n = re.subn(pattern, lambda m: m.group(1) + version + m.group(2), text, count=1, flags=re.M)
        if n != 1:
            print(f"{rel}: version string not found")
            return 1
        with open(path, "w", encoding="utf-8", newline="") as f:
            f.write(new)
        print(f"{rel}: {version}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
