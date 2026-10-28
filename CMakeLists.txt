cmake_minimum_required(VERSION 3.16)
project(limit_order_book CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(LOB_NATIVE "Optimise for the build machine's CPU (-march=native)" ON)
option(LOB_SANITIZE "Build with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)

add_library(lob INTERFACE)
target_include_directories(lob INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(lob INTERFACE -Wall -Wextra -Wpedantic)
if(LOB_NATIVE)
  target_compile_options(lob INTERFACE -march=native)
endif()
if(LOB_SANITIZE)
  target_compile_options(lob INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(lob INTERFACE -fsanitize=address,undefined)
endif()

add_executable(itch_replay src/replay.cpp)
target_link_libraries(itch_replay PRIVATE lob)

add_executable(engine_bench src/bench_engine.cpp)
target_link_libraries(engine_bench PRIVATE lob)

add_executable(lob_tests tests/test_main.cpp)
target_link_libraries(lob_tests PRIVATE lob)

enable_testing()
add_test(NAME lob_tests COMMAND lob_tests)
