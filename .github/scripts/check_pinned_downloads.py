#!/usr/bin/env python3
"""Checks that the downloads pinned in this repository still exist and still have the pinned SHA-256.

Reads, as text only (nothing is executed):
  * setup.ps1: pairs of lines  $<Name>Url = '<url>'  and  $<Name>Sha256 = '<64 hex>'
    (FFmpeg, Xess, Sl, FfxFg today). A URL without a hash, a hash without a URL, or no pair at all is an error.
  * tools/shaderscan.version: the lines  url=<url>  and  sha256=<64 hex>.
Each URL is downloaded (streamed, nothing is written to disk) and its SHA-256 compared with the pin.
Exit 1 when any download fails (after 3 tries) or has another hash; every pin is checked before exiting.
Usage: python3 .github/scripts/check_pinned_downloads.py [repo_root]
"""
import hashlib
import os
import re
import sys
import time
import urllib.request

TRIES = 3
TIMEOUT = 60  # seconds without data


def read_setup_pins(path):
    text = open(path, encoding='utf-8-sig').read()
    urls = dict(re.findall(r"^\$(\w+?)Url\s*=\s*'(https://[^']+)'", text, re.MULTILINE))
    hashes = dict(re.findall(r"^\$(\w+?)Sha256\s*=\s*'([0-9A-Fa-f]{64})'", text, re.MULTILINE))
    pins, errors = [], []
    for name in sorted(set(urls) | set(hashes)):
        if name not in urls:
            errors.append('setup.ps1: $%sSha256 has no $%sUrl' % (name, name))
        elif name not in hashes:
            errors.append('setup.ps1: $%sUrl has no $%sSha256' % (name, name))
        else:
            pins.append(('setup.ps1 ' + name, urls[name], hashes[name]))
    if not pins:
        errors.append('setup.ps1: no $<Name>Url / $<Name>Sha256 pair found (did the format change?)')
    return pins, errors


def read_version_pin(path):
    kv = {}
    for line in open(path, encoding='utf-8-sig'):
        line = line.strip()
        if line and not line.startswith('#') and '=' in line:
            k, v = line.split('=', 1)
            kv[k.strip()] = v.strip()
    if not re.fullmatch(r'https://\S+', kv.get('url', '')) or not re.fullmatch(r'[0-9A-Fa-f]{64}', kv.get('sha256', '')):
        return [], ['tools/shaderscan.version: needs a url=https://... line and a 64-hex sha256= line']
    return [('shaderscan.version', kv['url'], kv['sha256'])], []


def sha256_of_url(url):
    last = None
    for attempt in range(1, TRIES + 1):
        try:
            h, n = hashlib.sha256(), 0
            req = urllib.request.Request(url, headers={'User-Agent': 'pinned-downloads-check'})
            with urllib.request.urlopen(req, timeout=TIMEOUT) as r:
                while True:
                    b = r.read(1 << 20)
                    if not b:
                        break
                    h.update(b)
                    n += len(b)
            return h.hexdigest(), n, None
        except Exception as e:  # network errors, HTTP errors, timeouts
            last = '%s: %s' % (type(e).__name__, e)
            time.sleep(5 * attempt)
    return None, 0, last


def main():
    root = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else os.getcwd()
    pins, errors = read_setup_pins(os.path.join(root, 'setup.ps1'))
    p2, e2 = read_version_pin(os.path.join(root, 'tools', 'shaderscan.version'))
    pins += p2
    errors += e2
    for label, url, want in pins:
        got, size, err = sha256_of_url(url)
        if err:
            errors.append('%s: download failed: %s (%s)' % (label, url, err))
        elif got.lower() != want.lower():
            errors.append('%s: SHA-256 is %s, pinned %s (%s)' % (label, got, want.lower(), url))
        else:
            print('ok   %-24s %s  %d bytes' % (label, got, size))
    for e in errors:
        print('ERROR: ' + e)
    if errors:
        print('%d problem(s) in %d pin(s) checked' % (len(errors), len(pins)))
        return 1
    print('ok: %d pinned downloads available with the pinned SHA-256' % len(pins))
    return 0


if __name__ == '__main__':
    sys.exit(main())
