#!/usr/bin/env python3
"""Run tools/m6_inventory.py over the local disc catalog, one title at a time.

  python3 tools/catalog_inventory.py [--recompile] [--order build/catalog/recompile_order.json]
         [--ids ID ...] [--tool-timeout 300] [--decode-tool ...] [--analyse-tool ...]
         [--recomp-tool ...] [--xenon-source ...] [--force]

Reads build/catalog/<id>/disc.json (tools/iso_extract.py). For every PASS disc
with a default.xex:
  without --recompile: decode + imports + jump tables into <id>/inventory/;
  with --recompile:    the same plus XenonRecomp into <id>/inventory-recompile/,
                       then deletes that run's regenerable ppc/ sources once
                       inventory.json is written, so the disk does not fill up.
Each run also writes run.json (command, exit code, duration, tool hashes and
the tool's last output lines) so a decode failure or a timeout is recorded as
FAIL instead of being skipped. Everything stays under build/catalog/.
"""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build"
sys.path.insert(0, str(ROOT / "tools"))
from m6_inventory import TOOLS, XENON_SRC  # noqa: E402


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while chunk := f.read(8 << 20):
            h.update(chunk)
    return h.hexdigest()


def executable(path):
    p = Path(path)
    return p if p.is_file() else p.with_name(p.name + ".exe")


def entries(catalog, ids, order):
    found = {}
    for disc_json in sorted(Path(catalog).glob("*/disc.json")):
        disc = json.loads(disc_json.read_text(encoding="utf-8"))
        found[disc_json.parent.name] = disc
    keys = list(found)
    if order:
        ranked = [k for k in json.loads(Path(order).read_text(encoding="utf-8"))["order"] if k in found]
        keys = ranked + [k for k in keys if k not in ranked]
    if ids:
        keys = [k for k in keys if k in ids]
    return [(k, found[k]) for k in keys]


def remove_regenerable(path, catalog):
    """Delete a previous run directory: only inside build/catalog/<id>/, never a link."""
    path = Path(path)
    if path.is_symlink() or not path.resolve().is_relative_to(Path(catalog).resolve()) or \
            path.parent.parent.resolve() != Path(catalog).resolve():
        raise SystemExit(f"refusing to delete {path}")
    shutil.rmtree(path)


def run_one(ident, disc, catalog, a, tools):
    entry = Path(catalog) / ident
    out = entry / ("inventory-recompile" if a.recompile else "inventory")
    record = {"tool": "tools/catalog_inventory.py", "id": ident, "recompile": a.recompile,
              "date": datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="seconds"),
              "tool_timeout": a.tool_timeout}
    run_json = entry / (out.name + ".run.json")
    if disc.get("status") != "PASS" or not (disc.get("default_xex") or {}).get("present"):
        record.update(status="NOT TESTED", reason="no extracted default.xex (see disc.json)")
        run_json.write_text(json.dumps(record, indent=1) + "\n", encoding="utf-8")
        return record
    if out.exists():
        if (out / "inventory.json").is_file() and not a.force:
            print(f"{ident}: {out.name} exists; --force to redo")
            return json.loads(run_json.read_text(encoding="utf-8")) if run_json.is_file() else record
        remove_regenerable(out, catalog)
    xex = entry / "disc" / disc["default_xex"]["path"]
    cmd = [sys.executable, str(ROOT / "tools/m6_inventory.py"), str(xex), "--out", str(out),
           "--tool-timeout", str(a.tool_timeout), "--decode-tool", str(tools["decode"]),
           "--analyse-tool", str(tools["analyse"]), "--recomp-tool", str(tools["recomp"]),
           "--xenon-source", str(a.xenon_source)]
    if a.recompile:
        cmd.append("--recompile")
    record["command"] = [os.path.relpath(c, ROOT) if os.path.isabs(c) else c for c in cmd[1:]]
    record["tools"] = {k: {"path": os.path.relpath(p, ROOT), "sha256": sha256(p)} for k, p in tools.items()
                       if a.recompile or k != "recomp"}
    started = time.time()
    # The tool bounds each child by --tool-timeout; this outer bound only catches a hang of its own.
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, errors="replace",
                           timeout=a.tool_timeout * 4 + 120)
        rc, stdout, stderr = p.returncode, p.stdout, p.stderr
    except subprocess.TimeoutExpired:
        rc, stdout, stderr = 124, "", f"catalog_inventory: m6_inventory exceeded {a.tool_timeout * 4 + 120} s"
    record.update(exit_code=rc, seconds=round(time.time() - started, 1),
                  output_tail=(stdout + stderr).strip().splitlines()[-6:])
    report = out / "inventory.json"
    if report.is_file():
        inv = json.loads(report.read_text(encoding="utf-8"))
        x = inv.get("xenonrecomp", {})
        timed_out = x.get("exit_code") == 124 or inv.get("jump_tables", {}).get("xenonanalyse_exit") == 124
        record["status"] = "FAIL" if timed_out or rc else "PASS"
        record["reason"] = "tool timeout" if timed_out else (f"m6_inventory exited {rc}" if rc else None)
        if a.recompile and (out / "ppc").is_dir():
            record["ppc_bytes_deleted"] = sum(f.stat().st_size for f in (out / "ppc").rglob("*") if f.is_file())
            shutil.rmtree(out / "ppc")
    else:
        record["status"] = "FAIL"
        record["reason"] = ("tool timeout" if rc == 124 else "no inventory.json: " +
                            " | ".join(record["output_tail"])[-500:])
    run_json.write_text(json.dumps(record, indent=1) + "\n", encoding="utf-8")
    print(f"{ident}: {record['status']}" + (f" ({record['reason']})" if record.get("reason") else "") +
          f" in {record['seconds']} s")
    return record


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--catalog", default=str(BUILD / "catalog"))
    ap.add_argument("--recompile", action="store_true")
    ap.add_argument("--order", help="JSON with an 'order' list of ids (tools/aggregate_inventories.py)")
    ap.add_argument("--ids", nargs="*")
    ap.add_argument("--tool-timeout", type=int, default=300)
    ap.add_argument("--decode-tool", default=TOOLS["decode"])
    ap.add_argument("--analyse-tool", default=TOOLS["analyse"])
    ap.add_argument("--recomp-tool", default=TOOLS["recomp"])
    ap.add_argument("--xenon-source", default=XENON_SRC)
    ap.add_argument("--force", action="store_true", help="redo runs that already have an inventory.json")
    a = ap.parse_args()
    if not Path(a.catalog).resolve().is_relative_to(BUILD.resolve()):
        ap.error("the catalog must stay under build/")
    tools = {"decode": executable(a.decode_tool), "analyse": executable(a.analyse_tool),
             "recomp": executable(a.recomp_tool)}
    for k, p in tools.items():
        if (a.recompile or k != "recomp") and not p.is_file():
            ap.error(f"missing {k} tool {p}")
    results = [run_one(k, d, a.catalog, a, tools) for k, d in entries(a.catalog, a.ids, a.order)]
    print(f"{sum(r.get('status') == 'PASS' for r in results)}/{len(results)} PASS")
    sys.exit(1 if any(r.get("status") == "FAIL" for r in results) else 0)


if __name__ == "__main__":
    main()
