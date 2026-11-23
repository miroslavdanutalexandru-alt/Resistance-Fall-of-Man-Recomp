#!/usr/bin/env python3
"""rfom_movie_pace.h: 30 fps movies keep vsync on 60/120/240 Hz and without tearing presents,
other refresh rates with tearing go to VRR (project/tests/movie_pace_test.c)."""
import os
import shutil
import subprocess
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]


def main():
    src = REPO / 'project' / 'tests' / 'movie_pace_test.c'
    cc = os.environ.get('CC') or shutil.which('clang') or shutil.which('gcc') or shutil.which('cc')
    if not cc:
        print('SKIP: no C compiler (set CC)')
        return
    out = REPO / 'scratch' / 'host_tests'
    out.mkdir(parents=True, exist_ok=True)
    exe = out / ('movie_pace_test' + ('.exe' if os.name == 'nt' else ''))
    subprocess.run([cc, '-O1', '-Wall', '-I' + str(REPO / 'project'), str(src), '-o', str(exe)] + ([] if os.name == 'nt' else ['-lm']),
                   check=True, timeout=120)
    subprocess.run([str(exe)], check=True, timeout=30)


if __name__ == '__main__':
    main()
