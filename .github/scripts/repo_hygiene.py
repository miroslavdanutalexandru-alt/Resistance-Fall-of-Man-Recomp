#!/usr/bin/env python3
"""Repository hygiene check over `git ls-files`.

1. Fails on any tracked path that matches a game-file deny pattern (the $strict
   list of scripts/package_beta.ps1, read from that file).
2. Fails on any tracked file over MAX_BYTES unless its path is in
   .github/large-file-allowlist.txt.

Reads only paths and file sizes; it never opens file contents and runs no
repository code. Usage: python3 .github/scripts/repo_hygiene.py [repo_root]
"""
import os
import re
import subprocess
import sys

MAX_BYTES = 5 * 1024 * 1024  # 5 MB

PACKAGE_SCRIPT = os.path.join('scripts', 'package_beta.ps1')


def load_strict(root):
    """Reads the $strict deny list straight from scripts/package_beta.ps1 (the only copy).

    Only the text is parsed; the script is never run. Each entry is a line
    `'label' = 'regex'` inside `$strict = [ordered]@{ ... }`. PowerShell -match is
    case-insensitive, so re.IGNORECASE is used here.
    """
    text = open(os.path.join(root, PACKAGE_SCRIPT), encoding='utf-8-sig').read()
    block = re.search(r'^\$strict\s*=\s*\[ordered\]@\{\s*$(.*?)^\}', text, re.MULTILINE | re.DOTALL)
    if not block:
        raise SystemExit('ERROR: no $strict = [ordered]@{ ... } block in ' + PACKAGE_SCRIPT)
    entry = re.compile(r"^\s*'((?:[^']|'')*)'\s*=\s*'((?:[^']|'')*)'\s*$")
    strict = {}
    for line in block.group(1).splitlines():
        if not line.strip() or line.strip().startswith('#'):
            continue
        m = entry.match(line)
        if not m:
            raise SystemExit('ERROR: unexpected line in the $strict block of %s: %s' % (PACKAGE_SCRIPT, line.strip()))
        strict[m.group(1).replace("''", "'")] = re.compile(m.group(2).replace("''", "'"), re.IGNORECASE)
    if not strict:
        raise SystemExit('ERROR: the $strict block of %s is empty' % PACKAGE_SCRIPT)
    return strict


def main():
    root = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else os.getcwd()
    strict = load_strict(root)
    out = subprocess.run(['git', '-C', root, 'ls-files', '-z'], check=True, capture_output=True).stdout
    paths = [p.decode('utf-8', 'surrogateescape') for p in out.split(b'\0') if p]

    allow_file = os.path.join(root, '.github', 'large-file-allowlist.txt')
    allow = set()
    if os.path.exists(allow_file):
        for line in open(allow_file, encoding='utf-8'):
            line = line.split('#', 1)[0].strip()
            if line:
                allow.add(line)

    errors = []
    big_seen = set()
    for p in paths:
        for label, rx in strict.items():
            if rx.search(p):
                errors.append('game-file pattern [%s]: %s' % (label, p))
                break
        full = os.path.join(root, p)
        if os.path.islink(full) or not os.path.isfile(full):
            continue  # symlinks, submodules, files deleted in the working tree
        size = os.path.getsize(full)
        if size > MAX_BYTES:
            big_seen.add(p)
            if p not in allow:
                errors.append('over %d MB and not in .github/large-file-allowlist.txt: %s (%d bytes)'
                              % (MAX_BYTES // (1024 * 1024), p, size))

    for p in sorted(allow - big_seen):
        print('note: allow-list entry is not over the cap (or not tracked): ' + p)
    if errors:
        for e in errors:
            print('ERROR: ' + e)
        print('%d problem(s) in %d tracked files' % (len(errors), len(paths)))
        return 1
    print('ok: %d tracked files, no game-file patterns, none over the cap without an allow-list entry' % len(paths))
    return 0


if __name__ == '__main__':
    sys.exit(main())
