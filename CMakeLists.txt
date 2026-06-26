cmake_minimum_required(VERSION 3.16)
project(lmusic VERSION 2.0.0 LANGUAGES C CXX)

# ── C11 / C++17 ──────────────────────────────────────
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── FTXUI (FetchContent) ─────────────────────────────
include(FetchContent)
FetchContent_Declare(
  ftxui
  GIT_REPOSITORY https://github.com/ArthurSonzogni/FTXUI.git
  GIT_TAG        v6.0.0
)
set(F TXUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(F TXUI_BUILD_DOCS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(ftxui)

# ── System deps ──────────────────────────────────────
find_package(PkgConfig REQUIRED)

pkg_check_modules(YYJSON REQUIRED IMPORTED_TARGET yyjson)
pkg_check_modules(YAML   REQUIRED IMPORTED_TARGET yaml-0.1)
pkg_check_modules(CURL   REQUIRED IMPORTED_TARGET libcurl)
pkg_check_modules(ALSA   REQUIRED IMPORTED_TARGET alsa)
pkg_check_modules(PULSE  REQUIRED IMPORTED_TARGET libpulse-simple)

find_package(Threads REQUIRED)

# ── Source files ─────────────────────────────────────
set(CORE_SOURCES
  src/core/event_bus.c
  src/core/music_source.c
)

set(INFRA_SOURCES
  src/infra/log.c
  src/infra/config.c
)

set(UI_SOURCES
  src/ui/state_store.cpp
)

set(ALL_SOURCES
  src/main.cpp
  ${CORE_SOURCES}
  ${INFRA_SOURCES}
  ${UI_SOURCES}
)

# ── Executable ───────────────────────────────────────
add_executable(lmusic ${ALL_SOURCES})

target_include_directories(lmusic PRIVATE src)

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
  dl
)

# ── Compiler warnings ────────────────────────────────
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(lmusic PRIVATE -Wall -Wextra -Wpedantic)
endif()
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(lmusic PRIVATE -Wall -Wextra -Wpedantic)
endif()
