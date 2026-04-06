#!/bin/zsh

set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
SNIFFER_SRC="$SCRIPT_DIR/mk1_ipc_sniffer.c"
SNIFFER_DYLIB="${MK1_IPC_SNIFFER_DYLIB:-/tmp/libmk1-ipc-sniffer.dylib}"
SNIFFER_LOG="${MK1_IPC_SNIFFER_PATH:-/tmp/mk1-ipc-sniffer.log}"

usage() {
  cat <<'EOF'
Usage:
  run-maschine-with-sniffer.sh [executable-path]

Compiles the IPC sniffer dylib (if needed), then launches the given
executable with DYLD_INSERT_LIBRARIES to passively log all CFMessagePort
traffic.

The sniffer does NOT interfere with normal operation — it wraps
CFMessagePortSendRequest and CFMessagePortCreateLocal callbacks to log
messages before passing them through.

Arguments:
  executable-path   Path to Maschine 2 (or NIHardwareAgent) binary.
                    If omitted, searches standard NI install locations.

Environment overrides:
  MK1_IPC_SNIFFER_DYLIB   Path for compiled dylib (default: /tmp/libmk1-ipc-sniffer.dylib)
  MK1_IPC_SNIFFER_PATH    Output log path (default: /tmp/mk1-ipc-sniffer.log)

Examples:
  # Sniff Maschine 2 IPC
  ./run-maschine-with-sniffer.sh "/Applications/Native Instruments/Maschine 2/Maschine 2.app/Contents/MacOS/Maschine 2"

  # Sniff NIHardwareAgent IPC
  ./run-maschine-with-sniffer.sh "/Applications/Native Instruments/NIHardwareAgent.app/Contents/MacOS/NIHardwareAgent"

  # Watch log live in another terminal
  tail -f /tmp/mk1-ipc-sniffer.log
EOF
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  usage
  exit 0
fi

# --- Compile if needed ---
if [[ ! -f "$SNIFFER_DYLIB" || "$SNIFFER_SRC" -nt "$SNIFFER_DYLIB" ]]; then
  echo "Compiling sniffer dylib..."
  cc -dynamiclib -framework CoreFoundation \
     -arch x86_64 \
     -mmacosx-version-min=10.13 \
     -o "$SNIFFER_DYLIB" "$SNIFFER_SRC"
  echo "  -> $SNIFFER_DYLIB"
fi

# --- Find target executable ---
TARGET_PATH="${1:-}"

if [[ -z "$TARGET_PATH" ]]; then
  for candidate in \
    "/Applications/Native Instruments/Maschine 2/Maschine 2.app/Contents/MacOS/Maschine 2" \
    "/Applications/Native Instruments/Maschine 2.app/Contents/MacOS/Maschine 2" \
    "/Applications/Maschine 2.app/Contents/MacOS/Maschine 2"
  do
    if [[ -x "$candidate" ]]; then
      TARGET_PATH="$candidate"
      break
    fi
  done
fi

if [[ -z "$TARGET_PATH" ]]; then
  echo "No executable path provided and Maschine 2 not found in standard locations." >&2
  usage >&2
  exit 1
fi

if [[ ! -x "$TARGET_PATH" ]]; then
  echo "Not executable: $TARGET_PATH" >&2
  exit 1
fi

# --- Launch ---
export MK1_IPC_SNIFFER_PATH="$SNIFFER_LOG"
export DYLD_INSERT_LIBRARIES="$SNIFFER_DYLIB"
# No DYLD_FORCE_FLAT_NAMESPACE needed — uses __DATA,__interpose section

echo "=== mk1-ipc-sniffer ==="
echo "  Target:  $TARGET_PATH"
echo "  Dylib:   $SNIFFER_DYLIB"
echo "  Log:     $SNIFFER_LOG"
echo ""
echo "  Tip: tail -f $SNIFFER_LOG"
echo ""

exec "$TARGET_PATH"
