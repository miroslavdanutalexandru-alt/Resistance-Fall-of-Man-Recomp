#!/usr/bin/env python3
"""RSX feature coverage, static half (identical in all three ports: scripts/featcov_static.py).

Finds the RSX methods a game's own code builds command buffers with, and says
whether each port's renderer handles them. No game run needed.

1. Method headers are immediates: (count << 18) | (subchannel << 13) | method,
   bit 30 = non-increment. Scanned in
     - the PPU ELF text (lis/li + ori/oris/addi/addis constant propagation;
       "dyn" = rlwinm/slwi by 18 of a variable count, then ori of the method),
     - every PPU ELF segment as data (runs of >= 3 chained headers: prebuilt
       command templates, also inside the SPU images embedded in the ELF),
     - the lifted SPU code project/spu/**/*.c (ilhu + iohl pairs, and the
       "data in .text" words the lifter keeps).
   A hit only counts if every register the header covers is a known method
   (RPCS3 gcm_enums.h names, with the per-unit / per-attribute arrays).
2. Status per port:
     R1: ps3recomp/libs/video/rsx_commands.c (+ cellGcmSys.c 2D / sync paths);
         "handled" = the dispatch stores it AND rsx_d3d12_backend.c (or its .inc
         / pso cache / ext-state helpers) reads the field, "stored-ignored" =
         stored but nothing in the backend reads it, "unhandled" = no handler.
     R2/R3: their RPCS3 tree (rsx_methods.cpp methods[] + rsx_methods.h
         accessors + who calls the accessors in VK/, Common/, RSXThread.cpp...).
         "handler" = methods[] binds a function, "consumed" = nullptr (register
         store) and an accessor of it is used by the renderer, "stored-ignored" =
         no renderer reads it, "invalid" = invalid_method (logged and dropped).
         Port bypasses (env-gated skips in the port's RPCS3 changes) are listed.

Usage:
  python scripts/featcov_static.py                     # this repo's game
  python scripts/featcov_static.py --all --md out.md   # the three games
  python scripts/featcov_static.py --emit-header ps3recomp/libs/video/rsx_featcov_tables.h
Writes scripts/featcov_static_<game>.json (read by featcov_report.py).
"""
import argparse
import glob
import json
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
GAMES_ROOT = os.path.dirname(REPO)
REPOS = {
    'r1': os.path.join(GAMES_ROOT, 'ResistancePort'),
    'r2': os.path.join(GAMES_ROOT, 'Resistance2Port'),
    'r3': os.path.join(GAMES_ROOT, 'Resistance3Port'),
}
RSX_REL = os.path.join('ps3recomp', 'third_party', 'rpcs3', 'rpcs3', 'Emu', 'RSX')


def game_of(repo):
    b = os.path.basename(os.path.normpath(repo)).lower()
    return 'r2' if '2port' in b else 'r3' if '3port' in b else 'r1'


# ---------------------------------------------------------------------------------------------
# Method names (RPCS3 gcm_enums.h) and the register arrays
# ---------------------------------------------------------------------------------------------
# (base offset, units, stride bytes, words per unit, label)
ARRAYS = [
    (0x0900, 4, 0x20, 8, 'VTEX'), (0x1A00, 16, 0x20, 8, 'TEX'),
    (0x0A20, 4, 4, 1, None), (0x0A30, 4, 4, 1, None),
    (0x0A80, 16, 8, 2, None), (0x0B00, 16, 4, 1, None), (0x0B40, 10, 4, 1, None),
    (0x0B80, 32, 4, 1, None), (0x1480, 32, 4, 1, None), (0x1500, 16, 16, 4, None),
    (0x1680, 16, 4, 1, None), (0x1740, 16, 4, 1, None), (0x1840, 16, 4, 1, None),
    (0x1880, 16, 8, 2, None), (0x1900, 16, 4, 1, None), (0x1940, 16, 4, 1, None),
    (0x1980, 16, 8, 2, None), (0x1C00, 16, 16, 4, None), (0x1D00, 16, 4, 1, None),
    (0x1E40, 16, 4, 1, None), (0x1F00, 32, 4, 1, None),
    (0x2400, 0x200, 4, 1, None),   # NV308A_COLOR (0xA400 composite, see below) handled separately
]


def load_names(rsx_dir):
    """gcm_enums.h CellGcmMethod symbols: byte offset -> enum symbol (used to grep the renderers)."""
    txt = open(os.path.join(rsx_dir, 'gcm_enums.h'), encoding='utf-8', errors='replace').read()
    m = re.search(r'enum CellGcmMethod[^{]*\{(.*?)\};', txt, re.S)
    names = {}
    for nm, off in re.findall(r'(\w+)\s*=\s*0x([0-9A-Fa-f]+)\s*>>\s*2', m.group(1)):
        off = int(off, 16)
        names.setdefault(off, nm)
    return names


def load_display_names(rsx_dir):
    """RPCS3's own method name table (gcm_printing.cpp methods_name, what rsx::get_method_name and
    the RSX disassembler print, minus the CELL_GCM_ prefix): byte offset -> name. Keys are
    expressions like NV4097_SET_ANISO_SPREAD + 4 / 4 (C integer arithmetic, in words)."""
    enums = open(os.path.join(rsx_dir, 'gcm_enums.h'), encoding='utf-8', errors='replace').read()
    sym = {n: int(v, 16) >> 2 for n, v in re.findall(r'\b(\w+)\s*=\s*0x([0-9A-Fa-f]+)\s*>>\s*2', enums)}
    p = os.path.join(rsx_dir, 'gcm_printing.cpp')
    if not os.path.exists(p):
        return {}
    txt = open(p, encoding='utf-8', errors='replace').read()
    m = re.search(r'methods_name\s*=\s*\{(.*?)\n\s*\};', txt, re.S)
    out = {}
    for key, name in re.findall(r'\{\s*([A-Za-z_]\w*[^,{}"]*?)\s*,\s*"([^"]+)"\s*\}', m.group(1) if m else ''):
        s0 = re.match(r'([A-Za-z_]\w*)(.*)$', key)
        if not s0 or s0.group(1) not in sym or not re.fullmatch(r'[\s0-9a-fA-FxX+/*]*', s0.group(2)):
            continue
        word = sym[s0.group(1)] + (eval(s0.group(2).replace('/', '//') or '0', {'__builtins__': {}}) if s0.group(2).strip() else 0)
        out.setdefault(word * 4, name)
    return out


class Names:
    def __init__(self, rsx_dir):
        self.exact = load_names(rsx_dir)
        self.display = load_display_names(rsx_dir)
        self.arrays = [a for a in ARRAYS if a[0] < 0x2000]
        # NV308A_COLOR is 0x400 words long; NV3089 / NV0039 / NV3062 are exact names only.
        self.arrays.append((0xA400, 0x700 // 4, 4, 1, None))

    def name(self, off):
        """Canonical name of a composite offset (subchannel << 13 | method), or None if unknown.
        RPCS3's get_method_name string when it has one, so all three ports print the same names."""
        if off in self.display:
            return self.display[off]
        return self.sym_name(off)

    def sym_name(self, off):
        """gcm_enums.h symbol (with [unit] for the register arrays)."""
        if off in self.exact:
            return self.exact[off]
        for base, units, stride, words, lab in self.arrays:
            if base <= off < base + units * stride:
                rel = off - base
                u, w = rel // stride, rel % stride
                if lab:                        # per-unit blocks: name of the field at unit 0
                    nm = self.exact.get(base + w)
                    return f'{nm}[{u}]' if nm else None
                nm = self.exact.get(base)
                if nm is None:
                    return None
                return f'{nm}[{rel // 4}]'
        return None

    def base_name(self, off):
        n = self.sym_name(off)
        return re.sub(r'\[\d+\]$', '', n) if n else None

    def known(self, off):
        return self.sym_name(off) is not None     # gcm_enums.h registers; names may come from RPCS3's table


FEATURES = [
    (r'BLEND|COLOR_MASK|LOGIC_OP|DITHER', 'blending / colour output'),
    (r'ALPHA_(TEST|FUNC|REF)', 'alpha test'),
    (r'STENCIL', 'stencil'),
    (r'DEPTH|POLY_OFFSET|POLYGON_OFFSET|CLIP_MIN|CLIP_MAX|ZMIN_MAX', 'depth / depth bias'),
    (r'ZCULL|SCULL|ZPASS|REPORT|CLIP_ID', 'zcull / occlusion queries'),
    (r'FOG', 'fog'),
    (r'POINT', 'points / point sprites'),
    (r'LINE', 'lines'),
    (r'ANTI_ALIASING|SURFACE_COMPRESSION|REDUCE_DST', 'MSAA / alpha-to-coverage'),
    (r'USER_CLIP|UCLIP', 'user clip planes'),
    (r'POLYGON_MODE|POLY_SMOOTH|STIPPLE|EDGE_FLAG', 'polygon mode / stipple'),
    (r'SHADE_MODE|FLAT_SHADE|SPECULAR|TWO_SIDE_LIGHT', 'shading model'),
    (r'CULL_FACE|FRONT_FACE', 'culling'),
    (r'SURFACE|WINDOW|SHADER_WINDOW|CONTEXT_DMA_(COLOR|ZETA)', 'render targets'),
    (r'VIEWPORT|SCISSOR|CLEAR_RECT', 'viewport / scissor'),
    (r'CLEAR', 'clears'),
    (r'VERTEX_TEXTURE', 'vertex textures'),
    (r'TEXTURE|TEX_COORD|ANISO|CYLINDRICAL|COLOR_KEY|PARANOID', 'textures / samplers'),
    (r'TRANSFORM|SHADER_PROGRAM|SHADER_CONTROL|SHADER_PACKER|ATTRIB_(INPUT|OUTPUT)|INDEXED_CONSTANT|ATTRIB_COLOR|ATTRIB_TEX', 'shaders / constants'),
    (r'VERTEX_DATA|VERTEX_ATTRIB|FREQUENCY|VERTEX_CACHE|VERTEX_FILE|ARRAY_ELEMENT|INLINE_ARRAY', 'vertex input'),
    (r'BEGIN_END|DRAW_|INDEX_ARRAY|RESTART_INDEX|RENDER_ENABLE', 'draws / conditional render'),
    (r'SEMAPHORE|NOTIFY|REFERENCE|WAIT_FOR_IDLE|NO_OPERATION|PIPE_NOP|INVALIDATE|PM_TRIGGER|SET_OBJECT|CONTEXT_DMA|TIMEOUT|PERFORMANCE|CONTROL0|EXECUTION_MODE', 'sync / misc'),
    (r'^NV0039', 'blit: memory-to-memory (NV0039)'),
    (r'^NV3062', 'blit: 2D surface (NV3062)'),
    (r'^NV309E', 'blit: swizzled surface (NV309E)'),
    (r'^NV308A', 'blit: image from CPU (NV308A)'),
    (r'^NV3089', 'blit: scaled image (NV3089)'),
    (r'^GCM_', 'GCM driver (flip / user command)'),
]


def feature_of(name):
    if not name:
        return '?'
    for rx, f in sorted(FEATURES, key=lambda e: not e[0].startswith('^')):   # class prefixes first
        if re.search(rx, name):
            return f
    return 'other'


# ---------------------------------------------------------------------------------------------
# Header validation
# ---------------------------------------------------------------------------------------------
def header_regs(val, names):
    """Composite register offsets a header writes, or None if it is not a plausible header."""
    # FIFO control words are not method headers (psdevwiki RSXFIFOCommands):
    #   new jump (w & 3) == 1, call (w & 3) == 2, return w == 0x00020000, reserved bits 16-17,
    #   old jump (w & 0xE0000003) == 0x20000000 (top == 1). Non-increment (bit 30, top == 2) is kept.
    if val & 0x00030003:
        return None
    top = val >> 29
    if top not in (0, 2):
        return None
    count = (val >> 18) & 0x7FF
    if count == 0:
        return None
    off = val & 0xFFFC
    if off == 0 or off in (0x2000, 0x4000, 0x6000, 0x8000, 0xA000, 0xC000, 0xE000) and count > 1:
        return None
    if not names.known(off):
        return None
    if top == 2:                                  # non-increment: one register, count writes
        return [off]
    regs = [off + 4 * i for i in range(count)]
    if not all(names.known(r) for r in regs):
        return None
    return regs


# ---------------------------------------------------------------------------------------------
# PPU ELF scan
# ---------------------------------------------------------------------------------------------
def elf_segments(path):
    d = open(path, 'rb').read()
    if d[:4] != b'\x7fELF':
        raise ValueError(f'{path}: not an ELF')
    is64 = d[4] == 2
    if is64:
        phoff, = struct.unpack_from('>Q', d, 0x20)
        phentsize, phnum = struct.unpack_from('>HH', d, 0x36)
    else:
        phoff, = struct.unpack_from('>I', d, 0x1C)
        phentsize, phnum = struct.unpack_from('>HH', d, 0x2A)
    segs = []
    for i in range(phnum):
        o = phoff + i * phentsize
        if is64:
            ptype, flags, off, vaddr, _pa, filesz = struct.unpack_from('>IIQQQQ', d, o)
        else:
            ptype, off, vaddr, _pa, filesz, _ms, flags = struct.unpack_from('>IIIIIII', d, o)
        if ptype == 1 and filesz:
            segs.append((vaddr, flags, d[off:off + filesz]))
    return segs


def scan_ppu_code(blob, vaddr, names, hits):
    import numpy as np
    w = np.frombuffer(blob[:len(blob) // 4 * 4], dtype='>u4').astype(np.uint32)
    op = w >> 26
    # only words near something interesting get the slow per-instruction walk
    interesting = (op == 15) | (op == 14) | (op == 21) | (op == 24) | (op == 25)
    idxs = np.nonzero(interesting)[0]
    known = {}        # reg -> (value, index)
    dyn = {}          # reg -> index of rlwinm/slwi by 18
    last = -100
    WIN = 24
    for i in idxs.tolist():
        if i - last > WIN:
            known.clear(); dyn.clear()
        last = i
        x = int(w[i]); o = x >> 26
        rd = (x >> 21) & 31; ra = (x >> 16) & 31; imm = x & 0xFFFF
        simm = imm - 0x10000 if imm & 0x8000 else imm
        val = None
        if o == 15:                                   # addis / lis
            if ra == 0:
                known[rd] = ((imm << 16) & 0xFFFFFFFF, i); continue
            k = known.get(ra)
            if k and i - k[1] <= WIN:
                val = (k[0] + (imm << 16)) & 0xFFFFFFFF; known[rd] = (val, i)
            else:
                known.pop(rd, None)
        elif o == 14:                                 # addi / li
            if ra == 0:
                known[rd] = (simm & 0xFFFFFFFF, i); continue
            k = known.get(ra)
            if k and i - k[1] <= WIN:
                val = (k[0] + simm) & 0xFFFFFFFF; known[rd] = (val, i)
            else:
                known.pop(rd, None)
        elif o == 24 or o == 25:                      # ori / oris  (rS = rd field, rA = dest)
            k = known.get(rd)
            if k and i - k[1] <= WIN:
                val = (k[0] | (imm if o == 24 else imm << 16)) & 0xFFFFFFFF; known[ra] = (val, i)
            else:
                dj = dyn.get(rd)
                if o == 24 and dj is not None and i - dj <= 8 and imm and not imm & 3:
                    off = imm & 0xFFFC
                    if names.known(off):
                        hits.setdefault(off, {}).setdefault('ppu_dyn', []).append(vaddr + 4 * i)
                known.pop(ra, None)
        elif o == 21:                                 # rlwinm
            sh = (x >> 11) & 31
            if sh == 18:
                dyn[ra] = i
            known.pop(ra, None)
            continue
        if val is not None:
            regs = header_regs(val, names)
            if regs:
                for r in regs:
                    hits.setdefault(r, {}).setdefault('ppu_const', []).append(vaddr + 4 * i)


def scan_data_chains(blob, vaddr, names, hits, src):
    """Runs of >= 3 headers where each one sits right after the previous header's data."""
    import numpy as np
    n = len(blob) // 4
    if n < 4:
        return
    w = np.frombuffer(blob[:n * 4], dtype='>u4').astype(np.uint32)
    known = np.zeros(0x4000, dtype=bool)
    for off in range(4, 0x10000, 4):
        if names.known(off):
            known[off >> 2] = True
    top = w >> 29
    cnt = (w >> 18) & 0x7FF
    ok = ((w & 0x00030003) == 0) & ((top == 0) | (top == 2)) & (cnt > 0) & (cnt <= 64) & known[(w & 0xFFFC) >> 2]
    ok &= (w & 0xFFFC) != 0
    cand = set(np.nonzero(ok)[0].tolist())
    for i in sorted(cand):
        chain = [i]
        j = i
        while True:
            j = j + 1 + int(cnt[j])
            if j in cand:
                chain.append(j)
                if len(chain) > 64:
                    break
            else:
                break
        if len(chain) >= 3:
            for c in chain:
                regs = header_regs(int(w[c]), names)
                if regs:
                    for r in regs:
                        hits.setdefault(r, {}).setdefault(src, []).append(vaddr + 4 * c)


# ---------------------------------------------------------------------------------------------
# SPU lifted C scan
# ---------------------------------------------------------------------------------------------
SPU_RX = re.compile(r'(?:ctx->gpr\[(\d+)\]\s*=\s*)?(?:S_(\d+)\s*=\s*)?spu_(ilhu|iohl|il|ila)\(([^)]*)\)')
SPU_DATA_RX = re.compile(r'invalid SPU encoding 0x([0-9A-Fa-f]{8})')


def scan_spu_c(spu_dir, names, hits):
    files = glob.glob(os.path.join(spu_dir, '**', '*.c'), recursive=True)
    nfiles = 0
    for f in files:
        if os.path.basename(f) in ('jobmods_register.c', 'spu_workloads.c'):
            continue
        nfiles += 1
        rel = os.path.relpath(f, spu_dir)
        mod = rel.split(os.sep)[0]
        known = {}
        with open(f, encoding='utf-8', errors='replace') as fh:
            for ln, line in enumerate(fh, 1):
                if '_spu_func_' in line and line.startswith('void '):
                    known.clear()
                    continue
                if 'spu_' not in line:
                    continue
                dm = SPU_DATA_RX.search(line)
                if dm:
                    regs = header_regs(int(dm.group(1), 16), names)
                    if regs and (int(dm.group(1), 16) & 0xFFFC) >= 0x100:
                        for r in regs:
                            hits.setdefault(r, {}).setdefault('spu_data', []).append(f'{mod}:{ln}')
                for m in SPU_RX.finditer(line):
                    rd = m.group(1) or m.group(2)
                    if rd is None:
                        continue
                    rd = int(rd); kind = m.group(3); args = [a.strip() for a in m.group(4).split(',')]
                    try:
                        if kind == 'ilhu':
                            known[rd] = (int(args[0], 0) & 0xFFFF) << 16
                        elif kind in ('il', 'ila'):
                            known[rd] = int(args[0], 0) & 0xFFFFFFFF
                        elif kind == 'iohl':
                            src = re.match(r'S_(\d+)|ctx->gpr\[(\d+)\]', args[0])
                            s = int(src.group(1) or src.group(2)) if src else rd
                            if s in known:
                                val = known[s] | (int(args[1], 0) & 0xFFFF)
                                known[rd] = val
                                regs = header_regs(val, names)
                                if regs:
                                    for r in regs:
                                        hits.setdefault(r, {}).setdefault('spu_const', []).append(f'{mod}:{ln}')
                            else:
                                known.pop(rd, None)
                    except (ValueError, IndexError):
                        known.pop(rd, None)
    return nfiles


# ---------------------------------------------------------------------------------------------
# R1 status: rsx_commands.c + backend consumption
# ---------------------------------------------------------------------------------------------
# (lo, hi exclusive, evidence regex in rsx_commands.c (or 'gcm:' cellGcmSys.c), backend fields, kind)
# kind: 'state' (stored for the backend), 'action' (does something itself), 'noop' (deliberate no-op)
R1_TABLE = [
    (0x0050, 0x0054, r'method == 0x0050', [], 'action'),
    (0x0060, 0x0070, r'gcm:method == 0x64u \|\| method == 0x68u', [], 'action'),
    (0x0104, 0x0108, r'method == 0x0104', [], 'action'),
    (0x0180, 0x0184, r'method == 0x0180', [], 'action'),
    (0x01A4, 0x01A8, r'method == 0x01A4', [], 'action'),
    (0x01A8, 0x01AC, r'gcm:if \(m == 0x1A8\)', [], 'action'),
    (0x0200, 0x0204, r'case NV4097_SET_SURFACE_CLIP_HORIZONTAL', ['surface_clip_x', 'surface_clip_w'], 'state'),
    (0x0204, 0x0208, r'case NV4097_SET_SURFACE_CLIP_VERTICAL', ['surface_clip_y', 'surface_clip_h'], 'state'),
    (0x0208, 0x020C, r'case NV4097_SET_SURFACE_FORMAT', ['surface_format'], 'state'),
    (0x020C, 0x0210, r'case NV4097_SET_SURFACE_PITCH_A', ['surface_color_pitch'], 'state'),
    (0x0210, 0x0214, r'case NV4097_SET_SURFACE_COLOR_AOFFSET', ['surface_color_offset'], 'state'),
    (0x0214, 0x0218, r'case NV4097_SET_SURFACE_ZETA_OFFSET', ['surface_zeta_offset'], 'state'),
    (0x0218, 0x021C, r'case NV4097_SET_SURFACE_COLOR_BOFFSET', ['surface_color_offset'], 'state'),
    (0x021C, 0x0220, r'case NV4097_SET_SURFACE_PITCH_B', ['surface_color_pitch'], 'state'),
    (0x0220, 0x0224, r'case NV4097_SET_SURFACE_COLOR_TARGET', ['color_target'], 'state'),
    (0x022C, 0x0230, r'case NV4097_SET_SURFACE_PITCH_Z', ['surface_zeta_pitch'], 'state'),
    (0x0280, 0x0284, r'case NV4097_SET_SURFACE_PITCH_C', ['surface_color_pitch'], 'state'),
    (0x0284, 0x0288, r'case NV4097_SET_SURFACE_PITCH_D', ['surface_color_pitch'], 'state'),
    (0x0288, 0x028C, r'case NV4097_SET_SURFACE_COLOR_COFFSET', ['surface_color_offset'], 'state'),
    (0x028C, 0x0290, r'case NV4097_SET_SURFACE_COLOR_DOFFSET', ['surface_color_offset'], 'state'),
    (0x0304, 0x0308, r'method == NV4097_SET_ALPHA_TEST_ENABLE', ['alpha_test_enable'], 'state'),
    (0x0308, 0x030C, r'method == NV4097_SET_ALPHA_FUNC', ['alpha_func'], 'state'),
    (0x030C, 0x0310, r'method == NV4097_SET_ALPHA_REF', ['alpha_ref'], 'state'),
    (0x0310, 0x0314, r'method == NV4097_SET_BLEND_ENABLE', ['blend_enable'], 'state'),
    (0x0314, 0x0318, r'method == NV4097_SET_BLEND_FUNC_SFACTOR', ['blend_sfactor'], 'state'),
    (0x0318, 0x031C, r'method == NV4097_SET_BLEND_FUNC_DFACTOR', ['blend_dfactor'], 'state'),
    (0x0320, 0x0324, r'method == NV4097_SET_BLEND_EQUATION', ['blend_equation'], 'state'),
    (0x0324, 0x0328, r'method == NV4097_SET_COLOR_MASK\b', ['color_mask'], 'state'),
    (0x0328, 0x032C, r'method == NV4097_SET_STENCIL_TEST_ENABLE', ['stencil_test_enable'], 'state'),
    (0x032C, 0x0330, r'case NV4097_SET_STENCIL_MASK', ['stencil_write_mask'], 'state'),
    (0x0330, 0x0334, r'method == NV4097_SET_STENCIL_FUNC\)', ['stencil_func'], 'state'),
    (0x0334, 0x0338, r'method == NV4097_SET_STENCIL_FUNC_REF', ['stencil_ref'], 'state'),
    (0x0338, 0x033C, r'method == NV4097_SET_STENCIL_FUNC_MASK', ['stencil_mask'], 'state'),
    (0x033C, 0x0340, r'method == NV4097_SET_STENCIL_OP_FAIL', ['stencil_op_fail'], 'state'),
    (0x0340, 0x0344, r'method == NV4097_SET_STENCIL_OP_ZFAIL', ['stencil_op_zfail'], 'state'),
    (0x0344, 0x0348, r'method == NV4097_SET_STENCIL_OP_ZPASS', ['stencil_op_zpass'], 'state'),
    (0x0348, 0x034C, r'case NV4097_SET_TWO_SIDED_STENCIL_TEST_ENABLE', ['two_sided_stencil'], 'state'),
    (0x034C, 0x0350, r'case NV4097_SET_BACK_STENCIL_MASK', ['back_stencil_write_mask'], 'state'),
    (0x0350, 0x0354, r'case NV4097_SET_BACK_STENCIL_FUNC:', ['back_stencil_func'], 'state'),
    (0x0354, 0x0358, r'case NV4097_SET_BACK_STENCIL_FUNC_REF', ['back_stencil_ref'], 'state'),
    (0x0358, 0x035C, r'case NV4097_SET_BACK_STENCIL_FUNC_MASK', ['back_stencil_mask'], 'state'),
    (0x035C, 0x0360, r'case NV4097_SET_BACK_STENCIL_OP_FAIL', ['back_stencil_op_fail'], 'state'),
    (0x0360, 0x0364, r'case NV4097_SET_BACK_STENCIL_OP_ZFAIL', ['back_stencil_op_zfail'], 'state'),
    (0x0364, 0x0368, r'case NV4097_SET_BACK_STENCIL_OP_ZPASS', ['back_stencil_op_zpass'], 'state'),
    (0x08C0, 0x08C4, r'method == NV4097_SET_SCISSOR_HORIZONTAL', ['scissor_x', 'scissor_w'], 'state'),
    (0x08C4, 0x08C8, r'method == NV4097_SET_SCISSOR_VERTICAL', ['scissor_y', 'scissor_h'], 'state'),
    (0x08E4, 0x08E8, r'method == NV4097_SET_SHADER_PROGRAM', ['fragment_program_addr', 'shader_program'], 'state'),
    (0x0900, 0x0980, r'process_vertex_texture_method\(state', ['vertex_textures'], 'state'),
    (0x0A00, 0x0A04, r'method == NV4097_SET_VIEWPORT_HORIZONTAL', ['viewport_x', 'viewport_w'], 'state'),
    (0x0A04, 0x0A08, r'method == NV4097_SET_VIEWPORT_VERTICAL', ['viewport_y', 'viewport_h'], 'state'),
    (0x0A20, 0x0A30, r'method >= 0x0A20 && method < 0x0A30', ['viewport_offset'], 'state'),
    (0x0A30, 0x0A40, r'method >= 0x0A30 && method < 0x0A40', ['viewport_scale'], 'state'),
    (0x0A68, 0x0A6C, r'method == 0x0A68u', ['poly_offset_fill'], 'state'),
    (0x0A6C, 0x0A70, r'method == NV4097_SET_DEPTH_FUNC', ['depth_func'], 'state'),
    (0x0A70, 0x0A74, r'method == NV4097_SET_DEPTH_MASK', ['depth_mask'], 'state'),
    (0x0A74, 0x0A78, r'method == NV4097_SET_DEPTH_TEST_ENABLE', ['depth_test_enable'], 'state'),
    (0x0A78, 0x0A7C, r'method == 0x0A78u', ['poly_offset_scale'], 'state'),
    (0x0A7C, 0x0A80, r'method == 0x0A7Cu', ['poly_offset_bias'], 'state'),
    (0x0A80, 0x0B00, r'SCALED4S_M \*/', ['vertex_data4f'], 'state'),
    (0x0B80, 0x0C00, r'method >= NV4097_SET_TRANSFORM_PROGRAM &&', ['vp_ucode'], 'state'),
    (0x1478, 0x147C, r'method == 0x00001478', ['clip_plane_control'], 'state'),
    (0x1500, 0x1600, r'3F_M \*/', ['vertex_data4f'], 'state'),
    (0x1680, 0x16C0, r'Vertex attribute OFFSET', ['vertex_attribs'], 'state'),
    (0x1738, 0x173C, r'method == 0x00001738', ['vertex_data_base_offset'], 'state'),
    (0x173C, 0x1740, r'method == 0x0000173C', ['vertex_data_base_index'], 'state'),
    (0x1740, 0x1780, r'Vertex attribute FORMAT', ['vertex_attribs'], 'state'),
    (0x1800, 0x1804, r'gcm:if \(m == 0x1800\)', [], 'action'),
    (0x1808, 0x180C, r'method == NV4097_SET_BEGIN_END', [], 'action'),
    (0x1814, 0x1818, r'method == NV4097_DRAW_ARRAYS', [], 'action'),
    (0x181C, 0x1820, r'method == NV4097_SET_INDEX_ARRAY_ADDRESS', ['index_array_offset'], 'state'),
    (0x1820, 0x1824, r'method == NV4097_SET_INDEX_ARRAY_DMA', ['index_array_dma'], 'state'),
    (0x1824, 0x1828, r'method == NV4097_DRAW_INDEX_ARRAY', [], 'action'),
    (0x1830, 0x1834, r'method == NV4097_SET_CULL_FACE\)', ['cull_face'], 'state'),
    (0x1834, 0x1838, r'method == NV4097_SET_FRONT_FACE', ['front_face'], 'state'),
    (0x183C, 0x1840, r'method == NV4097_SET_CULL_FACE_ENABLE', ['cull_face_enable'], 'state'),
    (0x1840, 0x1880, r'process_texture_control3\(state', ['control3'], 'state'),
    (0x1880, 0x1900, r'2F_M \*/', ['vertex_data4f'], 'state'),
    (0x1900, 0x1940, r'2S_M \*/', ['vertex_data4f'], 'state'),
    (0x1940, 0x1980, r'4UB_M \*/', ['vertex_data4f'], 'state'),
    (0x1980, 0x1A00, r'4S_M \*/', ['vertex_data4f'], 'state'),
    (0x1C00, 0x1D00, r'method >= 0x1C00u && method < 0x1C00u', ['vertex_data4f'], 'state'),
    (0x1D60, 0x1D64, r'method == NV4097_SET_SHADER_CONTROL', ['shader_control'], 'state'),
    (0x1D6C, 0x1D78, r'method == 0x1D6C', [], 'action'),
    (0x1D7C, 0x1D80, r'method == 0x00001D7C', ['aa_control'], 'state'),
    (0x1D88, 0x1D8C, r'method == 0x1D88', ['shader_window'], 'state'),
    (0x1D8C, 0x1D90, r'method == NV4097_SET_ZSTENCIL_CLEAR_VALUE', ['zstencil_clear_value'], 'clearval'),
    (0x1D90, 0x1D94, r'method == NV4097_SET_COLOR_CLEAR_VALUE', ['color_clear_value'], 'clearval'),
    (0x1D94, 0x1D98, r'method == NV4097_CLEAR_SURFACE', [], 'action'),
    (0x1DAC, 0x1DB0, r'method == 0x1DAC', ['restart_index_enable'], 'state'),
    (0x1DB0, 0x1DB4, r'method == 0x1DB0', ['restart_index'], 'state'),
    (0x1E40, 0x1E80, r'1F_M \*/', ['vertex_data4f'], 'state'),
    (0x1E9C, 0x1EA0, r'method == NV4097_SET_TRANSFORM_PROGRAM_LOAD', ['vp_ucode'], 'state'),
    (0x1EA0, 0x1EA4, r'method == NV4097_SET_TRANSFORM_PROGRAM_START', ['transform_program_start'], 'state'),
    (0x1EE0, 0x1EE4, r'method == 0x00001EE0', ['point_size'], 'state'),
    (0x1EE8, 0x1EEC, r'method == 0x00001EE8', ['point_sprite_control'], 'state'),
    (0x1EFC, 0x1F00, r'method == NV4097_SET_TRANSFORM_CONSTANT_LOAD', ['vertex_constants'], 'state'),
    (0x1F00, 0x1F80, r'L_tconst:', ['vertex_constants'], 'state'),
    (0x1FC0, 0x1FC4, r'method == 0x00001FC0', ['frequency_divider_op'], 'state'),
    (0x1FD8, 0x1FDC, r'method == NV4097_INVALIDATE_L2', [], 'noop'),
    (0x1FF0, 0x1FF4, r'method == NV4097_SET_VERTEX_ATTRIB_INPUT_MASK', ['vertex_attrib_input_mask'], 'state'),
    (0x1FF4, 0x1FF8, r'method == NV4097_SET_VERTEX_ATTRIB_OUTPUT_MASK', ['vertex_attrib_output_mask'], 'state'),
    (0x1FF8, 0x1FFC, r'method == NV4097_SET_TRANSFORM_BRANCH_BITS', ['transform_branch_bits'], 'state'),
    # 2D engine (cellGcmSys.c gcm_2d_method); composite offsets as RPCS3 numbers them
    (0x6184, 0x618C, r'gcm:case 0x0184: case 0x0188: s_gcm2d.dst_dma', [], 'action'),
    (0x6300, 0x6308, r'gcm:case 0x0300: s_gcm2d.color_fmt', [], 'action'),
    (0x630C, 0x6310, r'gcm:case 0x030C: s_gcm2d.dst_offset', [], 'action'),
    (0x8300, 0x8308, r'gcm:if \(method == 0x0300\) \{ s_nv309e.fmt', [], 'action'),
    (0xA304, 0xA30C, r'gcm:case 0x0304: s_gcm2d.point', [], 'action'),
    (0xA30C, 0xA310, r'gcm:case 0x030C: return;', [], 'noop'),
    (0xA400, 0xA800, r'gcm:method >= 0x0400 && method <= 0x07FC', [], 'action'),
    (0xC184, 0xC188, r'gcm:case 0x0184: s_nv3089.src_dma', [], 'action'),
    (0xC198, 0xC19C, r'gcm:case 0x0198: s_nv3089.dst_surf', [], 'action'),
    (0xC2FC, 0xC320, r'gcm:case 0x0300: s_nv3089.fmt', [], 'action'),
    (0xC400, 0xC410, r'gcm:case 0x040C: s_nv3089.in_uv', [], 'action'),
    (0xEB00, 0xEB04, r'gcm:subch == 7 && m == 0x0B00u', [], 'action'),
]
# texture block fields (unit stride 0x20)
R1_TEX_FIELDS = {0x00: 'offset', 0x04: 'format', 0x08: 'address', 0x0C: 'control0', 0x10: 'control1',
                 0x14: 'filter', 0x18: 'image_rect', 0x1C: 'border_color'}
R1_BACKEND_GLOBS = ['rsx_d3d12_backend.c', '*.inc', 'rsx_pso_cache.c', 'rsx_ext_state.h', 'rsx_d3d12_backend.h']


def find_line(lines, rx):
    r = re.compile(rx)
    for i, l in enumerate(lines, 1):
        if r.search(l):
            return i
    return None


class R1Status:
    def __init__(self, repo):
        v = os.path.join(repo, 'ps3recomp', 'libs', 'video')
        self.cmd_path = os.path.join(v, 'rsx_commands.c')
        self.gcm_path = os.path.join(v, 'cellGcmSys.c')
        self.cmd = open(self.cmd_path, encoding='utf-8', errors='replace').read().splitlines()
        self.gcm = open(self.gcm_path, encoding='utf-8', errors='replace').read().splitlines()
        self.backend = []
        for g in R1_BACKEND_GLOBS:
            for f in sorted(glob.glob(os.path.join(v, g))):
                self.backend.append((os.path.basename(f), open(f, encoding='utf-8', errors='replace').read().splitlines()))
        self._field_cache = {}

    def field_use(self, field):
        if field in self._field_cache:
            return self._field_cache[field]
        rx = re.compile(r'(->|\.)' + re.escape(field) + r'\b')
        res = None
        for name, lines in self.backend:
            for i, l in enumerate(lines, 1):
                if rx.search(l):
                    res = f'{name}:{i}'
                    break
            if res:
                break
        self._field_cache[field] = res
        return res

    def status(self, off):
        """-> (status, evidence)"""
        if 0x1A00 <= off < 0x1C00:
            f = R1_TEX_FIELDS[(off - 0x1A00) % 0x20]
            ln = find_line(self.cmd, r'case 0x%02X: /\* TEXTURE_' % ((off - 0x1A00) % 0x20))
            use = self.field_use(f) if f not in ('offset', 'format') else 'rsx_d3d12_backend.c'
            ev = f'rsx_commands.c:{ln}'
            if f == 'border_color' and not self.field_use('border_color'):
                return 'stored-ignored', ev + ' (tex->border_color never read by the backend)'
            return ('handled' if use else 'stored-ignored'), ev + (f' -> {use}' if use else '')
        for lo, hi, rx, fields, kind in R1_TABLE:
            if lo <= off < hi:
                if rx.startswith('gcm:'):
                    ln = find_line(self.gcm, rx[4:]); ev = f'cellGcmSys.c:{ln}'
                else:
                    ln = find_line(self.cmd, rx); ev = f'rsx_commands.c:{ln}'
                if kind in ('action', 'clearval'):
                    return 'handled', ev
                if kind == 'noop':
                    return 'noop', ev
                uses = [(f, self.field_use(f)) for f in fields]
                used = [u for f, u in uses if u]
                if used:
                    return 'handled', f'{ev} -> {used[0]}'
                return 'stored-ignored', f'{ev} (backend never reads {"/".join(fields)})'
        if off >= 0x2000:
            ln = find_line(self.gcm, r'static void gcm_2d_method')
            return 'unhandled', f'cellGcmSys.c:{ln} (gcm_2d_method drops it)'
        ln = find_line(self.cmd, r'Unrecognized method')
        return 'unhandled', f'rsx_commands.c:{ln} (falls through, returns -1)'


# ---------------------------------------------------------------------------------------------
# RPCS3 status (R2 / R3 trees)
# ---------------------------------------------------------------------------------------------
RPCS3_USER_GLOBS = ['VK/*.cpp', 'VK/*.h', 'Common/*.cpp', 'Common/*.h', 'Core/*.cpp', 'Core/*.h',
                    'Program/*.cpp', 'Program/*.h', 'RSXThread.cpp', 'RSXThread.h', 'RSXTexture.cpp',
                    'RSXZCULL.cpp', 'rsx_vertex_data.cpp', 'NV47/HW/*.cpp', 'NV47/HW/*.h',
                    'rsx_methods.cpp', 'RSXOffload.cpp', 'rsx_cache.h', 'Host/*.cpp']


class RpcsStatus:
    def __init__(self, repo, names):
        self.repo = repo
        self.rsx = os.path.join(repo, RSX_REL)
        self.names = names
        self.mcpp_path = os.path.join(self.rsx, 'rsx_methods.cpp')
        mcpp = open(self.mcpp_path, encoding='utf-8', errors='replace').read().splitlines()
        self.mcpp = mcpp
        start = find_line(mcpp, r's_methods_init = \[\]')
        self.method_state = {}       # name -> ('nullptr'|'bind', line)
        for i in range(start, len(mcpp)):
            l = mcpp[i]
            m = re.match(r'\s*methods\[(\w+)\]\s*=\s*nullptr', l)
            if m:
                self.method_state[m.group(1)] = ('nullptr', i + 1); continue
            m = re.match(r'\s*bind(?:_array|_range<)?\(?\s*(\w+)', l)
            if m and m.group(1) not in ('id',):
                self.method_state[m.group(1)] = ('bind', i + 1); continue
            m = re.match(r'\s*bind_range<\s*(\w+)', l)
            if m:
                self.method_state[m.group(1)] = ('bind', i + 1)
        # accessors in rsx_methods.h: function name -> registers it decodes
        mh = open(os.path.join(self.rsx, 'rsx_methods.h'), encoding='utf-8', errors='replace').read().splitlines()
        self.reg_accessors = {}      # NV name -> [(accessor, line)]
        cur = None
        for i, l in enumerate(mh, 1):
            fm = re.match(r'\s*(?:[\w:<>,\s\*&]+?)\s+(\w+)\s*\([^;]*\)\s*(const)?\s*$', l)
            if fm and fm.group(1) not in ('if', 'for', 'while', 'switch', 'return'):
                cur = fm.group(1)
            for nm in re.findall(r'(?:decode<|registers\[)\s*(NV[0-9A-F]{4}_\w+)', l):
                self.reg_accessors.setdefault(nm, []).append((cur, i))
        self.users = []
        for g in RPCS3_USER_GLOBS:
            for f in sorted(glob.glob(os.path.join(self.rsx, g))):
                try:
                    self.users.append((os.path.relpath(f, self.rsx), open(f, encoding='utf-8', errors='replace').read().splitlines()))
                except OSError:
                    pass
        self._use = {}

    def use_of(self, token):
        if token in self._use:
            return self._use[token]
        rx = re.compile(r'\b' + re.escape(token) + r'\b')
        res = None
        for name, lines in self.users:
            for i, l in enumerate(lines, 1):
                if rx.search(l) and not re.match(r'\s*case\s', l) and not re.search(r'registers\[\w+\]\s*=[^=]', l) and not (name == 'rsx_methods.cpp' and re.search(r'methods\[|state_signals|bind', l)):
                    res = f'{name}:{i}'
                    break
            if res:
                break
        self._use[token] = res
        return res

    def status(self, off):
        nm = self.names.base_name(off)
        if nm is None:
            return 'invalid', ''
        ms = self.method_state.get(nm)
        if ms and ms[0] == 'bind':
            return 'handler', f'rsx_methods.cpp:{ms[1]}'
        if ms is None:
            ln = find_line(self.mcpp, r'methods\.fill\(&invalid_method\)')
            return 'invalid', f'rsx_methods.cpp:{ln} (invalid_method)'
        # register store; who reads it?
        for acc, line in self.reg_accessors.get(nm, []):
            if acc:
                u = self.use_of(acc)
                if u:
                    return 'consumed', f'rsx_methods.h:{line} {acc}() -> {u}'
        u = self.use_of(nm)
        if u:
            return 'consumed', f'{u} (reads {nm})'
        return 'stored-ignored', f'rsx_methods.cpp:{ms[1]} (nullptr; no renderer reads it)'


def rpcs3_port_bypasses(repo):
    """Env-gated skips the port added to its RPCS3 tree's RSX method handling (from git diff)."""
    import subprocess
    tree = os.path.join(repo, 'ps3recomp', 'third_party', 'rpcs3')
    out = []
    try:
        diff = subprocess.run(['git', '-C', tree, 'diff', '-U0', '--', 'rpcs3/Emu/RSX/rsx_methods.cpp',
                               'rpcs3/Emu/RSX/RSXFIFO.cpp', 'rpcs3/Emu/RSX/NV47/HW/nv4097.cpp',
                               'rpcs3/Emu/RSX/NV47/HW/nv3089.cpp', 'rpcs3/Emu/RSX/NV47/HW/nv0039.cpp',
                               'rpcs3/Emu/RSX/NV47/HW/nv308a.cpp', 'rpcs3/Emu/RSX/NV47/HW/nv406e.cpp'],
                              capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=120).stdout
    except Exception as e:      # noqa: BLE001
        return [f'(git diff failed: {e})']
    f = ln = None
    for l in diff.splitlines():
        if l.startswith('+++ '):
            f = l[6:]
        elif l.startswith('@@'):
            m = re.search(r'\+(\d+)', l); ln = int(m.group(1)) if m else 0
        elif l.startswith('+') and not l.startswith('+++'):
            if re.search(r'(getenv|enabled|_on\b|skip|bypass|return;)', l) and re.search(r'R2|R3|PORT|port|getenv|r2', l):
                out.append(f'{os.path.basename(f)}:{ln}: {l[1:].strip()[:140]}')
            ln += 1
    return out


# ---------------------------------------------------------------------------------------------
def scan_game(game, names):
    repo = REPOS[game]
    hits = {}
    info = {'game': game}
    elf = None
    for c in ('game/EBOOT.elf', 'game/game.elf'):
        p = os.path.join(repo, c)
        if os.path.exists(p):
            elf = p; break
    info['elf'] = os.path.relpath(elf, repo).replace(os.sep, '/') if elf else None
    if elf:
        for vaddr, flags, blob in elf_segments(elf):
            if flags & 1:
                scan_ppu_code(blob, vaddr, names, hits)
            scan_data_chains(blob, vaddr, names, hits, 'elf_data')
    info['spu_files'] = scan_spu_c(os.path.join(repo, 'project', 'spu'), names, hits)
    return hits, info


def status_tables(names):
    r1 = R1Status(REPOS['r1'])
    rp = {g: RpcsStatus(REPOS[g], names) for g in ('r2', 'r3') if os.path.isdir(os.path.join(REPOS[g], RSX_REL))}
    return r1, rp


def all_known_offsets(names):
    return [o for o in range(0, 0x10000, 4) if names.known(o)]


def build_rows(game, hits, names, r1, rp):
    rows = []
    for off in sorted(hits):
        h = hits[off]
        nm = names.name(off)
        ev = {k: len(v) for k, v in h.items()}
        sample = next(iter(h.values()))[0]
        row = {'off': off, 'name': nm, 'feature': feature_of(names.base_name(off)), 'static': ev,
               'sample': hex(sample) if isinstance(sample, int) else sample}
        row['r1'] = r1.status(off)
        for g, st in rp.items():
            row[g] = st.status(off)
        rows.append(row)
    return rows


def confidence(ev):
    strong = ev.get('ppu_const', 0) + ev.get('spu_const', 0) + ev.get('elf_data', 0)
    if strong >= 2 or ev.get('elf_data', 0):
        return 'high'
    if strong == 1 or ev.get('ppu_dyn', 0) >= 2:
        return 'med'
    return 'low'


def group_name(name):
    """NV4097_SET_TEXTURE_FORMAT[3] / NV4097_SET_TRANSFORM_PROGRAM + 4 / ... + 0x20 -> one group."""
    return re.sub(r'\s*\+\s*(0x[0-9A-Fa-f]+|\d+)$', '[]', re.sub(r'\[\d+\]', '[]', name))


def collapse(rows):
    """One line per method base (TEX[0..15] FORMAT -> one row), worst status kept."""
    order = {'unhandled': 0, 'invalid': 0, 'stored-ignored': 1, 'noop': 2, 'handled': 3, 'handler': 3, 'consumed': 3}
    out = {}
    for r in rows:
        base = group_name(r['name'] or hex(r['off']))
        o = out.get(base)
        if not o:
            o = dict(r); o['name'] = base; o['static'] = dict(r['static']); o['units'] = 1
            out[base] = o
            continue
        o['units'] += 1
        for k, v in r['static'].items():
            o['static'][k] = o['static'].get(k, 0) + v
        for g in ('r1', 'r2', 'r3'):
            if g in r and order.get(r[g][0], 3) < order.get(o[g][0], 3):
                o[g] = r[g]
    return sorted(out.values(), key=lambda r: r['off'])


def md_table(game, rows, info):
    L = [f'### {game.upper()} — methods the game code builds ({len(rows)} method groups)', '',
         f'ELF: `{info.get("elf")}`; lifted SPU files: {info.get("spu_files")}', '',
         '| method | name | feature | static evidence (conf) | R1 | R2 | R3 |', '|---|---|---|---|---|---|---|']
    for r in rows:
        ev = ', '.join(f'{k}={v}' for k, v in sorted(r['static'].items()))
        cells = []
        for g in ('r1', 'r2', 'r3'):
            st = r.get(g)
            cells.append(f'**{st[0]}** {st[1]}' if st else '-')
        L.append(f'| 0x{r["off"]:04X} | {r["name"]} | {r["feature"]} | {ev} ({confidence(r["static"])}) | ' + ' | '.join(cells) + ' |')
    return '\n'.join(L)


def gap_list(game, rows):
    port = 'r1' if game == 'r1' else game
    bad = [r for r in rows if r.get(port) and r[port][0] in ('unhandled', 'invalid', 'stored-ignored', 'noop')]
    L = [f'#### {game.upper()} static gaps (used by the game code, not fully handled by its own renderer)', '']
    for r in bad:
        L.append(f'- 0x{r["off"]:04X} {r["name"]} ({r["feature"]}; {confidence(r["static"])}): '
                 f'{r[port][0]} — {r[port][1]}')
    if not bad:
        L.append('- none')
    return '\n'.join(L)


def r1_shader_masks(repo):
    """Opcodes R1's own decompilers translate (rsx_fp_decompiler.c / rsx_vp_decompiler.c case lists).
    FP value = opcode | branch << 6 (as in the [featcov] fp_op lines)."""
    v = os.path.join(repo, 'ps3recomp', 'libs', 'video')
    fp = open(os.path.join(v, 'rsx_fp_decompiler.c'), encoding='utf-8', errors='replace').read()
    ops = {n: int(x, 16) for n, x in re.findall(r'\b(OP_\w+)\s*=\s*0x([0-9A-Fa-f]+)', fp)}
    fp_ok = 0
    for m in re.finditer(r'switch \(opcode\) \{(.*?)\n\s*default:', fp, re.S):
        body = m.group(1)
        if 'case OP_MOV' in body:
            for n in re.findall(r'case (OP_\w+)\s*:', body):
                if n in ops:
                    fp_ok |= 1 << ops[n]
        elif 'IFE' in body:                  # flow control (branch bit set)
            for x in re.findall(r'case 0x([0-9A-Fa-f]+):', body):
                fp_ok |= 1 << (64 + int(x, 16))
            fp_ok |= 1 << 64                 # BRK (its own case after the IFE/LOOP/REP group)
    if 'case OP_KIL' in fp:
        fp_ok |= 1 << ops['OP_KIL']
    vp = open(os.path.join(v, 'rsx_vp_decompiler.c'), encoding='utf-8', errors='replace').read()
    vec_ok = sca_ok = 1                      # NOP
    m = re.search(r'switch \(vec_op\) \{(.*?)\n\s*default:', vp, re.S)
    for x in re.findall(r'case 0x([0-9A-Fa-f]+):', m.group(1) if m else ''):
        vec_ok |= 1 << int(x, 16)
    m = re.search(r'switch \(sca_op\) \{(.*?)\n\s*default:', vp, re.S)
    for x in re.findall(r'case 0x([0-9A-Fa-f]+):', m.group(1) if m else ''):
        sca_ok |= 1 << int(x, 16)
    for x in (0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x11, 0x12):   # flow_term: BRA BRI CAL CLI RET BRB CLB
        if re.search(r'sca_op == 0x%02X' % x, vp) or x == 0x12:
            sca_ok |= 1 << x
    return fp_ok & ((1 << 64) - 1), fp_ok >> 64, vec_ok, sca_ok


def emit_header(path, names, r1, rp):
    """rsx_featcov_tables.h: method names + per-port status for the runtime tracer."""
    codes = {'handled': 0, 'handler': 0, 'consumed': 0, 'noop': 1, 'stored-ignored': 2, 'unhandled': 3, 'invalid': 3}
    offs = all_known_offsets(names)
    rp_any = rp.get('r3') or rp.get('r2')
    lines = ['/* rsx_featcov_tables.h -- GENERATED by scripts/featcov_static.py --emit-header; do not edit.',
             ' * Identical in all three ports. Per composite method offset (subchannel << 13 | method):',
             ' * the RPCS3 name and the static handler status for R1 (rsx_commands.c + D3D12 backend) and',
             ' * RPCS3 (R2/R3: methods[] + renderer reads). Status: 0 real, 1 no-op, 2 stored but ignored,',
             ' * 3 unhandled/invalid. Offsets not listed are unknown registers (status 3 on both). */',
             '#ifndef RSX_FEATCOV_TABLES_H', '#define RSX_FEATCOV_TABLES_H', '',
             'typedef struct { unsigned short off; unsigned char r1, rpcs3; const char* name; } rsx_featcov_meth;',
             'static const rsx_featcov_meth k_rsx_featcov_meths[] = {']
    for o in offs:
        s1 = codes[r1.status(o)[0]]
        s2 = codes[rp_any.status(o)[0]] if rp_any else 3
        lines.append(f'    {{0x{o:04X}, {s1}, {s2}, "{names.name(o)}"}},')
    lines += ['};', f'#define RSX_FEATCOV_NMETHS {len(offs)}', '']
    fp_ok, fp_br_ok, vec_ok, sca_ok = r1_shader_masks(REPOS['r1'])
    lines += ['/* R1 decompiler coverage (rsx_fp_decompiler.c / rsx_vp_decompiler.c case lists): bit n = opcode n',
              ' * translated. FP branch ops (BRK CAL IFE LOOP REP RET = 0..5) in their own mask. */',
              f'#define RSX_FEATCOV_R1_FP_OK      0x{fp_ok:016X}ull',
              f'#define RSX_FEATCOV_R1_FP_BRANCH_OK 0x{fp_br_ok:02X}u',
              f'#define RSX_FEATCOV_R1_VP_VEC_OK  0x{vec_ok:08X}u',
              f'#define RSX_FEATCOV_R1_VP_SCA_OK  0x{sca_ok:08X}u', '', '#endif', '']
    with open(path, 'w', newline='\n') as f:
        f.write('\n'.join(lines))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--game', choices=['r1', 'r2', 'r3'], default=game_of(REPO))
    ap.add_argument('--all', action='store_true', help='scan all three games')
    ap.add_argument('--md', help='write markdown tables here')
    ap.add_argument('--emit-header', help='write rsx_featcov_tables.h here')
    ap.add_argument('--json-dir', default=HERE)
    a = ap.parse_args()
    rsx = os.path.join(REPOS['r3'], RSX_REL)
    if not os.path.isdir(rsx):
        rsx = os.path.join(REPO, RSX_REL)
    names = Names(rsx)
    r1, rp = status_tables(names)
    if a.emit_header:
        emit_header(a.emit_header, names, r1, rp)
        print(f'wrote {a.emit_header}')
        if not a.md and not a.all:
            return 0
    md = []
    for g in (['r1', 'r2', 'r3'] if a.all else [a.game]):
        hits, info = scan_game(g, names)
        rows = build_rows(g, hits, names, r1, rp)
        rows_c = collapse(rows)
        out = {'game': g, 'info': info, 'rows': rows, 'groups': rows_c}
        jp = os.path.join(a.json_dir if not a.all else os.path.join(REPOS[g], 'scripts'), f'featcov_static_{g}.json')
        with open(jp, 'w') as f:
            json.dump(out, f, indent=1)
        print(f'{g}: {len(rows)} registers / {len(rows_c)} method groups -> {jp}')
        md.append(gap_list(g, rows_c))
        md.append(md_table(g, rows_c, info))
    if a.all:
        byp = {}
        for g in ('r2', 'r3'):
            byp[g] = rpcs3_port_bypasses(REPOS[g])
        md.append('#### RPCS3 port changes that gate / skip method handling (git diff of the RPCS3 tree)')
        for g, l in byp.items():
            md.append(f'\n{g.upper()}:\n' + ('\n'.join(f'- {x}' for x in l) if l else '- none'))
    if a.md:
        with open(a.md, 'w', encoding='utf-8') as f:
            f.write('\n\n'.join(md) + '\n')
        print(f'wrote {a.md}')
    else:
        print('\n\n'.join(md[:1]))
    return 0


if __name__ == '__main__':
    sys.exit(main())
