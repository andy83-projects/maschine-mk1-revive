#!/bin/sh
set -eu

export COPYFILE_DISABLE=1

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname "$0")" && pwd)"
REPO_ROOT="$(CDPATH= cd -- "${SCRIPT_DIR}/.." && pwd)"

CONFIGURATION="${CONFIGURATION:-Debug}"
PKG_VERSION="${PKG_VERSION:-0.1.0}"
BUILD_DIR="${REPO_ROOT}/build/${CONFIGURATION}"
BINARY_PATH="${BUILD_DIR}/mk1-bridge"
PLIST_SOURCE="${REPO_ROOT}/mk1-bridge/com.dragco.mk1-bridge.plist"
PKG_ID="com.dragco.mk1-bridge"
OUT_DIR="${REPO_ROOT}/dist"
OUT_PKG="${OUT_DIR}/mk1-bridge-${PKG_VERSION}-${CONFIGURATION}.pkg"
STAGE_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/mk1-bridge-pkg-root.XXXXXX")"
trap 'rm -rf "${STAGE_ROOT}"' EXIT INT TERM

if [ ! -x "${BINARY_PATH}" ]; then
    xcodebuild -project "${REPO_ROOT}/maschine-mk1-revive.xcodeproj" \
        -scheme mk1-bridge \
        -configuration "${CONFIGURATION}" \
        build
fi

install -d "${STAGE_ROOT}/usr/local/bin"
install -d "${STAGE_ROOT}/Library/LaunchAgents"
install -m 755 "${BINARY_PATH}" "${STAGE_ROOT}/usr/local/bin/mk1-bridge"
install -m 644 "${PLIST_SOURCE}" "${STAGE_ROOT}/Library/LaunchAgents/com.dragco.mk1-bridge.plist"
xattr -cr "${STAGE_ROOT}"
find "${STAGE_ROOT}" -name '._*' -delete

mkdir -p "${OUT_DIR}"
rm -f "${OUT_PKG}"

pkgbuild \
    --root "${STAGE_ROOT}" \
    --scripts "${REPO_ROOT}/packaging/scripts" \
    --identifier "${PKG_ID}" \
    --version "${PKG_VERSION}" \
    --filter '\.DS_Store$' \
    --filter '(^|/)\._' \
    --install-location / \
    "${OUT_PKG}"

printf 'Built installer package: %s\n' "${OUT_PKG}"
