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

   using WASM_HOST_FN = wasm_trap_t* (void* pEnv, wasmtime_caller_t* pCaller, const wasmtime_val_t* pArgs, size_t nArgs, wasmtime_val_t* pResults, size_t nResults);

   std::string ReadWasmString (wasmtime_caller_t* pCaller, int32_t nPtr, int32_t nLen);

   // --- Console host functions (module: "Console") ---

   WASM_HOST_FN Console_Log;
   WASM_HOST_FN Console_Debug;
   WASM_HOST_FN Console_Info;
   WASM_HOST_FN Console_Warn;
   WASM_HOST_FN Console_Error;
   WASM_HOST_FN Console_Assert;
   WASM_HOST_FN Console_Group;
   WASM_HOST_FN Console_GroupCollapsed;
   WASM_HOST_FN Console_GroupEnd;
   WASM_HOST_FN Console_Count;
   WASM_HOST_FN Console_CountReset;
   WASM_HOST_FN Console_Time;
   WASM_HOST_FN Console_TimeEnd;
   WASM_HOST_FN Console_TimeLog;

   // --- Storage host functions (module: "Storage") ---

   WASM_HOST_FN Storage_Get;
   WASM_HOST_FN Storage_Set;
   WASM_HOST_FN Storage_Remove;
   WASM_HOST_FN Storage_Has;
   WASM_HOST_FN Storage_GetJson;
   WASM_HOST_FN Storage_SetJson;

   // --- Scene host functions (module: "Scene") ---

   WASM_HOST_FN Scene_Node_Map;
   WASM_HOST_FN Scene_Node_Root;
   WASM_HOST_FN Scene_Node_Open;
   WASM_HOST_FN Scene_Node_Close;
   WASM_HOST_FN Scene_Node_Position;
   WASM_HOST_FN Scene_Node_Scale;
   WASM_HOST_FN Scene_Node_Bound;
   WASM_HOST_FN Scene_Node_Color;
   WASM_HOST_FN Scene_Node_Name;
   WASM_HOST_FN Scene_Node_Radius;
   WASM_HOST_FN Scene_Node_Texture;
   WASM_HOST_FN Scene_Node_Panel;

   // --- Timer host functions (module: "Timer") ---

   WASM_HOST_FN Timer_Set;
   WASM_HOST_FN Timer_Clear;

   } // namespace DEP
} // namespace SNEEZE

#endif // SNEEZE_WASM_HOSTFUNCTIONS_H
