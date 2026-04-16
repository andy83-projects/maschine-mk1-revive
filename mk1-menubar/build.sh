#!/bin/bash
# Builds mk1-menubar.app and copies it to /Applications (or a local dist/ dir).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="${REPO_ROOT}/build"
APP_NAME="MK1 Revive"
APP_BUNDLE="${BUILD_DIR}/${APP_NAME}.app"
BINARY_NAME="mk1-menubar"

echo "==> Compiling ${BINARY_NAME}..."
swiftc \
    -framework AppKit \
    -O \
    "${SCRIPT_DIR}/main.swift" \
    -o "${BUILD_DIR}/${BINARY_NAME}"

echo "==> Packaging app bundle..."
CONTENTS="${APP_BUNDLE}/Contents"
mkdir -p "${CONTENTS}/MacOS"
mkdir -p "${CONTENTS}/Resources"

cp "${BUILD_DIR}/${BINARY_NAME}"    "${CONTENTS}/MacOS/${BINARY_NAME}"
cp "${SCRIPT_DIR}/Info.plist"       "${CONTENTS}/Info.plist"
cp "${SCRIPT_DIR}/AppIcon.icns"     "${CONTENTS}/Resources/AppIcon.icns"
cp "${SCRIPT_DIR}/menubar.png"      "${CONTENTS}/Resources/menubar.png"
cp "${SCRIPT_DIR}/menubar@2x.png"   "${CONTENTS}/Resources/menubar@2x.png"

# Optional code-sign with ad-hoc identity so Gatekeeper doesn't block it
# (ad-hoc is fine for a personal tool; replace "-" with your Developer ID cert
#  name if you want a proper signature)
if command -v codesign &>/dev/null; then
    echo "==> Code-signing (ad-hoc)..."
    codesign --force --sign - "${APP_BUNDLE}"
fi

echo ""
echo "Built: ${APP_BUNDLE}"
echo ""
echo "To install, run:"
echo "  cp -r \"${APP_BUNDLE}\" /Applications/"
echo "  open \"/Applications/${APP_NAME}.app\""
echo ""
echo "To auto-launch at login, add it via:"
echo "  System Settings → General → Login Items"
