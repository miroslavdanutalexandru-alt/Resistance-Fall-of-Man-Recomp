#!/usr/bin/env python3
"""sys_ppu_thread_create's JOINABLE flag through the real sys_ppu_thread.c: non-joinable threads free their slot."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
root=Path(__file__).resolve().parents[1]
if os.name=='nt':
    # The join/exit slot race (R1 #17) is in the Windows half of the file, so build it there too: clang-cl with an
    # MSVC environment (vcvars64) for the CRT and SDK libraries.
    cl=shutil.which('clang-cl') or r'C:\Program Files\LLVM\bin\clang-cl.exe'
    if not Path(cl).exists() or not os.environ.get('LIB'):
        print('SKIP: needs clang-cl and an MSVC environment (vcvars64)')
        raise SystemExit(0)
    with tempfile.TemporaryDirectory(prefix='ppu-thread-slots-') as tmp:
        binary=Path(tmp)/'test.exe'
        subprocess.run([cl,'/nologo','/O1','/D_CRT_SECURE_NO_WARNINGS','-Wno-comment',
            '/I'+str(root/'include'),'/I'+str(root/'runtime'),'/I'+str(root/'runtime/syscalls'),
            str(root/'tests/test_ppu_thread_slots.c'),'/Fe:'+str(binary),'/Fo:'+str(Path(tmp)/'test.obj'),
            '/link','Synchronization.lib'],check=True,cwd=tmp)
        r=subprocess.run([str(binary)],timeout=120,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,errors='replace')
        print(r.stdout.strip())
        if r.returncode:
            print('\n'.join(l for l in r.stderr.splitlines() if 'FAIL' in l or 'lost' in l or 'failed' in l))
            raise SystemExit(r.returncode)
    raise SystemExit(0)
with tempfile.TemporaryDirectory(prefix='ppu-thread-slots-') as tmp:
    binary=Path(tmp)/'test'
    linker=['-Wl,-dead_strip'] if sys.platform=='darwin' else ['-Wl,--gc-sections','-pthread','-lm']
    subprocess.run([os.environ.get('CC','cc'),'-std=gnu17','-D_GNU_SOURCE','-O1','-UNDEBUG',
        '-ffunction-sections','-fdata-sections',
        '-I'+str(root/'include'),'-I'+str(root/'runtime'),'-I'+str(root/'runtime/syscalls'),
        str(root/'tests/test_ppu_thread_slots.c'),*linker,'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True,timeout=60,stderr=subprocess.DEVNULL if os.environ.get('CI') else None)
