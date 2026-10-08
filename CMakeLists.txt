cmake_minimum_required(VERSION 3.21)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if ("${CMAKE_SYSTEM_NAME}" STREQUAL "iOS" OR IOS)
    set(CMAKE_OSX_ARCHITECTURES "arm64")
else()
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64")
endif()
set(CMAKE_CXX_VISIBILITY_PRESET hidden)

project(InstantReplay VERSION 1.0.1)

add_library(${PROJECT_NAME} SHARED
    src/main.cpp
    src/recorder/ReplayManager.cpp
    src/capture/FrameGrabber.cpp
    src/capture/AudioGrabber.cpp
    src/encoder/MediaFoundationEncoder.cpp
    src/ui/ReplayPopup.cpp
    src/ui/ReplayNotification.cpp
    src/ui/CodecGuidePopup.cpp
)

if (NOT DEFINED ENV{GEODE_SDK})
    message(FATAL_ERROR "Unable to find Geode SDK! Please define GEODE_SDK environment variable to point to Geode")
else()
    message(STATUS "Found Geode: $ENV{GEODE_SDK}")
endif()

add_subdirectory($ENV{GEODE_SDK} ${CMAKE_CURRENT_BINARY_DIR}/geode)

target_include_directories(${PROJECT_NAME} PUBLIC
    src
)

setup_geode_mod(${PROJECT_NAME})

if (WIN32)
    target_link_libraries(${PROJECT_NAME}
        mfplat
        mfreadwrite
        mfuuid
        ole32
        oleaut32
        uuid
        opengl32
        winmm
    )
endif()
