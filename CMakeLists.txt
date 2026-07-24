cmake_minimum_required(VERSION 3.16)
project(casc-tool CXX)

# Nix provides ccache in the dev shell and sets CCACHE_DIR; use it if present
# rather than assuming it's on PATH outside that shell.
find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
  set(CMAKE_C_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
  set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CASC_BUILD_SHARED_LIB OFF CACHE BOOL "" FORCE)
set(CASC_BUILD_STATIC_LIB ON CACHE BOOL "" FORCE)
set(CASC_BUILD_TESTS OFF CACHE BOOL "" FORCE)

# CascLib's own CMakeLists still declares cmake_minimum_required(VERSION 2.6),
# which modern CMake (>=4.0) refuses outright. Allow it for that one
# subdirectory instead of patching the vendored source.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
add_subdirectory(vendor/CascLib)

# Everything except main.cpp, as a library: lets the test binary link against
# the exact same command/helper code the real CLI runs, instead of
# re-including sources or testing a copy.
add_library(casc-tool-lib STATIC
  src/cli.cpp
  src/storage.cpp
  src/listfile.cpp
  src/cmd_list.cpp
  src/cmd_info.cpp
  src/cmd_extract.cpp
  src/cmd_extract_batch.cpp
  src/cmd_diff.cpp
)
target_include_directories(casc-tool-lib PUBLIC src)
target_link_libraries(casc-tool-lib PUBLIC casc_static)

add_executable(casc-tool src/main.cpp)
target_link_libraries(casc-tool PRIVATE casc-tool-lib)

option(CASC_TOOL_BUILD_TESTS "Build the doctest suite" ON)
if(CASC_TOOL_BUILD_TESTS)
  find_package(doctest CONFIG REQUIRED)
  include(doctest)
  enable_testing()

  add_executable(casc-tool-tests
    tests/test_main.cpp
    tests/test_cli.cpp
    tests/test_format.cpp
    tests/test_storage.cpp
    tests/test_listfile.cpp
    tests/test_integration.cpp
  )
  target_link_libraries(casc-tool-tests PRIVATE casc-tool-lib doctest::doctest)
  target_compile_definitions(casc-tool-tests PRIVATE
    CASC_TOOL_BINARY="$<TARGET_FILE:casc-tool>"
  )
  add_dependencies(casc-tool-tests casc-tool)
  doctest_discover_tests(casc-tool-tests)
endif()
