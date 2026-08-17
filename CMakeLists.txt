cmake_minimum_required(VERSION 3.18)
project(netune VERSION 2.0.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── FetchContent helper and dependency resolution macro ─
include(FetchContent)

# netune_find_dep(<Name> <min-version> [FETCH_CONTENT <args>...])
# First tries find_package(<Name> <min-version> CONFIG QUIET).
# If not found and FETCH_CONTENT arguments are provided, falls back to
# FetchContent_Declare + FetchContent_MakeAvailable from the given source.
macro(netune_find_dep _name _min_ver)
  set(_args "${ARGN}")
  list(FIND _args "FETCH_CONTENT" _split_idx)
  if(_split_idx GREATER -1)
    math(EXPR _fc_start "${_split_idx} + 1")
    list(SUBLIST _args ${_fc_start} -1 _fc_args)
  endif()

  find_package(${_name} ${_min_ver} CONFIG QUIET)
  if(${_name}_FOUND)
    message(STATUS "${_name}: system package found (>= ${_min_ver})")
  elseif(_fc_args)
    message(STATUS "${_name}: system package not found, building from source")
    FetchContent_Declare(${_name} ${_fc_args})
    FetchContent_MakeAvailable(${_name})
  else()
    message(FATAL_ERROR "${_name}: not found (required >= ${_min_ver})")
  endif()
endmacro()

# ── FTXUI ─────────────────────────────────────────────
set(FTXUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_DOCS OFF CACHE BOOL "" FORCE)
# v6.1.9 fixes a Windows-only NULL deref in FTXUI Render() (focused.node is
# NULL when the screen has no focus element, but FTXUI_MICROSOFT_TERMINAL_FALLBACK
# defaults ON on Windows, so the Hidden-cursor branch still dereferences it).
# Linux keeps v6.0.0 (the macro is OFF there, so the branch is never taken).
# On Windows the system-package lookup is skipped entirely: a found v6.0.0
# would satisfy find_package() yet still carry the NULL deref bug.
if(WIN32)
  set(NETUNE_FTXUI_TAG v6.1.9)
  FetchContent_Declare(ftxui
    GIT_REPOSITORY https://github.com/ArthurSonzogni/FTXUI.git
    GIT_TAG        ${NETUNE_FTXUI_TAG}
  )
  FetchContent_MakeAvailable(ftxui)
else()
  set(NETUNE_FTXUI_TAG v6.0.0)
  netune_find_dep(ftxui 6.0.0
    FETCH_CONTENT
      GIT_REPOSITORY https://github.com/ArthurSonzogni/FTXUI.git
      GIT_TAG        ${NETUNE_FTXUI_TAG}
  )
endif()

# ── yyjson (official CMake) ──────────────────────────
netune_find_dep(yyjson 0.10.0
  FETCH_CONTENT
    GIT_REPOSITORY https://github.com/ibireme/yyjson.git
    GIT_TAG        0.10.0
)
# FetchContent builds the plain target 'yyjson'; provide the namespaced
# alias that the rest of the project expects.
if(NOT TARGET yyjson::yyjson AND TARGET yyjson)
  add_library(yyjson::yyjson ALIAS yyjson)
endif()

# ── Remaining deps (pkg-config) ──────────────────────
# libyaml only ships .pc files, no CMake config → must use pkg-config.
# On Windows, vcpkg's pkgconf provides this.
find_package(PkgConfig REQUIRED)
pkg_check_modules(YAML   REQUIRED IMPORTED_TARGET yaml-0.1)
pkg_check_modules(DBUS   QUIET IMPORTED_TARGET dbus-1)
pkg_check_modules(ALSA   QUIET IMPORTED_TARGET alsa)
pkg_check_modules(PULSE  QUIET IMPORTED_TARGET libpulse-simple)
pkg_check_modules(SDL2   QUIET IMPORTED_TARGET sdl2)
find_package(Threads REQUIRED)

# ── FFmpeg (system first, then build from source) ────
# NETUNE_PREFER_SYSTEM_FFMPEG=ON (default): use the system FFmpeg if it meets
# the minimum version. Otherwise build a minimal FFmpeg from source.
# NETUNE_PREFER_SYSTEM_FFMPEG=OFF: always build FFmpeg from source.
option(NETUNE_PREFER_SYSTEM_FFMPEG "Prefer system FFmpeg over building from source" ON)

# Try to locate a system FFmpeg that is new enough. FFmpeg 5.1 introduced
# ch_layout / swr_alloc_set_opts2 used by this project.
function(netune_find_system_ffmpeg out_target)
  pkg_check_modules(FFMPEG_SYSTEM QUIET IMPORTED_TARGET
    libavformat>=59.27.100
    libavcodec>=59.37.100
    libswresample>=4.7.100
    libavutil>=57.28.100)
  if(FFMPEG_SYSTEM_FOUND)
    set(${out_target} PkgConfig::FFMPEG_SYSTEM PARENT_SCOPE)
  else()
    set(${out_target} "" PARENT_SCOPE)
  endif()
endfunction()

# Build a minimal FFmpeg from the official Git repository.
function(netune_build_ffmpeg_from_source out_target)
  message(STATUS "FFmpeg: building from source")

  find_program(MAKE_EXECUTABLE make REQUIRED)
  find_program(NASM_EXECUTABLE nasm)
  if(NOT NASM_EXECUTABLE)
    find_program(YASM_EXECUTABLE yasm)
    if(NOT YASM_EXECUTABLE)
      message(FATAL_ERROR "FFmpeg: nasm or yasm is required to build from source")
    endif()
  endif()

  include(ExternalProject)
  set(FFMPEG_INSTALL_DIR ${CMAKE_BINARY_DIR}/ffmpeg-install)

  # Create placeholder directories/files so that CMake's configure-time
  # validation of IMPORTED targets succeeds. The real files are produced by
  # the ExternalProject build step.
  file(MAKE_DIRECTORY ${FFMPEG_INSTALL_DIR}/include)
  file(MAKE_DIRECTORY ${FFMPEG_INSTALL_DIR}/lib)
  foreach(_lib avformat avcodec swresample avutil)
    file(WRITE ${FFMPEG_INSTALL_DIR}/lib/lib${_lib}.so "")
  endforeach()

  ExternalProject_Add(ffmpeg_external
    PREFIX ${CMAKE_BINARY_DIR}/ffmpeg-prefix
    GIT_REPOSITORY https://github.com/FFmpeg/FFmpeg.git
    GIT_TAG n6.1
    CONFIGURE_COMMAND <SOURCE_DIR>/configure
      --prefix=${FFMPEG_INSTALL_DIR}
      --enable-shared
      --disable-static
      --disable-doc
      --disable-programs
      --disable-everything
      --enable-protocol=file,http,https,tcp,tls
      --enable-demuxer=mp3,flac,wav
      --enable-decoder=mp3,flac,pcm_s16le
      --enable-encoder=pcm_s16le
      --enable-openssl
      --enable-swresample
      --enable-avformat
      --enable-avcodec
      --enable-avutil
    BUILD_COMMAND ${MAKE_EXECUTABLE} -j${CMAKE_BUILD_PARALLEL_LEVEL}
    INSTALL_COMMAND ${MAKE_EXECUTABLE} install
    BUILD_BYPRODUCTS
      ${FFMPEG_INSTALL_DIR}/lib/libavformat.so
      ${FFMPEG_INSTALL_DIR}/lib/libavcodec.so
      ${FFMPEG_INSTALL_DIR}/lib/libswresample.so
      ${FFMPEG_INSTALL_DIR}/lib/libavutil.so
  )

  foreach(_lib avformat avcodec swresample avutil)
    add_library(ffmpeg_${_lib} SHARED IMPORTED)
    add_dependencies(ffmpeg_${_lib} ffmpeg_external)
    set_target_properties(ffmpeg_${_lib} PROPERTIES
      IMPORTED_LOCATION ${FFMPEG_INSTALL_DIR}/lib/lib${_lib}.so
      INTERFACE_INCLUDE_DIRECTORIES ${FFMPEG_INSTALL_DIR}/include
    )
  endforeach()

  add_library(PkgConfig::FFMPEG_NETUNE_BUILT INTERFACE IMPORTED)
  set_target_properties(PkgConfig::FFMPEG_NETUNE_BUILT PROPERTIES
    INTERFACE_LINK_LIBRARIES "ffmpeg_avformat;ffmpeg_avcodec;ffmpeg_swresample;ffmpeg_avutil"
    INTERFACE_INCLUDE_DIRECTORIES ${FFMPEG_INSTALL_DIR}/include
  )

  list(APPEND CMAKE_BUILD_RPATH ${FFMPEG_INSTALL_DIR}/lib)
  set(${out_target} PkgConfig::FFMPEG_NETUNE_BUILT PARENT_SCOPE)
endfunction()

# Resolve FFmpeg: system first (if allowed and new enough), then source build.
if(NETUNE_PREFER_SYSTEM_FFMPEG)
  netune_find_system_ffmpeg(NETUNE_FFMPEG_TARGET)
  if(NETUNE_FFMPEG_TARGET)
    message(STATUS "FFmpeg: using system package")
  endif()
endif()

if(NOT NETUNE_FFMPEG_TARGET)
  if(WIN32)
    message(FATAL_ERROR
      "FFmpeg: system FFmpeg not found or too old.\n"
      "On Windows, install FFmpeg via vcpkg:\n"
      "  vcpkg install ffmpeg:x64-windows\n"
      "and pass -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake")
  endif()
  netune_build_ffmpeg_from_source(NETUNE_FFMPEG_TARGET)
endif()

# ── Sources ───────────────────────────────────────────
set(CORE_SOURCES
  src/core/cover.c
  src/core/event_bus.c
  src/core/music_source.c
  src/core/music_source_manager.c
  src/core/decoder_manager.c
  src/core/audio_output_mgr.c
  src/core/playback_coordinator.c
  src/core/cache_manager.c
  src/core/search_manager.c
  src/core/spectrum.c
  src/core/term_gfx.c
)

set(INFRA_SOURCES
  src/infra/log.c
  src/infra/config.c
  src/infra/path.c
  src/infra/ring_buffer.c
  src/infra/thread_pool.c
)

set(PLUGIN_DECODER_SOURCES
  src/plugins/decoders/dr_mp3/mp3_decoder.c
  src/plugins/decoders/ffmpeg/ffmpeg_stream.c
  src/plugins/decoders/dr_flac/flac_decoder.c
  src/plugins/decoders/dr_flac/wav_decoder.c
)

set(PLUGIN_AUDIO_SOURCES)

if(ALSA_FOUND)
  list(APPEND PLUGIN_AUDIO_SOURCES src/plugins/audio_outputs/alsa/alsa_plugin.c)
  message(STATUS "ALSA found: building ALSA audio backend")
else()
  message(STATUS "ALSA not found: skipping ALSA")
endif()

if(PULSE_FOUND)
  list(APPEND PLUGIN_AUDIO_SOURCES src/plugins/audio_outputs/pulseaudio/pulse_plugin.c)
  message(STATUS "PulseAudio found: building PulseAudio audio backend")
else()
  message(STATUS "PulseAudio not found: skipping PulseAudio")
endif()

if(SDL2_FOUND)
  list(APPEND PLUGIN_AUDIO_SOURCES src/plugins/audio_outputs/sdl/sdl_plugin.c)
  message(STATUS "SDL2 found: building SDL audio backend")
else()
  message(STATUS "SDL2 not found: skipping SDL (needed on Windows)")
endif()

set(PLUGIN_LYRIC_SOURCES
  src/plugins/lyrics/lrc/lrc_parser.c
)

set(PLUGIN_MUSIC_SOURCES
  src/plugins/music_sources/local/local_source.c
  src/plugins/music_sources/netease/netease_source.c
  src/plugins/music_sources/netease/netease_api.c
)

set(UI_SOURCES
  src/ui/state_store.cpp
  src/ui/keybindings.cpp
  src/ui/theme.cpp
  src/ui/layout_engine.cpp
  src/ui/components/theme_util.cpp
  src/ui/components/top_bar.cpp
  src/ui/components/group_list.cpp
  src/ui/components/song_list.cpp
  src/ui/components/status_bar.cpp
  src/ui/components/help_screen.cpp
  src/ui/components/login_screen.cpp
  src/ui/components/spinner.cpp
  src/ui/components/lyric_panel.cpp
)

if(DBUS_FOUND AND NOT WIN32)
  list(APPEND UI_SOURCES src/ui/mpris.cpp)
  message(STATUS "D-Bus found: building MPRIS support")
else()
  message(STATUS "D-Bus not found: skipping MPRIS")
endif()

set(ALL_SOURCES
  src/app.cpp
  src/main.cpp
  ${CORE_SOURCES}
  ${INFRA_SOURCES}
  ${PLUGIN_DECODER_SOURCES}
  ${PLUGIN_AUDIO_SOURCES}
  ${PLUGIN_MUSIC_SOURCES}
  ${PLUGIN_LYRIC_SOURCES}
  ${UI_SOURCES}
)

# ── Executable ───────────────────────────────────────
add_executable(netune ${ALL_SOURCES})

target_include_directories(netune PRIVATE src)
if(NOT WIN32)
  target_compile_definitions(netune PRIVATE _GNU_SOURCE)
endif()
if(ALSA_FOUND)
  target_compile_definitions(netune PRIVATE HAS_ALSA)
endif()
if(PULSE_FOUND)
  target_compile_definitions(netune PRIVATE HAS_PULSE)
endif()
if(SDL2_FOUND)
  target_compile_definitions(netune PRIVATE HAS_SDL)
endif()

target_link_libraries(netune PRIVATE
  ftxui::screen
  ftxui::dom
  ftxui::component
  yyjson::yyjson
  PkgConfig::YAML
  ${NETUNE_FFMPEG_TARGET}
  Threads::Threads
)

if(ALSA_FOUND)
  target_link_libraries(netune PRIVATE PkgConfig::ALSA)
endif()
if(PULSE_FOUND)
  target_link_libraries(netune PRIVATE PkgConfig::PULSE)
endif()
if(SDL2_FOUND)
  target_link_libraries(netune PRIVATE PkgConfig::SDL2)
endif()
if(DBUS_FOUND AND NOT WIN32)
  target_link_libraries(netune PRIVATE PkgConfig::DBUS)
endif()

if(NOT WIN32)
  target_link_libraries(netune PRIVATE dl m)
endif()

if(WIN32)
  # Both MSVC and MinGW-w64 need the compat headers to shadow POSIX names
  # (dirent.h, strings.h, utf8.h, wcwidth_compat.h, wordexp.h)
  target_include_directories(netune BEFORE PRIVATE src/compat)
endif()

if(MSVC)
  # MSVC also needs the pthread shim (MinGW-w64 provides its own <pthread.h>)
  target_include_directories(netune BEFORE PRIVATE src/compat/msvc)
  target_compile_definitions(netune PRIVATE _CRT_NONSTDC_NO_DEPRECATE)
  target_compile_options(netune PRIVATE /utf-8)
endif()

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(netune PRIVATE -Wall -Wextra)
endif()
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(netune PRIVATE -Wall -Wextra)
endif()

# ── netease-cli (Go helper for Netease Cloud Music) ───
# The binary is built from source when a Go toolchain is available, or
# downloaded prebuilt from GitHub releases otherwise. Both paths run as
# build-time custom commands via `cmake -P` scripts, so a failure never
# aborts the build and is retried on the next build. The output lands in
# the build directory next to netune.exe, where cli_path() looks for it.
find_program(GO_EXECUTABLE go)
set(NETEASE_CLI_DIR "${CMAKE_SOURCE_DIR}/src/plugins/music_sources/netease/netease-cli")
if(WIN32)
  set(NETEASE_CLI_OUT "${CMAKE_BINARY_DIR}/netease-cli.exe")
else()
  set(NETEASE_CLI_OUT "${CMAKE_BINARY_DIR}/netease-cli")
endif()

# netease-cli's Go module requires go >= 1.22 (uses math/rand/v2). A too-old
# go binary silently fails the build, which would leave Netease features dead
# on Linux/macOS. Verify the version and fall back to the prebuilt download
# when the toolchain is absent or too old.
set(GO_MEETS_VERSION FALSE)
if(GO_EXECUTABLE)
  execute_process(
    COMMAND "${GO_EXECUTABLE}" version
    OUTPUT_VARIABLE _go_ver_out
    ERROR_VARIABLE _go_ver_err
    RESULT_VARIABLE _go_ver_rc
  )
  if(_go_ver_rc EQUAL 0 AND _go_ver_out MATCHES "go([0-9]+)\\.([0-9]+)")
    set(_go_major "${CMAKE_MATCH_1}")
    set(_go_minor "${CMAKE_MATCH_2}")
    if(_go_major GREATER 1 OR (_go_major EQUAL 1 AND _go_minor GREATER_EQUAL 22))
      set(GO_MEETS_VERSION TRUE)
    endif()
  endif()
endif()

if(GO_MEETS_VERSION)
  add_custom_command(
    OUTPUT  "${NETEASE_CLI_OUT}"
    COMMAND ${CMAKE_COMMAND}
            -DGO_EXECUTABLE=${GO_EXECUTABLE}
            -DNETEASE_CLI_OUT=${NETEASE_CLI_OUT}
            -DNETEASE_CLI_DIR=${NETEASE_CLI_DIR}
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/build-netease-cli.cmake"
    WORKING_DIRECTORY "${NETEASE_CLI_DIR}"
    COMMENT "Building netease-cli (Go helper)..."
    VERBATIM
  )
  message(STATUS "netease-cli: Go ${GO_EXECUTABLE} found and >= 1.22, will auto-build")
else()
  if(GO_EXECUTABLE)
    message(STATUS "netease-cli: Go found but too old (need >= 1.22), will download prebuilt binary")
  else()
    message(STATUS "netease-cli: Go not found, will download prebuilt binary")
  endif()
  # No (usable) Go toolchain: fetch the prebuilt netease-cli from the
  # GitHub release (built by CI) so Netease features work without Go.
  if(WIN32)
    set(NETEASE_CLI_ASSET "netease-cli-windows-x86_64.zip")
  elseif(APPLE)
    set(NETEASE_CLI_ASSET "netease-cli-macos-x86_64.tar.gz")
  else()
    set(NETEASE_CLI_ASSET "netease-cli-linux-x86_64.tar.gz")
  endif()
  set(NETEASE_CLI_URL
      "https://github.com/ThripleQ/Netune/releases/latest/download/${NETEASE_CLI_ASSET}")
  # GitHub release mirrors for the prebuilt download; candidates are
  # speed-probed at build time and the fastest one is used.
  set(NETEASE_CLI_MIRRORS
      "https://gh-proxy.com/;https://ghfast.top/"
      CACHE STRING "Semicolon-list of mirror prefixes for netease-cli downloads")
  add_custom_command(
    OUTPUT  "${NETEASE_CLI_OUT}"
    COMMAND ${CMAKE_COMMAND}
            -DNETEASE_CLI_URL=${NETEASE_CLI_URL}
            -DNETEASE_CLI_ASSET=${NETEASE_CLI_ASSET}
            -DNETEASE_CLI_BIN_DIR=${CMAKE_BINARY_DIR}
            -DNETEASE_CLI_MIRRORS=${NETEASE_CLI_MIRRORS}
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/fetch-netease-cli.cmake"
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    COMMENT "Downloading prebuilt netease-cli (${NETEASE_CLI_ASSET})..."
    VERBATIM
  )
endif()

add_custom_target(netease-cli-build ALL DEPENDS "${NETEASE_CLI_OUT}")
add_dependencies(netune netease-cli-build)

# Install netune and netease-cli side by side so cli_path() finds the
# helper next to the main executable. OPTIONAL: if the helper failed to
# build/download, installation still succeeds.
install(TARGETS netune RUNTIME DESTINATION bin)
install(PROGRAMS "${NETEASE_CLI_OUT}" DESTINATION bin OPTIONAL)
