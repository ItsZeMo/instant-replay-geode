#!/usr/bin/env bash
# Builds the iOS capture harness (tests/ios/CaptureHarness.mm + the production iOS capture / encoder
# sources) for the iOS Simulator, runs it and reports the result. Geometry Dash is not needed.
#
# Usage: tests/ios/run_capture_harness.sh [simulator name or UDID]
#        (default: "iPhone 18 Pro", else the first available iPhone simulator)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$ROOT/build-ios-sim"
APP="$OUT/InstantReplayCaptureHarness.app"
BUNDLE_ID="dev.zemo.instant-replay.capture-harness"
ARCH="$(uname -m)"
SDK_FLAGS=(-arch "$ARCH" -mios-simulator-version-min=14.0)
# -Wno-#warnings: Xcode's libc++ warns that iOS 14 (Geode's minimum, kept here on purpose) is old.
CXXFLAGS=(-std=c++23 -O1 -g -Wall -Wextra -Wno-#warnings -DGLES_SILENCE_DEPRECATION -DIR_MOCK_LOG_PRINT
          -I"$ROOT/src" -I"$ROOT/tests/ios/mock" -I"$ROOT/tests/host/mock_geode")

rm -rf "$APP" "$OUT/obj"
mkdir -p "$APP" "$OUT/obj"
cp "$ROOT/tests/ios/Info.plist" "$APP/Info.plist"

echo "== Building harness ($ARCH simulator)"
xcrun --sdk iphonesimulator clang++ "${SDK_FLAGS[@]}" "${CXXFLAGS[@]}" \
    -c "$ROOT/src/capture/FrameGrabber.cpp" -o "$OUT/obj/FrameGrabber.o"
for src in tests/ios/CaptureHarness.mm src/capture/FrameGrabberIOS.mm src/platform/IOSPlatformUtils.mm \
           src/platform/MacVideoCapabilities.mm src/encoder/VideoToolboxEncoder.mm; do
    xcrun --sdk iphonesimulator clang++ "${SDK_FLAGS[@]}" "${CXXFLAGS[@]}" -fobjc-arc -fobjc-arc-exceptions \
        -c "$ROOT/$src" -o "$OUT/obj/$(basename "$src" .mm).o"
done
xcrun --sdk iphonesimulator clang++ "${SDK_FLAGS[@]}" "$OUT"/obj/*.o -o "$APP/InstantReplayCaptureHarness" \
    -framework UIKit -framework OpenGLES -framework QuartzCore -framework CoreGraphics -framework AVFoundation -framework CoreMedia \
    -framework CoreVideo -framework VideoToolbox -framework AudioToolbox -framework CoreFoundation -framework Foundation
codesign --force --sign - "$APP" >/dev/null

DEVICE="${1:-iPhone 18 Pro}"
UDID="$(xcrun simctl list devices available | grep -F "$DEVICE (" | head -1 | sed -E 's/.*\(([0-9A-F-]{36})\).*/\1/' || true)"
if [[ -z "$UDID" ]]; then
    UDID="$(xcrun simctl list devices available | grep -E "iPhone" | head -1 | sed -E 's/.*\(([0-9A-F-]{36})\).*/\1/')"
fi
BOOTED_HERE=0
if ! xcrun simctl list devices | grep -F "$UDID" | grep -q "(Booted)"; then
    echo "== Booting simulator $UDID"
    xcrun simctl boot "$UDID"
    BOOTED_HERE=1
fi
xcrun simctl bootstatus "$UDID" -b >/dev/null

echo "== Running on $UDID"
xcrun simctl install "$UDID" "$APP"
LOG="$OUT/harness.log"
xcrun simctl launch --console --terminate-running-process "$UDID" "$BUNDLE_ID" 2>&1 | tee "$LOG"

DATA="$(xcrun simctl get_app_container "$UDID" "$BUNDLE_ID" data 2>/dev/null || true)"
if [[ -n "$DATA" && -d "$DATA/tmp/ir_harness" ]]; then
    rm -rf "$OUT/videos" && cp -R "$DATA/tmp/ir_harness" "$OUT/videos"
    echo "== Videos copied to $OUT/videos"
fi
xcrun simctl uninstall "$UDID" "$BUNDLE_ID" >/dev/null 2>&1 || true
if [[ "$BOOTED_HERE" == 1 ]]; then
    xcrun simctl shutdown "$UDID" >/dev/null 2>&1 || true
fi

grep -q "ALL iOS CAPTURE HARNESS TESTS PASSED" "$LOG"
