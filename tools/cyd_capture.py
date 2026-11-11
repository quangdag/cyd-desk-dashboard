#!/usr/bin/env python3
"""
Screenshots and health numbers from a running CYD over USB (no camera, pixel-exact).

    python cyd_capture.py COM8 shot out.png          current screen -> PNG (2x upscaled)
    python cyd_capture.py COM8 tab 4                  open a page (ScreenState number, 0 = Home)
    python cyd_capture.py COM8 stats                  heap, Wi-Fi, frame time, latencies (JSON)
    python cyd_capture.py COM8 tour DIR [--gif demo.gif]
                                                      every page -> DIR/<name>.png, optional animated GIF

The firmware answers SHOT by reading the panel back line by line (ILI9341 GRAM) at 921600 baud.
Opening the port does not reset the board (DTR/RTS are held inactive). Needs: pip install pyserial pillow
"""
import argparse
import json
import sys
import time
from pathlib import Path

import serial
from PIL import Image

PAGES = [  # (ScreenState, file name)
    (0, "home"), (1, "clock"), (2, "weather"), (3, "forecast"), (4, "claude"), (5, "account"),
    (6, "music"), (7, "system"), (8, "pomodoro"), (9, "settings"), (10, "device"),
]


def open_port(port: str) -> serial.Serial:
    s = serial.Serial()
    s.port, s.baudrate, s.timeout = port, 115200, 2
    s.dtr = s.rts = False  # keep EN / IO0 alone: no reset
    s.open()
    time.sleep(0.2)
    s.reset_input_buffer()
    return s


def command(s: serial.Serial, cmd: str, expect: str, timeout: float = 5) -> str:
    s.write((cmd + "\n").encode())
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        line = s.readline().decode("utf-8", "replace").strip()
        if line.startswith(expect):
            return line
    raise TimeoutError(f"no {expect!r} reply to {cmd!r}")


def shot(s: serial.Serial) -> Image.Image:
    head = command(s, "SHOT", "SHOT ")
    _, w, h, baud = head.split()
    w, h = int(w), int(h)
    s.baudrate = int(baud)
    need = w * h * 2
    data = bytearray()
    end = time.monotonic() + 20
    while len(data) < need and time.monotonic() < end:
        data += s.read(need - len(data))
    s.baudrate = 115200
    time.sleep(0.3)
    s.reset_input_buffer()
    if len(data) < need:
        raise TimeoutError(f"got {len(data)} of {need} bytes")
    img = Image.new("RGB", (w, h))
    px = []
    for i in range(0, need, 2):
        # TFT_eSPI packs the 3 bytes it reads back as 5-6-5 bits, but this panel (ILI9341_2_DRIVER, BGR)
        # sends them as blue, red, green: fields = b5 | r6 | g6>>1 (measured against the known palette)
        v = (data[i] << 8) | data[i + 1]
        b5, r6, g5 = (v >> 11) & 0x1F, (v >> 5) & 0x3F, v & 0x1F
        r5, g6 = r6 >> 1, (g5 << 1) | (g5 >> 4)
        px.append(((r5 << 3) | (r5 >> 2), (g6 << 2) | (g6 >> 4), (b5 << 3) | (b5 >> 2)))
    img.putdata(px)
    img.paste(img.crop((1, 0, 2, h)), (0, 0))  # column 0 reads back garbled: repeat column 1
    return img


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("port")
    ap.add_argument("action", choices=["shot", "tab", "stats", "tour"])
    ap.add_argument("arg", nargs="?")
    ap.add_argument("--gif", type=Path, help="tour: also write an animated GIF")
    ap.add_argument("--scale", type=int, default=2)
    a = ap.parse_args()
    s = open_port(a.port)
    if a.action == "stats":
        print(json.dumps(json.loads(command(s, "STATS", "{")), indent=2))
    elif a.action == "tab":
        command(s, f"TAB {int(a.arg)}", "OK")
    elif a.action == "shot":
        img = shot(s)
        img.resize((img.width * a.scale, img.height * a.scale), Image.NEAREST).save(a.arg or "cyd.png")
    else:
        out = Path(a.arg or "screens")
        out.mkdir(parents=True, exist_ok=True)
        frames = []
        for page, name in PAGES:
            command(s, f"TAB {page}", "OK")
            time.sleep(2.5 if page in (5, 6) else 1.2)  # let account / music data arrive
            img = shot(s)
            img.resize((img.width * a.scale, img.height * a.scale), Image.NEAREST).save(out / f"{name}.png")
            frames.append(img)
            print(f"{name}.png", file=sys.stderr)
        command(s, "TAB 0", "OK")
        if a.gif:
            pal = [f.convert("P", palette=Image.ADAPTIVE, colors=128) for f in frames]
            pal[0].save(a.gif, save_all=True, append_images=pal[1:], duration=1800, loop=0, optimize=True)
            print(a.gif, file=sys.stderr)


if __name__ == "__main__":
    main()
