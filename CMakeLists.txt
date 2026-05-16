cmake_minimum_required(VERSION 3.22)
project(monoview LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --------------------------------------------------------------------------
# android_native_app_glue (from NDK)
# --------------------------------------------------------------------------
find_library(log-lib log)
find_library(android-lib android)

add_library(native_app_glue STATIC
    ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c
)
target_include_directories(native_app_glue PUBLIC
    ${ANDROID_NDK}/sources/android/native_app_glue
)

# --------------------------------------------------------------------------
# OpenXR loader  (prebuilt .so from Meta's Mobile SDK)
# Place libopenxr_loader.so in src/main/jniLibs/arm64-v8a/
# --------------------------------------------------------------------------
add_library(openxr_loader SHARED IMPORTED)
set_target_properties(openxr_loader PROPERTIES
    IMPORTED_LOCATION
    ${CMAKE_SOURCE_DIR}/src/main/jniLibs/arm64-v8a/libopenxr_loader.so
)

# OpenXR headers — fetch from Khronos if not already present
include(FetchContent)
FetchContent_Declare(openxr_sdk
    GIT_REPOSITORY https://github.com/KhronosGroup/OpenXR-SDK.git
    GIT_TAG        release-1.1.36
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(openxr_sdk)

# --------------------------------------------------------------------------
# monoview shared library
# --------------------------------------------------------------------------
add_library(monoview SHARED
    src/main/cpp/main.cpp
    src/main/cpp/xr_context.cpp
    src/main/cpp/renderer.cpp
    src/main/cpp/text_overlay.cpp
)

target_include_directories(monoview PRIVATE
    src/main/cpp
    ${openxr_sdk_SOURCE_DIR}/include
    ${ANDROID_NDK}/sources/android/native_app_glue
)

target_link_libraries(monoview
    native_app_glue
    openxr_loader
    ${android-lib}
    ${log-lib}
    EGL
    GLESv3
)
