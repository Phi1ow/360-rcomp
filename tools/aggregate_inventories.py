#!/usr/bin/env python3
"""Aggregate the disc catalog into per-title stages, ranked blockers and a work plan.

  python3 tools/aggregate_inventories.py [--catalog build/catalog]
          [--evidence docs/compatibility_evidence.json] [--baseline build/catalog/aggregate.prev.json]
          [--docs docs/COMPATIBILITY.md] [--date YYYY-MM-DD]

Reads build/catalog/<id>/disc.json (tools/iso_extract.py), and the
inventory.json files of <id>/inventory/ (decode + imports + jump tables) and
<id>/inventory-recompile/ (plus XenonRecomp) with their run records
(tools/catalog_inventory.py). Writes:
  build/catalog/aggregate.json         machine-readable result (local);
  build/catalog/REPORT.md              full local report (may name the images);
  build/catalog/recompile_order.json   the order for the --recompile pass;
  docs/COMPATIBILITY.md                sanitized: title/media IDs, disc numbers,
                                       stages, statuses, import names, counts.
The evidence file records what the inventories cannot see (host boot, PS5
boot, rendering, playable; engine; peripheral-only or online-only tags) as
metadata with a reference to the document that holds the proof.
"""
import argparse
import collections
import datetime
import json
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
CATALOG = ROOT / "build" / "catalog"
EVIDENCE = ROOT / "docs" / "compatibility_evidence.json"
DOCS = ROOT / "docs" / "COMPATIBILITY.md"
STAGES = ("extract", "decode", "analyse", "recompile")
LATER_STAGES = ("host boot", "PS5 boot", "rendering", "playable")
STATUSES = ("PASS", "FAIL", "BLOCKED", "NOT TESTED")
# Tags that park a title for later (prompt: leave them, BLOCKED with that reason).
PARKING_TAGS = {"kinect": "Kinect title", "peripheral-only": "requires a peripheral other than a controller",
                "multi-disc": "multi-disc set", "install-disc": "install or data disc",
                "online-only": "online-only title"}
KINECT_IMPORT = re.compile(r"^(XamNui|Nui[A-Z])")
SIGNATURES = {  # generator warning key -> failure signature
    "switch_without_table": "switch tables", "switch_case_outside_function": "switch tables",
    "functions_ending_prematurely": "function bounds", "direct_calls_without_function": "function bounds",
    "undecodable_instructions": "undecodable instructions", "rc_bit_without_comparison": "rc bit without comparison",
    "helper_address_unspecified": "register helpers", "unclassified_diagnostic": "unclassified diagnostics",
}


def supported_kernel_profiles(root=ROOT):
    """The owner-approved packed kernel profiles the runtime accepts (runtime/src/kernel_variables.cpp)."""
    try:
        text = (Path(root) / "runtime/src/kernel_variables.cpp").read_text(encoding="utf-8")
    except OSError:
        return None
    m = re.search(r"kSupportedPackedProfiles\[\]\s*=\s*\{([^}]*)\}", text)
    return {int(v, 16) for v in re.findall(r"0x([0-9A-Fa-f]+)", m[1])} if m else None


def packed_version(text):
    major, minor, build, qfe = (int(x) for x in text.split("."))
    return major << 28 | minor << 24 | build << 8 | qfe


def kernel_profile_blocker(libraries, supported):
    """Mirror of register_xboxkrnl_kernel_variables_from_xex: what stops Title::Create."""
    libs = {lib["name"].lower(): lib for lib in libraries}
    kernel, xam = libs.get("xboxkrnl.exe"), libs.get("xam.xex")
    if not kernel:
        return "kernel compatibility profile: no xboxkrnl.exe import library"
    for name, lib in (("xboxkrnl.exe", kernel), ("xam.xex", xam)):
        if lib and lib["version"] != lib["min_version"]:
            return (f"kernel compatibility profile conflict: {name} version {lib['version']} "
                    f"differs from its minimum {lib['min_version']}")
    if xam and xam["min_version"] != kernel["min_version"]:
        return "kernel compatibility profile conflict: xam.xex and xboxkrnl.exe minimums differ"
    if supported is not None and packed_version(kernel["min_version"]) not in supported:
        return f"kernel compatibility profile {kernel['min_version']} is not an owner-approved profile"
    return None


def load(path):
    try:
        return json.loads(Path(path).read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None


def key_of(title_id, media_id, disc_number):
    return f"{title_id}:{media_id}:{disc_number}"


def title_record(ident, entry, evidence, supported=None):
    disc = load(entry / "disc.json") or {}
    inv = load(entry / "inventory" / "inventory.json")
    inv_run = load(entry / "inventory.run.json")
    rec = load(entry / "inventory-recompile" / "inventory.json")
    rec_run = load(entry / "inventory-recompile.run.json")
    header = ((disc.get("default_xex") or {}).get("header") or {})
    info = header.get("execution_info") or {}
    t = {"id": ident, "title_id": info.get("title_id"), "media_id": info.get("media_id"),
         "disc_number": info.get("disc_number"), "disc_count": info.get("disc_count"),
         "version": info.get("version"), "image_name": (disc.get("iso") or {}).get("file_name"),
         "layout": (disc.get("disc") or {}).get("layout"),
         "xdk": sorted({lib["version"] for lib in header.get("import_libraries", [])}),
         "secondary_modules": (disc.get("flags") or {}).get("secondary_modules", 0),
         "stage": None, "failures": [], "missing_imports": [], "missing_variables": [],
         "unknown_ordinals": [], "generator": {}, "ambiguous_helpers": [], "signatures": [],
         "tags": set(), "later": None, "engine": None, "date": (disc.get("date") or "")[:10],
         "boot_blockers": []}
    t["key"] = key_of(t["title_id"], t["media_id"], t["disc_number"])
    if header.get("import_libraries"):
        profile = kernel_profile_blocker(header["import_libraries"], supported)
        if profile:
            t["boot_blockers"].append(profile)
    flags = disc.get("flags") or {}
    if flags.get("multi_disc"):
        t["tags"].add("multi-disc")
    if flags.get("install_disc_suspected"):
        t["tags"].add("install-disc")
    ev = (evidence.get("titles") or {}).get(t["key"]) or {}
    t["tags"].update(ev.get("tags", []))
    t["engine"] = ev.get("engine")
    t["later"] = ev.get("stages")
    # extract
    if disc.get("status") != "PASS":
        t["failures"].append(("extract", disc.get("reason") or "disc.json missing"))
        return t
    t["stage"] = "extract"
    if not (disc.get("default_xex") or {}).get("present"):
        t["failures"].append(("decode", "no default.xex on the disc"))
        return t
    # decode / analyse (prefer the recompile run: it repeats both)
    source = rec or inv
    run = rec_run if rec else inv_run
    if source is None:
        reason = (run or {}).get("reason") or "inventory not run"
        t["failures"].append(("decode", reason) if run else ("decode", "NOT TESTED: inventory not run"))
        return t
    t["date"] = max(t["date"], ((run or {}).get("date") or "")[:10])
    if not source.get("decode", {}).get("supported"):
        t["failures"].append(("decode", "unsupported compression/LZX form"))
        return t
    t["stage"] = "decode"
    for module, d in sorted(source.get("imports", {}).items()):
        t["missing_imports"] += [f"{module}:{n}" for n in d.get("functions_missing", [])]
        t["missing_variables"] += [f"{module}:{n}" for n in d.get("variables_missing", [])]
        t["unknown_ordinals"] += [f"{module}:{o}" for o in d.get("unknown_ordinals", [])]
        for name in d.get("functions_missing", []) + d.get("functions_implemented", []):
            if module.lower() == "xam.xex" and KINECT_IMPORT.match(name):
                t["tags"].add("kinect")
    t["ambiguous_helpers"] = sorted(k for k, v in source.get("register_helpers", {}).items() if v.get("matches", 0) > 1)
    jt = source.get("jump_tables", {})
    if jt.get("xenonanalyse_exit") != 0 or jt.get("tables") is None:
        why = "timeout" if jt.get("xenonanalyse_exit") == 124 else f"exit {jt.get('xenonanalyse_exit')}"
        t["failures"].append(("analyse", f"XenonAnalyse {why}"))
        t["signatures"].append("jump-table analysis")
        return t
    t["stage"] = "analyse"
    if not rec:
        if rec_run and rec_run.get("status") == "FAIL":
            t["failures"].append(("recompile", rec_run.get("reason") or "recompile run failed"))
        else:
            t["failures"].append(("recompile", "NOT TESTED: recompile pass not run"))
        return t
    x = rec.get("xenonrecomp", {})
    t["generator"] = {"exit_code": x.get("exit_code"), "functions": x.get("functions"),
                      "warnings": x.get("warnings") or {}, "unrecognized": x.get("unrecognized_instructions") or {},
                      "seconds": x.get("seconds")}
    sig = set()
    for k in t["generator"]["warnings"]:
        sig.add(SIGNATURES.get(k, "unclassified diagnostics"))
    for mnemonic in t["generator"]["unrecognized"]:
        sig.add("unimplemented instruction: " + mnemonic)
    if t["ambiguous_helpers"]:
        sig.add("register helpers")
    if x.get("exit_code") == 124:
        sig.add("generator timeout")
    elif x.get("exit_code"):
        sig.add(f"generator exit {x.get('exit_code')}")
    if not x.get("functions"):
        sig.add("no functions generated")
    t["signatures"] = sorted(sig)
    if sig:
        t["failures"].append(("recompile", "generator: " + ", ".join(sorted(sig))))
        return t
    t["stage"] = "recompile"
    return t


def blockers(t):
    return (len(t["missing_imports"]) + len(t["missing_variables"]) + len(t["unknown_ordinals"]) + len(t["signatures"])
            + len(t["boot_blockers"]))


def status_of(t):
    parked = sorted(tag for tag in t["tags"] if tag in PARKING_TAGS)
    if parked:
        return "BLOCKED", "; ".join(PARKING_TAGS[tag] for tag in parked)
    hard = [f for f in t["failures"] if not f[1].startswith("NOT TESTED")]
    if hard:
        stage, reason = hard[0]
        return "FAIL", f"{stage}: {reason}"
    if t["missing_imports"] or t["missing_variables"] or t["unknown_ordinals"]:
        parts = []
        if t["missing_imports"]:
            parts.append(f"{len(t['missing_imports'])} missing imports")
        if t["missing_variables"]:
            parts.append(f"{len(t['missing_variables'])} missing variables")
        if t["unknown_ordinals"]:
            parts.append(f"{len(t['unknown_ordinals'])} unknown ordinals")
        return "FAIL", "import gate: " + ", ".join(parts) + "".join("; " + b for b in t["boot_blockers"])
    if t["boot_blockers"]:
        return "FAIL", "; ".join(t["boot_blockers"])
    if t["failures"]:
        return "NOT TESTED", t["failures"][0][1].removeprefix("NOT TESTED: ")
    return "PASS", None


def rank_imports(titles):
    active = [t for t in titles if not (t["tags"] & PARKING_TAGS.keys()) and t["stage"]]
    blocks = collections.defaultdict(list)
    for t in active:
        for name in t["missing_imports"] + t["missing_variables"]:
            blocks[name].append(t)
    rows = []
    for name, ts in blocks.items():
        alone = [t for t in ts if len(t["missing_imports"]) + len(t["missing_variables"]) + len(t["unknown_ordinals"]) == 1]
        weight = sum(1.0 / (len(t["missing_imports"]) + len(t["missing_variables"])) for t in ts)
        rows.append({"import": name, "blocks": len(ts), "completes": len(alone), "weight": round(weight, 4),
                     "titles": sorted(t["key"] for t in ts),
                     "engines": sorted({t["engine"] for t in ts if t["engine"]})})
    rows.sort(key=lambda r: (-r["blocks"], -r["completes"], -r["weight"], -len(r["engines"]), r["import"]))
    return rows


def greedy_batches(titles, size=15):
    """Smallest import sets that complete whole titles, cheapest titles first."""
    active = [t for t in titles if not (t["tags"] & PARKING_TAGS.keys()) and t["stage"] and not t["unknown_ordinals"]]
    remaining = {t["key"]: set(t["missing_imports"] + t["missing_variables"]) for t in active}
    done, plan = set(), []
    while True:
        open_titles = {k: v - done for k, v in remaining.items() if v - done}
        if not open_titles:
            break
        k, need = min(open_titles.items(), key=lambda kv: (len(kv[1]), kv[0]))
        done |= need
        completed = sorted(key for key, v in remaining.items() if v and v <= done)
        plan.append({"adds": sorted(need), "cumulative_imports": len(done), "titles_import_complete": completed})
        if len(done) > size * 10:
            break
    return plan


def signature_groups(titles):
    groups = collections.defaultdict(list)
    for t in titles:
        for s in t["signatures"]:
            groups[s].append(t["key"])
    out = [{"signature": s, "titles": sorted(k)} for s, k in groups.items()]
    out.append({"signature": "setjmp/longjmp", "titles": [], "note": "NOT TESTED: no automatic detector; "
                "XenonRecomp needs setjmp/longjmp addresses supplied by hand when a title uses them"})
    return sorted(out, key=lambda g: (-len(g["titles"]), g["signature"]))


def stage_label(t):
    later = t.get("later") or {}
    reached = [s for s in LATER_STAGES if (later.get(s) or {}).get("status") == "PASS"]
    if reached:
        return reached[-1]
    if t["stage"] == "recompile":
        return "recompiled"
    if t["stage"] in ("decode", "analyse"):
        return "inventory"
    return t["stage"] or "none"


def aggregate(catalog, evidence):
    supported = supported_kernel_profiles()
    titles = [title_record(p.parent.name, p.parent, evidence, supported)
              for p in sorted(Path(catalog).glob("*/disc.json"))]
    for t in titles:
        t["status"], t["reason"] = status_of(t)
        t["blocker_count"] = blockers(t)
        t["label"] = stage_label(t)
    near = sorted((t for t in titles if t["status"] != "BLOCKED" and t["stage"]),
                  key=lambda t: (t["blocker_count"], t["key"]))
    result = {"tool": "tools/aggregate_inventories.py",
              "titles": [{**t, "tags": sorted(t["tags"])} for t in titles],
              "stage_counts": dict(collections.Counter(t["stage"] or "none" for t in titles)),
              "label_counts": dict(collections.Counter(t["label"] for t in titles)),
              "status_counts": dict(collections.Counter(t["status"] for t in titles)),
              "imports_ranked": rank_imports(titles), "greedy_plan": greedy_batches(titles),
              "near_ready": [{"key": t["key"], "blockers": t["blocker_count"], "missing_imports": len(t["missing_imports"]),
                              "missing_variables": len(t["missing_variables"]), "signatures": t["signatures"]} for t in near],
              "signatures": signature_groups(titles),
              "boot_blockers": sorted({b for t in titles for b in t["boot_blockers"]})}
    result["recompile_order"] = [t["id"] for t in near]
    return result


def compare(before, after):
    old = {t["key"]: t for t in (before or {}).get("titles", [])}
    rows = []
    for t in after["titles"]:
        o = old.get(t["key"])
        if o is None:
            rows.append((t["key"], None, t["label"], None, t["blocker_count"]))
        elif (o["label"], o["blocker_count"]) != (t["label"], t["blocker_count"]):
            rows.append((t["key"], o["label"], t["label"], o["blocker_count"], t["blocker_count"]))
    return rows


def sanitized_reason(reason):
    """Reasons in docs carry only stage names, import names, counts and diagnostics."""
    if reason is None:
        return "—"
    return re.sub(r"[A-Za-z]:[\\/][^\s,;|]*|/[^\s,;|]*/[^\s,;|]*", "<path>", reason)


def write_docs(result, path, date):
    L = ["# Xbox 360 title compatibility", "",
         f"Generated by `tools/aggregate_inventories.py` on {date} from the owner's local disc catalog "
         "(`tools/iso_extract.py`, `tools/catalog_inventory.py`). Metadata only: title and media IDs, disc numbers, "
         "stages, statuses, import names and counts. No game name, file or byte is recorded here.", "",
         "Stages: inventory → recompiled → host boot → PS5 boot → rendering → playable. The first two come from the "
         "inventories (`tools/m6_inventory.py`); the later ones only from recorded evidence "
         "(`docs/compatibility_evidence.json`, which names the proof). Host results are never PS5 evidence. "
         "A generator `unimplemented` warning is FAIL. Kinect, peripheral-only, multi-disc, install-disc and online-only "
         "titles are BLOCKED with that reason and left for later.", "",
         "## Titles", "",
         "| Title ID | Media ID | Disc | Engine | Stage reached | Status | Blocking reason | Date |",
         "| --- | --- | --- | --- | --- | --- | --- | --- |"]
    for t in sorted(result["titles"], key=lambda t: t["key"]):
        disc = f"{t['disc_number']}/{t['disc_count']}" if t["disc_number"] else "—"
        later = t.get("later") or {}
        notes = [f"{s}: {later[s]['status']}" for s in LATER_STAGES if s in later and later[s].get("status") != "PASS"]
        reason = sanitized_reason(t["reason"]) + ("; " + "; ".join(notes) if notes else "")
        L.append(f"| {t['title_id'] or '—'} | {t['media_id'] or '—'} | {disc} | {t['engine'] or '—'} | {t['label']} | "
                 f"{t['status']} | {reason} | {t['date'] or '—'} |")
    L += ["", "## Counts", "",
          "| Stage reached | Titles |", "| --- | --- |"]
    L += [f"| {k} | {v} |" for k, v in sorted(result["label_counts"].items())]
    L += ["", "| Status | Titles |", "| --- | --- |"]
    L += [f"| {k} | {result['status_counts'].get(k, 0)} |" for k in STATUSES]
    L += ["", "## Missing imports ranked by titles blocked", "",
          "`completes`: titles that become import-complete if this import alone is implemented.", "",
          "| Import | Titles blocked | Completes | Engines |", "| --- | --- | --- | --- |"]
    L += [f"| {r['import']} | {r['blocks']} | {r['completes']} | {', '.join(r['engines']) or '—'} |"
          for r in result["imports_ranked"]]
    boot = collections.Counter(b for t in result["titles"] for b in t["boot_blockers"])
    L += ["", "## Boot blockers outside the inventory", "",
          "Checks of the title metadata that `Title::Create` performs before any guest code runs.", "",
          "| Blocker | Titles |", "| --- | --- |"]
    L += [f"| {b} | {n} |" for b, n in sorted(boot.items())] or ["| none | 0 |"]
    L += ["", "## Generator failure signatures", "", "| Signature | Titles |", "| --- | --- |"]
    L += [f"| {g['signature']} | {len(g['titles'])}{' (' + g['note'] + ')' if g.get('note') else ''} |"
          for g in result["signatures"]]
    Path(path).write_text("\n".join(L) + "\n", encoding="utf-8")


def write_report(result, path, changes):
    L = ["# Local catalog report (do not share: names the disc images)", ""]
    L += ["| Catalog id | Image | Key | Engine | XDK | Stage | Label | Status | Blockers | Reason |",
          "| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |"]
    for t in result["titles"]:
        L.append(f"| {t['id']} | {t['image_name']} | {t['key']} | {t['engine'] or '—'} | {', '.join(t['xdk'])} | "
                 f"{t['stage']} | {t['label']} | {t['status']} | {t['blocker_count']} | {t['reason'] or '—'} |")
    L += ["", "## Near-ready (fewest blockers first)", ""]
    L += [f"- {n['key']}: {n['blockers']} blockers ({n['missing_imports']} imports, {n['missing_variables']} variables, "
          f"signatures: {', '.join(n['signatures']) or 'none'})" for n in result["near_ready"]]
    L += ["", "## Ranked missing imports", "", "| Import | Blocks | Completes | Weight |", "| --- | --- | --- | --- |"]
    L += [f"| {r['import']} | {r['blocks']} | {r['completes']} | {r['weight']} |" for r in result["imports_ranked"]]
    L += ["", "## Greedy plan (cheapest title first)", ""]
    for step in result["greedy_plan"]:
        L.append(f"- +{len(step['adds'])} imports (total {step['cumulative_imports']}): "
                 f"import-complete {', '.join(step['titles_import_complete'])}")
    L += ["", "## Per title", ""]
    for t in result["titles"]:
        L += [f"### {t['key']} ({t['image_name']})", "",
              f"- missing imports ({len(t['missing_imports'])}): {', '.join(t['missing_imports']) or 'none'}",
              f"- missing variables: {', '.join(t['missing_variables']) or 'none'}",
              f"- unknown ordinals: {', '.join(t['unknown_ordinals']) or 'none'}",
              f"- generator: {json.dumps(t['generator'], sort_keys=True) if t['generator'] else 'not run'}",
              f"- ambiguous register helpers: {', '.join(t['ambiguous_helpers']) or 'none'}",
              f"- tags: {', '.join(t['tags']) or 'none'}",
              f"- boot blockers: {'; '.join(t['boot_blockers']) or 'none'}", ""]
    if changes is not None:
        L += ["## Changes since the baseline", ""]
        L += [f"- {k}: {a} -> {b} stage, {c} -> {d} blockers" for k, a, b, c, d in changes] or ["- none"]
    Path(path).write_text("\n".join(L) + "\n", encoding="utf-8")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--catalog", default=str(CATALOG))
    ap.add_argument("--evidence", default=str(EVIDENCE))
    ap.add_argument("--baseline", help="a previous aggregate.json to compare with")
    ap.add_argument("--docs", default=str(DOCS))
    ap.add_argument("--date", default=datetime.date.today().isoformat())
    a = ap.parse_args()
    catalog = Path(a.catalog)
    evidence = load(a.evidence) or {}
    for key, ev in (evidence.get("titles") or {}).items():
        for stage, s in (ev.get("stages") or {}).items():
            if stage not in LATER_STAGES or s.get("status") not in STATUSES or not s.get("reference"):
                sys.exit(f"evidence {key}: stage {stage!r} needs a status in {STATUSES} and a reference")
    result = aggregate(catalog, evidence)
    baseline = load(a.baseline) if a.baseline else None
    changes = compare(baseline, result) if a.baseline else None
    result["changes"] = changes
    (catalog / "aggregate.json").write_text(json.dumps(result, indent=1) + "\n", encoding="utf-8")
    (catalog / "recompile_order.json").write_text(json.dumps({"order": result["recompile_order"]}, indent=1) + "\n",
                                                  encoding="utf-8")
    write_report(result, catalog / "REPORT.md", changes)
    if a.docs:
        write_docs(result, a.docs, a.date)
    print(json.dumps({"titles": len(result["titles"]), "labels": result["label_counts"],
                      "statuses": result["status_counts"], "changes": len(changes) if changes is not None else None}))


if __name__ == "__main__":
    main()
