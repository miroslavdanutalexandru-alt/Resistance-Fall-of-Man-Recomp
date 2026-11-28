# Resistance: Fall of Man PC port: community beta release notes

**Version:** 0.3.2-beta (GitHub pre-release). **Release date:** 2026-11-28.

A native PC port of *Resistance: Fall of Man* (PS3) built by static recompilation: the game's PowerPC and SPU code is translated to C/C++ and compiled for Windows. Nothing is emulated. It renders through the port's own **Direct3D 12** renderer.

> **No game files and no prebuilt game are included.** This release is the source code plus Setup. You need your own legally dumped copy of the game (see *Install*); Setup builds the game on your PC from that copy.

This is a **beta**. The campaign has been played up to Cheddar Gorge. Later levels haven't been tested. Please report what you find (see *Reporting problems*).

## 0.3.2-beta (2026-11-28)

An update on top of 0.3.1: real surround sound, a choice of ambient occlusion, game files on another drive, steadier ultrawide menus, Frame Generation at twice the real frame rate with less ghosting, and fixes for zoom on Hold and fixed-refresh pacing. Online play on a community server is in the launcher as an experiment (off by default). Run Setup over your install (or use *Update now* in the launcher); your settings, saves and game copy are kept. **This update changes the generated game code, so Setup / *Update now* regenerates the PowerPC code once: expect 10 to 25 minutes before the build itself starts.** Tested in game for this release: Frame Generation at twice the real frame rate, the Frame Generation real camera and zoom on Hold (#37). Items marked *Not yet tested* are new and have not been played with yet; please report how they work.

### Fixes
- **Zoom set to Hold sometimes stayed zoomed (issue #37).** The game's zoom is a toggle, so Hold releases and clicks R3 again; a slow frame could merge the two into one press. The click is now timed in game frames as well, and if the view is still zoomed 400 ms later it is clicked again (at most 3 times in 3 s). `PAD_AIM_HOLD_OLD=1` restores the old click.
- **Fixed refresh: 48 fps instead of 36 when the PC can't hold 72 (issue #38).** On a 144 Hz screen without VRR, Frame rate 60 is paced at 72; a PC that could not hold that fell to a steady 36. Now more than 20% late frames over 2 s step down one refresh (72 -> 48), and after 10 s of clean frames the faster rate is tried again. Loading screens and stalls do not count. `PORT_PACING_ADAPT=0` restores the old pacing. Only visible without G-Sync / FreeSync. *Not yet tested in game.*
- **Ultrawide: the menu screen no longer flips between 21:9 and 16:9 (issue #2).** Menus and other screens drawn without player control could set a 3D camera now and then, or draw short bursts of 3D, which turned the stretch on for a moment each time. Now, once the picture is back at its 16:9 letterbox outside play, it stays there until you are in control again (the stretch returns at once) or the game has drawn 3D steadily for a second (cutscenes; a cutscene right after a loading screen may start letterboxed for that second). A pause menu with the level still drawn behind it may settle at 21:9 after that second, but no longer flips. Gameplay, aiming down sights and scopes keep the 21:9 picture as before. `RFOM_ASPECT_REARM_MS=<ms>` (default 1000) sets how long the 3D has to be steady; `RFOM_MENU_ASPECT=0` (or `RFOM_ASPECT_REARM_MS=0`) restores the 0.3.1 behaviour. *Not yet tested at 21:9; please report.*
- **Menu stalled for about 20 s after an attract movie** (RecompLauncher report 128478). At the end of the movie that plays when you leave the title screen alone, the movie player waited for an event nothing sends any more. The wait is now capped, as it already was in the game itself (`RFOM_MOVIE_EVWAIT_MS`, default 100; `0` = the old wait). *Not yet tested in game.*
- **A game that closes itself no longer freezes.** When the game program quits on its own (an internal error), the port used to stop on the last frame. It now saves a report in `logs\crash` and shows "The game hit an internal error and closed. A report was saved in logs\crash." Please send that report. Quitting yourself (window close, quit to desktop) is unchanged. `PORT_GUEST_EXIT_QUIT=0` = old. *Not yet tested in game.*
- **Ambient occlusion (CACAO / Port SSAO) no longer greys buildings seen through smoke and fog.** The AO was multiplied into the finished scene, after smoke, fog and other see-through effects, so they were darkened with the shape of the walls behind them. It is now applied at the end of the solid scene, before the first see-through effect. `RFOM_AO_BEFORE_FX=0` restores the old point. *Not yet tested in game.*
- **Ambient occlusion CACAO tuned for Fall of Man (issue #45):** building fronts and walls are no longer greyed (smaller radius, half strength, fades out from 20 to 90 m). *Not yet tested in game.*
- **Setup with a Python managed by uv (issue #16):** such a Python refuses `pip install` (externally-managed-environment), so pycryptodome could not be installed. Setup now retries with the flag that allows it, and `Setup.bat -RemoveTools` removes it the same way. Other Pythons are unchanged.
- **Linux build (issue #30):** the game no longer fails to link on a case-sensitive file system (`synchronization.lib`).

### Performance
- **No debug statistics during normal play.** Periodic statistics and trace lines that 0.3.1 still produced by default (the `[surmix]` timing, `[DLSS] camera unsolved`, the per-draw `[AAUV]` check, `[spu-sched] 10s`, the `[mfirst]` list, the first `[fs] read` lines and the frame-time recorder) are now off, so a normal run spends no time on clock reads, counters or log formatting for them. `logs\run.log` keeps the start-up settings, warnings, errors and crash / hang reports. For bug hunting, `PORT_DIAG=1` turns all of them back on (`PORT_STATS_LOG=1` the statistics only, `PORT_FRAMETIME=1` the frame-time report). Nothing in the game itself changes.

### Improvements
- **Frame Generation: twice the real frame rate (about 60 instead of 30 fps rendered).** With DLSS Frame Generation, Reflex's frame limit was applied to the shown frames, so 2x generation halved the real frames and every generated frame had to bridge twice the motion (more ghosting). The limit is now set per shown frame. `RFOM_REFLEX_FG_DIV=0` restores the old limit.
- **Frame Generation: floors and walls no longer ghost in quick turns.** Far world draws whose matched motion was exactly zero (whole floors and building fronts) now get the camera's motion (`RSX_DLSS_MV_ZEROFIX=0` = off). The gun and arms keep zero screen motion when they stay put on screen (`RSX_DLSS_MV_VIEWSTILL=0` = off).
- **Frame Generation gets the real camera.** DLSS-G was told the camera never moves; its scene-change detection and disocclusion now get a camera position and axes that follow the game's camera (`RFOM_DLSS_FG_REAL_CAM=0` = the old fixed camera).
- **Frame generation: less ghosting on the weapon, doors and other moving objects.** Generated frames need to know how each object moved. Objects that could not be paired confidently with last frame's draw (two identical doors, the first-person arms, an object whose material changed) used to get the camera's motion only, so they ghosted against the background. Those objects are now paired by where they appear on screen. Near-eye objects such as the weapon and arms can be paired with the draw that stayed on screen. An object whose material changed is paired by its mesh. This changes only the motion data, not the picture. Each part can be turned off: `RSX_DLSS_MV_REMATCH=0`, `RSX_DLSS_MV_VIEWLOCK=0` and `RSX_DLSS_MV_LOOSE=0`. With `PORT_STATS_LOG=1`, the `[DLSS] object motion/600` lines in `logs\run.log` show how many objects were paired this way and which still have only the camera's motion. *Not yet tested in game.*
- **Frame Generation: whole scene doubled while turning, ported from the Resistance 2 port (fixed there at the Chicago Leviathan fight); not yet tested in Resistance 1 play.** When the port can't work out how the camera moved on a frame (the draws disagree on the camera), Frame Generation got no camera motion (or, for up to 3 frames, the last camera) and could blend two different views into its in-between frame. When that happens while the camera moves, Frame Generation now resets for that frame instead (no in-between frame, so a short repeat instead of a doubled picture); DLSS / FSR upscaling is unchanged. `RFOM_DLSS_FG_CAM_RESET=0` restores the old behaviour, `2` also covers frames that reused the last camera.

### New options
- **Real 5.1 / 7.1 sound (issue #39).** The port always opened a stereo stream and folded the game's surround mix into it. Now it follows the Windows playback device (Sound > Configure), and every PS3 channel goes to its own speaker. New launcher option *Audio > Audio output*: Auto (default) / Stereo / 5.1 / 7.1 (`PORT_AUDIO_CHANNELS`); *Speaker layout* gets Auto. Stereo devices sound as before. *Not yet tested on 5.1 / 7.1.*
- **Ambient occlusion: CACAO, and the choice of AO (issue #45). Not yet tested in game.** *Graphics > Quality > Ambient occlusion* (and F9) is now Game / Port SSAO (extra) / CACAO / Off (`RFOM_AO_MODE`), with *Ambient occlusion quality* Low / Medium / High (`RFOM_AO_QUALITY`). Fall of Man has no AO of its own; CACAO (AMD FidelityFX CACAO) is a higher-quality alternative to the port SSAO, applied at the same point. Game is the default and unchanged; a saved "On" is Port SSAO.
- **Game files folder on another drive (Resistance3Port #47).** Launcher > *Saves & tools* > **Game files folder** (Choose folder / Use default / Open folder). The game, updates and rebuilds, Setup.exe, trophies, the HD UI pack, the shader scan and *Report a problem* all use it. Saves do not move. A `PS3_VFS_ROOT` you set yourself still wins. *Not yet tested in game.*

### Experimental (opt-in, off by default)
- **Online custom games on a community server (PR #18 by soccerzebra).** New launcher page *Multiplayer* > **Online play** = On lets the game's Online menu sign in to `r1.rfom.online` (run by a fan, not Sony) and find, create and join custom games. Your player name is random (saved in `online_id.txt`) or set in *Edit online.txt*; never your Windows name. The server sees your IP and that name. Keyboard and mouse work in matches (no more flat yellow screen), and the lobby's prompts name your keys instead of PS buttons. With Online play off nothing touches the network. *Sign-in and lobby not yet confirmed with this build; please report.*
- **Pause menu keys in matches** (launcher *Multiplayer*, default Off, `RFOM_MP_PAUSE_KEYS=1`): Esc / WASD / Enter / Backspace drive the pause menu of an online match.
- **Online through RPCN and PS Rewired (`PORT_ONLINE=1`, not in the launcher).** For players with an RPCN account: see *Online (experimental)* in the README and `online.ini.example`. Untested against the live server.

### Technical details
- **Generated code:** the PowerPC hooks changed (`tools\hooks\eboot.json`: attract-movie event wait, online multiplayer; `tools\hooks\game.json`: the `[surmix]` timing is now opt-in), so the PPU half of the generated code is rebuilt on update; the SPU half is reused.
- **Diagnostics umbrella:** `PORT_DIAG=1` (new) = `PORT_STATS_LOG=1` + `[surmix]`, `[DLSS] camera unsolved`, `[AAUV]` (`RSX_AA_UV_JITTER`), `[spu-sched] 10s`, `[mfirst]`, the first 50 `[fs] read` lines and `PS3_VERBOSE`. `PORT_FRAMETIME=1` = frame-time recorder.
- `build.bat` stops with a clear message when the lifted SPU code (`project\spu`) is missing, instead of building a game that hangs at the first level load. The frame-time report (`logs\frametimes_*.txt`, now opt-in with `PORT_FRAMETIME=1`) is now also written when you quit with the window's close button or *Quit to desktop*. Frame Generation motion-vector tools: `RSX_MV_DUMP` bursts with `tools\fgq_r1.py`, and `R1_LIVE=1` for live A/B tests (`tools\r1live.py`); all off by default.

## 0.3.1-beta

Released 2026-11-25. Fixes for the reports on 0.3.0 (missing HUD on Windows 10, split-screen, ultrawide, display scaling, Setup on PCs that needed new build tools), smoother videos and frame pacing, less frame generation ghosting, an aim assist option, optional single-player cheats and a native Linux build. Run Setup over your install (or use *Update now* in the launcher); your settings, saves and game copy are kept. This update generates the game code again (new hooks for the cheats and more SPU code compiled ahead of time), so Setup takes longer than a plain update; files that come out the same are not compiled again.

### New
- **Optional single-player cheats** (off by default): launcher > Advanced > *Cheats (single player)*, also in F9 (changes take effect at once): **God mode**, **Infinite ammo**, **Infinite grenades**, and *Cheat hotkeys* (**Ctrl+F1** toggles God mode, **Ctrl+F2** ammo and grenades; ignored while F1 / F2 are bound in `controls.txt`). Only your hero (and the other local split-screen player) is affected; enemies are untouched and scripted deaths (falls, instant kills) still happen. Once a cheat has been on, achievements are off until you restart the game. `PORT_CHEAT_GODMODE=1`, `PORT_CHEAT_AMMO=1`, `PORT_CHEAT_GRENADES=1`, `PORT_CHEAT_KEYS=1`. Thanks to bungholio and hyper2k, whose Artemis codes (issues #6 / #24) showed where to hook.
- **Groundwork for online play against community servers** (PR #29, contributed by soccerzebra): hostname overrides for the game's retired online services (`PS3_NET_HOSTS=<file>`, also answered for the DNS queries the game sends itself), the PSN sign-in ticket the game asks for before going online, and a random online name (for example an adjective and an animal) that is saved in `online_id.txt` and reused, instead of your Windows user name. All of it is only active with `PS3_NET_ONLINE=1`; offline play is unchanged. Details in `ps3recomp\docs\ONLINE.md`. Thank you, soccerzebra!
- **Build on Linux** (experimental, issue #30): Setup.exe / Setup.bat need Windows PowerShell and Visual Studio, so they don't run under Wine / Proton. `setup_linux.sh` does the same steps natively on Linux (checks and copies your game, decrypts it, generates the game code) and `build_linux.sh` builds the same Windows programs with LLVM against Microsoft's C runtime and Windows SDK fetched by xwin; the result runs under Proton. Under Proton the launcher's *Update now* is not offered (update with `git pull` and `./setup_linux.sh`). See *Building on Linux* in the README.
- **Aim assist option (issue #26).** Launcher > Controls > Mouse > **Aim assist**, or in game with F9 (F10 > *More PC Options*), where a change takes effect at once and is saved for the launcher too: *Auto* (the default) turns the game's aim assist off while you play with keyboard & mouse and keeps it with a controller; *On* keeps it always, as on PS3; *Off* turns it off for everyone. Aim assist is the slowdown when your aim passes over an enemy and the pull toward it. In split-screen, *Auto* only turns it off for the keyboard & mouse player. Enemies aim as before. `PORT_AIM_ASSIST=on` / `off` (unset = Auto; `RFOM_AIM_ASSIST` also works). *Not yet tested in play; please report how it feels.*
- **Frame generation diagnostics log (`RFOM_FG_DIAG`).** `RFOM_FG_DIAG=3` records every real frame to `logs\fgdiag.txt` until you close the game (frame times, Reflex sleep, DLSS-G / FSR 3 on or off and why, resets with their reason, frames Streamline presented, camera solved / held / unsolved, motion-vector match counts, HUD split; the upscaler frame number ties each row to the `[FSR]` / `[FG-HUD]` / `[FG-MV]` lines), plus a `[fg-diag]` summary per second in `logs\run.log`. Press **Pause** when you see ghosting or a duplicated frame and **Ctrl+Shift+F2** at bad spots: both put a numbered marker in the file. `1` = per-second lines only (the keys save the frames around the press), `2` = also saves the frames around suspect events by itself. It is written by its own thread, capped at 512 MB (`RFOM_FG_DIAG_MAX_MB`), keeps the last session as `fgdiag_prev.txt`, and is off (no cost) by default. See the README.

### Controls
- **Weapon wheel on keyboard & mouse: the mouse picks a slot** (as in the Resistance 3 port, where a video review found the Marksman slot unreachable). Holding R2 already kept WASD on the stick; the mouse did nothing in the wheel. Now, while the weapon wheel key (E, R2 (E)) is held, moving the mouse toward a slot selects it and the selection stays when the mouse stops; WASD diagonals are full length; the view holds still; and the choice is still there in the frame you let go of the key, when the game takes it. A quick tap works as before, and in vehicles the key stays the throttle (it never opens the wheel there). `PORT_KBM_WHEEL=0` restores the old behaviour. *Not yet tested in play; please report how it works.*
- **No more split-screen from a second controller or a virtual copy of your controller (reported on Resistance 3, Resistance3Port #36, #37).** With DSX (or DS4Windows / Steam Input) running next to the default SDL3 controller support, the game could see both the real DualSense and the tool's virtual controller and treat them as two players, so pressing START could start split-screen. Now:
  - **Local co-op is off by default.** The game sees one player however many controllers are connected, and player 1 is **the controller you used last** (SDL3): pick up any connected controller and press a button. To play split-screen, set launcher > Controls > **Local co-op (split-screen)** to **On** (`PAD_COOP=1`); a saved *On* from an older version loads as *Off*, so turn it on again. The *Player 2 controller* option applies when it is on. Keyboard and mouse work as before, and split-screen still pauses DLSS / FSR / XeSS (issue #25).
  - **Better duplicate detection:** a controller that connects while another one is already playing (for example DSX's virtual DualShock 4 or Xbox pad, or a Steam Input pad) only joins once it is used on its own; if it copies several different inputs of the other controller, it is ignored (two real controllers simply held the same way are not mistaken for copies, and an ignored controller comes back when you press several different buttons on it while the other controller is untouched; it is then still watched, so a copy that comes back is ignored again). `PAD_DEDUP=0` turns this off. *Not yet tested with DSX; please report how it works.*
- **DualSense / DualShock 4 not detected: clearer `logs\run.log` (Resistance3Port #6).** You don't need DSX or DS4Windows: with *Controller support* on **SDL3** (the default), the game reads them directly over USB or Bluetooth. If one isn't seen, the log now lists controllers SDL3 found but can't use, notes when HidHide (used by DSX and DS4Windows to hide the real controller) is installed or a Steam setting may be hiding controllers, and says `controllers: XInput only` when the game was built without SDL3. If your controller only works with DSX, close DSX completely (also from the tray) and check that HidHide isn't still hiding the controller.
- **A real second co-op controller is ignored?** Unplug it and plug it back in (`PAD_DEDUP=0` turns the duplicate check off).

### Fixes
- **More SPU code compiled ahead of time.** The code generator now also finds SPU routines that are only reached through code pointers (constructor lists, function-pointer tables, addresses built in registers and called indirectly) and translates them like the rest, instead of leaving them to the SPU interpreter at run time. In this game it adds a static constructor of one SPU job module (LS `0x4088` of `7d5f74861e88`); the interpreter stays the fallback for anything not found. `SPU_LIFT_CODE_PTRS=0` at code generation turns the scan off. *Takes effect when Setup generates the game code again, which this update does.*
- **Missing HUD / menu highlight on some Windows 10 PCs:** textures whose size isn't a multiple of 4 are decompressed before upload (`RSX_BC_UNALIGNED`, default on).
- **Intro and cutscene videos no longer stutter on 144 / 165 Hz screens with FreeSync / G-Sync.** Videos play at 30 fps, which a 60, 120, 240 or 360 Hz screen shows evenly, so the game keeps vsync for them there. Other refresh rates can't show 30 fps evenly (at 165 Hz each frame stayed up for 5 or 6 refreshes, which looked like a stutter), so on 144 / 165 Hz screens videos now follow the same present mode as gameplay (smooth with FreeSync / G-Sync). If your screen has no VRR and videos tear, set `RFOM_MOVIE_VSYNC=1`. `RFOM_MOVIE_VSYNC=0` never forces vsync for videos. `logs\run.log` says which mode was picked (`[D3D12] movie:`).
- **Videos on 144 / 165 Hz VRR screens: each video frame is now shown twice, evenly spaced.** At 30 fps a video runs below the range FreeSync / G-Sync can follow (usually 48 Hz and up), and when the driver doesn't make up for that itself (I saw it on a gaming laptop whose screen hangs off the integrated GPU) the screen falls back to its fixed refresh and the video still stutters. Now the game presents every video frame twice, half a frame apart (60 presents per second), which stays inside the VRR range. Gameplay and frame generation are not affected. `RFOM_MOVIE_LFC=0` turns this off, `RFOM_MOVIE_LFC=3` presents each frame 3 times. `logs\run.log` says what was picked (`[D3D12] movie LFC:`), and also notes when the NVIDIA profile forces Vertical sync On. With `PORT_STATS_LOG=1` it also logs a `[D3D12] movie LFC 5.0 s:` line every 5 s of a video (presents and their spacing, repeats presented / dropped / cut off, pictures the game skipped, slowest decode) to tell a pacing problem from a decoding one. Frame generation is held off for the whole video so video frames are never interpolated.
- **Judder / duplicated frames on screens where G-Sync / FreeSync isn't actually working (new *Display sync* option, launcher > Display > Frame rate).** The game's own pacing was even (for example 60 rendered + 60 generated frames per second), but a 165 Hz screen without working VRR shows 120 frames per second for 1 or 2 refreshes each in an uneven pattern, and 30 fps videos for 5 or 6. *Auto* (the default) now measures at start whether the screen's refresh follows the game's frames and, if not, paces for the fixed refresh rate: VSync on and a frame limit of refresh / k, so every frame stays up equally long (165 Hz: 55 fps, or 41.25 → 82.5 fps with frame generation 2x; 144 Hz: 72, or 72 → 144; 120 Hz: 60, or 60 → 120), and videos with VSync. *Fixed refresh* always does this, *VRR (G-Sync / FreeSync)* never does. `RFOM_DISPLAY_SYNC=auto|vrr|fixed`. `logs\run.log` shows `[display] VRR effective: yes/no` and the pacing chosen. *Not yet tested on a fixed-refresh screen; please report.*
- **Frame generation: steadier frame pacing (NVIDIA DLSS Frame Generation).** With frame generation on, the real (rendered) frames could arrive unevenly. NVIDIA Reflex now also keeps the frame cap: it gets the frame rate limit you set (or the one *Display sync* picked, for example 41.25 rendered frames per second at 165 Hz with frame generation 2x), counted in rendered frames before frame generation, and follows it when it changes. The game's own limiter steps back a little so the two don't fight. While *Display sync* locks frames to a fixed-refresh screen's refresh, the game's own limiter stays in charge as before. `RFOM_FPS` also accepts decimals (for example `RFOM_FPS=41.25`). `RFOM_REFLEX_LIMIT=0` restores the old behaviour; `logs\run.log` shows the limit in use (`[reflex] frame limit`). *Not yet tested; please report how it feels.*
- **Ultrawide:** the Hor+ stretch no longer drops back to 16:9 a few seconds into gameplay; it stays while 3D scenes are drawn (issue #2; `RFOM_ASPECT_SCENE_HOLD`).
- **Building with an older Windows SDK works again.** 0.3.0 only built with the Windows 11 24H2 SDK (10.0.26100); with 10.0.22621 the renderer failed to compile (`D3D12_FEATURE_DATA_D3D12_OPTIONS14`, `D3D12_DEPTH_STENCIL_DESC2` undeclared). It now builds with any Windows SDK from 10.0.20348 on and the game works the same: the driver, not the SDK, decides whether the newer D3D12 features are used. An SDK older than that now stops the build at once with a message saying which one to install, and the build log names the SDK used (`Using Windows SDK ...`).
- **Frame generation:** less ghosting on the first-person weapon during fast turns (`RSX_DLSS_VIEWMODEL_W`).
- **Frame generation: the dim unlit digits and bullet marks of the ammo / health display no longer flicker.** Those parts were missing from the generated frames, so they blinked in and out (for example a faint "8" in front of the reserve ammo or the grenade count). They now stay in the HUD layer like the rest of the HUD. `RSX_FG_HUD_DIM=0` restores the old behaviour; `RFOM_FG_HUDLOG=3` writes what the HUD split did for three frames to `logs\run.log`, which helps if some HUD part still flickers.
- **Frame generation: less doubling and smearing at doorways while panning** (door frames, crates and the room behind them). A few kinds of objects gave a camera estimate far from the real one, and on those frames the camera often went unsolved, so generated frames used an old or empty camera. Those objects are now left out of the camera estimate once they keep disagreeing. `RFOM_DLSS_CAM_REP=0` restores the old behaviour; the `[DLSS] camera/600` lines in `logs\run.log` show how many were left out.
- **Frame generation: camera estimate steadier while walking slowly, smaller logs, and an artifact report key.** Objects whose camera estimate is a little off (0.02 ndc and more) are now left out too, so the camera should no longer go unsolved for a second or more at walking speed. The repeated Streamline "RSYNC: Inconsistency detected" message (when the NVIDIA profile forces Vertical sync On, about 3 lines per frame) is now logged once and then with a repeat count. `RFOM_FG_HUDLOG` now starts once frame generation is on (`RFOM_FG_HUDLOG_AT=<frame>` picks a frame), and **Ctrl+Shift+F2** writes what the HUD split and the motion matching did for the next frames to `logs\run.log` (`[FG-HUD]` / `[FG-MV]` lines): press it where you see doubling or smearing.
- **Frame generation: less ghosting on walls, stairs and doorways while turning quickly.** Some of the game's shaders carry a value that follows the camera, so while the camera moved every wall or step piece drawn with them looked "uncertain" when matched to its previous frame, and in a fast turn the port threw those correct matches away for a camera-only estimate that missed part of the camera's movement. A match whose difference is exactly the same camera change seen on the other pieces of that shader is now kept. `RSX_DLSS_MATCH_COMMON=0` restores the old behaviour; the `[FSR] stats/600` lines in `logs\run.log` count the kept matches (*common camera term kept*).
- **Frame generation: less HUD ghosting on frames whose HUD could not be separated** (ported from the Resistance 2 port, where it was confirmed "much better"). About 2% of gameplay frames (36 of 1645 in a diagnostics capture) had no separable HUD, so the HUD was generated together with the scene and smeared while turning. Those frames now keep the HUD area of the last good frame (up to 2 s) out of frame generation, with DLSS-G also told that area does not move (`RFOM_DLSS_FG_HUD_HOLD=0` = old behaviour, `RFOM_DLSS_FG_HUD_DEBUG=1` shows the area in yellow). New diagnostics: `RFOM_DLSS_FG_STATS=1` (`[fg-stats]` lines in `logs\run.log` every 10 s), `RFOM_DLSS_FG_MV_DEBUG=1` (motion-vector tint), and an experimental `RFOM_DLSS_FG_FX_MV=1|2` for smoke / spark trails.
- **Split-screen co-op:** DLSS / FSR / XeSS and frame generation pause while two players are in play and the two views render natively (the upscaler assumes one camera per frame; issue #25, player 1's half black). `RFOM_SPLIT_AA=1` keeps them on. `RFOM_SPLIT_LOG=64` logs how the two views are cleared and drawn (`[coop]` lines; off by default).
- **Split-screen co-op: less texture shimmer** (issue #25). While the upscaler is paused for split screen, textures also drop the sharper mip selection the upscaler needs (the *Texture detail* Auto setting, -1 at Performance): at native resolution with no upscaler to smooth it, that made ground and walls shimmer. A Texture detail value you picked yourself is kept. `RFOM_SPLIT_LOD=0` restores the old behaviour.
- **Window title shows the real render size** (issue #25): it used to show only the game's own 1280x720 frame, which looked like the game ran at 720p on a 4K screen; it now reads for example `render 3840x2160 (game 1280x720)`, or `render 1920x1080 -> 3840x2160` while an upscaler runs.
- **Soft, shimmering image with Windows display scaling** (issue #25): with scaling above 100% (common on 4K TVs and laptops) Windows treated the game as an old program and stretched a smaller picture up to the screen, in windowed mode (a 3840x2160 window came out 1264x681) and in borderless fullscreen. The game now draws at the screen's real resolution, the window's inside is exactly the chosen Window size, and moving the window to a monitor with other scaling keeps that size. `RSX_DPI_AWARE=0` restores the old behaviour. A remembered window position from an older version is not reused once (the window opens centred).
- **No diagnostic logging during normal play.** Statistics and debug logging that earlier versions wrote by default (every few seconds or on every wait / file open) are now off, so a normal session only logs start-up, warnings and errors. Each one can be turned back on for a bug report: `PORT_STATS_LOG=1` (the 5 s `[fps]` / `[present]` / `[scream]` / `[GPUFETCH]` lines, `[FSR] stats`, `[DLSS] camera`, the DLSS-G / FSR 3 status repeats, the movie LFC line and the small diagnostic files in `logs\`), `PS3_VERBOSE=1` (`[WAIT]` / event lines), `PORT_AUDIO_LOG=5000` (`[audio]` 5 s lines, speaker-layout check), `PORT_AUDIO_UNDERRUN_LOG=1`, `SPURS_WSTATS=1` and `SPURS_SLOTLOG=1` (SPURS statistics), `SPU_PUTLLC_DIAG=1`, `SPU_PMTRACE=1`, `RSX_FRAMETIME_CSV=1` (`logs\frametimes.csv`), `RSX_SHMISS_SAVE=1` (extra `logs\shmiss\` copies of new shaders), `PS3_FSLOG=1` (file opens) and `RFOM_SPLIT_LOG=64` (`[coop]`). The frame-time report (`PORT_FRAMETIME`), crash and hang reports stay on.
- **Mods folder: a mod file could be overwritten by the game.** The mods folder replaced a game file for every kind of access, so when the game wrote or copied to a path a mod replaces (for example its first-start copy of data to `/dev_hdd0`) it wrote over the mod's file instead. Mods now only replace files the game reads; writes, renames and deletes always go to the game's own file.

### Setup
- **Progress for every step, with time left.** Generating the game code now reports how far it is (stage by stage, function by function), and building the game shows *game code file X of N*, the file compiling right now and how long it has been at it, and a time-left estimate from how fast this PC really is. The game code files take very different times to compile, so the estimate now uses each file's measured compile time instead of counting build steps (no more "about a minute left" with a quarter of an hour to go). A *still working, last activity N s ago* note tells a slow step apart from a stuck one. One overall bar covers the whole install. The status line while the game code is generated no longer repeats "Generating the game code", and its first time-left estimate comes from how long each generator stage took on this PC.
- **Setup remembers how long each step took** (`logs\setup_timings.json`, and a summary in `logs\setup_script.log`) and uses it for the next estimates. `logs\setup.log` and the build logs can now be opened while Setup is running.
- **Faster updates.** When an update changes the code generator, files that come out the same as before are left untouched, so the build only recompiles what really changed. This update does generate the game code again (the cheats' hooks and the SPU code scan change what it writes), but only the files that changed are compiled again.
- **Setup could stop on a fresh install that needed new build tools** (issue #16, and the same code in the Resistance 3 port, Resistance3Port #6): unpacking the portable LLVM / xwin always failed with "could not unpack ...\tar.exe", and with the portable tools the build's configure step then failed for lack of the debug C runtime. Setup also checked the first `clang-cl` on PATH while the build uses the one in `tools_cache\llvm` or `C:\Program Files\LLVM` first, so an older LLVM elsewhere on PATH kept Setup asking to update LLVM after LLVM 22+ was installed; it now checks the one the build uses. A Python that reported a missing program path is no longer taken for the Python it ran. The LLVM / clang-cl and Python fixes were contributed by pclapida (PR #28). Thank you!
- **UDF-only disc images (.iso) are accepted.** The game check only read ISO 9660 images, so an image with only a UDF file system was rejected as unreadable although Windows can open it. Setup and the installer now mount such an image, check it like a game folder and copy from it; an image you already have open is used as is and left open.
- **A build in a second copy of the port no longer closes the game** you are playing from another folder; `build.bat` only closes a game started from its own folder.
- **Add the optional upscaler / Frame Generation SDKs after installing**: run `Setup.bat -GetSdks` (or run `Setup.exe` again with the SDK box ticked). It downloads the DLSS / XeSS / Streamline / FSR 3 Frame Generation files Setup can fetch and rebuilds the port with them.

### Known issues
- **Frame Generation is still experimental:** ghosting is much reduced but not gone (for example smoke and spark trails). `RFOM_FG_DIAG=3` and the Pause / Ctrl+Shift+F2 markers help me find the rest: please attach `logs\fgdiag.txt` to your report.
- **Not yet tested in play:** aim assist, the mouse in the weapon wheel, duplicate controller detection with DSX, *Display sync* on a fixed-refresh screen and Reflex pacing the frame cap. Please report how they work for you.
- **The Linux build is new and experimental** and has not been tried on a Steam Deck yet.

### Support
If you enjoy the port and want to support it: [buymeacoffee.com/damiro98](https://buymeacoffee.com/damiro98) (always optional; nothing is unlocked by it).

## 0.3.0-beta

The first big update after the setup hotfixes: performance and stability work, tank controls, many new PC options and rendering fixes. Unzip it over your existing port folder (or use the launcher's new in-place updater next time) and run Setup; it only rebuilds what changed. Your settings, saves and game copy are kept.

### Fixes
- **No more black screen after the door in "A Fate Worse Than Death"** after a few cutscene movies: finished threads kept their slots in the game's thread table (`PS3_THREAD_SLOT_FIX`, default on).
- **Movie freeze:** the movie demuxer's input wait now gives up after 100 ms instead of waiting forever (`RFOM_MOVIE_DEMUX_WAIT_MS`, default 100; 0 = old behaviour).
- **DLSS Frame Generation now really starts on RTX 40 / 50:** the signature check rejected NVIDIA's genuine `nvngx_dlssg.dll`, so it always fell back to FSR 3 frame generation.
- **FSR ghosting:** the shadow pass and modulating blends are no longer marked in the FSR reactive mask (`RSX_FSR_RX_MUL=1` restores the old mask).
- **Split-screen co-op:** clearing player 2's half no longer wipes player 1's view (issue #13; `RSX_CLEAR_SCISSOR`, default on).
- **Mouse look in co-op** turns only player 1's camera (`RFOM_COOP_ML`, default on).
- **Freeze safety:** SPU writes into the GPU command buffer write the jump word last (`GCM_PUT_ORDER=2`); a new park-clear grace for frame-label freezes is opt-in (`GCM_PARK_CLEAR=1`, off by default until tested on this game).
- **Settings carry over:** `settings.ini` keys renamed to the names shared with the R2 / R3 ports (for example `lod_bias` → `lodbias`, `hud169` → `hudfix`) are migrated automatically.
- **Fall detection dumps** (`logs\fall_*.bin`) no longer pile up: only the newest 5 are kept (`RFOM_FALL_DUMPS=<n>`, `0` = log line only).
- **Gyro aiming stops in the background:** while the game window is in the background and *Controller in the background* does not let controllers drive the game, gyro look now stops like the buttons and sticks instead of still turning the camera.
- **Achievement pop-ups are no longer lost:** an achievement earned while the game window was not in the foreground (for example while the front end hands over to the game) is now shown once the pop-up can appear, instead of being marked as announced without ever showing.
- **Ultrawide: aiming no longer flips the picture to 16:9.** Aiming down sights and scopes set their own 16:9 camera, which switched the present back to 16:9 with the wide view squeezed in (issue #2). Every 16:9 camera (gameplay, ADS, scopes) is now widened alike and the stretch follows the window's aspect, so 21:9 / 32:9 stays put while aiming (`RFOM_ASPECT`; 16:9 art screens without a 3D camera are letterboxed).
- **The game window gets the focus on Play:** started from the launcher it no longer ran in the background until Alt+Tab (`RSX_CLAIM_FOREGROUND=0` = old).
- **HD UI pack:** no more dark squares in the HUD corners (loose images are upscaled separately; run *Update HD UI pack* once), and a graphics-card failure while making the pack no longer takes the driver down (see *HD UI pack* below).
- **Build fix for folders with spaces:** the build no longer fails on the FFmpeg headers when the port folder's path contains spaces (issues #4 / #5).
- **No more turret jitter with the mouse:** where the mouse drives the virtual stick (vehicles, turrets), slow moves now give a slow, even turn: the deflection starts past the game's deadzone, fractions of a step carry over and the poll interval uses a precise clock (issue #9).
- **Hang fix:** the GPU command position is no longer reported past a label before the label is written, which could leave the game waiting forever (`GCM_GET_AT_LABEL`, default on).
- **Crash fix:** game threads opening and closing files at the same time could corrupt the file table and crash (shared with the R2 / R3 ports).
- **Level-change freeze fixed** (a deadlock in the loading cover window), and the always-on-top F10 hint box, a likely cause of the intro flicker, is removed.
- **Smoother movies:** movie frames are presented in step with V-Sync, and the decoder logs no longer flood `run.log`.
- **HDR10 stays on** after a window resize or a switch to borderless / fullscreen.
- **Faster exit:** quitting no longer runs the full PS3 shutdown path that could hang (`PS3_EXIT_CRT=1` = old).
- **No debug keys in normal play:** the developer trace / profiler keys (F5 / F6 / F7 / F9 / F11 memory dump, pad watch, frame trace, audio recording, profiler) were armed by `run.bat` and fired together with photo mode (F7), the extras menu (F9) and the benchmark key (F11). They are now off; the renderer's debug keys (Ctrl+F10, Ctrl+F12, Home, Shift+F10) need `PORT_DEBUG_KEYS=1`.

### New features
- **In-place updater:** the launcher's *Updates* option (Advanced page): **Notify** (default) offers *Update now*, which downloads the release (SHA-256 checked), replaces the port's files, runs Setup in update mode and restarts the launcher; **Auto-install**; **Off** (`PORT_NO_UPDATE_CHECK=1`). Settings, saves, game copy and builds are kept; a failed update is rolled back (`logs\update.log`). `PORT_UPDATE_NOTIFY_ONLY=1` = the old notice. Git checkouts and RecompLauncher installs only get the notice.
- **PC SETTINGS inside the game's own menus** (*Options > PC SETTINGS*, main menu and pause menu), grouped like the R2 / R3 ports. The main menu's OPTIONS list gets an EXTRAS entry for MOVIES and CREDITS.
- **PS3-style achievement pop-ups** for the fan-made achievements (`PORT_TROPHY_STYLE=old` = the older pop-up with the grade badge; `PORT_TROPHY_TEST=1` shows a sample of each grade).
- **Save backups:** before the game overwrites a save, the previous version is copied to `saves_backup\` (newest 5 per save; `PORT_SAVE_BACKUP=0` off, `PORT_SAVE_BACKUP_KEEP=<n>`). The launcher's **Saves & tools** page lists, restores and deletes its own backups (`RFOM_SAVE_BACKUP`, keep 10) and can **import saves from RPCS3**.
- **Frame-time benchmark:** **F11** in game starts / stops a capture (`RFOM_BENCH_KEY`, default F11; `F6`, `F5`, `off`) and writes `logs\bench\RFOM_bench_<date>.txt` with average, 1 % / 0.1 % low and stutters. A frame-time report (`logs\frametimes_*.txt`) is written at exit (`PORT_FRAMETIME=0` off); **Scroll Lock** marks a bad moment.
- **HD UI pack** (experimental, off): launcher › Saves & tools › *Create HD UI pack* upscales the menu / HUD textures from your own game on your PC (`RSX_HD_UI=1`): on the graphics card in one Real-ESRGAN run (official `realesrgan-x4plus` model, downloaded once); if the graphics card fails, the upscaler is stopped at once and the rest is made on the CPU, and later runs use the CPU (delete `tools\upscaler\gpu_fault.txt` or run `HDUIPack.exe --use-gpu` to retry the GPU).
- **CPU check and a build for older CPUs:** on a CPU without AVX2 Setup builds a slower variant (`build_baseline\`, needs SSE4.1) and the launcher starts it; the game explains instead of crashing when the CPU is below what the build needs.
- **Game language:** launcher (Advanced) and F9 option *Game language* (`PS3_LANG`): Windows language (default) or English, English (UK), French, German, Italian, Spanish, Dutch, Portuguese, Russian, Polish, Finnish, Swedish, Danish, Norwegian, Japanese, Korean. Languages your copy does not have fall back to English; takes effect at the next start.
- **Local co-op options:** *Local co-op (split-screen)* on / off (`PAD_COOP`) and which controller is player 1 / player 2 (`PAD_P1_CONTROLLER`, `PAD_P2_CONTROLLER`; SDL3 controller support).
- **Compile shaders before playing** (`PORT_SHADER_PRELOAD`, default On): shaders already seen are built behind a "Compiling shaders N / M" panel at start, so they never stutter in game; Off builds them in the background while you play.
- **Crosshair colour and size** for the extra crosshair (`RFOM_CROSSHAIR_COLOR`, `RFOM_CROSSHAIR_SIZE`).
- **Frame generation backend** choice in the launcher (`RFOM_FG_BACKEND`), plus `PORT_FG=dlssg|fsr3|off` (the name shared with the R2 / R3 ports) to override it.
- **Mods load order:** `mods\order.txt`, also editable on Saves & tools › Mods.
- **Window position memory:** a windowed game reopens where you left it (`RSX_WINDOW_REMEMBER`, default on).
- **Portable build tools** (experimental): Setup can put the missing build tools into `tools_cache\` inside the port folder instead of installing them (**P** at the tools question, or `Setup.bat -PortableTools`): LLVM, CMake, Ninja, Python, MinGit and the MSVC libraries + Windows SDK (via xwin) from their official downloads, SHA-checked; nothing outside the port folder changes. Setup now explains each missing tool (what it is for, where it comes from) before asking, records what it installed in `tools_cache\installed_by_setup.txt`, and `Setup.bat -RemoveTools` removes it again (`tools\installer\buildtools.ps1`).
- **Setup help:** if the compiler runs out of memory, Setup retries with fewer files at a time and continues where it stopped; it suggests excluding the port folder from Microsoft Defender and warns about slow USB / exFAT / network drives.
- **Build repair and disk space:** `tools\build_doctor.ps1` (`-DiskUsage` how much space each part takes, `-ResetConfig` a fresh CMake configuration that keeps the compiled files, `-FirstError` the first error in the build logs). Setup uses it to retry a failed build once with a fresh configuration (also after the port folder was moved) and prints the disk usage at the end.

### Controls
- **Tank turret with the mouse** (Ice and Iron): the mouse turns the turret by a set angle like on-foot look, instead of a slowly accelerating stick (`RFOM_TANK_MOUSE`, default on; speed: `mouse_tank_sensitivity = 1.0` in `controls.txt`, 0.05–10).
- **Keyboard driving in the LU-P Lynx:** W / S accelerate / brake, A / D steer (`RFOM_KBM_DRIVE`, default on).
- **Controller options** (Controls page and F9): outer deadzone `PAD_OUTER_DEADZONE`, anti-deadzone `PAD_ANTI_DEADZONE`, trigger deadzone `PAD_TRIGGER_DEADZONE`, turn boost `PAD_TURN_BOOST` (all off by default), crouch toggle / hold `PAD_CROUCH_MODE` (experimental), vibration strength `PAD_RUMBLE` (100 %), confirm button `PS3_ENTER_BUTTON=circle`, controllers in the background `PAD_BACKGROUND_INPUT` (Auto).
- Round sticks reach the corners like the PS3's square stick (`PAD_SQUIRCLE`, default 4000; `0` = old); `PAD_MERGE=1` lets every controller drive player 1 (off, because of co-op); Nintendo button prompts (`RFOM_PAD_GLYPHS=nintendo`, auto for Switch controllers).
- The skip-cutscene hint now follows *Skip unwatched movies* (**Auto**, default): it shows when that option is on, since every story movie can then be skipped (`PORT_SKIP_HINT=1` always, `0` never). _Not yet tested in game._
- **Skip unwatched movies:** holding Space / Enter / A / Menu for 1 s now also skips a story movie you have not watched yet; the game itself only allowed skipping movies already seen. The port lifts the game's watched check only after the hold, so a quick tap never skips (`RFOM_CUTSCENE_SKIP_ALL`, default on, `0` = the game's rule; needs *Hold to skip movies*). Launcher: Interface & Notifications › *Skip unwatched movies*; also in F9 and PC SETTINGS › Controls.

### Graphics
- **Ambient occlusion** (experimental, off): `RSX_SSAO=1`, with `RSX_SSAO_STRENGTH` (0.6) and `RSX_SSAO_RADIUS` (1.0).
- **Ultrawide HUD fix** (experimental, off): Display › *Ultrawide HUD* = Centred 16:9 (`RSX_HUD_FIX=1`) keeps the HUD in a centred 16:9 area; world markers may sit off their targets.
- **Brightness** (`PS3_DISPLAY_GAMMA`, 1.0 = as the game; SDR), **HDR saturation** (`RFOM_HDR_SAT`) and **texture LOD bias** (`RSX_LOD_BIAS`, auto) are now launcher / F9 options.
- **Rendering accuracy:** strips split over draw batches, quad strips, point sprites (particles), user clip planes, alpha-to-coverage, per-texture game samplers, fog, border colours, depth bias and more fragment-program ops now follow the PS3 (each has an `RSX_*` switch, default on; see the README's *Other options*). Matching depth passes no longer z-fight (`RSX_VS_STRICT`, on except on NVIDIA).
- **Frame Generation quality** (DLSS-G and FSR 3): the generated frames now get the real camera solved from the game's matrices, with fallbacks when the candidates disagree, a short camera hold and a pause of frame generation while the camera is unsure; per-object motion vectors follow each draw's depth range and only accept close matches, which removes most ghosting at building entrances and doorways. DLSS-G menu detection is automatic, which removes 100–175 ms gameplay hitches; the DLSS feature is created off the render thread. All on by default; opt-outs for comparison: `RFOM_DLSS_FG_REAL_CAM=0`, `RFOM_DLSS_CAM_MEDOID=0`, `RFOM_DLSS_CAM_MEDOID_REL` (0.75), `RFOM_DLSS_CAM_TOL2=0`, `RFOM_DLSS_FG_CAM_HOLD=0`, `RFOM_FG_UNSURE_PAUSE=0`, `RSX_DLSS_MATCH_RATIO=0`, `RSX_DLSS_MV_ZRANGE=0`, `RFOM_DLSSG_MENU_DETECT=0|1` (force off / on), `RFOM_DLSS_PRECREATE=0`.
- **AMD FSR 1 scaling:** Display › *Scaling* = AMD FSR 1 (`RSX_FILTER=fsr`) fits the final image to the screen with FSR 1's edge-adaptive upscale.
- **Photo mode hides the HUD** (`RFOM_PHOTO_HIDEHUD`, default on).
- **Pipeline library:** compiled pipelines are kept in `shadercache\pso_library_*.bin` and reused at start (`RSX_PSO_LIBRARY`, default on).
- **DLSS quality options** (opt-in, off by default): `PORT_DLSS_RESET_HITCH_MS=<ms>` resets the DLSS history after a frame gap longer than that, `PORT_DLSS_RESET_CUTS=1` also on render / output size changes, and `PORT_UPSCALE_LOD_BIAS=auto|nv|<n>` overrides the texture LOD bias under upscaling. `PORT_DLSS_BIAS_MASK` is the name shared with the R2 / R3 ports for the DLSS effect mask (`RFOM_DLSS_REACTIVE`). The *DLSS preset* is checked against the installed DLSS version (a warning in the log if it ignores the preset), and Auto now picks CNN preset E on RTX 20 / 30, where the transformer model costs much more (`PORT_DLSS_AUTO_ARCH=0` = NVIDIA's default everywhere).

### Audio
- **Steadier audio:** the buffer grows after underruns and shrinks again when clean (`PORT_AUDIO_ADAPTIVE`, on), clock drift is steered smoothly (`PORT_AUDIO_DRIFT`, on), audio freezes cleanly while paused (`PORT_AUDIO_PAUSE_FREEZE`, on) and restarts fresh after the PC menu (`PORT_AUDIO_FOCUS_REPRIME`, on). Elastic late-block handling is experimental and off (`PORT_AUDIO_ELASTIC=1`).
- **Movie sound** no longer sounds hollow in stereo (`AUDIO_P1_DECOMB`, on).
- **Volume and mono:** master volume `PORT_VOLUME` (100) and an accessibility mono downmix `PORT_AUDIO_MONO=1`.
- **English (UK) voice fallback:** speech missing from the UK set uses the English file (`PS3_LANG_VOICE_FALLBACK`, default 1; 2 = any language, 0 = off).

### Performance
- **PGO-optimised builds:** `build.bat` uses the committed profile `pgo\r1.profdata` automatically (2–5 % on CPU-bound frames); `set PORT_PGO=0` or `build.bat nopgo` builds without it. The profile needs **LLVM 22 or newer**; Setup updates an older LLVM.
- **No-spin waits:** SPU and system waits park after a short spin instead of holding a core (`PORT_NOSPIN`, default on; `auto` = only below 12 threads; `0` = old), and idle SPUs skip their spin (`PORT_SPU_SPIN_ADAPT`, default 25 %).
- **Thread placement:** on Intel hybrid CPUs the game's busy threads prefer P-cores (`PORT_CPU_PCORES`, on); optional raised priority (`PORT_THREAD_PRIORITY`, off) and large pages (`PORT_LARGE_PAGES`, off).
- **Render thread offload:** texture views and change checks run on worker threads (`RSX_MT_RECORD`, on); multi-threaded draw recording is opt-in (`RSX_MT_REPLAY=1`).
- **No Windows power throttling:** the game's threads opt out of EcoQoS, so a game window that started in the background is not slowed down (`PORT_ECOQOS=1` keeps the Windows default).
- **Native texture decode** with SSE2, self-checked against the reference (`RSX_NATIVE_TEXDEC`, on).
- **Few-core CPUs:** the port detects a CPU with few physical cores (`PORT_CORE_SCARCE`, below `PORT_CORE_SCARCE_BELOW` = 6 cores; `0` / `1` forces it, logged as `[cores]`) and then lets the GPU frame clock sleep until new work arrives instead of spinning on a core (`RSX_SPINWAIT=4`; other CPUs keep `1`, the old busy spin).

### Settings
- **Overlay:** the F3 overlay draws a frame-time graph (`PORT_OVERLAY_GRAPH`, on); the PC menus, overlay and notifications scale with the window (`PORT_UI_SCALE`, auto; 75–200 %).
- **Latency reduction:** `PORT_LATENCY` = `off`, `reflex`, `antilag` (**AMD Anti-Lag 2**, Radeon RX 5000 or newer) or `auto`; unset = the NVIDIA Reflex setting alone decides.
- **Frame rate:** new 40 and 45 fps caps; *Unlimited* is now capped at 240 fps.
- New launcher options: *Achievement pop-up style* and *Show cutscene skip hint* (Interface & Notifications), *Brightness*, *HDR saturation* and *Ultrawide HUD* (Display); Brightness and HDR saturation are also in F9.

### Known issues
- **Frame Generation is still experimental:** some artifacts remain, such as some ghosting at building entrances and artifacts on the weapon (carbine) and at a distance. They will be fixed in an upcoming hot patch.
- **Missing HUD / health bar and menu selection highlight:** some players see no HUD or health bar, or no highlight on the selected menu entry. Under investigation; please send `logs\run.log` if it happens to you.

### Support
If you enjoy the port and want to support it: [buymeacoffee.com/damiro98](https://buymeacoffee.com/damiro98) (always optional; nothing is unlocked by it).

## 0.2.3-beta (last hotfix)

This is the last hotfix. The next updates focus on performance and stability, based on your reports and playthroughs.

- **Setup works without winget.** If winget is missing or fails on your PC, Setup downloads the official installers of the build tools (Visual Studio Build Tools, CMake, Ninja, LLVM, Python, Git) directly, checks them (SHA-256 or Microsoft's signature) and installs them.
- **RecompLauncher support.** The port can be installed, set up and played from [RecompLauncher](https://recomplauncher.com): it checks your copy, builds the game, shows the main video settings on the game's page and starts the game. Your own Launcher.exe and Setup.exe work as before.
- The release zip is now named `...-Windows-x64.zip`.
## 0.2.2-beta (setup hotfix)

- **Fixed: the build could fail right away with a Ninja/CMake error** ("build.ninja still dirty", or CMake re-running in a loop) for players in time zones behind ours. Files from the download could be dated a few hours in the future. The installer and zips now carry correct times, and the build resets any future-dated files before it starts.

If your setup failed this way: install 0.2.2 (or wait a few hours) and run Setup again; it continues where it stopped.

## 0.2.1-beta (setup hotfix)

Setup fixes only; the game itself is unchanged from 0.2.0.

- **Progress bar during the build.** The bar no longer stops partway while the game compiles; it follows the build to the end, so Setup no longer looks stuck.
- **Setup shows that it is working:** a running clock for each step, the game copy in GB with the time left, the time left while the game compiles, and a "still working" note during steps that print nothing for several minutes.
- **The installer now opens the Setup window** (with the progress bar and your game already filled in) instead of a console window. Click **Install** there.
- **Much smaller build logs.** The compiler was set to report every possible warning, which made `logs\build.log` grow past 150 MB and slowed the build. It now reports normal warnings only.
- **Low-RAM PCs:** if the build runs out of memory, Setup again detects it and retries with fewer parallel jobs (this check could not read the build log in 0.2.0).

If 0.2.0 is already built and runs, you don't need this update.

## What's in this beta

### Launcher
- `Launcher.exe` holds all settings and key bindings. On first start it detects your hardware and picks a preset (Steam Deck included).
- Pages: Display, Graphics, Upscaling & Frame Gen, Performance, Controls, Audio, Interface & Notifications, Advanced, Achievements, Saves & tools and About (`Launcher.exe --page N` opens one, 0–10).
- **Updates:** the launcher checks the Releases page at start and can update in place (*Update now*: downloads, replaces the port's files, runs Setup and restarts; a failed update is rolled back). The *Updates* option on the Advanced page picks Notify (default), Auto-install or Off (`PORT_NO_UPDATE_CHECK=1`).

### Graphics
- **Anti-aliasing / upscaling:** Off, AMD FSR 3.1, NVIDIA DLSS / DLAA (GeForce RTX) or **Intel XeSS 2** (any DirectX 12 GPU with SM 6.4). Set it in the launcher or with `RSX_FSR` = 0 / 1 / 2 / 3.
  - If DLSS or XeSS can't start, the port uses FSR 3.1.
- **Render resolution** up to 4K (`RSX_RES_SCALE`). **Upscaling** renders the 3D scene at 67, 58, 50 or 33 % (`RSX_UPSCALE`); menus and HUD stay at full resolution.
  - A *1080p → DLSS 4K* preset is available for RTX 40 / 50 cards.
- **Frame rate targets** from 30 to 165, *Just under refresh* (`RFOM_FPS=vrr`, for G-Sync / FreeSync), *Original* pacing, or unlimited (`RFOM_FPS`).
  - Game speed is correct at every frame rate.
  - Frame-timed scripted sequences step at 30 fps automatically.
- **VRR / tearing control:** *Tearing: Allowed* presents immediately with VSync off, so G-Sync / FreeSync follows the frame rate. *Never* drops late frames instead of tearing (`RFOM_PRESENT=tearing|notear`).
- **NVIDIA Reflex** Low Latency through NVAPI (`RFOM_REFLEX`, off by default).
- **Ultrawide (Hor+)**, a field-of-view slider, and a performance overlay (F3).
- **Shader precompile:** with *Compile shaders before playing* (default on, `RSX_PSO_PRECOMPILE`), pipelines are built before the first frame, with a **Compiling shaders N / M** progress panel. Before Play, the launcher also runs the offline shader scan (shaderscan) with its own progress panel. Shader caches are built on your PC, never shipped. They are backed up to `%LOCALAPPDATA%\ResistancePort\shadercache`.
- **Frame Generation** (experimental, off by default; Upscaling & Frame Gen page, `RFOM_DLSS_FG`, `RFOM_FG_BACKEND=auto|dlss|fsr3`): NVIDIA DLSS Frame Generation 2x on an RTX 40, up to 4x on an RTX 50; other GPUs get AMD FSR 3 2x. The HUD is kept out of the generated frames.

### Controls
- **Mouse and keyboard:** raw mouse look (it doesn't emulate the stick), rebindable keys, and separate melee and interact keys.
- **Controllers through SDL3** (the default; `PAD_BACKEND=xinput` is still available): Xbox, PlayStation and others, swappable at any time.
- **Button prompts follow your device:** your keyboard keys, or Xbox buttons that match the real mapping. *Controller button prompts* (`RFOM_PAD_GLYPHS`) forces Xbox or PlayStation.

### PC features
- **PC menu:** F10, or hold Esc / Start. It has Resume, PC Options and **Quit to Desktop** with confirmation.
- **PC wording:** console references in the game's text use PC terms (`RFOM_PC_TEXT=0` turns this off).
- **Screenshots** (F12), a **mods folder**, fan-made **achievements** (launcher Achievements page, read from your save; the game itself has no trophies), and saves in *Saved Games*.
- **Notifications in the game image:** the F3 overlay and toasts are drawn into the game's own frame, so they also show in exclusive fullscreen (`RFOM_OSD=window` uses separate windows). The movie-skip hint can be turned off (*Show cutscene skip hint*, `PORT_SKIP_HINT=0`).
- A black cover window hides the switch between programs during level loads (`RFoMSwitch.exe`).
- **Crash and hang reports** are written automatically, and the game process can restart after a freeze.

### Audio
- **Time stretching** (SoundTouch, `PORT_AUDIO_STRETCH=1`, default on): frame drops make the audio slightly slower instead of crackling.

## What you download

- **`ResistancePort-Setup-<version>.exe`** (for this release `ResistancePort-Setup-0.3.2-beta.exe`) from the GitHub release: an installer with the port's source, scripts and `Launcher.exe`. It installs the files and starts Setup.
- or the **source code** (the release's source archive or `git clone` at the tag), then `Setup.bat`.

Either way, the game itself is built on your PC; no built game program is published. Setup downloads the build tools, FFmpeg (an LGPL build), shaderscan, AMD's FSR 3 frame generation DLL and, if you accept their licences, the NVIDIA DLSS, Streamline (DLSS Frame Generation) and Intel XeSS SDKs.

## Requirements

- Windows 10 or 11, 64-bit. Steam Deck / Proton works; see the README.
- A CPU with **AVX2** (Intel 4th generation or newer, or any AMD Ryzen) for the normal build. Older CPUs with SSE4.1 (Intel Core 2 Penryn or newer, AMD Bulldozer / FX or newer) get a slower variant without AVX2, which Setup builds by itself.
- A **DirectX 12** GPU. It has been tested on AMD RX 6700 XT and NVIDIA RTX 5070. DLSS needs a GeForce RTX card.
- The Microsoft Visual C++ 2015–2022 x64 runtime, version 14.40 or newer.
- Your own copy of **Resistance: Fall of Man, US disc (BCUS98107), version 1.00**. Other regions and patched versions are not supported yet.
- About **25 GB** of free disk space: about 16.5 GB for the game files plus the build.
- For setup: an internet connection and the free build tools (LLVM 22 or newer). Setup installs or updates the tools itself.

## Install

1. **Dump your own game.** Use a decrypted ISO, or a game folder containing `PS3_GAME` (the same kind of folder RPCS3 uses), from your own disc.
2. **Install the port.** Either:
   - run `ResistancePort-Setup-<version>.exe`, which installs to `C:\Games\ResistancePort` by default and asks for your game; or
   - download the source code (the release's source archive, or `git clone` at the release tag) into a folder without spaces and double-click `Setup.bat`.
3. **Setup** finds or asks for your game (`Setup.bat -Game "<ISO or folder>"` skips the question). It then:
   - checks the copy and copies it to `gamefiles\PS3_GAME`;
   - decrypts the game's two program files to `game\`, using the public application keys from RPCS3's source;
   - downloads FFmpeg, shaderscan and the optional SDKs (DLSS, Streamline, XeSS; each after you accept its licence);
   - generates and builds the game-specific code. This takes 20–60 minutes the first time.
4. **Play** with `Launcher.exe`.

**How the port finds your game:**
- `run.bat`, which the launcher starts, runs `build\ResistanceFoM.exe` with `game\EBOOT.elf` and points the game's virtual disc at `gamefiles` (`PS3_VFS_ROOT`).
- Settings and controls are also kept in `%USERPROFILE%\Saved Games\Resistance Fall of Man\config`. An empty `portable.txt` turns that off.

**First launch:** before the game starts, the launcher prepares the shader cache from your game files (a progress panel; about 1–2 minutes the first time; **Skip for now** or Esc skips it). The game then compiles its known shaders behind a **Compiling shaders N / M** panel and shows its one-time "initial setup" screens. Later starts are fast.

## Known issues

- **Performance can dip in heavy areas.** The cost comes from the game's chained SPU jobs; a faster CPU helps the most.
- **Frame Generation is experimental and barely tested** (DLSS on RTX 40/50, FSR 3 elsewhere; off by default). Expect ghosting and HUD or menu artifacts; reports are welcome.
- **Audio crackle** can happen when the frame rate drops in intensive spots, for example coming out of the crater at the start of York.
- **Falling through the floor** has been seen a few times. A safeguard tries to put you back in place.
- **Shadows can flicker** occasionally.
- **FSR can ghost** on some things, such as the sky, at rare moments. DLSS doesn't show this.
- **Out-of-bounds water** (off the dock in Grimsby, under the bridge in Manchester) shows some stray triangles.
- **The Rollers level** hasn't been reached on this port yet. On RPCS3 it needed OpenGL.
- **Ultrawide HUD:** by default the HUD is stretched with the view. The experimental *Ultrawide HUD* option (`RSX_HUD_FIX=1`) keeps it in a centred 16:9 area; world markers may then sit off their targets and full-screen fades may cover only the centre.
- **Two programs:** the main menu and the levels are separate programs on the disc, so the game process restarts between them; a black cover window hides the switch.
- Everything else, including levels after Cheddar Gorge: **TBD after testing**.
- What's planned next: [ROADMAP.md](ROADMAP.md).

## Reporting problems

Please read **[REPORTING.md](REPORTING.md)**. Include how to reproduce the problem, the session's log from `logs\` and any crash or hang report.

## Licences

The port is MIT-licensed (see `LICENSE`). This port uses NVIDIA DLSS. Third-party components are listed in `THIRD_PARTY_NOTICES.md`. *Resistance: Fall of Man* is a trademark of its owners. This project is not affiliated with or endorsed by them.
