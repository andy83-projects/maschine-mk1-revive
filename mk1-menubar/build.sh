#!/bin/bash
# Builds mk1-menubar.app and copies it to /Applications (or a local dist/ dir).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="${REPO_ROOT}/build"
CONFIGURATION="${CONFIGURATION:-Debug}"
APP_NAME="MK1 Revive"
APP_BUNDLE="${BUILD_DIR}/${APP_NAME}.app"
BINARY_NAME="mk1-menubar"

echo "==> Compiling ${BINARY_NAME}..."
swiftc \
    -framework AppKit \
    -framework ServiceManagement \
    -O \
    -target arm64-apple-macosx14.0 \
    "${SCRIPT_DIR}/main.swift" \
    -o "${BUILD_DIR}/${BINARY_NAME}"

echo "==> Packaging app bundle..."
CONTENTS="${APP_BUNDLE}/Contents"
mkdir -p "${CONTENTS}/MacOS"
mkdir -p "${CONTENTS}/Resources"

cp "${BUILD_DIR}/${BINARY_NAME}"                    "${CONTENTS}/MacOS/${BINARY_NAME}"
cp "${BUILD_DIR}/${CONFIGURATION}/mk1-bridge"       "${CONTENTS}/MacOS/mk1-bridge"
cp "${SCRIPT_DIR}/Info.plist"                       "${CONTENTS}/Info.plist"
cp "${SCRIPT_DIR}/AppIcon.icns"     "${CONTENTS}/Resources/AppIcon.icns"
cp "${SCRIPT_DIR}/menubar.png"      "${CONTENTS}/Resources/menubar.png"
cp "${SCRIPT_DIR}/menubar@2x.png"   "${CONTENTS}/Resources/menubar@2x.png"

# Optional code-sign with ad-hoc identity so Gatekeeper doesn't block it
# (ad-hoc is fine for a personal tool; replace "-" with your Developer ID cert
#  name if you want a proper signature)
if command -v codesign &>/dev/null; then
    echo "==> Code-signing (ad-hoc)..."
    xattr -cr "${APP_BUNDLE}"
    codesign --force --sign - "${APP_BUNDLE}"
fi

echo ""
echo "Built: ${APP_BUNDLE}"
echo ""
echo "To install, run:"
echo "  cp -r \"${APP_BUNDLE}\" /Applications/"
echo "  open \"/Applications/${APP_NAME}.app\""
echo ""
echo "Launch at Login can be toggled from the MK1 Revive menu bar icon."
