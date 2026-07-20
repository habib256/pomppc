cmake_minimum_required(VERSION 3.16)
project(POMPPC VERSION 0.1 LANGUAGES C CXX)

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

# GDBus lives in gio-unix-2.0 (we pass FDs → need the Unix bits).
pkg_check_modules(GIO REQUIRED IMPORTED_TARGET gio-2.0 gio-unix-2.0 glib-2.0 gobject-2.0)

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
set(DBUS_GEN_C ${DBUS_GEN_DIR}/dbus-display1.c)
set(DBUS_GEN_H ${DBUS_GEN_DIR}/dbus-display1.h)
file(MAKE_DIRECTORY ${DBUS_GEN_DIR})
add_custom_command(
    OUTPUT ${DBUS_GEN_C} ${DBUS_GEN_H}
    COMMAND ${GDBUS_CODEGEN} ${CMAKE_CURRENT_SOURCE_DIR}/dbus/dbus-display1.xml
            --glib-min-required 2.64
            --output-directory ${DBUS_GEN_DIR}
            --interface-prefix org.qemu.
            --c-namespace QemuDBus
            --generate-c-code dbus-display1
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/dbus/dbus-display1.xml
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

# ─── Frontend executable ─────────────────────────────────────────────────
add_executable(pomppc
    src/main.cpp
    src/QemuBridge.cpp
    ${IMGUI_SOURCES})
target_include_directories(pomppc PRIVATE ${IMGUI_DIR} ${IMGUI_DIR}/backends src)
target_link_libraries(pomppc PRIVATE
    dbus_display1
    PkgConfig::GIO
    ${GLFW_TARGET}
    OpenGL::GL
    Threads::Threads
    ${CMAKE_DL_LIBS})

# ─── Headless verification probe (no GLFW/X) ─────────────────────────────
add_executable(bridge_probe tests/bridge_probe.cpp src/QemuBridge.cpp)
target_include_directories(bridge_probe PRIVATE src)
target_link_libraries(bridge_probe PRIVATE
    dbus_display1 PkgConfig::GIO Threads::Threads ${CMAKE_DL_LIBS})
