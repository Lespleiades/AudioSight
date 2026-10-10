
# AudioSight: Directional Audio Overlay

<img width="800" height="450" alt="AudioSight" src="https://github.com/user-attachments/assets/29162396-5154-4411-a4d6-8089892aade8" />


A overlay that shows where sounds come from in a shooter game.
It was built as an accessibility aid for players who are deaf or hard of hearing.

<!-- Add a screenshot here, e.g. docs/screenshot.png -->
<!-- ![Screenshot](docs/screenshot.png) -->

## Quick start

1. Download latest release: [AudioSight.exe](https://github.com/Lespleiades/AudioSight/releases) or [build it yourself](#building).
2. Put it in its own folder (it creates `AudioSight.ini` next to itself) and run it.
3. Set your game to **Borderless Windowed** (see [Limitations](#limitations)).
4. Press **F8** to place the elements where you want them, press **F8** again to confirm.
5. Press **F7** (or click the gear) to open the settings and tune the sensitivity.

## Important

Select **Borderless Windowed** in your options game menu.

## Hotkeys

Global hotkeys, they work while the game has focus.

| Key | Action |
|---|---|
| **F7** | Open / close the settings panel |
| **F8** | Move mode on / off (drag elements with the mouse, press again to save) |
| **F9** / **F10** | Volume bars more / less sensitive |
| **F11** / **F12** | Shot detection more / less sensitive |

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

## Cautions

This tool **only presents audio information from your device audio output**. Still, rules differ between games, servers and tournaments: **check that overlays and accessibility tools are allowed where you play**.

## Windows Release

[AudioSight.exe](https://github.com/Lespleiades/AudioSight/releases)

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
