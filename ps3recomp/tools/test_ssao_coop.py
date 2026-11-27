"""R1 PC parity features that cannot be checked by the build:

1. rsx_ssao.inc: the SSAO HLSL is compiled at run time (D3DCompile), so a syntax error would only show in game.
   Every entry point (vs, ps_ao, ps_apply) is compiled here with d3dcompiler_47.dll, as the backend does.
2. cellPad.c pad_coop_order: which connected controller slot feeds which pad port with PAD_P1_CONTROLLER /
   PAD_P2_CONTROLLER (local co-op), and with co-op off which controller is player 1 (the one used last,
   pad_sdl3_busy). The functions are cut out of cellPad.c and run with a small C driver.
3. cellPad.c pad_bg_neutral (PAD_BACKGROUND_INPUT off): every connected port goes neutral, also when port 0 is
   empty (co-op: a pinned player 2 alone), and both controller polls call it outside the port-0 guard;
   gyro look stops too (test_pad_gyro_bg.py).

SKIP parts when d3dcompiler_47.dll or a C compiler (clang / cl) is not there.
"""
import ctypes
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
LIBS = os.path.normpath(os.path.join(HERE, "..", "libs"))
SSAO = os.path.join(LIBS, "video", "rsx_ssao.inc")
CELLPAD = os.path.join(LIBS, "input", "cellPad.c")


def c_string_literal(src, name):
    m = re.search(r"static const char " + name + r"\[\]\s*=\s*((?:\s*\"(?:[^\"\\]|\\.)*\"\s*(?:/\*.*?\*/)?)+);", src, re.S)
    assert m, name
    parts = re.findall(r"\"((?:[^\"\\]|\\.)*)\"", m.group(1))
    return "".join(parts).encode("latin-1").decode("unicode_escape")


class ShaderMacro(ctypes.Structure):
    _fields_ = [("name", ctypes.c_char_p), ("definition", ctypes.c_char_p)]


def compile_hlsl(code, entry, target, macros=()):
    try:
        dc = ctypes.WinDLL("d3dcompiler_47.dll")
    except OSError:
        return None
    blob, err = ctypes.c_void_p(), ctypes.c_void_p()
    src = code.encode("ascii")
    mac = (ShaderMacro * (len(macros) + 1))(*[ShaderMacro(k.encode(), v.encode()) for k, v in macros])
    hr = dc.D3DCompile(src, ctypes.c_size_t(len(src)), b"rsx_ssao", mac, None, entry.encode(), target.encode(), 0, 0,
                       ctypes.byref(blob), ctypes.byref(err))
    msg = ""
    if err.value:
        vt = ctypes.cast(ctypes.cast(err, ctypes.POINTER(ctypes.c_void_p))[0], ctypes.POINTER(ctypes.c_void_p))
        ptr = ctypes.WINFUNCTYPE(ctypes.c_void_p, ctypes.c_void_p)(vt[3])(err)
        msg = ctypes.string_at(ptr).decode("latin-1", "replace")
    return (hr & 0xFFFFFFFF) == 0, msg


def test_ssao_hlsl():
    code = c_string_literal(open(SSAO, encoding="utf-8").read(), "s_ao_hlsl")
    for entry, target in (("vs", "vs_5_0"), ("ps_ao", "ps_5_0"), ("ps_apply", "ps_5_0"), ("ps_apply1", "ps_5_0")):
        r = compile_hlsl(code, entry, target)
        if r is None:
            print("SKIP ssao hlsl: no d3dcompiler_47.dll")
            return
        ok, msg = r
        assert ok, f"{entry}: {msg}"
        print(f"ssao hlsl {entry} ({target}) ok")


def test_cacao_hlsl():
    """rsx_cacao.inc (RFOM_AO_MODE=cacao): every entry, ps_gen at each quality preset (cacao_pso's macros)."""
    code = c_string_literal(open(os.path.join(LIBS, "video", "rsx_cacao.inc"), encoding="utf-8").read(), "s_cc_hlsl")
    jobs = [("vs", "vs_5_0", 0), ("ps_prepare", "ps_5_0", 0), ("ps_blur", "ps_5_0", 0), ("ps_apply", "ps_5_0", 0)]
    jobs += [("ps_gen", "ps_5_0", q) for q in (0, 1, 2)]
    for entry, target, q in jobs:
        r = compile_hlsl(code, entry, target, (("Q", str(q)), ("NUM_TAPS", str((3, 5, 12)[q]))))
        if r is None:
            print("SKIP cacao hlsl: no d3dcompiler_47.dll")
            return
        ok, msg = r
        assert ok, f"{entry} Q={q}: {msg}"
        print(f"cacao hlsl {entry} Q={q} ({target}) ok")


DRIVER = r"""
#include <stdio.h>
#include <string.h>
typedef struct { int connected; unsigned short buttons; unsigned char lt, rt; short lx, ly, rx, ry; } PadxState;
%s
%s
static int run(const char* conn_s, int want, int p2, const char* expect)
{
    int conn[8] = {0}, order[8], n = 0;
    for (int i = 0; conn_s[i]; i++) conn[i] = conn_s[i] == '1';
    pad_coop_order(conn, NULL, 8, want, p2, NULL, order, &n);
    char got[32] = {0};
    for (int i = 0; i < n; i++) got[i] = order[i] < 0 ? '-' : (char)('1' + order[i]);   /* '-' = port left empty */
    if (strcmp(got, expect)) { printf("FAIL conn=%%s p1=%%d p2=%%d: got '%%s' want '%%s'\n", conn_s, want + 1, p2 + 1, got, expect); return 1; }
    return 0;
}
/* One player (co-op off): busy_s = controllers in use this poll, *last carried between polls. */
static int run1(const char* conn_s, const char* busy_s, int want, int* last, const char* expect)
{
    int conn[8] = {0}, busy[8] = {0}, order[8], n = 0;
    for (int i = 0; conn_s[i]; i++) conn[i] = conn_s[i] == '1';
    for (int i = 0; busy_s[i]; i++) busy[i] = busy_s[i] == '1';
    pad_coop_order(conn, busy, 8, want, -1, last, order, &n);
    char got[32] = {0};
    for (int i = 0; i < n; i++) got[i] = order[i] < 0 ? '-' : (char)('1' + order[i]);
    if (strcmp(got, expect)) { printf("FAIL single conn=%%s busy=%%s: got '%%s' want '%%s'\n", conn_s, busy_s, got, expect); return 1; }
    return 0;
}
static int busy_case(int lx, unsigned short buttons, unsigned char lt, int expect)
{
    PadxState s; memset(&s, 0, sizeof s);
    s.connected = 1; s.lx = (short)lx; s.buttons = buttons; s.lt = lt;
    if (pad_sdl3_busy(&s) != expect) { printf("FAIL busy lx=%%d buttons=%%u lt=%%u\n", lx, buttons, lt); return 1; }
    return 0;
}
int main(void)
{
    int f = 0;
    f += run("11100000", -1, -1, "123");   /* automatic: connection order */
    f += run("11100000", -1,  2, "132");   /* player 2 = controller 3 */
    f += run("11100000",  1,  2, "231");   /* player 1 = 2, player 2 = 3 */
    f += run("11100000",  1, -1, "213");   /* player 1 only */
    f += run("11100000",  0,  0, "123");   /* same controller twice: player 2 falls back to automatic */
    f += run("10100000", -1,  1, "13");    /* player 2's controller not connected */
    f += run("00100000", -1,  2, "-3");    /* only player 2's controller: it stays player 2, port 0 empty */
    f += run("00100000",  0,  2, "-3");    /* player 1's pinned controller missing: same */
    f += run("00100000", -1, -1, "3");     /* no player 2 pinned (or co-op off): the only controller is player 1 */
    f += run("00100001", -1,  2, "83");    /* another controller connected: it is player 1 */
    f += run("10100000",  0,  2, "13");    /* both pinned and connected */
    f += run("01100000", -1,  1, "32");    /* automatic player 1 skips the controller reserved for player 2 */
    f += run("00000000",  0,  1, "");      /* nothing connected */
    /* One player: player 1 is the controller used last (the caller keeps only port 0). */
    int last = -1;
    f += run1("11000000", "00000000", -1, &last, "12");   /* nobody playing yet: the first one */
    f += run1("11000000", "11000000", -1, &last, "12");   /* both busy at once (a mirror): no switch */
    f += run1("11000000", "01000000", -1, &last, "21");   /* the other one is used while player 1's is idle */
    f += run1("11000000", "00000000", -1, &last, "21");   /* idle: it stays */
    f += run1("11000000", "11000000", -1, &last, "21");   /* mirror again: still no switch */
    f += run1("11000000", "10000000", -1, &last, "12");   /* back to the first one */
    f += run1("01000000", "00000000", -1, &last, "2");    /* it is unplugged: the other one takes over */
    f += run1("11000000", "00000000", -1, &last, "21");   /* plugged back: no change until it is used */
    last = -1;
    f += run1("11000000", "01000000",  0, &last, "12");   /* PAD_P1_CONTROLLER pins player 1 */
    f += busy_case(0, 0, 0, 0);            /* resting */
    f += busy_case(12000, 0, 0, 0);        /* stick drift / a light touch doesn't count */
    f += busy_case(-30000, 0, 0, 1);       /* a stick pushed far does */
    f += busy_case(0, 0x1000, 0, 1);       /* a button */
    f += busy_case(0, 0, 200, 1);          /* a trigger */
    printf(f ? "coop order: %%d failures\n" : "coop order ok\n", f);
    return f != 0;
}
"""


def find_cc():
    llvm = os.path.join(os.environ.get("ProgramFiles", r"C:\Program Files"), "LLVM", "bin")
    for c in (os.environ.get("CC"), "clang", "gcc", os.path.join(llvm, "clang.exe"), "clang-cl",
              os.path.join(llvm, "clang-cl.exe"), "cl"):
        if c and (os.path.isfile(c) or shutil.which(c)):
            return shutil.which(c) or c
    return None


def test_coop_order():
    src = open(CELLPAD, encoding="utf-8", errors="replace").read()
    m = re.search(r"static void pad_coop_order\(.*?\n\}\n", src.replace("\r\n", "\n"), re.S)
    assert m, "pad_coop_order not found in cellPad.c"
    b = re.search(r"static int pad_sdl3_busy\(.*?\n\}\n", src.replace("\r\n", "\n"), re.S)
    assert b, "pad_sdl3_busy not found in cellPad.c"
    cc = find_cc()
    if not cc:
        print("SKIP coop order: no C compiler")
        return
    work = os.path.join(HERE, "_test_ssao_coop")
    os.makedirs(work, exist_ok=True)
    c = os.path.join(work, "coop.c")
    exe = os.path.join(work, "coop.exe")
    with open(c, "w") as f:
        f.write(DRIVER % (m.group(0), b.group(0)))
    if os.path.basename(cc).lower() in ("cl.exe", "clang-cl.exe", "cl", "clang-cl"):
        cmd = [cc, "/nologo", "/W3", c, "/Fe" + exe, "/Fo" + work + os.sep]
    else:
        cmd = [cc, "-O1", "-Wall", c, "-o", exe]
    r = subprocess.run(cmd, capture_output=True, text=True)
    assert r.returncode == 0, r.stdout + r.stderr
    r = subprocess.run([exe], capture_output=True, text=True)
    print(r.stdout.strip())
    shutil.rmtree(work, ignore_errors=True)
    assert r.returncode == 0, r.stdout


BG_DRIVER = r"""
#include <stdio.h>
#include <string.h>
#define PAD_MAX_HOST_PORTS 4
typedef unsigned char u8;
typedef unsigned short u16;
typedef struct { int connected; u16 buttons; u8 analog_lx, analog_ly, analog_rx, analog_ry; u8 press_r1; } PadHostState;
static PadHostState s_host_state[PAD_MAX_HOST_PORTS];
%s
int main(void)
{
    int f = 0;
    /* port 0 empty (keyboard player 1), player 2's controller on port 1 holding buttons and a stick */
    s_host_state[1].connected = 1; s_host_state[1].buttons = 0x0808; s_host_state[1].analog_lx = 255; s_host_state[1].press_r1 = 255;
    pad_bg_neutral(1);
    if (s_host_state[1].buttons != 0x0808) { printf("FAIL focused: input dropped\n"); f++; }
    pad_bg_neutral(0);
    const PadHostState* h = &s_host_state[1];
    if (!h->connected || h->buttons || h->press_r1 || h->analog_lx != 128 || h->analog_ry != 128) { printf("FAIL background: port 1 not neutral\n"); f++; }
    if (s_host_state[0].connected || s_host_state[0].analog_lx) { printf("FAIL background: empty port 0 touched\n"); f++; }
    printf(f ? "bg neutral: %%d failures\n" : "bg neutral ok\n", f);
    return f != 0;
}
"""


def _func(src, sig):
    m = re.search(re.escape(sig) + r".*?\n\}\n", src, re.S)
    assert m, sig + " not found in cellPad.c"
    return m.group(0)


def test_bg_neutral():
    src = open(CELLPAD, encoding="utf-8", errors="replace").read().replace("\r\n", "\n")
    neutral = _func(src, "static void pad_bg_neutral(int allowed)")
    assert "pad_bg_input_ok" not in _func(src, "static void pad_port0_post(void)"), "background clearing still in pad_port0_post"
    for poll in ("static void pad_poll_xinput(void)", "static void pad_poll_sdl3(void)"):
        body = _func(src, poll)
        assert re.search(r"\n    pad_bg_neutral\(pad_bg_input_ok\(\)\);\n    if \(s_xi_present\[0\]\) pad_port0_post\(\);", body), \
            poll + ": pad_bg_neutral must run unconditionally before the port-0 post"
    run_c(BG_DRIVER % neutral, "bg")
    sys.path.insert(0, HERE)
    import test_pad_gyro_bg   # gyro (pad_gyro_gate) is off in the background too
    test_pad_gyro_bg.test_gyro_gate_background()


def run_c(code, name):
    cc = find_cc()
    if not cc:
        print("SKIP " + name + ": no C compiler")
        return
    work = os.path.join(HERE, "_test_ssao_coop_" + name)
    os.makedirs(work, exist_ok=True)
    c = os.path.join(work, name + ".c")
    exe = os.path.join(work, name + ".exe")
    with open(c, "w") as f:
        f.write(code)
    if os.path.basename(cc).lower() in ("cl.exe", "clang-cl.exe", "cl", "clang-cl"):
        cmd = [cc, "/nologo", "/W3", c, "/Fe" + exe, "/Fo" + work + os.sep]
    else:
        cmd = [cc, "-O1", "-Wall", c, "-o", exe]
    r = subprocess.run(cmd, capture_output=True, text=True)
    assert r.returncode == 0, r.stdout + r.stderr
    r = subprocess.run([exe], capture_output=True, text=True)
    print(r.stdout.strip())
    shutil.rmtree(work, ignore_errors=True)
    assert r.returncode == 0, r.stdout


if __name__ == "__main__":
    test_ssao_hlsl()
    test_cacao_hlsl()
    test_coop_order()
    test_bg_neutral()
    print("OK")
    sys.exit(0)
