#!/usr/bin/env python3
"""Resistance: Fall of Man port -- recover the unhooked lift from an edited tree.

  mirror <edited_dir> <hooks.json> <out_dir>   reverse-apply the hooks

`rfom_hooks.py extract` needs two trees: the edited one and a *fresh* lift of
the same game. A full re-lift is minutes of work per edit, and the regenerated
trees kept around (build_regen/eboot) are snapshots at some earlier revision --
they already carry some hooks, so neither `extract` nor `apply` round-trips
against them.

This script rebuilds the missing input: it copies <edited_dir>, removes the
preamble lines and modifies the header, then undoes every hunk in <hooks.json>
(bottom-up, matched by the removed lines plus one line of context, same search
`apply_hunks` uses). The result equals the fresh lift for those functions, so

    python tools/rfom_fresh_mirror.py mirror recompiled tools/hooks/eboot.json /tmp/fresh
    python tools/rfom_hooks.py extract recompiled /tmp/fresh tools/hooks/eboot.json

records the edits again from scratch. Verify with

    python tools/rfom_hooks.py apply /tmp/fresh /tmp/fresh_copy_apply

which must reproduce <edited_dir> exactly (`diff -r`).

Hunks whose added lines are not found are an error, not a warning: a silent
skip would write a hooks.json that no longer describes the tree.
"""
import glob
import json
import os
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from rfom_hooks import rd, apply_hunks, split_chunk, FR   # noqa: E402


def reverse(hs):
    """Turn hunks into their inverse. `at` is the position in the hooked body
    and is only a starting guess for apply_hunks' +-400 search, so reusing the
    recorded position is harmless."""
    out = []
    for h in hs:
        inv = {'at': h['at'], 'ctx': h['ctx'], 'rm': h['add'], 'add': h['rm']}
        inv['at'] = h['at'] + max(0, len(h['add']) - len(h['rm']))
        out.append(inv)
    return out


def main(argv):
    if len(argv) != 5 or argv[1] != 'mirror':
        print(__doc__)
        return 2
    ed, hp, out = argv[2], argv[3], argv[4]
    if os.path.abspath(out) == os.path.abspath(ed):
        raise SystemExit('refusing to mirror onto the edited tree itself')
    H = json.load(open(hp))
    if os.path.isdir(out):
        shutil.rmtree(out)
    shutil.copytree(ed, out)

    todo = dict(H['funcs'])
    for f in sorted(glob.glob(os.path.join(out, 'ppu_recomp_0*.cpp'))):
        L = rd(f)
        # preamble lines sit right after the include, one copy per chunk
        if H['preamble']:
            for k, l in enumerate(L):
                if l.startswith('#include "ppu_recomp.h"'):
                    j = k + 1
                    while j < len(L) and L[j].startswith('/*') and L[j].endswith('*/'):
                        j += 1
                    # remove them one at a time, in recorded order: a hunk may
                    # have added only some of them to a tree that already had
                    # the rest, so match the sequence rather than the whole list
                    for want in H['preamble']:
                        if j < len(L) and L[j].rstrip('\r') == want:
                            del L[j]
                    break
        k = 0
        while k < len(L):
            m = FR.match(L[k])
            if m and m.group(1) in todo:
                e = k + 1
                while e < len(L) and not FR.match(L[e]):
                    e += 1
                body = apply_hunks(L[k:e], reverse(todo.pop(m.group(1))), m.group(1))
                L[k:e] = body
                k += len(body)
                continue
            k += 1
        open(f, 'w', encoding='utf-8', errors='surrogateescape').write('\n'.join(L))
    if todo:
        raise SystemExit('functions not found: %s' % ', '.join(sorted(todo)[:10]))
    if H.get('header'):
        hp2 = os.path.join(out, 'ppu_recomp.h')
        txt = '\n'.join(apply_hunks(rd(hp2), reverse(H['header']), 'ppu_recomp.h'))
        open(hp2, 'w', encoding='utf-8', errors='surrogateescape').write(txt)
    print('mirrored %s -> %s from %s' % (ed, out, hp))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
