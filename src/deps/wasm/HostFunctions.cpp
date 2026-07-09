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

#include "HostFunctions.h"
#include "Wasm.h"

#include <Sneeze.h>

#include <cstdlib>
#include <cstring>

namespace SNEEZE
{
namespace DEP
{

// Key of the MSF payload block that holds the scene/node tree.
#define PAYLOAD_KEY_DATA                "data"

// ---------------------------------------------------------------------------
// ReadWasmString — reads a UTF-8 string from the caller's linear memory.
// ---------------------------------------------------------------------------

std::string ReadWasmString (wasmtime_caller_t* pCaller, int32_t nPtr, int32_t nLen)
{
   std::string sResult;

   if (nPtr < 0  ||  nLen <= 0)
      return sResult;

   wasmtime_extern_t ext;
   bool bFound = wasmtime_caller_export_get (pCaller, "memory", 6, &ext);

   if (bFound  &&  ext.kind == WASMTIME_EXTERN_MEMORY)
   {
      wasmtime_context_t* pCtx = wasmtime_caller_context (pCaller);
      uint8_t* pData = wasmtime_memory_data (pCtx, &ext.of.memory);
      size_t nMemSize = wasmtime_memory_data_size (pCtx, &ext.of.memory);

      if (static_cast<size_t> (nPtr + nLen) <= nMemSize)
         sResult.assign (reinterpret_cast<const char*> (pData + nPtr), static_cast<size_t> (nLen));
   }

   return sResult;
}

// ---------------------------------------------------------------------------
// ReadWasmBytes — reads raw bytes from the caller's linear memory.
// ---------------------------------------------------------------------------

const uint8_t* ReadWasmBytes (wasmtime_caller_t* pCaller, int32_t nPtr, int32_t nLen)
{
   if (nPtr < 0  ||  nLen <= 0)
      return nullptr;

   wasmtime_extern_t ext;
   bool bFound = wasmtime_caller_export_get (pCaller, "memory", 6, &ext);

   if (bFound  &&  ext.kind == WASMTIME_EXTERN_MEMORY)
   {
      wasmtime_context_t* pCtx = wasmtime_caller_context (pCaller);
      uint8_t* pData = wasmtime_memory_data (pCtx, &ext.of.memory);
      size_t nMemSize = wasmtime_memory_data_size (pCtx, &ext.of.memory);

      if (static_cast<size_t> (nPtr + nLen) <= nMemSize)
         return pData + nPtr;
   }

   return nullptr;
}

// ---------------------------------------------------------------------------
// WriteWasmString — writes a UTF-8 string into the caller's linear memory.
//
// Always returns the full size of sValue (the bytes needed), regardless of
// how many were actually written. Writes up to nLen bytes; the caller derives
// the count written as min(return, nLen). A query call (nLen == 0) returns the
// required size without writing — the caller can then allocate exactly and
// call again. A return greater than the nLen passed in signals truncation.
// ---------------------------------------------------------------------------

int32_t WriteWasmString (wasmtime_caller_t* pCaller, int32_t nPtr, int32_t nLen, const std::string& sValue)
{
   int32_t nNeeded = static_cast<int32_t> (sValue.size ());

   if (nPtr >= 0  &&  nLen > 0)
   {
      wasmtime_extern_t ext;
      bool bFound = wasmtime_caller_export_get (pCaller, "memory", 6, &ext);

      if (bFound  &&  ext.kind == WASMTIME_EXTERN_MEMORY)
      {
         wasmtime_context_t* pCtx = wasmtime_caller_context (pCaller);
         uint8_t* pData = wasmtime_memory_data (pCtx, &ext.of.memory);
         size_t nMemSize = wasmtime_memory_data_size (pCtx, &ext.of.memory);

         if (static_cast<size_t> (nPtr + nLen) <= nMemSize)
         {
            int32_t nWritten = (nNeeded < nLen) ? nNeeded : nLen;

            memcpy (pData + nPtr, sValue.data (), static_cast<size_t> (nWritten));
         }
      }
   }

   return nNeeded;
}

// ---------------------------------------------------------------------------
// Container — recovers the CONTAINER* from the env pointer chain.
// pEnv is a WASM_STORE* whose HostData() points to the owning CONTAINER*.
// ---------------------------------------------------------------------------

static CONTAINER* Container (void* pEnv)
{
   WASM_STORE* pWasm_Store = static_cast<WASM_STORE*> (pEnv);

   CONTAINER* pContainer = nullptr;

   if (pWasm_Store)
   {
      pContainer = static_cast<CONTAINER*> (pWasm_Store->HostData ());
   }

   return pContainer;
}

static SCENE* Scene (void* pEnv)
{
   CONTAINER* pContainer = Container (pEnv);

   return pContainer ? pContainer->Context ()->Scene () : nullptr;
}

static STREAM* Stream (void* pEnv)
{
   CONTAINER* pContainer = Container (pEnv);

   return pContainer ? pContainer->Stream () : nullptr;
}

static SILO* Silo (void* pEnv)
{
   CONTAINER* pContainer = Container (pEnv);

   return pContainer ? pContainer->Silo () : nullptr;
}

// ---------------------------------------------------------------------------
// Console host functions — forward calls to the container's STREAM.
// ---------------------------------------------------------------------------

wasm_trap_t* Console_Log (void* pEnv, wasmtime_caller_t* pCaller, const wasmtime_val_t* pArgs, size_t nArgs, wasmtime_val_t* pResults, size_t nResults)
{
   (void) pResults; (void) nResults;

   STREAM* pStream;

   if (nArgs >= 2  &&  (pStream = Stream (pEnv)))
      pStream->Log (ReadWasmString (pCaller, pArgs[0].of.i32, pArgs[1].of.i32));

   return nullptr;
}

wasm_trap_t* Console_Debug (void* pEnv, wasmtime_caller_t* pCaller, const wasmtime_val_t* pArgs, size_t nArgs, wasmtime_val_t* pResults, size_t nResults)
{
   (void) pResults; (void) nResults;

   STREAM* pStream;

   if (nArgs >= 2  &&  (pStream = Stream (pEnv)))
      pStream->Debug (ReadWasmString (pCaller, pArgs[0].of.i32, pArgs[1].of.i32));

   return nullptr;
}

wasm_trap_t* Console_Info (void* pEnv, wasmtime_caller_t* pCaller, const wasmtime_val_t* pArgs, size_t nArgs, wasmtime_val_t* pResults, size_t nResults)
{
   (void) pResults; (void) nResults;

   STREAM* pStream;

   if (nArgs >= 2  &&  (pStream = Stream (pEnv)))
      pStream->Info (ReadWasmString (pCaller, pArgs[0].of.i32, pArgs[1].of.i32));

   return nullptr;
}

wasm_trap_t* Console_Warn (void* pEnv, wasmtime_caller_t* pCaller, const wasmtime_val_t* pArgs, size_t nArgs, wasmtime_val_t* pResults, size_t nResults)
{
   (void) pResults; (void) nResults;

   STREAM* pStream;

   if (nArgs >= 2  &&  (pStream = Stream (pEnv)))
      pStream->Warn (ReadWasmString (pCaller, pArgs[0].of.i32, pArgs[1].of.i32));

   return nullptr;
}

wasm_trap_t* Console_Error (void* pEnv, wasmtime_caller_t* pCaller, const wasmtime_val_t* pArgs, size_t nArgs, wasmtime_val_t* pResults, size_t nResults)
{
   (void) pResults; (void) nResults;

   STREAM* pStream;

   if (nArgs >= 2  &&  (pStream = Stream (pEnv)))
      pStream->Error (ReadWasmString (pCaller, pArgs[0].of.i32, pArgs[1].of.i32));

   return nullptr;
}

wasm_trap_t* Console_Assert (void* pEnv, wasmtime_caller_t* pCaller, const wasmtime_val_t* pArgs, size_t nArgs, wasmtime_val_t* pResults, size_t nResults)
{
   (void) pResults; (void) nResults;

   STREAM* pStream;

   if (nArgs >= 3  &&  (pStream = Stream (pEnv)))
      pStream->Assert (pArgs[0].of.i32 != 0, ReadWasmString (pCaller, pArgs[1].of.i32, pArgs[2].of.i32));

   return nullptr;
}

wasm_trap_t* Console_Group (void* pEnv, wasmtime_caller_t* pCaller, const wasmtime_val_t* pArgs, size_t nArgs, wasmtime_val_t* pResults, size_t nResults)
{
   (void) pResults; (void) nResults;

   STREAM* pStream;

   if (nArgs >= 2  &&  (pStream = Stream (pEnv)))
      pStream->Group (ReadWasmString (pCaller, pArgs[0].of.i32, pArgs[1].of.i32));

   return nullptr;
}

wasm_trap_t* Console_GroupCollapsed (void* pEnv, wasmtime_caller_t* pCaller, const wasmtime_val_t* pArgs, size_t nArgs, wasmtime_val_t* pResults, size_t nResults)
{
   (void) pResults; (void) nResults;

   STREAM* pStream;

   if (nArgs >= 2  &&  (pStream = Stream (pEnv)))
      pStream->GroupCollapsed (ReadWasmString (pCaller, pArgs[0].of.i32, pArgs[1].of.i32));

   return nullptr;
}

wasm_trap_t* Console_GroupEnd (void* pEnv, wasmtime_caller_t* pCaller, const wasmtime_val_t* pArgs, size_t nArgs, wasmtime_val_t* pResults, size_t nResults)
{
   (void) pCaller; (void) pArgs; (void) nArgs; (void) pResults; (void) nResults;

   STREAM* pStream;

   if ((pStream = Stream (pEnv)))
      pStream->GroupEnd ();

   return nullptr;
}

wasm_trap_t* Console_Count (void* pEnv, wasmtime_caller_t* pCaller, const wasmtime_val_t* pArgs, size_t nArgs, wasmtime_val_t* pResults, size_t nResults)
{
   (void) pResults; (void) nResults;

   STREAM* pStream;

   if (nArgs >= 2  &&  (pStream = Stream (pEnv)))
      pStream->Count (ReadWasmString (pCaller, pArgs[0].of.i32, pArgs[1].of.i32));

   return nullptr;
}

wasm_trap_t* Console_CountReset (void* pEnv, wasmtime_caller_t* pCaller, const wasmtime_val_t* pArgs, size_t nArgs, wasmtime_val_t* pResults, size_t nResults)
{
   (void) pResults; (void) nResults;

   STREAM* pStream;

   if (nArgs >= 2  &&  (pStream = Stream (pEnv)))
      pStream->CountReset (ReadWasmString (pCaller, pArgs[0].of.i32, pArgs[1].of.i32));

   return nullptr;
}

wasm_trap_t* Console_Time (void* pEnv, wasmtime_caller_t* pCaller, const wasmtime_val_t* pArgs, size_t nArgs, wasmtime_val_t* pResults, size_t nResults)
{
   (void) pResults; (void) nResults;

   STREAM* pStream;

   if (nArgs >= 2  &&  (pStream = Stream (pEnv)))
      pStream->Time (ReadWasmString (pCaller, pArgs[0].of.i32, pArgs[1].of.i32));

   return nullptr;
}

wasm_trap_t* Console_TimeEnd (void* pEnv, wasmtime_caller_t* pCaller, const wasmtime_val_t* pArgs, size_t nArgs, wasmtime_val_t* pResults, size_t nResults)
{
   (void) pResults; (void) nResults;

   STREAM* pStream;

   if (nArgs >= 2  &&  (pStream = Stream (pEnv)))
      pStream->TimeEnd (ReadWasmString (pCaller, pArgs[0].of.i32, pArgs[1].of.i32));

   return nullptr;
}

wasm_trap_t* Console_TimeLog (void* pEnv, wasmtime_caller_t* pCaller, const wasmtime_val_t* pArgs, size_t nArgs, wasmtime_val_t* pResults, size_t nResults)
{
   (void) pResults; (void) nResults;

   STREAM* pStream;

   if (nArgs >= 2  &&  (pStream = Stream (pEnv)))
      pStream->TimeLog (ReadWasmString (pCaller, pArgs[0].of.i32, pArgs[1].of.i32));

   return nullptr;
}

// ---------------------------------------------------------------------------
// Storage host functions — forward calls to the container's SILO.
//
// Get:     (i32 scope, i32 pathPtr, i32 pathLen, i32 outPtr, i32 outLen) -> i32 size needed
// Set:     (i32 scope, i32 pathPtr, i32 pathLen, i32 valPtr, i32 valLen) -> i32 success
// Remove:  (i32 scope, i32 pathPtr, i32 pathLen)                         -> i32 success
// Has:     (i32 scope, i32 pathPtr, i32 pathLen)                         -> i32 bool
// GetJson: (i32 scope, i32 outPtr,  i32 outLen)                          -> i32 size needed
// SetJson: (i32 scope, i32 jsonPtr, i32 jsonLen)                         -> i32 success
//
// Get/GetJson return the full byte size of the value. The caller derives the
// count written as min(return, outLen); a return > outLen means truncation —
// reallocate to the returned size and call again. Passing outLen == 0 queries
// the size without writing.
// ---------------------------------------------------------------------------

wasm_trap_t* Storage_Get (void* pEnv, wasmtime_caller_t* pCaller, const wasmtime_val_t* pArgs, size_t nArgs, wasmtime_val_t* pResults, size_t nResults)
{
   int32_t nResult = 0;

   SILO* pSilo;

   if (nArgs >= 5  &&  (pSilo = Silo (pEnv)))
   {
      eSILO_SCOPE eScope = static_cast<eSILO_SCOPE> (pArgs[0].of.i32);
      std::string sPath  = ReadWasmString (pCaller, pArgs[1].of.i32, pArgs[2].of.i32);

      nlohmann::json jValue = pSilo->Get (eScope, sPath);

      std::string sValue = jValue.is_null () ? std::string () : jValue.dump ();

      nResult = WriteWasmString (pCaller, pArgs[3].of.i32, pArgs[4].of.i32, sValue);
   }

   if (nResults > 0) pResults[0].of.i32 = nResult;

   return nullptr;
}

wasm_trap_t* Storage_Set (void* pEnv, wasmtime_caller_t* pCaller, const wasmtime_val_t* pArgs, size_t nArgs, wasmtime_val_t* pResults, size_t nResults)
{
   int32_t nResult = 0;

   SILO* pSilo;

   if (nArgs >= 5  &&  (pSilo = Silo (pEnv)))
   {
      eSILO_SCOPE eScope = static_cast<eSILO_SCOPE> (pArgs[0].of.i32);
      std::string sPath  = ReadWasmString (pCaller, pArgs[1].of.i32, pArgs[2].of.i32);
      std::string sValue = ReadWasmString (pCaller, pArgs[3].of.i32, pArgs[4].of.i32);

      nlohmann::json jValue = nlohmann::json::parse (sValue, nullptr, false);

      if (!jValue.is_discarded ())
      {
         pSilo->Set (eScope, sPath, jValue);

         nResult = 1;
      }
   }

   if (nResults > 0) pResults[0].of.i32 = nResult;

   return nullptr;
}

wasm_trap_t* Storage_Remove (void* pEnv, wasmtime_caller_t* pCaller, const wasmtime_val_t* pArgs, size_t nArgs, wasmtime_val_t* pResults, size_t nResults)
{
   int32_t nResult = 0;

   SILO* pSilo;

   if (nArgs >= 3  &&  (pSilo = Silo (pEnv)))
   {
      eSILO_SCOPE eScope = static_cast<eSILO_SCOPE> (pArgs[0].of.i32);
      std::string sPath  = ReadWasmString (pCaller, pArgs[1].of.i32, pArgs[2].of.i32);

      pSilo->Remove (eScope, sPath);

      nResult = 1;
   }

   if (nResults > 0) pResults[0].of.i32 = nResult;

   return nullptr;
}

wasm_trap_t* Storage_Has (void* pEnv, wasmtime_caller_t* pCaller, const wasmtime_val_t* pArgs, size_t nArgs, wasmtime_val_t* pResults, size_t nResults)
{
   int32_t nResult = 0;

   SILO* pSilo;

   if (nArgs >= 3  &&  (pSilo = Silo (pEnv)))
   {
      eSILO_SCOPE eScope = static_cast<eSILO_SCOPE> (pArgs[0].of.i32);
      std::string sPath  = ReadWasmString (pCaller, pArgs[1].of.i32, pArgs[2].of.i32);

      nResult = pSilo->Has (eScope, sPath) ? 1 : 0;
   }

   if (nResults > 0) pResults[0].of.i32 = nResult;

   return nullptr;
}

wasm_trap_t* Storage_GetJson (void* pEnv, wasmtime_caller_t* pCaller, const wasmtime_val_t* pArgs, size_t nArgs, wasmtime_val_t* pResults, size_t nResults)
{
   int32_t nResult = 0;

   SILO* pSilo;

   if (nArgs >= 3  &&  (pSilo = Silo (pEnv)))
   {
      eSILO_SCOPE eScope = static_cast<eSILO_SCOPE> (pArgs[0].of.i32);

      std::string sJson = pSilo->Json (eScope);

      nResult = WriteWasmString (pCaller, pArgs[1].of.i32, pArgs[2].of.i32, sJson);
   }

   if (nResults > 0) pResults[0].of.i32 = nResult;

   return nullptr;
}

wasm_trap_t* Storage_SetJson (void* pEnv, wasmtime_caller_t* pCaller, const wasmtime_val_t* pArgs, size_t nArgs, wasmtime_val_t* pResults, size_t nResults)
{
   int32_t nResult = 0;

   SILO* pSilo;

   if (nArgs >= 3  &&  (pSilo = Silo (pEnv)))
   {
      eSILO_SCOPE eScope = static_cast<eSILO_SCOPE> (pArgs[0].of.i32);
      std::string sJson  = ReadWasmString (pCaller, pArgs[1].of.i32, pArgs[2].of.i32);

      pSilo->Json (eScope, sJson);

      nResult = 1;
   }

   if (nResults > 0) pResults[0].of.i32 = nResult;

   return nullptr;
}

// ---------------------------------------------------------------------------
// Scene host functions
//
// Node_Map:   (i64 twFabricIx, i32 ptr, i32 len) -> i64 twRootIx
//   Map-managed mode: reads a node tree out of the MSF "data" block for
//   twFabricIx and builds the whole fabric graph host-side (no per-node WASM
//   calls). [ptr..ptr+len) is a UTF-8, dot-separated path locating the tree
//   inside "data" (e.g. "scene", or "a.b.c"); an empty path uses the "data"
//   object itself. The rest of "data" is free for the module's own use.
//   Simulates a map service injecting nodes. Mutually exclusive with
//   WASM-managed Node_Root.
//
// Node_Root:  (i32 twFabricIx, i32 ptr, i32 len) -> i64 twObjectIx
//   Creates a root node on the fabric identified by twFabricIx.
//   Reads an RMCOBJECT (528 bytes) from WASM linear memory at [ptr..ptr+len).
//
// Node_Open:  (i64 twParentIx, i32 ptr, i32 len) -> i64 twObjectIx
//   Creates a child node under twParentIx (fabric inherited from parent).
//   Reads an RMCOBJECT (528 bytes) from WASM linear memory at [ptr..ptr+len).
//
// Node_Close: (i64 twObjectIx) -> i32 success
//   Removes and deletes the node identified by twObjectIx.
//
// Node_Panel: (i64 twParentIx, i32 objPtr, i32 objLen, i32 srcPtr, i32 srcLen) -> i64 twObjectIx
//   Creates a child panel node under twParentIx from an RMCOBJECT (528 bytes),
//   forcing its class to MAP_OBJECT_CLASS_PANEL, then sets the panel's RML+CSS
//   source from [srcPtr..srcPtr+srcLen). Returns the new object index.
//
// Node_Panel_Map: (i64 twFabricIx, i64 twParentIx, i32 objPtr, i32 objLen, i32 pathPtr, i32 pathLen) -> i64 twObjectIx
//   Like Node_Panel, but the RML+CSS source is not passed from WASM memory;
//   instead [pathPtr..pathPtr+pathLen) is a UTF-8, dot-separated path locating
//   a string value inside the MSF "data" block for twFabricIx (e.g. "panel",
//   or "a.b.c"; an empty path is the "data" object itself, which is not a
//   string). This lets the panel's document be authored in the fabric file
//   rather than embedded in the module. If the path resolves to no string, the
//   panel shows the engine's built-in default document.
//
// Mutators:   (i64 twObjectIx, ...) -> void
//   Modify properties on the MAP_OBJECT through the handle table.
// ---------------------------------------------------------------------------

// Resolve a dot-separated path inside a JSON object (e.g. "scene" or "a.b.c").
// An empty path returns the root itself. Returns nullptr if any segment is
// missing or a non-object is traversed. The returned pointer aliases jRoot, so
// it is only valid while jRoot is alive.
static const nlohmann::json* Data_Resolve (const nlohmann::json& jRoot, const std::string& sPath)
{
   const nlohmann::json* pNode  = &jRoot;
   bool                  bFound = true;
   size_t                nStart = 0;

   while (bFound  &&  nStart < sPath.size ())
   {
      size_t      nDot = sPath.find ('.', nStart);
      size_t      nEnd = (nDot == std::string::npos) ? sPath.size () : nDot;
      std::string sKey = sPath.substr (nStart, nEnd - nStart);

      if (pNode->is_object ()  &&  pNode->contains (sKey))
         pNode = &(*pNode)[sKey];
      else
         bFound = false;

      nStart = nEnd + 1;
   }

   return bFound ? pNode : nullptr;
}

wasm_trap_t* Scene_Node_Map (void* pEnv, wasmtime_caller_t* pCaller, const wasmtime_val_t* pArgs, size_t nArgs, wasmtime_val_t* pResults, size_t nResults)
{
   uint64_t twResult = OBJECTIX_ERROR;

   if (nArgs >= 3)
   {
      uint64_t    twFabricIx = static_cast<uint64_t> (pArgs[0].of.i64);
      std::string sPath      = ReadWasmString (pCaller, pArgs[1].of.i32, pArgs[2].of.i32);

      CONTAINER* pContainer = Container (pEnv);
      SCENE*     pScene     = pContainer ? pContainer->Context ()->Scene () : nullptr;
      FABRIC*    pFabric    = pScene     ? pScene->Fabric_Find (twFabricIx) : nullptr;
      MSF*       pMsf       = pFabric    ? pFabric->Msf ()                  : nullptr;

      if (pMsf)
      {
         nlohmann::json jPayload = pMsf->Payload ();

         if (jPayload.is_object ()  &&  jPayload.contains (PAYLOAD_KEY_DATA)  &&  jPayload[PAYLOAD_KEY_DATA].is_object ())
         {
            // The scene tree lives somewhere inside the "data" block, addressed
            // by a dot-separated path. An empty path is the "data" block itself.
            const nlohmann::json* pRoot = Data_Resolve (jPayload[PAYLOAD_KEY_DATA], sPath);

            if (pRoot)
               twResult = pContainer->Branch_Add (twFabricIx, *pRoot);
         }
      }
   }

   if (nResults > 0)
   {
      pResults[0].kind   = WASMTIME_I64;
      pResults[0].of.i64 = static_cast<int64_t> (twResult);
   }

   return nullptr;
}

wasm_trap_t* Scene_Node_Root (void* pEnv, wasmtime_caller_t* pCaller, const wasmtime_val_t* pArgs, size_t nArgs, wasmtime_val_t* pResults, size_t nResults)
{
   uint64_t twResult = OBJECTIX_ERROR;

   if (nArgs >= 3)
   {
      uint64_t twFabricIx = static_cast<uint64_t> (pArgs[0].of.i64);
      int32_t  nPtr       = pArgs[1].of.i32;
      int32_t  nLen       = pArgs[2].of.i32;

      if (nLen >= static_cast<int32_t> (sizeof (RMCOBJECT)))
      {
         const uint8_t* pBytes = ReadWasmBytes (pCaller, nPtr, nLen);

         if (pBytes)
         {
            auto* pContainer = Container (pEnv);

            if (pContainer)
            {
               const auto* pObject = reinterpret_cast<const RMCOBJECT*> (pBytes);
               twResult = pContainer->Node_Root (twFabricIx, pObject);
            }
         }
      }
   }

   if (nResults > 0)
   {
      pResults[0].kind   = WASMTIME_I64;
      pResults[0].of.i64 = static_cast<int64_t> (twResult);
   }

   return nullptr;
}

wasm_trap_t* Scene_Node_Open (void* pEnv, wasmtime_caller_t* pCaller, const wasmtime_val_t* pArgs, size_t nArgs, wasmtime_val_t* pResults, size_t nResults)
{
   uint64_t twResult = OBJECTIX_ERROR;

   if (nArgs >= 3)
   {
      uint64_t twParentIx = static_cast<uint64_t> (pArgs[0].of.i64);
      int32_t  nPtr       = pArgs[1].of.i32;
      int32_t  nLen       = pArgs[2].of.i32;

      if (nLen >= static_cast<int32_t> (sizeof (RMCOBJECT)))
      {
         const uint8_t* pBytes = ReadWasmBytes (pCaller, nPtr, nLen);

         if (pBytes)
         {
            auto* pContainer = Container (pEnv);

            if (pContainer)
            {
               const auto* pObject = reinterpret_cast<const RMCOBJECT*> (pBytes);
               twResult = pContainer->Node_Open (twParentIx, pObject);
            }
         }
      }
   }

   if (nResults > 0)
   {
      pResults[0].kind   = WASMTIME_I64;
      pResults[0].of.i64 = static_cast<int64_t> (twResult);
   }

   return nullptr;
}

wasm_trap_t* Scene_Node_Close (void* pEnv, wasmtime_caller_t* pCaller, const wasmtime_val_t* pArgs, size_t nArgs, wasmtime_val_t* pResults, size_t nResults)
{
   (void) pCaller;

   int32_t nResult = 0;

   if (nArgs >= 1)
   {
      uint64_t twObjectIx = static_cast<uint64_t> (pArgs[0].of.i64);
      
      auto* pContainer = Container (pEnv);

      if (pContainer  &&  pContainer->Node_Close (twObjectIx))
         nResult = 1;
   }

   if (nResults > 0) pResults[0].of.i32 = nResult;

   return nullptr;
}

wasm_trap_t* Scene_Node_Position (void* pEnv, wasmtime_caller_t* pCaller, const wasmtime_val_t* pArgs, size_t nArgs, wasmtime_val_t* pResults, size_t nResults)
{
   (void) pCaller; (void) pResults; (void) nResults;

   if (nArgs >= 4)
   {
      uint64_t twObjectIx = static_cast<uint64_t> (pArgs[0].of.i64);
      auto* pContainer = Container (pEnv);

      if (pContainer)
      {
         NODE* pNode = pContainer->Node_Find (twObjectIx);
         MAP_OBJECT* pObj = pNode ? pNode->Map_Object () : nullptr;

         if (pObj)
         {
            pObj->Transform.d3Position[0] = pArgs[1].of.f64;
            pObj->Transform.d3Position[1] = pArgs[2].of.f64;
            pObj->Transform.d3Position[2] = pArgs[3].of.f64;
         }
      }
   }

   return nullptr;
}

wasm_trap_t* Scene_Node_Scale (void* pEnv, wasmtime_caller_t* pCaller, const wasmtime_val_t* pArgs, size_t nArgs, wasmtime_val_t* pResults, size_t nResults)
{
   (void) pCaller; (void) pResults; (void) nResults;

   if (nArgs >= 2)
   {
      uint64_t twObjectIx = static_cast<uint64_t> (pArgs[0].of.i64);
      auto* pContainer = Container (pEnv);

      if (pContainer)
      {
         NODE* pNode = pContainer->Node_Find (twObjectIx);
         MAP_OBJECT* pObj = pNode ? pNode->Map_Object () : nullptr;

         if (pObj)
            pObj->Transform.d3Scale[0] = pArgs[1].of.f64;
      }
   }

   return nullptr;
}

wasm_trap_t* Scene_Node_Bound (void* pEnv, wasmtime_caller_t* pCaller, const wasmtime_val_t* pArgs, size_t nArgs, wasmtime_val_t* pResults, size_t nResults)
{
   (void) pCaller; (void) pResults; (void) nResults;

   if (nArgs >= 2)
   {
      uint64_t twObjectIx = static_cast<uint64_t> (pArgs[0].of.i64);
      auto* pContainer = Container (pEnv);

      if (pContainer)
      {
         NODE* pNode = pContainer->Node_Find (twObjectIx);
         MAP_OBJECT* pObj = pNode ? pNode->Map_Object () : nullptr;

         if (pObj)
         {
            pObj->Bound.d3Max[0] = pArgs[1].of.f64;
            pObj->Bound.d3Max[1] = pArgs[1].of.f64;
            pObj->Bound.d3Max[2] = pArgs[1].of.f64;
         }
      }
   }

   return nullptr;
}

wasm_trap_t* Scene_Node_Color (void* pEnv, wasmtime_caller_t* pCaller, const wasmtime_val_t* pArgs, size_t nArgs, wasmtime_val_t* pResults, size_t nResults)
{
   (void) pCaller; (void) pResults; (void) nResults;

   if (nArgs >= 2)
   {
      uint64_t twObjectIx = static_cast<uint64_t> (pArgs[0].of.i64);
      auto* pContainer = Container (pEnv);

      if (pContainer)
      {
         NODE* pNode = pContainer->Node_Find (twObjectIx);
         MAP_OBJECT* pObj = pNode ? pNode->Map_Object () : nullptr;

         if (pObj)
         {
            uint32_t nColor = static_cast<uint32_t> (pArgs[1].of.i32);
            memcpy (&pObj->Properties.Celestial.fColor, &nColor, 4);
         }
      }
   }

   return nullptr;
}

wasm_trap_t* Scene_Node_Name (void* pEnv, wasmtime_caller_t* pCaller, const wasmtime_val_t* pArgs, size_t nArgs, wasmtime_val_t* pResults, size_t nResults)
{
   (void) pResults; (void) nResults;

   if (nArgs >= 3)
   {
      uint64_t twObjectIx = static_cast<uint64_t> (pArgs[0].of.i64);
      auto* pContainer = Container (pEnv);

      if (pContainer)
      {
         NODE* pNode = pContainer->Node_Find (twObjectIx);
         MAP_OBJECT* pObj = pNode ? pNode->Map_Object () : nullptr;

         if (pObj)
         {
            std::string sName = ReadWasmString (pCaller, pArgs[1].of.i32, pArgs[2].of.i32);

            memset (&pObj->Name, 0, sizeof (MAP_OBJECT::MAP_OBJECT_NAME));
            size_t nLen = std::min<size_t> (sName.size (), 47);

            for (int i = 0; i < nLen; i++)
               pObj->Name.wsName[i] = static_cast<uint16_t> (static_cast<uint8_t> (sName[i]));
         }
      }
   }

   return nullptr;
}

wasm_trap_t* Scene_Node_Radius (void* pEnv, wasmtime_caller_t* pCaller, const wasmtime_val_t* pArgs, size_t nArgs, wasmtime_val_t* pResults, size_t nResults)
{
   (void) pCaller; (void) pResults; (void) nResults;

   if (nArgs >= 2)
   {
      uint64_t twObjectIx = static_cast<uint64_t> (pArgs[0].of.i64);
      auto* pContainer = Container (pEnv);

      if (pContainer)
      {
         NODE* pNode = pContainer->Node_Find (twObjectIx);
         MAP_OBJECT* pObj = pNode ? pNode->Map_Object () : nullptr;

         if (pObj)
         {
            pObj->Bound.d3Max[0] = pArgs[1].of.f64;
            pObj->Bound.d3Max[1] = pArgs[1].of.f64;
            pObj->Bound.d3Max[2] = pArgs[1].of.f64;
         }
      }
   }

   return nullptr;
}

wasm_trap_t* Scene_Node_Texture (void* pEnv, wasmtime_caller_t* pCaller, const wasmtime_val_t* pArgs, size_t nArgs, wasmtime_val_t* pResults, size_t nResults)
{
   (void) pResults; (void) nResults;

   if (nArgs >= 3)
   {
      uint64_t twObjectIx = static_cast<uint64_t> (pArgs[0].of.i64);
      auto* pContainer = Container (pEnv);

      if (pContainer)
      {
         NODE* pNode = pContainer->Node_Find (twObjectIx);
         MAP_OBJECT* pObj = pNode ? pNode->Map_Object () : nullptr;

         if (pObj)
         {
            std::string sUrl = ReadWasmString (pCaller, pArgs[1].of.i32, pArgs[2].of.i32);
            strncpy (pObj->Resource.sReference, sUrl.c_str (), sizeof (pObj->Resource.sReference) - 1);
            pObj->Resource.sReference[sizeof (pObj->Resource.sReference) - 1] = '\0';
         }
      }
   }

   return nullptr;
}

// Panel_Create — shared panel node construction. Copies the wire object,
// forces its class to PANEL (regardless of how the caller composed Head.Self),
// opens it under twParentIx, and applies the RML+CSS source. An empty source
// leaves the panel showing the engine's built-in default document. Returns the
// new object index, or OBJECTIX_ERROR on failure.
static uint64_t Panel_Create (CONTAINER* pContainer, uint64_t twParentIx, const RMCOBJECT* pObject, const std::string& sSource)
{
   uint64_t twResult = OBJECTIX_ERROR;

   RMCOBJECT Object = *pObject;

   Object.Head.Self.qwComposed = OBJECTIX_COMPOSE (MAP_OBJECT::MAP_OBJECT_CLASS_PANEL, Object.Head.Self.ObjectIx ());

   twResult = pContainer->Node_Open (twParentIx, &Object);

   if (twResult != OBJECTIX_ERROR)
   {
      NODE*       pNode = pContainer->Node_Find (twResult);
      MAP_OBJECT* pObj  = pNode ? pNode->Map_Object () : nullptr;

      MAP_OBJECT_PANEL* pPanel = dynamic_cast<MAP_OBJECT_PANEL*> (pObj);

      if (pPanel  &&  !sSource.empty ())
         pPanel->Source (sSource);
   }

   return twResult;
}

wasm_trap_t* Scene_Node_Panel (void* pEnv, wasmtime_caller_t* pCaller, const wasmtime_val_t* pArgs, size_t nArgs, wasmtime_val_t* pResults, size_t nResults)
{
   uint64_t twResult = OBJECTIX_ERROR;

   if (nArgs >= 5)
   {
      uint64_t twParentIx = static_cast<uint64_t> (pArgs[0].of.i64);
      int32_t  nObjPtr    = pArgs[1].of.i32;
      int32_t  nObjLen    = pArgs[2].of.i32;
      int32_t  nSrcPtr    = pArgs[3].of.i32;
      int32_t  nSrcLen    = pArgs[4].of.i32;

      if (nObjLen >= static_cast<int32_t> (sizeof (RMCOBJECT)))
      {
         const uint8_t* pBytes     = ReadWasmBytes (pCaller, nObjPtr, nObjLen);
         auto*          pContainer = Container (pEnv);

         if (pBytes  &&  pContainer)
         {
            std::string sSource = ReadWasmString (pCaller, nSrcPtr, nSrcLen);

            twResult = Panel_Create (pContainer, twParentIx, reinterpret_cast<const RMCOBJECT*> (pBytes), sSource);
         }
      }
   }

   if (nResults > 0)
   {
      pResults[0].kind   = WASMTIME_I64;
      pResults[0].of.i64 = static_cast<int64_t> (twResult);
   }

   return nullptr;
}

wasm_trap_t* Scene_Node_Panel_Map (void* pEnv, wasmtime_caller_t* pCaller, const wasmtime_val_t* pArgs, size_t nArgs, wasmtime_val_t* pResults, size_t nResults)
{
   uint64_t twResult = OBJECTIX_ERROR;

   if (nArgs >= 6)
   {
      uint64_t twFabricIx = static_cast<uint64_t> (pArgs[0].of.i64);
      uint64_t twParentIx = static_cast<uint64_t> (pArgs[1].of.i64);
      int32_t  nObjPtr    = pArgs[2].of.i32;
      int32_t  nObjLen    = pArgs[3].of.i32;
      int32_t  nPathPtr   = pArgs[4].of.i32;
      int32_t  nPathLen   = pArgs[5].of.i32;

      if (nObjLen >= static_cast<int32_t> (sizeof (RMCOBJECT)))
      {
         const uint8_t* pBytes     = ReadWasmBytes (pCaller, nObjPtr, nObjLen);
         std::string    sPath      = ReadWasmString (pCaller, nPathPtr, nPathLen);

         CONTAINER* pContainer = Container (pEnv);
         SCENE*     pScene     = pContainer ? pContainer->Context ()->Scene () : nullptr;
         FABRIC*    pFabric    = pScene     ? pScene->Fabric_Find (twFabricIx) : nullptr;
         MSF*       pMsf       = pFabric    ? pFabric->Msf ()                  : nullptr;

         if (pBytes  &&  pContainer  &&  pMsf)
         {
            // The panel's RML+CSS source lives inside the MSF "data" block,
            // addressed by a dot-separated path (e.g. "panel"). An empty path
            // is the "data" block itself.
            std::string sSource;

            nlohmann::json jPayload = pMsf->Payload ();

            if (jPayload.is_object ()  &&  jPayload.contains (PAYLOAD_KEY_DATA)  &&  jPayload[PAYLOAD_KEY_DATA].is_object ())
            {
               const nlohmann::json* pSource = Data_Resolve (jPayload[PAYLOAD_KEY_DATA], sPath);

               if (pSource  &&  pSource->is_string ())
                  sSource = pSource->get<std::string> ();
            }

            if (sSource.empty ())
               pScene->Engine ()->Log (IENGINE::kLOGLEVEL_Warning, "PANEL", "No RML string found at data path \"" + sPath + "\"; using default panel document");

            twResult = Panel_Create (pContainer, twParentIx, reinterpret_cast<const RMCOBJECT*> (pBytes), sSource);
         }
      }
   }

   if (nResults > 0)
   {
      pResults[0].kind   = WASMTIME_I64;
      pResults[0].of.i64 = static_cast<int64_t> (twResult);
   }

   return nullptr;
}

// ---------------------------------------------------------------------------
// Timer host function stubs
// ---------------------------------------------------------------------------

wasm_trap_t* Timer_Set (void* pEnv, wasmtime_caller_t* pCaller, const wasmtime_val_t* pArgs, size_t nArgs, wasmtime_val_t* pResults, size_t nResults)
{
   (void) pEnv; (void) pCaller; (void) pArgs; (void) nArgs;
   if (nResults > 0) pResults[0].of.i32 = 0;
   return nullptr;
}

wasm_trap_t* Timer_Clear (void* pEnv, wasmtime_caller_t* pCaller, const wasmtime_val_t* pArgs, size_t nArgs, wasmtime_val_t* pResults, size_t nResults)
{
   (void) pEnv; (void) pCaller; (void) pArgs; (void) nArgs; (void) pResults; (void) nResults;
   return nullptr;
}

} // namespace DEP
} // namespace SNEEZE
