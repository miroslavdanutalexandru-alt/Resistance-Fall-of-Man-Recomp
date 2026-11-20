#!/usr/bin/env python3
"""Setup tooling: tools\\installer\\tests\\*.ps1 (build-tool manifest, portable tools, RPCS3 fetch),
prereqs.ps1 -Explain (what Setup says before it installs anything) and a parse check of the setup scripts.
Nothing is installed or downloaded."""
import glob
import os
import re
import shutil
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PS = os.path.join(os.environ.get('SystemRoot', r'C:\Windows'), r'System32\WindowsPowerShell\v1.0\powershell.exe')


def ps(args, **kw):
    return subprocess.run([PS, '-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass'] + args, cwd=REPO,
                          capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=600, **kw)


def test_ps_tests():
    tests = sorted(glob.glob(os.path.join(REPO, 'tools', 'installer', 'tests', 'test_*.ps1')))
    assert tests, 'no tools\\installer\\tests\\test_*.ps1'
    for t in tests:
        r = ps(['-File', t])
        if r.returncode != 0:
            print(r.stdout[-4000:], r.stderr[-2000:])
        assert r.returncode == 0, f'{os.path.basename(t)} exit {r.returncode}'
        print(f'  {os.path.basename(t)}: ok')


def test_parse():
    files = ['setup.ps1', r'tools\installer\prereqs.ps1', r'tools\installer\buildtools.ps1', r'tools\fetch_rpcs3.ps1']
    files = [f for f in files if os.path.exists(os.path.join(REPO, f))]
    cmd = ('$bad = 0; foreach ($f in $env:PORT_PARSE_FILES -split [char]59) { $e = $null; [void][Management.Automation.Language.Parser]::ParseFile($f, [ref]$null, [ref]$e); '
           'foreach ($x in $e) { Write-Output "$f : $($x.Message)"; $bad++ } }; exit $bad')
    env = dict(os.environ, PORT_PARSE_FILES=';'.join(os.path.join(REPO, f) for f in files))
    r = ps(['-Command', cmd], env=env)
    assert r.returncode == 0, r.stdout


def test_explain():
    # what Setup shows before it asks: what each tool is for and where it comes from
    cache = os.path.join(REPO, 'tools_cache')
    had_cache = os.path.isdir(cache)
    r = ps(['-File', os.path.join(REPO, r'tools\installer\prereqs.ps1'), '-Explain', '-Ids',
            'Python.Python.3.12,LLVM.LLVM,Microsoft.VisualStudio.2022.BuildTools'])
    assert r.returncode == 0, r.stdout + r.stderr
    out = r.stdout
    assert 'IDLE' in out and 'python.org' in out, out
    assert out.count('why:') == 3 and out.count('from:') == 3, out
    assert 'on drive C:' in out and 'licence' not in out, out
    r = ps(['-File', os.path.join(REPO, r'tools\installer\prereqs.ps1'), '-Explain', '-Portable', '-Ids', 'VSModify'])
    assert r.returncode == 0, r.stdout + r.stderr
    assert 'xwin' in r.stdout and 'go.microsoft.com/fwlink/?LinkId=2086102' in r.stdout and 'tools_cache' in r.stdout, r.stdout
    if not had_cache and os.path.isdir(cache) and not os.listdir(cache):
        shutil.rmtree(cache)
    assert had_cache or not os.path.isdir(cache), '-Explain created tools_cache\\'


def test_rpcs3_pin():
    # scripts\regen_patches.ps1 reads the pinned commit with this regex: keep fetch_rpcs3.ps1's pin line in that form
    f = os.path.join(REPO, 'tools', 'fetch_rpcs3.ps1')
    if not os.path.exists(f):
        return
    with open(f, encoding='utf-8-sig') as h:
        text = h.read()
    pins = re.findall(r"^\$(?:script:)?Commit\s*=\s*'([0-9a-f]{40})'", text, re.M)
    assert len(pins) == 1, f'fetch_rpcs3.ps1: expected one "$script:Commit = \'<40 hex>\'" pin line, found {pins}'
    regen = os.path.join(REPO, 'scripts', 'regen_patches.ps1')
    if os.path.exists(regen):
        with open(regen, encoding='utf-8-sig') as h:
            assert 'tools\\fetch_rpcs3.ps1' in h.read()
    assert glob.glob(os.path.join(REPO, 'ps3recomp', 'patches', '*-rpcs3.patch')), 'no ps3recomp\\patches\\*-rpcs3.patch'


def read(rel):
    with open(os.path.join(REPO, rel), encoding='utf-8-sig') as h:
        return h.read()


def test_wiring():
    # review fixes that live in the scripts' control flow (the decisions themselves are tested in test_buildtools.ps1)
    pr = read(r'tools\installer\prereqs.ps1')
    i = pr.index('$record = -not (Test-BtPresentBefore $id $st)')
    assert i < pr.index('$c = Install-Winget $id') and i < pr.index('else { $c = Install-Direct $id }'), 'prereqs: decide before installing'
    assert re.search(r'if \(\$ok -and \$record -and .*Add-BtManifest', pr), 'prereqs: record only fresh installs'
    s = read('setup.ps1')
    w = s[s.index('function Winget-Install'):]
    assert w.index('Confirm-SystemInstall $id $label') < w.index('$fresh = -not (Test-BtPresentBefore $id)') < w.index('& winget @a'), 'setup.ps1: consent + decision before winget'
    assert 'if ($fresh -and (Test-BtFreshInstall $c))' in w
    assert '$InstallTools -or $Unattended' not in s, 'setup.ps1: -Unattended alone must not mean "install system-wide"'
    assert 'Get-BtToolMode -PortableTools:$PortableTools -InstallTools:$InstallTools -Unattended:$Unattended' in s
    cpp = read(r'tools\installer\setup_exe\setup.cpp')
    run = cpp[cpp.index('static int RunSetupOnce'):]
    run = run[:run.index('\n}\n')]
    assert 'if (consent == 1) cmd += L" -InstallTools";' in run, 'Setup.exe passes -InstallTools after its question'
    assert 'if (!consent && g_prepare)' in run and 'consent = PortableChosenBefore(&ms) ? (ms ? 3 : 2) : 1;' in run, '--prepare: launcher asked; portable kept'
    assert cpp.count('g_toolConsent = 1;') == 1 and cpp.count('g_toolConsent = (ms || msBefore) ? 3 : 2;') == 1
    # re-review 3 #2: --prepare takes the portable branch when it was chosen before, and never accepts the licence itself
    wk = cpp[cpp.index('static void SetupWorker'):]
    assert 'int a = g_console ? (portableBefore ? IDNO : IDYES) : Ask(' in wk
    assert wk.index('if (ms && !msBefore)') < wk.index('InstallPortable(ids'), '--prepare stops when the licence was never accepted'
    assert cpp.count('PortableChosenBefore(') == 3, 'one shared manifest helper'
    # re-review 3 #1: setup.ps1 treats an MSYS2 cmake as missing (portable mode then fetches CMake) and never falls back to winget in portable mode
    g = s[s.index('function Get-MissingToolKeys'):]
    assert "if (-not (Test-CmakeOk)) { $k += 'cmake' }" in g[:g.index('\n}')]
    i = s.index('function Install-CMakeTool')
    assert "$script:ToolMode -eq 'P' -or (-not $script:ToolMode -and (Test-BtPortableChosen))" in s[i:i + 300]
    assert s.count("Winget-Install 'Kitware.CMake'") == 1
    # re-review #3: an MSYS2/Cygwin cmake must count as missing in -Check, so Setup.exe asks before installing CMake
    st = pr[pr.index('function Get-ToolStatus'):]
    assert 'Test-BtCmakeNative' in st[:st.index('\nfunction ')], 'prereqs -Check: native cmake probe'
    assert 'Test-BtCmakeNative $c.Source' in s, 'setup.ps1 uses the same probe'
    assert s.count('In Setup.exe click Install/Resume') == 2, 'unattended stop messages name the Setup.exe action'
    # player report "python 3.10+ missing": one finder (buildtools.ps1 Find-BtPython) for setup.ps1 and prereqs -Check,
    # no "py / python on PATH" loops left, and the portable Python when an installed one still cannot be found
    assert 'Find-BtPython' in s[s.index('function Find-Python'):][:400], 'setup.ps1 Find-Python uses Find-BtPython'
    assert "foreach ($c in @('py', 'python'))" not in s and 'Python did not install.' not in s
    assert 'if (-not $py) { Fail (Get-BtPythonHelp $script:PyFind) }' in s
    assert '$pf = Find-BtPython' in st[:st.index('\nfunction ')], 'prereqs -Check uses Find-BtPython'
    assert "foreach ($c in @('py', 'python'))" not in pr
    i = pr.index("if ($id -eq 'Python.Python.3.12' -and -not $DryRun)")
    assert i < pr.index("Install-PortableTools @('python')", i) < pr.index('Result $id $ok $c', i), 'prereqs: portable Python fallback before the result'
    assert '-not $viaPortable' in pr, 'prereqs: the failed system install is not recorded when the portable Python was used'


def test_ffmpeg_path_spaces():
    # GitHub R3 #4/#5/#18: v0.2.3 passed -DPS3RECOMP_FFMPEG_INCLUDE_DIR=%~dp0ffmpeg\include unquoted, so a port
    # folder with a space ("D:\Games\Resistance 3\...") reached CMake cut short and the build stopped at
    # "'libavcodec/avcodec.h' file not found"; quotes nested in set "..." instead broke on "&" and ")".
    # Run the real set lines and cmake lines (cmake swapped for an argv echo, each in a ( ) block like build.bat)
    # from such folders and check that every path arrives as one intact argument.
    echo = '"' + sys.executable + '" -c "import sys; print(chr(10).join(sys.argv[1:]))"'
    keep = re.compile(r'set "(FFOPT|FFINC|COMMON|PGOPROF)=')
    for name in ('ffopt space test', 'R&C (x) test'):
        d = os.path.join(REPO, 'logs', name)
        os.makedirs(d, exist_ok=True)
        try:
            for bat in ('build.bat', 'build_cpu.bat'):
                if not os.path.exists(os.path.join(REPO, bat)):
                    continue
                lines = read(bat).splitlines()
                sets = [l for l in lines if keep.search(l)]
                calls = [l.strip() for l in lines if l.strip().startswith('cmake -S project')]
                assert any('FFINC=' in l for l in sets) and calls, f'{bat}: no FFINC / cmake -S project lines'
                body = ['@echo off', 'set "MODE=pgo-use"'] + sets
                for c in calls:
                    c = echo + c[len('cmake'):].split(' > logs')[0]
                    body += ['(', '  ' + c, ')', 'echo ----']
                t = os.path.join(d, 't.bat')
                with open(t, 'w', encoding='ascii') as h:
                    h.write('\r\n'.join(body) + '\r\n')
                r = subprocess.run(['cmd', '/d', '/c', r'.\t.bat'], cwd=d, capture_output=True, text=True, timeout=60)
                runs = r.stdout.split('----')[:-1]
                assert r.returncode == 0 and len(runs) == len(calls) and not r.stderr.strip(), \
                    f'{bat} in "{name}": rc {r.returncode}, {len(runs)}/{len(calls)} calls, {r.stderr.strip()}'
                inc = '-DPS3RECOMP_FFMPEG_INCLUDE_DIR=' + os.path.join(d, 'ffmpeg', 'include')
                for args in (x.strip().splitlines() for x in runs):
                    assert inc in args, f'{bat} in "{name}": FFmpeg include dir not one argument: {args}'
                    assert '-DPS3RECOMP_DIR=' + os.path.join(d, 'ps3recomp') in args, f'{bat} in "{name}": {args}'
                    if any('PGOPROF=' in l for l in sets):
                        assert any(a.startswith('-DPORT_PGO_PROFILE=' + d + os.sep + 'pgo' + os.sep) for a in args), args
        finally:
            shutil.rmtree(d, ignore_errors=True)
    cm = read(r'ps3recomp\CMakeLists.txt')
    assert 'if(NOT EXISTS "${PS3RECOMP_FFMPEG_INCLUDE_DIR}/libavcodec/avcodec.h")' in cm, 'configure-time FFmpeg header check'


def main():
    if os.name != 'nt' or not os.path.exists(PS):
        print('SKIP test_setup_tooling: needs Windows PowerShell')
        return 0
    test_rpcs3_pin()
    test_wiring()
    test_ffmpeg_path_spaces()
    test_parse()
    test_explain()
    test_ps_tests()
    print('test_setup_tooling: ok')
    return 0


if __name__ == '__main__':
    sys.exit(main())
