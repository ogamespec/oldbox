cmake_minimum_required(VERSION 3.25)
project(oldbox VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(OLDBOX_WITH_SDL "Build the SDL3 frontend" ON)
option(OLDBOX_BUILD_TESTS "Build unit and integration tests" ON)
option(OLDBOX_FETCH_SDL "Fetch SDL3 when no system package is found" ON)

# No built-in or fallback firmware: OldBox runs a REAL Award BIOS dump
# (assets/bios/EP-ZXA.bin) provided via --bios / assets.
function(oldbox_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus /bigobj)
        # The emulator keeps a few large tables; the default Windows stack is
        # only 1 MiB, which is too tight for a stack-allocated Machine.
        target_link_options(${target} PRIVATE /STACK:8388608)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wno-unused-parameter)
    endif()
endfunction()

add_library(oldbox_core STATIC
    src/common.cpp
    src/config.cpp
    src/bus.cpp
    src/flash.cpp
    src/pci.cpp
    src/cpu/cpu.cpp
    src/cpu/cpu_0f.cpp
    src/cpu/cpu_x87.cpp
    src/cpu/cpu_system.cpp
    src/machine.cpp
    src/console.cpp
    src/keymap.cpp
    src/chipset/i440bx.cpp
    src/chipset/piix4.cpp
    src/chipset/pic.cpp
    src/chipset/pit.cpp
    src/chipset/rtc.cpp
    src/chipset/dma.cpp
    src/chipset/keyboard.cpp
    src/chipset/smbus.cpp
    src/chipset/lm78.cpp
    src/chipset/w83877tf.cpp
    src/cpu/apic.cpp
    src/video/s3_trio64.cpp
    src/video/voodoo.cpp
    src/audio/awe64.cpp
    src/audio/emu8000.cpp
    src/storage/ata.cpp
    src/storage/floppy.cpp
)
target_include_directories(oldbox_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_definitions(oldbox_core PUBLIC OLDBOX_VERSION="${PROJECT_VERSION}")
oldbox_warnings(oldbox_core)

if(OLDBOX_WITH_SDL)
    if(OLDBOX_FETCH_SDL)
        # The console overlay uses SDL_RenderDebugText (SDL 3.2+).
        find_package(SDL3 3.2 QUIET)
        if(NOT SDL3_FOUND)
            if(UNIX AND NOT APPLE)
                include(CheckIncludeFile)
                check_include_file(X11/Xlib.h OLDBOX_HAVE_X11)
                check_include_file(wayland-client.h OLDBOX_HAVE_WAYLAND)
                if(NOT OLDBOX_HAVE_X11 AND NOT OLDBOX_HAVE_WAYLAND)
                    set(SDL_UNIX_CONSOLE_BUILD ON CACHE BOOL "" FORCE)
                endif()
            endif()
            include(FetchContent)
            FetchContent_Declare(SDL3
                URL https://github.com/libsdl-org/SDL/releases/download/release-3.2.30/SDL3-3.2.30.tar.gz
                URL_HASH SHA256=4c3b09330d866dc52eb65b66259a6684ad387252ca8c57901b3a2b534eb42e3d
                DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
            set(SDL_SHARED OFF CACHE BOOL "" FORCE)
            set(SDL_STATIC ON CACHE BOOL "" FORCE)
            set(SDL_TEST OFF CACHE BOOL "" FORCE)
            set(SDL_TESTS OFF CACHE BOOL "" FORCE)
            set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
            FetchContent_MakeAvailable(SDL3)
        endif()
    else()
        find_package(SDL3 3.2 REQUIRED)
    endif()
    add_executable(oldbox src/main.cpp src/frontend/sdl3.cpp)
    target_compile_definitions(oldbox PRIVATE OLDBOX_WITH_SDL)
    target_link_libraries(oldbox PRIVATE oldbox_core SDL3::SDL3)
    if(WIN32)
        target_compile_definitions(oldbox PRIVATE SDL_MAIN_HANDLED NOMINMAX WIN32_LEAN_AND_MEAN)
    endif()
    oldbox_warnings(oldbox)
else()
    add_executable(oldbox src/main.cpp)
    target_link_libraries(oldbox PRIVATE oldbox_core)
endif()

if(WIN32)
    # Explorer/taskbar icon embedded in oldbox.exe.  The artwork is generated
    # by tools/make_icon.py; the window icon set at runtime lives in
    # src/frontend/icon_rgba.inc.
    enable_language(RC)
    target_sources(oldbox PRIVATE src/win32/oldbox.rc)
endif()

if(OLDBOX_BUILD_TESTS)
    enable_testing()
    add_executable(oldbox_tests tests/test_main.cpp tests/test_cpu.cpp tests/test_cpu_instructions.cpp tests/test_cpu_modes.cpp tests/test_cpu_system.cpp tests/test_cpu_fpu.cpp tests/test_io.cpp tests/test_voodoo.cpp
        tests/test_s3.cpp tests/test_audio.cpp tests/test_storage.cpp tests/test_devices.cpp
        tests/test_chipset.cpp
        tests/test_keyboard.cpp tests/test_machine.cpp tests/test_console.cpp)
    target_link_libraries(oldbox_tests PRIVATE oldbox_core)
    target_include_directories(oldbox_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    oldbox_warnings(oldbox_tests)
    if(MSVC)
        target_compile_options(oldbox_tests PRIVATE /bigobj)
    endif()
    add_test(NAME oldbox_tests COMMAND oldbox_tests)
    # Tests use the asset paths published in the repository (assets/...).
    set_tests_properties(oldbox_tests PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endif()

install(TARGETS oldbox RUNTIME DESTINATION bin)
