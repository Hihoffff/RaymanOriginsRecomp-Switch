#!/usr/bin/env bash
# Собирает переносимый NVK-пакет switch/nvk-switch/ из результатов сборки
# mesa-switch (https://github.com/danfromtico/mesa-switch, см. docs/sessions,
# шаг 3.0–3.1 плана docs/PLAN_STAGE3_GRAPHICS.md).
#
# Использование:
#   switch/make_nvk_package.sh /path/to/mesa-switch
#
# Пакет (НЕ коммитится, в .gitignore):
#   switch/nvk-switch/include/vulkan/...   (заголовки Mesa, включая vulkan_vi.h)
#   switch/nvk-switch/lib/libvulkan.a      (merged-архив NVK)
#   switch/nvk-switch/lib/libdl.a          (пустая заглушка для линковки)
# zlib/zstd/expat CMake берёт из devkitPro portlibs (fallback в CMakeLists).

set -euo pipefail

MESA=${1:?укажи путь к клону mesa-switch с собранным builddir-switch}
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$REPO_ROOT/switch/nvk-switch"

LIB="$MESA/builddir-switch/src/nouveau/vulkan/libvulkan.a"
HDR="$MESA/include/vulkan/vulkan.h"
VI_HDR="$MESA/include/vulkan/vulkan_vi.h"
for f in "$LIB" "$HDR" "$VI_HDR"; do
    [ -f "$f" ] || { echo "нет $f — сначала собери mesa-switch (build-switch.sh)" >&2; exit 1; }
done

mkdir -p "$OUT/include" "$OUT/lib"
rm -rf "$OUT/include/vulkan"
cp -R "$MESA/include/vulkan" "$OUT/include/vulkan"
cp "$LIB" "$OUT/lib/libvulkan.a"

# libdl на Horizon отсутствует; NVK его не вызывает (проверено nm -u), но
# контракт пакета его требует — кладём пустую заглушку, как build-switch.sh
# mesa-switch делает для portlibs контейнера.
"$REPO_ROOT/switch/docker.sh" '
set -e
cd /tmp
echo "void __nvk_package_stub_dl(void) {}" > stub_dl.c
aarch64-none-elf-gcc -c stub_dl.c -o stub_dl.o
aarch64-none-elf-ar rcs /work/switch/nvk-switch/lib/libdl.a stub_dl.o
'

echo "OK: пакет собран в switch/nvk-switch"
ls -la "$OUT/lib"
