#!/bin/sh
set -eu

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname "$0")" && pwd)"
REPO_ROOT="$(CDPATH= cd -- "${SCRIPT_DIR}/.." && pwd)"

VERSION="${VERSION:-0.1.0}"
CONFIGURATION="${CONFIGURATION:-Release}"
BUILD_DIR="${REPO_ROOT}/build/${CONFIGURATION}"
OUT_DIR="${REPO_ROOT}/dist"

PKG_NAME="maschine-mk1-revive-v${VERSION}-macos.pkg"
ZIP_NAME="maschine-mk1-revive-v${VERSION}-manual-install.zip"

PKG_SOURCE="${OUT_DIR}/mk1-bridge-${VERSION}-${CONFIGURATION}.pkg"
PKG_DEST="${OUT_DIR}/${PKG_NAME}"
ZIP_DEST="${OUT_DIR}/${ZIP_NAME}"

STAGE_DIR="$(mktemp -d "${TMPDIR:-/tmp}/mk1-bridge-release-assets.XXXXXX")"
ZIP_ROOT="${STAGE_DIR}/maschine-mk1-revive-v${VERSION}"
trap 'rm -rf "${STAGE_DIR}"' EXIT INT TERM

PKG_VERSION="${VERSION}" CONFIGURATION="${CONFIGURATION}" sh "${SCRIPT_DIR}/build-installer-pkg.sh"

mkdir -p "${OUT_DIR}"
cp "${PKG_SOURCE}" "${PKG_DEST}"

mkdir -p "${ZIP_ROOT}"
install -m 755 "${BUILD_DIR}/mk1-bridge" "${ZIP_ROOT}/mk1-bridge"
install -m 644 "${REPO_ROOT}/mk1-bridge/com.dragco.mk1-bridge.plist" \
    "${ZIP_ROOT}/com.dragco.mk1-bridge.plist"

rm -f "${ZIP_DEST}"
(
    cd "${STAGE_DIR}"
    zip -qry "${ZIP_DEST}" "maschine-mk1-revive-v${VERSION}"
)

printf 'Built release assets:\n'
printf '  %s\n' "${PKG_DEST}"
printf '  %s\n' "${ZIP_DEST}"
