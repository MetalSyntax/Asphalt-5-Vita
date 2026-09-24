# Asphalt 5 HD - PS Vita Port (v1.0) 🏎️💨

**Asphalt 5** for PS Vita is out of beta. The game is fully playable from start to finish with
physical controls, full audio and cutscene video.

This port uses a custom Android wrapper (soLoader/FalsoJNI) to run the original ARMv6 Android
executable natively on the Vita, fully hardware-accelerated via `vitaGL`.

### 🔧 Fixed since the Beta
* **Nitro works on a single press.** Before, ❌ (Cross) often needed several presses unless you
  were also steering or braking. The engine only reads a nitro press during its 25 Hz game-logic
  step, and on the Vita some frames run no step at all, so the press got lost. It's now held
  until the car logic reads it.
* **The drift skid no longer gets stuck** after a drift ends (before, it kept playing until you
  paused).
* **Race music no longer plays twice** on top of itself.
* **Scenery ambient loops tamed.** A "revving + skidding" clip tied to some animated track
  objects looped at full volume from far away and sounded like a phantom car. It now fades out
  much faster with distance and never goes above 50%.
* **Brake and nitro on-screen icons at ~1% opacity**, since the physical buttons drive them.
  Press **SELECT** during a race to show or hide them.
* **Exit closes the app** from the pause menu and the main menu.
* **Cutscene video at ~30 FPS** (was ~10), still skippable with ❌ or Start.
* **No screen-off freeze** while idling in menus.
* **Left analog stick steering**, a sharper internal render resolution (800x480), Touch Buttons
  as the default control scheme on a fresh install, and Circle no longer duplicating Start's
  pause mid-race.

### ✨ Features
* **Full physical controls.** A custom input router bridges the Vita's buttons with the game's
  native touch engine. It only works with the **Touch Buttons** control scheme, which is set
  automatically on a fresh install.
  * **Menus:** **D-Pad** or **Left Analog Stick** (Up/Down). **❌ (Cross)** selects, **⭕ (Circle)**
    goes back.
  * **Title screens:** any face button skips.
  * **Racing:**
    * **D-Pad Left/Right, L/R Triggers, or Left Analog Stick** to steer.
    * **❌ (Cross)** for nitro.
    * **🟥 (Square)** to brake / drift.
    * **Start** to pause.
    * **Select** to show / hide the on-screen brake & nitro icons.
* **Custom audio engine.** A 32-bit fixed-point mixer with linear interpolation at 48 kHz. It
  follows the engine's routing: looped engine and skid sounds track RPM live, music has a
  dedicated voice that SFX can't steal, and a soft limiter keeps busy scenes clean.
* **Hardware-accelerated graphics** via `vitaGL`.
* **Post-race flow.** Results screens advance with a tap or ❌. Held race inputs are released
  cleanly at the state change.

### ⚠️ Known Issues
* Only the **Touch Buttons** control scheme works with physical controls. Don't switch to Tilt or
  the drag schemes in Options.
* Manual standby (power button sleep/resume mid-game) is untested. Idling is safe.
* Heavy tracks may drop a few frames at busy moments.
* The intro trailer needs its `.mp4` in `ux0:data/asphalt5/data/`. Otherwise it's skipped.

### 🛠️ Installation Instructions
1. Install `libshacccg.suprx` if you don't have it already (required for vitaGL).
2. Install the provided `asphalt5.vpk`.
3. Extract the game data files (assets) into `ux0:data/asphalt5/`. *(Data files are not
   provided with this release. You must extract them from your own legally obtained copy of the
   Android game.)*
4. Launch the game and enjoy!

---
*A huge thanks to the PS Vita homebrew community and the vitaGL contributors for making this possible! Let us know your feedback and bug reports.*
