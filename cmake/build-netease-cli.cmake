# Build netease-cli (Go helper) from source.
#
# Invoked by CMakeLists.txt via `cmake -P`. Failure is tolerated: the
# netune runtime resolves the binary next to its own executable and
# degrades gracefully (Netease features off) when it is missing.
#
# Required -D variables:
#   GO_EXECUTABLE    path to the go toolchain
#   NETEASE_CLI_OUT  output binary path (e.g. <build>/netease-cli.exe)
#   NETEASE_CLI_DIR  directory containing go.mod/main.go

if(NOT GO_EXECUTABLE OR NOT NETEASE_CLI_OUT OR NOT NETEASE_CLI_DIR)
  message(WARNING "netease-cli: missing required script variables")
  return()
endif()

execute_process(
  COMMAND "${GO_EXECUTABLE}" build "-ldflags=-s -w" -o "${NETEASE_CLI_OUT}" .
  WORKING_DIRECTORY "${NETEASE_CLI_DIR}"
  RESULT_VARIABLE _rc
  OUTPUT_VARIABLE _out
  ERROR_VARIABLE _err
)

if(NOT _rc EQUAL 0)
  # Remove any half-written binary so the runtime never picks up a
  # truncated helper; the missing OUTPUT file also triggers a retry
  # on the next build.
  file(REMOVE "${NETEASE_CLI_OUT}")
  message(WARNING
    "============================================================\n"
    "  netease-cli: Go 源码构建失败 — 网易云功能不可用\n"
    "  go 命令: ${GO_EXECUTABLE}\n"
    "  错误信息: ${_err}\n"
    "  修复方法:\n"
    "    - 确认 go >= 1.22 且可离线拉取模块依赖（go env GOPROXY）\n"
    "    - 修复后重新运行 cmake --build（会自动重试）\n"
    "============================================================")
endif()
