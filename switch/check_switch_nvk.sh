#!/bin/sh
# Проверка переносимого NVK-пакета для Switch-сборки (шаг 3.1 плана
# docs/PLAN_STAGE3_GRAPHICS.md). Скрипт взят из ветки `switch` upstream
# (tools/check_switch_nvk.sh) без изменений логики.
# Только инспектирует заголовки/библиотеки пакета, ничего не собирает.
#
# Запуск: switch/docker.sh "sh switch/check_switch_nvk.sh /work/switch/nvk-switch"
set -eu

ROOT=${1:-${RAYMAN_SWITCH_NVK_ROOT:-}}
DEVKIT=${DEVKITPRO:-/opt/devkitpro}
NM=$DEVKIT/devkitA64/bin/aarch64-none-elf-nm

if [ -z "$ROOT" ]; then
    echo "usage: sh tools/check_switch_nvk.sh /path/to/nvk-switch" >&2
    exit 2
fi

HEADER=$ROOT/include/vulkan/vulkan.h
VI_HEADER=$ROOT/include/vulkan/vulkan_vi.h
LIBRARY=$ROOT/lib/libvulkan.a
for required in "$HEADER" "$VI_HEADER" "$LIBRARY" "$NM"; do
    if [ ! -f "$required" ]; then
        echo "missing Switch NVK input: $required" >&2
        exit 1
    fi
done

for name in z zstd expat dl; do
    packaged=$ROOT/lib/lib$name.a
    system=$DEVKIT/portlibs/switch/lib/lib$name.a
    if [ -f "$packaged" ]; then
        :
    elif [ -f "$system" ]; then
        :
    else
        echo "missing Switch portlib required by NVK: lib$name.a" >&2
        echo "  checked $ROOT/lib and $DEVKIT/portlibs/switch/lib" >&2
        exit 1
    fi
done

SYMBOLS=$(mktemp "${TMPDIR:-/tmp}/rayman-nvk-symbols.XXXXXX")
trap 'rm -f "$SYMBOLS"' EXIT HUP INT TERM
$NM "$LIBRARY" >"$SYMBOLS" 2>/dev/null
# Пакет mesa-switch (Mesa 26.2.4) — loaderless ICD: vk_icdGetInstanceProcAddr
# обязателен. Символы-обёртки __wrap_* (open/close/stat/lstat/
# vk_icdGetInstanceProcAddr) требовал пакет Mesa 25.0.7 из ветки `switch`
# upstream: там libc-вызовы Mesa перенаправлялись линкером. В mesa-switch
# Horizon-бэкенд нативный, обёрток нет и --wrap-флаги не нужны
# (проверено nm 2026-11-09) — поэтому они здесь только информационные.
for symbol in vk_icdGetInstanceProcAddr; do
    if ! grep -Eq " [Tt] $symbol$" "$SYMBOLS"; then
        echo "Switch NVK archive does not define $symbol: $LIBRARY" >&2
        exit 1
    fi
done
WRAPS=""
for symbol in \
    __wrap_vk_icdGetInstanceProcAddr \
    __wrap_open \
    __wrap_close \
    __wrap_stat \
    __wrap_lstat; do
    if grep -Eq " [Tt] $symbol$" "$SYMBOLS"; then
        WRAPS="$WRAPS $symbol"
    fi
done
if [ -n "$WRAPS" ]; then
    echo "info: archive defines libc/ICD wrap shims:$WRAPS"
    echo "info: (Mesa 25.0.7-style package; add matching --wrap link options)"
fi

if ! grep -Eiq 'CreateViSurfaceNN|wsi_switch_init_wsi' "$SYMBOLS"; then
    echo "Switch NVK archive has no VK_NN_vi_surface implementation: $LIBRARY" >&2
    exit 1
fi

echo "Switch NVK package OK"
echo "  root: $ROOT"
echo "  archive: $LIBRARY"
echo "  VI surface and loaderless ICD entry: present"
echo "  zlib, zstd, expat and libdl Switch archives: present"
