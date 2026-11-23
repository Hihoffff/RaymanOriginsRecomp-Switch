# Rayman Origins (Xbox 360) recompiled - Nintendo Switch headless bring-up (.nro)
#
# Build (Docker, see switch/README.md):
#   switch/docker.sh "cmake -S switch/game -B switch/game/build -G Ninja \
#       -DCMAKE_TOOLCHAIN_FILE=/work/switch/toolchains/switch-devkitA64.cmake \
#       -DRAYMAN_GEN=/work/upstream/rex/generated/default -DREXGLUE_ENABLE_TRACY=OFF"
#   switch/docker.sh "cmake --build switch/game/build"
#
# RAYMAN_GEN points at the game-derived generated sources (private, never committed).

cmake_minimum_required(VERSION 3.25)
project(rayman_switch LANGUAGES C CXX ASM)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(RAYMAN_GEN "" CACHE PATH "Directory with generated recompiled sources (generated/default)")
if(NOT EXISTS "${RAYMAN_GEN}/sources.cmake")
    message(FATAL_ERROR "Set RAYMAN_GEN to the generated/default directory (sources.cmake not found)")
endif()

set(REXSDK_SRC "${CMAKE_CURRENT_LIST_DIR}/../../upstream/tools/forks/rexglue-src")
# Diagnostics (temporary bring-up logging): per-session logs in logs/, hitch
# lines, [save]/[fsdiag]/[cpu]/[perf] in the runtime. OFF = warnings/errors
# and crash.log only, the diagnostic code is compiled out.
option(RAYMAN_SWITCH_DIAG "Switch diagnostic logging" ON)
if(RAYMAN_SWITCH_DIAG)
    add_compile_definitions(REX_SWITCH_DIAG=1 RAYMAN_DIAG=1)
else()
    add_compile_definitions(REX_SWITCH_DIAG=0 RAYMAN_DIAG=0)
endif()
message(STATUS "RAYMAN_SWITCH_DIAG=${RAYMAN_SWITCH_DIAG}")
add_subdirectory("${REXSDK_SRC}" rexglue-sdk)

include("${RAYMAN_GEN}/sources.cmake")   # defines GENERATED_SOURCES

add_library(rayman_recomp OBJECT ${GENERATED_SOURCES})
target_include_directories(rayman_recomp PRIVATE "${RAYMAN_GEN}")
target_link_libraries(rayman_recomp PRIVATE rex::runtime)
rexglue_apply_target_settings(rayman_recomp)
# Release is -O3 globally; the generated code is huge, keep it at -O2 and w/o debug info.
target_compile_options(rayman_recomp PRIVATE -O2 -g0 -w
    -include "${CMAKE_CURRENT_LIST_DIR}/src/rex_func_gcc.h")

add_executable(rayman_switch src/main.cpp src/selftest.cpp src/switch_input.cpp src/switch_audio.cpp)
target_include_directories(rayman_switch PRIVATE "${RAYMAN_GEN}")
# Экспорты ядра (__imp__Xam*/__imp__Xbdm*/CRT-стабы) регистрируются статическими
# объектами PPCFuncRegistrar в своих .o. Игра запрашивает многие из них по ordinal
# только в рантайме (XexGetProcedureAddress), ссылок на этапе линковки нет, и
# статический линкер выбрасывает такие .o целиком: без --whole-archive в .elf
# попадало 1211 из 2866 экспортов (проверено nm, 2026-11-13), в т.ч. терялись
# XamMediaVerification* — главный поток игры зацикливался на проверке носителя.
target_link_libraries(rayman_switch PRIVATE rayman_recomp
    "-Wl,--whole-archive" $<TARGET_FILE:rexruntime> "-Wl,--no-whole-archive"
    rex::runtime rex::gpu-null-switch nx)
rexglue_apply_target_settings(rayman_switch)

# --- NVK (Vulkan) для Switch: внешний пакет, опция выключена по умолчанию ---
# Пакет собирается извне (mesa-switch, см. docs/sessions, шаг 3.0–3.1):
#   <root>/include/vulkan/vulkan.h, vulkan_vi.h; <root>/lib/libvulkan.a
# Проверка пакета: switch/docker.sh "sh switch/check_switch_nvk.sh <root>".
# Сборка с NVK:
#   switch/docker.sh "cmake -S switch/game -B switch/game/build ... \
#       -DRAYMAN_SWITCH_NVK_ROOT=/work/switch/nvk-switch"
set(RAYMAN_SWITCH_NVK_ROOT "" CACHE PATH
    "Корень NVK-пакета для Switch (include/vulkan + lib/libvulkan.a); пусто = сборка без Vulkan")
if(RAYMAN_SWITCH_NVK_ROOT)
    foreach(_nvk_input IN ITEMS
            "${RAYMAN_SWITCH_NVK_ROOT}/include/vulkan/vulkan.h"
            "${RAYMAN_SWITCH_NVK_ROOT}/include/vulkan/vulkan_vi.h"
            "${RAYMAN_SWITCH_NVK_ROOT}/lib/libvulkan.a")
        if(NOT EXISTS "${_nvk_input}")
            message(FATAL_ERROR "Неполный NVK-пакет: нет ${_nvk_input}")
        endif()
    endforeach()
    # Портлибы: сначала внутри пакета, иначе из devkitPro.
    set(_nvk_portlibs "")
    foreach(_nvk_portlib IN ITEMS z zstd expat dl)
        if(EXISTS "${RAYMAN_SWITCH_NVK_ROOT}/lib/lib${_nvk_portlib}.a")
            list(APPEND _nvk_portlibs "${RAYMAN_SWITCH_NVK_ROOT}/lib/lib${_nvk_portlib}.a")
        elseif(EXISTS "${DEVKITPRO}/portlibs/switch/lib/lib${_nvk_portlib}.a")
            list(APPEND _nvk_portlibs "${DEVKITPRO}/portlibs/switch/lib/lib${_nvk_portlib}.a")
        else()
            message(FATAL_ERROR
                "NVK требует lib${_nvk_portlib}.a в ${RAYMAN_SWITCH_NVK_ROOT}/lib "
                "или ${DEVKITPRO}/portlibs/switch/lib")
        endif()
    endforeach()

    target_sources(rayman_switch PRIVATE src/vulkan_probe.cpp src/vulkan_clear.cpp
        src/native/native_hooks_switch.cpp src/native/native_capture_switch.cpp
        src/native/native_renderer_switch.cpp)
    # Native renderer (upstream/tools/native_renderer, header-only). shim/ comes
    # first: its volk.h replaces volk with the NVK package's direct prototypes.
    target_include_directories(rayman_switch BEFORE PRIVATE "${CMAKE_CURRENT_LIST_DIR}/src/native/shim")
    target_include_directories(rayman_switch PRIVATE
        "${CMAKE_CURRENT_LIST_DIR}/../../upstream/tools/native_renderer"
        "${REXSDK_SRC}/thirdparty/xxHash")
    target_include_directories(rayman_switch PRIVATE "${RAYMAN_SWITCH_NVK_ROOT}/include")
    target_compile_definitions(rayman_switch PRIVATE RAYMAN_SWITCH_VULKAN=1 VK_USE_PLATFORM_VI_NN=1)
    # Merged libvulkan.a: таблицы entrypoint Mesa ссылаются слабо — без
    # --whole-archive часть WSI-объектов тихо выбрасывается (заметка upstream).
    # При --whole-archive дублируются сгенерированные nv_push_cl*.o и
    # vulkan-dispatch.o внутри архива — upstream линкует с
    # --allow-multiple-definition; дубли идентичны по содержимому.
    target_link_options(rayman_switch PRIVATE "LINKER:--allow-multiple-definition")
    target_link_libraries(rayman_switch PRIVATE
        "-Wl,--start-group"
        "-Wl,--whole-archive"
        "${RAYMAN_SWITCH_NVK_ROOT}/lib/libvulkan.a"
        "-Wl,--no-whole-archive"
        ${_nvk_portlibs}
        stdc++
        m
        pthread
        "-Wl,--end-group")
endif()

nx_generate_nacp(OUTPUT rayman_switch.nacp NAME "Rayman Origins (home port)"
                 AUTHOR "home project" VERSION "0.0.1")
# Icon: taken locally from the owner's own game dump (private/, never in git):
# private/switch_icon.jpg, 256x256 JPEG. Without it the default icon is used.
set(RAYMAN_ICON "${CMAKE_CURRENT_LIST_DIR}/../../private/switch_icon.jpg")
if(EXISTS "${RAYMAN_ICON}")
    nx_create_nro(rayman_switch NACP rayman_switch.nacp ICON "${RAYMAN_ICON}")
else()
    nx_create_nro(rayman_switch NACP rayman_switch.nacp)
endif()
