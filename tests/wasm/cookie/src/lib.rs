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

// Build the wasm executable (run from this crate's directory):
//    cargo build --target wasm32-unknown-unknown --release --target-dir target
// Output: target/wasm32-unknown-unknown/release/cookie.wasm
// Always pass --target-dir target; otherwise a CARGO_TARGET_DIR set in the shell
// can redirect the output elsewhere.

#![allow(non_snake_case, non_camel_case_types, dead_code)]

#[link(wasm_import_module = "Console")]
extern "C"
{
   fn Log (dwOffset: u32, dwLength: u32);
}

#[link(wasm_import_module = "Storage")]
extern "C"
{
   // Set writes a JSON value (dwVal..) at a dot-notation path (dwPath..) within
   // the container's Silo document for the given scope. The value is supplied by
   // this module — this is the ordinary imperative setter a service uses to
   // record runtime state.
   fn Set (nScope: i32, dwPathOffset: u32, dwPathLength: u32, dwValOffset: u32, dwValLength: u32) -> i32;
}

fn LogMsg (sMsg: &str)
{
   unsafe
   {
      Log (sMsg.as_ptr () as u32, sMsg.len () as u32);
   }
}

// SILO scope selector — must match SNEEZE::eSILO_SCOPE (Storage.h).
const SILO_SCOPE_PERMANENT_ORG:       i32 = 0;
const SILO_SCOPE_PERMANENT_CONTAINER: i32 = 1;
const SILO_SCOPE_TEMPORARY_ORG:       i32 = 2;
const SILO_SCOPE_TEMPORARY_CONTAINER: i32 = 3;

// The scope this module writes its cookie state to (per-container permanent).
const COOKIE_SCOPE: i32 = SILO_SCOPE_PERMANENT_CONTAINER;

// StorageSet — thin wrapper over the Storage.Set host function. sVal is JSON
// text (a quoted string, number, bool, object, or array).
fn StorageSet (nScope: i32, sPath: &str, sVal: &str) -> i32
{
   unsafe
   {
      Set (nScope,
           sPath.as_ptr () as u32, sPath.len () as u32,
           sVal.as_ptr ()  as u32, sVal.len ()  as u32)
   }
}

// ---------------------------------------------------------------------------
// WASM lifecycle exports
// ---------------------------------------------------------------------------

#[no_mangle]
pub extern "C" fn Init ()
{
   LogMsg ("Cookie WASM: Init");
}

#[no_mangle]
pub extern "C" fn Open (_twFabricIx: u64, _dwOffset: u32, _dwLength: u32)
{
   LogMsg ("Cookie WASM: Open");

   // Record the cookie's fields into the container's Silo, one Set per key.
   // These are values this service produces at runtime; here they are literals
   // for the demo. Each value is JSON text (strings are quoted).
   StorageSet (COOKIE_SCOPE, "session", "\"8f3ab29c1d\"");
   StorageSet (COOKIE_SCOPE, "theme",   "\"light\"");
   StorageSet (COOKIE_SCOPE, "consent", "\"granted\"");
   StorageSet (COOKIE_SCOPE, "preferences", "{\"language\":\"en-US\",\"notifications\":true}");

   LogMsg ("  Cookie fields written to Silo (permanent/container)");
}

#[no_mangle]
pub extern "C" fn Close (_twFabricIx: u64)
{
   LogMsg ("Cookie WASM: Close");
}

#[no_mangle]
pub extern "C" fn Shutdown ()
{
   LogMsg ("Cookie WASM: Shutdown");
}
