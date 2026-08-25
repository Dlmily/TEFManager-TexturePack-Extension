cmake_minimum_required(VERSION 3.22)
project(texturepack_extension LANGUAGES C CXX)

set(CMAKE_C_STANDARD 17)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(texturepack_extension SHARED
    include/tefkernel-cpp-wrapper/tefkernel/tef_api_imp.c
    src/core.cpp
    src/miniz.c
    src/TexturePack.cpp
    src/TextureManager.cpp
    src/stb_image.c
    src/SoundPack.cpp
    src/XnbSound.cpp
    src/AndroidPcmPlayer.cpp
)

target_include_directories(texturepack_extension PRIVATE include)

if(ANDROID)
    target_link_libraries(texturepack_extension PRIVATE log OpenSLES)
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Release")
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR ANDROID OR CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        add_custom_command(TARGET texturepack_extension POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E echo "Stripping symbols from texturepack_extension..."
            COMMAND ${CMAKE_STRIP} --strip-all $<TARGET_FILE:texturepack_extension>
            COMMENT "Stripping symbols from texturepack_extension library using strip command"
            VERBATIM)
    endif()
endif()

if(ANDROID)
    set(SYSTEM_NAME "android")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(SYSTEM_NAME "linux")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(SYSTEM_NAME "darwin")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    set(SYSTEM_NAME "windows")
else()
    set(SYSTEM_NAME "unknown")
endif()

if(ANDROID)
    if(ANDROID_ABI STREQUAL "arm64-v8a")
        set(ARCH "arm64")
    elseif(ANDROID_ABI STREQUAL "armeabi-v7a")
        set(ARCH "arm")
    else()
        set(ARCH "${ANDROID_ABI}")
    endif()
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|ARM64")
    set(ARCH "arm64")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "armv7|armv7a|ARMv7|arm")
    set(ARCH "arm")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|AMD64|x64")
    set(ARCH "x64")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "i686|i386|x86")
    set(ARCH "x86")
else()
    set(ARCH "${CMAKE_SYSTEM_PROCESSOR}")
endif()

set_target_properties(texturepack_extension PROPERTIES OUTPUT_NAME "module.${SYSTEM_NAME}.${ARCH}")
