#!/bin/zsh

set -euo pipefail

script_dir=$(cd "$(dirname "$0")" && pwd)
repo_root=$(cd "$script_dir/.." && pwd)
tracer="$script_dir/mk1-trace"

if [[ ! -x "$tracer" ]]; then
    echo "mk1-trace binary not found at: $tracer" >&2
    echo "Build the mk1-trace target in Xcode first." >&2
    exit 1
fi

duration="${MK1_TRACE_DURATION:-20}"
serial="${1:-${MK1_SERIAL:-SN-buscwvye}}"
remote_host="${MK1_TRACE_REMOTE_HOST:-m4}"
remote_base_dir="${MK1_TRACE_REMOTE_DIR:-~/Documents/GitRepos/maschine-mk1-revive/trace}"
run_stamp=$(date +"%Y%m%d-%H%M%S")
remote_run_dir="${remote_base_dir}/mk1-trace-return-${run_stamp}"
scp_dest="${remote_host}:${remote_run_dir}"

cd "$repo_root"

echo "Preparing remote return directory: $scp_dest" >&2
ssh "$remote_host" "mkdir -p '$remote_run_dir'"

if [[ -n "$serial" ]]; then
    echo "Running mk1-trace with preset serial: $serial" >&2
    exec "$tracer" --duration "$duration" --serial "$serial" --scp-dest "$scp_dest"
fi

echo "Running mk1-trace without preset serial." >&2
echo "If this only captures the handshake, rerun as: mk1-trace/run.sh SN-EXAMPLE" >&2
exec "$tracer" --duration "$duration" --scp-dest "$scp_dest"
