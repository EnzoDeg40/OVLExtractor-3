cmake_minimum_required(VERSION 3.21)
project(OVLExtractor3
    VERSION 0.1.0
    DESCRIPTION "Cross-platform RCT3 OVL file extractor (fork of OVLExtractor-2)"
    HOMEPAGE_URL "https://github.com/gamemodderz/OVLExtractor-2"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(OVL_BUILD_CLI "Build the ovlextract CLI executable" ON)
option(OVL_BUILD_TESTS "Build unit tests" ON)
option(OVL_BUILD_GUI "Build the Qt GUI (Phase 2, off by default)" OFF)
option(OVL_ENABLE_SANITIZERS "Enable AddressSanitizer + UBSan in Debug builds" OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
include(CompilerWarnings)

if(OVL_ENABLE_SANITIZERS AND CMAKE_BUILD_TYPE STREQUAL "Debug"
   AND NOT MSVC)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

add_subdirectory(core)
add_subdirectory(extractors)

if(OVL_BUILD_CLI)
    add_subdirectory(cli)
endif()

if(OVL_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(OVL_BUILD_GUI)
    message(STATUS "GUI build is a Phase 2 feature - not yet implemented")
endif()

message(STATUS "")
message(STATUS "OVLExtractor-3 ${PROJECT_VERSION} configuration:")
message(STATUS "  Build type:    ${CMAKE_BUILD_TYPE}")
message(STATUS "  C++ standard:  ${CMAKE_CXX_STANDARD}")
message(STATUS "  Build CLI:     ${OVL_BUILD_CLI}")
message(STATUS "  Build tests:   ${OVL_BUILD_TESTS}")
message(STATUS "  Sanitizers:    ${OVL_ENABLE_SANITIZERS}")
message(STATUS "")
