#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
NDK_ROOT="${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-}}"

if [[ -z "${NDK_ROOT}" ]]; then
  SDK_ROOT="${ANDROID_SDK_ROOT:-${ANDROID_HOME:-}}"
  if [[ -n "${SDK_ROOT}" && -d "${SDK_ROOT}/ndk" ]]; then
    TOOLCHAIN_FILE="$(find "${SDK_ROOT}/ndk" -maxdepth 4 -name android.toolchain.cmake -print -quit)"
    if [[ -n "${TOOLCHAIN_FILE}" ]]; then
      NDK_ROOT="$(cd "$(dirname "${TOOLCHAIN_FILE}")/../.." && pwd)"
    fi
  fi
fi

if [[ -z "${NDK_ROOT}" || ! -f "${NDK_ROOT}/build/cmake/android.toolchain.cmake" ]]; then
  echo "Set ANDROID_NDK_HOME to an installed Android NDK root." >&2
  exit 2
fi

echo "Preparing dependencies..."
git submodule update --init --recursive

# 1. Patch fmt for Clang 15+ (Android NDK r26+)
FMT_HEADER="${ROOT_DIR}/extern/dynarmic/externals/fmt/include/fmt/format.h"
if [[ -f "${FMT_HEADER}" ]]; then
  sed -i.bak 's/#define FMT_STRING(s) FMT_STRING_IMPL(s, fmt::detail::compile_string, )/#define FMT_STRING(s) s/g' "${FMT_HEADER}" || true
fi

# 2. Download sqlite3 amalgamation if missing
if [[ ! -f "${ROOT_DIR}/extern/sqlite3/sqlite3.c" ]]; then
  echo "Downloading sqlite3..."
  mkdir -p "${ROOT_DIR}/extern/sqlite3"
  curl -sSL "https://www.sqlite.org/2023/sqlite-amalgamation-3430200.zip" -o "${ROOT_DIR}/extern/sqlite3.zip"
  unzip -q "${ROOT_DIR}/extern/sqlite3.zip" -d "${ROOT_DIR}/extern/"
  mv "${ROOT_DIR}/extern/sqlite-amalgamation-3430200/"* "${ROOT_DIR}/extern/sqlite3/"
  rm -rf "${ROOT_DIR}/extern/sqlite3.zip" "${ROOT_DIR}/extern/sqlite-amalgamation-3430200/"
fi

# 3. Download stb_truetype.h if missing
if [[ ! -f "${ROOT_DIR}/extern/stb/stb_truetype.h" ]]; then
  echo "Downloading stb_truetype.h..."
  mkdir -p "${ROOT_DIR}/extern/stb"
  curl -sSL "https://raw.githubusercontent.com/nothings/stb/master/stb_truetype.h" -o "${ROOT_DIR}/extern/stb/stb_truetype.h"
fi

# 4. Download boost headers if missing
if [[ ! -d "${ROOT_DIR}/extern/boost" ]]; then
  echo "Downloading boost headers..."
  mkdir -p "${ROOT_DIR}/extern"
  curl -sSL "https://archives.boost.io/release/1.82.0/source/boost_1_82_0.tar.gz" | tar -xz -C "${ROOT_DIR}/extern" boost_1_82_0/boost
  mv "${ROOT_DIR}/extern/boost_1_82_0/boost" "${ROOT_DIR}/extern/boost"
  rm -rf "${ROOT_DIR}/extern/boost_1_82_0"
fi

CMAKE_EXTRA_ARGS=("-DBoost_INCLUDE_DIR=${ROOT_DIR}/extern")

cmake -S "${ROOT_DIR}" -B "${ROOT_DIR}/build-android-arm64" \
  -DCMAKE_TOOLCHAIN_FILE="${NDK_ROOT}/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a \
  -DANDROID_PLATFORM=android-24 \
  -DCMAKE_BUILD_TYPE=Release \
  "${CMAKE_EXTRA_ARGS[@]}"

cmake --build "${ROOT_DIR}/build-android-arm64" -j"$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)"
