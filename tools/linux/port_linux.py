#!/usr/bin/env python3
"""Helpers for setup_linux.sh (Linux / Steam Deck setup, see README "Linux / Steam Deck").

Python 3.8+ standard library only. The Windows setup does the same with PowerShell
(tools/installer/gamefiles.ps1, setup.ps1); this file is the Linux counterpart and is
not used on Windows.

    port_linux.py check-game <game_profile.psd1> <iso | game folder>
    port_linux.py copy-game  <game_profile.psd1> <iso | game folder> <dest>
    port_linux.py ffmpeg     <ffmpeg-...-win64-lgpl-shared-7.1.zip> <dest dir>
    port_linux.py hash       sha1|sha256 <file>
    port_linux.py gen-hash   <port dir>
    port_linux.py gen-snapshot <port dir> <snapshot.json> <dir>...
    port_linux.py gen-restore  <port dir> <snapshot.json>
"""
import hashlib, json, os, re, shutil, struct, subprocess, sys, zipfile

SKIP_DIRS = ('PS3_CONTENT', 'PS3_UPDATE')   # disc folders the game never reads (as gamefiles.ps1)


# ------------------------------------------------------------------ game profile (tools/installer/game_profile.psd1)
def load_profile(path):
    t = open(path, encoding='utf-8-sig').read()
    t = re.sub(r'(?m)#.*$', '', t)

    def s(key, default=''):
        m = re.search(r'(?m)^\s*%s\s*=\s*\'([^\']*)\'' % key, t)
        return m.group(1) if m else default

    def arr(key):
        m = re.search(r'(?ms)^\s*%s\s*=\s*@\((.*?)\)' % key, t)
        return re.findall(r"'([^']*)'", m.group(1)) if m else []

    def num(key):
        m = re.search(r'(?m)^\s*%s\s*=\s*(\d+)' % key, t)
        return int(m.group(1)) if m else 0

    sha = {}
    m = re.search(r'(?ms)^\s*Sha1\s*=\s*@\{(.*?)\}', t)
    if m:
        for k, v in re.findall(r"'([^']+)'\s*=\s*'([0-9a-fA-F]{40})'", m.group(1)):
            sha[k] = v.lower()
    p = {'GameName': s('GameName'), 'TitleId': s('TitleId'), 'RegionLabel': s('RegionLabel'),
         'AppVer': s('AppVer'), 'TitleMatch': s('TitleMatch', '$^'), 'Required': arr('Required'),
         'Program': arr('Program'), 'Sha1': sha, 'MinBytes': num('MinBytes'), 'SizeLabel': s('SizeLabel')}
    if not p['TitleId']:
        raise SystemExit('Bad game profile: %s' % path)
    return p


# ------------------------------------------------------------------ PARAM.SFO
def read_sfo(b):
    if not b or len(b) < 20 or b[:4] != b'\0PSF':
        return None
    key_tab, data_tab, n = struct.unpack_from('<III', b, 8)
    r = {}
    for i in range(n):
        e = 20 + 16 * i
        if e + 16 > len(b):
            break
        ko, fmt, ln, _mx, do = struct.unpack_from('<HHIII', b, e)
        ko += key_tab; do += data_tab
        if ko >= len(b) or do + ln > len(b):
            continue
        ke = b.find(b'\0', ko)
        key = b[ko:ke if ke >= 0 else len(b)].decode('ascii', 'replace')
        if fmt == 0x0404:
            r[key] = struct.unpack_from('<I', b, do)[0]
        else:
            r[key] = b[do:do + ln].split(b'\0', 1)[0].decode('utf-8', 'replace')
    return r


# ------------------------------------------------------------------ ISO 9660 reader (no mounting, no root)
class Iso:
    def __init__(self, path):
        self.f = open(path, 'rb')
        self.root = None
        self.joliet = False
        for s in range(16, 64):
            self.f.seek(s * 2048)
            buf = self.f.read(2048)
            if len(buf) != 2048 or buf[1:6] != b'CD001':
                break
            typ = buf[0]
            if typ == 255:
                break
            rec = {'Dir': True, 'Extents': [(struct.unpack_from('<I', buf, 158)[0], struct.unpack_from('<I', buf, 166)[0])]}
            if typ == 1 and not self.root:
                self.root = rec
            if typ == 2 and buf[88] == 0x25 and buf[89] == 0x2F and buf[90] in (0x40, 0x43, 0x45):
                self.root = rec; self.joliet = True
        if not self.root:
            self.f.close()
            raise ValueError('not an ISO 9660 image')

    def close(self):
        self.f.close()

    def listdir(self, d):
        lba, size = d['Extents'][0]
        self.f.seek(lba * 2048)
        data = self.f.read(size)
        out, pos, pending = {}, 0, None
        while pos < len(data):
            ln = data[pos]
            if ln == 0:
                pos = (pos // 2048 + 1) * 2048
                continue
            if pos + 33 > len(data):
                break
            elba = struct.unpack_from('<I', data, pos + 2)[0]
            esize = struct.unpack_from('<I', data, pos + 10)[0]
            flags = data[pos + 25]; nl = data[pos + 32]
            raw = data[pos + 33:pos + 33 + nl]
            if nl == 1 and raw in (b'\0', b'\1'):
                pos += ln
                continue
            name = raw.decode('utf-16-be', 'replace') if self.joliet else raw.decode('ascii', 'replace')
            name = re.sub(r';\d+$', '', name).rstrip('.')
            if pending and pending['Name'] == name:
                e = pending
            else:
                e = {'Name': name, 'Dir': bool(flags & 2), 'Extents': [], 'Size': 0}
            e['Extents'].append((elba, esize)); e['Size'] += esize
            if flags & 0x80:
                pending = e          # multi-extent file (> 4 GB): more records follow
            else:
                pending = None
                out[name.upper()] = e
            pos += ln
        return out

    def entry(self, rel):
        cur = self.root
        for part in [p for p in re.split(r'[\\/]', rel) if p]:
            if not cur['Dir']:
                return None
            cur = self.listdir(cur).get(part.upper())
            if not cur:
                return None
        return cur

    def chunks(self, e, limit=0):
        want = e['Size'] if not limit else min(limit, e['Size'])
        for lba, size in e['Extents']:
            self.f.seek(lba * 2048)
            left = min(size, want)
            while left > 0:
                b = self.f.read(min(left, 4 << 20))
                if not b:
                    raise IOError('read error in the disc image at %s' % e['Name'])
                yield b
                left -= len(b); want -= len(b)
            if want <= 0:
                break

    def read(self, e, limit=0):
        return b''.join(self.chunks(e, limit))


# ------------------------------------------------------------------ checking a copy (as gamefiles.ps1 Test-GameSource)
ENC_FIX = ('Use a decrypted .iso or a game folder. An encrypted image (for example a Redump .iso with a .dkey file) '
           'can be decrypted first with PS3Dec and its disc key.')


class Check:
    def __init__(self):
        self.status, self.code, self.problem, self.fix = 'ok', '', '', ''
        self.kind, self.root, self.base, self.bytes, self.notes = '', '', '', 0, []

    def bad(self, status, code, problem, fix):
        if self.status != 'error':
            self.status, self.code, self.problem, self.fix = status, code, problem, fix


def check_sfo(prof, c, sfo, where):
    want = '%s (%s disc, %s)' % (prof['GameName'], prof['RegionLabel'], prof['TitleId'])
    if not sfo:
        c.bad('error', 'ENCRYPTED', "The game files can't be read: PARAM.SFO is not valid. This usually means an "
              "encrypted disc image.", ENC_FIX)
        return False
    title = re.sub(r'[\r\n]+', ' ', str(sfo.get('TITLE', ''))).replace('\u2122', '').replace('\u00ae', '')
    tid = str(sfo.get('TITLE_ID', ''))
    if sfo.get('CATEGORY') == 'GP' or re.search(r'[\\/]dev_hdd0[\\/]game[\\/]', where):
        c.bad('error', 'UPDATE_FOLDER', 'This is a game update or game data folder (%s, from dev_hdd0/game), not the '
              'disc.' % tid, 'Pick the disc copy: the .iso, or the folder that contains PS3_GAME and PS3_DISC.SFB.')
        return False
    if tid == prof['TitleId']:
        return True
    if tid.startswith('NP'):
        c.bad('error', 'PSN', 'This is the PlayStation Store version of %s (%s).' % (title, tid),
              'Only the disc version is supported: %s.' % want)
    elif re.search(prof['TitleMatch'], title, re.I):
        c.bad('error', 'WRONG_REGION', 'This is %s, %s.' % (title, tid), 'The port supports only %s for now.' % want)
    else:
        c.bad('error', 'WRONG_GAME', 'This is %s (%s), not %s.' % (title or 'another game', tid, prof['GameName']),
              'Pick your copy of %s.' % want)
    return False


def check_program(prof, c, rel, head, sha1_fn):
    if head[:4] == b'\x7fELF':
        c.bad('error', 'DECRYPTED', '%s in this copy has been decrypted or replaced (it is a plain ELF file).' % rel,
              'Use the original, unmodified file from the disc. Setup decrypts it itself.')
        return
    if head[:3] != b'SCE':
        c.bad('error', 'ENCRYPTED', "%s can't be read (it does not start like a PS3 program file). This usually means "
              "an encrypted disc image." % rel, ENC_FIX)
        return
    known = prof['Sha1'].get(rel)
    if known and sha1_fn() != known:
        c.bad('warn', 'MODIFIED', '%s is different from the known file of %s version %s. The copy may be modified or a '
              'different release.' % (rel, prof['TitleId'], prof['AppVer']),
              'Use an original, unmodified copy if you can. Setup checks the file again after decrypting it.')


def sha1_file(path):
    h = hashlib.sha1()
    with open(path, 'rb') as f:
        for b in iter(lambda: f.read(4 << 20), b''):
            h.update(b)
    return h.hexdigest()


def find_dump_root(p):
    q = os.path.abspath(p)
    for _ in range(4):
        if os.path.isfile(os.path.join(q, 'PS3_GAME', 'PARAM.SFO')):
            return q
        if os.path.basename(q) == 'PS3_GAME' and os.path.isfile(os.path.join(q, 'PARAM.SFO')):
            return os.path.dirname(q)
        nq = os.path.dirname(q)
        if nq == q:
            break
        q = nq
    if os.path.isdir(p):
        for n in sorted(os.listdir(p)):
            if os.path.isfile(os.path.join(p, n, 'PS3_GAME', 'PARAM.SFO')):
                return os.path.join(p, n)
    return None


def check_folder(prof, c, path):
    c.kind = 'folder'
    root = find_dump_root(path)
    if not root:
        if os.path.isdir(path) and any(n.lower().endswith('.iso') for n in os.listdir(path)):
            c.bad('error', 'NOT_PS3', 'This folder has .iso files but no unpacked game.', 'Pick the .iso file itself.')
        else:
            c.bad('error', 'NOT_PS3', 'No PS3 game was found here (there is no PS3_GAME/PARAM.SFO).',
                  'Pick the .iso, or the folder that contains PS3_GAME and PS3_DISC.SFB.')
        return c
    c.root = root
    try:
        sfo = read_sfo(open(os.path.join(root, 'PS3_GAME', 'PARAM.SFO'), 'rb').read())
    except OSError:
        sfo = None
    if not check_sfo(prof, c, sfo, root):
        return c
    missing = [r for r in prof['Required'] if not os.path.isfile(os.path.join(root, *r.split('/')))]
    if missing:
        c.bad('error', 'MISSING', 'Some game files are missing: %s.' % ', '.join(missing),
              'The copy is incomplete. Copy or dump the game again, then try again.')
        return c
    for rel in prof['Program']:
        full = os.path.join(root, *rel.split('/'))
        with open(full, 'rb') as f:
            head = f.read(4)
        check_program(prof, c, rel, head, lambda: sha1_file(full))
    total = 0
    for d, _dirs, files in os.walk(os.path.join(root, 'PS3_GAME')):
        for n in files:
            try:
                total += os.path.getsize(os.path.join(d, n))
            except OSError:
                pass
    c.bytes = total
    if prof['MinBytes'] and total < prof['MinBytes']:
        c.bad('warn', 'SMALL', 'The game files are smaller than expected (%.1f GB of about %s).' % (total / 2**30, prof['SizeLabel']),
              'The copy may be incomplete. Copy or dump the game again if the port fails to start.')
    return c


def check_iso(prof, c, path):
    c.kind = 'iso'; c.root = path; c.bytes = os.path.getsize(path)
    if os.path.exists(os.path.splitext(path)[0] + '.dkey'):
        c.notes.append('A .dkey file is next to the image: it may be an encrypted (Redump) image.')
    try:
        iso = Iso(path)
    except (OSError, ValueError):
        if is_udf(path):
            c.kind = 'udf'
            if find_7z():
                c.notes.append('This is a UDF disc image: it is unpacked with 7-Zip and checked after copying.')
            else:
                c.bad('error', 'ISO_UDF', 'This disc image uses the UDF format, which Setup reads with 7-Zip, and '
                      '7-Zip was not found.', 'Install 7-Zip (the 7zip or p7zip package; 7z or 7zz command) and run '
                      'Setup again, or unpack the image yourself and pick the folder.')
            return c
        c.bad('error', 'ISO_UNREADABLE', 'This file is not a readable disc image.',
              'Check that the file was copied or downloaded completely, then pick it again.')
        return c
    try:
        pre = ''
        if not iso.entry('PS3_GAME/PARAM.SFO'):
            for e in iso.listdir(iso.root).values():
                if e['Dir'] and iso.entry(e['Name'] + '/PS3_GAME/PARAM.SFO'):
                    pre = e['Name'] + '/'
                    break
            if not pre:
                c.bad('error', 'NOT_PS3', 'This disc image has no PS3 game in it (there is no PS3_GAME/PARAM.SFO).',
                      'Pick the .iso of your %s disc.' % prof['GameName'])
                return c
        c.base = pre.rstrip('/')
        sfo = read_sfo(iso.read(iso.entry(pre + 'PS3_GAME/PARAM.SFO')))
        if not check_sfo(prof, c, sfo, path):
            return c
        missing = [r for r in prof['Required'] if not iso.entry(pre + r)]
        if missing:
            c.bad('error', 'MISSING', 'Some game files are missing: %s.' % ', '.join(missing),
                  'The copy is incomplete. Copy or dump the game again, then try again.')
            return c
        for rel in prof['Program']:
            e = iso.entry(pre + rel)

            def sha1_iso(e=e):
                h = hashlib.sha1()
                for b in iso.chunks(e):
                    h.update(b)
                return h.hexdigest()
            check_program(prof, c, rel, iso.read(e, 4), sha1_iso)
        if prof['MinBytes'] and c.bytes < prof['MinBytes']:
            c.bad('warn', 'SMALL', 'The disc image is smaller than expected (%.1f GB of about %s).' % (c.bytes / 2**30, prof['SizeLabel']),
                  'The copy may be incomplete. Dump the game again if the port fails to start.')
    except (OSError, IOError) as ex:
        c.bad('error', 'ISO_UNREADABLE', 'The disc image could not be read (%s).' % ex,
              'Check that the file was copied or downloaded completely, then pick it again.')
    finally:
        iso.close()
    return c


def is_udf(path):
    try:
        with open(path, 'rb') as f:
            for s in range(16, 32):
                f.seek(s * 2048)
                if f.read(6)[1:6] in (b'BEA01', b'NSR02', b'NSR03'):
                    return True
    except OSError:
        pass
    return False


def find_7z():
    for n in ('7zz', '7z', '7za'):
        p = shutil.which(n)
        if p:
            return p
    return None


def check_source(prof, path):
    c = Check()
    path = os.path.expanduser(path.strip().strip('"').strip("'"))
    if not os.path.exists(path):
        c.bad('error', 'NOT_FOUND', 'Nothing was found at this path.', 'Check the path, or type the .iso or the game folder again.')
        return c
    if os.path.isfile(path):
        ext = os.path.splitext(path)[1].lower()
        if ext in ('.zip', '.7z', '.rar'):
            c.bad('error', 'ARCHIVE', 'This is a compressed archive (%s), not the game itself.' % ext,
                  'Extract it first, then pick the .iso or the game folder inside.')
            return c
        if ext == '.pkg':
            c.bad('error', 'PKG', 'This is a PlayStation Store package (.pkg), such as a game update.',
                  'Pick the disc copy instead: the .iso, or the folder that contains PS3_GAME.')
            return c
        if ext == '.iso':
            return check_iso(prof, c, path)
        if ext in ('.bin', '.cue', '.img', '.mdf', '.nrg', '.chd') and not find_dump_root(os.path.dirname(path)):
            c.bad('error', 'FORMAT', 'This disc image format (%s) is not supported.' % ext,
                  'Use an .iso file or a game folder (the same kind RPCS3 uses).')
            return c
        path = os.path.dirname(path)
    return check_folder(prof, c, path)


def print_check(c):
    print('   %s copy: %s' % (c.kind or 'game', c.root))
    for n in c.notes:
        print('   note: ' + n)
    if c.status != 'ok':
        print('   %s [%s]: %s' % ('ERROR' if c.status == 'error' else 'WARNING', c.code, c.problem))
        print('   What to do: ' + c.fix)
    else:
        print('   OK: the game copy looks right.')


# ------------------------------------------------------------------ copying
def copy_folder(src, dest):
    total = done = 0
    plan = []
    for d, dirs, files in os.walk(src):
        if os.path.abspath(d) == os.path.abspath(src):
            dirs[:] = [x for x in dirs if x not in SKIP_DIRS]
        for n in files:
            s = os.path.join(d, n)
            plan.append(s); total += os.path.getsize(s)
    last = -1
    for s in plan:
        t = os.path.join(dest, os.path.relpath(s, src))
        os.makedirs(os.path.dirname(t), exist_ok=True)
        st = os.stat(s)
        if not (os.path.exists(t) and os.path.getsize(t) == st.st_size and int(os.path.getmtime(t)) == int(st.st_mtime)):
            shutil.copy2(s, t)
        done += st.st_size
        pct = int(100 * done / total) if total else 100
        if pct != last:
            print('\r   copying: %3d %%' % pct, end='', flush=True); last = pct
    print()


def copy_iso(path, base, dest):
    iso = Iso(path)
    try:
        start = iso.entry(base) if base else iso.root
        state = {'done': 0, 'last': -1}
        total = os.path.getsize(path)

        def walk(d, out, top):
            os.makedirs(out, exist_ok=True)
            for e in iso.listdir(d).values():
                if e['Dir']:
                    if top and e['Name'] in SKIP_DIRS:
                        continue
                    walk(e, os.path.join(out, e['Name']), False)
                    continue
                t = os.path.join(out, e['Name'])
                if os.path.exists(t) and os.path.getsize(t) == e['Size']:
                    state['done'] += e['Size']
                    continue
                with open(t + '.part', 'wb') as f:
                    for b in iso.chunks(e):
                        f.write(b); state['done'] += len(b)
                        pct = int(100 * state['done'] / total) if total else 100
                        if pct != state['last']:
                            print('\r   copying from the disc image: %3d %%' % min(pct, 100), end='', flush=True)
                            state['last'] = pct
                os.replace(t + '.part', t)
        walk(start, dest, True)
        print()
    finally:
        iso.close()


def cmd_copy_game(prof, src, dest):
    c = check_source(prof, src)
    print_check(c)
    if c.status == 'error':
        return 1
    marker = dest.rstrip('/\\') + '.copying'
    open(marker, 'w').close()        # an interrupted copy is finished next time
    if c.kind == 'iso':
        copy_iso(c.root, c.base, dest)
    elif c.kind == 'udf':
        tmp = dest.rstrip('/\\') + '.unpack'
        if os.path.isdir(tmp):
            shutil.rmtree(tmp)
        print('   unpacking the disc image with 7-Zip (this takes a few minutes)...', flush=True)
        r = subprocess.call([find_7z(), 'x', '-y', '-bso0', '-bsp1', '-o' + tmp, c.root,
                             '-xr!PS3_CONTENT', '-xr!PS3_UPDATE', '-xr!*.pkg'])
        if r != 0:
            print('   ERROR: 7-Zip could not unpack the disc image (code %d).' % r)
            return 1
        root = find_dump_root(tmp)
        if not root:
            print('   ERROR: the unpacked disc image has no PS3_GAME/PARAM.SFO.')
            return 1
        c2 = check_folder(prof, Check(), root)
        print_check(c2)
        if c2.status == 'error':
            return 1
        if os.path.isdir(dest):
            shutil.rmtree(dest)
        os.replace(root, dest)
        shutil.rmtree(tmp, ignore_errors=True)
    else:
        copy_folder(c.root, dest)
    missing = [r for r in prof['Required'] if not os.path.isfile(os.path.join(dest, *r.split('/')))]
    if missing:
        print('   ERROR: after copying, these files are still missing: %s' % ', '.join(missing))
        return 1
    os.remove(marker)
    print('   Game files copied to %s' % dest)
    return 0


# ------------------------------------------------------------------ FFmpeg (as setup.ps1 step 5)
def cmd_ffmpeg(zpath, dest):
    want = ('avcodec-61.dll', 'avutil-59.dll', 'swresample-5.dll')
    with zipfile.ZipFile(zpath) as z:
        names = z.namelist()
        dlls = {os.path.basename(n): n for n in names if os.path.basename(n) in want}
        lic = sorted((n for n in names if os.path.basename(n) == 'LICENSE.txt'), key=len)
        if len(dlls) != len(want):
            print('This FFmpeg zip is not an FFmpeg 7.1 shared build (needs %s).' % ', '.join(want)); return 1
        if not lic or 'GNU LESSER GENERAL PUBLIC LICENSE' not in z.read(lic[0]).decode('utf-8', 'replace').splitlines()[0]:
            print("This FFmpeg zip is not an LGPL build (its LICENSE.txt is not the GNU LGPL). Use a 'lgpl-shared' build."); return 1
        if os.path.isdir(dest):
            shutil.rmtree(dest)
        os.makedirs(dest)
        for n in want:
            with open(os.path.join(dest, n), 'wb') as f:
                f.write(z.read(dlls[n]))
        with open(os.path.join(dest, 'LICENSE.txt'), 'wb') as f:
            f.write(z.read(lic[0]))
    return 0


# ------------------------------------------------------------------ generated code: generator hash and file dates
GEN_EXT = ('.py', '.json', '.c', '.cpp', '.h', '.hpp', '.inc', '.txt')


def gen_inputs(port):
    """The code generator's inputs, broadly: every script in tools/ and ps3recomp/tools/ and everything in
    tools/hooks/. Broader than the Windows setup's list (build_doctor.ps1), so a change regenerates rather than
    being missed; regenerating keeps the dates of unchanged files (gen-snapshot / gen-restore)."""
    files = []
    for top in ('tools', os.path.join('ps3recomp', 'tools')):
        d = os.path.join(port, top)
        if os.path.isdir(d):
            files += [os.path.join(d, n) for n in os.listdir(d) if n.endswith('.py')]
    hk = os.path.join(port, 'tools', 'hooks')
    for d, _dirs, fs in os.walk(hk):
        files += [os.path.join(d, n) for n in fs if n.lower().endswith(GEN_EXT)]
    return sorted(set(files), key=lambda p: os.path.relpath(p, port).replace(os.sep, '/').lower())


def cmd_gen_hash(port):
    h = hashlib.sha256()
    for f in gen_inputs(port):
        h.update(os.path.relpath(f, port).replace(os.sep, '/').lower().encode() + b'\n')
        h.update(open(f, 'rb').read().replace(b'\r\n', b'\n') + b'\n')
    print(h.hexdigest())
    return 0


def md5_file(p):
    h = hashlib.md5()
    with open(p, 'rb') as f:
        for b in iter(lambda: f.read(4 << 20), b''):
            h.update(b)
    return h.hexdigest()


def cmd_gen_snapshot(port, out, dirs):
    snap = {}
    for d in dirs:
        for root, _ds, fs in os.walk(os.path.join(port, d)):
            for n in fs:
                p = os.path.join(root, n)
                st = os.stat(p)
                snap[os.path.relpath(p, port)] = [st.st_size, st.st_mtime, md5_file(p)]
    with open(out, 'w') as f:
        json.dump(snap, f)
    return 0


def cmd_gen_restore(port, snapfile):
    with open(snapfile) as f:
        snap = json.load(f)
    kept = changed = 0
    for rel, (size, mtime, md5) in snap.items():
        p = os.path.join(port, rel)
        if not os.path.isfile(p):
            continue
        if os.path.getsize(p) == size and md5_file(p) == md5:
            os.utime(p, (mtime, mtime)); kept += 1
        else:
            changed += 1
    print('   Generated files: %d changed, %d unchanged.' % (changed, kept))
    return 0


def main(a):
    if len(a) >= 3 and a[0] == 'check-game':
        c = check_source(load_profile(a[1]), a[2]); print_check(c)
        return {'ok': 0, 'warn': 2}.get(c.status, 1)
    if len(a) >= 4 and a[0] == 'copy-game':
        return cmd_copy_game(load_profile(a[1]), a[2], a[3])
    if len(a) >= 3 and a[0] == 'ffmpeg':
        return cmd_ffmpeg(a[1], a[2])
    if len(a) >= 3 and a[0] == 'hash':
        h = hashlib.new(a[1])
        with open(a[2], 'rb') as f:
            for b in iter(lambda: f.read(4 << 20), b''):
                h.update(b)
        print(h.hexdigest()); return 0
    if len(a) >= 2 and a[0] == 'gen-hash':
        return cmd_gen_hash(a[1])
    if len(a) >= 4 and a[0] == 'gen-snapshot':
        return cmd_gen_snapshot(a[1], a[2], a[3:])
    if len(a) >= 3 and a[0] == 'gen-restore':
        return cmd_gen_restore(a[1], a[2])
    print(__doc__)
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
