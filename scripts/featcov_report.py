#!/usr/bin/env python3
"""RSX feature coverage gap report (identical in all three ports: scripts/featcov_report.py).

Reads the "[featcov]" lines the runtime tracer (ps3recomp/libs/video/rsx_featcov.c)
writes to logs/run.log, and the static scan (scripts/featcov_static_<game>.json,
from scripts/featcov_static.py), and prints what the game uses that this port's
renderer does not handle:

  1. runtime: methods / state values / shader opcodes seen in the run whose
     handler is a no-op, stored-but-ignored, unhandled, or that the shader
     decompiler has no translation for;
  2. static: method groups the game's code builds that this port leaves
     unhandled / ignored, with whether this run reached them.

Usage:
  python scripts/featcov_report.py                     # logs/run.log, this repo's game
  python scripts/featcov_report.py path/to/run.log [--game r1|r2|r3] [--static x.json]
  python scripts/featcov_report.py --all               # also list everything seen
"""
import argparse
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)

BAD = ('unhandled', 'invalid', 'stored-ignored', 'ignored', 'noop')

# names may contain spaces (RPCS3's 'NV4097_SET_TEXTURE_FORMAT + 0x20'): the status is the last token
RE_M = re.compile(r'\[featcov\] m 0x([0-9A-Fa-f]+) (.+?) (\w[\w-]*)\s*$')
RE_V = re.compile(r'\[featcov\] v (\w+) 0x([0-9A-Fa-f]+) (\w[\w-]*)(?: via=0x([0-9A-Fa-f]+))?')
RE_CAP = re.compile(r'\[featcov\] cap (\w+) (\d+)')
RE_DUMP = re.compile(r'\[featcov\] dump #(\d+) (\S+) t=(\d+)s port=(\w+)')

PRIM = {1: 'POINTS', 2: 'LINES', 3: 'LINE_LOOP', 4: 'LINE_STRIP', 5: 'TRIANGLES', 6: 'TRIANGLE_STRIP',
        7: 'TRIANGLE_FAN', 8: 'QUADS', 9: 'QUAD_STRIP', 10: 'POLYGON'}
GL_ENUM = {0x2601: 'LINEAR', 0x0800: 'EXP', 0x0801: 'EXP2', 0x0802: 'EXP_ABS', 0x0803: 'EXP2_ABS', 0x0804: 'LINEAR_ABS',
           0x8006: 'FUNC_ADD', 0x8007: 'MIN', 0x8008: 'MAX', 0x800A: 'FUNC_SUBTRACT',
           0x800B: 'FUNC_REVERSE_SUBTRACT', 0xF005: 'FUNC_REVERSE_SUBTRACT_SIGNED',
           0xF006: 'FUNC_ADD_SIGNED', 0xF007: 'FUNC_REVERSE_ADD_SIGNED',
           0x0200: 'NEVER', 0x0201: 'LESS', 0x0202: 'EQUAL', 0x0203: 'LEQUAL', 0x0204: 'GREATER',
           0x0205: 'NOTEQUAL', 0x0206: 'GEQUAL', 0x0207: 'ALWAYS',
           0x1B00: 'POINT', 0x1B01: 'LINE', 0x1B02: 'FILL', 0x1D00: 'FLAT', 0x1D01: 'SMOOTH',
           0x0000: 'ZERO', 0x0001: 'ONE', 0x0300: 'SRC_COLOR', 0x0301: 'ONE_MINUS_SRC_COLOR',
           0x0302: 'SRC_ALPHA', 0x0303: 'ONE_MINUS_SRC_ALPHA', 0x0304: 'DST_ALPHA',
           0x0305: 'ONE_MINUS_DST_ALPHA', 0x0306: 'DST_COLOR', 0x0307: 'ONE_MINUS_DST_COLOR',
           0x0308: 'SRC_ALPHA_SATURATE', 0x8001: 'CONSTANT_COLOR', 0x8002: 'ONE_MINUS_CONSTANT_COLOR',
           0x8003: 'CONSTANT_ALPHA', 0x8004: 'ONE_MINUS_CONSTANT_ALPHA'}
LOGIC_OP = {0x1500 + i: n for i, n in enumerate(
    'CLEAR AND AND_REVERSE COPY AND_INVERTED NOOP XOR OR NOR EQUIV INVERT OR_REVERSE COPY_INVERTED '
    'OR_INVERTED NAND SET'.split())}
FP_OPS = ('NOP MOV MUL ADD MAD DP3 DP4 DST MIN MAX SLT SGE SLE SGT SNE SEQ FRC FLR KIL PK4 UP4 DDX DDY TEX '
          'TXP TXD RCP RSQ EX2 LG2 LIT LRP STR SFL COS SIN PK2 UP2 POW PKB UPB PK16 UP16 BEM PKG UPG DP2A '
          'TXL NULL TXB NULL TEXBEM TXPBEM BEMLUM REFL TIMESWTEX DP2 NRM DIV DIVSQ LIF FENCT FENCB NULL '
          'BRK CAL IFE LOOP REP RET').split()
FP_BRANCH = {0x40 + i: n for i, n in enumerate('BRK CAL IFE LOOP REP RET OR16_LO OR16_HI'.split())}
VP_VEC = 'NOP MOV MUL ADD MAD DP3 DPH DP4 DST MIN MAX SLT SGE ARL FRC FLR SEQ SFL SGT SLE SNE STR SSG NULL NULL TXL'.split()
VP_SCA = 'NOP MOV RCP RCC RSQ EXP LOG LIT BRA BRI CAL CLI RET LG2 EX2 SIN COS BRB CLB PSH POP'.split()
TEXDIM = {1: '1D', 2: '2D', 3: '3D', 4: 'CUBE'}


def value_name(kind, v):
    if kind == 'prim':
        return PRIM.get(v, '')
    if kind in ('fog_mode', 'blend_eq', 'blend_sfactor', 'blend_dfactor', 'alpha_func', 'poly_mode_front',
                'poly_mode_back', 'shade_mode', 'tex_zfunc'):
        return GL_ENUM.get(v, '')
    if kind == 'logic_op':
        return LOGIC_OP.get(v, '')
    if kind in ('fp_op', 'fp_deriv'):
        if v in FP_BRANCH:
            return FP_BRANCH[v]
        return FP_OPS[v] if v < 0x40 and v < len(FP_OPS) else ''
    if kind == 'vp_vec':
        return VP_VEC[v] if v < len(VP_VEC) else ''
    if kind == 'vp_sca':
        return VP_SCA[v] if v < len(VP_SCA) else ''
    if kind in ('fp_texdim', 'vp_texdim'):
        return TEXDIM.get(v, '')
    if kind == 'tex_dim':
        return f'{v & 15}D' + (' cube' if v >> 4 else '')
    return ''


def game_of(repo):
    b = os.path.basename(os.path.normpath(repo)).lower()
    return 'r2' if '2port' in b else 'r3' if '3port' in b else 'r1'


def parse_log(lines):
    """-> dict(methods={off: (name, st)}, values={(kind, v): (st, via)}, caps=set, dumps=n, port=str)"""
    run = {'methods': {}, 'values': {}, 'caps': set(), 'dumps': 0, 'port': None}
    for ln in lines:
        if '[featcov]' not in ln:
            continue
        m = RE_M.search(ln)
        if m:
            run['methods'][int(m.group(1), 16)] = (m.group(2), m.group(3))
            continue
        m = RE_V.search(ln)
        if m:
            key = (m.group(1), int(m.group(2), 16))
            st, via = m.group(3), int(m.group(4), 16) if m.group(4) else None
            old = run['values'].get(key)
            if not old or (st != 'real'):          # a later "unhandled" report overrides
                run['values'][key] = (st, via)
            continue
        m = RE_CAP.search(ln)
        if m:
            run['caps'].add(m.group(1))
            continue
        m = RE_DUMP.search(ln)
        if m:
            run['dumps'] += 1
            run['port'] = m.group(4)
    return run


def base_name(name):
    """Group name, as featcov_static.py's group_name: X[3] / 'X + 0x20' / 'X + 4' -> X[]."""
    return re.sub(r'\s*\+\s*(0x[0-9A-Fa-f]+|\d+)$', '[]', re.sub(r'\[\d+\]', '[]', name.strip()))


def report(run, static, game, show_all=False):
    port = 'r1' if game == 'r1' else game
    L = []
    nm, nv = len(run['methods']), len(run['values'])
    L.append(f'== featcov report: {game.upper()} (port={run["port"] or "?"}) - {run["dumps"]} dumps, '
             f'{nm} methods, {nv} state/shader values seen at runtime')
    if not run['dumps']:
        L.append('   (no [featcov] dump lines in the log: tracer off (RSX_FEATCOV=0), or the run ended before the first dump)')

    # 1. runtime gaps
    L.append('')
    L.append('-- runtime: used by the game, not (fully) handled --')
    groups = {}
    for off, (name, st) in sorted(run['methods'].items()):
        if st in BAD:
            g = groups.setdefault((base_name(name), st), [])
            g.append(off)
    for (name, st), offs in sorted(groups.items(), key=lambda x: x[1][0]):
        more = f' (+{len(offs) - 1} regs)' if len(offs) > 1 else ''
        L.append(f'  m 0x{offs[0]:04X} {name:<44} {st}{more}')
    vbad = [(k, v) for k, v in sorted(run['values'].items()) if v[0] in BAD]
    for (kind, val), (st, via) in vbad:
        nmv = value_name(kind, val)
        L.append(f'  v {kind:<16} 0x{val:X}{(" " + nmv) if nmv else ""}  {st}'
                 + (f' (via 0x{via:04X})' if via is not None else ''))
    if not groups and not vbad:
        L.append('  none')
    if run['caps']:
        L.append(f'  (value sets capped at the tracer limit: {", ".join(sorted(run["caps"]))})')

    # 2. static gaps
    L.append('')
    if static is None:
        L.append(f'-- static: no featcov_static_{game}.json (run scripts/featcov_static.py) --')
    else:
        seen_bases = {base_name(n) for n, _ in run['methods'].values()}
        bad = [g for g in static['groups'] if g.get(port) and g[port][0] in BAD]
        L.append(f'-- static: the game code builds these, this port does not (fully) handle them ({len(bad)}) --')
        for g in bad:
            seen = 'SEEN' if g['name'] in seen_bases else 'not seen' if run['dumps'] else '-'
            L.append(f'  0x{g["off"]:04X} {g["name"]:<44} {g[port][0]:<15} run:{seen:<8} {g["feature"]}  [{g[port][1]}]')
        if not bad:
            L.append('  none')
        if run['dumps']:
            static_bases = {g['name'] for g in static['groups']}
            extra = sorted({base_name(n) for n, _ in run['methods'].values()} - static_bases)
            L.append('')
            L.append(f'-- runtime methods the static scan did not find ({len(extra)}; built by code the scan cannot see) --')
            L.append('  ' + (', '.join(extra) if extra else 'none'))

    if show_all:
        L.append('')
        L.append('-- everything seen --')
        for off, (name, st) in sorted(run['methods'].items()):
            L.append(f'  m 0x{off:04X} {name} {st}')
        for (kind, val), (st, via) in sorted(run['values'].items()):
            nmv = value_name(kind, val)
            L.append(f'  v {kind} 0x{val:X}{(" " + nmv) if nmv else ""} {st}')
    return '\n'.join(L)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('log', nargs='?', default=os.path.join(REPO, 'logs', 'run.log'))
    ap.add_argument('--game', choices=['r1', 'r2', 'r3'], default=game_of(REPO))
    ap.add_argument('--static', help='featcov_static_<game>.json (default: scripts/)')
    ap.add_argument('--all', action='store_true', help='also list everything seen')
    a = ap.parse_args(argv)
    sp = a.static or os.path.join(HERE, f'featcov_static_{a.game}.json')
    static = None
    if os.path.exists(sp):
        with open(sp, encoding='utf-8') as f:
            static = json.load(f)
    if not os.path.exists(a.log):
        print(f'no log at {a.log}', file=sys.stderr)
        run = parse_log([])
    else:
        with open(a.log, encoding='utf-8', errors='replace') as f:
            run = parse_log(f)
    print(report(run, static, a.game, a.all))
    return 0


if __name__ == '__main__':
    sys.exit(main())
