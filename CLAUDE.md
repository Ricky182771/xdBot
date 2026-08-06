# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

xdBot is a botting/macro tool for Geometry Dash, built as a [Geode](https://geode-sdk.org) mod (mod id `zilko.xdbot`). It records and replays player inputs as macros, with a renderer, practice fixes, and various gameplay hacks. Targets GD `2.2081` on Windows and Android; Geode `5.8.2`.

## Building

This is a Geode mod — it is not built with a plain CMake invocation. Building requires the Geode SDK.

- **Requirement:** `GEODE_SDK` environment variable must point to a Geode SDK checkout (CMake fails hard otherwise, see [CMakeLists.txt](CMakeLists.txt#L41)).
- **Local build:** `geode build` (Geode CLI wraps CMake + packaging into a `.geode` file). C++23, requires the `geode.custom-keybinds` dependency (`>=v1.10.0`, Windows only).
- **CI:** [.github/workflows/multi-platform.yml](.github/workflows/multi-platform.yml) builds Windows (`RelWithDebInfo`) + Android32/Android64 via `geode-sdk/build-geode-mod` on every push to `main`. There is no separate lint or test suite — the compiler and CI are the checks.
- **Adding a source file:** new `.cpp` files must be added to the `add_library` list in [CMakeLists.txt](CMakeLists.txt#L9); headers are `#include`d directly and don't need listing.

When bumping the version, update both `version` in [mod.json](mod.json#L7) and `xdBotVersion` in [src/gdr/gdr.hpp](src/gdr/gdr.hpp#L17) (the latter is embedded in saved macros).

## Architecture

**Global singleton.** [src/global.cpp](src/global.cpp) / the `Global` class in [src/includes.hpp](src/includes.hpp#L46) is the central hub — accessed everywhere via `Global::get()`. It holds the current `Macro`, the `Renderer`, the recording/playing `state`, all feature toggles, keybind mappings, and per-frame bookkeeping. Nearly every feature reads and mutates this shared state, so treat it as the source of truth for runtime flags. `includes.hpp` is the common header included by most translation units.

**Hooking model.** Behavior is injected via Geode's `$modify(SomeClass)` hooks (e.g. `PlayLayer`, `GJBaseGameLayer`, `PauseLayer`, `PlayerObject`) spread across the source tree. `$execute` blocks register setting-change listeners at load. Game state is read/written through Geode's bindings to RobTop's classes.

**Macro system.** A macro (`Macro` struct in [src/macro.hpp](src/macro.hpp#L38)) is built on the shared **GDR replay format** ([src/gdr/gdr.hpp](src/gdr/gdr.hpp), `gdr::Replay`/`gdr::Input`). Recording captures button `input`s per frame; "frame fixes" additionally snapshot player position/rotation to correct desync. [src/macro.cpp](src/macro.cpp) handles record/play/save/autosave and the `.xd`↔GDR conversion. Playback accuracy has three modes (Vanilla / Input Fixes / Frame Fixes, `macro_accuracy` setting), driven by the `frameFixes`/`inputFixes` flags on `Global`.

**PlayerData snapshots.** The large `PlayerData` struct in [src/macro.hpp](src/macro.hpp#L86) mirrors the full memory layout of RobTop's `PlayerObject` (many `m_unk*`/reverse-engineered fields). It is used to snapshot and restore player state for checkpoints (`CheckpointData`) and practice fixes. Fields are layout-sensitive — order matters, and the Windows-only block is `#ifdef GEODE_IS_WINDOWS`.

**Directory layout.**
- `src/hacks/` — self-contained features (autoclicker, clickbot, coin finder, frame stepper, layout mode, trajectory, tps bypass, noclip, etc.).
- `src/ui/` — Geode/cocos2d popups and layers (the menu, macro editor, record layer, render/clickbot settings). UI uses the `STATIC_CREATE` macro ([src/includes.hpp](src/includes.hpp#L35)) for the standard `create()` boilerplate.
- `src/practice_fixes/` — checkpoint/respawn desync corrections split by hooked class (`input.cpp`, `player.cpp`, `play_layer.cpp`).
- `src/renderer/` — video renderer that captures frames to an FBO and hands them to one of three encoder backends, chosen by `Renderer::selectBackend()` (`VideoBackend` in [renderer.hpp](src/renderer/renderer.hpp)): `FFmpegAPI` (in-process, via the `eclipse.ffmpeg-api` mod), `WindowsExe` (subprocess `ffmpeg.exe`, path via the `ffmpeg_path` setting), and `NativeUnix` ([native_ffmpeg.cpp](src/renderer/native_ffmpeg.cpp) — under Wine/Proton, drives the *host* Linux ffmpeg for hardware encoding). `renderer/ffmpeg/` holds the recorder/export/settings. Credited to ReplayBot.
  - The backend also selects the OpenGL capture path: `usesCoreGL()` picks core GL functions vs. the `_EXT` variants — these are two separate concerns that happen to share one switch, so changing one without the other breaks capture.
  - `NativeUnix` writes an `/bin/sh` launcher script, spawns it with `start.exe /unix`, and streams raw rgb24 frames to ffmpeg over loopback TCP (ffmpeg listens, the mod connects). Every give-up path on the encoder thread must go through the `abortRender` helper: `Renderer::captureFrame` spins on the **main** thread until `frameHasData` clears or `encoderFailed` is set, so a bail-out that forgets the flag freezes the game with no way to even show a popup.
- `src/keybinds.cpp` — integrates with `geode.custom-keybinds`; the six action buttons (jump/left/right × p1/p2) map to `buttonIDs` in [src/includes.hpp](src/includes.hpp#L26).
- `src/utils/` — shared helpers (`Utils::` namespace), plus `subprocess.hpp`.

**Frames & seeds.** Everything is frame-indexed via `Global::getCurrentFrame()`. The `seed` (RNG) is captured/restored so replays are deterministic; `seedAddr` in [src/includes.hpp](src/includes.hpp#L18) is a hardcoded memory offset.

## Conventions

- Platform-specific code is guarded with `GEODE_IS_WINDOWS` / `GEODE_IS_ANDROID`; several settings and the keybinds dependency are Windows-only (see `platforms` in [mod.json](mod.json)).
- User-facing settings live in [mod.json](mod.json) and are read via `Mod::get()->getSettingValue<T>(...)`; changes that need to update runtime flags are wired through `listenForSettingChanges` in [src/main.cpp](src/main.cpp#L11).
- `changelog.md` is user-facing and kept in sync with version bumps.
