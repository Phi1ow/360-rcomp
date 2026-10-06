"""Mode 3: Xbox 360 ISO (or extracted folder) -> real R-comp pipeline -> PS5 app folder.

The steps call R-comp's own tools; nothing of the recompiler is reimplemented here:
  1. extract-xiso (build/catalog-tools/extract-xiso)            ISO -> <work>/files
  2. tools/m6_inventory.py with XenonAnalyse/XenonRecomp           default.xex -> <work>/inventory (plain.xex, ppc/)
  3. tools/build_ps5_archives.sh                                  generated C++ + runtime + Xenos -> AOT archives
  4. gpu/vulkan/ps5/build-radv-probes.sh game                     archives + pinned RADV -> fake-signed eboot.bin
  5. package.build_app                                            -> <output>/<PPSA id>/
R-comp's scripts 3 and 4 only accept outputs inside R-comp/build/ (git-ignored); their intermediate output goes
to R-comp/build/rcomp-installer/<run>/ for that reason. Everything else stays in this tool's work/output folders.
"""
import json
import os
import pathlib
import re
import shutil
import time

from . import cygwin, package, profiles, xex
from .jobs import Blocked, Failed


def first_existing(*candidates):
    """The first candidate that exists; when none does, the last one (the layout a fresh checkout builds), which
    'Check toolchain' then reports as missing."""
    return next((c for c in candidates if c.exists()), candidates[-1])


def paths(settings):
    r = pathlib.Path(settings.get('rcomp_root'))
    # XenonRecomp v21 = v19 + patches 0020 (PPC_HOST_PTR) and 0021 (update forms, VMX, jump tables): with the physical window option
    # off it compiles to the same objects as v19 (proved on GTA IV, 222/222 objects); the generic profile turns it on.
    # v22 = v21 + patch 0022 (symbol_prefix, module_import_libraries for AOT modules); without those options its
    # output is byte-identical to v21 (proved on GTA IV, EFLC and Halo 3).
    # v23 = v22 + patch 0023 (integer divisions through the non-trapping PPC_DIV* helpers of R-comp's prelude: a
    # zero divisor raised SIGFPE on the PS5 in Halo 3); on Halo 3 + its 4 AOT modules only the 838 quotient lines differ.
    # Two layouts hold these tools: the developer's catalog build (build/catalog-tools, build/prime-xex-decode-v18) and what
    # a fresh checkout builds with tools/m6_setup.sh (build/cpu-xenonrecomp, build/cpu-xex-decode; every patch applied,
    # so v23 today). The first layout that exists wins.
    gen = r / 'build/catalog-tools/xenonrecomp-v23'
    m6 = r / 'build/cpu-xenonrecomp'
    return {
        'rcomp': r,
        'extract_xiso': r / 'build/catalog-tools/extract-xiso/extract-xiso.exe',
        'decode': first_existing(r / 'build/prime-xex-decode-v18/rcomp_xex_decode.exe', r / 'build/cpu-xex-decode.exe'),
        'analyse': first_existing(gen / 'XenonAnalyse/XenonAnalyse.exe', m6 / 'XenonAnalyse/XenonAnalyse.exe'),
        'recomp': first_existing(gen / 'XenonRecomp/XenonRecomp.exe', m6 / 'XenonRecomp/XenonRecomp.exe'),
        'xenon_src': first_existing(r / 'build/catalog-tools/xenonrecomp-v23-src', r / 'build/cpu-xenonrecomp-src'),
        'inventory_py': r / 'tools/m6_inventory.py',
        'archives_sh': r / 'tools/build_ps5_archives.sh',
        'link_sh': r / 'gpu/vulkan/ps5/build-radv-probes.sh',
        'xenos_src': r / 'build/prime-radv-resume-20260929/xenos-source-v3',
        'sdk': r / 'build/vulkan-gta-radv-20260928/ps5-sdk',
        'radv': r / 'build/vulkan-gta-radv-20260928/radv-ps5/src/amd/vulkan/libvulkan_radeon.a',
        'xma': r / 'build/prime-ffmpeg-xma-20260929/ps5/prefix',
        'native_tool': r / 'build/platform-ps5-tools/ps5-native-tool.exe',
        'libc': r / 'build/platform-ps5-tools/libc.prx',
    }


# What to run when 'Check toolchain' reports a piece as missing (shown in the page and in the job log).
_HOST_TOOLS = 'run python tools/setup_windows.py (or, in the R-comp Cygwin: bash tools/m6_setup.sh)'
_KIT = 'run python tools/install_kit.py (the prebuilt toolchain kit)'
HINTS = {
    'cygwin_bash': 'run python tools/bootstrap_cygwin.py, or set the path of a Cygwin bash.exe in Settings',
    'extract_xiso': 'run python tools/setup_windows.py (or, in the R-comp Cygwin: bash tools/build_extract_xiso.sh)',
    'xex_decode': _HOST_TOOLS,
    'xenon_analyse': _HOST_TOOLS,
    'xenon_recomp': _HOST_TOOLS,
    'xenon_source': _HOST_TOOLS,
    'm6_inventory': 'the "R-comp checkout" setting does not point at the repository root',
    'build_ps5_archives': 'the "R-comp checkout" setting does not point at the repository root',
    'radv_link': 'the "R-comp checkout" setting does not point at the repository root',
    'xenos_source': _KIT,
    'ps5_sdk': _KIT,
    'radv_driver': _KIT,
    'xma_codec': _KIT,
    'ps5_native_tool': _KIT,
    'libc_prx': _KIT,
}


def tool_report(settings):
    p = paths(settings)
    checks = {
        'cygwin_bash': cygwin.bash_path(settings),
        'extract_xiso': p['extract_xiso'],
        'xex_decode': p['decode'],
        'xenon_analyse': p['analyse'],
        'xenon_recomp': p['recomp'],
        'xenon_source': p['xenon_src'] / 'XenonUtils',
        'm6_inventory': p['inventory_py'],
        'build_ps5_archives': p['archives_sh'],
        'radv_link': p['link_sh'],
        'xenos_source': p['xenos_src'] / 'src/graphics/vulkan/command_processor.cpp',
        'ps5_sdk': p['sdk'] / 'bin/prospero-clang',
        'radv_driver': p['radv'],
        'xma_codec': p['xma'] / 'lib/libavcodec.a',
        'ps5_native_tool': p['native_tool'],
        'libc_prx': p['libc'],
    }
    tools = {k: {'path': str(v) if v else None, 'status': 'PASS' if v and pathlib.Path(v).exists() else 'BLOCKED'}
             for k, v in checks.items()}
    for k, v in tools.items():
        if v['status'] != 'PASS':
            v['hint'] = HINTS.get(k, '')
    return {'tools': tools}


DATA_DIR = pathlib.Path(__file__).resolve().parent.parent / 'profiles-data'


def link_order_file(profile):
    """The linker symbol-ordering file of a profile (profiles-data/<name>), or None."""
    name = profile.get('link_order')
    path = DATA_DIR / name if name else None
    return path if path is not None and path.is_file() else None


def recomp_option_args(options):
    """tools/m6_inventory.py arguments for the generator options of a build profile ({'cr_as_local': 'true', ...})."""
    args = []
    for k, v in sorted((options or {}).items()):
        args += ['--recomp-option', f'{k}={v}']
    return args


def choose_profile(a, info, files):
    """(key, reason, profile) of a title: the profile the request names, else the one detected from its XEX and disc."""
    key = (a.get('profile') or 'auto').strip()
    why = 'requested'
    if key == 'auto':
        key, why = profiles.detect(info['title_id'], info['media_id'], files)
    elif key not in profiles.PROFILES:
        raise Blocked(f'unknown build profile {key!r}')
    return key, why, profiles.PROFILES[key]


def guess_libc(settings):
    p = paths(settings)['libc']
    return str(p) if p.is_file() else ''


def _safe_name(text):
    return re.sub(r'[^A-Za-z0-9]+', '-', text).strip('-')[:40] or 'title'


def extract_disc(job, settings, src, work):
    """ISO -> <work>/files with R-comp's extract-xiso; an extracted folder is used in place."""
    log, p = job.log, paths(settings)
    src = pathlib.Path(src)
    if src.is_file():
        files = work / 'files'
        log('STEP', f'1/5 extracting the disc image {src.name} with extract-xiso')
        free = shutil.disk_usage(work).free
        if free < src.stat().st_size + (1 << 30):
            raise Blocked(f'not enough free space in {work} ({free / 2**30:.1f} GiB free)')
        code = cygwin.run(job, settings, [cygwin.to_cyg(p['extract_xiso']), '-x', '-d', cygwin.to_cyg(files),
                                          cygwin.to_cyg(src)], cwd=work, label='extract-xiso')
        if code != 0:
            raise Failed(f'extract-xiso failed (exit {code})')
    elif src.is_dir():
        files = src
        log('STEP', f'1/5 using the extracted disc folder {files}')
    else:
        raise Blocked(f'source not found: {src}')
    if not (files / 'default.xex').is_file():
        raise Blocked(f'no default.xex in {files}')
    count = sum(len(f) for _, _, f in os.walk(files))
    log('PASS', f'disc files ready: {files} ({count} files)')
    return files


def run(job, settings, a, forbidden_roots):
    log = job.log
    src = pathlib.Path(a.get('source') or '')
    title_id = (a.get('title_id') or '').strip().upper()
    if not package.TITLE_ID_RE.fullmatch(title_id):
        raise Blocked(f'PS5 title id must be PPSA + 5 digits (got {title_id!r})')
    exploratory = bool(a.get('exploratory'))
    scale = int(a.get('scale') or 3)
    if scale not in (1, 2, 3):
        raise Blocked('resolution scale must be 1, 2 or 3')
    p = paths(settings)
    log('STEP', 'checking the R-comp toolchain')
    missing = [k for k, v in tool_report(settings)['tools'].items() if v['status'] != 'PASS']
    if missing:
        for k in missing:
            tool = tool_report(settings)['tools'][k]
            log('BLOCKED', f'missing: {k} ({tool["path"]})' + (f': {tool["hint"]}' if tool.get('hint') else ''))
        raise Blocked('R-comp toolchain incomplete: ' + ', '.join(missing))
    log('PASS', 'all pipeline tools present')

    work_root = pathlib.Path(settings.get('work_dir')).resolve()
    resume = (a.get('resume') or '').strip()
    if resume:
        # Resume a previous run of this tool: reuse its disc files and inventory; the archive build is
        # incremental (ninja) and the link and packaging run again.
        if not re.fullmatch(r'PPSA\d{5}-\d{8}-\d{6}', resume) or not (work_root / resume).is_dir():
            raise Blocked(f'no previous run {resume!r} in {work_root}')
        if not resume.startswith(title_id + '-') and not a.get('reuse_other_title'):
            raise Blocked(f'run {resume} was made for another PS5 title id (set reuse_other_title to share its build folder)')
        run_name = resume
    else:
        run_name = f'{title_id}-{time.strftime("%Y%m%d-%H%M%S")}'
    work = work_root / run_name
    for root in forbidden_roots:
        root = pathlib.Path(root).resolve()
        if work == root or root in work.parents:
            raise Blocked(f'work folder must not be inside {root}; change it in Settings')
    rbuild = p['rcomp'] / 'build' / 'rcomp-installer' / run_name
    work.mkdir(parents=True, exist_ok=bool(resume))
    (work / 'run.json').write_text(json.dumps({k: str(a.get(k)) for k in (
        'source', 'title_id', 'name', 'scale', 'jobs', 'exploratory', 'profile')}, indent=2) + '\n', encoding='utf-8')
    log('INFO', f'work folder: {work}' + (' (resumed)' if resume else ''))
    log('INFO', f'R-comp build folder (required by its scripts): {rbuild}')
    job.result.update({'work': str(work), 'rcomp_build': str(rbuild), 'run': run_name})

    # 1. Disc
    if resume and (work / 'files' / 'default.xex').is_file():
        files = work / 'files'
        log('INFO', f'1/5 reusing the extracted disc {files}')
    else:
        files = extract_disc(job, settings, src, work)
    default_xex = files / 'default.xex'

    # 2. Inventory + recompilation (XenonAnalyse / XenonRecomp)
    inv = work / 'inventory'
    if resume and (inv / 'inventory.json').is_file() and (inv / 'ppc').is_dir():
        log('INFO', f'2/5 reusing the inventory and generated C++ in {inv}')
    else:
        if inv.exists():
            shutil.rmtree(inv)  # m6_inventory wants a fresh output; this run's own folder only
        def inventory(out, hints=None, label='m6_inventory', options=None):
            if out.exists():
                shutil.rmtree(out)  # m6_inventory wants a fresh output; this run's own folder only
            cmd = ['python3', 'tools/m6_inventory.py', cygwin.to_cyg(default_xex), '--out', cygwin.to_cyg(out),
                   '--require-supported', '--tool-timeout', str(int(a.get('tool_timeout') or 900)),
                   '--decode-tool', cygwin.to_cyg(p['decode'].with_suffix('')),
                   '--analyse-tool', cygwin.to_cyg(p['analyse'].with_suffix('')),
                   '--recomp-tool', cygwin.to_cyg(p['recomp'].with_suffix('')),
                   '--xenon-source', cygwin.to_cyg(p['xenon_src'])]
            if hints is not None:
                cmd += ['--function-hints', cygwin.to_cyg(hints)]
            cmd += recomp_option_args(options)
            rc = cygwin.run(job, settings, cmd, cwd=p['rcomp'], label=label)
            if not (out / 'inventory.json').is_file():
                raise Failed(f'{label} produced no inventory.json (exit {rc})')
            return rc

        def duplicate_functions(out):
            mapping = (out / 'ppc' / 'ppc_func_mapping.cpp').read_text(encoding='utf-8', errors='replace')
            counts = {}
            for m in re.finditer(r'\{ (0x[0-9A-Fa-f]+)', mapping):
                counts[m.group(1).upper()] = counts.get(m.group(1).upper(), 0) + 1
            return {k for k, v in counts.items() if v > 1}

        log('STEP', '2/5 decoding the XEX and recompiling it (m6_inventory.py: XenonAnalyse + XenonRecomp)')
        code = inventory(inv)
        # The first generation decoded the XEX: the title's profile is known, and with it the generator options its
        # measurements were made with (the tuned profiles); the first generation itself runs with the generator defaults.
        gen_options = dict(choose_profile(a, xex.read_xex(inv / 'plain.xex'), files)[2].get('generator') or {})
        # Functions reached only through code pointers in data (virtual method tables, callbacks) are missed
        # by the generator's discovery: GTA IV's free play called one (RCOMP-FATAL indirect_target, 4 October
        # 2026). Scan the first generation, regenerate with those functions as hints, and drop the hints the
        # generator discovers by itself (a function present twice makes the title refuse its function table).
        hints = work / 'function_hints.json'
        scan = cygwin.run(job, settings, ['python3', 'tools/m6_code_pointer_functions.py', cygwin.to_cyg(inv),
                                          '--out', cygwin.to_cyg(hints)], cwd=p['rcomp'], label='m6_code_pointer_functions')
        hint_list = json.loads(hints.read_text(encoding='utf-8')) if scan == 0 and hints.is_file() else []
        if hint_list or gen_options:
            parts = []
            if hint_list:
                parts.append(f'{len(hint_list)} functions reached only through code pointers')
            if gen_options:
                parts.append('the generator options of the profile (' + ' '.join(f'{k}={v}' for k, v in sorted(gen_options.items())) + ')')
            log('STEP', '2b/5 regenerating with ' + ' and '.join(parts))
            pass1 = work / 'inventory-hints-pass1'
            inventory(pass1, hints if hint_list else None, label='m6_inventory (pass 1)', options=gen_options)
            twice = duplicate_functions(pass1) if hint_list else set()
            kept = [h for h in hint_list if str(h['address']).upper() not in twice]
            if len(kept) != len(hint_list):
                log('INFO', f'{len(hint_list) - len(kept)} of them the generator discovers by itself: dropped, regenerating')
                hints.write_text(json.dumps(kept, indent=1) + '\n', encoding='utf-8')
                inventory(inv, hints if kept else None, label='m6_inventory (pass 2)', options=gen_options)
                shutil.rmtree(pass1, ignore_errors=True)
            else:
                shutil.rmtree(inv)
                pass1.rename(inv)
            if hint_list:
                left = duplicate_functions(inv)
                if left:
                    raise Failed(f'the function table still has {len(left)} duplicate entries after the hints: ' + ', '.join(sorted(left))[:300])
                log('PASS', f'{len(kept)} code-pointer functions added to the generation, no duplicate entry')
    report_path = inv / 'inventory.json'
    if not report_path.is_file():
        raise Failed(f'm6_inventory produced no inventory.json (exit {code})')
    report = json.loads(report_path.read_text(encoding='utf-8'))
    xr = report.get('xenonrecomp') or {}
    if not xr.get('ran') or xr.get('exit_code') != 0:
        raise Failed(f'XenonRecomp did not complete (ran={xr.get("ran")}, exit={xr.get("exit_code")})')
    if xr.get('warnings') or xr.get('unrecognized_instructions'):
        raise Failed('generator warnings / unrecognized instructions (a generator warning is FAIL): '
                     + json.dumps({'warnings': xr.get('warnings'),
                                   'unrecognized': xr.get('unrecognized_instructions')})[:600])
    log('PASS', f'XenonRecomp: {xr.get("functions")} functions, no generator warning')
    log('INFO', 'generator options of the inventory: ' + (' '.join(f'{k}={v}' for k, v in sorted((xr.get('options') or {}).items())) or 'none (generator defaults)'))
    # AOT modules: m6_inventory.py also recompiled every other XEX2 DLL of the disc (modules/<key>/); the
    # archives step compiles them into the title. A generator warning in any of them is FAIL too.
    aot = [m for m in ((report.get('modules') or {}).get('aot') or [])]
    for m in aot:
        mr = m.get('xenonrecomp') or {}
        if not mr.get('ran') or mr.get('exit_code') != 0 or not m.get('descriptor'):
            raise Failed(f'XenonRecomp did not complete for AOT module {m.get("disc_path")} '
                         f'(ran={mr.get("ran")}, exit={mr.get("exit_code")}): ' + '; '.join(m.get('blocking_reasons') or [])[:400])
        if mr.get('warnings') or mr.get('unrecognized_instructions'):
            raise Failed(f'generator warnings in AOT module {m["disc_path"]} (a generator warning is FAIL): '
                         + json.dumps({'warnings': mr.get('warnings'),
                                       'unrecognized': mr.get('unrecognized_instructions')})[:600])
    if aot:
        log('PASS', 'AOT modules: ' + ', '.join(f'{m["disc_path"]} ({m.get("functions")} functions)' for m in aot))
    for m in ((report.get('modules') or {}).get('other_xex') or []):
        log('INFO', f'other XEX on the disc, not compiled: {m.get("disc_path")} ({m.get("reason")})')
    info = xex.read_xex(inv / 'plain.xex')
    log('INFO', f'XEX {info["title_id"]} media {info["media_id"]}: "{info["name"]}"')
    key, why, profile = choose_profile(a, info, files)
    if (a.get('profile') or 'auto').strip() == 'auto':
        log('INFO', f'build profile: {key} ({why})')
    log('INFO', f'build profile {key}: {profile["label"]}; measured: {profile["measured"]}')
    want = dict(profile.get('generator') or {})
    have = {k: str(v).lower() for k, v in (xr.get('options') or {}).items()}
    if any(have.get(k) != v for k, v in want.items()):
        # A resumed run keeps the inventory of its earlier run: the profile's measured generator options are not in it.
        log('WARN', f'this inventory was generated without the generator options of profile {key} ({want}); '
                    f'delete {inv} (keep the extracted disc) and run again to regenerate it')
    name = (a.get('name') or profile['name'] or info['name'] or title_id).strip()
    job.result.update({'profile': key})
    reasons = report.get('blocking_reasons') or []
    if report.get('supported_for_link'):
        log('PASS', 'inventory: supported for link (production gate)')
    else:
        for r in reasons:
            log('WARN', 'blocker: ' + r[:400] + ('…' if len(r) > 400 else ''))
        if not exploratory and profile['exploratory']:
            exploratory = True
            log('WARN', f'profile {key} builds this title in exploratory mode (as it is played today)')
        if not exploratory:
            raise Blocked('the title has blockers R-comp does not support yet (see above). Tick "exploratory '
                          'build" to link anyway: listed missing imports stay fatal when the game calls them')
        log('WARN', 'exploratory build: only missing functions are admitted, each stays fatal on call')

    # 3. AOT archives
    archives = rbuild / 'archives'
    log('STEP', f'3/5 compiling the AOT archives (tools/build_ps5_archives.sh, resolution scale {scale}); '
                'this takes a long time')
    env = {'RCOMP_M6_DRAW_RESOLUTION_SCALE': scale, 'RCOMP_BUILD_JOBS': int(a.get('jobs') or 8),
           # The profile's optimization level of the generated code from the first compile on (otherwise the
           # profile step would recompile every generated file a second time).
           'RCOMP_GENERATED_OPT': profile['options'].get('RCOMP_M6_GENERATED_OPT', '-O2')}
    if exploratory:
        env['RCOMP_EXPLORATORY_INVENTORY'] = 1
    code = cygwin.run(job, settings, [
        'bash', 'tools/build_ps5_archives.sh', cygwin.to_cyg(inv), cygwin.to_cyg(p['xenos_src']),
        cygwin.to_cyg(p['sdk']), cygwin.to_cyg(p['xma']), cygwin.to_cyg(archives)],
        cwd=p['rcomp'], env=env, tails=[archives / 'build.log'], label='build_ps5_archives')
    if code != 0:
        raise Failed(f'AOT archive build failed (exit {code}); logs in {archives}')
    # build_ps5_archives.sh builds rcomp_m6_title and rcomp_m6_generated only. The runtime calls the platform
    # audio API (rcomp_audio_*), whose backend archive is a separate R-comp CMake target; the link script
    # refuses to link without it. Build that target of the same configured tree.
    code = cygwin.run(job, settings, ['cmake', '--build', cygwin.to_cyg(archives), '--target', 'rcomp_platform_audio'],
                      cwd=p['rcomp'], label='rcomp_platform_audio')
    if code != 0 or not (archives / 'platform' / 'librcomp_platform_audio.a').is_file():
        raise Failed(f'platform audio backend build failed (exit {code})')
    log('PASS', 'AOT archives compiled (with the platform audio backend)')
    # The profile's options are R-comp CMake cache options of the same tree (build_ps5_archives.sh does not
    # pass them, so they persist); the reconfigure only rebuilds what they affect.
    opts = [f'-D{k}={v}' for k, v in sorted(profile['options'].items())]
    log('STEP', f'3b/5 applying build profile {key}: ' + ' '.join(opts))
    code = cygwin.run(job, settings, ['cmake', '-S', 'app/m6', '-B', cygwin.to_cyg(archives), *opts],
                      cwd=p['rcomp'], tails=[], label='cmake (profile)')
    if code != 0:
        raise Failed(f'profile configure failed (exit {code})')
    code = cygwin.run(job, settings, ['cmake', '--build', cygwin.to_cyg(archives), '--target', 'rcomp_m6_title',
                                      'rcomp_m6_generated', 'rcomp_platform_audio', '-j', str(int(a.get('jobs') or 8))],
                      cwd=p['rcomp'], label='cmake --build (profile)')
    if code != 0:
        raise Failed(f'profile rebuild failed (exit {code})')
    cache = {}
    for line in (archives / 'CMakeCache.txt').read_text(encoding='utf-8', errors='replace').splitlines():
        entry, sep, value = line.partition('=')
        if sep and ':' in entry:
            cache[entry.split(':', 1)[0]] = value.strip()
    for k, v in profile['options'].items():
        if cache.get(k) != v:
            raise Failed(f'profile option {k}={v} is not in the build cache (found {cache.get(k)!r})')
    log('PASS', f'build profile {key} applied and verified in CMakeCache.txt')

    # 4. RADV link -> eboot.bin
    artifacts = rbuild / 'artifacts'
    link_name = re.sub(r'[^A-Za-z0-9 :._-]', '', name).strip() or title_id
    log('STEP', '4/5 linking with the pinned RADV driver and fake-signing (build-radv-probes.sh game)')
    link_env = {
        'RCOMP_RADV_PROBES_OUT': cygwin.to_cyg(artifacts), 'RCOMP_RADV_GAME_BUILD': cygwin.to_cyg(archives),
        'RCOMP_RADV_GAME_XEX': cygwin.to_cyg(inv / 'plain.xex'), 'RCOMP_TITLE_ID': title_id,
        'RCOMP_TITLE_NAME': link_name}
    order = link_order_file(profile)
    if order is not None:
        link_env['RCOMP_LINK_ORDER_FILE'] = cygwin.to_cyg(order)
        log('INFO', f'link order: {order.name} ({sum(1 for _ in order.open(encoding="utf-8"))} functions, hottest first)')
    elif profile.get('link_order'):
        log('WARN', f'link order file {profile["link_order"]} of profile {key} not found in {DATA_DIR}: linking in the default order')
    code = cygwin.run(job, settings, ['bash', 'gpu/vulkan/ps5/build-radv-probes.sh', 'game'], cwd=p['rcomp'], env=link_env,
                      label='build-radv-probes')
    eboot = artifacts / 'game' / 'dist' / title_id / 'eboot.bin'
    if code != 0 or not eboot.is_file():
        raise Failed(f'RADV link failed (exit {code}); see {artifacts / "game"}')
    log('PASS', f'eboot.bin linked: {eboot} ({eboot.stat().st_size / 2**20:.1f} MiB)')

    # 5. App folder
    log('STEP', '5/5 assembling the PS5 app folder')
    res = package.build_app(job, eboot, inv / 'plain.xex', files, p['libc'],
                            a.get('out') or settings.get('output_dir'), title_id, name, a.get('icon') or None,
                            bool(a.get('replace')), forbidden_roots,
                            modules=[(inv / m['dir'] / 'plain.xex', m['disc_path']) for m in aot])
    res.update({'work': str(work), 'rcomp_build': str(rbuild), 'exploratory': exploratory, 'profile': key,
                'production_gate': 'PASS' if report.get('supported_for_link') else 'BLOCKED'})
    gate = '' if report.get('supported_for_link') else ' (exploratory: production gate BLOCKED)'
    return 'PASS', f'recompiled and packaged {res["app"]}{gate}; PS5 boot NOT TESTED until installed and run', res
