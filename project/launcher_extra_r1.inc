// launcher_extra_r1.inc -- PC extras for the launcher (included by launcher.cpp):
//   * the options behind the game's "More PC Options" menu (rfom_pcextra.cpp): both write the same
//     settings.ini keys and settings.bat variables,
//   * controller button layout (PAD_MAP_*, PAD_SOUTHPAW; libs/input/cellPad.c XInput hook),
//   * the "Saves & tools" page: save list, back up / restore / open, automatic backup before Play
//     (keeps the newest N), import from RPCS3, the mods list (mods\order.txt) and the built-in benchmark
//     (RFOM_BENCH=1).
// Extra settings are appended to g_set after launcher_dist.inc's "updates" (so S_UPDATES stays S_COUNT);
// lx_write_bat() adds their settings.bat lines.
// Needs from launcher.cpp: g_set, Setting, Choice, path_of, toast, add, deco, SF, layout_setting_row, g_hw, g_dir.

static void play();
static std::string saves_folder(bool* exists);
#include "../ps3recomp/include/ps3emu/port_pc_extras.h"   // RPCS3 import (PARAM.SFO, title IDs)
#pragma comment(lib, "uuid.lib")                           // CLSID_FileOpenDialog (folder picker)
#define LX_TITLE_IDS "BCUS98107", "BCES00001", "BCJS30003", "BCAS20003", "BCKS10004", "BCAS20037", "BCJS70001", \
                     "BCJS70010", "BCKS10038", "NPEA00430"   // every disc / region of Fall of Man; first = fallback

struct LxOpt { int si; const char* env; int page; };     // page: where the option was first added (0 Display, 1 Graphics,
                                                         // 2 Controls, 3 shader cache, 4 upscaling, 5 audio, 6 gameplay,
                                                         // 7 mouse); the pages it is shown on come from launcher_cats.inc
static std::vector<LxOpt> g_lx;
static void lx_add(int page, const char* key, const char* title, const char* help, const char* env,
                   std::vector<Choice> ch, int def = 0)
{
    g_set.push_back({ key, title, help, std::move(ch), def, def });
    g_lx.push_back({ (int)g_set.size() - 1, env, page });
}

// A launcher started from a build subfolder (build_feat\Launcher.exe) works on the port folder above it and
// starts the programs next to it: run.bat uses RFOM_BUILD_DIR instead of build\.
static void lx_init_dirs()
{
    if (GetFileAttributesA(path_of("run.bat").c_str()) != INVALID_FILE_ATTRIBUTES) return;
    std::string here = g_dir, up = here.substr(0, here.find_last_of('\\'));
    if (up.empty() || GetFileAttributesA((up + "\\run.bat").c_str()) == INVALID_FILE_ATTRIBUTES) return;
    if (GetFileAttributesA((here + "\\ResistanceFoM.exe").c_str()) != INVALID_FILE_ATTRIBUTES)
        SetEnvironmentVariableA("RFOM_BUILD_DIR", here.c_str());
    snprintf(g_dir, MAX_PATH, "%s", up.c_str());
}

static void lx_register()
{
    const std::vector<Choice> on_def = { {"On", ""}, {"Off", "0"} };
    // "WINDOW, HDR AND EXTRAS" options
    lx_add(0, "resizable", "Resizable window",
           "Windowed mode: drag the window edges (or maximize) to resize it; the picture follows the new size.",
           "RSX_RESIZABLE", on_def);
    lx_add(0, "altenter", "Alt+Enter",
           "Alt+Enter switches between windowed and borderless fullscreen in game.", "RSX_ALT_ENTER", on_def);
    lx_add(0, "skipintro", "Skip intro movies",
           "Skips the opening intro movie if the game plays one when it starts (the same as pressing Start through it). "
           "The game has no logo videos to skip.", "RFOM_SKIP_INTRO", on_def);
    lx_add(0, "cutscene_skip", "Hold to skip movies",
           "Hold Space / Enter / A / Menu for a second to skip a story movie (the game's own skip).",
           "RFOM_CUTSCENE_SKIP", on_def);
    lx_add(0, "cutscene_skip_all", "Skip unwatched movies",
           "Hold to skip movies also skips a story movie you have not watched yet (the game itself only lets you "
           "skip movies you have seen). Needs Hold to skip movies on.", "RFOM_CUTSCENE_SKIP_ALL", on_def);
    // rfom_pcextra.cpp: the "Hold Space / A to skip" notification when a movie starts (skipping is cutscene_skip)
    lx_add(0, "skiphint", "Show cutscene skip hint",
           "A short \"Hold Space / A to skip\" notification in the corner when a movie            starts. Auto (default): "
                      "shown when Skip unwatched movies is on, since every story movie can then be skipped. "
                      "Holding the button skips either way (Hold to skip movies).",
                      "PORT_SKIP_HINT", { {"Auto", ""}, {"On", "1"}, {"Off", "0"} });
    // rfom_trophies.cpp
    lx_add(0, "trophytoast", "Achievement notifications",
           "A notification in the corner of the game window when you earn an achievement (unofficial trophy), "
           "checked from your profile save every few seconds. Off hides it; achievements are still recorded (see "
           "the Achievements page).",
           "RFOM_TROPHY_TOAST", on_def);
    lx_add(0, "trophystyle", "Achievement pop-up style",
           "PS3: a PS3 trophy-style pop-up at the top corner (\"You have earned an achievement.\", a cup in the "
           "grade's colour and the name) that slides in for about 6 seconds. Classic: the port's older notification "
           "with the grade badge and the description.",
           "PORT_TROPHY_STYLE", { {"PS3", ""}, {"Classic", "old"} });
    lx_add(0, "hdr", "HDR output",
           "Experimental. Needs Windows HDR on (Settings > Display > Use HDR). Draws the final image into an HDR10 (PQ) "
           "or scRGB swap chain: the game's SDR picture stays at the paper-white brightness below and only highlights "
           "are expanded towards the peak. Auto turns HDR on only when Windows HDR is on for the game's display. Falls "
           "back to SDR otherwise. Screenshots are saved as normal SDR PNGs. Pros: brighter highlights and deeper "
           "contrast. Cons: needs a real HDR display; wrong brightness settings look washed out or clip highlights.",
           "RFOM_HDR", { {"Off", ""}, {"Auto", "auto"}, {"On  (HDR10)", "1"}, {"On  (scRGB)", "scrgb"} });
    lx_add(0, "hdr_paper", "HDR paper white",
           "How bright the HUD, menus and the game's normal image are in HDR, in nits (203 is the HDR reference white). "
           "Lower for a dark room, higher for a bright one.", "RFOM_HDR_PAPER",
           { {"203 nits", ""}, {"100 nits", "100"}, {"160 nits", "160"}, {"250 nits", "250"}, {"300 nits", "300"},
             {"400 nits", "400"} });
    lx_add(0, "hdr_peak", "HDR peak brightness",
           "How bright the brightest highlights get, in nits (also sent to the display as HDR metadata). Display = the "
           "peak Windows reports for the monitor. Set it equal to paper white to turn highlight expansion off.",
           "RFOM_HDR_PEAK",
           { {"Display", ""}, {"400 nits", "400"}, {"600 nits", "600"}, {"800 nits", "800"}, {"1000 nits", "1000"},
             {"1500 nits", "1500"}, {"2000 nits", "2000"} });
    lx_add(0, "hdr_sat", "HDR saturation",
           "Colour saturation of the HDR picture (1.0 = as the game).", "RFOM_HDR_SAT",
           { {"1.0", ""}, {"0.9", "0.9"}, {"1.1", "1.1"}, {"1.2", "1.2"} });
    lx_add(0, "reflex", "NVIDIA Reflex / low latency",
           "Cuts input lag when the GPU is the limit. On an NVIDIA GPU this is NVIDIA Reflex Low Latency (On + Boost "
           "also keeps the GPU clocks up); on other GPUs it keeps at most one frame queued (a waitable swap chain). "
           "It is not a frame limiter - the Frame rate setting still decides the cap.", "RFOM_REFLEX",
           { {"Off", ""}, {"On", "1"}, {"On + Boost", "2"} });
    lx_add(0, "latency", "Latency reduction",
           "Which low-latency technology to use. NVIDIA Reflex needs a GeForce GPU; AMD Anti-Lag 2 needs a Radeon RX 5000 or newer "
           "with a recent driver. Auto picks Reflex on NVIDIA and Anti-Lag 2 on AMD. Default leaves it to the row above. "
           "Not used while frame generation is on (that has its own). Experimental.", "PORT_LATENCY",
           { {"Default  (Low latency row)", ""}, {"Off", "off"}, {"NVIDIA Reflex", "reflex"}, {"AMD Anti-Lag 2", "antilag"}, {"Auto", "auto"} });
    lx_add(0, "hudfix", "Ultrawide HUD",
           "With an ultrawide Aspect ratio the game's HUD is stretched with the picture. On keeps the HUD (ammo, health, "
           "text) undistorted in a centred 16:9 area. Experimental: markers attached to the world can sit off their "
           "targets, and a full-screen tint or fade may cover only the centre.", "RSX_HUD_FIX",
           { {"Stretched", ""}, {"Centred 16:9  (experimental)", "1"} });
    lx_add(0, "photo", "Photo mode  (F7)",
           "F7 in game pauses, hides the port's overlays and lets you take clean F12 screenshots. No free camera "
           "(see the README).", "RFOM_PHOTO", on_def);
    lx_add(0, "discord", "Discord Rich Presence",
           "Shows \"Playing Resistance: Fall of Man\" in your Discord status. Needs the Discord app running and an "
           "application id in discord_appid.txt (or the RFOM_DISCORD_APPID variable); does nothing otherwise.",
           "RFOM_DISCORD", { {"Off", ""}, {"On", "1"} });
    lx_add(0, "savebackup", "Back up saves before playing",
           "Play copies your saved games to save_backups (next to the saves) first, only when they changed, keeping "
           "the newest ones. See the Saves & tools page.", "RFOM_SAVE_BACKUP",
           { {"On  (keep 10)", ""}, {"On  (keep 5)", "5"}, {"On  (keep 30)", "30"}, {"Off", "0"} });
    // hybrid CPUs (port_cpu_sets.h): Advanced > CPU THREADS
    lx_add(0, "pcores", "Prefer P-cores",
           "Intel 12th gen and newer (P-cores + E-cores): the game's main, SPU, RSX and audio threads prefer the fast "
           "P-cores and are never power-throttled; other threads use any core. Does nothing on CPUs without E-cores "
           "(AMD, older Intel). Off leaves every thread to Windows.",
           "PORT_CPU_PCORES", { {"Auto  (hybrid CPUs only)", ""}, {"Off", "0"} });
    lx_add(0, "threadprio", "Raise game thread priority",
           "Runs the main and RSX threads at above-normal priority, so background programs interrupt them less. "
           "Can make other programs sluggish on CPUs with 4 cores or fewer. Off is the Windows default.",
           "PORT_THREAD_PRIORITY", { {"Off", ""}, {"On", "1"} });
    lx_add(0, "overlay_pos", "Overlay corner",
           "Which corner of the game window the F3 performance overlay sits in. Notifications (screenshot saved, "
           "movie skip hint) appear in the opposite top corner. Both are drawn into the game's own image, so they "
           "show in every display mode and in recordings, but never in F12 screenshots.", "RFOM_OVERLAY_POS",
           { {"Top left", ""}, {"Top right", "tr"}, {"Bottom left", "bl"}, {"Bottom right", "br"} });
    lx_add(0, "seamless", "Seamless level loads",
           "The game swaps to another program at some level loads; On holds a black cover over the window meanwhile, "
           "so the game window does not close and reopen. Pros: no flashing window or desktop between levels. "
           "Cons: none known; Off is the old behaviour if the window ever stays black.",
           "RFOM_SEAMLESS", { {"On", ""}, {"Off", "0"} });
    // "AUDIO" options (stubs.cpp rfom_speakers, cellAudio.c audio_buffer_ms)
    lx_add(5, "speakers", "Speaker layout",
           "The channel layout the game mixes for. Auto is 7.1 when Audio output is 7.1, else 5.1. "
           "Pros of 5.1 / 7.1: proper surround on a surround system (it is folded down on stereo). Cons: on plain "
           "stereo, Stereo can sound a little fuller, since nothing is folded down.", "RFOM_SPEAKERS",
           { {"Auto", ""}, {"7.1 surround", "8"}, {"Stereo", "2"}, {"5.1 surround", "6"} });
    lx_add(5, "audio_channels", "Audio output",
           "The channels the port sends to Windows. Auto follows the default playback device's speaker setup "
           "(Windows Sound settings > your device > Speaker setup / Configure): 5.1 or 7.1 there gives every "
           "surround speaker its own channel; stereo devices get the surround mix folded down as before. "
           "Force 5.1 / 7.1 if Windows reports fewer channels than your receiver has; Windows down-mixes "
           "a forced layout on a device that has fewer.", "PORT_AUDIO_CHANNELS",
           { {"Auto", ""}, {"Stereo", "2"}, {"5.1", "6"}, {"7.1", "8"} });
    lx_add(5, "audio_buffer", "Audio buffer",
           "How much sound is buffered ahead. Auto is 100 ms (150 ms on CPUs with fewer than 12 threads). "
           "Pros of a larger buffer: no crackles or stretching when the CPU is busy. Cons: sound lags the picture a "
           "little more. Smaller: tighter sound, but may crackle on a busy CPU.", "PORT_AUDIO_BUFFER_MS",
           { {"Auto", ""}, {"80 ms", "80"}, {"100 ms", "100"}, {"150 ms", "150"}, {"200 ms", "200"} });
    // "GAMEPLAY" options (stubs.cpp)
    lx_add(0, "gamma", "Brightness",
           "Picture gamma on the final image: lower is brighter, higher is darker (1.0 = as the game). SDR only; in HDR "
           "use HDR paper white. Also in game: F9 (changes at once).", "PS3_DISPLAY_GAMMA",
           { {"1.0  (as the game)", ""}, {"0.8  (brighter)", "0.8"}, {"0.9  (brighter)", "0.9"}, {"1.1  (darker)", "1.1"},
             {"1.2  (darker)", "1.2"} });
    lx_add(6, "language", "Game language",
           "The language the game is told the console uses: its text and speech, for the languages your copy of the "
           "game has (others fall back to English). Windows language = the language of Windows. Also in game: F9 "
           "(takes effect at the next start).", "PS3_LANG",
           { {"Windows language", ""}, {"English", "en"}, {"English (UK)", "en-gb"}, {"French", "fr"}, {"German", "de"},
             {"Italian", "it"}, {"Spanish", "es"}, {"Dutch", "nl"}, {"Portuguese", "pt"}, {"Russian", "ru"},
             {"Polish", "pl"}, {"Finnish", "fi"}, {"Swedish", "sv"}, {"Danish", "da"}, {"Norwegian", "no"},
             {"Japanese", "ja"}, {"Korean", "ko"} });
    lx_add(6, "gore", "Blood and gore",
           "Full keeps the game's full-gore setting (blood), whatever region the console settings say. Console "
           "region leaves it to the game, which may pick reduced gore.", "RFOM_GORE",
           { {"Full", ""}, {"As the console region", "0"} });
    lx_add(6, "dyndiff", "Adaptive difficulty",
           "The PS3 game quietly makes enemies a little less deadly the longer you are stuck in a section and the "
           "more you die there. Pros of On: the original experience, hard spots ease up. Cons: the difficulty you "
           "picked is not what you get; Off keeps it fixed.", "RFOM_DYNDIFF",
           { {"On  (PS3 original)", ""}, {"Off  (fixed difficulty)", "0"} });
    // rfom_cheats.cpp: optional single-player cheats (also F9 rows; never in a benchmark run)
    lx_add(6, "cheat_god", "God mode",
           "Single-player cheat: your health stays full and enemy hits do no damage. Only you (and the other local "
           "split-screen player); enemies are not affected. Trophies are off until the game is restarted.",
           "PORT_CHEAT_GODMODE", { {"Off", ""}, {"On", "1"} });
    lx_add(6, "cheat_ammo", "Infinite ammo",
           "Single-player cheat: your weapons' magazines never run down. Trophies are off until the game is restarted.",
           "PORT_CHEAT_AMMO", { {"Off", ""}, {"On", "1"} });
    lx_add(6, "cheat_grenades", "Infinite grenades",
           "Single-player cheat: throwing a grenade does not use it up. Trophies are off until the game is restarted.",
           "PORT_CHEAT_GRENADES", { {"Off", ""}, {"On", "1"} });
    lx_add(6, "cheat_keys", "Cheat hotkeys",
           "In the game, Ctrl+F1 toggles God mode and Ctrl+F2 Infinite ammo and grenades. Off: only this page and "
           "the F9 menu change them.",
           "PORT_CHEAT_KEYS", { {"Off", ""}, {"On", "1"} });
    // "MORE MOUSE OPTIONS" options
    lx_add(7, "aimassist", "Aim assist",
           "The game's aim assist: your aim slows down when it passes over an enemy and is pulled toward it. Auto turns "
           "it off while you play with keyboard & mouse and keeps it with a controller (in split-screen only the "
           "keyboard & mouse player loses it). On: always, as on PS3. Off: never, also with a controller. "
           "Also in game: F10 > More PC Options, or F9 (changes at once).", "PORT_AIM_ASSIST",
           { {"Auto (off with mouse, on with controller)", ""}, {"On", "on"}, {"Off", "off"} });
    lx_add(7, "grab_mouse", "Shake off grabs with the mouse",
           "When an enemy grabs you, shaking the mouse breaks free (scaled so vigorous shaking matches shaking both "
           "sticks). Pros: works with keyboard & mouse, where the movement keys barely counted. Cons: none; Off uses "
           "the game's own stick meter. A controller is not affected.", "RFOM_GRAB_MOUSE",
           { {"On", ""}, {"Off", "0"} });
    // "ACCESSIBILITY" options
    lx_add(1, "colorblind", "Colour-blind filter",
           "Shifts colours so they are easier to tell apart (daltonization of the final image). Also in game: "
           "F10 > More PC Options, or F9.", "RFOM_COLORBLIND",
           { {"Off", ""}, {"Protanopia  (red)", "1"}, {"Deuteranopia  (green)", "2"}, {"Tritanopia  (blue)", "3"} });
    lx_add(1, "crosshair", "Extra crosshair",
           "A small dot or cross at the screen centre while on foot, for when the game's reticle is hard to see.",
           "RFOM_CROSSHAIR", { {"Off", ""}, {"Dot", "dot"}, {"Cross", "cross"} });
    lx_add(1, "xhair_color", "Crosshair colour",
           "Colour of the extra crosshair. High-contrast choices; a dark outline is always drawn around it.",
           "RFOM_CROSSHAIR_COLOR", { {"White", ""}, {"Green", "green"}, {"Yellow", "yellow"}, {"Cyan", "cyan"}, {"Red", "red"},
                                     {"Pink", "pink"}, {"Orange", "orange"} });
    lx_add(1, "xhair_size", "Crosshair size", "Size of the extra crosshair (Small is the original 24 px marker).",
           "RFOM_CROSSHAIR_SIZE", { {"Small", ""}, {"Medium", "medium"}, {"Large", "large"}, {"Huge", "huge"} });
    // "UPSCALING" options (rsx_fsr.inc)
    lx_add(4, "objmv", "Object motion vectors",
           "DLSS / FSR: On gives moving characters and objects their own motion. Pros: sharper moving objects, less "
           "ghosting and smearing. Cons: a small GPU cost. Off uses camera motion only: try it if moving objects "
           "smear, shuffle or stutter.",
           "RSX_DLSS_OBJMV", { {"On", "1"}, {"Off", "0"} });
    lx_add(4, "dlsspreset", "DLSS preset",
           "Which DLSS model NVIDIA DLSS uses (only with Anti-aliasing = NVIDIA DLSS / DLAA on an RTX GPU). Auto lets "
           "NVIDIA choose per quality mode (K for DLAA / Quality / Balanced, M for Performance, L for Ultra "
           "Performance). Transformer presets (J, K, L, M): sharper, more stable detail and less ghosting, a little "
           "more GPU time; K is the all-round pick, J trades slightly less ghosting for more flicker, M and L are "
           "tuned for Performance / Ultra Performance. CNN presets (E, F, older models NVIDIA has deprecated): "
           "cheaper but softer in motion; F suits Ultra Performance. A preset the driver's DLSS no longer accepts "
           "falls back to Auto (see the log).",
           "RSX_DLSS_PRESET", { {"Auto (NVIDIA default)", ""}, {"K  (transformer)", "11"}, {"J  (transformer)", "10"},
                                {"M  (transformer, Performance)", "13"}, {"L  (transformer, Ultra Performance)", "12"},
                                {"E  (CNN)", "5"}, {"F  (CNN, Ultra Performance)", "6"} });
    lx_add(4, "dlss_fg", "Frame Generation",
           "Frame generation: raises smoothness when the CPU is the limit, adds a little input latency, needs an "
           "RTX 40/50; 4x is for high-refresh displays. DLSS choices need Anti-aliasing = NVIDIA DLSS (turns Reflex on; "
           "3x / 4x need an RTX 50, an RTX 40 uses 2x); without DLSS Frame Generation they fall back to FSR 3 2x. "
           "FSR 3 2x (AMD, open source) works on other GPUs too, with any temporal anti-aliasing (FSR 3.1, DLSS or "
           "XeSS). The HUD is kept out of the generated frames. Set the Frame rate to the rendered rate you "
           "want (144 Hz: about 69 for 2x, 46 for 3x, 34 for 4x).",
           "RFOM_DLSS_FG", { {"Off", ""}, {"DLSS 2x", "2"}, {"DLSS 3x", "3"}, {"DLSS 4x", "4"}, {"FSR 3 2x", "fsr3"} });
    lx_add(4, "fg_backend", "Frame generation backend",
           "Which frame generation runs when Frame Generation is a DLSS choice. Auto: DLSS Frame Generation, or "
           "AMD FSR 3 2x when DLSS Frame Generation can't start (no RTX 40/50, old driver). DLSS only: no fallback - "
           "frame generation stays off if DLSS Frame Generation can't start. FSR 3: always AMD FSR 3 2x (any GPU). "
           "Takes effect at the next start.",
           "RFOM_FG_BACKEND", { {"Auto", ""}, {"DLSS Frame Generation only", "dlss"}, {"AMD FSR 3", "fsr3"} });
    lx_add(4, "fsr_sharp", "FSR 3.1 sharpening",
           "FSR 3.1's own sharpening (RCAS), only when FSR 3.1 is the anti-aliasing / upscaler. Separate from the "
           "Sharpening setting above, which works on the final picture. Pros: crisper textures from a lower render "
           "share. Cons: high values give halos and grain.", "RSX_FSR_SHARP",
           { {"Low  (0.25)", ""}, {"Off", "0"}, {"Medium  (0.5)", "0.5"}, {"High  (0.75)", "0.75"},
             {"Max  (1.0)", "1"} });
    lx_add(4, "lodbias", "Texture detail (LOD bias)",
           "Auto sharpens texture mip selection to match the upscaler's output resolution, capped at your monitor's "
           "resolution (for example -1 at Performance; -0.6 for Ultra Performance at 4K shown on a 1080p screen) and "
           "leaves it alone without upscaling. Negative = sharper textures. Pros: crisper distant "
           "textures. Cons: too negative shimmers and aliases in motion; positive is blurrier.", "RSX_LOD_BIAS",
           { {"Auto", ""}, {"0  (as the game)", "0"}, {"-0.5", "-0.5"}, {"-1", "-1"}, {"-1.5", "-1.5"},
             {"+0.5  (softer)", "0.5"} });
    lx_add(4, "shadowscale", "Shadow map resolution",
           "Render scale of the shadow maps, separate from the main resolution (never above it). Auto = the main "
           "resolution up to 2x. Pros of lower: much less GPU time and VRAM in shadowed scenes at 3x/4x, where the "
           "shadow passes were drawn at the full supersampling factor. Cons: slightly softer, more stepped shadow "
           "edges. Same as resolution = the old behaviour (sharpest, slowest).", "RFOM_SHADOW_SCALE",
           { {"Auto  (up to 2x)", ""}, {"1x  (PS3)", "1"}, {"1.5x", "1.5"}, {"2x", "2"}, {"3x", "3"},
             {"Same as resolution", "8"} });
    // rsx_ssao.inc / rsx_cacao.inc: ambient occlusion passes of the D3D12 renderer (same option as Resistance 2 / 3)
    lx_add(1, "ssao", "Ambient occlusion",
           "Game: as the game (Fall of Man has no ambient occlusion of its own; the default). Port SSAO adds the "
           "port's own screen-space AO. CACAO (AMD FidelityFX CACAO) draws higher-quality soft contact shadows in "
           "corners and creases from the scene depth. Off draws no AO. Works with every Anti-aliasing setting; with "
           "DLSS, FSR or XeSS it is computed at the internal resolution, before upscaling. Costs GPU time. Experimental.",
           "RFOM_AO_MODE", { {"Game  (none of its own)", ""}, {"Port SSAO  (extra, experimental)", "1"},
                             {"CACAO  (experimental)", "cacao"}, {"Off  (no AO)", "off"} });
    lx_add(1, "aoquality", "Ambient occlusion quality",
           "Quality of the CACAO ambient occlusion: Low (3 samples), Medium (5 samples, sharper edges and detail) or High (12 samples, normal-aware edges). Costs GPU time; only used with Ambient occlusion: CACAO.",
           "RFOM_AO_QUALITY", { {"Medium", ""}, {"Low", "low"}, {"High", "high"} });
    // "SHADER CACHE" options (rsx_d3d12_backend.c rsx_pso_precompile_boot)
    lx_add(3, "precompile", "Compile shaders before playing",
           "On: at start-up the game builds every shader pipeline it has already used on this PC before play begins "
           "(a \"Compiling shaders N / M\" panel shows the progress), so effects seen before never stutter. Usually a few "
           "seconds; about a minute after a driver update or Rebuild shader cache. Off: most of them are built in the "
           "background while you play instead (can hitch briefly). Pros of On: no stutter from shaders already seen. "
           "Cons: longer start-up.",
           "PORT_SHADER_PRELOAD", { {"On", "1"}, {"Off", "0"} });
    lx_add(3, "shaderthreads", "Shader compile threads",
           "CPU threads used by Compile shaders before playing. Auto uses every logical core but one.",
           "RSX_PSO_PRECOMPILE_THREADS",
           { {"Auto  (all cores but one)", ""}, {"2", "2"}, {"4", "4"}, {"6", "6"}, {"8", "8"}, {"12", "12"}, {"16", "16"} });
    // PC extras (ps3emu/port_pc_extras.h; cellPad.c, cellAudio.c, rfom_pcfeat.cpp); also F10 > More PC Options
    lx_add(2, "rumble", "Vibration", "Controller rumble strength (also in game: F10 \xE2\x80\xBA More PC Options).",
           "PAD_RUMBLE", { {"100%", ""}, {"75%", "75"}, {"50%", "50"}, {"25%", "25"}, {"Off", "0"} });
    lx_add(2, "enterbtn", "Confirm button",
           "Which face button the console settings say confirms: Cross (Western consoles) or Circle (Japanese "
           "consoles; Cross then goes back). Experimental: the game may keep its own layout.",
           "PS3_ENTER_BUTTON", { {"Cross", ""}, {"Circle", "circle"} });
    lx_add(2, "padbg", "Controller in the background",
           "Whether the controller still works while another window has focus. Auto: only when Audio \xE2\x80\xBA "
           "In the background is Keep running or Mute only. On: always (handy with a second screen). Off: never.",
           "PAD_BACKGROUND_INPUT", { {"Auto", ""}, {"On", "1"}, {"Off", "0"} });
    lx_add(2, "outerdz", "Stick outer deadzone",
           "The last part of the stick's travel already counts as full deflection. Pros: full speed even with worn "
           "sticks that never reach the edge (slow maximum turn speed).",
           "PAD_OUTER_DEADZONE", { {"Off", ""}, {"5%", "5"}, {"10%", "10"}, {"15%", "15"} });
    lx_add(2, "antidz", "Stick anti-deadzone",
           "Skips the game's own dead zone, so the smallest stick movement already moves. Pros: finer aiming "
           "near the centre. Cons: too much makes the view creep with stick drift.",
           "PAD_ANTI_DEADZONE", { {"Off", ""}, {"10%", "10"}, {"20%", "20"}, {"30%", "30"} });
    lx_add(2, "trigdz", "Trigger deadzone",
           "How far a trigger must be pulled before it counts (a light touch on L2 / R2 does nothing).",
           "PAD_TRIGGER_DEADZONE", { {"Off", ""}, {"10%", "10"}, {"20%", "20"}, {"30%", "30"} });
    lx_add(2, "turnboost", "Turn boost",
           "Look acceleration: holding the right stick at the edge adds extra turn speed after a short "
           "moment, for quick 180s. Not while aiming down the sights.",
           "PAD_TURN_BOOST", { {"Off", ""}, {"90 deg/s", "90"}, {"180 deg/s", "180"}, {"270 deg/s", "270"} });
    lx_add(2, "crouchmode", "Crouch button  (experimental)",
           "L2 (C on the keyboard) as the game has it, Toggle: press to crouch, press again to stand, or Hold: "
           "crouch only while the button is held. L2 is also sprint in some layouts, so this affects it too.",
           "PAD_CROUCH_MODE", { {"As the game", ""}, {"Toggle", "toggle"}, {"Hold", "hold"} });
    lx_add(5, "volume", "Master volume",
           "All game sound, on top of the game's own volume options (also in game: F10 \xE2\x80\xBA More PC Options).",
           "PORT_VOLUME", { {"100%", ""}, {"90%", "90"}, {"80%", "80"}, {"70%", "70"}, {"60%", "60"}, {"50%", "50"},
                            {"40%", "40"}, {"30%", "30"}, {"20%", "20"}, {"10%", "10"}, {"Muted", "0"} });
    lx_add(5, "audio_mono", "Mono audio",
           "Accessibility: mixes every channel to both ears, so nothing is lost when you hear with one ear or "
           "use a single speaker.", "PORT_AUDIO_MONO", { {"Off", ""}, {"On", "1"} });
    lx_add(0, "ovgraph", "Frame-time graph",
           "Adds a graph of the last 120 frame times under the F3 performance overlay's numbers. Tall yellow or red "
           "bars are slow frames (stutter).", "PORT_OVERLAY_GRAPH", { {"On", ""}, {"Off", "0"} });
    lx_add(0, "uiscale", "UI scale",
           "Size of the PC menus (F9 / F10), the F3 overlay and notifications. Auto makes the overlay and "
           "notifications larger on 1440p and 4K screens (the menus already follow the window size).",
           "PORT_UI_SCALE", { {"Auto", ""}, {"75%", "75"}, {"100%", "100"}, {"125%", "125"}, {"150%", "150"},
                              {"175%", "175"}, {"200%", "200"} });
    lx_add(0, "benchkey", "Benchmark capture key",
           "Press it in game to start recording frame times, press again to stop: logs\\bench\\ gets a summary (average "
           "fps, 1% / 0.1% low, stutters) and a CSV with every frame time, and the summary shows on the Saves & tools "
           "page. F11 is free unless you bound it in controls.txt.", "RFOM_BENCH_KEY",
           { {"F11", ""}, {"F6", "F6"}, {"F5", "F5"}, {"Off", "off"} });
    lx_add(0, "winremember", "Remember window position",
           "Windowed mode: the game reopens where you last left its window (and at that size), while the window "
           "size and monitor settings stay the same and the spot is still on a screen (window_pos.txt). Off: centred.",
           "RSX_WINDOW_REMEMBER", { {"On", ""}, {"Off  (always centred)", "0"} });
    // "CONTROLLER BUTTONS" options
    lx_add(2, "pad_disconnect_pause", "Pause when the controller disconnects",
           "Pauses the game and shows a message when the controller you play with is unplugged or runs out of battery. "
           "Reconnect it, or press Enter, to continue.", "RFOM_PAD_DISCONNECT_PAUSE", on_def);
    lx_add(2, "pad_southpaw", "Swap sticks  (southpaw)", "Left stick looks, right stick moves.", "PAD_SOUTHPAW",
           { {"Off", ""}, {"On", "1"} });
    lx_add(2, "p1pad", "Controller to play with",
           "With several controllers connected, which one is player 1 (in the order Windows connected them). "
           "Automatic: the controller you used last (in local co-op: the first one connected). Needs "
           "Controller support = SDL3 (the default); with XInput the first one is used. Pros: "
           "pick your pad without unplugging the others. Cons: the number can change when controllers reconnect.",
           "PAD_P1_CONTROLLER",
           { {"Automatic", ""}, {"Controller 1", "1"}, {"Controller 2", "2"}, {"Controller 3", "3"},
             {"Controller 4", "4"} });
    // Local co-op (cellPad.c pad_coop_init): the game's own split-screen; ports 1-3 are the extra controllers
    lx_add(2, "coop", "Local co-op  (split-screen)",
           "On: a second controller can be player 2: press START on it to join (see the README, \"Local "
           "co-op\"). Off: one player, however many controllers are connected (also when DSX, DS4Windows or "
           "Steam Input adds a virtual copy of your controller); player 1 is the controller you used last.",
           "PAD_COOP", { {"Off", "0"}, {"On", "1"} });
    lx_add(2, "p2pad", "Player 2 controller",
           "Which controller is player 2 when Local co-op is On. Automatic takes the next one found. Needs "
           "Controller support = SDL3 (the default).",
           "PAD_P2_CONTROLLER", { {"Automatic", ""}, {"Controller 1", "1"}, {"Controller 2", "2"},
                                  {"Controller 3", "3"}, {"Controller 4", "4"}, {"Controller 5", "5"},
                                  {"Controller 6", "6"}, {"Controller 7", "7"}, {"Controller 8", "8"} });
    static const char* keys[] = { "A", "B", "X", "Y", "LB", "RB", "LT", "RT", "LS", "RS", "BACK", "START" };
    static const char* names[] = { "A", "B", "X", "Y", "LB", "RB", "LT", "RT", "Left stick click", "Right stick click",
                                   "View", "Menu" };
    for (int i = 0; i < 12; i++) {
        std::vector<Choice> ch = { { "Default", "" } };
        for (int j = 0; j < 12; j++) {
            if (j == i) continue;
            char* l = (char*)malloc(48); snprintf(l, 48, "Acts as %s", names[j]);
            ch.push_back({ l, keys[j] });
        }
        char* key = (char*)malloc(24); snprintf(key, 24, "padmap_%s", keys[i]);
        for (char* c = key; *c; c++) *c = (char)tolower((unsigned char)*c);
        char* env = (char*)malloc(24); snprintf(env, 24, "PAD_MAP_%s", keys[i]);
        char* title = (char*)malloc(48); snprintf(title, 48, "Button %s", names[i]);
        lx_add(2, key, title,
               "What this controller button does in game (Xbox names; a PlayStation pad through Steam Input / DS4Windows "
               "maps the same way). Swapping two buttons needs both rows. Also in game: F10 > More PC Options > "
               "Controller layout.", env, std::move(ch));
    }
    lx_add(6, "hdui", "HD UI pack",
           "Loads the AI-upscaled menu / HUD textures from mods\\hd_ui, made on this PC from your game files by "
           "Saves & tools > Create HD UI pack. Missing or damaged files fall back to the game's own textures. "
           "Experimental.",
           "RSX_HD_UI", { {"Off", ""}, {"On  (mods\\hd_ui)", "1"} });
}

static void lx_write_bat(FILE* f)
{
    for (auto& o : g_lx) {
        const char* v = g_set[o.si].choices[g_set[o.si].sel].value;
        if (*v) fprintf(f, "set \"%s=%s\"\n", o.env, v);
    }
}
static const char* lx_val(const char* key)
{
    for (auto& s : g_set) if (!strcmp(s.key, key)) return s.choices[s.sel].value;
    return "";
}

// ---- saves -----------------------------------------------------------------------------------------
static std::string lx_save_dir()
{
    bool ex = false; std::string p = saves_folder(&ex);
    return ex ? p : p + "\\dev_hdd0\\home\\00000001\\savedata";
}
// next to dev_hdd0 (Saved Games\Resistance Fall of Man, or gamedata when portable), so it survives a reinstall
static std::string lx_backup_root()
{
    std::string sd = lx_save_dir();
    const std::string tail = "\\dev_hdd0\\home\\00000001\\savedata";
    if (sd.size() > tail.size()) sd.resize(sd.size() - tail.size());
    return sd + "\\save_backups";
}
struct LxEntry { std::string name, title; ULONGLONG newest = 0, bytes = 0; int files = 0; };
static void lx_scan(const std::string& dir, LxEntry& e)
{
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { lx_scan(dir + "\\" + fd.cFileName, e); continue; }
        const ULONGLONG t = ((ULONGLONG)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime;
        e.newest = std::max(e.newest, t);
        e.bytes += ((ULONGLONG)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        e.files++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}
// PARAM.SFO: SUB_TITLE (or TITLE) of a PS3 save
static std::string lx_sfo_title(const std::string& dir)
{
    FILE* f = fopen((dir + "\\PARAM.SFO").c_str(), "rb");
    if (!f) return "";
    std::vector<unsigned char> b(65536);
    const size_t n = fread(b.data(), 1, b.size(), f); fclose(f);
    if (n < 20 || memcmp(b.data(), "\0PSF", 4)) return "";
    auto u32 = [&](size_t o) { return o + 4 <= n ? (unsigned)b[o] | b[o + 1] << 8 | b[o + 2] << 16 | (unsigned)b[o + 3] << 24 : 0u; };
    auto u16 = [&](size_t o) { return o + 2 <= n ? (unsigned)(b[o] | b[o + 1] << 8) : 0u; };
    const unsigned kt = u32(8), dt = u32(12), cnt = u32(16);
    std::string sub, title;
    for (unsigned i = 0; i < cnt && i < 64; i++) {
        const size_t ent = 20 + i * 16;
        const size_t ko = kt + u16(ent), dof = dt + u32(ent + 12), len = u32(ent + 4);
        if (ko >= n || dof + len > n) continue;
        const char* k = (const char*)&b[ko];
        std::string v((const char*)&b[dof], strnlen((const char*)&b[dof], len));
        if (!strcmp(k, "SUB_TITLE")) sub = v; else if (!strcmp(k, "TITLE")) title = v;
    }
    return !sub.empty() ? sub : title;
}
static std::vector<LxEntry> lx_list(const std::string& root, bool saves)
{
    std::vector<LxEntry> out;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((root + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.') continue;
        LxEntry e; e.name = fd.cFileName;
        lx_scan(root + "\\" + e.name, e);
        if (saves) e.title = lx_sfo_title(root + "\\" + e.name);
        out.push_back(e);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    std::sort(out.begin(), out.end(), [&](const LxEntry& a, const LxEntry& b) { return saves ? a.name < b.name : a.name > b.name; });
    return out;
}
static bool lx_copy_tree(const std::string& from, const std::string& to)
{
    CreateDirectoryA(to.c_str(), NULL);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((from + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool ok = true;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        const std::string a = from + "\\" + fd.cFileName, b = to + "\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ok = lx_copy_tree(a, b) && ok;
        else ok = CopyFileA(a.c_str(), b.c_str(), FALSE) && ok;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return ok;
}
static void lx_delete_tree(const std::string& dir)
{
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
            const std::string p = dir + "\\" + fd.cFileName;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) lx_delete_tree(p);
            else { SetFileAttributesA(p.c_str(), FILE_ATTRIBUTE_NORMAL); DeleteFileA(p.c_str()); }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryA(dir.c_str());
}
// a backup is save_backups\<YYYY-MM-DD_HHMMSS>_<kind>\<the savedata folders>; kind = auto / manual / before-restore
static std::string lx_backup(const char* kind)
{
    LxEntry cur; lx_scan(lx_save_dir(), cur);
    if (!cur.files) return "";
    SHCreateDirectoryExA(NULL, lx_backup_root().c_str(), NULL);
    SYSTEMTIME t; GetLocalTime(&t);
    char name[96];
    snprintf(name, sizeof name, "%04u-%02u-%02u_%02u%02u%02u_%s", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, kind);
    const std::string dst = lx_backup_root() + "\\" + name;
    if (!lx_copy_tree(lx_save_dir(), dst)) { lx_delete_tree(dst); return ""; }
    return name;
}

// RPCS3 import staging: savedata\.pcimport\<slot>.tmp (the copy) and <slot>.old (the replaced save). The game and
// the saves list skip folders starting with '.', so a leftover never shows as a slot. A run that died mid-swap is
// finished here: <slot> missing + <slot>.old = put the original back; <slot>.old next to the new <slot> = only the
// delete was missed; a .tmp is always an incomplete copy. Returns the originals that could not be moved back.
static std::string lx_import_recover()
{
    const std::string sd = lx_save_dir(), st = sd + "\\.pcimport";
    std::vector<std::string> names;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((st + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return "";
    do { if (strcmp(fd.cFileName, ".") && strcmp(fd.cFileName, "..")) names.push_back(fd.cFileName); } while (FindNextFileA(h, &fd));
    FindClose(h);
    std::string stuck;
    for (auto& n : names) {
        const std::string p = st + "\\" + n;
        const bool is_old = n.size() > 4 && !_stricmp(n.c_str() + n.size() - 4, ".old");
        if (!is_old) { lx_delete_tree(p); continue; }
        const std::string dst = sd + "\\" + n.substr(0, n.size() - 4);
        if (GetFileAttributesA(dst.c_str()) != INVALID_FILE_ATTRIBUTES) lx_delete_tree(p);
        else if (!MoveFileExA(p.c_str(), dst.c_str(), 0)) stuck += "  " + p + "\n";
    }
    RemoveDirectoryA(st.c_str());   // only goes when empty
    return stuck;
}
// before Play: back up when the saves changed since the newest backup; keep the newest N automatic ones
static void lx_auto_backup()
{
    lx_import_recover();
    const char* v = lx_val("savebackup");
    if (!strcmp(v, "0")) return;
    const int keep = *v ? std::max(1, atoi(v)) : 10;
    LxEntry cur; lx_scan(lx_save_dir(), cur);
    if (!cur.files) return;
    auto bk = lx_list(lx_backup_root(), false);
    for (auto& b : bk) if (b.newest >= cur.newest && b.files == cur.files && b.bytes == cur.bytes) return;   // unchanged
    lx_backup("auto");
    bk = lx_list(lx_backup_root(), false);      // newest first
    int n = 0;
    for (auto& b : bk) {
        if (b.name.size() < 5 || b.name.compare(b.name.size() - 5, 5, "_auto") != 0) continue;
        if (++n > keep) lx_delete_tree(lx_backup_root() + "\\" + b.name);
    }
}
// ---- mods list (mods\order.txt, read by the game's mods layer: rfom_mods.cpp, ps3emu/port_pc_extras.h pcx_mod_rank)
// Listed in the order they are applied (a later mod wins a file both replace). Folders order.txt does not
// name follow alphabetically; '_' / '-' at the start of a folder name still disables it (older layout).
struct LxMod { std::string name; bool on = true, legacy_off = false; };
static std::string lx_mods_root() { return path_of("mods"); }
static std::vector<LxMod> lx_mods_load()
{
    std::vector<std::pair<std::string, bool>> lines;     // name, disabled
    if (FILE* f = fopen((lx_mods_root() + "\\order.txt").c_str(), "r")) {
        char b[512];
        while (fgets(b, sizeof b, f)) {
            std::string l = b;
            l.erase(0, l.find_first_not_of(" \t"));
            l.erase(l.find_last_not_of(" \t\r\n") + 1);
            if (l.empty() || l[0] == '#') continue;
            const bool dis = l[0] == '-';
            lines.push_back({ dis ? l.substr(1) : l, dis });
        }
        fclose(f);
    }
    std::vector<std::pair<size_t, LxMod>> v;
    for (auto& e : lx_list(lx_mods_root(), false)) {
        // not mods: the HD UI pack folder (Advanced > HD UI pack) and caches
        if (!_stricmp(e.name.c_str(), "hd_ui") || e.name[0] == '.') continue;
        LxMod m; m.name = e.name;
        m.legacy_off = m.name[0] == '_' || m.name[0] == '-';
        size_t pos = (size_t)1 << 30;
        for (size_t i = 0; i < lines.size(); i++)
            if (!_stricmp(lines[i].first.c_str(), m.name.c_str())) { pos = i; m.on = !lines[i].second; break; }
        if (m.legacy_off) m.on = false;
        v.push_back({ pos, m });
    }
    std::sort(v.begin(), v.end(), [](const std::pair<size_t, LxMod>& a, const std::pair<size_t, LxMod>& b) {
        return a.first != b.first ? a.first < b.first : _stricmp(a.second.name.c_str(), b.second.name.c_str()) < 0; });
    std::vector<LxMod> out;
    for (auto& p : v) out.push_back(p.second);
    return out;
}
static bool lx_mods_save(const std::vector<LxMod>& mods)
{
    FILE* f = fopen((lx_mods_root() + "\\order.txt").c_str(), "w");
    if (!f) return false;
    fprintf(f, "# Mods in the order they are applied: a later mod wins a file both replace. \"-Name\" = off.\n"
               "# Written by the launcher (Saves & tools > Mods); folders not listed here come last, alphabetically.\n");
    for (auto& m : mods) if (!m.legacy_off) fprintf(f, "%s%s\n", m.on ? "" : "-", m.name.c_str());
    fclose(f);
    return true;
}

static std::vector<LxEntry> g_lx_saves, g_lx_backups;
static std::vector<LxMod> g_lx_mods;
static DWORD g_lx_loaded = 0;
static void lx_refresh(bool force)
{
    if (!force && g_lx_loaded && GetTickCount() - g_lx_loaded < 3000) return;
    g_lx_loaded = GetTickCount();
    lx_import_recover();
    g_lx_saves = lx_list(lx_save_dir(), true);
    g_lx_backups = lx_list(lx_backup_root(), false);
    g_lx_mods = lx_mods_load();
}

// ---- RPCS3 import ---------------------------------------------------------------------------------
static std::string lx_pick_folder(const wchar_t* title)
{
    std::string out;
    const HRESULT ci = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    IFileOpenDialog* d = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) {
        DWORD o = 0;
        d->GetOptions(&o);
        d->SetOptions(o | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        d->SetTitle(title);
        IShellItem* it = nullptr;
        if (SUCCEEDED(d->Show(g_hw)) && SUCCEEDED(d->GetResult(&it))) {
            PWSTR w = nullptr;
            if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &w))) {
                char b[MAX_PATH * 4];
                if (WideCharToMultiByte(CP_ACP, 0, w, -1, b, sizeof b, NULL, NULL) > 0) out = b;
                CoTaskMemFree(w);
            }
            it->Release();
        }
        d->Release();
    }
    if (SUCCEEDED(ci)) CoUninitialize();
    while (out.size() > 3 && out.back() == '\\') out.pop_back();
    return out;
}
static const char* const k_lx_ids[] = { LX_TITLE_IDS, nullptr };
static bool lx_read_file(const std::string& p, std::vector<unsigned char>& b)
{
    FILE* f = fopen(p.c_str(), "rb");
    if (!f) return false;
    b.resize(65536);
    b.resize(fread(b.data(), 1, b.size(), f));
    fclose(f);
    return !b.empty();
}
// the title ID this copy of the game saves under: the game's PARAM.SFO, else the saves already there
static std::string lx_local_id()
{
    std::vector<unsigned char> b;
    char id[32];
    if (lx_read_file(gf_path("PS3_GAME\\PARAM.SFO"), b) && pcx_sfo_get(b.data(), b.size(), "TITLE_ID", id, sizeof id) && strlen(id) == 9) return id;
    for (const char* p : { "game\\PS3_GAME\\PARAM.SFO", "gamefiles\\PS3_GAME\\PARAM.SFO", "PS3_GAME\\PARAM.SFO", "game\\PARAM.SFO" })
        if (lx_read_file(path_of(p), b) && pcx_sfo_get(b.data(), b.size(), "TITLE_ID", id, sizeof id) && strlen(id) == 9) return id;
    for (auto& e : lx_list(lx_save_dir(), false))
        if (pcx_title_match(e.name.c_str(), k_lx_ids)) return e.name.substr(0, 9);
    return k_lx_ids[0];
}
static void lx_import_rpcs3()
{
    const std::string pick = lx_pick_folder(L"Pick the RPCS3 folder (or its dev_hdd0 or savedata folder)");
    if (pick.empty()) return;
    const std::string left = lx_import_recover();
    if (!left.empty()) {
        MessageBoxA(g_hw, ("An earlier import left these saved games aside and they could not be moved back yet (in use?):\n\n" +
                           left + "\nClose the game and try again.").c_str(), "Import saves from RPCS3", MB_OK | MB_ICONWARNING);
        return;
    }
    std::string src;
    for (const char* sub : { "\\dev_hdd0\\home\\00000001\\savedata", "\\home\\00000001\\savedata", "\\00000001\\savedata", "\\savedata", "" }) {
        const DWORD a = GetFileAttributesA((pick + sub).c_str());
        if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) { src = pick + sub; break; }
    }
    const std::string local = lx_local_id(), sd = lx_save_dir();
    struct Item { std::string from, to, title; bool local; ULONGLONG mtime; };
    std::vector<Item> items;
    std::string list, skipped;
    for (auto& e : lx_list(src, true)) {
        if (!pcx_title_match(e.name.c_str(), k_lx_ids)) continue;
        WIN32_FILE_ATTRIBUTE_DATA fa;
        if (!GetFileAttributesExA((src + "\\" + e.name + "\\PARAM.SFO").c_str(), GetFileExInfoStandard, &fa)) continue;
        char to[260];
        Item it{ e.name, pcx_save_retarget(e.name.c_str(), k_lx_ids, local.c_str(), to, sizeof to) ? std::string(to) : e.name,
                 e.title, !_strnicmp(e.name.c_str(), local.c_str(), 9),
                 ((ULONGLONG)fa.ftLastWriteTime.dwHighDateTime << 32) | fa.ftLastWriteTime.dwLowDateTime };
        // two regions' saves of the same slot: keep this copy's region, else the newer one
        bool dup = false;
        for (auto& o : items) {
            if (_stricmp(o.to.c_str(), it.to.c_str())) continue;
            if (pcx_import_prefer(it.local, it.mtime, o.local, o.mtime)) std::swap(o, it);   // o = the winner
            skipped += "  " + it.from + "  (same slot as " + o.from + ")\n";
            dup = true;
            break;
        }
        if (!dup) items.push_back(it);
    }
    for (auto& it : items) {
        const bool exists = GetFileAttributesA((sd + "\\" + it.to).c_str()) != INVALID_FILE_ATTRIBUTES;
        list += "  " + (it.title.empty() ? it.from : it.title + "  (" + it.from + ")") + (it.to != it.from ? "  -> " + it.to : "") +
                (exists ? "  [replaces yours]" : "") + "\n";
    }
    if (items.empty()) {
        MessageBoxA(g_hw, ("No Resistance: Fall of Man saved games found in\n" + (src.empty() ? pick : src) +
                           "\n\nPick the RPCS3 folder (the one with rpcs3.exe and dev_hdd0).").c_str(),
                    "Import saves from RPCS3", MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (MessageBoxA(g_hw, ("Copy these saved games from RPCS3?\n\n" + list +
                           (skipped.empty() ? std::string() : "\nSkipped (another region's save of the same slot; this "
                            "copy's region or the newer one is used):\n" + skipped) +
                           "\nYour current saves are backed up first "
                           "(\"before-import\"). Don't do this while the game is running.").c_str(),
                    "Import saves from RPCS3", MB_YESNO | MB_ICONQUESTION) != IDYES) return;
    {
        LxEntry cur; lx_scan(sd, cur);
        if (cur.files && lx_backup("before-import").empty()) {
            MessageBoxA(g_hw, ("Could not back up your current saves to\n" + lx_backup_root() +
                               "\n\n(disk full, or the folder is read-only?), so nothing was imported.").c_str(),
                        "Import saves from RPCS3", MB_OK | MB_ICONERROR);
            return;
        }
    }
    SHCreateDirectoryExA(NULL, sd.c_str(), NULL);
    const std::string stage = sd + "\\.pcimport";
    CreateDirectoryA(stage.c_str(), NULL);
    int ok = 0;
    std::string stuck;
    for (auto& it : items) {
        // copy into the staging folder first; only a complete copy replaces the slot (the old one is moved
        // aside, then deleted; lx_import_recover finishes a swap a crash cut short)
        const std::string dst = sd + "\\" + it.to, tmp = stage + "\\" + it.to + ".tmp", old = stage + "\\" + it.to + ".old";
        lx_delete_tree(tmp);
        bool good = lx_copy_tree(src + "\\" + it.from, tmp);
        if (good && it.to != it.from) {
            // another region's save: its PARAM.SFO names the folder too
            std::vector<unsigned char> b;
            size_t off = 0, len = 0;
            if (lx_read_file(tmp + "\\PARAM.SFO", b) && pcx_sfo_find(b.data(), b.size(), "SAVEDATA_DIRECTORY", &off, &len) &&
                len >= 9 && !memcmp(&b[off], it.from.data(), 9)) {
                memcpy(&b[off], it.to.data(), 9);
                FILE* f = fopen((tmp + "\\PARAM.SFO").c_str(), "r+b");
                good = f && fwrite(b.data(), 1, b.size(), f) == b.size();
                if (f && fclose(f)) good = false;
            }
        }
        const bool had = GetFileAttributesA(dst.c_str()) != INVALID_FILE_ATTRIBUTES;
        if (good && had) { lx_delete_tree(old); good = MoveFileExA(dst.c_str(), old.c_str(), 0) != 0; }
        if (good && !MoveFileExA(tmp.c_str(), dst.c_str(), 0)) {
            if (had && !MoveFileExA(old.c_str(), dst.c_str(), 0)) stuck += "  " + old + "\n";   // put the original back
            good = false;
        }
        if (!good) { lx_delete_tree(tmp); continue; }
        if (had) lx_delete_tree(old);
        ok++;
    }
    RemoveDirectoryA(stage.c_str());
    if (!stuck.empty())
        MessageBoxA(g_hw, ("These saved games could not be put back after a failed copy. They are safe here (the "
                           "launcher retries on its next start), or move them back into\n" + sd +
                           "\nwithout the .old ending:\n\n" + stuck).c_str(), "Import saves from RPCS3", MB_OK | MB_ICONWARNING);
    toast(std::to_string(ok) + " of " + std::to_string(items.size()) + " saved games imported from RPCS3." +
          (ok < (int)items.size() ? " The others could not be copied; those slots were left as they were." : ""));
    lx_refresh(true);
}
static std::string lx_when(ULONGLONG ft)
{
    if (!ft) return "";
    FILETIME f = { (DWORD)ft, (DWORD)(ft >> 32) }, l; SYSTEMTIME s;
    FileTimeToLocalFileTime(&f, &l); FileTimeToSystemTime(&l, &s);
    char b[64]; snprintf(b, sizeof b, "%04u-%02u-%02u %02u:%02u", s.wYear, s.wMonth, s.wDay, s.wHour, s.wMinute);
    return b;
}
static std::string lx_size(ULONGLONG b) { char s[32]; snprintf(s, sizeof s, b > 1048576 ? "%.1f MB" : "%.0f KB", b > 1048576 ? b / 1048576.0 : b / 1024.0); return s; }

// ---- benchmark ---------------------------------------------------------------------------------------
static std::string lx_last_bench()
{
    WIN32_FIND_DATAA fd; std::string best; FILETIME bt = {};
    HANDLE h = FindFirstFileA(path_of("logs\\bench\\RFOM_bench_*.txt").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return "";
    do { if (CompareFileTime(&fd.ftLastWriteTime, &bt) > 0) { bt = fd.ftLastWriteTime; best = fd.cFileName; } } while (FindNextFileA(h, &fd));
    FindClose(h);
    FILE* f = fopen(path_of(("logs\\bench\\" + best).c_str()).c_str(), "r");
    if (!f) return "";
    std::string avg, low, low01, stut, usd, mode, line; char b[256];
    while (fgets(b, sizeof b, f)) {
        line = b; line.erase(line.find_last_not_of("\r\n ") + 1);
        auto v = [&](const char* k) {
            if (line.compare(0, strlen(k), k) != 0) return std::string();
            const size_t s = line.find_first_not_of(' ', strlen(k));
            return s == std::string::npos ? std::string() : line.substr(s); };
        if (!v("avg fps").empty()) avg = v("avg fps");
        if (!v("1% low fps").empty()) low = v("1% low fps");
        if (!v("0.1% low fps").empty()) low01 = v("0.1% low fps");
        if (!v("stutters ").empty()) stut = v("stutters ");
        if (!v("us/draw").empty()) usd = v("us/draw");
        if (!v("mode").empty()) mode = v("mode");
    }
    fclose(f);
    std::string s = (mode == "hotkey capture" ? "Last capture (" : "Last run (") +
                    lx_when(((ULONGLONG)bt.dwHighDateTime << 32) | bt.dwLowDateTime) + "):  " + avg + " fps average,  " + low + " 1% low";
    if (!low01.empty()) s += ",  " + low01 + " 0.1% low";
    if (!stut.empty()) s += ",  " + stut + " stutters";
    if (!usd.empty()) s += ",  " + usd + " \xC2\xB5s per draw";
    return s;
}

// ---- Game files folder (port_gamefiles.h) ---------------------------------------------------------------------
// The folder with the player's copy of the game (PS3_GAME). Default <port>\gamefiles; another one is kept in
// settings.ini gamefiles= / settings.bat PORT_GAMEFILES, which run.bat (PS3_VFS_ROOT) and setup.ps1 (updates) read.
// The game also writes the PS3 hard-drive folders cache\<id> (rebuilt when missing) and game\<id> (its own data, R1:
// quick.sav and settings) there; saved games are elsewhere (lx_save_dir).
static std::string lx_gf_note()
{
    int src = PORT_GF_DEFAULT;
    const std::string gf = gf_dir(&src);
    std::string s = gf + (src == PORT_GF_ENV ? "   (from the PS3_VFS_ROOT environment variable, which wins over this setting)"
                          : src == PORT_GF_SETTING ? "" : "   (the default: Setup copied your game here)");
    if (!port_gamefiles_problem(gf, k_gf_required).empty()) s += "\nNot found or not complete: Play will not start until it is.";
    return s;
}
static void lx_gf_apply(const std::string& dir)   // dir: a checked, complete game files folder
{
    const std::string old = gf_dir(), stored = port_gamefiles_same(dir, path_of("gamefiles")) ? "" : port_gamefiles_clean(dir);
    std::string id = port_gamefiles_title_id(dir);
    if (id.size() != 9) id = k_gf_title_ids[0];
    // the game's own data on the PS3 hard drive (game\<id>): offered to the new folder when it has none, so the
    // game does not start as on a new PS3. cache\<id> is only a cache: the game fills it again.
    const std::string from = old + "\\game\\" + id, to = dir + "\\game\\" + id;
    if (!port_gamefiles_same(old, dir) && port_gamefiles_is_dir(from)) {
        LxEntry e; lx_scan(from, e);
        LxEntry t; if (port_gamefiles_is_dir(to)) lx_scan(to, t);
        if (e.files > 0 && t.files == 0) {
            const int r = MessageBoxA(g_hw, ("Copy the game's data (game\\" + id + ", " + lx_size(e.bytes) + ": its settings and "
                "the quick save) from\n" + old + "\nto the new folder?\n\nYes = copy (recommended). No = the game starts there "
                "as on a new PS3. Your saved games are not in this folder and stay where they are.").c_str(),
                "Game files folder", MB_YESNOCANCEL | MB_ICONQUESTION);
            if (r == IDCANCEL) return;
            if (r == IDYES) {
                SHCreateDirectoryExA(NULL, (dir + "\\game").c_str(), NULL);
                HCURSOR was = SetCursor(LoadCursor(NULL, IDC_WAIT));
                const bool ok = lx_copy_tree(from, to);
                SetCursor(was);
                if (!ok) {
                    lx_delete_tree(to);   // it was empty: do not leave half of it
                    MessageBoxA(g_hw, ("Could not copy " + from + " to " + to + ". Is the folder writable? Nothing was changed.").c_str(),
                                "Game files folder", MB_ICONERROR);
                    return;
                }
            }
        }
    }
    // the PS3 hard-drive folders the game expects (as Setup makes them); this also checks the folder is writable
    const std::string cache = dir + "\\cache\\" + id, gdata = dir + "\\game\\" + id + "\\USRDIR";
    SHCreateDirectoryExA(NULL, cache.c_str(), NULL); SHCreateDirectoryExA(NULL, gdata.c_str(), NULL);
    if (!port_gamefiles_is_dir(cache) || !port_gamefiles_is_dir(gdata)) {
        MessageBoxA(g_hw, ("The game writes to its game files folder (cache\\" + id + ", game\\" + id + "), but this one is not "
                           "writable:\n" + dir + "\n\nPick a folder you can write to. Nothing was changed.").c_str(),
                    "Game files folder", MB_ICONERROR);
        return;
    }
    g_gamefiles = stored;
    if (!save_settings()) { MessageBoxA(g_hw, "Could not write settings.ini / settings.bat.", "Game files folder", MB_ICONERROR); return; }
    int src = PORT_GF_DEFAULT; gf_dir(&src);
    if (src == PORT_GF_ENV)
        MessageBoxA(g_hw, "Saved. But PS3_VFS_ROOT is set in your environment, and it wins over this setting: remove it "
                          "(Windows: Edit environment variables) to use the folder picked here.", "Game files folder", MB_ICONWARNING);
    toast(stored.empty() ? "Game files: the default folder (gamefiles)." : "Game files folder: " + stored);
}
static void lx_gf_pick()
{
    const std::string pick = lx_pick_folder(L"Pick your game files folder (the one with PS3_GAME in it)");
    if (pick.empty()) return;
    const std::string dir = port_gamefiles_normalize(pick);
    const std::string bad_path = port_gamefiles_same(dir, path_of("gamefiles")) ? "" : port_gamefiles_path_problem(dir);
    const std::string bad = !bad_path.empty() ? bad_path : port_gamefiles_problem(dir, k_gf_required);
    if (!bad.empty()) { MessageBoxA(g_hw, (bad + "\n\nNothing was changed.").c_str(), "Game files folder", MB_ICONERROR); return; }
    const std::string id = port_gamefiles_title_id(dir);
    bool known = false;
    for (const char* const* k = k_gf_title_ids; *k; k++) known = known || id == *k;
    if (!known && MessageBoxA(g_hw, ("This copy of the game is " + (id.empty() ? std::string("of an unknown version") : id) +
                                     ", but the port is made for " + k_gf_title_ids[0] + " (the version Setup checks). Use it anyway?").c_str(),
                              "Game files folder", MB_YESNO | MB_ICONWARNING) != IDYES) return;
    lx_gf_apply(dir);
}
static void lx_gf_default()
{
    const std::string def = path_of("gamefiles");
    if (!port_gamefiles_problem(def, k_gf_required).empty()) {
        if (MessageBoxA(g_hw, ("The default folder has no complete copy of the game:\n" + def + "\n\nUse it anyway? Play will not "
                               "start until Setup copies your game there (or you pick a folder again).").c_str(),
                        "Game files folder", MB_YESNO | MB_ICONWARNING) != IDYES) return;
        g_gamefiles.clear();
        if (!save_settings()) MessageBoxA(g_hw, "Could not write settings.ini / settings.bat.", "Game files folder", MB_ICONERROR);
        else toast("Game files: the default folder (gamefiles).");
        return;
    }
    lx_gf_apply(def);
}

enum { BT_X_BACKUP = 100, BT_X_OPEN_SAVES, BT_X_OPEN_BACKUPS, BT_X_BENCH, BT_X_BENCH_LOGS, BT_X_RESTORE, BT_X_DELETE,
       BT_X_IMPORT, BT_X_MOD_UP, BT_X_MOD_DOWN, BT_X_MOD_TOGGLE, BT_X_OPEN_MODS, BT_X_HDUI_MAKE, BT_X_HDUI_REMOVE,
       BT_X_GF_PICK, BT_X_GF_DEFAULT, BT_X_GF_OPEN };

static void lx_layout_tools(float x, float& y, float w)
{
    lx_refresh(false);
    deco(D_H1, RectF(x, y, w, SF(34)), "Saves & tools"); y += SF(36);
    deco(D_INTRO, RectF(x, y, w, SF(20)), "Back up, restore and import saved games, manage mods, choose where your game files "
                                          "are, and measure performance.");
    y += SF(40);
    deco(D_H2, RectF(x, y, w, SF(22)), "SAVED GAMES"); y += SF(30);
    if (g_lx_saves.empty()) { deco(D_NOTE, RectF(x, y, w, SF(22)), "No saved games yet."); y += SF(30); }
    for (size_t i = 0; i < g_lx_saves.size(); i++) {
        const LxEntry& e = g_lx_saves[i];
        Widget& row = add(W_ROW, RectF(x - SF(12), y, w + SF(24), SF(34)), -300 - (int)i);
        row.title = e.name; row.help = e.title.empty() ? e.name : e.title + "  (" + e.name + ")";
        deco(D_LABEL, RectF(x, y, w - SF(260), SF(34)), e.title.empty() ? e.name : e.title);
        deco(D_LABEL_DIM, RectF(x + w - SF(250), y, SF(250), SF(34)), lx_when(e.newest) + "  \xC2\xB7  " + lx_size(e.bytes));
        y += SF(36);
    }
    y += SF(8);
    Widget& b1 = add(W_BUTTON, RectF(x, y, SF(150), SF(38)), BT_X_BACKUP);
    b1.title = "Back up now"; b1.help = "Copies all saved games to save_backups (kept until you delete them).";
    Widget& b2 = add(W_BUTTON, RectF(x + SF(162), y, SF(170), SF(38)), BT_SAVES);
    b2.title = "Open save folder"; b2.help = "The game's save folder (see Display \xE2\x80\xBA Save location).";
    Widget& b3 = add(W_BUTTON, RectF(x + SF(344), y, SF(190), SF(38)), BT_X_OPEN_BACKUPS);
    b3.title = "Open backups folder"; b3.help = lx_backup_root();
    y += SF(46);
    Widget& bi = add(W_BUTTON, RectF(x, y, SF(240), SF(38)), BT_X_IMPORT);
    bi.title = "Import saves from RPCS3"; bi.help = "Pick your RPCS3 folder: copies its Resistance: Fall of Man saved games (any "
        "region) into this game's saves. Yours are backed up first; a save of the same slot is replaced.";
    y += SF(56);
    deco(D_H2, RectF(x, y, w, SF(22)), "BACKUPS"); y += SF(30);
    if (g_lx_backups.empty()) { deco(D_NOTE, RectF(x, y, w, SF(22)), "No backups yet. Play makes one automatically (Advanced page, "
                                                                     "\"Back up saves before playing\")."); y += SF(30); }
    for (size_t i = 0; i < g_lx_backups.size() && i < 40; i++) {
        const LxEntry& e = g_lx_backups[i];
        Widget& row = add(W_ROW, RectF(x - SF(12), y, w + SF(24), SF(40)), -400 - (int)i);
        row.title = e.name; row.help = "Backup " + e.name + ": " + std::to_string(e.files) + " files, " + lx_size(e.bytes) + ".";
        deco(D_LABEL, RectF(x, y, w - SF(230), SF(40)), e.name);
        Widget& r = add(W_BUTTON, RectF(x + w - SF(220), y + SF(4), SF(110), SF(32)), BT_X_RESTORE, (int)i);
        r.title = "Restore"; r.help = "Replaces your current saved games with this backup (they are backed up first).";
        Widget& d = add(W_BUTTON, RectF(x + w - SF(100), y + SF(4), SF(100), SF(32)), BT_X_DELETE, (int)i);
        d.title = "Delete"; d.help = "Deletes this backup.";
        y += SF(42);
    }
    y += SF(24);
    deco(D_H2, RectF(x, y, w, SF(22)), "MODS"); y += SF(30);
    deco(D_NOTE, RectF(x, y, w, SF(40)), std::string(strcmp(val(S_MODS), "") && strcmp(val(S_MODS), "0") ? "" :
         "Mods are off: turn on Advanced \xE2\x80\xBA Mods folder. ") +
         "Each folder in mods\\ is one mod. They are applied top to bottom: a lower one wins a file both replace.");
    y += SF(44);
    if (g_lx_mods.empty()) { deco(D_NOTE, RectF(x, y, w, SF(22)), "No mods installed (mods\\<mod name>\\USRDIR\\...)."); y += SF(30); }
    for (size_t i = 0; i < g_lx_mods.size() && i < 60; i++) {
        const LxMod& m = g_lx_mods[i];
        Widget& row = add(W_ROW, RectF(x - SF(12), y, w + SF(24), SF(40)), -500 - (int)i);
        row.title = m.name;
        row.help = m.legacy_off ? "Off: the folder name starts with '_' or '-'. Rename it to turn it on." : "mods\\" + m.name;
        deco(m.on ? D_LABEL : D_LABEL_DIM, RectF(x, y, w - SF(330), SF(40)), m.name + (m.on ? "" : "   (off)"));
        Widget& u = add(W_BUTTON, RectF(x + w - SF(320), y + SF(4), SF(90), SF(32)), BT_X_MOD_UP, (int)i);
        u.title = "Up"; u.help = "Applied earlier (a mod below it wins a shared file)."; u.disabled = i == 0 || m.legacy_off;
        Widget& dn = add(W_BUTTON, RectF(x + w - SF(220), y + SF(4), SF(90), SF(32)), BT_X_MOD_DOWN, (int)i);
        dn.title = "Down"; dn.help = "Applied later (wins a file it shares with the mods above)."; dn.disabled = i + 1 == g_lx_mods.size() || m.legacy_off;
        Widget& t = add(W_BUTTON, RectF(x + w - SF(120), y + SF(4), SF(120), SF(32)), BT_X_MOD_TOGGLE, (int)i);
        t.title = m.on ? "Turn off" : "Turn on"; t.help = "Turns this mod on or off (mods\\order.txt)."; t.disabled = m.legacy_off;
        y += SF(42);
    }
    Widget& bm = add(W_BUTTON, RectF(x, y + SF(4), SF(190), SF(38)), BT_X_OPEN_MODS);
    bm.title = "Open mods folder"; bm.help = "mods\\ in the game folder (created if missing).";
    y += SF(56);
    deco(D_H2, RectF(x, y, w, SF(22)), "HD UI PACK"); y += SF(30);
    {
        const bool have = hdui_have_pack();
        deco(D_NOTE, RectF(x, y, w, SF(60)), std::string(have ? (*lx_val("hdui") ? "Installed and on. " : "Installed, but off (Advanced > HD UI pack). ")
                                                              : "Not created yet. ") +
             "Upscales the menu and HUD textures 2-4x with an AI model on the graphics card (the CPU if that fails), from "
             "your own game files (minutes on a recent graphics card, several GB once). Then turns Advanced > HD UI pack on.");
        y += SF(66);
        Widget& hm = add(W_BUTTON, RectF(x, y, SF(240), SF(38)), BT_X_HDUI_MAKE);
        hm.title = have ? "Update HD UI pack" : "Create HD UI pack";
        hm.help = "Makes mods\\hd_ui from the game files on the graphics card, or the CPU if that fails (Real-ESRGAN comes with the "
                  "port), skips textures already made. "
                  "Log: logs\\hd_ui_pack.log.";
        Widget& hr = add(W_BUTTON, RectF(x + SF(252), y, SF(190), SF(38)), BT_X_HDUI_REMOVE);
        hr.title = "Remove HD UI pack"; hr.help = "Deletes mods\\hd_ui and turns HD UI pack off."; hr.disabled = !have;
        y += SF(56);
    }
    deco(D_H2, RectF(x, y, w, SF(22)), "GAME FILES FOLDER"); y += SF(30);
    deco(D_NOTE, RectF(x, y, w, SF(40)), lx_gf_note());
    y += SF(44);
    {
        Widget& gp = add(W_BUTTON, RectF(x, y, SF(190), SF(38)), BT_X_GF_PICK);
        gp.title = "Choose folder..."; gp.help = "Where your copy of the game is (the folder with PS3_GAME in it), if you moved "
            "it out of gamefiles. Play, updates and Setup use it. Checked before it is saved.";
        Widget& gd = add(W_BUTTON, RectF(x + SF(202), y, SF(150), SF(38)), BT_X_GF_DEFAULT);
        gd.title = "Use default"; gd.help = "Back to gamefiles in the game folder."; gd.disabled = g_gamefiles.empty();
        Widget& go = add(W_BUTTON, RectF(x + SF(364), y, SF(150), SF(38)), BT_X_GF_OPEN);
        go.title = "Open folder"; go.help = "Opens the game files folder in Explorer.";
        y += SF(56);
    }
    deco(D_H2, RectF(x, y, w, SF(22)), "BENCHMARK"); y += SF(30);
    deco(D_NOTE, RectF(x, y, w, SF(60)), "Starts the game with your settings and continues your campaign from the title "
                                         "screen by itself; in the level it waits 45 s, measures for 60 s, writes logs\\bench\\ "
                                         "and closes. Don't touch the keyboard or controller while it runs (about 5 minutes). "
                                         "Or press F11 in game to record any stretch yourself (F11 again stops).");
    y += SF(66);
    Widget& b4 = add(W_BUTTON, RectF(x, y, SF(170), SF(38)), BT_X_BENCH);
    b4.title = "Run benchmark"; b4.help = "Average fps, 1% / 0.1% low (average of the slowest frames), stutters and CPU time per draw, plus a CSV of every frame time, to logs\\bench\\. Needs a saved "
                                          "campaign to continue (or set RFOM_BENCH_SCRIPT=none and start a level yourself).";
    Widget& b5 = add(W_BUTTON, RectF(x + SF(182), y, SF(170), SF(38)), BT_X_BENCH_LOGS);
    b5.title = "Open results"; b5.help = "logs\\bench\\: one report per run and bench_history.csv.";
    y += SF(46);
    const std::string last = lx_last_bench();
    if (!last.empty()) { deco(D_LABEL_DIM, RectF(x, y, w, SF(28)), last); y += SF(30); }
}

// returns true when the button was ours
static bool lx_click(int id, int sub)
{
    if (id == BT_X_BACKUP) {
        const std::string n = lx_backup("manual");
        toast(n.empty() ? "Nothing to back up (no saved games yet)." : "Saved games backed up to save_backups\\" + n);
        lx_refresh(true);
    } else if (id == BT_X_OPEN_BACKUPS) {
        SHCreateDirectoryExA(NULL, lx_backup_root().c_str(), NULL);
        ShellExecuteA(g_hw, "open", lx_backup_root().c_str(), NULL, NULL, SW_SHOWNORMAL);
    } else if (id == BT_X_RESTORE || id == BT_X_DELETE) {
        lx_refresh(true);
        if (sub < 0 || sub >= (int)g_lx_backups.size()) return true;
        const std::string name = g_lx_backups[sub].name, src = lx_backup_root() + "\\" + name;
        if (id == BT_X_DELETE) {
            if (MessageBoxA(g_hw, ("Delete the backup " + name + "?").c_str(), "Saves", MB_YESNO | MB_ICONQUESTION) == IDYES) {
                lx_delete_tree(src); toast("Backup deleted.");
            }
        } else if (MessageBoxA(g_hw, ("Replace your saved games with the backup " + name + "?\n\nYour current saves are backed "
                                      "up first (\"before-restore\"). Don't do this while the game is running.").c_str(),
                               "Restore saves", MB_YESNO | MB_ICONWARNING) == IDYES) {
            lx_backup("before-restore");
            const std::string sd = lx_save_dir();
            for (auto& s : lx_list(sd, true)) lx_delete_tree(sd + "\\" + s.name);
            SHCreateDirectoryExA(NULL, sd.c_str(), NULL);
            toast(lx_copy_tree(src, sd) ? "Saved games restored from " + name + "." : "Restore failed - see save_backups.");
        }
        lx_refresh(true);
    } else if (id == BT_X_IMPORT) {
        lx_import_rpcs3();
    } else if (id == BT_X_MOD_UP || id == BT_X_MOD_DOWN || id == BT_X_MOD_TOGGLE) {
        g_lx_mods = lx_mods_load();
        if (sub < 0 || sub >= (int)g_lx_mods.size() || g_lx_mods[sub].legacy_off) return true;
        if (id == BT_X_MOD_TOGGLE) g_lx_mods[sub].on = !g_lx_mods[sub].on;
        else {
            const int to = sub + (id == BT_X_MOD_UP ? -1 : 1);
            if (to < 0 || to >= (int)g_lx_mods.size()) return true;
            std::swap(g_lx_mods[sub], g_lx_mods[to]);
        }
        if (!lx_mods_save(g_lx_mods)) toast("Could not write mods\\order.txt.");
        lx_refresh(true);
    } else if (id == BT_X_OPEN_MODS) {
        CreateDirectoryA(lx_mods_root().c_str(), NULL);
        ShellExecuteA(g_hw, "open", lx_mods_root().c_str(), NULL, NULL, SW_SHOWNORMAL);
    } else if (id == BT_X_HDUI_MAKE) {
        hdui_start();
    } else if (id == BT_X_HDUI_REMOVE) {
        hdui_remove();
    } else if (id == BT_X_GF_PICK) {
        lx_gf_pick();
    } else if (id == BT_X_GF_DEFAULT) {
        lx_gf_default();
    } else if (id == BT_X_GF_OPEN) {
        ShellExecuteA(g_hw, "open", gf_dir().c_str(), NULL, NULL, SW_SHOWNORMAL);
    } else if (id == BT_X_BENCH) {
        if (MessageBoxA(g_hw, "Run the benchmark now?\n\nThe game starts, continues your campaign by itself, measures for 60 "
                              "seconds in the level and closes. Don't press anything while it runs.", "Benchmark",
                        MB_OKCANCEL | MB_ICONINFORMATION) != IDOK) return true;
        SetEnvironmentVariableA("RFOM_BENCH", "1");      // the game also turns pause-on-focus-loss off for the run
        play();
    } else if (id == BT_X_BENCH_LOGS) {
        CreateDirectoryA(path_of("logs").c_str(), NULL); CreateDirectoryA(path_of("logs\\bench").c_str(), NULL);
        ShellExecuteA(g_hw, "open", path_of("logs\\bench").c_str(), NULL, NULL, SW_SHOWNORMAL);
    } else return false;
    return true;
}
