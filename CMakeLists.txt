cmake_minimum_required(VERSION 3.16)
project(MyPersistKV CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(PKQL_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(PKQL_BENCH "Build the benchmark against SQLite (needs SQLite3)" ON)

# MinGW: link the C++ runtime statically. Otherwise the program loads
# whichever libstdc++-6.dll comes first on PATH, and an older,
# ABI-incompatible copy (from another MinGW or Git install) crashes it.
if(MINGW)
  add_link_options(-static)
endif()

# The storage engine, pinned as a git submodule.
add_subdirectory(third_party/persist_KV)

if(MSVC)
  add_compile_options(/W4)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
  add_compile_options(-Wall -Wextra)
  if(PKQL_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
    add_link_options(-fsanitize=address,undefined)
  endif()
endif()

add_library(pkql STATIC
  src/codec.cpp
  src/engine.cpp
  src/exec.cpp
  src/expr.cpp
  src/optimizer.cpp
  src/parser.cpp
  src/planner.cpp
  src/storage.cpp
  src/value.cpp
  src/vector.cpp)
target_include_directories(pkql PUBLIC include)
target_link_libraries(pkql PUBLIC persistkv)

add_executable(pkql_shell tools/pkql.cpp)
set_target_properties(pkql_shell PROPERTIES OUTPUT_NAME pkql)
target_link_libraries(pkql_shell PRIVATE pkql)

add_executable(pkql_tests tests/unit_tests.cpp)
target_link_libraries(pkql_tests PRIVATE pkql)

enable_testing()
add_test(NAME pkql_tests COMMAND pkql_tests WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

if(PKQL_BENCH)
  find_package(SQLite3 REQUIRED)
  # CMake 4 renamed the imported target; older versions only have the old name.
  if(TARGET SQLite3::SQLite3)
    set(PKQL_SQLITE SQLite3::SQLite3)
  else()
    set(PKQL_SQLITE SQLite::SQLite3)
  endif()
  add_executable(differential_test tests/differential_test.cpp)
  target_link_libraries(differential_test PRIVATE pkql ${PKQL_SQLITE})
  add_test(NAME differential_test COMMAND differential_test WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

  add_executable(bench_vs_sqlite bench/bench_vs_sqlite.cpp)
  target_link_libraries(bench_vs_sqlite PRIVATE pkql ${PKQL_SQLITE})
  add_executable(bench_events bench/bench_events.cpp)
  target_link_libraries(bench_events PRIVATE pkql ${PKQL_SQLITE})
endif()
