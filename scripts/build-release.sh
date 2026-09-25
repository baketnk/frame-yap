#!/bin/sh
# Build a FrameYap native-only release archive on Linux ARM64.
#
#   sh scripts/build-release.sh WORKDIR VERSION
#
# Downloads the pinned SDL3 and OpenVR SDK sources into WORKDIR (SHA-256
# checked), builds audio-only SDL3 and FrameYap, runs the tests, then stages
# and packages WORKDIR/out/frameyap-VERSION-linux-aarch64.tar.gz(.sha256).
# No ASR runtime, model, Kestrel or Torch is downloaded or bundled.
set -eu

SDL_VERSION=3.2.16
SDL_URL="https://github.com/libsdl-org/SDL/releases/download/release-$SDL_VERSION/SDL3-$SDL_VERSION.tar.gz"
SDL_SHA256=6340e58879b2d15830c8460d2f589a385c444d1faa2a4828a9626c7322562be8
OPENVR_VERSION=2.15.6
OPENVR_URL="https://github.com/ValveSoftware/openvr/archive/refs/tags/v$OPENVR_VERSION.tar.gz"
OPENVR_SHA256=e184cb625010fab7043a9d5e1e000fdeb3067a152bb3169ef53f64dfac37164c

if [ "$#" -ne 2 ]; then
    echo "usage: sh scripts/build-release.sh WORKDIR VERSION" >&2
    exit 2
fi
case "$1" in /*) work=$1 ;; *) work=$(pwd)/$1 ;; esac
version=$2
source=$(cd "$(dirname "$0")/.." && pwd)
[ "$(uname -s)-$(uname -m)" = Linux-aarch64 ] || { echo "release archives are built on Linux aarch64" >&2; exit 1; }
if [ -e "$work/out/frameyap-$version-linux-aarch64.tar.gz" ]; then
    echo "release already exists in $work/out" >&2
    exit 1
fi
mkdir -p "$work/downloads"

fetch() { # url sha256 destination
    if [ ! -f "$3" ]; then
        curl --fail --silent --show-error --location --proto =https --output "$3.part" "$1"
        mv "$3.part" "$3"
    fi
    echo "$2  $3" | sha256sum -c - >/dev/null || { echo "SHA-256 mismatch: $3" >&2; exit 1; }
}
fetch "$SDL_URL" "$SDL_SHA256" "$work/downloads/SDL3-$SDL_VERSION.tar.gz"
fetch "$OPENVR_URL" "$OPENVR_SHA256" "$work/downloads/openvr-$OPENVR_VERSION.tar.gz"

rm -rf "$work/src" "$work/sdl-build" "$work/sdl" "$work/build" "$work/stage"
mkdir -p "$work/src" "$work/out"
tar -xzf "$work/downloads/SDL3-$SDL_VERSION.tar.gz" -C "$work/src"
tar -xzf "$work/downloads/openvr-$OPENVR_VERSION.tar.gz" -C "$work/src"
openvr=$work/src/openvr-$OPENVR_VERSION

# Audio only; PipeWire/Pulse/ALSA backends are loaded at runtime when present.
cmake -S "$work/src/SDL3-$SDL_VERSION" -B "$work/sdl-build" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$work/sdl" -DCMAKE_INSTALL_LIBDIR=lib \
    -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF -DSDL_VIDEO=OFF \
    -DSDL_AUDIO=ON -DSDL_PIPEWIRE=ON -DSDL_PULSEAUDIO=ON -DSDL_ALSA=ON
cmake --build "$work/sdl-build" --parallel
cmake --install "$work/sdl-build"

PKG_CONFIG_PATH="$work/sdl/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}" \
cmake -S "$source" -B "$work/build" -DCMAKE_BUILD_TYPE=Release -DFRAMEYAP_VERSION="$version" \
    -DFRAMEYAP_NATIVE=ON -DFRAMEYAP_UI_TESTS=ON -DOPENVR_ROOT="$openvr" \
    -DOPENVR_LIBRARY="$openvr/lib/linuxarm64/libopenvr_api.so"
cmake --build "$work/build" --parallel
ctest --test-dir "$work/build" --output-on-failure

python3 "$source/scripts/stage-native.py" --build "$work/build" --destination "$work/stage" \
    --openvr-library "$openvr/lib/linuxarm64/libopenvr_api.so" --openvr-license "$openvr/LICENSE" \
    --sdl-library "$work/sdl/lib/libSDL3.so.0" --sdl-license "$work/src/SDL3-$SDL_VERSION/LICENSE.txt"
revision=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["model"]["revision"])' \
    "$source/assets/backends/redux.json")
python3 "$source/scripts/package-release.py" --stage "$work/stage" --output "$work/out" \
    --version "$version" --arch linux-aarch64 --model-revision "$revision" --external-runtime
