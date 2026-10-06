#!/usr/bin/env python3
"""Compare PS5 CPU corpus results with the host reference, test by test.

  tools/compare_cpu_results.py HOST.jsonl PS5.jsonl

HOST.jsonl is shipped in the kit (expected/cpu-host.jsonl, same corpus build);
PS5.jsonl comes from platform/ps5/tools/collect_cpu_results.py. Prints every
test whose status differs and a summary; exit 0 only if all executed tests
agree and none is missing.
"""
import json
import sys


def load(path):
    out = {}
    with open(path) as f:
        for line in f:
            line = line.strip()
            if line.startswith("{"):
                r = json.loads(line)
                if "id" in r and "status" in r:
                    out[r["id"]] = r
    return out


def main():
    host, ps5 = load(sys.argv[1]), load(sys.argv[2])
    diff = missing = 0
    for tid, h in sorted(host.items()):
        p = ps5.get(tid)
        if p is None:
            missing += 1
            continue
        if p["status"] != h["status"]:
            diff += 1
            print(f"DIFF {tid}: host {h['status']} ps5 {p['status']} {p.get('reason', '')[:200]}")
    extra = len(set(ps5) - set(host))
    print(json.dumps({"host": len(host), "ps5": len(ps5), "status_differences": diff,
                      "missing_on_ps5": missing, "only_on_ps5": extra}))
    return 0 if diff == 0 and missing == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
