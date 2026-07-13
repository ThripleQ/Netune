cmake_minimum_required(VERSION 3.16)
project(netune VERSION 2.0.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── FTXUI ─────────────────────────────────────────────
include(FetchContent)
FetchContent_Declare(
  ftxui
  GIT_REPOSITORY https://github.com/ArthurSonzogni/FTXUI.git
  GIT_TAG        v6.0.0
)
set(FTXUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_DOCS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(ftxui)

# ── System deps ──────────────────────────────────────
# Linux: use pkg-config; Windows (vcpkg): use find_package
find_package(PkgConfig QUIET)

if(PkgConfig_FOUND)
  pkg_check_modules(YYJSON REQUIRED IMPORTED_TARGET yyjson)
  pkg_check_modules(YAML   REQUIRED IMPORTED_TARGET yaml-0.1)
  pkg_check_modules(ALSA   QUIET IMPORTED_TARGET alsa)
  pkg_check_modules(PULSE  QUIET IMPORTED_TARGET libpulse-simple)
  pkg_check_modules(SDL2   QUIET IMPORTED_TARGET sdl2)
  pkg_check_modules(FFMPEG QUIET IMPORTED_TARGET
    libavformat libavcodec libswresample libavutil)
else()
  # Windows / vcpkg fallback
  find_package(yyjson REQUIRED)
  find_package(yaml REQUIRED)
  find_package(ALSA QUIET)
  find_package(PulseAudio QUIET)
  find_package(SDL2 QUIET)
  find_package(FFMPEG QUIET
    COMPONENTS libavformat libavcodec libswresample libavutil)
endif()
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
  Threads::Threads
)

if(PkgConfig_FOUND)
  target_link_libraries(netune PRIVATE
    PkgConfig::YYJSON PkgConfig::YAML PkgConfig::FFMPEG)
  if(ALSA_FOUND)  target_link_libraries(netune PRIVATE PkgConfig::ALSA)  endif()
  if(PULSE_FOUND) target_link_libraries(netune PRIVATE PkgConfig::PULSE) endif()
  if(SDL2_FOUND)  target_link_libraries(netune PRIVATE PkgConfig::SDL2)  endif()
else()
  target_link_libraries(netune PRIVATE yyjson::yyjson yaml::yaml)
  if(ALSA_FOUND)       target_link_libraries(netune PRIVATE ALSA::ALSA)         endif()
  if(PulseAudio_FOUND) target_link_libraries(netune PRIVATE PulseAudio::PulseAudio) endif()
  if(SDL2_FOUND)       target_link_libraries(netune PRIVATE SDL2::SDL2)         endif()
  if(FFMPEG_FOUND)     target_link_libraries(netune PRIVATE
    FFMPEG::libavformat FFMPEG::libavcodec FFMPEG::libswresample FFMPEG::libavutil) endif()
endif()

if(NOT WIN32)
  target_link_libraries(netune PRIVATE dl m)
endif()

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(netune PRIVATE -Wall -Wextra)
endif()
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(netune PRIVATE -Wall -Wextra)
endif()
