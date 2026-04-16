#!/bin/zsh

set -euo pipefail

FRIDA_BIN="${HOME}/frida-env/bin/frida"
SCRIPT_PATH="/private/tmp/frida-iokit-userclient.js"
LOG_DIR="/tmp"

NIHA_LOG="${LOG_DIR}/frida-niha-iokit.log"
NIHIA_LOG="${LOG_DIR}/frida-nihia-iokit.log"

pkill -f "frida.*NIHardwareAgent" || true
pkill -f "frida.*NIHostIntegrationAgent" || true
rm -f "$NIHA_LOG" "$NIHIA_LOG"

"$FRIDA_BIN" -q -n NIHardwareAgent -l "$SCRIPT_PATH" >"$NIHA_LOG" 2>&1 &
NIHA_PID=$!

"$FRIDA_BIN" -q -n NIHostIntegrationAgent -l "$SCRIPT_PATH" >"$NIHIA_LOG" 2>&1 &
NIHIA_PID=$!

echo "Started Frida hooks:"
echo "  NIHardwareAgent pid=$NIHA_PID log=$NIHA_LOG"
echo "  NIHostIntegrationAgent pid=$NIHIA_PID log=$NIHIA_LOG"
