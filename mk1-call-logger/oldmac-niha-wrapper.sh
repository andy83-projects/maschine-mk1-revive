#!/bin/zsh

set -euo pipefail

REAL_BIN="/Library/Application Support/Native Instruments/Hardware/NIHardwareAgent.app/Contents/MacOS/NIHardwareAgent.real"
LOGGER_DYLIB="/private/tmp/libmk1-call-logger.dylib"
LOG_PATH="/tmp/mk1-call-logger.log"

if [[ ! -x "$REAL_BIN" ]]; then
  echo "Missing real NIHardwareAgent binary: $REAL_BIN" >&2
  exit 1
fi

if [[ ! -f "$LOGGER_DYLIB" ]]; then
  echo "Missing logger dylib: $LOGGER_DYLIB" >&2
  exit 1
fi

export MK1_CALL_LOGGER_PATH="${MK1_CALL_LOGGER_PATH:-$LOG_PATH}"
export DYLD_INSERT_LIBRARIES="$LOGGER_DYLIB"
export DYLD_FORCE_FLAT_NAMESPACE=1

exec "$REAL_BIN" "$@"
