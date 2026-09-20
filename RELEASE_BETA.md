# Asphalt 5 HD - PS Vita Port (Beta Release) 🏎️💨

We are incredibly excited to announce the first public Beta release of **Asphalt 5** for the PS Vita! 

This port uses a custom Android wrapper (soLoader/FalsoJNI) to run the original ARMv6 Android executable natively on the Vita, fully hardware-accelerated via `vitaGL`. 

Since this is a **Beta release**, the game is fully playable from start to finish, but please keep in mind that there are still some minor bugs and details being ironed out.

### ✨ Key Features in this Beta:
* **Full Physical Controls Integration:** We've built a custom input router that seamlessly bridges Vita physical buttons with the game's native touch engine. It only works with the **Touch Buttons** control scheme (not Tilt or the drag-to-steer variants) — this is now forced automatically the very first time you run the port, before any save file exists.
  * **Menus:** Fully navigable using the **D-Pad** or **Left Analog Stick** (Up/Down). Use **❌ (Cross)** to select and **⭕ (Circle)** to go back.
  * **Title Screens:** Press any face button to instantly skip splash screens.
  * **In-Game Racing:** 
    * **D-Pad Left/Right, L/R Triggers, or Left Analog Stick** to Steer.
    * **❌ (Cross)** to trigger Nitrous.
    * **🟥 (Square)** to Brake / Drift.
    * **Start** to Pause (Circle no longer duplicates this in-race — it's menu-only Back now).
* **Custom Optimized Audio Engine:** The original Android game used heavy floating-point math for audio mixing, which crippled the Vita's CPU. We wrote a custom 32-bit fixed-point audio mixer from scratch with **Linear Interpolation**, providing perfectly smooth engine pitches, zero stuttering, and high-fidelity 48kHz audio output through the Vita's MAIN audio port. It follows the engine's own routing: looped sounds (engine hum, skids) track RPM live, long music tracks get a dedicated voice that SFX can never steal, and the master bus uses a soft limiter so heavy scenes don't square-wave.
* **Hardware Accelerated Graphics:** Running buttery smooth thanks to `vitaGL`.
* **Post-Race Flow:** Results screens advance with a tap anywhere, or with **❌ (Cross)** on the pad — held steering/nitro from the race is cleanly released at the state change, so no more stuck input after crossing the finish line.
* **Intro & Cutscene Video Playback:** The game's original MPEG-4 `.mp4` cutscenes (which the Vita has no hardware decoder for) play in software at a smooth **30 FPS** with in-sync audio — the intro trailer (`A5_Ultimate_VNFS_2.mp4`) and the in-race videos alike. Videos are decoded at half resolution and upscaled, so motion stays fluid; press **❌ (Cross)** or **Start** at any time to skip.
* **No Screen-Off Freeze:** The port resets the Vita's idle timer every frame, so the console's auto power-save can no longer turn the screen off and freeze the game while you read a menu.

### ⚠️ Known Issues / Beta Status:
* **Open issue — Touch Buttons icons still visible, and Nitro often needs several presses:** the on-screen brake/nitro icons are drawn even though physical controls work, and ❌ (Cross) frequently needs multiple presses before Nitro actually fires — **except** pressing 🟥 (Square, brake/drift) immediately before ❌ (Cross), which reliably triggers Nitro on the very first try. Root cause not isolated yet; we need a console log capturing that exact drift-then-nitro pattern to fix it for real instead of guessing.
* **Recent adjustments (build clean, not yet confirmed on real hardware):** left analog stick steering, a sharper internal render resolution (800x480 instead of 720x432), Touch Buttons forced as the default control scheme on first run, and Circle no longer opening the pause menu mid-race (Start already did that, so the overlap was removed).
* This is a Beta! You might encounter occasional UI quirks or unmapped buttons in very specific sub-menus. 
* Performance is mostly solid, but some heavy tracks might experience slight frame drops. Further optimizations are planned.
* The intro trailer needs its `.mp4` present in `ux0:data/asphalt5/data/` — otherwise it is skipped.
* Manual standby with the power button (sleep/resume) is still untested — leaving the game idle is safe, but putting the console to sleep mid-game may not resume cleanly yet.

### 🛠️ Installation Instructions:
1. Install `libshacccg.suprx` if you don't have it already (required for vitaGL).
2. Install the provided `Asphalt5.vpk`.
3. Extract the game data files (assets) into `ux0:data/asphalt5/`. *(Note: Data files are not provided with this release; you must extract them from your own legally obtained copy of the Android game).*
4. Launch the game and enjoy!

---
*A huge thanks to the PS Vita homebrew community and the vitaGL contributors for making this possible! Let us know your feedback and bug reports.*
