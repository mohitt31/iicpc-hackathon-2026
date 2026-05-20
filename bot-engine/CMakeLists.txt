# Build instructions:
# mkdir build && cd build
# cmake ..
# make -j4
#
# Run Responder: ./null_responder [--stall-mode]
# Run Bot: ./bot --interval-us 100 --duration-sec 10

cmake_minimum_required(VERSION 3.16)
project(HFTBenchmarkingPlatform VERSION 1.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# HFT optimization flags
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -march=native -flto -Wall -Wextra -Wpedantic")

# Include the contracts directory at the repo root
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/src/)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/third_party/HdrHistogram_c/include)

# Add HdrHistogram_c
option(HDR_HISTOGRAM_BUILD_PROGRAMS "" OFF)
option(HDR_HISTOGRAM_BUILD_SHARED "" OFF)
add_subdirectory(third_party/HdrHistogram_c)

# Executable targets
add_executable(null_responder src/null_responder.cpp)
add_executable(bot src/bot.cpp)
add_executable(reference_server src/reference_server.cpp)
add_executable(refengine src/reference_engine.cpp)

# Link threads if necessary
find_package(Threads REQUIRED)
target_link_libraries(null_responder PRIVATE Threads::Threads)
target_link_libraries(bot PRIVATE Threads::Threads hdr_histogram_static)
target_link_libraries(reference_server PRIVATE Threads::Threads)
target_link_libraries(refengine PRIVATE Threads::Threads)
