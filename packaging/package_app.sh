#!/bin/bash
# Build "Commandos 2.app": native binary + embedded SDL3 + game data + icon, ad-hoc signed.
#
#   packaging/package_app.sh [--with-saves] [binary]        (or: cmake --build build --target package_app)
#
# Layout:
#   Contents/MacOS/Commandos2                 the recompiled game
#   Contents/Frameworks/libSDL3.0.dylib       copied from Homebrew, so the app runs without it
#   Contents/Resources/game/                  DATA, DATA.PCK, DATA2.PCK, HD, default .ini files, comm2.exe (1.2)
#   Contents/Resources/Assets.car, AppIcon.icns
# At run time the game data in the bundle is read-only; OUTPUT\ (saves, profiles, COMM2.CFG, key mapping) and the two
# .ini files live in ~/Library/Application Support/Commandos 2 Recomp, seeded from the bundle on first launch.
# --with-saves puts the current gamedata/OUTPUT into the bundle as that seed (your profiles and saves);
# without it a new install starts clean.
set -euo pipefail
cd "$(dirname "$0")/.."

WITH_SAVES=0
BIN=build/Commandos2
for a in "$@"; do
    case "$a" in
        --with-saves) WITH_SAVES=1 ;;
        -h|--help) sed -n 2,13p "$0"; exit 0 ;;
        *) BIN="$a" ;;
    esac
done
GAME=gamedata
EXE="$GAME/comm2.exe"
ICON=assets/AppIcon.icon
OUT=dist
APP="$OUT/Commandos 2.app"
C="$APP/Contents"

[ -x "$BIN" ] || { echo "binary $BIN not found: build first (cmake --build build)"; exit 1; }
[ -f "$EXE" ] || { echo "$EXE not found"; exit 1; }
[ -f "$GAME/DATA.PCK" ] || { echo "$GAME/DATA.PCK not found"; exit 1; }

echo "==> $APP"
rm -rf "$APP"
mkdir -p "$C/MacOS" "$C/Frameworks" "$C/Resources/game"

# --- binary + SDL3
cp "$BIN" "$C/MacOS/Commandos2"
SDL_REF=$(otool -L "$BIN" | awk '/libSDL3/ {print $1; exit}')
if [ -n "$SDL_REF" ]; then
    SDL_REAL=$(python3 -c 'import os,sys; print(os.path.realpath(sys.argv[1]))' "$SDL_REF")
    SDL_NAME=$(basename "$SDL_REF")
    cp "$SDL_REAL" "$C/Frameworks/$SDL_NAME"
    chmod u+w "$C/Frameworks/$SDL_NAME"
    install_name_tool -id "@rpath/$SDL_NAME" "$C/Frameworks/$SDL_NAME"
    install_name_tool -change "$SDL_REF" "@executable_path/../Frameworks/$SDL_NAME" "$C/MacOS/Commandos2"
    if otool -L "$C/Frameworks/$SDL_NAME" | tail -n +2 | grep -q /opt/homebrew; then
        echo "warning: $SDL_NAME links other Homebrew libraries"; fi
fi

# --- game data (APFS clones: no extra disk space while source and bundle are on the same volume)
echo "==> game data"
for f in DATA DATA.PCK DATA2.PCK HD Commandos2-ModernPatch.ini; do
    [ -e "$GAME/$f" ] && cp -cR "$GAME/$f" "$C/Resources/game/"
done
if [ "$WITH_SAVES" = 1 ]; then
    cp -cR "$GAME/OUTPUT" "$C/Resources/game/"
    rm -f "$C/Resources/game/OUTPUT/"*.user-backup
fi
cp -c "$EXE" "$C/Resources/game/comm2.exe"
find "$APP" -name .DS_Store -delete

# --- icon (Icon Composer .icon -> Assets.car for macOS 26+, .icns fallback)
echo "==> icon"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
ICON_NAME=$(basename "$ICON" .icon)
if ! xcrun actool "$ICON" --compile "$C/Resources" --app-icon "$ICON_NAME" --platform macosx \
    --target-device mac --minimum-deployment-target 11.0 --output-partial-info-plist "$TMP/icon.plist" \
    --enable-on-demand-resources NO --development-region en >"$TMP/actool.log" 2>&1; then
    echo "warning: actool unavailable; using a standard .icns icon"
    ICONSET="$TMP/$ICON_NAME.iconset"
    mkdir -p "$ICONSET"
    for size in 16 32 128 256 512; do
        sips -z "$size" "$size" "$ICON/Assets/commandos2.png" --out "$ICONSET/icon_${size}x${size}.png" >/dev/null
        double=$((size * 2))
        sips -z "$double" "$double" "$ICON/Assets/commandos2.png" --out "$ICONSET/icon_${size}x${size}@2x.png" >/dev/null
    done
    # Wrap the PNG representations directly; iconutil can also fail in a sandbox.
    python3 - "$ICONSET" "$C/Resources/$ICON_NAME.icns" <<'PYICON'
from pathlib import Path
import struct
import sys
source = Path(sys.argv[1])
representations = {
    "icp4": "icon_16x16.png", "icp5": "icon_32x32.png",
    "ic07": "icon_128x128.png", "ic08": "icon_256x256.png",
    "ic09": "icon_512x512.png", "ic10": "icon_512x512@2x.png",
    "ic11": "icon_16x16@2x.png", "ic12": "icon_32x32@2x.png",
    "ic13": "icon_128x128@2x.png", "ic14": "icon_256x256@2x.png",
}
chunks = []
for kind, filename in representations.items():
    data = (source / filename).read_bytes()
    if not data.startswith(b"\x89PNG\r\n\x1a\n"):
        raise SystemExit(f"Expected PNG icon representation: {filename}")
    chunks.append(kind.encode("ascii") + struct.pack(">I", len(data) + 8) + data)
body = b"".join(chunks)
Path(sys.argv[2]).write_bytes(b"icns" + struct.pack(">I", len(body) + 8) + body)
PYICON
fi

# --- Info.plist
VER=1.20
BUILD=$(git rev-parse --short HEAD 2>/dev/null || echo 1)
MINOS=$(otool -l "$BIN" | awk '/LC_BUILD_VERSION/ {f=1} f && /minos/ {print $2; exit}')
cat > "$C/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleName</key><string>Commandos 2</string>
    <key>CFBundleDisplayName</key><string>Commandos 2</string>
    <key>CFBundleIdentifier</key><string>local.c2native.commandos2</string>
    <key>CFBundleExecutable</key><string>Commandos2</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>CFBundleShortVersionString</key><string>$VER</string>
    <key>CFBundleVersion</key><string>$BUILD</string>
    <key>CFBundleIconFile</key><string>$ICON_NAME</string>
    <key>CFBundleIconName</key><string>$ICON_NAME</string>
    <key>LSMinimumSystemVersion</key><string>${MINOS:-11.0}</string>
    <key>LSApplicationCategoryType</key><string>public.app-category.strategy-games</string>
    <key>NSHighResolutionCapable</key><true/>
    <key>NSSupportsAutomaticGraphicsSwitching</key><true/>
</dict>
</plist>
EOF
plutil -lint "$C/Info.plist" >/dev/null

# --- ad-hoc signature (needed on Apple silicon after install_name_tool)
echo "==> signing (ad-hoc)"
[ -n "${SDL_NAME:-}" ] && codesign --force --sign - "$C/Frameworks/$SDL_NAME"
codesign --force --sign - "$APP"
codesign --verify --strict "$APP"

echo "==> done: $APP ($(du -sh "$APP" | cut -f1) apparent; game data is cloned, not copied)"
