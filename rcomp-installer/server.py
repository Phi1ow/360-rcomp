#!/usr/bin/env python3
"""R-comp Installer: local web app that packages recompiled Xbox 360 titles as PS5 apps and installs them.

  python server.py [--port 8360] [--no-browser]

Serves http://127.0.0.1:<port>/ (loopback only). Standard library only.
"""
import argparse
import base64
import json
import mimetypes
import os
import pathlib
import string
import sys
import threading
import time
import urllib.parse
import urllib.request
import webbrowser
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from rinstaller import console as consolemod
from rinstaller import compat, package, payloads, pipeline, profiles, xex
from rinstaller.jobs import Blocked, Failed, JobManager
from rinstaller.settings import TOOL_ROOT, Settings

STATIC = TOOL_ROOT / 'static'
SETTINGS = Settings()
JOBS = JobManager(TOOL_ROOT / 'build' / 'logs')
X360DB = 'https://raw.githubusercontent.com/xenia-manager/x360db/main/titles/{}/'
CHECKS = TOOL_ROOT / 'build' / 'checks'


def make_console():
    s = SETTINGS.all()
    if not str(s['ps5_host']).strip():
        raise Blocked('no PS5 address: set it in Settings')
    return consolemod.Console(s['ps5_host'], s['ftp_port'], s['loader_port'], s['ctl_port'], s['klog_port'])


def forbidden_roots():
    """The tool never writes its output inside the R-comp checkout or the reference dependencies."""
    return [p for p in (SETTINGS.get('rcomp_root'), SETTINGS.get('deps_root')) if p]


# ---- job bodies --------------------------------------------------------------------------------------
def job_install(job, app_path, overwrite, register):
    job.log('STEP', f'checking the app folder {app_path}')
    app = package.inspect_app(app_path, job.log, job.check_cancel)
    elf = payloads.chmod_elf(SETTINGS, job)
    return consolemod.install(job, make_console(), app, elf, overwrite=overwrite, register=register,
                              wait_seconds=SETTINGS.get('registration_wait'),
                              connections=SETTINGS.get('ftp_connections'))


def job_register(job, title_id):
    title_id = title_id.strip().upper()
    if not package.TITLE_ID_RE.fullmatch(title_id):
        raise Blocked(f'invalid title id {title_id!r}')
    c = make_console()
    name = next((t.get('name', '') for t in c.titles() if t['id'] == title_id), '')
    return consolemod.register_title(job, c, title_id, name, SETTINGS.get('registration_wait'))


def job_package(job, a):
    res = package.build_app(job, a.get('eboot'), a.get('xex'), a.get('game'), a.get('libc'),
                            a.get('out') or SETTINGS.get('output_dir'), a.get('title_id'), a.get('name'),
                            a.get('icon') or None, bool(a.get('replace')), forbidden_roots())
    return 'PASS', f'app folder ready: {res["app"]} (PS5 install NOT TESTED until you install it)', res


def job_recompile(job, a):
    return pipeline.run(job, SETTINGS, a, forbidden_roots())


def job_check_game(job, source):
    return compat.analyze(job, SETTINGS, source, CHECKS)


def load_check(check_id):
    path = CHECKS / check_id / 'report.json'
    if not compat.re.fullmatch(r'\d{8}-\d{6}', check_id or '') or not path.is_file():
        raise Blocked(f'no check {check_id!r}')
    return json.loads(path.read_text(encoding='utf-8'))


def installed_id(c, report, allocate):
    """The PS5 title id of a checked game: the one it is installed under, else (allocate) the next free one."""
    titles = c.titles()
    title_id = next((t['id'] for t in titles if t.get('folder') and t.get('name') == report['name']
                     and package.TITLE_ID_RE.fullmatch(t['id'])), None)
    if title_id or not allocate:
        return title_id
    used = {t['id'] for t in titles}
    # An app folder already built for this game keeps its id when that id is free on the console or only
    # holds an interrupted upload of it (a folder without param.json, never registered).
    unnamed = {t['id'] for t in titles if t.get('folder') and not t.get('name') and not t.get('registered')}
    for param in sorted(pathlib.Path(SETTINGS.get('output_dir')).glob('PPSA*/sce_sys/param.json')):
        try:
            name = package.title_name(json.loads(param.read_text(encoding='utf-8')))
        except ValueError:
            continue
        built_id = param.parent.parent.name
        if name == report['name'] and package.TITLE_ID_RE.fullmatch(built_id) and \
                (built_id not in used or built_id in unnamed):
            return built_id
    return next(f'PPSA{n:05d}' for n in range(88370, 100000) if f'PPSA{n:05d}' not in used)


def job_try_game(job, check_id, watch, reuse=True):
    """Build the checked game as it is played today (exploratory), install it, start it, watch what happens
    and record the outcome in the game's check and the backlog."""
    report = load_check(check_id)
    if report['verdict'] == 'BLOCKED':
        raise Blocked('this game is BLOCKED (see its report): building it cannot work yet')
    source = report['files_dir'] if pathlib.Path(report['files_dir'], 'default.xex').is_file() else report['source']
    if not pathlib.Path(source).exists():
        raise Blocked(f'the disc of this check is gone ({report["source"]}): put it back or check it again')
    c = make_console()
    title_id = installed_id(c, report, False)
    if title_id:
        job.log('INFO', f'"{report["name"]}" is already installed as {title_id}: it is updated in place')
    else:
        title_id = installed_id(c, report, True)
        job.log('INFO', f'new PS5 title id {title_id} for "{report["name"]}"')
    built = pathlib.Path(SETTINGS.get('output_dir')) / title_id
    if reuse and (built / 'manifest.sha256').is_file() and (built / 'eboot.bin').is_file() and \
            built.stat().st_mtime > time.mktime(time.strptime(check_id, '%Y%m%d-%H%M%S')):
        job.log('INFO', f'reusing the app folder built after this check: {built}')
        result = {'app': str(built)}
    else:
        status, summary, result = pipeline.run(job, SETTINGS, {
            'source': source, 'title_id': title_id, 'name': report['name'], 'scale': 3,
            'jobs': max(2, (os.cpu_count() or 4) - 2), 'exploratory': True, 'replace': True, 'profile': 'auto',
            'out': SETTINGS.get('output_dir')}, forbidden_roots())
        if status != 'PASS':
            return status, summary, result
    job.log('STEP', 'installing on the PS5')
    app = package.inspect_app(result['app'], job.log, job.check_cancel)
    status, summary, installed = consolemod.install(job, c, app, payloads.chmod_elf(SETTINGS, job), overwrite=False,
                                                    register=True, wait_seconds=SETTINGS.get('registration_wait'),
                                                    connections=SETTINGS.get('ftp_connections'))
    result.update(installed)
    if status != 'PASS':
        return status, summary, result
    if not watch:
        return 'PASS', (f'"{report["name"]}" is on your PS5 as {title_id}: start it from the home screen, then use '
                        '"Record the last run" so its result lands in the backlog'), result
    outcome = compat.observe_run(job, c, title_id, SETTINGS.get('watch_seconds'))
    compat.record_try(CHECKS, check_id, outcome)
    job.log('PASS' if outcome['result'] == 'running' else 'WARN', f'PS5: {outcome["result"]} - {outcome["reason"]}')
    result['outcome'] = outcome
    return ('PASS' if outcome['result'] == 'running' else 'FAIL',
            f'"{report["name"]}" ({title_id}) on the PS5: {outcome["result"]}, {outcome["reason"]} (recorded in the backlog)',
            result)


def job_record_run(job, check_id):
    """Record the latest run of a checked game that was played from the PS5 home screen."""
    report = load_check(check_id)
    c = make_console()
    title_id = installed_id(c, report, False)
    if not title_id:
        raise Blocked(f'"{report["name"]}" is not installed on the PS5: use "Try it on my PS5" first')
    outcome = compat.read_last_run(c, title_id)
    compat.record_try(CHECKS, check_id, outcome)
    job.log('PASS' if outcome['result'] == 'running' else 'WARN', f'PS5: {outcome["result"]} - {outcome["reason"]}')
    return 'PASS', f'"{report["name"]}": {outcome["result"]}, {outcome["reason"]} (recorded in the backlog)', \
        {'outcome': outcome}


DISC_SUFFIXES = ('.iso', '.xiso')


def job_check_all(job, folder, force):
    """Check every disc image of a folder in turn (already checked ones are skipped unless force)."""
    root = pathlib.Path(folder or SETTINGS.get('isos_dir'))
    if not root.is_dir():
        raise Blocked(f'not a folder: {root}')
    discs = sorted(p for p in root.iterdir() if p.is_file() and p.suffix.lower() in DISC_SUFFIXES)
    discs += sorted(p for p in root.iterdir() if p.is_dir() and (p / 'default.xex').is_file())
    if not discs:
        raise Blocked(f'no .iso file or extracted disc folder in {root}')
    done = compat.checked_sources(CHECKS)
    results, checked = [], 0
    for n, disc in enumerate(discs, 1):
        job.check_cancel()
        if not force and str(disc.resolve()).lower() in done:
            job.log('INFO', f'[{n}/{len(discs)}] {disc.name}: already checked ({done[str(disc.resolve()).lower()]})')
            continue
        job.log('STEP', f'[{n}/{len(discs)}] checking {disc.name}')
        try:
            status, summary, res = compat.analyze(job, SETTINGS, disc, CHECKS)
            results.append({'disc': disc.name, 'status': status, 'summary': summary})
            checked += 1
            time.sleep(1.1)  # check ids are per second
        except (Blocked, Failed) as e:
            job.log('WARN', f'{disc.name}: {e}')
            results.append({'disc': disc.name, 'status': 'BLOCKED', 'summary': str(e)})
    data = compat.write_backlog(CHECKS)
    return 'PASS', (f'{checked} disc(s) checked, {len(discs) - checked} skipped or blocked; backlog: '
                    f'{len(data["games"])} games, {len(data["functions"])} missing functions'), {'discs': results}


def job_launch(job, title_id):
    c = make_console()
    procs = c.ctl_or_none('procs')
    if procs is None:
        raise Blocked(f'ps5vkctl not reachable on {c.host}:{c.ctl_port}; load it first (Console tab)')
    job.log('OUT', 'procs: ' + procs)
    if 'count=0' not in procs:
        raise Blocked('an application is already running; close it first')
    reply = c.ctl('launch ' + title_id, 40)
    job.log('OUT', 'launch: ' + reply)
    time.sleep(3)
    after = c.ctl_or_none('procs') or ''
    job.log('OUT', 'procs: ' + after)
    if title_id in after:
        return 'PASS', f'{title_id} is running', {}
    return 'FAIL', f'{title_id} not seen in procs after launch', {}


def job_kill(job, title_id):
    c = make_console()
    reply = c.ctl_or_none('kill ' + title_id)
    if reply is None:
        raise Blocked('ps5vkctl not reachable')
    job.log('OUT', 'kill: ' + reply)
    after = c.ctl_or_none('procs') or ''
    job.log('OUT', 'procs: ' + after)
    return ('PASS', f'{title_id} closed', {}) if title_id not in after else ('FAIL', f'{title_id} still running', {})


def job_load_ctl(job):
    return payloads.load_ctl(job, SETTINGS, make_console())


def job_klog(job, seconds):
    c = make_console()
    job.log('STEP', f'kernel log {c.host}:{c.klog_port} for {seconds} s')
    try:
        c.klog(seconds, lambda line: job.log('OUT', line), job.check_cancel)
    except OSError as e:
        raise Blocked(f'kernel log port unreachable ({e})')
    return 'PASS', 'kernel log capture ended', {}


def job_build_payload(job):
    elf = payloads.chmod_elf(SETTINGS, job, force=True)
    return 'PASS', f'chmod payload built ({len(elf)} bytes); console execution NOT TESTED until an install', {}


# ---- HTTP --------------------------------------------------------------------------------------------
class Handler(BaseHTTPRequestHandler):
    server_version = 'RcompInstaller/1'

    def log_message(self, fmt, *args):
        if os.environ.get('RCOMP_INSTALLER_HTTP_LOG'):
            super().log_message(fmt, *args)

    # Loopback only, and only for pages served by this server (DNS-rebinding and cross-site POST guard).
    def _trusted(self):
        host = (self.headers.get('Host') or '').rsplit(':', 1)[0].strip('[]')
        if host not in ('127.0.0.1', 'localhost', '::1'):
            return False
        origin = self.headers.get('Origin')
        if origin:
            o = urllib.parse.urlparse(origin)
            if o.hostname not in ('127.0.0.1', 'localhost', '::1') or o.port != self.server.server_address[1]:
                return False
        return True

    def send_json(self, obj, code=200):
        body = json.dumps(obj, ensure_ascii=False).encode('utf-8')
        self.send_response(code)
        self.send_header('Content-Type', 'application/json; charset=utf-8')
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Cache-Control', 'no-store')
        self.end_headers()
        self.wfile.write(body)

    def send_bytes(self, data, ctype, code=200):
        self.send_response(code)
        self.send_header('Content-Type', ctype)
        self.send_header('Content-Length', str(len(data)))
        self.send_header('Cache-Control', 'no-store')
        self.end_headers()
        self.wfile.write(data)

    def body(self):
        if 'application/json' not in (self.headers.get('Content-Type') or ''):
            raise ValueError('JSON body required')
        n = int(self.headers.get('Content-Length') or 0)
        return json.loads(self.rfile.read(n) or b'{}')

    def do_GET(self):
        if not self._trusted():
            return self.send_json({'error': 'forbidden'}, 403)
        url = urllib.parse.urlparse(self.path)
        q = {k: v[0] for k, v in urllib.parse.parse_qs(url.query).items()}
        try:
            return self.route_get(url.path, q)
        except (BrokenPipeError, ConnectionResetError):
            pass
        except Exception as e:
            try:
                self.send_json({'error': f'{type(e).__name__}: {e}'}, 500)
            except OSError:
                pass

    def do_POST(self):
        if not self._trusted():
            return self.send_json({'error': 'forbidden'}, 403)
        try:
            data = self.body()
            return self.route_post(urllib.parse.urlparse(self.path).path, data)
        except Blocked as e:
            self.send_json({'error': str(e), 'status': 'BLOCKED'}, 400)
        except ValueError as e:
            self.send_json({'error': str(e)}, 400)
        except Exception as e:
            self.send_json({'error': f'{type(e).__name__}: {e}'}, 500)

    # GET routes
    def route_get(self, path, q):
        if path in ('/', '/index.html'):
            return self.static('index.html')
        if path.startswith('/static/'):
            return self.static(path[len('/static/'):])
        if path == '/api/settings':
            return self.send_json(SETTINGS.all())
        if path == '/api/fs':
            return self.send_json(list_dir(q.get('path', '')))
        if path == '/api/xex/icon':
            info = xex.read_xex(q['path'])
            if not info['icon_png']:
                return self.send_json({'error': 'no image in the XEX'}, 404)
            return self.send_bytes(info['icon_png'], 'image/png')
        if path == '/api/app/icon':
            p = pathlib.Path(q['path']) / 'sce_sys' / 'icon0.png'
            return self.send_bytes(p.read_bytes(), 'image/png')
        if path == '/api/jobs':
            return self.send_json(JOBS.list())
        if path.startswith('/api/jobs/'):
            parts = path.split('/')
            job = JOBS.get(int(parts[3]))
            if not job:
                return self.send_json({'error': 'no such job'}, 404)
            if len(parts) > 4 and parts[4] == 'events':
                return self.stream(job, int(q.get('since', 0)))
            return self.send_json(job.view(int(q.get('since', 0))))
        if path == '/api/console/titles':
            try:
                return self.send_json({'titles': make_console().titles()})
            except Blocked as e:
                return self.send_json({'error': str(e), 'status': 'BLOCKED'}, 503)
        if path == '/api/console/status':
            return self.send_json(console_status())
        if path == '/api/profiles':
            return self.send_json({k: {'label': v['label'], 'measured': v['measured'], 'options': v['options']}
                                   for k, v in profiles.PROFILES.items()})
        if path == '/api/checks':
            out = []
            for d in sorted(CHECKS.glob('*/report.json'), reverse=True):
                r = json.loads(d.read_text(encoding='utf-8'))
                item = {k: r.get(k) for k in ('check', 'name', 'title_id', 'verdict', 'checked', 'missing_functions')}
                tries = d.parent / 'tries.json'
                last = json.loads(tries.read_text(encoding='utf-8'))[-1] if tries.is_file() else None
                item['last_try'] = {k: last[k] for k in ('when', 'result', 'reason')} if last else None
                out.append(item)
            return self.send_json(out)
        if path == '/api/backlog':
            return self.send_json(compat.write_backlog(CHECKS))
        if path == '/api/backlog/export':
            compat.write_backlog(CHECKS)
            data = (CHECKS / 'BACKLOG.md').read_bytes()
            self.send_response(200)
            self.send_header('Content-Type', 'text/markdown; charset=utf-8')
            self.send_header('Content-Disposition', 'attachment; filename="RCOMP_BACKLOG.md"')
            self.send_header('Content-Length', str(len(data)))
            self.end_headers()
            self.wfile.write(data)
            return None
        if path.startswith('/api/checks/'):
            parts = path.split('/')
            report = load_check(parts[3])
            if len(parts) > 4 and parts[4] == 'work-order':
                text = (CHECKS / parts[3] / 'WORK_ORDER.md').read_bytes()
                return self.send_bytes(text, 'text/markdown; charset=utf-8')
            report['work_order_path'] = str(CHECKS / parts[3] / 'WORK_ORDER.md')
            return self.send_json(report)
        if path == '/api/tools':
            return self.send_json(pipeline.tool_report(SETTINGS) | {'payload': payloads.report(SETTINGS)})
        return self.send_json({'error': 'not found'}, 404)

    def route_post(self, path, d):
        if path == '/api/settings':
            return self.send_json(SETTINGS.update(d))
        if path == '/api/xex/info':
            info = xex.read_xex(d['path'])
            return self.send_json(xex.summary(info))
        if path == '/api/app/inspect':
            app = package.inspect_app(d['path'], lambda *a: None, lambda: None, verify_hashes=False)
            return self.send_json({'title_id': app['title_id'], 'name': app['name'], 'files': len(app['files']),
                                   'bytes': app['bytes'],
                                   'has_manifest': (pathlib.Path(d['path']) / 'manifest.sha256').is_file()})
        if path == '/api/x360db':
            return self.send_json(x360db(d['title_id']))
        if path == '/api/guess-libc':
            return self.send_json({'path': pipeline.guess_libc(SETTINGS)})
        if path.startswith('/api/jobs/') and path.endswith('/cancel'):
            job = JOBS.get(int(path.split('/')[3]))
            if job:
                job.cancel_event.set()
            return self.send_json({'ok': bool(job)})
        starters = {
            '/api/jobs/install': lambda: JOBS.start('install', f'Install {d["app"]}',
                                                    lambda j: job_install(j, d['app'], bool(d.get('overwrite')),
                                                                          d.get('register', True)), True),
            '/api/jobs/register': lambda: JOBS.start('register', f'Register {d["title_id"]}',
                                                     lambda j: job_register(j, d['title_id']), True),
            '/api/jobs/package': lambda: JOBS.start('package', f'Package {d.get("title_id", "")}',
                                                    lambda j: job_package(j, d)),
            '/api/jobs/check-game': lambda: JOBS.start('check-game', f'Check {pathlib.Path(d.get("source", "")).name}',
                                                       lambda j: job_check_game(j, d.get('source', ''))),
            '/api/jobs/try-game': lambda: JOBS.start('try-game', f'Try {d.get("check", "")} on the PS5',
                                                     lambda j: job_try_game(j, d.get('check', ''),
                                                                            d.get('watch', True)), True),
            '/api/jobs/record-run': lambda: JOBS.start('record-run', f'Record the last run of {d.get("check", "")}',
                                                       lambda j: job_record_run(j, d.get('check', '')), True),
            '/api/jobs/check-all': lambda: JOBS.start('check-all', 'Check all discs',
                                                      lambda j: job_check_all(j, d.get('folder', ''),
                                                                              bool(d.get('force')))),
            '/api/jobs/recompile': lambda: JOBS.start('recompile', f'Recompile {d.get("source", "")}',
                                                      lambda j: job_recompile(j, d)),
            '/api/jobs/launch': lambda: JOBS.start('launch', f'Launch {d["title_id"]}',
                                                   lambda j: job_launch(j, d['title_id']), True),
            '/api/jobs/kill': lambda: JOBS.start('kill', f'Close {d["title_id"]}',
                                                 lambda j: job_kill(j, d['title_id']), True),
            '/api/jobs/load-ctl': lambda: JOBS.start('load-ctl', 'Load ps5vkctl', job_load_ctl, True),
            '/api/jobs/klog': lambda: JOBS.start('klog', 'Kernel log',
                                                 lambda j: job_klog(j, min(int(d.get('seconds', 30)), 600))),
            '/api/jobs/build-payload': lambda: JOBS.start('payload', 'Build chmod payload', job_build_payload),
        }
        if path in starters:
            job = starters[path]()
            return self.send_json({'job': job.id})
        return self.send_json({'error': 'not found'}, 404)

    def static(self, rel):
        p = (STATIC / rel).resolve()
        if STATIC.resolve() not in p.parents or not p.is_file():
            return self.send_json({'error': 'not found'}, 404)
        ctype = mimetypes.guess_type(p.name)[0] or 'application/octet-stream'
        if ctype.startswith('text/') or ctype in ('application/javascript',):
            ctype += '; charset=utf-8'
        return self.send_bytes(p.read_bytes(), ctype)

    def stream(self, job, since):
        """Server-sent events: one 'line' event per log line, then one 'end' event."""
        self.send_response(200)
        self.send_header('Content-Type', 'text/event-stream; charset=utf-8')
        self.send_header('Cache-Control', 'no-store')
        self.end_headers()
        while True:
            job.wait(since, 15)
            view = job.view(since)
            for line in view['lines']:
                self.wfile.write(b'event: line\ndata: ' + json.dumps(line, ensure_ascii=False).encode() + b'\n\n')
            since += len(view['lines'])
            if view['status'] != 'RUNNING' and not view['lines']:
                view.pop('lines')
                self.wfile.write(b'event: end\ndata: ' + json.dumps(view, ensure_ascii=False).encode() + b'\n\n')
                self.wfile.flush()
                return
            if not view['lines']:
                self.wfile.write(b': keep-alive\n\n')
            self.wfile.flush()


def list_dir(path):
    if not path:
        if os.name == 'nt':
            drives = [f'{d}:\\' for d in string.ascii_uppercase if os.path.exists(f'{d}:\\')]
            return {'path': '', 'parent': None, 'entries': [{'name': d, 'dir': True} for d in drives],
                    'home': str(pathlib.Path.home())}
        path = '/'
    p = pathlib.Path(path).expanduser()
    if p.is_file():
        p = p.parent
    entries = []
    try:
        for e in os.scandir(p):
            try:
                is_dir = e.is_dir()
                entries.append({'name': e.name, 'dir': is_dir, 'size': None if is_dir else e.stat().st_size})
            except OSError:
                continue
    except OSError as e:
        return {'path': str(p), 'parent': str(p.parent) if p.parent != p else '', 'entries': [], 'error': str(e)}
    entries.sort(key=lambda e: (not e['dir'], e['name'].lower()))
    parent = str(p.parent) if p.parent != p else ''
    return {'path': str(p), 'parent': parent, 'entries': entries, 'home': str(pathlib.Path.home())}


def console_status():
    c = make_console()
    out = {'host': c.host}
    try:
        with c.ftp(5):
            out['ftp'] = 'PASS'
    except Blocked as e:
        out['ftp'] = 'BLOCKED'
        out['ftp_error'] = str(e)
    out['ctl'] = c.ctl_or_none('status', 5)
    out['procs'] = c.ctl_or_none('procs', 5)
    return out


def x360db(title_id):
    title_id = title_id.strip().upper()
    if not all(ch in string.hexdigits for ch in title_id) or len(title_id) != 8:
        raise ValueError('title id must be 8 hex digits')
    url = X360DB.format(title_id)
    out = {'title_id': title_id, 'source': url + 'info.json'}
    try:
        with urllib.request.urlopen(url + 'info.json', timeout=10) as r:
            info = json.loads(r.read().decode('utf-8'))
        out['info'] = info
        out['boxart'] = url + 'artwork/boxart.jpg'
    except Exception as e:
        out['error'] = f'x360db lookup failed: {e}'
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--port', type=int, default=8360)
    ap.add_argument('--no-browser', action='store_true')
    a = ap.parse_args()
    httpd = ThreadingHTTPServer(('127.0.0.1', a.port), Handler)
    httpd.daemon_threads = True
    url = f'http://127.0.0.1:{a.port}/'
    print(f'R-comp Installer on {url}  (Ctrl+C to stop)', flush=True)
    if not a.no_browser:
        threading.Timer(0.5, lambda: webbrowser.open(url)).start()
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == '__main__':
    sys.exit(main())
