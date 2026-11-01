<p align="center">
  <img src="project%20logo/animated%20logo%20small.gif" alt="Metaphor ReFantazio PC Android Super Hyper Turbo Fixes Enhanced Plus and Knuckles logo">
</p>

# Metaphor Refantazio PC Android Super Hyper Turbo Fixes Enhanced Plus and Knuckles

A performance mod for **Metaphor: ReFantazio** (PC version). It was made for playing the game on Android phones and tablets through Winlator / GameNative (Wine + DXVK + FEX), and it also runs on Windows where it provides performance improvements without altering the visual presentation (except far shadows running at half speed, hardly noticeable, can be turned off).
For Android upgrades were made in CPU handling, far meshes polygon count, removal of the white light rim around models (causes shimmering), Dynamic LOD, dropping SMAA from Ultra to High and more.

It is an ASI plugin: no game file is changed. It patches the game's code and shaders in memory while the game runs, and one INI file controls everything.

Also the logo has flames, so you know it makes the game run faster.

## AI Disclaimer

I am not a programmer, I just enjoy modding games sometimes, this mod was fully vibe-coded by Claude Opus 5.5 with my full supervision in every test to assess and calibrate the output quality. You can find all included and rejected functions in [RESULTS.md](RESULTS.md).

## What it gives

Measured on a RedMagic Astra tablet (Snapdragon 8 Elite) in the game's heaviest common scene (the Sunlumeo street in the capital), 1600×900, 30 FPS limit, in one session switching between the game as it is and the mod:

| | The game as it is | With MetaphorCPU |
|---|---|---|
| Frame rate | 29.6 FPS (28.6 once the tablet throttles) | **29.9 FPS** (the 30 FPS limit holds) |
| Battery power | 9.9 W | **7.5 W (−25%)** |
| Energy per frame | 340 mJ | **250 mJ (−26%)** |
| GPU | 87–99% busy, 607–832 MHz | **82% busy at 443 MHz** |

What it changes, in short:

* **No visible change:** shadow casters that cannot reach a shadow cascade are skipped; the CPU waste the engine has under Wine (spinlocks, `Sleep(0)` loops, timer calls, a job semaphore that wakes every worker) is fixed (Android only).
* **Hardly visible** (Android defaults): simpler far meshes made by the mod, far shadows updated every 2nd frame and ending at 100 m, far NPCs animated every 2nd frame, the game's simple NPC models from 60 m (closer when the frame rate drops), SMAA High instead of Ultra, glare off, character outlines within 40 m, no toon rim light.
* **Optional, `SteadyImage=on`:** a steadier picture: each frame is blended with the previous one, following the camera and the characters' movement, so laces, hair, thin lines and railings shimmer much less. It costs about 2 ms of GPU work per frame on the tablet.

Every change with its measured saving, what it looks like and how it was tested: **[RESULTS.md](RESULTS.md)**. Comparison shots are available in [VisualComparison.md](VisualComparison.md) .

## Install

It works with the updated steam PC build of the game from April 2025.

**PC and Android**

1. I suggest to download [MetaphorFix](https://codeberg.org/Lyall/MetaphorFix), you will get 3 files (`MetaphorFix.ini`, `MetaphorFix.asi`, and `winmm.dll` which acts as the loader for the code in the asi files). Otherwise (Not recommended) If you don't want MetaphorFix download the x64 [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases) and put it in the game folder as `winmm.dll`.
2. Download also **`MetaphorCPU.asi`** and **`MetaphorCPU.ini`** (from the release section of this mod).
3. Place all the 5 files (`MetaphorFix.ini`, `MetaphorFix.asi`, `winmm.dll`, `MetaphorCPU.asi`, `MetaphorCPU.ini`) next to `METAPHOR.exe` in the game folder. On android you can open the game container and copy the files in the desktop environment, or if you use GameNative, you can click on the gear icon in the title card, select import files, select the files on your device and click import to automatically place them next to the exe. On PC you can find the folder by going to Steam > Library > right click the game > manage > browse local files.
4. On PC you can start the game, on SteamDeck or android you must set a DLL override (see below).

**Android (Winlator / [WinNative](https://github.com/WinNative-Emu/WinNative) / [GameNative](https://github.com/utkarshdalal/GameNative)):**

* After doing the above.
* Set the DLL override `winmm=n,b` (for example `WINEDLLOVERRIDES=winmm=n,b` in the container's environment variables).
* (Recommended) In the container's **Processor Affinity, leave every core ticked**: this gave the best performance AND power efficiency.
* Start the game. The mod works if `MetaphorCPU.log` appears next to the game exe; its first lines show the profile in use and the patches found.
* (Recommended) In the game, set Ambient occlusion `off` , Frame rate limit to `30`, V-sync `off`, Antialiasing choose `SMAA`.

The mod notices Wine by itself and uses its Android settings there (`[General] Profile=auto`).

**To uninstall,** delete `MetaphorCPU.asi` the `.ini` and `.log`.

## Other Android recommendations

In the game: 
- Leave texture quality at high if you have at least 12GB RAM, it makes 0 difference on performance (tested).

In the android app settings (eg GameNative):
- Game seems to perform better on proton 10 than the latest proton 11 (needs more testing).
- WinNative usually does what follows automatically, but if you are using another program ...
- Grab a recent GPU Turnip driver from [StevenMXZ](https://github.com/StevenMXZ/Adreno-Tools-Drivers/releases) (recommended patched version) for your specific mobile GPU and load it. You can also use a [driver from the WinNative developers](https://github.com/WinNative-Emu/Drivers/releases) in its p version to force higher clocks in your GPU (this obviously raises your power consumption, so do this only if you can't reach your target FPS and if the GPU scaling isn't working).
- Also install and use the latest DXVK gplasync, find it [here](https://github.com/Arihany/WinlatorWCPHub) , latest is 3.1.1 at the time of writing.
- If you use WinNative turn on SGSR in the FX tab for better sharpness. I suggest to try an equivalent upscaler in your chosen app.
- On RedMagic Astra (Snapdragon 8 elite, 9 inch screen) the game ran fine at 900p with no external cooler, please choose an appropriate resolution for your device and screen size.

Other changes:
- Open `MetaphorFix.ini` in a text editor and set `[Shadow Quality]` to `Resolution = 1024` and also `[Disable Camera Shake]` to `Enabled = true` (reduces shimmering). Leave LOD at 10 to let MetaphorCPU handle LOD Dynamically.

## Settings

`MetaphorCPU.ini` explains each option in a line or two. The defaults are the tested ones for each device. `auto` means "the default for this device". The options you are most likely to change are at the top of `[Quality]`: `SteadyImage`, `Smaa`, the outlines and the NPC distance.
Much time was given to dial in the best settings to give the best presentation for the small android screens, so I suggest you just go out and play already! The steady image options reduce shimmering at the cost of performance and are off by default, I suggest to use them only if you have residual performance.

If you are really curious and want to test how the game performs at a different resolution just leave the game running for 10 minutes (I suggest leaving the game running at Sunlumeo street, standing still), grab the logs of your runs and have your AI of choice read the results to you. You can also use one of the tests in the presets folder (they are preconfigured ini files that switch settings about every 30 seconds, rename one of them to `MetaphorCPU.ini` and replace it) to see how the mod features affect performance on your specific device if you really feel like tinkering instead of playing the game already!!

## Building from source

On Windows 10 or 11 with Python 3.11 or later, in the project folder:

```bat
python -m venv .venv
.venv\Scripts\pip install -r requirements.txt
powershell -ExecutionPolicy Bypass -File build.ps1
```

The mod, the launcher and the tests go to `build\`. Add `-RunTests` to run the tests. Two kinds need the game and are skipped without it: the signature scan (`-GameExe <path to METAPHOR.exe>`) and the shader tests ([tests/data/README.md](tests/data/README.md)).

| Folder / file | What it holds |
|---|---|
| `src\` | The mod (C), and `src\shaders\` its own HLSL shaders (built into `shaders_gen.h` by `tools\compile_shaders.py`) |
| `config\MetaphorCPU.ini` | The INI shipped with the mod |
| `presets\` | INI files for test runs and play variants: copy one over `MetaphorCPU.ini` |
| `tests\` | Unit tests and test INIs (`build.ps1 -RunTests`) |
| `tools\` | Log analysis (`summarize_variants.py`) and the binary-analysis scripts used while developing |
| `launcher\` | A launcher that injects the mod without an ASI loader, for testing |
| `third_party\meshoptimizer\` | The mesh simplifier used for the far meshes |
| `RESULTS.md` | What every change saves and how it was tested |

## A note for the best PC experience
If your PC is capable enough you should use the [Luma framework](https://github.com/Filoppi/Luma-Framework/releases) and use DLSS if you have an Nvidia card to completely fix the antialiasing in the game and give a much better image, shoutout to the devs for the amazing work. This can also be used on Android to activate FSR but the overhead is too much and results inconsistent.

## Credits and licences

* MetaphorCPU is released under the MIT licence ([LICENSE](LICENSE)).
* [meshoptimizer](https://github.com/zeux/meshoptimizer) by Arseny Kapoulkine (MIT licence, `third_party\meshoptimizer\LICENSE.md`).
* MetaphorFix (by Lyall) and Luma are separate projects; the documents mention them where they were used alongside this mod.
* Winlator, WinNative and GameNative are projects of the respective authors and are not affiliated with this mod. A special thanks to all of them for making it possible to run the game on android.
* This project is not affiliated with Atlus or Sega. It contains no game files: no code, shaders or textures. The only pictures of the game are the screenshots in [VisualComparison.md](VisualComparison.md). A special thanks to the developers for making this amazing game.
