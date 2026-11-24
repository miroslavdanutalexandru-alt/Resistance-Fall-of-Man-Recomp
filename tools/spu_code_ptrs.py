#!/usr/bin/env python3
"""Static discovery of SPU code reached only through code pointers.

find_spu_functions.detect_functions seeds functions from direct branches and a
few indirect idioms. Code whose address only exists as DATA, or as an
immediate that is later called through `bisl`/`bi`, is never seeded; at run
time the call lands on an address with no lifted function and the SPU
interpreter runs it ("unlifted job code at LS 0x... -- interpreting").
Example: Resistance 2's mesh job module (image 222) runs its static
constructors from the GCC `.ctors` list (__do_global_ctors_aux: walk back from
__CTOR_END__ calling every word until the -1 head). One of them is an empty
`bi $r0` stub at LS 0x49D8 that nothing else names, so every mesh job ran it
in the interpreter.

This pass finds three kinds of code pointers:

  ctors    GCC .ctors/.dtors lists: 0xFFFFFFFF, p1 .. pn, 0x00000000 with every
           p a code address (n >= 1). Exact by construction.
  tables   runs of >= 2 consecutive words that are all entry-like code
           addresses (function-pointer tables, vtables, absolute jump tables)
  imms     `ila`/`il`/`ilhu`+`iohl` immediates that are entry-like code
           addresses (address-of-function, continuation pointers)

A target is entry-like when it is 4-aligned inside the code range, decodes to
a real instruction, and either starts a known function or follows a
terminator (`bi`/`br`/`bra`/`iret`/`stop`) or padding (`lnop`/`nop`). A target
in a gap between detected functions is accepted only when the code from it
decodes cleanly up to a terminator with every direct branch inside the image,
so data tails stay un-disassembled.

discover_elf() splits the targets for spu_lifter.py: 'observed' (inside a
detected function: --observed-entries only split it, no new --merge-chunks
group) and 'gap_runs' ((start, end) for --gap-funcs: a gap target runs to the
next function when everything up to it decodes, else to its last terminator).
The interpreter stays as the fallback for anything still missed.

    python tools/spu_code_ptrs.py <image.bin|wrapped.elf> --base 0x4000 [--lo X] [--hi X]
"""
import argparse, os, struct, sys

HERE = os.path.dirname(os.path.abspath(__file__))
for _tk in (os.path.join(HERE, '..', 'ps3recomp', 'tools'), HERE):
    if os.path.isdir(_tk) and _tk not in sys.path:
        sys.path.insert(0, _tk)
from spu_disasm import disassemble_spu  # noqa: E402

PAD = {0x00200000, 0x40200000}                      # lnop, nop
TERM = {'br', 'bra', 'bi', 'iret', 'stop', 'stopd'}
DIRECT = {'br', 'brsl', 'brz', 'brnz', 'brhz', 'brhnz', 'bra', 'brasl'}


def enabled():
    return os.environ.get('SPU_LIFT_CODE_PTRS', '1') != '0'


def _target(ins):
    for t in ins.operands.split(','):
        t = t.strip()
        if t.lower().startswith('0x'):
            try:
                return int(t, 16)
            except ValueError:
                return None
    return None


def _is_term(i):
    if i.mnemonic not in TERM:
        return False
    if i.mnemonic == 'stop' and (i.raw & 0x3FFF) in (0x100, 0x110, 0x111):
        return False                     # lv2 resuming stops: execution continues
    return True


def code_pointers(code, base, lo=None, hi=None, bounds=(), insns=None, max_walk=4096, stops=()):
    """code: image bytes placed at LS `base`; lo/hi: code range (default the
    whole image); bounds: [(start, end)] functions already found (their starts
    count as entry-like; targets outside them must pass the gap check).
    Returns dict(ctors=set, tables=set, imms=set, all=set) of LS addresses."""
    end = base + len(code)
    lo = base if lo is None else max(lo, base)
    hi = end if hi is None else min(hi, end)
    if insns is None:
        insns = disassemble_spu(code, base)
    by = {i.addr: i for i in insns}
    starts = {s for s, _ in bounds}
    # A gap run also ends at any other function the caller adds (e.g. the
    # --full sweep's gap functions): the lifter skips a gap func whose start
    # lies inside an earlier one, so running over it would drop its entry.
    run_stops = starts | set(stops)
    spans = sorted(bounds)

    def covered(a):
        for s, e in spans:
            if s <= a < e:
                return True
            if s > a:
                break
        return False

    def valid(a):
        i = by.get(a)
        return i is not None and i.mnemonic != '.word' and i.raw != 0 and i.raw not in PAD

    gap_end = {}

    def gap_ok(t):
        """Code from t decodes cleanly to a terminator past every forward
        branch, branches stay inside the image; records the run's end."""
        a, far = t, t
        for _ in range(max_walk):
            i = by.get(a)
            if i is None or i.mnemonic == '.word' or i.raw == 0:
                return False
            if i.mnemonic in DIRECT:
                x = _target(i)
                if x is None or x & 3 or not (base <= x < end) or not valid(x):
                    return False
                if i.mnemonic not in ('brsl', 'brasl') and a < x < a + 0x1000:
                    far = max(far, x)
            if _is_term(i) and a >= far:
                # Keep the code that follows up to the next function when it
                # all decodes (fallthrough-free tails reached by other means);
                # stop at the last terminator before data otherwise.
                nxt = min([s for s in run_stops if s > t] + [hi])
                last = b = a + 4
                while b < nxt:
                    j = by.get(b)
                    if j is None or j.mnemonic == '.word' or j.raw == 0:
                        break
                    if _is_term(j):
                        last = b + 4
                    b += 4
                else:
                    last = nxt
                gap_end[t] = last
                return True
            a += 4
            if a >= hi:
                return False
        return False

    cache = {}

    def entry_like(t):
        if t in cache:
            return cache[t]
        ok = False
        if not t & 3 and lo <= t < hi and valid(t):
            if t in starts or t == lo:
                ok = True
            else:
                p = by.get(t - 4)
                ok = p is not None and (p.raw in PAD or _is_term(p))
            if ok and not covered(t):
                ok = gap_ok(t)
        cache[t] = ok
        return ok

    n = len(code) // 4
    words = struct.unpack_from('>%dI' % n, code, 0)
    ctors, tables, imms = set(), set(), set()

    # ctors/dtors: -1, p1..pn, 0
    for k in range(n):
        if words[k] != 0xFFFFFFFF:
            continue
        j = k + 1
        ps = []
        while j < n and words[j] not in (0, 0xFFFFFFFF):
            ps.append(words[j]); j += 1
        if j < n and words[j] == 0 and ps and len(ps) <= 256 and all(entry_like(p) for p in ps):
            ctors.update(ps)

    # pointer tables: >= 2 consecutive entry-like words
    k = 0
    while k < n:
        j = k
        while j < n and entry_like(words[j]):
            j += 1
        if j - k >= 2 and len(set(words[k:j])) >= 2:
            tables.update(words[k:j])
        k = max(j, k + 1)

    # immediates (the image's own code only)
    hi16 = {}
    for i in insns:
        if not (lo <= i.addr < hi):
            continue
        m = i.mnemonic
        if m == '.word':
            continue
        rt = i.raw & 0x7F
        v = None
        if m == 'ila':
            v = (i.raw >> 7) & 0x3FFFF
        elif m == 'il':
            v = (i.raw >> 7) & 0xFFFF
            v = v - 0x10000 if v & 0x8000 else v
        elif m == 'ilhu':
            hi16[rt] = ((i.raw >> 7) & 0xFFFF) << 16
            continue
        elif m == 'iohl' and rt in hi16:
            v = hi16.pop(rt) | ((i.raw >> 7) & 0xFFFF)
        if v is not None and v > 0 and entry_like(v):
            imms.add(v)

    found = ctors | tables | imms
    return dict(ctors=ctors, tables=tables, imms=imms, all=found,
                gaps={t: gap_end[t] for t in found if t in gap_end})


def discover_elf(elf_bytes, base=None, lo=None, hi=None, stops=()):
    """Code-pointer targets of a wrapped SPU ELF, split for spu_lifter.py:
    'observed' (inside detected functions: --observed-entries) and 'gaps'
    ([(start, end)] runs between them: --gap-funcs), plus the raw result.
    stops: other function starts (e.g. --full gap funcs) a gap run must not cross."""
    bounds, code, b = bounds_of_elf(elf_bytes)
    base = b if base is None else base
    r = code_pointers(code, base, lo, hi, bounds, stops=stops)
    r['observed'] = sorted(a for a in r['all'] if a not in r['gaps'])
    r['gap_runs'] = sorted(r['gaps'].items())
    starts = {s0 for s0, _ in bounds}
    r['new'] = sorted(a for a in r['all'] if a not in starts)
    return r


def gap_arg(runs):
    return ','.join('%X:%X' % g for g in runs)


def bounds_of_elf(elf_bytes):
    """detect_functions bounds of a wrapped SPU ELF (quietly)."""
    import io, contextlib
    from find_spu_functions import detect_functions, parse_elf, text_image
    with contextlib.redirect_stdout(io.StringIO()):
        funcs, (_, b, _) = detect_functions(elf_bytes)
    code, _ = text_image(elf_bytes, parse_elf(elf_bytes)['phs'])
    return list(funcs), code, b


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('image')
    ap.add_argument('--base', type=lambda x: int(x, 0), default=None)
    ap.add_argument('--lo', type=lambda x: int(x, 0), default=None)
    ap.add_argument('--hi', type=lambda x: int(x, 0), default=None)
    a = ap.parse_args()
    raw = open(a.image, 'rb').read()
    if raw[:4] == b'\x7fELF':
        bounds, code, b = bounds_of_elf(raw)
        base = b if a.base is None else a.base
    else:
        code, base, bounds = raw, a.base or 0, []
    r = code_pointers(code, base, a.lo, a.hi, bounds)
    for k in ('ctors', 'tables', 'imms'):
        print('%s: %s' % (k, ','.join('0x%X' % x for x in sorted(r[k]))))


if __name__ == '__main__':
    main()
