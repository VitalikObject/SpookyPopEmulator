cmake_minimum_required(VERSION 3.21)

project(spookypop_loader LANGUAGES C CXX)

set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(DYNARMIC_FRONTENDS "A32" CACHE STRING "" FORCE)
set(DYNARMIC_USE_BUNDLED_EXTERNALS ON CACHE BOOL "" FORCE)
set(DYNARMIC_TESTS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_USE_PRECOMPILED_HEADERS OFF CACHE BOOL "" FORCE)

if (ANDROID AND DEFINED CMAKE_ANDROID_ARCH_ABI AND NOT CMAKE_ANDROID_ARCH_ABI STREQUAL "arm64-v8a")
    message(FATAL_ERROR "The Android port expects arm64-v8a so Dynarmic can JIT guest A32 to host A64.")
endif()

add_compile_definitions(FMT_USE_NONTYPE_TEMPLATE_PARAMETERS=0)

add_subdirectory(extern/dynarmic EXCLUDE_FROM_ALL)

if (APPLE)
    enable_language(OBJCXX)
endif()

set(CLASH_LOADER_COMMON_SOURCES
    src/crypto_compat.cpp
    src/emulator.cpp
    src/graphics/host_gl_backend_stub.cpp
    src/guest_memory.cpp
    src/libc_abi.cpp
    src/libstdcpp_abi.cpp
    src/macho_loader.cpp
    src/objc_abi.cpp
    src/shims/audiotoolbox_shim.cpp
    src/shims/avfoundation_shim.cpp
    src/shims/corefoundation_shim.cpp
    src/shims/coregraphics_shim.cpp
    src/shims/coretelephony_shim.cpp
    src/shims/foundation_shim.cpp
    src/shims/gamekit_shim.cpp
    src/shims/libgcc_shim.cpp
    src/shims/libobjc_shim.cpp
    src/shims/libstdcpp_shim.cpp
    src/shims/libsystem_shim.cpp
    src/shims/mediaplayer_shim.cpp
    src/shims/messageui_shim.cpp
    src/shims/openal_shim.cpp
    src/shims/opengles_shim.cpp
    src/shims/quartzcore_shim.cpp
    src/shims/security_shim.cpp
    src/shims/self_shim.cpp
    src/shims/shim_registry.cpp
    src/shims/social_shim.cpp
    src/shims/storekit_shim.cpp
    src/shims/systemconfiguration_shim.cpp
    src/shims/uikit_shim.cpp
    src/shims/accounts_shim.cpp
    src/shims/coremedia_shim.cpp
    src/shims/libsqlite3_shim.cpp
)

if (ANDROID)
    add_library(spookypop_loader SHARED
        ${CLASH_LOADER_COMMON_SOURCES}
        src/android_native_entry.cpp
        ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c
    )
    set_target_properties(spookypop_loader PROPERTIES OUTPUT_NAME g)
    target_include_directories(spookypop_loader PRIVATE ${ANDROID_NDK}/sources/android/native_app_glue)
else()
    add_executable(spookypop_loader
        ${CLASH_LOADER_COMMON_SOURCES}
        src/main.cpp
    )
endif()

if (APPLE)
    target_sources(spookypop_loader PRIVATE src/graphics/host_gl_backend_macos.mm)
endif()

if (ANDROID)
    target_sources(spookypop_loader PRIVATE src/graphics/host_gl_backend_android.cpp)
endif()

target_include_directories(spookypop_loader PRIVATE src extern extern/sqlite3)

# Bundled SQLite3 amalgamation — real database support
add_library(sqlite3_amalgamation STATIC extern/sqlite3/sqlite3.c)
target_include_directories(sqlite3_amalgamation PUBLIC extern/sqlite3)
target_compile_definitions(sqlite3_amalgamation PRIVATE
    SQLITE_THREADSAFE=0
    SQLITE_OMIT_LOAD_EXTENSION
    SQLITE_OMIT_DEPRECATED
)
if (NOT MSVC)
    target_compile_options(sqlite3_amalgamation PRIVATE -w)
endif()

target_link_libraries(spookypop_loader PRIVATE dynarmic z sqlite3_amalgamation)

if (APPLE)
    target_link_libraries(spookypop_loader PRIVATE "-framework Cocoa" "-framework OpenGL")
endif()

if (ANDROID)
    target_link_libraries(spookypop_loader PRIVATE EGL GLESv2 OpenSLES android log dl)
endif()

if (MSVC)
    target_compile_options(spookypop_loader PRIVATE /W4 /wd4996)
else()
    target_compile_options(spookypop_loader PRIVATE -Wall -Wextra -Wpedantic)
endif()
