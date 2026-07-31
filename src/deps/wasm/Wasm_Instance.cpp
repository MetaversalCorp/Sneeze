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

#include "Wasm.h"

using namespace SNEEZE::DEP;

WASM_INSTANCE::WASM_INSTANCE (ENGINE* pEngine, WASM_STORE* pStore, const std::string& sUrl, const std::string& sHash) :
   m_pEngine       (pEngine),
   m_pStore        (pStore),
   m_sUrl          (sUrl),
   m_sHash         (sHash),
   m_bState        (INSTANCE_STATE_DORMANT), 
   m_nRefCount     (0), 
   m_pModule       (nullptr), 
   m_bInstantiated (false), 
   m_wasmInstance  (), 
   m_fnInit        (), 
   m_fnShutdown    (), 
   m_fnOpen        (), 
   m_fnClose       (), 
   m_fnOnTimer     (), 
   m_fnAlloc       (), 
   m_fnFree        (), 
   m_fnNotify      (), 
   m_bHas_Init     (false), 
   m_bHas_Shutdown (false), 
   m_bHas_Open     (false), 
   m_bHas_Close    (false), 
   m_bHas_OnTimer  (false), 
   m_bHas_Alloc    (false), 
   m_bHas_Free     (false), 
   m_bHas_Notify   (false)
{
   memset (&m_wasmInstance, 0, sizeof (m_wasmInstance));
   memset (&m_fnInit,       0, sizeof (m_fnInit));
   memset (&m_fnShutdown,   0, sizeof (m_fnShutdown));
   memset (&m_fnOpen,       0, sizeof (m_fnOpen));
   memset (&m_fnClose,      0, sizeof (m_fnClose));
   memset (&m_fnOnTimer,    0, sizeof (m_fnOnTimer));
   memset (&m_fnAlloc,      0, sizeof (m_fnAlloc));
   memset (&m_fnFree,       0, sizeof (m_fnFree));
   memset (&m_fnNotify,     0, sizeof (m_fnNotify));
}

WASM_INSTANCE::~WASM_INSTANCE ()
{
   if (m_pModule)
   {
      wasmtime_module_delete (m_pModule);
      m_pModule = nullptr;
   }
}

// ---------------------------------------------------------------------------
// Compile — compiles WASM bytecode into a module. Does not yet instantiate.
// ---------------------------------------------------------------------------

bool WASM_INSTANCE::Compile (wasm_engine_t* pEngine, const uint8_t* pBytes, size_t nSize)
{
   bool bResult = false;

   if (m_pModule)
      bResult = true;
   else
   {
      wasmtime_error_t* pError = wasmtime_module_new (pEngine, pBytes, nSize, &m_pModule);

      if (pError)
      {
         wasm_message_t msg;
         wasmtime_error_message (pError, &msg);
         m_pEngine->Log (IENGINE::kLOGLEVEL_Error, "WASM_INSTANCE", "Compile failed [" + m_sUrl + "]: " + std::string (msg.data, msg.size));
         wasm_byte_vec_delete (&msg);
         wasmtime_error_delete (pError);
      }
      else
      {
         char sSizeBuf[32];
         std::snprintf (sSizeBuf, sizeof (sSizeBuf), "%.1f", static_cast<double> (nSize) / 1024.0);
         m_pEngine->Log (IENGINE::kLOGLEVEL_Info, "WASM_INSTANCE", "Compiled module [" + m_sUrl + "] (" + sSizeBuf + " KB)");

         bResult = true;
      }
   }

   return bResult;
}

// ---------------------------------------------------------------------------
// Instantiate — uses the store's linker to resolve imports and create a
// live instance. Looks up and caches guest export function handles.
// ---------------------------------------------------------------------------

bool WASM_INSTANCE::Instantiate ()
{
   bool bResult = false;

   if (m_bInstantiated)
      bResult = true;
   else if (m_pModule)
   {
      wasmtime_linker_t* pLinker = m_pStore->Linker ();

      if (pLinker)
      {
         wasmtime_context_t* pCtx = m_pStore->Context ();
         wasm_trap_t* pTrap = nullptr;

         wasmtime_error_t* pError = wasmtime_linker_instantiate (pLinker, pCtx, m_pModule, &m_wasmInstance, &pTrap);

         if (pError)
         {
            wasm_message_t msg;
            wasmtime_error_message (pError, &msg);
            m_pEngine->Log (IENGINE::kLOGLEVEL_Error, "WASM_INSTANCE", "Instantiate failed [" + m_sUrl + "]: " + std::string (msg.data, msg.size));
            wasm_byte_vec_delete (&msg);
            wasmtime_error_delete (pError);
         }
         else if (pTrap)
         {
            wasm_message_t msg;
            wasm_trap_message (pTrap, &msg);
            m_pEngine->Log (IENGINE::kLOGLEVEL_Error, "WASM_INSTANCE", "Instantiate trapped [" + m_sUrl + "]: " + std::string (msg.data, msg.size));
            wasm_byte_vec_delete (&msg);
            wasm_trap_delete (pTrap);
         }
         else
         {
            Export_Lookup ("Init",     &m_fnInit,     &m_bHas_Init);
            Export_Lookup ("Shutdown", &m_fnShutdown, &m_bHas_Shutdown);
            Export_Lookup ("Open",     &m_fnOpen,     &m_bHas_Open);
            Export_Lookup ("Close",    &m_fnClose,    &m_bHas_Close);
            // OnTimer is deprecated for WASM: timer ticks are delivered through
            // Notify. Looked up only to support not-yet-migrated call sites.
            Export_Lookup ("OnTimer",  &m_fnOnTimer,  &m_bHas_OnTimer);
            Export_Lookup ("Alloc",    &m_fnAlloc,    &m_bHas_Alloc);
            Export_Lookup ("Free",     &m_fnFree,     &m_bHas_Free);
            Export_Lookup ("Notify",   &m_fnNotify,   &m_bHas_Notify);

            m_bInstantiated = true;

            m_pEngine->Log (IENGINE::kLOGLEVEL_Info, "WASM_INSTANCE", "Instantiated [" + m_sUrl + "] (exports: Init=" + std::to_string (m_bHas_Init) + " Shutdown=" + std::to_string (m_bHas_Shutdown) + " Open=" + std::to_string (m_bHas_Open) + " Close=" + std::to_string (m_bHas_Close) + ")");

            bResult = true;
         }
      }
      else m_pEngine->Log (IENGINE::kLOGLEVEL_Error, "WASM_INSTANCE", "Cannot instantiate — no linker on store [" + m_sUrl + "]");
   }
   else m_pEngine->Log (IENGINE::kLOGLEVEL_Error, "WASM_INSTANCE", "Cannot instantiate — module not compiled [" + m_sUrl + "]");

   return bResult;
}

// ---------------------------------------------------------------------------
// Export_Lookup — finds a named function export in the instantiated module.
// ---------------------------------------------------------------------------

bool WASM_INSTANCE::Export_Lookup (const char* sName, wasmtime_func_t* pFunc, bool* pFound)
{
   *pFound = false;

   wasmtime_context_t* pCtx = m_pStore->Context ();
   wasmtime_extern_t ext;

   bool bFound = wasmtime_instance_export_get (pCtx, &m_wasmInstance, sName, strlen (sName), &ext);

   if (bFound  &&  ext.kind == WASMTIME_EXTERN_FUNC)
   {
      *pFunc = ext.of.func;
      *pFound = true;
   }

   return *pFound;
}

// ---------------------------------------------------------------------------
// Open-snapshot push handshake — Alloc_Guest / Memory_Write / Free_Guest.
//
// The engine cannot hand the guest a host pointer (separate address spaces) or
// truncate one to i32. Instead it asks the guest to reserve a block of its own
// linear memory (Alloc), copies the blob in (Memory_Write), passes the guest
// offset to Open, then releases the block (Free) once Open has consumed it.
// ---------------------------------------------------------------------------

int32_t WASM_INSTANCE::Alloc_Guest (int32_t nSize)
{
   int32_t nOffset = 0;

   if (m_bHas_Alloc  &&  nSize > 0)
   {
      wasmtime_context_t* pCtx = m_pStore->Context ();

      wasmtime_val_t aArgs[1];
      aArgs[0].kind = WASMTIME_I32;  aArgs[0].of.i32 = nSize;

      wasmtime_val_t aResults[1];
      wasm_trap_t*   pTrap  = nullptr;
      wasmtime_error_t* pError = wasmtime_func_call (pCtx, &m_fnAlloc, aArgs, 1, aResults, 1, &pTrap);

      if (pError)
      {
         wasm_message_t msg;
         wasmtime_error_message (pError, &msg);
         m_pEngine->Log (IENGINE::kLOGLEVEL_Error, "WASM_INSTANCE", "Alloc failed [" + m_sUrl + "]: " + std::string (msg.data, msg.size));
         wasm_byte_vec_delete (&msg);
         wasmtime_error_delete (pError);
      }
      else if (pTrap)
      {
         wasm_message_t msg;
         wasm_trap_message (pTrap, &msg);
         m_pEngine->Log (IENGINE::kLOGLEVEL_Error, "WASM_INSTANCE", "Alloc trapped [" + m_sUrl + "]: " + std::string (msg.data, msg.size));
         wasm_byte_vec_delete (&msg);
         wasm_trap_delete (pTrap);
      }
      else if (aResults[0].kind == WASMTIME_I32)
         nOffset = aResults[0].of.i32;
   }

   return nOffset;
}

void WASM_INSTANCE::Free_Guest (int32_t nOffset, int32_t nSize)
{
   if (m_bHas_Free  &&  nOffset != 0  &&  nSize > 0)
   {
      wasmtime_context_t* pCtx = m_pStore->Context ();

      wasmtime_val_t aArgs[2];
      aArgs[0].kind = WASMTIME_I32;  aArgs[0].of.i32 = nOffset;
      aArgs[1].kind = WASMTIME_I32;  aArgs[1].of.i32 = nSize;

      wasm_trap_t*      pTrap  = nullptr;
      wasmtime_error_t* pError = wasmtime_func_call (pCtx, &m_fnFree, aArgs, 2, nullptr, 0, &pTrap);

      if (pError)
      {
         wasm_message_t msg;
         wasmtime_error_message (pError, &msg);
         m_pEngine->Log (IENGINE::kLOGLEVEL_Error, "WASM_INSTANCE", "Free failed [" + m_sUrl + "]: " + std::string (msg.data, msg.size));
         wasm_byte_vec_delete (&msg);
         wasmtime_error_delete (pError);
      }
      else if (pTrap)
      {
         wasm_message_t msg;
         wasm_trap_message (pTrap, &msg);
         m_pEngine->Log (IENGINE::kLOGLEVEL_Error, "WASM_INSTANCE", "Free trapped [" + m_sUrl + "]: " + std::string (msg.data, msg.size));
         wasm_byte_vec_delete (&msg);
         wasm_trap_delete (pTrap);
      }
   }
}

bool WASM_INSTANCE::Memory_Write (int32_t nOffset, const uint8_t* pBytes, size_t nSize)
{
   bool bResult = false;

   wasmtime_context_t* pCtx = m_pStore->Context ();
   wasmtime_extern_t   ext;

   if (nOffset >= 0  &&  pBytes  &&  wasmtime_instance_export_get (pCtx, &m_wasmInstance, "memory", 6, &ext)  &&  ext.kind == WASMTIME_EXTERN_MEMORY)
   {
      uint8_t* pData    = wasmtime_memory_data (pCtx, &ext.of.memory);
      size_t   nMemSize = wasmtime_memory_data_size (pCtx, &ext.of.memory);

      if (static_cast<size_t> (nOffset) + nSize <= nMemSize)
      {
         memcpy (pData + nOffset, pBytes, nSize);
         bResult = true;
      }
   }

   return bResult;
}

// ---------------------------------------------------------------------------
// Lifecycle calls — invoke the exported functions in the WASM module.
// Open/Close manage the refcount internally. Initialize fires before the
// first Open; Shutdown fires after the last Close.
// ---------------------------------------------------------------------------

bool WASM_INSTANCE::Initialize ()
{
   bool bResult = true;

   if (!m_bInstantiated  &&  !Instantiate ())
      bResult = false;

   if (bResult)
   {
      m_bState = INSTANCE_STATE_ACTIVE;

      if (m_bHas_Init)
      {
         wasmtime_context_t* pCtx = m_pStore->Context ();
         wasm_trap_t* pTrap = nullptr;

         wasmtime_error_t* pError = wasmtime_func_call (pCtx, &m_fnInit, nullptr, 0, nullptr, 0, &pTrap);

         if (pError)
         {
            wasm_message_t msg;
            wasmtime_error_message (pError, &msg);
            m_pEngine->Log (IENGINE::kLOGLEVEL_Error, "WASM_INSTANCE", "Init failed [" + m_sUrl + "]: " + std::string (msg.data, msg.size));
            wasm_byte_vec_delete (&msg);
            wasmtime_error_delete (pError);

            bResult = false;
         }
         else if (pTrap)
         {
            wasm_message_t msg;
            wasm_trap_message (pTrap, &msg);
            m_pEngine->Log (IENGINE::kLOGLEVEL_Error, "WASM_INSTANCE", "Init trapped [" + m_sUrl + "]: " + std::string (msg.data, msg.size));
            wasm_byte_vec_delete (&msg);
            wasm_trap_delete (pTrap);

            bResult = false;
         }
         else m_pEngine->Log (IENGINE::kLOGLEVEL_Trace, "WASM_INSTANCE", "Init [" + m_sUrl + "]");
      }
   }

   return bResult;
}

bool WASM_INSTANCE::Open (uint64_t twFabricIx, const uint8_t* pParams, size_t nParamsSize)
{
   bool bResult = false;

   int nPrev = m_nRefCount;
   m_nRefCount++;

   if (nPrev == 0  &&  !Initialize ())
      bResult = false;
   else if (!m_bHas_Open)
      bResult = true;
   else
   {
      wasmtime_context_t* pCtx = m_pStore->Context ();

      // Push the snapshot into guest memory via the alloc handshake. On any
      // failure the offset/size collapse to 0 so the guest sees an empty
      // snapshot rather than a bogus pointer.
      int32_t nBlobOffset = 0;
      int32_t nBlobSize   = 0;

      if (pParams  &&  nParamsSize > 0)
      {
         int32_t nWant   = static_cast<int32_t> (nParamsSize);
         int32_t nOffset = Alloc_Guest (nWant);

         if (nOffset != 0  &&  Memory_Write (nOffset, pParams, nParamsSize))
         {
            nBlobOffset = nOffset;
            nBlobSize   = nWant;
         }
         else if (nOffset != 0)
            Free_Guest (nOffset, nWant);
      }

      wasmtime_val_t aArgs[3];
      aArgs[0].kind = WASMTIME_I64;  aArgs[0].of.i64 = static_cast<int64_t> (twFabricIx);
      aArgs[1].kind = WASMTIME_I32;  aArgs[1].of.i32 = nBlobOffset;
      aArgs[2].kind = WASMTIME_I32;  aArgs[2].of.i32 = nBlobSize;

      wasm_trap_t* pTrap = nullptr;
      wasmtime_error_t* pError = wasmtime_func_call (pCtx, &m_fnOpen, aArgs, 3, nullptr, 0, &pTrap);

      if (pError)
      {
         wasm_message_t msg;
         wasmtime_error_message (pError, &msg);
         m_pEngine->Log (IENGINE::kLOGLEVEL_Error, "WASM_INSTANCE", "Open failed [" + m_sUrl + "]: " + std::string (msg.data, msg.size));
         wasm_byte_vec_delete (&msg);
         wasmtime_error_delete (pError);
      }
      else if (pTrap)
      {
         wasm_message_t msg;
         wasm_trap_message (pTrap, &msg);
         m_pEngine->Log (IENGINE::kLOGLEVEL_Error, "WASM_INSTANCE", "Open trapped [" + m_sUrl + "]: " + std::string (msg.data, msg.size));
         wasm_byte_vec_delete (&msg);
         wasm_trap_delete (pTrap);
      }
      else
      {
         m_pEngine->Log (IENGINE::kLOGLEVEL_Trace, "WASM_INSTANCE", "Open [" + m_sUrl + "] fabric=" + std::to_string (twFabricIx));
         bResult = true;
      }

      // Open has consumed the snapshot; release the guest block (mirror of
      // Alloc_Guest). The guest copies out whatever it needs during Open.
      if (nBlobOffset != 0)
         Free_Guest (nBlobOffset, nBlobSize);
   }

   return bResult;
}

bool WASM_INSTANCE::Close (uint64_t twFabricIx)
{
   bool bResult = true;

   if (m_bHas_Close)
   {
      wasmtime_context_t* pCtx = m_pStore->Context ();

      wasmtime_val_t aArgs[1];
      aArgs[0].kind = WASMTIME_I64;  aArgs[0].of.i64 = static_cast<int64_t> (twFabricIx);

      wasm_trap_t* pTrap = nullptr;
      wasmtime_error_t* pError = wasmtime_func_call (pCtx, &m_fnClose, aArgs, 1, nullptr, 0, &pTrap);

      if (pError)
      {
         wasm_message_t msg;
         wasmtime_error_message (pError, &msg);
         m_pEngine->Log (IENGINE::kLOGLEVEL_Error, "WASM_INSTANCE", "Close failed [" + m_sUrl + "]: " + std::string (msg.data, msg.size));
         wasm_byte_vec_delete (&msg);
         wasmtime_error_delete (pError);
         bResult = false;
      }
      else if (pTrap)
      {
         wasm_message_t msg;
         wasm_trap_message (pTrap, &msg);
         m_pEngine->Log (IENGINE::kLOGLEVEL_Error, "WASM_INSTANCE", "Close trapped [" + m_sUrl + "]: " + std::string (msg.data, msg.size));
         wasm_byte_vec_delete (&msg);
         wasm_trap_delete (pTrap);
         bResult = false;
      }
      else
         m_pEngine->Log (IENGINE::kLOGLEVEL_Trace, "WASM_INSTANCE", "Close [" + m_sUrl + "] fabric=" + std::to_string (twFabricIx));
   }

   if (m_nRefCount > 0)
      m_nRefCount--;

   if (m_nRefCount == 0)
      Finalize ();

   return bResult;
}

// ---------------------------------------------------------------------------
// Notify_Guest — the host -> guest event push. Mirrors Open's snapshot
// handshake: reserve a guest block (Alloc), copy the packet in (Memory_Write),
// call the guest's Notify export with the (offset, size), then release the
// block (Free). The store lock is the caller's responsibility. Dormant
// instances and modules with no Notify export are silently skipped.
// ---------------------------------------------------------------------------

bool WASM_INSTANCE::Notify_Guest (const uint8_t* pPacket, size_t nSize)
{
   bool bResult = false;

   if (m_bHas_Notify  &&  m_bState == INSTANCE_STATE_ACTIVE  &&  pPacket  &&  nSize > 0)
   {
      int32_t nWant   = static_cast<int32_t> (nSize);
      int32_t nOffset = Alloc_Guest (nWant);

      if (nOffset != 0  &&  Memory_Write (nOffset, pPacket, nSize))
      {
         wasmtime_context_t* pCtx = m_pStore->Context ();

         wasmtime_val_t aArgs[2];
         aArgs[0].kind = WASMTIME_I32;  aArgs[0].of.i32 = nOffset;
         aArgs[1].kind = WASMTIME_I32;  aArgs[1].of.i32 = nWant;

         wasmtime_val_t    aResults[1];
         wasm_trap_t*      pTrap  = nullptr;
         wasmtime_error_t* pError = wasmtime_func_call (pCtx, &m_fnNotify, aArgs, 2, aResults, 1, &pTrap);

         if (pError)
         {
            wasm_message_t msg;
            wasmtime_error_message (pError, &msg);
            m_pEngine->Log (IENGINE::kLOGLEVEL_Error, "WASM_INSTANCE", "Notify failed [" + m_sUrl + "]: " + std::string (msg.data, msg.size));
            wasm_byte_vec_delete (&msg);
            wasmtime_error_delete (pError);
         }
         else if (pTrap)
         {
            wasm_message_t msg;
            wasm_trap_message (pTrap, &msg);
            m_pEngine->Log (IENGINE::kLOGLEVEL_Error, "WASM_INSTANCE", "Notify trapped [" + m_sUrl + "]: " + std::string (msg.data, msg.size));
            wasm_byte_vec_delete (&msg);
            wasm_trap_delete (pTrap);
         }
         else bResult = true;
      }

      if (nOffset != 0)
         Free_Guest (nOffset, nWant);
   }

   return bResult;
}

bool WASM_INSTANCE::Finalize ()
{
   bool bResult = true;

   m_bState = INSTANCE_STATE_DORMANT;

   if (m_bHas_Shutdown)
   {
      wasmtime_context_t* pCtx = m_pStore->Context ();
      wasm_trap_t* pTrap = nullptr;

      wasmtime_error_t* pError = wasmtime_func_call (pCtx, &m_fnShutdown, nullptr, 0, nullptr, 0, &pTrap);

      if (pError)
      {
         wasm_message_t msg;
         wasmtime_error_message (pError, &msg);
         m_pEngine->Log (IENGINE::kLOGLEVEL_Error, "WASM_INSTANCE", "Shutdown failed [" + m_sUrl + "]: " + std::string (msg.data, msg.size));
         wasm_byte_vec_delete (&msg);
         wasmtime_error_delete (pError);
         bResult = false;
      }
      else if (pTrap)
      {
         wasm_message_t msg;
         wasm_trap_message (pTrap, &msg);
         m_pEngine->Log (IENGINE::kLOGLEVEL_Error, "WASM_INSTANCE", "Shutdown trapped [" + m_sUrl + "]: " + std::string (msg.data, msg.size));
         wasm_byte_vec_delete (&msg);
         wasm_trap_delete (pTrap);
         bResult = false;
      }
      else m_pEngine->Log (IENGINE::kLOGLEVEL_Trace, "WASM_INSTANCE", "Shutdown [" + m_sUrl + "]");
   }

   return bResult;
}
