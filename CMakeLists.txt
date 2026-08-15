cmake_minimum_required(VERSION 3.16)
project(LimitOrderBook CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_CXX_FLAGS_RELEASE "-O2")

add_library(lob INTERFACE)
target_include_directories(lob INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lob
)
target_compile_options(lob INTERFACE -Wall -Wextra)

add_executable(lob_tests src/main.cpp)
target_link_libraries(lob_tests PRIVATE lob)

add_executable(lob_bench benchmarks/bench.cpp)
target_link_libraries(lob_bench PRIVATE lob)
target_compile_options(lob_bench PRIVATE -march=native)
