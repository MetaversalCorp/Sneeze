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

//! # sneeze - the guest-side SDK for Sneeze WASM modules
//!
//! A module talks to the engine through exactly one host import (`Sneeze.Call`)
//! and a handful of exports. This crate hides that ABI behind typed objects:
//! a [`FABRIC`] handed to your [`INSTANCE`] at `Open`, from which you reach
//! [`CONSOLE`], [`STORAGE`], [`DATA`], and [`SCENE`], build nodes with [`SNEEZE_ABI_MAPOBJECT`], and mutate
//! them through [`NODE`].
//!
//! ```ignore
//! use sneeze::*;
//!
//! struct MY_MODULE;
//! impl INSTANCE for MY_MODULE
//! {
//!    fn Open (pFabric: FABRIC)
//!    {
//!       pFabric.Console ().Log ("hello from wasm");
//!
//!       let mut root = SNEEZE_ABI_MAPOBJECT::Physical ();
//!       root.Name ("Stool").Reference ("assets/Stool.glb");
//!       pFabric.Scene ().Node_Root (&root);
//!    }
//! }
//!
//! sneeze::instance! (MY_MODULE);
//! ```
//!
//! The ABI contract is `sdk/include/sneeze_abi.h`; this crate mirrors it.

#![allow(non_snake_case, non_upper_case_globals, non_camel_case_types, dead_code, unused_parens)]

pub mod abi;
mod ffi;
mod objects;
mod mapobject;
mod snapshot;

use nanoserde::DeJson;

pub use abi::{SNEEZE_OBJECTIX_CLASS, SNEEZE_OBJECTIX_COMPOSE, SNEEZE_OBJECTIX_INDEX, SNEEZE_OBJECTIX_ERROR, SNEEZE_OBJECTIX_IDENTITY};
pub use abi::eSNEEZE_ABI_SILO_SCOPE;
pub use objects::{CONSOLE, DATA, FABRIC, NODE, SCENE, STORAGE};
pub use mapobject::SNEEZE_ABI_MAPOBJECT;
pub use snapshot::{LOCATION, RESOURCE, CONTAINER, SIGNATURE, AGENT, SERVICE, MODULE};

use snapshot::SNAPSHOT_DATA;

// ---------------------------------------------------------------------------
// The parsed Open snapshot, held privately for the life of the module instance.
// A module never touches it directly; it reads the typed views off its FABRIC
// (Location/Resource/Signature/Agent/Container). Single-threaded wasm, written
// once by the generated Open before user code runs, read-only thereafter.
// ---------------------------------------------------------------------------

static mut SNAPSHOT_STORE: Option<SNAPSHOT_DATA> = None;

pub (crate) fn Snapshot () -> &'static SNAPSHOT_DATA
{
   unsafe
   {
      let pStore = &mut *core::ptr::addr_of_mut! (SNAPSHOT_STORE);

      if pStore.is_none ()
      {
         *pStore = Some (SNAPSHOT_DATA::default ());
      }

      pStore.as_ref ().unwrap ()
   }
}

#[doc(hidden)]
pub fn Snapshot_Load (pSnapshot: SNAPSHOT)
{
   unsafe
   {
      let pStore = &mut *core::ptr::addr_of_mut! (SNAPSHOT_STORE);

      *pStore = Some (pSnapshot.Parse ());
   }
}

// ---------------------------------------------------------------------------
// SNAPSHOT - the immutable blob the engine pushes at Open. Internal plumbing:
// the engine synthesizes a JSON document of fixed-shape sections, copies it into
// guest memory via the Alloc handshake, and hands the generated Open its
// (offset, size). Snapshot_Load parses it once into the private SNAPSHOT_STORE
// before user code runs. A module never sees this type; it reads the parsed
// data through the typed FABRIC views (Location/Resource/Signature/Agent/
// Container). The raw bytes are valid only for the duration of the generated
// Open - the engine frees the guest block as soon as Open returns.
// ---------------------------------------------------------------------------

#[doc(hidden)]
#[derive(Copy, Clone)]
pub struct SNAPSHOT
{
   m_nOffset: u32,
   m_nLength: u32,
}

impl SNAPSHOT
{
   pub fn From_Raw (nOffset: i32, nSize: i32) -> Self
   {
      SNAPSHOT { m_nOffset: nOffset as u32, m_nLength: nSize as u32 }
   }

   pub (crate) fn Bytes (&self) -> &[u8]
   {
      if self.m_nLength == 0
      {
         &[]
      }
      else
      {
         unsafe { core::slice::from_raw_parts (self.m_nOffset as *const u8, self.m_nLength as usize) }
      }
   }

   pub (crate) fn Text (&self) -> &str
   {
      core::str::from_utf8 (self.Bytes ()).unwrap_or ("")
   }

   pub (crate) fn Parse (&self) -> SNAPSHOT_DATA
   {
      SNAPSHOT_DATA::deserialize_json (self.Text ()).unwrap_or_default ()
   }
}

// ---------------------------------------------------------------------------
// INSTANCE - the lifecycle a guest wasm instance implements. Wire it up with
// instance!. (The engine calls the running module a WASM_INSTANCE; a declared
// module in the manifest is the separate MODULE record.) Open receives only the
// FABRIC handle; the Open snapshot is parsed privately and read through the
// fabric's typed views.
// ---------------------------------------------------------------------------

pub trait INSTANCE
{
   fn Init () {}
   fn Open (pFabric: FABRIC) { let _ = pFabric; }
   fn Close (pFabric: FABRIC) { let _ = pFabric; }
   fn Shutdown () {}
}

// ---------------------------------------------------------------------------
// Memory + event exports the host calls. module! re-exports these under the
// ABI names. Alloc/Free let the host place bytes into guest memory (the Open
// snapshot, and later event packets); Notify is the host -> guest event entry
// point (inert until node events land).
// ---------------------------------------------------------------------------

pub fn Alloc (nSize: i32) -> i32
{
   let mut nOffset = 0;

   if nSize > 0
   {
      let mut aByte: Vec<u8> = Vec::with_capacity (nSize as usize);
      let pByte = aByte.as_mut_ptr ();

      core::mem::forget (aByte);

      nOffset = pByte as u32 as i32;
   }

   nOffset
}

pub fn Free (nOffset: i32, nSize: i32)
{
   if nOffset != 0  &&  nSize > 0
   {
      unsafe
      {
         let _ = Vec::from_raw_parts (nOffset as u32 as *mut u8, 0, nSize as usize);
      }
   }
}

pub fn Notify (_nOffset: i32, _nSize: i32) -> i64
{
   0
}

// ---------------------------------------------------------------------------
// instance! - generates the exports the engine looks up, delegating the
// lifecycle to a type that implements INSTANCE.
// ---------------------------------------------------------------------------

#[macro_export]
macro_rules! instance
{
   ($instance:ty) =>
   {
      #[no_mangle]
      pub extern "C" fn Init ()
      {
         <$instance as $crate::INSTANCE>::Init ();
      }

      #[no_mangle]
      pub extern "C" fn Open (twFabricIx: u64, nOffset: i32, nSize: i32)
      {
         $crate::Snapshot_Load ($crate::SNAPSHOT::From_Raw (nOffset, nSize));
         <$instance as $crate::INSTANCE>::Open ($crate::FABRIC::New (twFabricIx));
      }

      #[no_mangle]
      pub extern "C" fn Close (twFabricIx: u64)
      {
         <$instance as $crate::INSTANCE>::Close ($crate::FABRIC::New (twFabricIx));
      }

      #[no_mangle]
      pub extern "C" fn Shutdown ()
      {
         <$instance as $crate::INSTANCE>::Shutdown ();
      }

      #[no_mangle]
      pub extern "C" fn Alloc (nSize: i32) -> i32
      {
         $crate::Alloc (nSize)
      }

      #[no_mangle]
      pub extern "C" fn Free (nOffset: i32, nSize: i32)
      {
         $crate::Free (nOffset, nSize);
      }

      #[no_mangle]
      pub extern "C" fn Notify (nOffset: i32, nSize: i32) -> i64
      {
         $crate::Notify (nOffset, nSize)
      }
   };
}
