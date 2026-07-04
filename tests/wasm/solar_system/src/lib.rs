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
// Output: target/wasm32-unknown-unknown/release/solar_system.wasm
// Always pass --target-dir target; otherwise a CARGO_TARGET_DIR set in the shell
// can redirect the output elsewhere.

#![allow(non_snake_case, non_camel_case_types, dead_code)]

mod star;
mod planets;
mod moons_earth;
mod moons_mars;
mod moons_jupiter;
mod moons_saturn;
mod moons_uranus;
mod moons_neptune;
mod moons_pluto;
mod debris;

#[link(wasm_import_module = "Console")]
extern "C"
{
   fn Log (dwOffset: u32, dwLength: u32);
}

#[link(wasm_import_module = "Scene")]
extern "C"
{
   fn Node_Root     (twFabricIx: u64, dwOffset: u32, dwLength: u32) -> u64;
   fn Node_Open     (twParentIx: u64, dwOffset: u32, dwLength: u32) -> u64;
   fn Node_Close    (twObjectIx: u64) -> i32;
   fn Node_Position (twObjectIx: u64, dX: f64, dY: f64, dZ: f64);
   fn Node_Scale    (twObjectIx: u64, dScale: f64);
   fn Node_Bound    (twObjectIx: u64, dBound: f64);
   fn Node_Color    (twObjectIx: u64, nColor: i32);
   fn Node_Name     (twObjectIx: u64, dwOffset: u32, dwLength: u32);
   fn Node_Radius   (twObjectIx: u64, dRadius: f64);
}

fn LogMsg (sMsg: &str)
{
   unsafe
   {
      Log (sMsg.as_ptr () as u32, sMsg.len () as u32);
   }
}

const MAP_OBJECT_CLASS_ROOT                         : u16 = 70;
const MAP_OBJECT_CLASS_CELESTIAL                    : u16 = 71;
const MAP_OBJECT_CLASS_TERRESTRIAL                  : u16 = 72;
const MAP_OBJECT_CLASS_PHYSICAL                     : u16 = 73;

const MAP_OBJECT_TYPE_TYPE_CELESTIAL_NONE           : u8 = 0;
const MAP_OBJECT_TYPE_TYPE_CELESTIAL_UNIVERSE       : u8 = 1;
const MAP_OBJECT_TYPE_TYPE_CELESTIAL_SUPERCLUSTER   : u8 = 2;
const MAP_OBJECT_TYPE_TYPE_CELESTIAL_GALAXYCLUSTER  : u8 = 3;
const MAP_OBJECT_TYPE_TYPE_CELESTIAL_GALAXY         : u8 = 4;
const MAP_OBJECT_TYPE_TYPE_CELESTIAL_SECTOR         : u8 = 5;
const MAP_OBJECT_TYPE_TYPE_CELESTIAL_NEBULA         : u8 = 6;
const MAP_OBJECT_TYPE_TYPE_CELESTIAL_STARCLUSTER    : u8 = 7;
const MAP_OBJECT_TYPE_TYPE_CELESTIAL_BLACKHOLE      : u8 = 8;
const MAP_OBJECT_TYPE_TYPE_CELESTIAL_STARSYSTEM     : u8 = 9;
const MAP_OBJECT_TYPE_TYPE_CELESTIAL_STAR           : u8 = 10;
const MAP_OBJECT_TYPE_TYPE_CELESTIAL_PLANETSYSTEM   : u8 = 11;
const MAP_OBJECT_TYPE_TYPE_CELESTIAL_PLANET         : u8 = 12;
const MAP_OBJECT_TYPE_TYPE_CELESTIAL_MOONSYSTEM     : u8 = 125;
const MAP_OBJECT_TYPE_TYPE_CELESTIAL_MOON           : u8 = 13;
const MAP_OBJECT_TYPE_TYPE_CELESTIAL_DEBRISSYSTEM   : u8 = 135;
const MAP_OBJECT_TYPE_TYPE_CELESTIAL_DEBRIS         : u8 = 14;
const MAP_OBJECT_TYPE_TYPE_CELESTIAL_SATELLITE      : u8 = 15;
const MAP_OBJECT_TYPE_TYPE_CELESTIAL_TRANSPORT      : u8 = 16;
const MAP_OBJECT_TYPE_TYPE_CELESTIAL_SURFACE        : u8 = 17;

const fn OBJECTIX_COMPOSE (wClass: u16, twObjectIx: u64) -> u64
{
   ((wClass as u64) << 48)  |  (twObjectIx & 0x0000_FFFF_FFFF_FFFF)
}

const TEX_BASE: &str = "https://cdn.rp1.com/res/texture/celestial/";

// ---------------------------------------------------------------------------
// RMCOBJECT layout (528 bytes, packed)
// ---------------------------------------------------------------------------

#[repr(C, packed)]
struct RMCOBJECT
{
   // OBJECT_HEAD (24 bytes)
   qwObjectIx_Parent:       u64,
   qwObjectIx_Self:         u64,
   qwEvent:                 u64,

   // MAP_OBJECT_NAME (96 bytes)
   wsName:                  [u16; 48],

   // MAP_OBJECT_TYPE (8 bytes)
   bType:                   u8,
   bSubtype:                u8,
   bFiction:                u8,
   abReserved_Type:         [u8; 5],

   // MAP_OBJECT_OWNER (8 bytes)
   twOwner:                 u64,

   // MAP_OBJECT_RESOURCE (200 bytes)
   qwResource:              u64,
   sName_Resource:          [u8; 64],
   sReference:              [u8; 128],

   // MAP_OBJECT_TRANSFORM (80 bytes)
   d3Position:              [f64; 3],
   d4Rotation:              [f64; 4],
   d3Scale:                 [f64; 3],

   // MAP_OBJECT_ORBIT (32 bytes)
   tmPeriod:                i64,
   tmOrigin:                i64,
   dA:                      f64,
   dB:                      f64,

   // MAP_OBJECT_BOUND (48 bytes)
   abReserved_Bound:        [u8; 24],
   d3Max:                   [f64; 3],

   // MAP_OBJECT_PROPERTIES (32 bytes)
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

   fn Reference_Set (&mut self, sRef: &str)
   {
      let abRef = sRef.as_bytes ();
      let nLen  = if abRef.len () < 127 { abRef.len () } else { 127 };
      self.sReference[..nLen].copy_from_slice (&abRef[..nLen]);
   }
}

// ---------------------------------------------------------------------------
// Submit helpers — build and submit one RMCOBJECT per call
//
// Three variants match the three levels of the scene hierarchy:
//   System  — orbital frame (STARSYSTEM, PLANETSYSTEM, MOONSYSTEM, DEBRISSYSTEM)
//   Body    — physical body (STAR, PLANET, MOON, DEBRIS)
//   Surface — texture attachment (SURFACE)
// ---------------------------------------------------------------------------

fn Submit_Node (obj: &RMCOBJECT)
{
   let dwOffset = obj as *const RMCOBJECT as u32;
   let dwLength = core::mem::size_of::<RMCOBJECT> () as u32;
   unsafe { Node_Open (obj.qwObjectIx_Parent, dwOffset, dwLength) };
}

#[allow(clippy::too_many_arguments)]
fn Submit_System (nParent: u64, nSelf: u64, sName: &str, bType: u8, dA: f64, dB: f64, tmPeriod: i64, tmOrigin: i64, qx: f64, qy: f64, qz: f64, qw: f64, precX: f64, precY: f64, precZ: f64, dBound: f64, fMass: f32, nColor: u32)
{
   let mut obj = RMCOBJECT::New ();
   obj.qwObjectIx_Parent = OBJECTIX_COMPOSE (MAP_OBJECT_CLASS_CELESTIAL, nParent);
   obj.qwObjectIx_Self   = OBJECTIX_COMPOSE (MAP_OBJECT_CLASS_CELESTIAL, nSelf);
   obj.bType             = bType;
   obj.bSubtype          = 0;
   obj.d3Position        = [precX, precY, precZ];
   obj.d4Rotation        = [qx, qy, qz, qw];
   obj.d3Scale           = [1.0, 1.0, 1.0];
   obj.d3Max             = [dBound, dBound, dBound];
   obj.fMass             = fMass;
   obj.fColor            = f32::from_bits (nColor);
   obj.dA                = dA;
   obj.dB                = dB;
   obj.tmPeriod          = tmPeriod;
   obj.tmOrigin          = tmOrigin;
   obj.Name_Set (sName);
   Submit_Node (&obj);
}

#[allow(clippy::too_many_arguments)]
fn Submit_Body (nParent: u64, nSelf: u64, sName: &str, bType: u8, dRadius: f64, fMass: f32, nColor: u32, qx: f64, qy: f64, qz: f64, qw: f64, precX: f64, precY: f64, precZ: f64)
{
   let mut obj = RMCOBJECT::New ();
   obj.qwObjectIx_Parent = OBJECTIX_COMPOSE (MAP_OBJECT_CLASS_CELESTIAL, nParent);
   obj.qwObjectIx_Self   = OBJECTIX_COMPOSE (MAP_OBJECT_CLASS_CELESTIAL, nSelf);
   obj.bType             = bType;
   obj.bSubtype          = 0;
   obj.d3Position        = [precX, precY, precZ];
   obj.d4Rotation        = [qx, qy, qz, qw];
   obj.d3Scale           = [1.0, 1.0, 1.0];
   obj.d3Max             = [dRadius, dRadius, dRadius];
   obj.fMass             = fMass;
   obj.fColor            = f32::from_bits (nColor);
   obj.Name_Set (sName);
   Submit_Node (&obj);
}

fn Submit_Surface (nParent: u64, nSelf: u64, sName: &str, sTexture: &str, dW0Rad: f64, tmSpinPeriod: i64)
{
   let mut obj = RMCOBJECT::New ();
   obj.qwObjectIx_Parent = OBJECTIX_COMPOSE (MAP_OBJECT_CLASS_CELESTIAL, nParent);
   obj.qwObjectIx_Self   = OBJECTIX_COMPOSE (MAP_OBJECT_CLASS_CELESTIAL, nSelf);
   obj.bType             = MAP_OBJECT_TYPE_TYPE_CELESTIAL_SURFACE;
   obj.bSubtype          = 0;
   obj.d4Rotation        = [0.0, 0.0, 0.0, 1.0];
   obj.d3Scale           = [1.0, 1.0, 1.0];
   obj.dA                = dW0Rad;
   obj.tmPeriod          = tmSpinPeriod;
   obj.Name_Set (sName);
   if !sTexture.is_empty ()
   {
      let sUrl = [TEX_BASE, sTexture].concat ();
      obj.Reference_Set (&sUrl);
   }
   Submit_Node (&obj);
}

// ---------------------------------------------------------------------------
// WASM lifecycle exports
// ---------------------------------------------------------------------------

#[no_mangle]
pub extern "C" fn Init ()
{
   LogMsg ("Solar System WASM: Init");
}

#[no_mangle]
pub extern "C" fn Open (twFabricIx: u64, _dwOffset: u32, _dwLength: u32)
{
   LogMsg (&format! ("Solar System WASM: Open (twFabricIx={})", twFabricIx));

   // The "Solar System" frame (index 2, STARSYSTEM) is the fabric's root node.
   let mut objRoot = RMCOBJECT::New ();
   objRoot.qwObjectIx_Parent = OBJECTIX_COMPOSE (MAP_OBJECT_CLASS_ROOT,      0);
   objRoot.qwObjectIx_Self   = OBJECTIX_COMPOSE (MAP_OBJECT_CLASS_CELESTIAL, 2);
   objRoot.bType             = MAP_OBJECT_TYPE_TYPE_CELESTIAL_STARSYSTEM;
   objRoot.bSubtype          = 0;
   objRoot.d3Scale           = [1.0, 1.0, 1.0];
   objRoot.d4Rotation        = [0.0, 0.0, 0.0, 1.0];
   objRoot.fColor            = f32::from_bits (0x00888888);
   objRoot.Name_Set ("Solar System");
   let dwOffset = &objRoot as *const RMCOBJECT as u32;
   let dwLength = core::mem::size_of::<RMCOBJECT> () as u32;
   let twRoot = unsafe { Node_Root (twFabricIx, dwOffset, dwLength) };
   if twRoot == 0
   {
      LogMsg ("  ERROR: Failed to create root node");
      return;
   }

   let mut nTotal: u32 = 0;
   nTotal += star::Submit ();
   nTotal += planets::Submit ();
   nTotal += moons_earth::Submit ();
   // nTotal += moons_mars::Submit ();
   // nTotal += moons_jupiter::Submit ();
   // nTotal += moons_saturn::Submit ();
   // nTotal += moons_uranus::Submit ();
   // nTotal += moons_neptune::Submit ();
   // nTotal += moons_pluto::Submit ();
   nTotal += debris::Submit ();

   LogMsg (&format! ("  Solar system complete: {} objects", nTotal));
}

#[no_mangle]
pub extern "C" fn Close (_twFabricIx: u64)
{
   LogMsg ("Solar System WASM: Close");
}

#[no_mangle]
pub extern "C" fn Shutdown ()
{
   LogMsg ("Solar System WASM: Shutdown");
}
