#!/bin/sh
set -eu

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname "$0")" && pwd)"
REPO_ROOT="$(CDPATH= cd -- "${SCRIPT_DIR}/.." && pwd)"

VERSION="${VERSION:-0.1.0}"
CONFIGURATION="${CONFIGURATION:-Release}"
OUT_DIR="${REPO_ROOT}/dist"
ZIP_DEST="${OUT_DIR}/maschine-mk1-revive-v${VERSION}-macos.zip"

STAGE_DIR="$(mktemp -d "${TMPDIR:-/tmp}/mk1-bridge-release-assets.XXXXXX")"
trap 'rm -rf "${STAGE_DIR}"' EXIT INT TERM

# Build bridge binary then bundle it inside the app
xcodebuild -project "${REPO_ROOT}/maschine-mk1-revive.xcodeproj" \
    -scheme mk1-bridge \
    -configuration "${CONFIGURATION}" \
    build

CONFIGURATION="${CONFIGURATION}" bash "${REPO_ROOT}/mk1-menubar/build.sh"

# Zip just the app bundle — drag to /Applications and launch to install
cp -r "${REPO_ROOT}/build/MK1 Revive.app" "${STAGE_DIR}/MK1 Revive.app"
xattr -cr "${STAGE_DIR}/MK1 Revive.app"

mkdir -p "${OUT_DIR}"
rm -f "${ZIP_DEST}"
(
    cd "${STAGE_DIR}"
    zip -qry "${ZIP_DEST}" "MK1 Revive.app"
)

printf 'Built: %s\n' "${ZIP_DEST}"
