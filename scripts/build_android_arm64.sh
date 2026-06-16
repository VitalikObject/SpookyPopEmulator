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

CMAKE_EXTRA_ARGS=()
if [[ -z "${Boost_INCLUDE_DIR:-}" ]]; then
  for candidate in \
    "/opt/homebrew/include" \
    "/usr/local/include" \
    "$(brew --prefix boost 2>/dev/null)/include"; do
    if [[ -f "${candidate}/boost/version.hpp" ]]; then
      CMAKE_EXTRA_ARGS+=("-DBoost_INCLUDE_DIR=${candidate}")
      break
    fi
  done
else
  CMAKE_EXTRA_ARGS+=("-DBoost_INCLUDE_DIR=${Boost_INCLUDE_DIR}")
fi

cmake -S "${ROOT_DIR}" -B "${ROOT_DIR}/build-android-arm64" \
  -DCMAKE_TOOLCHAIN_FILE="${NDK_ROOT}/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a \
  -DANDROID_PLATFORM=android-24 \
  -DCMAKE_BUILD_TYPE=Release \
  "${CMAKE_EXTRA_ARGS[@]}"

cmake --build "${ROOT_DIR}/build-android-arm64" -j"$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)"
