# Copyright 2026 Metaversal Corporation
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

cmake_minimum_required (VERSION 3.20)
project (Capture VERSION 0.1.0 LANGUAGES C CXX)

set (CMAKE_CXX_STANDARD 17)
set (CMAKE_CXX_STANDARD_REQUIRED ON)

if (APPLE AND NOT ANDROID)
   enable_language (OBJCXX)
endif ()

set (CAPTURE_SOURCES
   src/Capture.cpp
   src/Capture_Convert.cpp
   src/Capture_Pinhole.cpp
)
if (WIN32)
   list (APPEND CAPTURE_SOURCES src/Capture_Win.cpp)
elseif (ANDROID)
   list (APPEND CAPTURE_SOURCES src/Capture_Android.cpp)
elseif (APPLE)
   list (APPEND CAPTURE_SOURCES src/Capture_Apple.mm)
   set_source_files_properties (src/Capture_Apple.mm PROPERTIES
      COMPILE_FLAGS "-fobjc-arc"
      SKIP_PRECOMPILE_HEADERS ON)
else ()
   list (APPEND CAPTURE_SOURCES src/Capture_Linux.cpp)
endif ()

add_library (Capture STATIC ${CAPTURE_SOURCES})
add_library (Capture::Capture ALIAS Capture)

set_target_properties (Capture PROPERTIES
   OUTPUT_NAME Capture
   POSITION_INDEPENDENT_CODE ON
)

target_include_directories (Capture
   PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
      $<INSTALL_INTERFACE:include>
   PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/src
)

if (WIN32)
   target_link_libraries (Capture PUBLIC mfplat mfreadwrite mf mfuuid)
elseif (APPLE AND NOT ANDROID)
   find_library (AVFOUNDATION_FW AVFoundation)
   find_library (COREMEDIA_FW CoreMedia)
   find_library (COREVIDEO_FW CoreVideo)
   find_library (FOUNDATION_FW Foundation)
   target_link_libraries (Capture PUBLIC
      ${AVFOUNDATION_FW} ${COREMEDIA_FW} ${COREVIDEO_FW} ${FOUNDATION_FW}
   )
elseif (ANDROID)
   target_link_libraries (Capture PUBLIC camera2ndk mediandk)
endif ()

install (TARGETS Capture
   EXPORT CaptureTargets
   ARCHIVE DESTINATION lib
   LIBRARY DESTINATION lib
)
install (DIRECTORY include/ DESTINATION include)
install (EXPORT CaptureTargets
   FILE CaptureTargets.cmake
   NAMESPACE Capture::
   DESTINATION lib/cmake/Capture
)
