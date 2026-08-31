# ASPHALT 5 — PS Vita Port

<p align="center">
  <img src="extras/livearea/pic0.png" width="700" alt="Asphalt 5 PS Vita Banner" />
</p>

<p align="center">
  <b>Native port of Asphalt 5 HD (Gameloft) for PlayStation Vita and PlayStation TV.</b>
</p>

<p align="center">
  <img src="https://img.shields.io/badge/Platform-PS%20Vita%20%7C%20PS%20TV-003791.svg?style=flat-square&logo=playstation" alt="Platform PS Vita" />
  <img src="https://img.shields.io/badge/Title%20ID-ASPHALT05-ff69b4.svg?style=flat-square" alt="Title ID ASPHALT05" />
  <img src="https://img.shields.io/badge/Engine-Gameloft%20Proprietary-brightgreen.svg?style=flat-square" alt="Engine" />
  <img src="https://img.shields.io/badge/Renderer-vitaGL%20%28GLES%201.1%29-orange.svg?style=flat-square" alt="Renderer" />
  <img src="https://img.shields.io/badge/Status-Playable%20(Beta)-success.svg?style=flat-square" alt="Status: Playable" />
</p>

---

## 📖 Description

**Asphalt 5** is Gameloft's arcade racing game, originally released for Android as
`Asphalt-5-HD-v3.4.1.apk`. This port runs the compiled native library (`libasphalt5.so`)
from the Android release directly on the PS Vita's ARM Cortex-A9 processor, using a
dynamic loader (*soloader*) and an Android environment emulation layer (*FalsoJNI*),
with [vitaGL](https://github.com/Rinnegatamante/vitaGL) providing the GLES 1.1
fixed-function rendering backend.

### 🎮 Current Status: Playable (Beta)

The game **is fully playable from start to finish**. Extensive work has gone into diagnosing and fixing several performance bottlenecks (synchronous SD card I/O, cache thrashing, soft-float audio overhead). See [`port_progress.md`](port_progress.md) for the full bug-by-bug history. 

Since this is a Beta release, keep in mind:
* Audio is working and has been heavily optimized (fixed-point math, linear interpolation), but still has some minor quirks and room for improvement.
* There may be other undiscovered bugs or occasional UI glitches.

### ✨ What Works

- **Native ARM Execution**: `libasphalt5.so` (armeabi/ARMv6) runs directly on the
  Vita's CPU via the soloader, no interpretation/emulation of game code.
- **Boots to Title + Main Menu**: Full JNI lifecycle bootstrap
  (`nativeGetJNIEnv` → `GLResLoader`/`GLMediaPlayer` init → `Asphalt5_nativeInit` →
  `Asphalt5Renderer_nativeInit`) matching the real Android `onSurfaceCreated()` order.
- **vitaGL Graphics Pipeline**: GLES 1.1 fixed-function rendering, with an internal
  720x432 offscreen FBO downsampled to the native 960x544 panel (menu layout still
  reports 800x480 to the engine so UI scaling stays correct).
- **Audio**: Custom 32-bit fixed-point audio mixer with linear interpolation running on the `MAIN` audio port.
- **Input**: Full physical button support! D-Pad/Analog for menus, and full steering/pedal support during races.
- **Assets from `ux0:`**: Resource loader reads game assets/chunks from `ux0:data/asphalt5/`
  with an LRU cache to reduce SD card stutter.

### ⚠️ Known Issues

- **Audio quirks**: While heavily improved, the audio mixing still has some minor distortions or volume balancing issues in certain heavy tracks.
- **Beta bugs**: Unmapped physical buttons in very specific sub-menus or rare cache trashing between
  the asset cache and the GPU resource pool, and vitaGL vertex pool pressure —
  see the bug log in [`port_progress.md`](port_progress.md) (Bugs #9, #16–#22).
- **Video playback**: The Gameloft intro trailer is skipped instantly instead of
  being decoded (h264 decoding not implemented).

---

## 📋 Prerequisites

To run this port on your PS Vita or PS TV, you will need:

1. A PS Vita / PS TV console running Custom Firmware (**HENkaku** or **Enso**),
   firmware 3.60/3.65 or later recommended.
2. [**kubridge**](https://github.com/TheOfficialFloW/kubridge/releases) and
   [**FdFix**](https://github.com/TheOfficialFloW/FdFix/releases) installed as
   kernel plugins (`ur0:tai/config.txt` under `*KERNEL`).
3. [**libshacccg.suprx**](https://github.com/Rinnegatamante/ShaRKBR33D/releases/latest)
   installed in `ur0:data/`.
4. A legally obtained copy of **Asphalt 5 HD v3.4.1** (`Asphalt-5-HD-v3.4.1.apk`,
   package `com.gameloft.android.GAND.GloftA5HD`).

---

## 📦 Installation Instructions

1. Install the `asphalt5.vpk` file on your console using **VitaShell**.
2. On your PC, place `Asphalt-5-HD-v3.4.1.apk` in the project root (or extract it
   into `asphalt5_extract/`).
3. Use **psvita-port-toolkit** (the standalone tool this port is managed with) to
   prepare and transfer the asset files to your console — open the toolkit and
   select "Continuar con un port existente" pointing at this folder.
4. Transfer the resulting game data to `ux0:data/asphalt5/` via FTP or USB using
   VitaShell.

### Final File Structure in `ux0:data/asphalt5/`

```text
ux0:data/asphalt5/
├── libasphalt5.so     <- Native library extracted from lib/armeabi/
├── assets/            <- Game data files (.cnk chunks, packages, etc.)
├── logs/               <- Incremental debug logs (asphalt5_NNN.log)
└── cg/ glsl/           <- Shader cache (created at runtime/build)
```

---

## 🛠️ Building from Source

This port does **not** keep a local copy of `porting_tools/` — all build, deploy,
log, LiveArea, and crash-dump workflows are handled by **psvita-port-toolkit**, a
standalone tool kept outside this repository.

### Build Prerequisites

- **VitaSDK**, fully compiled with softfp usage (`vitasdk-softfp/vdpm`).
- VitaSDK libraries: `vitaGL`, `vitashark`, `kubridge`, `pthread`.
- CMake and Make.

### Build Steps

```bash
cmake -Bbuild .
cmake --build build
```

This produces `build/asphalt5.vpk`. For day-to-day development (build + deploy +
crash-dump parsing), use **psvita-port-toolkit** instead of raw `cmake`/`make`.

---

## 🏗️ Project Structure

- `source/`: Native C/C++ loader (lifecycle, GLES rendering, audio, input, JNI
  resource loader, video).
- `lib/`: Auxiliary libraries (`so_util`, `falso_jni`, `libc_bridge`, `fios`,
  `kubridge`, `minimp3`, `sha1`, `stb`).
- `extras/`: LiveArea assets (`icon0.png`, `bg0.png`, `pic0.png`, `startup.png`,
  `template.xml`), plus `cpuinfo`/`meminfo` and debug scripts.
- `PORTING_PLAN.md`: Living plan — engine findings, JNI export table, checklist.
- `port_progress.md`: Bug-by-bug diagnosis log, one confirmed bug at a time.

---

## ⚖️ Disclaimer

**Asphalt 5** is a registered trademark of Gameloft. The work presented in this
repository is not "official" or produced or sanctioned by Gameloft or any other
registered trademark mentioned in this repository.

This software does not contain the original code, executables, assets, or other
non-redistributable parts of the original game product. The authors of this work
do not promote or condone piracy in any way. To launch and play the game on their
PS Vita device, users must possess their own legally obtained copy of the game in
the form of an `.apk` file.

---

## 👥 Credits and Acknowledgements

- **Gameloft**: Original developers of Asphalt 5.
- **TheFloW**: For `so_util`, `kubridge`, `FdFix`, and foundational techniques for
  loading Android executables on PS Vita.
- **Rinnegatamante**: For `vitaGL` and continued support to the PS Vita porting scene.
- **v-atamanenko**: For `FalsoJNI` and the `soloader-boilerplate` base template.
- **Vita Community**: To all developers and enthusiasts in the PS Vita homebrew community.

---

## License

This software may be modified and distributed under the terms of the MIT license.
See the [LICENSE](LICENSE) file for details.
