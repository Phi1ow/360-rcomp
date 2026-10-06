#!/usr/bin/env python3
"""Run the CPU corpus binary and write a machine-readable report (Agent 6).

The report merges:
  * static results from gen_harness.py (BLOCKED at assembly, FAIL at
    generation: those tests are never executed and never counted as PASS);
  * execution results from the test binary (restarted after a crash so one
    faulting test cannot hide the others).

It records source SHAs and dirty state, sha256 of inputs and artefacts, the
exact commands with return codes and stderr, the environment and the scope.
Host execution is labelled platform=host; it is never evidence for PS5.
"""
import argparse
import hashlib
import json
import os
import platform
import subprocess
import sys
import time


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 16), b""):
            h.update(chunk)
    return h.hexdigest()


def git(path, *a):
    return subprocess.run(["git", "-C", path, *a], capture_output=True, text=True).stdout.strip()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", required=True)
    ap.add_argument("--corpus", required=True)
    ap.add_argument("--cases", required=True)
    ap.add_argument("--root", required=True)
    ap.add_argument("--report", required=True)
    ap.add_argument("--label", default="host")
    ap.add_argument("--isa", default="")
    ap.add_argument("--timeout", type=int, default=300)
    args = ap.parse_args()

    runs, results, start = [], {}, 0
    while True:
        cmd = [args.binary, "--start", str(start)]
        t0 = time.time()
        try:
            p = subprocess.run(cmd, capture_output=True, text=True, timeout=args.timeout)
            rc, out, err = p.returncode, p.stdout, p.stderr
        except subprocess.TimeoutExpired as e:
            rc, out, err = "timeout", (e.stdout or b"").decode(errors="replace") if isinstance(e.stdout, bytes) else (e.stdout or ""), "timeout"
        runs.append({"cmd": cmd, "rc": rc, "seconds": round(time.time() - t0, 3), "stderr_tail": err[-4000:]})
        last = -1
        for line in out.splitlines():
            if line.startswith("{"):
                r = json.loads(line)
                results[r["id"]] = r
                last = max(last, r["index"])
        if rc == 3 and last >= start:
            start = last + 1  # crashed test already reported; continue after it
            continue
        break

    cases = json.load(open(os.path.join(args.cases, "cases.json")))
    for r in cases["static_results"]:
        results.setdefault(r["id"], r)
    counts = {}
    for r in results.values():
        counts[r["status"]] = counts.get(r["status"], 0) + 1
    completed = runs[-1]["rc"] == 0

    manifest = json.load(open(os.path.join(args.corpus, "manifest.json")))
    xr = os.path.join(args.root, "third_party", "XenonRecomp")
    report = {
        "schema": "rcomp-cpu-report/1",
        "platform": args.label,
        "scope": "XenonRecomp test-mode corpus: PPC fixture -> GNU as -> XenonRecomp -> clang -> execution "
                 "against fixture annotations. Host results are not PS5 evidence.",
        "completed": completed,
        "counts": counts,
        "sources": {
            "rcomp": {"commit": git(args.root, "rev-parse", "HEAD") or "uncommitted",
                      "dirty_files": len(git(args.root, "status", "--porcelain").splitlines())},
            "XenonRecomp": {"commit": git(xr, "rev-parse", "HEAD"),
                            "dirty_files": len(git(xr, "status", "--porcelain").splitlines())},
            "fixture_sets": manifest["sets"],
        },
        "generator": manifest["generator"],
        "tools": manifest["tools"],
        "artefacts": {"binary": os.path.abspath(args.binary), "binary_sha256": sha256(args.binary),
                      "corpus_manifest_sha256": sha256(os.path.join(args.corpus, "manifest.json")),
                      "cases_sha256": sha256(os.path.join(args.cases, "cases.json"))},
        "isa_flags": args.isa,
        "environment": {"host": platform.platform(), "machine": platform.machine(),
                        "python": platform.python_version()},
        "runs": runs,
        "results": sorted(results.values(), key=lambda r: r["id"]),
    }
    os.makedirs(os.path.dirname(os.path.abspath(args.report)), exist_ok=True)
    with open(args.report, "w") as f:
        json.dump(report, f, indent=1)
    print(json.dumps({"platform": args.label, "completed": completed, "counts": counts}))
    return 0 if completed else 1


if __name__ == "__main__":
    sys.exit(main())
