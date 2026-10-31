#!/bin/bash
# Builds the game for iPad or the iPad simulator with CMake and puts it in Xcode's app bundle.
# Xcode then signs that bundle. Works from Xcode or from Terminal (without the copy step).
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
# Xcode names the platform; from Terminal, pass iphoneos or iphonesimulator.
platform="${PLATFORM_NAME:-${1:-iphonesimulator}}"
case "$platform" in
    iphoneos) build="$root/build/ios-device" ;;
    *) platform=iphonesimulator; build="$root/build/ios-sim" ;;
esac
brew="/opt/homebrew/bin"
for tool in cmake ninja python3; do
    command -v "$brew/$tool" >/dev/null || { echo "error: $tool is missing. Run: brew install cmake ninja llvm python3" >&2; exit 1; }
done
[ -x /opt/homebrew/opt/llvm/bin/llvm-nm ] || { echo "error: LLVM is missing. Run: brew install llvm" >&2; exit 1; }
[ -e "$root/extern/sdl/CMakeLists.txt" ] || { echo "error: dependencies are missing. Run: python3 tools/setup_dependencies.py" >&2; exit 1; }
# Xcode's SDK and deployment variables would otherwise leak into CMake's compiler checks.
clean=(env -i HOME="$HOME" DEVELOPER_DIR="${DEVELOPER_DIR:-$(xcode-select -p)}"
    PATH="/opt/homebrew/opt/llvm/bin:$brew:/usr/bin:/bin:/usr/sbin:/sbin")
# Patches are applied at configure time, and the build refuses stale prepared sources.
if [ ! -f "$build/build.ninja" ] || [ -n "$(find "$root/patches" -newer "$build/build.ninja" -print -quit)" ]; then
    "${clean[@]}" cmake -S "$root" -B "$build" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS=-g -DCMAKE_CXX_FLAGS=-g -DBUILD_TESTING=OFF \
        -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT="$platform" \
        -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=17.0 \
        -DCMAKE_C_COMPILER="$(xcrun --find clang)" -DCMAKE_CXX_COMPILER="$(xcrun --find clang++)" \
        -DBEACHLIFE_ENABLE_EXPERIMENTAL_GAME=ON -DBEACHLIFE_ENABLE_BOOT_GAME=ON -DBEACHLIFE_ENABLE_TWEAKS=ON
fi
"${clean[@]}" cmake --build "$build" --target beachlife
if [ -n "${TARGET_BUILD_DIR:-}" ]; then
    app="$TARGET_BUILD_DIR/$WRAPPER_NAME"
    # Xcode skips its signing step when it thinks nothing changed, but this replaces the
    # executable every build. Keep what an already signed app was entitled to and re-sign.
    entitlements=""
    if [ -d "$app/_CodeSignature" ]; then
        entitlements="$(mktemp)"
        codesign -d --entitlements :- "$app" > "$entitlements" 2>/dev/null || true
    fi
    cp -f "$build/beachlife.app/beachlife" "$app/$EXECUTABLE_NAME"
    rm -rf "$app/assets"
    cp -R "$build/beachlife.app/assets" "$app/assets"
    # HD art made by tools/hd_buildings.py from your own game files. Never committed.
    rm -rf "$app/hd"
    if [ -d "$root/build/hd_assets" ]; then
        cp -R "$root/build/hd_assets" "$app/hd"
    fi
    if [ -n "$entitlements" ]; then
        # Simulator builds have no entitlements, and codesign refuses an empty file.
        if [ -s "$entitlements" ]; then
            codesign --force --sign "${EXPANDED_CODE_SIGN_IDENTITY:--}" --entitlements "$entitlements" "$app"
        else
            codesign --force --sign "${EXPANDED_CODE_SIGN_IDENTITY:--}" "$app"
        fi
        rm -f "$entitlements"
    fi
fi
