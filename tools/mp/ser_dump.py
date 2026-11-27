#!/usr/bin/env python3
"""Print the wire layout of a Medius message from its lifted (de)serializer.

The +8 entry of each Medius table slot (logs/mp_sess_dump.log) is a function
that walks the message struct with a fixed set of primitives:

    func_0065C8C4(buf, ptr, n)   raw n bytes
    func_0065CFEC(buf, n)        n pad bytes (wire only)
    func_0065CC18(buf, ptr)      u32 LE
    func_0065C9E8(buf, ptr)      u8
    func_0065C6F4(buf, ptr)      NetAddress {u32 type, char[16], u32 port}

This follows the straight-line register arithmetic (base register + offset,
r5 = size) well enough for the generated (de)serializers, which have no
branches except count-driven ones; anything it can't follow is printed as '?'.

    python tools/mp/ser_dump.py 4 0x37 [4 0x38 ...]     by class/id
    python tools/mp/ser_dump.py 0x00622830               by function
"""
import os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
REC = os.path.join(ROOT, 'recompiled')
DUMP = os.path.join(ROOT, 'logs', 'mp_sess_dump.log')
PRIMS = {'0065C8C4': 'raw', '0065CFEC': 'pad', '0065CC18': 'u32', '0065C9E8': 'u8',
         '0065C6F4': 'netaddr', '0065CAFC': 'u16?', '0065CD34': 'u64?', '0065CE80': 'f32?'}
SIZES = {'u32': 4, 'u8': 1, 'netaddr': 24, 'u16?': 2, 'u64?': 8, 'f32?': 4}


def find_func(addr):
    pat = re.compile(r'^void func_%08X\(ppu_context\* ctx\) \{' % addr)
    for fn in sorted(os.listdir(REC)):
        if not fn.endswith('.cpp'):
            continue
        with open(os.path.join(REC, fn), encoding='utf-8', errors='replace') as f:
            lines = f.readlines()
        for i, l in enumerate(lines):
            if pat.match(l):
                body = []
                for l2 in lines[i + 1:]:
                    if l2.startswith('}'):
                        break
                    body.append(l2.strip())
                return body
    return None


def table(cls, mid):
    pat = re.compile(r'class=%d id=0x%02X .*\+0=0x\w+\(code (0x\w+)\).*\+8=0x\w+\(code (0x\w+)\)' % (cls, mid))
    for l in open(DUMP, encoding='utf-8', errors='replace'):
        m = pat.search(l)
        if m:
            return int(m.group(1), 16), int(m.group(2), 16)
    return None, None


def walk(addr):
    body = find_func(addr)
    if body is None:
        print(f'  func_{addr:08X} not found'); return
    reg = {}           # reg -> ('base', off) | ('imm', v)
    base_reg = None
    wire = 0
    out = []
    for l in body:
        m = re.match(r'ctx->gpr\[(\d+)\] = vm_read32\(ctx->gpr\[4\] \+ 0x0\);', l)
        if m and base_reg is None:
            base_reg = int(m.group(1)); reg[base_reg] = ('base', 0); continue
        m = re.match(r'ctx->gpr\[(\d+)\] = ctx->gpr\[(\d+)\] \| ctx->gpr\[\2\];', l)
        if m:
            src = int(m.group(2))
            if src in reg: reg[int(m.group(1))] = reg[src]
            elif base_reg is None and src == 4: pass
            else: reg.pop(int(m.group(1)), None)
            continue
        m = re.match(r'ctx->gpr\[(\d+)\] = ctx->gpr\[(\d+)\] \+ \(int64_t\)\((-?0x[0-9A-F]+|-?\d+)\);', l)
        if m:
            src = int(m.group(2))
            if src in reg and reg[src][0] == 'base':
                reg[int(m.group(1))] = ('base', reg[src][1] + int(m.group(3), 0))
            else:
                reg.pop(int(m.group(1)), None)
            continue
        m = re.match(r'ctx->gpr\[(\d+)\] = ppc_rldicl\(ctx->gpr\[(\d+)\], 0, 32\);', l)
        if m:
            src = int(m.group(2))
            if src in reg: reg[int(m.group(1))] = reg[src]
            continue
        m = re.match(r'ctx->gpr\[(\d+)\] = \(int64_t\)\(int32_t\)\((-?0x[0-9A-F]+|-?\d+)\);', l)
        if m:
            reg[int(m.group(1))] = ('imm', int(m.group(2), 0)); continue
        m = re.search(r'func_([0-9A-F]{8})\(ctx\)', l)
        if m:
            f = m.group(1)
            kind = PRIMS.get(f)
            r4, r5 = reg.get(4), reg.get(5)
            if kind == 'raw':
                n = r5[1] if r5 and r5[0] == 'imm' else '?'
                off = r4[1] if r4 and r4[0] == 'base' else '?'
                out.append((wire, f'raw[{n}]', off)); wire = wire + n if isinstance(n, int) and isinstance(wire, int) else '?'
            elif kind == 'pad':
                n = r4[1] if r4 and r4[0] == 'imm' else '?'
                out.append((wire, f'pad {n}', '-')); wire = wire + n if isinstance(n, int) and isinstance(wire, int) else '?'
            elif kind:
                off = r4[1] if r4 and r4[0] == 'base' else '?'
                out.append((wire, kind, off)); wire = wire + SIZES[kind] if isinstance(wire, int) else '?'
            else:
                out.append((wire, f'call func_{f}', '?'))
            for r in (3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 0):
                if r != base_reg and r not in (28, 29, 30, 31):
                    reg.pop(r, None)
            continue
        if 'goto' in l or l.startswith('loc_'):
            out.append((wire, '(branch: ' + l[:60] + ')', ''))
    for w, k, o in out:
        so = f'@0x{o:X}' if isinstance(o, int) else str(o)
        sw = f'{w:4d}' if isinstance(w, int) else '   ?'
        print(f'  wire {sw}  {k:14s} struct {so}')
    print(f'  total wire bytes: {wire}')


def main(a):
    i = 0
    while i < len(a):
        if i + 1 < len(a) and not a[i].lower().startswith('0x00'):
            cls, mid = int(a[i], 0), int(a[i + 1], 0)
            h, s = table(cls, mid)
            print(f'class {cls} id 0x{mid:02X}: handler {h and hex(h)}  ser {s and hex(s)}')
            if s:
                walk(s)
            i += 2
        else:
            addr = int(a[i], 16); print(f'func_{addr:08X}'); walk(addr); i += 1


if __name__ == '__main__':
    main(sys.argv[1:])
