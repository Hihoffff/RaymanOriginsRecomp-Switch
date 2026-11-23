#!/usr/bin/env bash
# switch/nxlink.sh <IP-консоли> — отправить .nro на Switch по Wi-Fi и показывать его вывод здесь.
# Консоль: hbmenu -> открыть любую homebrew-программу при зажатой R? НЕТ: нужен пункт
# "NetLoader" (hbmenu, Y). Консоль и Mac — в одной сети. Проверено 2026-11-09: работает (HOS 21.2.0). -t нужен для построчного вывода.
# Игровые файлы всё равно должны лежать на SD (sdmc:/switch/RaymanOriginsRecomp/game/).
set -euo pipefail
[ $# -ge 1 ] || { echo "Использование: switch/nxlink.sh <IP-консоли>" >&2; exit 2; }
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
docker run --rm -t --platform linux/arm64 -p 28771:28771 \
  -v "$REPO_ROOT":/work -w /work devkitpro/devkita64:latest \
  /opt/devkitpro/tools/bin/nxlink -a "$1" -s switch/game/build/rayman_switch.nro
