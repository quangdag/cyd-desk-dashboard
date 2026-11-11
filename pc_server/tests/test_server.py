"""Tests for the PC server pieces that don't need Windows or a CYD.  python -m unittest discover pc_server/tests"""
import sys
import threading
import time
import unittest
from datetime import datetime, timedelta, timezone
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import claude_usage_server as srv  # noqa: E402
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


if __name__ == "__main__":
    unittest.main()
