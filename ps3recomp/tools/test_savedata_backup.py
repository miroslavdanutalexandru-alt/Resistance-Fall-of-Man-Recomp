#!/usr/bin/env python3
"""Per-slot save backups (libs/system/savedata_backup.h).

Two checks:
  1. libs/system/tests/test_savedata_backup.c: stamp names, the prune plan
     (newest PORT_SAVE_BACKUP_KEEP per slot, .tmp leftovers removed), the
     saves_backup folder next to gamedata, and a real backup / skip-identical /
     rotation round trip in a scratch folder.
  2. cellSaveData.c's savedata_execute() calls sdbk_before_save() for saves,
     before the save body runs.

It needs a host C compiler (under CI=1 a missing one fails instead of
skipping).

Usage:
    python3 tools/test_savedata_backup.py
"""

import os
import re
import shutil
import subprocess
import sys
import tempfile

TOOLS = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(TOOLS)
SOURCE = os.path.join(ROOT, "libs", "system", "tests", "test_savedata_backup.c")
SAVEDATA = os.path.join(ROOT, "libs", "system", "cellSaveData.c")


def _cc():
    return (shutil.which(os.environ.get("CC", "cc")) or shutil.which("clang")
            or shutil.which("gcc"))


def _wiring():
    with open(SAVEDATA, encoding="utf-8", errors="replace") as f:
        src = re.sub(r"/\*.*?\*/|//[^\n]*", "", f.read(), flags=re.S)
    if not re.search(r'#include\s+"savedata_backup\.h"', src):
        return False
    m = re.search(r"static\s+s32\s+savedata_execute\s*\([^)]*\)\s*\{", src)
    if not m:
        return False
    depth, i = 1, m.end()
    while i < len(src) and depth:
        depth += {"{": 1, "}": -1}.get(src[i], 0)
        i += 1
    body = src[m.end():i]
    hook = re.search(r"\bsdbk_before_save\s*\(", body)
    run = re.search(r"\bsavedata_execute_(?:body|impl)\s*\(", body)
    return bool(hook and run and hook.start() < run.start())


def main():
    failed = 0
    ok = _wiring()
    print("%s: savedata_execute() backs the slot up before the save body"
          % ("ok  " if ok else "FAIL"))
    failed += not ok

    cc = _cc()
    if cc is None:
        if os.environ.get("CI"):
            print("FAIL: no C compiler found")
            return 1
        print("skip test_savedata_backup.c: no C compiler")
        return 1 if failed else 0
    with tempfile.TemporaryDirectory() as td:
        exe = os.path.join(td, "test_savedata_backup")
        cmd = [cc, "-std=gnu17", "-w", "-o", exe, SOURCE]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            print("FAIL: fixture did not build")
            print((r.stdout + r.stderr)[-4000:])
            return 1
        scratch = os.path.join(td, "scratch")
        os.mkdir(scratch)
        r = subprocess.run([exe, scratch], capture_output=True, text=True)
        print(r.stdout.rstrip())
        if r.returncode != 0:
            failed += 1
    print("PASS" if not failed else "FAIL")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
