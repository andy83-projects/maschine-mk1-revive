#!/bin/sh
set -eu

export COPYFILE_DISABLE=1

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname "$0")" && pwd)"
REPO_ROOT="$(CDPATH= cd -- "${SCRIPT_DIR}/.." && pwd)"

PKG_VERSION="${PKG_VERSION:-0.1.0}"
PKG_ID="com.dragco.mk1-bridge.uninstall"
OUT_DIR="${REPO_ROOT}/dist"
OUT_PKG="${OUT_DIR}/mk1-bridge-uninstaller-${PKG_VERSION}.pkg"

mkdir -p "${OUT_DIR}"
rm -f "${OUT_PKG}"

pkgbuild \
    --nopayload \
    --scripts "${REPO_ROOT}/packaging/uninstall-scripts" \
    --identifier "${PKG_ID}" \
    --version "${PKG_VERSION}" \
    "${OUT_PKG}"

printf 'Built uninstaller package: %s\n' "${OUT_PKG}"
