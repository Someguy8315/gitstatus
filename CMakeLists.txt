# Native Windows build of gitstatusd; see README.md. Requires clang-cl: gitstatusd uses GNU C++
# extensions. Other platforms use ./build.
cmake_minimum_required(VERSION 3.15)
project(gitstatusd C CXX)

if(NOT WIN32)
  message(FATAL_ERROR "This CMake build is Windows-only; use ./build on this platform.")
endif()

file(STRINGS build.info gitstatus_version REGEX "^gitstatus_version=")
string(REGEX REPLACE "^gitstatus_version=\"(.*)\"$" "\\1" gitstatus_version "${gitstatus_version}")

# The same libgit2 configuration as ./build, minus the network transports that gitstatusd never uses.
foreach(option BUILD_SHARED_LIBS BUILD_TESTS BUILD_CLI USE_SSH USE_GSSAPI USE_NTLMCLIENT)
  set(${option} OFF CACHE BOOL "" FORCE)
endforeach()
foreach(option USE_THREADS USE_BUNDLED_ZLIB STATIC_CRT)
  set(${option} ON CACHE BOOL "" FORCE)
endforeach()
set(USE_HTTPS OFF CACHE STRING "" FORCE)
set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreaded)
set(REGEX_BACKEND builtin CACHE STRING "" FORCE)
set(USE_HTTP_PARSER builtin CACHE STRING "" FORCE)
add_compile_definitions(GIT_ZERO_NSEC)
add_subdirectory(deps/libgit2 EXCLUDE_FROM_ALL)

file(GLOB sources CONFIGURE_DEPENDS src/*.cc)
list(REMOVE_ITEM sources ${CMAKE_CURRENT_SOURCE_DIR}/src/check_dir_mtime.cc)

add_executable(gitstatusd ${sources})
set_target_properties(gitstatusd PROPERTIES
  CXX_STANDARD 14
  CXX_STANDARD_REQUIRED ON)
target_compile_definitions(gitstatusd PRIVATE
  GITSTATUS_VERSION=${gitstatus_version} GITSTATUS_ZERO_NSEC NOMINMAX NOGDI WIN32_LEAN_AND_MEAN
  _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS)
target_compile_options(gitstatusd PRIVATE
  /EHsc /J /FI${CMAKE_CURRENT_SOURCE_DIR}/src/compat_windows.h
  # Upstream relies on these GNU extensions.
  -Wno-c99-designator -Wno-gnu-conditional-omitted-operand -Wno-gnu-zero-variadic-macro-arguments
  -Wno-variadic-macros)
target_include_directories(gitstatusd PRIVATE deps/libgit2/include)
target_link_libraries(gitstatusd PRIVATE libgit2package)
