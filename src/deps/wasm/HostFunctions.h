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

#ifndef SNEEZE_WASM_HOSTFUNCTIONS_H
#define SNEEZE_WASM_HOSTFUNCTIONS_H

#include <wasmtime.h>
#include <cstdint>
#include <string>

namespace SNEEZE
{
   namespace DEP
   {

   using WASM_HOST_FN = wasm_trap_t* (void* pWasm_Store, wasmtime_caller_t* pCaller, const wasmtime_val_t* pArgs, size_t nArgs, wasmtime_val_t* pResults, size_t nResults);

   std::string ReadWasmString (wasmtime_caller_t* pCaller, int32_t nPtr, int32_t nLen);

   // --- The single guest -> host entry point (import module "Sneeze") ---
   //
   // Call reads an 8-byte SNEEZE_ABI_PACKET_HEADER at pArgs[0] (offset) / pArgs[1]
   // (size) from the caller's linear memory, routes on (wType, wMethod) to the
   // owning subsystem, and returns an i64 (a created object index, a 0/1 status,
   // a boolean, or the byte size an out-buffer needs). See sdk/include/sneeze_abi.h
   // for the full ABI contract.

   WASM_HOST_FN Call;

   } // namespace DEP
} // namespace SNEEZE

#endif // SNEEZE_WASM_HOSTFUNCTIONS_H
