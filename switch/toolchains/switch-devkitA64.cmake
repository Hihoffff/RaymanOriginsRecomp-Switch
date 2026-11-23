# Nintendo Switch (devkitA64 + libnx) — тулчейн для сборки ReXGlue и игры.
#
# Обёртка над официальным тулчейном devkitPro. Сборка выполняется внутри
# контейнера devkitpro/devkita64 (см. switch/docker.sh), поэтому путь
# /opt/devkitpro — контейнерный.
#
# Использование:
#   cmake -DCMAKE_TOOLCHAIN_FILE=$PWD/switch/toolchains/switch-devkitA64.cmake ...

set(CMAKE_SYSTEM_NAME NintendoSwitch)

if(NOT EXISTS "/opt/devkitpro/cmake/Switch.cmake")
  message(FATAL_ERROR
    "Официальный тулчейн devkitPro не найден по пути /opt/devkitpro/cmake/Switch.cmake. "
    "Сборку под Switch запускайте через switch/docker.sh (контейнер devkitpro/devkita64).")
endif()

include("/opt/devkitpro/cmake/Switch.cmake")
