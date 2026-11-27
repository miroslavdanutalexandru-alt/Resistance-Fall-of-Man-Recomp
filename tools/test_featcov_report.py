#!/usr/bin/env python3
"""scripts/featcov_report.py: [featcov] run.log lines + static table -> gap list."""
import os
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, 'scripts'))
import featcov_report as fr  # noqa: E402

LOG = """\
boot...
[featcov] on (dump every 60s and at exit; RSX_FEATCOV=0 disables)
[featcov] dump #1 periodic t=60s port=r1 new_methods=4/4 new_values=4/4
[featcov] m 0x1808 NV4097_SET_BEGIN_END real
[featcov] m 0x08CC NV4097_SET_FOG_MODE unhandled
[featcov] m 0x0220 NV4097_SET_SURFACE_PITCH_A ignored
[featcov] m 0x1A04 NV4097_SET_TEXTURE_FORMAT[0] real
[featcov] v prim 0x5 real via=0x1808
[featcov] v fog_mode 0x2601 unhandled via=0x08CC
[featcov] v fp_op 0x15 unhandled
[featcov] v fp_op 0x1 real
[featcov] dump #2 exit t=75s port=r1 new_methods=1/5 new_values=0/4
[featcov] m 0x1A24 NV4097_SET_TEXTURE_FORMAT[1] real
[featcov] cap tex_fmt 96 values
"""

STATIC = {'game': 'r1', 'groups': [
    {'off': 0x8CC, 'name': 'NV4097_SET_FOG_MODE', 'feature': 'fog', 'static': {'ppu_const': 2},
     'r1': ['unhandled', 'no case'], 'r2': ['consumed', 'x'], 'r3': ['consumed', 'x']},
    {'off': 0x380, 'name': 'NV4097_SET_DEPTH_BOUNDS_TEST_ENABLE', 'feature': 'depth', 'static': {'ppu_const': 1},
     'r1': ['unhandled', 'no case'], 'r2': ['consumed', 'x'], 'r3': ['consumed', 'x']},
    {'off': 0x1A04, 'name': 'NV4097_SET_TEXTURE_FORMAT[]', 'feature': 'texture', 'static': {'ppu_const': 9},
     'r1': ['handled', 'rsx_commands.c:1'], 'r2': ['consumed', 'x'], 'r3': ['consumed', 'x']},
]}


def test_spaced_names():
    # RPCS3 get_method_name names contain spaces; the status is the last token
    run = fr.parse_log([
        '[featcov] dump #1 exit t=3s port=rpcs3 new_methods=4/4 new_values=0/0',
        '[featcov] m 0x1A24 NV4097_SET_TEXTURE_FORMAT + 0x20 real',
        '[featcov] m 0x03C4 NV4097_SET_ANISO_SPREAD + 4 ignored',
        '[featcov] m 0xE944 DRIVER_QUEUE + 0x4 real\r',
        '[featcov] m 0x1A04 NV4097_SET_TEXTURE_FORMAT unhandled',
    ])
    assert run['methods'][0x1A24] == ('NV4097_SET_TEXTURE_FORMAT + 0x20', 'real'), run
    assert run['methods'][0x03C4] == ('NV4097_SET_ANISO_SPREAD + 4', 'ignored')
    assert run['methods'][0xE944] == ('DRIVER_QUEUE + 0x4', 'real')
    assert fr.base_name('NV4097_SET_TEXTURE_FORMAT + 0x20') == 'NV4097_SET_TEXTURE_FORMAT[]'
    assert fr.base_name('NV4097_SET_ANISO_SPREAD + 4') == 'NV4097_SET_ANISO_SPREAD[]'
    assert fr.base_name('NV4097_SET_TEX_COORD_CONTROL[3]') == 'NV4097_SET_TEX_COORD_CONTROL[]'
    assert fr.base_name('NV4097_SET_TEXTURE_FORMAT') == 'NV4097_SET_TEXTURE_FORMAT'
    # runtime "X + 0x20" lines up with the static "X[]" group: SEEN, and not reported as static-missed
    static = {'game': 'r3', 'groups': [
        {'off': 0x1A24, 'name': 'NV4097_SET_TEXTURE_FORMAT[]', 'feature': 'texture', 'static': {'ppu_const': 1},
         'r1': ['handled', 'x'], 'r2': ['stored-ignored', 'x'], 'r3': ['stored-ignored', 'x']},
        {'off': 0x03C4, 'name': 'NV4097_SET_ANISO_SPREAD[]', 'feature': 'texture', 'static': {'ppu_const': 1},
         'r1': ['handled', 'x'], 'r2': ['consumed', 'x'], 'r3': ['consumed', 'x']},
    ]}
    out = fr.report(run, static, 'r3')
    rt, st = out.split('-- static', 1)
    assert 'NV4097_SET_ANISO_SPREAD[]' in rt and 'ignored' in rt, out
    tf = [l for l in st.splitlines() if 'TEXTURE_FORMAT[]' in l][0]
    assert 'run:SEEN' in tf, tf
    missed = st.split('-- runtime methods')[1]
    assert 'ANISO' not in missed and 'TEXTURE_FORMAT[]' not in missed, missed


def main():
    run = fr.parse_log(LOG.splitlines())
    assert run['dumps'] == 2 and run['port'] == 'r1', run
    assert len(run['methods']) == 5
    assert run['methods'][0x8CC] == ('NV4097_SET_FOG_MODE', 'unhandled')
    assert run['values'][('fog_mode', 0x2601)] == ('unhandled', 0x8CC)
    assert run['values'][('fp_op', 0x15)] == ('unhandled', None)
    assert run['caps'] == {'tex_fmt'}

    out = fr.report(run, STATIC, 'r1')
    print(out)
    rt = out.split('-- static')[0]
    assert 'NV4097_SET_FOG_MODE' in rt and 'unhandled' in rt
    assert 'NV4097_SET_SURFACE_PITCH_A' in rt and 'ignored' in rt
    assert 'fog_mode' in rt and 'LINEAR' in rt
    assert 'fp_op' in rt and 'DDX' in rt
    assert 'NV4097_SET_BEGIN_END' not in rt and 'TEXTURE_FORMAT' not in rt
    st = out.split('-- static')[1]
    fog = [l for l in st.splitlines() if 'FOG_MODE' in l][0]
    db = [l for l in st.splitlines() if 'DEPTH_BOUNDS' in l][0]
    assert 'run:SEEN' in fog and 'run:not seen' in db, (fog, db)
    assert 'TEXTURE_FORMAT' not in st.split('-- runtime methods')[0]
    assert 'NV4097_SET_BEGIN_END' in st.split('-- runtime methods')[1]   # static scan missed it

    # same port, RPCS3 status words; nothing traced -> explicit note, static list still printed
    out = fr.report(fr.parse_log(['no featcov here']), STATIC, 'r1')
    assert 'no [featcov] dump lines' in out and 'DEPTH_BOUNDS' in out

    # the shipped static table for this repo loads and reports
    game = fr.game_of(REPO)
    sp = os.path.join(REPO, 'scripts', f'featcov_static_{game}.json')
    if os.path.exists(sp):
        assert fr.main([os.path.join(REPO, 'no_such_run.log'), '--static', sp]) == 0
    test_spaced_names()
    print('test_featcov_report: ok')
    return 0


if __name__ == '__main__':
    sys.exit(main())
