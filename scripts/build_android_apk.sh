#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SDK_ROOT="${ANDROID_SDK_ROOT:-${ANDROID_HOME:-/opt/homebrew/share/android-commandlinetools}}"
BUILD_TOOLS_DIR="${ANDROID_BUILD_TOOLS_DIR:-}"

if [[ -z "${BUILD_TOOLS_DIR}" ]]; then
  BUILD_TOOLS_DIR="$(find "${SDK_ROOT}/build-tools" -maxdepth 1 -mindepth 1 -type d 2>/dev/null | sort -V | tail -n 1)"
fi

PLATFORM_DIR="$(find "${SDK_ROOT}/platforms" -maxdepth 1 -mindepth 1 -type d -name 'android-*' 2>/dev/null | sort -V | tail -n 1)"
if [[ -z "${BUILD_TOOLS_DIR}" || ! -x "${BUILD_TOOLS_DIR}/aapt2" ]]; then
  echo "Android build-tools with aapt2 were not found. Set ANDROID_SDK_ROOT or ANDROID_BUILD_TOOLS_DIR." >&2
  exit 2
fi
if [[ -z "${PLATFORM_DIR}" || ! -f "${PLATFORM_DIR}/android.jar" ]]; then
  echo "Android platform android.jar was not found. Set ANDROID_SDK_ROOT." >&2
  exit 2
fi

"${ROOT_DIR}/scripts/build_android_arm64.sh"

OUT_DIR="${ROOT_DIR}/build/android-apk"
ASSETS_DIR="${OUT_DIR}/assets"
APKROOT_DIR="${OUT_DIR}/apkroot"
rm -rf "${OUT_DIR}"
mkdir -p "${ASSETS_DIR}/binary" "${OUT_DIR}/res" "${OUT_DIR}/gen" "${APKROOT_DIR}/lib/arm64-v8a"

cp "${ROOT_DIR}/external/Spooky Pop" "${ASSETS_DIR}/binary/Spooky Pop"
cp -R "${ROOT_DIR}/external/res/." "${ASSETS_DIR}/"
find "${ASSETS_DIR}" -name .DS_Store -delete

cp -R "${ROOT_DIR}/app/src/main/res/." "${OUT_DIR}/res/"
python3 - <<'PY' "${ROOT_DIR}/app/src/main/AndroidManifest.xml" "${OUT_DIR}/AndroidManifest.xml"
from pathlib import Path
import sys

source = Path(sys.argv[1]).read_text()
target = Path(sys.argv[2])
if ' package=' not in source.partition('>')[0]:
    source = source.replace(
        '<manifest xmlns:android="http://schemas.android.com/apk/res/android">',
        '<manifest xmlns:android="http://schemas.android.com/apk/res/android" package="com.supercell.phoenix">',
        1,
    )
target.write_text(source)
PY

cp "${ROOT_DIR}/build-android-arm64/libg.so" "${APKROOT_DIR}/lib/arm64-v8a/libg.so"
/opt/homebrew/opt/binutils/bin/strip "${APKROOT_DIR}/lib/arm64-v8a/libg.so"

JAVA_SRC_DIR="${ROOT_DIR}/app/src/main/java"
if find "${JAVA_SRC_DIR}" -name '*.java' -print -quit | grep -q .; then
  CLASSES_DIR="${OUT_DIR}/classes"
  DEX_DIR="${OUT_DIR}/dex"
  mkdir -p "${CLASSES_DIR}" "${DEX_DIR}"
  find "${JAVA_SRC_DIR}" -name '*.java' > "${OUT_DIR}/java-sources.txt"
  javac \
    -encoding UTF-8 \
    --release 17 \
    -classpath "${PLATFORM_DIR}/android.jar" \
    -d "${CLASSES_DIR}" \
    @"${OUT_DIR}/java-sources.txt"
  (cd "${CLASSES_DIR}" && jar cf "${OUT_DIR}/classes.jar" .)
  "${BUILD_TOOLS_DIR}/d8" \
    --lib "${PLATFORM_DIR}/android.jar" \
    --min-api 24 \
    --output "${DEX_DIR}" \
    "${OUT_DIR}/classes.jar"
  cp "${DEX_DIR}/classes.dex" "${APKROOT_DIR}/classes.dex"
fi

"${BUILD_TOOLS_DIR}/aapt2" compile --dir "${OUT_DIR}/res" -o "${OUT_DIR}/compiled-res.zip"
"${BUILD_TOOLS_DIR}/aapt2" link \
  -o "${OUT_DIR}/clash-loader-unsigned.apk" \
  -I "${PLATFORM_DIR}/android.jar" \
  --manifest "${OUT_DIR}/AndroidManifest.xml" \
  --min-sdk-version 24 \
  --target-sdk-version 24 \
  --version-code 1 \
  --version-name 0.2.10 \
  -A "${ASSETS_DIR}" \
  --java "${OUT_DIR}/gen" \
  "${OUT_DIR}/compiled-res.zip"

(cd "${APKROOT_DIR}" && zip -qry "${OUT_DIR}/clash-loader-unsigned.apk" lib classes.dex)

KEYSTORE="${ANDROID_DEBUG_KEYSTORE:-${ROOT_DIR}/build/android-debug.keystore}"
if [[ ! -f "${KEYSTORE}" ]]; then
  keytool -genkeypair \
    -keystore "${KEYSTORE}" \
    -storepass android \
    -keypass android \
    -alias androiddebugkey \
    -dname "CN=Android Debug,O=Android,C=US" \
    -keyalg RSA \
    -keysize 2048 \
    -validity 10000 >/dev/null
fi

"${BUILD_TOOLS_DIR}/zipalign" -f 4 "${OUT_DIR}/clash-loader-unsigned.apk" "${OUT_DIR}/clash-loader-aligned.apk"
"${BUILD_TOOLS_DIR}/apksigner" sign \
  --ks "${KEYSTORE}" \
  --ks-pass pass:android \
  --key-pass pass:android \
  --out "${OUT_DIR}/clash-loader-debug.apk" \
  "${OUT_DIR}/clash-loader-aligned.apk"
"${BUILD_TOOLS_DIR}/apksigner" verify --verbose "${OUT_DIR}/clash-loader-debug.apk"

echo "APK: ${OUT_DIR}/clash-loader-debug.apk"
