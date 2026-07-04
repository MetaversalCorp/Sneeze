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
// Output: target/wasm32-unknown-unknown/release/map.wasm
// Always pass --target-dir target; otherwise a CARGO_TARGET_DIR set in the shell
// can redirect the output elsewhere.

#![allow(non_snake_case, non_camel_case_types, dead_code)]

// ---------------------------------------------------------------------------
// Generic map module
//
// This module owns no scene data. On Open it hands the fabric over to the
// browser's map-managed path: a single call to Scene.Node_Map injects the
// node tree the MSF "data" block carries at the "data.scene" path (see
// SCENE_PATH). The rest of "data" is free for other uses. This stands in for
// a real map service until network connectivity to one is built in.
// ---------------------------------------------------------------------------

#[link(wasm_import_module = "Console")]
extern "C"
{
   fn Log (dwOffset: u32, dwLength: u32);
}

#[link(wasm_import_module = "Scene")]
extern "C"
{
   fn Node_Map (twFabricIx: u64, dwOffset: u32, dwLength: u32) -> u64;
}

const OBJECTIX_ERROR: u64 = 0x0000_FFFF_FFFF_FFFE;
const OBJECTIX_NULL:  u64 = 0x0000_0000_0000_0000;

// Where this module expects the scene tree to live inside the MSF "data"
// block, as a dot-separated path. This is a hardcoded contract: any fabric
// that uses map.wasm must place its node tree at "data.scene". An empty string
// here would instead read the whole "data" block as the tree.
const SCENE_PATH: &str = "scene";

fn LogMsg (sMsg: &str)
{
   unsafe
   {
      Log (sMsg.as_ptr () as u32, sMsg.len () as u32);
   }
}

#[no_mangle]
pub extern "C" fn Init ()
{
   LogMsg ("Map WASM: Init");
}

#[no_mangle]
pub extern "C" fn Open (twFabricIx: u64, _dwOffset: u32, _dwLength: u32)
{
   LogMsg (&format! ("Map WASM: Open (twFabricIx={})", twFabricIx));

   let twRoot = unsafe { Node_Map (twFabricIx, SCENE_PATH.as_ptr () as u32, SCENE_PATH.len () as u32) };

   if twRoot == OBJECTIX_NULL  ||  twRoot == OBJECTIX_ERROR
   {
      LogMsg ("  ERROR: Node_Map failed");
   }
   else
   {
      LogMsg (&format! ("  Map loaded: root={}", twRoot));
   }
}

#[no_mangle]
pub extern "C" fn Close (_twFabricIx: u64)
{
   LogMsg ("Map WASM: Close");
}

#[no_mangle]
pub extern "C" fn Shutdown ()
{
   LogMsg ("Map WASM: Shutdown");
}
