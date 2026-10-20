#!/bin/bash
# Build a GroveAO debug APK. Requires: Qt 6.5+ for Android (matching ABI) AND a Qt
# desktop kit of the same version, Android SDK + NDK (see Qt docs for the NDK
# version your Qt release wants), JDK 17, cmake, ninja, git, curl, unzip.
#
#   QT_ANDROID=~/Qt/6.5.3/android_arm64_v8a QT_HOST=~/Qt/6.5.3/gcc_64 \
#   ANDROID_SDK_ROOT=~/Android/Sdk ANDROID_NDK_ROOT=~/Android/Sdk/ndk/25.1.8937393 \
#   ./scripts/build-android.sh [arm64-v8a|armeabi-v7a|x86_64]
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."

ABI="${1:-arm64-v8a}"
: "${QT_ANDROID:?set QT_ANDROID to the Qt Android kit dir}"
: "${QT_HOST:?set QT_HOST to the Qt desktop kit dir}"
: "${ANDROID_SDK_ROOT:?set ANDROID_SDK_ROOT}"
: "${ANDROID_NDK_ROOT:?set ANDROID_NDK_ROOT}"

# BASS audio (Android build, free for non-commercial use; see un4seen.com licence)
mkdir -p tmp "lib/android/${ABI}"
if [ ! -f "lib/android/${ABI}/libbass.so" ]; then
  curl -fL https://www.un4seen.com/files/bass24-android.zip -o tmp/bass-android.zip
  unzip -oj tmp/bass-android.zip "libs/${ABI}/libbass.so" -d "lib/android/${ABI}"
  unzip -oj tmp/bass-android.zip "c/bass.h" -d lib
fi
if [ ! -f "lib/android/${ABI}/libbassopus.so" ]; then
  curl -fL https://www.un4seen.com/files/bassopus24-android.zip -o tmp/bassopus-android.zip
  unzip -oj tmp/bassopus-android.zip "libs/${ABI}/libbassopus.so" -d "lib/android/${ABI}"
  unzip -oj tmp/bassopus-android.zip "c/bassopus.h" -d lib
fi

# QtApng (APNG image plugin), built statically into the app by CMake.
if [ ! -f qtapng/src/plugins/imageformats/apng/apngimageplugin.cpp ]; then
  rm -rf qtapng
  git clone --depth 1 https://github.com/jurplel/QtApng.git qtapng
fi

"${QT_ANDROID}/bin/qt-cmake" -S . -B "build-android-${ABI}" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DANDROID_ABI="${ABI}" \
  -DANDROID_SDK_ROOT="${ANDROID_SDK_ROOT}" \
  -DANDROID_NDK_ROOT="${ANDROID_NDK_ROOT}" \
  -DQT_HOST_PATH="${QT_HOST}" \
  -DCMAKE_MODULE_PATH="$PWD/cmake" \
  -DAO_BUILD_TESTS=OFF -DAO_ENABLE_DISCORD_RPC=OFF

cmake --build "build-android-${ABI}" --target AttorneyOnline_make_apk
find "build-android-${ABI}" -name '*.apk'
