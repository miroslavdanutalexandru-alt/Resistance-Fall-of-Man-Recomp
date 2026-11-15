#!/usr/bin/env python3
"""Find the lifted functions that load a given string (v184).

Game code reaches strings two ways: `rX = [r2 + off]` (TOC slot holding the
string address) or `rB = [r2 + off]; rX = [rB + off2]` (a module's pointer
table reached through a TOC base). This resolves both against game/EBOOT.elf
for every function in recompiled/*.cpp and reports the functions whose loads
resolve to an address holding the wanted text.

    python tools/mp/find_string_refs.py "JoinGame failed" "pResp == NULL"
"""
import os, re, struct, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
ELF = open(os.path.join(ROOT, 'game', 'EBOOT.elf'), 'rb').read()
SEGS = ((0, 0x10000, 0x714c14), (0x720000, 0x730000, 0x584f0),
        (0x780000, 0x10000000, 0x41c50), (0x7d0000, 0x10050000, 0x144900))
TOC = 0x78ACB8


def rd(va, n=4):
    for off, v, fs in SEGS:
        if v <= va < v + fs:
            return ELF[off + va - v:off + va - v + n]
    return None


def u32(va):
    b = rd(va)
    return struct.unpack('>I', b)[0] if b and len(b) == 4 else None


def text_at(va):
    b = rd(va, 120)
    return b.split(b'\0')[0] if b else b''


def main(needles):
    needles = [n.encode() for n in needles]
    fn_re = re.compile(r'^void (func_[0-9A-F]{8})\(')
    toc_re = re.compile(r'ctx->gpr\[(\d+)\] = vm_read32\(ctx->gpr\[2\] \+ (-?0x[0-9A-F]+)\);')
    rel_re = re.compile(r'ctx->gpr\[(\d+)\] = vm_read32\(ctx->gpr\[(\d+)\] \+ (-?0x[0-9A-F]+)\);')
    for fn in sorted(os.listdir(os.path.join(ROOT, 'recompiled'))):
        if not fn.endswith('.cpp'):
            continue
        cur, base = None, {}
        with open(os.path.join(ROOT, 'recompiled', fn), encoding='utf-8', errors='replace') as f:
            for ln, line in enumerate(f, 1):
                m = fn_re.match(line)
                if m:
                    cur, base = m.group(1), {}
                    continue
                m = toc_re.search(line)
                if m:
                    v = u32(TOC + int(m.group(2), 16))
                    if v is not None:
                        base[int(m.group(1))] = v
                        t = text_at(v)
                        if any(n in t for n in needles):
                            print(f'{cur} {fn}:{ln}  [r2{m.group(2)}] -> {v:#x} {t[:70]!r}')
                    continue
                m = rel_re.search(line)
                if m and int(m.group(2)) in base:
                    slot = base[int(m.group(2))] + int(m.group(3), 16)
                    v = u32(slot)
                    if v is not None:
                        t = text_at(v)
                        if any(n in t for n in needles):
                            print(f'{cur} {fn}:{ln}  [[r2..]{m.group(3)}] -> {v:#x} {t[:70]!r}')


if __name__ == '__main__':
    main(sys.argv[1:])
