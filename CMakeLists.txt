cmake_minimum_required(VERSION 3.28)
project(leht
    VERSION 0.1.0
    DESCRIPTION "Fast, desktop-neutral PDF toolkit"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

option(LEHT_BUILD_CLI    "Build the leht command-line tool"  ON)
option(LEHT_BUILD_UI     "Build the Qt6 desktop application" OFF)
option(LEHT_BUILD_TESTS  "Build the test suite"              ON)
option(LEHT_BUILD_BENCH  "Build the benchmark harness"       ON)
option(LEHT_SANITIZE     "Enable AddressSanitizer + UBSan"   OFF)
option(LEHT_BUILD_FUZZ   "Build the fuzz target"              ON)

include(CompilerWarnings)

if(LEHT_BUILD_TESTS)
    enable_testing()
endif()

add_subdirectory(core)

if(LEHT_BUILD_CLI)
    add_subdirectory(cli)
endif()
if(LEHT_BUILD_UI)
    add_subdirectory(ui)
endif()
if(LEHT_BUILD_BENCH)
    add_subdirectory(bench)
endif()
if(LEHT_BUILD_FUZZ)
    add_subdirectory(fuzz)
endif()
if(LEHT_BUILD_TESTS)
    add_subdirectory(core/tests)
endif()

message(STATUS "")
message(STATUS "leht ${PROJECT_VERSION}  (${CMAKE_BUILD_TYPE})")
message(STATUS "  cli=${LEHT_BUILD_CLI}  ui=${LEHT_BUILD_UI}  tests=${LEHT_BUILD_TESTS}  bench=${LEHT_BUILD_BENCH}  fuzz=${LEHT_BUILD_FUZZ}  sanitize=${LEHT_SANITIZE}")
message(STATUS "")
