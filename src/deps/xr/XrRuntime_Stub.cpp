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
//
// No-op XR_RUNTIME for platforms without OpenXR SDK (iOS today; any
// build where SNEEZE_ENABLE_XR is OFF). Initialize succeeds but the
// runtime reports "no VR/AR runtime detected" through HasRuntime () so
// the rest of the engine can branch on that the same way it does for a
// real XR loader that couldn't find a runtime on the host.

#include <Sneeze.h>
#include "xr/XrRuntime.h"

namespace SNEEZE { namespace DEP {

class XR_RUNTIME::Impl {};

XR_RUNTIME::XR_RUNTIME () : m_pImpl (nullptr)         {}
XR_RUNTIME::~XR_RUNTIME ()                            {}

bool XR_RUNTIME::Initialize (ENGINE* pEngine)
{
   pEngine->Log (IENGINE::kLOGLEVEL_Info, "XR_RUNTIME",
      "OpenXR support disabled at build time (no headset / runtime expected on this platform).");
   return true;
}

bool XR_RUNTIME::HasRuntime () const                  { return false; }
std::string XR_RUNTIME::GetRuntimeName () const       { return {};    }

}} // namespace SNEEZE::DEP
