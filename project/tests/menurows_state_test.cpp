// Unit test for the per-menu PC SETTINGS entry state in project/rfom_menurows.cpp (no game): the pause
// menu and the main menu each keep their own entry, so one menu's entry never turns off the other's
// volume-row fallback. Builds the real file with stand-ins for the recompiler runtime.
// clang-cl /nologo /O1 /EHsc /std:c++17 /I ps3recomp\include project\tests\menurows_state_test.cpp /Fe:scratch\menurows_state_test.exe /Fo:scratch\menurows_state_test.obj
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#define RFOM_MENUROWS_TEST
#define PPU_THREAD_LOCAL thread_local
struct ppu_context { uint64_t gpr[32]; double fpr[32]; uint64_t lr; };
extern "C" void vm_write32(uint64_t addr, uint32_t val);
#include "../rfom_menurows.cpp"

static std::vector<uint8_t> g_mem(0x1100000);
extern "C" {
uint8_t* vm_base = g_mem.data();
uint32_t ppu_hle_inject_base = 0;
volatile uint32_t g_rfom_pad0_btns = 0;
thread_local void (*g_trampoline_fn)(void*) = nullptr;
void vm_write32(uint64_t addr, uint32_t val) { std::memcpy(vm_base + addr, &val, 4); }
int rfom_pm_rows(void) { return 5; }
static const char* const kPmTitle[5] = { "Frame rate", "Anti-aliasing", "Mouse sensitivity", "Field of view", "Button prompts" };
static int g_steps[8];
static const char* g_fps_val = "ON";
void rfom_pm_row(int i, char* t, int tc, char* v, int vc) { std::snprintf(t, tc, "%s", kPmTitle[i]); std::snprintf(v, vc, "%s", i ? "ON" : g_fps_val); }
void rfom_pm_change(int i, int d) { g_steps[i] += d; }
// More PC Options: one known key per category but effects, and one key without a category
static const char* const kPcxKey[3] = { "volume", "colorblind", "nokey_xyz" };
int rfom_pcx_rows(void) { return 3; }
void rfom_pcx_row(int i, char* t, int tc, char* v, int vc) { std::snprintf(t, tc, "X%d", i); std::snprintf(v, vc, "V"); }
void rfom_pcx_change(int i, int d) { g_steps[5 + i] += d; }
const char* rfom_pcx_key(int i) { return i >= 0 && i < 3 ? kPcxKey[i] : nullptr; }
}

static int g_fail, g_checks;
#define CHECK(c) do { ++g_checks; if (!(c)) { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static ppu_context g_ctx;
static uint32_t getkey(const char* name)
{
    std::strcpy((char*)vm_base + 0x200000, name);
    g_ctx.gpr[3] = 0x200000;
    CHECK(rfom_menu_rows_getkey(&g_ctx) == 1);
    return (uint32_t)g_ctx.gpr[3];
}
static int getnum(const char* name)
{
    g_ctx.gpr[3] = getkey(name);
    CHECK(rfom_menu_rows_getnum(&g_ctx) == 1);
    return (int)g_ctx.fpr[1];
}
// a chunk of `name` that is not the version the entry is written for: loadbuf leaves it alone
static void load_other_version(const char* name)
{
    uint8_t* p = vm_base + 0x100000;
    std::memset(p, 0x55, 12000);
    std::memcpy(p, "\x1bLuaP", 5);
    const uint32_t n = (uint32_t)std::strlen(name) + 1;
    std::memcpy(p + 18, &n, 4);
    std::memcpy(p + 22, name, n);
    g_ctx.gpr[4] = 0x100000; g_ctx.gpr[5] = 12000;
    CHECK(rfom_menu_rows_loadbuf(&g_ctx, nullptr) == 0);
}
// a menu whose list script was swapped (the hash gate passed): the script asks PC_ENTRY, reports ok
static void menu_with_entry(int m)
{
    list_swapped(m, true);
    CHECK(getnum(m ? "PC_ENTRY:1" : "PC_ENTRY:0") == 1);
    getkey(m ? "PC_ENTRY:ok:1" : "PC_ENTRY:ok:0");
}
// the text in a slot key's buffer
static std::string slot(uint32_t key)
{
    CHECK(key >= KEY_SLOT0 && key < KEY_SLOT0 + SLOTS);
    return (const char*)vm_base + area() + 0x8000u + (key - KEY_SLOT0) * SLOT_LEN;
}
static void reset()
{
    for (MenuState& st : s_menu) st = MenuState();
    s_cur_menu = -1;
    s_mmu.failed = s_win.failed = s_oh.failed = s_ohfe.failed = false;
}

int main()
{
    _putenv("R1_MENU_ROWS=1");

    // pause menu matches, then the main menu's list is another version
    reset();
    menu_with_entry(MENU_PAUSE);
    load_other_version("windowHandler");
    CHECK(getnum("PC_HERE:1") == 1);       // main menu: X on a volume row still opens the pages
    CHECK(getnum("PC_ENTRY:1") == 0);
    CHECK(getnum("PC_HERE:0") == 0);       // pause menu: the entry, volume rows as in the original

    // the other order: main menu matches, then the pause menu's list is another version
    reset();
    menu_with_entry(MENU_MAIN);
    load_other_version("MainMenuUpdater");
    CHECK(getnum("PC_HERE:0") == 1);
    CHECK(getnum("PC_ENTRY:0") == 0);
    CHECK(getnum("PC_HERE:1") == 0);

    // intel.ig's own MainMenuUpdater before / after pause.ig's: the pause menu keeps its entry
    reset();
    load_other_version("MainMenuUpdater");
    menu_with_entry(MENU_PAUSE);
    load_other_version("MainMenuUpdater");
    CHECK(getnum("PC_HERE:0") == 0 && s_menu[MENU_PAUSE].entry_live == 1);

    // a menu that reports a failure after ok (e.g. the button text) falls back alone
    reset();
    menu_with_entry(MENU_PAUSE);
    menu_with_entry(MENU_MAIN);
    getkey("PC_ENTRY:fail:1:the button text could not be set");
    CHECK(getnum("PC_HERE:1") == 1 && getnum("PC_HERE:0") == 0);

    // the categories: only the ones with rows, in the shared order; rows by settings key
    // (Frame rate, Field of view, the key without a category -> DISPLAY; Anti-aliasing -> GRAPHICS;
    // colorblind -> EFFECTS; Mouse sensitivity, Button prompts -> CONTROLS & HUD; volume -> AUDIO & LANGUAGE)
    CHECK(getnum("PC_CATS") == 5);
    CHECK(slot(getkey("PC_CAT:1")) == "PC DISPLAY" && slot(getkey("PC_CAT:4")) == "PC CONTROLS & HUD");
    CHECK(slot(getkey("PC_CAT:5")) == "PC AUDIO & LANGUAGE" && slot(getkey("PC_CAT:6")).empty());
    CHECK(getnum("PC_CPAGES:1") == 1 && getnum("PC_CPAGES:6") == 0);
    CHECK(slot(getkey("PC_ROW:1:0")) == "FRAME RATE: ON" && slot(getkey("PC_ROW:1:1")) == "FIELD OF VIEW: ON");
    CHECK(slot(getkey("PC_ROW:1:2")) == "X2: V" && slot(getkey("PC_ROW:1:3")).empty());
    CHECK(slot(getkey("PC_ROW:3:0")) == "COLOUR-BLIND: V" && slot(getkey("PC_ROW:4:1")) == "BUTTON PROMPTS: ON");
    CHECK(slot(getkey("PC_NAV:4:1")) == "BACK");   // one page: the category is in the title
    // rows fit the label column (28 characters from x = -370); a long value is cut with ".."
    g_fps_val = "UNLIMITED (MONITOR REFRESH RATE, VRR)";
    { const std::string r = slot(getkey("PC_ROW:1:0"));
      CHECK(r.size() <= 28 && r.compare(0, 12, "FRAME RATE: ") == 0 && r.substr(r.size() - 2) == ".."); }
    g_fps_val = "ON";
    // the two columns: the short name for the label, the value for the row's own Value text
    CHECK(slot(getkey("PC_RNAME:1:0")) == "FRAME RATE" && slot(getkey("PC_RVAL:1:0")) == "ON");
    CHECK(slot(getkey("PC_RNAME:3:0")) == "COLOUR-BLIND" && slot(getkey("PC_RVAL:1:2")) == "V");
    CHECK(slot(getkey("PC_RNAME:1:3")).empty() && slot(getkey("PC_RVAL:1:3")).empty());   // no such row: both blank
    g_fps_val = "UNLIMITED (MONITOR REFRESH RATE, VRR)";
    { const std::string nm = slot(getkey("PC_RNAME:1:0")), v = slot(getkey("PC_RVAL:1:0"));
      CHECK(nm.size() + 3 + v.size() <= 28 && v.size() >= 8 && v.substr(v.size() - 2) == ".." && v.compare(0, 9, "UNLIMITED") == 0); }
    g_fps_val = "";
    CHECK(slot(getkey("PC_RVAL:1:0")) == "-");    // never an empty-looking value column
    g_fps_val = "ON";
    // the page diagnostics carry the pad buttons the game read; at most 10 a second, the dropped ones counted
    g_rfom_pad0_btns = (1u << 13) | (1u << 3);
    { const std::string l = diag_line("back (category 0, row 1)", 5000);
      CHECK(l.find("): back (category 0, row 1); pad START|CIRCLE") != std::string::npos); }
    for (int i = 0; i < 12; i++) diag_line("move", 5100);
    CHECK(diag_line("move", 5999).empty());
    CHECK(diag_line("select", 6000).find("pad START|CIRCLE [4 lines dropped]") != std::string::npos);
    g_rfom_pad0_btns = 0;
    CHECK(diag_line("right", 6001).find("; pad none") != std::string::npos);
    // a big gap between two text slot rounds: two pages of names, values and the title stay readable
    { uint32_t k[22]; for (int i = 0; i < 22; i++) k[i] = put_slot(std::string(1, (char)('A' + i)));
      bool ok = true; for (int i = 0; i < 22; i++) ok = ok && slot(k[i]) == std::string(1, (char)('A' + i));
      CHECK(ok); }
    // the page title: the Options list title is replaced, never left as AUDIO VISUAL
    CHECK(slot(getkey("PC_TITLE:0")) == "PC SETTINGS");
    CHECK(slot(getkey("PC_TITLE:4")) == "PC CONTROLS & HUD");
    getkey("PC_STEP:4:0:-1"); getkey("PC_STEP:5:0:1"); getkey("PC_STEP:5:7:1");   // the last one: no such row
    CHECK(g_steps[2] == -1 && g_steps[5] == 1 && g_steps[0] == 0 && g_steps[6] == 0);
    getkey("PC_SHOW:1"); CHECK(s_show == 1);
    getkey("PC_SHOW:2:4"); CHECK(s_show == 2);
    getkey("PC_SHOW:0"); CHECK(s_show == 0);
    // the open request: 1 = PC SETTINGS (once; the page opens straight in), 2 = plain AUDIO / VIDEO
    CHECK(getnum("PC_OPEN") == 0);
    getkey("PC_OPEN:1"); CHECK(getnum("PC_OPEN") == 1 && s_direct == 1 && getnum("PC_OPEN") == 0);
    getkey("PC_SHOW:0"); getkey("PC_OPEN:0"); CHECK(getnum("PC_OPEN") == 2 && s_direct == 0 && getnum("PC_OPEN") == 0);
    // diagnostics are logged and never reach the game as a real key
    CHECK(getkey("PC_DIAG:painted: 5 rows") == KEY_PAGES);
    // main menu Back: handleBack's PC_KEEP report answers kNav when it came first, PC_HOLD when kNav came first,
    // and the report after kNav pairs with that same Back (never answers the next one)
    CHECK(getkey("PC_HOLD:1") == KEY_KEEP && s_hold == 1);
    keep_report(1, 10000); CHECK(keep_query(10001) == 1);                     // handleBack first: a category -> the list
    s_hold = 0; keep_report(0, 11000); CHECK(keep_query(11001) == 0);         // the list (direct) -> the main menu slide
    s_hold = 1; CHECK(keep_query(12000) == 1); keep_report(1, 12001);         // kNav first: a category -> the list
    s_hold = 0; CHECK(keep_query(12100) == 0); keep_report(0, 12101);         // 100 ms later, the next Back leaves
    keep_report(1, 13000); CHECK(keep_query(14000) == 0);                     // a stale report (1 s) does not count
    getkey("PC_HOLD:1"); getkey("PC_SHOW:0"); CHECK(s_hold == 0);             // the page closed: nothing held
    CHECK(getnum("PC_KEEP") == 0);
    // FrontEndMenuNav: only the exact frontend.ig chunk is swapped
    load_other_version("FrontEndMenuNav");
    CHECK(back_nav() == 1 && !s_nav.failed);
    std::printf("%s: %d checks, %d failed\n", g_fail ? "FAIL" : "menurows state OK", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
