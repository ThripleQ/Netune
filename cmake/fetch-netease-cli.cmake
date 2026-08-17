# Download and extract the prebuilt netease-cli binary from GitHub
# releases (used when no Go toolchain is available).
#
# Invoked by CMakeLists.txt via `cmake -P`. Failure is tolerated: the
# netune runtime degrades gracefully when the binary is missing.
#
# Required -D variables:
#   NETEASE_CLI_URL       full download URL
#   NETEASE_CLI_ASSET     archive file name (e.g. netease-cli-windows-x86_64.zip)
#   NETEASE_CLI_BIN_DIR   directory to extract into (usually CMAKE_BINARY_DIR)

if(NOT NETEASE_CLI_URL OR NOT NETEASE_CLI_ASSET OR NOT NETEASE_CLI_BIN_DIR)
  message(WARNING "netease-cli: missing required script variables")
  return()
endif()

set(_archive "${NETEASE_CLI_BIN_DIR}/${NETEASE_CLI_ASSET}")

file(DOWNLOAD "${NETEASE_CLI_URL}" "${_archive}"
     STATUS _dl_status TLS_VERIFY ON TIMEOUT 120)

list(GET _dl_status 0 _dl_rc)
if(NOT _dl_rc EQUAL 0)
  list(GET _dl_status 1 _dl_msg)
  message(WARNING
    "netease-cli: prebuilt download failed (Netease features unavailable): ${_dl_msg}")
  return()
endif()

file(ARCHIVE_EXTRACT INPUT "${_archive}" DESTINATION "${NETEASE_CLI_BIN_DIR}")
