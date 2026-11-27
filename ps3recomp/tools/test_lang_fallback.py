#!/usr/bin/env python3
"""English voice-over for a system language the disc has no speech in.

Windows set to English (UK) gives PS3 language 18. Resistance 2/3 then show UK
text but ask their Bink cutscenes for voice group 190, which the disc does not
have, and the cutscenes play without dialogue. This builds
libs/system/tests/test_lang_fallback.c, which checks the Bink track remap
(20.. -> 190..), the disc-wide dub check that keeps a dubbed language from
getting English in a movie that lacks it, the loose speech-file name fallback
and the env switches in
libs/system/lang_fallback.h.

It needs a host C compiler (under CI=1 a missing one fails instead of
skipping).

Usage:
    python3 tools/test_lang_fallback.py
"""

import os
import shutil
import subprocess
import sys
import tempfile

TOOLS = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(TOOLS)
SOURCES = [os.path.join(ROOT, "libs", "system", "tests", "test_lang_fallback.c")]


def _cc():
    return (shutil.which(os.environ.get("CC", "cc")) or shutil.which("clang")
            or shutil.which("gcc"))


def main():
    cc = _cc()
    if cc is None:
        if os.environ.get("CI"):
            print("FAIL: no C compiler found")
            return 1
        print("skip test_lang_fallback: no C compiler")
        return 0
    with tempfile.TemporaryDirectory() as td:
        exe = os.path.join(td, "test_lang_fallback" + (".exe" if sys.platform == "win32" else ""))
        cmd = [cc, "-std=gnu17", "-Wall", "-Wextra", "-Werror", "-Wno-deprecated-declarations",
               "-o", exe] + SOURCES
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            print("FAIL: fixture did not build")
            print((r.stdout + r.stderr)[-4000:])
            return 1
        env = {k: v for k, v in os.environ.items()
               if k not in ("PS3_LANG_RESOLVED", "PS3_LANG_VOICE_FALLBACK")}
        r = subprocess.run([exe], capture_output=True, text=True, env=env, cwd=td)
        sys.stdout.write(r.stdout + r.stderr)
        return r.returncode


if __name__ == "__main__":
    sys.exit(main())
