#!/usr/bin/env bash
# switch/docker.sh "<команда>" — выполняет команду в контейнере devkitPro
# (образ devkitpro/devkita64), монтируя репозиторий в /work.
#
# Внутри контейнера активируется /opt/devkitpro/switchvars.sh
# (DEVKITPRO, DEVKITA64, portlibs, PATH с aarch64-none-elf-*).
#
# Пример:
#   switch/docker.sh "cmake -S upstream/tools/forks/rexglue-src \
#     -B upstream/tools/forks/rexglue-src/out/build/switch-arm64 -G Ninja \
#     -DCMAKE_TOOLCHAIN_FILE=/work/switch/toolchains/switch-devkitA64.cmake"

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGE="${DEVKITA64_IMAGE:-devkitpro/devkita64:latest}"

if [ $# -eq 0 ]; then
  echo "Использование: switch/docker.sh \"<команда>\"" >&2
  exit 2
fi

docker run --rm --platform linux/arm64 \
  -v "$REPO_ROOT":/work -w /work \
  "$IMAGE" bash -c "source /opt/devkitpro/switchvars.sh && $*"
