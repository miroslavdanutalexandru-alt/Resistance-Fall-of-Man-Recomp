#!/usr/bin/env python3
"""Resistance: Fall of Man port -- regenerate all game-derived code from the
player's own decrypted EBOOT.elf and game.elf.

    python tools/rfom_regen.py --eboot <EBOOT.elf> --game <game.elf> [--port <port dir>]

Writes, inside the port folder:
    recompiled/          front end (EBOOT)      PPU code
    recompiled_game/     game (game.self)       PPU code
    project/spu/<mod>/   SPU job modules, job manager, raw SPU programs
Nothing here is taken from the game except through the lifters run on the
player's own files; tools/hooks/ holds the port's own edits.
"""
import argparse, glob, json, os, re, shutil, struct, subprocess, sys, hashlib, threading, time

HERE = os.path.dirname(os.path.abspath(__file__))

# Machine-readable progress for Setup (setup.ps1 reads these lines; PORT_REGEN_PROGRESS=0 turns them off):
#   @@PROGRESS stage=<name> done=<n> total=<n>
_plock = threading.Lock()
_plast = {}

def progress(stage, done, total, force=False):
    if os.environ.get('PORT_REGEN_PROGRESS', '1') == '0':
        return
    now = time.time()
    with _plock:
        # at most ~4 lines a second per stage, but always the first and the last one
        if not force and 0 < done < total and now - _plast.get(stage, 0) < 0.25:
            return
        _plast[stage] = now
        print('@@PROGRESS stage=%s done=%d total=%d' % (stage, done, total), flush=True)

def run(cmd, **kw):
    r = subprocess.run(cmd, capture_output=True, text=True, **kw)
    if r.returncode != 0:
        sys.stderr.write(r.stdout[-2000:] + r.stderr[-4000:])
        raise SystemExit('FAILED: ' + ' '.join(cmd[:4]))
    return r.stdout

_LIFT_RE = re.compile(r'\.\.\. (lifted|emitting) (\d+)/(\d+) functions')

def run_progress(cmd, prefix='', **kw):
    """run() for the PPU lifter: its "... lifted N/M functions" / "... emitting N/M functions" lines become
    @@PROGRESS lines (stages <prefix>ppu_lift / ppu_emit) while it runs; the rest of its output is kept for errors."""
    env = dict(kw.pop('env', None) or os.environ, PYTHONUNBUFFERED='1')
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors='replace', env=env, **kw)
    tail = []
    for line in p.stdout:
        m = _LIFT_RE.search(line)
        if m:
            progress(prefix + ('ppu_lift' if m.group(1) == 'lifted' else 'ppu_emit'), int(m.group(2)), int(m.group(3)))
        tail.append(line)
        if len(tail) > 400: del tail[:200]
    p.wait()
    if p.returncode != 0:
        sys.stderr.write(''.join(tail)[-6000:])
        raise SystemExit('FAILED: ' + ' '.join(cmd[:4]))

def same_content(a, b):
    """True when files a and b exist and have the same bytes."""
    try:
        if os.path.getsize(a) != os.path.getsize(b): return False
        with open(a, 'rb') as fa, open(b, 'rb') as fb:
            while True:
                x = fa.read(1 << 20); y = fb.read(1 << 20)
                if x != y: return False
                if not x: return True
    except OSError:
        return False

def copy_if_changed(src, dst):
    """Copies src to dst only when the content differs (an unchanged file keeps its date). True when copied."""
    if same_content(src, dst): return False
    shutil.copyfile(src, dst)
    return True

def sync_tree(src, dst, keep=(), stage=None):
    """Makes dst a copy of src, writing only the files whose content changed (they get a new date; the others keep
    theirs, so Ninja compiles only what really changed) and deleting files src does not have (names in keep are
    left alone). Returns (changed, unchanged, removed)."""
    files = []
    for root, _, names in os.walk(src):
        for n in names: files.append(os.path.relpath(os.path.join(root, n), src))
    files.sort()
    changed = kept = removed = 0
    if stage: progress(stage, 0, len(files), force=True)
    for i, rel in enumerate(files):
        s, d = os.path.join(src, rel), os.path.join(dst, rel)
        if same_content(s, d):
            kept += 1
        else:
            os.makedirs(os.path.dirname(d), exist_ok=True)
            shutil.copyfile(s, d)
            changed += 1
        if stage: progress(stage, i + 1, len(files))
    want = {r.lower() for r in files}
    keep = {k.lower() for k in keep}
    if os.path.isdir(dst):
        for root, dirs, names in os.walk(dst, topdown=False):
            for n in names:
                rel = os.path.relpath(os.path.join(root, n), dst)
                if rel.lower() not in want and n.lower() not in keep:
                    os.remove(os.path.join(root, n)); removed += 1
            if root != dst and not os.listdir(root):
                os.rmdir(root)
    return changed, kept, removed

def ppu(port, tk, elf, outdir, name, chunks, post_passes, hooks, work):
    print('[%s] lifting PPU code...' % name, flush=True)
    progress(name + ':ppu_analyze', 0, 1, force=True)
    od = os.path.join(work, name + '_an')
    run([sys.executable, os.path.join(tk, 'ppu_loader.py'), elf, '-o', od])
    progress(name + ':ppu_analyze', 1, 1, force=True)
    stem = os.path.splitext(os.path.basename(elf))[0]
    tmp = os.path.join(work, name)
    if os.path.isdir(tmp): shutil.rmtree(tmp)
    run_progress([sys.executable, os.path.join(tk, 'ppu_lifter.py'), elf, '--functions', os.path.join(od, stem + '.functions.json'),
                  '--hle-stubs', os.path.join(od, stem + '.imports.json'), '--chunks', str(chunks), '--output', tmp], prefix=name + ':')
    chunksf = sorted(glob.glob(os.path.join(tmp, 'ppu_recomp_0*.cpp')))
    if post_passes:
        print('[%s] register cache + fast memory passes...' % name, flush=True)
        # weighted by size: the chunks differ a lot
        sizes = [os.path.getsize(f) for f in chunksf]
        tot = sum(sizes) or 1
        done = 0
        progress(name + ':ppu_post', 0, tot, force=True)
        for f, sz in zip(chunksf, sizes):
            run([sys.executable, os.path.join(HERE, 'regcache.py'), f, f])
            run([sys.executable, os.path.join(HERE, 'ppu_fastmem.py'), f, f])
            done += sz
            progress(name + ':ppu_post', done, tot, force=True)
        # the function table is its own file in this lifter version: give it the
        # same vm_base declaration guard ppu_fastmem.py gives the chunks
        tf = os.path.join(tmp, 'ppu_recomp_table.cpp')
        if os.path.exists(tf):
            s = open(tf, encoding='utf-8', errors='surrogateescape').read()
            s = s.replace('\nextern "C" uint8_t* vm_base;\n', '\n#ifndef PPU_FIXED_BOUNDS   /* v163: ppu_recomp.h declares it const */\nextern "C" uint8_t* vm_base;\n#endif\n', 1)
            open(tf, 'w', encoding='utf-8', errors='surrogateescape').write(s)
    progress(name + ':ppu_hooks', 0, 1, force=True)
    run([sys.executable, os.path.join(HERE, 'rfom_hooks.py'), 'apply', tmp, hooks])
    shutil.copy(os.path.join(HERE, 'hooks', 'ppu_hle_nids.cpp'), os.path.join(tmp, 'ppu_hle_nids.cpp'))
    progress(name + ':ppu_hooks', 1, 1, force=True)
    dst = os.path.join(port, outdir)
    # only the files whose content changed are written (Setup's code-generator stamp is kept)
    c, k, r = sync_tree(tmp, dst, keep=('.generator_inputs',), stage=name + ':ppu_copy')
    print('[%s] %s: %d files changed, %d unchanged, %d removed' % (name, outdir, c, k, r), flush=True)

def spu_post(f, hooks):
    run([sys.executable, os.path.join(HERE, 'rfom_spu_hooks.py'), 'apply', 'pre', f, hooks])
    run([sys.executable, os.path.join(HERE, 'spu_regcache.py'), f, f])
    run([sys.executable, os.path.join(HERE, 'spu_directhop.py'), f])
    run([sys.executable, os.path.join(HERE, 'rfom_spu_hooks.py'), 'apply', 'post', f, hooks])

def fix_includes(d):
    for f in glob.glob(os.path.join(d, 'spu_recomp.*')):
        s = open(f, encoding='utf-8', errors='surrogateescape').read().replace('"../../runtime/spu/', '"')
        open(f, 'w', encoding='utf-8', errors='surrogateescape').write(s)

def spu(port, tk, eboot, game, work, stage_raw=None):
    sys.path.insert(0, tk)
    from wrap_spu_elf import wrap
    spudir = os.path.join(port, 'project', 'spu')
    hk = os.path.join(HERE, 'hooks', 'spu')
    elfs = {'EBOOT': open(eboot, 'rb').read(), 'game': open(game, 'rb').read()}
    spec = json.load(open(os.path.join(HERE, 'rfom_spu_specs.json')))
    lsteps = 2 + len(spec)
    progress('spu_lift', 0, lsteps, force=True)
    # 1. WWS job modules (C0DEC0DE images in game.elf's data)
    print('[spu] extracting + lifting job modules...', flush=True)
    jm = os.path.join(work, 'jobmods'); out = os.path.join(work, 'spu')
    for d in (jm, out):
        if os.path.isdir(d): shutil.rmtree(d)
    run([sys.executable, os.path.join(HERE, 'extract_jobmods_rfom.py'), game, jm])
    progress('spu_lift', 1, lsteps, force=True)
    env = dict(os.environ, PYTHONPATH=tk)
    run([sys.executable, os.path.join(HERE, 'lift_jobmods_base.py'), '--jobmods', jm, '--out', out, '--register', os.path.join(work, 'jobmods_register.c')], env=env)
    progress('spu_lift', 2, lsteps, force=True)
    mods = sorted(os.path.basename(d) for d in glob.glob(os.path.join(out, 'jobmod_*')))
    os.makedirs(spudir, exist_ok=True)
    copy_if_changed(os.path.join(work, 'jobmods_register.c'), os.path.join(spudir, 'jobmods_register.c'))
    # 2. job manager (policy module) and the two raw SPU programs
    for i, (n, s) in enumerate(spec.items()):
        print('[spu] lifting %s...' % n, flush=True)
        src = elfs[s['elf']]
        raw = b'\0' * s['pad'] + src[s['off'] + s['pad']:s['off'] + s['size']]
        os.makedirs(os.path.join(work, n), exist_ok=True)
        pe = os.path.join(work, n, 'p.elf'); open(pe, 'wb').write(wrap(raw, base=s['base'], entry=s['entry']))
        run([sys.executable, os.path.join(tk, 'spu_lifter.py'), '--auto-functions', pe, '--base', hex(s['base']),
             '--symbol-prefix', n + '_', '-o', os.path.join(out, n), '--source-name', 'spu_recomp.c',
             '--header-name', 'spu_recomp.h', '--extra-funcs', ','.join(s['funcs'])], env=env)
        mods.append(n)
        progress('spu_lift', 3 + i, lsteps, force=True)
    print('[spu] port edits + register cache...', flush=True)
    changed = kept = removed = 0
    progress('spu_post', 0, len(mods), force=True)
    for i, n in enumerate(mods):
        d = os.path.join(out, n); fix_includes(d)
        if stage_raw is not None: shutil.copytree(d, os.path.join(stage_raw, n))
        hp = os.path.join(hk, n + '.json')
        if os.path.exists(hp): spu_post(os.path.join(d, 'spu_recomp.c'), hp)
        else:
            f = os.path.join(d, 'spu_recomp.c')
            run([sys.executable, os.path.join(HERE, 'spu_regcache.py'), f, f]); run([sys.executable, os.path.join(HERE, 'spu_directhop.py'), f])
            if n.startswith('jobmod_'):   # v189: registers stay in locals inside a function
                run([sys.executable, os.path.join(HERE, 'spu_writeback.py'), f, f])
                run([sys.executable, os.path.join(HERE, 'spu_group_entry.py'), f, f])   # v191: enter merged groups mid-way
        # only the files whose content changed are written, so the build compiles only the modules that changed
        c, k, r = sync_tree(d, os.path.join(spudir, n))
        changed += c; kept += k; removed += r
        progress('spu_post', i + 1, len(mods), force=True)
    print('[spu] project/spu: %d files changed, %d unchanged, %d removed' % (changed, kept, removed), flush=True)

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--eboot', required=True); ap.add_argument('--game', required=True)
    ap.add_argument('--port', default=os.path.dirname(HERE)); ap.add_argument('--work', default=None)
    ap.add_argument('--only', choices=['ppu', 'spu', 'eboot', 'gameppu'], default=None)
    ap.add_argument('--stage-raw', default=None, help=argparse.SUPPRESS)
    a = ap.parse_args()
    port = os.path.abspath(a.port); tk = os.path.join(port, 'ps3recomp', 'tools')
    work = os.path.abspath(a.work or os.path.join(port, 'build_regen'))
    os.makedirs(work, exist_ok=True)
    H = os.path.join(HERE, 'hooks')
    if a.only in (None, 'ppu', 'eboot'):
        ppu(port, tk, a.eboot, 'recompiled', 'eboot', 15, False, os.path.join(H, 'eboot.json'), work)
    if a.only in (None, 'ppu', 'gameppu'):
        ppu(port, tk, a.game, 'recompiled_game', 'game', 13, True, os.path.join(H, 'game.json'), work)
    if a.only in (None, 'spu'):
        spu(port, tk, a.eboot, a.game, work, a.stage_raw)
    print('done: game code regenerated.')

if __name__ == '__main__':
    main()
