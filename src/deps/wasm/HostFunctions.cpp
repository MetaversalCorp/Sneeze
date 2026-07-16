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

#include <sneeze_abi.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace SNEEZE
{
namespace DEP
{

// Key of the MSF payload block that holds the scene/node tree.
#define PAYLOAD_KEY_DATA                "Data"

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
// Container — recovers the CONTAINER* from the store pointer chain.
// pWasm_Store is a WASM_STORE* whose HostData() points to the owning CONTAINER.
// One store per container, so this is always the right container; the packet's
// twFabricIx selects which fabric within it.
// ---------------------------------------------------------------------------

static CONTAINER* Container (void* pWasm_Store)
{
   WASM_STORE* pStore = static_cast<WASM_STORE*> (pWasm_Store);

   CONTAINER* pContainer = nullptr;

   if (pStore)
   {
      pContainer = static_cast<CONTAINER*> (pStore->HostData ());
   }

   return pContainer;
}

static SCENE* Scene (void* pWasm_Store)
{
   CONTAINER* pContainer = Container (pWasm_Store);

   return pContainer ? pContainer->Context ()->Scene () : nullptr;
}

static STREAM* Stream (void* pWasm_Store)
{
   CONTAINER* pContainer = Container (pWasm_Store);

   return pContainer ? pContainer->Stream () : nullptr;
}

static SILO* Silo (void* pWasm_Store)
{
   CONTAINER* pContainer = Container (pWasm_Store);

   return pContainer ? pContainer->Silo () : nullptr;
}

// ---------------------------------------------------------------------------
// Payload field readers — pull little-endian scalars out of a packet payload,
// advancing the cursor. Host (x64) and guest (wasm32) are both little-endian,
// so a plain memcpy is the wire decode. Layout per method is documented in
// sdk/include/sneeze.h.
// ---------------------------------------------------------------------------

static uint64_t Payload_U64 (const uint8_t* pPayload, size_t& n)
{
   uint64_t v;
   memcpy (&v, pPayload + n, sizeof (v));
   n += sizeof (v);
   return v;
}

static int32_t Payload_I32 (const uint8_t* pPayload, size_t& n)
{
   int32_t v;
   memcpy (&v, pPayload + n, sizeof (v));
   n += sizeof (v);
   return v;
}

static double Payload_F64 (const uint8_t* pPayload, size_t& n)
{
   double v;
   memcpy (&v, pPayload + n, sizeof (v));
   n += sizeof (v);
   return v;
}

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

// ---------------------------------------------------------------------------
// CONSOLE dispatch — forwards to the container's STREAM.
// Payload: (u64 twFabricIx, then per method - see sneeze.h).
// ---------------------------------------------------------------------------

static int64_t Dispatch_Console (void* pWasm_Store, wasmtime_caller_t* pCaller, uint16_t wMethod, const uint8_t* pPayload)
{
   STREAM* pStream = Stream (pWasm_Store);

   if (pStream)
   {
      size_t   n          = 0;
      uint64_t twFabricIx = Payload_U64 (pPayload, n);   // reserved: per-fabric routing / permissions

      (void) twFabricIx;

      if (wMethod == kSNEEZE_ABI_METHOD_CONSOLE_GROUP_END)
      {
         pStream->GroupEnd ();
      }
      else if (wMethod == kSNEEZE_ABI_METHOD_CONSOLE_ASSERT)
      {
         int32_t     bCondition = Payload_I32 (pPayload, n);
         int32_t     nOffset    = Payload_I32 (pPayload, n);
         int32_t     nLen       = Payload_I32 (pPayload, n);
         std::string sMessage   = ReadWasmString (pCaller, nOffset, nLen);

         pStream->Assert (bCondition != 0, sMessage);
      }
      else
      {
         int32_t     nOffset  = Payload_I32 (pPayload, n);
         int32_t     nLen     = Payload_I32 (pPayload, n);
         std::string sMessage = ReadWasmString (pCaller, nOffset, nLen);

         switch (wMethod)
         {
            case kSNEEZE_ABI_METHOD_CONSOLE_LOG:             pStream->Log            (sMessage); break;
            case kSNEEZE_ABI_METHOD_CONSOLE_DEBUG:           pStream->Debug          (sMessage); break;
            case kSNEEZE_ABI_METHOD_CONSOLE_INFO:            pStream->Info           (sMessage); break;
            case kSNEEZE_ABI_METHOD_CONSOLE_WARN:            pStream->Warn           (sMessage); break;
            case kSNEEZE_ABI_METHOD_CONSOLE_ERROR:           pStream->Error          (sMessage); break;
            case kSNEEZE_ABI_METHOD_CONSOLE_GROUP:           pStream->Group          (sMessage); break;
            case kSNEEZE_ABI_METHOD_CONSOLE_GROUP_COLLAPSED: pStream->GroupCollapsed (sMessage); break;
            case kSNEEZE_ABI_METHOD_CONSOLE_COUNT:           pStream->Count          (sMessage); break;
            case kSNEEZE_ABI_METHOD_CONSOLE_COUNT_RESET:     pStream->CountReset     (sMessage); break;
            case kSNEEZE_ABI_METHOD_CONSOLE_TIME:            pStream->Time           (sMessage); break;
            case kSNEEZE_ABI_METHOD_CONSOLE_TIME_END:        pStream->TimeEnd        (sMessage); break;
            case kSNEEZE_ABI_METHOD_CONSOLE_TIME_LOG:        pStream->TimeLog        (sMessage); break;
            default:                                                                             break;
         }
      }
   }

   return 0;
}

// ---------------------------------------------------------------------------
// STORAGE dispatch — forwards to the container's SILO.
// Payload: (u64 twFabricIx, i32 eScope, i32 pathOffset, i32 pathLen, ...).
//
// An empty path ("") addresses the scope's root document: Get returns the whole
// document, Set replaces it, Remove clears it, Has reports the root. Get returns
// the full byte size of the value (min(return, outLen) written; outLen == 0
// queries size only; return > outLen means truncation).
// ---------------------------------------------------------------------------

static int64_t Dispatch_Storage (void* pWasm_Store, wasmtime_caller_t* pCaller, uint16_t wMethod, const uint8_t* pPayload)
{
   int64_t nResult = 0;

   SILO* pSilo = Silo (pWasm_Store);

   if (pSilo)
   {
      size_t   n          = 0;
      uint64_t twFabricIx = Payload_U64 (pPayload, n);   // reserved: per-fabric routing / permissions

      (void) twFabricIx;

      eSILO_SCOPE eScope     = static_cast<eSILO_SCOPE> (Payload_I32 (pPayload, n));
      int32_t     nPathOff   = Payload_I32 (pPayload, n);
      int32_t     nPathLen   = Payload_I32 (pPayload, n);
      std::string sPath      = ReadWasmString (pCaller, nPathOff, nPathLen);

      switch (wMethod)
      {
         case kSNEEZE_ABI_METHOD_STORAGE_HAS:
         {
            nResult = pSilo->Has (eScope, sPath) ? 1 : 0;
         } break;

         case kSNEEZE_ABI_METHOD_STORAGE_GET:
         {
            int32_t nOutOff = Payload_I32 (pPayload, n);
            int32_t nOutLen = Payload_I32 (pPayload, n);

            nlohmann::json jValue = pSilo->Get (eScope, sPath);
            std::string    sValue = jValue.is_null () ? std::string () : jValue.dump ();

            nResult = WriteWasmString (pCaller, nOutOff, nOutLen, sValue);
         } break;

         case kSNEEZE_ABI_METHOD_STORAGE_SET:
         {
            int32_t     nValOff = Payload_I32 (pPayload, n);
            int32_t     nValLen = Payload_I32 (pPayload, n);
            std::string sValue  = ReadWasmString (pCaller, nValOff, nValLen);

            nlohmann::json jValue = nlohmann::json::parse (sValue, nullptr, false);

            if (!jValue.is_discarded ())
            {
               pSilo->Set (eScope, sPath, jValue);
               nResult = 1;
            }
         } break;

         case kSNEEZE_ABI_METHOD_STORAGE_REMOVE:
         {
            pSilo->Remove (eScope, sPath);
            nResult = 1;
         } break;

         default:
            break;
      }
   }

   return nResult;
}

// ---------------------------------------------------------------------------
// DATA dispatch — read-only reads of the fabric's config "Data" tree, resolved
// from the fabric's MSF payload (the immutable analog of STORAGE, so no
// Set/Remove and no scope). Mirrors NODE_MAP's read of the same "Data" block.
// Payload: (u64 twFabricIx, i32 pathOffset, i32 pathLen, then per method).
//
// An empty path ("") addresses the whole data document. Get returns the value
// as JSON text (query outLen == 0 for size only; return > outLen means truncation).
// ---------------------------------------------------------------------------

static int64_t Dispatch_Data (void* pWasm_Store, wasmtime_caller_t* pCaller, uint16_t wMethod, const uint8_t* pPayload)
{
   int64_t nResult = 0;

   SCENE* pScene = Scene (pWasm_Store);

   if (pScene)
   {
      size_t   n          = 0;
      uint64_t twFabricIx = Payload_U64 (pPayload, n);
      int32_t  nPathOff   = Payload_I32 (pPayload, n);
      int32_t  nPathLen   = Payload_I32 (pPayload, n);
      std::string sPath   = ReadWasmString (pCaller, nPathOff, nPathLen);

      FABRIC* pFabric = pScene->Fabric_Find (twFabricIx);
      MSF*    pMsf    = pFabric ? pFabric->Msf () : nullptr;

      if (pMsf)
      {
         nlohmann::json        jPayload = pMsf->Payload ();
         const nlohmann::json* pNode    = nullptr;

         if (jPayload.is_object ()  &&  jPayload.contains (PAYLOAD_KEY_DATA)  &&  jPayload[PAYLOAD_KEY_DATA].is_object ())
            pNode = Data_Resolve (jPayload[PAYLOAD_KEY_DATA], sPath);

         switch (wMethod)
         {
            case kSNEEZE_ABI_METHOD_DATA_HAS:
            {
               nResult = (pNode  &&  !pNode->is_null ()) ? 1 : 0;
            } break;

            case kSNEEZE_ABI_METHOD_DATA_GET:
            {
               int32_t nOutOff = Payload_I32 (pPayload, n);
               int32_t nOutLen = Payload_I32 (pPayload, n);

               std::string sValue = (pNode  &&  !pNode->is_null ()) ? pNode->dump () : std::string ();

               nResult = WriteWasmString (pCaller, nOutOff, nOutLen, sValue);
            } break;

            default:
               break;
         }
      }
   }

   return nResult;
}

// ---------------------------------------------------------------------------
// SCENE dispatch — node-tree construction on the container.
//
//   NODE_ROOT  (u64 twFabricIx, i32 objOffset, i32 objLen)  -> twObjectIx
//   NODE_MAP   (u64 twFabricIx, i32 pathOffset, i32 pathLen)-> twRootIx
//   NODE_OPEN  (u64 twParentIx, i32 objOffset, i32 objLen)  -> twObjectIx
//   NODE_CLOSE (u64 twObjectIx)                             -> 0/1
//
// NODE_MAP reads a node tree out of the MSF "Data" block (a dot-separated path
// locating the tree; empty path = the "Data" object itself) and builds the whole
// fabric graph host-side. NODE_ROOT / NODE_OPEN read an RMCOBJECT from guest
// memory. Node creation is mutually exclusive with NODE_MAP per fabric.
// ---------------------------------------------------------------------------

static int64_t Dispatch_Scene (void* pWasm_Store, wasmtime_caller_t* pCaller, uint16_t wMethod, const uint8_t* pPayload)
{
   int64_t nResult = 0;

   CONTAINER* pContainer = Container (pWasm_Store);

   if (pContainer)
   {
      size_t n = 0;

      switch (wMethod)
      {
         case kSNEEZE_ABI_METHOD_SCENE_NODE_ROOT:
         {
            uint64_t twFabricIx = Payload_U64 (pPayload, n);
            int32_t  nObjOff    = Payload_I32 (pPayload, n);
            int32_t  nObjLen    = Payload_I32 (pPayload, n);
            uint64_t twResult   = OBJECTIX_ERROR;

            if (nObjLen >= static_cast<int32_t> (sizeof (RMCOBJECT)))
            {
               const uint8_t* pBytes = ReadWasmBytes (pCaller, nObjOff, nObjLen);

               if (pBytes)
                  twResult = pContainer->Node_Root (twFabricIx, reinterpret_cast<const RMCOBJECT*> (pBytes));
            }

            nResult = static_cast<int64_t> (twResult);
         } break;

         case kSNEEZE_ABI_METHOD_SCENE_NODE_MAP:
         {
            uint64_t    twFabricIx = Payload_U64 (pPayload, n);
            int32_t     nPathOff   = Payload_I32 (pPayload, n);
            int32_t     nPathLen   = Payload_I32 (pPayload, n);
            std::string sPath      = ReadWasmString (pCaller, nPathOff, nPathLen);
            uint64_t    twResult   = OBJECTIX_ERROR;

            FABRIC* pFabric = pContainer->Context ()->Scene ()->Fabric_Find (twFabricIx);
            MSF*    pMsf    = pFabric ? pFabric->Msf () : nullptr;

            if (pMsf)
            {
               nlohmann::json jPayload = pMsf->Payload ();

               if (jPayload.is_object ()  &&  jPayload.contains (PAYLOAD_KEY_DATA)  &&  jPayload[PAYLOAD_KEY_DATA].is_object ())
               {
                  const nlohmann::json* pRoot = Data_Resolve (jPayload[PAYLOAD_KEY_DATA], sPath);

                  if (pRoot)
                     twResult = pContainer->Branch_Add (twFabricIx, *pRoot);
               }
            }

            nResult = static_cast<int64_t> (twResult);
         } break;

         case kSNEEZE_ABI_METHOD_SCENE_NODE_OPEN:
         {
            int32_t  nObjOff    = Payload_I32 (pPayload, n);
            int32_t  nObjLen    = Payload_I32 (pPayload, n);
            uint64_t twResult   = OBJECTIX_ERROR;

            if (nObjLen >= static_cast<int32_t> (sizeof (RMCOBJECT)))
            {
               const uint8_t* pBytes = ReadWasmBytes (pCaller, nObjOff, nObjLen);

               if (pBytes)
                  twResult = pContainer->Node_Open (reinterpret_cast<const RMCOBJECT*> (pBytes));
            }

            nResult = static_cast<int64_t> (twResult);
         } break;

         case kSNEEZE_ABI_METHOD_SCENE_NODE_CLOSE:
         {
            uint64_t twObjectIx = Payload_U64 (pPayload, n);

            nResult = pContainer->Node_Close (twObjectIx) ? 1 : 0;
         } break;

         default:
            break;
      }
   }

   return nResult;
}

// ---------------------------------------------------------------------------
// NODE dispatch — property mutators on a live MAP_OBJECT.
// Payload: (u64 twObjectIx, then per method - see sneeze.h).
// ---------------------------------------------------------------------------

static int64_t Dispatch_Node (void* pWasm_Store, wasmtime_caller_t* pCaller, uint16_t wMethod, const uint8_t* pPayload)
{
   CONTAINER* pContainer = Container (pWasm_Store);

   if (pContainer)
   {
      size_t   n          = 0;
      uint64_t twObjectIx = Payload_U64 (pPayload, n);

      NODE*       pNode = pContainer->Node_Find (twObjectIx);
      MAP_OBJECT* pObj  = pNode ? pNode->Map_Object () : nullptr;

      if (pObj)
      {
         switch (wMethod)
         {
            case kSNEEZE_ABI_METHOD_NODE_POSITION:
            {
               pObj->Transform.d3Position[0] = Payload_F64 (pPayload, n);
               pObj->Transform.d3Position[1] = Payload_F64 (pPayload, n);
               pObj->Transform.d3Position[2] = Payload_F64 (pPayload, n);
            } break;

            case kSNEEZE_ABI_METHOD_NODE_SCALE:
            {
               double dScale = Payload_F64 (pPayload, n);

               pObj->Transform.d3Scale[0] = dScale;
               pObj->Transform.d3Scale[1] = dScale;
               pObj->Transform.d3Scale[2] = dScale;
            } break;

            case kSNEEZE_ABI_METHOD_NODE_SCALE_AXES:
            {
               pObj->Transform.d3Scale[0] = Payload_F64 (pPayload, n);
               pObj->Transform.d3Scale[1] = Payload_F64 (pPayload, n);
               pObj->Transform.d3Scale[2] = Payload_F64 (pPayload, n);
            } break;

            case kSNEEZE_ABI_METHOD_NODE_BOUND:
            {
               pObj->Bound.d3Max[0] = Payload_F64 (pPayload, n);
               pObj->Bound.d3Max[1] = Payload_F64 (pPayload, n);
               pObj->Bound.d3Max[2] = Payload_F64 (pPayload, n);
            } break;

            case kSNEEZE_ABI_METHOD_NODE_NAME:
            {
               int32_t     nOffset = Payload_I32 (pPayload, n);
               int32_t     nLen    = Payload_I32 (pPayload, n);
               std::string sName   = ReadWasmString (pCaller, nOffset, nLen);

               memset (&pObj->Name, 0, sizeof (MAP_OBJECT::MAP_OBJECT_NAME));
               size_t nCount = std::min<size_t> (sName.size (), 47);

               for (size_t i = 0; i < nCount; i++)
                  pObj->Name.wsName[i] = static_cast<uint16_t> (static_cast<uint8_t> (sName[i]));
            } break;

            case kSNEEZE_ABI_METHOD_NODE_RESOURCE:
            {
               int32_t     nOffset = Payload_I32 (pPayload, n);
               int32_t     nLen    = Payload_I32 (pPayload, n);
               std::string sUrl    = ReadWasmString (pCaller, nOffset, nLen);

               strncpy (pObj->Resource.sReference, sUrl.c_str (), sizeof (pObj->Resource.sReference) - 1);
               pObj->Resource.sReference[sizeof (pObj->Resource.sReference) - 1] = '\0';
            } break;

            case kSNEEZE_ABI_METHOD_NODE_PANEL:
            {
               int32_t     nOffset = Payload_I32 (pPayload, n);
               int32_t     nLen    = Payload_I32 (pPayload, n);
               std::string sRml    = ReadWasmString (pCaller, nOffset, nLen);

               MAP_OBJECT_PANEL* pPanel = dynamic_cast<MAP_OBJECT_PANEL*> (pObj);

               if (pPanel  &&  !sRml.empty ())
                  pPanel->Source (sRml);
            } break;

            default:
               break;
         }
      }
   }

   return 0;
}

// ---------------------------------------------------------------------------
// Call — the single guest -> host entry point (import module "Sneeze").
//
// Reads the 8-byte SNEEZE_ABI_PACKET_HEADER at (offset, size), then the payload,
// routes on (wType, wMethod), and returns the subsystem's i64 result. Unknown
// or not-yet-implemented (wType, wMethod) pairs (NETWORK, VIEWPORT, and the
// SCENE/NODE host-new slots) fall through to a 0 result.
// ---------------------------------------------------------------------------

wasm_trap_t* Call (void* pWasm_Store, wasmtime_caller_t* pCaller, const wasmtime_val_t* pArgs, size_t nArgs, wasmtime_val_t* pResults, size_t nResults)
{
   int64_t nResult = 0;

   if (nArgs >= 2)
   {
      int32_t nPacketOffset = pArgs[0].of.i32;
      int32_t nPacketSize   = pArgs[1].of.i32;

      if (nPacketSize >= static_cast<int32_t> (sizeof (SNEEZE_ABI_PACKET_HEADER)))
      {
         const uint8_t* pHeader = ReadWasmBytes (pCaller, nPacketOffset, static_cast<int32_t> (sizeof (SNEEZE_ABI_PACKET_HEADER)));

         if (pHeader)
         {
            uint16_t wType;
            uint16_t wMethod;
            uint32_t dwSize;

            memcpy (&wType,   pHeader + 0, sizeof (wType));
            memcpy (&wMethod, pHeader + 2, sizeof (wMethod));
            memcpy (&dwSize,  pHeader + 4, sizeof (dwSize));

            const uint8_t* pPayload = (dwSize > 0)
               ? ReadWasmBytes (pCaller, nPacketOffset + static_cast<int32_t> (sizeof (SNEEZE_ABI_PACKET_HEADER)), static_cast<int32_t> (dwSize))
               : pHeader;

            if (pPayload)
            {
               switch (wType)
               {
                  case kSNEEZE_ABI_TYPE_DATA:     nResult = Dispatch_Data    (pWasm_Store, pCaller, wMethod, pPayload); break;
                  case kSNEEZE_ABI_TYPE_CONSOLE:  nResult = Dispatch_Console (pWasm_Store, pCaller, wMethod, pPayload); break;
                  case kSNEEZE_ABI_TYPE_STORAGE:  nResult = Dispatch_Storage (pWasm_Store, pCaller, wMethod, pPayload); break;
                  case kSNEEZE_ABI_TYPE_SCENE:    nResult = Dispatch_Scene   (pWasm_Store, pCaller, wMethod, pPayload); break;
                  case kSNEEZE_ABI_TYPE_NODE:     nResult = Dispatch_Node    (pWasm_Store, pCaller, wMethod, pPayload); break;
                  default:                                                                                              break;
               }
            }
         }
      }
   }

   if (nResults > 0)
   {
      pResults[0].kind   = WASMTIME_I64;
      pResults[0].of.i64 = nResult;
   }

   return nullptr;
}

} // namespace DEP
} // namespace SNEEZE
