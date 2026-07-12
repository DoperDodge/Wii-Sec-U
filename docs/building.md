# Building

## Prebuilt PC binaries (no toolchain needed)

Every green CI run on `main` uploads ready-to-run packages: repo →
**Actions** → newest run → **Artifacts**:

- **`wsu-windows-x64`** — unzip anywhere; contains `wsu.exe` +
  `SDL2.dll` (keep them together). It's a command-line app: open a
  terminal in that folder and run `wsu.exe --help`. Windows SmartScreen
  may warn on first run (unsigned binary) — choose "Run anyway".
- **`wsu-linux-x64`** — needs the system SDL2 runtime
  (`sudo apt install libsdl2-2.0-0`), then `chmod +x wsu && ./wsu --help`.
- **`wiiu-plugins`** — the two `.wps` files for the console.

Note: downloading artifacts from the Actions page requires being logged
in to GitHub.

## PC app (`pc/`)

Requirements: CMake ≥ 3.16 and a C++17 compiler (GCC/Clang/MSVC). The
core has **no mandatory external dependencies** — sockets and threads come
from the OS.

```sh
cmake -S pc -B pc/build -DCMAKE_BUILD_TYPE=Release
cmake --build pc/build --parallel
ctest --test-dir pc/build --output-on-failure
```

Outputs `pc/build/wsu` (`wsu.exe` on Windows).

Options:

| CMake option | Default | Effect |
|---|---|---|
| `WSU_WITH_SDL` | `OFF` | Builds the SDL2 game-controller input backend (`--input sdl`) **and the display window with audio** (`--display sdl`) — you want this ON for actually playing. Requires SDL2 dev packages (`libsdl2-dev` on Ubuntu, `vcpkg install sdl2` on Windows, `brew install sdl2` on macOS). |
| `WSU_BUILD_TESTS` | `ON` | Unit + integration tests (SDL builds add a headless display test using SDL's dummy drivers). |

MJPEG decoding for the display uses the vendored stb_image
(`pc/third_party/stb/`) — no FFmpeg or system codec needed.

Windows note: any recent Visual Studio works — `cmake -S pc -B pc/build`
then `cmake --build pc/build --config Release`. WinSock is linked
automatically.

## Wii U plugins (`wiiu/`)

The plugins target the [Aroma](https://aroma.foryour.cafe/) environment
and build with devkitPPC + [wut](https://github.com/devkitPro/wut) +
[WUPS](https://github.com/wiiu-env/WiiUPluginSystem). The provided Docker
image pins the same toolchain the wiiu-env plugin ecosystem uses:

```sh
docker build -t wsu-wiiu-builder -f wiiu/Dockerfile .
docker run --rm -v "$PWD":/project wsu-wiiu-builder make -C wiiu/input-plugin  -j"$(nproc)"
docker run --rm -v "$PWD":/project wsu-wiiu-builder make -C wiiu/stream-plugin -j"$(nproc)"
```

Outputs `wiiu/input-plugin/wsu-input.wps` and
`wiiu/stream-plugin/wsu-stream.wps`.

Building natively instead: install devkitPPC + wut via devkitPro pacman,
install WUPS per its README, add the `ppc-libjpeg-turbo` portlib
(`dkp-pacman -S ppc-libjpeg-turbo`, needed by wsu-stream), then run the
same `make` commands with `DEVKITPRO` set.

## Installing on the console

Copy both `.wps` files to `sd:/wiiu/environments/aroma/plugins/` and boot
Aroma. Optional config files on the SD card:

```
sd:/wiiu/wsu-input.cfg     # port=4404
sd:/wiiu/wsu-stream.cfg    # width=428 height=240 fps=20 quality=60 port=4406 audio=1
```

Both plugins log through the system log and WHB's UDP logger — run
`udplogserver` from wut's tools on any LAN PC to watch them.

## Continuous integration

`.github/workflows/ci.yml` builds and tests the PC app on Linux and
Windows and builds both plugins in the Docker image, uploading the `.wps`
files as workflow artifacts.
