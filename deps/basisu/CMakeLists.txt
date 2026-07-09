# Sneeze wrapper build for the Basis Universal *transcoder*.
#
# Upstream basis_universal ships no install() rules and its only static lib
# (basisu_encoder) bundles the whole encoder + CLI. Sneeze only needs the
# runtime transcoder to decode KTX2 / .basis textures (KHR_texture_basisu), so
# we build the minimal transcoder-only target that upstream itself uses for its
# example_transcoding / transcoder WASM module:
#
#     transcoder/basisu_transcoder.cpp  +  zstd/zstddeclib.c
#
# This file is copied into <clone>/sneeze_build/ by deps/basisu.cmake and
# configured via ExternalProject SOURCE_SUBDIR, so it never overwrites any
# upstream file. Upstream sources sit one directory up.

cmake_minimum_required (VERSION 3.20)
project (basisu_transcoder LANGUAGES C CXX)

get_filename_component (BASISU_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

add_library (basisu_transcoder STATIC
   "${BASISU_ROOT}/transcoder/basisu_transcoder.cpp"
   "${BASISU_ROOT}/zstd/zstddeclib.c"
)

# basisu_transcoder.cpp #errors out unless both are defined. KTX2 is the glTF
# container (KHR_texture_basisu); ZSTD covers zstd-supercompressed UASTC.
target_compile_definitions (basisu_transcoder PRIVATE
   BASISD_SUPPORT_KTX2=1
   BASISD_SUPPORT_KTX2_ZSTD=1
)

set_target_properties (basisu_transcoder PROPERTIES
   CXX_STANDARD 17
   CXX_STANDARD_REQUIRED ON
   POSITION_INDEPENDENT_CODE ON
)

if (MSVC)
   target_compile_definitions (basisu_transcoder PRIVATE _CRT_SECURE_NO_WARNINGS)
endif ()

install (TARGETS basisu_transcoder
   ARCHIVE DESTINATION lib
   LIBRARY DESTINATION lib)

# Public transcoder headers only (consumers include
# <basisu/transcoder/basisu_transcoder.h>). The .inc/.inl table files are
# needed to *compile* the .cpp above, not by consumers.
install (DIRECTORY "${BASISU_ROOT}/transcoder/"
   DESTINATION include/basisu/transcoder
   FILES_MATCHING PATTERN "*.h")
