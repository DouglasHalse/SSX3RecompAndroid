cmake_minimum_required(VERSION 3.20)

# ============================================================================
# SSX 3 - Static Recompilation
#
# Copy this template to your game project root and customize:
#   1. Change the project name below
#   2. Set XBOXRECOMP_DIR to where you cloned xboxrecomp
#   3. Adjust source file lists as you add game-specific code
#   4. Add any game-specific compile definitions or link libraries
# ============================================================================

project(ssx3_recomp C)

# Static C runtime: the release runs without the Visual C++ Redistributable
# (a missing VCRUNTIME140.dll would stop launcher.exe before it opens).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Common definitions
add_compile_definitions(
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

# WebAssembly (emcmake cmake ..., see docs/WEB.md): every guest and host
# thread is a pthread, so the whole tree is built with shared memory.
if(EMSCRIPTEN)
    add_compile_options(-pthread)
    add_link_options(-pthread)
endif()

# ── xboxrecomp toolkit ──────────────────────────────────────────
#
# Option A: xboxrecomp as a subdirectory (simplest)
#   Place xboxrecomp/ next to your game project and use a relative path.
#
# Option B: find_package (if you install xboxrecomp system-wide)
#   find_package(xboxrecomp REQUIRED)
#
# Adjust this path to point to your xboxrecomp clone:
set(XBOXRECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../xboxrecomp" CACHE PATH
    "Path to the xboxrecomp toolkit root directory")

# Add xboxrecomp as a subdirectory -- this builds all 6 runtime libraries
# and provides the 'xboxrecomp' umbrella target to link against.
add_subdirectory(${XBOXRECOMP_DIR} ${CMAKE_BINARY_DIR}/xboxrecomp)

# ── Recompiled game executable ──────────────────────────────────

# Collect generated recompiled code.
# The recomp pipeline outputs files into src/recomp/gen/.
# With --split 1000, you get gen_0000.c, gen_0001.c, etc.
# CONFIGURE_DEPENDS so re-running the recompiler picks up added/removed files
# without a manual re-configure. A plain GLOB is evaluated once at configure
# time, so a newly generated file (an extra chunk, recomp_stubs_unresolved.c)
# is silently left out of the build and shows up as unresolved externals.
file(GLOB RECOMP_GEN_SOURCES CONFIGURE_DEPENDS "src/recomp/gen/*.c")

# Core game sources
set(GAME_SOURCES
    src/main.c
    src/log.c
    src/journal.c
    src/mark.c
    src/perf.c
    src/recomp_manual.c
    src/ssx3_overrides.c
    src/ssx3_music_capture.c
    src/thread_dump.c
    src/game_state.c
    src/camera_mode.c
    src/autonav.c
    src/mmio_posix.c
    src/web_platform.c
    # Add game-specific sources here, e.g.:
    # src/asset_loader.c
    # src/custom_renderer.c
)

# The recomp pipeline also generates a dispatch header and function declarations.
# These are included by the generated .c files:
#   src/recomp/gen/recomp_funcs.h    - extern declarations for all recompiled functions
#   src/recomp/recomp_dispatch.h     - binary search dispatch table
#   src/recomp/recomp_types.h        - register model, ICALL macros, MEM helpers

if(WIN32)
    add_executable(${PROJECT_NAME} WIN32 ${GAME_SOURCES} ${RECOMP_GEN_SOURCES})
elseif(ANDROID)
    # The recompiled guest is plain C, so it builds for arm64 unchanged. The
    # APK loads this as libmain.so: SDL2's Java activity (SDLActivity) calls
    # SDL_main in it. main.c's main() is renamed so src/android/sdl_main.c can
    # be the SDL_main entry that owns SDL video and starts the game on a
    # worker thread.
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
    add_library(${PROJECT_NAME} SHARED ${GAME_SOURCES} ${RECOMP_GEN_SOURCES}
                src/android/sdl_main.c)
    set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME "main")
    set_source_files_properties(src/main.c PROPERTIES COMPILE_DEFINITIONS "main=ssx3_main")
    set_source_files_properties(src/android/sdl_main.c PROPERTIES COMPILE_DEFINITIONS "")

    # The launcher's importer, as its own small library so the screen that asks
    # for a disc image does not have to load the game to work: libssx3import.so
    # is xiso.c (XDVDFS reader/extractor) behind a JNI wrapper.
    add_library(ssx3import SHARED src/android/xiso.c src/android/xiso_jni.c)
    target_include_directories(ssx3import PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/android)
    set_target_properties(ssx3import PROPERTIES OUTPUT_NAME "ssx3import")
    target_link_libraries(ssx3import PRIVATE m log)
else()
    add_executable(${PROJECT_NAME} ${GAME_SOURCES} ${RECOMP_GEN_SOURCES})
endif()

# The build's version for the session log (src/journal.c)
add_custom_target(ssx3_build_info
    COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_SOURCE_DIR} -DOUT=${CMAKE_BINARY_DIR}/build_info.h
            -P ${CMAKE_SOURCE_DIR}/cmake/build_info.cmake
    BYPRODUCTS ${CMAKE_BINARY_DIR}/build_info.h
    VERBATIM)
add_dependencies(${PROJECT_NAME} ssx3_build_info)
target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_BINARY_DIR})

# Link against the xboxrecomp umbrella target (all 6 runtime libraries)
target_link_libraries(${PROJECT_NAME} PRIVATE xboxrecomp)

# Include paths for generated recomp headers
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/recomp
    ${XBOXRECOMP_DIR}/include
)

# Windows SDK libraries needed at link time
if(WIN32)
    target_link_libraries(${PROJECT_NAME} PRIVATE
        d3d11
        dxgi
        dxguid
        xinput
        winmm
        # Lets the fault handler resolve the host RIP back to the generated
        # function's symbol, i.e. to the guest function that faulted.
        dbghelp
    )
elseif(ANDROID)
    # libmain.so carries the game; SDL2 and Vulkan are its only outside deps.
    target_link_libraries(${PROJECT_NAME} PRIVATE m log android dl)
elseif(EMSCRIPTEN)
    # main() runs on a worker (PROXY_TO_PTHREAD): guest code blocks freely and
    # the browser thread stays free for input, audio and presenting. Guest
    # call chains are native call chains, so threads get deep stacks.
    # One fixed 2 GB memory: growing a shared memory is slow, and every
    # thread's view would have to follow. Guest memory is ~155 MB of it.
    set_target_properties(${PROJECT_NAME} PROPERTIES SUFFIX ".js")
    target_link_options(${PROJECT_NAME} PRIVATE
        --profiling-funcs
        -sPROXY_TO_PTHREAD=1
        -sINITIAL_MEMORY=2GB
        -sALLOW_MEMORY_GROWTH=0
        -sSTACK_SIZE=8MB
        -sDEFAULT_PTHREAD_STACK_SIZE=4MB
        -sPTHREAD_POOL_SIZE=40
        -sWASMFS=1
        -sENVIRONMENT=web,worker
        -sMODULARIZE=1
        -sEXPORT_NAME=SSX3
        -sEXIT_RUNTIME=0
        -sINVOKE_RUN=0
        -sEXPORTED_FUNCTIONS=_main,_web_key,_web_pad,_web_frame,_web_audio_ring,_malloc,_free
        -sEXPORTED_RUNTIME_METHODS=HEAPU8,ENV,callMain
    )
    # The page next to the module: build-web/ is what tools/serve_web.py serves.
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_CURRENT_SOURCE_DIR}/web/index.html
            ${CMAKE_CURRENT_SOURCE_DIR}/web/main.js
            ${CMAKE_CURRENT_SOURCE_DIR}/web/audio-worklet.js
            ${CMAKE_CURRENT_SOURCE_DIR}/web/gpu.js
            ${CMAKE_CURRENT_SOURCE_DIR}/web/xbox.wgsl
            $<TARGET_FILE_DIR:${PROJECT_NAME}>)
else()
    target_link_libraries(${PROJECT_NAME} PRIVATE m pthread dl)
    # Symbol names (sub_XXXXXXXX) in the crash handler's backtrace.
    target_link_options(${PROJECT_NAME} PRIVATE -rdynamic)
endif()

# Vulkan renderer for the NV2A executor (RECOMP_GPU=vk), see docs/VULKAN.html.
# Both hosts; on Windows it needs the Vulkan SDK (tools\build.bat finds it).
if(EMSCRIPTEN)
    set(SSX3_VULKAN_DEFAULT OFF)
else()
    set(SSX3_VULKAN_DEFAULT ON)
endif()
option(SSX3_VULKAN "Build the Vulkan renderer" ${SSX3_VULKAN_DEFAULT})
if(EMSCRIPTEN)
    # The same renderer interface (src/vk/vkr.h, driven by vkr_backend.c) on
    # WebGPU: src/web/wgr.c records frames, web/gpu.js replays them on the
    # page thread. RECOMP_GPU=sw keeps the software rasteriser.
    target_sources(${PROJECT_NAME} PRIVATE src/vk/vkr_backend.c src/web/wgr.c)
    target_include_directories(${PROJECT_NAME} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/vk ${XBOXRECOMP_DIR}/src/kernel)
    target_compile_definitions(${PROJECT_NAME} PRIVATE SSX3_VULKAN=1)
elseif(SSX3_VULKAN)
    add_subdirectory(src/vk ${CMAKE_BINARY_DIR}/vk)
    target_sources(${PROJECT_NAME} PRIVATE src/vk/vkr_backend.c src/vk/vkr_hdtex.c)
    target_include_directories(${PROJECT_NAME} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/vk ${XBOXRECOMP_DIR}/src/kernel)
    target_link_libraries(${PROJECT_NAME} PRIVATE vkr)
    target_compile_definitions(${PROJECT_NAME} PRIVATE SSX3_VULKAN=1)
endif()

# MSVC-specific: large address aware, increase section limit for big generated files
if(MSVC)
    target_link_options(${PROJECT_NAME} PRIVATE
        /LARGEADDRESSAWARE
        # build/ssx3_recomp.map: every generated function is a symbol in it, so
        # xboxrecomp/tools/symbolize.py turns a crash RIP into sub_XXXXXXXX.
        /MAP
    )
    # The generated code files can be very large. Increase object file section limit.
    target_compile_options(${PROJECT_NAME} PRIVATE /bigobj)
    # A few generated functions are thousands of lines of gotos (CRT/XAPI and
    # game code); MSVC's full /O2 on them is superlinear -- recomp_0064.c
    # alone took ~1000 s and was the whole build. This internal switch limits
    # optimization inside such huge functions only: that file now compiles in
    # ~70 s, everything else is optimized as before.
    target_compile_options(${PROJECT_NAME} PRIVATE /d2ReducedOptimizeHugeFunctions)
endif()

# ── launcher.exe ─────────────────────────────────────────────────
# Settings GUI that starts the game (src/launcher/launcher.c): game folder
# picker with a check of default.xbe, display options, controllers.
if(ANDROID)
    # No launcher on Android: there is no folder picker, and the game folder is
    # fixed inside the app's own storage (src/android/sdl_main.c sets it up).
elseif(WIN32)
    add_executable(ssx3_launcher WIN32 src/launcher/launcher.c)
    set_target_properties(ssx3_launcher PROPERTIES OUTPUT_NAME launcher)
    target_link_libraries(ssx3_launcher PRIVATE comctl32 ole32 shell32 xinput)
else()
    # The same launcher on SDL2 (src/launcher/launcher_sdl.c): text through
    # SDL2_ttf in the desktop's font (fontconfig). Optional: without them
    # the game builds as before.
    find_package(SDL2 QUIET)
    find_package(SDL2_ttf QUIET)
    find_package(Fontconfig QUIET)
    if(TARGET SDL2::SDL2 AND TARGET SDL2_ttf::SDL2_ttf AND TARGET Fontconfig::Fontconfig)
        add_executable(ssx3_launcher src/launcher/launcher_sdl.c)
        set_target_properties(ssx3_launcher PROPERTIES OUTPUT_NAME launcher)
        target_link_libraries(ssx3_launcher PRIVATE SDL2::SDL2 SDL2_ttf::SDL2_ttf
                              Fontconfig::Fontconfig)
    else()
        message(STATUS "launcher: SDL2_ttf or fontconfig not found, not built")
    endif()
endif()

# ── Game-specific additions ─────────────────────────────────────
#
# Add any game-specific libraries, loaders, or custom renderers below.
# Examples:
#
# # Custom texture loader for your game's format
# target_sources(${PROJECT_NAME} PRIVATE src/texture_loader.c)
#
# # If your game uses RenderWare, you may need a RW bridge layer
# target_sources(${PROJECT_NAME} PRIVATE src/rw_bridge.c)
#
# # Additional compile definitions for debugging
# target_compile_definitions(${PROJECT_NAME} PRIVATE
#     ENABLE_ICALL_TRACE=1
#     VERBOSE_KERNEL_LOG=1
# )
