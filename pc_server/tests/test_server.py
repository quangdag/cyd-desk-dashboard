"""Tests for the PC server pieces that don't need Windows or a CYD.  python -m unittest discover pc_server/tests"""
import json
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from datetime import datetime, timedelta, timezone
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import claude_usage_server as srv  # noqa: E402
import ensure_server  # noqa: E402
import notify  # noqa: E402
import permission  # noqa: E402

TZ = datetime.now().astimezone().tzinfo


def ics(*events: str) -> str:
    return "BEGIN:VCALENDAR\r\nVERSION:2.0\r\n" + "".join(events) + "END:VCALENDAR\r\n"


def vevent(start: str, end: str, title: str, extra: str = "") -> str:
    return f"BEGIN:VEVENT\r\nDTSTART:{start}\r\nDTEND:{end}\r\nSUMMARY:{title}\r\n{extra}END:VEVENT\r\n"


def stamp(dt: datetime) -> str:
    return dt.strftime("%Y%m%dT%H%M%S")


class ThaiMarks(unittest.TestCase):
    def test_tone_mark_over_upper_vowel_is_raised(self):
        self.assertEqual(srv.thai_marks("ที่"), "ที")

    def test_tone_mark_before_sara_am_is_raised(self):
        self.assertEqual(srv.thai_marks("น้ำ"), "นำ")

    def test_plain_text_is_untouched(self):
        for text in ("Sơn Tùng M-TP", "ไก่", ""):
            self.assertEqual(srv.thai_marks(text), text)


class CalendarTests(unittest.TestCase):
    def setUp(self):
        self.now = datetime(2026, 11, 11, 10, 0, tzinfo=TZ)  # a Wednesday

    def next(self, text):
        return srv.next_event(srv.parse_ics(text), self.now)

    def test_nothing(self):
        self.assertIsNone(self.next(ics()))

    def test_later_today(self):
        e = self.next(ics(vevent("20261111T140000", "20261111T150000", "Team meeting")))
        self.assertEqual(e, {"when": "NEXT", "start": "14:00", "title": "Team meeting"})

    def test_ongoing_beats_later(self):
        e = self.next(ics(vevent("20261111T093000", "20261111T110000", "Lab"),
                          vevent("20261111T140000", "20261111T150000", "Meeting")))
        self.assertEqual((e["when"], e["title"]), ("NOW", "Lab"))

    def test_past_event_is_skipped_and_tomorrow_shown(self):
        e = self.next(ics(vevent("20261111T080000", "20261111T090000", "Done"),
                          vevent("20261112T090000", "20261112T100000", "Exam")))
        self.assertEqual((e["when"], e["start"], e["title"]), ("TOMORROW", "09:00", "Exam"))

    def test_all_day_today(self):
        e = self.next(ics("BEGIN:VEVENT\r\nDTSTART;VALUE=DATE:20261111\r\nDTEND;VALUE=DATE:20261112\r\n"
                          "SUMMARY:Holiday\r\nEND:VEVENT\r\n"))
        self.assertEqual(e, {"when": "TODAY", "start": "", "title": "Holiday"})

    def test_weekly_rule_with_byday(self):
        # started on a Monday months ago, repeats Mon/Wed -> today (Wednesday) 13:00
        e = self.next(ics(vevent("20260105T130000", "20260105T140000", "Class",
                                 "RRULE:FREQ=WEEKLY;BYDAY=MO,WE\r\n")))
        self.assertEqual((e["when"], e["start"], e["title"]), ("NEXT", "13:00", "Class"))

    def test_exdate_removes_one_occurrence(self):
        e = self.next(ics(vevent("20260105T130000", "20260105T140000", "Class",
                                 "RRULE:FREQ=WEEKLY;BYDAY=WE\r\nEXDATE:20261111T130000\r\n")))
        self.assertIsNone(e)

    def test_until_ends_the_series(self):
        e = self.next(ics(vevent("20260105T130000", "20260105T140000", "Class",
                                 "RRULE:FREQ=DAILY;UNTIL=20260301T000000Z\r\n")))
        self.assertIsNone(e)

    def test_cancelled_and_folded_lines(self):
        e = self.next(ics(vevent("20261111T120000", "20261111T130000", "Gone", "STATUS:CANCELLED\r\n"),
                          "BEGIN:VEVENT\r\nDTSTART:20261111T160000\r\nDTEND:20261111T170000\r\n"
                          "SUMMARY:Long\r\n  title\\, folded\r\nEND:VEVENT\r\n"))
        self.assertEqual(e["title"], "Long title, folded")

    def test_utc_time_is_converted(self):
        start = datetime(2026, 11, 11, 15, 0, tzinfo=TZ).astimezone(timezone.utc)
        e = self.next(ics(vevent(start.strftime("%Y%m%dT%H%M%SZ"), start.strftime("%Y%m%dT%H%M%SZ"), "Call")))
        self.assertEqual(e["start"], "15:00")


class PermissionBrokerTests(unittest.TestCase):
    def setUp(self):
        self.notified = 0
        self.broker = srv.PermissionBroker(self._notify)

    def _notify(self):
        self.notified += 1

    def test_not_routed_while_at_the_pc(self):
        with mock.patch.object(srv, "user_idle_seconds", return_value=5):
            self.assertEqual(self.broker.open("Bash", "p", "ls", 30, cyd_age_s=1), (None, "at the PC"))

    def test_not_routed_without_a_cyd(self):
        with mock.patch.object(srv, "user_idle_seconds", return_value=600):
            self.assertEqual(self.broker.open("Bash", "p", "ls", 30, cyd_age_s=1e9), (None, "no CYD online"))

    def test_answer_reaches_the_waiting_hook(self):
        with mock.patch.object(srv, "user_idle_seconds", return_value=600):
            pid, _ = self.broker.open("Bash", "proj", "npm test", 30, cyd_age_s=1)
            card = self.broker.current()
            self.assertEqual((card["id"], card["tool"], card["detail"]), (pid, "Bash", "npm test"))
            threading.Timer(0.2, self.broker.answer, args=(pid, "allow")).start()
            self.assertEqual(self.broker.wait(pid, 5), "allow")
        self.assertIsNone(self.broker.current())
        self.assertGreaterEqual(self.notified, 2)  # card shown, card hidden

    def test_no_answer_means_no_decision(self):
        with mock.patch.object(srv, "user_idle_seconds", return_value=600):
            pid, _ = self.broker.open("Bash", "proj", "rm x", 1, cyd_age_s=1)
            t0 = time.monotonic()
            self.assertIsNone(self.broker.wait(pid, 5))
            self.assertLess(time.monotonic() - t0, 3)

    def test_coming_back_cancels(self):
        with mock.patch.object(srv, "user_idle_seconds", return_value=600):
            pid, _ = self.broker.open("Bash", "proj", "rm x", 30, cyd_age_s=1)
        with mock.patch.object(srv, "user_idle_seconds", return_value=0):
            self.assertIsNone(self.broker.wait(pid, 5))

    def test_bad_answers_are_ignored(self):
        self.assertFalse(self.broker.answer(99, "allow"))
        with mock.patch.object(srv, "user_idle_seconds", return_value=600):
            pid, _ = self.broker.open("Bash", "p", "x", 30, cyd_age_s=1)
        self.assertFalse(self.broker.answer(pid, "maybe"))
        self.assertTrue(self.broker.answer(pid, "deny"))
        self.assertFalse(self.broker.answer(pid, "allow"))  # first answer wins


class HookAndMediaHelpers(unittest.TestCase):
    def test_describe(self):
        self.assertEqual(permission.describe("Bash", {"command": "npm  run\n build"}), "npm run build")
        self.assertEqual(permission.describe("Edit", {"file_path": "/a/src/main.c"}), "main.c  (src)")
        self.assertEqual(permission.describe("WebFetch", {"url": "https://x.y"}), "https://x.y")

    def test_app_icon_detection(self):
        from PIL import Image
        self.assertTrue(srv.MediaSessions._is_app_icon(Image.new("RGBA", (256, 256), (0, 0, 0, 0))))
        self.assertTrue(srv.MediaSessions._is_app_icon(Image.new("RGB", (48, 48))))
        self.assertFalse(srv.MediaSessions._is_app_icon(Image.new("RGB", (300, 300))))

    def test_fallback_title_outside_browsers(self):
        self.assertEqual(srv.MediaSessions._fallback_title("Spotify", ""), ("Playing", "Spotify"))

    def test_token_file_is_created_once(self):
        import tempfile
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "token.txt"
            self.assertEqual(srv.token_from_file(path, create=False), "")
            first = srv.token_from_file(path, create=True)
            self.assertGreaterEqual(len(first), 12)
            self.assertEqual(srv.token_from_file(path, create=True), first)


class FakeStore:
    def __init__(self, recs):
        self.recs = recs

    def records_since(self, since):
        return [r for r in self.recs if r[srv.K_TS] >= since]


def opus_rec(ts: datetime, out_tokens: int, key: str):
    # (key, ts, model, in, out, cache_read, cache_write, sidechain, project)
    return ((key, key), ts, "claude-opus", 0, out_tokens, 0, 0, False, "proj")


def stats_args(cal_path: Path):
    import argparse
    return argparse.Namespace(plan="pro", session_cap=0, weekly_cap=0, calibration=cal_path, reset_weekday=0,
                              reset_hour=0, context_limit=1_000_000)


class ExactUsageNumbers(unittest.TestCase):
    """The CYD must show Anthropic's exact numbers, not an estimate that runs 5-8x ahead (seen 2026-11-11)."""

    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.cal = Path(self.dir.name) / "calibration.json"
        self.cal.write_text('{"session_cap_usd": 10, "weekly_cap_usd": 20}', encoding="utf-8")  # far too small
        srv._recal["at"] = -srv.RECAL_EVERY_S
        self.now = datetime.now().astimezone()

    def tearDown(self):
        srv._cal_cache.update({"mtime": None, "data": {}})
        self.dir.cleanup()

    def snap(self, fetched_ago_s=60, week=56.0, session=31.0, week_resets_in_h=60):
        return {"fetched_at": self.now - timedelta(seconds=fetched_ago_s), "session_pct": session,
                "session_resets": self.now + timedelta(hours=2), "week_pct": week,
                "week_resets": self.now + timedelta(hours=week_resets_in_h), "breakdown": [], "extra": {}}

    def stats(self, snap, recs):
        exact = mock.Mock()
        exact.current.return_value = snap
        return srv.compute_stats(FakeStore(recs), stats_args(self.cal), exact)

    def test_spending_after_the_fetch_does_not_inflate_the_numbers(self):
        recs = [opus_rec(self.now - timedelta(seconds=10), 200_000, "a")]   # $15 after the fetch
        d = self.stats(self.snap(), recs)
        self.assertEqual(d["budgets"]["code_all"], 56)
        self.assertEqual(d["session"]["pct_used"], 31)
        self.assertEqual(d["source"], "exact")

    def test_week_that_reset_since_the_fetch_shows_zero(self):
        d = self.stats(self.snap(week_resets_in_h=-1), [])
        self.assertEqual(d["budgets"]["code_all"], 0)

    def test_caps_recalibrate_from_exact_numbers(self):
        # $150 of Opus output this week at 50% real -> 100% = $300 (the stored $20 was far too small)
        recs = [opus_rec(self.now - timedelta(minutes=30), 2_000_000, "b")]
        self.stats(self.snap(week=50.0, session=50.0), recs)
        data = json.loads(self.cal.read_text(encoding="utf-8"))
        self.assertAlmostEqual(data["weekly_cap_usd"], 300, delta=1)
        self.assertAlmostEqual(data["session_cap_usd"], 300, delta=1)
        self.assertTrue(data["calibrated_with"]["auto"])

    def test_small_drift_does_not_rewrite_the_file(self):
        self.cal.write_text('{"session_cap_usd": 300, "weekly_cap_usd": 310}', encoding="utf-8")
        recs = [opus_rec(self.now - timedelta(minutes=30), 2_000_000, "c")]
        u = srv.usage_now(FakeStore(recs), stats_args(self.cal))
        self.assertFalse(srv.maybe_recalibrate(self.cal, u, self.snap(week=50.0, session=50.0)))

    def test_stale_or_tiny_numbers_never_recalibrate(self):
        recs = [opus_rec(self.now - timedelta(minutes=30), 2_000_000, "d")]
        u = srv.usage_now(FakeStore(recs), stats_args(self.cal))
        self.assertFalse(srv.maybe_recalibrate(self.cal, u, self.snap(fetched_ago_s=3600, week=50.0)))
        self.assertEqual(srv.implied_caps(u, self.snap(week=2.0, session=1.0)), {})

    def test_exact_snapshot_survives_a_restart(self):
        cache = Path(self.dir.name) / "usage_cache.json"
        first = srv.ExactUsage(Path("missing.json"), 120, cache)
        first._save_cache(self.snap())
        again = srv.ExactUsage(Path("missing.json"), 120, cache)
        self.assertEqual(again.current()["week_pct"], 56.0)
        cache.write_text("{damaged", encoding="utf-8")
        self.assertIsNone(srv.ExactUsage(Path("missing.json"), 120, cache).current())

    def test_old_cached_snapshot_is_not_used(self):
        cache = Path(self.dir.name) / "usage_cache.json"
        srv.ExactUsage(Path("missing.json"), 120, cache)._save_cache(self.snap(fetched_ago_s=srv.EXACT_MAX_AGE_S + 60))
        self.assertIsNone(srv.ExactUsage(Path("missing.json"), 120, cache).current())


class DoneCardText(unittest.TestCase):
    def test_clip_utf8_never_cuts_a_letter(self):
        text = "Đã sửa xong lỗi"
        for n in range(1, 25):
            clipped = srv.clip_utf8(text, n)
            self.assertLessEqual(len(clipped.encode("utf-8")), n)
            self.assertTrue(text.startswith(clipped))

    def test_last_reply_is_the_final_main_chat_text_without_markdown(self):
        lines = [
            {"type": "user", "message": {"content": "sửa lỗi"}},
            {"type": "assistant", "message": {"content": [{"type": "text", "text": "Đang xem..."}]}},
            {"type": "assistant", "message": {"content": [
                {"type": "text", "text": "## Xong\n- **Đã sửa** `ensure_server.py` → chạy ổn"}]}},
            {"type": "assistant", "isSidechain": True, "message": {"content": [{"type": "text", "text": "subagent"}]}},
            {"type": "assistant", "message": {"content": [{"type": "tool_use", "name": "Bash"}]}},
        ]
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "t.jsonl"
            path.write_text("\n".join(json.dumps(x, ensure_ascii=False) for x in lines), encoding="utf-8")
            self.assertEqual(notify.last_reply(str(path)), "Đã sửa ensure_server.py -> chạy ổn")  # heading dropped
        self.assertEqual(notify.last_reply("does-not-exist.jsonl"), "")


class EnsureServerLaunch(unittest.TestCase):
    """The server must not start inside the Claude app's job object, or it stops with that Claude session."""

    CMD = ["C:/Program Files/Python/pythonw.exe", "C:/My Projects/cyd/claude_usage_server.py", "--plan", "pro"]

    def run_launch(self, in_job: bool, breakaway_ok: bool, wmi_ok: bool):
        calls = []

        def popen(cmd, cwd, extra=0):
            calls.append(("popen", extra))
            if extra == ensure_server.BREAKAWAY and not breakaway_ok:
                raise PermissionError("job forbids breakaway")

        def wmi(cmd, cwd):
            calls.append(("wmi", 0))
            return wmi_ok

        how = ensure_server.launch_windows(self.CMD, Path("."), job=lambda: in_job, popen=popen, wmi=wmi)
        return how, calls

    def test_not_in_a_job_starts_directly(self):
        how, calls = self.run_launch(in_job=False, breakaway_ok=False, wmi_ok=False)
        self.assertEqual(how, "direct")
        self.assertEqual(calls, [("popen", 0)])

    def test_job_that_allows_breakaway(self):
        how, calls = self.run_launch(in_job=True, breakaway_ok=True, wmi_ok=False)
        self.assertEqual(how, "breakaway from job")
        self.assertEqual(calls, [("popen", ensure_server.BREAKAWAY)])

    def test_claude_job_without_breakaway_uses_wmi(self):
        how, calls = self.run_launch(in_job=True, breakaway_ok=False, wmi_ok=True)
        self.assertIn("WMI", how)
        self.assertEqual(calls, [("popen", ensure_server.BREAKAWAY), ("wmi", 0)])  # exactly one server started

    def test_last_resort_still_starts_the_server(self):
        how, calls = self.run_launch(in_job=True, breakaway_ok=False, wmi_ok=False)
        self.assertIn("inside the job", how)
        self.assertEqual(calls[-1], ("popen", 0))

    def test_command_line_keeps_paths_with_spaces_together(self):
        line = subprocess.list2cmdline(self.CMD)
        self.assertEqual(line, '"C:/Program Files/Python/pythonw.exe" "C:/My Projects/cyd/claude_usage_server.py" '
                               "--plan pro")

    def test_launch_is_written_to_the_server_log(self):
        with tempfile.TemporaryDirectory() as d:
            log = Path(d) / "server.log"
            ensure_server.log_line(["--plan", "pro", "--log", str(log)], "WMI (outside the job)")
            self.assertIn("ensure_server: started WMI (outside the job)", log.read_text(encoding="utf-8"))
            ensure_server.log_line(["--plan", "pro"], "direct")   # no --log: nothing to write, no error


if __name__ == "__main__":
    unittest.main()
