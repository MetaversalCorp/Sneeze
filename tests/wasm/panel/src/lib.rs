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
// Output: target/wasm32-unknown-unknown/release/panel.wasm
// Always pass --target-dir target; otherwise a CARGO_TARGET_DIR set in the shell
// can redirect the output elsewhere.

#![allow(non_snake_case, non_camel_case_types, dead_code)]

#[link(wasm_import_module = "Console")]
extern "C"
{
   fn Log (dwOffset: u32, dwLength: u32);
}

#[link(wasm_import_module = "Scene")]
extern "C"
{
   fn Node_Root      (twFabricIx: u64, dwOffset: u32, dwLength: u32) -> u64;
   // Node_Panel_Map creates a panel like Node_Panel, but the RML+CSS source is
   // read host-side from the fabric's MSF "data" block at a dot-separated path
   // (dwPath..) rather than passed from this module's memory. This keeps the
   // panel document authored in the fabric file instead of hard-coded here.
   fn Node_Panel_Map (twFabricIx: u64, twParentIx: u64, dwObjOffset: u32, dwObjLength: u32, dwPathOffset: u32, dwPathLength: u32) -> u64;
}

fn LogMsg (sMsg: &str)
{
   unsafe
   {
      Log (sMsg.as_ptr () as u32, sMsg.len () as u32);
   }
}

const MAP_OBJECT_CLASS_ROOT:                     u16 = 70;
const MAP_OBJECT_CLASS_CELESTIAL:                u16 = 71;
const MAP_OBJECT_CLASS_PANEL:                    u16 = 74;

const MAP_OBJECT_TYPE_TYPE_CELESTIAL_STARSYSTEM: u8  = 9;

// Where this module expects the panel's RML+CSS document to live inside the
// MSF "data" block, as a dot-separated path. This is a contract with the
// fabric: any fabric that uses panel.wasm must place the panel document string
// at "data.panel". The host reads it and hands it to the panel node.
const PANEL_PATH: &str = "panel";

const fn OBJECTIX_COMPOSE (wClass: u16, twObjectIx: u64) -> u64
{
   ((wClass as u64) << 48)  |  (twObjectIx & 0x0000_FFFF_FFFF_FFFF)
}

// ---------------------------------------------------------------------------
// RMCOBJECT layout (528 bytes, packed) — must match the host wire format.
// ---------------------------------------------------------------------------

#[repr(C, packed)]
struct RMCOBJECT
{
   qwObjectIx_Parent:       u64,
   qwObjectIx_Self:         u64,
   qwEvent:                 u64,

   wsName:                  [u16; 48],

   bType:                   u8,
   bSubtype:                u8,
   bFiction:                u8,
   abReserved_Type:         [u8; 5],

   twOwner:                 u64,

   qwResource:              u64,
   sName_Resource:          [u8; 64],
   sReference:              [u8; 128],

   d3Position:              [f64; 3],
   d4Rotation:              [f64; 4],
   d3Scale:                 [f64; 3],

   tmPeriod:                i64,
   tmOrigin:                i64,
   dA:                      f64,
   dB:                      f64,

   abReserved_Bound:        [u8; 24],
   d3Max:                   [f64; 3],

   fMass:                   f32,
   fGravity:                f32,
   fColor:                  f32,
   fBrightness:             f32,
   fReflectivity:           f32,
   abReserved_Properties:   [u8; 12],
}

const _: () = assert!(core::mem::size_of::<RMCOBJECT> () == 528);

impl RMCOBJECT
{
   fn New () -> Self
   {
      unsafe
      {
         core::mem::zeroed ()
      }
   }

   fn Name_Set (&mut self, sName: &str)
   {
      for (i, c) in sName.chars ().enumerate ()
      {
         if i >= 48
         {
            break;
         }
         self.wsName[i] = c as u16;
      }
   }
}

// Submit_Panel — create an in-scene UI panel under nParent. The RML+CSS source
// is not carried by this module; the host reads it from the fabric's "data"
// block at sPath. d3Max[0,1] carries the quad aspect ratio only; the host
// rasterizes the document (512x512) and the compositor anchors/sizes the quad.
fn Submit_Panel (twFabricIx: u64, nParent: u64, nSelf: u64, sName: &str, dAspectW: f64, dAspectH: f64, precX: f64, precY: f64, precZ: f64, sPath: &str) -> u64
{
   let mut obj = RMCOBJECT::New ();
   obj.qwObjectIx_Parent = OBJECTIX_COMPOSE (MAP_OBJECT_CLASS_CELESTIAL, nParent);
   obj.qwObjectIx_Self   = OBJECTIX_COMPOSE (MAP_OBJECT_CLASS_PANEL,     nSelf);
   obj.d3Position        = [precX, precY, precZ];
   obj.d4Rotation        = [0.0, 0.0, 0.0, 1.0];
   obj.d3Scale           = [1.0, 1.0, 1.0];
   obj.d3Max             = [dAspectW, dAspectH, 0.0];
   obj.Name_Set (sName);

   let dwObjOffset = &obj as *const RMCOBJECT as u32;
   let dwObjLength = core::mem::size_of::<RMCOBJECT> () as u32;

   unsafe
   {
      Node_Panel_Map (twFabricIx, obj.qwObjectIx_Parent, dwObjOffset, dwObjLength, sPath.as_ptr () as u32, sPath.len () as u32)
   }
}

// ---------------------------------------------------------------------------
// WASM lifecycle exports
// ---------------------------------------------------------------------------

#[no_mangle]
pub extern "C" fn Init ()
{
   LogMsg ("Panel WASM: Init");
}

#[no_mangle]
pub extern "C" fn Open (twFabricIx: u64, _dwOffset: u32, _dwLength: u32)
{
   LogMsg (&format! ("Panel WASM: Open (twFabricIx={})", twFabricIx));

   // Root frame (STARSYSTEM) — the fabric's root node. Produces no geometry of
   // its own; it only parents the panel.
   let mut objRoot = RMCOBJECT::New ();
   objRoot.qwObjectIx_Parent = OBJECTIX_COMPOSE (MAP_OBJECT_CLASS_ROOT,      0);
   objRoot.qwObjectIx_Self   = OBJECTIX_COMPOSE (MAP_OBJECT_CLASS_CELESTIAL, 2);
   objRoot.bType             = MAP_OBJECT_TYPE_TYPE_CELESTIAL_STARSYSTEM;
   objRoot.d3Scale           = [1.0, 1.0, 1.0];
   objRoot.d4Rotation        = [0.0, 0.0, 0.0, 1.0];
   objRoot.Name_Set ("Panel");
   let dwOffset = &objRoot as *const RMCOBJECT as u32;
   let dwLength = core::mem::size_of::<RMCOBJECT> () as u32;
   let twRoot = unsafe { Node_Root (twFabricIx, dwOffset, dwLength) };
   if twRoot == 0
   {
      LogMsg ("  ERROR: Failed to create root node");
      return;
   }

   // In-scene UI panel: an RmlUi RML+CSS document the host rasterizes to a
   // textured quad. Placed at the world origin so the default camera frames it
   // dead-center (with no other geometry, the scene render-scale is 1.0). The
   // document itself is authored in the fabric file at "data.panel"; the host
   // reads it by path (PANEL_PATH) rather than this module embedding it.
   let twPanel = Submit_Panel (twFabricIx, 2, 7400, "Panel", 1.6, 1.0, 0.0, 0.0, 0.0, PANEL_PATH);
   if twPanel != 0
   {
      LogMsg ("  Panel created");
   }
   else
   {
      LogMsg ("  ERROR: Failed to create panel");
   }
}

#[no_mangle]
pub extern "C" fn Close (_twFabricIx: u64)
{
   LogMsg ("Panel WASM: Close");
}

#[no_mangle]
pub extern "C" fn Shutdown ()
{
   LogMsg ("Panel WASM: Shutdown");
}
