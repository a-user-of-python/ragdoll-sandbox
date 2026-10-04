# Game/ — Ragdoll Sandbox game logic.
cmake_minimum_required(VERSION 3.16)
project(rs_game CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Physics engine (sibling directory).
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../Engine
                 ${CMAKE_CURRENT_BINARY_DIR}/engine)

# Scripting (for RS_RunLuaFile/RS_GetLuaError/RS_LuaTick).
# The Lua module owns those symbols; Game must NOT define them.
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../Scripting
                 ${CMAKE_CURRENT_BINARY_DIR}/scripting)

add_library(rs_game STATIC
  rs_game.cpp
)

target_include_directories(rs_game PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/../Engine/include
  ${CMAKE_CURRENT_SOURCE_DIR}/../Scripting
)

target_link_libraries(rs_game PUBLIC rs2d_physics)

# Host tests.
add_executable(rs_game_tests
  tests/test_game.cpp
)
target_link_libraries(rs_game_tests PRIVATE rs_game rs_lua)
target_include_directories(rs_game_tests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/../Engine/include
  ${CMAKE_CURRENT_SOURCE_DIR}/../Scripting
)
enable_testing()
add_test(NAME game_tests COMMAND rs_game_tests)

# Sandbox hardening tests (H1-H6): real game + real Lua bindings.
add_executable(test_lua_hardening
  tests/test_lua_hardening.cpp
)
target_link_libraries(test_lua_hardening PRIVATE rs_game rs_lua)
target_include_directories(test_lua_hardening PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/../Engine/include
  ${CMAKE_CURRENT_SOURCE_DIR}/../Scripting
)
target_compile_options(test_lua_hardening PRIVATE -Wall -Wextra)
add_test(NAME lua_hardening COMMAND test_lua_hardening)
