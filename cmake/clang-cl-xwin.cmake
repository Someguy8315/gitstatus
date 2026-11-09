# Cross-compile for x86_64 Windows (MSVC ABI) from Linux with clang-cl and lld-link.
#
# The MSVC CRT and Windows SDK come from `xwin splat` (https://github.com/Jake-Shadle/xwin).
# Point XWIN_DIR (CMake variable or environment variable) at the splat output directory.
# Not needed on Windows, where a Visual Studio developer prompt provides these paths.

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_VERSION 10.0)
set(CMAKE_SYSTEM_PROCESSOR AMD64)

if(NOT XWIN_DIR)
  set(XWIN_DIR "$ENV{XWIN_DIR}")
endif()
if(NOT XWIN_DIR)
  message(FATAL_ERROR "Set XWIN_DIR to the output directory of `xwin splat`.")
endif()
# Cached so that automatic re-configuration during a build doesn't need the environment.
set(XWIN_DIR "${XWIN_DIR}" CACHE PATH "Output directory of `xwin splat`")
# Try-compile projects re-read this file; pass XWIN_DIR through to them.
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES XWIN_DIR)

set(CMAKE_C_COMPILER clang-cl)
set(CMAKE_CXX_COMPILER clang-cl)
set(CMAKE_LINKER lld-link)
set(CMAKE_AR llvm-lib)
set(CMAKE_RC_COMPILER llvm-rc)
set(CMAKE_MT llvm-mt)

set(_xwin_flags
  "--target=x86_64-pc-windows-msvc"
  "/imsvc${XWIN_DIR}/crt/include"
  "/imsvc${XWIN_DIR}/sdk/include/ucrt"
  "/imsvc${XWIN_DIR}/sdk/include/um"
  "/imsvc${XWIN_DIR}/sdk/include/shared")
list(JOIN _xwin_flags " " _xwin_flags)
set(CMAKE_C_FLAGS_INIT "${_xwin_flags}")
set(CMAKE_CXX_FLAGS_INIT "${_xwin_flags}")
set(CMAKE_RC_FLAGS_INIT
  "-I ${XWIN_DIR}/sdk/include/um -I ${XWIN_DIR}/sdk/include/shared -I ${XWIN_DIR}/sdk/include/ucrt -I ${XWIN_DIR}/crt/include")

set(_xwin_libpath
  "/libpath:${XWIN_DIR}/crt/lib/x86_64"
  "/libpath:${XWIN_DIR}/sdk/lib/um/x86_64"
  "/libpath:${XWIN_DIR}/sdk/lib/ucrt/x86_64")
list(JOIN _xwin_libpath " " _xwin_libpath)
set(CMAKE_EXE_LINKER_FLAGS_INIT "${_xwin_libpath}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${_xwin_libpath}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${_xwin_libpath}")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
