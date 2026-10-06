#!/usr/bin/env bash
# R-comp PS5 test title runner (owner: Agent 2). Console execution evidence
# is recorded per artifact in docs/PS5_RESULTS.md and the session reports.
# Per AGENTS.md only PRIME starts a console run.
#
#   platform/ps5/tools/run_title.sh [--dry-run] [--cpu-corpus | --app <dir>]
#
# --app <dir> runs any prepared title folder instead (e.g. a PS5 test kit
# folder N-name/PPSA88360); it must be named after the title id and carry a
# manifest.sha256 that matches its files.
#
# --cpu-corpus runs the build/platform-ps5-title-cpu/ folder (build_title.sh
# --cpu-corpus). RCOMP_CPU_START=<n> (optional) uploads rcomp_cpu_start.txt so
# the corpus resumes at test n after a crash; without it any stale start file
# is deleted. Extract results afterwards with collect_cpu_results.py.
#
# Sequence: check the console is idle -> upload build/platform-ps5-title/dist/<ID>/
# to /data/homebrew/<ID>/ over FTP and read every file back (sha256) -> start the
# kernel-log capture -> launch <ID> -> wait for the log's end line, an exit or the timeout ->
# on timeout close OUR title only -> download /data/homebrew/<ID>/rcomp_title.log
# -> judge the last "RCOMP-TITLE end status=N" line.
#
# Console-side services it talks to (none of them is provided by this repo):
#   * an FTP server payload (upload/readback; writes only under /data/homebrew/<ID>/)
#   * a kernel-log (klog) TCP stream
#   * PS5_Vulkan's resident control payload "ps5vkctl" (GPL-3.0, payload/ps5vkctl):
#     one line per connection: "procs", "launch <ID>", "kill <ID>"; its kill
#     refuses unless the running application is that title id.
#
# Required environment (no defaults, nothing is guessed):
#   RCOMP_PS5_HOST            console address
#   RCOMP_PS5_FTP_PORT        FTP port of the console's FTP payload
#   RCOMP_PS5_KLOG_PORT       kernel-log stream port
#   RCOMP_PS5_CTL_PORT        ps5vkctl port
#   RCOMP_RUN_TIMEOUT         seconds to let the title run (5..600)
#   RCOMP_RUN_LOG_DIR         local directory for klog + title log + verdict
#   RCOMP_CONSOLE_RUN_AUTHORIZED=1   explicit go-ahead for touching the console
# Optional: RCOMP_PS5_FTP_USER (default anonymous), RCOMP_PS5_FTP_PASSWORD (default empty)
#   RCOMP_PS5_LOADER_PORT + RCOMP_PS5_CHMOD_ELF: ELF loader port and a built
#   tools/ps5_chmod_title payload, sent after the upload. Needed when the FTP
#   server cannot set the execute bit (zftpd writes 0666 and ignores SITE CHMOD;
#   the system then refuses eboot.bin with 0x80aa001a errno 13). Without them
#   the script checks the mode over FTP and stops (exit 5) if eboot.bin is not
#   executable.
#
# The titles never end themselves (a PS5 application cannot: libc exit() ->
# SIGSYS, see docs/PS5_RESULTS.md): they write "RCOMP-TITLE end status=N" and
# park. The wait loop reads the log tail and closes the title as soon as that
# line is there; the timeout only covers titles that never get that far.
#
# Exit: 0 title reported status=0 | 1 title reported failure | 2 usage/missing
# settings | 3 console busy or refused | 4 no result (timeout, crash, no log) |
# 5 deploy/readback mismatch.
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../.." && pwd)
dry=0
cpu=0
app_arg=
while (( $# )); do
    case "$1" in
        --dry-run) dry=1 ;;
        --cpu-corpus) cpu=1 ;;
        --app) [[ $# -ge 2 ]] || { echo "--app needs a directory" >&2; exit 2; }; app_arg=$2; shift ;;
        *) echo "usage: ${0##*/} [--dry-run] [--cpu-corpus | --app <dir>]" >&2; exit 2 ;;
    esac
    shift
done
[[ -z $app_arg || $cpu == 0 ]] || { echo "--app and --cpu-corpus are exclusive" >&2; exit 2; }
if [[ -n ${RCOMP_CPU_START:-} ]]; then
    (( cpu )) && [[ $RCOMP_CPU_START =~ ^[0-9]+$ ]] ||
        { echo "RCOMP_CPU_START needs --cpu-corpus and a test index" >&2; exit 2; }
fi

missing=()
for v in RCOMP_PS5_HOST RCOMP_PS5_FTP_PORT RCOMP_PS5_KLOG_PORT RCOMP_PS5_CTL_PORT \
         RCOMP_RUN_TIMEOUT RCOMP_RUN_LOG_DIR RCOMP_CONSOLE_RUN_AUTHORIZED; do
    [[ -n ${!v:-} ]] || missing+=("$v")
done
if (( ${#missing[@]} )); then
    printf 'refusing to run: missing required settings: %s\n' "${missing[*]}" >&2
    echo "(see the header of ${0##*/}; no console address or port is ever assumed)" >&2
    exit 2
fi
[[ $RCOMP_CONSOLE_RUN_AUTHORIZED == 1 ]] ||
    { echo "refusing to run: RCOMP_CONSOLE_RUN_AUTHORIZED must be 1" >&2; exit 2; }
for v in RCOMP_PS5_FTP_PORT RCOMP_PS5_KLOG_PORT RCOMP_PS5_CTL_PORT; do
    [[ ${!v} =~ ^[0-9]+$ ]] && (( ${!v} > 0 && ${!v} < 65536 )) ||
        { echo "$v must be a TCP port number" >&2; exit 2; }
done
[[ $RCOMP_RUN_TIMEOUT =~ ^[0-9]+$ ]] && (( RCOMP_RUN_TIMEOUT >= 5 && RCOMP_RUN_TIMEOUT <= 600 )) ||
    { echo "RCOMP_RUN_TIMEOUT must be 5..600 seconds" >&2; exit 2; }
command -v python3 >/dev/null || { echo "missing python3" >&2; exit 2; }

# The title id is the one in the param.json of the folder being run (--app), else of the platform test title's.
param_json="$root/platform/ps5/title/sce_sys/param.json"
if [[ -n $app_arg && -f $app_arg/sce_sys/param.json ]]; then
    param_json="$app_arg/sce_sys/param.json"
fi
title_id=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["titleId"])' "$param_json")
[[ $title_id =~ ^PPSA[0-9]{5}$ ]] || { echo "bad title id $title_id" >&2; exit 2; }
if [[ -n $app_arg ]]; then
    app=$(cd -- "$app_arg" 2>/dev/null && pwd) || { echo "no directory $app_arg" >&2; exit 2; }
    [[ ${app##*/} == "$title_id" ]] || { echo "$app must be named $title_id" >&2; exit 2; }
elif (( cpu )); then
    app="$root/build/platform-ps5-title-cpu/dist/$title_id"
else
    app="$root/build/platform-ps5-title/dist/$title_id"
fi
[[ -f $app/eboot.bin && -f $app/manifest.sha256 ]] ||
    { echo "no built title at $app (run build_title.sh)" >&2; exit 2; }
(cd "$app" && sha256sum --quiet -c manifest.sha256) ||
    { echo "$app does not match its manifest" >&2; exit 2; }

stamp=$(date -u +%Y%m%dT%H%M%SZ)
logdir="$RCOMP_RUN_LOG_DIR/$title_id-$stamp"
if (( dry )); then
    cat <<EOF
dry run, nothing sent. Plan:
  1. ctl $RCOMP_PS5_HOST:$RCOMP_PS5_CTL_PORT "procs"      -> refuse unless no app is running
  2. ftp $RCOMP_PS5_HOST:$RCOMP_PS5_FTP_PORT upload $app -> /data/homebrew/$title_id/ and read back
     (cpu corpus: rcomp_cpu_start.txt = ${RCOMP_CPU_START:-<deleted>}; mode cpu=$cpu)
  3. klog $RCOMP_PS5_HOST:$RCOMP_PS5_KLOG_PORT -> $logdir/klog.txt (started before launch)
     then execute bit: ps5_chmod_title via the ELF loader if configured, else a mode check
  4. ctl "launch $title_id"; poll "procs" and the log tail every 2 s for up to ${RCOMP_RUN_TIMEOUT}s
  5. once the log has its end line, or on timeout, only if procs reports title=$title_id:
     ctl "kill $title_id"
  6. ftp download /data/homebrew/$title_id/rcomp_title.log -> $logdir/
  7. verdict from the last "RCOMP-TITLE end status=N" line
EOF
    exit 0
fi

mkdir -p "$logdir"
export RUN_TITLE_ID=$title_id RUN_APP=$app RUN_LOGDIR=$logdir
exec python3 - <<'PY'
import atexit, ftplib, hashlib, io, os, pathlib, re, socket, sys, threading, time

host = os.environ["RCOMP_PS5_HOST"]
ctl_port = int(os.environ["RCOMP_PS5_CTL_PORT"])
ftp_port = int(os.environ["RCOMP_PS5_FTP_PORT"])
klog_port = int(os.environ["RCOMP_PS5_KLOG_PORT"])
timeout = int(os.environ["RCOMP_RUN_TIMEOUT"])
title = os.environ["RUN_TITLE_ID"]
app = pathlib.Path(os.environ["RUN_APP"])
logdir = pathlib.Path(os.environ["RUN_LOGDIR"])
remote = f"/data/homebrew/{title}"

def say(msg):
    print(f"==> [run] {msg}", flush=True)
    with open(logdir / "run.txt", "a") as f:
        f.write(msg + "\n")

def ctl(cmd):
    with socket.create_connection((host, ctl_port), timeout=15) as s:
        s.sendall((cmd + "\n").encode("ascii"))
        # ps5vkctl may poll four launch attempts for five seconds each.
        s.settimeout(30)
        buf = b""
        while b"\n" not in buf:
            chunk = s.recv(512)
            if not chunk:
                break
            buf += chunk
    return buf.decode("ascii", "replace").strip()

def running_title():
    reply = ctl("procs")
    m = re.search(r"title=(\S*)", reply)
    c = re.search(r"count=(\d+)", reply)
    return (m.group(1) if m else ""), int(c.group(1)) if c else -1, reply

def ftp():
    f = ftplib.FTP()
    f.connect(host, ftp_port, timeout=30)
    f.login(os.environ.get("RCOMP_PS5_FTP_USER", "anonymous"),
            os.environ.get("RCOMP_PS5_FTP_PASSWORD", ""))
    return f

def ensure_dir(f, path):
    try:
        f.mkd(path)
    except ftplib.error_perm:
        pass

def clear_stale_title_log(f, directory):
    try:
        f.delete(f"{directory}/rcomp_title.log")
    except ftplib.error_perm:
        pass  # Could mean absent or denied: the directory listing decides.
    names = f.nlst(directory)
    if any(pathlib.PurePosixPath(name.rstrip("/")).name == "rcomp_title.log" for name in names):
        raise RuntimeError("stale rcomp_title.log is still present")

# 1. idle check
t, count, reply = running_title()
say(f"console: {reply}")
if count != 0:
    say(f"VERDICT BUSY: console runs '{t}' ({count} processes); not deploying or launching")
    sys.exit(3)

# 2. deploy + readback (only under /data/homebrew/<ID>/)
files = sorted(p for p in app.rglob("*") if p.is_file())
with ftp() as f:
    ensure_dir(f, remote)
    for p in files:
        rel = p.relative_to(app).as_posix()
        parts = rel.split("/")[:-1]
        for i in range(len(parts)):
            ensure_dir(f, remote + "/" + "/".join(parts[: i + 1]))
        with open(p, "rb") as src:
            f.storbinary(f"STOR {remote}/{rel}", src)
    try:
        clear_stale_title_log(f, remote)
    except (ftplib.Error, OSError, EOFError, RuntimeError) as e:
        say(f"VERDICT DEPLOY-MISMATCH: cannot verify removal of stale title log ({e}); not launching")
        sys.exit(5)
    start = os.environ.get("RCOMP_CPU_START", "")
    if start:
        f.storbinary(f"STOR {remote}/rcomp_cpu_start.txt", io.BytesIO(f"{int(start)}\n".encode()))
        say(f"cpu corpus resumes at test {int(start)}")
    else:
        try:
            f.delete(f"{remote}/rcomp_cpu_start.txt")
        except ftplib.error_perm:
            pass
    for p in files:
        rel = p.relative_to(app).as_posix()
        buf = io.BytesIO()
        f.retrbinary(f"RETR {remote}/{rel}", buf.write)
        if hashlib.sha256(buf.getvalue()).digest() != hashlib.sha256(p.read_bytes()).digest():
            say(f"VERDICT DEPLOY-MISMATCH: {rel} read back differs")
            sys.exit(5)
say(f"deployed and verified {len(files)} files in {remote}")

# 2b. execute bit (see the header)
loader_port = os.environ.get("RCOMP_PS5_LOADER_PORT", "")
chmod_elf = os.environ.get("RCOMP_PS5_CHMOD_ELF", "")
if loader_port and chmod_elf:
    with socket.create_connection((host, int(loader_port)), timeout=20) as s:
        s.sendall(pathlib.Path(chmod_elf).read_bytes())
        s.shutdown(socket.SHUT_WR)
        s.settimeout(10)
        reply = b""
        try:
            while chunk := s.recv(4096):
                reply += chunk
        except socket.timeout:
            pass
    say("chmod: " + reply.decode(errors="replace").strip())
    if b"failed=0" not in reply:
        say("VERDICT DEPLOY-MISMATCH: ps5_chmod_title did not report failed=0")
        sys.exit(5)
with ftp() as f:
    listing = []
    f.retrlines(f"LIST {remote}", listing.append)
mode = next((l.split()[0] for l in listing if l.split()[-1:] == ["eboot.bin"]), "")
if mode[3:4] != "x":
    say(f"VERDICT DEPLOY-MISMATCH: eboot.bin is {mode or '?'} (no execute bit); set "
        "RCOMP_PS5_LOADER_PORT and RCOMP_PS5_CHMOD_ELF (tools/ps5_chmod_title)")
    sys.exit(5)

# 3. klog capture, started before the launch
stop = threading.Event()
def capture():
    try:
        with socket.create_connection((host, klog_port), timeout=10) as s, \
                open(logdir / "klog.txt", "wb") as out:
            s.settimeout(1)
            while not stop.is_set():
                try:
                    chunk = s.recv(65536)
                except socket.timeout:
                    continue
                if not chunk:
                    break
                out.write(chunk)
                out.flush()
    except OSError as e:
        (logdir / "klog.error").write_text(str(e))
capture_thread = threading.Thread(target=capture, daemon=True)
capture_thread.start()

def stop_capture():
    stop.set()
    # Covers the ten-second connect timeout and one-second receive timeout.
    capture_thread.join(timeout=12)
    if capture_thread.is_alive():
        (logdir / "klog.error").write_text("capture thread did not stop within 12 seconds")

# Also close/flush the capture if a control or FTP exception unwinds the run.
atexit.register(stop_capture)
time.sleep(2)

# 4. launch and wait
reply = ctl(f"launch {title}")
say(f"launch: {reply}")
if not reply.startswith("ok"):
    stop_capture()
    say("VERDICT REFUSED: launch was not accepted")
    sys.exit(3)
deadline = time.time() + timeout
exited = False
time.sleep(2)
def log_has_end():
    try:
        with ftp() as f:
            f.voidcmd("TYPE I")
            path = f"{remote}/rcomp_title.log"
            size = f.size(path)
            buf = io.BytesIO()
            f.retrbinary(f"RETR {path}", buf.write, rest=max(0, size - 8192))
    except ftplib.all_errors:
        return False
    tail = buf.getvalue().decode(errors="replace")
    if "RCOMP-TITLE begin" in tail:
        tail = tail[tail.rfind("RCOMP-TITLE begin"):]
    return "RCOMP-TITLE end status=" in tail

ended = False
while time.time() < deadline:
    t, count, _ = running_title()
    if t != title or count == 0:
        exited = True
        break
    if log_has_end():
        ended = True
        break
    time.sleep(2)

# 5. targeted close of OUR title only
if not exited:
    t, count, reply = running_title()
    if t == title and count > 0:
        why = "end line in the log" if ended else f"timeout after {timeout}s"
        say(f"{why}; closing {title}: {ctl(f'kill {title}')}")
    else:
        say(f"timeout, but running title is '{t}'; nothing closed")
time.sleep(2)
stop_capture()

# 6. collect the title log
log = logdir / "rcomp_title.log"
try:
    with ftp() as f, open(log, "wb") as out:
        f.retrbinary(f"RETR {remote}/rcomp_title.log", out.write)
except ftplib.all_errors as e:
    say(f"VERDICT NO-RESULT: cannot download rcomp_title.log ({e}); see klog.txt")
    sys.exit(4)

# 7. verdict from the last run in the log
def last_title_run(text):
    begins = list(re.finditer(r"^RCOMP-TITLE begin(?:[ \t].*)?\r?$", text, re.M))
    if not begins:
        return "", None
    current = text[begins[-1].start():]
    ends = re.findall(r"^RCOMP-TITLE end status=(\d+)[ \t]*\r?$", current, re.M)
    return current, int(ends[-1]) if ends else None

text, status = last_title_run(log.read_text("utf-8", "replace"))
cpu_crash = re.findall(r'^\{.*"reason":"signal \d+"\}$', text, re.M)
if cpu_crash:
    say(f"cpu corpus crashed on: {cpu_crash[-1]} (rerun with RCOMP_CPU_START=<index+1>)")
if status is None:
    say("VERDICT NO-RESULT: latest title run has no begin/end pair (crash or timeout); see klog.txt")
    sys.exit(4)
say(f"VERDICT {'PASS' if status == 0 else 'FAIL'}: title status={status}; logs in {logdir}")
sys.exit(0 if status == 0 else 1)
PY
