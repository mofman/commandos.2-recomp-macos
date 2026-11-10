# Commandos 2 Recomp for macOS

An experimental native Apple silicon recompilation of **Commandos 2: Men of Courage** (the original 2001 game). Python tools translate the supported Windows executable's x86 code into C; a small runtime implements the Windows and DirectX interfaces using SDL3 and macOS. The resulting arm64 executable runs without Wine or Rosetta.

You must supply your own game files and the exact supported executable. This project is unofficial and is not affiliated with Pyro Studios, Eidos, or the game's current rights holders.

## Improvements

Implemented in this macOS version:

- Native Apple silicon execution, SDL3 graphics, audio and input, and a Finder-launchable `.app` bundle.
- Widescreen support with eight selectable resolutions, with correct labels and saved selection: **984×640, 1232×800, 1280×800, 1512×982, 1680×1050, 1920×1200, 1600×900, and 1920×1080**. These include MacBook, 16:10, and 16:9 display shapes.
- Higher frame rate rendering in missions and inventory, with interpolated camera scrolling and zoom. Game logic stays at its original **25 ticks per second**; the render rate follows the display or a configured cap.
- Portrait and animated cursor guards so extra rendered frames do not speed up their animations.
- Pause menus that retain the mission resolution, and correctly centred popup dialogs at larger resolutions.
- Fixes for high-resolution map blit overflows and tile-cache limits that could cause crashes or missing/smeared map areas when zooming.
- A fix for ignored rapid inventory clicks.
- Reuse of freed large memory allocations, preventing guest heap exhaustion during repeated allocations.
- macOS fullscreen/windowed switching, Command+Tab app switching, and Command-key shortcuts, plus writable saves and configuration outside the packaged app.
- Zoom with the main keyboard’s `-` and `+`/`=` keys, the numeric keypad, or a mouse wheel/trackpad—no numeric keypad required.

Several enhancements were adapted from [Commandos2-ModernPatch](https://github.com/TweenzY-Y/Commandos2-ModernPatch). This is a partial port: object movement interpolation, interface scaling, the in-game patch settings menu, cheat extensions, and water optimisations are **not implemented** here. Do not assume every feature or INI key from the Windows patch is supported. See [the address map and porting notes](runtime/overrides/ADDRESSES.md).

## Requirements

- An Apple silicon Mac. The current CMake configuration builds **arm64 only**.
- Xcode Command Line Tools for compilation and packaging. **Xcode 26 or newer with Icon Composer support** enables the modern `.icon` app icon; packaging falls back to a standard `.icns` icon if the icon compiler is unavailable.
- [Homebrew](https://brew.sh/), CMake 3.24+, Ninja, SDL3, and Python 3.10+ with Capstone.
- A licensed copy of the original game and the exact executable described below. The HD Remaster is a different game and is unsupported.

```sh
xcode-select --install
brew install cmake ninja sdl3 python
python3 -m venv .venv
source .venv/bin/activate
python -m pip install -r requirements.txt
```

Run all subsequent commands from the repository root. Generating and compiling the translated game is substantial; allow time and disk space, and reduce the parallel job count if memory is limited.

## Obtain and prepare the game files

Use your original discs of **Commandos 2: Men of Courage**.

Install the original game on Windows along with last 1.2 patch available below, then copy the contents of its installation folder (usually under Program Files) directly into `gamedata/`, including `comm2.exe`. Copy the **installed files**, rather than the installer CAB archives. Keep the directory names and contents intact:

```text
gamedata/
  DATA/
  OUTPUT/
  DATA.PCK
  DATA2.PCK
  comm2.exe                 # exact supported 1.2 executable, supplied by you
```

The current hooks target the **1.2 patch executable**, not the Steam main executable or the original retail executable. An archive of the [international 1.2 patch is listed at The Patches Scrolls](https://www.patches-scrolls.com/commandos2.php). Apply the appropriate official patch to your licensed installation and check the resulting executable before continuing. Other regional or store builds may differ; they have not been validated against these hooks.

Verify the executable copied with the rest of the game files:

```sh
mkdir -p gamedata
# Copy the contents of your game installation folder into gamedata, including comm2.exe.
shasum -a 256 gamedata/comm2.exe
```

Expected SHA-256:

```text
f4ca330c86ad3fad293a26ba2d3425dbb69002494e6fb524e13936fcb60253be
```

If it differs, stop: renaming a different executable will not make it compatible. Both generation and runtime loading require this executable. No game downloads, installers, executable, or assets are supplied by the source export.

## Generate and build

Generate the analysis and C sources locally from your executable:

```sh
source .venv/bin/activate
mkdir -p analysis
python recomp/analyze.py gamedata/comm2.exe analysis/functions-1.2.json
python recomp/lift.py gamedata/comm2.exe analysis/functions-1.2.json generated
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --parallel 4
```

The generator applies the hooks in `recomp/overrides.txt` and verifies expected patch bytes. A patch-byte mismatch usually indicates an unsupported executable. Generate sources **before** configuring CMake; they are intentionally excluded from version control. Regenerate after changing the lifter or overrides, then rebuild. Runtime-only C changes just need a rebuild.

If SDL3 is not found, provide its CMake prefix:

```sh
cmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH="$(brew --prefix sdl3)"
```

Start from the repository root:

```sh
./build/Commandos2
# Or start in a window:
C2_WINDOWED=1 ./build/Commandos2
```

The default data directory is `gamedata`, and the default executable is `gamedata/comm2.exe`. `C2_GAME_DIR` overrides the data directory; the executable defaults to `comm2.exe` inside that directory. Set `C2_EXE` to override the executable separately. Use absolute paths for these overrides. Set `C2_USER_DIR` to use a separate writable directory for saves and settings; without it, an unbundled run writes into the game directory.

### Widescreen and controls

Choose a resolution in the game's video options that matches your display shape: **1600×900** or **1920×1080** for 16:9, **1280×800**, **1680×1050**, or **1920×1200** for 16:10, or **984×640**, **1232×800**, or **1512×982** for the supported MacBook display shape. The game renders at the selected resolution; fullscreen presentation preserves its aspect ratio and adds black bars when the display shape differs.

| Action | Control |
|---|---|
| Zoom out | Main-row `-` or numeric keypad `-` |
| Zoom in | Main-row `+`/`=` key (no Shift required) or numeric keypad `+` |
| Wheel/trackpad zoom | Wheel forward or two fingers up to zoom in; reverse to zoom out |
| Switch to another app (the macOS equivalent of Alt+Tab) | **Command+Tab**; switch back to return to the game |
| Toggle fullscreen/windowed | **Command+Return** |
| Quit immediately | **Command+Q**; save your progress first |

The main-row zoom keys work on MacBook keyboards without a numeric keypad. App switching is handled by macOS, and the game restores its in-game cursor when focus returns.

## Configuration and saves

Optionally create `gamedata/Commandos2-ModernPatch.ini`:

```ini
[Commandos2-ModernPatch]
Enabled=1
Camera=1
HDMenus=1
MaxFps=0
Debug=0
```

`Enabled` controls the extra-frame rendering hooks; `Camera` enables camera interpolation; `HDMenus` keeps the pause menu in the mission resolution. `MaxFps=0` uses the display refresh rate; a positive value sets a cap (clamped to 25–1000). Restart after editing. The resolution list is defined in `runtime/overrides/resolution.c`, rather than by the Windows patch's `HDResolution` key.

A packaged app uses:

```text
~/Library/Application Support/Commandos 2 Recomp/
  OUTPUT/                       # profiles, saves, COMM2.CFG, key mappings
  Commandos2-ModernPatch.ini     # if supplied in the bundle
  c2native.log
```

Files are seeded from the bundle on first launch; existing user files are retained. Changing the bundle's INI will not replace an existing user copy. Back up this directory before experimenting with settings or saves.

## Package a macOS app

With the game built, game files in the default locations above, and Xcode Command Line Tools available:

```sh
cmake --build build --target package_app
open "dist/Commandos 2.app"
```

Or invoke the script directly:

```sh
./packaging/package_app.sh build/Commandos2
# Optionally seed the app with your current profiles and saves:
./packaging/package_app.sh --with-saves build/Commandos2
```

The script replaces `dist/Commandos 2.app`, embeds the SDL3 dylib, copies the game files and supported executable, compiles the icon from `assets/AppIcon.icon`, writes `Info.plist`, and ad-hoc signs the result. It uses macOS clone-copy support (`cp -c`); use an APFS volume. The app's minimum OS version comes from the compiled binary; the project's packaging fallback of 11.0 is not a tested compatibility guarantee.

**This app contains your game files. Keep it for personal use; do not upload it as a public release.** The default package omits your `OUTPUT` directory. `--with-saves` includes personal profiles and saves as first-launch seed data.

The signature is ad-hoc, not Developer ID signing or notarisation. To inspect the result:

```sh
codesign --verify --strict "dist/Commandos 2.app"
otool -L "dist/Commandos 2.app/Contents/MacOS/Commandos2"
```

The executable should reference the embedded SDL3 library. The packaging script warns if SDL3 itself still depends on other Homebrew libraries; resolve those dependencies before expecting the bundle to work on a machine without Homebrew.

## Checks and troubleshooting

```sh
python3 tests/heap_regression.py
```

This exercises large allocation reuse, reallocation and heap boundary handling without game data. It is not a full gameplay test. For a manual smoke test, start a mission, scroll and zoom, change resolution, open the pause menu, save, quit and reload. Full campaign, multiplayer, all regional data sets, and older macOS versions are not certified by this project.

- **Missing generated C files:** run the analysis and lift steps before CMake configuration.
- **Patch-byte mismatch or missing dispatch target:** confirm the executable checksum and regenerate with the current hooks. Report the terminal log and steps to reproduce.
- **Missing game data:** confirm `DATA.PCK` is directly inside `gamedata`, not one directory deeper. Launch the unbundled binary from the repository root.
- **Standard icon fallback:** packaging uses a standard `.icns` icon when `actool` cannot compile the modern icon. For the modern appearance, ensure `xcrun --find actool` uses a full Xcode installation supporting `.icon` assets.
- **Finder launch fails:** inspect `~/Library/Application Support/Commandos 2 Recomp/c2native.log`. Run `C2_LOG_STDERR=1 "dist/Commandos 2.app/Contents/MacOS/Commandos2"` for terminal diagnostics.


## Credits

- Original game: Pyro Studios / Eidos and the respective rights holders.
- [Commandos2-ModernPatch](https://github.com/TweenzY-Y/Commandos2-ModernPatch), whose hooks and fixes informed this partial native port. Its [MIT licence notice](third_party/Commandos2-ModernPatch.LICENSE) is retained.
- [SDL3](https://github.com/libsdl-org/SDL) for platform services, and [Capstone](https://www.capstone-engine.org/) for disassembly. They are external build dependencies with their own licences; retain applicable notices when redistributing their components.
