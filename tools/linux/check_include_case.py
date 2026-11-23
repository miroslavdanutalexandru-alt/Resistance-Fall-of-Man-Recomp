#!/usr/bin/env python3
"""Finds #include "..." lines whose spelling differs in case from the file on disk.

Windows does not care, Linux does: a build on Linux (build_linux.sh) fails on such a line. Checks only
quoted includes that resolve to a file next to the source or in the given include folders.

    check_include_case.py <source folder>... [-I <include folder>]...
Exit code 1 when a mismatch is found.
"""
import os, re, sys

INC = re.compile(r'^\s*#\s*include\s*"([^"]+)"', re.M)
EXT = ('.c', '.cc', '.cpp', '.cxx', '.h', '.hh', '.hpp', '.hxx', '.inc', '.inl')


def exact_case(base, rel):
    """True/False when rel exists under base (exact case or not); None when it does not exist at all."""
    cur = base
    exact = True
    for part in [p for p in re.split(r'[\\/]', rel) if p not in ('', '.')]:
        if part == '..':
            cur = os.path.dirname(cur)
            continue
        try:
            names = os.listdir(cur)
        except OSError:
            return None
        if part in names:
            cur = os.path.join(cur, part)
            continue
        low = [n for n in names if n.lower() == part.lower()]
        if not low:
            return None
        exact = False
        cur = os.path.join(cur, low[0])
    return exact


def main(argv):
    srcs, incs, i = [], [], 0
    while i < len(argv):
        if argv[i] == '-I' and i + 1 < len(argv):
            incs.append(argv[i + 1]); i += 2
        else:
            srcs.append(argv[i]); i += 1
    bad = 0
    for top in srcs:
        for d, _ds, fs in os.walk(top):
            for n in fs:
                if not n.lower().endswith(EXT):
                    continue
                p = os.path.join(d, n)
                try:
                    text = open(p, encoding='utf-8', errors='replace').read()
                except OSError:
                    continue
                for m in INC.finditer(text):
                    rel = m.group(1)
                    if '\\' in rel:
                        print('%s: #include "%s" uses a backslash' % (p, rel)); bad += 1
                        continue
                    for base in [d] + incs:
                        r = exact_case(base, rel)
                        if r is None:
                            continue
                        if r is False:
                            print('%s: #include "%s" differs in case from the file on disk' % (p, rel)); bad += 1
                        break
    print('%d include line(s) to fix.' % bad)
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
