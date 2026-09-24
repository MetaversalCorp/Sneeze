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

#include <wasmtime.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static int nPassed = 0;
static int nFailed = 0;

static void Check (bool bCondition, const char* szName)
{
   if (bCondition)
   {
      std::printf ("  PASS: %s\n", szName);
      nPassed++;
   }
   else
   {
      std::printf ("  FAIL: %s\n", szName);
      nFailed++;
   }
}

// Reusable helper: print a wasmtime_error_t and free it
static void PrintError (wasmtime_error_t* pError)
{
   wasm_byte_vec_t msg;
   wasmtime_error_message (pError, &msg);
   std::fprintf (stderr, "    error: %.*s\n", static_cast<int> (msg.size), msg.data);
   wasm_byte_vec_delete (&msg);
   wasmtime_error_delete (pError);
}

// ---------------------------------------------------------------------------
// Test 1: Engine + Store creation (basic sanity)
// ---------------------------------------------------------------------------
static void TestEngineAndStore ()
{
   std::printf ("\n[Test 1] Engine and Store creation\n");

   wasm_engine_t* pEngine = wasm_engine_new ();
   Check (pEngine != nullptr, "Engine created");

   wasmtime_store_t* pStore = wasmtime_store_new (pEngine, nullptr, nullptr);
   Check (pStore != nullptr, "Store created");

   wasmtime_store_delete (pStore);
   wasm_engine_delete (pEngine);
}

// ---------------------------------------------------------------------------
// Test 2: Compile WAT, instantiate, call an exported function
// ---------------------------------------------------------------------------
static void TestCompileAndCall ()
{
   std::printf ("\n[Test 2] Compile WAT -> Instantiate -> Call 'add(3, 4)'\n");

   static const char* szWat =
      "(module"
      "  (func (export \"add\") (param i32 i32) (result i32)"
      "    local.get 0"
      "    local.get 1"
      "    i32.add"
      "  )"
      ")";

   wasm_engine_t* pEngine = wasm_engine_new ();
   wasmtime_store_t* pStore = wasmtime_store_new (pEngine, nullptr, nullptr);
   wasmtime_context_t* pContext = wasmtime_store_context (pStore);

   wasm_byte_vec_t vWasm;
   wasmtime_error_t* pErr = wasmtime_wat2wasm (szWat, std::strlen (szWat), &vWasm);
   Check (pErr == nullptr, "WAT compiled to WASM");

   if (pErr)
   {
      PrintError (pErr);
   }
   else
   {
      wasmtime_module_t* pModule = nullptr;
      pErr = wasmtime_module_new (pEngine, reinterpret_cast<uint8_t*> (vWasm.data),
         vWasm.size, &pModule);
      wasm_byte_vec_delete (&vWasm);
      Check (pErr == nullptr  &&  pModule != nullptr, "Module compiled");

      if (pErr)
      {
         PrintError (pErr);
      }
      else
      {
         wasmtime_instance_t instance;
         wasm_trap_t* pTrap = nullptr;
         pErr = wasmtime_instance_new (pContext, pModule, nullptr, 0, &instance, &pTrap);
         Check (pErr == nullptr  &&  pTrap == nullptr, "Module instantiated");

         if (pErr)
         {
            PrintError (pErr);
         }
         else
         {
            wasmtime_extern_t exportItem;
            bool bFound = wasmtime_instance_export_get (pContext, &instance, "add", 3, &exportItem);
            Check (bFound  &&  exportItem.kind == WASMTIME_EXTERN_FUNC, "Export 'add' found");

            wasmtime_val_t aParams[2];
            aParams[0].kind = WASMTIME_I32;
            aParams[0].of.i32 = 3;
            aParams[1].kind = WASMTIME_I32;
            aParams[1].of.i32 = 4;

            wasmtime_val_t aResults[1];
            pErr = wasmtime_func_call (pContext, &exportItem.of.func, aParams, 2, aResults, 1, &pTrap);
            Check (pErr == nullptr  &&  pTrap == nullptr, "Function called without error");
            Check (aResults[0].kind == WASMTIME_I32  &&  aResults[0].of.i32 == 7,
               "add(3, 4) == 7");

            if (pErr)
            {
               PrintError (pErr);
            }
            if (pTrap)
            {
               wasm_trap_delete (pTrap);
            }
         }

         wasmtime_module_delete (pModule);
      }
   }

   wasmtime_store_delete (pStore);
   wasm_engine_delete (pEngine);
}

// ---------------------------------------------------------------------------
// Test 3: Fuel metering - CPU budget enforcement
// ---------------------------------------------------------------------------
static void TestFuelMetering ()
{
   std::printf ("\n[Test 3] Fuel metering (CPU budget enforcement)\n");

   wasm_config_t* pConfig = wasm_config_new ();
   wasmtime_config_consume_fuel_set (pConfig, true);

   wasm_engine_t* pEngine = wasm_engine_new_with_config (pConfig);
   wasmtime_store_t* pStore = wasmtime_store_new (pEngine, nullptr, nullptr);
   wasmtime_context_t* pContext = wasmtime_store_context (pStore);

   wasmtime_error_t* pErr = wasmtime_context_set_fuel (pContext, 10000);
   Check (pErr == nullptr, "Fuel set to 10000");
   if (pErr)
   {
      PrintError (pErr);
   }

   static const char* szWat =
      "(module"
      "  (func (export \"spin\") (param i32) (result i32)"
      "    (local i32)"
      "    (local.set 1 (i32.const 0))"
      "    (block $break"
      "      (loop $loop"
      "        (br_if $break (i32.ge_u (local.get 1) (local.get 0)))"
      "        (local.set 1 (i32.add (local.get 1) (i32.const 1)))"
      "        (br $loop)"
      "      )"
      "    )"
      "    (local.get 1)"
      "  )"
      ")";

   wasm_byte_vec_t vWasm;
   pErr = wasmtime_wat2wasm (szWat, std::strlen (szWat), &vWasm);
   Check (pErr == nullptr, "Loop WAT compiled");

   if (pErr)
   {
      PrintError (pErr);
   }
   else
   {
      wasmtime_module_t* pModule = nullptr;
      pErr = wasmtime_module_new (pEngine, reinterpret_cast<uint8_t*> (vWasm.data),
         vWasm.size, &pModule);
      wasm_byte_vec_delete (&vWasm);
      Check (pErr == nullptr, "Loop module compiled");

      if (pErr)
      {
         PrintError (pErr);
      }
      else
      {
         wasmtime_instance_t instance;
         wasm_trap_t* pTrap = nullptr;
         pErr = wasmtime_instance_new (pContext, pModule, nullptr, 0, &instance, &pTrap);
         Check (pErr == nullptr  &&  pTrap == nullptr, "Loop module instantiated");

         wasmtime_extern_t spinExport;
         wasmtime_instance_export_get (pContext, &instance, "spin", 4, &spinExport);

         wasmtime_val_t param;
         param.kind = WASMTIME_I32;
         param.of.i32 = 10;

         wasmtime_val_t result;
         pErr = wasmtime_func_call (pContext, &spinExport.of.func, &param, 1, &result, 1, &pTrap);
         Check (pErr == nullptr  &&  pTrap == nullptr  &&  result.of.i32 == 10,
            "spin(10) succeeds with fuel");
         if (pTrap)
         {
            wasm_trap_delete (pTrap);
            pTrap = nullptr;
         }
         if (pErr)
         {
            PrintError (pErr);
            pErr = nullptr;
         }

         uint64_t nFuelRemaining = 0;
         wasmtime_context_get_fuel (pContext, &nFuelRemaining);
         Check (nFuelRemaining < 10000, "Fuel was consumed");
         std::printf ("    (fuel remaining: %llu of 10000)\n",
            static_cast<unsigned long long> (nFuelRemaining));

         wasmtime_context_set_fuel (pContext, 5);
         param.of.i32 = 1000000;

         pErr = wasmtime_func_call (pContext, &spinExport.of.func, &param, 1, &result, 1, &pTrap);
         Check (pTrap != nullptr, "spin(1000000) traps when fuel exhausted");
         if (pTrap)
         {
            wasm_trap_delete (pTrap);
         }
         if (pErr)
         {
            PrintError (pErr);
         }

         wasmtime_module_delete (pModule);
      }
   }

   wasmtime_store_delete (pStore);
   wasm_engine_delete (pEngine);
}

// ---------------------------------------------------------------------------
// Test 4: Error handling - invalid WAT
// ---------------------------------------------------------------------------
static void TestInvalidWat ()
{
   std::printf ("\n[Test 4] Error handling (invalid WAT)\n");

   static const char* szBadWat = "(module (func (export \"oops\") (result i32) i32.oops))";

   wasm_byte_vec_t vWasm;
   wasmtime_error_t* pErr = wasmtime_wat2wasm (szBadWat, std::strlen (szBadWat), &vWasm);
   Check (pErr != nullptr, "Invalid WAT correctly rejected");
   if (pErr)
   {
      PrintError (pErr);
   }
}

// ---------------------------------------------------------------------------
// Test 5: Host function - calling back from WASM into C++
// ---------------------------------------------------------------------------
static wasm_trap_t* HostMultiply (
   void* /*env*/, wasmtime_caller_t* /*caller*/,
   const wasmtime_val_t* args, size_t /*nargs*/,
   wasmtime_val_t* results, size_t /*nresults*/)
{
   results[0].kind = WASMTIME_I32;
   results[0].of.i32 = args[0].of.i32 * args[1].of.i32;
   return nullptr;
}

static void TestHostFunction ()
{
   std::printf ("\n[Test 5] Host function (WASM calls back into C++)\n");

   wasm_engine_t* pEngine = wasm_engine_new ();
   wasmtime_store_t* pStore = wasmtime_store_new (pEngine, nullptr, nullptr);
   wasmtime_context_t* pContext = wasmtime_store_context (pStore);

   wasm_valtype_t* aParamTypes[] = { wasm_valtype_new (WASM_I32), wasm_valtype_new (WASM_I32) };
   wasm_valtype_t* aResultTypes[] = { wasm_valtype_new (WASM_I32) };
   wasm_valtype_vec_t vParams, vResults;
   wasm_valtype_vec_new (&vParams, 2, aParamTypes);
   wasm_valtype_vec_new (&vResults, 1, aResultTypes);
   wasm_functype_t* pFuncType = wasm_functype_new (&vParams, &vResults);

   wasmtime_func_t hostFunc;
   wasmtime_func_new (pContext, pFuncType, HostMultiply, nullptr, nullptr, &hostFunc);
   wasm_functype_delete (pFuncType);

   static const char* szWat =
      "(module"
      "  (import \"host\" \"multiply\" (func $mul (param i32 i32) (result i32)))"
      "  (func (export \"square\") (param i32) (result i32)"
      "    local.get 0"
      "    local.get 0"
      "    call $mul"
      "  )"
      ")";

   wasm_byte_vec_t vWasm;
   wasmtime_error_t* pErr = wasmtime_wat2wasm (szWat, std::strlen (szWat), &vWasm);
   Check (pErr == nullptr, "Host-import WAT compiled");

   if (pErr)
   {
      PrintError (pErr);
   }
   else
   {
      wasmtime_module_t* pModule = nullptr;
      pErr = wasmtime_module_new (pEngine, reinterpret_cast<uint8_t*> (vWasm.data),
         vWasm.size, &pModule);
      wasm_byte_vec_delete (&vWasm);
      Check (pErr == nullptr, "Host-import module compiled");

      if (pErr)
      {
         PrintError (pErr);
      }
      else
      {
         wasmtime_extern_t import;
         import.kind = WASMTIME_EXTERN_FUNC;
         import.of.func = hostFunc;

         wasmtime_instance_t instance;
         wasm_trap_t* pTrap = nullptr;
         pErr = wasmtime_instance_new (pContext, pModule, &import, 1, &instance, &pTrap);
         Check (pErr == nullptr  &&  pTrap == nullptr, "Module with host import instantiated");

         wasmtime_extern_t squareExport;
         wasmtime_instance_export_get (pContext, &instance, "square", 6, &squareExport);

         wasmtime_val_t param;
         param.kind = WASMTIME_I32;
         param.of.i32 = 7;

         wasmtime_val_t result;
         pErr = wasmtime_func_call (pContext, &squareExport.of.func, &param, 1, &result, 1, &pTrap);
         Check (pErr == nullptr  &&  pTrap == nullptr, "square() called");
         Check (result.of.i32 == 49, "square(7) == 49");

         if (pTrap)
         {
            wasm_trap_delete (pTrap);
         }
         if (pErr)
         {
            PrintError (pErr);
         }

         wasmtime_module_delete (pModule);
      }
   }

   wasmtime_store_delete (pStore);
   wasm_engine_delete (pEngine);
}

// ---------------------------------------------------------------------------
// Test 6: Hello WASM module - compiled Rust module with host function imports
// ---------------------------------------------------------------------------

static std::vector<std::string> g_aLogMessages;

static wasm_trap_t* TestConsoleLog (
   void* /*env*/, wasmtime_caller_t* pCaller,
   const wasmtime_val_t* pArgs, size_t nArgs,
   wasmtime_val_t* /*results*/, size_t /*nresults*/)
{
   if (nArgs >= 2)
   {
      int32_t nPtr = pArgs[0].of.i32;
      int32_t nLen = pArgs[1].of.i32;

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
               std::string sMsg (reinterpret_cast<const char*> (pData + nPtr), static_cast<size_t> (nLen));
               g_aLogMessages.push_back (sMsg);
               std::printf ("    [Console.Log] %s\n", sMsg.c_str ());
            }
         }
      }
   }

   return nullptr;
}

static void TestHelloWasm ()
{
   std::printf ("\n[Test 6] Hello WASM module (Rust, linker, lifecycle)\n");

   g_aLogMessages.clear ();

   const char* sPath = "tools/HelloWasm/target/wasm32-unknown-unknown/release/hello_wasm.wasm";
   FILE* pFile = std::fopen (sPath, "rb");
   Check (pFile != nullptr, "hello_wasm.wasm found on disk");

   if (!pFile)
   {
      std::printf ("    (looked for: %s)\n", sPath);
      return;
   }

   std::fseek (pFile, 0, SEEK_END);
   long nSize = std::ftell (pFile);
   std::fseek (pFile, 0, SEEK_SET);

   std::vector<uint8_t> aBytes (static_cast<size_t> (nSize));
   std::fread (aBytes.data (), 1, static_cast<size_t> (nSize), pFile);
   std::fclose (pFile);

   Check (nSize > 4  &&  aBytes[0] == 0x00  &&  aBytes[1] == 0x61  &&  aBytes[2] == 0x73  &&  aBytes[3] == 0x6D,
      "Valid WASM magic number");

   wasm_engine_t* pEngine = wasm_engine_new ();
   wasmtime_store_t* pStore = wasmtime_store_new (pEngine, nullptr, nullptr);
   wasmtime_context_t* pContext = wasmtime_store_context (pStore);

   wasmtime_linker_t* pLinker = wasmtime_linker_new (pEngine);
   Check (pLinker != nullptr, "Linker created");

   wasm_valtype_t* aConsoleLogParams[] = { wasm_valtype_new (WASM_I32), wasm_valtype_new (WASM_I32) };
   wasm_valtype_vec_t vParams;
   wasm_valtype_vec_new (&vParams, 2, aConsoleLogParams);
   wasm_valtype_vec_t vNoResults;
   wasm_valtype_vec_new_empty (&vNoResults);
   wasm_functype_t* pLogType = wasm_functype_new (&vParams, &vNoResults);

   wasmtime_error_t* pErr = wasmtime_linker_define_func (pLinker,
      "Console", 7, "Log", 3,
      pLogType, TestConsoleLog, nullptr, nullptr);
   wasm_functype_delete (pLogType);
   Check (pErr == nullptr, "Console.Log registered in linker");
   if (pErr) PrintError (pErr);

   wasmtime_module_t* pModule = nullptr;
   pErr = wasmtime_module_new (pEngine, aBytes.data (), aBytes.size (), &pModule);
   Check (pErr == nullptr  &&  pModule != nullptr, "Module compiled from .wasm bytes");
   if (pErr) { PrintError (pErr); }

   if (pModule)
   {
      wasmtime_instance_t instance;
      wasm_trap_t* pTrap = nullptr;
      pErr = wasmtime_linker_instantiate (pLinker, pContext, pModule, &instance, &pTrap);
      Check (pErr == nullptr  &&  pTrap == nullptr, "Module instantiated via linker");
      if (pErr) { PrintError (pErr); }
      if (pTrap)
      {
         wasm_message_t msg;
         wasm_trap_message (pTrap, &msg);
         std::fprintf (stderr, "    trap: %.*s\n", static_cast<int> (msg.size), msg.data);
         wasm_byte_vec_delete (&msg);
         wasm_trap_delete (pTrap);
         pTrap = nullptr;
      }

      if (!pErr  &&  !pTrap)
      {
         wasmtime_extern_t ext;

         bool bInit = wasmtime_instance_export_get (pContext, &instance, "Init", 4, &ext);
         Check (bInit  &&  ext.kind == WASMTIME_EXTERN_FUNC, "Export 'Init' found");

         if (bInit)
         {
            pErr = wasmtime_func_call (pContext, &ext.of.func, nullptr, 0, nullptr, 0, &pTrap);
            Check (pErr == nullptr  &&  pTrap == nullptr, "Init() called successfully");
            if (pErr) PrintError (pErr);
            if (pTrap) { wasm_trap_delete (pTrap); pTrap = nullptr; }
         }

         bool bOpen = wasmtime_instance_export_get (pContext, &instance, "Open", 4, &ext);
         Check (bOpen  &&  ext.kind == WASMTIME_EXTERN_FUNC, "Export 'Open' found");

         if (bOpen)
         {
            wasmtime_val_t aArgs[4];
            aArgs[0].kind = WASMTIME_I32;  aArgs[0].of.i32 = 42;
            aArgs[1].kind = WASMTIME_I32;  aArgs[1].of.i32 = 0;
            aArgs[2].kind = WASMTIME_I32;  aArgs[2].of.i32 = 0;
            aArgs[3].kind = WASMTIME_I32;  aArgs[3].of.i32 = 0;
            pErr = wasmtime_func_call (pContext, &ext.of.func, aArgs, 4, nullptr, 0, &pTrap);
            Check (pErr == nullptr  &&  pTrap == nullptr, "Open(42) called successfully");
            if (pErr) PrintError (pErr);
            if (pTrap) { wasm_trap_delete (pTrap); pTrap = nullptr; }
         }

         bool bClose = wasmtime_instance_export_get (pContext, &instance, "Close", 5, &ext);
         Check (bClose  &&  ext.kind == WASMTIME_EXTERN_FUNC, "Export 'Close' found");

         if (bClose)
         {
            wasmtime_val_t aArgs[1];
            aArgs[0].kind = WASMTIME_I32;  aArgs[0].of.i32 = 42;
            pErr = wasmtime_func_call (pContext, &ext.of.func, aArgs, 1, nullptr, 0, &pTrap);
            Check (pErr == nullptr  &&  pTrap == nullptr, "Close(42) called successfully");
            if (pErr) PrintError (pErr);
            if (pTrap) { wasm_trap_delete (pTrap); pTrap = nullptr; }
         }

         bool bShutdown = wasmtime_instance_export_get (pContext, &instance, "Shutdown", 8, &ext);
         Check (bShutdown  &&  ext.kind == WASMTIME_EXTERN_FUNC, "Export 'Shutdown' found");

         if (bShutdown)
         {
            pErr = wasmtime_func_call (pContext, &ext.of.func, nullptr, 0, nullptr, 0, &pTrap);
            Check (pErr == nullptr  &&  pTrap == nullptr, "Shutdown() called successfully");
            if (pErr) PrintError (pErr);
            if (pTrap) { wasm_trap_delete (pTrap); pTrap = nullptr; }
         }

         Check (g_aLogMessages.size () == 4, "Console.Log called 4 times");

         if (g_aLogMessages.size () >= 4)
         {
            Check (g_aLogMessages[0] == "Hello from WASM!",          "Init message correct");
            Check (g_aLogMessages[1] == "WASM Open called",          "Open message correct");
            Check (g_aLogMessages[2] == "WASM Close called",         "Close message correct");
            Check (g_aLogMessages[3] == "WASM module shutting down",  "Shutdown message correct");
         }
      }

      wasmtime_module_delete (pModule);
   }

   wasmtime_linker_delete (pLinker);
   wasmtime_store_delete (pStore);
   wasm_engine_delete (pEngine);
}

// ---------------------------------------------------------------------------

int RunWasmTests (int /*nArgc*/, char** /*aArgv*/)
{
   std::printf ("=== Wasmtime Integration Test Suite ===\n");
   std::printf ("Wasmtime version: %s\n", WASMTIME_VERSION);

   TestEngineAndStore ();
   TestCompileAndCall ();
   TestFuelMetering ();
   TestInvalidWat ();
   TestHostFunction ();
   TestHelloWasm ();

   std::printf ("\n=== Results: %d passed, %d failed ===\n", nPassed, nFailed);

   return (nFailed > 0) ? 1 : 0;
}
