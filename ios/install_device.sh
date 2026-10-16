#!/bin/bash
# Install and launch the signed build on a connected iPhone (cable or same Wi-Fi after pairing).
set -e
APP="$(cd "$(dirname "$0")/.." && pwd)/build/xcode-dd/Build/Products/Release-iphoneos/Wolverine.app"
[ -d "$APP" ] || { echo "no signed build at $APP"; exit 1; }
echo "== connected devices"
xcrun devicectl list devices
LINE=$(xcrun devicectl list devices 2>/dev/null | awk '/iPhone[0-9]+,[0-9]+/ {print; exit}')
DEV=$(echo "$LINE" | grep -oE '[0-9A-F]{8}-[0-9A-F]{4}-[0-9A-F]{4}-[0-9A-F]{4}-[0-9A-F]{12}' | head -1)
echo "$LINE" | grep -q " unavailable " && { echo "iPhone is unavailable: unlock it, keep it on the same Wi-Fi or plug in the cable"; exit 1; }
[ -n "$DEV" ] || { echo "iPhone not found: unlock it, tap Trust, enable Developer Mode"; exit 1; }
echo "== installing to $DEV"
xcrun devicectl device install app --device "$DEV" "$APP"
echo "== launching"
xcrun devicectl device process launch --device "$DEV" com.chaikosh.wolverine
