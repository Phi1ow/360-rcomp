#!/usr/bin/env python3
"""Extract CPU-corpus results from PS5 test-title logs (owner: Agent 2).

    collect_cpu_results.py LOG [LOG ...] --out results.jsonl [--meta meta.json]

LOG is an rcomp_title.log fetched from the console (run_title.sh saves it as
<RCOMP_RUN_LOG_DIR>/<ID>-<stamp>/rcomp_title.log). From each LOG only the LAST
run is used: the text after the last "RCOMP-TITLE begin" line. Several LOGs
are given when a crashed corpus was resumed (RCOMP_CPU_START); they are merged
in the order given, a later result for the same test id replacing an earlier
one, exactly like tests/cpu/run_cpu_tests.py does across restarts.

Output: one JSON object per test (the harness's own lines, unchanged), sorted by
index, in --out. --meta writes per-run facts (title build, platform status, cpu
start/status, crash line, count) for PRIME's report. Nothing here is PS5
evidence by itself: it is only as good as the log it reads.

Exit: 0 last run completed (cpu status=0), 1 incomplete (crash, no cpu status,
nonzero status), 2 no results / usage error.
"""
import argparse
import json
import re
import sys

BEGIN = re.compile(r"^RCOMP-TITLE begin (.*)$")
KV = re.compile(r"(\w+)=(\S+)")


def last_run(text):
    lines = text.splitlines()
    starts = [i for i, l in enumerate(lines) if BEGIN.match(l)]
    if not starts:
        return None, []
    return BEGIN.match(lines[starts[-1]]).group(1), lines[starts[-1] + 1:]


def parse_run(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        header, lines = last_run(f.read())
    meta = {"log": path, "begin": header, "platform_status": None, "cpu_start": None,
            "cpu_status": None, "title_status": None, "crash": None, "results": 0,
            "bad_lines": 0}
    results = []
    if header is None:
        return meta, results
    last_begin = None
    for line in lines:
        mb = re.match(r"^RCOMP-CPU begin index=(\d+) id=(\S+)$", line)
        if mb:
            last_begin = (int(mb.group(1)), mb.group(2))
            continue
        if line.startswith("{"):
            try:
                r = json.loads(line)
            except json.JSONDecodeError:
                meta["bad_lines"] += 1  # e.g. a line cut by a crash
                continue
            if not isinstance(r, dict) or "id" not in r or "status" not in r:
                meta["bad_lines"] += 1
                continue
            results.append(r)
            if r.get("stage") == "execute" and str(r.get("reason", "")).startswith("signal "):
                meta["crash"] = r
            continue
        m = re.match(r"^RCOMP-TITLE (platform|cpu|end)\b(.*)$", line)
        if not m:
            continue
        kv = dict(KV.findall(m.group(2)))
        if m.group(1) == "platform":
            meta["platform_status"] = int(kv.get("status", -1))
        elif m.group(1) == "cpu" and "start" in kv:
            meta["cpu_start"] = int(kv["start"])
        elif m.group(1) == "cpu" and "status" in kv:
            meta["cpu_status"] = int(kv["status"])
        elif m.group(1) == "end":
            meta["title_status"] = int(kv.get("status", -1))
    # A test that started but never reported ended the process (no signal
    # handler runs on PS5): record it as a crash so the run can resume after it.
    if meta["crash"] is None and meta["title_status"] is None and last_begin is not None \
            and not any(r.get("index") == last_begin[0] for r in results):
        crash = {"id": last_begin[1], "index": last_begin[0], "status": "FAIL", "stage": "execute",
                 "reason": "process ended during the test (no result line)"}
        results.append(crash)
        meta["crash"] = crash
    meta["results"] = len(results)
    return meta, results


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("logs", nargs="+")
    ap.add_argument("--out", required=True, help="output .jsonl")
    ap.add_argument("--meta", help="optional per-run metadata .json")
    a = ap.parse_args()

    merged, runs = {}, []
    for path in a.logs:
        meta, results = parse_run(path)
        runs.append(meta)
        for r in results:
            merged[r["id"]] = r
    if not merged:
        print("no CPU results found (no 'RCOMP-TITLE begin' run with JSON lines)", file=sys.stderr)
        return 2
    ordered = sorted(merged.values(), key=lambda r: (r.get("index", 1 << 30), r["id"]))
    with open(a.out, "w", encoding="utf-8") as f:
        for r in ordered:
            f.write(json.dumps(r, separators=(",", ":")) + "\n")
    last = runs[-1]
    completed = last["cpu_status"] == 0 and last["crash"] is None
    counts = {}
    for r in ordered:
        counts[r["status"]] = counts.get(r["status"], 0) + 1
    summary = {"platform": "ps5", "completed": completed, "counts": counts, "runs": runs}
    if a.meta:
        with open(a.meta, "w", encoding="utf-8") as f:
            json.dump(summary, f, indent=1)
    print(json.dumps({"platform": "ps5", "completed": completed, "counts": counts,
                      "next_start": (last["crash"]["index"] + 1) if last["crash"] else None}))
    return 0 if completed else 1


if __name__ == "__main__":
    sys.exit(main())
