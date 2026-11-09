# Native Windows build of gitstatusd with clang-cl (MSVC ABI). See docs/windows.md.
#
# Linux, macOS, BSD and Cygwin/MSYS2 builds use ./build and the Makefile instead.

cmake_minimum_required(VERSION 3.20)
project(gitstatusd LANGUAGES CXX)

if(NOT WIN32 OR NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  message(FATAL_ERROR "This CMake build only supports clang-cl targeting Windows; use ./build elsewhere.")
endif()

set(LIBGIT2_PREFIX "" CACHE PATH "Install prefix of the static gitstatus-patched libgit2")
if(NOT EXISTS "${LIBGIT2_PREFIX}/include/git2.h")
  message(FATAL_ERROR "LIBGIT2_PREFIX must point at an installed static libgit2 (include/git2.h).")
endif()

# Matches libgit2's STATIC_CRT=ON so the executable has no VC++ runtime DLL dependency.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

file(STRINGS build.info _version REGEX "^gitstatus_version=")
string(REGEX REPLACE "^gitstatus_version=\"(.*)\"$" "\\1" GITSTATUS_VERSION "${_version}")

file(GLOB GITSTATUSD_SOURCES CONFIGURE_DEPENDS src/*.cc)
add_executable(gitstatusd ${GITSTATUSD_SOURCES} src/gitstatusd.manifest)
set_target_properties(gitstatusd PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
target_include_directories(gitstatusd PRIVATE "${LIBGIT2_PREFIX}/include")
target_compile_definitions(gitstatusd PRIVATE
  GITSTATUS_VERSION=${GITSTATUS_VERSION}
  GITSTATUS_ZERO_NSEC
  NOMINMAX
  NOGDI
  WIN32_LEAN_AND_MEAN
  UNICODE
  _UNICODE
  _CRT_SECURE_NO_WARNINGS
  _CRT_NONSTDC_NO_DEPRECATE
  # tag_db.cc intentionally holds a lock for the duration of one expression.
  _SILENCE_NODISCARD_LOCK_WARNINGS)
target_compile_options(gitstatusd PRIVATE /EHsc /O2 /DNDEBUG /J /utf-8)
target_link_libraries(gitstatusd PRIVATE
  "${LIBGIT2_PREFIX}/lib/git2.lib"
  ntdll advapi32 ole32 rpcrt4 crypt32 secur32 ws2_32 winhttp)
