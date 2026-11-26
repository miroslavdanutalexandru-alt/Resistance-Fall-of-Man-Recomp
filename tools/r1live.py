"""R1_LIVE reader / controller (ps3recomp/libs/video/rsx_r1live.inc; needs R1_LIVE=1 in the game's environment).

  python tools\\r1live.py                      10 s of per-second counters of the running ResistanceFoMGame.exe
  python tools\\r1live.py watch 30 [csv]       30 s of per-second lines (optionally appended to a csv file)
  python tools\\r1live.py set b                switch the live A/B variant (letter shown bottom-left in the game)
  python tools\\r1live.py tint on|off          per-draw match tint (RSX_DLSS_DEBUG=match) on / off
  python tools\\r1live.py cycle ab 20 3        a, b, a, b, a, b for 20 s each, then per-variant totals
  python tools\\r1live.py status               cumulative counters
  --pid N                                       pick the process (default: the first one with the block)

Read-only apart from the one request word; it never suspends or stops the game.
"""
import ctypes, ctypes.wintypes as wt, subprocess, sys, time, struct

MAGIC = 0x564C3152
NAMES = ["frames", "fg_frames", "key_near", "key_probe", "nearx", "rm_budget", "near_cam", "near_budget",
         "near_unclear", "near_noloose", "near_other", "rm_ok", "rm_vl", "loose_ok", "loose_fail", "nokey",
         "nohist", "cam_only", "amb_drop", "rm_unclear", "late", "nopso", "rec"]
SHORT = ["key_near", "nearx", "rm_budget", "near_cam", "near_budget", "near_unclear", "near_noloose", "near_other",
         "rm_ok", "rm_vl", "loose_fail", "nokey", "nohist", "cam_only", "late"]
VARS = {"a": "fix (6b5bbd7): view-attached keys past the re-match caps", "b": "old (6b84476)", "c": "view-lock off",
        "d": "re-match + loose off", "e": "fix, no re-match caps", "f": "camera-only motion", "g": "FG off",
        "h": "SR jitter off"}
EXES = ("ResistanceFoMGame.exe", "ResistanceFoM.exe")
k32 = ctypes.WinDLL("kernel32", use_last_error=True)
k32.OpenFileMappingW.restype = wt.HANDLE
k32.OpenFileMappingW.argtypes = [wt.DWORD, wt.BOOL, wt.LPCWSTR]
k32.MapViewOfFile.restype = ctypes.c_void_p
k32.MapViewOfFile.argtypes = [wt.HANDLE, wt.DWORD, wt.DWORD, wt.DWORD, ctypes.c_size_t]


def find_pids():
    pids = []
    for exe in EXES:
        out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq " + exe, "/FO", "CSV", "/NH"], capture_output=True, text=True).stdout
        pids += [int(l.split('","')[1]) for l in out.splitlines() if l.lower().startswith('"' + exe.lower() + '"')]
    return pids


def open_blk(pid):
    h = k32.OpenFileMappingW(0x0002 | 0x0004, False, "Local\\R1Live_%d" % pid)  # FILE_MAP_WRITE | FILE_MAP_READ
    if not h:
        return None
    return k32.MapViewOfFile(h, 0x0002 | 0x0004, 0, 0, 4096)


def read(p):
    raw = ctypes.string_at(p, 64 + 4 * len(NAMES))
    return struct.unpack_from("<16I", raw, 0), list(struct.unpack_from("<%dI" % len(NAMES), raw, 64))


def var_of(hdr):
    return chr(hdr[6]) if hdr[6] else "-"


def request(blk, ch):
    ctypes.c_uint32.from_address(blk + 7 * 4).value = ord(ch)
    for _ in range(40):  # taken once per present
        time.sleep(0.05)
        if ctypes.c_uint32.from_address(blk + 7 * 4).value == 0:
            return True
    return False


def line(hdr, d, secs):
    fr = d[0]
    return "f%-7d var %s fg %d tint %d fps %3d  " % (hdr[3], var_of(hdr), hdr[4], hdr[5], fr / secs if secs else 0) + \
        " ".join("%s=%d" % (n, d[NAMES.index(n)]) for n in SHORT)


def diff(c0, c):
    return [(b - a) & 0xFFFFFFFF for a, b in zip(c0, c)]


def main():
    args = sys.argv[1:]
    pid_arg = None
    if "--pid" in args:
        i = args.index("--pid"); pid_arg = int(args[i + 1]); del args[i:i + 2]
    pids = [pid_arg] if pid_arg else find_pids()
    blk = None
    for pid in pids:
        blk = open_blk(pid)
        if blk:
            break
    if not blk:
        sys.exit("no ResistanceFoMGame.exe with R1_LIVE=1 found (pids %s)" % pids)
    hdr, _ = read(blk)
    if hdr[0] != MAGIC:
        sys.exit("bad magic 0x%08X" % hdr[0])
    cmd = args[0] if args else "watch"
    if cmd == "set":
        v = args[1].lower()[:1]
        if v not in VARS:
            sys.exit("variants: " + ", ".join("%s = %s" % kv for kv in VARS.items()))
        ok = request(blk, v)
        hdr, _ = read(blk)
        print("pid %d present %d variant now %s (%s)%s" % (hdr[2], hdr[3], var_of(hdr), VARS.get(var_of(hdr), "?"),
                                                           "" if ok else "  [request not taken yet: game paused / minimised?]"))
        return
    if cmd == "tint":
        on = (args[1].lower() if len(args) > 1 else "on") in ("on", "1", "+")
        ok = request(blk, "+" if on else "-")
        hdr, _ = read(blk)
        print("pid %d tint %s, variant %s%s" % (hdr[2], "on" if hdr[5] else "off", var_of(hdr), "" if ok else "  [request not taken yet]"))
        return
    if cmd == "status":
        print("pid %d present %d variant %s fg %d tint %d" % (hdr[2], hdr[3], var_of(hdr), hdr[4], hdr[5]))
        _, c = read(blk)
        print(" ".join("%s=%d" % (n, c[i]) for i, n in enumerate(NAMES)))
        return
    if cmd == "cycle":
        letters = args[1].lower() if len(args) > 1 else "ab"
        secs = int(args[2]) if len(args) > 2 else 20
        rounds = int(args[3]) if len(args) > 3 else 3
        tot = {v: [0] * len(NAMES) for v in letters}
        start = var_of(hdr)
        for r in range(rounds):
            for v in letters:
                request(blk, v)
                time.sleep(1.0)  # let the switch settle (history of the old variant)
                h0, c0 = read(blk)
                for _ in range(secs - 1):
                    time.sleep(1.0)
                    h1, c1 = read(blk)
                    print(line(h1, diff(c0, c1), 1.0), flush=True)
                    tot[v] = [a + b for a, b in zip(tot[v], diff(c0, c1))]
                    h0, c0 = h1, c1
        if start in VARS:
            request(blk, start)
        print("\nper 1000 frames (frames counted):")
        for v in letters:
            t = tot[v]; fr = t[0] or 1
            print("%s %-28s frames %6d  " % (v, VARS[v][:28], t[0]) + " ".join("%s=%.1f" % (n, 1000.0 * t[NAMES.index(n)] / fr) for n in SHORT))
        return
    secs = int(args[1]) if cmd == "watch" and len(args) > 1 else 10
    csv = open(args[2], "a") if cmd == "watch" and len(args) > 2 else None
    if csv and csv.tell() == 0:
        csv.write("time,present,variant,fg,tint," + ",".join(NAMES) + "\n")
    hdr0, c0 = read(blk)
    print("pid %d; per second: %s" % (hdr0[2], " ".join(SHORT)))
    for _ in range(secs):
        time.sleep(1.0)
        hdr, c = read(blk)
        d = diff(c0, c)
        print(line(hdr, d, 1.0), flush=True)
        if csv:
            csv.write("%f,%d,%s,%d,%d,%s\n" % (time.time(), hdr[3], var_of(hdr), hdr[4], hdr[5], ",".join(str(x) for x in d)))
            csv.flush()
        hdr0, c0 = hdr, c


if __name__ == "__main__":
    main()
