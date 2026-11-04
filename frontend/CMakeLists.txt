# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
cmake_minimum_required(VERSION 3.16)
project(POMPPC VERSION 0.4.2 LANGUAGES C CXX)

# POMPPC frontend — Dear ImGui + GLFW/OpenGL3 shell embedding QEMU's display
# over D-Bus (-display dbus,p2p=on). Same habillage as pom68k / POM1 / POM2 /
# POMIIGS, but the "CPU" is a QEMU process driven through GDBus + QMP
# (QemuBridge), not an in-process core. See README.md.

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

find_package(PkgConfig REQUIRED)
find_package(OpenGL REQUIRED)
find_package(Threads REQUIRED)

if(WIN32)
    # Pas de gio-unix sous Windows : les sockets passent par WSADuplicateSocketW (src/QemuBridge.cpp).
    pkg_check_modules(GIO REQUIRED IMPORTED_TARGET gio-2.0 glib-2.0 gobject-2.0)
    enable_language(RC)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    # Ce qui remplace bash et POSIX. Le reste du frontal y fait appel par #ifdef _WIN32.
    set(POMPPC_WIN_SOURCES src/WinHost.cpp src/LaunchPlan.cpp)
    set(POMPPC_WIN_LIBS ws2_32 comdlg32 shell32 ole32)
else()
    # GDBus lives in gio-unix-2.0 (we pass FDs → need the Unix bits).
    pkg_check_modules(GIO REQUIRED IMPORTED_TARGET gio-2.0 gio-unix-2.0 glib-2.0 gobject-2.0)
    set(POMPPC_WIN_SOURCES "")
    set(POMPPC_WIN_LIBS "")
endif()

# GLFW (system, or vendored via pkg-config).
find_package(glfw3 3.3 QUIET CONFIG)
if(glfw3_FOUND)
    set(GLFW_TARGET glfw)
else()
    pkg_check_modules(GLFW3 REQUIRED IMPORTED_TARGET glfw3)
    set(GLFW_TARGET PkgConfig::GLFW3)
endif()

# ─── D-Bus bindings (generated from the vendored, preprocessed contract) ──
# Same gdbus-codegen invocation as qemu/ui/meson.build so the symbol names
# (QemuDBusDisplay1*, qemu_dbus_display1_*) match the reference client in
# qemu/tests/qtest/dbus-display-test.c.
find_program(GDBUS_CODEGEN gdbus-codegen REQUIRED)
set(DBUS_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/dbus-gen)
# Le contrat est prétraité par hôte, comme dans QEMU (ui/meson.build, HOST_OS) : RegisterListener y prend
# `h` ou `ay`, et Listener.Unix.Map devient Listener.Win32.Map. dbus-display1-win32.xml est
# QEMU ui/dbus-display1.xml @ v11.1.2 passé par scripts/xml-preprocess.py avec HOST_OS=windows.
if(WIN32)
    set(DBUS_XML ${CMAKE_CURRENT_SOURCE_DIR}/dbus/dbus-display1-win32.xml)
    # [A] CI du 04/11/2026 : le gdbus-codegen de MSYS2 UCRT64 est un vrai .exe, qu'un Python ne sait
    # pas lire. S'il n'en est pas un, c'est un script que CreateProcess ne sait pas lancer seul.
    if(GDBUS_CODEGEN MATCHES "\\.[eE][xX][eE]$")
        set(DBUS_CODEGEN_CMD ${GDBUS_CODEGEN})
    else()
        set(DBUS_CODEGEN_CMD ${Python3_EXECUTABLE} ${GDBUS_CODEGEN})
    endif()
else()
    set(DBUS_XML ${CMAKE_CURRENT_SOURCE_DIR}/dbus/dbus-display1.xml)
    set(DBUS_CODEGEN_CMD ${GDBUS_CODEGEN})
endif()
set(DBUS_GEN_C ${DBUS_GEN_DIR}/dbus-display1.c)
set(DBUS_GEN_H ${DBUS_GEN_DIR}/dbus-display1.h)
file(MAKE_DIRECTORY ${DBUS_GEN_DIR})
add_custom_command(
    OUTPUT ${DBUS_GEN_C} ${DBUS_GEN_H}
    COMMAND ${DBUS_CODEGEN_CMD} ${DBUS_XML}
            --glib-min-required 2.64
            --output-directory ${DBUS_GEN_DIR}
            --interface-prefix org.qemu.
            --c-namespace QemuDBus
            --generate-c-code dbus-display1
    DEPENDS ${DBUS_XML}
    COMMENT "gdbus-codegen: org.qemu.Display1 bindings")
add_library(dbus_display1 STATIC ${DBUS_GEN_C})
target_include_directories(dbus_display1 PUBLIC ${DBUS_GEN_DIR})
target_link_libraries(dbus_display1 PUBLIC PkgConfig::GIO)
target_compile_options(dbus_display1 PRIVATE -w)  # generated code: silence warnings

# ─── Dear ImGui (fetched by setup.sh, not vendored in git) ───────────────
set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/imgui)
if(NOT EXISTS "${IMGUI_DIR}/imgui.cpp")
    message(FATAL_ERROR "Dear ImGui absent (${IMGUI_DIR}) — run ./setup.sh first.")
endif()
set(IMGUI_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp)

# ─── Police de l'interface, embarquée dans l'exécutable ──────────────────
# Roboto (Apache-2.0), fournie avec Dear ImGui : vectorielle, donc nette à toute densité d'écran.
set(FONT_HEADER ${CMAKE_CURRENT_BINARY_DIR}/roboto_font.h)
file(READ ${IMGUI_DIR}/misc/fonts/Roboto-Medium.ttf FONT_HEX HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," FONT_BYTES "${FONT_HEX}")
file(WRITE ${FONT_HEADER} "static const unsigned char kRobotoFont[] = {${FONT_BYTES}};\n")

# ─── Frontend executable ─────────────────────────────────────────────────
add_executable(pomppc
    src/main.cpp
    src/QemuBridge.cpp
    src/StartupChime.cpp
    src/FrontendSettings.cpp
    src/DiskConfig.cpp
    src/SetupFlow.cpp
    src/SetupUi.cpp
    src/AppExtras.cpp
    src/SharedFolder.cpp
    ${IMGUI_SOURCES})
target_include_directories(pomppc PRIVATE ${CMAKE_CURRENT_BINARY_DIR} ${IMGUI_DIR} ${IMGUI_DIR}/backends src)
if(APPLE)
    enable_language(OBJCXX)
    set_property(TARGET pomppc PROPERTY OBJCXX_STANDARD 17)
    target_sources(pomppc PRIVATE src/StartupChimeMac.mm)
    target_link_libraries(pomppc PRIVATE "-framework AppKit")
    target_compile_definitions(pomppc PRIVATE GL_SILENCE_DEPRECATION)
    # objc runtime: main.cpp unbinds Cocoa's Ctrl+Cmd+F "Enter Full Screen".
    target_link_libraries(pomppc PRIVATE objc)
elseif(WIN32)
    # POMPPC.exe : la fenêtre seule, sans console ; manifeste UTF-8 (src/WinHost.h). Pas de carillon :
    # StartupChime.cpp garde son talon, qui le dit, et l'invité a son propre son.
    set_target_properties(pomppc PROPERTIES WIN32_EXECUTABLE ON OUTPUT_NAME POMPPC)
    target_sources(pomppc PRIVATE src/pomppc.rc ${POMPPC_WIN_SOURCES})
    target_compile_definitions(pomppc PRIVATE POMPPC_VERSION_TEXT="${PROJECT_VERSION}")
    target_link_libraries(pomppc PRIVATE ${POMPPC_WIN_LIBS})
else()
    # Linux: the chime through PulseAudio (PipeWire's pulse server too); without
    # libpulse-simple the frontend keeps the stub, which plays nothing and says so.
    pkg_check_modules(PULSE_SIMPLE IMPORTED_TARGET libpulse-simple)
    if(PULSE_SIMPLE_FOUND)
        target_sources(pomppc PRIVATE src/StartupChimeLinux.cpp)
        target_compile_definitions(pomppc PRIVATE POMPPC_CHIME_PULSE)
        target_link_libraries(pomppc PRIVATE PkgConfig::PULSE_SIMPLE)
    else()
        message(WARNING "libpulse-simple absent : pas de carillon de démarrage")
    endif()
endif()
target_link_libraries(pomppc PRIVATE
    dbus_display1
    PkgConfig::GIO
    ${GLFW_TARGET}
    OpenGL::GL
    Threads::Threads
    ${CMAKE_DL_LIBS})

# ─── Headless verification probe (no GLFW/X) ─────────────────────────────
include(CTest)
if(WIN32 OR BUILD_TESTING)
    # La ligne de QEMU du paquet Windows : se teste sur tous les hôtes, sans QEMU ni Windows.
    add_executable(launchplan_test tests/launchplan_test.cpp src/LaunchPlan.cpp)
    target_include_directories(launchplan_test PRIVATE src)
    target_compile_options(launchplan_test PRIVATE -UNDEBUG)
    add_test(NAME launch_plan COMMAND launchplan_test)
endif()
# Sous Windows : -DBUILD_TESTING=OFF en CI (ces tests lancent bash, sh, cp).
if(BUILD_TESTING)
    add_executable(pointer_test tests/pointer_test.cpp)
    target_include_directories(pointer_test PRIVATE src)
    target_compile_options(pointer_test PRIVATE -UNDEBUG)
    add_test(NAME pointer_policy COMMAND pointer_test)
    add_executable(chime_test tests/chime_test.cpp src/StartupChime.cpp)
    target_include_directories(chime_test PRIVATE src)
    target_compile_options(chime_test PRIVATE -UNDEBUG)
    add_test(NAME startup_chime COMMAND chime_test)
    add_executable(disks_test tests/disks_test.cpp src/DiskConfig.cpp)
    target_include_directories(disks_test PRIVATE src)
    target_compile_options(disks_test PRIVATE -UNDEBUG)
    add_test(NAME disk_choice COMMAND disks_test)
    add_executable(extras_test tests/extras_test.cpp src/AppExtras.cpp src/SetupFlow.cpp src/DiskConfig.cpp)
    target_include_directories(extras_test PRIVATE src)
    target_compile_options(extras_test PRIVATE -UNDEBUG)
    target_link_libraries(extras_test PRIVATE Threads::Threads)
    add_test(NAME app_extras COMMAND extras_test)
    add_executable(shared_test tests/shared_test.cpp src/SharedFolder.cpp src/SetupFlow.cpp src/DiskConfig.cpp)
    target_include_directories(shared_test PRIVATE src)
    target_compile_options(shared_test PRIVATE -UNDEBUG)
    target_link_libraries(shared_test PRIVATE Threads::Threads)
    add_test(NAME shared_folder COMMAND shared_test)
    add_executable(setup_test tests/setup_test.cpp src/SetupFlow.cpp src/DiskConfig.cpp)
    target_include_directories(setup_test PRIVATE src)
    target_compile_options(setup_test PRIVATE -UNDEBUG)
    target_link_libraries(setup_test PRIVATE Threads::Threads)
    add_test(NAME setup_flow COMMAND setup_test)
    add_executable(settings_test tests/settings_test.cpp src/FrontendSettings.cpp src/DiskConfig.cpp)
    target_include_directories(settings_test PRIVATE src)
    target_compile_options(settings_test PRIVATE -UNDEBUG)
    add_test(NAME frontend_settings COMMAND settings_test)
endif()

add_executable(bridge_probe tests/bridge_probe.cpp src/QemuBridge.cpp ${POMPPC_WIN_SOURCES})
target_include_directories(bridge_probe PRIVATE src)
target_link_libraries(bridge_probe PRIVATE
    dbus_display1 PkgConfig::GIO Threads::Threads ${CMAKE_DL_LIBS} ${POMPPC_WIN_LIBS})
