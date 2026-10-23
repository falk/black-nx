# black-nx

An unofficial Nintendo Switch port of the original Xbox version of **BLACK** (2006), made by static recompilation with native hot paths and a native deko3d renderer. The output is a homebrew `.nro`.

It is built on [Xbox XBE Recomp](https://github.com/TGP482/XboxXBE-Recomp) and TGP482's [BLACK recompilation](https://github.com/TGP482/BLACK-XBERecomp). The game's x86 code is translated to C++ ahead of time. The runtime provides the Xbox kernel, XDK and Direct3D 8. The NV2A draw state goes to the Switch GPU through deko3d.

> [!NOTE]
> This project does not include any game files. You need the complete files from your own legally acquired copy of the game, the **USA** release.

## Status

- Controller input, rumble, stereo audio and films.
- Dynamic resolution up to 1280x720, mostly at 60 fps. Performance varies by scene.
- The first visit to a scene can stutter briefly while its shaders compile. After that they are cached on the SD card.

## Installing

1. Build `black.nro` from your own copy of the game (see [Building](#building)). No builds are distributed.
2. On the SD card, make this layout:
   ```text
   sdmc:/switch/black/black.nro
   sdmc:/switch/black/game/default.xbe
   sdmc:/switch/black/game/data/
   sdmc:/switch/black/game/levels/
   sdmc:/switch/black/game/sound/
   sdmc:/switch/black/game/videos/
   ```
   Copy the **entire extracted disc**, including its other files and folders. A missing film or level file can stop the game at a disc-error screen.
3. Start it from the Homebrew Menu opened through a game: hold **R** while launching any game. Applet mode (the Album) does not have enough memory.

The game creates its shader cache, logs and emulated hard-disk saves under `sdmc:/switch/black/`.

## Controls

| Switch | Xbox |
|---|---|
| Left / right stick | Move / look |
| A, B, X, Y | The matching Xbox button labels |
| ZL / ZR | Left / right trigger |
| L / R | White / black button |
| + / − | Start / Back |
| Stick clicks, D-pad | The matching Xbox controls |

## Building

The game's code is translated from your own `default.xbe` at build time. This repository and the build tools contain no game code, so every build needs your copy of the game.

Builds run in Docker. Only [Docker](https://docs.docker.com/get-docker/) and git are needed, on Linux, macOS or Windows (WSL2).

```sh
git clone https://github.com/falk/black-nx.git BLACK-XBERecomp
cd BLACK-XBERecomp
mkdir -p assets
cp /path/to/your/game/default.xbe assets/    # from your own USA disc
docker/build.sh                              # -> out/switch-deko/black.nro
```

The first run builds the `black-nx-builder` image: devkitPro's devkitA64 with libnx and deko3d, the host tools, the recompiler SDK at the pinned commit, and the deko3d shader compiler. That takes a few minutes. Then it recompiles `default.xbe` into `generated/` and builds the NRO, which takes a while the first time. Later runs reuse the image and only rebuild what changed.

The image never contains the game: `default.xbe` is read from your `assets/` folder, mounted into the container while it builds. `PROF=0 docker/build.sh` leaves the profiler out. `JOBS=<n>` sets the number of parallel compiles.

### Run over the network

For quick testing, send the NRO over the network: open the Homebrew Menu, press **Y** for the netloader, then run

```sh
$DEVKITPRO/tools/bin/nxlink -a <switch-ip> -s out/switch-deko/black.nro BLACK_PERF_DIAG=1
```

The game files still need to be on the SD card. `-s` prints the game's log in your terminal. Arguments of the form `NAME=value` set these options:

| Option | Effect |
| --- | --- |
| `BLACK_PERF_DIAG=1` | Frame time, GPU time and fps log |
| `BLACK_PROF=1` | Sampling profiler of the game thread, with function names |
| `BLACK_SKIP_FILMS=1` | End every film as it starts, except the menu backgrounds |

### Tests

`tests/` has host tests for the native hot paths and the input and renderer helpers. Each is a standalone C++ program, for example:

```sh
g++ -std=c++20 -O2 -Itests/switch_mock -I../XboxXBE-Recomp/runtime tests/switch_input_test.cpp switch/headless/switch_input.cpp -o out/switch-input-test && out/switch-input-test
```

## Layout

| Path | Contents |
| --- | --- |
| `config/game.toml` | Recompiler configuration: functions, partitions, hooks |
| `src/game/` | Native replacements for hot functions, and game hooks |
| `src/d3d/` | Direct3D 8 for the inlined XDK code, and the render thread |
| `switch/deko3d/` | The deko3d renderer: NV2A shader translation, shader cache, draw state |
| `switch/headless/` | Switch platform layer: entry, input, audio, clocks, profiler |
| `docker/` | The Docker build environment and `docker/build.sh` |
| `switch/tools/` | Recompiler build, profiler symbols and reports, test helpers |
| `tools/` | Build helpers, among them `mmx2c.py`, which translates the video decoder's IDCTs from your `default.xbe` |
| `tests/` | Host tests |

## Credits

- [TGP482](https://github.com/TGP482): the original BLACK recompilation and Xbox XBE Recomp.
- The [CoD4 Switch port](https://github.com/falk/KisakCOD-nx): deko3d and Horizon platform reference.
- [portingpete/Black-2006-Recomp](https://github.com/portingpete/Black-2006-Recomp): additional reference.
- [deko3d](https://github.com/devkitPro/deko3d), [UAM](https://github.com/devkitPro/uam) and [libnx](https://github.com/switchbrew/libnx).
- BLACK is © Electronic Arts / Criterion Games. This project is not affiliated with or endorsed by them.
