# Building for Nintendo Switch

The local build was verified on Apple Silicon macOS with devkitA64 GCC 15.2.0
in the ARM64 devkitPro Docker image. Generating the game sources and shaders
uses tools on the host. The upstream shader script selects macOS ARM64 DXC.
Linux, Intel macOS, and Windows have yet to be verified for the full workflow.
The standalone GOD extraction utility uses Python 3.

## Prepare the upstream code and your game dump

From the root of this repository, clone
[XDanfr/RaymanOriginsRecomp](https://github.com/XDanfr/RaymanOriginsRecomp) into
`upstream/` and check out `c1bf7b90e95d476e63aa4a0b077905ff6dbde9bd`.
Apply the renderer patches in numerical order:

    for patch in "$PWD"/switch/native-renderer-patches/[0-9][0-9][0-9][0-9]-*.patch; do
        git -C upstream apply "$patch" || exit 1
    done

Put the files from your own Xbox 360 dump in `upstream/private/game/`.
Preserve the original directory structure. The tested Switch port expects
a retail XEX with SHA-256
`53533ae1e2017cfbd20d954f0c04452bba915697fc5308271b30c9a02c07b8dd`.
The Switch graphics hooks use fixed addresses for this revision. Upstream's
retail XEX revision differs and needs a separate hook port.

If your own dump is in the Xbox 360 GOD format, the included
[extraction tool](../tools/god_extract.py) can unpack it.

## Generate game code and shaders locally

The upstream `tools/jumptables` tool generates ReXGlue hints. From the root
of this repository:

    mkdir -p upstream/private/data
    (cd upstream && sh tools/jumptables/build.sh)
    upstream/tools/jumptables/rayman_jumptables upstream/private/game/default.xex \
        upstream/private/data/tables.toml upstream/private/data/functions.toml \
        upstream/rex/rayman_hints.toml

Use the host ReXGlue SDK v0.10.0 to generate sources:

    (cd upstream/rex && rexglue codegen rayman_manifest.toml)

This writes `upstream/rex/generated/default/`. Generate the SPIR-V shaders
with the steps in upstream's
[native renderer guide](https://github.com/XDanfr/RaymanOriginsRecomp/blob/c1bf7b90e95d476e63aa4a0b077905ff6dbde9bd/docs/NATIVE_RENDERER.md).
The full build from a fresh clone and dump is still pending verification.

## Build the Switch runtime and NRO

Place the [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) v0.10.0
checkout at `upstream/tools/forks/rexglue-src`. Set up the SDK dependencies,
apply the upstream Android ReXGlue patches, then apply this project's Switch
ReXGlue patches in numerical order. From this repository's root:

    sdk="$PWD/upstream/tools/forks/rexglue-src"
    git -C "$sdk" am "$PWD"/upstream/android/rexglue-patches/*.patch
    git -C "$sdk" am "$PWD"/switch/rexglue-patches/[0-9][0-9][0-9][0-9]-*.patch
    git -C "$sdk/thirdparty/FFmpeg" apply "$PWD/switch/rexglue-patches/submodule-FFmpeg.diff"
    git -C "$sdk/thirdparty/spdlog" apply "$PWD/switch/rexglue-patches/submodule-spdlog.diff"
    cp switch/rexglue-patches/FFmpeg-config_switch_aarch64.h "$sdk/thirdparty/FFmpeg/config_switch_aarch64.h"

Build [mesa-switch](https://github.com/danfromtico/mesa-switch) with its build
script and use `switch/make_nvk_package.sh /path/to/mesa-switch` to prepare
the local NVK package. The CMake build reads it from `switch/nvk-switch/`.
Build the NRO with the existing ARM64 Docker wrapper:

    bash switch/docker.sh 'cmake -S switch/game -B switch/game/build -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE=/work/switch/toolchains/switch-devkitA64.cmake \
        -DREXGLUE_ENABLE_TRACY=OFF \
        -DRAYMAN_GEN=/work/upstream/rex/generated/default \
        -DRAYMAN_SWITCH_NVK_ROOT=/work/switch/nvk-switch'
    bash switch/docker.sh 'cmake --build switch/game/build --target rayman_switch_nro'

The output is `switch/game/build/rayman_switch.nro`. It contains
recompiled game code and is for local use.

## Load on your Switch

Use the full-memory homebrew launch mode. Place the NRO under
`sdmc:/switch/RaymanOriginsRecomp/`, the game files under `game/`, and the
locally generated shaders under `native/spirv_ubo/` in that same directory.
Diagnostic builds write session logs under `logs/`.
