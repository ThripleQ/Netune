cmake_minimum_required(VERSION 3.16)
project(netune VERSION 2.0.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── FetchContent helper ────────────────────────────────
include(FetchContent)

# ── FTXUI ─────────────────────────────────────────────
FetchContent_Declare(
  ftxui
  GIT_REPOSITORY https://github.com/ArthurSonzogni/FTXUI.git
  GIT_TAG        v6.0.0
)
set(FTXUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_DOCS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(ftxui)

# ── yyjson (official CMake) ──────────────────────────
# Try system install first; fall back to FetchContent
find_package(yyjson CONFIG QUIET)
if(NOT yyjson_FOUND)
  message(STATUS "yyjson: not found as system package, building from source")
  FetchContent_Declare(
    yyjson
    GIT_REPOSITORY https://github.com/ibireme/yyjson.git
    GIT_TAG        0.10.0
  )
  FetchContent_MakeAvailable(yyjson)
endif()

# ── Remaining deps (pkg-config) ──────────────────────
# libyaml only ships .pc files, no CMake config → must use pkg-config.
# On Windows, vcpkg's pkgconf provides this.
find_package(PkgConfig REQUIRED)
pkg_check_modules(YAML   REQUIRED IMPORTED_TARGET yaml-0.1)
pkg_check_modules(ALSA   QUIET IMPORTED_TARGET alsa)
pkg_check_modules(PULSE  QUIET IMPORTED_TARGET libpulse-simple)
pkg_check_modules(SDL2   QUIET IMPORTED_TARGET sdl2)
pkg_check_modules(FFMPEG QUIET IMPORTED_TARGET
  libavformat libavcodec libswresample libavutil)
find_package(Threads REQUIRED)

# ── Sources ───────────────────────────────────────────
set(CORE_SOURCES
  src/core/event_bus.c
  src/core/music_source.c
  src/core/music_source_manager.c
  src/core/decoder_manager.c
  src/core/audio_output_mgr.c
  src/core/playback_coordinator.c
  src/core/cache_manager.c
  src/core/search_manager.c
)

set(INFRA_SOURCES
  src/infra/log.c
  src/infra/config.c
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
  message(STATUS "PulseAudio found: building PulseAudio backend")
else()
  message(STATUS "PulseAudio not found: skipping PulseAudio")
endif()

if(SDL2_FOUND)
  list(APPEND PLUGIN_AUDIO_SOURCES src/plugins/audio_outputs/sdl/sdl_plugin.c)
  message(STATUS "SDL2 found: building SDL audio backend")
else()
  message(STATUS "SDL2 not found: skipping SDL (needed on Windows)")
endif()

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
)

set(ALL_SOURCES
  src/app.cpp
  src/main.cpp
  ${CORE_SOURCES}
  ${INFRA_SOURCES}
  ${PLUGIN_DECODER_SOURCES}
  ${PLUGIN_AUDIO_SOURCES}
  ${PLUGIN_MUSIC_SOURCES}
  ${UI_SOURCES}
)

# ── Executable ───────────────────────────────────────
add_executable(netune ${ALL_SOURCES})
# ── netease-cli ──────────────────────────────────────
# Pre-built binary for the host platform; fall back to PATH otherwise
if(EXISTS "${CMAKE_SOURCE_DIR}/bin/netease-cli")
  configure_file("${CMAKE_SOURCE_DIR}/bin/netease-cli" "${CMAKE_BINARY_DIR}/netease-cli" COPYONLY)
  message(STATUS "netease-cli: using pre-built binary")
endif()
if(EXISTS "${CMAKE_BINARY_DIR}/netease-cli")
  message(STATUS "netease-cli: pre-built copied to build dir")
endif()

target_include_directories(netune PRIVATE src)
target_compile_definitions(netune PRIVATE _GNU_SOURCE)
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
  PkgConfig::FFMPEG
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

if(NOT WIN32)
  target_link_libraries(netune PRIVATE dl m)
endif()

if(MSVC)
  target_compile_definitions(netune PRIVATE _CRT_NONSTDC_NO_DEPRECATE)
endif()

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(netune PRIVATE -Wall -Wextra)
endif()
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(netune PRIVATE -Wall -Wextra)
endif()
