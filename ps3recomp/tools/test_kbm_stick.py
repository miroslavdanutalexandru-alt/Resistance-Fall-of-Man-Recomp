#!/usr/bin/env python3
"""kbm_stick.h: mouse -> right stick with deadzone-aware, sub-step carried mapping
(ps3recomp/tests/test_kbm_stick.c; vehicle / turret aiming)."""
import os
import shutil
import subprocess
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]


def main():
    src = REPO / 'ps3recomp' / 'tests' / 'test_kbm_stick.c'
    cc = os.environ.get('CC') or shutil.which('clang') or shutil.which('gcc') or shutil.which('cc')
    if not cc:
        print('SKIP: no C compiler (set CC)')
        return
    out = REPO / 'scratch' / 'host_tests'
    out.mkdir(parents=True, exist_ok=True)
    exe = out / ('test_kbm_stick' + ('.exe' if os.name == 'nt' else ''))
    subprocess.run([cc, '-O1', '-Wall', str(src), '-o', str(exe)] + ([] if os.name == 'nt' else ['-lm']),
                   check=True, timeout=120)
    subprocess.run([str(exe)], check=True, timeout=30)


if __name__ == '__main__':
    main()
