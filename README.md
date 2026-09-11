# EmuCoreR

[![Support on Patreon](https://img.shields.io/badge/Patreon-Support%20EmuCore-ff424d?logo=patreon&logoColor=white)](https://www.patreon.com/c/emucore/membership)

EmuCoreR is a PlayStation 1 emulator and game library for Android, built around the SwanStation libretro core (a DuckStation-derived PS1 emulator) and a purpose-built Compose interface for phones, tablets, and Android TV.

## Highlights

- SwanStation libretro core with an ARM64 CPU recompiler/JIT and interpreter fallback
- OpenGL ES, Vulkan, and software renderers with up to 10x internal resolution and texture filtering (Nearest, Bilinear, JINC2, xBR)
- True color rendering, scaled dithering, deinterlacing, and NTSC timing controls
- PGXP geometry precision, widescreen hacks, fast boot, and software renderer readbacks
- Disc image support for CUE/BIN, ISO, IMG, CHD, PBP, ECM, MDS, and PSF, plus M3U playlists
- Game library with folder scanning, cover art (flat, 3D, or custom), search, and recent games
- Save states with screenshot previews, quick save and quick load, auto-save, and auto-load
- Memory card manager with slot assignment, backup/restore, import, and export
- Per-game settings profiles for graphics, controls, and audio
- In-game menu with renderer and display switching, disc swap, quick actions, and region info
- Touch controls with a layout editor, rumble, analog mode control, and physical gamepad support
- Theme manager with light, dark, neon, and custom themes
- Cheats and HD texture packs from online EmuCore catalogs
- Hub with PlayStation news, history, videos, and manuals
- Discord Rich Presence integration
- 18 interface languages

## What This Repository Contains

This repository contains the Android application, its Compose UI, settings and data layers, the JNI libretro frontend, and the vendored SwanStation emulation core.

## Tech Stack

- Kotlin + Jetpack Compose
- Android DataStore and Room
- JNI bridge to a native C++ libretro frontend
- SwanStation libretro core vendored under `third_party/swanstation`
- Coil and WorkManager for library artwork and background jobs
- Discord Social SDK for Rich Presence and party features
- Firebase services used by the Hub and feedback flows

## Current App Scope

EmuCoreR version `0.0.1` currently targets Android with:

- `minSdk 26`
- `targetSdk 37`
- package id `com.sbro.emucorer`
- version `0.0.1`

## Building Locally

### Requirements

- Android Studio with Android SDK and NDK configured
- JDK compatible with the Gradle setup in this project
- A device or emulator for Android testing

### Debug Build

```powershell
.\gradlew :app:assembleDebug
```

### Release Build

```powershell
.\gradlew :app:assembleRelease
```

## Project Structure

- `app/` Android application module
- `app/src/main/java/com/sbro/emucorer` Kotlin app code
- `app/src/main/cpp` JNI libretro frontend that drives the SwanStation core
- `app/src/main/res` Android resources and translations
- `app/src/main/assets/catalog` game catalog, cover index, and PS1 serial/title index
- `third_party/swanstation` vendored SwanStation libretro core and its in-tree dependencies
- `tools/` local release scripts; ignored by git

## Notes

- BIOS files and game images are not distributed with this project.
- You must use your own legally obtained BIOS files and game dumps.
- Compatibility, performance, and graphics behavior vary by device and renderer.
- Releases marked as "parallel" are identical to the primary build but use an alternate package ID to allow side-by-side installation.

## Credits

EmuCoreR builds on the open-source SwanStation and DuckStation projects, together with its own Android interface, library system, runtime controls, and handheld-focused UX. The Kotlin bridge, game library, libretro frontend, and renderer integration are maintained in this repository.

## Support

If you want to support ongoing development:

- Patreon: https://www.patreon.com/c/emucore/membership
- More apps by the author: https://play.google.com/store/apps/dev?id=7136622298887775989

## License

The bundled emulation core derives from SwanStation and DuckStation, which are distributed under the GNU General Public License v3.0 or later. The core sources, including EmuCoreR adaptations, are published with the app at https://github.com/sashkinbro/EmuCoreR.

The EmuCoreR Android application code is not open source and is distributed under its own terms.
