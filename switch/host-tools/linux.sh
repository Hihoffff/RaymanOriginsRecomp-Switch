#!/usr/bin/env bash
set -euo pipefail
ulimit -c 0

jobs=$1
tools=$(mktemp -d /tmp/rayman-host-tools.XXXXXX)
for dir in XenonRecomp jumptables diag native_renderer forks/xenosrecomp; do
    mkdir -p "$tools/$dir"
    tar -C "/upstream/tools/$dir" --exclude=.git --exclude=./build -cf - . |
        tar -C "$tools/$dir" -xf -
done

export CC=clang CXX=clang++
cmake -S "$tools/XenonRecomp" -B "$tools/XenonRecomp/build" -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build "$tools/XenonRecomp/build" --target XenonUtils disasm fmt --parallel "$jobs"
sh "$tools/jumptables/build.sh"
sh "$tools/diag/build.sh"

xenos="$tools/forks/xenosrecomp"
# Match the serial loop used by the verified Mac build.
sed -i 's/std::for_each(std::execution::par_unseq, /std::for_each(/' "$xenos/XenosRecomp/main.cpp"
cmake -S "$xenos" -B "$xenos/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_FLAGS='-include cstdlib -include cstdint -include algorithm'
cmake --build "$xenos/build" --target XenosRecomp --parallel "$jobs"
clang++ -std=c++17 -O2 -I "$xenos/thirdparty/xxHash" \
    "$tools/native_renderer/shaderprep.cpp" -o "$tools/native_renderer/shaderprep"

bash /host-tools/generate.sh /output "$tools" /opt/rexglue/linux-amd64/bin/rexglue
