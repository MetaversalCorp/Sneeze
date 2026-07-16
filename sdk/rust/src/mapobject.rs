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

//! `SNEEZE_ABI_MAPOBJECT` - the 528-byte map-object wire struct and its fluent builder. Fill one
//! in guest memory, then hand it to `SCENE::Node_Root` / `SCENE::Node_Open`,
//! which pass its (offset, length) to the host. Layout mirrors
//! `include/Map_Object.h` / `sdk/include/sneeze_abi.h` field for field.

#![allow(non_snake_case)]

use crate::abi::*;

#[repr(C, packed)]
pub struct SNEEZE_ABI_MAPOBJECT
{
   // OBJECT_HEAD (24 bytes)
   qwComposed_Parent:     u64,
   qwComposed_Self:       u64,
   qwEvent:               u64,

   // MAP_OBJECT_NAME (96 bytes)
   wsName:                [u16; 48],

   // MAP_OBJECT_TYPE (8 bytes)
   bType:                 u8,
   bSubtype:              u8,
   bFiction:              u8,
   abReserved_Type:       [u8; 5],

   // MAP_OBJECT_OWNER (8 bytes)
   twOwner:               u64,

   // MAP_OBJECT_RESOURCE (200 bytes)
   qwResource:            u64,
   sName_Resource:        [u8; 64],
   sReference:            [u8; 128],

   // MAP_OBJECT_TRANSFORM (80 bytes)
   d3Position:            [f64; 3],
   d4Rotation:            [f64; 4],
   d3Scale:               [f64; 3],

   // MAP_OBJECT_ORBIT (32 bytes)
   tmPeriod:              i64,
   tmOrigin:              i64,
   dA:                    f64,
   dB:                    f64,

   // MAP_OBJECT_BOUND (48 bytes)
   abReserved_Bound:      [u8; 24],
   d3Max:                 [f64; 3],

   // MAP_OBJECT_PROPERTIES (32 bytes)
   fMass:                 f32,
   fGravity:              f32,
   fColor:                f32,
   fBrightness:           f32,
   fReflectivity:         f32,
   abReserved_Properties: [u8; 12],
}

const _: () = assert! (core::mem::size_of::<SNEEZE_ABI_MAPOBJECT> () == 528);

impl SNEEZE_ABI_MAPOBJECT
{
   pub const SIZE: usize = 528;

   // --- Construction: one factory per class. Self starts as SNEEZE_OBJECTIX_IDENTITY
   //     ("assign me the next free index"); override with ObjectIx. ---

   pub fn New (wClass: u16) -> Self
   {
      let mut pObject: SNEEZE_ABI_MAPOBJECT = unsafe { core::mem::zeroed () };

      pObject.qwComposed_Self = SNEEZE_OBJECTIX_COMPOSE (wClass, SNEEZE_OBJECTIX_IDENTITY);
      pObject.d3Scale         = [1.0, 1.0, 1.0];
      pObject.d4Rotation      = [0.0, 0.0, 0.0, 1.0];

      pObject
   }

   pub fn Root        () -> Self { Self::New (kSNEEZE_ABI_MAP_OBJECT_CLASS_ROOT) }
   pub fn Celestial   () -> Self { Self::New (kSNEEZE_ABI_MAP_OBJECT_CLASS_CELESTIAL) }
   pub fn Terrestrial () -> Self { Self::New (kSNEEZE_ABI_MAP_OBJECT_CLASS_TERRESTRIAL) }
   pub fn Physical    () -> Self { Self::New (kSNEEZE_ABI_MAP_OBJECT_CLASS_PHYSICAL) }
   pub fn Panel       () -> Self { Self::New (kSNEEZE_ABI_MAP_OBJECT_CLASS_PANEL) }
   pub fn Light       () -> Self { Self::New (kSNEEZE_ABI_MAP_OBJECT_CLASS_LIGHT) }

   // --- Identity ---

   pub fn Parent (&mut self, wClass: u16, twObjectIx: u64) -> &mut Self
   {
      self.qwComposed_Parent = SNEEZE_OBJECTIX_COMPOSE (wClass, twObjectIx);
      self
   }

   pub fn ObjectIx (&mut self, twObjectIx: u64) -> &mut Self
   {
      let wClass = SNEEZE_OBJECTIX_CLASS (self.qwComposed_Self);
      self.qwComposed_Self = SNEEZE_OBJECTIX_COMPOSE (wClass, twObjectIx);
      self
   }

   // --- Name / resource (UTF-16 name; UTF-8 reference URL) ---

   pub fn Name (&mut self, sName: &str) -> &mut Self
   {
      self.wsName = [0u16; 48];

      for (nIz, cCharacter) in sName.chars ().enumerate ()
      {
         if nIz >= 48
         {
            break;
         }
         self.wsName[nIz] = cCharacter as u16;
      }

      self
   }

   pub fn Reference (&mut self, sReference: &str) -> &mut Self
   {
      self.sReference = [0u8; 128];

      let aByte    = sReference.as_bytes ();
      let nLength  = if aByte.len () < 127 { aByte.len () } else { 127 };

      self.sReference[..nLength].copy_from_slice (&aByte[..nLength]);
      self
   }

   // --- Type ---

   pub fn Type (&mut self, bType: u8) -> &mut Self { self.bType = bType; self }
   pub fn Subtype (&mut self, bSubtype: u8) -> &mut Self { self.bSubtype = bSubtype; self }

   // --- Transform (whole-array writes - packed fields are never referenced) ---

   pub fn Position   (&mut self, dX: f64, dY: f64, dZ: f64)          -> &mut Self { self.d3Position = [dX, dY, dZ];             self }
   pub fn Rotation   (&mut self, dX: f64, dY: f64, dZ: f64, dW: f64) -> &mut Self { self.d4Rotation = [dX, dY, dZ, dW];         self }
   pub fn Scale      (&mut self, dScale: f64)                        -> &mut Self { self.d3Scale    = [dScale, dScale, dScale]; self }
   pub fn Scale_Axes (&mut self, dX: f64, dY: f64, dZ: f64)          -> &mut Self { self.d3Scale    = [dX, dY, dZ];             self }
   pub fn Bound      (&mut self, dX: f64, dY: f64, dZ: f64)          -> &mut Self { self.d3Max      = [dX, dY, dZ];             self }

   // --- Orbit ---

   pub fn Orbit (&mut self, dA: f64, dB: f64, tmPeriod: i64, tmOrigin: i64) -> &mut Self
   {
      self.dA       = dA;
      self.dB       = dB;
      self.tmPeriod = tmPeriod;
      self.tmOrigin = tmOrigin;
      self
   }

   // --- Properties (set as a unit, one call per class; dwColor is 0xRRGGBB,
   //     packed into fColor's bits - the engine reads the bits as the color) ---

   pub fn Properties_Celestial (&mut self, fMass: f32, fGravity: f32, dwColor: u32, fBrightness: f32, fReflectivity: f32) -> &mut Self
   {
      self.fMass         = fMass;
      self.fGravity      = fGravity;
      self.fColor        = f32::from_bits (dwColor);
      self.fBrightness   = fBrightness;
      self.fReflectivity = fReflectivity;
      self
   }

   // A light overlays the celestial region: fOpeningAngle/fFalloffAngle occupy
   // the fMass/fGravity bytes (degrees; spot only), sharing fColor/fBrightness.
   pub fn Properties_Light (&mut self, fOpeningAngle: f32, fFalloffAngle: f32, dwColor: u32, fBrightness: f32) -> &mut Self
   {
      self.fMass       = fOpeningAngle;
      self.fGravity    = fFalloffAngle;
      self.fColor      = f32::from_bits (dwColor);
      self.fBrightness = fBrightness;
      self
   }

   // --- Wire access ---

   pub fn Composed_Parent (&self) -> u64 { self.qwComposed_Parent }

   pub fn Pointer (&self) -> *const u8
   {
      self as *const SNEEZE_ABI_MAPOBJECT as *const u8
   }
}
