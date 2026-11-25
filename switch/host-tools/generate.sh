#!/usr/bin/env bash
set -euo pipefail

work=$1
tools=$2
rexglue=$3
xenos="$tools/forks/xenosrecomp"
renderer="$tools/native_renderer"
game="$work/private/game"
data="$work/private/data"

printf '\n[1/4] Generating ReXGlue hints and C++\n'
"$tools/jumptables/rayman_jumptables" "$game/default.xex" \
    "$data/tables.toml" "$data/functions.toml" "$work/rex/rayman_hints.toml"
(cd "$work/rex" && "$rexglue" codegen rayman_manifest.toml)
test -s "$work/rex/generated/default/sources.cmake"
if ! grep -q '^#if REX_PLATFORM_SWITCH$' "$work/rex/generated/default/rayman_pch.h"; then
    printf 'Generated code is missing the Switch memory mapping.\n' >&2
    exit 1
fi

printf '\n[2/4] Generating SPIR-V shaders\n'
"$tools/diag/imagedump" "$game/default.xex" "$data/image.bin"
for name in hlsl_ubo.py shaderprep; do
    ln -s "$renderer/$name" "$work/tools/native_renderer/$name"
done
ln -s "$xenos" "$work/tools/forks/xenosrecomp"
shader_script="$work/tools/native_renderer/build_spirv.sh"
case "$(uname -s):$(uname -m)" in
    Darwin:arm64) cp "$renderer/build_spirv.sh" "$shader_script" ;;
    Linux:x86_64)
        # Keep upstream's shader pipeline and flags, selecting its Linux DXC.
        sed -e 's|^export DYLD_LIBRARY_PATH=\$DXC_DIR/lib/arm64$|export LD_LIBRARY_PATH=$DXC_DIR/lib/x64|' \
            -e 's|^DXC=\$DXC_DIR/bin/arm64/dxc-macos$|DXC=$DXC_DIR/bin/x64/dxc-linux|' \
            "$renderer/build_spirv.sh" > "$shader_script"
        grep -Fqx 'DXC=$DXC_DIR/bin/x64/dxc-linux' "$shader_script"
        ;;
    *) printf 'Unsupported code generation environment.\n' >&2; exit 1 ;;
esac
if ! sh "$shader_script" "$work/spirv_ubo" > "$work/shaders.log" 2>&1; then
    printf 'Shader generation failed. See shaders.log in the build workspace.\n' >&2
    exit 1
fi
tail -n 1 "$work/shaders.log"
