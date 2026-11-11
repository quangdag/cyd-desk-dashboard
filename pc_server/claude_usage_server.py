#!/usr/bin/env python3
"""
Claude usage server for the ESP32 CYD.

Reads Claude Code's local logs (~/.claude/projects/**/*.jsonl) and serves
GET /v1/stats in the claudometer v1 schema, so the CYD can show session /
weekly / today usage. The idea and numbers follow
https://github.com/krizdingus/claudometer (MIT); its Go daemon only runs on
macOS/Linux, so this is a small standard-library-only port that also runs on
Windows.

Run:
    python claude_usage_server.py --plan max-5x
    python claude_usage_server.py --print        # print one JSON snapshot and exit

The bearer token is read from ../ESP32_tft_clock_weather/secrets.h
(CLAUDE_TOKEN) unless --token or CLAUDE_USAGE_TOKEN is given.

All percentages are ESTIMATES: Anthropic does not publish token caps, and
these caps were reverse-derived by claudometer. Tune with --session-cap /
--weekly-cap if the numbers drift from claude.ai's usage page.
"""
from __future__ import annotations

import argparse
import hmac
import json
import os
import re
import socket
import sys
import threading
import time
from datetime import datetime, timedelta
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse

VERSION = "1.0.0"

# (session block tokens, weekly all-models, weekly opus-only) - from claudometer planinfo.go
PLAN_CAPS = {
    "free": (200_000, 1_000_000, 0),
    "pro": (1_000_000, 4_500_000, 0),
    "max-5x": (5_000_000, 45_000_000, 9_000_000),
    "max-20x": (20_000_000, 180_000_000, 36_000_000),
}

# USD per 1M tokens: input, output, cache read, cache write.
# claudometer's snapshot, matched by model family name so new versions still get a price.
PRICING = {
    "opus": (15.00, 75.00, 1.50, 18.75),
    "sonnet": (3.00, 15.00, 0.30, 3.75),
    "haiku": (1.00, 5.00, 0.10, 1.25),
}

BLOCK = timedelta(hours=5)

# UDP broadcast so the CYD finds this PC even when DHCP hands the laptop a new IP.
BEACON_PORT = 7843
BEACON_INTERVAL_S = 5

# Record tuple layout
K_KEY, K_TS, K_MODEL, K_IN, K_OUT, K_CREAD, K_CWRITE, K_SIDECHAIN, K_PROJECT = range(9)

CONTEXT_MAX_AGE = timedelta(hours=12)   # older main-chat turns -> no "current" session to show


def parse_ts(value):
    if not isinstance(value, str) or not value:
        return None
    try:
        return datetime.fromisoformat(value.replace("Z", "+00:00")).astimezone()
    except ValueError:
        return None


def quota_tokens(r) -> int:
    # Same as claudometer: input + output + cache creation; cache reads don't count toward limits.
    return r[K_IN] + r[K_OUT] + r[K_CWRITE]


def price_of(model: str):
    m = (model or "").lower()
    for family, price in PRICING.items():
        if family in m:
            return price
    return None


def cost_usd(r) -> float:
    p = price_of(r[K_MODEL])
    if p is None:
        return 0.0
    return (r[K_IN] * p[0] + r[K_OUT] * p[1] + r[K_CREAD] * p[2] + r[K_CWRITE] * p[3]) / 1_000_000


class LogStore:
    """Parses JSONL files, caching each one until its size/mtime changes."""

    def __init__(self, root: Path):
        self.root = root
        self._files: dict[Path, tuple[float, int, list]] = {}
        self._lock = threading.Lock()

    @staticmethod
    def _parse_file(path: Path) -> list:
        recs = []
        try:
            with open(path, encoding="utf-8", errors="replace") as f:
                for line in f:
                    if '"usage"' not in line or '"assistant"' not in line:
                        continue
                    try:
                        obj = json.loads(line)
                    except ValueError:
                        continue
                    if obj.get("type") != "assistant":
                        continue
                    msg = obj.get("message") or {}
                    usage = msg.get("usage")
                    if not isinstance(usage, dict):
                        continue
                    ts = parse_ts(obj.get("timestamp"))
                    if ts is None:
                        continue
                    recs.append((
                        (msg.get("id"), obj.get("requestId")),
                        ts,
                        msg.get("model") or "unknown",
                        int(usage.get("input_tokens") or 0),
                        int(usage.get("output_tokens") or 0),
                        int(usage.get("cache_read_input_tokens") or 0),
                        int(usage.get("cache_creation_input_tokens") or 0),
                        bool(obj.get("isSidechain")),          # subagent turn (not the main chat)
                        Path(obj.get("cwd") or "").name,       # project folder
                    ))
        except OSError:
            pass
        return recs

    def records_since(self, since: datetime) -> list:
        cutoff = since.timestamp()
        out, seen_keys, live = [], set(), set()
        with self._lock:
            for path in self.root.rglob("*.jsonl"):
                try:
                    st = path.stat()
                except OSError:
                    continue
                if st.st_mtime < cutoff:
                    continue  # not touched since the window began -> nothing new inside it
                live.add(path)
                cached = self._files.get(path)
                if not cached or cached[0] != st.st_mtime or cached[1] != st.st_size:
                    self._files[path] = (st.st_mtime, st.st_size, self._parse_file(path))
            for path in list(self._files):
                if path not in live:
                    del self._files[path]
            for path in live:
                for r in self._files[path][2]:
                    if r[K_TS] < since:
                        continue
                    key = r[K_KEY]
                    if key != (None, None):
                        # Claude Code writes each turn several times with the same ids.
                        if key in seen_keys:
                            continue
                        seen_keys.add(key)
                    out.append(r)
        out.sort(key=lambda r: r[K_TS])
        return out


def weekly_window(now: datetime, weekday: int, hour: int):
    days = (now.weekday() - weekday) % 7
    start = (now - timedelta(days=days)).replace(hour=hour, minute=0, second=0, microsecond=0)
    if start > now:
        start -= timedelta(days=7)
    return start, start + timedelta(days=7)


def fmt_duration(td: timedelta) -> str:
    mins = max(int(td.total_seconds() // 60), 0)
    d, rem = divmod(mins, 24 * 60)
    h, m = divmod(rem, 60)
    if d:
        return f"{d}d{h}h"
    if h:
        return f"{h}h{m:02d}m"
    return f"{m}m"


def pct(used: float, cap: float) -> int:
    if cap <= 0:
        return 0
    return max(0, min(int(round(used * 100 / cap)), 100))


# ---- Calibration -------------------------------------------------------------
# Raw token caps drift from claude.ai because limits weigh models differently
# (an Opus token costs far more than a Haiku token). After calibration the
# server measures usage in API-equivalent dollars and divides by a dollar cap
# taken from what claude.ai showed: cap = cost_now / (pct_on_claude_ai / 100).

_cal_cache: dict = {"mtime": None, "data": {}}


def load_calibration(path: Path) -> dict:
    try:
        mtime = path.stat().st_mtime
    except OSError:
        return {}
    if _cal_cache["mtime"] != mtime:  # re-read when the file changes; no restart needed
        try:
            _cal_cache["data"] = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            _cal_cache["data"] = {}
        _cal_cache["mtime"] = mtime
    return _cal_cache["data"]


def usage_now(store: LogStore, args) -> dict:
    """Current session block and weekly window, in tokens and API-equivalent USD."""
    now = datetime.now().astimezone()
    week_start, week_end = weekly_window(now, args.reset_weekday, args.reset_hour)
    midnight = now.replace(hour=0, minute=0, second=0, microsecond=0)
    recs = [r for r in store.records_since(min(week_start, midnight) - timedelta(days=1)) if r[K_TS] <= now]

    # 5-hour blocks: a block starts at the first message after the previous block ended.
    blocks = []
    for r in recs:
        if not blocks or r[K_TS] >= blocks[-1]["end"]:
            blocks.append({"start": r[K_TS], "end": r[K_TS] + BLOCK, "tokens": 0, "cost": 0.0, "models": {}})
        b = blocks[-1]
        q = quota_tokens(r)
        b["tokens"] += q
        b["cost"] += cost_usd(r)
        b["models"][r[K_MODEL]] = b["models"].get(r[K_MODEL], 0) + q
    active = blocks[-1] if blocks and blocks[-1]["start"] <= now < blocks[-1]["end"] else None

    week = [r for r in recs if week_start <= r[K_TS] < week_end]
    return {
        "now": now, "midnight": midnight, "recs": recs, "active": active, "week_end": week_end,
        "week_tokens": sum(quota_tokens(r) for r in week),
        "week_opus_tokens": sum(quota_tokens(r) for r in week if "opus" in r[K_MODEL].lower()),
        "week_cost": sum(cost_usd(r) for r in week),
    }


def calibrate(store: LogStore, args, session_pct: float, week_pct: float):
    u = usage_now(store, args)
    data = dict(load_calibration(args.calibration))
    if session_pct > 0:
        if not u["active"] or u["active"]["cost"] <= 0:
            sys.exit("No active session in the logs right now - calibrate while a session is running.")
        data["session_cap_usd"] = u["active"]["cost"] * 100 / session_pct
    if week_pct > 0:
        if u["week_cost"] <= 0:
            sys.exit("No usage in the current weekly window - check --reset-weekday/--reset-hour.")
        data["weekly_cap_usd"] = u["week_cost"] * 100 / week_pct
    data["calibrated_at"] = u["now"].isoformat(timespec="seconds")
    data["calibrated_with"] = {"session_pct": session_pct, "week_pct": week_pct}
    args.calibration.write_text(json.dumps(data, indent=2), encoding="utf-8")
    print(f"Saved {args.calibration}")
    print(json.dumps(data, indent=2))


# ---- Exact usage from Anthropic ----------------------------------------------
# Same source as Claude Code's /usage: GET api.anthropic.com/api/oauth/usage with the
# CLI's own login (~/.claude/.credentials.json, written by `claude auth login`).
# The token is only ever sent to Anthropic. Between fetches the % is extended in
# real time from local log costs, using the calibrated dollar caps.

USAGE_URL = "https://api.anthropic.com/api/oauth/usage"
CLI_REFRESH_MIN_S = 600   # after a failed CLI refresh, wait this long before trying again
EXACT_MAX_AGE_S = 30 * 60                                      # older than this -> fall back to estimate
BACKOFF_MIN_S, BACKOFF_MAX_S = 120, 15 * 60                    # after HTTP 429 (rate limited)


def _http_json(url: str, method: str = "GET", headers: dict | None = None, body: dict | None = None):
    import urllib.request
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(url, data=data, method=method, headers=headers or {})
    with urllib.request.urlopen(req, timeout=15) as r:
        return json.loads(r.read().decode())


def _parse_iso(value):
    if not isinstance(value, str) or not value:
        return None
    try:
        return datetime.fromisoformat(value.replace("Z", "+00:00")).astimezone()
    except ValueError:
        return None


class ExactUsage:
    def __init__(self, creds_path: Path, interval_s: int):
        self.creds_path = creds_path
        self.interval_s = interval_s
        self.lock = threading.Lock()
        self.busy = False
        self.last_try = 0.0
        self.snapshot = None          # dict with pct / resets / fetched_at
        self.last_error = ""
        self.block_until = 0.0        # monotonic time before which we don't ask again (rate limit)
        self.backoff = 0
        self.cli_refresh_at = -CLI_REFRESH_MIN_S

    # -- credentials --
    def _read_oauth(self) -> dict:
        try:
            return json.loads(self.creds_path.read_text(encoding="utf-8")).get("claudeAiOauth") or {}
        except (OSError, ValueError):
            return {}

    def _refresh(self) -> dict:
        """Let the official Claude Code CLI renew its own login: `claude auth status` refreshes an
        expired access token and writes it back to .credentials.json. The server never calls the
        token endpoint itself. Throttled, so a logged-out CLI is not spawned every poll."""
        import shutil
        import subprocess
        now = time.monotonic()
        if now - self.cli_refresh_at < CLI_REFRESH_MIN_S:
            raise RuntimeError("Claude login expired - run: claude auth login")
        self.cli_refresh_at = now
        exe = shutil.which("claude") or str(Path.home() / ".local" / "bin" / "claude.exe")
        try:
            subprocess.run([exe, "auth", "status"], stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL, timeout=60, creationflags=0x08000000)  # CREATE_NO_WINDOW
        except (OSError, subprocess.SubprocessError) as exc:
            raise RuntimeError(f"cannot run the Claude CLI ({exc}) - run: claude auth login") from exc
        oauth = self._read_oauth()
        if oauth.get("expiresAt", 0) / 1000 - time.time() < 60:
            raise RuntimeError("Claude login expired - run: claude auth login")
        self.cli_refresh_at = 0.0  # worked: allow the next refresh whenever it is due
        print(f"{datetime.now():%H:%M:%S} Claude CLI refreshed its login token", file=sys.stderr)
        return oauth

    def _token(self) -> str:
        oauth = self._read_oauth()
        if not oauth.get("accessToken"):
            raise RuntimeError("no Claude login - run: claude auth login")
        if oauth.get("expiresAt", 0) / 1000 - time.time() < 300:
            oauth = self._refresh()
        return oauth["accessToken"]

    # -- fetching --
    def _fetch(self):
        import urllib.error
        try:
            for attempt in range(2):
                headers = {"Authorization": f"Bearer {self._token()}", "anthropic-beta": "oauth-2025-04-20",
                           "Accept": "application/json", "User-Agent": f"cyd-usage-server/{VERSION}"}
                try:
                    data = _http_json(USAGE_URL, headers=headers)
                    break
                except urllib.error.HTTPError as e:
                    if e.code == 401 and attempt == 0:  # revoked early: let the CLI renew it once
                        self._refresh()
                        continue
                    raise
            fh, sd = data.get("five_hour") or {}, data.get("seven_day") or {}
            rows = (data.get("seven_day_breakdown") or {}).get("rows") or []
            extra = data.get("extra_usage") or {}
            snap = {
                "fetched_at": datetime.now().astimezone(),
                "session_pct": float(fh.get("utilization") or 0),
                "session_resets": _parse_iso(fh.get("resets_at")),
                "week_pct": float(sd.get("utilization") or 0),
                "week_resets": _parse_iso(sd.get("resets_at")),
                # Share of this week's usage per surface (Claude Code / Chats / Cowork / Other)
                "breakdown": [{"key": str(r.get("key") or ""), "name": str(r.get("display_name") or r.get("key") or ""),
                               "percent": int(r.get("percent") or 0)} for r in rows if isinstance(r, dict)][:5],
                "extra": {"enabled": bool(extra.get("is_enabled")), "used": float(extra.get("used_credits") or 0),
                          "limit": float(extra.get("monthly_limit") or 0) / 100,
                          "utilization": float(extra.get("utilization") or 0)},
            }
            self.backoff = 0
            with self.lock:
                first = self.snapshot is None or self.last_error
                self.snapshot, self.last_error = snap, ""
            if first:
                print(f"{datetime.now():%H:%M:%S} exact usage OK: session {snap['session_pct']:.0f}%, "
                      f"week {snap['week_pct']:.0f}%", file=sys.stderr)
        except Exception as exc:  # network down, logged out, endpoint changed -> keep estimating
            if getattr(exc, "code", None) == 429:  # rate limited: wait longer each time (2 -> 15 min)
                self.backoff = min(max(self.backoff * 2, BACKOFF_MIN_S), BACKOFF_MAX_S)
                try:
                    retry_after = int(exc.headers.get("Retry-After", 0))
                except (TypeError, ValueError):
                    retry_after = 0
                self.block_until = time.monotonic() + max(self.backoff, retry_after)
            msg = f"{type(exc).__name__}: {exc}"
            if msg != self.last_error:
                print(f"{datetime.now():%H:%M:%S} exact usage unavailable ({msg}) - using estimate", file=sys.stderr)
            self.last_error = msg
        finally:
            self.busy = False

    def poke(self):
        """Called on each CYD request: refresh in the background at most every interval_s.
        No CYD polling (laptop away from home) -> no calls to Anthropic."""
        now = time.monotonic()
        if self.busy or now - self.last_try < self.interval_s or now < self.block_until:
            return
        self.busy, self.last_try = True, now
        threading.Thread(target=self._fetch, daemon=True).start()

    def current(self):
        with self.lock:
            s = self.snapshot
        if not s or (datetime.now().astimezone() - s["fetched_at"]).total_seconds() > EXACT_MAX_AGE_S:
            return None
        return s


def compute_stats(store: LogStore, args, exact: ExactUsage | None = None) -> dict:
    u = usage_now(store, args)
    now, midnight, recs, active, week_end = u["now"], u["midnight"], u["recs"], u["active"], u["week_end"]

    session_cap, weekly_cap, opus_cap = PLAN_CAPS[args.plan]
    session_cap = args.session_cap or session_cap
    weekly_cap = args.weekly_cap or weekly_cap
    cal = load_calibration(args.calibration)

    def session_pct(block) -> int:
        if cal.get("session_cap_usd"):
            return pct(block["cost"], cal["session_cap_usd"])
        return pct(block["tokens"], session_cap)

    if cal.get("weekly_cap_usd"):
        week_pct = pct(u["week_cost"], cal["weekly_cap_usd"])
    else:
        week_pct = pct(u["week_tokens"], weekly_cap)

    if active:
        top = sorted(active["models"].items(), key=lambda kv: kv[1], reverse=True)[:3]
        session = {
            "pct_used": session_pct(active),
            "minutes_remaining": int((active["end"] - now).total_seconds() // 60),
            "resets_at": active["end"].strftime("%H:%M"),
            "models": [{"model": m, "tokens": t} for m, t in top],
        }
    else:
        session = {"pct_used": 0, "minutes_remaining": 0, "resets_at": "", "models": []}
    week_resets_in = fmt_duration(week_end - now)

    # Exact numbers from Anthropic override the estimate; local log cost moves them in between fetches.
    snap = exact.current() if exact else None
    if snap:
        def roll(pct0, resets, cap):
            if resets and resets <= now:   # window rolled over since the fetch
                base, since, resets = 0.0, resets, None
            else:
                base, since = pct0, snap["fetched_at"]
            delta = sum(cost_usd(r) for r in recs if r[K_TS] > since)
            return pct(base + (delta * 100 / cap if cap else 0), 100), resets

        s_pct, s_res = roll(snap["session_pct"], snap["session_resets"], cal.get("session_cap_usd"))
        w_pct, w_res = roll(snap["week_pct"], snap["week_resets"], cal.get("weekly_cap_usd"))
        session["pct_used"] = s_pct
        session["resets_at"] = (s_res + timedelta(seconds=30)).strftime("%H:%M") if s_res else ""  # 00:29:59 -> 00:30
        session["minutes_remaining"] = int((s_res - now).total_seconds() // 60) if s_res else 0
        week_pct = w_pct
        if w_res:
            week_resets_in = fmt_duration(w_res - now)

    today = [r for r in recs if r[K_TS] >= midnight]
    by_model: dict[str, int] = {}
    for r in today:
        by_model[r[K_MODEL]] = by_model.get(r[K_MODEL], 0) + quota_tokens(r)

    # Tokens per hour for the last 24 hours (oldest first; last bar = current hour)
    hour0 = now.replace(minute=0, second=0, microsecond=0) - timedelta(hours=23)
    hourly = [0] * 24
    for r in recs:
        if r[K_TS] >= hour0:
            hourly[min(int((r[K_TS] - hour0).total_seconds() // 3600), 23)] += quota_tokens(r)

    # Context window of the most recent main-chat turn = what Claude Code's context meter shows:
    # the whole prompt of that request (fresh input + cache read + cache write).
    context = None
    main = [r for r in recs if not r[K_SIDECHAIN] and now - r[K_TS] <= CONTEXT_MAX_AGE]
    if main:
        last = max(main, key=lambda r: r[K_TS])
        context = {
            "tokens": last[K_IN] + last[K_CREAD] + last[K_CWRITE],
            "limit": args.context_limit,
            "project": last[K_PROJECT][:40],
            "age_min": int((now - last[K_TS]).total_seconds() // 60),
        }

    # Whole-account view (all Claude surfaces share the same plan limits)
    account = None
    if snap:
        account = {"breakdown": snap["breakdown"], "extra": snap["extra"]}

    return {
        "account": account,
        "context": context,
        "activity_24h": {"start_hour": hour0.hour, "tokens": hourly},
        "schema": 1,
        "source": "exact" if snap else "estimate",
        "generated_at": now.isoformat(timespec="seconds"),
        "local_time": now.strftime("%H:%M"),
        "session": session,
        "models_today": {
            "total_tokens": sum(by_model.values()),
            "by_model": [{"model": m, "tokens": t} for m, t in sorted(by_model.items(), key=lambda kv: -kv[1])],
            "est_cost_usd": round(sum(cost_usd(r) for r in today), 2),
        },
        "chat": {"messages_today": 0, "daily_cap": 0, "resets_at": "00:00"},
        "routines": [],
        "budgets": {
            "code_all": week_pct,
            "code_opus": pct(u["week_opus_tokens"], opus_cap),
            "chat": 0,
            "plan": args.plan,
            "resets_in": week_resets_in,
        },
    }


def user_idle_seconds() -> float:
    """Seconds since the last mouse/keyboard input on this PC (0 if unknown / not Windows)."""
    if os.name != "nt":
        return 0.0
    import ctypes

    class LASTINPUTINFO(ctypes.Structure):
        _fields_ = [("cbSize", ctypes.c_uint), ("dwTime", ctypes.c_uint)]

    lii = LASTINPUTINFO()
    lii.cbSize = ctypes.sizeof(lii)
    if not ctypes.windll.user32.GetLastInputInfo(ctypes.byref(lii)):
        return 0.0
    return max(0, (ctypes.windll.kernel32.GetTickCount() - lii.dwTime) & 0xFFFFFFFF) / 1000


# ---- PC stats (only measured while the CYD shows its PC page) ----------------

class ThermalZone:
    """CPU-area temperature from the ACPI thermal zone perf counter (readable without admin).
    `typeperf` streams a sample every second for at most 60 s (-sc 60), so it always exits on
    its own - even if this server is killed - and is restarted on the next request.
    It runs for at most a minute after each request, so it stops when the CYD stops asking."""

    COUNTER = r"\Thermal Zone Information(*)\Temperature"

    def __init__(self):
        self.lock = threading.Lock()
        self.proc = None
        self.value = None
        self.value_ts = 0.0
        self.first = threading.Event()
        self.last_used = 0.0

    def _reader(self, proc):
        import csv
        for row in csv.reader(proc.stdout):
            if time.monotonic() - self.last_used > 60:
                break
            temps = []
            for cell in row[1:]:
                try:
                    k = float(cell)
                except ValueError:
                    continue
                if 250 < k < 400:  # Kelvin, sanity range
                    temps.append(k - 273.15)
            if temps:
                self.value = round(max(temps))
                self.value_ts = time.monotonic()
                self.first.set()
        proc.kill()
        with self.lock:
            if self.proc is proc:
                self.proc = None

    def get(self, wait_s: float = 0.0):
        """Latest reading. A reading up to 10 min old is still returned while the CYD polls slowly
        (Home page, once a minute), so its System tile has a value from boot on. With wait_s, a cold
        start blocks briefly for the first sample instead of answering 'no value'."""
        import subprocess
        self.last_used = time.monotonic()
        with self.lock:
            if self.proc is None:
                try:
                    self.first.clear()
                    self.proc = subprocess.Popen(
                        ["typeperf", self.COUNTER, "-si", "1", "-sc", "60"], stdout=subprocess.PIPE,
                        stderr=subprocess.DEVNULL, text=True, creationflags=0x08000000)  # CREATE_NO_WINDOW
                    threading.Thread(target=self._reader, args=(self.proc,), daemon=True).start()
                except OSError:
                    return None
        if time.monotonic() - self.value_ts > 600:
            if wait_s > 0 and self.first.wait(wait_s):
                return self.value
            return None
        return self.value


class PcStats:
    """CPU / RAM via Win32 API (ctypes, no extra packages) and NVIDIA GPU via nvidia-smi."""

    def __init__(self):
        self.lock = threading.Lock()
        self.prev_cpu = None
        self.gpu = (0.0, None)
        self.cached = (0.0, None)
        self.thermal = ThermalZone()

    @staticmethod
    def _filetime(ft) -> int:
        return (ft.dwHighDateTime << 32) | ft.dwLowDateTime

    def _cpu_pct(self):
        import ctypes
        from ctypes import wintypes
        idle, kern, user = wintypes.FILETIME(), wintypes.FILETIME(), wintypes.FILETIME()
        if not ctypes.windll.kernel32.GetSystemTimes(ctypes.byref(idle), ctypes.byref(kern), ctypes.byref(user)):
            return None
        cur = (self._filetime(idle), self._filetime(kern) + self._filetime(user))  # kernel time includes idle
        prev, self.prev_cpu = self.prev_cpu, cur
        if not prev or cur[1] == prev[1]:
            return None
        return round(100 * (1 - (cur[0] - prev[0]) / (cur[1] - prev[1])))

    @staticmethod
    def _ram():
        import ctypes

        class MEMSTAT(ctypes.Structure):
            _fields_ = [("dwLength", ctypes.c_ulong), ("dwMemoryLoad", ctypes.c_ulong),
                        ("ullTotalPhys", ctypes.c_ulonglong), ("ullAvailPhys", ctypes.c_ulonglong),
                        ("ullTotalPageFile", ctypes.c_ulonglong), ("ullAvailPageFile", ctypes.c_ulonglong),
                        ("ullTotalVirtual", ctypes.c_ulonglong), ("ullAvailVirtual", ctypes.c_ulonglong),
                        ("ullAvailExtendedVirtual", ctypes.c_ulonglong)]
        m = MEMSTAT()
        m.dwLength = ctypes.sizeof(MEMSTAT)
        ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(m))
        gb = 1024 ** 3
        return {"pct": int(m.dwMemoryLoad), "used_gb": round((m.ullTotalPhys - m.ullAvailPhys) / gb, 1),
                "total_gb": round(m.ullTotalPhys / gb, 1)}

    def _gpu(self):
        import subprocess
        ts, val = self.gpu
        if time.monotonic() - ts < 4:
            return val
        val = None
        try:
            out = subprocess.run(
                ["nvidia-smi", "--query-gpu=name,temperature.gpu,utilization.gpu,memory.used,memory.total",
                 "--format=csv,noheader,nounits"],
                capture_output=True, text=True, timeout=3, creationflags=0x08000000)  # CREATE_NO_WINDOW
            name, temp, util, mused, mtotal = [x.strip() for x in out.stdout.strip().splitlines()[0].split(",")]
            val = {"name": name.replace("NVIDIA GeForce ", ""), "temp_c": int(float(temp)), "util_pct": int(float(util)),
                   "mem_used_gb": round(float(mused) / 1024, 1), "mem_total_gb": round(float(mtotal) / 1024, 1)}
        except Exception:
            pass
        self.gpu = (time.monotonic(), val)
        return val

    def get(self):
        if os.name != "nt":
            return None
        with self.lock:
            ts, val = self.cached
            if val is None or time.monotonic() - ts > 2:
                try:
                    self.thermal.get()  # starts typeperf (first sample ~2 s) while the rest is measured
                    if self.prev_cpu is None:  # first request after start: take a short baseline sample
                        self._cpu_pct()
                        time.sleep(0.3)
                    gpu = self._gpu()
                    val = {"cpu_pct": self._cpu_pct(), "cpu_temp_c": self.thermal.get(wait_s=2.8),
                           "ram": self._ram(), "gpu": gpu}
                except Exception:
                    val = None
                self.cached = (time.monotonic(), val)
            return val


# ---- Now playing (Windows media sessions: Spotify, YouTube in Chrome/Edge, ...) ----

THAI_UPPER = set("ัิีึื็")


def thai_marks(text: str) -> str:
    """The CYD draws Thai without shaping, so a tone mark (U+0E48-0E4C) over an upper vowel or before
    SARA AM would sit on top of it. Swap those for the raised copies in the CYD's Thai font (U+F748-F74C)."""
    if not any("่" <= ch <= "์" for ch in text):
        return text
    out = list(text)
    for i, ch in enumerate(out):
        if "่" <= ch <= "์":
            prev_upper = i > 0 and text[i - 1] in THAI_UPPER
            next_am = i + 1 < len(text) and text[i + 1] == "ำ"
            if prev_upper or next_am:
                out[i] = chr(0xF700 + ord(ch) - 0x0E00)
    return "".join(out)


class MediaSessions:
    """Reads and controls whatever app is playing through Windows' global media controls
    (needs: pip install winrt-runtime winrt-Windows.Media.Control winrt-Windows.Foundation).
    Only queried while the CYD shows its Music page."""

    APP_NAMES = {"spotify": "Spotify", "chrome": "Chrome", "msedge": "Edge", "firefox": "Firefox",
                 "zunemusic": "Media Player", "applemusic": "Apple Music", "vlc": "VLC", "foobar": "foobar2000"}

    def __init__(self):
        self.lock = threading.Lock()
        self.cached = (0.0, None)
        self.art = ("", b"")          # (art_id, 80x80 RGB565 bytes) of the current track
        try:
            from winrt.windows.media.control import GlobalSystemMediaTransportControlsSessionManager as mm
            self.mm = mm
        except Exception:
            self.mm = None

    def _app_name(self, aumid: str) -> str:
        low = aumid.lower()
        for key, name in self.APP_NAMES.items():
            if key in low:
                return name
        return aumid.split("!")[0].split(".")[0][:16] or "Media"

    async def _session(self):
        """The session to show: Windows' current one if it is playing, otherwise any session that is
        playing (e.g. a Facebook video in Chrome while Spotify sits paused), otherwise the current one."""
        mgr = await self.mm.request_async()
        cur = mgr.get_current_session()
        if cur and int(cur.get_playback_info().playback_status) == 4:
            return cur
        for s in mgr.get_sessions():
            if int(s.get_playback_info().playback_status) == 4:
                return s
        return cur

    SITE_NAMES = {"facebook": "Facebook", "youtube": "YouTube", "tiktok": "TikTok", "instagram": "Instagram",
                  "soundcloud": "SoundCloud", "netflix": "Netflix", "twitch": "Twitch", "zingmp3": "Zing MP3",
                  "nhaccuatui": "NhacCuaTui"}

    @classmethod
    def _browser_tab_title(cls, exe: str) -> str:
        """Title of the browser's audible-looking window, used when a site (Facebook...) publishes no
        media metadata. Picks the first visible top-level window of that browser that names a known site."""
        import ctypes
        from ctypes import wintypes
        user32, kernel32 = ctypes.windll.user32, ctypes.windll.kernel32
        titles = []

        @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
        def enum(hwnd, _):
            if not user32.IsWindowVisible(hwnd):
                return True
            n = user32.GetWindowTextLengthW(hwnd)
            if n <= 0:
                return True
            pid = wintypes.DWORD()
            user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
            h = kernel32.OpenProcess(0x1000, False, pid.value)  # PROCESS_QUERY_LIMITED_INFORMATION
            if not h:
                return True
            try:
                buf, size = ctypes.create_unicode_buffer(260), wintypes.DWORD(260)
                if kernel32.QueryFullProcessImageNameW(h, 0, buf, ctypes.byref(size)) and                         buf.value.lower().endswith(exe):
                    t = ctypes.create_unicode_buffer(n + 1)
                    user32.GetWindowTextW(hwnd, t, n + 1)
                    titles.append(t.value)
            finally:
                kernel32.CloseHandle(h)
            return True

        try:
            user32.EnumWindows(enum, 0)
        except Exception:
            return ""
        for t in titles:
            if any(k in t.lower() for k in cls.SITE_NAMES):
                return t
        return ""  # no known site in any window title: do not guess from an unrelated tab

    @classmethod
    def _fallback_title(cls, app: str, aumid: str) -> tuple[str, str]:
        """(title, artist) for a session with no metadata. Browsers: tab title without the browser suffix."""
        import re
        exe = {"Chrome": "chrome.exe", "Edge": "msedge.exe", "Firefox": "firefox.exe"}.get(app)
        if not exe:
            return "Playing", app
        t = cls._browser_tab_title(exe)
        t = re.sub(r"\s+[-–—]\s+(Google Chrome|Microsoft​? Edge|Mozilla Firefox).*$", "", t)
        t = re.sub(r"^\(\d+\+?\)\s*", "", t)  # "(3) Facebook" -> "Facebook"
        site = next((name for k, name in cls.SITE_NAMES.items() if k in t.lower()), "")
        if not t or t.lower() == site.lower():
            return (f"{site} video" if site else "Playing in " + app), app
        return t, site or app

    async def _read(self):
        import unicodedata
        s = await self._session()
        if not s:
            return None
        props = await s.try_get_media_properties_async()
        info = s.get_playback_info()
        tl = s.get_timeline_properties()
        playing = int(info.playback_status) == 4
        pos = tl.position.total_seconds()
        if playing and tl.last_updated_time:  # Windows reports the position at its last update
            pos += max(0.0, (datetime.now(tl.last_updated_time.tzinfo) - tl.last_updated_time).total_seconds())
        dur = max(0.0, (tl.end_time - tl.start_time).total_seconds())
        nfc = lambda t: unicodedata.normalize("NFC", t or "")[:80]  # composed Vietnamese for the CYD font
        title, artist = nfc(props.title), nfc(props.artist)
        app = self._app_name(s.source_app_user_model_id or "")
        if not title.strip():  # e.g. Facebook videos in Chrome publish no metadata at all
            title, fb_artist = self._fallback_title(app, s.source_app_user_model_id or "")
            title, artist = nfc(title), artist or nfc(fb_artist)
        title, artist = thai_marks(title), thai_marks(artist)
        art_id = ""
        if props.thumbnail:
            art_id = format(abs(hash((title, artist, s.source_app_user_model_id))) % (1 << 32), "08x")
            if art_id != self.art[0]:
                self.art = (art_id, await self._art_rgb565(props.thumbnail))
            if not self.art[1]:
                art_id = ""
        return {"app": app, "title": title,
                "artist": artist, "playing": playing, "art_id": art_id,
                "pos_s": int(min(pos, dur) if dur else pos), "dur_s": int(dur)}

    ART_SIZE = 80

    async def _art_rgb565(self, ref) -> bytes:
        """Album art -> 80x80 RGB565 little-endian (what LVGL draws directly; no JPEG/PNG decoder on the CYD)."""
        import io
        try:
            from winrt.windows.storage.streams import DataReader
            from PIL import Image
            stream = await ref.open_read_async()
            reader = DataReader(stream.get_input_stream_at(0))
            await reader.load_async(stream.size)
            img = Image.open(io.BytesIO(bytes(reader.read_buffer(stream.size))))
            if self._is_app_icon(img):
                return b""  # browser/app icon, not artwork -> the CYD shows its own music-note frame
            img = img.convert("RGB")
            w, h = img.size  # crop to a centred square (YouTube thumbnails are 16:9)
            side = min(w, h)
            img = img.crop(((w - side) // 2, (h - side) // 2, (w + side) // 2, (h + side) // 2))
            img = img.resize((self.ART_SIZE, self.ART_SIZE), Image.LANCZOS)
            out = bytearray()
            for r, g, b in img.getdata():
                v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
                out += bytes((v & 0xFF, v >> 8))
            return bytes(out)
        except Exception:
            return b""

    @staticmethod
    def _is_app_icon(img) -> bool:
        """Chrome hands over its own logo (transparent background) when a site has no artwork; drawn as
        RGB that becomes a logo in a black square. Treat small or partly transparent images as 'no art'."""
        if min(img.size) < 96:
            return True
        if img.mode in ("RGBA", "LA", "PA") or (img.mode == "P" and "transparency" in img.info):
            alpha = img.convert("RGBA").getchannel("A")
            clear = sum(alpha.histogram()[:128])
            return clear > 0.05 * img.size[0] * img.size[1]
        return False

    def art_bytes(self) -> tuple[str, bytes]:
        return self.art

    def get(self):
        if not self.mm:
            return {"error": "winrt not installed"}
        import asyncio
        with self.lock:
            ts, val = self.cached
            if time.monotonic() - ts > 1:
                try:
                    val = asyncio.run(self._read())
                except Exception as exc:
                    val = {"error": type(exc).__name__}
                self.cached = (time.monotonic(), val)
            return val

    def control(self, action: str) -> bool:
        if not self.mm:
            return False
        import asyncio

        async def run():
            s = await self._session()
            if not s:
                return False
            fn = {"toggle": s.try_toggle_play_pause_async, "next": s.try_skip_next_async,
                  "prev": s.try_skip_previous_async}.get(action)
            return bool(await fn()) if fn else False

        with self.lock:
            self.cached = (0.0, None)  # next read shows the new state
        try:
            return asyncio.run(run())
        except Exception:
            return False


class StatsServer(ThreadingHTTPServer):
    daemon_threads = True
    # On Windows SO_REUSEADDR lets a second copy bind the same port; without it a second start fails
    # cleanly, which ensure_server.py and the Startup shortcut rely on.
    allow_reuse_address = os.name != "nt"

    def __init__(self, addr, handler, store, args, token, exact=None):
        super().__init__(addr, handler)
        self.store, self.args, self.token, self.exact = store, args, token, exact
        self._cache = (0.0, None)
        self._cache_lock = threading.Lock()
        self.pc = PcStats()
        self.media = MediaSessions()
        self.event = None          # last Claude Code hook event
        self.event_id = 0

    def push_event(self, kind: str, project: str, message: str, duration_s: int = -1):
        self.event_id += 1
        # idle_s: how long you've been away from mouse/keyboard -> the CYD can stay quiet while you're at the PC
        self.event = {"id": self.event_id, "type": kind, "project": project[:40], "message": message[:120],
                      "duration_s": int(duration_s), "idle_s": int(user_idle_seconds()), "ts": time.time()}
        print(f"{datetime.now():%H:%M:%S} event {kind} {project!r}", file=sys.stderr)
        if not self.args.no_beacon:  # tell the CYD right away instead of waiting for its next poll
            threading.Thread(target=broadcast, args=(f"CLAUDE_EVENT {self.event_id}".encode(), 2), daemon=True).start()

    def event_payload(self):
        e = self.event
        if not e or time.time() - e["ts"] > 30 * 60:
            return None
        return {k: v for k, v in e.items() if k != "ts"} | {"age_s": int(time.time() - e["ts"])}

    def stats(self) -> dict:
        if self.exact:
            self.exact.poke()
        with self._cache_lock:
            ts, data = self._cache
            if data is None or time.monotonic() - ts > 3:
                data = compute_stats(self.store, self.args, self.exact)
                self._cache = (time.monotonic(), data)
            return data


class Handler(BaseHTTPRequestHandler):
    server_version = f"claude-usage/{VERSION}"

    def _send(self, code: int, payload: dict):
        body = json.dumps(payload).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _authorized(self) -> bool:
        expected = f"Bearer {self.server.token}".encode()
        return hmac.compare_digest(self.headers.get("Authorization", "").encode(), expected)

    def do_GET(self):
        url = urlparse(self.path)
        if url.path == "/v1/status":
            return self._send(200, {"version": VERSION, "paired_count": 1, "pending_code": ""})
        if url.path == "/v1/media/art":  # 80x80 RGB565 album art of the current track
            if self.server.token and not self._authorized():
                return self._send(401, {"error": "unauthorized"})
            art_id, data = self.server.media.art_bytes()
            if not data:
                return self._send(404, {"error": "no art"})
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(len(data)))
            self.send_header("X-Art-Id", art_id)
            self.end_headers()
            self.wfile.write(data)
            return
        if url.path != "/v1/stats":
            return self._send(404, {"error": "not found"})
        if self.server.token and not self._authorized():
            return self._send(401, {"error": "unauthorized"})
        try:
            data = dict(self.server.stats())
            data["event"] = self.server.event_payload()
            screens = parse_qs(url.query).get("screens", [""])[0].split(",")
            if "pc" in screens:  # PC sensors are only read while the CYD shows that page
                data["pc"] = self.server.pc.get()
            if "media" in screens:  # same for the now-playing page
                data["media"] = self.server.media.get()
            return self._send(200, data)
        except Exception as exc:  # keep serving even if a log line is odd
            sys.stderr.write(f"stats error: {exc!r}\n")
            return self._send(500, {"error": "internal"})

    def do_POST(self):
        # /v1/event: Claude Code hooks (pc_server/notify.py) report "stop" / "notification"
        # /v1/media: CYD music buttons -> {"action": "toggle" | "next" | "prev"}
        path = urlparse(self.path).path
        if path not in ("/v1/event", "/v1/media"):
            return self._send(404, {"error": "not found"})
        if self.server.token and not self._authorized():
            return self._send(401, {"error": "unauthorized"})
        try:
            length = min(int(self.headers.get("Content-Length", 0)), 8192)
            body = json.loads(self.rfile.read(length) or b"{}")
        except ValueError:
            return self._send(400, {"error": "bad json"})
        if path == "/v1/media":
            return self._send(200, {"ok": self.server.media.control(str(body.get("action") or ""))})
        kind = body.get("type") if body.get("type") in ("stop", "notification") else "stop"
        try:
            duration = int(body.get("duration_s", -1))
        except (TypeError, ValueError):
            duration = -1
        self.server.push_event(kind, str(body.get("project") or ""), str(body.get("message") or ""), duration)
        return self._send(200, {"ok": True, "id": self.server.event_id})

    def log_message(self, fmt, *args):
        if self.server.args.verbose:
            super().log_message(fmt, *args)


def token_from_secrets(path: Path) -> str:
    try:
        m = re.search(r'CLAUDE_TOKEN\[\]\s*=\s*"([^"]*)"', path.read_text(encoding="utf-8"))
    except OSError:
        return ""
    return m.group(1) if m else ""


def lan_ips() -> list[str]:
    ips = set()
    try:
        for info in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
            ip = info[4][0]
            if not ip.startswith(("127.", "169.254.")):
                ips.add(ip)
    except OSError:
        pass
    return sorted(ips)


def broadcast(msg: bytes, repeat: int = 1):
    """UDP broadcast on each LAN interface (assumes /24 home networks)."""
    for i in range(repeat):
        for ip in lan_ips() or ["0.0.0.0"]:
            bcast = ".".join(ip.split(".")[:3] + ["255"]) if ip != "0.0.0.0" else "255.255.255.255"
            try:
                with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
                    s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
                    s.bind((ip, 0))
                    s.sendto(msg, (bcast, BEACON_PORT))
            except OSError:
                pass  # adapter went away (laptop changed networks) - try again next round
        if i + 1 < repeat:
            time.sleep(0.3)


def beacon_loop(http_port: int):
    """Every few seconds announce 'CLAUDE_USAGE 1 <port>' so the CYD finds this PC."""
    msg = f"CLAUDE_USAGE 1 {http_port}".encode()
    while True:
        broadcast(msg)
        time.sleep(BEACON_INTERVAL_S)


def main():
    # pythonw (no console) leaves stdout/stderr as None
    if sys.stdout is None or sys.stderr is None:
        sys.stdout = sys.stderr = open(os.devnull, "w")
    here = Path(__file__).resolve().parent
    ap = argparse.ArgumentParser(description="Claude Code usage server for the ESP32 CYD (claudometer v1 schema)")
    ap.add_argument("--plan", default=os.environ.get("CLAUDE_USAGE_PLAN", "pro"), choices=sorted(PLAN_CAPS))
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=7842)
    ap.add_argument("--token", default=os.environ.get("CLAUDE_USAGE_TOKEN", ""))
    ap.add_argument("--secrets", type=Path, default=here.parent / "ESP32_tft_clock_weather" / "secrets.h",
                    help="secrets.h to read CLAUDE_TOKEN from when --token is not given")
    ap.add_argument("--claude-dir", type=Path, default=Path.home() / ".claude" / "projects")
    ap.add_argument("--session-cap", type=int, default=0, help="override tokens per 5h session")
    ap.add_argument("--weekly-cap", type=int, default=0, help="override weekly all-model tokens")
    ap.add_argument("--reset-weekday", type=int, default=0, help="weekly reset day, 0=Mon..6=Sun (default Mon)")
    ap.add_argument("--reset-hour", type=int, default=9, help="weekly reset hour, local time (default 9)")
    ap.add_argument("--no-auth", action="store_true", help="serve without a bearer token (LAN only!)")
    ap.add_argument("--print", action="store_true", help="print one stats snapshot and exit")
    ap.add_argument("--verbose", action="store_true", help="log every request")
    ap.add_argument("--log", type=Path, help="write output to this file (for running hidden with pythonw)")
    ap.add_argument("--no-beacon", action="store_true", help="don't broadcast this PC's address on the LAN")
    ap.add_argument("--calibration", type=Path, default=here / "calibration.json",
                    help="file holding the caps learned with --calibrate")
    ap.add_argument("--calibrate", nargs=2, type=float, metavar=("SESSION_PCT", "WEEK_PCT"),
                    help="match claude.ai: pass the session %% and weekly %% it shows right now (0 = skip one)")
    ap.add_argument("--context-limit", type=int, default=1_000_000, help="context window size shown on the CYD")
    ap.add_argument("--no-exact", action="store_true", help="never ask Anthropic, only estimate from logs")
    ap.add_argument("--exact-interval", type=int, default=120, help="seconds between Anthropic usage fetches")
    ap.add_argument("--credentials", type=Path, default=Path.home() / ".claude" / ".credentials.json",
                    help="Claude Code CLI login file (created by: claude auth login)")
    args = ap.parse_args()

    if args.log:
        if args.log.exists() and args.log.stat().st_size > 1_000_000:
            args.log.unlink()  # keep the log small
        sys.stdout = sys.stderr = open(args.log, "a", encoding="utf-8", buffering=1)
        print(f"--- start {datetime.now():%Y-%m-%d %H:%M:%S}")

    store = LogStore(args.claude_dir)
    exact = None if args.no_exact else ExactUsage(args.credentials, max(args.exact_interval, 30))
    if args.calibrate:
        calibrate(store, args, *args.calibrate)
        return
    if args.print:
        if exact:
            exact.busy = True
            exact._fetch()  # synchronous so the snapshot shows exact numbers when available
        print(json.dumps(compute_stats(store, args, exact), indent=2))
        return

    token = "" if args.no_auth else (args.token or token_from_secrets(args.secrets))
    if not token and not args.no_auth:
        sys.exit(f"No token: pass --token, set CLAUDE_USAGE_TOKEN, or put CLAUDE_TOKEN in {args.secrets}")
    if not args.claude_dir.is_dir():
        print(f"warning: {args.claude_dir} not found - stats will be empty", file=sys.stderr)

    try:
        srv = StatsServer((args.host, args.port), Handler, store, args, token, exact)
    except OSError as exc:
        sys.exit(f"Cannot listen on {args.host}:{args.port} ({exc}) - is another copy already running?")
    if not args.no_beacon:
        threading.Thread(target=beacon_loop, args=(args.port,), daemon=True).start()
    print(f"Claude usage server {VERSION} - plan {args.plan}, logs {args.claude_dir}")
    print(f"Listening on {args.host}:{args.port}; CYD CLAUDE_HOST should be one of: {', '.join(lan_ips()) or '?'}")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
