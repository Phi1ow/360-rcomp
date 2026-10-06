"""'Check a game': what R-comp can and cannot do yet for one disc, and the work order to support it.

The check runs R-comp's own tools (extract-xiso, m6_inventory.py with XenonAnalyse/XenonRecomp) and reads
their inventory: nothing is estimated. It adds the facts the inventory does not judge: the kernel version
rule of the runtime (the title's own xboxkrnl.exe version, Xbox 360 kernel 2.0.1888 to 2.0.17559, never
below its declared minimum; owner decision of 3 Oct 2026), multi-disc and Kinect titles.

Verdicts:
  READY          every import and variable is implemented, the generator is clean: build and play.
  TRY IT         only console functions are missing; each stops the game with an explicit message if the
                 game ever calls it, so the game can be built and tried (the GTA IV / EFLC situation).
  BLOCKED        something stops the game before or at start: generator diagnostics, missing kernel
                 variables, unknown ordinals, an unsupported kernel version, Kinect, a second disc.
"""
import json
import pathlib
import re
import shutil
import struct
import time

from . import cygwin, pipeline, profiles, xex
from .jobs import Blocked, Failed

KERNEL_MIN_BUILD, KERNEL_MAX_BUILD = 1888, 17559

# Console service families, for the report and the work order (first matching rule wins).
CATEGORIES = [
    ('Kinect', re.compile(r'^(XamNui|Nui)')),
    ('Saves and storage', re.compile(r'^(XamContent|XamShowDeviceSelector|XamLoaderGetDvd|XamMu)')),
    ('Profile and sign-in', re.compile(r'^XamUser|^XamProfile|^XamShowSignin')),
    ('Xbox LIVE and network', re.compile(r'^(NetDll_|XNet|XamSession|XOnline|XamShowMarketplace|XamShowGamerCard|'
                                         r'XamShowFriends|XamShowPlayerReview|XamShowMessageCompose|XamParty|XamUserAreUsersFriends)')),
    ('System dialogs', re.compile(r'^XamShow')),
    ('Voice', re.compile(r'^XamVoice')),
    ('Achievements and stats', re.compile(r'^(XamUserCreateAchievement|XamUserWriteAchievement|XamUserCreateStats)')),
    ('Exceptions (SEH)', re.compile(r'^(RtlUnwind|__C_specific_handler|RtlCaptureContext|RtlRaiseException|_except|RtlDispatch)')),
    ('Files and devices', re.compile(r'^(Io|Ob|Stfs|NtDeviceIoControl|NtReadFileScatter|NtWriteFileGather|NtQueryDirectory|'
                                     r'NtCreateFile|NtOpenFile|NtSetInformationFile|NtQueryInformationFile|NtFsControl)')),
    ('Audio', re.compile(r'^(XAudio|XMA|XMP|XamMediaPlayer)')),
    ('Video', re.compile(r'^(Vd|VideoGetOutput)')),
    ('Security and crypto', re.compile(r'^(XeKeys|XeCrypt)')),
    ('Threads, sync and memory', re.compile(r'^(Ke|Ex|Nt|Mm|Interlocked|Kf|Hal|Rtl)')),
    ('Tasks and messages', re.compile(r'^(XamTask|XMsg|XNotify|XamNotify)')),
]


def category(name):
    for label, rule in CATEGORIES:
        if rule.search(name):
            return label
    return 'Other'


def _be32(b, o):
    return struct.unpack_from('>I', b, o)[0]


def kernel_check(plain_xex):
    """The runtime's kernel version rule on the decoded XEX's import libraries (kernel_variables.cpp)."""
    data = pathlib.Path(plain_xex).read_bytes()
    out = {'status': 'BLOCKED', 'reason': '', 'libraries': {}}
    if data[:4] != b'XEX2':
        out['reason'] = 'not a decoded XEX2 image'
        return out
    keys = {_be32(data, 24 + 8 * i): _be32(data, 28 + 8 * i) for i in range(_be32(data, 20))}
    off = keys.get(0x000103FF)
    if off is None:
        out['reason'] = 'no import library header'
        return out
    strtab_len, count = _be32(data, off + 4), _be32(data, off + 8)
    names, p = [], off + 12
    while len(names) < count and p < off + 12 + strtab_len:
        end = data.index(b'\0', p)
        names.append(data[p:end].decode('ascii', 'replace'))
        p += (end - p + 1 + 3) & ~3
    lib = off + 12 + strtab_len
    for _ in range(count):
        size, version, minimum = _be32(data, lib), _be32(data, lib + 0x1C), _be32(data, lib + 0x20)
        name_index = struct.unpack_from('>H', data, lib + 0x24)[0]
        name = names[name_index] if name_index < len(names) else f'#{name_index}'
        out['libraries'][name] = {'version': _fmt(version), 'minimum': _fmt(minimum), 'packed': version, 'min_packed': minimum}
        lib += size
    k = next((v for n, v in out['libraries'].items() if n.lower() == 'xboxkrnl.exe'), None)
    x = next((v for n, v in out['libraries'].items() if n.lower() == 'xam.xex'), None)
    if not k:
        out['reason'] = 'the title does not import xboxkrnl.exe'
        return out
    # Runtime rule (runtime/src/kernel_variables.cpp, presented_kernel_profile): a title runs when its declared
    # minimum is a retail kernel 2.0.1888..2.0.17559; it sees its own version, or the final retail kernel
    # 2.0.17559 when it was built against a later XDK (as on a console, which checks only the minimum).
    def retail(packed):
        return (packed >> 28) == 2 and ((packed >> 24) & 0xF) == 0 and \
            KERNEL_MIN_BUILD <= (packed >> 8) & 0xFFFF <= KERNEL_MAX_BUILD
    final = (2 << 28) | (KERNEL_MAX_BUILD << 8)
    presented = k['packed'] if retail(k['packed']) else final
    if k['packed'] < k['min_packed']:
        out['reason'] = f'xboxkrnl.exe version {k["version"]} is below its own minimum {k["minimum"]}'
    elif not retail(k['min_packed']):
        out['reason'] = (f'its minimum kernel {k["minimum"]} is not an Xbox 360 retail kernel '
                         f'2.0.{KERNEL_MIN_BUILD}-2.0.{KERNEL_MAX_BUILD}')
    elif x and x['packed'] > k['packed']:
        out['reason'] = f'xam.xex {x["version"]} is newer than xboxkrnl.exe {k["version"]}'
    else:
        out['status'] = 'PASS'
        out['reason'] = (f'the game will see kernel {_fmt(presented)} (built against {k["version"]}; minimum '
                         f'{k["minimum"]}' + (', capped at the final retail kernel)' if presented != k['packed'] else ')'))
    out['kernel'] = _fmt(presented)
    return out


def _fmt(packed):
    return f'{packed >> 28}.{(packed >> 24) & 0xF}.{(packed >> 8) & 0xFFFF}.{packed & 0xFF}'


def analyze(job, settings, source, checks_root):
    """Extract + inventory one disc; write report.json and WORK_ORDER.md under checks_root/<id>/."""
    log = job.log
    src = pathlib.Path(source)
    if not src.exists():
        raise Blocked(f'not found: {source}')
    missing_tools = [k for k, v in pipeline.tool_report(settings)['tools'].items()
                     if v['status'] != 'PASS' and k in ('cygwin_bash', 'extract_xiso', 'xex_decode', 'xenon_analyse',
                                                        'xenon_recomp', 'xenon_source', 'm6_inventory')]
    if missing_tools:
        report = pipeline.tool_report(settings)['tools']
        for k in missing_tools:
            job.log('BLOCKED', f'missing: {k} ({report[k]["path"]})' + (f': {report[k]["hint"]}' if report[k].get('hint') else ''))
        raise Blocked('R-comp toolchain incomplete for a check: ' + ', '.join(missing_tools))
    check_id = time.strftime('%Y%m%d-%H%M%S')
    work = pathlib.Path(checks_root) / check_id
    work.mkdir(parents=True)
    job.result.update({'check': check_id, 'dir': str(work)})
    log('STEP', 'reading the disc')
    try:
        return _analyze(job, settings, src, work, check_id, checks_root)
    finally:
        # The generated C++ and an extracted image are large and regenerable: "Try it on my PS5" extracts
        # again from the source. An extracted folder given as the source is the user's own: never touched.
        shutil.rmtree(work / 'inventory' / 'ppc', ignore_errors=True)
        if src.is_file():
            shutil.rmtree(work / 'files', ignore_errors=True)


def _analyze(job, settings, src, work, check_id, checks_root):
    log, source = job.log, src
    files = pipeline.extract_disc(job, settings, src, work)
    modules = sorted(p.relative_to(files).as_posix() for p in files.rglob('*')
                     if p.suffix.lower() in ('.xex', '.xexp') and p.is_file())
    p = pipeline.paths(settings)
    inv = work / 'inventory'
    log('STEP', 'recompiling the game code and listing what it needs (m6_inventory.py)')
    code = cygwin.run(job, settings, [
        'python3', 'tools/m6_inventory.py', cygwin.to_cyg(files / 'default.xex'), '--out', cygwin.to_cyg(inv),
        '--require-supported', '--tool-timeout', '900',
        '--decode-tool', cygwin.to_cyg(p['decode'].with_suffix('')),
        '--analyse-tool', cygwin.to_cyg(p['analyse'].with_suffix('')),
        '--recomp-tool', cygwin.to_cyg(p['recomp'].with_suffix('')),
        '--xenon-source', cygwin.to_cyg(p['xenon_src'])], cwd=p['rcomp'], label='m6_inventory')
    if not (inv / 'inventory.json').is_file():
        raise Failed(f'the inventory produced no report (exit {code}); see the log above')
    report = build_report(json.loads((inv / 'inventory.json').read_text(encoding='utf-8')), inv, files, modules,
                          source, check_id)
    (work / 'report.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    (work / 'WORK_ORDER.md').write_text(work_order(report), encoding='utf-8', newline='\n')
    write_backlog(checks_root)
    log('PASS' if report['verdict'] == 'READY' else 'WARN' if report['verdict'] == 'TRY IT' else 'BLOCKED',
        f'{report["name"]}: {report["verdict"]} - {report["verdict_text"]}')
    status = 'PASS' if report['verdict'] in ('READY', 'TRY IT') else 'BLOCKED'
    return status, f'{report["name"]}: {report["verdict"]}', {'check': check_id, 'report': report}


def build_report(inv, inv_dir, files, modules, source, check_id):
    plain = inv_dir / 'plain.xex'
    info = xex.read_xex(plain) if plain.is_file() else {'title_id': '', 'media_id': '', 'name': '', 'disc_count': 0,
                                                        'disc_number': 0}
    key, why = profiles.detect(info['title_id'], info['media_id'], files)
    profile = profiles.PROFILES[key]
    name = profile['name'] or info['name'] or pathlib.Path(source).stem
    xr = inv.get('xenonrecomp') or {}
    kernel = kernel_check(plain) if plain.is_file() else {'status': 'BLOCKED', 'reason': 'the XEX could not be decoded'}
    services, missing_functions, missing_variables, unknown = {}, [], [], []
    for module, v in (inv.get('imports') or {}).items():
        implemented = v.get('functions_implemented') or []
        missing = v.get('functions_missing') or []
        services[module] = {'implemented': len(implemented) if isinstance(implemented, list) else implemented,
                            'total': v.get('functions_total'), 'missing': missing}
        missing_functions += [(module, f) for f in missing]
        missing_variables += [(module, f) for f in (v.get('variables_missing') or [])]
        if v.get('unknown_ordinals'):
            unknown.append((module, v['unknown_ordinals']))
    kinect = any(category(f) == 'Kinect' for _, f in missing_functions) or any(
        category(f) == 'Kinect' for v in (inv.get('imports') or {}).values() for f in (v.get('functions_implemented') or []))
    multi_disc = int(info.get('disc_count') or 0) > 1
    blockers = []
    if not (inv.get('decode') or {}).get('supported', True):
        blockers.append('the executable could not be decoded')
    if not xr.get('ran') or xr.get('exit_code'):
        blockers.append('XenonRecomp did not complete')
    if xr.get('warnings') or xr.get('unrecognized_instructions'):
        blockers.append('XenonRecomp reports unsupported instructions or code patterns (generator work)')
    if missing_variables:
        blockers.append('kernel variables are missing: ' + ', '.join(f for _, f in missing_variables))
    if unknown:
        blockers.append('imports with unknown ordinals: ' + ', '.join(f'{m} {u}' for m, u in unknown))
    if kernel['status'] != 'PASS':
        blockers.append('kernel version: ' + kernel['reason'])
    if kinect:
        blockers.append('Kinect title: R-comp has no Kinect support')
    if multi_disc:
        blockers.append(f'disc {info.get("disc_number")} of {info.get("disc_count")}: multi-disc titles are not supported yet')
    if blockers:
        verdict, text = 'BLOCKED', 'something stops this game before or at start (see the blockers)'
    elif not missing_functions:
        verdict, text = 'READY', 'every console service it imports is implemented: build it and play'
    else:
        verdict, text = 'TRY IT', (f'{len(missing_functions)} console functions are missing; the game can be built '
                                   'and tried, and stops with an explicit message only if it calls one of them')
    groups = {}
    for module, f in missing_functions:
        groups.setdefault(category(f), []).append(f'{f} ({module})')
    return {
        'check': check_id, 'source': str(source), 'name': name, 'title_id': info['title_id'],
        'media_id': info['media_id'], 'disc': f'{info.get("disc_number")}/{info.get("disc_count")}',
        'modules': modules, 'profile': key, 'profile_label': profile['label'], 'profile_reason': why,
        'functions': xr.get('functions'), 'generator': {'warnings': xr.get('warnings') or {},
                                                        'unrecognized': xr.get('unrecognized_instructions') or {}},
        'kernel': kernel, 'services': services, 'missing_by_category': dict(sorted(groups.items())),
        'missing_functions': len(missing_functions), 'missing_variables': [f for _, f in missing_variables],
        'blockers': blockers, 'verdict': verdict, 'verdict_text': text,
        'files_dir': str(files), 'checked': time.strftime('%Y-%m-%d %H:%M'),
    }


def work_order(r):
    """The work order an R-comp developer (or agent) needs to add support for this game."""
    lines = [
        f'# R-comp support work order: {r["name"]}',
        '',
        f'Checked {r["checked"]} by R-comp Installer from `{pathlib.Path(r["source"]).name}`. Title ID '
        f'`{r["title_id"]}`, media ID `{r["media_id"]}`, disc {r["disc"]}, {r["functions"]} recompiled functions, '
        f'build profile `{r["profile"]}`.',
        '',
        f'**Verdict: {r["verdict"]}.** {r["verdict_text"]}.',
        '',
        'Rules: read `AGENTS.md` first. Implement real behaviour only (no stub returning success), keep test',
        'doubles under `runtime/tests` with the `TESTDOUBLE_` prefix, never edit generated C++, add a host test',
        'per service, and validate on the PS5. Statuses: PASS / FAIL / BLOCKED / NOT TESTED.',
        '',
    ]
    if r['blockers']:
        lines += ['## 1. Blockers (fix these first)', '']
        lines += [f'- {b}' for b in r['blockers']]
        if r['generator']['warnings'] or r['generator']['unrecognized']:
            lines += ['', 'Generator diagnostics (`cpu/patches/xenonrecomp`, see patch 0019 for the pattern):', '',
                      '```', json.dumps(r['generator'], indent=2)[:4000], '```']
        lines.append('')
    lines += [f'## {2 if r["blockers"] else 1}. Missing console functions ({r["missing_functions"]})', '']
    if not r['missing_by_category']:
        lines.append('None: every imported function is implemented.')
    for cat, names in r['missing_by_category'].items():
        lines += [f'### {cat} ({len(names)})', ''] + [f'- `{n}`' for n in sorted(names)] + ['']
    lines += ['## Implemented today', '']
    for module, s in r['services'].items():
        lines.append(f'- `{module}`: {s["implemented"]} of {s["total"]} functions')
    lines += ['', f'Kernel version: {r["kernel"].get("reason", "")} ({r["kernel"]["status"]}).', '',
              '## Reproduce', '', '```sh',
              f'python3 tools/m6_inventory.py <extracted disc>/default.xex --out build/<work>/inventory --require-supported \\',
              '  --decode-tool build/prime-xex-decode-v18/rcomp_xex_decode \\',
              '  --analyse-tool build/catalog-tools/xenonrecomp-v23/XenonAnalyse/XenonAnalyse \\',
              '  --recomp-tool build/catalog-tools/xenonrecomp-v23/XenonRecomp/XenonRecomp \\',
              '  --xenon-source build/catalog-tools/xenonrecomp-v23-src',
              '```', '',
              'Then build and install with R-comp Installer (mode 3, exploratory) and test on the PS5.', '']
    tries = r.get('tries') or []
    if tries:
        lines += ['## PS5 tries', '']
        for t in tries:
            lines.append(f'- {t["when"]} ({t["seconds"]} s): **{t["result"]}**: {t["reason"]}')
            lines += [f'  - `{line[:220]}`' for line in t.get('evidence') or []]
        lines.append('')
    return '\n'.join(lines)


# ---- what really happens on the PS5 -------------------------------------------------------------------
# The title writes its stdout to /app0/rcomp_title.log (appended, one "RCOMP-TITLE begin" per run) and its
# stderr to /app0/rcomp_title.err (truncated at every start): gpu/vulkan/ps5/radv_title_main.c.

FATAL_RE = re.compile(r'RCOMP-FATAL kind=(\S+) ?(.*)')
MISSING_RE = re.compile(r'module=(\S+) ordinal=(0x[0-9A-Fa-f]+) name=(\S+)')
END_RE = re.compile(r'RCOMP-TITLE end status=(-?\d+)')
FPS_RE = re.compile(r'RCOMP-FPS fps=([\d.]+)')


def last_run_lines(log_text, err_text, log_start=0):
    """The lines of the latest run: the .log from its last 'RCOMP-TITLE begin' (searched after log_start), the
    whole .err. None when no run began after log_start."""
    tail = log_text[log_start:]
    at = tail.rfind('RCOMP-TITLE begin')
    if at < 0:
        return None
    return tail[at:].splitlines() + err_text.splitlines()


def judge(lines, running, seconds):
    """The outcome of one run from its lines: running / stopped (RCOMP-FATAL) / crashed / exited."""
    fatal = [l for l in lines if l.startswith('RCOMP-FATAL')]
    crash = [l for l in lines if l.startswith('RCOMP-CRASH')]
    fps = [float(m.group(1)) for m in map(FPS_RE.match, lines) if m]
    end = next((m for m in map(END_RE.search, reversed(lines)) if m), None)
    out = {'when': time.strftime('%Y-%m-%d %H:%M'), 'seconds': int(seconds), 'missing_import': None,
           'fps_last': fps[-1] if fps else None, 'fps_frames_seen': len(fps),
           'evidence': (fatal[:3] + crash[:6])}
    if fatal:
        out['result'] = 'stopped'
        m = FATAL_RE.search(fatal[0])
        out['reason'] = f'{m.group(1)}: {m.group(2)[:200]}' if m else fatal[0][:200]
        mi = MISSING_RE.search(fatal[0])
        if m and m.group(1) == 'missing_import' and mi:
            out['missing_import'] = {'module': mi.group(1), 'ordinal': mi.group(2), 'name': mi.group(3)}
    elif crash:
        out['result'] = 'crashed'
        out['reason'] = crash[0][:200]
        addr = re.search(r'addr=(?:0x)?([0-9a-fA-F]+)', crash[0])
        gbase = next((m for m in map(re.compile(r'RCOMP-CRASH guest_base=(?:0x)?([0-9a-fA-F]+)').match, crash) if m), None)
        lr = next((m for m in map(re.compile(r'RCOMP-CRASH lr=([0-9A-Fa-f]+)').match, crash) if m), None)
        if addr and gbase:
            offset = int(addr.group(1), 16) - int(gbase.group(1), 16)
            if 0 <= offset < 1 << 32:
                kind = 'NULL pointer' if offset < 0x10000 else 'guest memory'
                out['reason'] = (f'guest code touched {kind} at guest address 0x{offset:08X}'
                                 + (f' (last call returns to 0x{lr.group(1)})' if lr else '') + f'; {crash[0][:120]}')
    elif end:
        out['result'] = 'exited'
        out['reason'] = f'the program returned {end.group(1)} without an R-comp fatal line'
        out['evidence'] = [l for l in lines if l.strip()][-6:]
    elif running:
        out['result'] = 'running'
        out['reason'] = (f'still running after {int(seconds)} s' +
                         (f', {fps[-1]:.1f} fps on screen' if fps else ', no frame counted yet'))
    else:
        out['result'] = 'vanished'
        out['reason'] = 'the process ended without any R-comp line (killed by the system?)'
        out['evidence'] = [l for l in lines if l.strip()][-6:]
    return out


def _read_logs(console, title_id):
    root = f'/data/homebrew/{title_id}'
    with console.ftp() as f:
        log = (console.try_read(f, f'{root}/rcomp_title.log') or b'').decode('utf-8', 'replace')
        err = (console.try_read(f, f'{root}/rcomp_title.err') or b'').decode('utf-8', 'replace')
    return log, err


def _running(console, title_id):
    return title_id in (console.ctl_or_none('procs') or '')


def observe_run(job, console, title_id, watch_seconds):
    """Start the installed title, watch it for up to watch_seconds (stops early on a fatal line or exit),
    close it, and return the judged outcome."""
    log = job.log
    procs = console.ctl_or_none('procs')
    if procs is None:
        raise Blocked('ps5vkctl is not reachable: load it from the Console tab, then try again')
    if 'count=0' not in procs:
        raise Blocked('another application is running on the PS5: close it first')
    log_text, _ = _read_logs(console, title_id)
    start_size = len(log_text)
    log('STEP', f'starting {title_id} and watching it for up to {watch_seconds} s')
    log('OUT', 'launch: ' + console.ctl('launch ' + title_id, 40))
    start, lines, running = time.time(), None, True
    while time.time() - start < watch_seconds:
        job.check_cancel()
        time.sleep(10)
        running = _running(console, title_id)
        log_text, err_text = _read_logs(console, title_id)
        lines = last_run_lines(log_text, err_text, start_size)
        if lines is None:
            if time.time() - start > 60:
                break  # nothing written in a minute: it never started
            continue
        fps = [l for l in lines if l.startswith('RCOMP-FPS fps=')]
        log('INFO', f'  +{int(time.time() - start)} s: {"running" if running else "not running"}'
                    + (f', {fps[-1][:40]}' if fps else ''))
        if not running or any(l.startswith(('RCOMP-FATAL', 'RCOMP-CRASH')) or END_RE.search(l) for l in lines):
            break
    seconds = time.time() - start
    if _running(console, title_id):
        log('OUT', 'kill: ' + (console.ctl_or_none('kill ' + title_id) or ''))
    if lines is None:
        out = {'when': time.strftime('%Y-%m-%d %H:%M'), 'seconds': int(seconds), 'result': 'did not start',
               'reason': 'the title wrote no log after the launch request', 'missing_import': None,
               'fps_last': None, 'fps_frames_seen': 0, 'evidence': []}
    else:
        out = judge(lines, running, seconds)
    out['title_id'] = title_id
    out['how'] = f'launched by the tool, watched {int(seconds)} s'
    return out


def read_last_run(console, title_id):
    """The outcome of the latest run of an installed title (after playing it yourself)."""
    log_text, err_text = _read_logs(console, title_id)
    lines = last_run_lines(log_text, err_text)
    if lines is None:
        raise Blocked(f'{title_id} has no run in its log yet: start it from the PS5 home screen first')
    running = _running(console, title_id)
    out = judge(lines, running, 0)
    out['seconds'] = len([l for l in lines if l.startswith('RCOMP-FPS fps=')])  # one line per second
    out['title_id'] = title_id
    out['how'] = 'recorded from the log after a manual run' + (' (still running)' if running else '')
    return out


def tries_path(checks_root, check_id):
    return pathlib.Path(checks_root) / check_id / 'tries.json'


def record_try(checks_root, check_id, outcome):
    path = tries_path(checks_root, check_id)
    tries = json.loads(path.read_text(encoding='utf-8')) if path.is_file() else []
    tries.append(outcome)
    path.write_text(json.dumps(tries, indent=2) + '\n', encoding='utf-8')
    report = load_report(checks_root, check_id)
    (path.parent / 'WORK_ORDER.md').write_text(work_order(report), encoding='utf-8', newline='\n')
    write_backlog(checks_root)


def load_report(checks_root, check_id):
    d = pathlib.Path(checks_root) / check_id
    r = json.loads((d / 'report.json').read_text(encoding='utf-8'))
    t = d / 'tries.json'
    r['tries'] = json.loads(t.read_text(encoding='utf-8')) if t.is_file() else []
    return r


# ---- one backlog for every checked game ----------------------------------------------------------------

def load_all(checks_root):
    """The latest check of each game (same title id and media id), with its PS5 tries."""
    latest = {}
    for report in sorted(pathlib.Path(checks_root).glob('*/report.json')):
        r = load_report(checks_root, report.parent.name)
        latest[(r.get('title_id'), r.get('media_id'), r['name'])] = r
    return sorted(latest.values(), key=lambda r: r['name'].lower())


def checked_sources(checks_root):
    """{source path: check id} of the latest check of each source file."""
    out = {}
    for report in sorted(pathlib.Path(checks_root).glob('*/report.json')):
        r = json.loads(report.read_text(encoding='utf-8'))
        out[str(pathlib.Path(r['source']).resolve()).lower()] = r['check']
    return out


def backlog(checks_root):
    """Every game and every missing function, ranked: first what a game really called on the PS5, then what
    the most games import."""
    games = load_all(checks_root)
    need = {}
    for g in games:
        for cat, names in g.get('missing_by_category', {}).items():
            for n in names:
                need.setdefault(n, {'category': cat, 'games': set(), 'hit_on_ps5': set()})['games'].add(g['name'])
        for t in g['tries']:
            mi = t.get('missing_import')
            if mi:
                key = f'{mi["name"]} ({mi["module"]})'
                e = need.setdefault(key, {'category': category(mi['name']), 'games': set(), 'hit_on_ps5': set()})
                e['games'].add(g['name'])
                e['hit_on_ps5'].add(g['name'])
    ranked = sorted(need.items(), key=lambda kv: (-len(kv[1]['hit_on_ps5']), -len(kv[1]['games']), kv[0].lower()))
    rows = []
    for g in games:
        last = g['tries'][-1] if g['tries'] else None
        rows.append({'check': g['check'], 'name': g['name'], 'title_id': g['title_id'], 'media_id': g['media_id'],
                     'source': pathlib.Path(g['source']).name, 'verdict': g['verdict'],
                     'missing_functions': g['missing_functions'], 'blockers': g['blockers'],
                     'last_try': last, 'tries': len(g['tries'])})
    return {'generated': time.strftime('%Y-%m-%d %H:%M'), 'games': rows,
            'functions': [{'name': n, 'category': e['category'], 'games': sorted(e['games']),
                           'hit_on_ps5': sorted(e['hit_on_ps5'])} for n, e in ranked]}, games


def write_backlog(checks_root):
    data, games = backlog(checks_root)
    root = pathlib.Path(checks_root)
    root.mkdir(parents=True, exist_ok=True)
    (root / 'backlog.json').write_text(json.dumps(data, indent=2) + '\n', encoding='utf-8')
    (root / 'BACKLOG.md').write_text(backlog_markdown(data, games), encoding='utf-8', newline='\n')
    return data


def _cell(text):
    return str(text).replace('|', '/').replace('\n', ' ')


def backlog_markdown(data, games):
    """One self-contained file to hand to the developer or AI agent who adds support for all these games."""
    L = ['# R-comp backlog: every game checked and tried', '',
         f'Generated {data["generated"]} by R-comp Installer from {len(games)} checked game(s).', '',
         'This file is self-contained: it is the whole work list. Rules: read `AGENTS.md` of R-comp first.',
         'Implement real behaviour only (no stub returning success; test doubles under `runtime/tests` with the',
         '`TESTDOUBLE_` prefix), never edit generated C++, add a host test per service, validate on the PS5.',
         'Statuses: PASS / FAIL / BLOCKED / NOT TESTED. Work in the priority order below: a function a game',
         'really called on the PS5 first, then blockers, then the functions the most games import.', '',
         '## Games', '', '| Game | Title / media | Check | Missing | Last PS5 try |', '| --- | --- | --- | --- | --- |']
    for g in data['games']:
        t = g['last_try']
        tried = f'{t["result"]}: {t["reason"]}' if t else 'not tried'
        L.append(f'| {_cell(g["name"])} | {g["title_id"]} / {g["media_id"]} | {g["verdict"]} | '
                 f'{g["missing_functions"]} | {_cell(tried)[:180]} |')
    hit = [f for f in data['functions'] if f['hit_on_ps5']]
    L += ['', '## Priority 1: functions a game really called on the PS5', '']
    L += [f'- `{f["name"]}` ({f["category"]}): stopped {", ".join(f["hit_on_ps5"])}' for f in hit] or [
        'None recorded yet (try the games on the PS5 to fill this).']
    L += ['', '## Priority 2: blockers (the game cannot start until they are fixed)', '']
    L += [f'- {g["name"]}: {b}' for g in data['games'] for b in g['blockers'] or []] or ['None.']
    rest = [f for f in data['functions'] if not f['hit_on_ps5']]
    L += ['', f'## Priority 3: missing functions by the number of games that import them ({len(rest)})', '',
          '| Function | Family | Games |', '| --- | --- | --- |']
    L += [f'| `{f["name"]}` | {f["category"]} | {len(f["games"])}: {_cell(", ".join(f["games"]))} |' for f in rest]
    L += ['', '## Per game', '']
    for g in games:
        L += [f'### {g["name"]}', '',
              f'Check `{g["check"]}` of `{pathlib.Path(g["source"]).name}`: title `{g["title_id"]}`, media '
              f'`{g["media_id"]}`, {g["functions"]} recompiled functions, profile `{g["profile"]}`, kernel '
              f'{g["kernel"].get("reason", "")} ({g["kernel"]["status"]}).', '',
              f'**{g["verdict"]}**: {g["verdict_text"]}.', '']
        if g['blockers']:
            L += ['Blockers:', ''] + [f'- {b}' for b in g['blockers']] + ['']
        if g['generator']['warnings'] or g['generator']['unrecognized']:
            L += ['Generator diagnostics:', '', '```', json.dumps(g['generator'], indent=2)[:3000], '```', '']
        for cat, names in g['missing_by_category'].items():
            L.append(f'- {cat}: ' + ', '.join(f'`{n}`' for n in sorted(names)))
        if g['missing_by_category']:
            L.append('')
        for t in g['tries']:
            L.append(f'- PS5 {t["when"]} ({t.get("how", "")}): **{t["result"]}**: {t["reason"]}')
            L += [f'  - `{line[:220]}`' for line in t.get('evidence') or []]
        if g['tries']:
            L.append('')
    L += ['## Reproduce a check', '', 'From the R-comp checkout, on the extracted disc of a game:', '', '```sh',
          'python3 tools/m6_inventory.py <extracted disc>/default.xex --out build/<work>/inventory --require-supported \\',
          '  --decode-tool build/prime-xex-decode-v18/rcomp_xex_decode \\',
          '  --analyse-tool build/catalog-tools/xenonrecomp-v23/XenonAnalyse/XenonAnalyse \\',
          '  --recomp-tool build/catalog-tools/xenonrecomp-v23/XenonRecomp/XenonRecomp \\',
          '  --xenon-source build/catalog-tools/xenonrecomp-v23-src', '```', '',
          'After a fix: rebuild R-comp, then in R-comp Installer "Check all" (force) and "Try it on my PS5" for the',
          'games concerned; their new results land in this file.', '']
    return '\n'.join(L)
