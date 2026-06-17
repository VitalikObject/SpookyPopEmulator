# Spooky Pop Emulator

An open-source emulator for the iOS version of "Spooky Pop" (a discontinued game by Supercell), allowing it to run natively on Android and macOS.

This emulator translates iOS instructions and shims iOS frameworks (like UIKit, Foundation, CoreGraphics, etc.) to allow the game to run on modern platforms. It uses [Dynarmic](https://github.com/lioncash/dynarmic.git) for dynamic binary translation.

## Features
- **Cross-Platform**: Designed to run on Android and macOS.
- **JIT Compilation**: Fast execution utilizing the Dynarmic JIT engine.
- **iOS Framework Shimming**: Emulates a subset of iOS APIs required by the game.

## Requirements
- CMake 3.21+
- C++20 compatible compiler
- Android NDK (for Android builds)
- macOS with Xcode command-line tools (for macOS builds)

## Game Assets
Before building or running the emulator, you must provide the original game files. Extract the decrypted iOS application and place the `Spooky Pop` executable and the `res/` folder directly into the `external/` directory (next to the `.pastehere` file).

## Build Instructions

### macOS
```bash
mkdir build
cd build
cmake ..
cmake --build . -j$(sysctl -n hw.ncpu)
```

### Android
An automated build script is provided to generate the APK. Ensure your Android SDK and NDK are properly configured.
```bash
export ANDROID_SDK_ROOT=/path/to/android/sdk
export ANDROID_NDK_HOME=/path/to/android/ndk
./scripts/build_android_apk.sh
```

## License
This project is licensed under the [GNU General Public License v3.0](LICENSE).
