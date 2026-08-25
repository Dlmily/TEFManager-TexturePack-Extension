cmake_minimum_required(VERSION 3.15)
project(TEFManager-TexturePack-Extension)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Source files
set(SOURCES
    src/core.cpp
    src/TextureManager.cpp
    src/TexturePack.cpp
    src/SoundPack.cpp
    src/XnbSound.cpp
    src/AndroidPcmPlayer.cpp
)

# Header files
set(HEADERS
    include/core.hpp
    include/TextureManager.hpp
    include/TexturePack.hpp
    include/SoundPack.hpp
    include/XnbSound.hpp
    include/AndroidPcmPlayer.hpp
)

add_library(${PROJECT_NAME} SHARED ${SOURCES} ${HEADERS})

target_include_directories(${PROJECT_NAME} PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
