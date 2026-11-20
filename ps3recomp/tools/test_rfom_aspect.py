#!/usr/bin/env python3
"""rfom_aspect.h: ultrawide present stretch follows the target aspect, not the camera (ADS),
with hysteresis (project/tests/aspect_test.c)."""
import os
import shutil
import subprocess
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]


def main():
    src = REPO / 'project' / 'tests' / 'aspect_test.c'
    cc = os.environ.get('CC') or shutil.which('clang') or shutil.which('gcc') or shutil.which('cc')
    if not cc:
        print('SKIP: no C compiler (set CC)')
        return
    out = REPO / 'scratch' / 'host_tests'
    out.mkdir(parents=True, exist_ok=True)
    exe = out / ('aspect_test' + ('.exe' if os.name == 'nt' else ''))
    subprocess.run([cc, '-O1', '-Wall', '-I' + str(REPO / 'project'), str(src), '-o', str(exe)] + ([] if os.name == 'nt' else ['-lm']),
                   check=True, timeout=120)
    subprocess.run([str(exe)], check=True, timeout=30)


if __name__ == '__main__':
    main()
