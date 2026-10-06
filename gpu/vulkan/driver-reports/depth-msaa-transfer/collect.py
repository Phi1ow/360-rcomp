#!/usr/bin/env python3
"""Collect original probe evidence after PRIME's serial console run. No launch."""
import argparse, ftplib, hashlib, json, os, pathlib, re, sys
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('logdir',type=pathlib.Path)
p.add_argument('--fetch',action='store_true',help='FTP readback only; requires authorized console environment')
p.add_argument('--build-out',type=pathlib.Path,required=True,help='expected isolated build directory; provenance is mandatory')
a=p.parse_args()
a.logdir.mkdir(parents=True,exist_ok=True)
build_file=a.build_out/'draw/BUILD_ID.txt'
expected_build=build_file.read_text().strip() if build_file.exists() else ''
if expected_build and not re.fullmatch('[0-9a-f]{16}',expected_build):expected_build=''
def read_stdout():
    file=a.logdir/'rcomp_title.log'
    return file.read_text(errors='replace') if file.exists() else ''
def current_begin(text):
    begins=list(re.finditer(r'^RCOMP-TITLE begin [^\n]*\bbuild=([0-9a-f]+)\b[^\n]*',text,re.M))
    if not begins:return None
    return begins[-1]
def provenance(text):
    begin=current_begin(text)
    return bool(expected_build and begin and begin[1]==expected_build)
fetch_errors=[]
fetch_stdout_failed=False
if a.fetch:
    if os.environ.get('RCOMP_CONSOLE_RUN_AUTHORIZED')!='1':p.error('--fetch requires RCOMP_CONSOLE_RUN_AUTHORIZED=1')
    host=os.environ.get('RCOMP_PS5_HOST');port=os.environ.get('RCOMP_PS5_FTP_PORT')
    if not host or not port:p.error('--fetch requires RCOMP_PS5_HOST and RCOMP_PS5_FTP_PORT')
    try:
        with ftplib.FTP() as f:
            f.connect(host,int(port),timeout=20);f.login(os.environ.get('RCOMP_PS5_FTP_USER','anonymous'),os.environ.get('RCOMP_PS5_FTP_PASSWORD',''))
            def fetch(name):
                try:
                    data=bytearray();f.retrbinary('RETR /data/homebrew/PPSA88360/'+name,data.extend)
                    (a.logdir/name).write_bytes(data)
                except ftplib.all_errors as e:
                    # Do not retain an older local file when current retrieval fails.
                    (a.logdir/name).unlink(missing_ok=True)
                    fetch_errors.append(name+': '+str(e))
            fetch('rcomp_title.log')
            # Do not fetch or judge old stderr/PPM after a refused/stale launch.
            if provenance(read_stdout()):
                begin=current_begin(read_stdout());tail=read_stdout()[begin.start():]
                if re.search(r'^RCOMP-TITLE end status=\d+\s*$',tail,re.M):
                    fetch('rcomp_title.err')
                    if re.search(r'^RCOMP-TITLE end status=0\s*$',tail,re.M):fetch('depth_tiles.ppm')
    except ftplib.all_errors as e:
        fetch_errors.append('connection/collection: '+str(e))
        fetch_stdout_failed=True
stdout=read_stdout();begin=current_begin(stdout);valid_provenance=provenance(stdout) and not fetch_stdout_failed and not any(e.startswith('rcomp_title.log:') for e in fetch_errors)
status='NOT TESTED';reasons=[];s='';native_status=None
checkpoints=['source4x-patterns','pattern-transfer256','equal-depth-stencil-zfail-replace208','constant-import-quarter']
for i in range(3):checkpoints.extend(['opaque-tile%d-preclear'%i,'clear-tile%d-depth0-stencil0'%i])
if fetch_stdout_failed or any(e.startswith('rcomp_title.log:') for e in fetch_errors):reasons.append('current stdout collection failed; older files excluded')
elif not expected_build:reasons.append('expected BUILD_ID missing/invalid')
elif not begin:reasons.append('expected native BEGIN missing')
elif begin[1]!=expected_build:reasons.append('native BEGIN build differs: observed='+begin[1]+' expected='+expected_build)
else:
    tail=stdout[begin.start():]
    ends=re.findall(r'^RCOMP-TITLE end status=(\d+)\s*$',tail,re.M)
    if not ends:reasons.append('matching BEGIN but no native result; timeout/incomplete collection')
    else:
        native_status=int(ends[-1]);s=tail
        err=a.logdir/'rcomp_title.err'
        if err.exists():s+='\n'+err.read_text(errors='replace')
        if native_status==2 and 'BLOCKED depth-probe' in s:
            status='BLOCKED';reasons=re.findall(r'BLOCKED depth-probe [^\n]+',s)
        elif native_status!=0:
            status='FAIL';reasons=['completed native status='+str(native_status)]+re.findall(r'FAIL [^\n]+',s)[:20]
        else:
            status='PASS'
            if 'FAIL ' in s or 'BLOCKED depth-probe' in s:reasons.extend(re.findall(r'(?:FAIL|BLOCKED) [^\n]+',s)[:20])
            for name in checkpoints:
                if not re.search(r'PASS depth-probe checkpoint='+re.escape(name)+r' checked_samples=2621440 new_mismatches=0',s):reasons.append('completed status0 missing PASS checkpoint '+name)
                m=re.search(r'depth-probe readback='+re.escape(name)+r' hash_observed=([0-9A-F]{16}) hash_expected=([0-9A-F]{16})',s)
                if not m or m[1]!=m[2]:reasons.append('readback hash missing/different '+name)
            if 'PASS depth-probe total_mismatches=0;' not in s:reasons.append('missing final zero-mismatch result')
            if 'depth-probe target=PS5/R-comp-public-RADV' not in s:reasons.append('missing actual PS5 target')
            ppm=a.logdir/'depth_tiles.ppm'
            expected=b'P6\n1280 720\n255\n'+b'\xff\x00\x00'*(1280*256)+b'\x00\xff\x00'*(1280*256)+b'\x00\x00\xff'*(1280*208)
            if not ppm.exists():reasons.append('completed status0 composite readback missing')
            elif ppm.read_bytes()!=expected:reasons.append('composite differs from red256/green256/blue208 oracle')
            if reasons:status='FAIL'
# Old stderr/PPM are excluded from evidence unless provenance and completion
# have been established. PPM is evidence only for a completed status0 run.
evidence=['rcomp_title.log'] if (a.logdir/'rcomp_title.log').exists() and not fetch_stdout_failed else []
if valid_provenance and native_status is not None:evidence.append('rcomp_title.err')
if valid_provenance and native_status==0:evidence.append('depth_tiles.ppm')
report={'status':status,'scope':'isolated native Vulkan exact transfer/fixed-function oracle; no title assets; no established driver bug','expected_build':expected_build or None,'observed_build':begin[1] if begin else None,'native_status':native_status,'reasons':reasons,'fetch_errors':fetch_errors,'readbacks':re.findall(r'depth-probe readback=[^\n]+',s),'capabilities':re.findall(r'depth-probe device=[^\n]+|depth-probe stencilExport=[^\n]+',s),'files':{name:hashlib.sha256((a.logdir/name).read_bytes()).hexdigest() for name in evidence if (a.logdir/name).exists()}}
(a.logdir/'depth-probe-result.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps(report,indent=2))
sys.exit(0 if status=='PASS' else 2 if status in ('BLOCKED','NOT TESTED') else 1)
