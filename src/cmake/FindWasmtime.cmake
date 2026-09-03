# FindWasmtime.cmake — Locate the Wasmtime C API
#
# Sets:
#   Wasmtime_FOUND
#   WASMTIME_ROOT      — Install prefix (parent of lib/ and include/)
#   WASMTIME_LIB       — Library path
#   WASMTIME_INCLUDE   — Include directory
#
# Hints:
#   LIBS_DIR — Parent of Wasmtime/install/

if (NOT LIBS_DIR)
   message (FATAL_ERROR "LIBS_DIR must be set to find Wasmtime")
endif ()

set (WASMTIME_ROOT  "${LIBS_DIR}/Wasmtime/install")
set (_WASMTIME_ROOT "${WASMTIME_ROOT}")

if (WIN32)
   find_library (WASMTIME_LIB NAMES wasmtime.dll
      PATHS "${_WASMTIME_ROOT}/lib" NO_DEFAULT_PATH REQUIRED)
elseif (CMAKE_SYSTEM_NAME STREQUAL "iOS" OR ANDROID)
   # Android cargo often emits libwasmtime.a; iOS is always static.
   set (_saved ${CMAKE_FIND_LIBRARY_SUFFIXES})
   set (CMAKE_FIND_LIBRARY_SUFFIXES .a .so)
   find_library (WASMTIME_LIB NAMES wasmtime
      PATHS "${_WASMTIME_ROOT}/lib" NO_DEFAULT_PATH REQUIRED)
   set (CMAKE_FIND_LIBRARY_SUFFIXES ${_saved})
else ()
   find_library (WASMTIME_LIB NAMES wasmtime
      PATHS "${_WASMTIME_ROOT}/lib" NO_DEFAULT_PATH REQUIRED)
endif ()

find_path (WASMTIME_INCLUDE wasmtime.h
   PATHS "${_WASMTIME_ROOT}/include" NO_DEFAULT_PATH REQUIRED)

include (FindPackageHandleStandardArgs)
find_package_handle_standard_args (Wasmtime DEFAULT_MSG WASMTIME_LIB WASMTIME_INCLUDE)

unset (_WASMTIME_ROOT)
