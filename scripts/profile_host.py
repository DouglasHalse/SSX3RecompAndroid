#!/usr/bin/env python3
"""Where the guest's time goes, sampled on this machine.

The recompiled exe has no symbols and no PDB, but the linker writes a map file
(/MAP is on), so a sample of a thread's instruction pointer resolves to a
function name by binary search over the map's RVA column. Samples outside the
image are attributed to the module that owns the address, which is how driver
and runtime time shows up.

    python scripts/profile_host.py <exe-name> [seconds] [threads]

Only the busiest threads are sampled (by cumulative CPU time): the point is the
frame loop, not a hundred idle runtime threads. No admin rights needed.
"""
import ctypes
import ctypes.wintypes as wt
import random
import sys
import time
from collections import Counter, defaultdict
from pathlib import Path

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
psapi = ctypes.WinDLL("psapi", use_last_error=True)

k32.OpenProcess.restype = wt.HANDLE
k32.OpenProcess.argtypes = [wt.DWORD, wt.BOOL, wt.DWORD]
k32.OpenThread.restype = wt.HANDLE
k32.OpenThread.argtypes = [wt.DWORD, wt.BOOL, wt.DWORD]
psapi.EnumProcessModulesEx.restype = wt.BOOL
psapi.EnumProcessModulesEx.argtypes = [wt.HANDLE, ctypes.POINTER(ctypes.c_void_p),
                                       wt.DWORD, ctypes.POINTER(wt.DWORD), wt.DWORD]
psapi.GetModuleBaseNameW.restype = wt.DWORD
psapi.GetModuleBaseNameW.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_wchar_p, wt.DWORD]
psapi.GetModuleInformation.restype = wt.BOOL
psapi.GetModuleInformation.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_char_p, wt.DWORD]

TH32CS_SNAPPROCESS = 0x00000002
TH32CS_SNAPTHREAD = 0x00000004
THREAD_SUSPEND_RESUME = 0x0002
THREAD_GET_CONTEXT = 0x0008
THREAD_QUERY_INFORMATION = 0x0040
PROCESS_QUERY_INFORMATION = 0x0400
PROCESS_VM_READ = 0x0010
CONTEXT_CONTROL = 0x00100001
CONTEXT_SIZE = 0x4D0
CONTEXT_FLAGS_OFF = 0x30
CONTEXT_RIP_OFF = 0xF8
LIST_MODULES_ALL = 0x03


class THREADENTRY32(ctypes.Structure):
    _fields_ = [("dwSize", wt.DWORD), ("cntUsage", wt.DWORD),
                ("th32ThreadID", wt.DWORD), ("th32OwnerProcessID", wt.DWORD),
                ("tpBasePri", wt.LONG), ("tpDeltaPri", wt.LONG),
                ("dwFlags", wt.DWORD)]


def pids_of(exe):
    out = []
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    pe = ctypes.create_string_buffer(0x400)
    ctypes.c_uint32.from_buffer(pe, 0).value = 0x400
    ok = k32.Process32First(snap, pe)
    while ok:
        pid = ctypes.c_uint32.from_buffer(pe, 8).value
        name = ctypes.string_at(ctypes.addressof(pe) + 0x2C).decode("ansi", "replace")
        if name.lower() == exe.lower():
            out.append(pid)
        ok = k32.Process32Next(snap, pe)
    k32.CloseHandle(snap)
    return out


def threads_of(pid):
    out = []
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0)
    te = THREADENTRY32()
    te.dwSize = ctypes.sizeof(te)
    ok = k32.Thread32First(snap, ctypes.byref(te))
    while ok:
        if te.th32OwnerProcessID == pid:
            out.append(te.th32ThreadID)
        ok = k32.Thread32Next(snap, ctypes.byref(te))
    k32.CloseHandle(snap)
    return out


def open_thread(tid):
    return k32.OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                          False, tid)


def thread_cpu(tid):
    h = open_thread(tid)
    if not h:
        return 0
    try:
        c, e, k, u = wt.FILETIME(), wt.FILETIME(), wt.FILETIME(), wt.FILETIME()
        if not k32.GetThreadTimes(h, ctypes.byref(c), ctypes.byref(e),
                                  ctypes.byref(k), ctypes.byref(u)):
            return 0
        return (k.dwHighDateTime << 32 | k.dwLowDateTime) + \
               (u.dwHighDateTime << 32 | u.dwLowDateTime)
    finally:
        k32.CloseHandle(h)


def rip_of(h):
    if k32.SuspendThread(h) == 0xFFFFFFFF:
        return None
    try:
        ctx = ctypes.create_string_buffer(CONTEXT_SIZE)
        ctypes.memset(ctx, 0, CONTEXT_SIZE)
        ctypes.c_uint32.from_buffer(ctx, CONTEXT_FLAGS_OFF).value = CONTEXT_CONTROL
        if k32.GetThreadContext(h, ctx):
            return ctypes.c_uint64.from_buffer(ctx, CONTEXT_RIP_OFF).value
        return None
    finally:
        k32.ResumeThread(h)


def modules(pid):
    h = k32.OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, False, pid)
    if not h:
        return []
    out = []
    try:
        mods = (ctypes.c_void_p * 512)()
        need = wt.DWORD()
        if not psapi.EnumProcessModulesEx(h, mods, ctypes.sizeof(mods),
                                          ctypes.byref(need), LIST_MODULES_ALL):
            return []
        n = need.value // ctypes.sizeof(ctypes.c_void_p)
        for i in range(min(n, 512)):
            base = mods[i]
            info = (ctypes.c_char * 24)()   # MODULEINFO is 24 bytes on x64
            if not psapi.GetModuleInformation(h, ctypes.c_void_p(base), info, 24):
                continue
            size = ctypes.c_uint32.from_buffer(info, 8).value
            name = ctypes.create_unicode_buffer(260)
            psapi.GetModuleBaseNameW(h, ctypes.c_void_p(base), name, 260)
            out.append((base, base + size, name.value))
    finally:
        k32.CloseHandle(h)
    return sorted(out)


def load_map(path):
    """(rva, name) sorted. The map's Rva+Base column holds preferred-address
    VAs, so the preferred load address is subtracted, leaving a plain RVA that
    can be compared against any real load address."""
    rows = []
    pref = 0
    for line in Path(path).read_text(errors="replace").splitlines():
        if "Preferred load address is" in line:
            try:
                pref = int(line.rsplit(None, 1)[1], 16)
            except ValueError:
                pass
            continue
        parts = line.split()
        if len(parts) >= 3 and parts[0].count(":") == 1 and parts[0].split(":")[0].isdigit():
            try:
                va = int(parts[2], 16)
            except ValueError:
                continue
            rows.append((va - pref, parts[1]))
    rows = [r for r in rows if 0 <= r[0] < 0x40000000]
    rows.sort()
    return rows


def symbolise(rows, rip, base):
    if not rows:
        return None
    rva = rip - base
    if rva < rows[0][0]:
        return None
    lo, hi = 0, len(rows)
    while lo < hi:
        mid = (lo + hi) // 2
        if rows[mid][0] <= rva:
            lo = mid + 1
        else:
            hi = mid
    if lo == 0:
        return None
    return rows[lo - 1][1]


def main():
    exe = sys.argv[1]
    secs = float(sys.argv[2]) if len(sys.argv) > 2 else 20.0
    nthreads = int(sys.argv[3]) if len(sys.argv) > 3 else 8

    mapfile = Path("build") / (Path(exe).stem + ".map")
    rows = load_map(mapfile)
    pids = pids_of(exe)
    if not pids:
        sys.exit(f"{exe} is not running")
    pid = pids[0]
    mods = modules(pid)
    base = next((b for b, _e, n in mods if n.lower() == exe.lower()), None)
    if not base:
        sys.exit("could not find the module base")
    print(f"{mapfile}: {len(rows)} functions; image at {base:#x}")

    threads = threads_of(pid)
    cpu0 = {t: thread_cpu(t) for t in threads}
    time.sleep(0.5)
    busy = sorted(threads, key=lambda t: -(thread_cpu(t) - cpu0.get(t, 0)))[:nthreads]
    busy = [t for t in busy if thread_cpu(t)]
    print(f"pid {pid}: sampling {len(busy)} of {len(threads)} threads for {secs}s\n")

    per_thread = defaultdict(Counter)
    handles = {}
    for t in busy:
        h = open_thread(t)
        if h:
            handles[t] = h

    t0 = time.time()
    samples = 0
    while time.time() - t0 < secs:
        for tid, h in random.sample(list(handles.items()), len(handles)):
            rip = rip_of(h)
            if rip is None:
                continue
            samples += 1
            owner = next((n for b, e, n in mods if b <= rip < e), None)
            if owner and owner.lower() == exe.lower():
                hit = symbolise(rows, rip, base)
                per_thread[tid][hit or f"<{rip - base:#x}>"] += 1
            else:
                per_thread[tid][f"{owner or 'external'}"] += 1
        time.sleep(0.001)

    print(f"{samples} samples\n")
    for tid, c in sorted(per_thread.items(), key=lambda kv: -sum(kv[1].values())):
        total = sum(c.values())
        if total < 20:
            continue
        print(f"thread {tid}: {total} samples ({100.0 * total / max(samples, 1):.1f}%)")
        for name, n in c.most_common(14):
            print(f"   {100.0 * n / total:5.1f}%  {name}")
        print()


if __name__ == "__main__":
    main()
