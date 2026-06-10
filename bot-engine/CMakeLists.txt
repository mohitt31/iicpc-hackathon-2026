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

# Sanitizer build for testing (never for benchmarking — ASan skews timing).
#   cmake -B build-asan -DENABLE_SANITIZERS=ON && cmake --build build-asan
option(ENABLE_SANITIZERS "Build with AddressSanitizer + UBSan (testing only)" OFF)
if(ENABLE_SANITIZERS)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g -O1 -fno-lto)
    add_link_options(-fsanitize=address,undefined -fno-lto)
endif()

# Include the contracts directory at the repo root
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/src/)

# Locate hdr_histogram_c — installed per BUILD.md (system library).
# Fresh-clone build will fail with a clear, actionable error if it's 
# not installed.
find_path(HDR_INC NAMES hdr/hdr_histogram.h
          PATHS /usr/local/include /opt/homebrew/include /usr/include)
find_library(HDR_LIB NAMES hdr_histogram hdr_histogram_static
             PATHS /usr/local/lib /opt/homebrew/lib /usr/lib)
if(NOT HDR_INC OR NOT HDR_LIB)
    message(FATAL_ERROR
      "\n"
      "=========================================================\n"
      "  hdr_histogram_c library not found.\n"
      "=========================================================\n"
      "\n"
      "  Install it per BUILD.md:\n"
      "\n"
      "    Ubuntu/Debian:  sudo apt install libhdrhistogram-dev\n"
      "    Fedora/RHEL:    sudo dnf install HdrHistogram_c-devel\n"
      "    macOS:          brew install hdrhistogram\n"
      "\n"
      "  Or build from source (see BUILD.md for full instructions).\n"
      "\n"
      "  After install, delete the build/ directory and re-run cmake.\n"
      "=========================================================\n"
      "\n")
endif()
include_directories(${HDR_INC})

# Executable targets
add_executable(null_responder src/null_responder.cpp)
add_executable(bot src/bot.cpp)
add_executable(refengine src/reference_engine.cpp)
add_executable(buggy_engine demo/buggy_engine.cpp)
add_executable(hdr_merge src/hdr_merge.cpp)
add_executable(refserver src/reference_server.cpp)

# Link threads if necessary
find_package(Threads REQUIRED)
target_link_libraries(null_responder PRIVATE Threads::Threads)
target_link_libraries(refserver PRIVATE Threads::Threads)
target_link_libraries(bot PRIVATE Threads::Threads ${HDR_LIB})
target_link_libraries(hdr_merge PRIVATE ${HDR_LIB})
target_link_libraries(refengine PRIVATE ${HDR_LIB})
target_link_libraries(buggy_engine PRIVATE ${HDR_LIB})

# Unit tests for the matching engine
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test/test_order_book.cpp)
    add_executable(test_order_book test/test_order_book.cpp)
    target_link_libraries(test_order_book PRIVATE Threads::Threads)
    enable_testing()
    add_test(NAME test_order_book COMMAND test_order_book)
endif()
