# FindSneezeWebp.cmake -- Locate libwebp decoder (static lib + headers)
#
# Sets:
#   SneezeWebp_FOUND
#   WEBP_INCLUDE   -- Include directory (contains webp/decode.h)
#   WEBP_LIB       -- Decode-only static library (libwebpdecoder)

if (NOT LIBS_DIR)
   message (FATAL_ERROR "LIBS_DIR must be set to find webp")
endif ()

set (_ROOT "${LIBS_DIR}/webp/install")

find_path (WEBP_INCLUDE webp/decode.h
   PATHS "${_ROOT}/include" NO_DEFAULT_PATH REQUIRED)

# webpdecoder is the self-contained decode-only archive. Names cover MSVC
# (webpdecoder.lib), Unix (libwebpdecoder.a), and any debug-postfixed variant.
find_library (WEBP_LIB NAMES webpdecoder libwebpdecoder webpdecoderd
   PATHS "${_ROOT}/lib" NO_DEFAULT_PATH REQUIRED)

include (FindPackageHandleStandardArgs)
find_package_handle_standard_args (SneezeWebp DEFAULT_MSG WEBP_INCLUDE WEBP_LIB)

unset (_ROOT)
