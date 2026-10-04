# Scripting/CMakeLists.txt — Lua modding layer.
#
# Builds:
#   lua      — static lib from ThirdParty/lua (vendored PUC Lua 5.4.9)
#   rs_lua   — static lib: lua_bindings.cpp (the `rs` API + sandbox)
#   test_lua_bindings — host test (Linux/macOS) with stub RS_* functions
#
# The bindings include "rs_game.h". If the real Game/rs_game.h exists it is
# used; otherwise the clearly-marked test stubs in tests/stubs/ are used and
# only the TEST target is built (the real app links rs_lua against Game).

cmake_minimum_required(VERSION 3.16)
project(ragdoll_sandbox_scripting C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------- Lua library
file(GLOB LUA_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/../ThirdParty/lua/src/*.c")
list(FILTER LUA_SOURCES EXCLUDE REGEX ".*/(lua|luac)\\.c$") # safety net
add_library(lua STATIC ${LUA_SOURCES})
target_include_directories(lua PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../ThirdParty/lua/src")

# Platform define: LUA_USE_LINUX enables POSIX bits (popen etc. — still
# neutered at runtime by our sandbox). iOS builds define LUA_USE_IOS.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_compile_definitions(lua PUBLIC LUA_USE_LINUX)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    target_compile_definitions(lua PUBLIC LUA_USE_MACOSX)
endif()
if(MSVC)
    target_compile_definitions(lua PUBLIC _CRT_SECURE_NO_WARNINGS)
endif()

# ------------------------------------------------- rs_game.h include selection
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../Game/rs_game.h")
    set(RS_GAME_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../Game")
    set(RS_GAME_IS_REAL TRUE)
    message(STATUS "Scripting: using real Game/rs_game.h")
else()
    set(RS_GAME_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/tests/stubs")
    set(RS_GAME_IS_REAL FALSE)
    message(STATUS "Scripting: Game/rs_game.h missing — using TEST STUBS")
endif()

# ---------------------------------------------------------------- Bindings lib
add_library(rs_lua STATIC lua_bindings.cpp lua_bindings.h)
target_include_directories(rs_lua PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/../ThirdParty/lua/src"
    "${RS_GAME_INCLUDE_DIR}")
target_link_libraries(rs_lua PUBLIC lua)
target_compile_options(rs_lua PRIVATE -Wall -Wextra)

# ---------------------------------------------------------------- Host test
# (only meaningful with the stub header; with the real header the test would
#  need the real Game library, which is out of scope here)
if(NOT RS_GAME_IS_REAL)
    add_executable(test_lua_bindings
        tests/test_lua_bindings.cpp
        tests/stubs/rs_game_stubs.cpp)
    target_include_directories(test_lua_bindings PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/../ThirdParty/lua/src"
        "${RS_GAME_INCLUDE_DIR}")
    target_link_libraries(test_lua_bindings PRIVATE rs_lua)
    target_compile_options(test_lua_bindings PRIVATE -Wall -Wextra)

    enable_testing()
    add_test(NAME lua_bindings
        COMMAND test_lua_bindings
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_script.lua"
            "${CMAKE_CURRENT_SOURCE_DIR}/sample_mods")
endif()
