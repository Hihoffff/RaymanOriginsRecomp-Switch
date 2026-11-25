#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: bash switch/build.sh [--check] [--game-dir PATH] [--mac-tools]

Build the Switch port from your own extracted Xbox 360 game files.
The default input is private/game/. Each build writes to a new directory
under private/build-docker/. Use --check to validate the dump only.
Docker runs code generation, shader compilation and the Switch build.
Use --mac-tools to use the existing Apple Silicon host tools for generation.
EOF
}

fail() {
    printf 'Error: %s\n' "$*" >&2
    exit 1
}

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)
game="$root/private/game"
check=0
mac_tools=0
while [ "$#" -gt 0 ]; do
    case "$1" in
        --game-dir)
            [ "$#" -ge 2 ] && [ -n "$2" ] || fail "--game-dir needs a path."
            game=$2
            shift 2
            ;;
        --check) check=1; shift ;;
        --mac-tools) mac_tools=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) fail "Unknown argument: $1. See --help." ;;
    esac
done

[ -d "$game" ] || fail "Game folder not found: $game"
game=$(cd "$game" && pwd -P)
# The default layout also supports this project's original extraction folder.
if [ "$game" = "$root/private/game" ] && [ ! -f "$game/default.xex" ] &&
   [ -f "$game/extracted/default.xex" ]; then
    game="$game/extracted"
fi

for filename in default.xex bootsequence_X360.ipk menus_X360.ipk secure_fat.gf localisation/localisation.loc; do
    [ -s "$game/$filename" ] || fail "Missing or empty game file: $filename (in $game). Use an extracted dump and preserve its directories."
done
expected=53533ae1e2017cfbd20d954f0c04452bba915697fc5308271b30c9a02c07b8dd
if command -v sha256sum >/dev/null; then
    hash_output=$(sha256sum < "$game/default.xex")
else
    command -v shasum >/dev/null || fail "Install sha256sum or shasum to verify default.xex."
    hash_output=$(shasum -a 256 < "$game/default.xex")
fi
actual=${hash_output%% *}
if [ "$actual" != "$expected" ]; then
    printf 'Unsupported default.xex revision.\nExpected: %s\nActual:   %s\n' "$expected" "$actual" >&2
    exit 1
fi
printf 'Required game files found. default.xex SHA-256 matches the supported revision.\n'
[ "$check" -eq 0 ] || exit 0

for tool in docker git; do
    command -v "$tool" >/dev/null || fail "Required tool missing: $tool."
done

upstream="$root/upstream"
sdk="$upstream/tools/forks/rexglue-src"
nvk="$root/switch/nvk-switch"
renderer="$upstream/tools/native_renderer"
rexglue="$upstream/tools/rexglue/mac-arm64/bin/rexglue"
image="devkitpro/devkita64@sha256:1fc388c3a0d34bd2045a6dadcb1020e069d5f876a187fd705de14b4440c00282"
if [ "$mac_tools" -eq 1 ]; then
    [ "$(uname -s):$(uname -m)" = Darwin:arm64 ] ||
        fail "--mac-tools requires Apple Silicon macOS."
    command -v python3 >/dev/null || fail "--mac-tools requires Python 3."
    for tool in "$rexglue" "$upstream/tools/jumptables/rayman_jumptables" "$upstream/tools/diag/imagedump" \
                "$renderer/shaderprep" "$upstream/tools/forks/xenosrecomp/build/XenosRecomp/XenosRecomp" \
                "$upstream/tools/forks/xenosrecomp/thirdparty/dxc-bin/bin/arm64/dxc-macos"; do
        [ -x "$tool" ] || fail "Prepared host tool missing: $tool. See switch/README.md for the dependency setup."
    done
else
    for path in "$upstream/tools/XenonRecomp/thirdparty/fmt/CMakeLists.txt" \
                "$upstream/tools/forks/xenosrecomp/thirdparty/dxc-bin/bin/x64/dxc-linux"; do
        [ -f "$path" ] || fail "Missing dependency source: $path. See switch/README.md for dependency setup."
    done
fi
for path in "$sdk/CMakeLists.txt" "$nvk/include/vulkan/vulkan.h" \
            "$nvk/include/vulkan/vulkan_vi.h" "$nvk/lib/libvulkan.a"; do
    [ -f "$path" ] || fail "Prepared Switch dependency missing: $path. See switch/README.md."
done
pch_template="$sdk/resources/templates/codegen/pch_h.inja"
grep -q '^#if REX_PLATFORM_SWITCH$' "$pch_template" ||
    fail "ReXGlue's Switch memory patch is missing from the code generation template. See switch/README.md."
jobs=${RAYMAN_BUILD_JOBS:-8}
case "$jobs" in
    ''|0|0*|*[!0-9]*) fail "RAYMAN_BUILD_JOBS must be a positive integer." ;;
esac
docker_arch=$(docker info --format '{{.Architecture}}') ||
    fail "Start Docker and make sure this terminal can access it."
case "${RAYMAN_BUILD_PLATFORM:-$docker_arch}" in
    aarch64|arm64|linux/arm64) platform=linux/arm64 ;;
    x86_64|amd64|linux/amd64) platform=linux/amd64 ;;
    *) fail "The Switch toolchain container supports linux/arm64 and linux/amd64." ;;
esac

output="$root/private/build-docker"
mkdir -p "$output"
work=$(mktemp -d "$output/run.XXXXXX")
printf 'Build workspace: %s\n' "$work"
trap 'printf "Build failed. Workspace: %s\n" "$work" >&2' ERR
mkdir -p "$work/rex/templates/codegen" "$work/private/game" "$work/private/data" \
    "$work/tools/native_renderer" "$work/tools/forks" "$work/sdk-out"
for name in rayman_manifest.toml rayman_hooks.toml; do
    git -C "$upstream" show "c1bf7b90e95d476e63aa4a0b077905ff6dbde9bd:rex/$name" > "$work/rex/$name"
done
# The stock host SDK embeds a desktop template. Reuse the Switch template
# from the patched SDK so the generated PPC code sees Horizon memory aliases.
cp "$pch_template" "$work/rex/templates/codegen/pch_h.inja"
printf '\ntemplate_dir = "templates"\n' >> "$work/rex/rayman_manifest.toml"
for name in default.xex bootsequence_X360.ipk; do
    cp "$game/$name" "$work/private/game/$name"
done

if [ "$mac_tools" -eq 1 ]; then
    bash "$root/switch/host-tools/generate.sh" "$work" "$upstream/tools" "$rexglue"
else
    tools_image=rayman-build-tools:rexglue-0.10.0
    printf '\nPreparing Linux code generation tools\n'
    docker build --platform linux/amd64 --tag "$tools_image" "$root/switch/host-tools"
    docker run --rm --platform linux/amd64 --network none \
        --user "$(id -u):$(id -g)" --env HOME=/tmp \
        --mount "type=bind,src=$upstream,dst=/upstream,readonly" \
        --mount "type=bind,src=$root/switch/host-tools,dst=/host-tools,readonly" \
        --mount "type=bind,src=$work,dst=/output" \
        "$tools_image" bash /host-tools/linux.sh "$jobs"
fi
# The upstream script may exit successfully after skipping failed compiles.
shopt -s nullglob
shaders=("$work"/spirv_ubo/*.spv)
[ "${#shaders[@]}" -eq 35 ] || fail "Expected 35 shaders, got ${#shaders[@]}. See $work/shaders.log and $work/spirv_ubo/work."
for shader in "${shaders[@]}"; do
    [ -s "$shader" ] || fail "Empty shader: $shader"
done

printf '\n[3/4] Building NRO with devkitA64\n'
# ReXGlue writes libraries to out/ inside its source tree.
# Docker needs the nested mount point to exist before mounting the SDK read-only.
mkdir -p "$sdk/out"
docker run --rm --platform "$platform" --network none \
    --user "$(id -u):$(id -g)" --env HOME=/tmp \
    --mount "type=bind,src=$root/switch,dst=/work/switch,readonly" \
    --mount "type=bind,src=$sdk,dst=/work/upstream/tools/forks/rexglue-src,readonly" \
    --mount "type=bind,src=$work/sdk-out,dst=/work/upstream/tools/forks/rexglue-src/out" \
    --mount "type=bind,src=$renderer,dst=/work/upstream/tools/native_renderer,readonly" \
    --mount "type=bind,src=$work,dst=/output" \
    -w /work "$image" bash -ec '
        source /opt/devkitpro/switchvars.sh
        cmake -S /work/switch/game -B /output/nro-build -G Ninja \
            -DCMAKE_TOOLCHAIN_FILE=/work/switch/toolchains/switch-devkitA64.cmake \
            -DRAYMAN_GEN=/output/rex/generated/default \
            -DRAYMAN_SWITCH_NVK_ROOT=/work/switch/nvk-switch \
            -DREXGLUE_ENABLE_TRACY=OFF
        cmake --build /output/nro-build --target rayman_switch_nro --parallel "$1"
    ' _ "$jobs"

printf '\n[4/4] Preparing SD card files\n'
sd="$work/sd/switch/RaymanOriginsRecomp"
mkdir -p "$sd/game" "$sd/native/spirv_ubo"
cp "$work/nro-build/rayman_switch.nro" "$sd/rayman_switch.nro"
cp "${shaders[@]}" "$sd/native/spirv_ubo/"
cp -RL "$game/." "$sd/game/"
printf '\nBuild complete. Copy the switch/ folder from %s/sd/ to the root of your SD card.\n' "$work"
