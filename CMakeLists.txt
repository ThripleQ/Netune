cmake_minimum_required(VERSION 3.16)
project(lmusic VERSION 2.0.0 LANGUAGES C CXX)

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
find_package(PkgConfig REQUIRED)
pkg_check_modules(YYJSON REQUIRED IMPORTED_TARGET yyjson)
pkg_check_modules(YAML   REQUIRED IMPORTED_TARGET yaml-0.1)
pkg_check_modules(CURL   REQUIRED IMPORTED_TARGET libcurl)
pkg_check_modules(ALSA   REQUIRED IMPORTED_TARGET alsa)
pkg_check_modules(PULSE  REQUIRED IMPORTED_TARGET libpulse-simple)
find_package(Threads REQUIRED)

# ── Sources ───────────────────────────────────────────
set(CORE_SOURCES
  src/core/event_bus.c
  src/core/music_source.c
  src/core/music_source_manager.c
  src/core/decoder_manager.c
  src/core/audio_output_mgr.c
  src/core/playback_coordinator.c
  src/core/playlist_manager.c
)

set(INFRA_SOURCES
  src/infra/log.c
  src/infra/config.c
)

set(PLUGIN_DECODER_SOURCES
  src/plugins/decoders/dr_mp3/mp3_decoder.c
  src/plugins/decoders/dr_flac/flac_decoder.c
  src/plugins/decoders/dr_flac/wav_decoder.c
)

set(PLUGIN_AUDIO_SOURCES
  src/plugins/audio_outputs/alsa/alsa_plugin.c
)

set(PLUGIN_MUSIC_SOURCES
  src/plugins/music_sources/local/local_source.c
)

set(UI_SOURCES
  src/ui/state_store.cpp
  src/ui/keybindings.cpp
  src/ui/components/group_list.cpp
  src/ui/components/song_list.cpp
  src/ui/components/status_bar.cpp
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
add_executable(lmusic ${ALL_SOURCES})
target_include_directories(lmusic PRIVATE src)
target_compile_definitions(lmusic PRIVATE _GNU_SOURCE)

target_link_libraries(lmusic PRIVATE
  ftxui::screen
  ftxui::dom
  ftxui::component
  PkgConfig::YYJSON
  PkgConfig::YAML
  PkgConfig::CURL
  PkgConfig::ALSA
  PkgConfig::PULSE
  Threads::Threads
  dl m
)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(lmusic PRIVATE -Wall -Wextra)
endif()
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(lmusic PRIVATE -Wall -Wextra)
endif()
