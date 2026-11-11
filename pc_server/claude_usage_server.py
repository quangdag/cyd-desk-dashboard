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

The bearer token comes from --token / CLAUDE_USAGE_TOKEN, else CLAUDE_TOKEN in
../ESP32_tft_clock_weather/secrets.h, else token.txt next to this file (created with a
random token on first start - type it into the CYD's Wi-Fi setup page).

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
import unicodedata
from urllib.parse import parse_qs, urlparse

VERSION = "2.0.4"

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


def clip_utf8(text: str, max_bytes: int) -> str:
    """At most max_bytes of UTF-8 (a C buffer of max_bytes + 1 on the CYD), cut between whole characters."""
    data = (text or "").encode("utf-8")
    if len(data) <= max_bytes:
        return text or ""
    return data[:max_bytes].decode("utf-8", "ignore")


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


# Keep the dollar caps in step with Anthropic's exact numbers, so the estimate used when Anthropic can't be reached
# (offline, rate limited, logged out) stays realistic. A cap measured once drifts: on 2026-11-11 the stored $244/week
# was 5-8x too small and the fallback estimate read 100% at 56% real.
RECAL_MIN_PCT = 5          # below this the rounding of the % makes the implied cap too noisy
RECAL_MIN_CHANGE = 0.15    # rewrite only when the cap moves by more than 15 %
RECAL_EVERY_S = 600
RECAL_MAX_AGE_S = 600      # only from a fresh fetch
_recal = {"at": -RECAL_EVERY_S}


def implied_caps(u: dict, snap: dict) -> dict:
    """Dollar caps implied by local log cost and the exact % right now: cap = cost / (pct / 100)."""
    caps = {}
    if snap["week_pct"] >= RECAL_MIN_PCT and u["week_cost"] > 0:
        caps["weekly_cap_usd"] = u["week_cost"] * 100 / snap["week_pct"]
    active = u["active"]
    if active and snap["session_pct"] >= RECAL_MIN_PCT and active["cost"] > 0:
        caps["session_cap_usd"] = active["cost"] * 100 / snap["session_pct"]
    return caps


def maybe_recalibrate(path: Path, u: dict, snap: dict) -> bool:
    now = time.monotonic()
    age = (datetime.now().astimezone() - snap["fetched_at"]).total_seconds()
    if now - _recal["at"] < RECAL_EVERY_S or age > RECAL_MAX_AGE_S:
        return False
    data = dict(load_calibration(path))
    changed = {}
    for key, cap in implied_caps(u, snap).items():
        old = data.get(key) or 0
        if not old or abs(cap - old) / old > RECAL_MIN_CHANGE:
            changed[key] = (old, cap)
            data[key] = cap
    _recal["at"] = now
    if not changed:
        return False
    data["calibrated_at"] = datetime.now().astimezone().isoformat(timespec="seconds")
    data["calibrated_with"] = {"session_pct": snap["session_pct"], "week_pct": snap["week_pct"], "auto": True}
    try:
        path.write_text(json.dumps(data, indent=2), encoding="utf-8")
    except OSError:
        return False
    print(f"{datetime.now():%H:%M:%S} calibration updated from exact usage: " +
          ", ".join(f"{k} ${a:.0f} -> ${b:.0f}" for k, (a, b) in changed.items()), file=sys.stderr)
    return True


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
    def __init__(self, creds_path: Path, interval_s: int, cache_path: Path | None = None):
        self.creds_path = creds_path
        self.interval_s = interval_s
        self.cache_path = cache_path  # last exact numbers, so a restart doesn't serve a raw estimate meanwhile
        self.lock = threading.Lock()
        self.busy = False
        self.last_try = 0.0
        self.snapshot = None          # dict with pct / resets / fetched_at
        self.last_error = ""
        self.block_until = 0.0        # monotonic time before which we don't ask again (rate limit)
        self.backoff = 0
        self.cli_refresh_at = -CLI_REFRESH_MIN_S
        self._load_cache()

    # -- cache: the snapshot survives a server restart (used while still fresh, see current()) --
    _TIME_KEYS = ("fetched_at", "session_resets", "week_resets")

    def _save_cache(self, snap: dict):
        if not self.cache_path:
            return
        data = {k: (v.isoformat() if k in self._TIME_KEYS and v else v) for k, v in snap.items()}
        try:
            self.cache_path.write_text(json.dumps(data), encoding="utf-8")
        except OSError:
            pass

    def _load_cache(self):
        if not self.cache_path:
            return
        try:
            data = json.loads(self.cache_path.read_text(encoding="utf-8"))
            for k in self._TIME_KEYS:
                data[k] = _parse_iso(data.get(k))
            if not isinstance(data["fetched_at"], datetime):
                return
            float(data["session_pct"]), float(data["week_pct"])
        except (OSError, ValueError, KeyError, TypeError):
            return  # missing or damaged cache: start without it, as before
        self.snapshot = data

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
            self._save_cache(snap)
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

    # Exact numbers from Anthropic override the estimate. They are shown as fetched (every --exact-interval):
    # extrapolating with local log cost overshot 5-8x, because the % also counts claude.ai chat use and the
    # dollar caps drift, so the CYD jumped up and back. Only a window that has reset since the fetch drops to 0.
    snap = exact.current() if exact else None
    if snap:
        maybe_recalibrate(args.calibration, u, snap)

        def roll(pct0, resets):
            if resets and resets <= now:   # window rolled over since the fetch
                return 0, None
            return pct(pct0, 100), resets

        s_pct, s_res = roll(snap["session_pct"], snap["session_resets"])
        w_pct, w_res = roll(snap["week_pct"], snap["week_resets"])
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


def workstation_locked() -> bool:
    """True while Windows shows the lock screen (the input desktop is not the user's "Default" one)."""
    if os.name != "nt":
        return False
    import ctypes
    from ctypes import wintypes
    user32 = ctypes.windll.user32
    user32.OpenInputDesktop.restype = wintypes.HANDLE
    user32.OpenInputDesktop.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    user32.CloseDesktop.argtypes = [wintypes.HANDLE]
    user32.GetUserObjectInformationW.argtypes = [wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p,
                                                 wintypes.DWORD, ctypes.POINTER(wintypes.DWORD)]
    h = user32.OpenInputDesktop(0, False, 0x0100)  # DESKTOP_SWITCHDESKTOP; fails on the secure desktop
    if not h:
        return True
    try:
        buf, need = ctypes.create_unicode_buffer(64), wintypes.DWORD()
        if user32.GetUserObjectInformationW(h, 2, buf, ctypes.sizeof(buf), ctypes.byref(need)):  # UOI_NAME
            return buf.value.lower() != "default"
        return False
    finally:
        user32.CloseDesktop(h)


# ---- Calendar: next event from an iCalendar (.ics) feed -----------------------

class Calendar:
    """Reads an .ics URL (e.g. Google Calendar's "secret address in iCal format") or file every 10 min
    and answers "what's next today / tomorrow". Handles plain events, all-day events and the common
    RRULEs (DAILY / WEEKLY with BYDAY / MONTHLY / YEARLY, INTERVAL, COUNT, UNTIL, EXDATE)."""

    REFRESH_S = 600

    def __init__(self, source: str):
        self.source = source.strip()
        self.events = []          # (start, end, title, all_day, rrule, exdates)
        self.lock = threading.Lock()
        self.last_error = ""
        if self.source:
            threading.Thread(target=self._loop, daemon=True).start()

    def _loop(self):
        while True:
            try:
                text = self._read()
                events = parse_ics(text)
                with self.lock:
                    self.events = events
                if self.last_error:
                    print(f"{datetime.now():%H:%M:%S} calendar OK ({len(events)} events)", file=sys.stderr)
                self.last_error = ""
            except Exception as exc:
                msg = f"{type(exc).__name__}: {exc}"
                if msg != self.last_error:
                    print(f"{datetime.now():%H:%M:%S} calendar unavailable ({msg})", file=sys.stderr)
                self.last_error = msg
            time.sleep(self.REFRESH_S)

    def _read(self) -> str:
        if self.source.startswith(("http://", "https://", "webcal://")):
            import urllib.request
            url = "https://" + self.source[len("webcal://"):] if self.source.startswith("webcal://") else self.source
            req = urllib.request.Request(url, headers={"User-Agent": f"cyd-usage-server/{VERSION}"})
            with urllib.request.urlopen(req, timeout=20) as r:
                return r.read().decode("utf-8", "replace")
        return Path(self.source).read_text(encoding="utf-8", errors="replace")

    def next_event(self, now: datetime | None = None):
        with self.lock:
            events = list(self.events)
        return next_event(events, now or datetime.now().astimezone())


def _ics_lines(text: str) -> list[str]:
    out = []
    for raw in text.replace("\r\n", "\n").replace("\r", "\n").split("\n"):
        if raw[:1] in (" ", "\t") and out:
            out[-1] += raw[1:]          # folded line
        elif raw:
            out.append(raw)
    return out


def _ics_unescape(v: str) -> str:
    return v.replace("\\n", " ").replace("\\N", " ").replace("\\,", ",").replace("\\;", ";").replace("\\\\", "\\")


def _ics_time(value: str, params: dict):
    """-> (aware datetime, all_day)"""
    value = value.strip()
    local = datetime.now().astimezone().tzinfo
    if len(value) == 8 or params.get("VALUE") == "DATE":
        d = datetime.strptime(value[:8], "%Y%m%d")
        return d.replace(tzinfo=local), True
    if value.endswith("Z"):
        from datetime import timezone
        return datetime.strptime(value[:15], "%Y%m%dT%H%M%S").replace(tzinfo=timezone.utc), False
    dt = datetime.strptime(value[:15], "%Y%m%dT%H%M%S")
    tz = local
    if params.get("TZID"):
        try:
            from zoneinfo import ZoneInfo
            tz = ZoneInfo(params["TZID"].strip('"'))
        except Exception:
            tz = local                  # unknown zone (or no tzdata on Windows): assume this PC's zone
    return dt.replace(tzinfo=tz), False


def parse_ics(text: str) -> list:
    events, cur = [], None
    for line in _ics_lines(text):
        if line == "BEGIN:VEVENT":
            cur = {"exdates": []}
            continue
        if line == "END:VEVENT":
            if cur and "start" in cur and cur.get("status") != "CANCELLED":
                start, all_day = cur["start"]
                if "end" in cur:
                    end = cur["end"][0]
                elif "duration" in cur:
                    end = start + cur["duration"]
                else:
                    end = start + (timedelta(days=1) if all_day else timedelta(0))
                events.append((start, end, cur.get("title", "(no title)"), all_day, cur.get("rrule"), cur["exdates"]))
            cur = None
            continue
        if cur is None or ":" not in line:
            continue
        head, value = line.split(":", 1)
        name, *ps = head.split(";")
        params = dict(x.split("=", 1) for x in ps if "=" in x)
        name = name.upper()
        try:
            if name == "SUMMARY":
                cur["title"] = _ics_unescape(value).strip()
            elif name == "DTSTART":
                cur["start"] = _ics_time(value, params)
            elif name == "DTEND":
                cur["end"] = _ics_time(value, params)
            elif name == "DURATION":
                m = re.fullmatch(r"P(?:(\d+)W)?(?:(\d+)D)?(?:T(?:(\d+)H)?(?:(\d+)M)?(?:(\d+)S)?)?", value.strip())
                if m:
                    w, d, h, mi, se = (int(x or 0) for x in m.groups())
                    cur["duration"] = timedelta(weeks=w, days=d, hours=h, minutes=mi, seconds=se)
            elif name == "RRULE":
                cur["rrule"] = dict(x.split("=", 1) for x in value.split(";") if "=" in x)
            elif name == "EXDATE":
                cur["exdates"] += [_ics_time(v, params)[0] for v in value.split(",")]
            elif name == "STATUS":
                cur["status"] = value.strip().upper()
        except ValueError:
            continue
    return events


_WEEKDAYS = ["MO", "TU", "WE", "TH", "FR", "SA", "SU"]


def _add_months(dt: datetime, months: int):
    y, m = divmod(dt.month - 1 + months, 12)
    try:
        return dt.replace(year=dt.year + y, month=m + 1)
    except ValueError:
        return None                     # e.g. 31st in a short month: RFC 5545 skips it


def occurrences(start: datetime, rrule: dict | None, until: datetime, limit: int = 3000):
    """Start times of a (possibly repeating) event, from DTSTART up to `until`."""
    if not rrule:
        if start <= until:
            yield start
        return
    freq = rrule.get("FREQ", "").upper()
    interval = max(1, int(rrule.get("INTERVAL", "1") or 1))
    count = int(rrule["COUNT"]) if rrule.get("COUNT", "").isdigit() else None
    end = None
    if rrule.get("UNTIL"):
        try:
            end = _ics_time(rrule["UNTIL"], {})[0]
        except ValueError:
            end = None
    byday = [d[-2:] for d in rrule.get("BYDAY", "").split(",") if d]
    if freq == "MONTHLY" and any(len(d) > 2 for d in rrule.get("BYDAY", "").split(",") if d):
        return                          # "2nd Monday" style rules are not supported
    n = 0
    step = 0
    while step < limit:
        if freq == "DAILY":
            cands = [start + timedelta(days=step * interval)]
        elif freq == "WEEKLY":
            week0 = start - timedelta(days=start.weekday())
            week = week0 + timedelta(weeks=step * interval)
            days = byday or [_WEEKDAYS[start.weekday()]]
            cands = sorted(week + timedelta(days=_WEEKDAYS.index(d)) for d in days if d in _WEEKDAYS)
            cands = [c for c in cands if c >= start]
        elif freq == "MONTHLY":
            cands = [c for c in [_add_months(start, step * interval)] if c]
        elif freq == "YEARLY":
            cands = [c for c in [_add_months(start, 12 * step * interval)] if c]
        else:
            if start <= until:
                yield start
            return
        for c in cands:
            if (end and c > end) or c > until:
                return
            n += 1
            if count is not None and n > count:
                return
            yield c
        step += 1


def next_event(events: list, now: datetime):
    """{"when": NOW|NEXT|TODAY|TOMORROW, "start": "HH:MM" or "", "title": ...} or None."""
    today = now.date()
    horizon = datetime.combine(today + timedelta(days=2), datetime.min.time()).replace(tzinfo=now.tzinfo)
    found = []                          # (priority, start, title, all_day)
    for start, end, title, all_day, rrule, exdates in events:
        length = end - start
        ex = {e.astimezone(now.tzinfo).replace(second=0, microsecond=0) for e in exdates}
        for occ in occurrences(start, rrule, horizon):
            occ_local = occ.astimezone(now.tzinfo)
            if occ_local.replace(second=0, microsecond=0) in ex:
                continue
            occ_end = occ_local + length
            if all_day:
                if occ_local.date() == today:
                    found.append((2, occ_local, title, True))
                elif occ_local.date() == today + timedelta(days=1):
                    found.append((4, occ_local, title, True))
            elif occ_local <= now < occ_end:
                found.append((0, occ_local, title, False))
            elif occ_local > now and occ_local.date() == today:
                found.append((1, occ_local, title, False))
            elif occ_local > now and occ_local.date() == today + timedelta(days=1):
                found.append((3, occ_local, title, False))
    if not found:
        return None
    prio, start, title, all_day = min(found, key=lambda f: (f[0], f[1]))
    when = ["NOW", "NEXT", "TODAY", "TOMORROW", "TOMORROW"][prio]
    return {"when": when, "start": "" if all_day else start.strftime("%H:%M"),
            "title": thai_marks(unicodedata.normalize("NFC", title))[:70]}


# ---- Claude Code permission prompts answered on the CYD -----------------------

class PermissionBroker:
    """pc_server/permission.py (a PermissionRequest hook) asks here while you're away from the PC.
    The request shows on the CYD; Allow / Deny comes back through /v1/permission. Nothing is decided
    without the CYD: no answer, an expired wait or you coming back to the PC -> the hook returns no
    decision and Claude Code shows its normal prompt."""

    AWAY_S = 60             # route to the CYD only after this much keyboard/mouse idle time
    CYD_FRESH_S = 150       # ... and only if a CYD polled recently

    def __init__(self, notify):
        self.notify = notify
        self.cond = threading.Condition()
        self.pending = {}   # id -> dict(tool, project, detail, deadline, decision)
        self.next_id = 1

    def open(self, tool: str, project: str, detail: str, timeout_s: int, cyd_age_s: float):
        if user_idle_seconds() < self.AWAY_S:
            return None, "at the PC"
        if cyd_age_s > self.CYD_FRESH_S:
            return None, "no CYD online"
        with self.cond:
            pid = self.next_id
            self.next_id += 1
            self.pending[pid] = {"tool": tool[:23], "project": project[:40], "detail": detail[:160],
                                 "deadline": time.monotonic() + timeout_s, "decision": None}
        print(f"{datetime.now():%H:%M:%S} permission #{pid} {tool} -> CYD", file=sys.stderr)
        self.notify()
        return pid, ""

    def wait(self, pid: int, timeout_s: float):
        """Block until the CYD answers, the request expires or you are back at the PC."""
        end = time.monotonic() + min(timeout_s, 300)
        try:
            with self.cond:
                while True:
                    req = self.pending.get(pid)
                    if not req:
                        return None
                    if req["decision"]:
                        return req["decision"]
                    left = min(end, req["deadline"]) - time.monotonic()
                    if left <= 0:
                        return None
                    if user_idle_seconds() < 3:     # back at the keyboard: answer on the PC instead
                        return None
                    self.cond.wait(min(left, 1.0))
        finally:
            with self.cond:
                self.pending.pop(pid, None)
            self.notify()                           # CYD hides the card

    def answer(self, pid: int, decision: str) -> bool:
        if decision not in ("allow", "deny"):
            return False
        with self.cond:
            req = self.pending.get(pid)
            if not req or req["decision"]:
                return False
            req["decision"] = decision
            self.cond.notify_all()
        print(f"{datetime.now():%H:%M:%S} permission #{pid} {decision} on the CYD", file=sys.stderr)
        return True

    def current(self):
        """Oldest unanswered request for the CYD card."""
        with self.cond:
            now = time.monotonic()
            live = [(pid, r) for pid, r in self.pending.items() if not r["decision"] and r["deadline"] > now]
        if not live:
            return None
        pid, r = min(live)
        return {"id": pid, "tool": r["tool"], "project": r["project"], "detail": r["detail"],
                "expires_in": max(1, int(r["deadline"] - time.monotonic()))}


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
        self.cyd_ip = ""           # last CYD that polled -> UDP "CLAUDE_DIRTY" goes straight to it
        self.cyd_seen = 0.0
        self.media_wanted = 0.0    # last time the CYD showed its Music page
        self._last_notify = 0.0
        self.permissions = PermissionBroker(self.notify_cyd)
        self.calendar = Calendar(args.ics)
        threading.Thread(target=self._watch, daemon=True).start()

    def notify_cyd(self):
        """Push: tell the CYD something changed so it fetches now instead of at its next poll."""
        ip = self.cyd_ip
        if not ip or self.args.no_beacon:
            return
        self._last_notify = time.monotonic()
        try:
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
                sock.sendto(b"CLAUDE_DIRTY", (ip, BEACON_PORT))
        except OSError:
            pass

    def _watch(self):
        """Watches what the CYD shows and pushes changes: the track / play state while the Music page is
        open (checked every second), and exact usage % whenever Anthropic's numbers move."""
        last_media, last_usage = None, None
        while True:
            time.sleep(1)
            try:
                if time.monotonic() - self.media_wanted < 30:
                    m = self.media.get() or {}
                    key = (m.get("title"), m.get("artist"), m.get("playing"), m.get("art_id"), m.get("error"))
                    if last_media is not None and key != last_media:
                        self.notify_cyd()
                    last_media = key
                else:
                    last_media = None
                snap = self.exact.current() if self.exact else None
                usage = (round(snap["session_pct"]), round(snap["week_pct"])) if snap else None
                if usage and last_usage and usage != last_usage:
                    with self._cache_lock:
                        self._cache = (0.0, None)
                    self.notify_cyd()
                last_usage = usage or last_usage
            except Exception as exc:  # never let the watcher die
                sys.stderr.write(f"watch error: {exc!r}\n")

    def push_event(self, kind: str, project: str, message: str, duration_s: int = -1):
        self.event_id += 1
        # idle_s: how long you've been away from mouse/keyboard -> the CYD can stay quiet while you're at the PC
        # Byte limits match the CYD's buffers (event_project[41], event_msg[161]); never cut inside a UTF-8 letter
        self.event = {"id": self.event_id, "type": kind, "project": clip_utf8(project, 40),
                      "message": clip_utf8(unicodedata.normalize("NFC", message), 160),
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
            srv = self.server
            srv.cyd_ip, srv.cyd_seen = self.client_address[0], time.monotonic()
            data = dict(srv.stats())
            data["event"] = srv.event_payload()
            data["permission"] = srv.permissions.current()
            data["calendar"] = srv.calendar.next_event()
            data["pc_locked"] = workstation_locked()
            screens = parse_qs(url.query).get("screens", [""])[0].split(",")
            if "pc" in screens:
                data["pc"] = srv.pc.get()
            if "media" in screens:  # the media session is only read while the CYD shows that page
                srv.media_wanted = time.monotonic()
                data["media"] = srv.media.get()
            return self._send(200, data)
        except Exception as exc:  # keep serving even if a log line is odd
            sys.stderr.write(f"stats error: {exc!r}\n")
            return self._send(500, {"error": "internal"})

    def do_POST(self):
        # /v1/event: Claude Code hooks (pc_server/notify.py) report "stop" / "notification"
        # /v1/media: CYD music buttons -> {"action": "toggle" | "next" | "prev"}
        # /v1/permission/request, /v1/permission/wait: pc_server/permission.py (localhost only)
        # /v1/permission: CYD answers -> {"id": n, "decision": "allow" | "deny"}
        path = urlparse(self.path).path
        if path not in ("/v1/event", "/v1/media", "/v1/permission", "/v1/permission/request", "/v1/permission/wait"):
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
        perms = self.server.permissions
        if path == "/v1/permission":
            try:
                ok = perms.answer(int(body.get("id", 0)), str(body.get("decision") or ""))
            except (TypeError, ValueError):
                ok = False
            return self._send(200, {"ok": ok})
        if path.startswith("/v1/permission/"):
            if self.client_address[0] not in ("127.0.0.1", "::1"):  # only the hook on this PC may ask
                return self._send(403, {"error": "local only"})
            if path.endswith("/request"):
                age = time.monotonic() - self.server.cyd_seen if self.server.cyd_seen else 1e9
                pid, why = perms.open(str(body.get("tool") or "?"), str(body.get("project") or ""),
                                      str(body.get("detail") or ""), int(body.get("timeout_s") or 90), age)
                return self._send(200, {"id": pid, "reason": why})
            try:
                decision = perms.wait(int(body.get("id", 0)), float(body.get("timeout_s") or 90))
            except (TypeError, ValueError):
                decision = None
            return self._send(200, {"decision": decision})
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


def token_from_file(path: Path, create: bool) -> str:
    """token.txt next to the server; with create, make a random one on first start."""
    try:
        token = path.read_text(encoding="utf-8").strip()
        if token:
            return token
    except OSError:
        pass
    if not create:
        return ""
    import secrets as _secrets
    token = _secrets.token_urlsafe(12)
    path.write_text(token + "\n", encoding="utf-8")
    print(f"created {path} - enter this token on the CYD's Wi-Fi setup page: {token}", file=sys.stderr)
    return token


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
    ap.add_argument("--usage-cache", type=Path, default=here / "usage_cache.json",
                    help="last exact usage, reused for up to 30 min after a restart")
    ap.add_argument("--credentials", type=Path, default=Path.home() / ".claude" / ".credentials.json",
                    help="Claude Code CLI login file (created by: claude auth login)")
    ap.add_argument("--ics", default=os.environ.get("CYD_ICS", ""),
                    help="calendar .ics URL or file for the CYD clock (default: first line of calendar.txt)")
    args = ap.parse_args()

    if args.log:
        if args.log.exists() and args.log.stat().st_size > 1_000_000:
            args.log.unlink()  # keep the log small
        sys.stdout = sys.stderr = open(args.log, "a", encoding="utf-8", buffering=1)
        print(f"--- start {datetime.now():%Y-%m-%d %H:%M:%S}")

    store = LogStore(args.claude_dir)
    exact = None if args.no_exact else ExactUsage(args.credentials, max(args.exact_interval, 30), args.usage_cache)
    if args.calibrate:
        calibrate(store, args, *args.calibrate)
        return
    if args.print:
        args.ics = ""
        if exact:
            exact.busy = True
            exact._fetch()  # synchronous so the snapshot shows exact numbers when available
        print(json.dumps(compute_stats(store, args, exact), indent=2))
        return

    if not args.ics:
        try:
            args.ics = (here / "calendar.txt").read_text(encoding="utf-8").strip().splitlines()[0]
        except (OSError, IndexError):
            args.ics = ""
    token = "" if args.no_auth else (args.token or token_from_secrets(args.secrets)
                                     or token_from_file(here / "token.txt", create=True))
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
