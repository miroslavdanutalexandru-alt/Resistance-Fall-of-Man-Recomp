# Resistance: Fall of Man PC port: community beta release notes

**Version:** 0.3.0-beta (GitHub pre-release). **Release date:** 2026-11-20.

A native PC port of *Resistance: Fall of Man* (PS3) built by static recompilation: the game's PowerPC and SPU code is translated to C/C++ and compiled for Windows. Nothing is emulated. It renders through the port's own **Direct3D 12** renderer.

> **No game files and no prebuilt game are included.** This release is the source code plus Setup. You need your own legally dumped copy of the game (see *Install*); Setup builds the game on your PC from that copy.

This is a **beta**. The campaign has been played up to Cheddar Gorge. Later levels haven't been tested. Please report what you find (see *Reporting problems*).

## 0.3.1-beta (unreleased)

### New
- **Aim assist option (issue #26).** Launcher > Controls > Mouse > **Aim assist**, or in game with F9 (F10 > *More PC Options*), where a change takes effect at once and is saved for the launcher too: *Auto* (the default) turns the game's aim assist off while you play with keyboard & mouse and keeps it with a controller; *On* keeps it always, as on PS3; *Off* turns it off for everyone. Aim assist is the slowdown when your aim passes over an enemy and the pull toward it. In split-screen, *Auto* only turns it off for the keyboard & mouse player. Enemies aim as before. `PORT_AIM_ASSIST=on` / `off` (unset = Auto; `RFOM_AIM_ASSIST` also works). *Not yet tested in play; please report how it feels.*
- **Frame generation diagnostics log (`RFOM_FG_DIAG`).** `RFOM_FG_DIAG=3` records every real frame to `logs\fgdiag.txt` until you close the game (frame times, Reflex sleep, DLSS-G / FSR 3 on or off and why, resets with their reason, frames Streamline presented, camera solved / held / unsolved, motion-vector match counts, HUD split; the upscaler frame number ties each row to the `[FSR]` / `[FG-HUD]` / `[FG-MV]` lines), plus a `[fg-diag]` summary per second in `logs\run.log`. Press **Pause** when you see ghosting or a duplicated frame and **Ctrl+Shift+F2** at bad spots: both put a numbered marker in the file. `1` = per-second lines only (the keys save the frames around the press), `2` = also saves the frames around suspect events by itself. It is written by its own thread, capped at 512 MB (`RFOM_FG_DIAG_MAX_MB`), keeps the last session as `fgdiag_prev.txt`, and is off (no cost) by default. See the README.

### Controls
- **Weapon wheel on keyboard & mouse: the mouse picks a slot** (as in the Resistance 3 port, where a video review found the Marksman slot unreachable). Holding R2 already kept WASD on the stick; the mouse did nothing in the wheel. Now, while the weapon wheel key (E, R2 (E)) is held, moving the mouse toward a slot selects it and the selection stays when the mouse stops; WASD diagonals are full length; the view holds still; and the choice is still there in the frame you let go of the key, when the game takes it. A quick tap works as before. `PORT_KBM_WHEEL=0` restores the old behaviour. *Not yet tested in play; please report how it works.*
- **No more split-screen from a second controller or a virtual copy of your controller (reported on Resistance 3, Resistance3Port #36, #37).** With DSX (or DS4Windows / Steam Input) running next to the default SDL3 controller support, the game could see both the real DualSense and the tool's virtual controller and treat them as two players, so pressing START could start split-screen. Now:
  - **Local co-op is off by default.** The game sees one player however many controllers are connected, and player 1 is **the controller you used last** (SDL3): pick up any connected controller and press a button. To play split-screen, set launcher > Controls > **Local co-op (split-screen)** to **On** (`PAD_COOP=1`); a saved *On* from an older version loads as *Off*, so turn it on again. The *Player 2 controller* option applies when it is on. Keyboard and mouse work as before, and split-screen still pauses DLSS / FSR / XeSS (issue #25).
  - **Better duplicate detection:** a controller that connects while another one is already playing (for example DSX's virtual DualShock 4 or Xbox pad, or a Steam Input pad) only joins once it is used on its own; if it copies several different inputs of the other controller, it is ignored (two real controllers simply held the same way are not mistaken for copies, and an ignored controller comes back when you press several different buttons on it while the other controller is untouched; it is then still watched, so a copy that comes back is ignored again). `PAD_DEDUP=0` turns this off. *Not yet tested with DSX; please report how it works.*
- **DualSense / DualShock 4 not detected: clearer `logs\run.log` (Resistance3Port #6).** You don't need DSX or DS4Windows: with *Controller support* on **SDL3** (the default), the game reads them directly over USB or Bluetooth. If one isn't seen, the log now lists controllers SDL3 found but can't use, notes when HidHide (used by DSX and DS4Windows to hide the real controller) is installed or a Steam setting may be hiding controllers, and says `controllers: XInput only` when the game was built without SDL3. If your controller only works with DSX, close DSX completely (also from the tray) and check that HidHide isn't still hiding the controller.
- **A real second co-op controller is ignored?** Unplug it and plug it back in (`PAD_DEDUP=0` turns the duplicate check off).

### Fixes
- **Missing HUD / menu highlight on some Windows 10 PCs:** textures whose size isn't a multiple of 4 are decompressed before upload (`RSX_BC_UNALIGNED`, default on).
- **Intro and cutscene videos no longer stutter on 144 / 165 Hz screens with FreeSync / G-Sync.** Videos play at 30 fps, which a 60, 120, 240 or 360 Hz screen shows evenly, so the game keeps vsync for them there. Other refresh rates can't show 30 fps evenly (at 165 Hz each frame stayed up for 5 or 6 refreshes, which looked like a stutter), so on 144 / 165 Hz screens videos now follow the same present mode as gameplay (smooth with FreeSync / G-Sync). If your screen has no VRR and videos tear, set `RFOM_MOVIE_VSYNC=1`. `RFOM_MOVIE_VSYNC=0` never forces vsync for videos. `logs\run.log` says which mode was picked (`[D3D12] movie:`).
- **Videos on 144 / 165 Hz VRR screens: each video frame is now shown twice, evenly spaced.** At 30 fps a video runs below the range FreeSync / G-Sync can follow (usually 48 Hz and up), and when the driver doesn't make up for that itself (I saw it on a gaming laptop whose screen hangs off the integrated GPU) the screen falls back to its fixed refresh and the video still stutters. Now the game presents every video frame twice, half a frame apart (60 presents per second), which stays inside the VRR range. Gameplay and frame generation are not affected. `RFOM_MOVIE_LFC=0` turns this off, `RFOM_MOVIE_LFC=3` presents each frame 3 times. `logs\run.log` says what was picked (`[D3D12] movie LFC:`), and also notes when the NVIDIA profile forces Vertical sync On. Every 5 s of a video it now also logs a `[D3D12] movie LFC 5.0 s:` line (presents and their spacing, repeats presented / dropped / cut off, pictures the game skipped, slowest decode) to tell a pacing problem from a decoding one, and frame generation is held off for the whole video so video frames are never interpolated.
- **Judder / duplicated frames on screens where G-Sync / FreeSync isn't actually working (new *Display sync* option, launcher > Display > Frame rate).** The game's own pacing was even (for example 60 rendered + 60 generated frames per second), but a 165 Hz screen without working VRR shows 120 frames per second for 1 or 2 refreshes each in an uneven pattern, and 30 fps videos for 5 or 6. *Auto* (the default) now measures at start whether the screen's refresh follows the game's frames and, if not, paces for the fixed refresh rate: VSync on and a frame limit of refresh / k, so every frame stays up equally long (165 Hz: 55 fps, or 41.25 → 82.5 fps with frame generation 2x; 144 Hz: 72, or 72 → 144; 120 Hz: 60, or 60 → 120), and videos with VSync. *Fixed refresh* always does this, *VRR (G-Sync / FreeSync)* never does. `RFOM_DISPLAY_SYNC=auto|vrr|fixed`. `logs\run.log` shows `[display] VRR effective: yes/no` and the pacing chosen. *Not yet tested on a fixed-refresh screen; please report.*
- **Frame generation: steadier frame pacing (NVIDIA DLSS Frame Generation).** With frame generation on, the real (rendered) frames could arrive unevenly. NVIDIA Reflex now also keeps the frame cap: it gets the frame rate limit you set (or the one *Display sync* picked, for example 41.25 rendered frames per second at 165 Hz with frame generation 2x), counted in rendered frames before frame generation, and follows it when it changes. The game's own limiter steps back a little so the two don't fight. While *Display sync* locks frames to a fixed-refresh screen's refresh, the game's own limiter stays in charge as before. `RFOM_FPS` also accepts decimals (for example `RFOM_FPS=41.25`). `RFOM_REFLEX_LIMIT=0` restores the old behaviour; `logs\run.log` shows the limit in use (`[reflex] frame limit`). *Not yet tested; please report how it feels.*
- **Ultrawide:** the Hor+ stretch no longer drops back to 16:9 a few seconds into gameplay; it stays while 3D scenes are drawn (issue #2; `RFOM_ASPECT_SCENE_HOLD`).
- **Frame generation:** less ghosting on the first-person weapon during fast turns (`RSX_DLSS_VIEWMODEL_W`).
- **Frame generation: the dim unlit digits and bullet marks of the ammo / health display no longer flicker.** Those parts were missing from the generated frames, so they blinked in and out (for example a faint "8" in front of the reserve ammo or the grenade count). They now stay in the HUD layer like the rest of the HUD. `RSX_FG_HUD_DIM=0` restores the old behaviour; `RFOM_FG_HUDLOG=3` writes what the HUD split did for three frames to `logs\run.log`, which helps if some HUD part still flickers.
- **Frame generation: less doubling and smearing at doorways while panning** (door frames, crates and the room behind them). A few kinds of objects gave a camera estimate far from the real one, and on those frames the camera often went unsolved, so generated frames used an old or empty camera. Those objects are now left out of the camera estimate once they keep disagreeing. `RFOM_DLSS_CAM_REP=0` restores the old behaviour; the `[DLSS] camera/600` lines in `logs\run.log` show how many were left out.
- **Frame generation: camera estimate steadier while walking slowly, smaller logs, and an artifact report key.** Objects whose camera estimate is a little off (0.02 ndc and more) are now left out too, so the camera should no longer go unsolved for a second or more at walking speed. The repeated Streamline "RSYNC: Inconsistency detected" message (when the NVIDIA profile forces Vertical sync On, about 3 lines per frame) is now logged once and then with a repeat count. `RFOM_FG_HUDLOG` now starts once frame generation is on (`RFOM_FG_HUDLOG_AT=<frame>` picks a frame), and **Ctrl+Shift+F2** writes what the HUD split and the motion matching did for the next frames to `logs\run.log` (`[FG-HUD]` / `[FG-MV]` lines): press it where you see doubling or smearing.
- **Frame generation: less ghosting on walls, stairs and doorways while turning quickly.** Some of the game's shaders carry a value that follows the camera, so while the camera moved every wall or step piece drawn with them looked "uncertain" when matched to its previous frame, and in a fast turn the port threw those correct matches away for a camera-only estimate that missed part of the camera's movement. A match whose difference is exactly the same camera change seen on the other pieces of that shader is now kept. `RSX_DLSS_MATCH_COMMON=0` restores the old behaviour; the `[FSR] stats/600` lines in `logs\run.log` count the kept matches (*common camera term kept*).
- **Portable build tools:** unpacking xwin / LLVM fixed (issue #16).
- **Split-screen co-op:** DLSS / FSR / XeSS and frame generation pause while two players are in play and the two views render natively (the upscaler assumes one camera per frame; issue #25, player 1's half black). `RFOM_SPLIT_AA=1` keeps them on. The log now records how the two views are cleared and drawn (`[coop]` lines, `RFOM_SPLIT_LOG`).
- **Split-screen co-op: less texture shimmer** (issue #25). While the upscaler is paused for split screen, textures also drop the sharper mip selection the upscaler needs (the *Texture detail* Auto setting, -1 at Performance): at native resolution with no upscaler to smooth it, that made ground and walls shimmer. A Texture detail value you picked yourself is kept. `RFOM_SPLIT_LOD=0` restores the old behaviour.
- **Window title shows the real render size** (issue #25): it used to show only the game's own 1280x720 frame, which looked like the game ran at 720p on a 4K screen; it now reads for example `render 3840x2160 (game 1280x720)`, or `render 1920x1080 -> 3840x2160` while an upscaler runs.
- **Soft, shimmering image with Windows display scaling** (issue #25): with scaling above 100% (common on 4K TVs and laptops) Windows treated the game as an old program and stretched a smaller picture up to the screen, in windowed mode (a 3840x2160 window came out 1264x681) and in borderless fullscreen. The game now draws at the screen's real resolution, the window's inside is exactly the chosen Window size, and moving the window to a monitor with other scaling keeps that size. `RSX_DPI_AWARE=0` restores the old behaviour. A remembered window position from an older version is not reused once (the window opens centred).
- **Mods folder: a mod file could be overwritten by the game.** The mods folder replaced a game file for every kind of access, so when the game wrote or copied to a path a mod replaces (for example its first-start copy of data to `/dev_hdd0`) it wrote over the mod's file instead. Mods now only replace files the game reads; writes, renames and deletes always go to the game's own file.

### Setup
- **Progress for every step, with time left.** Generating the game code now reports how far it is (stage by stage, function by function), and building the game shows *game code file X of N*, the file compiling right now and how long it has been at it, and a time-left estimate from how fast this PC really is. The game code files take very different times to compile, so the estimate now uses each file's measured compile time instead of counting build steps (no more "about a minute left" with a quarter of an hour to go). A *still working, last activity N s ago* note tells a slow step apart from a stuck one. One overall bar covers the whole install. The status line while the game code is generated no longer repeats "Generating the game code", and its first time-left estimate comes from how long each generator stage took on this PC.
- **Setup remembers how long each step took** (`logs\setup_timings.json`, and a summary in `logs\setup_script.log`) and uses it for the next estimates. `logs\setup.log` and the build logs can now be opened while Setup is running.
- **Faster updates.** When an update changes the code generator, files that come out the same as before are left untouched, so the build only recompiles what really changed. This version's changes to the code generator only add progress reporting, so an install whose game code is up to date with the previous version keeps it: no generating again after this update.
- **Add the optional upscaler / Frame Generation SDKs after installing**: run `Setup.bat -GetSdks` (or run `Setup.exe` again with the SDK box ticked). It downloads the DLSS / XeSS / Streamline / FSR 3 Frame Generation files Setup can fetch and rebuilds the port with them.

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

- **`ResistancePort-Setup-<version>.exe`** (for this release `ResistancePort-Setup-0.3.0-beta.exe`) from the GitHub release: an installer with the port's source, scripts and `Launcher.exe`. It installs the files and starts Setup.
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
