cmake_minimum_required(VERSION 3.21)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)

if (CMAKE_HOST_APPLE AND (NOT DEFINED CMAKE_SYSTEM_NAME OR CMAKE_SYSTEM_NAME STREQUAL "Darwin") AND NOT DEFINED ENV{GEODE_IOS_SDK} AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    if (NOT DEFINED CMAKE_OSX_ARCHITECTURES OR CMAKE_OSX_ARCHITECTURES STREQUAL "")
        set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "Build universal arm64 + x86_64 binary" FORCE)
    endif()
    if (NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET OR CMAKE_OSX_DEPLOYMENT_TARGET STREQUAL "")
        set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS deployment target" FORCE)
    endif()
endif()

project(InstantReplay VERSION 1.0.5)

if (APPLE AND NOT IOS)
    enable_language(OBJCXX)
    set(CMAKE_OBJCXX_STANDARD 23)
    set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
endif()

if (NOT WIN32 AND NOT (APPLE AND NOT IOS))
    message(FATAL_ERROR "Instant Replay currently supports Windows (Media Foundation + WASAPI) and macOS (VideoToolbox + CoreAudio).")
endif()

set(IR_SOURCES
    src/main.cpp
    src/recorder/ReplayManager.cpp
    src/capture/FrameGrabber.cpp
    src/capture/AudioGrabber.cpp
    src/ui/ReplayPopup.cpp
    src/ui/ReplayNotification.cpp
    src/ui/CodecGuidePopup.cpp
)

if (WIN32)
    list(APPEND IR_SOURCES
        src/encoder/MediaFoundationEncoder.cpp
    )
elseif (APPLE AND NOT IOS)
    set(IR_OBJCXX_SOURCES
        src/platform/MacPlatformUtils.mm
        src/platform/MacVideoCapabilities.mm
        src/capture/AudioGrabberMac.mm
        src/encoder/VideoToolboxEncoder.mm
    )
    set_source_files_properties(${IR_OBJCXX_SOURCES} PROPERTIES
        COMPILE_OPTIONS "-fobjc-arc;-fobjc-arc-exceptions"
    )
    list(APPEND IR_SOURCES ${IR_OBJCXX_SOURCES})
endif()

add_library(${PROJECT_NAME} SHARED ${IR_SOURCES})

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
        shell32
        avrt
    )
elseif (APPLE AND NOT IOS)
    target_compile_definitions(${PROJECT_NAME} PRIVATE
        GL_SILENCE_DEPRECATION
    )
    target_link_libraries(${PROJECT_NAME}
        "-framework VideoToolbox"
        "-framework AVFoundation"
        "-framework CoreMedia"
        "-framework CoreVideo"
        "-framework AudioToolbox"
        "-framework CoreAudio"
        "-framework CoreFoundation"
        "-framework AppKit"
        "-framework OpenGL"
    )
endif()

