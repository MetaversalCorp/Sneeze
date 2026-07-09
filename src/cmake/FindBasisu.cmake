# FindBasisu.cmake -- Locate the Basis Universal transcoder (static lib + headers)
#
# Sets:
#   Basisu_FOUND
#   BASISU_INCLUDE   -- Include root; use <basisu/transcoder/basisu_transcoder.h>
#   BASISU_LIB       -- Static transcoder library

if (NOT LIBS_DIR)
   message (FATAL_ERROR "LIBS_DIR must be set to find basisu")
endif ()

set (_ROOT "${LIBS_DIR}/basisu/install")

find_path (BASISU_INCLUDE basisu/transcoder/basisu_transcoder.h
   PATHS "${_ROOT}/include" NO_DEFAULT_PATH REQUIRED)

find_library (BASISU_LIB NAMES basisu_transcoder
   PATHS "${_ROOT}/lib" NO_DEFAULT_PATH REQUIRED)

include (FindPackageHandleStandardArgs)
find_package_handle_standard_args (Basisu DEFAULT_MSG BASISU_INCLUDE BASISU_LIB)

unset (_ROOT)
