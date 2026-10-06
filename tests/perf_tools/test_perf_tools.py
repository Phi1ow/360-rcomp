#!/usr/bin/env python3
"""Host checks of the performance-analysis and generator-output tools (no PS5, no network).

tools/analyze_frame_pacing.py: the per-second frame-rate counter lines (RCOMP-FPS) that a
title built with RCOMP_M6_FPS_COUNTER writes are folded into `per_second_counter`, only for
the swaps inside the measured window, with and without the `cpu_cores` field.
tools/ab_compare.py: the two-arm statistics (means, pooled spread, 95 % interval).
"""
import contextlib
import importlib.util
import io
import json
import statistics
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def load(name, relative):
    spec = importlib.util.spec_from_file_location(name, ROOT / relative)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


pacing = load("analyze_frame_pacing", "tools/analyze_frame_pacing.py")
compare_tool = load("ab_compare", "tools/ab_compare.py")
pc_share = load("pc_share_compare", "tools/pc_share_compare.py")
bucket_tool = load("bucket_compare", "tools/bucket_compare.py")
scene_tool = load("scene_compare", "tools/scene_compare.py")


def make_log(observations, rates, with_cores=True, preamble=(), pacing=None):
    """One swap observation per second (30 swaps each) followed by its counter line."""
    lines = list(preamble)
    swaps, millis = 0, 10000
    for k in range(observations):
        swaps += 30
        millis += 1000
        lines.append(f"RCOMP-VD swap #{swaps} at {millis} ms")
        line = f"RCOMP-FPS fps={rates[k % len(rates)]:.2f} frame_ms=33.33 swaps={swaps}"
        if with_cores:
            line += f" cpu_cores={3.0 + (k % 3) * 0.25:.2f}"
        if pacing:
            line += f" vb={pacing[k % len(pacing)]}"
        lines.append(line)
    return ("\n".join(lines) + "\n").encode()


class CounterParsing(unittest.TestCase):
    def test_no_counter_lines_leave_the_result_unchanged(self):
        raw = "\n".join(f"RCOMP-VD swap #{30 * (k + 1)} at {11000 + 1000 * k} ms" for k in range(100)).encode()
        result, _ = pacing.analyze(raw, 90)
        self.assertNotIn("per_second_counter", result)
        self.assertAlmostEqual(result["fps"], 30.0, places=6)

    def test_counter_statistics_cover_only_the_measured_window(self):
        rates = [30.0, 36.0, 30.0, 33.0]
        early = ["RCOMP-FPS fps=99.00 frame_ms=10.10 swaps=5 cpu_cores=8.00"]  # before every swap line
        result, _ = pacing.analyze(make_log(200, rates, preamble=early), 90)
        counter = result["per_second_counter"]
        first, last = result["first_swap"], result["last_swap"]
        selected = [rates[k % 4] for k in range(200) if first <= 30 * (k + 1) <= last]
        self.assertEqual(counter["seconds"], len(selected))
        self.assertAlmostEqual(counter["mean"], statistics.mean(selected), places=9)
        self.assertEqual(counter["min"], min(selected))
        self.assertEqual(counter["max"], max(selected))
        ordered = sorted(selected)
        self.assertEqual(counter["median"], ordered[int(0.5 * (len(ordered) - 1) + 0.5)])
        self.assertLess(counter["max"], 99.0)  # the line before the window is excluded
        self.assertIn("cpu_cores_mean", counter)
        self.assertLessEqual(counter["cpu_cores_max"], 3.5)

    def test_swap_pacing_field_is_aggregated_over_the_window(self):
        vb = ["2:30", "1:12,2:24", "2:30", "2:29,3:1"]
        result, _ = pacing.analyze(make_log(200, [30.0], pacing=vb), 90)
        counter = result["per_second_counter"]
        inside = [k for k in range(200) if result["first_swap"] <= 30 * (k + 1) <= result["last_swap"]]
        expected = {"1": 0, "2": 0, "3": 0}
        for k in inside:
            for item in vb[k % 4].split(","):
                vblanks, swaps = item.split(":")
                expected[vblanks] += int(swaps)
        expected = {key: value for key, value in expected.items() if value}
        self.assertEqual(counter["swap_vblank_intervals"], expected)
        mixed = [k for k in inside if k % 4 == 1]
        self.assertEqual(counter["seconds_not_locked"], len(mixed))            # 12 of 36 swaps at one vblank
        self.assertEqual(counter["seconds_locked_to_vblanks"], {"2": len(inside) - len(mixed)})

    def test_lines_without_the_pacing_field_add_no_pacing_keys(self):
        result, _ = pacing.analyze(make_log(120, [32.0]), 90)
        self.assertNotIn("swap_vblank_intervals", result["per_second_counter"])

    def test_older_lines_without_cpu_cores_still_parse(self):
        result, _ = pacing.analyze(make_log(120, [32.0], with_cores=False), 90)
        counter = result["per_second_counter"]
        self.assertEqual(counter["median"], 32.0)
        self.assertNotIn("cpu_cores_mean", counter)


class AbStatistics(unittest.TestCase):
    @staticmethod
    def compare(a, b):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            compare_tool.compare("rate", a, b)
        return out.getvalue()

    def test_known_interval(self):
        # Means 2.5 and 3.5, both standard deviations sqrt(5/3), 6 degrees of freedom.
        text = self.compare([1.0, 2.0, 3.0, 4.0], [2.0, 3.0, 4.0, 5.0])
        self.assertIn("B - A = +1.000 fps (+40.00 %)", text)
        self.assertIn("[-1.234, +3.234]", text)
        self.assertIn("contains zero", text)

    def test_clear_difference_excludes_zero(self):
        self.assertIn("excludes zero", self.compare([10.0, 10.0, 10.0], [11.0, 11.0, 11.0]))

    def test_regime_split_separates_locked_and_fast_seconds(self):
        def interval(start, fps):
            return {"from_swap": start, "to_swap": start + 30, "elapsed_ms": 30000.0 / fps, "fps": fps}
        # Three seconds locked at 30 fps, one at 36 fps: the fast share is by time, not by count.
        items = [interval(0, 30.0), interval(30, 30.0), interval(60, 36.0), interval(90, 30.0)]
        fast, locked, share = compare_tool.regimes(items, 33.0)
        self.assertAlmostEqual(fast, 36.0, places=9)
        self.assertAlmostEqual(locked, 30.0, places=9)
        self.assertAlmostEqual(share, (30000.0 / 36.0) / (3 * 1000.0 + 30000.0 / 36.0), places=9)
        self.assertEqual(compare_tool.regimes([interval(0, 30.0)], 33.0)[0], None)   # no fast second

    def test_single_runs_give_no_interval(self):
        self.assertIn("no interval", self.compare([32.0], [31.0]))

    def test_run_directory_needs_a_passing_summary(self):
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaises(SystemExit):
                compare_tool.load_run(tmp)
            Path(tmp, "summary.json").write_text(json.dumps({
                "status": "PASS", "fps": 32.0, "interval_fps_min": 29.5, "interval_fps_max": 36.0,
                "fatal_count": 0, "crash_count": 0,
                "intervals": [{"from_swap": 30, "to_swap": 60, "elapsed_ms": 1000, "fps": 30.0}]}))
            run = compare_tool.load_run(tmp)
            self.assertEqual(run["fps"], 32.0)
            self.assertEqual(run["pause"], "NOT TESTED")  # no pause-monitor.json beside it


class Buckets(unittest.TestCase):
    @staticmethod
    def run_dir(tmp, name, seconds, rate, cores):
        """A run directory whose log has one counter line per second at a constant rate (30 swaps/s)."""
        directory = Path(tmp, name)
        directory.mkdir()
        lines = [f"RCOMP-FPS fps={rate:.2f} frame_ms={1000 / rate:.2f} swaps={30 * (k + 1)} cpu_cores={cores:.2f}"
                 for k in range(seconds)]
        (directory / "rcomp_title.err").write_text("\n".join(lines) + "\n", encoding="utf-8")
        return directory

    def test_buckets_average_the_seconds_of_each_scene_slice(self):
        with tempfile.TemporaryDirectory() as tmp:
            rows = bucket_tool.load(self.run_dir(tmp, "a", 100, 32.0, 3.5))
        got = bucket_tool.buckets(rows, 1200)        # 30 swaps a second: 40 seconds per bucket
        self.assertEqual(sorted(got), [0, 1200, 2400])   # 100 s = 3,000 swaps: the third bucket has 21 s
        self.assertEqual(got[0][2], 39)              # swaps 30..1170 fall in the first bucket
        self.assertAlmostEqual(got[0][0], 32.0)
        self.assertAlmostEqual(got[0][1], 3.5)

    def test_a_run_that_ended_early_is_reported_not_extended(self):
        with tempfile.TemporaryDirectory() as tmp:
            whole = self.run_dir(tmp, "whole", 160, 32.0, 3.5)
            short = self.run_dir(tmp, "short", 60, 33.0, 3.4)
            out = io.StringIO()
            old_argv = sys.argv
            sys.argv = ["bucket_compare.py", "--a", str(whole), "--b", str(short), "--step", "1200"]
            try:
                with contextlib.redirect_stdout(out):
                    bucket_tool.main()
            finally:
                sys.argv = old_argv
        text = out.getvalue()
        self.assertIn("+1.00", text)                 # the first bucket exists in both arms: 33 against 32
        self.assertIn("A only", text)                # later buckets exist only in the whole run
        self.assertNotIn("B only", text)

    def test_logs_without_counter_lines_are_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            Path(tmp, "rcomp_title.err").write_text("nothing here\n", encoding="utf-8")
            with self.assertRaises(SystemExit):
                bucket_tool.load(tmp)

    @staticmethod
    def run_dir_with_waits(tmp, name, seconds, waits):
        """Counter lines as printed by a title built with RCOMP_RUNTIME_WAIT_STATS (vb and waits fields)."""
        directory = Path(tmp, name)
        directory.mkdir()
        lines = [f"RCOMP-FPS fps=32.00 frame_ms=31.25 swaps={30 * (k + 1)} cpu_cores=3.50 vb=1:14,2:16 "
                 f"waits={waits[0]}:{waits[1]}:{waits[2]}" for k in range(seconds)]
        (directory / "rcomp_title.err").write_text("\n".join(lines) + "\n", encoding="utf-8")
        return directory

    def test_wait_counts_are_averaged_per_bucket_and_absent_without_the_field(self):
        with tempfile.TemporaryDirectory() as tmp:
            counted = bucket_tool.load(self.run_dir_with_waits(tmp, "counted", 60, (4000, 3000, 50)))
            plain = bucket_tool.load(self.run_dir(tmp, "plain", 60, 32.0, 3.5))
        self.assertEqual(counted[0][3], (4000, 3000, 50))
        got = bucket_tool.buckets(counted, 1200)
        self.assertEqual(got[0][3], (4000, 3000, 50))
        self.assertIsNone(bucket_tool.buckets(plain, 1200)[0][3])

    def test_the_wait_table_appears_only_when_an_arm_has_counts(self):
        def report(a_dir, b_dir):
            out = io.StringIO()
            old_argv = sys.argv
            sys.argv = ["bucket_compare.py", "--a", str(a_dir), "--b", str(b_dir), "--step", "1200"]
            try:
                with contextlib.redirect_stdout(out):
                    bucket_tool.main()
            finally:
                sys.argv = old_argv
            return out.getvalue()
        with tempfile.TemporaryDirectory() as tmp:
            plain = self.run_dir(tmp, "plain", 60, 32.0, 3.5)
            counted = self.run_dir_with_waits(tmp, "counted", 60, (4000, 3000, 50))
            text = report(plain, counted)
            self.assertIn("Guest waits per second", text)
            self.assertIn("4.0", text)                   # 4,000 ready waits a second, in thousands
            self.assertIn("3.0", text)
            self.assertIn("-", text.split("Guest waits per second")[1])   # the plain arm has no counts
            self.assertNotIn("Guest waits per second", report(plain, plain))


class SceneCompare(unittest.TestCase):
    """Runs aligned on content: the intro screen of a faster-loading build ends earlier."""
    WINDOWS = "one:3-9,two:15-25"

    @staticmethod
    def run_dir(tmp, name, intro_seconds, scale=1.0, cores=3.0):
        """60 fps for the intro, then scene one (10 s at 42 fps), then scene two (20 s at 37 fps)."""
        rates = [60.0] * intro_seconds + [42.0 * scale] * 10 + [37.0 * scale] * 20
        directory = Path(tmp, name)
        directory.mkdir()
        lines = [f"RCOMP-FPS fps={r:.2f} frame_ms={1000 / r:.2f} swaps={int(sum(rates[:k + 1]))} cpu_cores={cores:.2f}"
                 for k, r in enumerate(rates)]
        (directory / "rcomp_title.err").write_text("\n".join(lines) + "\n", encoding="utf-8")
        return directory

    def report(self, a_dirs, b_dirs, extra=()):
        out = io.StringIO()
        old_argv = sys.argv
        sys.argv = ["scene_compare.py", "--a", *map(str, a_dirs), "--b", *map(str, b_dirs), "--windows", self.WINDOWS,
                    *extra]
        try:
            with contextlib.redirect_stdout(out):
                scene_tool.main()
        finally:
            sys.argv = old_argv
        return out.getvalue()

    def test_time_zero_is_the_end_of_the_intro_screen(self):
        with tempfile.TemporaryDirectory() as tmp:
            rows = scene_tool.load(self.run_dir(tmp, "a", 34))
        self.assertEqual(scene_tool.first_change(rows, 50.0), 34)
        self.assertIsNone(scene_tool.first_change(rows[:30], 50.0))   # nothing after the intro in a short run

    def test_a_one_second_hitch_in_the_intro_is_not_the_end_of_the_intro(self):
        rows = ([(60.0, i) for i in range(25)] + [(34.0, 25)] + [(60.0, i) for i in range(26, 31)] +
                [(52.0, 31)] + [(51.0, i) for i in range(32, 45)])
        self.assertEqual(scene_tool.first_change(rows, 57.0), 31)
        self.assertEqual(scene_tool.first_change(rows, 57.0, sustained=1), 25)

    def test_the_same_content_shifted_by_the_intro_gives_no_difference(self):
        with tempfile.TemporaryDirectory() as tmp:
            text = self.report([self.run_dir(tmp, "slow_intro", 37)], [self.run_dir(tmp, "fast_intro", 34)])
        self.assertIn("B - A -3.0 s", text)              # the faster build leaves the intro 3 s earlier
        for line in text.splitlines():
            if line.lstrip().startswith(("one", "two")):
                self.assertIn("+0.00", line)             # but every scene runs at the same rate
                self.assertIn("+0.0%", line)

    def test_a_real_slowdown_shows_in_every_window_and_the_cores_in_their_column(self):
        with tempfile.TemporaryDirectory() as tmp:
            text = self.report([self.run_dir(tmp, "a", 35, scale=1.0, cores=3.5)],
                               [self.run_dir(tmp, "b", 35, scale=0.99, cores=3.4)])
        windows = [l for l in text.splitlines() if l.lstrip().startswith(("one", "two"))]
        self.assertEqual(len(windows), 2)
        for line in windows:
            self.assertIn("-1.0%", line)                 # the rate, 1 % lower
            self.assertIn("-2.9%", line)                 # the busy cores, 3.5 -> 3.4

    def test_a_window_a_run_does_not_reach_is_reported_not_extended(self):
        with tempfile.TemporaryDirectory() as tmp:
            whole = self.run_dir(tmp, "whole", 34)
            short = Path(tmp, "short")
            short.mkdir()
            lines = (whole / "rcomp_title.err").read_text(encoding="utf-8").splitlines()[:34 + 14]   # ends inside scene two
            (short / "rcomp_title.err").write_text("\n".join(lines) + "\n", encoding="utf-8")
            text = self.report([whole], [short])
        self.assertIn("only A reaches this window", text)
        self.assertIn("+0.0%", text)                     # scene one is in both

    def test_a_fixed_time_zero_compares_the_same_wall_clock_seconds(self):
        with tempfile.TemporaryDirectory() as tmp:
            a = self.run_dir(tmp, "a", 34)
            # b renders its first scene at the 60 fps lock: the detection would put time zero 10 s too late
            rates = [60.0] * 34 + [60.0] * 10 + [37.0] * 20
            b = Path(tmp, "b")
            b.mkdir()
            lines = [f"RCOMP-FPS fps={r:.2f} frame_ms={1000 / r:.2f} swaps={int(sum(rates[:k + 1]))} cpu_cores=3.00"
                     for k, r in enumerate(rates)]
            (b / "rcomp_title.err").write_text("\n".join(lines) + "\n", encoding="utf-8")
            detected = self.report([a], [b])
            fixed = self.report([a], [b], extra=("--t0", "34"))
        self.assertIn("B - A +10.0 s", detected)         # what the detection says
        one = next(l for l in fixed.splitlines() if l.lstrip().startswith("one"))
        two = next(l for l in fixed.splitlines() if l.lstrip().startswith("two"))
        self.assertIn("+42.9%", one)                     # scene one: 60 against 42 fps
        self.assertIn("+0.0%", two)                      # scene two: the same 37 fps
        self.assertIn("A [34]", fixed)
        self.assertIn("B [34]", fixed)

    def test_a_run_whose_rate_never_drops_is_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp, "flat")
            directory.mkdir()
            lines = [f"RCOMP-FPS fps=60.00 frame_ms=16.67 swaps={60 * (k + 1)} cpu_cores=2.00" for k in range(60)]
            (directory / "rcomp_title.err").write_text("\n".join(lines) + "\n", encoding="utf-8")
            with self.assertRaises(SystemExit):
                scene_tool.arm_summary([directory], 50.0, scene_tool.parse_windows(self.WINDOWS))


class PcShares(unittest.TestCase):
    BEFORE = """format 2 interrupted-rip; candidates never added to raw
== main raw total 1000
 13.5% sub_829CFE80(PPCContext&, unsigned char*)
  2.8% rcomp::lookup_function(unsigned int)
  3.5% __emutls_get_address
== main stack-scan heuristic; unvalidated candidate candidate total 100 raw denominator 1000
  9.7% of raw samples std::__1::chrono::steady_clock::now()
== others raw total 1000
 92.4% ?80000038c (outside verified title text)
"""
    AFTER = """format 2 interrupted-rip; candidates never added to raw
== main raw total 1000
 13.9% sub_829CFE80(PPCContext&, unsigned char*)
  0.4% rcomp::lookup_function(unsigned int)
== main stack-scan heuristic; unvalidated candidate candidate total 10 raw denominator 1000
  0.3% of raw samples std::__1::chrono::steady_clock::now()
== others raw total 1000
 91.0% ?80000038c (outside verified title text)
"""

    def write(self, directory, name, text):
        path = Path(directory, name)
        path.write_text(text, encoding="utf-8")
        return path

    def test_parse_reads_both_sections_per_group(self):
        with tempfile.TemporaryDirectory() as tmp:
            groups = pc_share.parse(self.write(tmp, "before.txt", self.BEFORE))
        self.assertEqual(sorted(groups), ["main", "others"])
        self.assertAlmostEqual(groups["main"]["raw"]["rcomp::lookup_function(unsigned int)"], 2.8)
        self.assertAlmostEqual(groups["main"]["stack"]["std::__1::chrono::steady_clock::now()"], 9.7)
        self.assertAlmostEqual(groups["others"]["raw"]["?80000038c (outside verified title text)"], 92.4)

    def test_table_lists_the_biggest_moves_and_the_requested_names(self):
        with tempfile.TemporaryDirectory() as tmp:
            before = pc_share.parse(self.write(tmp, "before.txt", self.BEFORE))["main"]["raw"]
            after = pc_share.parse(self.write(tmp, "after.txt", self.AFTER))["main"]["raw"]
        def listing(top, matches):
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                pc_share.table("t", before, after, top, matches)
            return out.getvalue()

        largest = listing(1, [])
        self.assertIn("__emutls_get_address", largest)   # 3.5 -> absent (0.0): the biggest move
        self.assertIn("-3.5%", largest)
        self.assertNotIn("lookup_function", largest)
        two = listing(2, ["sub_829"])
        self.assertIn("rcomp::lookup_function", two)     # 2.8 -> 0.4
        self.assertIn("-2.4%", two)
        self.assertIn("sub_829CFE80", two)               # a small move, listed because it was asked for
        self.assertIn("+0.4%", two)


if __name__ == "__main__":
    sys.exit(unittest.main(verbosity=2))
