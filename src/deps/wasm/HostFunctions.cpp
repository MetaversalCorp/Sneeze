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
#include "Chrono.h"

#include <sneeze_abi.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace SNEEZE
{
namespace DEP
{

   typedef struct tagOBJECT_HEADX
   {
      uint64_t                                                 qwComposed_Parent;
      uint64_t                                                 qwComposed_Self;
      uint64_t                                                 qwEvent;
   }
   OBJECT_HEADX;

   typedef struct tagMAP_OBJECT_PODX
   {
      OBJECT_HEADX                                             Object_Head;
      RMAP::MAP::MAP_OBJECT_POD                                Map_Object_Pod;
   }
   MAP_OBJECT_PODX;

// Key of the MSF payload block that holds the scene/node tree.
#define PAYLOAD_KEY_DATA                "Data"

// Key of the MSF payload block that holds the declared services (a name-keyed
// object; each value an arbitrary service-config object).
#define PAYLOAD_KEY_SERVICES            "Services"

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

      if (static_cast<size_t> (nPtr) + static_cast<size_t> (nLen) <= nMemSize)
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

      if (static_cast<size_t> (nPtr) + static_cast<size_t> (nLen) <= nMemSize)
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

         if (static_cast<size_t> (nPtr) + static_cast<size_t> (nLen) <= nMemSize)
         {
            int32_t nWritten = (nNeeded < nLen) ? nNeeded : nLen;

            memcpy (pData + nPtr, sValue.data (), static_cast<size_t> (nWritten));
         }
      }
   }

   return nNeeded;
}

// ---------------------------------------------------------------------------
// WriteWasmBytes — writes a raw struct into the caller's linear memory.
//
// Mirrors WriteWasmString for host -> guest binary payloads (a filled MOMENT):
// returns the full byte size of the source, writing min(nSize, nLen) bytes. A
// query call (nLen == 0) returns the size without writing.
// ---------------------------------------------------------------------------

static int32_t WriteWasmBytes (wasmtime_caller_t* pCaller, int32_t nPtr, int32_t nLen, const void* pSrc, int32_t nSize)
{
   if (nPtr >= 0  &&  nLen > 0  &&  nSize > 0)
   {
      wasmtime_extern_t ext;
      bool bFound = wasmtime_caller_export_get (pCaller, "memory", 6, &ext);

      if (bFound  &&  ext.kind == WASMTIME_EXTERN_MEMORY)
      {
         wasmtime_context_t* pCtx = wasmtime_caller_context (pCaller);
         uint8_t* pData = wasmtime_memory_data (pCtx, &ext.of.memory);
         size_t nMemSize = wasmtime_memory_data_size (pCtx, &ext.of.memory);

         if (static_cast<size_t> (nPtr) + static_cast<size_t> (nLen) <= nMemSize)
         {
            int32_t nWritten = (nSize < nLen) ? nSize : nLen;

            memcpy (pData + nPtr, pSrc, static_cast<size_t> (nWritten));
         }
      }
   }

   return nSize;
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
// PAYLOAD — a bounds-checked cursor over one packet's payload block. Every read
// verifies the field fits within the declared payload size; a short read trips
// m_bOk (and yields 0) and poisons every read that follows. Host (x64) and
// guest (wasm32) are both little-endian, so a plain memcpy is the wire decode.
// Layout per method is documented in sneeze_abi.h.
//
// A dispatcher must gate the operation on Exact(): the payload is only acted on
// when no read overran AND the cursor consumed exactly the whole payload — no
// under- or over-sized packet is ever executed. A zero-length payload (dwSize
// == 0) therefore fails the very first read and is rejected.
// ---------------------------------------------------------------------------

class PAYLOAD
{
   public:
      PAYLOAD (const uint8_t* pData, size_t nSize) : m_pData (pData), m_nSize (nSize), m_n (0), m_bOk (true)
      {
      }

      uint64_t U64 ()
      {
         uint64_t v = 0;
         Read (&v, sizeof (v));
         return v;
      }

      int32_t I32 ()
      {
         int32_t v = 0;
         Read (&v, sizeof (v));
         return v;
      }

      double F64 ()
      {
         double v = 0.0;
         Read (&v, sizeof (v));
         return v;
      }

      bool Exact () const
      {
         return m_bOk  &&  m_n == m_nSize;
      }

   private:
      void Read (void* pOut, size_t nBytes)
      {
         if (m_bOk  &&  m_n + nBytes <= m_nSize)
         {
            memcpy (pOut, m_pData + m_n, nBytes);
            m_n += nBytes;
         }
         else
         {
            m_bOk = false;
         }
      }

      const uint8_t* m_pData;
      size_t         m_nSize;
      size_t         m_n;
      bool           m_bOk;
};

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

static int64_t Dispatch_Console (void* pWasm_Store, wasmtime_caller_t* pCaller, uint16_t wMethod, PAYLOAD payload)
{
   STREAM* pStream = Stream (pWasm_Store);

   if (pStream)
   {
      uint64_t twFabricIx = payload.U64 ();   // reserved: per-fabric routing / permissions

      (void) twFabricIx;

      if (wMethod == kSNEEZE_ABI_METHOD_CONSOLE_GROUP_END)
      {
         if (payload.Exact ())
            pStream->GroupEnd ();
      }
      else if (wMethod == kSNEEZE_ABI_METHOD_CONSOLE_ASSERT)
      {
         int32_t bCondition = payload.I32 ();
         int32_t nOffset    = payload.I32 ();
         int32_t nLen       = payload.I32 ();

         if (payload.Exact ())
         {
            std::string sMessage = ReadWasmString (pCaller, nOffset, nLen);

            pStream->Assert (bCondition != 0, sMessage);
         }
      }
      else
      {
         int32_t nOffset = payload.I32 ();
         int32_t nLen    = payload.I32 ();

         if (payload.Exact ())
         {
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

static int64_t Dispatch_Storage (void* pWasm_Store, wasmtime_caller_t* pCaller, uint16_t wMethod, PAYLOAD payload)
{
   int64_t nResult = 0;

   SILO* pSilo = Silo (pWasm_Store);

   if (pSilo)
   {
      uint64_t twFabricIx = payload.U64 ();   // reserved: per-fabric routing / permissions

      (void) twFabricIx;

      eSILO_SCOPE eScope   = static_cast<eSILO_SCOPE> (payload.I32 ());
      int32_t     nPathOff = payload.I32 ();
      int32_t     nPathLen = payload.I32 ();

      switch (wMethod)
      {
         case kSNEEZE_ABI_METHOD_STORAGE_HAS:
         {
            if (payload.Exact ())
            {
               std::string sPath = ReadWasmString (pCaller, nPathOff, nPathLen);

               nResult = pSilo->Has (eScope, sPath) ? 1 : 0;
            }
         } break;

         case kSNEEZE_ABI_METHOD_STORAGE_GET:
         {
            int32_t nOutOff = payload.I32 ();
            int32_t nOutLen = payload.I32 ();

            if (payload.Exact ())
            {
               std::string    sPath  = ReadWasmString (pCaller, nPathOff, nPathLen);
               nlohmann::json jValue = pSilo->Get (eScope, sPath);
               std::string    sValue = jValue.is_null () ? std::string () : jValue.dump ();

               nResult = WriteWasmString (pCaller, nOutOff, nOutLen, sValue);
            }
         } break;

         case kSNEEZE_ABI_METHOD_STORAGE_SET:
         {
            int32_t nValOff = payload.I32 ();
            int32_t nValLen = payload.I32 ();

            if (payload.Exact ())
            {
               std::string    sPath  = ReadWasmString (pCaller, nPathOff, nPathLen);
               std::string    sValue = ReadWasmString (pCaller, nValOff, nValLen);
               nlohmann::json jValue = nlohmann::json::parse (sValue, nullptr, false);

               if (!jValue.is_discarded ())
               {
                  pSilo->Set (eScope, sPath, jValue);
                  nResult = 1;
               }
            }
         } break;

         case kSNEEZE_ABI_METHOD_STORAGE_REMOVE:
         {
            if (payload.Exact ())
            {
               std::string sPath = ReadWasmString (pCaller, nPathOff, nPathLen);

               pSilo->Remove (eScope, sPath);
               nResult = 1;
            }
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

static int64_t Dispatch_Data (void* pWasm_Store, wasmtime_caller_t* pCaller, uint16_t wMethod, PAYLOAD payload)
{
   int64_t nResult = 0;

   SCENE* pScene = Scene (pWasm_Store);

   if (pScene)
   {
      uint64_t twFabricIx = payload.U64 ();
      int32_t  nPathOff   = payload.I32 ();
      int32_t  nPathLen   = payload.I32 ();
      int32_t  nOutOff    = 0;
      int32_t  nOutLen    = 0;

      if (wMethod == kSNEEZE_ABI_METHOD_DATA_GET)
      {
         nOutOff = payload.I32 ();
         nOutLen = payload.I32 ();
      }

      if (payload.Exact ())
      {
         std::string sPath   = ReadWasmString (pCaller, nPathOff, nPathLen);
         FABRIC*     pFabric = pScene->Fabric_Find (twFabricIx);
         MSF*        pMsf    = pFabric ? pFabric->Msf () : nullptr;

         if (pMsf)
         {
            const nlohmann::json& jPayload = pMsf->Payload ();
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
                  std::string sValue = (pNode  &&  !pNode->is_null ()) ? pNode->dump () : std::string ();

                  nResult = WriteWasmString (pCaller, nOutOff, nOutLen, sValue);
               } break;

               default:
                  break;
            }
         }
      }
   }

   return nResult;
}

// ---------------------------------------------------------------------------
// SERVICES dispatch — read-only reads of the fabric's declared services, resolved
// from the fabric's MSF payload keyed by service name. The immutable analog of
// STORAGE, and the sibling of DATA: DATA resolves a dotted path within the
// "Data" tree, SERVICES resolves one service by name within the "Services" object.
// A service object may carry any fields the author chose, so Get returns the
// whole service object as JSON text.
// Payload: (u64 twFabricIx, i32 nameOffset, i32 nameLen, then per method).
//
//   HAS (…)                       -> 0/1
//   GET (…, i32 outOffset, i32 outLen) -> byte size (query outLen == 0
//                                                for size; return > outLen == truncation)
// ---------------------------------------------------------------------------

static int64_t Dispatch_Services (void* pWasm_Store, wasmtime_caller_t* pCaller, uint16_t wMethod, PAYLOAD payload)
{
   int64_t nResult = 0;

   SCENE* pScene = Scene (pWasm_Store);

   if (pScene)
   {
      uint64_t twFabricIx = payload.U64 ();
      int32_t  nNameOff   = payload.I32 ();
      int32_t  nNameLen   = payload.I32 ();
      int32_t  nOutOff    = 0;
      int32_t  nOutLen    = 0;

      if (wMethod == kSNEEZE_ABI_METHOD_SERVICES_GET)
      {
         nOutOff = payload.I32 ();
         nOutLen = payload.I32 ();
      }

      if (payload.Exact ())
      {
         std::string sName   = ReadWasmString (pCaller, nNameOff, nNameLen);
         FABRIC*     pFabric = pScene->Fabric_Find (twFabricIx);
         MSF*        pMsf    = pFabric ? pFabric->Msf () : nullptr;

         if (pMsf)
         {
            const nlohmann::json& jPayload = pMsf->Payload ();
            const nlohmann::json* pService = nullptr;

            if (jPayload.is_object ()  &&  jPayload.contains (PAYLOAD_KEY_SERVICES)  &&  jPayload[PAYLOAD_KEY_SERVICES].is_object ()  &&  jPayload[PAYLOAD_KEY_SERVICES].contains (sName))
               pService = &jPayload[PAYLOAD_KEY_SERVICES][sName];

            switch (wMethod)
            {
               case kSNEEZE_ABI_METHOD_SERVICES_HAS:
               {
                  nResult = (pService  &&  !pService->is_null ()) ? 1 : 0;
               } break;

               case kSNEEZE_ABI_METHOD_SERVICES_GET:
               {
                  std::string sValue = (pService  &&  !pService->is_null ()) ? pService->dump () : std::string ();

                  nResult = WriteWasmString (pCaller, nOutOff, nOutLen, sValue);
               } break;

               default:
                  break;
            }
         }
      }
   }

   return nResult;
}

static int64_t Dispatch_Fabric (void* pWasm_Store, wasmtime_caller_t* pCaller, uint16_t wMethod, PAYLOAD payload);

// ---------------------------------------------------------------------------
// SCENE dispatch — the four legacy node methods forward to the
// FABRIC subsystem, which now owns node-tree construction. Retained only so
// already-deployed modules keep working until they are migrated. Each call
// warns on the module's developer (inspector) console — not the host log — so
// the author sees the deprecation. Stage 4 will remove this forwarding and
// repurpose the SCENE enum for scene globals.
// ---------------------------------------------------------------------------

static int64_t Dispatch_Scene (void* pWasm_Store, wasmtime_caller_t* pCaller, uint16_t wMethod, PAYLOAD payload)
{
   uint16_t    wFabric = 0;
   const char* sOld    = nullptr;
   const char* sNew    = nullptr;

   switch (wMethod)
   {
      case kSNEEZE_ABI_METHOD_SCENE_NODE_ROOT:      wFabric = kSNEEZE_ABI_METHOD_FABRIC_NODE_ROOT;      sOld = "Scene.Node_Root";     sNew = "Fabric.Node_Root";     break;
      case kSNEEZE_ABI_METHOD_SCENE_NODE_MAP_DATA:  wFabric = kSNEEZE_ABI_METHOD_FABRIC_NODE_MAP_DATA;  sOld = "Scene.Node_Map_Data"; sNew = "Fabric.Node_Map_Data"; break;
      case kSNEEZE_ABI_METHOD_SCENE_NODE_OPEN:      wFabric = kSNEEZE_ABI_METHOD_FABRIC_NODE_OPEN;      sOld = "Scene.Node_Open";     sNew = "Fabric.Node_Open";     break;
      case kSNEEZE_ABI_METHOD_SCENE_NODE_CLOSE:     wFabric = kSNEEZE_ABI_METHOD_FABRIC_NODE_CLOSE;     sOld = "Scene.Node_Close";    sNew = "Fabric.Node_Close";    break;
/*
      case kSNEEZE_ABI_METHOD_SCENE_AMBIENT_GET:
      case kSNEEZE_ABI_METHOD_SCENE_AMBIENT_SET:
      case kSNEEZE_ABI_METHOD_SCENE_DIRECTIONAL_GET:
      case kSNEEZE_ABI_METHOD_SCENE_DIRECTIONAL_SET:
      case kSNEEZE_ABI_METHOD_SCENE_BACKGROUND_GET:
      case kSNEEZE_ABI_METHOD_SCENE_BACKGROUND_SET:
*/
      default:                                      break;
   }

   if (wFabric)
   {
      STREAM* pStream = Stream (pWasm_Store);

      if (pStream)
         pStream->Warn (std::string (sOld) + " is deprecated and will be removed; use " + sNew + " instead.");
   }

   return wFabric ? Dispatch_Fabric (pWasm_Store, pCaller, wFabric, payload) : 0;
}

// ---------------------------------------------------------------------------
// Map-service helpers — used by FABRIC's NODE_MAP_SERVICE / NODE_MAP_SERVICE_EX methods.
// Field_Set copies a std::string into a fixed NUL-padded ABI char[N] field,
// truncating and always leaving a terminator. Map_Service_From_Json fills a
// SNEEZE_ABI_MAP_SERVICE from a service-config JSON object (the EX path, where
// the host reads the service itself). Map_Service_Connect is the single landing
// point for a resolved map-service connection — for now it only records that
// the request arrived, logging every field plus the container's organization
// fingerprint; the actual RMAP connect/stream is a later job.
// ---------------------------------------------------------------------------

static void Field_Set (char* aField, size_t nCap, const std::string& sValue)
{
   size_t nCopy = (sValue.size () < nCap - 1) ? sValue.size () : nCap - 1;

   memset (aField, 0, nCap);
   memcpy (aField, sValue.data (), nCopy);
}

static void Map_Service_From_Json (const nlohmann::json& jService, SNEEZE_ABI_MAP_SERVICE& svc)
{
   memset (&svc, 0, sizeof (svc));

   if (jService.is_object ())
   {
      if (jService.contains ("sNamespace")  &&  jService["sNamespace"].is_string  ())   Field_Set (svc.sNamespace, sizeof (svc.sNamespace), jService["sNamespace"].get<std::string> ());
      if (jService.contains ("sService"  )  &&  jService["sService"  ].is_string  ())   Field_Set (svc.sService,   sizeof (svc.sService),   jService["sService"  ].get<std::string> ());
      if (jService.contains ("sConnect"  )  &&  jService["sConnect"  ].is_string  ())   Field_Set (svc.sConnect,   sizeof (svc.sConnect),   jService["sConnect"  ].get<std::string> ());
      if (jService.contains ("sRootUrl"  )  &&  jService["sRootUrl"  ].is_string  ())   Field_Set (svc.sRootUrl,   sizeof (svc.sRootUrl),   jService["sRootUrl"  ].get<std::string> ());
      if (jService.contains ("bAuth"     )  &&  jService["bAuth"     ].is_boolean ())   svc.bAuth      = jService["bAuth"].get<bool> () ? 1 : 0;
      if (jService.contains ("wClass"    )  &&  jService["wClass"    ].is_number  ())   svc.wClass     = static_cast<uint16_t> (jService["wClass"].get<uint64_t> ());
      if (jService.contains ("twObjectIx")  &&  jService["twObjectIx"].is_number  ())   svc.twObjectIx = jService["twObjectIx"].get<uint64_t> ();
   }
}

static void Map_Service_Connect (CONTAINER* pContainer, uint64_t twFabricIx, const SNEEZE_ABI_MAP_SERVICE& svc, FABRIC* pFabric)
{
   const CONTAINER::CID* pCID         = pContainer->Identity ();
   std::string           sFingerprint = pCID ? pCID->sFingerprint : std::string ();

   std::string sNamespace (svc.sNamespace, strnlen (svc.sNamespace, sizeof (svc.sNamespace)));
   std::string sService   (svc.sService,   strnlen (svc.sService,   sizeof (svc.sService)));
   std::string sConnect   (svc.sConnect,   strnlen (svc.sConnect,   sizeof (svc.sConnect)));
   std::string sRootUrl   (svc.sRootUrl,   strnlen (svc.sRootUrl,   sizeof (svc.sRootUrl)));

   std::string sMessage =
      "Map service connect (fabric " + std::to_string (twFabricIx) + "):"
      + " namespace="   + sNamespace
      + " service="     + sService
      + " connect="     + sConnect
      + " rootUrl="     + sRootUrl
      + " auth="        + (svc.bAuth ? std::string ("true") : std::string ("false"))
      + " class="       + std::to_string (svc.wClass)
      + " objectIx="    + std::to_string (svc.twObjectIx)
      + " fingerprint=" + sFingerprint;

   pContainer->Context ()->Engine ()->Log (IENGINE::kLOGLEVEL_Info, "FABRIC", sMessage);

   pContainer->CreateMapSvc (twFabricIx, sNamespace, sService, sConnect, svc.wClass, svc.twObjectIx);
}

// ---------------------------------------------------------------------------
// FABRIC dispatch — node-tree construction on the container.
//
//   NODE_MAP_SERVICE    (u64 twFabricIx, i32 svcOffset, i32 svcLen)  -> 0/1
//   NODE_MAP_SERVICE_EX (u64 twFabricIx, i32 nameOffset, i32 nameLen)-> 0/1
//   NODE_MAP_DATA       (u64 twFabricIx, i32 pathOffset, i32 pathLen)-> twRootIx
//   NODE_ROOT           (u64 twFabricIx, i32 objOffset, i32 objLen)  -> twObjectIx
//   NODE_OPEN           (i32 objOffset, i32 objLen)                  -> twObjectIx
//   NODE_CLOSE          (u64 twObjectIx)                             -> 0/1
//
// NODE_MAP_SERVICE reads a guest-filled SNEEZE_ABI_MAP_SERVICE from guest memory;
// NODE_MAP_SERVICE_EX names a service in the MSF "Services" object and lets the
// host fill the struct from it. Both land at Map_Service_Connect (see above).
// The two map-service calls and NODE_MAP_DATA are each mutually exclusive with
// building the tree by hand (NODE_ROOT / NODE_OPEN / NODE_CLOSE): once a fabric
// is handed to the browser to map, the guest may no longer edit its nodes.
// NODE_MAP_DATA reads a node tree out of the MSF "Data" block (a dot-separated
// path locating the tree; empty path = the "Data" object itself) and builds the
// whole fabric graph host-side. NODE_ROOT / NODE_OPEN read an RMCOBJECT from
// guest memory (NODE_OPEN carries its parent inside that object, not in the
// payload). Every payload is size-validated before it is acted on.
//
// The deprecated SCENE node methods (type 6) forward here (see Dispatch_Scene).
// ---------------------------------------------------------------------------

static int64_t Dispatch_Fabric (void* pWasm_Store, wasmtime_caller_t* pCaller, uint16_t wMethod, PAYLOAD payload)
{
   int64_t nResult = 0;

   CONTAINER* pContainer = Container (pWasm_Store);
   SCENE*     pScene     = Scene (pWasm_Store);

   if (pContainer  &&  pScene)
   {
      switch (wMethod)
      {
         case kSNEEZE_ABI_METHOD_FABRIC_NODE_MAP_SERVICE:
         {
            uint64_t twFabricIx = payload.U64 ();
            int32_t  nSvcOff    = payload.I32 ();
            int32_t  nSvcLen    = payload.I32 ();

            if (payload.Exact ())
            {
               FABRIC* pFabric = pScene->Fabric_Find (twFabricIx);

               if (pFabric  &&  nSvcLen == static_cast<int32_t> (sizeof (SNEEZE_ABI_MAP_SERVICE)))
               {
                  const uint8_t* pBytes = ReadWasmBytes (pCaller, nSvcOff, nSvcLen);

                  if (pBytes)
                  {
                     SNEEZE_ABI_MAP_SERVICE svc;
                     memcpy (&svc, pBytes, sizeof (svc));
                     Map_Service_Connect (pContainer, twFabricIx, svc, pFabric);
                     nResult = 1;
                  }
               }
            }
         } break;

         case kSNEEZE_ABI_METHOD_FABRIC_NODE_MAP_SERVICE_EX:
         {
            uint64_t twFabricIx = payload.U64 ();
            int32_t  nNameOff   = payload.I32 ();
            int32_t  nNameLen   = payload.I32 ();

            if (payload.Exact ())
            {
               std::string sName   = ReadWasmString (pCaller, nNameOff, nNameLen);
               FABRIC*     pFabric = pScene->Fabric_Find (twFabricIx);
               MSF*        pMsf    = pFabric ? pFabric->Msf () : nullptr;

               if (pMsf)
               {
                  const nlohmann::json& jPayload = pMsf->Payload ();

                  if (jPayload.is_object ()  &&  jPayload.contains (PAYLOAD_KEY_SERVICES)  &&  jPayload[PAYLOAD_KEY_SERVICES].is_object ()  &&  jPayload[PAYLOAD_KEY_SERVICES].contains (sName))
                  {
                     SNEEZE_ABI_MAP_SERVICE svc;
                     Map_Service_From_Json (jPayload[PAYLOAD_KEY_SERVICES][sName], svc);
                     Map_Service_Connect (pContainer, twFabricIx, svc, pFabric);
                     nResult = 1;
                  }
               }
            }
         } break;

         case kSNEEZE_ABI_METHOD_FABRIC_NODE_MAP_DATA:
         {
            uint64_t twFabricIx = payload.U64 ();
            int32_t  nPathOff   = payload.I32 ();
            int32_t  nPathLen   = payload.I32 ();
            uint64_t twResult   = OBJECTIX_ERROR;

            if (payload.Exact ())
            {
               std::string sPath   = ReadWasmString (pCaller, nPathOff, nPathLen);
               FABRIC*     pFabric = pScene->Fabric_Find (twFabricIx);
               MSF*        pMsf    = pFabric ? pFabric->Msf () : nullptr;

               if (pMsf)
               {
                  const nlohmann::json& jPayload = pMsf->Payload ();

                  if (jPayload.is_object ()  &&  jPayload.contains (PAYLOAD_KEY_DATA)  &&  jPayload[PAYLOAD_KEY_DATA].is_object ())
                  {
                     const nlohmann::json* pRoot = Data_Resolve (jPayload[PAYLOAD_KEY_DATA], sPath);

                     if (pRoot)
                        twResult = pContainer->Branch_Add (twFabricIx, *pRoot);
                  }
               }
            }

            nResult = static_cast<int64_t> (twResult);
         } break;

         case kSNEEZE_ABI_METHOD_FABRIC_NODE_ROOT:
         {
            uint64_t twFabricIx = payload.U64 ();
            int32_t  nObjOff    = payload.I32 ();
            int32_t  nObjLen    = payload.I32 ();
            uint64_t twResult   = OBJECTIX_ERROR;

            if (payload.Exact ()  &&  nObjLen == static_cast<int32_t> (sizeof (MAP_OBJECT_PODX) ))
            {
               const uint8_t* pBytes = ReadWasmBytes (pCaller, nObjOff, nObjLen);

               if (pBytes)
               {
                  const MAP_OBJECT_PODX* pPod = reinterpret_cast<const MAP_OBJECT_PODX*> (pBytes);
                  RMAP::CORE::MEM::OBJECTIX ObjectIx;

                  ObjectIx.qwComposed = pPod->Object_Head.qwComposed_Self;

                  RMAP::MAP::MAP_OBJECT* pMap_Object = RMAP::MAP::MAP_OBJECT::Create (ObjectIx.Class (), ObjectIx.ObjectIx (), pPod->Map_Object_Pod);
                  twResult = pContainer->Node_Root (twFabricIx, pMap_Object);
                  // delete pMap_Object; FREED by Container
               }
            }

            nResult = static_cast<int64_t> (twResult);
         } break;

         case kSNEEZE_ABI_METHOD_FABRIC_NODE_OPEN:
         {
            int32_t  nObjOff  = payload.I32 ();
            int32_t  nObjLen  = payload.I32 ();
            uint64_t twResult = OBJECTIX_ERROR;

            if (payload.Exact ()  &&  nObjLen == static_cast<int32_t> (sizeof (MAP_OBJECT_PODX)))
            {
               const uint8_t* pBytes = ReadWasmBytes (pCaller, nObjOff, nObjLen);

               if (pBytes)
               {
                  const MAP_OBJECT_PODX* pPod = reinterpret_cast<const MAP_OBJECT_PODX*> (pBytes);
                  RMAP::CORE::MEM::OBJECTIX ObjectIx;

                  ObjectIx.qwComposed = pPod->Object_Head.qwComposed_Self;

                  RMAP::MAP::MAP_OBJECT* pMap_Object = RMAP::MAP::MAP_OBJECT::Create (ObjectIx.Class (), ObjectIx.ObjectIx (), pPod->Map_Object_Pod);
                  twResult = pContainer->Node_Open (pPod->Object_Head.qwComposed_Parent, pMap_Object);
                  // delete pMap_Object; Freed by Container
               }
            }

            nResult = static_cast<int64_t> (twResult);
         } break;

         case kSNEEZE_ABI_METHOD_FABRIC_NODE_CLOSE:
         {
            uint64_t twObjectIx = payload.U64 ();

            if (payload.Exact ())
               nResult = pContainer->Node_Close (twObjectIx) ? 1 : 0;
         } break;

         default:
            break;
      }
   }

   return nResult;
}

// ---------------------------------------------------------------------------
// NODE dispatch — property mutators on a live MAP_OBJECT. Each mutator reads
// its fields, then applies them only when the payload was exactly the size the
// method expects (see PAYLOAD::Exact).
// Payload: (u64 twObjectIx, then per method - see sneeze_abi.h).
// ---------------------------------------------------------------------------

static int64_t Dispatch_Node (void* pWasm_Store, wasmtime_caller_t* pCaller, uint16_t wMethod, PAYLOAD payload)
{
   CONTAINER* pContainer = Container (pWasm_Store);

   if (pContainer)
   {
      uint64_t twObjectIx = payload.U64 ();

      NODE*       pNode = pContainer->Node_Find (twObjectIx);
      RMAP::MAP::MAP_OBJECT* pMap_Object  = pNode ? pNode->Map_Object () : nullptr;

      if (pMap_Object)
      {
         switch (wMethod)
         {
            case kSNEEZE_ABI_METHOD_NODE_POSITION:
            {
               double dX = payload.F64 ();
               double dY = payload.F64 ();
               double dZ = payload.F64 ();

               if (payload.Exact ())
               {
                  pMap_Object->m_POD.Transform.d3Position[0] = dX;
                  pMap_Object->m_POD.Transform.d3Position[1] = dY;
                  pMap_Object->m_POD.Transform.d3Position[2] = dZ;
               }
            } break;

            case kSNEEZE_ABI_METHOD_NODE_ROTATION:
            {
               double dX = payload.F64 ();
               double dY = payload.F64 ();
               double dZ = payload.F64 ();
               double dW = payload.F64 ();

               if (payload.Exact ())
               {
                  pMap_Object->m_POD.Transform.d4Rotation[0] = dX;
                  pMap_Object->m_POD.Transform.d4Rotation[1] = dY;
                  pMap_Object->m_POD.Transform.d4Rotation[2] = dZ;
                  pMap_Object->m_POD.Transform.d4Rotation[3] = dW;
               }
            } break;

            case kSNEEZE_ABI_METHOD_NODE_SCALE:
            {
               double dScale = payload.F64 ();

               if (payload.Exact ())
               {
                  pMap_Object->m_POD.Transform.d3Scale[0] = dScale;
                  pMap_Object->m_POD.Transform.d3Scale[1] = dScale;
                  pMap_Object->m_POD.Transform.d3Scale[2] = dScale;
               }
            } break;

            case kSNEEZE_ABI_METHOD_NODE_SCALE_AXES:
            {
               double dX = payload.F64 ();
               double dY = payload.F64 ();
               double dZ = payload.F64 ();

               if (payload.Exact ())
               {
                  pMap_Object->m_POD.Transform.d3Scale[0] = dX;
                  pMap_Object->m_POD.Transform.d3Scale[1] = dY;
                  pMap_Object->m_POD.Transform.d3Scale[2] = dZ;
               }
            } break;

            case kSNEEZE_ABI_METHOD_NODE_BOUND:
            {
               double dX = payload.F64 ();
               double dY = payload.F64 ();
               double dZ = payload.F64 ();

               if (payload.Exact ())
               {
                  pMap_Object->m_POD.Bound.d3Max[0] = dX;
                  pMap_Object->m_POD.Bound.d3Max[1] = dY;
                  pMap_Object->m_POD.Bound.d3Max[2] = dZ;
               }
            } break;

            case kSNEEZE_ABI_METHOD_NODE_NAME:
            {
               int32_t nOffset = payload.I32 ();
               int32_t nLen    = payload.I32 ();

               if (payload.Exact ())
               {
                  std::string sName = ReadWasmString (pCaller, nOffset, nLen); // DAVE

                  size_t nCopyLength = sizeof (pMap_Object->m_POD.Name.wsName) / sizeof (uint16_t);
                  nCopyLength = std::min (sName.size (), static_cast<size_t>(nCopyLength - 1));

                  std::transform (sName.begin (), sName.begin () + nCopyLength, pMap_Object->m_POD.Name.wsName, [](unsigned char ch) { return static_cast<uint16_t> (ch); });
                  pMap_Object->m_POD.Name.wsName[nCopyLength] = 0;
               }
            } break;

            case kSNEEZE_ABI_METHOD_NODE_RESOURCE:
            {
               int32_t nOffset = payload.I32 ();
               int32_t nLen    = payload.I32 ();

               if (payload.Exact ())
               {
                  std::string sUrl = ReadWasmString (pCaller, nOffset, nLen);

                  size_t nCopyLength = sizeof (pMap_Object->m_POD.Resource.sReference) / sizeof (uint8_t);
                  nCopyLength = std::min (sUrl.size (), static_cast<size_t>(nCopyLength - 1));

                  std::transform (sUrl.begin (), sUrl.begin () + nCopyLength, pMap_Object->m_POD.Resource.sReference, [](unsigned char ch) { return static_cast<uint8_t> (ch); });
                  pMap_Object->m_POD.Resource.sReference[nCopyLength] = 0;
               }
            } break;

            case kSNEEZE_ABI_METHOD_NODE_PANEL:
            {
               int32_t nOffset = payload.I32 ();
               int32_t nLen    = payload.I32 ();

               if (payload.Exact ())
               {
                  std::string       sRml   = ReadWasmString (pCaller, nOffset, nLen);

                  if (!sRml.empty ())
                     pNode->Source (sRml);
               }
            } break;

            default:
               break;
         }
      }
   }

   return 0;
}

// ---------------------------------------------------------------------------
// CHRONO dispatch — the wall clock and all civil (calendar) logic. Clocks are
// global, so this needs neither the store nor the container. TIME/DATE return
// bare scalars; NOW/MOMENT/SET/PARSE fill a guest-supplied SNEEZE_ABI_MOMENT by
// (offset, length); FORMAT reads a filled MOMENT back and returns a string.
// Payload: (u64 twFabricIx reserved, then per method - see sneeze_abi.h).
// ---------------------------------------------------------------------------

static int64_t Dispatch_Chrono (void* pWasm_Store, wasmtime_caller_t* pCaller, uint16_t wMethod, PAYLOAD payload)
{
   int64_t nResult = 0;

   uint64_t twFabricIx = payload.U64 ();   // reserved: per-fabric routing / permissions

   (void) pWasm_Store;
   (void) twFabricIx;

   switch (wMethod)
   {
      case kSNEEZE_ABI_METHOD_CHRONO_TIME:
      {
         if (payload.Exact ())
            nResult = Chrono_Time ();
      } break;

      case kSNEEZE_ABI_METHOD_CHRONO_DATE:
      {
         if (payload.Exact ())
            nResult = Chrono_Date ();
      } break;

      case kSNEEZE_ABI_METHOD_CHRONO_NOW:
      {
         int32_t nMomOff = payload.I32 ();
         int32_t nMomLen = payload.I32 ();

         if (payload.Exact ())
         {
            SNEEZE_ABI_MOMENT moment;
            Chrono_Moment_Now (moment);

            WriteWasmBytes (pCaller, nMomOff, nMomLen, &moment, static_cast<int32_t> (sizeof (moment)));
            nResult = 1;
         }
      } break;

      case kSNEEZE_ABI_METHOD_CHRONO_MOMENT:
      {
         int32_t eSource = payload.I32 ();
         int64_t qwValue = static_cast<int64_t> (payload.U64 ());
         int32_t nMomOff = payload.I32 ();
         int32_t nMomLen = payload.I32 ();

         if (payload.Exact ())
         {
            SNEEZE_ABI_MOMENT moment;
            Chrono_Moment_Scalar (moment, qwValue, eSource == 1);

            WriteWasmBytes (pCaller, nMomOff, nMomLen, &moment, static_cast<int32_t> (sizeof (moment)));
            nResult = 1;
         }
      } break;

      case kSNEEZE_ABI_METHOD_CHRONO_SET:
      {
         int32_t eZone     = payload.I32 ();
         int32_t nYear     = payload.I32 ();
         int32_t nMonth    = payload.I32 ();
         int32_t nDay      = payload.I32 ();
         int32_t nHour     = payload.I32 ();
         int32_t nMinute   = payload.I32 ();
         int32_t nSecond   = payload.I32 ();
         int32_t nFraction = payload.I32 ();
         int32_t nMomOff   = payload.I32 ();
         int32_t nMomLen   = payload.I32 ();

         if (payload.Exact ())
         {
            SNEEZE_ABI_MOMENT moment;
            bool bOk = Chrono_Moment_Set (moment, eZone, nYear, nMonth, nDay, nHour, nMinute, nSecond, nFraction);

            WriteWasmBytes (pCaller, nMomOff, nMomLen, &moment, static_cast<int32_t> (sizeof (moment)));
            nResult = bOk ? 1 : 0;
         }
      } break;

      case kSNEEZE_ABI_METHOD_CHRONO_PARSE:
      {
         int32_t eZone   = payload.I32 ();
         int32_t nStrOff = payload.I32 ();
         int32_t nStrLen = payload.I32 ();
         int32_t nMomOff = payload.I32 ();
         int32_t nMomLen = payload.I32 ();

         if (payload.Exact ())
         {
            std::string sText = ReadWasmString (pCaller, nStrOff, nStrLen);

            SNEEZE_ABI_MOMENT moment;
            bool bOk = Chrono_Moment_Parse (moment, eZone, sText);

            WriteWasmBytes (pCaller, nMomOff, nMomLen, &moment, static_cast<int32_t> (sizeof (moment)));
            nResult = bOk ? 1 : 0;
         }
      } break;

      case kSNEEZE_ABI_METHOD_CHRONO_FORMAT:
      {
         int32_t eZone    = payload.I32 ();
         int32_t nSpecOff = payload.I32 ();
         int32_t nSpecLen = payload.I32 ();
         int32_t nMomOff  = payload.I32 ();
         int32_t nMomLen  = payload.I32 ();
         int32_t nOutOff  = payload.I32 ();
         int32_t nOutLen  = payload.I32 ();

         if (payload.Exact ())
         {
            std::string sSpec = ReadWasmString (pCaller, nSpecOff, nSpecLen);

            SNEEZE_ABI_MOMENT moment;
            memset (&moment, 0, sizeof (moment));

            const uint8_t* pMom = ReadWasmBytes (pCaller, nMomOff, nMomLen);

            if (pMom  &&  nMomLen >= static_cast<int32_t> (sizeof (moment)))
               memcpy (&moment, pMom, sizeof (moment));

            std::string sOut = Chrono_Format (moment, eZone, sSpec);

            nResult = WriteWasmString (pCaller, nOutOff, nOutLen, sOut);
         }
      } break;

      default:
         break;
   }

   return nResult;
}

// ---------------------------------------------------------------------------
// PERFORMANCE dispatch — the monotonic high-resolution clock. NOW returns
// 100 ns since this fabric's origin; ORIGIN fills the wall MOMENT captured at
// that origin (JS performance.timeOrigin). The origin is per fabric (each FABRIC
// captures it at load), so twFabricIx selects whose origin to read.
// Payload: (u64 twFabricIx, then per method - see sneeze_abi.h).
// ---------------------------------------------------------------------------

static int64_t Dispatch_Performance (void* pWasm_Store, wasmtime_caller_t* pCaller, uint16_t wMethod, PAYLOAD payload)
{
   int64_t nResult = 0;

   uint64_t twFabricIx = payload.U64 ();

   SCENE*  pScene  = Scene (pWasm_Store);
   FABRIC* pFabric = pScene ? pScene->Fabric_Find (twFabricIx) : nullptr;

   if (pFabric)
   {
      switch (wMethod)
      {
         case kSNEEZE_ABI_METHOD_PERFORMANCE_NOW:
         {
            if (payload.Exact ())
               nResult = Performance_Now (pFabric->Performance_Origin_Steady ());
         } break;

         case kSNEEZE_ABI_METHOD_PERFORMANCE_ORIGIN:
         {
            int32_t nMomOff = payload.I32 ();
            int32_t nMomLen = payload.I32 ();

            if (payload.Exact ())
            {
               SNEEZE_ABI_MOMENT moment;
               Performance_Origin (pFabric->Performance_Origin_Wall (), moment);

               WriteWasmBytes (pCaller, nMomOff, nMomLen, &moment, static_cast<int32_t> (sizeof (moment)));
               nResult = 1;
            }
         } break;

         default:
            break;
      }
   }

   return nResult;
}

// ---------------------------------------------------------------------------
// TIMER dispatch — arm and disarm guest timers on the engine-wide timer
// service (owned by WASM_RUNTIME). SET returns a nonzero twTimerIx (0 on an
// invalid unit/value); CLEAR returns 0/1. The store is the timer's home, so
// the entry is keyed by (store, id); firing is driven later by the TIMER agent
// pool, which Notifies the store. Payload: (u64 twFabricIx, then per method).
// ---------------------------------------------------------------------------

static int64_t Dispatch_Timer (void* pWasm_Store, wasmtime_caller_t* pCaller, uint16_t wMethod, PAYLOAD payload)
{
   int64_t nResult = 0;

   (void) pCaller;

   WASM_STORE*  pStore  = static_cast<WASM_STORE*> (pWasm_Store);
   WASM_TIMERS* pTimers = pStore ? pStore->Engine ()->Wasm_Runtime ()->Timers () : nullptr;

   if (pTimers)
   {
      uint64_t twFabricIx = payload.U64 ();

      switch (wMethod)
      {
         case kSNEEZE_ABI_METHOD_TIMER_SET:
         {
            int32_t  eUnit   = payload.I32 ();
            int32_t  nValue  = payload.I32 ();
            uint64_t qwParam = payload.U64 ();
            int32_t  bRepeat = payload.I32 ();

            if (payload.Exact ())
               nResult = static_cast<int64_t> (pTimers->Arm (pStore, twFabricIx, eUnit, nValue, qwParam, bRepeat != 0));
         } break;

         case kSNEEZE_ABI_METHOD_TIMER_CLEAR:
         {
            uint64_t twTimerIx = payload.U64 ();

            if (payload.Exact ())
               nResult = pTimers->Clear (pStore, twTimerIx) ? 1 : 0;
         } break;

         default:
            break;
      }
   }

   return nResult;
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
               PAYLOAD payload (pPayload, static_cast<size_t> (dwSize));

               switch (wType)
               {
                  case kSNEEZE_ABI_TYPE_DATA:        nResult = Dispatch_Data        (pWasm_Store, pCaller, wMethod, payload); break;
                  case kSNEEZE_ABI_TYPE_CONSOLE:     nResult = Dispatch_Console     (pWasm_Store, pCaller, wMethod, payload); break;
                  case kSNEEZE_ABI_TYPE_STORAGE:     nResult = Dispatch_Storage     (pWasm_Store, pCaller, wMethod, payload); break;
                  case kSNEEZE_ABI_TYPE_SCENE:       nResult = Dispatch_Scene       (pWasm_Store, pCaller, wMethod, payload); break;
                  case kSNEEZE_ABI_TYPE_FABRIC:      nResult = Dispatch_Fabric      (pWasm_Store, pCaller, wMethod, payload); break;
                  case kSNEEZE_ABI_TYPE_NODE:        nResult = Dispatch_Node        (pWasm_Store, pCaller, wMethod, payload); break;
                  case kSNEEZE_ABI_TYPE_CHRONO:      nResult = Dispatch_Chrono      (pWasm_Store, pCaller, wMethod, payload); break;
                  case kSNEEZE_ABI_TYPE_PERFORMANCE: nResult = Dispatch_Performance (pWasm_Store, pCaller, wMethod, payload); break;
                  case kSNEEZE_ABI_TYPE_TIMER:       nResult = Dispatch_Timer       (pWasm_Store, pCaller, wMethod, payload); break;
                  case kSNEEZE_ABI_TYPE_SERVICES:    nResult = Dispatch_Services    (pWasm_Store, pCaller, wMethod, payload); break;
                  default:                                                                                                     break;
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
