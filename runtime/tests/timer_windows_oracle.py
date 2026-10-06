"""Independent Windows ntdll observations for the NT timer exports (rt_timers).

Runs only on a Windows host: it calls ntdll's NtCreateTimer, NtSetTimer,
NtCancelTimer and NtWaitForSingleObject on fresh unnamed timers and records
status codes and observable signal states. The Xbox 360 kernel is NT-derived;
these observations are an NT reference for the rules runtime/src
hle_xboxkrnl_threads.cpp follows, not Xbox 360 or PS5 hardware proof.

  python runtime/tests/timer_windows_oracle.py --out runtime/tests/timer_windows_oracle.json
"""
import argparse
import ctypes as c
import json
import platform
import sys
from pathlib import Path

TIMER_ALL_ACCESS = 0x1F0003
TIMEOUT = 0x102


def ntdll():
    if sys.platform != "win32":
        sys.exit("this oracle needs Windows ntdll")
    n = c.WinDLL("ntdll")
    n.NtCreateTimer.argtypes = [c.POINTER(c.c_void_p), c.c_uint32, c.c_void_p, c.c_int]
    n.NtSetTimer.argtypes = [c.c_void_p, c.POINTER(c.c_int64), c.c_void_p, c.c_void_p, c.c_ubyte, c.c_int32,
                             c.POINTER(c.c_ubyte)]
    n.NtCancelTimer.argtypes = [c.c_void_p, c.POINTER(c.c_ubyte)]
    n.NtWaitForSingleObject.argtypes = [c.c_void_p, c.c_ubyte, c.POINTER(c.c_int64)]
    n.NtClose.argtypes = [c.c_void_p]
    for f in (n.NtCreateTimer, n.NtSetTimer, n.NtCancelTimer, n.NtWaitForSingleObject, n.NtClose):
        f.restype = c.c_uint32
    return n


class Oracle:
    def __init__(self):
        self.n = ntdll()

    def create(self, timer_type):
        h = c.c_void_p()
        status = self.n.NtCreateTimer(c.byref(h), TIMER_ALL_ACCESS, None, timer_type)
        return status, h

    def set(self, h, due_100ns, period_ms=0, resume=0, want_previous=True):
        due = c.c_int64(due_100ns)
        prev = c.c_ubyte(0xAA)
        status = self.n.NtSetTimer(h, c.byref(due), None, None, resume, period_ms,
                                   c.byref(prev) if want_previous else None)
        return status, prev.value

    def cancel(self, h):
        state = c.c_ubyte(0xAA)
        return self.n.NtCancelTimer(h, c.byref(state)), state.value

    def wait(self, h, ms):
        t = c.c_int64(-ms * 10000)
        return self.n.NtWaitForSingleObject(h, 0, c.byref(t))


def observe():
    o = Oracle()
    rows = []

    def row(name, **values):
        rows.append({"case": name, **{k: (f"0x{v:08X}" if k.endswith("status") else v) for k, v in values.items()}})

    s, bad = o.create(2)
    row("create invalid type 2", status=s)
    s, h = o.create(0)
    row("create notification", status=s, initially_signalled=o.wait(h, 0) == 0)
    s, prev = o.set(h, -10 * 10000)
    row("set notification relative 10 ms", status=s, previous_state=prev, signalled_at_once=o.wait(h, 0) == 0)
    row("notification after due", wait_status=o.wait(h, 500))
    row("notification stays signalled", wait_status=o.wait(h, 0))
    s, prev = o.set(h, -1000 * 10000)
    row("re-set signalled notification", status=s, previous_state=prev, signalled_after_set=o.wait(h, 0) == 0)
    s, state = o.cancel(h)
    row("cancel before due", status=s, current_state=state, wait_after_cancel_status=o.wait(h, 50))
    o.set(h, -1 * 10000)
    o.wait(h, 500)
    s, state = o.cancel(h)
    row("cancel after fire keeps state", status=s, current_state=state, wait_status=o.wait(h, 0))
    s, state = o.cancel(h)
    row("cancel idle timer", status=s, current_state=state)
    s, prev = o.set(h, 1)  # absolute FILETIME 1 = 1601: already past
    row("absolute due in the past", status=s, wait_status=o.wait(h, 0) if False else o.wait(h, 50))
    s, prev = o.set(h, -10 * 10000, period_ms=-1)
    row("negative period", status=s)
    s, prev = o.set(h, -10 * 10000, resume=1)
    row("resume requested", status=s, wait_status=o.wait(h, 500))
    s, _ = o.set(h, -10 * 10000, want_previous=False)
    row("set without previous state", status=s)
    o.n.NtClose(h)

    s, h = o.create(1)
    row("create synchronization", status=s)
    o.set(h, -10 * 10000)
    row("synchronization first wait", wait_status=o.wait(h, 500))
    row("synchronization auto reset", wait_status=o.wait(h, 0))
    o.set(h, -10 * 10000, period_ms=20)
    first, second = o.wait(h, 500), o.wait(h, 500)
    row("periodic synchronization fires repeatedly", first_status=first, second_status=second)
    s, state = o.cancel(h)
    row("cancel periodic", status=s, wait_after_cancel_status=o.wait(h, 100))
    o.n.NtClose(h)
    return rows


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", type=Path, required=True)
    a = ap.parse_args()
    report = {"oracle": "Windows ntdll", "platform": platform.platform(), "rows": observe()}
    a.out.write_text(json.dumps(report, indent=1) + "\n", encoding="utf-8")
    print(json.dumps(report["rows"], indent=1))


if __name__ == "__main__":
    main()
