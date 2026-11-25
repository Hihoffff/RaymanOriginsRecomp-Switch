# Building RaymanOriginsRecomp for Nintendo Switch

## Build with Docker

Use Docker with Linux containers and Git. On Windows, use the
[Docker Desktop WSL2 backend](https://docs.docker.com/desktop/features/wsl/)
and run the commands in an integrated WSL2 terminal. Keep the checkout in
the WSL filesystem. The output is a Nintendo Switch NRO.

1. **Prepare the source dependencies once.** Follow
   [Prepare dependencies](#prepare-dependencies) below.
2. **Prepare your own game dump.** Put the extracted files in `private/game/`,
   with `default.xex` at the root. Preserve the original directory structure.
   To validate the input, run `bash switch/build.sh --check`. The Switch
   hooks use the retail revision with SHA-256
   `53533ae1e2017cfbd20d954f0c04452bba915697fc5308271b30c9a02c07b8dd`.
3. **Build the port.** From the repository root, run:

       bash switch/build.sh

   Use `--game-dir /path/to/game` if the dump is elsewhere. The script checks
   the executable revision and required files, builds host tools in a Linux
   x86-64 container, generates C++ and shaders there, then builds the NRO
   with devkitA64. The devkitA64 container uses the Docker engine's CPU
   architecture. Apple Silicon requires Docker support for running x86-64
   Linux containers for the code generation step.

The full x86-64 container workflow was verified on Apple Silicon with
OrbStack and prepared source dependencies. A native Linux host, Windows
with WSL2, and a Switch run of the resulting NRO remain to be verified.

## Install on Switch

The script prints its run directory under `private/build-docker/`. After
a successful build, copy the `switch/` folder from the run's `sd/` folder
to the root of your SD card. Back up any existing saves before replacing
files. Launch the port from the Homebrew Menu in full-memory application mode.

If the build fails, use the reported run directory. Shader compiler output
is saved in `shaders.log`. If Docker is short of memory, lower the number
of parallel compiler jobs with `RAYMAN_BUILD_JOBS=4 bash switch/build.sh`.

On Apple Silicon, `bash switch/build.sh --mac-tools` is the fallback for a
checkout with the previously prepared macOS ReXGlue SDK 0.10.0, jumptables,
imagedump, shaderprep, XenosRecomp, and DXC binaries.

## Prepare dependencies

Run these commands from the repository root on a fresh checkout:

```sh
git clone https://github.com/XDanfr/RaymanOriginsRecomp.git upstream
git -C upstream checkout --detach c1bf7b90e95d476e63aa4a0b077905ff6dbde9bd
git -C upstream submodule update --init --recursive tools/XenonRecomp

git clone https://github.com/hedge-dev/XenosRecomp.git upstream/tools/forks/xenosrecomp
git -C upstream/tools/forks/xenosrecomp checkout --detach 990d03b28a27b50277ee5d8d942e1c5f873869d1
git -C upstream/tools/forks/xenosrecomp submodule update --init

for patch in "$PWD"/switch/native-renderer-patches/[0-9][0-9][0-9][0-9]-*.patch; do
    git -C upstream apply "$patch" || exit 1
done
```

### ReXGlue Switch runtime

Git needs a committer name and email to apply the patch series with
`git am`. The resulting dependency checkout is local.

```sh
sdk="$PWD/upstream/tools/forks/rexglue-src"
git clone --recurse-submodules --branch v0.10.0 https://github.com/rexglue/rexglue-sdk.git "$sdk"
git -C "$sdk" am "$PWD"/upstream/android/rexglue-patches/*.patch
git -C "$sdk" am "$PWD"/switch/rexglue-patches/[0-9][0-9][0-9][0-9]-*.patch
git -C "$sdk/thirdparty/FFmpeg" apply "$PWD/switch/rexglue-patches/submodule-FFmpeg.diff"
git -C "$sdk/thirdparty/spdlog" apply "$PWD/switch/rexglue-patches/submodule-spdlog.diff"
cp switch/rexglue-patches/FFmpeg-config_switch_aarch64.h "$sdk/thirdparty/FFmpeg/config_switch_aarch64.h"
```

### NVK graphics driver

Build [mesa-switch](https://github.com/danfromtico/mesa-switch) at
`0ff52079ca9e9e837ed27eb95a8d239f439a1864` with its Docker build script.
Then package the driver for the Switch target:

```sh
git clone --filter=blob:none --no-checkout https://github.com/danfromtico/mesa-switch.git ../mesa-switch
git -C ../mesa-switch checkout --detach 0ff52079ca9e9e837ed27eb95a8d239f439a1864
(cd ../mesa-switch && bash build-switch.sh)
DEVKITA64_PLATFORM=native bash switch/make_nvk_package.sh ../mesa-switch
```

The NVK package is stored locally in `switch/nvk-switch/`.

## Manual NRO build

If you already have generated game sources, you can run the Switch
CMake build directly:

```sh
bash switch/docker.sh 'cmake -S switch/game -B switch/game/build -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE=/work/switch/toolchains/switch-devkitA64.cmake \
    -DRAYMAN_GEN=/work/upstream/rex/generated/default \
    -DRAYMAN_SWITCH_NVK_ROOT=/work/switch/nvk-switch \
    -DREXGLUE_ENABLE_TRACY=OFF'
bash switch/docker.sh 'cmake --build switch/game/build --target rayman_switch_nro'
```

Keep the resulting `switch/game/build/rayman_switch.nro` and generated
game files local. The NRO contains recompiled game code.

## Switch diagnostics

Diagnostic builds write session logs to `logs/` in the port's SD folder.
A crash may also produce `crash.log`. Place an empty file in that folder
to run one of the following modes before launching the game:

- `selftest_only` runs the startup memory and threading checks and exits.
- `vulkan_probe` lists the Vulkan devices and extensions.
- `vulkan_clear` opens a Vulkan surface and displays a test color sequence.

For live logs, use `switch/nxlink.sh <console-ip>` with NetLoader open
in the Homebrew Menu.
