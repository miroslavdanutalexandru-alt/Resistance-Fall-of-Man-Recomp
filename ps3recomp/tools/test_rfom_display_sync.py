#!/usr/bin/env python3
"""port_display_sync.h: VRR verdict from vblank gaps, the auto-mode hysteresis and the fixed-refresh
pacing plans per refresh rate (project/tests/display_sync_test.c)."""
import os
import shutil
import subprocess
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]


def main():
    src = REPO / 'project' / 'tests' / 'display_sync_test.c'
    cc = os.environ.get('CC') or shutil.which('clang') or shutil.which('gcc') or shutil.which('cc')
    if not cc:
        print('SKIP: no C compiler (set CC)')
        return
    out = REPO / 'scratch' / 'host_tests'
    out.mkdir(parents=True, exist_ok=True)
    exe = out / ('display_sync_test' + ('.exe' if os.name == 'nt' else ''))
    subprocess.run([cc, '-O1', '-Wall', '-I' + str(REPO / 'project'), str(src), '-o', str(exe)] + ([] if os.name == 'nt' else ['-lm']),
                   check=True, timeout=120)
    subprocess.run([str(exe)], check=True, timeout=30)


if __name__ == '__main__':
    main()
