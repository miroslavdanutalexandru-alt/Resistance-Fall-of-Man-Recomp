"""cellPad.c pad_gyro_gate: gyro (extra right stick in pad_gyro_poll, direct look in cellpad_gyro_take) must
stop while the window is in the background and PAD_BACKGROUND_INPUT says the controllers may not drive the
game, like the buttons and sticks (pad_bg_input_ok). The function is cut out of cellPad.c and run with stubs.
Same file in the Resistance 1 / 2 / 3 ports (R1's gate takes no port and checks zoom; R2 / R3 check L1 on port p).

SKIP when no C compiler (clang / gcc / clang-cl / cl) is found.
"""
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CELLPAD = os.path.normpath(os.path.join(HERE, "..", "libs", "input", "cellPad.c"))

DRIVER = r"""
#include <stdio.h>
typedef unsigned short u16;
#define CELL_PAD_CTRL_L1 0x0004
typedef struct { int connected; u16 buttons; } PadHostState;
static PadHostState s_host_state[4];
static int g_rfom_input_block, g_port_ui_modal, s_gyro_mode, s_zoomed, s_bg_ok;
static int rfom_is_zoomed(void) { return s_zoomed; }
static int pad_bg_input_ok(void) { return s_bg_ok; }
%s
#define GATE() %s
int main(void)
{
    int f = 0;
    s_gyro_mode = 1; s_bg_ok = 1;
    if (!GATE()) { printf("FAIL focused: gyro gated\n"); f++; }
    s_bg_ok = 0;
    if (GATE()) { printf("FAIL background: gyro still turns the view\n"); f++; }
    s_gyro_mode = 2; s_zoomed = 1; s_host_state[0].buttons = CELL_PAD_CTRL_L1;
    if (GATE()) { printf("FAIL background: gyro (aim mode) still turns the view\n"); f++; }
    s_bg_ok = 1;
    if (!GATE()) { printf("FAIL focused aim mode: gyro gated\n"); f++; }
    g_rfom_input_block = g_port_ui_modal = 1;
    if (GATE()) { printf("FAIL PC menu: gyro not gated\n"); f++; }
    printf(f ? "gyro gate: %%d failures\n" : "gyro gate ok\n", f);
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


def test_gyro_gate_background():
    src = open(CELLPAD, encoding="utf-8", errors="replace").read().replace("\r\n", "\n")
    m = re.search(r"static int pad_gyro_gate\((void|int p)\)\n\{.*?\n\}\n", src, re.S)
    assert m, "pad_gyro_gate not found in cellPad.c"
    assert "pad_bg_input_ok()" in m.group(0), "pad_gyro_gate ignores PAD_BACKGROUND_INPUT"
    cc = find_cc()
    if not cc:
        print("SKIP gyro gate: no C compiler")
        return
    work = os.path.join(HERE, "_test_pad_gyro_bg")
    os.makedirs(work, exist_ok=True)
    c = os.path.join(work, "gyro.c")
    exe = os.path.join(work, "gyro.exe")
    with open(c, "w") as f:
        f.write(DRIVER % (m.group(0), "pad_gyro_gate()" if m.group(1) == "void" else "pad_gyro_gate(0)"))
    if os.path.basename(cc).lower() in ("cl.exe", "clang-cl.exe", "cl", "clang-cl"):
        cmd = [cc, "/nologo", c, "/Fe" + exe, "/Fo" + work + os.sep]
    else:
        cmd = [cc, "-O1", c, "-o", exe]
    r = subprocess.run(cmd, capture_output=True, text=True)
    assert r.returncode == 0, r.stdout + r.stderr
    r = subprocess.run([exe], capture_output=True, text=True)
    print(r.stdout.strip())
    shutil.rmtree(work, ignore_errors=True)
    assert r.returncode == 0, r.stdout


if __name__ == "__main__":
    test_gyro_gate_background()
    print("OK")
    sys.exit(0)
