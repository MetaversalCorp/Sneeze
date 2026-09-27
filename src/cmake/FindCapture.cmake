# FindCapture.cmake -- Locate the portable Capture static lib + headers
#
# Used when Capture is installed as a Sneeze dep under
# LIBS_DIR/Capture/install. Until then src/CMakeLists.txt add_subdirectorys
# the in-tree capture/ project and this module is not required.
#
# Sets:
#   Capture_FOUND
#   CAPTURE_INCLUDE
#   CAPTURE_LIB
# and, when found, imported target Capture::Capture.

if (NOT LIBS_DIR)
   message (FATAL_ERROR "LIBS_DIR must be set to find Capture")
endif ()

if (TARGET Capture::Capture)
   set (Capture_FOUND TRUE)
   return ()
endif ()

set (_ROOT "${LIBS_DIR}/Capture/install")

find_path (CAPTURE_INCLUDE Capture/Capture.h
   PATHS "${_ROOT}/include" NO_DEFAULT_PATH)

find_library (CAPTURE_LIB NAMES Capture
   PATHS "${_ROOT}/lib" NO_DEFAULT_PATH)

include (FindPackageHandleStandardArgs)
find_package_handle_standard_args (Capture DEFAULT_MSG CAPTURE_INCLUDE CAPTURE_LIB)

if (Capture_FOUND AND NOT TARGET Capture::Capture)
   add_library (Capture::Capture STATIC IMPORTED)
   set_target_properties (Capture::Capture PROPERTIES
      IMPORTED_LOCATION "${CAPTURE_LIB}"
      INTERFACE_INCLUDE_DIRECTORIES "${CAPTURE_INCLUDE}"
   )
   if (WIN32)
      set_property (TARGET Capture::Capture APPEND PROPERTY
         INTERFACE_LINK_LIBRARIES mfplat;mfreadwrite;mf;mfuuid)
   elseif (ANDROID)
      set_property (TARGET Capture::Capture APPEND PROPERTY
         INTERFACE_LINK_LIBRARIES camera2ndk;mediandk)
   endif ()
endif ()

unset (_ROOT)
