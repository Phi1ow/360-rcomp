#!/usr/bin/env python3
"""Runs one R-comp PS5 kit test end to end (outside the repository).

  python run_ps5_test.py <N-name> <timeout_s> [--resume-start K]

push (tools/ps5_ftp_kit.py) -> klog capture 3232 -> ps5vkctl "launch PPSA88360"
-> poll "procs" every 2 s until the title is gone or the timeout -> on timeout
"kill PPSA88360" (our title only) -> pull (tools/ps5_ftp_kit.py).
Everything is written to ps5-logs/<N-name>/ (run.txt, klog.txt, rcomp_title.log).
"""
import ftplib, io, os, socket, subprocess, sys, threading, time

HOST = os.environ["RCOMP_PS5_HOST"]  # the console's address
FTP, KLOG, CTL = 2120, 3232, 9111
TITLE = "PPSA88360"
ROOT = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.join(ROOT, "r-comp")
KIT = os.environ.get("RCOMP_KIT_DIR", os.path.join(ROOT, "kit", "ps5-kit"))
LOGS = os.environ.get("RCOMP_LOG_DIR", os.path.join(ROOT, "ps5-logs"))

name, timeout = sys.argv[1], int(sys.argv[2])
resume = None
if "--resume-start" in sys.argv:
    resume = int(sys.argv[sys.argv.index("--resume-start") + 1])
out = os.path.join(LOGS, name + (f"-resume{resume}" if resume is not None else ""))
os.makedirs(out, exist_ok=True)
runlog = open(os.path.join(out, "run.txt"), "a", encoding="utf-8")


def say(msg):
    line = f"{time.strftime('%H:%M:%S')} {msg}"
    print(line, flush=True)
    runlog.write(line + "\n")
    runlog.flush()


def ctl(cmd):
    with socket.create_connection((HOST, CTL), timeout=10) as s:
        s.sendall((cmd + "\n").encode())
        s.settimeout(30)
        data = b""
        while True:
            chunk = s.recv(4096)
            if not chunk:
                break
            data += chunk
    return data.decode(errors="replace").strip()


def tool(*args):
    cmd = [sys.executable, os.path.join(REPO, "tools", "ps5_ftp_kit.py"), *args,
           "--host", HOST, "--port", str(FTP)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    say(f"$ ps5_ftp_kit.py {' '.join(args)} -> exit {r.returncode}")
    for l in (r.stdout + r.stderr).splitlines():
        say("  " + l)
    return r.returncode


state = ctl("status")
say(f"status before: {state}")
if "idle" not in state:
    say("VERDICT BUSY: console is not idle; not deploying")
    sys.exit(3)

if tool("push", os.path.join(KIT, name, TITLE)) != 0:
    say("VERDICT DEPLOY-FAILED")
    sys.exit(5)
if resume is not None:
    f = ftplib.FTP(); f.connect(HOST, FTP, timeout=30); f.login()
    f.storbinary(f"STOR /data/homebrew/{TITLE}/rcomp_cpu_start.txt", io.BytesIO(f"{resume}\n".encode()))
    f.quit()
    say(f"uploaded rcomp_cpu_start.txt = {resume}")

with open(r"D:\ps5-toolchain\chmodtitle\chmodtitle.elf", "rb") as fe:
    elf = fe.read()
with socket.create_connection((HOST, 9021), timeout=20) as s:
    s.sendall(elf)
    s.shutdown(socket.SHUT_WR)
    s.settimeout(10)
    reply = b""
    try:
        while True:
            chunk = s.recv(4096)
            if not chunk:
                break
            reply += chunk
    except socket.timeout:
        pass
say("chmod: " + reply.decode(errors="replace").strip())
if b"failed=0" not in reply:
    say("VERDICT CHMOD-FAILED")
    sys.exit(5)

stop = threading.Event()


def klog():
    try:
        with socket.create_connection((HOST, KLOG), timeout=10) as s, \
                open(os.path.join(out, "klog.txt"), "ab") as fo:
            s.settimeout(1)
            while not stop.is_set():
                try:
                    chunk = s.recv(65536)
                except socket.timeout:
                    continue
                if not chunk:
                    break
                fo.write(chunk)
                fo.flush()
    except Exception as e:
        say(f"klog capture error: {e}")


th = threading.Thread(target=klog, daemon=True)
th.start()
time.sleep(2)

def log_has_end():
    """True once the title log's last run has its RCOMP-TITLE end line
    (reads only the last 8 KiB)."""
    try:
        f = ftplib.FTP(); f.connect(HOST, FTP, timeout=15); f.login(); f.voidcmd("TYPE I")
        path = f"/data/homebrew/{TITLE}/rcomp_title.log"
        size = f.size(path)
        buf = io.BytesIO()
        f.retrbinary("RETR " + path, buf.write, rest=max(0, size - 8192))
        f.quit()
    except ftplib.all_errors:
        return False
    tail = buf.getvalue().decode(errors="replace")
    tail = tail[tail.rfind("RCOMP-TITLE begin"):] if "RCOMP-TITLE begin" in tail else tail
    return "RCOMP-TITLE end status=" in tail


say(f"launch: {ctl('launch ' + TITLE)}")
t0 = time.time()
seen = False
finished = False
while time.time() - t0 < timeout:
    time.sleep(2)
    p = ctl("procs")
    running = f"title={TITLE}" in p and "count=0" not in p
    if running and not seen:
        say(f"running: {p}")
    seen = seen or running
    if seen and not running:
        say(f"title exited after {time.time() - t0:.0f}s: {p}")
        finished = True
        break
    if running and log_has_end():
        say(f"end line in the log after {time.time() - t0:.0f}s; closing: {ctl('kill ' + TITLE)}")
        time.sleep(3)
        finished = True
        break
    if not seen and time.time() - t0 > 30:
        say(f"title never seen running after 30s: {p}")
        break
if not finished and seen:
    say(f"TIMEOUT after {timeout}s: kill: {ctl('kill ' + TITLE)}")
    time.sleep(3)
say(f"status after: {ctl('status')}")
time.sleep(2)
stop.set()
th.join(5)

tool("pull", "--out", out)
