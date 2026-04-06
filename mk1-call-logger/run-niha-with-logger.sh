#!/bin/zsh

set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
DEFAULT_DYLIB="$SCRIPT_DIR/../build/Debug/libmk1-call-logger.dylib"
DEFAULT_LOG="/tmp/mk1-call-logger.log"

usage() {
  cat <<'EOF'
Usage:
  run-niha-with-logger.sh [NIHardwareAgent-path] [log-path]

Behavior:
  - Sets MK1_CALL_LOGGER_PATH
  - Sets DYLD_INSERT_LIBRARIES to libmk1-call-logger.dylib
  - Sets DYLD_FORCE_FLAT_NAMESPACE=1
  - Execs NIHardwareAgent under the logger

Environment overrides:
  MK1_CALL_LOGGER_DYLIB   Path to libmk1-call-logger.dylib
  MK1_CALL_LOGGER_PATH    Output log path

Examples:
  ./run-niha-with-logger.sh /Applications/Native\ Instruments/NIHardwareAgent.app/Contents/MacOS/NIHardwareAgent
  ./run-niha-with-logger.sh /path/to/NIHardwareAgent /tmp/niha-kext.log
EOF
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  usage
  exit 0
fi

NIHA_PATH="${1:-}"
LOG_PATH="${2:-${MK1_CALL_LOGGER_PATH:-$DEFAULT_LOG}}"
DYLIB_PATH="${MK1_CALL_LOGGER_DYLIB:-$DEFAULT_DYLIB}"

if [[ -z "$NIHA_PATH" ]]; then
  for candidate in \
    "/Applications/Native Instruments/NIHardwareAgent.app/Contents/MacOS/NIHardwareAgent" \
    "/Library/Application Support/Native Instruments/Hardware/NIHardwareAgent.app/Contents/MacOS/NIHardwareAgent"
  do
    if [[ -x "$candidate" ]]; then
      NIHA_PATH="$candidate"
      break
    fi
  done
fi

if [[ -z "$NIHA_PATH" ]]; then
  echo "NIHardwareAgent path not provided and no default installation was found." >&2
  usage >&2
  exit 1
fi

if [[ ! -x "$NIHA_PATH" ]]; then
  echo "NIHardwareAgent is not executable: $NIHA_PATH" >&2
  exit 1
fi

if [[ ! -f "$DYLIB_PATH" ]]; then
  echo "Logger dylib not found: $DYLIB_PATH" >&2
  exit 1
fi

mkdir -p "$(dirname "$LOG_PATH")"

export MK1_CALL_LOGGER_PATH="$LOG_PATH"
export DYLD_INSERT_LIBRARIES="$DYLIB_PATH"
export DYLD_FORCE_FLAT_NAMESPACE=1

echo "Launching NIHardwareAgent with mk1-call-logger"
echo "  NIHardwareAgent: $NIHA_PATH"
echo "  Logger dylib:    $DYLIB_PATH"
echo "  Log path:        $LOG_PATH"

exec "$NIHA_PATH"
