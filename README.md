
# AudioSight: Directional Audio Overlay

<img width="1920" height="1080" alt="AudioSight" src="https://github.com/user-attachments/assets/3e06dc0f-0486-412a-8921-c57a9260ef01" />

A tiny Windows overlay that shows **where sounds come from** in a game: a left/right volume bar, a marker that remembers the direction of the last shot, and indicators on the screen edges that flash when a loud sound is detected on that side.

It was built as an **accessibility aid for players who are deaf in one ear or hard of hearing** and cannot localize sounds (gunshots, footsteps, explosions...) in shooters and other games.

<!-- Add a screenshot here, e.g. docs/screenshot.png -->
<!-- ![Screenshot](docs/screenshot.png) -->

- Single small `.exe`, **no installation, no dependencies**
- Works with any game: it reads the Windows audio output, not the game
- Written in plain C++ (Win32, WASAPI, GDI+)

## Features

| Element | What it does |
|---|---|
| **Volume bar** | LED-style bars showing the left and right output level in real time. |
| **Direction marker** | A marker under the bar that jumps to the direction of the **last detected shot** and stays there, so you can glance at it a second later. |
| **Edge indicators** | Green indicators on the left, right and top edges of the screen. Invisible by default; the one matching the direction of a shot lights up and fades out after about a second. A centered sound lights the top one. |
| **Settings panel** | Click the gear icon in the bar container: sliders for every threshold, a live level meter, and placement tools. |
| **Move mode** | Drag every element anywhere on screen to fit your game's HUD. Positions are saved. |

Everything is drawn in click-through, always-on-top, per-pixel-transparent windows, so it never blocks your mouse or steals focus from the game.

## Quick start

1. Download  [AudioSight.exe](https://github.com/Lespleiades/AudioSight/releases/tag/executable) or [build it yourself](#building).
2. Put it in its own folder (it creates `AudioSight.ini` next to itself) and run it.
3. Set your game to **Borderless Windowed** (see [Limitations](#limitations)).
4. Press **F8** to place the elements where you want them, press **F8** again to confirm.
5. Press **F7** (or click the gear) to open the settings and tune the sensitivity.

A notification area icon lets you open the settings or **Quit**.

## Hotkeys

Global hotkeys, they work while the game has focus.

| Key | Action |
|---|---|
| **F7** | Open / close the settings panel |
| **F8** | Move mode on / off (drag elements with the mouse, press again to save) |
| **F9** / **F10** | Volume bars more / less sensitive |
| **F11** / **F12** | Shot detection more / less sensitive |

> The gear icon is only clickable while the mouse cursor is **visible** (game menu, pause, or a key that frees the cursor). While the game hides the cursor, the whole overlay stays click-through. The hotkeys always work.

## Tuning the detection

Open the settings panel and play for a moment. The **live meter** shows the current sound level with three marks:

- purple tick: the volume bars threshold
- green tick: the shot detection threshold and the level of the last detected shot


| Setting | Meaning |
|---|---|
| Volume bars sensitivity | Level at which the bars start to show (right = quieter sounds appear). |
| Shot detection sensitivity | Minimum level for a sound to count as a shot. |
| Sound suddenness | How much a sound must rise above the background level. Lower = also accepts less abrupt sounds. |
| "Center" zone | Left/right difference (dB) under which a shot is classified as "center" (top indicator). |
| Indicator duration | How long an edge indicator stays visible. |

Settings and positions are stored in `AudioSight.ini` next to the executable. Adding `debug=1` under `[settings]` prints every detected shot's levels in the bar, which helps tuning.

## How it works

1. **Capture**: the audio output of the default device is captured with WASAPI loopback (shared mode). The program follows the default device if you switch headphones/speakers.
2. **Analysis**: every ~21 ms block, the energy of the left-side and right-side channels is computed in dB. Multichannel formats (5.1 / 7.1) are mapped using the device's channel mask; the center channel counts for both sides, the LFE is ignored.
3. **Shot detection**: a sound is a "shot" if it is above the shot threshold **and** rises abruptly above a slowly-tracked background level.
4. **Direction**: the left/right level difference of the detected sound decides left, right or center.

The program does **not** read or modify game memory, does not inject anything and does not hook the game. It only listens to the system audio output and draws its own windows.

## Limitations

- **Borderless Windowed required.** Overlays cannot be displayed over *exclusive* fullscreen games.
- **Stereo only gives left/right.** It cannot tell front from back: "top" means "centered", not necessarily "in front". Your own weapon sounds are often centered and will light the top indicator.
- Windows 10 / 11, 64-bit.
- Direction accuracy depends on how the game mixes its audio (stereo headphone output works best).

### Fair play

This tool only presents audio information visually and never reveals enemy positions. Still, rules differ between games, servers and tournaments: check that overlays and accessibility tools are allowed where you play.

## Windows Release

[AudioSight.exe](https://github.com/Lespleiades/AudioSight/releases/tag/executable)

## Building

Requirements: a C++17 compiler for Windows. Two options are provided.

### MinGW-w64 (recommended)

Install [MinGW-w64](https://www.mingw-w64.org/) or [w64devkit](https://github.com/skeeto/w64devkit), make sure `g++` is in your `PATH`, then:

```bat
build_mingw.bat
```

or manually:

```bat
g++ -O2 -s -mwindows -static -static-libgcc -static-libstdc++ -o build\AudioSight.exe src\AudioSight.cpp -lgdiplus -lgdi32 -luser32 -lole32 -lshell32 -luuid
```

### Visual Studio (MSVC)

Open an **x64 Native Tools Command Prompt for VS**, then:

```bat
build_msvc.bat
```

### Cross-compiling from Linux

```sh
sudo apt install g++-mingw-w64-x86-64
x86_64-w64-mingw32-g++ -O2 -s -mwindows -static -static-libgcc -static-libstdc++ \
    -o AudioSight.exe src/AudioSight.cpp \
    -lgdiplus -lgdi32 -luser32 -lole32 -lshell32 -luuid
```

The resulting executable only depends on system DLLs that ship with Windows.

## Troubleshooting

- **Windows says it cannot access the file / the .exe disappears**: your antivirus (Windows Defender, Avast...) probably flagged it. Small unsigned programs that register global hotkeys and draw overlays often trigger heuristic false positives. Restore the file from quarantine and add an exclusion for its folder. You can also build it yourself from the source, or report the false positive to your antivirus vendor.
- **Nothing is displayed over the game**: switch the game to *Borderless Windowed*.
- **No bars / no reaction**: make sure the sound is played on the Windows *default* output device.
- **Shots are missed or footsteps trigger the marker**: tune the shot sensitivity with the live meter (see above).
- **Only one instance can run**: if you start it twice, the second one just shows a message. Quit the first one from the notification area icon.

## Project structure

```
.
|-- src/AudioSight.cpp   # the whole program (single file)
|-- build_mingw.bat         # build script for MinGW-w64
|-- build_msvc.bat          # build script for Visual Studio
|-- README.md
|-- LICENSE
`-- .gitignore
```

## Ideas / contributions

Contributions are welcome. Some ideas:

- Front/back estimation when the game outputs real 5.1/7.1
- Frequency filtering to better separate footsteps from other sounds
- Size and color options for the indicators
- Multi-monitor support (currently the primary screen is used for default positions)

## License

MIT, see [LICENSE](LICENSE).
