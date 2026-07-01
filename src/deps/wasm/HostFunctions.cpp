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

namespace SNEEZE
{
namespace DEP
{

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
// Node_Map:   (i64 twFabricIx) -> i64 twRootIx
//   Map-managed mode: reads the MSF "data" node tree for twFabricIx and builds
//   the whole fabric graph host-side (no per-node WASM calls). Simulates a map
//   service injecting nodes. Mutually exclusive with WASM-managed Node_Root.
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
// Mutators:   (i64 twObjectIx, ...) -> void
//   Modify properties on the MAP_OBJECT through the handle table.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// RmcObject_FromJson — inverse of RmcObject_ToJson. Fills a wire RMCOBJECT
// from one node object of the MSF "data" tree (the "Children" array is the
// caller's responsibility — it is not part of the flat wire object).
// ---------------------------------------------------------------------------

// ComposeFromId — turn a human "<class>-<index>" id (e.g. "P-5039") into a
// composed OBJECTIX. Class letters: R root, C celestial, T terrestrial,
// P physical, L light.
static uint64_t ComposeFromId (const std::string& sId)
{
   uint64_t twResult = 0;
   size_t   nDash    = sId.find ('-');

   if (nDash != std::string::npos)
   {
      char     cClass = sId[0];
      uint64_t nIndex = strtoull (sId.c_str () + nDash + 1, nullptr, 10);

      MAP_OBJECT::MAP_OBJECT_CLASS eClass = MAP_OBJECT::MAP_OBJECT_CLASS_PHYSICAL;
      if      (cClass == 'R') eClass = MAP_OBJECT::MAP_OBJECT_CLASS_ROOT;
      else if (cClass == 'C') eClass = MAP_OBJECT::MAP_OBJECT_CLASS_CELESTIAL;
      else if (cClass == 'T') eClass = MAP_OBJECT::MAP_OBJECT_CLASS_TERRESTRIAL;
      else if (cClass == 'P') eClass = MAP_OBJECT::MAP_OBJECT_CLASS_PHYSICAL;
      else if (cClass == 'L') eClass = MAP_OBJECT::MAP_OBJECT_CLASS_LIGHT;

      twResult = OBJECTIX_COMPOSE (eClass, nIndex);
   }

   return twResult;
}

static void RmcObject_FromJson (const nlohmann::json& j, RMCOBJECT* pObject)
{
   *pObject = RMCOBJECT {};

   // Sensible decode defaults for omitted transform fields: identity orientation and unit scale 
   // (a zero quaternion / zero scale would be degenerate). Present fields below overwrite these.
   pObject->Transform.d4Rotation[3] = 1.0;
   pObject->Transform.d3Scale[0]    = 1.0;
   pObject->Transform.d3Scale[1]    = 1.0;
   pObject->Transform.d3Scale[2]    = 1.0;

   auto Vec = [] (const nlohmann::json& a, double* pd, int n)
   {
      if (a.is_array ())
      {
         for (int i = 0; i < n  &&  i < static_cast<int> (a.size ()); i++)
            pd[i] = a[i].get<double> ();
      }
   };

   auto Str = [] (const nlohmann::json& v, char* pDst, size_t nMax)
   {
      if (v.is_string ())
      {
         std::string s = v.get<std::string> ();
         size_t nLen = s.size () < nMax - 1 ? s.size () : nMax - 1;
         memcpy (pDst, s.data (), nLen);
      }
   };

   if (j.contains ("Head"))
   {
      const auto& h = j["Head"];

      // Self accepts the human "class:index" id (preferred) or a raw composed
      // integer. Parent is never read (parentage comes from the node tree), so
      // it is ignored when absent.
      if (h.contains ("Self"))
      {
         if (h["Self"].is_string ())
            pObject->Head.Self.qwComposed = ComposeFromId (h["Self"].get<std::string> ());
         else
            pObject->Head.Self.qwComposed = h["Self"].get<uint64_t> ();
      }

      pObject->Head.qwEvent = h.value ("Event", static_cast<uint64_t> (0));
   }

   if (j.contains ("Name")  &&  j["Name"].is_string ())
   {
      std::string s = j["Name"].get<std::string> ();
      int i = 0;
      for (unsigned char c : s)
      {
         if (i >= 48)
            break;
         pObject->Name.wsName[i++] = static_cast<uint16_t> (c);
      }
   }

   if (j.contains ("Type"))
   {
      const auto& t = j["Type"];
      pObject->Type.bType    = static_cast<uint8_t> (t.value ("bType",    0));
      pObject->Type.bSubtype = static_cast<uint8_t> (t.value ("bSubtype", 0));
      pObject->Type.bFiction = static_cast<uint8_t> (t.value ("bFiction", 0));
   }

   pObject->Owner.twOwner = j.value ("Owner", static_cast<uint64_t> (0));

   if (j.contains ("Resource"))
   {
      const auto& r = j["Resource"];
      pObject->Resource.qwResource = r.value ("qwResource", static_cast<uint64_t> (0));
      if (r.contains ("sName"))      Str (r["sName"],      pObject->Resource.sName,      sizeof (pObject->Resource.sName));
      if (r.contains ("sReference")) Str (r["sReference"], pObject->Resource.sReference, sizeof (pObject->Resource.sReference));
   }

   if (j.contains ("Transform"))
   {
      const auto& tr = j["Transform"];
      if (tr.contains ("Position")) Vec (tr["Position"], pObject->Transform.d3Position, 3);
      if (tr.contains ("Rotation")) Vec (tr["Rotation"], pObject->Transform.d4Rotation, 4);
      if (tr.contains ("Scale"))    Vec (tr["Scale"],    pObject->Transform.d3Scale,    3);
   }

   if (j.contains ("Orbit"))
   {
      const auto& o = j["Orbit"];
      pObject->Orbit.tmPeriod = o.value ("tmPeriod", static_cast<int64_t> (0));
      pObject->Orbit.tmOrigin = o.value ("tmOrigin", static_cast<int64_t> (0));
      pObject->Orbit.dA       = o.value ("dA", 0.0);
      pObject->Orbit.dB       = o.value ("dB", 0.0);
   }

   if (j.contains ("Bound")  &&  j["Bound"].contains ("Max"))
      Vec (j["Bound"]["Max"], pObject->Bound.d3Max, 3);

   if (j.contains ("Properties"))
   {
      const auto& p = j["Properties"];
      pObject->Properties.fMass         = p.value ("fMass",         0.0f);
      pObject->Properties.fGravity      = p.value ("fGravity",      0.0f);
      pObject->Properties.fColor        = p.value ("fColor",        0.0f);
      pObject->Properties.fBrightness   = p.value ("fBrightness",   0.0f);
      pObject->Properties.fReflectivity = p.value ("fReflectivity", 0.0f);
   }
}

// ---------------------------------------------------------------------------
// Map_Open_Children — recursively opens each child of a JSON node under the
// already-created parent, returning the number of nodes created.
// ---------------------------------------------------------------------------

static uint32_t Map_Open_Children (CONTAINER* pContainer, uint64_t twParentIx, const nlohmann::json& jParent)
{
   uint32_t nCount = 0;

   if (jParent.contains ("Children")  &&  jParent["Children"].is_array ())
   {
      for (const auto& jChild : jParent["Children"])
      {
         RMCOBJECT RMCObject;
         RmcObject_FromJson (jChild, &RMCObject);

         uint64_t twChildIx = pContainer->Node_Open (twParentIx, &RMCObject);

         if (twChildIx != OBJECTIX_ERROR)
         {
            nCount += 1 + Map_Open_Children (pContainer, twChildIx, jChild);
         }
      }
   }

   return nCount;
}

wasm_trap_t* Scene_Node_Map (void* pEnv, wasmtime_caller_t* pCaller, const wasmtime_val_t* pArgs, size_t nArgs, wasmtime_val_t* pResults, size_t nResults)
{
   (void) pCaller;

   uint64_t twResult = OBJECTIX_ERROR;

   if (nArgs >= 1)
   {
      uint64_t twFabricIx = static_cast<uint64_t> (pArgs[0].of.i64);

      CONTAINER* pContainer = Container (pEnv);
      SCENE*     pScene     = pContainer ? pContainer->Context ()->Scene () : nullptr;
      FABRIC*    pFabric    = pScene     ? pScene->Fabric_Find (twFabricIx) : nullptr;
      MSF*       pMsf       = pFabric    ? pFabric->Msf ()                  : nullptr;

      if (pMsf)
      {
         nlohmann::json jPayload = pMsf->Payload ();

         if (jPayload.is_object ()  &&  jPayload.contains ("data")  &&  jPayload["data"].is_object ())
         {
            const nlohmann::json& jRoot = jPayload["data"];

            RMCOBJECT RMCObject;
            RmcObject_FromJson (jRoot, &RMCObject);

            uint64_t twRootIx = pContainer->Node_Root (twFabricIx, &RMCObject);

            if (twRootIx != OBJECTIX_ERROR)
            {
               uint32_t nCount = 1 + Map_Open_Children (pContainer, twRootIx, jRoot);

               pScene->Engine ()->Log (IENGINE::kLOGLEVEL_Info, "MAP", "Injected " + std::to_string (nCount) + " nodes from MSF data block");

               twResult = twRootIx;
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
            memcpy (&pObj->Properties.fColor, &nColor, 4);
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
            // Copy the wire object so we can force its class to PANEL regardless
            // of how the caller composed Head.Self.
            RMCOBJECT Object = *reinterpret_cast<const RMCOBJECT*> (pBytes);

            Object.Head.Self.qwComposed = OBJECTIX_COMPOSE (MAP_OBJECT::MAP_OBJECT_CLASS_PANEL, Object.Head.Self.ObjectIx ());

            twResult = pContainer->Node_Open (twParentIx, &Object);

            if (twResult != OBJECTIX_ERROR)
            {
               NODE*       pNode = pContainer->Node_Find (twResult);
               MAP_OBJECT* pObj  = pNode ? pNode->Map_Object () : nullptr;

               MAP_OBJECT_PANEL* pPanel = dynamic_cast<MAP_OBJECT_PANEL*> (pObj);

               if (pPanel)
               {
                  std::string sSource = ReadWasmString (pCaller, nSrcPtr, nSrcLen);

                  if (!sSource.empty ())
                     pPanel->Source (sSource);
               }
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
