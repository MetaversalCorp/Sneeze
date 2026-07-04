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
   fn Node_Root  (twFabricIx: u64, dwOffset: u32, dwLength: u32) -> u64;
   fn Node_Panel (twParentIx: u64, dwObjOffset: u32, dwObjLength: u32, dwSrcOffset: u32, dwSrcLength: u32) -> u64;
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

// Submit_Panel — create an in-scene UI panel under nParent and hand the engine
// its RML+CSS source. d3Max[0,1] carries the quad aspect ratio only; the host
// rasterizes the document (512x512) and the compositor anchors/sizes the quad.
fn Submit_Panel (nParent: u64, nSelf: u64, sName: &str, dAspectW: f64, dAspectH: f64, precX: f64, precY: f64, precZ: f64, sSource: &str) -> u64
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
      Node_Panel (obj.qwObjectIx_Parent, dwObjOffset, dwObjLength, sSource.as_ptr () as u32, sSource.len () as u32)
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
   // dead-center (with no other geometry, the scene render-scale is 1.0).
   const PANEL_RML: &str =
      "<rml><head><style>\
       body { width: 100%; height: 100%; font-family: Inter; color: #e9eef6; }\
       #card { position: absolute; left: 6%; top: 6%; width: 88%; height: 88%;\
               padding: 28px; border-radius: 18px;\
               background-color: rgba(18, 22, 32, 224);\
               border-width: 1px; border-color: rgba(255, 255, 255, 36); }\
       .title { display: block; font-size: 26px; font-weight: 600; color: #ffd089; margin: 0 0 14px 0; }\
       .body  { display: block; font-size: 16px; color: #c4ccd8; }\
       </style></head>\
       <body><div id='card'>\
       <span class='title'>Panel</span>\
       <span class='body'>Standalone in-scene RmlUi panel rendered by the engine and composited over the 3D scene.</span>\
       </div></body></rml>";

   let twPanel = Submit_Panel (2, 7400, "Panel", 1.6, 1.0, 0.0, 0.0, 0.0, PANEL_RML);
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
