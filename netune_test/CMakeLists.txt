# netune_test — regression tests for the streaming / audio-cache core.
#
# Opt in with -DNETUNE_BUILD_TESTS=ON (see the top-level CMakeLists.txt).
# The tests link the REAL core sources, not copies, so a behavioural change
# in stream_downloader / ffmpeg_stream / audio_cache shows up here.

set(NETUNE_TEST_CORE_SOURCES
  ${CMAKE_SOURCE_DIR}/src/core/music_source.c
  ${CMAKE_SOURCE_DIR}/src/core/stream_downloader.c
  ${CMAKE_SOURCE_DIR}/src/core/audio_cache.c
  ${CMAKE_SOURCE_DIR}/src/core/cache_segments.c
  ${CMAKE_SOURCE_DIR}/src/plugins/decoders/ffmpeg/ffmpeg_stream.c
  ${CMAKE_SOURCE_DIR}/src/infra/config.c
  ${CMAKE_SOURCE_DIR}/src/infra/config_paths.c
  ${CMAKE_SOURCE_DIR}/src/infra/log.c
)

add_library(netune_test_core STATIC ${NETUNE_TEST_CORE_SOURCES})
target_include_directories(netune_test_core PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(netune_test_core PUBLIC
  yyjson::yyjson
  ${NETUNE_FFMPEG_TARGET}
  Threads::Threads
)

if(NOT WIN32)
  target_compile_definitions(netune_test_core PUBLIC _GNU_SOURCE)
  target_link_libraries(netune_test_core PUBLIC dl m)
endif()

# stream_downloader.c compiles its real libcurl body only with HAVE_LIBCURL;
# without the define it degrades to a stub and test_download would silently
# assert nothing.
if(TARGET CURL::libcurl)
  target_link_libraries(netune_test_core PUBLIC CURL::libcurl)
  target_compile_definitions(netune_test_core PUBLIC HAVE_LIBCURL)
else()
  message(FATAL_ERROR
    "NETUNE_BUILD_TESTS=ON requires libcurl (CURL::libcurl); "
    "test_download needs the real stream_downloader.")
endif()

set(NETUNE_TESTS
  test_grow
  test_stream
  test_download
  test_partial
  test_cache
  test_netease_map
)

foreach(_t IN LISTS NETUNE_TESTS)
  add_executable(${_t} ${_t}.c test_util.c http_range_server.c)
  target_link_libraries(${_t} PRIVATE netune_test_core)
  add_test(NAME ${_t} COMMAND ${_t})
endforeach()
