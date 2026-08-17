# Download and extract the prebuilt netease-cli binary from GitHub
# releases (used when no Go toolchain is available).
#
# Invoked by CMakeLists.txt via `cmake -P`. Failure is tolerated: the
# netune runtime degrades gracefully when the binary is missing, but a
# loud warning with fix instructions is printed so the user knows the
# Netease features are off.
#
# Required -D variables:
#   NETEASE_CLI_URL       full download URL
#   NETEASE_CLI_ASSET     archive file name (e.g. netease-cli-windows-x86_64.zip)
#   NETEASE_CLI_BIN_DIR   directory to extract into (usually CMAKE_BINARY_DIR)
# Optional -D variables:
#   NETEASE_CLI_MIRRORS   semicolon-list of mirror prefixes (e.g.
#                         "https://gh-proxy.com/;https://ghfast.top/")
#                         prepended to NETEASE_CLI_URL. All candidates are
#                         speed-probed and the fastest one is used.

if(NOT NETEASE_CLI_URL OR NOT NETEASE_CLI_ASSET OR NOT NETEASE_CLI_BIN_DIR)
  message(WARNING "netease-cli: missing required script variables")
  return()
endif()

set(_archive "${NETEASE_CLI_BIN_DIR}/${NETEASE_CLI_ASSET}")

# ── candidate list: official + mirrors ─────────────────
set(_candidates "${NETEASE_CLI_URL}")
foreach(_m IN LISTS NETEASE_CLI_MIRRORS)
  if(_m)
    list(APPEND _candidates "${_m}${NETEASE_CLI_URL}")
  endif()
endforeach()

# ── speed probe (curl range request, ~2MB) ──────────────
find_program(_curl NAMES curl curl.exe)
set(_best_url "")
set(_best_speed -1)
set(_probe "${NETEASE_CLI_BIN_DIR}/.netease-cli-probe.bin")

if(_curl)
  foreach(_u IN LISTS _candidates)
    execute_process(
      COMMAND "${_curl}" -fsL --max-time 8 -r 0-2097151
              -o "${_probe}" -w "%{speed_download}" "${_u}"
      RESULT_VARIABLE _probe_rc
      OUTPUT_VARIABLE _probe_speed
    )
    file(REMOVE "${_probe}")
    if(_probe_rc EQUAL 0 AND _probe_speed MATCHES "^[0-9]+(\\.[0-9]+)?$")
      # CMake math is integer-only; compare the whole part of the speed.
      string(REGEX REPLACE "\\..*$" "" _probe_int "${_probe_speed}")
      if(_probe_int GREATER _best_speed)
        set(_best_speed "${_probe_int}")
        set(_best_url "${_u}")
      endif()
    endif()
  endforeach()
  if(_best_url)
    message(STATUS "netease-cli: fastest source picked (${_best_speed} B/s): ${_best_url}")
    execute_process(
      COMMAND "${_curl}" -fsL --retry 2 --connect-timeout 15 --max-time 300
              -o "${_archive}" "${_best_url}"
      RESULT_VARIABLE _dl_rc
    )
  else()
    message(WARNING "netease-cli: all download sources unreachable, trying plain download")
    set(_dl_rc 1)
  endif()
else()
  # No curl: fall back to a single plain download attempt.
  file(DOWNLOAD "${NETEASE_CLI_URL}" "${_archive}"
       STATUS _dl_status TLS_VERIFY ON TIMEOUT 120)
  list(GET _dl_status 0 _dl_rc)
endif()

if(NOT _dl_rc EQUAL 0)
  file(REMOVE "${_archive}" "${_probe}")
  message(WARNING
    "============================================================\n"
    "  netease-cli: 预编译二进制下载失败 — 网易云功能不可用\n"
    "  尝试的下载源: ${_candidates}\n"
    "  修复方法:\n"
    "    - 检查网络/代理后重新运行 cmake --build（会自动重试）\n"
    "    - 安装 Go >= 1.22 并重新 configure（cmake -B build）改走源码构建\n"
    "============================================================")
  return()
endif()

file(SIZE "${_archive}" _archive_size)
if(_archive_size LESS 1024)
  file(REMOVE "${_archive}")
  message(WARNING "netease-cli: downloaded archive is too small (${_archive_size} B), removing")
  return()
endif()

file(ARCHIVE_EXTRACT INPUT "${_archive}" DESTINATION "${NETEASE_CLI_BIN_DIR}"
     RESULT_VARIABLE _extract_rc)
if(NOT _extract_rc EQUAL 0)
  message(WARNING
    "============================================================\n"
    "  netease-cli: 压缩包解压失败（${NETEASE_CLI_ASSET}）— 网易云功能不可用\n"
    "  ${_extract_rc}\n"
    "  修复方法: 删除 ${_archive} 后重新 cmake --build\n"
    "============================================================")
endif()