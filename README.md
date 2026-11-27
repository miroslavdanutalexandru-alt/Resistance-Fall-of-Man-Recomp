# Resistance: Fall of Man: native PC port

A static recompilation of the PS3 game *Resistance: Fall of Man* into a native Windows program. Nothing is emulated: the game's PowerPC and SPU code is translated to C/C++ and compiled for your PC. It renders through its own Direct3D 12 renderer, with NVIDIA DLSS / DLAA and AMD FSR 3.1.

> **No game files and no translated game code are distributed, in this repository or in the installer.** You need your own copy of the game. Setup decrypts its program on your PC, translates it into C/C++ and builds the game there; the generated code and the built game never leave your PC, and no prebuilt game program is released. The repository holds the port's own code plus the small references it needs to find its place in the game's program: function addresses, patch values and short context lines from the generated code (`tools/hooks/`).

## Highlights

- **60 fps and higher** (30 to 165, *Just under refresh* for G-Sync / FreeSync, or unlimited). Game speed is correct at every frame rate.
- **NVIDIA DLSS / DLAA, AMD FSR 3.1 and Intel XeSS**, NVIDIA Reflex, render resolutions up to 4K, and a *1080p → DLSS 4K* preset for RTX 40 / 50 cards.
- **Native PC controls:** raw mouse look (not stick emulation), rebindable keys, and separate melee and interact keys. Xbox and PlayStation controllers work and can be swapped at any time.
- **Button prompts follow your device:** your keyboard keys, or Xbox buttons when an Xbox controller is in use.
- **In-game PC menu** (F10, or hold Esc / Start): Resume, PC Options, and **Quit to Desktop** with a confirmation. No Alt+F4 needed.
- **PC SETTINGS in the game's own menus:** *Options > PC SETTINGS* (main menu and pause menu) shows the PC options grouped into categories — PC DISPLAY, PC GRAPHICS, PC EFFECTS, PC CONTROLS & HUD, PC AUDIO & LANGUAGE (the same as the R2 / R3 ports) — as pages of the game's menu.
- **Ultrawide (Hor+)**, field of view slider, performance overlay (F3), screenshots (F12), mods folder, and saves in *Saved Games*.
- **Launcher** that detects your hardware and picks recommended settings.
- **Performance work:** the renderer was profiled down to about 5 µs of CPU per draw (see *Performance*).

## What you need

- A PC running Windows 10 or 11 (64-bit).
- A CPU with AVX2 (Intel 4th generation or newer, or any AMD Ryzen). Older CPUs get the slower variant without AVX2 (`build_baseline\`, built by setup); its minimum is SSE4.1 (Intel Core 2 Penryn / 45 nm or newer, AMD Bulldozer / FX or newer). On a CPU below that, the game says the processor is below the minimum (SSE4.1) and exits; setup skips the variant without AVX2 only when Windows reports SSE4.1 as missing (older Windows 10 versions can't tell, and then it is built as usual).
- A DirectX 12 GPU. It has been tested on AMD (RX 6700 XT) and NVIDIA (RTX 5070) cards. DLSS needs a GeForce RTX card; every other GPU uses FSR 3.1 or Intel XeSS (XeSS runs on any DirectX 12 GPU with Shader Model 6.4).
- Your own dump of **Resistance: Fall of Man, US disc (BCUS98107), version 1.00**: the folder that contains `PS3_GAME`, the same kind of folder RPCS3 uses. Other regions and patched versions are not supported yet.
- About 25 GB of free disk space: 16 GB for the game files, the rest for the build tools and the build.
- An internet connection for setup.

## Install

**Quick install (recommended):**

1. Download the release zip from the Releases page and unzip all of it to a folder with no spaces in the path, for example `C:\Games\ResistancePort`.
2. Run **`Setup.exe`** in that folder. (It is not code-signed: if Windows says *"Windows protected your PC"*, click **More info → Run anyway**.)
3. Pick your game: **ISO...** or **Folder...**. Setup checks the copy at once and says what to do if it can't be used (see *If your game copy is not accepted* below).
4. Click **Install** and wait. Setup lists the build tools that are missing, says what each one is for and where it comes from, and asks before installing them (system-wide with one administrator prompt, or, experimental, portable inside the port folder; [details](docs/INSTALL.md#build-tools)). Then it copies your game files, builds the game with as many parallel jobs as your RAM allows, adds Start-menu and desktop shortcuts and starts the launcher. It shows the progress and the time left. If it stops (error, sleep, power cut), run `Setup.exe` again and click **Resume**.

The release contains no game code: the game is always built from your own copy. Requirements, disk space, build times, uninstalling and troubleshooting (SmartScreen, antivirus, low RAM, no winget): [docs/INSTALL.md](docs/INSTALL.md).

### Manual install (advanced)

**With the Inno installer** (if a `-Setup-<version>.exe` is on the Releases page): download `ResistancePort-Setup-<version>.exe` from the Releases page and run it. It installs the port (default `C:\Games\ResistancePort`), adds Start-menu and desktop shortcuts, and asks for your game. It lists the copies it finds on the PC (common folders and RPCS3's game list); you can also type or paste a path, or use the **ISO...** / **Folder...** buttons. It checks the copy before going on and says what is wrong and what to do if it can't be used (see *If your game copy is not accepted* below). At the end it opens **Setup**, the window with the progress bar, with your game already filled in: click **Install** there (step 2 below). The installer contains only the port; the game files always come from your own copy. Use a decrypted ISO or a game folder (the same kind RPCS3 uses). Tick **Portable mode** to keep settings and saves only in the install folder.

**By hand:**

1. Download this repository (**Code → Download ZIP**) and unzip it to a path without spaces, for example `C:\Games\ResistancePort`.
2. Double-click **`Setup.bat`**. It:
   - lists the free build tools that are missing (Visual Studio 2022 Build Tools, LLVM, CMake, Ninja, Python), what each is for and where it comes from, and asks first: **S** installs them system-wide with `winget`, **P** puts them only into `tools_cache\` in the port folder (experimental), **N** stops. `Setup.bat -RemoveTools` removes what Setup installed again ([docs/INSTALL.md](docs/INSTALL.md#removing-the-build-tools));
   - looks for your game (an ISO or a game folder), checks it, then copies it into the port folder (or moves a folder, if it's on the same drive);
   - decrypts the game's two program files using the application keys from RPCS3's public source code;
   - downloads the DLSS SDK and FFmpeg;
   - generates the game code from your copy and builds the game.

   The first run takes a while: installing the tools, plus a 20 to 60 minute build.
3. Play with **`Launcher.exe`**. On the first start it detects your hardware and picks recommended settings; everything is adjustable there.

Run Setup again only after downloading a newer version of the port.

**Choosing the game in Setup.bat:** it lists the copies it found, numbered. Type a number, or type or paste the path of the ISO or game folder (you can also drag it into the window). **B** opens a file picker (Windows only). `Setup.bat -Game "<path>"` skips the question. The path can be an ISO, the game folder, or a file inside it. An ISO is opened by Windows, copied from and closed again; if Windows can't open it, Setup reads it directly.

### Setup FAQ

- **Why does the first setup take so long? Is it a one-time thing?** Mostly, yes. The first run installs the build tools (10-30 minutes, once per PC), copies your game (5-15 minutes) and then compiles the whole game from your copy. That compile is the long part: the generated game code is split into a few very large files, each needing up to 4 GB of RAM, so a PC can only compile as many of them at once as its RAM allows. Measured on a 12-thread desktop CPU: about 20-30 minutes with 32 GB of RAM, 30-45 minutes with 16 GB, about 1 hour with 12 GB; slower CPUs, laptops on battery and antivirus scanning add to that. Setup prints its own estimate and the time each step took (in `logs\setup.log`).
- **Updates are much faster.** Unzip the new version **over your existing port folder** (overwrite the files) and run Setup again: the tools, your game copy, the generated code and the compiled files are reused and only what changed is rebuilt, usually in 5-20 minutes. Unzipping into a **new** folder starts the game copy and the build from scratch again (the tools are still reused). When an update changes the code generator, Setup notices it by itself and generates the game code again (10-25 minutes; the first update after 0.2.3 does this once). Generated files that come out the same keep their date, so only the files that really changed are compiled again.
- **Making the first build faster:** close browsers and games while it runs (free RAM = more files at once); put the port on an internal NTFS drive (not a USB/exFAT drive or a network drive); optionally exclude the port folder from Microsoft Defender's real-time scan (Windows Security, *Virus & threat protection > Manage settings > Exclusions > Add an exclusion > Folder*), which otherwise scans every file the build writes. Setup tells you if Defender is scanning the folder; it never adds the exclusion itself.
- **How much disk space does it take, and what can I delete?** A finished install is about 20 GB: your game copy (`gamefiles`, the largest part), the generated code (`recompiled`), the compiled files (`build*` folders) and the tools. Keep them all: the build folders are what makes updates fast. Setup no longer copies the disc's bonus videos (`PS3_CONTENT`) or system update (`PS3_UPDATE`), which the game never reads, and deletes them and its work folder (`build_regen`) after a successful build. At the end Setup prints how much each part takes; run `powershell -ExecutionPolicy Bypass -File tools\build_doctor.ps1 -DiskUsage` to see it again. It also points out folders Setup did not make, such as a second copy of the port left by unzipping an update into a subfolder. Your original ISO or game folder is not needed after Setup has finished.
- **Can I keep the game files (`gamefiles`) on another drive?** Yes. Move the whole `gamefiles` folder (or copy it and delete the old one when the game works), then in the launcher open **Saves & tools > Game files folder > Choose folder...** and pick it (the folder that holds `PS3_GAME`; picking the folder above it, `PS3_GAME` or `USRDIR` also works). The launcher checks that the game's files are there (`PS3_GAME\PARAM.SFO`, `USRDIR\EBOOT.BIN`, `game.self`, `game.psarc`) and that it can write there, and offers to copy the game's settings and quick save (`game\BCUS98107` inside the game files folder) along. The game, the launcher's updates and rebuilds (Setup), Setup.exe, trophies, the HD UI pack and the problem report all use that folder from then on; **Use default** goes back to `gamefiles` in the port folder. Your saves do not move: they stay in `Saved Games\Resistance Fall of Man` (or `gamedata` with a portable save location). The path must be a full path in plain English letters (no accents, `%`, `!` or `"`), at most 200 characters. A `PS3_VFS_ROOT` you set yourself (in Windows' environment variables, or by hand in `run.bat`) wins over the launcher setting; updates replace `run.bat`, so the launcher setting is the one that lasts. Setup can also be pointed at a folder once with `setup.ps1 -GameFiles <folder>`. Uninstalling only ever deletes `gamefiles` in the port folder, never a folder you picked.
- **"ninja: error: manifest 'build.ninja' still dirty after 100 tries" and other build errors:** the "still dirty" error came from files dated in the future (the release zip's time zone, or a PC clock that was set wrong), which made the build loop. Fixed in 0.2.2: every build now resets such dates first. If a build still fails, Setup shows the first error and tries once more with a fresh build configuration (the files already compiled are kept). It does the same after the port folder was moved. To do it by hand: `powershell -ExecutionPolicy Bypass -File tools\build_doctor.ps1 -ResetConfig`, then run `build.bat` or Setup. `-FirstError` shows the first error from `logs\build.log` / `logs\build_game.log`; include that line and `logs\setup.log` when reporting a problem.

### If your game copy is not accepted

The installer and Setup check the copy before copying anything. The supported version is **Resistance: Fall of Man (US disc, BCUS98107)**, about 16.5 GB.

| Message | What to do |
|---|---|
| compressed archive (.zip, .7z, .rar) | Extract it first, then pick the ISO or folder inside. |
| PlayStation Store package (.pkg) / game update folder | Pick the disc copy. Game updates are not needed. |
| not &lt;game&gt; / from another region | Use the supported disc version listed above. |
| PlayStation Store version | Only the disc version is supported. |
| encrypted disc image / PARAM.SFO is not valid | Decrypt the ISO first (for example PS3Dec with the disc key from the `.dkey` file), or use a game folder from a PS3 or RPCS3. |
| program file was decrypted or replaced (plain ELF) | Use the original, unmodified files. Setup decrypts them itself. |
| files are missing / smaller than expected | The copy is incomplete. Copy or dump it again. |
| different from the known file (warning) | The copy may be modified. You can continue; Setup stops later if the decrypted program is not the supported version. |
| not enough free space | Free up space, or install on another drive. |

The checks run on your PC only. Nothing is uploaded.

**Updates:** the launcher checks this repository's Releases page once at start. The *Updates* option (Advanced page) picks what happens when a newer version is out: **Notify** (default) asks **Update now** / **Release page** / **Skip version**; **Auto-install** asks once, then updates; **Off** does not check (also `PORT_NO_UPDATE_CHECK=1`; `PORT_UPDATE_NOTIFY_ONLY=1` brings back the old notice that only opens the release page). *Update now* closes the launcher and runs `tools\update.ps1`: it downloads the release zip (checks its size and SHA-256), waits for the game to close, replaces the port's files, runs Setup in update mode (incremental: usually 5-20 minutes) and starts the launcher again. Your settings, key bindings you changed, saves, logs, game copy, generated code, build folders and shader caches are kept; a file is deleted only if an older release shipped it and you did not change it. If anything fails, the previous files are put back (log: `logs\update.log`; `tools\update.ps1 -Rollback` undoes an interrupted update, `-DryRun` only shows what would change). Git checkouts (`git pull`, then Setup) and copies installed by RecompLauncher (update them there) only get the notice. The installed version is in the `VERSION` file.

**Settings and controls** (`settings.ini`, `settings.bat`, `controls.txt`, `mouselook.cfg`) are also kept in `%USERPROFILE%\Saved Games\Resistance Fall of Man\config`. Missing files are restored from there when the launcher or game starts, so they survive reinstalling or updating the port. To keep everything in the port folder only, create an empty file named `portable.txt` next to `Launcher.exe` (the installer's *Portable mode* does this).

**No console window:** the launcher starts the game without a console. Everything the console used to show is written to `logs\run.log`.

**First launch:** the game shows its one-time "initial setup" screens. Shaders are compiled before the game starts, with a **Compiling shaders N / M** progress panel (see *Shader precompile*), so levels don't sit black while they compile. The shader cache is kept, including a backup that survives updates and reinstalls.

## Controls

Keyboard and mouse, Xbox controllers, PlayStation pads (DualShock 4, DualSense, DualShock 3 with its driver) and Switch Pro / Joy-Con all work, and you can switch between them at any time. Controllers are read through SDL3 (built in, no extra DLL), over USB or Bluetooth, so **DSX, DS4Windows and Steam Input are not needed**, and work best closed: they can hide the real controller from other programs (HidHide) and show only their virtual Xbox pad. If they run anyway and the port sees both the real controller and the tool's virtual copy, the copy is recognised by its mirrored input and ignored, so it doesn't become a ghost player 2 (with local co-op off, the default, a second controller never becomes player 2 anyway). If SDL3 can't start, the port falls back to XInput (`logs\run.log`: `[cellPad] controllers: SDL3` or `SDL3 could not start`; a build made without SDL3 says `controllers: XInput only`). If a controller isn't seen, `logs\run.log` lists every controller SDL3 found (`[pad-sdl3] controller 1: ...`), controllers it can't use, and notes when HidHide or a Steam setting may be hiding one.

- **Button prompts follow the last device you used.** On keyboard and mouse the game shows your key bindings. On an Xbox / XInput controller it shows Xbox buttons that match the real mapping: Cross = **A**, Circle = **B**, Square = **X**, Triangle = **Y**, L1/R1 = **LB/RB**, L2/R2 = **LT/RT**, L3/R3 = **LS/RS**, Select = **View**, Start = **Menu**. The Xbox glyphs are drawn by the port (coloured circles with letters); no Microsoft artwork is used. A PlayStation controller (by its SDL3 type, or with XInput by its USB / Bluetooth ID) keeps the PlayStation symbols. Switch controllers are read by button position (bottom = Cross) and show PlayStation symbols; with *Nintendo button layout = Nintendo style* A (right) confirms and B cancels, and the prompts show the Xbox-style letters. `RFOM_PAD_GLYPHS=auto|xbox|ps` overrides the choice (default `auto`; use it under Steam Input if the real controller type isn't reported).
- **Controller hot-plug:** controllers are polled every few milliseconds, so plugging one in or pulling it out works at any time without a restart. The log shows `[cellPad] controller N connected` / `disconnected`.
- **Mouse look is native.** Raw mouse movement (Windows raw input) turns the player's camera directly, 1:1 on foot; it is not converted into an analog stick. Scoped weapons scale it by their zoom.
- Melee and interact are split onto separate keys (on the PS3 they shared a button).
- Bindings are edited in the launcher (Controls page) or by hand in `controls.txt` (`KEY = ACTION`, one per line; the header of the file lists every key and action name).

Mouse and controller options in `controls.txt`:

| Option | Default | What it does |
|---|---|---|
| `mouse_look` | `direct` | `direct`: raw mouse deltas turn the camera. `stick`: the old analog-stick emulation. |
| `mouse_sensitivity` | `2.00` | Overall mouse speed (also multiplied by the game's own Look Sensitivity). |
| `mouse_degrees_per_count` | `0.050` | Degrees turned per mouse count in direct mode, before `mouse_sensitivity`. |
| `mouse_invert_y` | `0` | `1` inverts vertical look. |
| `mouse_deadzone`, `mouse_smoothing` | `0.28`, `0.03` | The stick the mouse feeds where it doesn't turn the camera directly (vehicles, turrets; everywhere with `mouse_look = stick`). The deflection starts past this deadzone, and fractions of a stick step carry over to the next frame. |
| `mouse_tank_sensitivity` | `1.0` | Tank turret (Ice and Iron): moving the mouse turns the turret by a set angle, the same as on-foot look (`mouse_degrees_per_count` × `mouse_sensitivity` × the game's Look Sensitivity), times this value (0.05–10). The turret no longer speeds up slowly from a stick deflection. `RFOM_TANK_MOUSE=0` turns this off. |
| `mouse_release_key` | `F8` | Frees / recaptures the mouse cursor. |
| `pad_interact` | `NONE` | A controller button that only uses things (doors, switches, vehicles); the Action button then only melees. `NONE` = as on PS3. |

More input options (launcher → Controls page, written to `settings.bat`; log line at start: `[controls] PC options: vertical mouse x.., stick deadzone .., curve .., zoom hold|toggle (game)`):

| Launcher setting | Choices (default) | Environment variable | What it does |
|---|---|---|---|
| Vertical sensitivity | 50%, 75%, **Same as horizontal**, 125%, 150%, 200% | `PAD_MOUSE_SENS_Y=0.1..4` | Up/down mouse speed relative to left/right (direct and stick mouse look). |
| Zoomed sensitivity | 50%, 75%, **Same**, 125%, 150% | `RFOM_ADS_SENS=0.1..3` | Extra mouse multiplier while zoomed or scoped, on top of the automatic zoom matching (`RFOM_ZOOM_SENS`). |
| Zoom | **Toggle** (PS3), Hold | `PAD_AIM_HOLD=1` | Hold: zoomed only while the zoom key / R3 is held. The game's zoom is a toggle, so the port presses zoom again when you let go (keyboard, mouse and the first controller), timed in game frames, and again if the game is still zoomed a moment later (`PAD_AIM_HOLD_OLD=1`: the old one-shot press). |
| Stick deadzone | **Default** (24%, per axis, as on PS3), 5-30% | `PAD_DEADZONE=0..0.9` | A radial deadzone for both controller sticks, rescaled so the full range stays usable. Raise it if the view drifts. |
| Stick response curve | **Linear**, Fast, Precise, Very precise | `PAD_CURVE=0.5..3` (exponent; 1 = linear) | Above 1: small movements are slower for fine aiming; below 1: quicker. A curve other than 1 also switches the sticks to a radial 24% deadzone when none is chosen. |
| Stick outer deadzone | **Off**, 5, 10, 15% | `PAD_OUTER_DEADZONE=0..50` | The outermost part of the stick's travel already counts as full deflection (worn sticks reach full turn speed). |
| Stick anti-deadzone | **Off**, 10, 20, 30% | `PAD_ANTI_DEADZONE=0..50` | The smallest movement outside the deadzone already starts at this much deflection, skipping the game's own dead zone. |
| Trigger deadzone | **Off**, 10, 20, 30% | `PAD_TRIGGER_DEADZONE=0..50` | L2 / R2 only count once pulled this far. |
| Turn boost | **Off**, 90, 180, 270 deg/s | `PAD_TURN_BOOST=0..720` | Look acceleration: the right stick held at the edge ramps up extra turn speed (not while zoomed). |
| (no launcher option) | **On**, Off, Left stick only | `PORT_KBM_WHEEL=0\|left` | Weapon wheel on keyboard & mouse: while the wheel key (E) is held, moving the mouse toward a slot (or WASD, diagonals full length) picks it, and the choice stays until you let go; the view holds still meanwhile. The direction goes to the left stick and, with direct mouse look, the right stick. `0` = old behaviour; `left` = left stick only. |
| Crouch | **As the game**, Toggle, Hold | `PAD_CROUCH_MODE=toggle\|hold` | Experimental. Toggle / hold for the crouch button (L2, C). L2 is sprint in some layouts, so it follows the same mode. |
| Vibration | **100%**, 75, 50, 25%, Off | `PAD_RUMBLE=0..100\|off` | Rumble strength of every controller (both motors scaled). |
| Confirm button | **Cross**, Circle | `PS3_ENTER_BUTTON=circle` | The console's "enter button" setting (Japanese layout: Circle confirms). Keyboard Enter / Esc keep meaning confirm / back in menus. Experimental: the game may keep its own layout. |
| Controller in the background | **Auto**, On, Off | `PAD_BACKGROUND_INPUT=1\|0` | Auto: controllers keep working while another window has focus only when *In the background* is *Keep running* or *Mute only* (the game keeps running). |

"Restore default controls" also resets these.

**Controllers (SDL3), gyro and light bar** (launcher → Controls page, and in game *More PC Options*; written to `settings.bat`). Rumble works on every pad that has it, DualSense included. **Gyro aiming** (DualShock 4, DualSense, Switch Pro, Joy-Con; off by default) turns the camera directly like the mouse (smooth, not limited by the stick; with `mouse_look = stick` it drives the right stick instead). Leave the controller still for about a second after it connects so its resting drift is calibrated.

| Launcher setting | Choices (default) | Environment variable | What it does |
|---|---|---|---|
| Controller support | **SDL3**, XInput | `PAD_BACKEND=sdl3\|xinput` | XInput = the old path (Xbox pads and wrappers only, up to four, no gyro / light bar). `sdl2` is accepted and uses SDL3. Restart to change. |
| Gyro aiming | **Off**, On, While zoomed | `PAD_GYRO=0\|1\|aim` | `aim`: only while zoomed in / scoped. |
| Gyro sensitivity | **2x**, 0.5x-6x | `PAD_GYRO_SENS=0.05..20` | View degrees per controller degree. |
| Gyro vertical | **Normal**, Inverted | `PAD_GYRO_INVERT_Y=1` | On top of the game's own invert setting. |
| Gyro turning axis | **Controller**, Player | `PAD_GYRO_SPACE=local\|player` | Player: yaw follows gravity, so it works with the controller flat or upright. |
| Gyro calibration | **On connect**, Off | `PAD_GYRO_CALIBRATE=0` | Off: no 1 s still-calibration. |
| Light bar | **Player colour** (blue), Off | `PAD_LIGHTBAR=0` | DualShock 4 / DualSense light bar. |
| Nintendo button layout | **By position**, Nintendo style | `PAD_NINTENDO_SWAP=1` | Switch controllers: A confirms, B cancels. |
| (none) | **Automatic** (the one used last) | `PAD_P1_CONTROLLER=1..8` | Which controller (SDL3: in connection order) is the player's while it is connected; otherwise the controller used last (SDL3; with XInput or local co-op on, the first connected one). |
| (none) | **On** | `PAD_DEDUP=0` | Keep virtual duplicates (DSX / Steam Input / DS4Windows / ViGEm) as separate controllers. With it on, a controller that connects while another one is playing joins once it is used on its own; one that copies several different inputs of the other controller is ignored until several different buttons are pressed on it while the other controller is untouched (and is still watched after that). |
| (none) | **360** | `PAD_GYRO_STICK_DPS=deg/s` | Gyro speed equal to a full right stick (only when gyro drives the stick). |

### Keys that always work

| Key | What it does |
|---|---|
| **Alt + Enter** | Switch between borderless fullscreen and a window. |
| **F8** | Free / capture the mouse cursor (changeable with `mouse_release_key`). |
| **F10**, or hold **Esc** for 1 s, or hold **Start / Menu** for 1.5 s | **PC menu**: Resume / PC Options / Quit to Desktop (see *Window, focus and quitting*). It always opens on Resume (the mouse takes over the highlight only once moved). The game is paused and muted while it is up, and its SPU threads idle instead of spinning. |
| **F12** or **PrtScn** | Screenshot: a PNG in the `screenshots` folder (`RFOM_YYYYMMDD_HHMMSS_mmm.png`); a "Screenshot saved" toast shows briefly. |
| **F3** | Performance overlay (see *Performance overlay*). |
| **Alt + F4** / the window's close button | Also quits cleanly (waits for a save in progress). |

## Local co-op
Two players on one PC, split-screen, as on PS3. In the launcher, Controls page, set **Local co-op (split-screen)** to **On** (it is off by default, so a second controller, or a virtual copy of your controller made by DSX / DS4Windows / Steam Input, can't start split-screen by itself). Then main menu → the co-op / split-screen option, and press **START on the second controller** to join. Each controller is a player; the keyboard and mouse belong to player 1.

- With local co-op off (the default): one player. Player 1 = the keyboard and mouse plus **the controller you used last**: pick up any connected controller and press a button, and it takes over (Controller support = SDL3, the default; with XInput it is the first controller). The game never sees a second controller.
- With local co-op on: player 1 = the keyboard and mouse plus the first controller found.
- `logs\run.log` shows the choice: `[cellPad] local co-op: off ...` / `on, player 2 = ...`, and `[cellPad] player 1 <- controller N (used last)` when another controller takes over.
- **A real second controller is ignored?** Unplug it and plug it back in; `PAD_DEDUP=0` turns the duplicate check off.

| Option | Values | Variable |
|---|---|---|
| Local co-op (split-screen) | **Off**, On | `PAD_COOP=1`: a second controller can join as player 2. Off (`PAD_COOP=0` or unset): the game sees only player 1, and extra controllers (or duplicate virtual pads from DSX / Steam Input / DS4Windows) can't join. *Player 2 controller* applies when it is On (outside the launcher, with `PAD_COOP` not set at all, setting `PAD_P2_CONTROLLER` turns it on) |
| Player 1 controller | **Automatic**, Controller 1-8 | `PAD_P1_CONTROLLER=<1..8>`: the controller that is player 1 (needs Controller support = SDL3, numbered in the order they were connected). Automatic = the controller used last (co-op off) or the first one found (co-op on) |
| Player 2 controller | **Automatic**, Controller 1-8 | `PAD_P2_CONTROLLER=<1..8>`: the controller that is player 2 when Local co-op is On (numbered as above; needs Controller support = SDL3) |

In co-op the mouse turns only player 1's camera, at the same speed as in single player (`RFOM_COOP_ML`). There is no *Player 2 uses keyboard and mouse* option in Resistance: Fall of Man: the port's mouse look writes straight into player 1's camera, so the keyboard can't be player 2. W/S/A/D only drive the Lynx while player 2's camera code isn't running, so not while they are on foot, in the Lynx gunner seat or on a turret (`RFOM_KBM_DRIVE`); E / Shift always work. Online play (PS3: 40 players on Sony's servers, now closed) is off by default; see Online (experimental) below.

## Online (experimental)

Off by default; single player is the same with it off. `PORT_ONLINE=1` turns on real network sockets, signs in to RPCN with your own existing account and points the game's server names at a community server such as PS Rewired (psrewired.com). Copy `online.ini.example` to `online.ini` next to `run.bat`, then fill in `rpcn_user` / `rpcn_password` and uncomment the PS Rewired lines. `online.ini` keeps the password in plain text and is never committed; the port sends RPCN only the derived hash, as RPCS3 does. Untested against the live server: the port is game version 1.00, and the server may expect the last patch. Levels load in a second process, which signs in again and drops any open connection. Details: `ps3recomp/docs/ONLINE.md`.
## PC features

Set in the launcher (the Display, Graphics, Upscaling & Frame Gen, Controls, Interface & Notifications and Advanced pages, and the **Saves & tools** page) or in game with **F9** (or F10 > *More PC Options*). Changes made in game are saved to `settings.bat` / `settings.ini`, so the launcher shows them too.

| Variable | Default | What it does |
|---|---|---|
| `PORT_VOLUME` | 100 | Master volume 0-100% on top of the game's own volume options (launcher: Audio; F9). |
| `PORT_AUDIO_MONO` | 0 | `1`: accessibility mono downmix, every channel to both ears. |
| `PORT_AIM_ASSIST` | auto | The game's aim assist (aim slows down over an enemy and is pulled toward it). Unset / `auto`: off with keyboard & mouse, on with a controller (in split-screen only the keyboard & mouse player loses it); `on`: always, as on PS3; `off`: never. Launcher: Controls > Mouse > *Aim assist*; F9 (changes at once). `RFOM_AIM_ASSIST` also works; `PORT_AIM_ASSIST` wins. |
| `PORT_UI_SCALE` | auto | Size of the PC menus (F9 / F10), the F3 overlay and notifications: `75`-`200` (%). Auto enlarges the overlay and notifications on 1440p (125%) and 4K (150%) windows. |
| `PORT_OVERLAY_GRAPH` | 1 | The F3 overlay draws a graph of the last 120 frame times under its numbers (yellow / red bars = slow frames); `0` hides it. |
| `RFOM_PAD_GLYPHS=nintendo` | auto | Nintendo (Switch) button prompts: A / B / X / Y letters in the Nintendo positions. Auto picks them for a Switch Pro controller or Joy-Cons. |
| `RSX_RESIZABLE` | `1` | The window can be resized or maximised; the swap chain follows the new size. `0` = fixed size. |
| `RSX_ALT_ENTER` | `1` | Alt+Enter switches between a window and borderless fullscreen. |
| `RFOM_SKIP_INTRO` | `1` | Skips the intro movie in the front end. (The game has no logo or legal videos; the intro skip is not yet verified.) |
| `RFOM_CUTSCENE_SKIP` | `1` | Hold Space / Enter or (A) / Start for 1 s to skip a playing movie. Only full-screen movies are skipped, not in-engine scenes. |
| `RFOM_CUTSCENE_SKIP_ALL` | `1` | The hold also skips a story movie you have not watched yet (the game itself only skips movies already seen). `0` = the game's rule. Needs `RFOM_CUTSCENE_SKIP`. Launcher: Interface & Notifications › *Skip unwatched movies*. |
| `PORT_SKIP_HINT` (or `RFOM_SKIP_HINT`) | unset (Auto) | "Hold Space / A to skip" notification when a movie starts. Unset = shown when Hold to skip movies and Skip unwatched movies are on, since every story movie can then be skipped; `1` = always; `0` = never. Launcher: Interface & Notifications › *Show cutscene skip hint*. |
| `RFOM_MOVIE_LFC` | unset (Auto) | Movies on a VRR (G-Sync / FreeSync) screen: each 30 fps movie frame is presented twice, evenly spaced, to stay inside the VRR range. `0` = off (the driver's own low-framerate compensation only; worth an A/B if movies stutter), `N` = N presents per frame (1-8). Every 5 s of a movie `logs\run.log` has a `[D3D12] movie LFC 5.0 s:` line: real movie presents and their spacing, repeats presented / scheduled / dropped (late) / cut off and how late they went out, and what the decoder delivered (pictures the game took or skipped, slowest decode). Frame generation is always held off while a movie plays. |
| `RFOM_TROPHY_TOAST` | `1` | `0` hides the achievement / trophy pop-up; unlocks are still recorded. Launcher: Interface & Notifications › *Achievement notifications*. |
| `PORT_TROPHY_STYLE` (or `RFOM_TROPHY_STYLE`) | PS3 | `old` uses the older pop-up with the grade badge instead of the PS3-style one. Launcher: Interface & Notifications › *Achievement pop-up style*. |
| `PORT_TROPHY_TEST` | off | `1` shows one sample achievement pop-up per grade (bronze, silver, gold, platinum) a few seconds after start-up. |
| `RFOM_HDR` | off | `1` = HDR10 (PQ, Rec.2020), `scrgb` = scRGB (FP16). The final image is converted from SDR with the paper-white setting; falls back to SDR when the display or Windows HDR is off. |
| `RFOM_HDR_PAPER` | `203` | HDR paper white in nits (how bright the game's white is). |
| `RFOM_HDR_PEAK` | the display's peak (else `1000`) | Display peak in nits, sent as HDR metadata and used for the roll-off. |
| `RFOM_HDR_SAT` | `1` | HDR colour saturation, 0…2 (1 = as the game). |
| `RFOM_REFLEX` | off | `1` = On, `2` = On + Boost. On an NVIDIA GPU this is **NVIDIA Reflex Low Latency** through NVAPI (`tools/nvapi`): `NvAPI_D3D_SetSleepMode`, one `NvAPI_D3D_Sleep` per frame right after the port's frame limiter, and the latency markers (simulation start/end, render submit start/end, present start/end, input sample); log `[reflex] NVIDIA Reflex Low Latency ...`. Elsewhere (other GPUs, old drivers) it falls back to a waitable DXGI swap chain with at most one queued frame (`[reflex] NVAPI unavailable ...`). Without frame generation Reflex is never a frame limiter: its own interval is 0 and the Frame rate (`RFOM_FPS`) decides the cap. |
| `RFOM_REFLEX_LIMIT` | 1 | With DLSS Frame Generation (Reflex through Streamline): `1` = Reflex's frame limit is the cap in effect (`RFOM_FPS`, or the *Display sync* plan), in rendered frames before generation (1e6 / fps us), updated when it changes; the port's limiter then ends its wait 5% early so Reflex sets the pace. Not while fixed-refresh pacing's phase lock runs (`2` = also then). `0` = no Reflex frame limit (the old behaviour). Log: `[reflex] frame limit N us (source ...)`. |
| `RFOM_REFLEX_FG_DIV` | 0 | Test (0.3.1.1 "MVQ3"): `1` = while frame generation is on, Reflex is given the limit per *presented* frame (the rendered-frame limit / (generated frames + 1)); back to the plain limit when generation is off (menus). With the plain 16667 us limit and 2x generation, rendered frames locked at exactly 30.0 fps (33.3 ms) against ~58 fps without generation: Reflex paces the output, so every generated frame bridged twice the motion. The port's own limiter still caps rendered frames. |
| `PORT_LATENCY` | unset | Latency reduction (launcher and F9). Unset: `RFOM_REFLEX` alone decides. `off`; `reflex` (Reflex On, or On + Boost with `RFOM_REFLEX=2`); `antilag` = **AMD Anti-Lag 2** through AMD's D3D12 SDK (`tools/antilag2`, MIT; Radeon RX 5000 or newer with a driver that has it): `AntiLag2DX12::Update` once per frame where the Reflex sleep would be, before input; `auto` = Reflex, and Anti-Lag 2 when NVAPI Reflex is unavailable. Never with frame generation (that keeps its own Reflex). Log: `[latency]`. |
| `RSX_HUD_FIX` (old name `RFOM_HUD_169` still read) | off | `1` (experimental; launcher Display › Screen *Ultrawide HUD*, settings.ini `hudfix`, was `hud169`; same key, label and variable as R2/R3): while the present pass stretches the 16:9 guest frame to an ultrawide aspect (`RFOM_ASPECT`), draws the renderer's HUD classifier takes for HUD (display-buffer alpha / additive sprites and text after the 3D scene) get a viewport squeezed by the same ratio, so the HUD stays undistorted in a centred 16:9 area. World-anchored markers may sit off their targets; a full-screen tint or fade may cover only the centre. |
| `RFOM_PHOTO` | `1` | **F7** photo mode: pauses the game, hides the port's overlays, **F12** saves a clean screenshot. No free camera (see below). |
| `RFOM_DISCORD` | off | Discord Rich Presence through the local Discord pipe; does nothing when Discord isn't running. Application id: `RFOM_DISCORD_APPID` or `discord_appid.txt` next to run.bat. |
| `RFOM_SAVE_BACKUP` | `10` | Play backs up your saves (when they changed) to `save_backups` next to the saves folder, keeping the newest N; `0` = off. The Saves & tools page lists, backs up, restores and deletes backups and opens the folders. |
| `RFOM_COLORBLIND` | off | Colour-blind filter on the final image: `1` protanopia, `2` deuteranopia, `3` tritanopia (also live in F9). |
| `RFOM_CROSSHAIR` | off | An extra centre crosshair: `dot` or `cross`. |
| `RFOM_CROSSHAIR_COLOR` | white | Extra crosshair colour: `green`, `yellow`, `cyan`, `red`, `pink` or `orange` (dark outline always drawn). Live in F9. |
| `RFOM_CROSSHAIR_SIZE` | small | Extra crosshair size: `medium`, `large`, `huge` or a percentage 50-400 (small = 24 px). Live in F9. |
| `RFOM_BENCH_KEY` | F11 | Benchmark capture hotkey (`F6`, `F5`, `off`), see *Benchmark* below. |
| `RSX_WINDOW_REMEMBER` | on | `0` = always open the window centred (see Display). |
| `RSX_DPI_AWARE` | on | The game draws in real screen pixels under Windows display scaling (125%, 150%, 300%...): the window's inside is exactly the chosen Window size and fullscreen uses the screen's full resolution. `0` = let Windows scale the game as before 0.3.1 (blurrier; for testing). |
| `RFOM_KBM_DRIVE` | `1` | Keyboard driving in the LU-P Lynx: while you are in its driver seat (the game's Lynx driver input is running for the keyboard player), **W** / **S** also press accelerate / brake-reverse (R2 / L2) and **A** / **D** also steer (right stick). On foot, on turrets, in gunner seats, in the tank (stick-driven, mouse aims the turret) and in menus W/S/A/D are unchanged. **E** / **Shift** (R2 / L2) work too. Co-op limitation: it only works while the other player's camera code isn't running (on foot, in the Lynx gunner seat or on a turret it keeps the keyboard in on-foot mode), so in co-op it rarely applies; use E / Shift then. `0` = W/S/A/D only press the d-pad there, as before. |
| `RFOM_TANK_MOUSE` | `1` | Tank turret with the mouse (Ice and Iron): mouse movement turns the turret by a set angle instead of acting like a stick deflection that the turret slowly speeds up to follow. Applies only while you drive with keyboard and mouse; controllers are unchanged. The speed is set by `mouse_tank_sensitivity` in `controls.txt`. The first mouse move in the tank still uses the stick while the port works out which way the stick reads (log: `[tank] mouse turret mode on ...`). `0` = the old stick behaviour. |
| `RFOM_COOP_ML` | `1` | Split-screen co-op: direct mouse look turns only the player the keyboard and mouse control (player 1, learned from which camera follows the mouse); the other player's look is left to their controller. `0` = the mouse look acts on every player, as before. |
| `RFOM_SPLIT_AA` | `0` | Split-screen co-op: DLSS / FSR / XeSS (and so frame generation) pause while two local players are in play, and both views render natively; they come back once one player has played alone for a minute (pause menus and turrets don't end it). `1` = keep the upscaler on in split screen, as in 0.3.0. |
| `RFOM_SPLIT_LOD` | `1` | Split-screen co-op: while the upscaler is paused (`RFOM_SPLIT_AA=0`), textures drop the upscaler's negative LOD bias (*Texture detail* Auto, -1 at Performance) so the natively rendered views don't shimmer. A hand-set `RSX_LOD_BIAS` / `PORT_UPSCALE_LOD_BIAS` number is kept. `0` = keep the upscaler's bias in split screen too. |
| `RFOM_SPLIT_LOG` | `0` | Split-screen co-op diagnostic: the first N clears and target / viewport changes are logged as `[coop]` lines (e.g. `64`, for co-op rendering reports). `0` = off. |
| `RFOM_PAD_DISCONNECT_PAUSE` | `1` | Pauses when the controller in use disconnects; reconnect it or press Enter. |
| `PAD_SOUTHPAW` | off | Swaps the controller sticks. |
| `PAD_MAP_<BUTTON>` | — | Controller remap (e.g. `PAD_MAP_CROSS=SQUARE`); set on the Controls page or in F9 > Controller layout. Keyboard and mouse keys are on the Controls page as before. |
| `RFOM_BUILD_DIR` | `build` | Which folder run.bat starts the game from (a Launcher.exe in a sub-folder sets it to that folder). |

**Benchmark** (Saves & tools > *Run benchmark*, or `RFOM_BENCH=1`): the game continues your campaign by itself (`RFOM_BENCH_SCRIPT`, default `120:START,8:CROSS,8:CROSS,8:CROSS,8:CROSS`, frames:button; `none` = start a level yourself), waits `RFOM_BENCH_SETTLE` s (45) in the level, measures `RFOM_BENCH_SECONDS` (60), writes `logs\bench\RFOM_bench_<date>.txt` (average fps, 1% low fps, frame times, CPU µs per draw) plus `logs\bench\bench_history.csv`, and closes (`RFOM_BENCH_QUIT=0` keeps it running). Focus pause is off during a run.

**Benchmark capture key** — `RFOM_BENCH_KEY` (default `F11`; `F6`, `F5` or `off`; launcher *Interface › Overlays*, F9): press it in game to start recording frame times, press again to stop (also stops by itself after ~28 minutes at 240 fps). Each capture, like the scripted run, writes `logs\bench\RFOM_bench_<date>.txt` (mode, average fps, 1 % / 0.1 % low, stutter count, median / p99 / max frame time, frame cap) and `logs\bench\RFOM_bench_<date>.csv` (`frame,t_ms,frame_ms`, every frame), and appends `bench_history.csv`; the launcher's *Saves & tools* page shows the last result. **1 % / 0.1 % low** = the average fps over the slowest 1 % / 0.1 % of frames (CapFrameX style, not the percentile). **Stutter** = a frame more than twice the average of the previous 30 and at least 4 ms over it; a run of such frames counts once. F11 is free unless you bound it in `controls.txt`. In the two-program setup either program can capture (the front end in menus, the level program in a level).

Already covered elsewhere: the frame rate limit (any cap is safe here; the game uses its own frame time), automatic keyboard / controller prompts, the FOV slider, key binding and hot-plug.

Not included, and why: motion blur / film grain / depth-of-field toggles (the game's own switches for these aren't known yet, and skipping their draws is unverified); subtitle size / background (drawn by the game's own font code); hiding the game's HUD and a free camera in photo mode (no known camera hook); trophies (the game has none; the port's trophy toasts are ready for a build that has them, see *Trophies*); exclusive fullscreen (the front end and the game are two processes handing the screen over through a cover window, and a display-mode switch at each hand-over would flash and fight for focus; borderless fullscreen with tearing allowed already gets the same latency and G-Sync / FreeSync behaviour on Windows 10/11).

### Sprint button

There is no *Sprint button* option (Resistance 2 / 3 have one): this game starts sprinting with a tap and ends it by itself (firing, stopping), and the port can't read that state back, so a Toggle or Hold made from taps would get out of step with the game.

### Ambient occlusion

| Page | Option | Values | Notes |
|---|---|---|---|
| Graphics › Quality | Ambient occlusion | **Game**, Port SSAO (extra), CACAO, Off | `RFOM_AO_MODE` = (game) / `1` / `cacao` / `off` (`RSX_SSAO=1` still means Port SSAO). Fall of Man has no AO of its own, so Game and Off look the same. CACAO: AMD FidelityFX CACAO (`rsx_cacao.inc`, higher quality) at the same point as the port SSAO, from the game camera's near / far / field of view; `RSX_CACAO_RADIUS` (0.6, in metres) and `RSX_CACAO_STRENGTH` (0.5) tune it (smaller than FFX's defaults so flat walls and facades stay unshaded). Also in F9 (on restart). Port SSAO: A screen-space AO pass computed from the scene depth when the scene pass ends, before the game's post-processing. `RSX_SSAO_STRENGTH=<0..1>` (0.6) and `RSX_SSAO_RADIUS=<0.1..8>` (1.0) tune it |
| Graphics › Quality | Ambient occlusion quality | **Medium**, Low, High | `RFOM_AO_QUALITY` = (medium) / `low` / `high`: CACAO quality preset (3 / 5 / 12 samples per pixel). |


### Effects Resistance 2 / 3 can turn off that this game doesn't have

Depth of field, aim-down-sights blur, motion blur and film grain: Resistance: Fall of Man draws none of these, so there is nothing to turn off. Its post effects are bloom, the damage / death blur and the Auger shimmer (`RSX_AUGER_FX`).

## Launcher settings

`Launcher.exe` writes `settings.ini` (its own choices) and `settings.bat` (the environment variables the game reads); **Play** runs `run.bat`, which loads `settings.bat`. On the first start (no `settings.ini` yet) the launcher detects your hardware and fills in the recommended settings. Each list tags the value recommended for your PC with a small "recommended" label. The Display, Graphics and Upscaling & Frame Gen pages open with a **Recommended for your PC** panel: what was detected (GPU, VRAM, CPU cores/threads, RAM, screen), the recommended configuration, whether your settings differ from it, and **Auto-detect (recommended)**, which applies it. Hover a setting to see what it does in the help strip at the bottom; most options state their pros and cons.

The options are sorted into pages (the same pages as the Resistance 2 / 3 launchers); each page groups them under headings. `Launcher.exe --page N` opens on page N:

| N | Page | What is on it |
|---|---|---|
| 0 | Display | Screen (mode, window size, monitor, resizable window, Alt+Enter, aspect ratio, seamless level loads), Frame rate (limit, menu frame rate, VSync, tearing), HDR |
| 1 | Graphics | Resolution (render resolution, scaling), Quality (texture filtering, LOD bias, shadow maps), View (field of view), Accessibility (colour-blind filter) |
| 2 | Upscaling & Frame Gen | Anti-aliasing and upscaling (DLSS / FSR / XeSS, DLSS model, sharpening, object motion vectors), Frame generation and latency (Frame Generation, Reflex) |
| 3 | Performance | Shader cache (compile before playing, compile threads, **Rebuild shader cache**) |
| 4 | Controls | Key bindings, Mouse, Controller, Gyro, Controller buttons |
| 5 | Audio | Sound (speaker layout, audio buffer), In the background |
| 6 | Interface & Notifications | Notifications (achievement pop-ups), Movies (hold to skip, skip hint), Overlays (F3 overlay and its corner), Photo mode and Discord |
| 7 | Advanced | Start-up and quitting, Gameplay (gore, adaptive difficulty), Cheats (single player), Mods and textures, Saves (location, backups, **Open save folder**), Updates |
| 8 | Achievements | Unofficial fan-made achievements read from your save (see *Achievements* under *Trophies*) |
| 9 | Saves & tools | Save backups, the benchmark |
| 10 | About | Version, credits and licences, the key list, folder buttons, the project page, **Report a problem**, **Reset all to defaults** |

Options that are empty by default write nothing to `settings.bat` until changed (the game's default is used).

**Display**

| Setting | Choices (default) | Environment variable | Notes |
|---|---|---|---|
| Display mode | **Borderless fullscreen**, Windowed | `RSX_DISPLAY=borderless\|windowed` | Alt+Enter switches in game. |
| Window size | 1280x720 … 3840x2160 (**1920x1080**) | `RSX_WINDOW=WxH` | Windowed mode only. |
| Monitor | **Main display**, Monitor 2-4 | `RSX_MONITOR=0\|2\|3\|4` | Numbered in Windows' order, main display first. The log shows `[display] monitor n of m`. |
| Remember window position | **On**, Off | `RSX_WINDOW_REMEMBER` (`0` = always centred) | Windowed: reopens where you last left the window, at that size (`window_pos.txt`, saved when a move / resize ends and on close, never while borderless). Ignored when the window size or monitor setting changed or the spot is off every screen. Borderless starts ignore it. Also in F9. |
| Resizable window / Alt+Enter | **On**, Off | `RSX_RESIZABLE=0`, `RSX_ALT_ENTER=0` | See *PC features*. |
| Aspect ratio | **Auto** (window shape), 16:9 (PS3), 16:10, 21:9, 32:9 | `RFOM_ASPECT=auto\|off\|16:10\|21:9\|32:9\|<a:b>\|<decimal>` | Ultrawide / Hor+; see *Ultrawide* below. |
| Seamless level loads | **On**, Off | `RFOM_SEAMLESS=0` | |
| Frame rate | 30, 40, 45, 55, **60**, 72, 82, 90, 120, 144, 165, Just under refresh (G-Sync / FreeSync), Unlimited (240) | `CHILD_RFOM_FPS` (→ `RFOM_FPS`, `vrr` = just under refresh; `0` still caps at 240) | Game speed is the same at every limit (see *Frame rate* below). 40 suits a 40 Hz handheld mode (Steam Deck) or a 120 Hz screen, 45 a 90 Hz one. |
| Menu frame rate | 30, 60, **90**, Same as the game | `RFOM_MENU_FPS` (0 = same as the game) | Cap while the pause / options menu is up over the game. Above ~90 fps the pause menu's dimming layer can flicker. |
| VSync | **Off**, Every refresh, Every 2nd, Every 3rd | `RSX_VSYNC=0..3` | Off is smoothest with FreeSync / G-Sync. |
| Tearing | **Allowed** (lowest latency, VRR), Never | `RFOM_PRESENT=tearing\|notear` | With VSync off: Allowed presents immediately (with G-Sync / FreeSync the screen follows the frame rate); Never drops late frames instead of tearing (`mailbox` / `fifo` are accepted as synonyms). Log: `[D3D12] present: ...`. |
| Display sync | **Auto**, VRR (G-Sync / FreeSync), Fixed refresh | `RFOM_DISPLAY_SYNC=auto\|vrr\|fixed\|off` (`PORT_DISPLAY_SYNC` when unset) | Auto measures the screen's vertical blanks in 2 s windows (only while the game is in front and not minimized): if 3 windows in a row show the refresh does not follow the frames (VRR off, or not active in a borderless window, or a laptop panel on the integrated GPU) it switches to fixed-refresh pacing, where every frame stays on screen for the same number of refreshes: VSync on, the frame limit set to refresh / k (no frame generation: 165 Hz → 55 fps, 144 → 72, 120 → 60; frame generation 2x: 165 Hz → 41.25 rendered / 82.5 shown, 144 → 72 / 144, 60 Hz → 30 / 60; never above the Frame rate setting, 60 allows up to 75; not paced at all when even every refresh would mean under 30 rendered fps, e.g. 60 Hz with frame generation 3x; a PC that can't hold it -- over 20% of the frames of a 2 s window late -- steps one refresh slower per frame, 144 Hz: 72 → 48 rather than a steady 36, and tries the faster rate again after 10 s without misses, waiting twice as long after each failed try, up to 2 min; loading and stalls of 1 s or more don't count; `PORT_PACING_ADAPT=0` = never, log `[display] fixed-refresh pacing: stepping to ...`), videos with VSync every refresh (on 144 / 165 Hz a 30 fps video can't be fully even; that is the least judder). 2 windows in a row where it does switch back. *Fixed refresh* always does this, *VRR* never (the old behaviour; the measurement is still logged), `off` = VRR without measuring. Not with Frame rate `orig` (videos only). Log: `[display] VRR effective: yes/no ...`, `[display] fixed-refresh pacing: ...`, `[fps] frame limit ...`. Also turn off a forced *Vertical sync* in the NVIDIA app / control panel profile for Auto / VRR to work as intended. |
| HDR output, paper white, peak brightness, saturation | **Off**, Auto, HDR10, scRGB | `RFOM_HDR`, `RFOM_HDR_PAPER`, `RFOM_HDR_PEAK`, `RFOM_HDR_SAT` | See *PC features*. |

Button: **Reset to defaults** (asks first; also on the Graphics and Upscaling & Frame Gen pages) puts the display, graphics and upscaling settings, the field of view and the launcher's original PC options back to the recommended defaults for this PC. Controls are not changed.

**Graphics**

| Setting | Choices (default) | Environment variable | Notes |
|---|---|---|---|
| Render resolution | 1280x720 (PS3), 1920x1080, **2560x1440**, 3200x1800, 3840x2160 | `RSX_RES_SCALE=1\|1.5\|2\|2.5\|3` (x 720p) | Above your screen's resolution it acts as supersampling. |
| Scaling | **Smooth**, Sharp pixels, AMD FSR 1 | `RSX_FILTER=linear\|point\|fsr` | How the final image is fitted to the screen. AMD FSR 1 = edge-adaptive EASU upscale in the present pass (Sharpening still applies). |
| Texture filtering | Trilinear (PS3), 2x, 4x, 8x, **16x** anisotropic | `RSX_ANISO=0\|2\|4\|8\|16` | |
| Texture LOD bias | **Auto** (matches the upscale ratio), 0, -0.5, -1, -1.5, +0.5 | `RSX_LOD_BIAS` | |
| Shadow map resolution | **Auto** (up to 2x), 1x (PS3), 1.5x, 2x, 3x, Same as resolution | `RFOM_SHADOW_SCALE` | |
| Field of view | slider 60-150 (default 82, the game's own) | `RFOM_FOV=<degrees>` | Gameplay camera FOV. |
| Colour-blind filter | **Off**, Protanopia, Deuteranopia, Tritanopia | `RFOM_COLORBLIND=1\|2\|3` | Also live in F9. | The filter decodes sRGB to linear light, daltonizes (ReShade *Daltonize* matrices: simulate the deficiency, shift the lost red/green error into green/blue), re-encodes, and runs before HDR encoding. Strength is fixed at full.

**Upscaling & Frame Gen**

| Setting | Choices (default) | Environment variable | Notes |
|---|---|---|---|
| Anti-aliasing | **Off**, AMD FSR 3.1, NVIDIA DLSS / DLAA, Intel XeSS | `RSX_FSR=0\|1\|2\|3` | DLSS needs a GeForce RTX card; XeSS runs on any GPU (XMX on Intel Arc, DP4a elsewhere). If DLSS or XeSS cannot start the game uses FSR 3.1, and if that fails too, plain bilinear scaling. |
| Upscaling | **Native**, Quality 67%, Balanced 58%, Performance 50%, Ultra Performance 33% | `RSX_UPSCALE=1\|0.667\|0.58\|0.5\|0.333` | With FSR / DLSS / XeSS on, the 3D scene is drawn at that share of the render resolution and rebuilt by the upscaler. The menus and HUD stay at full resolution. |
| DLSS preset | **Auto**, transformer K / J / M / L, CNN E / F | `RSX_DLSS_PRESET=K\|J\|M\|L\|E\|F` (or 11/10/13/12/5/6); unsupported values fall back to Auto with a log line | |
| Sharpening | Off, **Low**, Medium, High | `RSX_SHARPEN=0\|0.25\|0.5\|0.8` | |
| FSR 3.1 sharpening | Off, **0.25**, 0.5, 0.75, 1.0 | `RSX_FSR_SHARP` | |
| Object motion vectors | **On**, Off | `RSX_DLSS_OBJMV=1\|0` | |
| Frame Generation | **Off**, DLSS 2x, DLSS 3x, DLSS 4x (needs Anti-aliasing = NVIDIA DLSS on an RTX 40 / 50), FSR 3 2x (any GPU, any temporal anti-aliasing) | `RFOM_DLSS_FG=0\|2\|3\|4\|fsr3`, `RFOM_FG_BACKEND=auto\|dlss\|fsr3` | See *Frame Generation*. |
| NVIDIA Reflex / low latency | **Off**, On, On + Boost | `RFOM_REFLEX=1\|2` | See *PC features*. |
| Latency reduction | **Default**, Off, NVIDIA Reflex, AMD Anti-Lag 2, Auto | `PORT_LATENCY=off\|reflex\|antilag\|auto` | Default = the row above alone decides. See *PC features*. |

**Performance**

| Setting | Choices (default) | Environment variable | Notes |
|---|---|---|---|
| Compile shaders before playing | **On**, Off | `PORT_SHADER_PRELOAD` | See *Shader precompile*. |
| Shader compile threads | **Auto** (all cores but one), 2-16 | `RSX_PSO_PRECOMPILE_THREADS` | |

Button: **Rebuild shader cache** (asks first).

**Controls:** key bindings, the mouse (sensitivity, invert, mouse aiming, the free-cursor key, vertical and zoomed sensitivity, zoom mode, aim assist (`PORT_AIM_ASSIST=on\|off`, unset = Auto), shaking off grabs with the mouse (`RFOM_GRAB_MOUSE=0` = off), the extra crosshair), the controller (use / interact button, deadzone, response curve, controller support, button prompts `RFOM_PAD_GLYPHS=\|xbox\|ps`, Nintendo layout, light bar, pause on disconnect, southpaw, player 1 controller `PAD_P1_CONTROLLER=1..4`), gyro, and the controller button remap. See *Controls* above.

**Audio**

| Setting | Choices (default) | Environment variable | Notes |
|---|---|---|---|
| Speaker layout | **Auto**, 7.1 surround, Stereo, 5.1 surround | `RFOM_SPEAKERS=8\|2\|6` | Auto = 7.1 on a 7.1 output, else 5.1 |
| Audio output | **Auto**, Stereo, 5.1, 7.1 | `PORT_AUDIO_CHANNELS=2\|6\|8` | Auto = the Windows device's speaker setup |
| Audio buffer | **Auto** (100 ms; 150 ms under 12 CPU threads), 80, 100, 150, 200 ms | `PORT_AUDIO_BUFFER_MS` | See *Audio*. |
| In the background | **Pause and mute**, Mute only, Keep running | `RSX_FOCUS_PAUSE=0\|1`, `RSX_FOCUS_MUTE=0\|1` | What happens on Alt+Tab or a click on another window. Pausing frees the CPU/GPU, releases the mouse and resumes exactly where you were. |

**Interface & Notifications**

| Setting | Choices (default) | Environment variable | Notes |
|---|---|---|---|
| Achievement notifications | **On**, Off | `RFOM_TROPHY_TOAST=0` | The pop-up when an achievement is earned. Off still records it. |
| Achievement pop-up style | **PS3**, Classic | `PORT_TROPHY_STYLE=old` | PS3: a PS3 trophy-style pop-up ("You have earned an achievement.", a cup in the grade's colour, the name). Classic: the older panel with the grade badge and description. |
| Hold to skip movies | **On**, Off | `RFOM_CUTSCENE_SKIP=0` | |
| Skip unwatched movies | **On**, Off | `RFOM_CUTSCENE_SKIP_ALL=0` | The hold also skips story movies not watched yet. Needs *Hold to skip movies*. |
| Show cutscene skip hint | **Auto**, On, Off | `PORT_SKIP_HINT` (unset = Auto, `1` = on, `0` = off; `RFOM_SKIP_HINT` also works) | The "Hold Space / A to skip" notification when a movie starts. Auto: shown when Skip unwatched movies is on, since every story movie can then be skipped. |
| Performance overlay | **Off** (F3 shows it), On at start, Disabled | `RFOM_OVERLAY=\|1\|0` | See *Performance overlay*. |
| Overlay corner | **Top left**, Top right, Bottom left, Bottom right | `RFOM_OVERLAY_POS=tl\|tr\|bl\|br` | Notifications appear in the opposite top corner. |
| Photo mode (F7), Discord Rich Presence | **On** / **Off** | `RFOM_PHOTO=0`, `RFOM_DISCORD=1` | See *PC features*. |

**Advanced**

| Setting | Choices (default) | Environment variable | Notes |
|---|---|---|---|
| Skip intro movies | **On**, Off | `RFOM_SKIP_INTRO=0` | |
| Quit keys | **On**, Off (Alt+F4 only) | `RFOM_QUIT_PROMPT=1\|0` | F10 / hold Esc / hold Start → the PC menu (Resume / PC Options / Quit to Desktop). |
| Game language | **Windows language**, English, English (UK), French, German, Italian, Spanish, Dutch, Portuguese, Russian, Polish, Finnish, Swedish, Danish, Norwegian, Japanese, Korean | `PS3_LANG=en\|en-gb\|fr\|de\|...` | The language the game is told the console uses (text and speech your copy has; others fall back to English). Also in F9; takes effect at the next start. |
| Blood and gore | **Full**, Console region setting | `RFOM_GORE=0` | |
| Adaptive difficulty | **On (PS3 original)**, Off | `RFOM_DYNDIFF=0` | |
| Cheats (single player): God mode, Infinite ammo, Infinite grenades, Cheat hotkeys | **Off**, On | `PORT_CHEAT_GODMODE=1`, `PORT_CHEAT_AMMO=1`, `PORT_CHEAT_GRENADES=1`, `PORT_CHEAT_KEYS=1` | Only your hero (and the other local split-screen player); enemies are untouched. Also in F9 (live). With Cheat hotkeys on, **Ctrl+F1** toggles God mode and **Ctrl+F2** ammo and grenades (ignored while F1 / F2 is bound in controls.txt). Once a cheat has been on, trophies are off until the game is restarted; a benchmark run never cheats. Scripted deaths (falls, instant kills) still happen. |
| Mods folder | **Off**, On | `RFOM_MODS=0\|1\|<folder>` | See *Mods*. |
| Texture packs | **Off**, Load mods\textures, Dump to texdump | `RSX_TEXPACK=0\|1\|<folder>`, `RSX_TEXDUMP=0\|1\|<folder>` | See *Texture packs*. |
| HD UI pack | **Off**, On (mods\hd_ui) | `RSX_HD_UI=1\|<folder>` | Experimental. See *HD UI pack*. |
| Save location | **Saved Games folder**, Game folder (portable) | `RFOM_SAVE_DIR=` (empty) \| `portable` \| a folder path | See *Saves* below. |
| Back up saves before playing | **On (keep 10)**, keep 5, keep 30, Off | `RFOM_SAVE_BACKUP` | |
| Updates | **Notify**, Auto-install, Off | — (launcher only) | `PORT_NO_UPDATE_CHECK=1` = Off |

Button: **Open save folder**.

**Achievements, Saves & tools and About** are as before: unofficial achievements, save backups and the benchmark, and the version / credits / folders / **Report a problem** / **Reset all to defaults** (asks first; resets every setting, the key bindings included).

The sidebar picture is drawn by `tools\art\make_r1_art.py` (original artwork, generated in code) and embedded in `Launcher.exe` from `project\launcher_side.png`.

### Hardware presets (auto-detect)

The launcher's recommendation (also available without the launcher with `RFOM_PRESET`) picks the discrete GPU if there is one and maps it to a tier:

| Tier | Typical GPU | Render (output) | Anti-aliasing / upscaling | Filtering |
|---|---|---|---|---|
| Low | integrated graphics, < 1.5 GB cards | 720p | off | 4x |
| Low | GTX 1050 / RX 560 / Radeon 780M, 1.5-3.5 GB | 1080p | DLSS (RTX) or FSR 3.1, Quality (67%) | 8x |
| Medium | GTX 1060-1660 / RX 580 / RTX 3050, 3.5-7 GB | your screen, up to 1440p | DLSS or FSR 3.1 (Quality at 1440p, native AA below) | 16x |
| High | RTX 2070-4060 / RX 6600-7700, 7 GB+ | your screen | DLSS or FSR 3.1 (Performance on 4K screens, Quality at 1440p, native AA at 1080p) | 16x |
| **Ultra** | **RTX 4070 / 5070 or better, or RTX 3080+, with 10 GB+**; RX 6800 / 7800 or better with 12 GB+ | **3840x2160** | **DLSS Performance on RTX** (scene at 1080p → 4K), FSR 3.1 Performance otherwise | 16x |

The discrete GPU is preferred on laptops. Sharpening is Low (Medium for 720p on a larger screen). Frame rate: 60 fps (30 on CPUs with 4 threads or fewer); VSync off.

`RFOM_PRESET=auto|low|medium|high|ultra` fills in only the graphics variables you have **not** set yourself (running through the launcher sets them all, so the preset is for running `build\ResistanceFoM.exe` directly). The log shows `[preset] ...`.

### 1080p rendered → DLSS 4K output (recommended for RTX 40 / 50 cards)

The scene is drawn at 1920x1080, DLSS (Performance) rebuilds it at 3840x2160, and the 4K image is scaled down to your screen, which is much sharper than native 1080p on a 1080p monitor. The HUD and menus are drawn at 4K.

- **Launcher:** Graphics → Auto-detect (recommended) on an RTX 4070 / 5070-class card, or by hand: Render resolution **3840 x 2160**, Anti-aliasing **NVIDIA DLSS / DLAA**, Upscaling **Performance (50%)**.
- **Environment variables:** `RSX_RES_SCALE=3`, `RSX_FSR=2`, `RSX_UPSCALE=0.5` (or `RFOM_PRESET=ultra`).
- DLSS history is reset only on real scene changes (camera cuts, level loads); if DLSS cannot start, FSR 3.1 is used, then plain scaling.

## Frame rate

The game advances its simulation by the real time each frame took, so the frame rate limit (30 / 60 / 120 / 144 / Unlimited …) does **not** change game speed. Details:

- `RFOM_FPS=<n>` (the launcher writes `CHILD_RFOM_FPS`): target frame rate. `0` = uncapped (240), `orig` = the game's own pacing (as on PS3). Values below 30 are raised to 30.
- Scripted sequences that are timed in frames (a chapter's opening walk, for example) run at the original 30 fps steps automatically, then the full rate returns with control (`RFOM_SCRIPT30=0` disables this).
- `RFOM_DT_MAX=<seconds>` raises the largest time step (e.g. `0.05` keeps real speed down to 20 fps; otherwise the game slows down below 30 fps, as on the PS3).
- `RFOM_FPS=vrr` (launcher: *Just under refresh*): caps just below the refresh rate of the monitor the game window is on (refresh − refresh²/3600: 138 fps on 144 Hz, 157 on 165 Hz, 58 on 60 Hz), so a G-Sync / FreeSync screen stays in its variable-refresh range without VSync's latency. Use it with VSync Off. Log: `[fps] RFOM_FPS=vrr: 144 Hz monitor -> 138 fps cap`.
- `RFOM_MENU_FPS` caps menus over the game (default 90; `0` = off).
- Log: `[fps] target 60 fps: ...`.

## Window, focus and quitting

- **Alt+Enter** toggles borderless fullscreen / windowed. **Alt+Tab** is safe: with "In the background" = Pause, the game stops (no CPU / GPU use), mutes, releases the mouse cursor and continues exactly where it was when you come back. The log shows `[focus] paused (window in the background)` / `[focus] resumed`.
- The mouse is released whenever the window loses focus, and with **F8**. The cursor is only hidden while the game has it captured.
- **PC menu:** F10, holding Esc for 1 s or holding Start / Menu for 1.5 s opens the PC menu over the game (from the main menu, the pause menu or gameplay): **Resume**, **PC Options**, **Quit to Desktop**. Arrows / W S A D or the d-pad / left stick select and change, **Enter / Space / A** = OK, **Esc / Backspace / B** = back, **F10 / Start** = close; the mouse works too (left click = OK / next value, right click = previous value). The game is paused and muted, and does not see the keys, while it is up.
  - **PC Options**: *Frame rate* (30 … 165, Unlimited), *Anti-aliasing* (Off / AMD FSR 3.1 / NVIDIA DLSS / Intel XeSS), *Mouse sensitivity* (0.10–4.00), *Field of view* (82 PS3 / 90 / 100 / 110 / 120), *Button prompts* (Auto / Xbox / PlayStation). Mouse sensitivity, field of view and button prompts apply at once (the FOV as soon as the game next sets the camera's view — at the latest after a zoom, a checkpoint or a level load); frame rate and anti-aliasing are marked `*` and apply the next time the game starts. Changes are saved straight to `settings.ini`, `settings.bat` and `controls.txt`, so the launcher shows them too.
  - `RFOM_MENU_PC=0` turns the PC menu off: the keys then open only the "Quit to desktop?" box.
- **Quit to desktop** (PC menu): a small "Quit to desktop?" box (**Enter / A** = yes, **Esc / B** = back to the PC menu). On yes (or Alt+F4 / the close button) the port waits until no save is in progress (up to 30 s), flushes the logs and exits. Log: `[quit] prompt shown`, `[quit] ... waiting for saves`, `[quit] clean exit`. `ESC_CLOSES=1` makes a single Esc quit at once (debug).
- **PC SETTINGS in the game's own menu:** *Options > PC SETTINGS* opens a PC SETTINGS page drawn by the game's own menu (its list rows, font, highlight, sounds and Back; the title reads *PC SETTINGS* / the category; nothing is drawn over the game), grouped into categories: first the category list (**PC DISPLAY**, **PC GRAPHICS**, **PC EFFECTS**, **PC CONTROLS & HUD**, **PC AUDIO & LANGUAGE** — the same names and order as the R2 / R3 ports), one per row; X / Right opens one. A category shows its options four per page: Left / Right (or X / Enter) change the highlighted one, the fifth row pages (*NEXT PAGE (1/3)*, on the last page *BACK (3/3)*; X / Right next, Left previous; past either end = the category list) and Circle / Esc goes back to the category list, Circle / Esc there back to the Options list (in the main menu Back also saves the options, as Audio / Video does). They are the PC Options and More PC Options above, saved the same way; *(restart)* marks the ones that apply on the next start. The pause menu's Options list is *AUDIO / VIDEO, CONTROLS, PC SETTINGS, BACK*. The main menu's *OPTIONS* list has only 4 buttons, so it becomes *AUDIO / VIDEO, CONTROLS, PC SETTINGS, EXTRAS*; **EXTRAS** opens *MOVIES* and *CREDITS* in the same buttons (Circle / Esc = back to OPTIONS). Each menu script is swapped only in the exact version the entry is written for; where it is another version (logged, `[menurows] ... left alone`) or cannot be loaded, that menu (the other keeps its entry) opens the same categories with **X / Enter on one of the volume rows** of its *Audio / Video* (Circle on the category list = back to the audio rows). `R1_MENU_ROWS=1` (default) = the entry, `2` = only the volume-row way, `0` = the original menus. In the main menu the Audio / Video Back is also bound to the menu's own "back to the main menu" action, so its slide-change script (FrontEndMenuNav, exact version only) is swapped too and skips that while the PC page takes the Back (a category = back to the category list); `R1_MENU_BACKNAV=0` leaves it alone (every Back there then returns to the main menu). Log: `[menurows] ...`.
- **Screenshots:** F12 or PrtScn saves the presented frame (with HUD) as a PNG in `screenshots\`. Log: `[screenshot] saved ...`. (The old RSXCAP debug capture on F12 now needs `RSX_CAP_F12=1`.)
- **Intro:** the game has no separate logo videos to skip. The opening front-end sequence is drawn by the game itself and can be skipped as usual with Enter / Space / Cross.

## Ultrawide (Hor+)

`RFOM_ASPECT` (launcher: Display → Aspect ratio; default `auto`):

- `auto` follows the shape of the game window (checked twice a second, so resizing a window or Alt+Enter is followed). On a 16:9 window nothing changes.
- `off` / `0` keeps the PS3's 16:9. `21:9`, `32:9`, `16:10`, any `a:b` or a decimal (1.2-4) force an aspect.
- How it works: the game's camera setter is given the wider aspect and a horizontal field of view widened by the same ratio, so the **vertical** view stays as on PS3 and the sides open up (Hor+); the game's culling follows the wider frustum. Narrower screens (16:10) keep the horizontal view and gain height. The level-of-detail fields keep their 16:9 values, so distant models don't switch to far LODs. Your field-of-view setting applies first and Hor+ on top.
- The image the game draws keeps its 16:9 pixel grid and is stretched to the screen's shape at present time, so on a 21:9 screen each pixel is ~1.3x wider. With DLSS / FSR output at 3840 px (the 1080p → 4K preset), a 2560x1080 or 3440x1440 screen still gets more horizontal pixels than it has.
- Only 16:9 cameras are widened: the gameplay view, aiming down sights and scopes alike. The stretch follows the target aspect, not the camera, so aiming no longer switches it back to 16:9. It shows while a widened camera (gameplay, aiming or scope) was set within the last 2 s (`RFOM_ASPECT_HOLD=<ms>`, `0` = no timeout), and after that for as long as the game keeps drawing 3D frames (the game does not set its camera every frame; `RFOM_ASPECT_SCENE_HOLD=<ms>`, default 500, `0` = the camera timeout alone as in 0.3.0; `RFOM_ASPECT_SCENE_DRAWS`, default 32, is how many depth-writing draws make a 3D frame); screens without a 3D camera (loading screens, briefing and stats screens, menus) and movies keep their 16:9 picture with black bars. Once the stretch is off outside play it stays off, so menus that set a camera or draw a little 3D now and then don't flip between 21:9 and 16:9, until you are in control again (the on-foot look code runs; at once) or 3D frames have come steadily for 1 s (`RFOM_ASPECT_REARM_MS=<ms>`), so such a screen settles one way and stays: a pause menu with the level still drawn behind it may keep or go back to 21:9 after that second, but no longer flips. A cutscene right after a loading screen may start letterboxed for that second. `RFOM_MENU_ASPECT=0` (or `RFOM_ASPECT_REARM_MS=0`) turns this off and restores the 0.3.1 behaviour. It ends at once when the target aspect itself has been 16:9 for a second (`RFOM_ASPECT_OFF_MS`, default 1000), e.g. after resizing the window to 16:9.
- **The HUD is stretched with the view** (it is not kept in a centred 16:9 area): the renderer cannot reliably tell the game's HUD draws from its 3D draws, so they are scaled together.
- Log: `[aspect] RFOM_ASPECT: auto (window aspect)`, `[aspect] window aspect 2.370 (2560x1080): Hor+ on 16:9 cameras`, `[aspect] Hor+: 16:9 camera (aspect 1.778) -> 2.370, horizontal FOV tan x1.333`, then `[aspect] Hor+ camera set N times/s`.

## DLSS and upscaling

- **Anti-aliasing → NVIDIA DLSS / DLAA** uses NVIDIA DLSS on GeForce RTX cards and falls back to FSR 3.1 on other GPUs (log `[DLSS] NGX unavailable ... using FSR 3.1`); if FSR cannot start either, the image is scaled normally (`[FSR] ... FSR off`). The DLSS SDK is downloaded by Setup into `tools\dlss` (if that fails, the port still builds without it).
- **Upscaling** (with FSR, DLSS or XeSS on): renders the 3D scene at 67 / 58 / 50 / 33 % and lets the upscaler rebuild it at the render resolution. The DLSS preset (DLAA, Quality, Balanced, Performance, Ultra Performance) follows the chosen ratio. Menus and HUD stay at full resolution.
- **Anti-aliasing → Intel XeSS** (`RSX_FSR=3`) uses Intel XeSS 2 (`libxess.dll`, next to the exe) with the same inputs as DLSS (colour, depth, motion vectors, jitter). It runs on any GPU: XMX on Intel Arc, DP4a on NVIDIA / AMD. The XeSS preset follows the Upscaling ratio (Native → Native AA, 67% → Ultra Quality, 58% → Quality, 50% → Balanced, 33% → Ultra Performance; log `[XeSS] <preset> at WxH -> WxH`). `RSX_XESS_QUALITY=aa|uqp|uq|q|b|p|up` asks for a specific preset (used when its input range covers the chosen Upscaling ratio, otherwise auto); `RSX_XESS_JITTER_SIGN=-1` flips the jitter sign if the image shimmers. If `libxess.dll` is missing, XeSS cannot start, or it fails while running, FSR 3.1 takes over (`[XeSS] ... using FSR 3.1` / `switching to FSR 3.1`).
- The shader cache is backed up to `%LOCALAPPDATA%\ResistancePort\shadercache` after each session and restored before launch, so reinstalling or updating doesn't bring back first-visit shader stutter.

### DLSS Frame Generation

**Launcher → Upscaling & Frame Gen → Frame Generation** (`RFOM_DLSS_FG=0|2|3|4|fsr3`, default Off) adds NVIDIA DLSS Frame Generation: 2x on an RTX 40, 2x / 3x / 4x (DLSS 4 Multi Frame Generation) on an RTX 50. It raises smoothness when the CPU is the limit (heavy areas of this port are CPU-bound) and adds a little input latency; 4x is for high-refresh displays. It runs through NVIDIA Streamline 2.14.16 (`rfom_dlssg.dll` plus the Streamline DLLs next to the game; Setup downloads the SDK into `tools\streamline` after you accept NVIDIA's licences).

- It needs **Anti-aliasing = NVIDIA DLSS** (it reuses DLSS's depth, motion vectors and jitter) and turns **NVIDIA Reflex** on (required; *On + Boost* is kept if chosen). It is off, with a `[DLSS-G] frame generation disabled: ...` line in the log, on GPUs without DLSS-G, when the DLLs are missing, with FSR / XeSS / no upscaler, with scRGB HDR, and in the front-end program (main menu); with `RFOM_FG_BACKEND=auto` (default) FSR 3 2x is then tried instead (see *FSR 3 Frame Generation*). 3x / 4x on a GPU that only supports 2x uses 2x (`[DLSS-G] 4x frame generation is not supported on this GPU -- using 2x`).
- It switches itself off for frames without a 3D scene (menus, movies, loading screens) -- and for every frame while a movie plays, even one drawn over a 3D scene, so movie frames are never interpolated (FSR 3 FG too; log: `frame generation held off: a movie is playing`) -- while the window is in the background or minimised, and while the window is resized, and back on two frames later. A camera cut or a level load resets its history.
- **The HUD is not generated.** The renderer knows which draws are the HUD (drawn into the final picture after the scene, no depth, alpha-blended, not reading the scene) and hands DLSS-G the picture without the HUD plus the HUD as its own layer, which DLSS-G lays over every generated frame, so text and crosshairs do not warp or ghost. Frames it cannot classify cleanly are generated whole (as without the split). The port's own overlay (F3, notifications) is always in that layer. `RFOM_FG_HUDSPLIT=0` turns the split off; `RFOM_FG_HUDDBG=1` shows the HUD-less picture, `2` the separated HUD over grey (works with DLSS on even without frame generation; for checking).
- **Frame rate:** it caps the *rendered* frames; the screen gets 2, 3 or 4 times as many. On a 144 Hz G-Sync / FreeSync screen use about 69 (2x), 46 (3x) or 34 (4x) with VSync Off, so the output stays just under the refresh rate. VSync 1/2 or 1/3 is not supported with frame generation and uses normal VSync.
- Log: `[DLSS-G] Streamline ready ...`, `[DLSS-G] frame generation on: 3x, 1920x1080, HUD kept out of generated frames`, and every 600 frames a status line with the frames presented per rendered frame and how many frames had the HUD separated. `RFOM_FG_SLLOG=1` adds Streamline's own log.
- **Frame generation diagnostics** (DLSS-G and FSR 3; only the game program, the front end has no frame generation): `RFOM_FG_DIAG=1` writes a `[fg-diag]` line per second to `logs\run.log` (real presents, frame-time histogram, FG on / off / resets with their reasons, frames shown, camera solved / held / unsolved, motion-vector match counts, HUD split, Reflex sleep), and **Pause** (`RFOM_FG_DIAG_KEY=scroll|f1..f24|none`) or **Ctrl+Shift+F2** appends the previous 30 and next 120 presents to `logs\fgdiag.txt`; `2` also does that by itself after a reset, FG off, a spike or frames Streamline did not present (`RFOM_FG_DIAG_FRAMES`, `RFOM_FG_DIAG_AUTO_MAX`); `3` records **every real frame** to `logs\fgdiag.txt` until exit (one row each: frame index, upscaler frame as in `[FSR] frame N` / `[FG-HUD]` / `[FG-MV]`, present interval, CPU time, Present call, Reflex sleep, backend, on, frames per rendered frame, frames Streamline presented, DLSS-G status, reset / off reasons, camera state with spread and motion, exact / instance / nearest / camera-split / ambiguous / unmatched / common-kept matches, HUD split runs and draws), with `--- event ---` lines (resets with their reason, on / off, generated frames not presented, status changes, spikes) and `--- user marker --- #n` for each Pause / Ctrl+Shift+F2 press (also in `run.log`). The file is written by its own thread every 250 ms, stops at `RFOM_FG_DIAG_MAX_MB` (512), keeps the previous session as `fgdiag_prev.txt` (if that rename fails, e.g. the file is open in an editor, the old file is kept and the new one is `fgdiag_<pid>.txt`), continues across the level-load program switches, and is completed on exit. Off (unset / `0`) costs nothing.
- **Frame generation ghosting fixes** (ported from the Resistance 2 port). `RFOM_DLSS_FG_HUD_HOLD` (default 1, 0 = off): a frame whose HUD could not be separated (classifier unsure, HUD run closed by a scene draw, no HUD draws) keeps the HUD area of the last separated frame (under 2 s old) as UI: that part of the real frame goes to the UI layer, so the HUD baked into it is not warped with the scene, and with DLSS-G the motion vectors under it are zeroed (FSR 3 gets the UI part only). `RFOM_DLSS_FG_HUD_DEBUG=1` shows the held area in yellow; `RFOM_FG_DIAG=3` rows show `H` in the hud column. `RFOM_DLSS_FG_FX_MV` (experimental A/B, default 0 = as rendered): `1` gives pixels covered by blended effects (smoke, sparks, muzzle flashes; the reactive mask) camera motion only, `2` no motion -- frame generation's copy only, the upscaler keeps the original. `RFOM_DLSS_FG_STATS=1` (or `RFOM_FG_DEBUG=1`) logs two `[fg-stats]` lines every 10 s (presents, FG on, resets, scene tags per present, HUD split and why not, hold use, GPU-read motion statistics); `RFOM_DLSS_FG_MV_DEBUG=1` (or `RFOM_FG_DEBUG=2`) tints the picture with the motion vectors frame generation gets (green object motion, blue camera only, magenta invalid). R2's camera-cut sanity check (`R2_FG_CAM_SANITY`) is not needed here: R1 detects cuts from the solved camera, not from a per-draw depth-range vote, and gameplay logs show 0 cut resets. `RFOM_DLSS_FG_CAM_RESET=0|1|2` (**default `1`**; `0` = old behaviour; R2's `R2_DLSS_FG_CAM_RESET`, adapted to R1's camera solve): on a frame whose camera was not solved — the camera candidates disagreed and frame generation got no camera motion, or the opt-in `RFOM_DLSS_CAM_PLURALITY` guess — while the camera moves (the guess's own motion, whose near-zero motion means a still camera; else the last solved camera's motion, for up to 8 frames) by 1 render pixel or more, frame generation (DLSS-G and FSR 3) resets instead of blending the two frames with the wrong camera motion (the whole scene doubled while turning); the upscaler keeps its history, and the cost is one frame without a generated in-between. `2` also resets on frames that reused the last solved camera (`RFOM_DLSS_FG_CAM_HOLD`). With stats logging on, the `[DLSS] camera/600: frame generation reset on N moving frames without a solved camera` line counts them.

### FSR 3 Frame Generation (fallback)

On GPUs without DLSS Frame Generation (RTX 20 / 30, AMD, Intel) the **FSR 3 2x** choice (`RFOM_DLSS_FG=fsr3`) uses AMD FSR 3 frame generation (FidelityFX SDK 2.3.0, open source, MIT): one generated frame per rendered frame. `RFOM_FG_BACKEND` picks the backend: `auto` (default: DLSS-G when it can run, otherwise FSR 3 -- the DLSS choices fall back to FSR 3 2x with a `[FG] DLSS Frame Generation unavailable -- trying AMD FSR 3 frame generation (2x)` line), `dlss` (DLSS-G only) or `fsr3` (FSR 3 only, also on RTX 40 / 50).

- It needs any temporal anti-aliasing / upscaler (**FSR 3.1, NVIDIA DLSS or Intel XeSS**; their depth, motion vectors, jitter and resets are reused) and `amd_fidelityfx_framegeneration_dx12.dll` next to the game (Setup downloads it into `tools\ffx_bin`, ~40 MB, pinned by SHA-256; `build.bat` copies it). It is off, with a `[FSR3-FG] frame generation disabled: ...` line, without them, with scRGB HDR, in the front-end program, or if the GPU / driver cannot create it.
- Same rules as DLSS-G: off for frames without a 3D scene, in the background, minimised and while resizing; camera cuts reset it; the frame rate limit caps rendered frames (144 Hz G-Sync: about 69). Reflex stays as set in its own option.
- The HUD split is the same: FSR 3 interpolates the HUD-less picture and the HUD + overlay layer is drawn over each generated frame; the rendered frames are shown exactly as drawn. `RFOM_FG_HUDSPLIT` / `RFOM_FG_HUDDBG` work the same; `RFOM_FG_FSR3DBG=1` adds FSR 3's own reset / tear indicators.
- Log: `[FSR3-FG] AMD FSR 3 frame generation ready ...`, `[FSR3-FG] frame generation on: 2x, 1920x1080, HUD kept out of generated frames`, and every 6000 presents a count of generated frames and HUD-split frames. `RFOM_FG_DIAG` (see *DLSS Frame Generation*) works the same.

## Performance overlay

**F3** toggles a small overlay in a corner of the game window: the game's frame rate and frame time (with the worst frame of the last half second), the renderer's frame rate, draws per frame, and the CPU time per draw in µs (the same measurement as `RSX_DRAWPROF`, refreshed twice a second; measuring costs a little while the overlay is up and nothing while it is hidden). `RFOM_OVERLAY=1` shows it from the start, `RFOM_OVERLAY=0` disables F3; `RFOM_OVERLAY_POS=tl|tr|bl|br` picks the corner. With NVIDIA Reflex active it also shows the driver's PC latency (simulation start to present, render submit to present, GPU time; average of the last 64 frames).

The overlay and the port's notifications (toasts: "Screenshot saved", the movie-skip hint, trophies) are drawn by the renderer into the game's own image, after the F12 screenshot copy, so they show in every display mode (and in recordings of the game window) but never in F12 screenshots. They are pre-drawn once per change and cost nothing measurable. Toasts sit in the top corner opposite the overlay for 5 s, queue while the game is in the background, and use the same style as the Resistance 2 / 3 ports (dark panel, coloured letter badge); achievement / trophy pop-ups use the PS3-style look (see *Achievements*). `RFOM_OSD=window` uses the old separate always-on-top windows instead (also the automatic fallback if the renderer can't set the pass up; the log then says `[osd] in-swapchain notifications unavailable, using overlay windows`, otherwise `[osd] in-swapchain notifications ready`). The start-up "Compiling shaders" panel is always a separate window (nothing is presented while it shows).

## Other options

| Variable | Default | What it does |
|---|---|---|
| `RSX_LOD_BIAS` | `auto` | Texture mip LOD bias, -2…2. `auto` = log2 of the upscaling ratio when FSR / DLSS upscaling is on (-1 at Performance), so textures keep output-resolution detail; 0 otherwise. |
| `RSX_STRIP_CARRY` | `1` | A triangle strip / fan / quad list split over several 256-vertex draw batches is assembled as one stream, as on PS3 (stops small holes at the seams). A vertex-base or vertex-constant write between batches ends the stream, as on hardware. `0` = old per-batch assembly. Log: `[STRIPCARRY]`. |
| `RSX_QUAD_STRIP` | `1` | Quad strips (and polygons, drawn as fans) go through the normal vertex-program path. `0` = skip them as before. Log: `[QUADSTRIP]`. |
| `RSX_POINT_SPRITE` | `1` | Point-list draws (particles) are expanded to camera-facing squares of the game's point size, with sprite texture coordinates on the units the game selects (point sprite control). `0` = skip point draws as before. Log: `[PSPRITE]`, `[VSX]`. |
| `RSX_CLIP_PLANES` | `1` | The game's user clip planes (up to 6, from the vertex program's clip outputs) cut geometry as on PS3, e.g. for water reflections. `0` = ignore them. Log: `[VSX]`. |
| `RSX_A2C` | `1` | Alpha-to-coverage (foliage, fences, hair) is drawn as an alpha test at 0.5 when the game enables it (replacing no test or a looser one), so cut-out edges are not drawn as solid blocks. `0` = ignore it. Log: `[A2C]`. |
| `RSX_GUEST_SAMPLER` | `1` | Each texture uses the game's own filtering, anisotropy, LOD bias and min / max mip level (instead of one shared trilinear sampler). `RSX_ANISO` and `RSX_LOD_BIAS` still apply on top. `0` = shared samplers as before. Log: `[GSAMP]`. |
| `RSX_GUEST_SAMPLER_ADDR` | `0` | `1` = also take the wrap / clamp / mirror mode from the game's sampler (by default the shaders handle it). |
| `RSX_MRT_STATE` | `1` | With several render targets, targets 2-4 use the game's own per-target colour write mask and blend on / off (instead of copying target 1's). `0` = copy target 1 as before. |
| `RSX_BLEND_CONST` | `1` | The game's blend colour is used by blend modes that refer to a constant colour (8-bit and FP16 targets). `0` = constant stays 0 as before. |
| `RSX_LOGIC_OP` | `1` | Colour logic ops CLEAR, NOOP and COPY are reproduced with blend state (D3D12 logic ops need integer targets, which the game does not use). Other ops keep the normal blend and log once. `0` = ignore logic ops. Log: `[EXT]`. |
| `RSX_DEPTH_BOUNDS` | `1` | The depth bounds test (light volumes) runs on GPUs that support it. `0` = ignore it. Log: `[EXT]`. |
| `RSX_STENCIL_BACK_MASK` | `1` | Two-sided stencil uses separate back-face read / write masks on GPUs with independent front / back masks (otherwise the front masks, logged once). Back-face stencil functions and ops are always honoured. `0` = front masks for both. Log: `[STENCIL]`. |
| `RSX_SCISSOR` | `1` | The game's scissor rectangle clips drawing (intersected with the viewport). `0` = viewport rectangle only, as before. |
| `RSX_CLEAR_SCISSOR` | `1` | Colour / depth clears of an off-screen render target only clear the game's scissor rectangle (moved by the window offset), as on PS3. Split-screen co-op clears each player's half separately; clearing the whole target wiped player 1's view (issue #13). `0` = clears cover the whole target, as before. |
| `RSX_POLY_MODE` | `1` | Polygons the game draws in line mode are drawn as wireframe (point mode is drawn filled). `0` = always filled. |
| `RSX_DEPTH_CLIP` | `1` | Depth clipping follows the game's depth clip / clamp control (near and far planes clip unless the game asks for clamping). `0` = always clamp depth, as before. |
| `RSX_DEPTH_RANGE` | `1` | The game's depth range (clip min / max) is honoured. `0` = always 0 to 1, as before. |
| `RSX_TWO_SIDE_LIGHT` | `1` | With two-sided lighting on, back-facing polygons use the vertex program's back colours. `0` = front colours on both sides. Log: `[VSX]`. |
| `RSX_FLAT_SHADE` | `1` | Flat shading mode draws colours without interpolation (D3D takes the first vertex's colour where PS3 takes the last). `0` = always smooth. |
| `RSX_FOG` | `1` | The fragment program's fog input is computed from the game's fog mode and parameters (linear, exp, exp2 and their abs forms), as on PS3. `0` = raw fog coordinate as before. |
| `RSX_BORDER_COLOR` | `1` | Textures in border address mode return the game's border colour outside the texture, per axis (U=wrap, V=border borders V only). Mirror-once-border mirrors first and borders past 1. Plain 2D samples are patched in the shader unless the honoured guest sampler (`RSX_GUEST_SAMPLER_ADDR=1`) already applies it (plain border mode). `0` = clamp to the edge as before. |
| `RSX_BC_UNALIGNED` | `1` | DXT (BC1/2/3) textures whose width or height is not a multiple of 4 (many HUD and menu pieces) are decompressed to RGBA8 on upload. D3D12 can only create such BC textures when the device reports unaligned block textures (Windows 11 runtimes); without that, creation fails and the HUD / menu highlight is missing (Windows 10). `2` = decompress only when the device lacks the support, `0` = old path (always create as BC). The log's `[EXT]` line shows the device support and `[BCALIGN]` the first such texture. While they are decompressed, texture packs (`RSX_TEXDUMP` / `RSX_TEXPACK`) see them as colour textures (dumped as `.png`, with a different key than the `.dds` an older version dumped). |
| `RSX_ALPHA_FACTOR` | `1` | Blend CONSTANT_ALPHA uses the device's alpha blend factor (D3D12 OPTIONS13) when supported; otherwise the blend constant is splatted to its alpha when only CONSTANT_ALPHA is used. `0` = never use the device alpha factor (splat fallback). The pipeline manifest records the device caps (alpha factor, back-face stencil masks, depth bounds); a manifest from other caps (GPU/driver swap, toggling this) is discarded and re-recorded. |
| `RSX_LINE_AA` | `1` | Line smoothing turns on antialiased lines. `0` = aliased lines. |
| `RSX_FP_EXTOPS` | `1` | Fragment-program ops DDX, DDY, TXD, DST, LIT, STR, SFL, BEM, DP2A, RFL, TEXBEM and TXPBEM are translated as RPCS3 does (derivatives in game pixels with the game's window Y direction). `0` = these ops are dropped as before. |
| `RSX_FP_RFL` | `0` | Reflection op RFL: `0` = the NVIDIA formula `2*dot(a,b)/dot(a,a)*a - b` (axis need not be unit length). `1` = RPCS3's `reflect(a, b)`. |
| `RSX_WINDOW_OFFSET` | `1` | The game's window offset moves the viewport and scissor inside the render target, as on PS3. `0` = ignored as before. |
| `RSX_MIRROR` | `1` | Textures in mirror-once address modes are mirrored in the shader (D3D12 has no mirror-once-border mode; mirror-once-border axes always mirror, as part of the border path). `0` = no mirror rewrite for the other mirror-once modes. |
| `RSX_FEATCOV` | `0` | Graphics feature trace (diagnostic, off by default; `1` = on): every RSX method, render state value and shader op the game uses is listed once in `run.log` as `[featcov]` lines, each marked with what this renderer does with it (real / noop / ignored / unhandled). Summarise with `python scripts\featcov_report.py run.log`. Costs about 2.5 ns per method write when on. |
| `RSX_FEATCOV_SEC` | `60` | Seconds between `[featcov]` dumps (only newly seen items); a final dump is always written at exit. `0` = only at exit. |
| `PORT_DLSS_RESET_CUTS` | `0` | `1` = also reset the DLSS / FSR / XeSS history when the render or output size changes. |
| `AUDIO_P1_DECOMB` | `1` | Movie audio de-comb: the game copies the stereo movie track to the side surrounds about 4.6 ms late, and folding that back into stereo sounds hollow ("tin can"). When the sides are detected to be the delayed fronts they are faded out of the stereo fold (loudness compensated). `0` = old fold. Log: `[audio] port 1 de-comb on`. |
| `AUDIO_P1_DECOMB_LAG` | `222` | Delay in samples (48 kHz) that the de-comb looks for. |
| `RFOM_FOV_ANYCAM` | off | `1` = zoom-matched mouse speed follows any camera's FOV (old behaviour); by default only the player's view camera counts. |
| `RFOM_ZOOM_SENS` | `1.0` | Zoom-matched mouse speed when aiming / scoped: 1 = a mouse move covers the same part of the screen zoomed or not, 0 = off, up to 2. |
| `RFOM_KEY_PROMPTS` | `1` | `0` = no keyboard / Xbox prompt text (PlayStation prompts always). |
| `RFOM_PC_TEXT` | `1` | PC wording in the game's text, all languages: "PLAYSTATION®3" → "PC", "PLAYSTATION®Network" → "online service", HDD / save-icon / power-off warnings and "press and hold the PS button" → PC wording ("Press F10 to quit the game."), "wireless controller" → "controller". Only the text in memory changes (the game files aren't touched). Log: `[prompts] text file: N console wording(s) -> PC wording`. `0` = the original console wording. |
| `RFOM_SPEAKERS` | auto | Speaker layout: 6 = 5.1, 8 = 7.1, 2 = the game's stereo layout. Unset: 8 when the host output is 7.1, else 6. |
| `PORT_AUDIO_CHANNELS` | auto | Host output channels: 2 / 6 / 8 (`stereo` / `5.1` / `7.1`). Unset: the Windows default device's mix format; a 5.1 / 7.1 device gets every PS3 channel on its own speaker (L R C LFE, Ls/Rs on the side pair, Lb/Rb on the back pair). The `[audio] host output:` log line shows what was opened. |
| `PORT_AUDIO_SWAP_SURROUND` | `0` | `1`: PS3 Ls/Rs to the back speakers and Lb/Rb to the sides (if 7.1 surrounds sound swapped). |
| `RFOM_GORE` | `1` | Full gore (as the US release); `0` = the console setting, `-1` = reduced. |
| `PS3_MUTE`, `PS3_NO_AUDIO` | off | Mute / disable audio. |

Not included, and why: MSAA (the renderer's render targets are shared with PS3-format readbacks and post-process passes that expect single-sample surfaces; FSR / DLSS anti-aliasing covers it); a higher-precision depth buffer (the game's depth is read back in PS3's 24-bit format by its own effects; changing it risks shadow and depth-test artefacts).

## Mods

`RFOM_MODS=1` (launcher: Advanced → Mods folder → On) loads replacement game files from the `mods` folder in the port's folder (`RFOM_MODS=<folder>` uses any other folder):

```
mods  My Texture Mod    USRDIR\<path of the file inside the game's USRDIR>
  Another Mod    USRDIR\...
```

- A file replaces the game file with the same path (case does not matter). Every file access of the game goes through the check (cellFs, sys_fs and the port's own loaders), and only files you add are changed; nothing in `gamefiles` is touched.
- Mods load in alphabetical order; when two mods have the same file, the later one wins (the log says so). A folder starting with `_` or `-` is skipped (an easy way to turn a mod off).
- **Load order:** `mods\order.txt` (one folder name per line, `-Name` = off, `#` comments) sets the order and turns mods on or off without renaming folders; folders it does not list follow alphabetically. The launcher writes it: **Saves & tools → Mods** lists every mod with Up / Down / Turn on/off buttons.
- Log: `[mods] <mod>: N file(s)`, `[mods] override: usrdir/... -> <file>`, `[mods] N mod(s), M file override(s) from <folder>` at start, and `[mods] using <file>` the first time the game opens each replaced file.

### Texture packs

Textures can also be replaced one by one, without touching the game's archives:

1. `RSX_TEXDUMP=1` (launcher: Advanced → Texture packs → Dump to texdump) writes every texture the game draws with, once, to `texdump\` (`RSX_TEXDUMP=<folder>` for another folder). The name is `<16-digit key>_<width>x<height>_<format>_m<mips>` and each texture is listed in `texdump\manifest.tsv`; the key is a hash of the texture's content (all mip levels) plus size, format and mip count, so it is the same on every run (not a memory address); colour textures are saved as `.png`, DXT1/3/5 textures as `.dds` with their mip levels. Dumping slows loading; turn it off when done.
2. Edit the files you want and copy them to `mods\textures\` (sub-folders are fine; only the leading 16-digit hash of the name matters).
3. `RSX_TEXPACK=1` (Advanced → Texture packs → Load mods\textures; `RSX_TEXPACK=<folder>` for another folder) loads them. The log says `[texpack] N replacement textures in ...` at start and `[texpack] replaced ...` as they are used.

A replacement must have the same width and height as the original; a `.dds` must also keep its DXT format (DXT2/DXT4 are accepted for DXT3/DXT5). A `.png` replaces a colour (A8R8G8B8) texture, and its mip levels are generated; a `.dds` brings its own levels (missing ones keep the game's). A file that does not match is skipped with a log line. Cube maps are not dumped or replaced. With both options off the upload path is unchanged.

### HD UI pack

- **HD UI pack** (`RSX_HD_UI=1`; experimental, off until you make a pack): AI-upscaled menu / HUD textures, made on your PC from your own game (nothing game-derived ships with the port). **One click:** launcher › Saves & tools › **Create HD UI pack**. It runs `HDUIPack.exe` (built with the port by `build.bat`, nothing to install): it reads the menu / HUD textures straight from the game's archive (`USRDIR\game.psarc`: the menu texture banks, GUI sheets, HUD art, fonts, level maps and loading splashes), upscales every texture with Real-ESRGAN on the graphics card (`tools\hdui_upscaler\`, comes with the port: Real-ESRGAN-ncnn-vulkan built against ncnn 20261020, SHA-256 checked before every run, with the official `realesrgan-x4plus` model, taken from Real-ESRGAN's release zip, downloaded once (45 MB, SHA-256 pinned); alpha separately, so edges stay clean; textures under 64 px are padded with their edge pixels for the model and cropped back) to 2x..4x the original (4096 px per side at most) and writes DDS files in the game's own format with every mip level to `mods\hd_ui\`. The launcher shows progress (done / total, time left) and can cancel; when it finishes it turns HD UI pack on and shows a summary. A full pack (~770 textures) takes a few minutes on a recent graphics card (about 30 minutes on a 6-core CPU if it falls back to the CPU); running it again continues where it stopped (finished textures are kept, also after a cancel) and **Update HD UI pack** only makes what is missing. Every upscaled texture is compared with the original (reduced back to its size); one that does not match (damaged output) is not used: from the graphics card it is made again on the CPU, and if the CPU's is unlike it too the game keeps the original texture and `pack.tsv` notes it as `refused` so later runs leave it alone (`HDUIPack.exe --force` tries again). Textures an earlier version made with another model before this check are made again by the next Update. **Remove HD UI pack** deletes `mods\hd_ui\` and turns the option off (the launcher waits for the deletion to finish; anything an ended removal left in `mods\.hd_ui.removed` is deleted by the next run). Without a usable graphics card it runs on the CPU (see *GPU safety*). How it matches the game: the key the renderer's texture upload computes (a 64-bit hash of the texel data as uploaded, size, format and mip count) is computed by the tool from the same archive bytes with the same code (`ps3recomp/libs/video/r1_hd_ui.h`), so no dumping or playing is needed. The pack is built beside it in `mods\.hd_ui.stage` (a dot folder, which nothing loads; on the same drive, so `--out` and `--tools` may be on different drives) and renamed into place, so an interrupted run never leaves scratch files where the game loads replacements; if a run is cancelled between the two renames, the next run puts the new (or else the previous) pack back, and while a file is held open (antivirus, Explorer) it keeps both and says so instead of deleting anything. In game the log says `[texpack] HD UI pack: N texture(s) in mods\hd_ui` at start and `[texpack] HD UI pack: N texture upload(s) replaced so far` as they are used. A replacement is 2x..8x the game texture (same factor on both axes, 8192 px per side at most, whole 4x4 blocks for DXT) and is sampled over exactly the same screen area, so layouts do not change; files that do not match (size, format, missing mips) are skipped with a log line and the game's texture is used. Replaced textures are only ever sampled (never render targets or copy sources). `RSX_HD_UI=<folder>` uses another folder; `RSX_HD_UI_CACHE_MB` (default 512) caps the replacement files kept in memory. With the option off nothing is scanned or hashed. **GPU safety:** the AI upscaler runs on the graphics card (Vulkan) in **one** upscaler run for all textures, with the upscaler's own tile size and job counts, the way the Bloodborne PC port (Lantern Engine) runs the same Real-ESRGAN with the same model, which works on an RTX 5070 (earlier versions started the upscaler hundreds of times with tiny tiles and the remacri model; on an RTX 5070 that ended in NVIDIA driver resets, nvlddmkm 13 / 153, and a PC restart). If anything goes wrong on the graphics card (the upscaler's own stop at the first failed GPU submission, `GPU_STOP`; a lost device, `vkQueueSubmit failed -4`, seen by the watchdog; 90 s without progress; a crash or no output) the upscaler is ended at once, so nothing more is sent to the graphics card; the textures it already made are kept and the rest are made on the CPU (`tools\hdui_upscaler\bin\realesrgan-ncnn-cpu.exe`, which contains no Vulkan code at all, with the bundled `fast` model, at below-normal priority on about half the CPU threads). The failure is recorded in `tools\upscaler\gpu_fault.txt` and later runs use the CPU until you delete that file (e.g. after a driver update) or run `HDUIPack.exe --use-gpu` (when that run makes everything on the graphics card, the file is removed). When no usable Vulkan graphics card is found or the GPU upscaler does not start (no fault), that is recorded in `tools\upscaler\gpu_unavailable.txt`: later runs keep the textures the CPU made with `fast` instead of making the whole pack again, and each **Update** first runs one small test image on the graphics card; once it works (e.g. after a driver install) the file is removed and those textures are made again with `x4plus`. `tools\upscaler\upscaler_running.txt` exists only while the GPU upscaler runs; if it is still there after a reboot and Windows logged an unexpected shutdown at its next start (Kernel-Power 41 / EventLog 6008 within minutes of the first start after the run), or Windows' log cannot tell, that counts as a failure too and the marker and the upscaler's log are kept as `tools\upscaler\upscaler_crashed.txt` / `.log`; a normal restart or sign-out does not count. `HDUIPack.exe --cpu` uses only the CPU; `--safe` runs the GPU in conservative mode (`-c`: fp32 storage, no fp16 / int8 / cooperative-matrix / subgroup ops) with `-t 64` tiles and `-j 1:1:1`; `--status` prints where the next run upscales (`@gpu gpu|cpu ...`) and starts nothing. Advanced: `HDUIPack.exe --help` (`--model x4plus` (the default on the graphics card) / `fast` (the default on the CPU) / `remacri` / `ultrasharp` (sharper ESRGAN models for personal use), the larger ones downloaded once into `tools\upscaler\models\` and SHA-256 checked and about 30x slower than `fast` on the CPU; `--use-gpu`, `--cpu`, `--force`, `--safe`, `--status`, `--dry-run`, `--list`, `--gpu N`). Not replaced: cube maps, unnormalized (UN) textures, textures smaller than 8x8, the menu button-glyph atlas (the port draws its own buttons) and a few DXT textures with no 2x..4x size of whole blocks. Keys assume the default mip upload (`RSX_TEX_MIPS` unset). Tests (offline, no game run): `ctest -R hd_ui` in the ps3recomp test build (`hd_ui`: lookup, validation, fallback and a WARP D3D12 upload; `hd_ui_pack_*`: DDS writer, atomic pack folder, pinned upscaler files (and a test image through the real CPU-only build), GPU safety (command line, GPU or CPU after a fault, a crash's evidence kept, watchdog on a stand-in upscaler, padding of small textures, the single GPU run handing the rest to the CPU after a lost device; the Vulkan upscaler is never started) and, with the game files present, every key of the archive against the renderer's key code) and `python ps3recomp\tools\test_hd_ui_keys.py`. The `x4plus` and bundled `fast` models are BSD-3-Clause; the optional remacri / UltraSharp models are for personal use (CC BY-NC-SA); do not share the generated pack.

## Trophies

Resistance: Fall of Man (2006) has no trophies: the game does not use the trophy library and its disc has no trophy data, so on the retail game nothing is ever unlocked. The launcher has fan-made **Achievements** instead (below).

The port still carries the same trophy support as the Resistance 2 / 3 ports, for a build or mod that does call the trophy library (`sceNpTrophyUnlockTrophy`): each unlock is logged (`[trophy] unlocked #N <name> (<grade>)`), recorded in `%USERPROFILE%\Saved Games\Resistance Fall of Man\trophies.json` (same format as R2 / R3; names and grades come from `gamefiles\PS3_GAME\TROPDIR\<id>_00\TROPHY.TRP` when present), and shown as a PS3-style pop-up ("You have earned a trophy.", a cup in the grade's colour, the name) drawn inside the game image (see *Performance overlay*), so it also shows in fullscreen. A trophy already in that record is never announced twice (`[trophy] #N already earned ...`), and the platinum is awarded when every trophy counting towards it is unlocked. `RFOM_TROPHY_TOAST=0` turns the toast off (unlocks are still recorded).

### Achievements (unofficial, fan-made)

The launcher's **Achievements** page (page 8, between Advanced and Saves & tools) shows 16 achievements made up for the port and labelled *Unofficial · fan-made*. They are not part of the game. In game, the port reads the profile save whenever the game writes it and pops up each one the first time it shows up as earned (the ones already announced are listed in `%USERPROFILE%\Saved Games\Resistance Fall of Man\achievements_announced.txt`; on the first run with this, what the save already has is taken as announced). (PS3-style pop-up: a dark rounded box near the top corner (opposite the F3 overlay) that slides in for about 6 s, with a cup in the grade's colour at the left, "You have earned an achievement." and the name below it (scaled to the window height; drawn into the game image, so it also shows in fullscreen, HDR included). Several unlocks queue. `PORT_TROPHY_STYLE=old` (or `RFOM_TROPHY_STYLE=old`; launcher *Achievement pop-up style: Classic*) brings back the older notification with the grade badge and description. `PORT_TROPHY_TEST=1` shows one sample pop-up per grade a few seconds after start-up, to check the look. `RFOM_TROPHY_TOAST=0` turns the pop-ups off.) The page computes them when you open it from your profile save, `...\savedata\BCUS98107_PROFILE\PROFILE.SAV` (the folder the *Save location* setting picks; `PS3_GAMEDATA_ROOT` wins; a save not yet moved to Saved Games is read from `gamedata`). The file is only read, never written. Like the R2 / R3 Trophies pages there is a summary (earned / total, %, per grade) and a grade per achievement (P / G / S / B), with progress such as *7 / 20* for the counted ones. The save has no dates, so none are shown; the page shows when the save was last written.

| Achievement | Grade | Rule (from PROFILE.SAV) |
|---|---|---|
| First Mark, Sharpshooter, Veteran, Sentinel | B, B, S, S | 1 / 10 / 20 / 30 skill points |
| Perfectionist | G | all 43 skill points (the game's own *all skill points* rule) |
| Point Collector, High Scorer | B, S | 25 / 75 of the 126 skill-point points |
| Into the Fray, Pushing Back, Holding the Line, Liberator | B, B, S, G | 1 / 4 / 7 / 10 of the 11 campaign chapters finished (either campaign record) |
| Superhuman Ready, Superhuman | S, G | the game's *Superhuman unlocked* / *Superhuman completed* flags |
| Black Ops, Death's Head | S, S | the game's *Black Ops skin* / *Skull skin unlocked* flags |
| Fall of the Chimera | P | every other achievement |

Only what the save records reliably is used (decoded from the game's code: the skill point bit array, the point values and the per-chapter record). Not used: the skill point names (they are localised text), which difficulty a chapter was finished on (the save stores a number per chapter, but the mapping to Casual / Normal / Hard / Superhuman isn't confirmed), the journal / intel, and weapons. Before you have a save it says *Play the game to start earning achievements*.

## Saves

- Saves live in **`%USERPROFILE%\Saved Games\Resistance Fall of Man`** by default, so updating or reinstalling the port can't touch them. Your save files are in `...\dev_hdd0\home\00000001\savedata\` (`BCUS98107_*` folders).
- On the first start after this change, existing saves in the port's `gamedata` folder are **copied** there (nothing is overwritten; the old copy stays as a backup). Log: `[saves] folder: ...` and `[saves] copied N save file(s) ...`.
- `RFOM_SAVE_DIR=portable` (launcher: Save location → Game folder, or a `portable.txt` file next to `Launcher.exe`) keeps everything in `<port>\gamedata`; `RFOM_SAVE_DIR=<folder>` uses any folder; `PS3_GAMEDATA_ROOT` (advanced) overrides both.
- Launcher → **Open save folder** opens it. The game's own options are stored in the save profile, so they move with it.
- **Per-save backups (automatic):** just before the game overwrites a save, the previous version of that save folder is copied to `saves_backup\<save folder>\<date>_<time>\` (UTC time; the newest backup always sorts last, even after a clock change) (next to `gamedata`; inside the save folder you chose when the saves live elsewhere), keeping the newest 5 per save. Unchanged saves and saves over 64 MB are not copied; a failure only logs one `[savebackup]` line and the game saves as normal. `PORT_SAVE_BACKUP=0` turns this off, `PORT_SAVE_BACKUP_KEEP=<n>` (1-100) keeps n. This is separate from the launcher's whole-folder backups in `save_backups` (made when you press Play).
  **To restore one:** close the game, open `saves_backup\<save folder>\`, pick the `<date>_<time>` folder you want and copy its files into `...\dev_hdd0\home\00000001\savedata\<save folder>\` (replace the files), then start the game.
- **Import from RPCS3:** launcher → Saves & tools → *Import saves from RPCS3*, then pick your RPCS3 folder (or its `dev_hdd0` / `savedata` folder). Every Fall of Man save (any region's title ID, checked by its `PARAM.SFO`) is copied in; another region's save is renamed to this copy's title ID. Your saves are backed up first (`before-import`).

## Shader precompile and rebuilding the shader cache

Shader caches are never shipped with the port: they are built on your PC, from the shaders your game uses, the first time each one is seen. The renderer lists every pipeline (PSO) it builds in `shadercache\pso_manifest_fe2.bin` (front end) and `pso_manifest_game2.bin` (game) and keeps the shaders as `.hlsl` source and compiled `.dxbc`. Launcher, Performance page, **SHADER CACHE**:

- **Compile shaders before playing** (default On, `PORT_SHADER_PRELOAD=1`; `RSX_PSO_PRECOMPILE=0/1`, when set, still wins): right after the D3D12 device is created and before the first frame, every pipeline in the program's manifest is built, in parallel, and the game waits for it, so nothing seen before compiles during play. Each program (front end, game) does this for its own manifest. Progress: a centred **Compiling shaders N / M** panel with a progress bar over the game window (shown after 0.3 s) and log lines `[PSOWARM] shader precompile: building all N pipelines ... on T threads`, one every 10 %, then the `[PSOWARM] N pipelines pre-built in ... ms` summary. A `.dxbc` missing from `shadercache` is compiled again from its `.hlsl` (counted as "shaders recompiled from .hlsl"). Off: the pipelines are pre-built on one background thread while you play, as before (`RSX_PSO_PREWARM`). The launcher's **Compile shaders before playing** sets `PORT_SHADER_PRELOAD` (the same switch in all three Resistance ports, default on; `off`/`0`/`false`/`no` turns it off). Every start logs `[shaders] loaded N cached pipelines in X ms (...)` once the cached pipelines are built (with the option off, `... (background)` when the background build ends). Esc, Space, Enter or a click on the game window skips the rest of the precompile (they are built by the background thread during play; closing the window quits cleanly), and `PORT_SHADER_PRELOAD_BUDGET=<seconds>` (old name `RSX_PSO_PRECOMPILE_BUDGET`, used when the new one is unset or not a number; default 0 = no limit, negative counts as 0) stops it after that long, as if skipped: `[PSOWARM] shader precompile: <why> at n/N; ...`.
- **Pipeline library** (`RSX_PSO_LIBRARY`, default on; `off` = driver cache only): the built pipelines are also kept in a D3D12 pipeline library, `shadercache\pso_library_fe.bin` (front end) and `pso_library_game.bin` (game), saved 30 s after new pipelines appear and at exit, and loaded at start-up, so the precompile reuses them instead of compiling again (most of its time on a warm start). The file carries a header with the GPU / driver stamp, the port build and a checksum; a file from another GPU, driver or build, a damaged one, or one the driver refuses is ignored (`[shaders] pipeline library: ... starting empty`) and rewritten. Log: `[shaders] pipeline library: <file>, N pipelines (X MB) opened in ... ms` and `[shaders] pipeline library saved: ...`.
- **Shader compile threads** (`RSX_PSO_PRECOMPILE_THREADS`, default Auto = every logical core but one, 1 to 64).
- **Object motion vectors** (launcher page Upscaling & Frame Gen, `RSX_DLSS_OBJMV`, default On): DLSS/FSR get per-object motion for moving characters and objects; Off = camera motion only (try it if moving objects smear or stutter).
- **Offline shader scan:** [shaderscan](https://github.com/miroslavdanutalexandru-alt/shaderscan) (GPL-2.0, its own repository) is downloaded by Setup / `build.bat` into `tools\shaderscan.exe`: `tools\get_shaderscan.ps1` fetches the release pinned in `tools\shaderscan.version` and checks its SHA-256 (offline: skipped; set `SHADERSCAN_EXE=<path>` to use your own build). It is not part of the port's source or Setup, and the launcher works without it. When `shaderscan.exe` is next to `Launcher.exe` (or in `tools\`), Play first runs `shaderscan --check --out shadercache` (after saving the settings: the stamp also records `RSX_RES_SCALE`, `RSX_FP_KSLOTS`, `RSX_ALPHATEST`, `FP_SWAP_RB` and `RSX_FP_DEBUG`, which change the generated shaders); if the cache isn't seeded it runs `shaderscan --game <folder> --out shadercache --backend r1d3d12 --progress`, which translates every shader program in the game files and FXC-compiles the `<key>.dxbc` / `.hlsl` pairs the game uses (about 1-2 minutes the first time; later runs skip existing files; needs `d3dcompiler_47.dll`). A progress panel shows it (Esc or **Skip for now** stops it; the game starts either way); output goes to `logs\shaderscan.log`. **Rebuild shader cache** also deletes `shaderscan.stamp`, so the next Play scans again.
- **Rebuild shader cache** (button, asks first; refused while `ResistanceFoM.exe` or `ResistanceFoMGame.exe` is running) deletes the compiled shaders and pipelines and keeps what tells the game which ones to build. Deleted, from `shadercache\` and from its backup in `%LOCALAPPDATA%\ResistancePort\shadercache` (`run.bat` restores missing files from there):
  - `*.dxbc` that have a matching `.hlsl` (compiled shaders; a `.dxbc` without its source is kept, it could not be rebuilt)
  - `pso_gpu_stamp*.txt` (the GPU and driver they were built for)
  - `pso_library_*.bin` and `.bin.tmp` (the pipeline library: the compiled pipelines)

  Kept: `pso_manifest_*.bin` and `*.hlsl`. The compiled pipelines also live in the graphics driver's own shader cache; the next start rebuilds every shader and pipeline, which also refills that cache.
- **GPU or driver change:** `shadercache\pso_gpu_stamp.txt` records the adapter's vendor, device, subsystem and revision ids, the driver (UMD) version and the GPU name. When they differ at start-up the log says `[PSOWARM] GPU or driver changed since the pipelines were built (was: ..., now: ...)` and the precompile runs before the first frame even with the option off.

## Performance work

Measured in York (about 2,600 draws per frame), DLSS rendering at 1080p with 4K output, on an RTX 5070: **a locked 60 fps at about 5 µs of CPU per draw**. Low-end PCs benefit most from the performance work, because the per-draw cost is CPU time.

Performance fixes have been applied as far as realistically possible, all profile-driven, and carried over from the Resistance 2 port where they fit R1's own Direct3D 12 renderer:

- **Renderer (per draw):** a redundant-state filter (draws that change nothing skip the state re-apply), fragment-constant patch memoisation, pipeline (PSO) reuse in the pre-pass, cached descriptors, a disk shader / pipeline cache with background compilation and a backup in `%LOCALAPPDATA%\ResistancePort\shadercache`.
- **SPU / PPU runtime:** a fast SPU wait loop (short spin, then sleep; the SPU / usleep waits park after a short pause-only spin on every CPU: `PORT_NOSPIN`, default on, `auto` = only below 12 threads, `0` = the old spinning), yielding instead of spinning on CPUs with fewer than 8 threads, priority boosts and core pinning only where there are enough cores, a high-resolution sleep timer (also used for the runtime's 1 ms naps in SPU interrupt/mailbox waits, SPURS job-guard waits and timed mutex locks; `PORT_HIRES_SLEEP=0` = plain `Sleep(1)`), and the network HTTP poll (`cellHttpClientPollConnections`) implemented so the HTTP thread no longer busy-spins.
- **SPU code reached through pointers is translated too** (`tools/spu_code_ptrs.py`, code generation, default on; `SPU_LIFT_CODE_PTRS=0` when running `tools/rfom_regen.py` turns it off): SPU job code named only by an address stored as data (a C++ static-constructor list, a function-pointer table) or built in a register and called indirectly, which would otherwise run in the SPU interpreter. In this game it adds the static constructor at LS `0x4088` of job module `7d5f74861e88`; the regen lists it in `build_regen\spu\jobmods_code_ptrs.txt`.
- **`PORT_NOSPIN=1|auto|0`** — the no-spin wait profile. Default `1` (on for every CPU); `auto` = on only below 12 logical CPUs; `0` (or `off`) = the old spinning waits. In this port it drives the SPU channel wait (spin `PORT_SPU_SPINWAIT_US`, default 20 µs, with pauses only, then park); the R2/R3 dependents `SPURS_TARGETED_WAKE`, `PS3_USLEEP_PARK`, `PORT_SPU_SPINWAIT` and `R2/R3_JOBWAIT_WAKE` do not exist here. Logged as `[nospin] on|off`.
- **`PORT_SPU_SPIN_ADAPT=<percent>`** — adaptive SPU wait spin (adapted from the inFAMOUS 2 PC port, MIT). Each SPU thread tracks how often its pre-sleep spin ends with the channel ready, and skips the spin while that share is below `<percent>`. Every 8th skipped wait spins anyway to measure again. This stops an idle SPU (menus, pause, a sleeping sound engine) from spending its spin window on every wait, which matters most on 4-core CPUs. Default `25`; `0` = spin on every wait as before. Logged as `[spuwait] adaptive spin`.
- **`PORT_CORE_SCARCE=0|1`, `PORT_CORE_SCARCE_BELOW=<n>`** — "few cores" detection: physical cores (SMT siblings counted once) in the process affinity mask, below `<n>` (default `6`) = scarce. Logged as `[cores]`. `0`/`1` forces it.
- **`RSX_SPINWAIT=1|2|4|0`** — how the RSX frame clock idles while the guest has queued no new GPU work. `1` = busy spin until new work or the next vblank (holds a core). `4` = spin `RSX_SPIN_US` (150 µs), then sleep until the FIFO kick event (a game fence poll that finds the queue dry) or a high-resolution `RSX_IDLE_US` (100 µs) timer. This is the frame-clock kick wait from the inFAMOUS 2 PC port; `GCM_FIFO_KICK=0` = timer only. `2` = 2 ms spin + `Sleep(1)`; `0` = `Sleep(4)`. Default: `4` when `PORT_CORE_SCARCE` finds few cores, otherwise `1` as before. Logged as `[rsx] frame clock idle`.
- **PGO in the normal build:** when `pgo\r1.profdata` exists (committed once recorded), `build.bat` optimises the runtime (including the D3D12 renderer), the hand-written and the lifted SPU code of both programs with it, and prints `PGO: optimising with pgo\r1.profdata`. The profile is LLVM's indexed profile format 13, so it needs **LLVM / clang-cl 22 or newer** (Setup updates an older LLVM; `build.bat` stops with a clear message on an older clang). Opt out with `set PORT_PGO=0` or `build.bat nopgo`. A stale profile is fine (changed functions are just built without it). To record it: `powershell -ExecutionPolicy Bypass -File scripts\pgo_build_instrumented.ps1` (instrumented `build_pgo_gen\`), `tools\pgo_train.bat` (play 10–20 minutes: heavy fights, a busy outdoor level, a level load, a checkpoint reload, cutscenes, menus; profiles go to `pgo\raw\`), then `powershell -ExecutionPolicy Bypass -File scripts\pgo_merge_and_build.ps1` (merges into `pgo\r1.profdata` and builds `build_pgo\`; try it with `RFOM_BUILD_DIR=build_pgo`), then commit `pgo\r1.profdata`. `build_cpu.bat pgo-gen|pgo-use` are the build steps on their own.
- **CPU builds:** The normal `build.bat` build compiles the lifted PPU/SPU code for x86-64-v3 (AVX2, FMA, BMI2: Intel 4th gen / AMD Ryzen or newer) and the runtime and hand-written code for x86-64 + SSE4.1 (CMake `PORT_ARCH=mixed`, the default). FMA contraction is off everywhere (`-ffp-contract=off`), so the lifted float results match the PS3's separate multiply and add. At start the exe checks the CPU and, on one without AVX2, shows "This build cannot run on this processor" with the hint to build the variant without AVX2 instead of crashing (on a CPU without SSE4.1, which no build supports, it says the processor is below the minimum instead; `PORT_CPU_CHECK=OFF` drops the check). `build_cpu.bat baseline` builds the variant without AVX (x86-64 + SSE4.1, `PORT_ARCH=baseline`; its start-up check asks for SSE3, SSSE3 and SSE4.1) into `build_baseline\`; setup builds it by itself on a CPU without AVX2 that has SSE4.1, and the launcher then starts it (`PORT_CPU_V3=0`). `build_cpu.bat` (`PORT_ARCH=AVX2`) also builds the runtime and hand-written code for x86-64-v3.
- **Performance overlay:** F3 (see *Performance overlay*).
- **Per-draw profiler:** `RSX_DRAWPROF=1` logs the CPU cost per draw by phase every 5 s:
  `[drawprof] 5.0 s: N draws (... guest draw calls, ... tex binds), D draws/frame, F fps | X us/draw = record (verts, consts, tex binds) + prepass (srv, pso, tex upload) + replay (rt, pso, state, draw)`. The target is under 6 µs per draw.

Performance switches (defaults are the recommended values; set them before `run.bat` or in `settings.bat`):

| Variable | Default | What it does |
|---|---|---|
| `RSX_DRAWPROF` | `0` | `1` = per-draw CPU profile in the log every 5 s. |
| `RSX_DRAW_FILTER` | `1` | Redundant-state filter. Log: `[draw] redundant state filter on`. |
| `RSX_FPK_MEMO` | `1` | Fragment-program constant memoisation. |
| `RSX_SHADERCACHE`, `RSX_PSO_ASYNC` | `1` | Disk shader cache; background pipeline compilation. |
| `SPU_FAST_WAIT` | `1` | Fast SPU wait loop. |
| `SPU_WAIT_YIELD` | auto (on below 8 logical CPUs) | Yield the core on every SPU poll. |
| `PS3_PRIO_BOOST` | auto (on with 8+ logical CPUs) | Raise the priority of the game's busy threads. Log: `[threads] host priority boosts ...`. |
| `RFOM_MAIN_CORE` | auto (on with 12+ logical CPUs) | Pin the main thread (and the RSX FIFO thread, `RFOM_FIFO_CPU`) to their own cores. Log: `[affinity] ...`. |
| `PORT_CPU_PCORES` | on (no-op without E-cores) | Launcher: Advanced > CPU threads > Prefer P-cores. On Intel hybrid CPUs (P + E cores, 12th gen and newer) the main guest thread, raw SPUs, SPURS lanes, the RSX FIFO / render / engine threads (`rsx::thread`, FIFO walker) and the audio mixer get P-core thread CPU sets (`SetThreadSelectedCpuSets`; a thread runs only where its sets and its affinity overlap) and EcoQoS off, as many of them as the usable P-cores allow (about two hot threads per P-core: main and FIFO from 2, render / engine, raw SPU 0, lanes 0-2 and the mixer from 4, raw SPUs 1-3 from 6, lanes 3 and up from 8; fewer than 2 = nothing). Usable = P-cores inside the process affinity, or, while the core keeper hard-pins main / FIFO / raw SPU 0 (12+ logical CPUs), inside the mask it leaves for the other threads; the threads it pins get no CPU sets. Other threads are left to Windows (Resistance 2 / 3 with 4+ usable P-cores: the process default CPU sets are the P-cores too). CPUs whose cores all share one efficiency class (AMD, multi-CCD included, older Intel) are not changed. Default on; `off`/`0`/`false`/`no` turns it off; `PORT_PCORE_HOT=0` is the older name. `PORT_THREAD_PLACEMENT=off` also turns it off. Log: `[cpu] hybrid CPU ...` or `[cpu] P-core placement off (<why>; ...)`, and `pcore-pin` in the `[native]` line. |
| `PORT_LARGE_PAGES` | off | the guest's code, data and heap (the first 512 MB of the guest address space) are committed up front in large pages, locked in RAM for the whole session (512 MB that Windows can never page out). Large pages (2 MB instead of 4 KB) cut TLB misses on the game's scattered memory accesses; the gain is small (a few % at most, CPU-bound scenes only). Windows gives them only to an account with the **Lock pages in memory** right: run `secpol.msc` (Windows Pro / Enterprise; on Home use `ntrights -u %USERNAME% +r SeLockMemoryPrivilege` from the Windows Server 2003 Resource Kit, or a policy editor), Local Policies > User Rights Assignment > **Lock pages in memory** > Add User or Group > your account, then sign out and back in (no administrator rights needed afterwards). Without the right, or when Windows cannot find enough contiguous free memory (more likely the longer the PC has been up), normal pages are used and the `[native]` line says why: `large-pages(PORT_LARGE_PAGES,overridden: no privilege)` (or `not enough contiguous memory`, `not supported`). Used: `large-pages` in the on list. |
| `PORT_THREAD_PRIORITY` | off | Launcher: Advanced > CPU threads > Raise game thread priority. The main and RSX threads run at above-normal priority (only raised, never lowered; any CPU). Default off: on CPUs with 4 cores or fewer a raised spinning thread can starve the others. Each changed thread is logged as `[cpu] <thread> (thread N): ...`. |
| `RFOM_SPU_WORKERS` | unset (3 SPUs, new wake protocol) | SPURS job scheduler. `0`/`1` = the old scheduler exactly. `4`-`6` or `auto` (one per free physical core) = *experimental* extra virtual SPUs for job lists that may run on any SPU. Log: `[spu-mt] ...` once at start. |
| `RFOM_SPU_WAKE` | `1` | Targeted wakes: a job hand-off wakes only its lane, a kick wakes only the scheduler thread (`0` = wake every lane on every kick, the old way). |
| `RFOM_SPU_AFFINITY` | `1` | Ideal-CPU hints spreading the SPU lane threads over the cores the main / FIFO threads do not use (physical cores first, then SMT siblings). |
| `RFOM_SPU_PRIO` | `1` | `1` = the SPURS scheduler thread above normal (with `PS3_PRIO_BOOST`); `2` = the lanes too; `0` = off. |
| `PS3_USLEEP_TIMER_CACHE` | `1` | Reuse a high-resolution timer for short sleeps. |
| `RSX_NATIVE_TEXDEC` | `1` | Native SSE2 texture untile/convert (swizzled textures and linear byte-swap/palette formats), 1.3–3x faster than the reference decode and byte-exact with it. The first 64 decodes and every 512th after are checked against the reference; on a mismatch it switches itself off and uses the reference result. Log: `[native-hot] texdec: SSE2 4x4-block untile/convert engaged ...` (`... MISMATCH ...` on fallback). `0` = reference decode. |
| `RSX_NATIVE_WCCOPY` | `1` | Streaming-store (non-temporal) copy of vertex/index data and vertex constants into the GPU upload heap, with one store fence per batch. Self-tested against `memcpy` on first use (falls back on a mismatch). Log: `[native-hot] wccopy: streaming-store copy into the upload heap engaged ...`. `0` = `memcpy`. |
| `RSX_SRV_CACHE` | `0` | *Opt-in.* Create each texture view once in a CPU descriptor heap and copy it per draw instead of calling `CreateShaderResourceView` again. Not verified on a GPU yet, so off by default. Log: `[native-hot] srvcache: ...`. |
| `RSX_MT_RECORD` | `1` | Render-thread offload for the D3D12 frame prepass: a small worker pool creates the per-draw texture views (`RSX_MT_RECORD_SRV`, default `1`) and computes the per-frame "did this texture change" checksums (`RSX_MT_RECORD_CSUM`, default `1`) while the render thread walks the draws. The render thread decides every view and every re-upload exactly as before, and waits for the pool before it records the draws; the result is identical (WARP pixel-exact test `mt_record`). `RSX_MT_RECORD_THREADS` = worker count (`0`/unset = auto: 2 with 8+ logical CPUs, 1 with 4-7, none below 4). With `RSX_SRV_CACHE=1` the views stay on the render thread. Log: `[native-hot] mt-record: 2 worker threads take SRV creation and texture checksums off the render thread ...`; with `RSX_DRAWPROF=1` a `[drawprof] mt-record per frame: ...` line follows each report. `0` = all on the render thread. |
| `RSX_MT_REPLAY` | `0` | Opt-in (`1`; its gain is not measured in game yet). Records the frame's draws on several command lists: in frames of `RSX_MT_REPLAY_MIN_DRAWS`+ draws (default `512`), each run of draws that needs no render-target switch, copy, clear, blit or barrier is cut into contiguous chunks, and the `RSX_MT_RECORD` workers record the chunks into their own command lists while the render thread records the next one. Every list starts with the full state (root signature, heaps, targets, viewport, PSO, buffers, root arguments), and all lists go to the GPU in draw order in one `ExecuteCommandLists`, so the output is identical (WARP pixel-exact test `mt_record`, including frames that switch targets mid-frame). A worker list that fails to record is recorded again on the render thread; if that fails too it is not submitted (those draws are missing for one frame). After that the replay stays on one list. If one of the render thread's own lists fails to close, the frame cannot be submitted safely: the game logs `[D3D12] batch N not submitted (...) -- fatal` and exits through the crash report (`logs\crash`). Needs the `mt-record` workers; `RT_REBIND` turns it off. Log: `[native-hot] mt-replay: batches of 512+ draws record runs of ...`; with `RSX_DRAWPROF=1` a `[drawprof] mt-replay per batch: ...` line follows each report. `0` (default) = one command list. |
| `RSX_FIFO_GET_DEDUP` | `1` | The FIFO walker publishes the RSX `get` pointer to the game only when the value the game sees differs (as in Resistance 2 / 3). Guest memory stays identical; it drops the redundant stores to the control line the game polls. With `GCM_RATE=1` the `[RATE]` line also counts stores and skipped identical stores. `0` = always store. |
| `RFOM_GPU_TIMING` | `0` | Diagnostic. `1` = every `RFOM_GPU_TIMING_SEC` seconds (default 5) a `[gputime]` line with GPU time per pass category (scene, offscreen, post, motion, upscaler, present blit), the frame's GPU span (first to last timestamp), busy as a % of that span, and the gaps between command lists (GPU idle or unstamped work), plus render-thread recording time and fence / swap-chain waits. Busy near the span = GPU-bound; large gaps = the GPU waits for submits. Read 8 frames late, no GPU sync. |
| `PORT_DEBUG_KEYS` | `0` | Developer diagnostics only. `1` arms the debug hotkeys (Ctrl+F10 render-thread stall test, Ctrl+F12 frame log, Home upscaler A/B modes, Shift+F10 player trace); they do nothing otherwise. The F7 / F11 trace and profile keys of older builds now need `PS3_DEBUG` + `PS3_FTRACE` set by hand (see the comment in `run.bat`); normal play has no debug keys. |

**Native replacements at a glance:** one start-up log line lists which native (hand-written C/C++) replacements are on and which are off, with the switch that controls each, in the same format in all three Resistance ports: `[native] on: texdec(RSX_NATIVE_TEXDEC) wccopy(RSX_NATIVE_WCCOPY) spu-sched(PORT_SPU_SCHED) | off: srv-cache(RSX_SRV_CACHE)`. The module-specific lines (`[native-hot] ...`) follow with the details.

**SPU wake-ups:** `PORT_SPU_SCHED=0`: an SPU job that waits for another job or the PPU to change a shared memory line (lock-line reservation wait: SPURS job queues and locks) is woken by every write that goes through the runtime's coherence path. A write that bypasses it was only noticed on the next 10 ms poll; such waits now re-check the line every `PORT_SPU_SCHED_MS` (default 1, 1–10), so a missed wake costs at most about 1 ms. Waits only end when their condition holds, as before; other channel waits are unchanged. Logged once as `[spu-sched] PORT_SPU_SCHED=1 ...`, and every 10 s as `[spu-sched] 10s: lost-reservation sleeps=... missed wakes=...` (missed wakes = sleeps that timed out with the line already changed). `=0` restores the 10 ms poll. Shown as `spu-sched(PORT_SPU_SCHED)` in the `[native]` line. Offline test: `ps3recomp\runtime\spu\tests\test_spu_sched_wake.c`.

### GoW Ascension adoptions

Switches taken from the MIT-licensed God of War: Ascension recomp (see `THIRD_PARTY_NOTICES.md`). Each can be set back to the old behaviour.

| Variable | Default | Effect |
|---|---|---|
| `GCM_PUT_ORDER` | `2` | Shared with all three ports, after the GoW Ascension recomp (3 freezes in 17 runs → 0 in 41 there). `2`: an SPU DMA PUT into the GCM command buffer writes the block's park/JUMP word last, so the RSX never follows a jump into a half-written block. `1`: ascending order. `0`: the old single `memcpy`. |
| `GCM_PARK_WAIT_MS` | `50` | How long the GCM FIFO walker waits on a self-JUMP park whose word has not been patched yet before following `put` (which skips the parked commands). Raise it (e.g. `2000`) if a slow CPU freezes on a frame label; `50` is the old fixed grace. R2 / R3 use RPCS3's FIFO, which already waits on the park. |
| `SPURS_PRIO_SKIP_SAME` | `1` | `cellSpursSetPriorities` returns early when the priorities are unchanged (GoW: roughly half the watchdog CPU). `0` = always reapply (old behaviour). |
| `PAD_SQUIRCLE` | `4000` | Maps the round host stick onto the PS3's square stick range so diagonals reach the corners (squareness ×1000; `0` = old per-axis mapping). The centre stays exactly 128. |
| `PAD_MERGE` | `0` | `1`: every connected controller drives player 1 (buttons OR'd, each stick follows the controller that deflects it most). Off by default because of local co-op. |
| `RSX_POLY_OFFSET` | `3` | Depth bias. `3`: the constant term goes into the vertex shader's viewport z (bias/16777215 for Z24, /65535 for Z16) and the PSO keeps only the slope term, so small biases are no longer rounded away and there is no PSO per bias value. `1`: the old PSO-only bias. `2`: all in the vertex shader. `0`: off. |
| `RSX_VS_STRICT` | on (off on NVIDIA) | Vertex shaders are compiled with `precise` position math and IEEE strictness, so a depth pre-pass and the colour pass compute the same depth (fixes z-fighting and flicker on matching draws). New shader-cache keys: the first run after changing it recompiles vertex shaders. `0` = off, `1` = forced on. |


## Audio

When the frame rate drops, the game's own audio mixing (on the recompiled PPU/SPU threads) can fall behind real time. The port therefore keeps a buffer of mixed audio between the game and the sound card, as RPCS3 does with its "audio buffering" and "time stretching" options:

- **Buffer:** mixed audio goes into a ring buffer with a target of `PORT_AUDIO_BUFFER_MS` (100 ms by default). The sound card is kept topped up from it, about 30 ms at a time.
- **Time stretching:** when the buffer drops below `PORT_AUDIO_STRETCH_THRESHOLD` % of the target, playback slows down through [SoundTouch](https://www.surina.net/soundtouch/) without changing the pitch, so a slow stretch of frames sounds slightly slower instead of gapping. Tempo returns to 1.0 as the buffer refills.
- **Underrun:** if the buffer still runs dry (a hitch longer than the buffer), the last few milliseconds fade out to silence and fade back in when audio returns. No repeated blocks, no clicks.
- **Mixer thread:** runs at TIME_CRITICAL priority with Windows MMCSS "Pro Audio" scheduling. It also stays off the cores reserved for the main game thread and the RSX thread (`RFOM_MAIN_CPU`, `RFOM_FIFO_CPU`).
- **Late game audio:** when the game's next block of audio has not been written in time, the mixer waits up to `PORT_AUDIO_LATE_WAIT_MS` (20 ms) for it. The game's audio clock slips slightly and the buffer absorbs the slip. While the buffer is below target, the game's audio clock runs 3 % fast (`AUDIO_CATCHUP_PCT`) to refill it.

| Variable | Default | Effect |
|---|---|---|
| `PORT_AUDIO_STRETCH` | `1` | SoundTouch buffer and time stretching. `0` = the previous audio output path, unchanged. |
| `PORT_AUDIO_BUFFER_MS` | `100` (`150` with fewer than 12 logical CPUs) | Target buffer, 20–500 ms. More = fewer underruns, more latency. |
| `PORT_AUDIO_STRETCH_THRESHOLD` | `75` | Stretch below this % of the target buffer. |
| `PORT_AUDIO_STRETCH_MIN` | `0.5` | Slowest tempo while stretching (0.1–1). |
| `PORT_AUDIO_FADE_MS` | `5` | Fade-out / fade-in length on an underrun. |
| `PORT_AUDIO_DEVICE_MS` | `30` | Audio handed to the sound card at once. |
| `PORT_AUDIO_CPU` | – | Pin the mixer thread to one logical CPU; `-1` leaves its affinity alone. |
| `PORT_AUDIO_MMCSS` | `1` | `0` = no MMCSS "Pro Audio" scheduling for the mixer thread. |
| `PORT_AUDIO_LATE_WAIT_MS` | `20` | Longest wait for a late block of game audio (0–40, `0` = off). |
| `AUDIO_CATCHUP_PCT` | `3` | How much faster the game's audio clock runs while the buffer refills (0–20). |
| `PORT_AUDIO_PAUSE_FREEZE` | `1` | While the game is paused (PC menu, window in the background with `RSX_FOCUS_PAUSE`) the game's audio clock stops and the output is fed silence at the same buffer level. `0` = the old behaviour. |
| `PORT_AUDIO_FOCUS_REPRIME` | `1` | `0` = off. On: after the game's audio was held for at least `PORT_AUDIO_REPRIME_HOLD_MS` (default `500`; the F10 PC menu, a focus pause), restart the output stage: the leftover buffer and any slowed tempo are dropped and it re-primes to the target with fresh audio, costing about one buffer of silence. Same switch as in the R2 / R3 ports. |
| `PORT_AUDIO_DRIFT` | `1` | Smooth clock-drift steering: the game's audio clock is nudged (at most `AUDIO_CATCHUP_PCT`, 3 %) so the buffer settles on the target even when the sound card's clock runs slightly fast or slow. The estimate is in the 10 s line (`clock drift +NN ppm`). `0` = the old on/off catch-up. |
| `PORT_AUDIO_ADAPTIVE` | `1` | A 10 s window with an underrun or more than 2 % stretching raises the target buffer by 25 ms, up to `PORT_AUDIO_BUFFER_MAX_MS`; 5 clean minutes step it back down. `0` = fixed target. |
| `PORT_AUDIO_BUFFER_MAX_MS` | `300` | Ceiling for `PORT_AUDIO_ADAPTIVE` (up to 500). |
| `PORT_AUDIO_UNDERRUN_LOG` | off | Diagnostic. `1` = every 10 s: `[audio] underruns N in last 10 s ... session: N underruns | clock drift ...` (also on with `PORT_AUDIO_LOG`). |
| `PORT_AUDIO_ELASTIC` | `0` | Experimental, after the GoW Ascension recomp's `AUDIO_ELASTIC`. `1`: when a block of game audio is late, the mixer waits for it while the buffer stays above 90 % of the target, then plays a short fade to silence *without* skipping the game's block, which plays when it arrives; the time lost is made up later on silent blocks. Replaces the late-block wait. Log: `[audio] elastic 10 s: ...`. |
| `PORT_AUDIO_ELASTIC_CAP_MS` | `150` | With `PORT_AUDIO_ELASTIC=1`: a port with no block for this long is treated as stopped (no more waiting for it). |
| `PORT_AUDIO_ELASTIC_DEBT` | `8` | With `PORT_AUDIO_ELASTIC=1`: most concealed blocks to make up later; beyond this they are forgiven. |
| `PS3_LANG_VOICE_FALLBACK` | `1` | Shared with the R2 / R3 ports: for English (UK), a speech file the disc has no UK copy of is replaced by the English one (log: `[lang] ...`). `2` = do the same for any language; `0` = off. Resistance: Fall of Man picks its cutscene language from the movie's own audio streams, so this does not change it. |
| `PORT_AUDIO_LOG` | `0` (off) | Diagnostic. Period of the `[audio]` log line in ms (e.g. `5000`); `1` = every second (also with `R2_RSX_PROF=1`). It also turns on the 5 s `[audio]` gap / sentinel lines, the 10 s `[audio-mode]` speaker-layout check and `logs\sndsetup.txt`. |
| `PORT_STATS_LOG` | `0` | Diagnostic. `1` = periodic statistics in `logs\run.log` (`[fps]`, `[present]`, `[GPUFETCH]`, `[scream]`, `[FSR] stats`, `[DLSS] camera`, the DLSS-G / FSR 3 status repeats, ...) and the small diagnostic files in `logs\` (`rtmake.txt`, `zetamake.txt`, `alphactl.txt`, `cameras.txt`, `gore.txt`, `grab.txt`, `menumode.txt`, `prompts_*.txt`). Off by default so normal play writes no per-frame or periodic logging. Other diagnostics, all off by default: `PS3_VERBOSE=1` (per-wait `[WAIT]` / event lines), `SPURS_WSTATS=1` / `SPURS_SLOTLOG=1` (SPURS statistics), `SPU_PUTLLC_DIAG=1`, `SPU_PMTRACE=1`, `RSX_FRAMETIME_CSV=1` (`logs\frametimes.csv`), `RSX_SHMISS_SAVE=1` (`logs\shmiss\` copies and `[shmiss]` lines), `PS3_FSLOG=1` (file opens / reads). |
| `PS3_THREAD_SLOT_FIX` | `1` | Shared with the R2 / R3 ports: a thread that ended through `sys_ppu_thread_exit` while another thread was already waiting to join it could keep its slot in the 64-entry thread table for good. Each cutscene movie lost a few, and after a few movies the next one's threads could not be created: black screen after the door in "A Fate Worse Than Death" (issue #17). A refused thread create is now logged (`[SYS] sys_ppu_thread_create ... FAILED: all 64 thread slots in use`). `0` = the old behaviour. |

**Reading the log.** At start-up, `logs\run.log` shows `[audio] output: ... SoundTouch stretch (...)` with the MMCSS state and the mixer thread's affinity. With `PORT_AUDIO_LOG=5000` there is a line like this every 5 seconds:

```
[audio] 5.0 s: buffer min 82 avg 101 ms (target 100) | tempo min 1.00 avg 1.000 (stretched 0%) | underruns 0 (silence 0 ms, device dry 0) | ...
```

- `buffer min` well above 0 and `avg` near the target: healthy.
- `tempo min`/`avg` below 1.00 and `stretched` above 0 % during a frame-rate drop: the stretcher is covering it. Tempo should be back to 1.000 within a second or two.
- `underruns` and `device dry` should stay at 0. A non-zero count means a hitch longer than the buffer; try a larger `PORT_AUDIO_BUFFER_MS`.
- `ring overflow` at the end: the game produced audio faster than it was played, so the oldest audio was dropped (harmless after a long pause).

The offline test for this stage is `ps3recomp\libs\audio\tests\test_audio_out.cpp` (run it with `ps3recomp\libs\audio\tests\run_test_audio_out.ps1`).

## Crash and hang reports

- If the game crashes, a minidump and a line in `logs\crash\crashes.txt` are written (`[crash] reporter on` at start). Please send both with `logs\run.log`.
- A hang watchdog writes a dump if no frame is presented for 90 s (not while paused). `RFOM_HANG_S=<seconds>` changes the limit (minimum 10). `RFOM_CRASH_DUMPS=0` turns reports off, `=full` writes full-memory dumps.
- `FREEZE_RELAUNCH=1` restarts the game process after a freeze (pause-aware).
- **Frame-time report (stutter log):** the port records every present-to-present interval (lock-free, no per-frame allocation). At exit, and when a crash or hang report is written, it writes `logs\frametimes_<date>_<time>[_<exe>].txt`: average, 1% low, 0.1% low, worst frame, a histogram, the 20 worst frames with their time and clock time, and stalls of 1 s or more (loading screens, hangs; left out of the numbers). Game pauses (menu, unfocused window) are left out. The newest 10 files are kept; **Report a problem** includes the newest five (user name scrubbed). Press **Scroll Lock** while playing to flag a bad moment: a `[frametime] MARKER` line with the last 2 s of frame times goes to the log and to the file (F8 is the mouse-release key). `PORT_FRAMETIME=0` turns the recorder off; `PORT_FT_MARKER_KEY=scroll|pause|f1..f24|off` picks the marker key. If the recorder ran for over 60 s without seeing a frame, the log gets a `[frametime] WARNING: ... present hook ... is missing` line.

## Steam Deck / Linux

The port is a Windows program. On Linux and the Steam Deck it runs through Proton (Wine). This has not been tested on a real Deck yet, so treat it as experimental.

1. Build the port, either on Linux itself with `setup_linux.sh` (see [Building on Linux](#building-on-linux-experimental) below), or on a Windows PC with Setup (no file picker is needed: the installer and Setup find the game on their own, or take a typed path). If you built it on another PC, copy the whole port folder to the Deck, for example to `/home/deck/Games/ResistancePort` (or a microSD card). Setup.exe / Setup.bat themselves do not run under Wine or Proton (they need Windows PowerShell and Visual Studio).
2. In Desktop Mode, open Steam → **Games → Add a Non-Steam Game to My Library…**, click **Browse**, and pick `Launcher.exe` in the port folder.
3. Right-click the new entry → **Properties → Compatibility** → tick **Force the use of a specific Steam Play compatibility tool** and choose **Proton Experimental** (or Proton 9.0 or newer).
4. Launch options are not needed. `SteamDeck=1 %command%` forces the Deck preset on other Linux PCs.

What the port does under Proton:

- The launcher detects Proton and the Deck (log and the launcher's hardware line say `Steam Deck` or `Wine / Proton`). On the Deck, **Auto-detect** picks a Deck preset: PS3 resolution with FSR 3.1 anti-aliasing, 30 fps, 4x anisotropic filtering.
- **Preset: Steam Deck** (launcher, the *Recommended for your PC* panel): battery-friendly handheld settings on any PC — the Deck recommendation (PS3 resolution, FSR), **40 fps** (set the Deck's screen to 40 Hz), 30 fps menus, a 1280 x 800 window, 150 % UI scale and Xbox button prompts. Screen shake and subtitle size: no option (no hook found; subtitles are drawn at a fixed size). The Deck is detected by `SteamDeck=1` (set by Steam), the APU name, or (Windows on a Deck) the SMBIOS board name `Jupiter` / `Galileo`; there the 40 fps / 1280 x 800 choices appear and 1280 x 800, 150 % UI and Xbox prompts become the defaults.
- The frame rate list gets a **40** option. Set the Deck's screen to 40 Hz (Quick Access → Performance → Framerate limit / refresh rate) to match it. The window list gets **1280x800** (the Deck's screen).
- DLSS is not used under Proton; FSR 3.1 is used instead (XeSS should work through vkd3d-proton but is untested). The Direct3D 12 renderer runs through Proton's vkd3d-proton.
- In the launcher, the controller's **A** or **Start** button starts the game. In game, Steam Input presents the Deck's controls as an Xbox controller.
- The black cover window between the main menu and levels is off under Proton (the window closes and reopens). `RFOM_SEAMLESS=1` turns it on.
- The update check works the same way and can be turned off on the launcher's Advanced page.

### Building on Linux (experimental)

`setup_linux.sh` does what Setup does, natively on Linux and without PowerShell: it checks and copies your game, decrypts the program files, generates the game code and builds the same Windows programs with LLVM (clang-cl and lld-link). The Windows headers and libraries (Microsoft's C runtime and the Windows SDK) are downloaded from Microsoft with [xwin](https://github.com/Jake-Shadle/xwin) into `tools_cache/msvc_linux`, after you accept Microsoft's licence. You then play the result through Proton as described above. It has been checked less than the Windows setup, so please report problems with `logs/setup_linux.log`.

1. Install the tools (LLVM 22 or newer is recommended; with older LLVM the PGO profile is skipped and the game runs a bit slower):
   - Arch: `sudo pacman -S --needed python cmake ninja clang lld llvm curl 7zip`
   - Fedora: `sudo dnf install python3 cmake ninja-build clang lld llvm curl 7zip`
   - Debian / Ubuntu: `sudo apt install python3 python3-venv cmake ninja-build clang lld llvm curl 7zip` (LLVM 22 from [apt.llvm.org](https://apt.llvm.org) if your release has an older one)
2. Get the port (git clone or the source zip) into a folder with about 25 GB free, then run:
   ```
   ./setup_linux.sh --game "/path/to/Resistance Fall of Man [BCUS98107].iso"
   ```
   `--game` takes a decrypted .iso or a folder with `PS3_GAME` in it (for example from RPCS3's `games` folder). UDF disc images are unpacked with 7-Zip. Other options: `--jobs N` (fewer compile jobs if you have little RAM: 16 GB → 4, 8 GB → 2), `--check-only`, `--nopgo`; `./setup_linux.sh --help` lists them all.
3. Later rebuilds (after `git pull`): run `./setup_linux.sh` again (it regenerates the game code only when the code generator changed), or `./build_linux.sh` for a build only.

Steam Deck: SteamOS's system folders are read-only, so install the tools inside a [distrobox](https://distrobox.it/) container (for example Arch) and run `setup_linux.sh` there, or build on another Linux PC and copy the folder over. The build takes a long time on the Deck's 4 cores.

Not on Linux: NVIDIA DLSS / DLSS Frame Generation and Intel XeSS are not set up (Proton does not offer DLSS to the port; FSR 3.1 and FSR 3 Frame Generation are used), and the launcher's **Update now** is not offered under Proton (the updater is a Windows PowerShell script): update with `git pull` and `./setup_linux.sh`, or the new release's source.

## Status

- The campaign has been played up to Cheddar Gorge. Later levels haven't been tested yet, so problems are possible there.
- Crashes and freezes are rare. Crash and hang reports are written automatically (see *Crash and hang reports*), and the port can restart the game process after a freeze.
- The game is mostly CPU-bound and uses little GPU. CPU use and temperatures are far lower than when emulating the game.

## Known issues

- **Performance drops in heavy areas.** These come from the game's SPU jobs: they already run on three SPU threads at once, but many are chained (each waits for the previous), with the main thread waiting on the chain, so the cost is mostly latency rather than core count. The scheduler wakes only the thread that has work (`RFOM_SPU_WAKE`) and spreads SPU threads over free cores; `RFOM_SPU_WORKERS=auto` (experimental) adds virtual SPUs. A faster CPU helps most.
- **Audio crackle when the frame rate drops** in intensive spots, for example coming out of the crater at the start of York. Audio mixing is one of the SPU jobs. The audio buffer and time stretching (see [Audio](#audio)) are meant to cover these drops; check the `[audio]` log line if it still happens.
- **Falling through the floor** has been seen a few times and hasn't been reproduced yet. A safeguard tries to put you back in place before the fall kills you.
- **Shadows can flicker** occasionally.
- **FSR can ghost** on some things, such as the sky, at rare moments. DLSS doesn't show this.
- **Out-of-bounds water** (off the dock in Grimsby, under the bridge in Manchester) shows some stray triangles.
- **Rollers:** on RPCS3 they crashed the game unless OpenGL was used. Their level hasn't been reached on this port yet.
- **Ultrawide HUD:** the HUD is stretched with the view, not kept in a centred 16:9 area.
- **Frame Generation is experimental** (DLSS on RTX 40/50, FSR 3 elsewhere; off by default). Some artifacts remain: some ghosting at building entrances, and artifacts on the weapon (carbine) and at a distance. A hot patch is planned; reports are welcome.
- **Missing HUD / health bar or menu selection highlight:** some players see no HUD or health bar, or no highlight on the selected menu entry. Under investigation; please report it with `logs\run.log`.
- **Two programs:** on the PS3 disc the main menu and the levels are separate programs, so the port restarts the game process when you load into a level from the main menu (and back). A black cover window stays on screen during the switch, and the new window opens at the same place, so it looks like one window. `RFOM_SEAMLESS=0` turns the cover off (the window then closes and reopens). Log: `[exitspawn] cover window up`, `[exitspawn] cover released after N ms`.

If you hit a problem, describe how to reproduce it and send `logs\run.log` from that session (plus `logs\crash\` if there was a crash).

## Roadmap

What's planned after this beta, and how it relates to the issues above: [docs/ROADMAP.md](docs/ROADMAP.md).

## How it works

- `ps3recomp/` is a modified copy of [ps3recomp](https://github.com/sp00nznet/ps3recomp) by sp00nznet (MIT licence). It holds the runtime, the PS3 system libraries (graphics, audio, input, file system, SPU scheduling) and the lifters that translate PowerPC and SPU code into C/C++.
- `project/` is the game-specific part of the port: its hooks, the D3D12 renderer integration, mouse look, the PC menu, the launcher and the debug tools.
- `tools/rfom_regen.py` runs the lifters on your decrypted `EBOOT.elf` and `game.elf`, then re-applies this port's own edits to the generated code (`tools/hooks/`).

## Credits and licences

- Original Resistance: Fall of Man recomp: [Psyykosis](https://github.com/Psyykosis/Resistance-Fall-of-Man-Recomp). This fork adds DLSS / FSR 3.1, the PC menu, ultrawide, the performance work and the other PC features above.
- ps3recomp, by Ned Heller / sp00nznet: MIT.
- SoundTouch 2.4.1 by Olli Parviainen (audio time stretching), in `ps3recomp/third_party/soundtouch/`: LGPL-2.1 (`COPYING.TXT` there).
- at3_standalone, FFmpeg's ATRAC3 / ATRAC3plus decoders made standalone by the PPSSPP project (PPSSPP commit `ebc151f`), in `ps3recomp/third_party/at3_standalone/`: LGPL-2.1-or-later (`COPYING.LGPLv2.1` there). Like SoundTouch it is linked statically; this repository is the source you need to relink it with a modified version.
- AMD FidelityFX SDK (FSR 3.1), the DLLs in `tools/ffx_bin/`: MIT, © Advanced Micro Devices. FSR 3 frame generation (`amd_fidelityfx_framegeneration_dx12.dll`, SDK 2.3.0, MIT) is downloaded by Setup into the same folder.
- NVIDIA DLSS SDK v310.9.16, downloaded during setup into `tools/dlss/` only after you accept NVIDIA's licence: NVIDIA RTX SDK licence (`tools/dlss/LICENSE.txt`), not part of this repository. It is used only by the separate DLSS bridge DLL that build.bat builds on your PC. This port uses NVIDIA DLSS; NVIDIA, GeForce RTX and NVIDIA DLSS are trademarks of NVIDIA Corporation.
- NVIDIA Streamline SDK 2.14.16 (DLSS Frame Generation), downloaded during setup into `tools/streamline/` only after you accept NVIDIA's licences, and not part of this repository: the SDK and `sl.*.dll` are MIT (`tools/streamline/license.txt`, third-party notices in `tools/streamline/3rd-party-licenses.md`); `nvngx_dlssg.dll` (DLSS Frame Generation) is under the NVIDIA RTX SDK licence (`tools/streamline/bin/x64/nvngx_dlss.license.txt`) and Reflex under `reflex.license.txt`. `build.bat` builds the bridge `rfom_dlssg.dll` and copies the unmodified DLLs next to the game. NVIDIA DLSS Frame Generation and NVIDIA Reflex are trademarks of NVIDIA Corporation.
- NVIDIA NVAPI SDK (headers and `nvapi64.lib`, statically linked for Reflex), in `tools/nvapi/`: MIT licence (`tools/nvapi/License.txt`).
- Intel XeSS SDK 2.1.1 (runtime `libxess.dll` and headers), downloaded during setup into `tools/xess/` only after you accept Intel's licence, and not part of this repository: Intel Simplified Software License (`tools/xess/LICENSE.txt`, third-party notices in `tools/xess/third-party-programs.txt`). `build.bat` copies the unmodified `libxess.dll` next to the game; reverse engineering it is not allowed. Needs the Visual C++ 2015-2022 runtime (14.40 or newer).
- FFmpeg 7.1 (the BtbN `win64-lgpl-shared-7.1` build: unmodified `avcodec-61`, `avutil-59` and `swresample-5` DLLs), downloaded during setup into `tools/ffmpeg_bin/` for the game's videos and audio (the H.264, MPEG-2 video and AC-3 decoders): LGPL-3.0-or-later (`tools/ffmpeg_bin/LICENSE.txt`; `tools/ffmpeg_bin/SOURCE.txt` says where it came from and where its source is). It is loaded at run time and is not part of this repository. The FFmpeg headers in `ffmpeg/include/` are LGPL-2.1-or-later (see each file).
- RPCS3 (`key_vault.cpp`), downloaded during setup and used only to decrypt your own copy.
- The port's own code: MIT, see `LICENSE`.

## Support

This port is free and always will be: no paid builds, no early access and no perks. It is a fan project made in my spare time.
If you enjoy it and feel like buying me a coffee, it's genuinely appreciated and keeps the late-night debugging going ☕
[buymeacoffee.com/damiro98](https://buymeacoffee.com/damiro98)

A tip supports my own time and tools, such as [shaderscan](https://github.com/miroslavdanutalexandru-alt/shaderscan). It is not a payment for the game or this port, and nothing is unlocked by it. You still need your own copy of the game.
This port is built on Psyykosis' original recomp and on ps3recomp; they deserve the credit (and any coffee) just as much.

*Resistance: Fall of Man* is © Sony Interactive Entertainment, developed by Insomniac Games. This is an unofficial fan project, not affiliated with or endorsed by Sony or Insomniac. Please own the game you play.
