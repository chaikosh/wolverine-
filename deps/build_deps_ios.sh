#!/bin/bash
# Build SDL3 + minimal FFmpeg as static libs for an iOS SDK ($1 = iphonesimulator | iphoneos).
set -e
SDK=$1
ROOT=$(cd "$(dirname "$0")" && pwd)
PREFIX=$ROOT/ios-$SDK
TARGET=arm64-apple-ios15.0$([ "$SDK" = iphonesimulator ] && echo "-simulator" || true)
SYSROOT=$(xcrun -sdk $SDK --show-sdk-path)
mkdir -p "$PREFIX" "$ROOT/build"

if [ -z "$SKIP_SDL" ]; then echo "== SDL3 ($SDK)"
cmake -S "$ROOT/src/SDL" -B "$ROOT/build/sdl-$SDK" -G "Unix Makefiles" \
  -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=$SDK -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 \
  -DCMAKE_BUILD_TYPE=Release -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF \
  -DCMAKE_INSTALL_PREFIX="$PREFIX" > "$ROOT/build/sdl-$SDK.log" 2>&1
cmake --build "$ROOT/build/sdl-$SDK" -j4 >> "$ROOT/build/sdl-$SDK.log" 2>&1
cmake --install "$ROOT/build/sdl-$SDK" >> "$ROOT/build/sdl-$SDK.log" 2>&1
echo "   SDL3 ok"; fi

echo "== FFmpeg ($SDK)"
mkdir -p "$ROOT/build/ffmpeg-$SDK" && cd "$ROOT/build/ffmpeg-$SDK"
"$ROOT/src/ffmpeg/configure" --prefix="$PREFIX" --enable-cross-compile --target-os=darwin --arch=aarch64 \
  --cc="xcrun -sdk $SDK clang" --sysroot="$SYSROOT" \
  --extra-cflags="-target $TARGET" --extra-ldflags="-target $TARGET" \
  --disable-everything --enable-static --disable-shared --disable-programs --disable-doc --disable-debug \
  --disable-avdevice --disable-avfilter --enable-swscale --enable-swresample \
  --enable-demuxer=bink --enable-decoder=bink,binkaudio_dct,binkaudio_rdft --enable-protocol=file \
  --disable-videotoolbox --disable-audiotoolbox --disable-securetransport --disable-iconv --disable-zlib \
  --disable-bzlib --disable-lzma --disable-xlib --disable-sdl2 > "$ROOT/build/ffmpeg-$SDK.log" 2>&1
make -j4 >> "$ROOT/build/ffmpeg-$SDK.log" 2>&1
make install >> "$ROOT/build/ffmpeg-$SDK.log" 2>&1
echo "   FFmpeg ok"
ls "$PREFIX/lib"
