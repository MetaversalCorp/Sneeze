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

#ifndef SNEEZE_COMPUTE_COMPUTE_DISPATCH_H
#define SNEEZE_COMPUTE_COMPUTE_DISPATCH_H

#include <anari/anari.h>
#include <cstdint>
#include <cstddef>
#include <string>
#include <unordered_map>

// Forward-declare Vox types; no vox/Vox.h dependency in this header so
// consumers that don't need Vox directly don't pull in the backend.
namespace vox { class DEVICE; }

namespace DEP
{

struct BUFFER_BINDING
{
   uint32_t nBinding;
   void*    pData;
   size_t   nSize;
   bool     bReadOnly;
};

typedef void (*CpuKernelFn) (
   BUFFER_BINDING* aBindings,
   uint32_t        nBindingCount,
   const void*     pPushConstants,
   size_t          nPushConstantSize,
   uint32_t        nGroupsX,
   uint32_t        nGroupsY,
   uint32_t        nGroupsZ
);

class COMPUTE_DISPATCH
{
public:
   // The ANARIDevice parameter is retained for source compatibility but
   // is no longer used - ANARI never shipped a compute extension. GPU
   // dispatch goes through Vox (Vulkan/DX12/Metal) when available, CPU
   // kernel registry otherwise.
   COMPUTE_DISPATCH (ANARIDevice pDevice = nullptr);
   ~COMPUTE_DISPATCH ();

   COMPUTE_DISPATCH (const COMPUTE_DISPATCH&)            = delete;
   COMPUTE_DISPATCH& operator= (const COMPUTE_DISPATCH&) = delete;

   // True if a Vox GPU backend (Vulkan, DX12, or Metal) was created.
   bool SupportsNativeCompute () const;

   void RegisterCpuKernel (const char* szName, CpuKernelFn fnKernel);

   bool Dispatch (
      const char*     szKernelName,
      uint32_t        nGroupsX,
      uint32_t        nGroupsY,
      uint32_t        nGroupsZ,
      BUFFER_BINDING* aBindings,
      uint32_t        nBindingCount,
      const void*     pPushConstants,
      size_t          nPushConstantSize
   );

private:
   bool DispatchVox (
      const uint8_t*  pSpvBytes,
      size_t          nSpvSize,
      uint32_t        nGroupsX,
      uint32_t        nGroupsY,
      uint32_t        nGroupsZ,
      BUFFER_BINDING* aBindings,
      uint32_t        nBindingCount,
      const void*     pPushConstants,
      size_t          nPushConstantSize
   );

   vox::DEVICE*                                 m_pVoxDevice;
   std::unordered_map<std::string, CpuKernelFn> m_aCpuKernels;
};

} // namespace DEP

#endif // SNEEZE_COMPUTE_COMPUTE_DISPATCH_H
