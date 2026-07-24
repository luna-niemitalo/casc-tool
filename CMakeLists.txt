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

add_executable(casc-tool
  src/main.cpp
  src/cli.cpp
  src/storage.cpp
  src/cmd_list.cpp
  src/cmd_info.cpp
  src/cmd_extract.cpp
  src/cmd_extract_batch.cpp
  src/cmd_diff.cpp
)
target_link_libraries(casc-tool PRIVATE casc_static)
