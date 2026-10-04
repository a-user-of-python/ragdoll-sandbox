cmake_minimum_required(VERSION 3.16)
project(rs2d_physics CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# rs2d — 2D rigid-body physics engine for Ragdoll Sandbox.
# C++17, no external dependencies, no platform code.
# Target `rs2d_physics`, public API in include/rs2d/physics.h (see API.md).

add_library(rs2d_physics STATIC src/physics.cpp)
target_include_directories(rs2d_physics PUBLIC include)
if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
  target_compile_options(rs2d_physics PRIVATE -Wall -Wextra -O2)
endif()

if(CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME)
  add_executable(rs2d_tests tests/test_physics.cpp)
  target_link_libraries(rs2d_tests PRIVATE rs2d_physics)
  enable_testing()
  add_test(NAME rs2d_tests COMMAND rs2d_tests)
endif()
