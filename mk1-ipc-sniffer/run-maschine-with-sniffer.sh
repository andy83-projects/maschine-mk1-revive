#!/bin/zsh

set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
SNIFFER_SRC="$SCRIPT_DIR/mk1_ipc_sniffer.c"
SNIFFER_DYLIB="${MK1_IPC_SNIFFER_DYLIB:-/tmp/libmk1-ipc-sniffer.dylib}"
SNIFFER_SESSION_DIR_DEFAULT="/tmp/mk1-ipc-sniffer-$(date +%Y%m%d-%H%M%S)"
SNIFFER_SESSION_DIR="${MK1_IPC_SNIFFER_SESSION_DIR:-$SNIFFER_SESSION_DIR_DEFAULT}"
SNIFFER_LOG_DEFAULT="$SNIFFER_SESSION_DIR/mk1-ipc-sniffer.log"
SNIFFER_LOG="${MK1_IPC_SNIFFER_PATH:-$SNIFFER_LOG_DEFAULT}"
SCP_DEST_DEFAULT='m4:~/Documents/GitRepos/maschine-mk1-revive/build/Debug/logs-from-intel/'
SCP_DEST="${MK1_IPC_SNIFFER_SCP_DEST:-$SCP_DEST_DEFAULT}"
SESSION_META="$SNIFFER_SESSION_DIR/session-info.txt"
CHILD_PID=""
STOP_REQUESTED=0

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
  MK1_IPC_SNIFFER_SESSION_DIR  Directory for this capture session
  MK1_IPC_SNIFFER_DYLIB   Path for compiled dylib (default: /tmp/libmk1-ipc-sniffer.dylib)
  MK1_IPC_SNIFFER_PATH    Output log path (default: <session-dir>/mk1-ipc-sniffer.log)
  MK1_IPC_SNIFFER_SCP_DEST  SCP destination
                            (default: m4:~/Documents/GitRepos/maschine-mk1-revive/build/Debug/logs-from-intel/)

Examples:
  # Sniff Maschine 2 IPC
  ./run-maschine-with-sniffer.sh "/Applications/Native Instruments/Maschine 2/Maschine 2.app/Contents/MacOS/Maschine 2"

  # Sniff NIHardwareAgent IPC
  ./run-maschine-with-sniffer.sh "/Applications/Native Instruments/NIHardwareAgent.app/Contents/MacOS/NIHardwareAgent"

  # Watch log live in another terminal
  tail -f /tmp/mk1-ipc-sniffer-*/mk1-ipc-sniffer.log
EOF
}

write_session_metadata() {
  mkdir -p "$SNIFFER_SESSION_DIR"

  cat > "$SESSION_META" <<EOF
session_dir=$SNIFFER_SESSION_DIR
log_path=$SNIFFER_LOG
target_path=$TARGET_PATH
sniffer_dylib=$SNIFFER_DYLIB
started_at=$(date '+%Y-%m-%d %H:%M:%S %Z')
host=$(hostname)
EOF
}

upload_capture_artifacts() {
  local item

  echo ""
  echo "Uploading capture artifacts to $SCP_DEST"

  for item in "$SNIFFER_LOG" "$SESSION_META"; do
    if [[ -f "$item" ]]; then
      scp "$item" "$SCP_DEST"
    fi
  done

  echo "Upload complete."
}

stop_child() {
  local sig="${1:-TERM}"

  if [[ -n "${CHILD_PID:-}" ]] && kill -0 "$CHILD_PID" 2>/dev/null; then
    kill "-$sig" "$CHILD_PID" 2>/dev/null || true
  fi
}

cleanup() {
  local exit_code=$?

  if [[ $STOP_REQUESTED -eq 0 ]]; then
    STOP_REQUESTED=1
    stop_child TERM
  fi

  if [[ -n "${CHILD_PID:-}" ]]; then
    wait "$CHILD_PID" 2>/dev/null || true
  fi

  if [[ -f "$SNIFFER_LOG" || -f "$SESSION_META" ]]; then
    upload_capture_artifacts || {
      echo "SCP upload failed. Artifacts remain in $SNIFFER_SESSION_DIR" >&2
      exit_code=1
    }
  fi

  exit "$exit_code"
}

on_signal() {
  STOP_REQUESTED=1
  echo ""
  echo "Stopping capture..."
  stop_child TERM
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

mkdir -p "$SNIFFER_SESSION_DIR"
write_session_metadata

# --- Launch ---
export MK1_IPC_SNIFFER_PATH="$SNIFFER_LOG"
export DYLD_INSERT_LIBRARIES="$SNIFFER_DYLIB"
# No DYLD_FORCE_FLAT_NAMESPACE needed — uses __DATA,__interpose section

trap cleanup EXIT
trap on_signal INT TERM

echo "=== mk1-ipc-sniffer ==="
echo "  Target:  $TARGET_PATH"
echo "  Dylib:   $SNIFFER_DYLIB"
echo "  Log:     $SNIFFER_LOG"
echo "  Session: $SNIFFER_SESSION_DIR"
echo "  Upload:  $SCP_DEST"
echo ""
echo "  Tip: tail -f $SNIFFER_LOG"
echo "  Stop: press Enter here, or Ctrl-C"
echo ""

"$TARGET_PATH" &
CHILD_PID=$!

{
  IFS= read -r _
  STOP_REQUESTED=1
  echo ""
  echo "Stop requested from launcher."
  stop_child TERM
} &
INPUT_PID=$!

wait "$CHILD_PID" || true
kill "$INPUT_PID" 2>/dev/null || true
