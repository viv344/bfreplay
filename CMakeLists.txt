cmake_minimum_required(VERSION 3.20)
project(bfreplay LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -DNDEBUG")
add_compile_options(-Wall -Wextra -Wpedantic)

find_package(BZip2 REQUIRED)
find_package(Threads REQUIRED)
find_library(ZSTD_LIB zstd REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(CURL REQUIRED)

add_library(bfr STATIC src/sim.cpp)
target_include_directories(bfr PUBLIC include)
target_link_libraries(bfr PUBLIC BZip2::BZip2 ${ZSTD_LIB})

add_executable(bfreplay src/main.cpp)
target_link_libraries(bfreplay PRIVATE bfr Threads::Threads)

add_executable(bflive src/live/main.cpp src/live/rest.cpp src/live/stream.cpp)
target_link_libraries(bflive PRIVATE bfr Threads::Threads OpenSSL::SSL OpenSSL::Crypto CURL::libcurl)

# Optional private strategies (not part of the public repository).
if(EXISTS ${CMAKE_SOURCE_DIR}/private/private_hooks.hpp)
  message(STATUS "building with private strategies")
  foreach(t bfreplay bflive)
    target_compile_definitions(${t} PRIVATE BFR_HAS_PRIVATE)
    target_include_directories(${t} PRIVATE private)
  endforeach()
endif()

add_executable(bfconvert tools/bfconvert.cpp)
target_link_libraries(bfconvert PRIVATE bfr Threads::Threads)

add_executable(bench_json bench/bench_json.cpp)
target_link_libraries(bench_json PRIVATE bfr)

enable_testing()
find_package(GTest REQUIRED)
add_executable(tests tests/test_json.cpp tests/test_strategy.cpp tests/test_sim.cpp)
target_link_libraries(tests PRIVATE bfr GTest::gtest_main)
if(EXISTS ${CMAKE_SOURCE_DIR}/private/tests_private.cpp)
  target_sources(tests PRIVATE private/tests_private.cpp)
  target_include_directories(tests PRIVATE private)
endif()
include(GoogleTest)
gtest_discover_tests(tests)
