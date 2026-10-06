#!/usr/bin/env python3
"""Host checks of tools/xenos_pass_analysis.py on small synthetic logs (no game data, no console)."""
import contextlib
import importlib.util
import io
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("xenos_pass_analysis", ROOT / "tools/xenos_pass_analysis.py")
tool = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tool)

LOG = """\
noise before the tables
RCOMP-PASS-TABLE wall_ms=20000 swaps=600 classes=2
RCOMP-PASS key=1 n=100 ticks=1000000 px=200000000 tiles=3000 transfer dest=D:D24FS8/4x src=D:D24FS8/2x mode=1 hostdepth=0
RCOMP-PASS key=3 n=50 ticks=500000 px=50000000 tiles=900 resolve-dump rt=C:k_8_8_8_8/1x
RCOMP-PASS-TABLE wall_ms=40000 swaps=1200 classes=3
RCOMP-PASS key=1 n=300 ticks=4000000 px=600000000 tiles=9000 transfer dest=D:D24FS8/4x src=D:D24FS8/2x mode=1 hostdepth=0
RCOMP-PASS key=3 n=150 ticks=1500000 px=150000000 tiles=2700 resolve-dump rt=C:k_8_8_8_8/1x
RCOMP-PASS key=7 n=10 ticks=100000 px=1000 tiles=0 resolve-clear rt=C:k_8_8_8_8/1x
"""

EVENTS = """\
RCOMP-EV D+ n=1 f=1500 rts=b0.p16.m2.D1,b720.p16.m0.C3,-,-,- prim=4 flags=2 sc=0,0,1280,720 vp=0,0,1280,720
RCOMP-EV T n=2 f=1500 slot=0 dest=b0.p16.m2.D1 src=b720.p16.m0.C3 hd=- t=720..1440 cl=0 r=0,0+1280x720
RCOMP-EV D- n=1 draws=3 verts=9 zr=0 zw=3 st=0 bl=0 pm=0 nops=0 kill=0 clip=0 zalways=0 rect=0 wd=0 claim=0+720,720+720,0+0,0+0,0+0
RCOMP-EV R n=3 f=1501 src=4 span=0+16x45/16 dest=0x05108000 extent=0x05108000+0x640000 w=1280 h=720 fmt=23 clr=00 copy=1
RCOMP-EV D+ n=4 f=3500 rts=-,-,-,-,- prim=4 flags=0 sc=0,0,0,0 vp=0,0,0,0
"""


class PassTables(unittest.TestCase):
    def test_tables_are_parsed_in_order(self):
        tables = tool.parse_tables(LOG.splitlines(keepends=True))
        self.assertEqual([t["swaps"] for t in tables], [600, 1200])
        self.assertEqual(tables[0]["classes"][1]["ticks"], 1000000)
        self.assertEqual(tables[1]["classes"][7]["desc"], "resolve-clear rt=C:k_8_8_8_8/1x")

    def test_the_difference_of_two_tables_is_the_work_between_them(self):
        first, last = tool.parse_tables(LOG.splitlines(keepends=True))
        frames, rows = tool.diff_tables(first, last)
        self.assertEqual(frames, 600)
        by_key = {r["key"]: r for r in rows}
        self.assertEqual(by_key[1]["n"], 200)
        self.assertEqual(by_key[1]["ticks"], 3000000)
        self.assertEqual(by_key[7]["n"], 10)  # a class missing from the first table started at zero

    def test_passes_prints_milliseconds_per_frame(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "t.err"
            path.write_text(LOG)
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                tool.main(["passes", str(path)])
        text = out.getvalue()
        self.assertIn("600 frames", text)
        # 3,000,000 ticks at 100 MHz = 30 ms over 600 frames = 0.050 ms per frame
        self.assertRegex(text, r"0\.050 ms\s+0\.33\s+150\.0 us")
        self.assertIn("transfer", text)


class Timeline(unittest.TestCase):
    def test_events_keep_their_fields(self):
        events = tool.parse_events(EVENTS.splitlines(keepends=True))
        self.assertEqual([e["kind"] for e in events], ["D+", "T", "D-", "R", "D+"])
        self.assertEqual(events[1]["t"], "720..1440")
        self.assertEqual(events[1]["src"], "b720.p16.m0.C3")

    def test_windows_split_on_a_gap_of_frames(self):
        groups = tool.windows(tool.parse_events(EVENTS.splitlines(keepends=True)))
        self.assertEqual([len(g) for g in groups], [4, 1])

    def test_keys(self):
        key = tool.parse_key("b1024.p16.m2.D0")
        self.assertEqual((key["base"], key["pitch"], key["msaa"], key["depth"], key["fmt"]), (1024, 16, 2, True, 0))
        self.assertIsNone(tool.parse_key("-"))


OVERWRITES = """\
RCOMP-EV D+ n=1 f=3500 rts=b0.p16.m2.D1,-,-,-,- prim=8 flags=366 sc=0,0,640,8192 vp=0,0,5461,5461 ow=0,0,640,256 why=0 pos=-0.5,-0.5,1 cand=1 est=1 eb=0.000,0.000,640.000,256.000
RCOMP-EV T n=2 f=3500 slot=0 dest=b0.p16.m2.D1 src=b720.p16.m0.C3 hd=- t=0..720 cl=0 r=0,256+640x464
RCOMP-EV D+ n=3 f=3500 rts=b0.p7.m2.D1,-,-,-,- prim=8 flags=366 sc=0,0,280,8192 vp=0,0,5461,5461 why=12 pos=1,2,3 cand=1 est=0 eb=0.000,0.000,0.000,0.000
RCOMP-EV D+ n=4 f=3500 rts=b0.p7.m2.D1,-,-,-,- prim=8 flags=362 sc=0,0,280,8192 vp=0,0,5461,5461 why=12 pos=1,2,3 cand=0 est=-1 eb=0.000,0.000,0.000,0.000
RCOMP-EV T n=5 f=3500 slot=1 dest=b224.p7.m2.C3 src=b184.p8.m2.C3 hd=- t=224..368 cl=0 r=0,0+280x160;0,160+160x8
"""


def budget_log(draw_ticks_per_frame):
    """A counter of 100 seconds at 30 swaps a second, and a profile snapshot every 600 swaps (20 s)."""
    lines = [f"RCOMP-FPS fps=30.00 frame_ms=33.33 swaps={30 * (second + 1)} cpu_cores=3.00" for second in range(100)]
    for snapshot in range(1, 5):
        swaps = 600 * snapshot
        frames = swaps
        lines.append(f"RCOMP-PROFT wall=100 22:{swaps}/1 0:{swaps}/1")
        lines.append(f"RCOMP-GPUT draw={draw_ticks_per_frame * frames} transfer={20000 * frames} resolve={30000 * frames} "
                     f"swap=0 texload={40000 * frames} gap={5000 * frames} span=1")
    return "\n".join(lines) + "\n"


class Windows(unittest.TestCase):
    def test_a_window_is_the_gpu_time_between_two_snapshots_per_frame(self):
        lines = budget_log(100000).splitlines(keepends=True)
        rows = tool.parse_budget(lines)
        result = tool.gpu_window(lines, rows, 19, 79)    # swaps 600 .. 2400 on the counter clock
        self.assertAlmostEqual(result["draw"], 1.0)      # 100,000 ticks at 100 MHz = 1 ms a frame
        self.assertAlmostEqual(result["transfer"], 0.2)
        self.assertAlmostEqual(result["resolve"], 0.3)
        self.assertAlmostEqual(result["texload"], 0.4)
        self.assertAlmostEqual(result["gap"], 0.05)
        self.assertAlmostEqual(result["busy"], 1.9)      # draw + transfer + resolve + texload (+ swap, none)
        self.assertAlmostEqual(result["fps"], 30.0)

    def test_a_window_the_log_does_not_reach_is_none(self):
        lines = budget_log(100000).splitlines(keepends=True)
        self.assertIsNone(tool.gpu_window(lines, tool.parse_budget(lines), 19, 150))

    def test_windows_prints_both_runs_and_the_difference(self):
        with tempfile.TemporaryDirectory() as directory:
            a, b = Path(directory) / "a.err", Path(directory) / "b.err"
            a.write_text(budget_log(100000))
            b.write_text(budget_log(70000))
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                tool.main(["windows", str(a), str(b), "--windows", "X:19-79,Y:19-500"])
        text = out.getvalue()
        diff = next(l for l in text.splitlines() if l.split()[:2] == ["X", "B-A"])
        self.assertIn("-0.30", diff.split()[3])          # the draw column: 0.7 ms against 1.0 ms
        self.assertIn("not spanned by both runs", text)  # window Y is beyond the log


class Overwrites(unittest.TestCase):
    def test_rectangle_area_adds_every_rectangle(self):
        self.assertEqual(tool.rectangle_area("0,0+280x160;0,160+160x8"), 280 * 160 + 160 * 8)
        self.assertEqual(tool.rectangle_area("-4,2+3x5"), 15)
        self.assertEqual(tool.rectangle_area(""), 0)

    def test_overwrites_reports_candidates_verdicts_and_transfers(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "t.err"
            path.write_text(OVERWRITES)
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                tool.main(["overwrites", str(path)])
        text = out.getvalue()
        # three rectangle draws, two candidates (both depth), one estimated, one overwriting
        self.assertIn("rectangle draws 3, candidates 2 (depth 2, color 0), estimated 1, overwriting 1", text)
        # the transfers: 720 + 144 tiles, 640*464 + 280*160 + 160*8 pixels
        self.assertIn("transfers 2, 864 tiles, %d pixels" % (640 * 464 + 280 * 160 + 160 * 8), text)
        self.assertIn("why=12: 1", text)

    def test_overwrites_needs_timeline_events(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "t.err"
            path.write_text("RCOMP-FPS fps=40.0 frame_ms=25.0 swaps=100\n")
            with self.assertRaises(SystemExit):
                tool.main(["overwrites", str(path)])


def drawclass_log():
    """100 counter seconds at 30 swaps a second, profile rows and two pass tables carrying one draw run class and one texture load class."""
    lines = [f"RCOMP-FPS fps=30.00 frame_ms=33.33 swaps={30 * (second + 1)} cpu_cores=3.00" for second in range(100)]
    for snapshot in (1, 4):
        swaps = 600 * snapshot
        # 2 GHz: 2,000 Mcycles of wall time per counter second (the wall counter is in millions of cycles)
        lines.append(f"RCOMP-PROFT wall={swaps // 30 * 2000} 22:{swaps}/1 0:{swaps}/1")
        lines.append("RCOMP-GPUT draw=0 transfer=0 resolve=0 swap=0 texload=0 gap=0 span=1")
    key = (1 << 4) | 6          # draw run, depth bound only
    tables = [(600, 20000, 10, 1000000, 100000, 700, 50, 2000000000), (2400, 80000, 40, 181000000, 400000, 2800, 130, 9200000000)]
    out = []
    for swaps, wall_ms, n, ticks, verts, draws, load_n, cycles in tables:
        out.append(f"RCOMP-PASS-TABLE wall_ms={wall_ms} swaps={swaps} classes=2")
        out.append(f"RCOMP-PASS key={key:x} n={n} ticks={ticks} px={verts} tiles={draws} draw-run pitch=16 state=8 d=D:D24FS8/1x")
        out.append(f"RCOMP-PASS key=7 n={load_n} ticks=5000000 px=1 tiles=1 texload fmt=6 1280x720 scaled=1")
        out.append(f"RCOMP-CPCLASS key={key:x} draws={draws} cycles={cycles} draw-run pitch=16 state=8 d=D:D24FS8/1x")
    # the log has the profile rows first, then the tables
    return "\n".join(lines + out) + "\n"


class FineSlots(unittest.TestCase):
    def test_slots_from_64_on_are_scaled_by_the_sampling_factor(self):
        lines = [f"RCOMP-FPS fps=30.00 frame_ms=33.33 swaps={30 * (second + 1)} cpu_cores=3.00" for second in range(100)]
        # 100 Mcycles of wall per snapshot; the sampled slot 64 counts 10 draws and 10 Mcycles between the snapshots
        lines.append("RCOMP-PROFT wall=0 22:600/1 64:0/0")
        lines.append("RCOMP-GPUT draw=0 transfer=0 resolve=0 swap=0 texload=0 gap=0 span=1")
        lines.append("RCOMP-PROFT wall=1000 22:1200/1 64:10/10")
        lines.append("RCOMP-GPUT draw=0 transfer=0 resolve=0 swap=0 texload=0 gap=0 span=1")
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "t.err"
            path.write_text("\n".join(lines) + "\n")
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                tool.main(["budget", str(path), "--swaps", "600", "1200"])
        row = next(l for l in out.getvalue().splitlines() if "tex base request" in l)
        # 10 sampled Mcycles x 8 = 80 Mcycles of 1,000 over 20 counter seconds = 1.6 s... per 600 frames: 80/1000*20*1000/600 = 2.667 ms
        self.assertIn("2.667 ms", row)
        # 10 sampled draws x 8 over 600 frames
        self.assertIn("0.1", row.split("ms")[1])

    def test_counter_slots_print_events_and_units_per_frame_without_the_sampling_factor(self):
        lines = [f"RCOMP-FPS fps=30.00 frame_ms=33.33 swaps={30 * (second + 1)} cpu_cores=3.00" for second in range(100)]
        # slot 100: 600 uploads of 6,000 bytes each between the snapshots; slot 98 (a fine scope, sampled) 5 calls and 2 Mcycles
        lines.append("RCOMP-PROFT wall=0 22:600/1 98:0/0 100:0/0 116:0/0")
        lines.append("RCOMP-GPUT draw=0 transfer=0 resolve=0 swap=0 texload=0 gap=0 span=1")
        lines.append("RCOMP-PROFT wall=1000 22:1200/1 98:5/2 100:600/3600000 116:1200/900")
        lines.append("RCOMP-GPUT draw=0 transfer=0 resolve=0 swap=0 texload=0 gap=0 span=1")
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "t.err"
            path.write_text("\n".join(lines) + "\n")
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                tool.main(["budget", str(path), "--swaps", "600", "1200"])
        text = out.getvalue()
        fine = next(l for l in text.splitlines() if "tex request: bindings update" in l)
        self.assertIn("0.533 ms", fine)  # 2 sampled Mcycles x 8 of 1,000 over 20 counter seconds, per 600 frames
        self.assertIn("0.1", fine.split("ms")[1])  # 5 sampled calls x 8 = 40 over 600 frames
        counters = text.split("counters (per frame: events, units):")[1].splitlines()
        upload = next(l for l in counters if "upload vertex buffers" in l)
        self.assertEqual(upload.split()[-2:], ["1.0", "6000.0"])  # 600 events and 3.6e6 bytes over 600 frames
        epoch = next(l for l in counters if "no state register" in l)
        self.assertEqual(epoch.split()[-2:], ["2.0", "1.5"])  # 1,200 draws, 900 of them with no state change, over 600 frames


class CommandStatistics(unittest.TestCase):
    def test_cmdstat_prints_counts_and_time_per_command_type(self):
        lines = [f"RCOMP-FPS fps=30.00 frame_ms=33.33 swaps={30 * (second + 1)} cpu_cores=3.00" for second in range(100)]
        # wall 1,000 Mcycles over the 20 counter seconds between the snapshots (swaps 600 and 1200)
        lines.append("RCOMP-PROFT wall=0 22:600/1")
        lines.append("RCOMP-GPUT draw=0 transfer=0 resolve=0 swap=0 texload=0 gap=0 span=1")
        lines.append("RCOMP-CMDSTAT swaps=600 Draw:100/1 BindDescriptorSets:300/2")
        lines.append("RCOMP-PROFT wall=1000 22:1200/1")
        lines.append("RCOMP-GPUT draw=0 transfer=0 resolve=0 swap=0 texload=0 gap=0 span=1")
        lines.append("RCOMP-CMDSTAT swaps=1200 Draw:6100/31 BindDescriptorSets:12300/92")
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "t.err"
            path.write_text("\n".join(lines) + "\n")
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                tool.main(["cmdstat", str(path), "--swaps", "600", "1200"])
        text = out.getvalue()
        draw = next(l for l in text.splitlines() if l.strip().startswith("Draw"))
        self.assertEqual(draw.split()[1], "10.0")  # 6,000 draws over 600 frames
        self.assertIn("1.000 ms", draw)  # 30 Mcycles of 1,000 over 20 counter seconds: 0.6 s, per 600 frames: 1.000 ms
        bind = next(l for l in text.splitlines() if l.strip().startswith("BindDescriptorSets"))
        self.assertEqual(bind.split()[1], "20.0")
        total = next(l for l in text.splitlines() if l.strip().startswith("total"))
        self.assertEqual(total.split()[1], "30.0")


class DrawClasses(unittest.TestCase):
    def test_cp_classes_are_parsed_into_their_table(self):
        tables = tool.parse_tables(drawclass_log().splitlines(keepends=True))
        self.assertEqual(len(tables), 2)
        self.assertEqual(tables[1]["cp"][(1 << 4) | 6]["draws"], 2800)

    def test_drawclasses_charges_gpu_and_command_processor_time_per_frame(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "t.err"
            path.write_text(drawclass_log())
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                tool.main(["drawclasses", str(path), "--seconds", "19", "79"])
        text = out.getvalue()
        # 180,000,000 ticks (1.8 s) over 1800 frames = 1.000 ms; 7,200,000,000 cycles at 2 GHz (3.6 s) over 1800 frames
        # = 2.000 ms (the window is 60 counter seconds); 2,100 draws over 1800 frames = 1.2 draws a frame
        self.assertRegex(text, r"draw runs by class: GPU 1\.000 ms, CP 2\.000 ms, 1 draws per frame")
        self.assertRegex(text, r"1\.000\s+2\.000\s+1\.2\s+")
        self.assertIn("draw runs by render target set", text)
        self.assertNotIn("state=", text.split("draw runs by render target set")[1])

    def test_drawclasses_needs_the_new_classes(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "t.err"
            path.write_text(LOG)
            with self.assertRaises(SystemExit):
                tool.main(["drawclasses", str(path), "--seconds", "1", "2"])


if __name__ == "__main__":
    unittest.main()
