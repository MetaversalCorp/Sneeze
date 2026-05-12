// Copyright 2026 Metaversal Corporation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef SNEEZE_XR_RUNTIME_H
#define SNEEZE_XR_RUNTIME_H

#include <string>

namespace SNEEZE
{
   namespace DEP
   {
      // XR_RUNTIME exposes the OpenXR runtime to the engine. The implementation
      // is selected at CMake-configure time: when SNEEZE_ENABLE_XR is ON the
      // real OpenXR loader lives in XrRuntime.cpp; otherwise XrRuntime_Stub.cpp
      // provides a no-op stub (Initialize succeeds with HasRuntime () == false).
      // Either way the header is openxr-free so consumers don't need the SDK.
      class XR_RUNTIME
      {
      public:
         XR_RUNTIME ();
         ~XR_RUNTIME ();

         bool Initialize (ENGINE* pEngine);

         bool        HasRuntime () const;
         std::string GetRuntimeName () const;

      private:
         class Impl;
         Impl* m_pImpl;
      };
   } // namespace DEP
}

#endif // SNEEZE_XR_RUNTIME_H
