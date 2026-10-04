# Hasselblad X Unleashed

> A research project exploring and extending the in-camera capabilities of the Hasselblad X System.

**English** · [简体中文](README.md)

---

## 📌 Overview

This project explores the potential of Hasselblad X System camera firmware (X1D / X2D, etc.), implementing in-camera shooting and interaction features not offered by the factory — giving this precise medium-format camera a few more possibilities.

## 🚀 Progress

| Feature | Description | Status |
| --- | --- | :---: |
| Focus bracketing (depth-of-field bracketing) | Builds on the camera's existing sampling capability, automatically stepping the focus point across a burst for focus stacking in macro / product photography | ✅ Done |
| In-camera pixel shift merge | Uses sensor-shift multi-exposure and merges the high-resolution image directly on the camera body | ✅ Done |
| Direct wireless printing | The camera connects to a wireless printer and outputs photos directly, with no computer or phone in between | ✅ Done |
| In-camera NES games / DOOM | Runs an NES emulator and the classic game DOOM on the camera's display, with support for playing via a bridged PS5 (DualSense) controller | ✅ Done |
| In-camera wireless microphone | Supports wireless microphone audio input | ✅ Done |
| Video recording | Unlocks the camera body's video recording capability | 🚧 In progress |

> More features are being explored — feedback and discussion are welcome.

## 📝 TODO

- [ ] **Combine video recording with the wireless microphone audio track** — mux the wireless mic's audio directly into in-camera video recording.
- [ ] **Possible DJI RC control support** — explore controlling the camera via a DJI RC remote.

> These features will be open-sourced once they are polished. At that point, any parts involving potentially infringing or legally questionable exploits will be removed, keeping only publicly shareable, compliant implementation ideas.

## 📸 Screenshots

### Focus bracketing / Pixel Shift shooting modes

The in-camera shooting-mode menu, with new "Focus bracketing" and "Pixel Shift" options (the sensor-shift sequence drives IBIS).

![Shooting modes](assets/screenshots/pixel_shift.jpg)

### Stack merge

Pixel Shift merge and focus-bracketing merge are completed directly in-camera, output as TIF.

![Stack merge](assets/screenshots/stacks.jpg)

### In-camera wireless microphone

Connects directly to wireless mics such as the DJI MIC, with live battery, signal, level and dropped-frame monitoring, plus input gain, low-cut / high-cut and mute settings.

![Mic management](assets/screenshots/mic_1.jpg)
![Mic settings](assets/screenshots/mic_2.jpg)

### Direct wireless printing

The camera connects directly to a wireless printer to output photos (demo video):

🎬 [`assets/screenshots/print.mp4`](assets/screenshots/print.mp4)

### In-camera NES games / DOOM

Running an NES emulator and the classic game DOOM on the camera's display, with support for playing via a bridged PS5 (DualSense) controller (demo video):

🎬 [`assets/screenshots/nesgame.mp4`](assets/screenshots/nesgame.mp4)

## 🙏 Acknowledgements

Special thanks to **[@YuHaoyua](https://github.com/YuHaoyua)** for generously sharing the CIM firmware encryption scheme and the related keys. It is this spirit of openness that made this project possible:

👉 [hasselblad-cim-firmware-extractor](https://github.com/YuHaoyua/hasselblad-cim-firmware-extractor)

## ⚠️ Disclaimer

- To prevent anyone from profiting off an information gap, **this repository only shares workable ideas and approaches**. It does **not** cover remote command execution on the camera or any instructions for bypassing SELinux.
- This project is **for personal reference, learning, and research only**. Please do not use it for commercial purposes.
- The material is based on research using publicly available information and my own devices. If anything here is found to infringe on the legitimate rights of Hasselblad, please contact me and I will address it promptly.
