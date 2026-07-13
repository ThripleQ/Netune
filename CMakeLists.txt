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
find_package(PkgConfig REQUIRED)
pkg_check_modules(YYJSON REQUIRED IMPORTED_TARGET yyjson)
pkg_check_modules(YAML   REQUIRED IMPORTED_TARGET yaml-0.1)
pkg_check_modules(ALSA   REQUIRED IMPORTED_TARGET alsa)
pkg_check_modules(PULSE  REQUIRED IMPORTED_TARGET libpulse-simple)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
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

set(PLUGIN_AUDIO_SOURCES
  src/plugins/audio_outputs/alsa/alsa_plugin.c
  src/plugins/audio_outputs/pulseaudio/pulse_plugin.c
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
target_include_directories(netune PRIVATE src)
target_compile_definitions(netune PRIVATE _GNU_SOURCE)

target_link_libraries(netune PRIVATE
  ftxui::screen
  ftxui::dom
  ftxui::component
  PkgConfig::YYJSON
  PkgConfig::YAML
  PkgConfig::ALSA
  PkgConfig::PULSE
  PkgConfig::FFMPEG
  Threads::Threads
  dl m
)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(netune PRIVATE -Wall -Wextra)
endif()
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(netune PRIVATE -Wall -Wextra)
endif()
